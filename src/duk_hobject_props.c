/*
 *  Hobject property set/get functionality.
 *
 *  This is very central functionality for size, performance, and compliance.
 *  It is also rather intricate; see hobject-algorithms.txt for discussion on
 *  the algorithms and memory-management.txt for discussion on refcounts and
 *  side effect issues.
 *
 *  Notes:
 *
 *    - It might be tempting to assert "refcount nonzero" for objects
 *      being operated on, but that's not always correct: objects with
 *      a zero refcount may be operated on by the refcount implementation
 *      (finalization) for instance.  Hence, no refcount assertions are made.
 *
 *    - Many operations (memory allocation, identifier operations, etc)
 *      may cause arbitrary side effects (e.g. through GC and finalization).
 *      These side effects may invalidate duk_tval pointers which point to
 *      areas subject to reallocation (like value stack).  Heap objects
 *      themselves have stable pointers.  Holding heap object pointers or
 *      duk_tval copies is not problematic with respect to side effects;
 *      care must be taken when holding and using argument duk_tval pointers.
 *
 *    - If a finalizer is executed, it may operate on the the same object
 *      we're currently dealing with.  For instance, the finalizer might
 *      delete a certain property which has already been looked up and
 *      confirmed to exist.  Ideally finalizers would be disabled if GC
 *      happens during property access.  At the moment property table realloc
 *      disables finalizers, and all DECREFs may cause arbitrary changes so
 *      handle DECREF carefully.
 *
 *    - The order of operations for a DECREF matters.  When DECREF is executed,
 *      the entire object graph must be consistent; note that a refzero may
 *      lead to a mark-and-sweep through a refcount finalizer.
 */

/*
 *  XXX: array indices are mostly typed as duk_uint32_t here; duk_uarridx_t
 *  might be more appropriate.
 */

/*
 *  XXX: duk_uint_fast32_t should probably be used in many places here.
 */

#include "duk_internal.h"

/*
 *  Local defines
 */

#define DUK__NO_ARRAY_INDEX             DUK_HSTRING_NO_ARRAY_INDEX

/* hash probe sequence */
#define DUK__HASH_INITIAL(hash,h_size)  DUK_HOBJECT_HASH_INITIAL((hash),(h_size))
#define DUK__HASH_PROBE_STEP(hash)      DUK_HOBJECT_HASH_PROBE_STEP((hash))

/* marker values for hash part */
#define DUK__HASH_UNUSED                DUK_HOBJECT_HASHIDX_UNUSED
#define DUK__HASH_DELETED               DUK_HOBJECT_HASHIDX_DELETED

/* valstack space that suffices for all local calls, including recursion
 * of other than Duktape calls (getters etc)
 */
#define DUK__VALSTACK_SPACE             10

/* valstack space allocated especially for proxy lookup which does a
 * recursive property lookup
 */
#define DUK__VALSTACK_PROXY_LOOKUP      20

/*
 *  Local prototypes
 */

#define DUK__DESC_FLAG_PUSH_VALUE          (1 << 0)  /* push value to stack */
#define DUK__DESC_FLAG_IGNORE_PROTOLOOP    (1 << 1)  /* don't throw for prototype loop */

DUK_LOCAL_DECL duk_bool_t duk__check_arguments_map_for_get(duk_hthread *thr, duk_hobject *obj, duk_hstring *key, duk_propdesc *temp_desc);
DUK_LOCAL_DECL void duk__check_arguments_map_for_put(duk_hthread *thr, duk_hobject *obj, duk_hstring *key, duk_propdesc *temp_desc, duk_bool_t throw_flag);
DUK_LOCAL_DECL void duk__check_arguments_map_for_delete(duk_hthread *thr, duk_hobject *obj, duk_hstring *key, duk_propdesc *temp_desc);

DUK_LOCAL_DECL duk_bool_t duk__handle_put_array_length_smaller(duk_hthread *thr, duk_hobject *obj, duk_uint32_t old_len, duk_uint32_t new_len, duk_bool_t force_flag, duk_uint32_t *out_result_len);
DUK_LOCAL_DECL duk_bool_t duk__handle_put_array_length(duk_hthread *thr, duk_hobject *obj);

DUK_LOCAL_DECL duk_bool_t duk__get_property_desc(duk_hthread *thr, duk_hobject *obj, duk_hstring *key, duk_propdesc *out_desc, duk_small_uint_t flags);
DUK_LOCAL_DECL duk_bool_t duk__get_own_property_desc_raw(duk_hthread *thr, duk_hobject *obj, duk_hstring *key, duk_uint32_t arr_idx, duk_propdesc *out_desc, duk_small_uint_t flags);
DUK_LOCAL_DECL duk_bool_t duk__get_own_property_desc(duk_hthread *thr, duk_hobject *obj, duk_hstring *key, duk_propdesc *out_desc, duk_small_uint_t flags);

/*
 *  Misc helpers
 */

/* Convert a duk_tval number (caller checks) to a 32-bit index.  Returns
 * DUK__NO_ARRAY_INDEX if the number is not whole or not a valid array
 * index.
 */
DUK_LOCAL duk_uint32_t duk__tval_number_to_arr_idx(duk_tval *tv) {
	duk_double_t dbl;
	duk_uint32_t idx;

	DUK_ASSERT(tv != NULL);
	DUK_ASSERT(DUK_TVAL_IS_NUMBER(tv));

	dbl = DUK_TVAL_GET_NUMBER(tv);
	idx = (duk_uint32_t) dbl;
	if ((duk_double_t) idx == dbl) {
	        /* Is whole and within 32 bit range.  If the value happens to be 0xFFFFFFFF,
		 * it's not a valid array index but will then match DUK__NO_ARRAY_INDEX.
		 */
		return idx;
	}
	return DUK__NO_ARRAY_INDEX;
}

/* Push an arbitrary duk_tval to the stack, coerce it to string, and return
 * both a duk_hstring pointer and an array index (or DUK__NO_ARRAY_INDEX).
 */
DUK_LOCAL duk_uint32_t duk__push_tval_to_hstring_arr_idx(duk_context *ctx, duk_tval *tv, duk_hstring **out_h) {
	duk_uint32_t arr_idx;
	duk_hstring *h;

	DUK_ASSERT(ctx != NULL);
	DUK_ASSERT(tv != NULL);
	DUK_ASSERT(out_h != NULL);

	duk_push_tval(ctx, tv);
	duk_to_string(ctx, -1);
	h = duk_get_hstring(ctx, -1);
	DUK_ASSERT(h != NULL);
	*out_h = h;

	arr_idx = DUK_HSTRING_GET_ARRIDX_FAST(h);
	return arr_idx;
}

/* String is an own (virtual) property of a lightfunc. */
DUK_LOCAL duk_bool_t duk__key_is_lightfunc_ownprop(duk_hthread *thr, duk_hstring *key) {
	return (key == DUK_HTHREAD_STRING_LENGTH(thr) ||
	        key == DUK_HTHREAD_STRING_NAME(thr));
}

/*
 *  Helpers for managing property storage size
 */

/* Get default hash part size for a certain entry part size. */
#if defined(DUK_USE_HOBJECT_HASH_PART)
DUK_LOCAL duk_uint32_t duk__get_default_h_size(duk_uint32_t e_size) {
	DUK_ASSERT(e_size <= DUK_HOBJECT_MAX_PROPERTIES);

	if (e_size >= DUK_HOBJECT_E_USE_HASH_LIMIT) {
		duk_uint32_t res;

		/* result: hash_prime(floor(1.2 * e_size)) */
		res = duk_util_get_hash_prime(e_size + e_size / DUK_HOBJECT_H_SIZE_DIVISOR);

		/* if fails, e_size will be zero = not an issue, except performance-wise */
		DUK_ASSERT(res == 0 || res > e_size);
		return res;
	} else {
		return 0;
	}
}
#endif  /* USE_PROP_HASH_PART */

/* Get minimum entry part growth for a certain size. */
DUK_LOCAL duk_uint32_t duk__get_min_grow_e(duk_uint32_t e_size) {
	duk_uint32_t res;

	DUK_ASSERT(e_size <= DUK_HOBJECT_MAX_PROPERTIES);

	res = (e_size + DUK_HOBJECT_E_MIN_GROW_ADD) / DUK_HOBJECT_E_MIN_GROW_DIVISOR;
	DUK_ASSERT(res >= 1);  /* important for callers */
	return res;
}

/* Get minimum array part growth for a certain size. */
DUK_LOCAL duk_uint32_t duk__get_min_grow_a(duk_uint32_t a_size) {
	duk_uint32_t res;

	DUK_ASSERT((duk_size_t) a_size <= DUK_HOBJECT_MAX_PROPERTIES);

	res = (a_size + DUK_HOBJECT_A_MIN_GROW_ADD) / DUK_HOBJECT_A_MIN_GROW_DIVISOR;
	DUK_ASSERT(res >= 1);  /* important for callers */
	return res;
}

/* Count actually used entry part entries (non-NULL keys). */
DUK_LOCAL duk_uint32_t duk__count_used_e_keys(duk_hthread *thr, duk_hobject *obj) {
	duk_uint_fast32_t i;
	duk_uint_fast32_t n = 0;
	duk_hstring **e;

	DUK_ASSERT(obj != NULL);
	DUK_UNREF(thr);

	e = DUK_HOBJECT_E_GET_KEY_BASE(thr->heap, obj);
	for (i = 0; i < DUK_HOBJECT_GET_ENEXT(obj); i++) {
		if (*e++) {
			n++;
		}
	}
	return (duk_uint32_t) n;
}

/* Count actually used array part entries and array minimum size.
 * NOTE: 'out_min_size' can be computed much faster by starting from the
 * end and breaking out early when finding first used entry, but this is
 * not needed now.
 */
DUK_LOCAL void duk__compute_a_stats(duk_hthread *thr, duk_hobject *obj, duk_uint32_t *out_used, duk_uint32_t *out_min_size) {
	duk_uint_fast32_t i;
	duk_uint_fast32_t used = 0;
	duk_uint_fast32_t highest_idx = (duk_uint_fast32_t) -1;  /* see below */
	duk_tval *a;

	DUK_ASSERT(obj != NULL);
	DUK_ASSERT(out_used != NULL);
	DUK_ASSERT(out_min_size != NULL);
	DUK_UNREF(thr);

	a = DUK_HOBJECT_A_GET_BASE(thr->heap, obj);
	for (i = 0; i < DUK_HOBJECT_GET_ASIZE(obj); i++) {
		duk_tval *tv = a++;
		if (!DUK_TVAL_IS_UNDEFINED_UNUSED(tv)) {
			used++;
			highest_idx = i;
		}
	}

	/* Initial value for highest_idx is -1 coerced to unsigned.  This
	 * is a bit odd, but (highest_idx + 1) will then wrap to 0 below
	 * for out_min_size as intended.
	 */

	*out_used = used;
	*out_min_size = highest_idx + 1;  /* 0 if no used entries */
}

/* Check array density and indicate whether or not the array part should be abandoned. */
DUK_LOCAL duk_bool_t duk__abandon_array_density_check(duk_uint32_t a_used, duk_uint32_t a_size) {
	/*
	 *  Array abandon check; abandon if:
	 *
	 *    new_used / new_size < limit
	 *    new_used < limit * new_size        || limit is 3 bits fixed point
	 *    new_used < limit' / 8 * new_size   || *8
	 *    8*new_used < limit' * new_size     || :8
	 *    new_used < limit' * (new_size / 8)
	 *
	 *  Here, new_used = a_used, new_size = a_size.
	 *
	 *  Note: some callers use approximate values for a_used and/or a_size
	 *  (e.g. dropping a '+1' term).  This doesn't affect the usefulness
	 *  of the check, but may confuse debugging.
	 */

	return (a_used < DUK_HOBJECT_A_ABANDON_LIMIT * (a_size >> 3));
}

/* Fast check for extending array: check whether or not a slow density check is required. */
DUK_LOCAL duk_bool_t duk__abandon_array_slow_check_required(duk_uint32_t arr_idx, duk_uint32_t old_size) {
	/*
	 *  In a fast check we assume old_size equals old_used (i.e., existing
	 *  array is fully dense).
	 *
	 *  Slow check if:
	 *
	 *    (new_size - old_size) / old_size > limit
	 *    new_size - old_size > limit * old_size
	 *    new_size > (1 + limit) * old_size        || limit' is 3 bits fixed point
	 *    new_size > (1 + (limit' / 8)) * old_size || * 8
	 *    8 * new_size > (8 + limit') * old_size   || : 8
	 *    new_size > (8 + limit') * (old_size / 8)
	 *    new_size > limit'' * (old_size / 8)      || limit'' = 9 -> max 25% increase
	 *    arr_idx + 1 > limit'' * (old_size / 8)
	 *
	 *  This check doesn't work well for small values, so old_size is rounded
	 *  up for the check (and the '+ 1' of arr_idx can be ignored in practice):
	 *
	 *    arr_idx > limit'' * ((old_size + 7) / 8)
	 */

	return (arr_idx > DUK_HOBJECT_A_FAST_RESIZE_LIMIT * ((old_size + 7) >> 3));
}

/*
 *  Proxy helpers
 */

#if defined(DUK_USE_ES6_PROXY)
DUK_INTERNAL duk_bool_t duk_hobject_proxy_check(duk_hthread *thr, duk_hobject *obj, duk_hobject **out_target, duk_hobject **out_handler) {
	duk_tval *tv_target;
	duk_tval *tv_handler;
	duk_hobject *h_target;
	duk_hobject *h_handler;

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(obj != NULL);
	DUK_ASSERT(out_target != NULL);
	DUK_ASSERT(out_handler != NULL);

	/* Caller doesn't need to check exotic proxy behavior (but does so for
	 * some fast paths).
	 */
	if (DUK_LIKELY(!DUK_HOBJECT_HAS_EXOTIC_PROXYOBJ(obj))) {
		return 0;
	}

	tv_handler = duk_hobject_find_existing_entry_tval_ptr(thr->heap, obj, DUK_HTHREAD_STRING_INT_HANDLER(thr));
	if (!tv_handler) {
		DUK_ERROR(thr, DUK_ERR_TYPE_ERROR, DUK_STR_PROXY_REVOKED);
		return 0;
	}
	DUK_ASSERT(DUK_TVAL_IS_OBJECT(tv_handler));
	h_handler = DUK_TVAL_GET_OBJECT(tv_handler);
	DUK_ASSERT(h_handler != NULL);
	*out_handler = h_handler;
	tv_handler = NULL;  /* avoid issues with relocation */

	tv_target = duk_hobject_find_existing_entry_tval_ptr(thr->heap, obj, DUK_HTHREAD_STRING_INT_TARGET(thr));
	if (!tv_target) {
		DUK_ERROR(thr, DUK_ERR_TYPE_ERROR, DUK_STR_PROXY_REVOKED);
		return 0;
	}
	DUK_ASSERT(DUK_TVAL_IS_OBJECT(tv_target));
	h_target = DUK_TVAL_GET_OBJECT(tv_target);
	DUK_ASSERT(h_target != NULL);
	*out_target = h_target;
	tv_target = NULL;  /* avoid issues with relocation */

	return 1;
}
#endif

#if defined(DUK_USE_ES6_PROXY)
DUK_LOCAL duk_bool_t duk__proxy_check_prop(duk_hthread *thr, duk_hobject *obj, duk_small_uint_t stridx_trap, duk_tval *tv_key, duk_hobject **out_target) {
	duk_context *ctx = (duk_context *) thr;
	duk_hobject *h_handler;

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(obj != NULL);
	DUK_ASSERT(tv_key != NULL);
	DUK_ASSERT(out_target != NULL);

	if (!duk_hobject_proxy_check(thr, obj, out_target, &h_handler)) {
		return 0;
	}
	DUK_ASSERT(*out_target != NULL);
	DUK_ASSERT(h_handler != NULL);

	/* XXX: At the moment Duktape accesses internal keys like _Finalizer using a
	 * normal property set/get which would allow a proxy handler to interfere with
	 * such behavior and to get access to internal key strings.  This is not a problem
	 * as such because internal key strings can be created in other ways too (e.g.
	 * through buffers).  The best fix is to change Duktape internal lookups to
	 * skip proxy behavior.  Until that, internal property accesses bypass the
	 * proxy and are applied to the target (as if the handler did not exist).
	 * This has some side effects, see test-bi-proxy-internal-keys.js.
	 */

	if (DUK_TVAL_IS_STRING(tv_key)) {
		duk_hstring *h_key = (duk_hstring *) DUK_TVAL_GET_STRING(tv_key);
		DUK_ASSERT(h_key != NULL);
		if (DUK_HSTRING_HAS_INTERNAL(h_key)) {
			DUK_DDD(DUK_DDDPRINT("internal key, skip proxy handler and apply to target"));
			return 0;
		}
	}

	/* The handler is looked up with a normal property lookup; it may be an
	 * accessor or the handler object itself may be a proxy object.  If the
	 * handler is a proxy, we need to extend the valstack as we make a
	 * recursive proxy check without a function call in between (in fact
	 * there is no limit to the potential recursion here).
	 *
	 * (For sanity, proxy creation rejects another proxy object as either
	 * the handler or the target at the moment so recursive proxy cases
	 * are not realized now.)
	 */

	/* XXX: C recursion limit if proxies are allowed as handler/target values */

	duk_require_stack(ctx, DUK__VALSTACK_PROXY_LOOKUP);
	duk_push_hobject(ctx, h_handler);
	if (duk_get_prop_stridx(ctx, -1, stridx_trap)) {
		/* -> [ ... handler trap ] */
		duk_insert(ctx, -2);  /* -> [ ... trap handler ] */

		/* stack prepped for func call: [ ... trap handler ] */
		return 1;
	} else {
		duk_pop_2(ctx);
		return 0;
	}
}
#endif  /* DUK_USE_ES6_PROXY */

/*
 *  Reallocate property allocation, moving properties to the new allocation.
 *
 *  Includes key compaction, rehashing, and can also optionally abandoning
 *  the array part, 'migrating' array entries into the beginning of the
 *  new entry part.  Arguments are not validated here, so e.g. new_h_size
 *  MUST be a valid prime.
 *
 *  There is no support for in-place reallocation or just compacting keys
 *  without resizing the property allocation.  This is intentional to keep
 *  code size minimal.
 *
 *  The implementation is relatively straightforward, except for the array
 *  abandonment process.  Array abandonment requires that new string keys
 *  are interned, which may trigger GC.  All keys interned so far must be
 *  reachable for GC at all times; valstack is used for that now.
 *
 *  Also, a GC triggered during this reallocation process must not interfere
 *  with the object being resized.  This is currently controlled by using
 *  heap->mark_and_sweep_base_flags to indicate that no finalizers will be
 *  executed (as they can affect ANY object) and no objects are compacted
 *  (it would suffice to protect this particular object only, though).
 *
 *  Note: a non-checked variant would be nice but is a bit tricky to
 *  implement for the array abandonment process.  It's easy for
 *  everything else.
 *
 *  Note: because we need to potentially resize the valstack (as part
 *  of abandoning the array part), any tval pointers to the valstack
 *  will become invalid after this call.
 */

DUK_LOCAL
void duk__realloc_props(duk_hthread *thr,
                        duk_hobject *obj,
                        duk_uint32_t new_e_size,
                        duk_uint32_t new_a_size,
                        duk_uint32_t new_h_size,
                        duk_bool_t abandon_array) {
	duk_context *ctx = (duk_context *) thr;
#ifdef DUK_USE_MARK_AND_SWEEP
	duk_small_uint_t prev_mark_and_sweep_base_flags;
#endif
	duk_uint32_t new_alloc_size;
	duk_uint32_t new_e_size_adjusted;
	duk_uint8_t *new_p;
	duk_hstring **new_e_k;
	duk_propvalue *new_e_pv;
	duk_uint8_t *new_e_f;
	duk_tval *new_a;
	duk_uint32_t *new_h;
	duk_uint32_t new_e_next;
	duk_uint_fast32_t i;

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(ctx != NULL);
	DUK_ASSERT(obj != NULL);
	DUK_ASSERT(!abandon_array || new_a_size == 0);  /* if abandon_array, new_a_size must be 0 */
	DUK_ASSERT(DUK_HOBJECT_GET_PROPS(thr->heap, obj) != NULL || (DUK_HOBJECT_GET_ESIZE(obj) == 0 && DUK_HOBJECT_GET_ASIZE(obj) == 0));
	DUK_ASSERT(new_h_size == 0 || new_h_size >= new_e_size);  /* required to guarantee success of rehashing,
	                                                           * intentionally use unadjusted new_e_size
	                                                           */
	DUK_ASSERT_VALSTACK_SPACE(thr, DUK__VALSTACK_SPACE);

	/*
	 *  Pre resize assertions.
	 */

#ifdef DUK_USE_ASSERTIONS
	/* XXX: pre-checks (such as no duplicate keys) */
#endif

	/*
	 *  For property layout 1, tweak e_size to ensure that the whole entry
	 *  part (key + val + flags) is a suitable multiple for alignment
	 *  (platform specific).
	 *
	 *  Property layout 2 does not require this tweaking and is preferred
	 *  on low RAM platforms requiring alignment.
	 */

#if defined(DUK_USE_HOBJECT_LAYOUT_2) || defined(DUK_USE_HOBJECT_LAYOUT_3)
	DUK_DDD(DUK_DDDPRINT("using layout 2 or 3, no need to pad e_size: %ld", (long) new_e_size));
	new_e_size_adjusted = new_e_size;
#elif defined(DUK_USE_HOBJECT_LAYOUT_1) && (DUK_HOBJECT_ALIGN_TARGET == 1)
	DUK_DDD(DUK_DDDPRINT("using layout 1, but no need to pad e_size: %ld", (long) new_e_size));
	new_e_size_adjusted = new_e_size;
#elif defined(DUK_USE_HOBJECT_LAYOUT_1) && ((DUK_HOBJECT_ALIGN_TARGET == 4) || (DUK_HOBJECT_ALIGN_TARGET == 8))
	new_e_size_adjusted = (new_e_size + DUK_HOBJECT_ALIGN_TARGET - 1) & (~(DUK_HOBJECT_ALIGN_TARGET - 1));
	DUK_DDD(DUK_DDDPRINT("using layout 1, and alignment target is %ld, adjusted e_size: %ld -> %ld",
	                     (long) DUK_HOBJECT_ALIGN_TARGET, (long) new_e_size, (long) new_e_size_adjusted));
	DUK_ASSERT(new_e_size_adjusted >= new_e_size);
#else
#error invalid hobject layout defines
#endif

	/*
	 *  Debug logging after adjustment.
	 */

	DUK_DDD(DUK_DDDPRINT("attempt to resize hobject %p props (%ld -> %ld bytes), from {p=%p,e_size=%ld,e_next=%ld,a_size=%ld,h_size=%ld} to "
	                     "{e_size=%ld,a_size=%ld,h_size=%ld}, abandon_array=%ld, unadjusted new_e_size=%ld",
	                     (void *) obj,
	                     (long) DUK_HOBJECT_P_COMPUTE_SIZE(DUK_HOBJECT_GET_ESIZE(obj),
	                                                       DUK_HOBJECT_GET_ASIZE(obj),
	                                                       DUK_HOBJECT_GET_HSIZE(obj)),
	                     (long) DUK_HOBJECT_P_COMPUTE_SIZE(new_e_size_adjusted, new_a_size, new_h_size),
	                     (void *) DUK_HOBJECT_GET_PROPS(thr->heap, obj),
	                     (long) DUK_HOBJECT_GET_ESIZE(obj),
	                     (long) DUK_HOBJECT_GET_ENEXT(obj),
	                     (long) DUK_HOBJECT_GET_ASIZE(obj),
	                     (long) DUK_HOBJECT_GET_HSIZE(obj),
	                     (long) new_e_size_adjusted,
	                     (long) new_a_size,
	                     (long) new_h_size,
	                     (long) abandon_array,
	                     (long) new_e_size));

	/*
	 *  Property count check.  This is the only point where we ensure that
	 *  we don't get more (allocated) property space that we can handle.
	 *  There aren't hard limits as such, but some algorithms fail (e.g.
	 *  finding next higher prime, selecting hash part size) if we get too
	 *  close to the 4G property limit.
	 *
	 *  Since this works based on allocation size (not actually used size),
	 *  the limit is a bit approximate but good enough in practice.
	 */

	if (new_e_size_adjusted + new_a_size > DUK_HOBJECT_MAX_PROPERTIES) {
		DUK_ERROR(thr, DUK_ERR_ALLOC_ERROR, DUK_STR_OBJECT_PROPERTY_LIMIT);
	}

	/*
	 *  Compute new alloc size and alloc new area.
	 *
	 *  The new area is allocated as a dynamic buffer and placed into the
	 *  valstack for reachability.  The actual buffer is then detached at
	 *  the end.
	 *
	 *  Note: heap_mark_and_sweep_base_flags are altered here to ensure
	 *  no-one touches this object while we're resizing and rehashing it.
	 *  The flags must be reset on every exit path after it.  Finalizers
	 *  and compaction is prevented currently for all objects while it
	 *  would be enough to restrict it only for the current object.
	 */

#ifdef DUK_USE_MARK_AND_SWEEP
	prev_mark_and_sweep_base_flags = thr->heap->mark_and_sweep_base_flags;
	thr->heap->mark_and_sweep_base_flags |=
                DUK_MS_FLAG_NO_FINALIZERS |         /* avoid attempts to add/remove object keys */
	        DUK_MS_FLAG_NO_OBJECT_COMPACTION;   /* avoid attempt to compact the current object */
#endif

	new_alloc_size = DUK_HOBJECT_P_COMPUTE_SIZE(new_e_size_adjusted, new_a_size, new_h_size);
	DUK_DDD(DUK_DDDPRINT("new hobject allocation size is %ld", (long) new_alloc_size));
	if (new_alloc_size == 0) {
		/* for zero size, don't push anything on valstack */
		DUK_ASSERT(new_e_size_adjusted == 0);
		DUK_ASSERT(new_a_size == 0);
		DUK_ASSERT(new_h_size == 0);
		new_p = NULL;
	} else {
		/* This may trigger mark-and-sweep with arbitrary side effects,
		 * including an attempted resize of the object we're resizing,
		 * executing a finalizer which may add or remove properties of
		 * the object we're resizing etc.
		 */

		/* Note: buffer is dynamic so that we can 'steal' the actual
		 * allocation later.
		 */

		new_p = (duk_uint8_t *) duk_push_dynamic_buffer(ctx, new_alloc_size);  /* errors out if out of memory */
		DUK_ASSERT(new_p != NULL);  /* since new_alloc_size > 0 */
	}

	/* Set up pointers to the new property area: this is hidden behind a macro
	 * because it is memory layout specific.
	 */
	DUK_HOBJECT_P_SET_REALLOC_PTRS(new_p, new_e_k, new_e_pv, new_e_f, new_a, new_h,
	                               new_e_size_adjusted, new_a_size, new_h_size);
	DUK_UNREF(new_h);  /* happens when hash part dropped */
	new_e_next = 0;

	/* if new_p == NULL, all of these pointers are NULL */
	DUK_ASSERT((new_p != NULL) ||
	           (new_e_k == NULL && new_e_pv == NULL && new_e_f == NULL &&
	            new_a == NULL && new_h == NULL));

	DUK_DDD(DUK_DDDPRINT("new alloc size %ld, new_e_k=%p, new_e_pv=%p, new_e_f=%p, new_a=%p, new_h=%p",
	                     (long) new_alloc_size, (void *) new_e_k, (void *) new_e_pv, (void *) new_e_f,
	                     (void *) new_a, (void *) new_h));

	/*
	 *  Migrate array to start of entries if requested.
	 *
	 *  Note: from an enumeration perspective the order of entry keys matters.
	 *  Array keys should appear wherever they appeared before the array abandon
	 *  operation.
	 */

	if (abandon_array) {
		/*
		 *  Note: assuming new_a_size == 0, and that entry part contains
		 *  no conflicting keys, refcounts do not need to be adjusted for
		 *  the values, as they remain exactly the same.
		 *
		 *  The keys, however, need to be interned, incref'd, and be
		 *  reachable for GC.  Any intern attempt may trigger a GC and
		 *  claim any non-reachable strings, so every key must be reachable
		 *  at all times.
		 *
		 *  A longjmp must not occur here, as the new_p allocation would
		 *  be freed without these keys being decref'd, hence the messy
		 *  decref handling if intern fails.
		 */
		DUK_ASSERT(new_a_size == 0);

		for (i = 0; i < DUK_HOBJECT_GET_ASIZE(obj); i++) {
			duk_tval *tv1;
			duk_tval *tv2;
			duk_hstring *key;

			DUK_ASSERT(DUK_HOBJECT_GET_PROPS(thr->heap, obj) != NULL);

			tv1 = DUK_HOBJECT_A_GET_VALUE_PTR(thr->heap, obj, i);
			if (DUK_TVAL_IS_UNDEFINED_UNUSED(tv1)) {
				continue;
			}

			DUK_ASSERT(new_p != NULL && new_e_k != NULL &&
			           new_e_pv != NULL && new_e_f != NULL);

			/*
			 *  Intern key via the valstack to ensure reachability behaves
			 *  properly.  We must avoid longjmp's here so use non-checked
			 *  primitives.
			 *
			 *  Note: duk_check_stack() potentially reallocs the valstack,
			 *  invalidating any duk_tval pointers to valstack.  Callers
			 *  must be careful.
			 */

			/* never shrinks; auto-adds DUK_VALSTACK_INTERNAL_EXTRA, which is generous */
			if (!duk_check_stack(ctx, 1)) {
				goto abandon_error;
			}
			DUK_ASSERT_VALSTACK_SPACE(thr, 1);
			key = duk_heap_string_intern_u32(thr->heap, i);
			if (!key) {
				goto abandon_error;
			}
			duk_push_hstring(ctx, key);  /* keep key reachable for GC etc; guaranteed not to fail */

			/* key is now reachable in the valstack */

			DUK_HSTRING_INCREF(thr, key);   /* second incref for the entry reference */
			new_e_k[new_e_next] = key;
			tv2 = &new_e_pv[new_e_next].v;  /* array entries are all plain values */
			DUK_TVAL_SET_TVAL(tv2, tv1);
			new_e_f[new_e_next] = DUK_PROPDESC_FLAG_WRITABLE |
			                      DUK_PROPDESC_FLAG_ENUMERABLE |
			                      DUK_PROPDESC_FLAG_CONFIGURABLE;
			new_e_next++;

			/* Note: new_e_next matches pushed temp key count, and nothing can
			 * fail above between the push and this point.
			 */
		}

		DUK_DDD(DUK_DDDPRINT("abandon array: pop %ld key temps from valstack", (long) new_e_next));
		duk_pop_n(ctx, new_e_next);
	}

	/*
	 *  Copy keys and values in the entry part (compacting them at the same time).
	 */

	for (i = 0; i < DUK_HOBJECT_GET_ENEXT(obj); i++) {
		duk_hstring *key;

		DUK_ASSERT(DUK_HOBJECT_GET_PROPS(thr->heap, obj) != NULL);

		key = DUK_HOBJECT_E_GET_KEY(thr->heap, obj, i);
		if (!key) {
			continue;
		}

		DUK_ASSERT(new_p != NULL && new_e_k != NULL &&
		           new_e_pv != NULL && new_e_f != NULL);

		new_e_k[new_e_next] = key;
		new_e_pv[new_e_next] = DUK_HOBJECT_E_GET_VALUE(thr->heap, obj, i);
		new_e_f[new_e_next] = DUK_HOBJECT_E_GET_FLAGS(thr->heap, obj, i);
		new_e_next++;
	}
	/* the entries [new_e_next, new_e_size_adjusted[ are left uninitialized on purpose (ok, not gc reachable) */

	/*
	 *  Copy array elements to new array part.
	 */

	if (new_a_size > DUK_HOBJECT_GET_ASIZE(obj)) {
		/* copy existing entries as is */
		DUK_ASSERT(new_p != NULL && new_a != NULL);
		if (DUK_HOBJECT_GET_ASIZE(obj) > 0) {
			/* Avoid zero copy with an invalid pointer.  If obj->p is NULL,
			 * the 'new_a' pointer will be invalid which is not allowed even
			 * when copy size is zero.
			 */
			DUK_ASSERT(DUK_HOBJECT_GET_PROPS(thr->heap, obj) != NULL);
			DUK_ASSERT(DUK_HOBJECT_GET_ASIZE(obj) > 0);
			DUK_MEMCPY((void *) new_a, (void *) DUK_HOBJECT_A_GET_BASE(thr->heap, obj), sizeof(duk_tval) * DUK_HOBJECT_GET_ASIZE(obj));
		}

		/* fill new entries with -unused- (required, gc reachable) */
		for (i = DUK_HOBJECT_GET_ASIZE(obj); i < new_a_size; i++) {
			duk_tval *tv = &new_a[i];
			DUK_TVAL_SET_UNDEFINED_UNUSED(tv);
		}
	} else {
#ifdef DUK_USE_ASSERTIONS
		/* caller must have decref'd values above new_a_size (if that is necessary) */
		if (!abandon_array) {
			for (i = new_a_size; i < DUK_HOBJECT_GET_ASIZE(obj); i++) {
				duk_tval *tv;
				tv = DUK_HOBJECT_A_GET_VALUE_PTR(thr->heap, obj, i);

				/* current assertion is quite strong: decref's and set to unused */
				DUK_ASSERT(DUK_TVAL_IS_UNDEFINED_UNUSED(tv));
			}
		}
#endif
		if (new_a_size > 0) {
			/* Avoid zero copy with an invalid pointer.  If obj->p is NULL,
			 * the 'new_a' pointer will be invalid which is not allowed even
			 * when copy size is zero.
			 */
			DUK_ASSERT(DUK_HOBJECT_GET_PROPS(thr->heap, obj) != NULL);
			DUK_ASSERT(new_a_size > 0);
			DUK_MEMCPY((void *) new_a, (void *) DUK_HOBJECT_A_GET_BASE(thr->heap, obj), sizeof(duk_tval) * new_a_size);
		}
	}

	/*
	 *  Rebuild the hash part always from scratch (guaranteed to finish).
	 *
	 *  Any resize of hash part requires rehashing.  In addition, by rehashing
	 *  get rid of any elements marked deleted (DUK__HASH_DELETED) which is critical
	 *  to ensuring the hash part never fills up.
	 */

#if defined(DUK_USE_HOBJECT_HASH_PART)
	if (DUK_UNLIKELY(new_h_size > 0)) {
		DUK_ASSERT(new_h != NULL);

		/* fill new_h with u32 0xff = UNUSED */
		DUK_ASSERT(DUK_HOBJECT_GET_PROPS(thr->heap, obj) != NULL);
		DUK_ASSERT(new_h_size > 0);
		DUK_MEMSET(new_h, 0xff, sizeof(duk_uint32_t) * new_h_size);

		DUK_ASSERT(new_e_next <= new_h_size);  /* equality not actually possible */
		for (i = 0; i < new_e_next; i++) {
			duk_hstring *key = new_e_k[i];
			duk_uint32_t j, step;

			DUK_ASSERT(key != NULL);
			j = DUK__HASH_INITIAL(DUK_HSTRING_GET_HASH(key), new_h_size);
			step = DUK__HASH_PROBE_STEP(DUK_HSTRING_GET_HASH(key));

			for (;;) {
				DUK_ASSERT(new_h[j] != DUK__HASH_DELETED);  /* should never happen */
				if (new_h[j] == DUK__HASH_UNUSED) {
					DUK_DDD(DUK_DDDPRINT("rebuild hit %ld -> %ld", (long) j, (long) i));
					new_h[j] = i;
					break;
				}
				DUK_DDD(DUK_DDDPRINT("rebuild miss %ld, step %ld", (long) j, (long) step));
				j = (j + step) % new_h_size;

				/* guaranteed to finish */
				DUK_ASSERT(j != (duk_uint32_t) DUK__HASH_INITIAL(DUK_HSTRING_GET_HASH(key), new_h_size));
			}
		}
	} else {
		DUK_DDD(DUK_DDDPRINT("no hash part, no rehash"));
	}
#endif  /* DUK_USE_HOBJECT_HASH_PART */

	/*
	 *  Nice debug log.
	 */

	DUK_DD(DUK_DDPRINT("resized hobject %p props (%ld -> %ld bytes), from {p=%p,e_size=%ld,e_next=%ld,a_size=%ld,h_size=%ld} to "
	                   "{p=%p,e_size=%ld,e_next=%ld,a_size=%ld,h_size=%ld}, abandon_array=%ld, unadjusted new_e_size=%ld",
	                   (void *) obj,
	                   (long) DUK_HOBJECT_P_COMPUTE_SIZE(DUK_HOBJECT_GET_ESIZE(obj),
	                                                     DUK_HOBJECT_GET_ASIZE(obj),
	                                                     DUK_HOBJECT_GET_HSIZE(obj)),
	                   (long) new_alloc_size,
	                   (void *) DUK_HOBJECT_GET_PROPS(thr->heap, obj),
	                   (long) DUK_HOBJECT_GET_ESIZE(obj),
	                   (long) DUK_HOBJECT_GET_ENEXT(obj),
	                   (long) DUK_HOBJECT_GET_ASIZE(obj),
	                   (long) DUK_HOBJECT_GET_HSIZE(obj),
	                   (void *) new_p,
	                   (long) new_e_size_adjusted,
	                   (long) new_e_next,
	                   (long) new_a_size,
	                   (long) new_h_size,
	                   (long) abandon_array,
	                   (long) new_e_size));

	/*
	 *  All done, switch properties ('p') allocation to new one.
	 */

	DUK_FREE(thr->heap, DUK_HOBJECT_GET_PROPS(thr->heap, obj));  /* NULL obj->p is OK */
	DUK_HOBJECT_SET_PROPS(thr->heap, obj, new_p);
	DUK_HOBJECT_SET_ESIZE(obj, new_e_size_adjusted);
	DUK_HOBJECT_SET_ENEXT(obj, new_e_next);
	DUK_HOBJECT_SET_ASIZE(obj, new_a_size);
	DUK_HOBJECT_SET_HSIZE(obj, new_h_size);

	if (new_p) {
		/*
		 *  Detach actual buffer from dynamic buffer in valstack, and
		 *  pop it from the stack.
		 *
		 *  XXX: the buffer object is certainly not reachable at this point,
		 *  so it would be nice to free it forcibly even with only
		 *  mark-and-sweep enabled.  Not a big issue though.
		 */
		duk_hbuffer_dynamic *buf;
		DUK_ASSERT(new_alloc_size > 0);
		DUK_ASSERT(duk_is_buffer(ctx, -1));
		buf = (duk_hbuffer_dynamic *) duk_require_hbuffer(ctx, -1);
		DUK_ASSERT(buf != NULL);
		DUK_ASSERT(DUK_HBUFFER_HAS_DYNAMIC(buf));
		DUK_HBUFFER_DYNAMIC_SET_DATA_PTR_NULL(thr->heap, buf);
		DUK_HBUFFER_DYNAMIC_SET_SIZE(buf, 0);  /* these size resets are not strictly necessary, but nice for consistency */
		DUK_HBUFFER_DYNAMIC_SET_ALLOC_SIZE(buf, 0);
		duk_pop(ctx);
	} else {
		DUK_ASSERT(new_alloc_size == 0);
		/* no need to pop, nothing was pushed */
	}

	/* clear array part flag only after switching */
	if (abandon_array) {
		DUK_HOBJECT_CLEAR_ARRAY_PART(obj);
	}

	DUK_DDD(DUK_DDDPRINT("resize result: %!O", (duk_heaphdr *) obj));

#ifdef DUK_USE_MARK_AND_SWEEP
	thr->heap->mark_and_sweep_base_flags = prev_mark_and_sweep_base_flags;
#endif

	/*
	 *  Post resize assertions.
	 */

#ifdef DUK_USE_ASSERTIONS
	/* XXX: post-checks (such as no duplicate keys) */
#endif
	return;

	/*
	 *  Abandon array failed, need to decref keys already inserted
	 *  into the beginning of new_e_k before unwinding valstack.
	 */

 abandon_error:
	DUK_D(DUK_DPRINT("hobject resize failed during abandon array, decref keys"));
	i = new_e_next;
	while (i > 0) {
		i--;
		DUK_ASSERT(new_e_k != NULL);
		DUK_ASSERT(new_e_k[i] != NULL);
		DUK_HSTRING_DECREF(thr, new_e_k[i]);
	}

#ifdef DUK_USE_MARK_AND_SWEEP
	thr->heap->mark_and_sweep_base_flags = prev_mark_and_sweep_base_flags;
#endif

	DUK_ERROR(thr, DUK_ERR_ALLOC_ERROR, DUK_STR_OBJECT_RESIZE_FAILED);
}

/*
 *  Helpers to resize properties allocation on specific needs.
 */

/* Grow entry part allocation for one additional entry. */
DUK_LOCAL void duk__grow_props_for_new_entry_item(duk_hthread *thr, duk_hobject *obj) {
	duk_uint32_t old_e_used;  /* actually used, non-NULL entries */
	duk_uint32_t new_e_size;
	duk_uint32_t new_a_size;
	duk_uint32_t new_h_size;

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(obj != NULL);

	/* Duktape 0.11.0 and prior tried to optimize the resize by not
	 * counting the number of actually used keys prior to the resize.
	 * This worked mostly well but also caused weird leak-like behavior
	 * as in: test-bug-object-prop-alloc-unbounded.js.  So, now we count
	 * the keys explicitly to compute the new entry part size.
	 */

	old_e_used = duk__count_used_e_keys(thr, obj);
	new_e_size = old_e_used + duk__get_min_grow_e(old_e_used);
#if defined(DUK_USE_HOBJECT_HASH_PART)
	new_h_size = duk__get_default_h_size(new_e_size);
#else
	new_h_size = 0;
#endif
	new_a_size = DUK_HOBJECT_GET_ASIZE(obj);
	DUK_ASSERT(new_e_size >= old_e_used + 1);  /* duk__get_min_grow_e() is always >= 1 */

	duk__realloc_props(thr, obj, new_e_size, new_a_size, new_h_size, 0);
}

/* Grow array part for a new highest array index. */
DUK_LOCAL void duk__grow_props_for_array_item(duk_hthread *thr, duk_hobject *obj, duk_uint32_t highest_arr_idx) {
	duk_uint32_t new_e_size;
	duk_uint32_t new_a_size;
	duk_uint32_t new_h_size;

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(obj != NULL);
	DUK_ASSERT(highest_arr_idx >= DUK_HOBJECT_GET_ASIZE(obj));

	/* minimum new length is highest_arr_idx + 1 */

	new_e_size = DUK_HOBJECT_GET_ESIZE(obj);
	new_h_size = DUK_HOBJECT_GET_HSIZE(obj);
	new_a_size = highest_arr_idx + duk__get_min_grow_a(highest_arr_idx);
	DUK_ASSERT(new_a_size >= highest_arr_idx + 1);  /* duk__get_min_grow_a() is always >= 1 */

	duk__realloc_props(thr, obj, new_e_size, new_a_size, new_h_size, 0);
}

/* Abandon array part, moving array entries into entries part.
 * This requires a props resize, which is a heavy operation.
 * We also compact the entries part while we're at it, although
 * this is not strictly required.
 */
DUK_LOCAL void duk__abandon_array_checked(duk_hthread *thr, duk_hobject *obj) {
	duk_uint32_t new_e_size;
	duk_uint32_t new_a_size;
	duk_uint32_t new_h_size;
	duk_uint32_t e_used;  /* actually used, non-NULL keys */
	duk_uint32_t a_used;
	duk_uint32_t a_size;

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(obj != NULL);

	e_used = duk__count_used_e_keys(thr, obj);
	duk__compute_a_stats(thr, obj, &a_used, &a_size);

	/*
	 *  Must guarantee all actually used array entries will fit into
	 *  new entry part.  Add one growth step to ensure we don't run out
	 *  of space right away.
	 */

	new_e_size = e_used + a_used;
	new_e_size = new_e_size + duk__get_min_grow_e(new_e_size);
	new_a_size = 0;
#if defined(DUK_USE_HOBJECT_HASH_PART)
	new_h_size = duk__get_default_h_size(new_e_size);
#else
	new_h_size = 0;
#endif

	DUK_DD(DUK_DDPRINT("abandon array part for hobject %p, "
	                   "array stats before: e_used=%ld, a_used=%ld, a_size=%ld; "
	                   "resize to e_size=%ld, a_size=%ld, h_size=%ld",
	                   (void *) obj, (long) e_used, (long) a_used, (long) a_size,
	                   (long) new_e_size, (long) new_a_size, (long) new_h_size));

	duk__realloc_props(thr, obj, new_e_size, new_a_size, new_h_size, 1);
}

/*
 *  Compact an object.  Minimizes allocation size for objects which are
 *  not likely to be extended.  This is useful for internal and non-
 *  extensible objects, but can also be called for non-extensible objects.
 *  May abandon the array part if it is computed to be too sparse.
 *
 *  This call is relatively expensive, as it needs to scan both the
 *  entries and the array part.
 *
 *  The call may fail due to allocation error.
 */

DUK_INTERNAL void duk_hobject_compact_props(duk_hthread *thr, duk_hobject *obj) {
	duk_uint32_t e_size;       /* currently used -> new size */
	duk_uint32_t a_size;       /* currently required */
	duk_uint32_t a_used;       /* actually used */
	duk_uint32_t h_size;
	duk_bool_t abandon_array;

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(obj != NULL);

	e_size = duk__count_used_e_keys(thr, obj);
	duk__compute_a_stats(thr, obj, &a_used, &a_size);

	DUK_DD(DUK_DDPRINT("compacting hobject, used e keys %ld, used a keys %ld, min a size %ld, "
	                   "resized array density would be: %ld/%ld = %lf",
	                   (long) e_size, (long) a_used, (long) a_size,
	                   (long) a_used, (long) a_size,
	                   (double) a_used / (double) a_size));

	if (duk__abandon_array_density_check(a_used, a_size)) {
		DUK_DD(DUK_DDPRINT("decided to abandon array during compaction, a_used=%ld, a_size=%ld",
		                   (long) a_used, (long) a_size));
		abandon_array = 1;
		e_size += a_used;
		a_size = 0;
	} else {
		DUK_DD(DUK_DDPRINT("decided to keep array during compaction"));
		abandon_array = 0;
	}

#if defined(DUK_USE_HOBJECT_HASH_PART)
	if (e_size >= DUK_HOBJECT_E_USE_HASH_LIMIT) {
		h_size = duk__get_default_h_size(e_size);
	} else {
		h_size = 0;
	}
#else
	h_size = 0;
#endif

	DUK_DD(DUK_DDPRINT("compacting hobject -> new e_size %ld, new a_size=%ld, new h_size=%ld, abandon_array=%ld",
	                   (long) e_size, (long) a_size, (long) h_size, (long) abandon_array));

	duk__realloc_props(thr, obj, e_size, a_size, h_size, abandon_array);
}

/*
 *  Find an existing key from entry part either by linear scan or by
 *  using the hash index (if it exists).
 *
 *  Sets entry index (and possibly the hash index) to output variables,
 *  which allows the caller to update the entry and hash entries in-place.
 *  If entry is not found, both values are set to -1.  If entry is found
 *  but there is no hash part, h_idx is set to -1.
 */

DUK_INTERNAL void duk_hobject_find_existing_entry(duk_heap *heap, duk_hobject *obj, duk_hstring *key, duk_int_t *e_idx, duk_int_t *h_idx) {
	DUK_ASSERT(obj != NULL);
	DUK_ASSERT(key != NULL);
	DUK_ASSERT(e_idx != NULL);
	DUK_ASSERT(h_idx != NULL);
	DUK_UNREF(heap);

	if (DUK_LIKELY(DUK_HOBJECT_GET_HSIZE(obj) == 0))
	{
		/* Linear scan: more likely because most objects are small.
		 * This is an important fast path.
		 *
		 * XXX: this might be worth inlining for property lookups.
		 */
		duk_uint_fast32_t i;
		duk_uint_fast32_t n;
		duk_hstring **h_keys_base;
		DUK_DDD(DUK_DDDPRINT("duk_hobject_find_existing_entry() using linear scan for lookup"));

		h_keys_base = DUK_HOBJECT_E_GET_KEY_BASE(heap, obj);
		n = DUK_HOBJECT_GET_ENEXT(obj);
		for (i = 0; i < n; i++) {
			if (h_keys_base[i] == key) {
				*e_idx = i;
				*h_idx = -1;
				return;
			}
		}
	}
#if defined(DUK_USE_HOBJECT_HASH_PART)
	else
	{
		/* hash lookup */
		duk_uint32_t n;
		duk_uint32_t i, step;
		duk_uint32_t *h_base;

		DUK_DDD(DUK_DDDPRINT("duk_hobject_find_existing_entry() using hash part for lookup"));

		h_base = DUK_HOBJECT_H_GET_BASE(heap, obj);
		n = DUK_HOBJECT_GET_HSIZE(obj);
		i = DUK__HASH_INITIAL(DUK_HSTRING_GET_HASH(key), n);
		step = DUK__HASH_PROBE_STEP(DUK_HSTRING_GET_HASH(key));

		for (;;) {
			duk_uint32_t t;

			DUK_ASSERT_DISABLE(i >= 0);  /* unsigned */
			DUK_ASSERT(i < DUK_HOBJECT_GET_HSIZE(obj));
			t = h_base[i];
			DUK_ASSERT(t == DUK__HASH_UNUSED || t == DUK__HASH_DELETED ||
			           (t < DUK_HOBJECT_GET_ESIZE(obj)));  /* t >= 0 always true, unsigned */

			if (t == DUK__HASH_UNUSED) {
				break;
			} else if (t == DUK__HASH_DELETED) {
				DUK_DDD(DUK_DDDPRINT("lookup miss (deleted) i=%ld, t=%ld",
				                     (long) i, (long) t));
			} else {
				DUK_ASSERT(t < DUK_HOBJECT_GET_ESIZE(obj));
				if (DUK_HOBJECT_E_GET_KEY(heap, obj, t) == key) {
					DUK_DDD(DUK_DDDPRINT("lookup hit i=%ld, t=%ld -> key %p",
					                     (long) i, (long) t, (void *) key));
					*e_idx = t;
					*h_idx = i;
					return;
				}
				DUK_DDD(DUK_DDDPRINT("lookup miss i=%ld, t=%ld",
				                     (long) i, (long) t));
			}
			i = (i + step) % n;

			/* guaranteed to finish, as hash is never full */
			DUK_ASSERT(i != (duk_uint32_t) DUK__HASH_INITIAL(DUK_HSTRING_GET_HASH(key), n));
		}
	}
#endif  /* DUK_USE_HOBJECT_HASH_PART */

	/* not found */
	*e_idx = -1;
	*h_idx = -1;
}

/* For internal use: get non-accessor entry value */
DUK_INTERNAL duk_tval *duk_hobject_find_existing_entry_tval_ptr(duk_heap *heap, duk_hobject *obj, duk_hstring *key) {
	duk_int_t e_idx;
	duk_int_t h_idx;

	DUK_ASSERT(obj != NULL);
	DUK_ASSERT(key != NULL);
	DUK_UNREF(heap);

	duk_hobject_find_existing_entry(heap, obj, key, &e_idx, &h_idx);
	if (e_idx >= 0 && !DUK_HOBJECT_E_SLOT_IS_ACCESSOR(heap, obj, e_idx)) {
		return DUK_HOBJECT_E_GET_VALUE_TVAL_PTR(heap, obj, e_idx);
	} else {
		return NULL;
	}
}

/* For internal use: get non-accessor entry value and attributes */
DUK_INTERNAL duk_tval *duk_hobject_find_existing_entry_tval_ptr_and_attrs(duk_heap *heap, duk_hobject *obj, duk_hstring *key, duk_int_t *out_attrs) {
	duk_int_t e_idx;
	duk_int_t h_idx;

	DUK_ASSERT(obj != NULL);
	DUK_ASSERT(key != NULL);
	DUK_ASSERT(out_attrs != NULL);
	DUK_UNREF(heap);

	duk_hobject_find_existing_entry(heap, obj, key, &e_idx, &h_idx);
	if (e_idx >= 0 && !DUK_HOBJECT_E_SLOT_IS_ACCESSOR(heap, obj, e_idx)) {
		*out_attrs = DUK_HOBJECT_E_GET_FLAGS(heap, obj, e_idx);
		return DUK_HOBJECT_E_GET_VALUE_TVAL_PTR(heap, obj, e_idx);
	} else {
		*out_attrs = 0;
		return NULL;
	}
}

/* For internal use: get array part value */
DUK_INTERNAL duk_tval *duk_hobject_find_existing_array_entry_tval_ptr(duk_heap *heap, duk_hobject *obj, duk_uarridx_t i) {
	duk_tval *tv;

	DUK_ASSERT(obj != NULL);
	DUK_UNREF(heap);

	if (!DUK_HOBJECT_HAS_ARRAY_PART(obj)) {
		return NULL;
	}
	if (i >= DUK_HOBJECT_GET_ASIZE(obj)) {
		return NULL;
	}
	tv = DUK_HOBJECT_A_GET_VALUE_PTR(heap, obj, i);
	return tv;
}

/*
 *  Allocate and initialize a new entry, resizing the properties allocation
 *  if necessary.  Returns entry index (e_idx) or throws an error if alloc fails.
 *
 *  Sets the key of the entry (increasing the key's refcount), and updates
 *  the hash part if it exists.  Caller must set value and flags, and update
 *  the entry value refcount.  A decref for the previous value is not necessary.
 */

DUK_LOCAL duk_bool_t duk__alloc_entry_checked(duk_hthread *thr, duk_hobject *obj, duk_hstring *key) {
	duk_uint32_t idx;

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(obj != NULL);
	DUK_ASSERT(key != NULL);
	DUK_ASSERT(DUK_HOBJECT_GET_ENEXT(obj) <= DUK_HOBJECT_GET_ESIZE(obj));

#ifdef DUK_USE_ASSERTIONS
	/* key must not already exist in entry part */
	{
		duk_uint_fast32_t i;
		for (i = 0; i < DUK_HOBJECT_GET_ENEXT(obj); i++) {
			DUK_ASSERT(DUK_HOBJECT_E_GET_KEY(thr->heap, obj, i) != key);
		}
	}
#endif

	if (DUK_HOBJECT_GET_ENEXT(obj) >= DUK_HOBJECT_GET_ESIZE(obj)) {
		/* only need to guarantee 1 more slot, but allocation growth is in chunks */
		DUK_DDD(DUK_DDDPRINT("entry part full, allocate space for one more entry"));
		duk__grow_props_for_new_entry_item(thr, obj);
	}
	DUK_ASSERT(DUK_HOBJECT_GET_ENEXT(obj) < DUK_HOBJECT_GET_ESIZE(obj));
	idx = DUK_HOBJECT_POSTINC_ENEXT(obj);

	/* previous value is assumed to be garbage, so don't touch it */
	DUK_HOBJECT_E_SET_KEY(thr->heap, obj, idx, key);
	DUK_HSTRING_INCREF(thr, key);

#if defined(DUK_USE_HOBJECT_HASH_PART)
	if (DUK_UNLIKELY(DUK_HOBJECT_GET_HSIZE(obj) > 0)) {
		duk_uint32_t n;
		duk_uint32_t i, step;
		duk_uint32_t *h_base = DUK_HOBJECT_H_GET_BASE(thr->heap, obj);

		n = DUK_HOBJECT_GET_HSIZE(obj);
		i = DUK__HASH_INITIAL(DUK_HSTRING_GET_HASH(key), n);
		step = DUK__HASH_PROBE_STEP(DUK_HSTRING_GET_HASH(key));

		for (;;) {
			duk_uint32_t t = h_base[i];
			if (t == DUK__HASH_UNUSED || t == DUK__HASH_DELETED) {
				DUK_DDD(DUK_DDDPRINT("duk__alloc_entry_checked() inserted key into hash part, %ld -> %ld",
				                     (long) i, (long) idx));
				DUK_ASSERT_DISABLE(i >= 0);  /* unsigned */
				DUK_ASSERT(i < DUK_HOBJECT_GET_HSIZE(obj));
				DUK_ASSERT_DISABLE(idx >= 0);
				DUK_ASSERT(idx < DUK_HOBJECT_GET_ESIZE(obj));
				h_base[i] = idx;
				break;
			}
			DUK_DDD(DUK_DDDPRINT("duk__alloc_entry_checked() miss %ld", (long) i));
			i = (i + step) % n;

			/* guaranteed to find an empty slot */
			DUK_ASSERT(i != (duk_uint32_t) DUK__HASH_INITIAL(DUK_HSTRING_GET_HASH(key), DUK_HOBJECT_GET_HSIZE(obj)));
		}
	}
#endif  /* DUK_USE_HOBJECT_HASH_PART */

	/* Note: we could return the hash index here too, but it's not
	 * needed right now.
	 */

	DUK_ASSERT_DISABLE(idx >= 0);
	DUK_ASSERT(idx < DUK_HOBJECT_GET_ESIZE(obj));
	DUK_ASSERT(idx < DUK_HOBJECT_GET_ENEXT(obj));
	return idx;
}

/*
 *  Object internal value
 *
 *  Returned value is guaranteed to be reachable / incref'd, caller does not need
 *  to incref OR decref.  No proxies or accessors are invoked, no prototype walk.
 */

DUK_INTERNAL duk_bool_t duk_hobject_get_internal_value(duk_heap *heap, duk_hobject *obj, duk_tval *tv_out) {
	duk_int_t e_idx;
	duk_int_t h_idx;

	DUK_ASSERT(heap != NULL);
	DUK_ASSERT(obj != NULL);
	DUK_ASSERT(tv_out != NULL);

	DUK_TVAL_SET_UNDEFINED_UNUSED(tv_out);

	/* always in entry part, no need to look up parents etc */
	duk_hobject_find_existing_entry(heap, obj, DUK_HEAP_STRING_INT_VALUE(heap), &e_idx, &h_idx);
	if (e_idx >= 0) {
		DUK_ASSERT(!DUK_HOBJECT_E_SLOT_IS_ACCESSOR(heap, obj, e_idx));
		DUK_TVAL_SET_TVAL(tv_out, DUK_HOBJECT_E_GET_VALUE_TVAL_PTR(heap, obj, e_idx));
		return 1;
	}
	return 0;
}

DUK_INTERNAL duk_hstring *duk_hobject_get_internal_value_string(duk_heap *heap, duk_hobject *obj) {
	duk_tval tv;

	DUK_ASSERT(heap != NULL);
	DUK_ASSERT(obj != NULL);

	if (duk_hobject_get_internal_value(heap, obj, &tv)) {
		duk_hstring *h;
		DUK_ASSERT(DUK_TVAL_IS_STRING(&tv));
		h = DUK_TVAL_GET_STRING(&tv);
		return h;
	}

	return NULL;
}

DUK_INTERNAL duk_hbuffer *duk_hobject_get_internal_value_buffer(duk_heap *heap, duk_hobject *obj) {
	duk_tval tv;

	DUK_ASSERT(heap != NULL);
	DUK_ASSERT(obj != NULL);

	if (duk_hobject_get_internal_value(heap, obj, &tv)) {
		duk_hbuffer *h;
		DUK_ASSERT(DUK_TVAL_IS_BUFFER(&tv));
		h = DUK_TVAL_GET_BUFFER(&tv);
		return h;
	}

	return NULL;
}

/*
 *  Arguments handling helpers (argument map mainly).
 *
 *  An arguments object has exotic behavior for some numeric indices.
 *  Accesses may translate to identifier operations which may have
 *  arbitrary side effects (potentially invalidating any duk_tval
 *  pointers).
 */

/* Lookup 'key' from arguments internal 'map', perform a variable lookup
 * if mapped, and leave the result on top of stack (and return non-zero).
 * Used in E5 Section 10.6 algorithms [[Get]] and [[GetOwnProperty]].
 */
DUK_LOCAL
duk_bool_t duk__lookup_arguments_map(duk_hthread *thr,
                                     duk_hobject *obj,
                                     duk_hstring *key,
                                     duk_propdesc *temp_desc,
                                     duk_hobject **out_map,
                                     duk_hobject **out_varenv) {
	duk_context *ctx = (duk_context *) thr;
	duk_hobject *map;
	duk_hobject *varenv;
	duk_bool_t rc;

	DUK_ASSERT_VALSTACK_SPACE(thr, DUK__VALSTACK_SPACE);

	DUK_DDD(DUK_DDDPRINT("arguments map lookup: thr=%p, obj=%p, key=%p, temp_desc=%p "
	                     "(obj -> %!O, key -> %!O)",
	                     (void *) thr, (void *) obj, (void *) key, (void *) temp_desc,
	                     (duk_heaphdr *) obj, (duk_heaphdr *) key));

	if (!duk__get_own_property_desc(thr, obj, DUK_HTHREAD_STRING_INT_MAP(thr), temp_desc, DUK__DESC_FLAG_PUSH_VALUE)) {
		DUK_DDD(DUK_DDDPRINT("-> no 'map'"));
		return 0;
	}

	map = duk_require_hobject(ctx, -1);
	DUK_ASSERT(map != NULL);
	duk_pop(ctx);  /* map is reachable through obj */

	if (!duk__get_own_property_desc(thr, map, key, temp_desc, DUK__DESC_FLAG_PUSH_VALUE)) {
		DUK_DDD(DUK_DDDPRINT("-> 'map' exists, but key not in map"));
		return 0;
	}

	/* [... varname] */
	DUK_DDD(DUK_DDDPRINT("-> 'map' exists, and contains key, key is mapped to argument/variable binding %!T",
	                     (duk_tval *) duk_get_tval(ctx, -1)));
	DUK_ASSERT(duk_is_string(ctx, -1));  /* guaranteed when building arguments */

	/* get varenv for varname (callee's declarative lexical environment) */
	rc = duk__get_own_property_desc(thr, obj, DUK_HTHREAD_STRING_INT_VARENV(thr), temp_desc, DUK__DESC_FLAG_PUSH_VALUE);
	DUK_UNREF(rc);
	DUK_ASSERT(rc != 0);  /* arguments MUST have an initialized lexical environment reference */
	varenv = duk_require_hobject(ctx, -1);
	DUK_ASSERT(varenv != NULL);
	duk_pop(ctx);  /* varenv remains reachable through 'obj' */

	DUK_DDD(DUK_DDDPRINT("arguments varenv is: %!dO", (duk_heaphdr *) varenv));

	/* success: leave varname in stack */
	*out_map = map;
	*out_varenv = varenv;
	return 1;  /* [... varname] */
}

/* Lookup 'key' from arguments internal 'map', and leave replacement value
 * on stack top if mapped (and return non-zero).
 * Used in E5 Section 10.6 algorithm for [[GetOwnProperty]] (used by [[Get]]).
 */
DUK_LOCAL duk_bool_t duk__check_arguments_map_for_get(duk_hthread *thr, duk_hobject *obj, duk_hstring *key, duk_propdesc *temp_desc) {
	duk_context *ctx = (duk_context *) thr;
	duk_hobject *map;
	duk_hobject *varenv;
	duk_hstring *varname;

	DUK_ASSERT_VALSTACK_SPACE(thr, DUK__VALSTACK_SPACE);

	if (!duk__lookup_arguments_map(thr, obj, key, temp_desc, &map, &varenv)) {
		DUK_DDD(DUK_DDDPRINT("arguments: key not mapped, no exotic get behavior"));
		return 0;
	}

	/* [... varname] */

	varname = duk_require_hstring(ctx, -1);
	DUK_ASSERT(varname != NULL);
	duk_pop(ctx);  /* varname is still reachable */

	DUK_DDD(DUK_DDDPRINT("arguments object automatic getvar for a bound variable; "
	                     "key=%!O, varname=%!O",
	                     (duk_heaphdr *) key,
	                     (duk_heaphdr *) varname));

	(void) duk_js_getvar_envrec(thr, varenv, varname, 1 /*throw*/);

	/* [... value this_binding] */

	duk_pop(ctx);

	/* leave result on stack top */
	return 1;
}

/* Lookup 'key' from arguments internal 'map', perform a variable write if mapped.
 * Used in E5 Section 10.6 algorithm for [[DefineOwnProperty]] (used by [[Put]]).
 * Assumes stack top contains 'put' value (which is NOT popped).
 */
DUK_LOCAL void duk__check_arguments_map_for_put(duk_hthread *thr, duk_hobject *obj, duk_hstring *key, duk_propdesc *temp_desc, duk_bool_t throw_flag) {
	duk_context *ctx = (duk_context *) thr;
	duk_hobject *map;
	duk_hobject *varenv;
	duk_hstring *varname;

	DUK_ASSERT_VALSTACK_SPACE(thr, DUK__VALSTACK_SPACE);

	if (!duk__lookup_arguments_map(thr, obj, key, temp_desc, &map, &varenv)) {
		DUK_DDD(DUK_DDDPRINT("arguments: key not mapped, no exotic put behavior"));
		return;
	}

	/* [... put_value varname] */

	varname = duk_require_hstring(ctx, -1);
	DUK_ASSERT(varname != NULL);
	duk_pop(ctx);  /* varname is still reachable */

	DUK_DDD(DUK_DDDPRINT("arguments object automatic putvar for a bound variable; "
	                     "key=%!O, varname=%!O, value=%!T",
	                     (duk_heaphdr *) key,
	                     (duk_heaphdr *) varname,
	                     (duk_tval *) duk_require_tval(ctx, -1)));

	/* [... put_value] */

	/*
	 *  Note: although arguments object variable mappings are only established
	 *  for non-strict functions (and a call to a non-strict function created
	 *  the arguments object in question), an inner strict function may be doing
	 *  the actual property write.  Hence the throw_flag applied here comes from
	 *  the property write call.
	 */

	duk_js_putvar_envrec(thr, varenv, varname, duk_require_tval(ctx, -1), throw_flag);

	/* [... put_value] */
}

/* Lookup 'key' from arguments internal 'map', delete mapping if found.
 * Used in E5 Section 10.6 algorithm for [[Delete]].  Note that the
 * variable/argument itself (where the map points) is not deleted.
 */
DUK_LOCAL void duk__check_arguments_map_for_delete(duk_hthread *thr, duk_hobject *obj, duk_hstring *key, duk_propdesc *temp_desc) {
	duk_context *ctx = (duk_context *) thr;
	duk_hobject *map;

	DUK_ASSERT_VALSTACK_SPACE(thr, DUK__VALSTACK_SPACE);

	if (!duk__get_own_property_desc(thr, obj, DUK_HTHREAD_STRING_INT_MAP(thr), temp_desc, DUK__DESC_FLAG_PUSH_VALUE)) {
		DUK_DDD(DUK_DDDPRINT("arguments: key not mapped, no exotic delete behavior"));
		return;
	}

	map = duk_require_hobject(ctx, -1);
	DUK_ASSERT(map != NULL);
	duk_pop(ctx);  /* map is reachable through obj */

	DUK_DDD(DUK_DDDPRINT("-> have 'map', delete key %!O from map (if exists)); ignore result",
	                     (duk_heaphdr *) key));

	/* Note: no recursion issue, we can trust 'map' to behave */
	DUK_ASSERT(!DUK_HOBJECT_HAS_EXOTIC_BEHAVIOR(map));
	DUK_DDD(DUK_DDDPRINT("map before deletion: %!O", (duk_heaphdr *) map));
	(void) duk_hobject_delprop_raw(thr, map, key, 0);  /* ignore result */
	DUK_DDD(DUK_DDDPRINT("map after deletion: %!O", (duk_heaphdr *) map));
}

/*
 *  Ecmascript compliant [[GetOwnProperty]](P), for internal use only.
 *
 *  If property is found:
 *    - Fills descriptor fields to 'out_desc'
 *    - If DUK__DESC_FLAG_PUSH_VALUE is set, pushes a value related to the
 *      property onto the stack ('undefined' for accessor properties).
 *    - Returns non-zero
 *
 *  If property is not found:
 *    - 'out_desc' is left in untouched state (possibly garbage)
 *    - Nothing is pushed onto the stack (not even with DUK__DESC_FLAG_PUSH_VALUE
 *      set)
 *    - Returns zero
 *
 *  Notes:
 *
 *    - Getting a property descriptor may cause an allocation (and hence
 *      GC) to take place, hence reachability and refcount of all related
 *      values matter.  Reallocation of value stack, properties, etc may
 *      invalidate many duk_tval pointers (concretely, those which reside
 *      in memory areas subject to reallocation).  However, heap object
 *      pointers are never affected (heap objects have stable pointers).
 *
 *    - The value of a plain property is always reachable and has a non-zero
 *      reference count.
 *
 *    - The value of a virtual property is not necessarily reachable from
 *      elsewhere and may have a refcount of zero.  Hence we push it onto
 *      the valstack for the caller, which ensures it remains reachable
 *      while it is needed.
 *
 *    - There are no virtual accessor properties.  Hence, all getters and
 *      setters are always related to concretely stored properties, which
 *      ensures that the get/set functions in the resulting descriptor are
 *      reachable and have non-zero refcounts.  Should there be virtual
 *      accessor properties later, this would need to change.
 */

DUK_LOCAL duk_bool_t duk__get_own_property_desc_raw(duk_hthread *thr, duk_hobject *obj, duk_hstring *key, duk_uint32_t arr_idx, duk_propdesc *out_desc, duk_small_uint_t flags) {
	duk_context *ctx = (duk_context *) thr;
	duk_tval *tv;

	DUK_DDD(DUK_DDDPRINT("duk__get_own_property_desc: thr=%p, obj=%p, key=%p, out_desc=%p, flags=%lx, "
	                     "arr_idx=%ld (obj -> %!O, key -> %!O)",
	                     (void *) thr, (void *) obj, (void *) key, (void *) out_desc,
	                     (long) flags, (long) arr_idx,
	                     (duk_heaphdr *) obj, (duk_heaphdr *) key));

	DUK_ASSERT(ctx != NULL);
	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(thr->heap != NULL);
	DUK_ASSERT(obj != NULL);
	DUK_ASSERT(key != NULL);
	DUK_ASSERT(out_desc != NULL);
	DUK_ASSERT_VALSTACK_SPACE(thr, DUK__VALSTACK_SPACE);

	/* XXX: optimize this filling behavior later */
	out_desc->flags = 0;
	out_desc->get = NULL;
	out_desc->set = NULL;
	out_desc->e_idx = -1;
	out_desc->h_idx = -1;
	out_desc->a_idx = -1;

	/*
	 *  Array part
	 */

	if (DUK_HOBJECT_HAS_ARRAY_PART(obj) && arr_idx != DUK__NO_ARRAY_INDEX) {
		if (arr_idx < DUK_HOBJECT_GET_ASIZE(obj)) {
			tv = DUK_HOBJECT_A_GET_VALUE_PTR(thr->heap, obj, arr_idx);
			if (!DUK_TVAL_IS_UNDEFINED_UNUSED(tv)) {
				DUK_DDD(DUK_DDDPRINT("-> found in array part"));
				if (flags & DUK__DESC_FLAG_PUSH_VALUE) {
					duk_push_tval(ctx, tv);
				}
				/* implicit attributes */
				out_desc->flags = DUK_PROPDESC_FLAG_WRITABLE |
				                  DUK_PROPDESC_FLAG_CONFIGURABLE |
				                  DUK_PROPDESC_FLAG_ENUMERABLE;
				out_desc->a_idx = arr_idx;
				goto prop_found;
			}
		}
		/* assume array part is comprehensive (contains all array indexed elements
		 * or none of them); hence no need to check the entries part here.
		 */
		DUK_DDD(DUK_DDDPRINT("-> not found as a concrete property (has array part, "
		                     "should be there if present)"));
		goto prop_not_found_concrete;
	}

	/*
	 *  Entries part
	 */

	duk_hobject_find_existing_entry(thr->heap, obj, key, &out_desc->e_idx, &out_desc->h_idx);
	if (out_desc->e_idx >= 0) {
		duk_int_t e_idx = out_desc->e_idx;
		out_desc->flags = DUK_HOBJECT_E_GET_FLAGS(thr->heap, obj, e_idx);
		if (out_desc->flags & DUK_PROPDESC_FLAG_ACCESSOR) {
			DUK_DDD(DUK_DDDPRINT("-> found accessor property in entry part"));
			out_desc->get = DUK_HOBJECT_E_GET_VALUE_GETTER(thr->heap, obj, e_idx);
			out_desc->set = DUK_HOBJECT_E_GET_VALUE_SETTER(thr->heap, obj, e_idx);
			if (flags & DUK__DESC_FLAG_PUSH_VALUE) {
				/* a dummy undefined value is pushed to make valstack
				 * behavior uniform for caller
				 */
				duk_push_undefined(ctx);
			}
		} else {
			DUK_DDD(DUK_DDDPRINT("-> found plain property in entry part"));
			tv = DUK_HOBJECT_E_GET_VALUE_TVAL_PTR(thr->heap, obj, e_idx);
			if (flags & DUK__DESC_FLAG_PUSH_VALUE) {
				duk_push_tval(ctx, tv);
			}
		}
		goto prop_found;
	}

	/*
	 *  Not found as a concrete property, check whether a String object
	 *  virtual property matches.
	 */

 prop_not_found_concrete:

	if (DUK_HOBJECT_HAS_EXOTIC_STRINGOBJ(obj)) {
		DUK_DDD(DUK_DDDPRINT("string object exotic property get for key: %!O, arr_idx: %ld",
		                     (duk_heaphdr *) key, (long) arr_idx));

		if (arr_idx != DUK__NO_ARRAY_INDEX) {
			duk_hstring *h_val;

			DUK_DDD(DUK_DDDPRINT("array index exists"));

			h_val = duk_hobject_get_internal_value_string(thr->heap, obj);
			DUK_ASSERT(h_val);
			if (arr_idx < DUK_HSTRING_GET_CHARLEN(h_val)) {
				DUK_DDD(DUK_DDDPRINT("-> found, array index inside string"));
				if (flags & DUK__DESC_FLAG_PUSH_VALUE) {
					duk_push_hstring(ctx, h_val);
					duk_substring(ctx, -1, arr_idx, arr_idx + 1);  /* [str] -> [substr] */
				}
				out_desc->flags = DUK_PROPDESC_FLAG_ENUMERABLE |  /* E5 Section 15.5.5.2 */
				                  DUK_PROPDESC_FLAG_VIRTUAL;

				DUK_ASSERT(!DUK_HOBJECT_HAS_EXOTIC_ARGUMENTS(obj));
				return 1;  /* cannot be e.g. arguments exotic, since exotic 'traits' are mutually exclusive */
			} else {
				/* index is above internal string length -> property is fully normal */
				DUK_DDD(DUK_DDDPRINT("array index outside string -> normal property"));
			}
		} else if (key == DUK_HTHREAD_STRING_LENGTH(thr)) {
			duk_hstring *h_val;

			DUK_DDD(DUK_DDDPRINT("-> found, key is 'length', length exotic behavior"));

			h_val = duk_hobject_get_internal_value_string(thr->heap, obj);
			DUK_ASSERT(h_val != NULL);
			if (flags & DUK__DESC_FLAG_PUSH_VALUE) {
				duk_push_uint(ctx, (duk_uint_t) DUK_HSTRING_GET_CHARLEN(h_val));
			}
			out_desc->flags = DUK_PROPDESC_FLAG_VIRTUAL;  /* E5 Section 15.5.5.1 */

			DUK_ASSERT(!DUK_HOBJECT_HAS_EXOTIC_ARGUMENTS(obj));
			return 1;  /* cannot be arguments exotic */
		}
	} else if (DUK_HOBJECT_HAS_EXOTIC_BUFFEROBJ(obj)) {
		DUK_DDD(DUK_DDDPRINT("buffer object exotic property get for key: %!O, arr_idx: %ld",
		                     (duk_heaphdr *) key, (long) arr_idx));

		if (arr_idx != DUK__NO_ARRAY_INDEX) {
			duk_hbuffer *h_val;

			DUK_DDD(DUK_DDDPRINT("array index exists"));

			h_val = duk_hobject_get_internal_value_buffer(thr->heap, obj);
			DUK_ASSERT(h_val);
			/* SCANBUILD: h_val is known to be non-NULL but scan-build cannot
			 * know it, so it produces NULL pointer dereference warnings for
			 * 'h_val'.
			 */

			if (arr_idx < DUK_HBUFFER_GET_SIZE(h_val)) {
				DUK_DDD(DUK_DDDPRINT("-> found, array index inside buffer"));
				if (flags & DUK__DESC_FLAG_PUSH_VALUE) {
					duk_push_int(ctx, ((duk_uint8_t *) DUK_HBUFFER_GET_DATA_PTR(thr->heap, h_val))[arr_idx]);
				}
				out_desc->flags = DUK_PROPDESC_FLAG_WRITABLE |
				                  DUK_PROPDESC_FLAG_ENUMERABLE |
				                  DUK_PROPDESC_FLAG_VIRTUAL;

				DUK_ASSERT(!DUK_HOBJECT_HAS_EXOTIC_ARGUMENTS(obj));
				return 1;  /* cannot be e.g. arguments exotic, since exotic 'traits' are mutually exclusive */
			} else {
				/* index is above internal buffer length -> property is fully normal */
				DUK_DDD(DUK_DDDPRINT("array index outside buffer -> normal property"));
			}
		} else if (key == DUK_HTHREAD_STRING_LENGTH(thr)) {
			duk_hbuffer *h_val;

			DUK_DDD(DUK_DDDPRINT("-> found, key is 'length', length exotic behavior"));

			/* XXX: buffer length should be writable and have exotic behavior
			 * like arrays.  For now, make it read-only and use explicit methods
			 * to operate on buffer length.
			 */

			h_val = duk_hobject_get_internal_value_buffer(thr->heap, obj);
			DUK_ASSERT(h_val != NULL);
			if (flags & DUK__DESC_FLAG_PUSH_VALUE) {
				duk_push_uint(ctx, (duk_uint_t) DUK_HBUFFER_GET_SIZE(h_val));
			}
			out_desc->flags = DUK_PROPDESC_FLAG_VIRTUAL;

			DUK_ASSERT(!DUK_HOBJECT_HAS_EXOTIC_ARGUMENTS(obj));
			return 1;  /* cannot be arguments exotic */
		}
	} else if (DUK_HOBJECT_HAS_EXOTIC_DUKFUNC(obj)) {
		DUK_DDD(DUK_DDDPRINT("duktape/c object exotic property get for key: %!O, arr_idx: %ld",
		                     (duk_heaphdr *) key, (long) arr_idx));

		if (key == DUK_HTHREAD_STRING_LENGTH(thr)) {
			DUK_DDD(DUK_DDDPRINT("-> found, key is 'length', length exotic behavior"));

			if (flags & DUK__DESC_FLAG_PUSH_VALUE) {
				duk_int16_t func_nargs = ((duk_hnativefunction *) obj)->nargs;
				duk_push_int(ctx, func_nargs == DUK_HNATIVEFUNCTION_NARGS_VARARGS ? 0 : func_nargs);
			}
			out_desc->flags = DUK_PROPDESC_FLAG_VIRTUAL;  /* not enumerable */

			DUK_ASSERT(!DUK_HOBJECT_HAS_EXOTIC_ARGUMENTS(obj));
			return 1;  /* cannot be arguments exotic */
		}
	}

	/* Array properties have exotic behavior but they are concrete,
	 * so no special handling here.
	 *
	 * Arguments exotic behavior (E5 Section 10.6, [[GetOwnProperty]]
	 * is only relevant as a post-check implemented below; hence no
	 * check here.
	 */

	/*
	 *  Not found as concrete or virtual
	 */

	DUK_DDD(DUK_DDDPRINT("-> not found (virtual, entry part, or array part)"));
	return 0;

	/*
	 *  Found
	 *
	 *  Arguments object has exotic post-processing, see E5 Section 10.6,
	 *  description of [[GetOwnProperty]] variant for arguments.
	 */

 prop_found:
	DUK_DDD(DUK_DDDPRINT("-> property found, checking for arguments exotic post-behavior"));

	/* Notes:
	 *  - only numbered indices are relevant, so arr_idx fast reject is good
	 *    (this is valid unless there are more than 4**32-1 arguments).
	 *  - since variable lookup has no side effects, this can be skipped if
	 *    DUK__DESC_FLAG_PUSH_VALUE is not set.
	 */

	if (DUK_HOBJECT_HAS_EXOTIC_ARGUMENTS(obj) &&
	    arr_idx != DUK__NO_ARRAY_INDEX &&
	    (flags & DUK__DESC_FLAG_PUSH_VALUE)) {
		duk_propdesc temp_desc;

		/* Magically bound variable cannot be an accessor.  However,
		 * there may be an accessor property (or a plain property) in
		 * place with magic behavior removed.  This happens e.g. when
		 * a magic property is redefined with defineProperty().
		 * Cannot assert for "not accessor" here.
		 */

		/* replaces top of stack with new value if necessary */
		DUK_ASSERT((flags & DUK__DESC_FLAG_PUSH_VALUE) != 0);

		if (duk__check_arguments_map_for_get(thr, obj, key, &temp_desc)) {
			DUK_DDD(DUK_DDDPRINT("-> arguments exotic behavior overrides result: %!T -> %!T",
			                     (duk_tval *) duk_get_tval(ctx, -2),
			                     (duk_tval *) duk_get_tval(ctx, -1)));
			/* [... old_result result] -> [... result] */
			duk_remove(ctx, -2);
		}
	}

	return 1;
}

DUK_LOCAL duk_bool_t duk__get_own_property_desc(duk_hthread *thr, duk_hobject *obj, duk_hstring *key, duk_propdesc *out_desc, duk_small_uint_t flags) {
	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(obj != NULL);
	DUK_ASSERT(key != NULL);
	DUK_ASSERT(out_desc != NULL);
	DUK_ASSERT_VALSTACK_SPACE(thr, DUK__VALSTACK_SPACE);

	return duk__get_own_property_desc_raw(thr, obj, key, DUK_HSTRING_GET_ARRIDX_SLOW(key), out_desc, flags);
}

/*
 *  Ecmascript compliant [[GetProperty]](P), for internal use only.
 *
 *  If property is found:
 *    - Fills descriptor fields to 'out_desc'
 *    - If DUK__DESC_FLAG_PUSH_VALUE is set, pushes a value related to the
 *      property onto the stack ('undefined' for accessor properties).
 *    - Returns non-zero
 *
 *  If property is not found:
 *    - 'out_desc' is left in untouched state (possibly garbage)
 *    - Nothing is pushed onto the stack (not even with DUK__DESC_FLAG_PUSH_VALUE
 *      set)
 *    - Returns zero
 *
 *  May cause arbitrary side effects and invalidate (most) duk_tval
 *  pointers.
 */

DUK_LOCAL duk_bool_t duk__get_property_desc(duk_hthread *thr, duk_hobject *obj, duk_hstring *key, duk_propdesc *out_desc, duk_small_uint_t flags) {
	duk_hobject *curr;
	duk_uint32_t arr_idx;
	duk_uint_t sanity;

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(thr->heap != NULL);
	DUK_ASSERT(obj != NULL);
	DUK_ASSERT(key != NULL);
	DUK_ASSERT(out_desc != NULL);
	DUK_ASSERT_VALSTACK_SPACE(thr, DUK__VALSTACK_SPACE);

	arr_idx = DUK_HSTRING_GET_ARRIDX_FAST(key);

	DUK_DDD(DUK_DDDPRINT("duk__get_property_desc: thr=%p, obj=%p, key=%p, out_desc=%p, flags=%lx, "
	                     "arr_idx=%ld (obj -> %!O, key -> %!O)",
	                     (void *) thr, (void *) obj, (void *) key, (void *) out_desc,
	                     (long) flags, (long) arr_idx,
	                     (duk_heaphdr *) obj, (duk_heaphdr *) key));

	curr = obj;
	DUK_ASSERT(curr != NULL);
	sanity = DUK_HOBJECT_PROTOTYPE_CHAIN_SANITY;
	do {
		if (duk__get_own_property_desc_raw(thr, curr, key, arr_idx, out_desc, flags)) {
			/* stack contains value (if requested), 'out_desc' is set */
			return 1;
		}

		/* not found in 'curr', next in prototype chain; impose max depth */
		if (sanity-- == 0) {
			if (flags & DUK__DESC_FLAG_IGNORE_PROTOLOOP) {
				/* treat like property not found */
				break;
			} else {
				DUK_ERROR(thr, DUK_ERR_INTERNAL_ERROR, DUK_STR_PROTOTYPE_CHAIN_LIMIT);
			}
		}
		curr = DUK_HOBJECT_GET_PROTOTYPE(thr->heap, curr);
	} while (curr);

	/* out_desc is left untouched (possibly garbage), caller must use return
	 * value to determine whether out_desc can be looked up
	 */

	return 0;
}

/*
 *  Shallow fast path checks for accessing array elements with numeric
 *  indices.  The goal is to try to avoid coercing an array index to an
 *  (interned) string for the most common lookups, in particular, for
 *  standard Array objects.
 *
 *  Interning is avoided but only for a very narrow set of cases:
 *    - Object has array part, index is within array allocation, and
 *      value is not unused (= key exists)
 *    - Object has no interfering exotic behavior (e.g. arguments or
 *      string object exotic behaviors interfere, array exotic
 *      behavior does not).
 *
 *  Current shortcoming: if key does not exist (even if it is within
 *  the array allocation range) a slow path lookup with interning is
 *  always required.  This can probably be fixed so that there is a
 *  quick fast path for non-existent elements as well, at least for
 *  standard Array objects.
 */

#if 0  /* XXX: unused now */
DUK_LOCAL duk_tval *duk__shallow_fast_path_array_check_u32(duk_hobject *obj, duk_uint32_t key_idx) {
	duk_tval *tv;

	if ((!DUK_HOBJECT_HAS_EXOTIC_ARGUMENTS(obj)) &&
	    (!DUK_HOBJECT_HAS_EXOTIC_STRINGOBJ(obj)) &&
	    (!DUK_HOBJECT_HAS_EXOTIC_BUFFEROBJ(obj)) &&
	    (!DUK_HOBJECT_HAS_EXOTIC_PROXYOBJ(obj)) &&
	    (DUK_HOBJECT_HAS_ARRAY_PART(obj)) &&
	    (key_idx < obj->a_size)) {
		/* technically required to check, but obj->a_size check covers this */
		DUK_ASSERT(key_idx != 0xffffffffUL);

		DUK_DDD(DUK_DDDPRINT("fast path attempt (key is an array index, no exotic "
		                     "string/arguments/buffer behavior, object has array part, key "
		                     "inside array size)"));

		DUK_ASSERT(obj->a_size > 0);  /* true even for key_idx == 0 */
		tv = DUK_HOBJECT_A_GET_VALUE_PTR(obj, key_idx);
		if (!DUK_TVAL_IS_UNDEFINED_UNUSED(tv)) {
			DUK_DDD(DUK_DDDPRINT("-> fast path successful"));
			return tv;
		}

		/*
		 *  Not found, fall back to slow path.
		 *
		 *  Note: this approach has the unfortunate side effect that accesses
		 *  to undefined entries (or entries outside valid array range) cause
		 *  a string intern operation.
		 */

		DUK_DDD(DUK_DDDPRINT("fast path attempt failed, fall back to slow path"));
	}

	return NULL;
}
#endif

DUK_LOCAL duk_tval *duk__shallow_fast_path_array_check_tval(duk_hthread *thr, duk_hobject *obj, duk_tval *key_tv) {
	duk_tval *tv;

	DUK_UNREF(thr);

	if (DUK_TVAL_IS_NUMBER(key_tv) &&
	    (!DUK_HOBJECT_HAS_EXOTIC_ARGUMENTS(obj)) &&
	    (!DUK_HOBJECT_HAS_EXOTIC_STRINGOBJ(obj)) &&
	    (!DUK_HOBJECT_HAS_EXOTIC_BUFFEROBJ(obj)) &&
	    (!DUK_HOBJECT_HAS_EXOTIC_PROXYOBJ(obj)) &&
	    (DUK_HOBJECT_HAS_ARRAY_PART(obj))) {
		duk_uint32_t idx;

		DUK_DDD(DUK_DDDPRINT("fast path attempt (key is a number, no exotic string/arguments/buffer "
		                     "behavior, object has array part)"));

		idx = duk__tval_number_to_arr_idx(key_tv);

		if (idx != DUK__NO_ARRAY_INDEX) {
			/* Note: idx is not necessarily a valid array index (0xffffffffUL is not valid) */
			DUK_ASSERT_DISABLE(idx >= 0);  /* disabled because idx is duk_uint32_t so always true */
			DUK_ASSERT_DISABLE(idx <= 0xffffffffUL);  /* same */

			if (idx < DUK_HOBJECT_GET_ASIZE(obj)) {
				/* technically required to check, but obj->a_size check covers this */
				DUK_ASSERT(idx != 0xffffffffUL);

				DUK_DDD(DUK_DDDPRINT("key is a valid array index and inside array part"));
				tv = DUK_HOBJECT_A_GET_VALUE_PTR(thr->heap, obj, idx);
				if (!DUK_TVAL_IS_UNDEFINED_UNUSED(tv)) {
					DUK_DDD(DUK_DDDPRINT("-> fast path successful"));
					return tv;
				}
			} else {
				DUK_DDD(DUK_DDDPRINT("key is outside array part"));
			}
		} else {
			DUK_DDD(DUK_DDDPRINT("key is not a valid array index"));
		}

		/*
		 *  Not found in array part, use slow path.
		 */

		DUK_DDD(DUK_DDDPRINT("fast path attempt failed, fall back to slow path"));
	}

	return NULL;
}

/*
 *  GETPROP: Ecmascript property read.
 */

DUK_INTERNAL duk_bool_t duk_hobject_getprop(duk_hthread *thr, duk_tval *tv_obj, duk_tval *tv_key) {
	duk_context *ctx = (duk_context *) thr;
	duk_tval tv_obj_copy;
	duk_tval tv_key_copy;
	duk_hobject *curr = NULL;
	duk_hstring *key = NULL;
	duk_uint32_t arr_idx = DUK__NO_ARRAY_INDEX;
	duk_propdesc desc;
	duk_uint_t sanity;

	DUK_DDD(DUK_DDDPRINT("getprop: thr=%p, obj=%p, key=%p (obj -> %!T, key -> %!T)",
	                     (void *) thr, (void *) tv_obj, (void *) tv_key,
	                     (duk_tval *) tv_obj, (duk_tval *) tv_key));

	DUK_ASSERT(ctx != NULL);
	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(thr->heap != NULL);
	DUK_ASSERT(tv_obj != NULL);
	DUK_ASSERT(tv_key != NULL);

	DUK_ASSERT_VALSTACK_SPACE(thr, DUK__VALSTACK_SPACE);

	/*
	 *  Make a copy of tv_obj, tv_key, and tv_val to avoid any issues of
	 *  them being invalidated by a valstack resize.
	 *
	 *  XXX: this is now an overkill for many fast paths.  Rework this
	 *  to be faster (although switching to a valstack discipline might
	 *  be a better solution overall).
	 */

	DUK_TVAL_SET_TVAL(&tv_obj_copy, tv_obj);
	DUK_TVAL_SET_TVAL(&tv_key_copy, tv_key);
	tv_obj = &tv_obj_copy;
	tv_key = &tv_key_copy;

	/*
	 *  Coercion and fast path processing
	 */

	switch (DUK_TVAL_GET_TAG(tv_obj)) {
	case DUK_TAG_UNDEFINED:
	case DUK_TAG_NULL: {
		/* Note: unconditional throw */
		DUK_DDD(DUK_DDDPRINT("base object is undefined or null -> reject"));
		DUK_ERROR(thr, DUK_ERR_TYPE_ERROR, DUK_STR_INVALID_BASE);
		return 0;
	}

	case DUK_TAG_BOOLEAN: {
		DUK_DDD(DUK_DDDPRINT("base object is a boolean, start lookup from boolean prototype"));
		curr = thr->builtins[DUK_BIDX_BOOLEAN_PROTOTYPE];
		break;
	}

	case DUK_TAG_STRING: {
		duk_hstring *h = DUK_TVAL_GET_STRING(tv_obj);
		duk_int_t pop_count;

		if (DUK_TVAL_IS_NUMBER(tv_key)) {
			arr_idx = duk__tval_number_to_arr_idx(tv_key);
			DUK_DDD(DUK_DDDPRINT("base object string, key is a fast-path number; arr_idx %ld", (long) arr_idx));
			pop_count = 0;
		} else {
			arr_idx = duk__push_tval_to_hstring_arr_idx(ctx, tv_key, &key);
			DUK_ASSERT(key != NULL);
			DUK_DDD(DUK_DDDPRINT("base object string, key is a non-fast-path number; after "
			                     "coercion key is %!T, arr_idx %ld",
			                     (duk_tval *) duk_get_tval(ctx, -1), (long) arr_idx));
			pop_count = 1;
		}

		if (arr_idx != DUK__NO_ARRAY_INDEX &&
		    arr_idx < DUK_HSTRING_GET_CHARLEN(h)) {
			duk_pop_n(ctx, pop_count);
			duk_push_hstring(ctx, h);
			duk_substring(ctx, -1, arr_idx, arr_idx + 1);  /* [str] -> [substr] */

			DUK_DDD(DUK_DDDPRINT("-> %!T (base is string, key is an index inside string length "
			                     "after coercion -> return char)",
			                     (duk_tval *) duk_get_tval(ctx, -1)));
			return 1;
		}

		if (pop_count == 0) {
			/* This is a pretty awkward control flow, but we need to recheck the
			 * key coercion here.
			 */
			arr_idx = duk__push_tval_to_hstring_arr_idx(ctx, tv_key, &key);
			DUK_ASSERT(key != NULL);
			DUK_DDD(DUK_DDDPRINT("base object string, key is a non-fast-path number; after "
			                     "coercion key is %!T, arr_idx %ld",
			                     (duk_tval *) duk_get_tval(ctx, -1), (long) arr_idx));
		}

		if (key == DUK_HTHREAD_STRING_LENGTH(thr)) {
			duk_pop(ctx);  /* [key] -> [] */
			duk_push_uint(ctx, (duk_uint_t) DUK_HSTRING_GET_CHARLEN(h));  /* [] -> [res] */

			DUK_DDD(DUK_DDDPRINT("-> %!T (base is string, key is 'length' after coercion -> "
			                     "return string length)",
			                     (duk_tval *) duk_get_tval(ctx, -1)));
			return 1;
		}
		DUK_DDD(DUK_DDDPRINT("base object is a string, start lookup from string prototype"));
		curr = thr->builtins[DUK_BIDX_STRING_PROTOTYPE];
		goto lookup;  /* avoid double coercion */
	}

	case DUK_TAG_OBJECT: {
		duk_tval *tmp;

		curr = DUK_TVAL_GET_OBJECT(tv_obj);
		DUK_ASSERT(curr != NULL);

#if defined(DUK_USE_ES6_PROXY)
		if (DUK_UNLIKELY(DUK_HOBJECT_HAS_EXOTIC_PROXYOBJ(curr))) {
			duk_hobject *h_target;

			if (duk__proxy_check_prop(thr, curr, DUK_STRIDX_GET, tv_key, &h_target)) {
				/* -> [ ... trap handler ] */
				DUK_DDD(DUK_DDDPRINT("-> proxy object 'get' for key %!T", (duk_tval *) tv_key));
				duk_push_hobject(ctx, h_target);  /* target */
				duk_push_tval(ctx, tv_key);       /* P */
				duk_push_tval(ctx, tv_obj);       /* Receiver: Proxy object */
				duk_call_method(ctx, 3 /*nargs*/);

				/* Target object must be checked for a conflicting
				 * non-configurable property.
				 */
				arr_idx = duk__push_tval_to_hstring_arr_idx(ctx, tv_key, &key);
				DUK_ASSERT(key != NULL);

				if (duk__get_own_property_desc_raw(thr, h_target, key, arr_idx, &desc, DUK__DESC_FLAG_PUSH_VALUE)) {
					duk_tval *tv_hook = duk_require_tval(ctx, -3);  /* value from hook */
					duk_tval *tv_targ = duk_require_tval(ctx, -1);  /* value from target */
					duk_bool_t datadesc_reject;
					duk_bool_t accdesc_reject;

					DUK_DDD(DUK_DDDPRINT("proxy 'get': target has matching property %!O, check for "
					                     "conflicting property; tv_hook=%!T, tv_targ=%!T, desc.flags=0x%08lx, "
					                     "desc.get=%p, desc.set=%p",
					                     (duk_heaphdr *) key, (duk_tval *) tv_hook, (duk_tval *) tv_targ,
					                     (unsigned long) desc.flags,
					                     (void *) desc.get, (void *) desc.set));

					datadesc_reject = !(desc.flags & DUK_PROPDESC_FLAG_ACCESSOR) &&
					                  !(desc.flags & DUK_PROPDESC_FLAG_CONFIGURABLE) &&
					                  !(desc.flags & DUK_PROPDESC_FLAG_WRITABLE) &&
					                  !duk_js_samevalue(tv_hook, tv_targ);
					accdesc_reject = (desc.flags & DUK_PROPDESC_FLAG_ACCESSOR) &&
					                 !(desc.flags & DUK_PROPDESC_FLAG_CONFIGURABLE) &&
					                 (desc.get == NULL) &&
					                 !DUK_TVAL_IS_UNDEFINED(tv_hook);
					if (datadesc_reject || accdesc_reject) {
						DUK_ERROR(thr, DUK_ERR_TYPE_ERROR, DUK_STR_PROXY_REJECTED);
					}

					duk_pop_2(ctx);
				} else {
					duk_pop(ctx);
				}
				return 1;  /* return value */
			}

			curr = h_target;  /* resume lookup from target */
			DUK_TVAL_SET_OBJECT(tv_obj, curr);
		}
#endif  /* DUK_USE_ES6_PROXY */

		tmp = duk__shallow_fast_path_array_check_tval(thr, curr, tv_key);
		if (tmp) {
			duk_push_tval(ctx, tmp);

			DUK_DDD(DUK_DDDPRINT("-> %!T (base is object, key is a number, array part "
			                     "fast path)",
			                     (duk_tval *) duk_get_tval(ctx, -1)));
			return 1;
		}

		if (DUK_HOBJECT_HAS_EXOTIC_ARGUMENTS(curr)) {
			arr_idx = duk__push_tval_to_hstring_arr_idx(ctx, tv_key, &key);
			DUK_ASSERT(key != NULL);

			if (duk__check_arguments_map_for_get(thr, curr, key, &desc)) {
				DUK_DDD(DUK_DDDPRINT("-> %!T (base is object with arguments exotic behavior, "
				                     "key matches magically bound property -> skip standard "
				                     "Get with replacement value)",
				                     (duk_tval *) duk_get_tval(ctx, -1)));

				/* no need for 'caller' post-check, because 'key' must be an array index */

				duk_remove(ctx, -2);  /* [key result] -> [result] */
				return 1;
			}

			goto lookup;  /* avoid double coercion */
		}
		break;
	}

	/* Buffer has virtual properties similar to string, but indexed values
	 * are numbers, not 1-byte buffers/strings which would perform badly.
	 */
	case DUK_TAG_BUFFER: {
		duk_hbuffer *h = DUK_TVAL_GET_BUFFER(tv_obj);
		duk_int_t pop_count;

		/*
		 *  Because buffer values are often looped over, a number fast path
		 *  is important.
		 */

		if (DUK_TVAL_IS_NUMBER(tv_key)) {
			arr_idx = duk__tval_number_to_arr_idx(tv_key);
			DUK_DDD(DUK_DDDPRINT("base object buffer, key is a fast-path number; arr_idx %ld", (long) arr_idx));
			pop_count = 0;
		} else {
			arr_idx = duk__push_tval_to_hstring_arr_idx(ctx, tv_key, &key);
			DUK_ASSERT(key != NULL);
			DUK_DDD(DUK_DDDPRINT("base object buffer, key is a non-fast-path number; after "
			                     "coercion key is %!T, arr_idx %ld",
			                     (duk_tval *) duk_get_tval(ctx, -1), (long) arr_idx));
			pop_count = 1;
		}

		if (arr_idx != DUK__NO_ARRAY_INDEX &&
		    arr_idx < DUK_HBUFFER_GET_SIZE(h)) {
			duk_pop_n(ctx, pop_count);
			duk_push_int(ctx, ((duk_uint8_t *) DUK_HBUFFER_GET_DATA_PTR(thr->heap, h))[arr_idx]);

			DUK_DDD(DUK_DDDPRINT("-> %!T (base is buffer, key is an index inside buffer length "
			                     "after coercion -> return byte as number)",
			                     (duk_tval *) duk_get_tval(ctx, -1)));
			return 1;
		}

		if (pop_count == 0) {
			/* This is a pretty awkward control flow, but we need to recheck the
			 * key coercion here.
			 */
			arr_idx = duk__push_tval_to_hstring_arr_idx(ctx, tv_key, &key);
			DUK_ASSERT(key != NULL);
			DUK_DDD(DUK_DDDPRINT("base object buffer, key is a non-fast-path number; after "
			                     "coercion key is %!T, arr_idx %ld",
			                     (duk_tval *) duk_get_tval(ctx, -1), (long) arr_idx));
		}

		if (key == DUK_HTHREAD_STRING_LENGTH(thr)) {
			duk_pop(ctx);  /* [key] -> [] */
			duk_push_uint(ctx, (duk_uint_t) DUK_HBUFFER_GET_SIZE(h));  /* [] -> [res] */

			DUK_DDD(DUK_DDDPRINT("-> %!T (base is buffer, key is 'length' after coercion -> "
			                     "return buffer length)",
			                     (duk_tval *) duk_get_tval(ctx, -1)));
			return 1;
		}

		DUK_DDD(DUK_DDDPRINT("base object is a buffer, start lookup from buffer prototype"));
		curr = thr->builtins[DUK_BIDX_BUFFER_PROTOTYPE];
		goto lookup;  /* avoid double coercion */
	}

	case DUK_TAG_POINTER: {
		DUK_DDD(DUK_DDDPRINT("base object is a pointer, start lookup from pointer prototype"));
		curr = thr->builtins[DUK_BIDX_POINTER_PROTOTYPE];
		break;
	}

	case DUK_TAG_LIGHTFUNC: {
		duk_int_t lf_flags = DUK_TVAL_GET_LIGHTFUNC_FLAGS(tv_obj);

		/* Must coerce key: if key is an object, it may coerce to e.g. 'length'. */
		arr_idx = duk__push_tval_to_hstring_arr_idx(ctx, tv_key, &key);

		if (key == DUK_HTHREAD_STRING_LENGTH(thr)) {
			duk_int_t lf_len = DUK_LFUNC_FLAGS_GET_LENGTH(lf_flags);
			duk_pop(ctx);
			duk_push_int(ctx, lf_len);
			return 1;
		} else if (key == DUK_HTHREAD_STRING_NAME(thr)) {
			duk_pop(ctx);
			duk_push_lightfunc_name(ctx, tv_obj);
			return 1;
		}

		DUK_DDD(DUK_DDDPRINT("base object is a lightfunc, start lookup from function prototype"));
		curr = thr->builtins[DUK_BIDX_FUNCTION_PROTOTYPE];
		goto lookup;  /* avoid double coercion */
	}

	default: {
		/* number */
		DUK_DDD(DUK_DDDPRINT("base object is a number, start lookup from number prototype"));
		DUK_ASSERT(DUK_TVAL_IS_NUMBER(tv_obj));
		curr = thr->builtins[DUK_BIDX_NUMBER_PROTOTYPE];
		break;
	}
	}

	/* key coercion (unless already coerced above) */
	DUK_ASSERT(key == NULL);
	arr_idx = duk__push_tval_to_hstring_arr_idx(ctx, tv_key, &key);
	DUK_ASSERT(key != NULL);

	/*
	 *  Property lookup
	 */

 lookup:
	/* [key] (coerced) */
	DUK_ASSERT(curr != NULL);
	DUK_ASSERT(key != NULL);

	sanity = DUK_HOBJECT_PROTOTYPE_CHAIN_SANITY;
	do {
		if (!duk__get_own_property_desc_raw(thr, curr, key, arr_idx, &desc, DUK__DESC_FLAG_PUSH_VALUE)) {
			goto next_in_chain;
		}

		if (desc.get != NULL) {
			/* accessor with defined getter */
			DUK_ASSERT((desc.flags & DUK_PROPDESC_FLAG_ACCESSOR) != 0);

			duk_pop(ctx);                     /* [key undefined] -> [key] */
			duk_push_hobject(ctx, desc.get);
			duk_push_tval(ctx, tv_obj);       /* note: original, uncoerced base */
#ifdef DUK_USE_NONSTD_GETTER_KEY_ARGUMENT
			duk_dup(ctx, -3);
			duk_call_method(ctx, 1);          /* [key getter this key] -> [key retval] */
#else
			duk_call_method(ctx, 0);          /* [key getter this] -> [key retval] */
#endif
		} else {
			/* [key value] or [key undefined] */

			/* data property or accessor without getter */
			DUK_ASSERT(((desc.flags & DUK_PROPDESC_FLAG_ACCESSOR) == 0) ||
			           (desc.get == NULL));

			/* if accessor without getter, return value is undefined */
			DUK_ASSERT(((desc.flags & DUK_PROPDESC_FLAG_ACCESSOR) == 0) ||
			           duk_is_undefined(ctx, -1));

			/* Note: for an accessor without getter, falling through to
			 * check for "caller" exotic behavior is unnecessary as
			 * "undefined" will never activate the behavior.  But it does
			 * no harm, so we'll do it anyway.
			 */
		}

		goto found;  /* [key result] */

	 next_in_chain:
		/* XXX: option to pretend property doesn't exist if sanity limit is
		 * hit might be useful.
		 */
		if (sanity-- == 0) {
			DUK_ERROR(thr, DUK_ERR_INTERNAL_ERROR, DUK_STR_PROTOTYPE_CHAIN_LIMIT);
		}
		curr = DUK_HOBJECT_GET_PROTOTYPE(thr->heap, curr);
	} while (curr);

	/*
	 *  Not found
	 */

	duk_to_undefined(ctx, -1);  /* [key] -> [undefined] (default value) */

	DUK_DDD(DUK_DDDPRINT("-> %!T (not found)", (duk_tval *) duk_get_tval(ctx, -1)));
	return 0;

	/*
	 *  Found; post-processing (Function and arguments objects)
	 */

 found:
	/* [key result] */

#if !defined(DUK_USE_NONSTD_FUNC_CALLER_PROPERTY)
	/* Special behavior for 'caller' property of (non-bound) function objects
	 * and non-strict Arguments objects: if 'caller' -value- (!) is a strict
	 * mode function, throw a TypeError (E5 Sections 15.3.5.4, 10.6).
	 * Quite interestingly, a non-strict function with no formal arguments
	 * will get an arguments object -without- special 'caller' behavior!
	 *
	 * The E5.1 spec is a bit ambiguous if this special behavior applies when
	 * a bound function is the base value (not the 'caller' value): Section
	 * 15.3.4.5 (describing bind()) states that [[Get]] for bound functions
	 * matches that of Section 15.3.5.4 ([[Get]] for Function instances).
	 * However, Section 13.3.5.4 has "NOTE: Function objects created using
	 * Function.prototype.bind use the default [[Get]] internal method."
	 * The current implementation assumes this means that bound functions
	 * should not have the special [[Get]] behavior.
	 *
	 * The E5.1 spec is also a bit unclear if the TypeError throwing is
	 * applied if the 'caller' value is a strict bound function.  The
	 * current implementation will throw even for both strict non-bound
	 * and strict bound functions.
	 *
	 * See test-dev-strict-func-as-caller-prop-value.js for quite extensive
	 * tests.
	 *
	 * This exotic behavior is disabled when the non-standard 'caller' property
	 * is enabled, as it conflicts with the free use of 'caller'.
	 */
	if (key == DUK_HTHREAD_STRING_CALLER(thr) &&
	    DUK_TVAL_IS_OBJECT(tv_obj)) {
		duk_hobject *orig = DUK_TVAL_GET_OBJECT(tv_obj);
		DUK_ASSERT(orig != NULL);

		if (DUK_HOBJECT_IS_NONBOUND_FUNCTION(orig) ||
		    DUK_HOBJECT_HAS_EXOTIC_ARGUMENTS(orig)) {
			duk_hobject *h;

			/* XXX: The TypeError is currently not applied to bound
			 * functions because the 'strict' flag is not copied by
			 * bind().  This may or may not be correct, the specification
			 * only refers to the value being a "strict mode Function
			 * object" which is ambiguous.
			 */
			DUK_ASSERT(!DUK_HOBJECT_HAS_BOUND(orig));

			h = duk_get_hobject(ctx, -1);  /* NULL if not an object */
			if (h &&
			    DUK_HOBJECT_IS_FUNCTION(h) &&
			    DUK_HOBJECT_HAS_STRICT(h)) {
				/* XXX: sufficient to check 'strict', assert for 'is function' */
				DUK_ERROR(thr, DUK_ERR_TYPE_ERROR, DUK_STR_STRICT_CALLER_READ);
			}
		}
	}
#endif   /* !DUK_USE_NONSTD_FUNC_CALLER_PROPERTY */

	duk_remove(ctx, -2);  /* [key result] -> [result] */

	DUK_DDD(DUK_DDDPRINT("-> %!T (found)", (duk_tval *) duk_get_tval(ctx, -1)));
	return 1;
}

/*
 *  HASPROP: Ecmascript property existence check ("in" operator).
 *
 *  Interestingly, the 'in' operator does not do any coercion of
 *  the target object.
 */

DUK_INTERNAL duk_bool_t duk_hobject_hasprop(duk_hthread *thr, duk_tval *tv_obj, duk_tval *tv_key) {
	duk_context *ctx = (duk_context *) thr;
	duk_tval tv_key_copy;
	duk_hobject *obj;
	duk_hstring *key;
	duk_uint32_t arr_idx;
	duk_bool_t rc;
	duk_propdesc desc;

	DUK_DDD(DUK_DDDPRINT("hasprop: thr=%p, obj=%p, key=%p (obj -> %!T, key -> %!T)",
	                     (void *) thr, (void *) tv_obj, (void *) tv_key,
	                     (duk_tval *) tv_obj, (duk_tval *) tv_key));

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(thr->heap != NULL);
	DUK_ASSERT(tv_obj != NULL);
	DUK_ASSERT(tv_key != NULL);
	DUK_ASSERT_VALSTACK_SPACE(thr, DUK__VALSTACK_SPACE);

	DUK_TVAL_SET_TVAL(&tv_key_copy, tv_key);
	tv_key = &tv_key_copy;

	/*
	 *  The 'in' operator requires an object as its right hand side,
	 *  throwing a TypeError unconditionally if this is not the case.
	 *
	 *  However, lightfuncs need to behave like fully fledged objects
	 *  here to be maximally transparent, so we need to handle them
	 *  here.
	 */

	/* XXX: Refactor key coercion so that it's only called once.  It can't
	 * be trivially lifted here because the object must be type checked
	 * first.
	 */

	if (DUK_TVAL_IS_OBJECT(tv_obj)) {
		obj = DUK_TVAL_GET_OBJECT(tv_obj);
		DUK_ASSERT(obj != NULL);

		arr_idx = duk__push_tval_to_hstring_arr_idx(ctx, tv_key, &key);
	} else if (DUK_TVAL_IS_LIGHTFUNC(tv_obj)) {
		arr_idx = duk__push_tval_to_hstring_arr_idx(ctx, tv_key, &key);
		if (duk__key_is_lightfunc_ownprop(thr, key)) {
			/* FOUND */
			rc = 1;
			goto pop_and_return;
		}

		/* If not found, resume existence check from Function.prototype.
		 * We can just substitute the value in this case; nothing will
		 * need the original base value (as would be the case with e.g.
		 * setters/getters.
		 */
		obj = thr->builtins[DUK_BIDX_FUNCTION_PROTOTYPE];
	} else {
		/* Note: unconditional throw */
		DUK_DDD(DUK_DDDPRINT("base object is not an object -> reject"));
		DUK_ERROR(thr, DUK_ERR_TYPE_ERROR, DUK_STR_INVALID_BASE);
	}

	/* XXX: fast path for arrays? */

	DUK_ASSERT(key != NULL);
	DUK_ASSERT(obj != NULL);
	DUK_UNREF(arr_idx);

#if defined(DUK_USE_ES6_PROXY)
	if (DUK_UNLIKELY(DUK_HOBJECT_HAS_EXOTIC_PROXYOBJ(obj))) {
		duk_hobject *h_target;
		duk_bool_t tmp_bool;

		/* XXX: the key in 'key in obj' is string coerced before we're called
		 * (which is the required behavior in E5/E5.1/E6) so the key is a string
		 * here already.
		 */

		if (duk__proxy_check_prop(thr, obj, DUK_STRIDX_HAS, tv_key, &h_target)) {
			/* [ ... key trap handler ] */
			DUK_DDD(DUK_DDDPRINT("-> proxy object 'has' for key %!T", (duk_tval *) tv_key));
			duk_push_hobject(ctx, h_target);  /* target */
			duk_push_tval(ctx, tv_key);       /* P */
			duk_call_method(ctx, 2 /*nargs*/);
			tmp_bool = duk_to_boolean(ctx, -1);
			if (!tmp_bool) {
				/* Target object must be checked for a conflicting
				 * non-configurable property.
				 */

				if (duk__get_own_property_desc_raw(thr, h_target, key, arr_idx, &desc, 0 /*flags*/)) {  /* don't push value */
					DUK_DDD(DUK_DDDPRINT("proxy 'has': target has matching property %!O, check for "
					                     "conflicting property; desc.flags=0x%08lx, "
					                     "desc.get=%p, desc.set=%p",
					                     (duk_heaphdr *) key, (unsigned long) desc.flags,
					                     (void *) desc.get, (void *) desc.set));
					/* XXX: Extensibility check for target uses IsExtensible().  If we
					 * implemented the isExtensible trap and didn't reject proxies as
					 * proxy targets, it should be respected here.
					 */
					if (!((desc.flags & DUK_PROPDESC_FLAG_CONFIGURABLE) &&  /* property is configurable and */
					      DUK_HOBJECT_HAS_EXTENSIBLE(h_target))) {          /* ... target is extensible */
						DUK_ERROR(thr, DUK_ERR_TYPE_ERROR, DUK_STR_PROXY_REJECTED);
					}
				}
			}

			duk_pop_2(ctx);  /* [ key trap_result ] -> [] */
			return tmp_bool;
		}

		obj = h_target;  /* resume check from proxy target */
	}
#endif  /* DUK_USE_ES6_PROXY */

	/* XXX: inline into a prototype walking loop? */

	rc = duk__get_property_desc(thr, obj, key, &desc, 0 /*flags*/);  /* don't push value */
	/* fall through */

 pop_and_return:
	duk_pop(ctx);  /* [ key ] -> [] */
	return rc;
}

/*
 *  HASPROP variant used internally.
 *
 *  This primitive must never throw an error, callers rely on this.
 *  In particular, don't throw an error for prototype loops; instead,
 *  pretend like the property doesn't exist if a prototype sanity limit
 *  is reached.
 *
 *  Does not implement proxy behavior: if applied to a proxy object,
 *  returns key existence on the proxy object itself.
 */

DUK_INTERNAL duk_bool_t duk_hobject_hasprop_raw(duk_hthread *thr, duk_hobject *obj, duk_hstring *key) {
	duk_propdesc dummy;

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(thr->heap != NULL);
	DUK_ASSERT(obj != NULL);
	DUK_ASSERT(key != NULL);

	DUK_ASSERT_VALSTACK_SPACE(thr, DUK__VALSTACK_SPACE);

	return duk__get_property_desc(thr, obj, key, &dummy, DUK__DESC_FLAG_IGNORE_PROTOLOOP);  /* don't push value */
}

/*
 *  Helper: handle Array object 'length' write which automatically
 *  deletes properties, see E5 Section 15.4.5.1, step 3.  This is
 *  quite tricky to get right.
 *
 *  Used by duk_hobject_putprop().
 */

DUK_LOCAL duk_uint32_t duk__get_old_array_length(duk_hthread *thr, duk_hobject *obj, duk_propdesc *temp_desc) {
	duk_bool_t rc;
	duk_tval *tv;
	duk_uint32_t res;

	DUK_ASSERT_VALSTACK_SPACE(thr, DUK__VALSTACK_SPACE);

	/* This function is only called for objects with array exotic behavior.
	 * The [[DefineOwnProperty]] algorithm for arrays requires that
	 * 'length' can never have a value outside the unsigned 32-bit range,
	 * attempt to write such a value is a RangeError.  Here we can thus
	 * assert for this.  When Duktape internals go around the official
	 * property write interface (doesn't happen often) this assumption is
	 * easy to accidentally break, so such code must be written carefully.
	 * See test-bi-array-push-maxlen.js.
	 */

	rc = duk__get_own_property_desc_raw(thr, obj, DUK_HTHREAD_STRING_LENGTH(thr), DUK__NO_ARRAY_INDEX, temp_desc, 0 /*flags*/);  /* don't push value */
	DUK_UNREF(rc);
	DUK_ASSERT(rc != 0);  /* arrays MUST have a 'length' property */
	DUK_ASSERT(temp_desc->e_idx >= 0);

	tv = DUK_HOBJECT_E_GET_VALUE_TVAL_PTR(thr->heap, obj, temp_desc->e_idx);
	DUK_ASSERT(DUK_TVAL_IS_NUMBER(tv));  /* array 'length' is always a number, as we coerce it */
	DUK_ASSERT(DUK_TVAL_GET_NUMBER(tv) >= 0.0);
	DUK_ASSERT(DUK_TVAL_GET_NUMBER(tv) <= (double) 0xffffffffUL);
	res = (duk_uint32_t) DUK_TVAL_GET_NUMBER(tv);
	DUK_ASSERT((duk_double_t) res == DUK_TVAL_GET_NUMBER(tv));

	return res;
}

DUK_LOCAL duk_uint32_t duk__to_new_array_length_checked(duk_hthread *thr) {
	duk_context *ctx = (duk_context *) thr;
	duk_uint32_t res;

	/* Input value should be on stack top and will be coerced and
	 * left on stack top.  Refuse to update an Array's 'length'
	 * to a value outside the 32-bit range.
	 */

	res = ((duk_uint32_t) duk_to_number(ctx, -1)) & 0xffffffffUL;
	if (res != duk_get_number(ctx, -1)) {
		DUK_ERROR(thr, DUK_ERR_RANGE_ERROR, DUK_STR_INVALID_ARRAY_LENGTH);
	}
	return res;
}

/* Delete elements required by a smaller length, taking into account
 * potentially non-configurable elements.  Returns non-zero if all
 * elements could be deleted, and zero if all or some elements could
 * not be deleted.  Also writes final "target length" to 'out_result_len'.
 * This is the length value that should go into the 'length' property
 * (must be set by the caller).  Never throws an error.
 */
DUK_LOCAL
duk_bool_t duk__handle_put_array_length_smaller(duk_hthread *thr,
                                                duk_hobject *obj,
                                                duk_uint32_t old_len,
                                                duk_uint32_t new_len,
                                                duk_bool_t force_flag,
                                                duk_uint32_t *out_result_len) {
	duk_uint32_t target_len;
	duk_uint_fast32_t i;
	duk_uint32_t arr_idx;
	duk_hstring *key;
	duk_tval *tv;
	duk_tval tv_tmp;
	duk_bool_t rc;

	DUK_DDD(DUK_DDDPRINT("new array length smaller than old (%ld -> %ld), "
	                     "probably need to remove elements",
	                     (long) old_len, (long) new_len));

	/*
	 *  New length is smaller than old length, need to delete properties above
	 *  the new length.
	 *
	 *  If array part exists, this is straightforward: array entries cannot
	 *  be non-configurable so this is guaranteed to work.
	 *
	 *  If array part does not exist, array-indexed values are scattered
	 *  in the entry part, and some may not be configurable (preventing length
	 *  from becoming lower than their index + 1).  To handle the algorithm
	 *  in E5 Section 15.4.5.1, step l correctly, we scan the entire property
	 *  set twice.
	 */

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(obj != NULL);
	DUK_ASSERT(new_len < old_len);
	DUK_ASSERT(out_result_len != NULL);
	DUK_ASSERT_VALSTACK_SPACE(thr, DUK__VALSTACK_SPACE);

	if (DUK_HOBJECT_HAS_ARRAY_PART(obj)) {
		/*
		 *  All defined array-indexed properties are in the array part
		 *  (we assume the array part is comprehensive), and all array
		 *  entries are writable, configurable, and enumerable.  Thus,
		 *  nothing can prevent array entries from being deleted.
		 */

		DUK_DDD(DUK_DDDPRINT("have array part, easy case"));

		if (old_len < DUK_HOBJECT_GET_ASIZE(obj)) {
			/* XXX: assertion that entries >= old_len are already unused */
			i = old_len;
		} else {
			i = DUK_HOBJECT_GET_ASIZE(obj);
		}
		DUK_ASSERT(i <= DUK_HOBJECT_GET_ASIZE(obj));

		while (i > new_len) {
			i--;
			tv = DUK_HOBJECT_A_GET_VALUE_PTR(thr->heap, obj, i);
			DUK_TVAL_SET_TVAL(&tv_tmp, tv);
			DUK_TVAL_SET_UNDEFINED_UNUSED(tv);
			DUK_TVAL_DECREF(thr, &tv_tmp);
		}

		*out_result_len = new_len;
		return 1;
	} else {
		/*
		 *  Entries part is a bit more complex
		 */

		/* Stage 1: find highest preventing non-configurable entry (if any).
		 * When forcing, ignore non-configurability.
		 */

		DUK_DDD(DUK_DDDPRINT("no array part, slow case"));

		DUK_DDD(DUK_DDDPRINT("array length write, no array part, stage 1: find target_len "
		                     "(highest preventing non-configurable entry (if any))"));

		target_len = new_len;
		if (force_flag) {
			DUK_DDD(DUK_DDDPRINT("array length write, no array part; force flag -> skip stage 1"));
			goto skip_stage1;
		}
		for (i = 0; i < DUK_HOBJECT_GET_ENEXT(obj); i++) {
			key = DUK_HOBJECT_E_GET_KEY(thr->heap, obj, i);
			if (!key) {
				DUK_DDD(DUK_DDDPRINT("skip entry index %ld: null key", (long) i));
				continue;
			}
			if (!DUK_HSTRING_HAS_ARRIDX(key)) {
				DUK_DDD(DUK_DDDPRINT("skip entry index %ld: key not an array index", (long) i));
				continue;
			}

			DUK_ASSERT(DUK_HSTRING_HAS_ARRIDX(key));  /* XXX: macro checks for array index flag, which is unnecessary here */
			arr_idx = DUK_HSTRING_GET_ARRIDX_SLOW(key);
			DUK_ASSERT(arr_idx != DUK__NO_ARRAY_INDEX);
			DUK_ASSERT(arr_idx < old_len);  /* consistency requires this */

			if (arr_idx < new_len) {
				DUK_DDD(DUK_DDDPRINT("skip entry index %ld: key is array index %ld, below new_len",
				                     (long) i, (long) arr_idx));
				continue;
			}
			if (DUK_HOBJECT_E_SLOT_IS_CONFIGURABLE(thr->heap, obj, i)) {
				DUK_DDD(DUK_DDDPRINT("skip entry index %ld: key is a relevant array index %ld, but configurable",
				                     (long) i, (long) arr_idx));
				continue;
			}

			/* relevant array index is non-configurable, blocks write */
			if (arr_idx >= target_len) {
				DUK_DDD(DUK_DDDPRINT("entry at index %ld has arr_idx %ld, is not configurable, "
				                     "update target_len %ld -> %ld",
				                     (long) i, (long) arr_idx, (long) target_len,
				                     (long) (arr_idx + 1)));
				target_len = arr_idx + 1;
			}
		}
	 skip_stage1:

		/* stage 2: delete configurable entries above target length */

		DUK_DDD(DUK_DDDPRINT("old_len=%ld, new_len=%ld, target_len=%ld",
		                     (long) old_len, (long) new_len, (long) target_len));

		DUK_DDD(DUK_DDDPRINT("array length write, no array part, stage 2: remove "
		                     "entries >= target_len"));

		for (i = 0; i < DUK_HOBJECT_GET_ENEXT(obj); i++) {
			key = DUK_HOBJECT_E_GET_KEY(thr->heap, obj, i);
			if (!key) {
				DUK_DDD(DUK_DDDPRINT("skip entry index %ld: null key", (long) i));
				continue;
			}
			if (!DUK_HSTRING_HAS_ARRIDX(key)) {
				DUK_DDD(DUK_DDDPRINT("skip entry index %ld: key not an array index", (long) i));
				continue;
			}

			DUK_ASSERT(DUK_HSTRING_HAS_ARRIDX(key));  /* XXX: macro checks for array index flag, which is unnecessary here */
			arr_idx = DUK_HSTRING_GET_ARRIDX_SLOW(key);
			DUK_ASSERT(arr_idx != DUK__NO_ARRAY_INDEX);
			DUK_ASSERT(arr_idx < old_len);  /* consistency requires this */

			if (arr_idx < target_len) {
				DUK_DDD(DUK_DDDPRINT("skip entry index %ld: key is array index %ld, below target_len",
				                     (long) i, (long) arr_idx));
				continue;
			}
			DUK_ASSERT(force_flag || DUK_HOBJECT_E_SLOT_IS_CONFIGURABLE(thr->heap, obj, i));  /* stage 1 guarantees */

			DUK_DDD(DUK_DDDPRINT("delete entry index %ld: key is array index %ld",
			                     (long) i, (long) arr_idx));

			/*
			 *  Slow delete, but we don't care as we're already in a very slow path.
			 *  The delete always succeeds: key has no exotic behavior, property
			 *  is configurable, and no resize occurs.
			 */
			rc = duk_hobject_delprop_raw(thr, obj, key, force_flag ? DUK_DELPROP_FLAG_FORCE : 0);
			DUK_UNREF(rc);
			DUK_ASSERT(rc != 0);
		}

		/* stage 3: update length (done by caller), decide return code */

		DUK_DDD(DUK_DDDPRINT("array length write, no array part, stage 3: update length (done by caller)"));

		*out_result_len = target_len;

		if (target_len == new_len) {
			DUK_DDD(DUK_DDDPRINT("target_len matches new_len, return success"));
			return 1;
		}
		DUK_DDD(DUK_DDDPRINT("target_len does not match new_len (some entry prevented "
		                     "full length adjustment), return error"));
		return 0;
	}

	DUK_UNREACHABLE();
}

/* XXX: is valstack top best place for argument? */
DUK_LOCAL duk_bool_t duk__handle_put_array_length(duk_hthread *thr, duk_hobject *obj) {
	duk_context *ctx = (duk_context *) thr;
	duk_propdesc desc;
	duk_uint32_t old_len;
	duk_uint32_t new_len;
	duk_uint32_t result_len;
	duk_tval *tv;
	duk_bool_t rc;

	DUK_DDD(DUK_DDDPRINT("handling a put operation to array 'length' exotic property, "
	                     "new val: %!T",
	                     (duk_tval *) duk_get_tval(ctx, -1)));

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(ctx != NULL);
	DUK_ASSERT(obj != NULL);

	DUK_ASSERT_VALSTACK_SPACE(thr, DUK__VALSTACK_SPACE);

	DUK_ASSERT(duk_is_valid_index(ctx, -1));

	/*
	 *  Get old and new length
	 */

	old_len = duk__get_old_array_length(thr, obj, &desc);
	duk_dup(ctx, -1);  /* [in_val in_val] */
	new_len = duk__to_new_array_length_checked(thr);
	duk_pop(ctx);  /* [in_val in_val] -> [in_val] */
	DUK_DDD(DUK_DDDPRINT("old_len=%ld, new_len=%ld", (long) old_len, (long) new_len));

	/*
	 *  Writability check
	 */

	if (!(desc.flags & DUK_PROPDESC_FLAG_WRITABLE)) {
		DUK_DDD(DUK_DDDPRINT("length is not writable, fail"));
		return 0;
	}

	/*
	 *  New length not lower than old length => no changes needed
	 *  (not even array allocation).
	 */

	if (new_len >= old_len) {
		DUK_DDD(DUK_DDDPRINT("new length is higher than old length, just update length, no deletions"));

		DUK_ASSERT(desc.e_idx >= 0);
		DUK_ASSERT(!DUK_HOBJECT_E_SLOT_IS_ACCESSOR(thr->heap, obj, desc.e_idx));
		tv = DUK_HOBJECT_E_GET_VALUE_TVAL_PTR(thr->heap, obj, desc.e_idx);
		DUK_ASSERT(DUK_TVAL_IS_NUMBER(tv));
		DUK_TVAL_SET_NUMBER(tv, (duk_double_t) new_len);  /* no decref needed for a number */
		DUK_ASSERT(DUK_TVAL_IS_NUMBER(tv));
		return 1;
	}

	DUK_DDD(DUK_DDDPRINT("new length is lower than old length, probably must delete entries"));

	/*
	 *  New length lower than old length => delete elements, then
	 *  update length.
	 *
	 *  Note: even though a bunch of elements have been deleted, the 'desc' is
	 *  still valid as properties haven't been resized (and entries compacted).
	 */

	rc = duk__handle_put_array_length_smaller(thr, obj, old_len, new_len, 0 /*force_flag*/, &result_len);
	DUK_ASSERT(result_len >= new_len && result_len <= old_len);

	DUK_ASSERT(desc.e_idx >= 0);
	DUK_ASSERT(!DUK_HOBJECT_E_SLOT_IS_ACCESSOR(thr->heap, obj, desc.e_idx));
	tv = DUK_HOBJECT_E_GET_VALUE_TVAL_PTR(thr->heap, obj, desc.e_idx);
	DUK_ASSERT(DUK_TVAL_IS_NUMBER(tv));
	DUK_TVAL_SET_NUMBER(tv, (duk_double_t) result_len);  /* no decref needed for a number */
	DUK_ASSERT(DUK_TVAL_IS_NUMBER(tv));

	/* XXX: shrink array allocation or entries compaction here? */

	return rc;
}

/*
 *  PUTPROP: Ecmascript property write.
 *
 *  Unlike Ecmascript primitive which returns nothing, returns 1 to indicate
 *  success and 0 to indicate failure (assuming throw is not set).
 *
 *  This is an extremely tricky function.  Some examples:
 *
 *    * Currently a decref may trigger a GC, which may compact an object's
 *      property allocation.  Consequently, any entry indices (e_idx) will
 *      be potentially invalidated by a decref.
 *
 *    * Exotic behaviors (strings, arrays, arguments object) require,
 *      among other things:
 *
 *      - Preprocessing before and postprocessing after an actual property
 *        write.  For example, array index write requires pre-checking the
 *        array 'length' property for access control, and may require an
 *        array 'length' update after the actual write has succeeded (but
 *        not if it fails).
 *
 *      - Deletion of multiple entries, as a result of array 'length' write.
 *
 *    * Input values are taken as pointers which may point to the valstack.
 *      If valstack is resized because of the put (this may happen at least
 *      when the array part is abandoned), the pointers can be invalidated.
 *      (We currently make a copy of all of the input values to avoid issues.)
 */

DUK_INTERNAL duk_bool_t duk_hobject_putprop(duk_hthread *thr, duk_tval *tv_obj, duk_tval *tv_key, duk_tval *tv_val, duk_bool_t throw_flag) {
	duk_context *ctx = (duk_context *) thr;
	duk_tval tv_obj_copy;
	duk_tval tv_key_copy;
	duk_tval tv_val_copy;
	duk_hobject *orig = NULL;  /* NULL if tv_obj is primitive */
	duk_hobject *curr;
	duk_hstring *key = NULL;
	duk_propdesc desc;
	duk_tval *tv;
	duk_uint32_t arr_idx;
	duk_bool_t rc;
	duk_int_t e_idx;
	duk_uint_t sanity;
	duk_uint32_t new_array_length = 0;  /* 0 = no update */

	DUK_DDD(DUK_DDDPRINT("putprop: thr=%p, obj=%p, key=%p, val=%p, throw=%ld "
	                     "(obj -> %!T, key -> %!T, val -> %!T)",
	                     (void *) thr, (void *) tv_obj, (void *) tv_key, (void *) tv_val,
	                     (long) throw_flag, (duk_tval *) tv_obj, (duk_tval *) tv_key, (duk_tval *) tv_val));

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(thr->heap != NULL);
	DUK_ASSERT(ctx != NULL);
	DUK_ASSERT(tv_obj != NULL);
	DUK_ASSERT(tv_key != NULL);
	DUK_ASSERT(tv_val != NULL);

	DUK_ASSERT_VALSTACK_SPACE(thr, DUK__VALSTACK_SPACE);

	/*
	 *  Make a copy of tv_obj, tv_key, and tv_val to avoid any issues of
	 *  them being invalidated by a valstack resize.
	 *
	 *  XXX: this is an overkill for some paths, so optimize this later
	 *  (or maybe switch to a stack arguments model entirely).
	 */

	DUK_TVAL_SET_TVAL(&tv_obj_copy, tv_obj);
	DUK_TVAL_SET_TVAL(&tv_key_copy, tv_key);
	DUK_TVAL_SET_TVAL(&tv_val_copy, tv_val);
	tv_obj = &tv_obj_copy;
	tv_key = &tv_key_copy;
	tv_val = &tv_val_copy;

	/*
	 *  Coercion and fast path processing.
	 */

	switch (DUK_TVAL_GET_TAG(tv_obj)) {
	case DUK_TAG_UNDEFINED:
	case DUK_TAG_NULL: {
		/* Note: unconditional throw */
		DUK_DDD(DUK_DDDPRINT("base object is undefined or null -> reject (object=%!iT)",
		                     (duk_tval *) tv_obj));
		DUK_ERROR(thr, DUK_ERR_TYPE_ERROR, DUK_STR_INVALID_BASE);
		return 0;
	}

	case DUK_TAG_BOOLEAN: {
		DUK_DDD(DUK_DDDPRINT("base object is a boolean, start lookup from boolean prototype"));
		curr = thr->builtins[DUK_BIDX_BOOLEAN_PROTOTYPE];
		break;
	}

	case DUK_TAG_STRING: {
		duk_hstring *h = DUK_TVAL_GET_STRING(tv_obj);

		/*
		 *  Note: currently no fast path for array index writes.
		 *  They won't be possible anyway as strings are immutable.
		 */

		DUK_ASSERT(key == NULL);
		arr_idx = duk__push_tval_to_hstring_arr_idx(ctx, tv_key, &key);
		DUK_ASSERT(key != NULL);

		if (key == DUK_HTHREAD_STRING_LENGTH(thr)) {
			goto fail_not_writable;
		}

		if (arr_idx != DUK__NO_ARRAY_INDEX &&
		    arr_idx < DUK_HSTRING_GET_CHARLEN(h)) {
			goto fail_not_writable;
		}

		DUK_DDD(DUK_DDDPRINT("base object is a string, start lookup from string prototype"));
		curr = thr->builtins[DUK_BIDX_STRING_PROTOTYPE];
		goto lookup;  /* avoid double coercion */
	}

	case DUK_TAG_OBJECT: {
		/* Note: no fast paths for property put now */
		orig = DUK_TVAL_GET_OBJECT(tv_obj);
		DUK_ASSERT(orig != NULL);

#if defined(DUK_USE_ES6_PROXY)
		if (DUK_UNLIKELY(DUK_HOBJECT_HAS_EXOTIC_PROXYOBJ(orig))) {
			duk_hobject *h_target;
			duk_bool_t tmp_bool;

			if (duk__proxy_check_prop(thr, orig, DUK_STRIDX_SET, tv_key, &h_target)) {
				/* -> [ ... trap handler ] */
				DUK_DDD(DUK_DDDPRINT("-> proxy object 'set' for key %!T", (duk_tval *) tv_key));
				duk_push_hobject(ctx, h_target);  /* target */
				duk_push_tval(ctx, tv_key);       /* P */
				duk_push_tval(ctx, tv_val);       /* V */
				duk_push_tval(ctx, tv_obj);       /* Receiver: Proxy object */
				duk_call_method(ctx, 4 /*nargs*/);
				tmp_bool = duk_to_boolean(ctx, -1);
				duk_pop(ctx);
				if (!tmp_bool) {
					goto fail_proxy_rejected;
				}

				/* Target object must be checked for a conflicting
				 * non-configurable property.
				 */
				arr_idx = duk__push_tval_to_hstring_arr_idx(ctx, tv_key, &key);
				DUK_ASSERT(key != NULL);

				if (duk__get_own_property_desc_raw(thr, h_target, key, arr_idx, &desc, DUK__DESC_FLAG_PUSH_VALUE)) {
					duk_tval *tv_targ = duk_require_tval(ctx, -1);
					duk_bool_t datadesc_reject;
					duk_bool_t accdesc_reject;

					DUK_DDD(DUK_DDDPRINT("proxy 'set': target has matching property %!O, check for "
					                     "conflicting property; tv_val=%!T, tv_targ=%!T, desc.flags=0x%08lx, "
					                     "desc.get=%p, desc.set=%p",
					                     (duk_heaphdr *) key, (duk_tval *) tv_val, (duk_tval *) tv_targ,
					                     (unsigned long) desc.flags,
					                     (void *) desc.get, (void *) desc.set));

					datadesc_reject = !(desc.flags & DUK_PROPDESC_FLAG_ACCESSOR) &&
					                  !(desc.flags & DUK_PROPDESC_FLAG_CONFIGURABLE) &&
					                  !(desc.flags & DUK_PROPDESC_FLAG_WRITABLE) &&
					                  !duk_js_samevalue(tv_val, tv_targ);
					accdesc_reject = (desc.flags & DUK_PROPDESC_FLAG_ACCESSOR) &&
					                 !(desc.flags & DUK_PROPDESC_FLAG_CONFIGURABLE) &&
					                 (desc.set == NULL);
					if (datadesc_reject || accdesc_reject) {
						DUK_ERROR(thr, DUK_ERR_TYPE_ERROR, DUK_STR_PROXY_REJECTED);
					}

					duk_pop_2(ctx);
				} else {
					duk_pop(ctx);
				}
				return 1;  /* success */
			}

			orig = h_target;  /* resume write to target */
			DUK_TVAL_SET_OBJECT(tv_obj, orig);
		}
#endif  /* DUK_USE_ES6_PROXY */

		curr = orig;
		break;
	}

	case DUK_TAG_BUFFER: {
		duk_hbuffer *h = DUK_TVAL_GET_BUFFER(tv_obj);
		duk_int_t pop_count = 0;

		/*
		 *  Because buffer values may be looped over and read/written
		 *  from, an array index fast path is important.
		 */

		if (DUK_TVAL_IS_NUMBER(tv_key)) {
			arr_idx = duk__tval_number_to_arr_idx(tv_key);
			DUK_DDD(DUK_DDDPRINT("base object buffer, key is a fast-path number; arr_idx %ld", (long) arr_idx));
			pop_count = 0;
		} else {
			arr_idx = duk__push_tval_to_hstring_arr_idx(ctx, tv_key, &key);
			DUK_ASSERT(key != NULL);
			DUK_DDD(DUK_DDDPRINT("base object buffer, key is a non-fast-path number; after "
			                     "coercion key is %!T, arr_idx %ld",
			                     (duk_tval *) duk_get_tval(ctx, -1), (long) arr_idx));
			pop_count = 1;
		}

		if (arr_idx != DUK__NO_ARRAY_INDEX &&
		    arr_idx < DUK_HBUFFER_GET_SIZE(h)) {
			duk_uint8_t *data;
			DUK_DDD(DUK_DDDPRINT("writing to buffer data at index %ld", (long) arr_idx));
			data = (duk_uint8_t *) DUK_HBUFFER_GET_DATA_PTR(thr->heap, h);
			duk_push_tval(ctx, tv_val);
			data[arr_idx] = (duk_uint8_t) duk_to_number(ctx, -1);
			pop_count++;
			duk_pop_n(ctx, pop_count);
			DUK_DDD(DUK_DDDPRINT("result: success (buffer data write)"));
			return 1;
		}

		if (pop_count == 0) {
			/* This is a pretty awkward control flow, but we need to recheck the
			 * key coercion here.
			 */
			arr_idx = duk__push_tval_to_hstring_arr_idx(ctx, tv_key, &key);
			DUK_ASSERT(key != NULL);
			DUK_DDD(DUK_DDDPRINT("base object buffer, key is a non-fast-path number; after "
			                     "coercion key is %!T, arr_idx %ld",
			                     (duk_tval *) duk_get_tval(ctx, -1), (long) arr_idx));
		}

		if (key == DUK_HTHREAD_STRING_LENGTH(thr)) {
			goto fail_not_writable;
		}

		DUK_DDD(DUK_DDDPRINT("base object is a buffer, start lookup from buffer prototype"));
		curr = thr->builtins[DUK_BIDX_BUFFER_PROTOTYPE];
		goto lookup;  /* avoid double coercion */
	}

	case DUK_TAG_POINTER: {
		DUK_DDD(DUK_DDDPRINT("base object is a pointer, start lookup from pointer prototype"));
		curr = thr->builtins[DUK_BIDX_POINTER_PROTOTYPE];
		break;
	}

	case DUK_TAG_LIGHTFUNC: {
		/* All lightfunc own properties are non-writable and the lightfunc
		 * is considered non-extensible.  However, the write may be captured
		 * by an inherited setter which means we can't stop the lookup here.
		 */

		arr_idx = duk__push_tval_to_hstring_arr_idx(ctx, tv_key, &key);

		if (duk__key_is_lightfunc_ownprop(thr, key)) {
			goto fail_not_writable;
		}

		DUK_DDD(DUK_DDDPRINT("base object is a lightfunc, start lookup from function prototype"));
		curr = thr->builtins[DUK_BIDX_FUNCTION_PROTOTYPE];
		goto lookup;  /* avoid double coercion */
	}

	default: {
		/* number */
		DUK_DDD(DUK_DDDPRINT("base object is a number, start lookup from number prototype"));
		DUK_ASSERT(DUK_TVAL_IS_NUMBER(tv_obj));
		curr = thr->builtins[DUK_BIDX_NUMBER_PROTOTYPE];
		break;
	}
	}

	DUK_ASSERT(key == NULL);
	arr_idx = duk__push_tval_to_hstring_arr_idx(ctx, tv_key, &key);
	DUK_ASSERT(key != NULL);

 lookup:

	/*
	 *  Check whether the property already exists in the prototype chain.
	 *  Note that the actual write goes into the original base object
	 *  (except if an accessor property captures the write).
	 */

	/* [key] */

	DUK_ASSERT(curr != NULL);
	sanity = DUK_HOBJECT_PROTOTYPE_CHAIN_SANITY;
	do {
		if (!duk__get_own_property_desc_raw(thr, curr, key, arr_idx, &desc, 0 /*flags*/)) {  /* don't push value */
			goto next_in_chain;
		}

		if (desc.flags & DUK_PROPDESC_FLAG_ACCESSOR) {
			/*
			 *  Found existing accessor property (own or inherited).
			 *  Call setter with 'this' set to orig, and value as the only argument.
			 *
			 *  Note: no exotic arguments object behavior, because [[Put]] never
			 *  calls [[DefineOwnProperty]] (E5 Section 8.12.5, step 5.b).
			 */

			duk_hobject *setter;

			DUK_DD(DUK_DDPRINT("put to an own or inherited accessor, calling setter"));

			setter = DUK_HOBJECT_E_GET_VALUE_SETTER(thr->heap, curr, desc.e_idx);
			if (!setter) {
				goto fail_no_setter;
			}
			duk_push_hobject(ctx, setter);
			duk_push_tval(ctx, tv_obj);  /* note: original, uncoerced base */
			duk_push_tval(ctx, tv_val);  /* [key setter this val] */
#ifdef DUK_USE_NONSTD_SETTER_KEY_ARGUMENT
			duk_dup(ctx, -4);
			duk_call_method(ctx, 2);     /* [key setter this val key] -> [key retval] */
#else
			duk_call_method(ctx, 1);     /* [key setter this val] -> [key retval] */
#endif
			duk_pop(ctx);                /* ignore retval -> [key] */
			goto success_no_arguments_exotic;
		}

		if (orig == NULL) {
			/*
			 *  Found existing own or inherited plain property, but original
			 *  base is a primitive value.
			 */
			DUK_DD(DUK_DDPRINT("attempt to create a new property in a primitive base object"));
			goto fail_base_primitive;
		}

		if (curr != orig) {
			/*
			 *  Found existing inherited plain property.
			 *  Do an access control check, and if OK, write
			 *  new property to 'orig'.
			 */
			if (!DUK_HOBJECT_HAS_EXTENSIBLE(orig)) {
				DUK_DD(DUK_DDPRINT("found existing inherited plain property, but original object is not extensible"));
				goto fail_not_extensible;
			}
			if (!(desc.flags & DUK_PROPDESC_FLAG_WRITABLE)) {
				DUK_DD(DUK_DDPRINT("found existing inherited plain property, original object is extensible, but inherited property is not writable"));
				goto fail_not_writable;
			}
			DUK_DD(DUK_DDPRINT("put to new property, object extensible, inherited property found and is writable"));
			goto create_new;
		} else {
			/*
			 *  Found existing own (non-inherited) plain property.
			 *  Do an access control check and update in place.
			 */

			if (!(desc.flags & DUK_PROPDESC_FLAG_WRITABLE)) {
				DUK_DD(DUK_DDPRINT("found existing own (non-inherited) plain property, but property is not writable"));
				goto fail_not_writable;
			}
			if (desc.flags & DUK_PROPDESC_FLAG_VIRTUAL) {
				DUK_DD(DUK_DDPRINT("found existing own (non-inherited) virtual property, property is writable"));
				if (DUK_HOBJECT_HAS_EXOTIC_BUFFEROBJ(curr)) {
					duk_hbuffer *h;

					DUK_DD(DUK_DDPRINT("writable virtual property is in buffer object"));
					h = duk_hobject_get_internal_value_buffer(thr->heap, curr);
					DUK_ASSERT(h != NULL);

					if (arr_idx != DUK__NO_ARRAY_INDEX &&
					    arr_idx < DUK_HBUFFER_GET_SIZE(h)) {
						duk_uint8_t *data;
						DUK_DDD(DUK_DDDPRINT("writing to buffer data at index %ld", (long) arr_idx));
						data = (duk_uint8_t *) DUK_HBUFFER_GET_DATA_PTR(thr->heap, h);
						duk_push_tval(ctx, tv_val);
						data[arr_idx] = (duk_uint8_t) duk_to_number(ctx, -1);
						duk_pop(ctx);
						goto success_no_arguments_exotic;
					}
				}

				goto fail_internal;  /* should not happen */
			}
			DUK_DD(DUK_DDPRINT("put to existing own plain property, property is writable"));
			goto update_old;
		}
		DUK_UNREACHABLE();

	 next_in_chain:
		/* XXX: option to pretend property doesn't exist if sanity limit is
		 * hit might be useful.
		 */
		if (sanity-- == 0) {
			DUK_ERROR(thr, DUK_ERR_INTERNAL_ERROR, DUK_STR_PROTOTYPE_CHAIN_LIMIT);
		}
		curr = DUK_HOBJECT_GET_PROTOTYPE(thr->heap, curr);
	} while (curr);

	/*
	 *  Property not found in prototype chain.
	 */

	DUK_DDD(DUK_DDDPRINT("property not found in prototype chain"));

	if (orig == NULL) {
		DUK_DD(DUK_DDPRINT("attempt to create a new property in a primitive base object"));
		goto fail_base_primitive;
	}

	if (!DUK_HOBJECT_HAS_EXTENSIBLE(orig)) {
		DUK_DD(DUK_DDPRINT("put to a new property (not found in prototype chain), but original object not extensible"));
		goto fail_not_extensible;
	}

	goto create_new;

 update_old:

	/*
	 *  Update an existing property of the base object.
	 */

	/* [key] */

	DUK_DDD(DUK_DDDPRINT("update an existing property of the original object"));

	DUK_ASSERT(orig != NULL);

	/* Although there are writable virtual properties (e.g. plain buffer
	 * and buffer object number indices), they are handled before we come
	 * here.
	 */
	DUK_ASSERT((desc.flags & DUK_PROPDESC_FLAG_VIRTUAL) == 0);
	DUK_ASSERT(desc.a_idx >= 0 || desc.e_idx >= 0);

	if (DUK_HOBJECT_HAS_EXOTIC_ARRAY(orig) &&
	    key == DUK_HTHREAD_STRING_LENGTH(thr)) {
		/*
		 *  Write to 'length' of an array is a very complex case
		 *  handled in a helper which updates both the array elements
		 *  and writes the new 'length'.  The write may result in an
		 *  unconditional RangeError or a partial write (indicated
		 *  by a return code).
		 *
		 *  Note: the helper has an unnecessary writability check
		 *  for 'length', we already know it is writable.
		 */

		DUK_DDD(DUK_DDDPRINT("writing existing 'length' property to array exotic, invoke complex helper"));

		/* XXX: the helper currently assumes stack top contains new
		 * 'length' value and the whole calling convention is not very
		 * compatible with what we need.
		 */

		duk_push_tval(ctx, tv_val);  /* [key val] */
		rc = duk__handle_put_array_length(thr, orig);
		duk_pop(ctx);  /* [key val] -> [key] */
		if (!rc) {
			goto fail_array_length_partial;
		}

		/* key is 'length', cannot match argument exotic behavior */
		goto success_no_arguments_exotic;
	}

	if (desc.e_idx >= 0) {
		duk_tval tv_tmp;

		tv = DUK_HOBJECT_E_GET_VALUE_TVAL_PTR(thr->heap, orig, desc.e_idx);
		DUK_DDD(DUK_DDDPRINT("previous entry value: %!iT", (duk_tval *) tv));
		DUK_TVAL_SET_TVAL(&tv_tmp, tv);
		DUK_TVAL_SET_TVAL(tv, tv_val);
		DUK_TVAL_INCREF(thr, tv);
		DUK_TVAL_DECREF(thr, &tv_tmp);  /* note: may trigger gc and props compaction, must be last */
		/* don't touch property attributes or hash part */
		DUK_DD(DUK_DDPRINT("put to an existing entry at index %ld -> new value %!iT",
		                   (long) desc.e_idx, (duk_tval *) tv));
	} else {
		/* Note: array entries are always writable, so the writability check
		 * above is pointless for them.  The check could be avoided with some
		 * refactoring but is probably not worth it.
		 */
		duk_tval tv_tmp;

		DUK_ASSERT(desc.a_idx >= 0);
		tv = DUK_HOBJECT_A_GET_VALUE_PTR(thr->heap, orig, desc.a_idx);
		DUK_DDD(DUK_DDDPRINT("previous array value: %!iT", (duk_tval *) tv));
		DUK_TVAL_SET_TVAL(&tv_tmp, tv);
		DUK_TVAL_SET_TVAL(tv, tv_val);
		DUK_TVAL_INCREF(thr, tv);
		DUK_TVAL_DECREF(thr, &tv_tmp);  /* note: may trigger gc and props compaction, must be last */
		DUK_DD(DUK_DDPRINT("put to an existing array entry at index %ld -> new value %!iT",
		                   (long) desc.a_idx, (duk_tval *) tv));
	}

	/* Regardless of whether property is found in entry or array part,
	 * it may have arguments exotic behavior (array indices may reside
	 * in entry part for abandoned / non-existent array parts).
	 */
	goto success_with_arguments_exotic;

 create_new:

	/*
	 *  Create a new property in the original object.
	 *
	 *  Exotic properties need to be reconsidered here from a write
	 *  perspective (not just property attributes perspective).
	 *  However, the property does not exist in the object already,
	 *  so this limits the kind of exotic properties that apply.
	 */

	/* [key] */

	DUK_DDD(DUK_DDDPRINT("create new property to original object"));

	DUK_ASSERT(orig != NULL);

	/* Not possible because array object 'length' is present
	 * from its creation and cannot be deleted, and is thus
	 * caught as an existing property above.
	 */
	DUK_ASSERT(!(DUK_HOBJECT_HAS_EXOTIC_ARRAY(orig) &&
	             key == DUK_HTHREAD_STRING_LENGTH(thr)));

	if (DUK_HOBJECT_HAS_EXOTIC_ARRAY(orig) &&
	    arr_idx != DUK__NO_ARRAY_INDEX) {
		/* automatic length update */
		duk_uint32_t old_len;

		old_len = duk__get_old_array_length(thr, orig, &desc);

		if (arr_idx >= old_len) {
			DUK_DDD(DUK_DDDPRINT("write new array entry requires length update "
			                     "(arr_idx=%ld, old_len=%ld)",
			                     (long) arr_idx, (long) old_len));

			if (!(desc.flags & DUK_PROPDESC_FLAG_WRITABLE)) {
				DUK_DD(DUK_DDPRINT("attempt to extend array, but array 'length' is not writable"));
				goto fail_not_writable;
			}

			/* Note: actual update happens once write has been completed
			 * without error below.  The write should always succeed
			 * from a specification viewpoint, but we may e.g. run out
			 * of memory.  It's safer in this order.
			 */

			DUK_ASSERT(arr_idx != 0xffffffffUL);
			new_array_length = arr_idx + 1;  /* flag for later write */
		} else {
			DUK_DDD(DUK_DDDPRINT("write new array entry does not require length update "
			                     "(arr_idx=%ld, old_len=%ld)",
			                     (long) arr_idx, (long) old_len));
		}
	}

 /* write_to_array_part: */

	/*
	 *  Write to array part?
	 *
	 *  Note: array abandonding requires a property resize which uses
	 *  'rechecks' valstack for temporaries and may cause any existing
	 *  valstack pointers to be invalidated.  To protect against this,
	 *  tv_obj, tv_key, and tv_val are copies of the original inputs.
	 */

	if (arr_idx != DUK__NO_ARRAY_INDEX &&
	    DUK_HOBJECT_HAS_ARRAY_PART(orig)) {
		if (arr_idx < DUK_HOBJECT_GET_ASIZE(orig)) {
			goto no_array_growth;
		}

		/*
		 *  Array needs to grow, but we don't want it becoming too sparse.
		 *  If it were to become sparse, abandon array part, moving all
		 *  array entries into the entries part (for good).
		 *
		 *  Since we don't keep track of actual density (used vs. size) of
		 *  the array part, we need to estimate somehow.  The check is made
		 *  in two parts:
		 *
		 *    - Check whether the resize need is small compared to the
		 *      current size (relatively); if so, resize without further
		 *      checking (essentially we assume that the original part is
		 *      "dense" so that the result would be dense enough).
		 *
		 *    - Otherwise, compute the resize using an actual density
		 *      measurement based on counting the used array entries.
		 */

		DUK_DDD(DUK_DDDPRINT("write to new array requires array resize, decide whether to do a "
		                     "fast resize without abandon check (arr_idx=%ld, old_size=%ld)",
		                     (long) arr_idx, (long) DUK_HOBJECT_GET_ASIZE(orig)));

		if (duk__abandon_array_slow_check_required(arr_idx, DUK_HOBJECT_GET_ASIZE(orig))) {
			duk_uint32_t old_used;
			duk_uint32_t old_size;

			DUK_DDD(DUK_DDDPRINT("=> fast check is NOT OK, do slow check for array abandon"));

			duk__compute_a_stats(thr, orig, &old_used, &old_size);

			DUK_DDD(DUK_DDDPRINT("abandon check, array stats: old_used=%ld, old_size=%ld, arr_idx=%ld",
			                     (long) old_used, (long) old_size, (long) arr_idx));

			/* Note: intentionally use approximations to shave a few instructions:
			 *   a_used = old_used  (accurate: old_used + 1)
			 *   a_size = arr_idx   (accurate: arr_idx + 1)
			 */
			if (duk__abandon_array_density_check(old_used, arr_idx)) {
				DUK_DD(DUK_DDPRINT("write to new array entry beyond current length, "
				                   "decided to abandon array part (would become too sparse)"));

				/* abandoning requires a props allocation resize and
				 * 'rechecks' the valstack, invalidating any existing
				 * valstack value pointers!
				 */
				duk__abandon_array_checked(thr, orig);
				DUK_ASSERT(!DUK_HOBJECT_HAS_ARRAY_PART(orig));

				goto write_to_entry_part;
			}

			DUK_DDD(DUK_DDDPRINT("=> decided to keep array part"));
		} else {
			DUK_DDD(DUK_DDDPRINT("=> fast resize is OK"));
		}

		DUK_DD(DUK_DDPRINT("write to new array entry beyond current length, "
		                   "decided to extend current allocation"));

		duk__grow_props_for_array_item(thr, orig, arr_idx);

	 no_array_growth:

		/* Note: assume array part is comprehensive, so that either
		 * the write goes to the array part, or we've abandoned the
		 * array above (and will not come here).
		 */

		DUK_ASSERT(DUK_HOBJECT_HAS_ARRAY_PART(orig));
		DUK_ASSERT(arr_idx < DUK_HOBJECT_GET_ASIZE(orig));

		tv = DUK_HOBJECT_A_GET_VALUE_PTR(thr->heap, orig, arr_idx);
		/* prev value must be unused, no decref */
		DUK_ASSERT(DUK_TVAL_IS_UNDEFINED_UNUSED(tv));
		DUK_TVAL_SET_TVAL(tv, tv_val);
		DUK_TVAL_INCREF(thr, tv);
		DUK_DD(DUK_DDPRINT("put to new array entry: %ld -> %!T",
		                   (long) arr_idx, (duk_tval *) tv));

		/* Note: array part values are [[Writable]], [[Enumerable]],
		 * and [[Configurable]] which matches the required attributes
		 * here.
		 */
		goto entry_updated;
	}

 write_to_entry_part:

	/*
	 *  Write to entry part
	 */

	/* entry allocation updates hash part and increases the key
	 * refcount; may need a props allocation resize but doesn't
	 * 'recheck' the valstack.
	 */
	e_idx = duk__alloc_entry_checked(thr, orig, key);
	DUK_ASSERT(e_idx >= 0);

	tv = DUK_HOBJECT_E_GET_VALUE_TVAL_PTR(thr->heap, orig, e_idx);
	/* prev value can be garbage, no decref */
	DUK_TVAL_SET_TVAL(tv, tv_val);
	DUK_TVAL_INCREF(thr, tv);
	DUK_HOBJECT_E_SET_FLAGS(thr->heap, orig, e_idx, DUK_PROPDESC_FLAGS_WEC);
	goto entry_updated;

 entry_updated:

	/*
	 *  Possible pending array length update, which must only be done
	 *  if the actual entry write succeeded.
	 */

	if (new_array_length > 0) {
		/*
		 *  Note: zero works as a "no update" marker because the new length
		 *  can never be zero after a new property is written.
		 *
		 *  Note: must re-lookup because calls above (e.g. duk__alloc_entry_checked())
		 *  may realloc and compact properties and hence change e_idx.
		 */

		DUK_DDD(DUK_DDDPRINT("write successful, pending array length update to: %ld",
		                     (long) new_array_length));

		rc = duk__get_own_property_desc_raw(thr, orig, DUK_HTHREAD_STRING_LENGTH(thr), DUK__NO_ARRAY_INDEX, &desc, 0 /*flags*/);  /* don't push value */
		DUK_UNREF(rc);
		DUK_ASSERT(rc != 0);
		DUK_ASSERT(desc.e_idx >= 0);

		tv = DUK_HOBJECT_E_GET_VALUE_TVAL_PTR(thr->heap, orig, desc.e_idx);
		DUK_ASSERT(DUK_TVAL_IS_NUMBER(tv));
		DUK_TVAL_SET_NUMBER(tv, (duk_double_t) new_array_length);  /* no need for decref/incref because value is a number */
	}

	/*
	 *  Arguments exotic behavior not possible for new properties: all
	 *  magically bound properties are initially present in the arguments
	 *  object, and if they are deleted, the binding is also removed from
	 *  parameter map.
	 */

	goto success_no_arguments_exotic;

 success_with_arguments_exotic:

	/*
	 *  Arguments objects have exotic [[DefineOwnProperty]] which updates
	 *  the internal 'map' of arguments for writes to currently mapped
	 *  arguments.  More conretely, writes to mapped arguments generate
	 *  a write to a bound variable.
	 *
	 *  The [[Put]] algorithm invokes [[DefineOwnProperty]] for existing
	 *  data properties and new properties, but not for existing accessors.
	 *  Hence, in E5 Section 10.6 ([[DefinedOwnProperty]] algorithm), we
	 *  have a Desc with 'Value' (and possibly other properties too), and
	 *  we end up in step 5.b.i.
	 */

	if (arr_idx != DUK__NO_ARRAY_INDEX &&
	    DUK_HOBJECT_HAS_EXOTIC_ARGUMENTS(orig)) {
		/* Note: only numbered indices are relevant, so arr_idx fast reject
		 * is good (this is valid unless there are more than 4**32-1 arguments).
		 */

		DUK_DDD(DUK_DDDPRINT("putprop successful, arguments exotic behavior needed"));

		/* Note: we can reuse 'desc' here */

		/* XXX: top of stack must contain value, which helper doesn't touch,
		 * rework to use tv_val directly?
		 */

		duk_push_tval(ctx, tv_val);
		(void) duk__check_arguments_map_for_put(thr, orig, key, &desc, throw_flag);
		duk_pop(ctx);
	}
	/* fall thru */

 success_no_arguments_exotic:
	/* shared exit path now */
	DUK_DDD(DUK_DDDPRINT("result: success"));
	duk_pop(ctx);  /* remove key */
	return 1;

 fail_proxy_rejected:
	DUK_DDD(DUK_DDDPRINT("result: error, proxy rejects"));
	if (throw_flag) {
		DUK_ERROR(thr, DUK_ERR_TYPE_ERROR, DUK_STR_PROXY_REJECTED);
	}
	/* Note: no key on stack */
	return 0;

 fail_base_primitive:
	DUK_DDD(DUK_DDDPRINT("result: error, base primitive"));
	if (throw_flag) {
		DUK_ERROR(thr, DUK_ERR_TYPE_ERROR, DUK_STR_INVALID_BASE);
	}
	duk_pop(ctx);  /* remove key */
	return 0;

 fail_not_extensible:
	DUK_DDD(DUK_DDDPRINT("result: error, not extensible"));
	if (throw_flag) {
		DUK_ERROR(thr, DUK_ERR_TYPE_ERROR, DUK_STR_NOT_EXTENSIBLE);
	}
	duk_pop(ctx);  /* remove key */
	return 0;

 fail_not_writable:
	DUK_DDD(DUK_DDDPRINT("result: error, not writable"));
	if (throw_flag) {
		DUK_ERROR(thr, DUK_ERR_TYPE_ERROR, DUK_STR_NOT_WRITABLE);
	}
	duk_pop(ctx);  /* remove key */
	return 0;

 fail_array_length_partial:
	DUK_DDD(DUK_DDDPRINT("result: error, array length write only partially successful"));
	if (throw_flag) {
		DUK_ERROR(thr, DUK_ERR_TYPE_ERROR, DUK_STR_ARRAY_LENGTH_WRITE_FAILED);
	}
	duk_pop(ctx);  /* remove key */
	return 0;

 fail_no_setter:
	DUK_DDD(DUK_DDDPRINT("result: error, accessor property without setter"));
	if (throw_flag) {
		DUK_ERROR(thr, DUK_ERR_TYPE_ERROR, DUK_STR_SETTER_UNDEFINED);
	}
	duk_pop(ctx);  /* remove key */
	return 0;

 fail_internal:
	DUK_DDD(DUK_DDDPRINT("result: error, internal"));
	if (throw_flag) {
		DUK_ERROR(thr, DUK_ERR_TYPE_ERROR, DUK_STR_INTERNAL_ERROR);
	}
	duk_pop(ctx);  /* remove key */
	return 0;
}

/*
 *  Ecmascript compliant [[Delete]](P, Throw).
 */

DUK_INTERNAL duk_bool_t duk_hobject_delprop_raw(duk_hthread *thr, duk_hobject *obj, duk_hstring *key, duk_small_uint_t flags) {
	duk_propdesc desc;
	duk_tval *tv;
	duk_tval tv_tmp;
	duk_uint32_t arr_idx;
	duk_bool_t throw_flag;
	duk_bool_t force_flag;

	throw_flag = (flags & DUK_DELPROP_FLAG_THROW);
	force_flag = (flags & DUK_DELPROP_FLAG_FORCE);

	DUK_DDD(DUK_DDDPRINT("delprop_raw: thr=%p, obj=%p, key=%p, throw=%ld, force=%ld (obj -> %!O, key -> %!O)",
	                     (void *) thr, (void *) obj, (void *) key, (long) throw_flag, (long) force_flag,
	                     (duk_heaphdr *) obj, (duk_heaphdr *) key));

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(thr->heap != NULL);
	DUK_ASSERT(obj != NULL);
	DUK_ASSERT(key != NULL);

	DUK_ASSERT_VALSTACK_SPACE(thr, DUK__VALSTACK_SPACE);

	arr_idx = DUK_HSTRING_GET_ARRIDX_FAST(key);

	/* 0 = don't push current value */
	if (!duk__get_own_property_desc_raw(thr, obj, key, arr_idx, &desc, 0 /*flags*/)) {  /* don't push value */
		DUK_DDD(DUK_DDDPRINT("property not found, succeed always"));
		goto success;
	}

	if ((desc.flags & DUK_PROPDESC_FLAG_CONFIGURABLE) == 0 && !force_flag) {
		goto fail_not_configurable;
	}
	if (desc.a_idx < 0 && desc.e_idx < 0) {
		/* Currently there are no deletable virtual properties, but
		 * with force_flag we might attempt to delete one.
		 */
		goto fail_virtual;
	}

	if (desc.a_idx >= 0) {
		DUK_ASSERT(desc.e_idx < 0);

		tv = DUK_HOBJECT_A_GET_VALUE_PTR(thr->heap, obj, desc.a_idx);
		DUK_TVAL_SET_TVAL(&tv_tmp, tv);
		DUK_TVAL_SET_UNDEFINED_UNUSED(tv);
		DUK_TVAL_DECREF(thr, &tv_tmp);
		goto success;
	} else {
		DUK_ASSERT(desc.a_idx < 0);

		/* remove hash entry (no decref) */
#if defined(DUK_USE_HOBJECT_HASH_PART)
		if (desc.h_idx >= 0) {
			duk_uint32_t *h_base = DUK_HOBJECT_H_GET_BASE(thr->heap, obj);

			DUK_DDD(DUK_DDDPRINT("removing hash entry at h_idx %ld", (long) desc.h_idx));
			DUK_ASSERT(DUK_HOBJECT_GET_HSIZE(obj) > 0);
			DUK_ASSERT((duk_uint32_t) desc.h_idx < DUK_HOBJECT_GET_HSIZE(obj));
			h_base[desc.h_idx] = DUK__HASH_DELETED;
		} else {
			DUK_ASSERT(DUK_HOBJECT_GET_HSIZE(obj) == 0);
		}
#else
		DUK_ASSERT(DUK_HOBJECT_GET_HSIZE(obj) == 0);
#endif

		/* remove value */
		DUK_DDD(DUK_DDDPRINT("before removing value, e_idx %ld, key %p, key at slot %p",
		                     (long) desc.e_idx, (void *) key, (void *) DUK_HOBJECT_E_GET_KEY(thr->heap, obj, desc.e_idx)));
		DUK_DDD(DUK_DDDPRINT("removing value at e_idx %ld", (long) desc.e_idx));
		if (DUK_HOBJECT_E_SLOT_IS_ACCESSOR(thr->heap, obj, desc.e_idx)) {
			duk_hobject *tmp;

			tmp = DUK_HOBJECT_E_GET_VALUE_GETTER(thr->heap, obj, desc.e_idx);
			DUK_HOBJECT_E_SET_VALUE_GETTER(thr->heap, obj, desc.e_idx, NULL);
			DUK_UNREF(tmp);
			DUK_HOBJECT_DECREF(thr, tmp);

			tmp = DUK_HOBJECT_E_GET_VALUE_SETTER(thr->heap, obj, desc.e_idx);
			DUK_HOBJECT_E_SET_VALUE_SETTER(thr->heap, obj, desc.e_idx, NULL);
			DUK_UNREF(tmp);
			DUK_HOBJECT_DECREF(thr, tmp);
		} else {
			tv = DUK_HOBJECT_E_GET_VALUE_TVAL_PTR(thr->heap, obj, desc.e_idx);
			DUK_TVAL_SET_TVAL(&tv_tmp, tv);
			DUK_TVAL_SET_UNDEFINED_UNUSED(tv);
			DUK_TVAL_DECREF(thr, &tv_tmp);
		}
		/* this is not strictly necessary because if key == NULL, value MUST be ignored */
		DUK_HOBJECT_E_SET_FLAGS(thr->heap, obj, desc.e_idx, 0);
		DUK_TVAL_SET_UNDEFINED_UNUSED(DUK_HOBJECT_E_GET_VALUE_TVAL_PTR(thr->heap, obj, desc.e_idx));

		/* remove key */
		DUK_DDD(DUK_DDDPRINT("before removing key, e_idx %ld, key %p, key at slot %p",
		                     (long) desc.e_idx, (void *) key, (void *) DUK_HOBJECT_E_GET_KEY(thr->heap, obj, desc.e_idx)));
		DUK_DDD(DUK_DDDPRINT("removing key at e_idx %ld", (long) desc.e_idx));
		DUK_ASSERT(key == DUK_HOBJECT_E_GET_KEY(thr->heap, obj, desc.e_idx));
		DUK_HOBJECT_E_SET_KEY(thr->heap, obj, desc.e_idx, NULL);
		DUK_HSTRING_DECREF(thr, key);
		goto success;
	}

	DUK_UNREACHABLE();

 success:
	/*
	 *  Argument exotic [[Delete]] behavior (E5 Section 10.6) is
	 *  a post-check, keeping arguments internal 'map' in sync with
	 *  any successful deletes (note that property does not need to
	 *  exist for delete to 'succeed').
	 *
	 *  Delete key from 'map'.  Since 'map' only contains array index
	 *  keys, we can use arr_idx for a fast skip.
	 */

	DUK_DDD(DUK_DDDPRINT("delete successful, check for arguments exotic behavior"));

	if (arr_idx != DUK__NO_ARRAY_INDEX && DUK_HOBJECT_HAS_EXOTIC_ARGUMENTS(obj)) {
		/* Note: only numbered indices are relevant, so arr_idx fast reject
		 * is good (this is valid unless there are more than 4**32-1 arguments).
		 */

		DUK_DDD(DUK_DDDPRINT("delete successful, arguments exotic behavior needed"));

		/* Note: we can reuse 'desc' here */
		(void) duk__check_arguments_map_for_delete(thr, obj, key, &desc);
	}

	DUK_DDD(DUK_DDDPRINT("delete successful"));
	return 1;

 fail_virtual:
	DUK_DDD(DUK_DDDPRINT("delete failed: property found, force flag, but virtual"));

	if (throw_flag) {
		DUK_ERROR(thr, DUK_ERR_TYPE_ERROR, DUK_STR_PROPERTY_IS_VIRTUAL);
	}
	return 0;

 fail_not_configurable:
	DUK_DDD(DUK_DDDPRINT("delete failed: property found, not configurable"));

	if (throw_flag) {
		DUK_ERROR(thr, DUK_ERR_TYPE_ERROR, DUK_STR_NOT_CONFIGURABLE);
	}
	return 0;
}

/*
 *  DELPROP: Ecmascript property deletion.
 */

DUK_INTERNAL duk_bool_t duk_hobject_delprop(duk_hthread *thr, duk_tval *tv_obj, duk_tval *tv_key, duk_bool_t throw_flag) {
	duk_context *ctx = (duk_context *) thr;
	duk_hstring *key = NULL;
#if defined(DUK_USE_ES6_PROXY)
	duk_propdesc desc;
#endif
	duk_int_t entry_top;
	duk_uint32_t arr_idx = DUK__NO_ARRAY_INDEX;
	duk_bool_t rc;

	DUK_DDD(DUK_DDDPRINT("delprop: thr=%p, obj=%p, key=%p (obj -> %!T, key -> %!T)",
	                     (void *) thr, (void *) tv_obj, (void *) tv_key,
	                     (duk_tval *) tv_obj, (duk_tval *) tv_key));

	DUK_ASSERT(ctx != NULL);
	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(thr->heap != NULL);
	DUK_ASSERT(tv_obj != NULL);
	DUK_ASSERT(tv_key != NULL);

	DUK_ASSERT_VALSTACK_SPACE(thr, DUK__VALSTACK_SPACE);

	/* Storing the entry top is cheaper here to ensure stack is correct at exit,
	 * as there are several paths out.
	 */
	entry_top = duk_get_top(ctx);

	if (DUK_TVAL_IS_UNDEFINED(tv_obj) ||
	    DUK_TVAL_IS_NULL(tv_obj)) {
		DUK_DDD(DUK_DDDPRINT("base object is undefined or null -> reject"));
		goto fail_invalid_base_uncond;
	}

	duk_push_tval(ctx, tv_obj);
	duk_push_tval(ctx, tv_key);

	tv_obj = duk_get_tval(ctx, -2);
	if (DUK_TVAL_IS_OBJECT(tv_obj)) {
		duk_hobject *obj = DUK_TVAL_GET_OBJECT(tv_obj);
		DUK_ASSERT(obj != NULL);

#if defined(DUK_USE_ES6_PROXY)
		if (DUK_UNLIKELY(DUK_HOBJECT_HAS_EXOTIC_PROXYOBJ(obj))) {
			duk_hobject *h_target;
			duk_bool_t tmp_bool;

			/* Note: proxy handling must happen before key is string coerced. */

			if (duk__proxy_check_prop(thr, obj, DUK_STRIDX_DELETE_PROPERTY, tv_key, &h_target)) {
				/* -> [ ... trap handler ] */
				DUK_DDD(DUK_DDDPRINT("-> proxy object 'deleteProperty' for key %!T", (duk_tval *) tv_key));
				duk_push_hobject(ctx, h_target);  /* target */
				duk_push_tval(ctx, tv_key);       /* P */
				duk_call_method(ctx, 2 /*nargs*/);
				tmp_bool = duk_to_boolean(ctx, -1);
				duk_pop(ctx);
				if (!tmp_bool) {
					goto fail_proxy_rejected;  /* retval indicates delete failed */
				}

				/* Target object must be checked for a conflicting
				 * non-configurable property.
				 */
				arr_idx = duk__push_tval_to_hstring_arr_idx(ctx, tv_key, &key);
				DUK_ASSERT(key != NULL);

				if (duk__get_own_property_desc_raw(thr, h_target, key, arr_idx, &desc, 0 /*flags*/)) {  /* don't push value */
					int desc_reject;

					DUK_DDD(DUK_DDDPRINT("proxy 'deleteProperty': target has matching property %!O, check for "
					                     "conflicting property; desc.flags=0x%08lx, "
					                     "desc.get=%p, desc.set=%p",
					                     (duk_heaphdr *) key, (unsigned long) desc.flags,
					                     (void *) desc.get, (void *) desc.set));

					desc_reject = !(desc.flags & DUK_PROPDESC_FLAG_CONFIGURABLE);
					if (desc_reject) {
						/* unconditional */
						DUK_ERROR(thr, DUK_ERR_TYPE_ERROR, DUK_STR_PROXY_REJECTED);
					}
				}
				rc = 1;  /* success */
				goto done_rc;
			}

			obj = h_target;  /* resume delete to target */
		}
#endif  /* DUK_USE_ES6_PROXY */

		duk_to_string(ctx, -1);
		key = duk_get_hstring(ctx, -1);
		DUK_ASSERT(key != NULL);

		rc = duk_hobject_delprop_raw(thr, obj, key, throw_flag ? DUK_DELPROP_FLAG_THROW : 0);
		goto done_rc;
	} else if (DUK_TVAL_IS_STRING(tv_obj)) {
		/* XXX: unnecessary string coercion for array indices,
		 * intentional to keep small.
		 */
		duk_hstring *h = DUK_TVAL_GET_STRING(tv_obj);
		DUK_ASSERT(h != NULL);

		duk_to_string(ctx, -1);
		key = duk_get_hstring(ctx, -1);
		DUK_ASSERT(key != NULL);

		if (key == DUK_HTHREAD_STRING_LENGTH(thr)) {
			goto fail_not_configurable;
		}

		arr_idx = DUK_HSTRING_GET_ARRIDX_FAST(key);

		if (arr_idx != DUK__NO_ARRAY_INDEX &&
		    arr_idx < DUK_HSTRING_GET_CHARLEN(h)) {
			goto fail_not_configurable;
		}
	} else if (DUK_TVAL_IS_BUFFER(tv_obj)) {
		/* XXX: unnecessary string coercion for array indices,
		 * intentional to keep small; some overlap with string
		 * handling.
		 */
		duk_hbuffer *h = DUK_TVAL_GET_BUFFER(tv_obj);
		DUK_ASSERT(h != NULL);

		duk_to_string(ctx, -1);
		key = duk_get_hstring(ctx, -1);
		DUK_ASSERT(key != NULL);

		if (key == DUK_HTHREAD_STRING_LENGTH(thr)) {
			goto fail_not_configurable;
		}

		arr_idx = DUK_HSTRING_GET_ARRIDX_FAST(key);

		if (arr_idx != DUK__NO_ARRAY_INDEX &&
		    arr_idx < DUK_HBUFFER_GET_SIZE(h)) {
			goto fail_not_configurable;
		}
	} else if (DUK_TVAL_IS_LIGHTFUNC(tv_obj)) {
		/* Lightfunc virtual properties are non-configurable, so
		 * reject if match any of them.
		 */

		duk_to_string(ctx, -1);
		key = duk_get_hstring(ctx, -1);
		DUK_ASSERT(key != NULL);

		if (duk__key_is_lightfunc_ownprop(thr, key)) {
			goto fail_not_configurable;
		}
	}

	/* non-object base, no offending virtual property */
	rc = 1;
	goto done_rc;

 done_rc:
	duk_set_top(ctx, entry_top);
	return rc;

 fail_invalid_base_uncond:
	/* Note: unconditional throw */
	DUK_ASSERT(duk_get_top(ctx) == entry_top);
	DUK_ERROR(thr, DUK_ERR_TYPE_ERROR, DUK_STR_INVALID_BASE);
	return 0;

 fail_proxy_rejected:
	if (throw_flag) {
		DUK_ERROR(thr, DUK_ERR_TYPE_ERROR, DUK_STR_PROXY_REJECTED);
	}
	duk_set_top(ctx, entry_top);
	return 0;

 fail_not_configurable:
	if (throw_flag) {
		DUK_ERROR(thr, DUK_ERR_TYPE_ERROR, DUK_STR_NOT_CONFIGURABLE);
	}
	duk_set_top(ctx, entry_top);
	return 0;
}

/*
 *  Internal helper to define a property with specific flags, ignoring
 *  normal semantics such as extensibility, write protection etc.
 *  Overwrites any existing value and attributes unless caller requests
 *  that value only be updated if it doesn't already exists.
 *
 *  Does not support:
 *    - virtual properties (error if write attempted)
 *    - getter/setter properties (error if write attempted)
 *    - non-default (!= WEC) attributes for array entries (error if attempted)
 *    - array abandoning: if array part exists, it is always extended
 *    - array 'length' updating
 *
 *  Stack: [... in_val] -> []
 *
 *  Used for e.g. built-in initialization and environment record
 *  operations.
 */

DUK_INTERNAL void duk_hobject_define_property_internal(duk_hthread *thr, duk_hobject *obj, duk_hstring *key, duk_small_uint_t flags) {
	duk_context *ctx = (duk_context *) thr;
	duk_propdesc desc;
	duk_uint32_t arr_idx;
	duk_int_t e_idx;
	duk_tval tv_tmp;
	duk_tval *tv1 = NULL;
	duk_tval *tv2 = NULL;
	duk_small_uint_t propflags = flags & DUK_PROPDESC_FLAGS_MASK;  /* mask out flags not actually stored */

	DUK_DDD(DUK_DDDPRINT("define new property (internal): thr=%p, obj=%!O, key=%!O, flags=0x%02lx, val=%!T",
	                     (void *) thr, (duk_heaphdr *) obj, (duk_heaphdr *) key,
	                     (unsigned long) flags, (duk_tval *) duk_get_tval(ctx, -1)));

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(thr->heap != NULL);
	DUK_ASSERT(obj != NULL);
	DUK_ASSERT(key != NULL);

	DUK_ASSERT_VALSTACK_SPACE(thr, DUK__VALSTACK_SPACE);
	DUK_ASSERT(duk_is_valid_index(ctx, -1));  /* contains value */

	arr_idx = DUK_HSTRING_GET_ARRIDX_SLOW(key);

	if (duk__get_own_property_desc_raw(thr, obj, key, arr_idx, &desc, 0 /*flags*/)) {  /* don't push value */
		if (desc.e_idx >= 0) {
			if (flags & DUK_PROPDESC_FLAG_NO_OVERWRITE) {
				DUK_DDD(DUK_DDDPRINT("property already exists in the entry part -> skip as requested"));
				goto pop_exit;
			}
			DUK_DDD(DUK_DDDPRINT("property already exists in the entry part -> update value and attributes"));
			if (DUK_UNLIKELY(DUK_HOBJECT_E_SLOT_IS_ACCESSOR(thr->heap, obj, desc.e_idx))) {
				DUK_D(DUK_DPRINT("existing property is an accessor, not supported"));
				goto error_internal;
			}

			DUK_HOBJECT_E_SET_FLAGS(thr->heap, obj, desc.e_idx, propflags);
			tv1 = DUK_HOBJECT_E_GET_VALUE_TVAL_PTR(thr->heap, obj, desc.e_idx);
		} else if (desc.a_idx >= 0) {
			if (flags & DUK_PROPDESC_FLAG_NO_OVERWRITE) {
				DUK_DDD(DUK_DDDPRINT("property already exists in the array part -> skip as requested"));
				goto pop_exit;
			}
			DUK_DDD(DUK_DDDPRINT("property already exists in the array part -> update value (assert attributes)"));
			if (propflags != DUK_PROPDESC_FLAGS_WEC) {
				DUK_D(DUK_DPRINT("existing property in array part, but propflags not WEC (0x%02lx)",
				                 (unsigned long) propflags));
				goto error_internal;
			}

			tv1 = DUK_HOBJECT_A_GET_VALUE_PTR(thr->heap, obj, desc.a_idx);
		} else {
			if (flags & DUK_PROPDESC_FLAG_NO_OVERWRITE) {
				DUK_DDD(DUK_DDDPRINT("property already exists but is virtual -> skip as requested"));
				goto pop_exit;
			}
			DUK_DDD(DUK_DDDPRINT("property already exists but is virtual -> failure"));
			goto error_virtual;
		}

		goto write_value;
	}

	if (DUK_HOBJECT_HAS_ARRAY_PART(obj)) {
		if (arr_idx != DUK__NO_ARRAY_INDEX) {
			DUK_DDD(DUK_DDDPRINT("property does not exist, object has array part -> possibly extend array part and write value (assert attributes)"));
			DUK_ASSERT(propflags == DUK_PROPDESC_FLAGS_WEC);

			/* always grow the array, no sparse / abandon support here */
			if (arr_idx >= DUK_HOBJECT_GET_ASIZE(obj)) {
				duk__grow_props_for_array_item(thr, obj, arr_idx);
			}

			DUK_ASSERT(arr_idx < DUK_HOBJECT_GET_ASIZE(obj));
			tv1 = DUK_HOBJECT_A_GET_VALUE_PTR(thr->heap, obj, arr_idx);
			goto write_value;
		}
	}

	DUK_DDD(DUK_DDDPRINT("property does not exist, object belongs in entry part -> allocate new entry and write value and attributes"));
	e_idx = duk__alloc_entry_checked(thr, obj, key);  /* increases key refcount */
	DUK_ASSERT(e_idx >= 0);
	DUK_HOBJECT_E_SET_FLAGS(thr->heap, obj, e_idx, propflags);
	tv1 = DUK_HOBJECT_E_GET_VALUE_TVAL_PTR(thr->heap, obj, e_idx);
	/* new entry: previous value is garbage; set to undefined to share write_value */
	DUK_TVAL_SET_UNDEFINED_ACTUAL(tv1);
	goto write_value;

 write_value:
	/* tv1 points to value storage */

	tv2 = duk_require_tval(ctx, -1);  /* late lookup, avoid side effects */
	DUK_DDD(DUK_DDDPRINT("writing/updating value: %!T -> %!T",
	                     (duk_tval *) tv1, (duk_tval *) tv2));

	DUK_TVAL_SET_TVAL(&tv_tmp, tv1);
	DUK_TVAL_SET_TVAL(tv1, tv2);
	DUK_TVAL_INCREF(thr, tv1);
	DUK_TVAL_DECREF(thr, &tv_tmp);  /* side effects */
	goto pop_exit;

 pop_exit:
	duk_pop(ctx);  /* remove in_val */
	return;

 error_internal:
	DUK_ERROR(thr, DUK_ERR_INTERNAL_ERROR, DUK_STR_INTERNAL_ERROR);
	return;

 error_virtual:
	DUK_ERROR(thr, DUK_ERR_TYPE_ERROR, DUK_STR_REDEFINE_VIRT_PROP);
	return;
}

/*
 *  Fast path for defining array indexed values without interning the key.
 *  This is used by e.g. code for Array prototype and traceback creation so
 *  must avoid interning.
 */

DUK_INTERNAL void duk_hobject_define_property_internal_arridx(duk_hthread *thr, duk_hobject *obj, duk_uarridx_t arr_idx, duk_small_uint_t flags) {
	duk_context *ctx = (duk_context *) thr;
	duk_hstring *key;
	duk_tval *tv1, *tv2;
	duk_tval tv_tmp;

	DUK_DDD(DUK_DDDPRINT("define new property (internal) arr_idx fast path: thr=%p, obj=%!O, "
	                     "arr_idx=%ld, flags=0x%02lx, val=%!T",
	                     (void *) thr, obj, (long) arr_idx, (unsigned long) flags,
	                     (duk_tval *) duk_get_tval(ctx, -1)));

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(thr->heap != NULL);
	DUK_ASSERT(obj != NULL);

	if (DUK_HOBJECT_HAS_ARRAY_PART(obj) &&
	    arr_idx != DUK__NO_ARRAY_INDEX &&
	    flags == DUK_PROPDESC_FLAGS_WEC) {
		DUK_ASSERT((flags & DUK_PROPDESC_FLAG_NO_OVERWRITE) == 0);  /* covered by comparison */

		DUK_DDD(DUK_DDDPRINT("define property to array part (property may or may not exist yet)"));

		/* always grow the array, no sparse / abandon support here */
		if (arr_idx >= DUK_HOBJECT_GET_ASIZE(obj)) {
			duk__grow_props_for_array_item(thr, obj, arr_idx);
		}

		DUK_ASSERT(arr_idx < DUK_HOBJECT_GET_ASIZE(obj));
		tv1 = DUK_HOBJECT_A_GET_VALUE_PTR(thr->heap, obj, arr_idx);
		tv2 = duk_require_tval(ctx, -1);

		DUK_TVAL_SET_TVAL(&tv_tmp, tv1);
		DUK_TVAL_SET_TVAL(tv1, tv2);
		DUK_TVAL_INCREF(thr, tv1);
		DUK_TVAL_DECREF(thr, &tv_tmp);  /* side effects */

		duk_pop(ctx);  /* [ ...val ] -> [ ... ] */
		return;
	}

	DUK_DDD(DUK_DDDPRINT("define property fast path didn't work, use slow path"));

	duk_push_uint(ctx, (duk_uint_t) arr_idx);
	key = duk_to_hstring(ctx, -1);
	DUK_ASSERT(key != NULL);
	duk_insert(ctx, -2);  /* [ ... val key ] -> [ ... key val ] */

	duk_hobject_define_property_internal(thr, obj, key, flags);

	duk_pop(ctx);  /* [ ... key ] -> [ ... ] */
}

/*
 *  Internal helper for defining an accessor property, ignoring
 *  normal semantics such as extensibility, write protection etc.
 *  Overwrites any existing value and attributes.  This is called
 *  very rarely, so the implementation first sets a value to undefined
 *  and then changes the entry to an accessor (this is to save code space).
 */

DUK_INTERNAL void duk_hobject_define_accessor_internal(duk_hthread *thr, duk_hobject *obj, duk_hstring *key, duk_hobject *getter, duk_hobject *setter, duk_small_uint_t propflags) {
	duk_context *ctx = (duk_context *) thr;
	duk_int_t e_idx;
	duk_int_t h_idx;

	DUK_DDD(DUK_DDDPRINT("define new accessor (internal): thr=%p, obj=%!O, key=%!O, "
	                     "getter=%!O, setter=%!O, flags=0x%02lx",
	                     (void *) thr, (duk_heaphdr *) obj, (duk_heaphdr *) key,
	                     (duk_heaphdr *) getter, (duk_heaphdr *) setter,
	                     (unsigned long) propflags));

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(thr->heap != NULL);
	DUK_ASSERT(obj != NULL);
	DUK_ASSERT(key != NULL);
	DUK_ASSERT((propflags & ~DUK_PROPDESC_FLAGS_MASK) == 0);
	/* setter and/or getter may be NULL */

	DUK_ASSERT_VALSTACK_SPACE(thr, DUK__VALSTACK_SPACE);

	/* force the property to 'undefined' to create a slot for it */
	duk_push_undefined(ctx);
	duk_hobject_define_property_internal(thr, obj, key, propflags);
	duk_hobject_find_existing_entry(thr->heap, obj, key, &e_idx, &h_idx);
	DUK_DDD(DUK_DDDPRINT("accessor slot: e_idx=%ld, h_idx=%ld", (long) e_idx, (long) h_idx));
	DUK_ASSERT(e_idx >= 0);
	DUK_ASSERT((duk_uint32_t) e_idx < DUK_HOBJECT_GET_ENEXT(obj));

	/* no need to decref, as previous value is 'undefined' */
	DUK_HOBJECT_E_SLOT_SET_ACCESSOR(thr->heap, obj, e_idx);
	DUK_HOBJECT_E_SET_VALUE_GETTER(thr->heap, obj, e_idx, getter);
	DUK_HOBJECT_E_SET_VALUE_SETTER(thr->heap, obj, e_idx, setter);
	DUK_HOBJECT_INCREF(thr, getter);
	DUK_HOBJECT_INCREF(thr, setter);
}

/*
 *  Internal helpers for managing object 'length'
 */

/* XXX: awkward helpers */

DUK_INTERNAL void duk_hobject_set_length(duk_hthread *thr, duk_hobject *obj, duk_uint32_t length) {
	duk_context *ctx = (duk_context *) thr;
	duk_push_hobject(ctx, obj);
	duk_push_hstring_stridx(ctx, DUK_STRIDX_LENGTH);
	duk_push_u32(ctx, length);
	(void) duk_hobject_putprop(thr, duk_get_tval(ctx, -3), duk_get_tval(ctx, -2), duk_get_tval(ctx, -1), 0);
	duk_pop_n(ctx, 3);
}

DUK_INTERNAL void duk_hobject_set_length_zero(duk_hthread *thr, duk_hobject *obj) {
	duk_hobject_set_length(thr, obj, 0);
}

DUK_INTERNAL duk_uint32_t duk_hobject_get_length(duk_hthread *thr, duk_hobject *obj) {
	duk_context *ctx = (duk_context *) thr;
	duk_double_t val;
	duk_push_hobject(ctx, obj);
	duk_push_hstring_stridx(ctx, DUK_STRIDX_LENGTH);
	(void) duk_hobject_getprop(thr, duk_get_tval(ctx, -2), duk_get_tval(ctx, -1));
	val = duk_to_number(ctx, -1);
	duk_pop_n(ctx, 3);
	if (val >= 0.0 && val < DUK_DOUBLE_2TO32) {
		return (duk_uint32_t) val;
	}
	return 0;
}

/*
 *  Object.getOwnPropertyDescriptor()  (E5 Sections 15.2.3.3, 8.10.4)
 *
 *  This is an actual function call.
 */

DUK_INTERNAL duk_ret_t duk_hobject_object_get_own_property_descriptor(duk_context *ctx) {
	duk_hthread *thr = (duk_hthread *) ctx;
	duk_hobject *obj;
	duk_hstring *key;
	duk_propdesc pd;
	duk_bool_t rc;

	DUK_ASSERT(ctx != NULL);
	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(thr->heap != NULL);

	obj = duk_require_hobject_or_lfunc_coerce(ctx, 0);
	(void) duk_to_string(ctx, 1);
	key = duk_require_hstring(ctx, 1);

	DUK_ASSERT(obj != NULL);
	DUK_ASSERT(key != NULL);

	DUK_ASSERT_VALSTACK_SPACE(thr, DUK__VALSTACK_SPACE);

	rc = duk__get_own_property_desc(thr, obj, key, &pd, DUK__DESC_FLAG_PUSH_VALUE);
	if (!rc) {
		duk_push_undefined(ctx);

		/* [obj key undefined] */
		return 1;
	}

	duk_push_object(ctx);

	/* [obj key value desc] */

	if (DUK_PROPDESC_IS_ACCESSOR(&pd)) {
		/* If a setter/getter is missing (undefined), the descriptor must
		 * still have the property present with the value 'undefined'.
		 */
		if (pd.get) {
			duk_push_hobject(ctx, pd.get);
		} else {
			duk_push_undefined(ctx);
		}
		duk_put_prop_stridx(ctx, -2, DUK_STRIDX_GET);
		if (pd.set) {
			duk_push_hobject(ctx, pd.set);
		} else {
			duk_push_undefined(ctx);
		}
		duk_put_prop_stridx(ctx, -2, DUK_STRIDX_SET);
	} else {
		duk_dup(ctx, -2);  /* [obj key value desc value] */
		duk_put_prop_stridx(ctx, -2, DUK_STRIDX_VALUE);
		duk_push_boolean(ctx, DUK_PROPDESC_IS_WRITABLE(&pd));
		duk_put_prop_stridx(ctx, -2, DUK_STRIDX_WRITABLE);

		/* [obj key value desc] */
	}
	duk_push_boolean(ctx, DUK_PROPDESC_IS_ENUMERABLE(&pd));
	duk_put_prop_stridx(ctx, -2, DUK_STRIDX_ENUMERABLE);
	duk_push_boolean(ctx, DUK_PROPDESC_IS_CONFIGURABLE(&pd));
	duk_put_prop_stridx(ctx, -2, DUK_STRIDX_CONFIGURABLE);

	/* [obj key value desc] */
	return 1;
}

/*
 *  NormalizePropertyDescriptor() related helper.
 *
 *  Internal helper which validates and normalizes a property descriptor
 *  represented as an Ecmascript object (e.g. argument to defineProperty()).
 *  The output of this conversion is a set of defprop_flags and possibly
 *  some values pushed on the value stack; some subset of: property value,
 *  getter, setter.  Caller must manage stack top carefully because the
 *  number of values pushed depends on the input property descriptor.
 *
 *  The original descriptor object must not be altered in the process.
 */

/* XXX: very basic optimization -> duk_get_prop_stridx_top */

DUK_INTERNAL
void duk_hobject_prepare_property_descriptor(duk_context *ctx,
                                             duk_idx_t idx_in,
                                             duk_uint_t *out_defprop_flags,
                                             duk_idx_t *out_idx_value,
                                             duk_hobject **out_getter,
                                             duk_hobject **out_setter) {
	duk_hthread *thr = (duk_hthread *) ctx;
	duk_idx_t idx_value = -1;
	duk_hobject *getter = NULL;
	duk_hobject *setter = NULL;
	duk_bool_t is_data_desc = 0;
	duk_bool_t is_acc_desc = 0;
	duk_uint_t defprop_flags = 0;

	DUK_ASSERT(ctx != NULL);
	DUK_ASSERT(out_defprop_flags != NULL);
	DUK_ASSERT(out_idx_value != NULL);
	DUK_ASSERT(out_getter != NULL);
	DUK_ASSERT(out_setter != NULL);

	/* Must be an object, otherwise TypeError (E5.1 Section 8.10.5, step 1). */
	idx_in = duk_require_normalize_index(ctx, idx_in);
	(void) duk_require_hobject(ctx, idx_in);

	/* The coercion order must match the ToPropertyDescriptor() algorithm
	 * so that side effects in coercion happen in the correct order.
	 * (This order also happens to be compatible with duk_def_prop(),
	 * although it doesn't matter in practice.)
	 */

	if (duk_get_prop_stridx(ctx, idx_in, DUK_STRIDX_VALUE)) {
		is_data_desc = 1;
		defprop_flags |= DUK_DEFPROP_HAVE_VALUE;
		idx_value = duk_get_top_index(ctx);
		/* Leave 'value' on stack */
	} else {
		duk_pop(ctx);
	}

	if (duk_get_prop_stridx(ctx, idx_in, DUK_STRIDX_WRITABLE)) {
		is_data_desc = 1;
		if (duk_to_boolean(ctx, -1)) {
			defprop_flags |= DUK_DEFPROP_HAVE_WRITABLE | DUK_DEFPROP_WRITABLE;
		} else {
			defprop_flags |= DUK_DEFPROP_HAVE_WRITABLE;
		}
	}
	duk_pop(ctx);

	if (duk_get_prop_stridx(ctx, idx_in, DUK_STRIDX_GET)) {
		duk_tval *tv = duk_require_tval(ctx, -1);
		duk_hobject *h_get;

		if (DUK_TVAL_IS_UNDEFINED(tv)) {
			/* undefined is accepted */
			DUK_ASSERT(getter == NULL);
		} else {
			/* NOTE: lightfuncs are coerced to full functions because
			 * lightfuncs don't fit into a property value slot.  This
			 * has some side effects, see test-dev-lightfunc-accessor.js.
			 */
			h_get = duk_get_hobject_or_lfunc_coerce(ctx, -1);
			if (h_get == NULL || !DUK_HOBJECT_IS_CALLABLE(h_get)) {
				goto type_error;
			}
			getter = h_get;
		}
		is_acc_desc = 1;
		defprop_flags |= DUK_DEFPROP_HAVE_GETTER;
		/* Leave 'getter' on stack */
	} else {
		duk_pop(ctx);
	}

	if (duk_get_prop_stridx(ctx, idx_in, DUK_STRIDX_SET)) {
		duk_tval *tv = duk_require_tval(ctx, -1);
		duk_hobject *h_set;

		is_acc_desc = 1;
		if (DUK_TVAL_IS_UNDEFINED(tv)) {
			/* undefined is accepted */
			DUK_ASSERT(setter == NULL);
		}  else {
			/* NOTE: lightfuncs are coerced to full functions because
			 * lightfuncs don't fit into a property value slot.  This
			 * has some side effects, see test-dev-lightfunc-accessor.js.
			 */
			h_set = duk_get_hobject_or_lfunc_coerce(ctx, -1);
			if (h_set == NULL || !DUK_HOBJECT_IS_CALLABLE(h_set)) {
				goto type_error;
			}
			setter = h_set;
		}
		is_acc_desc = 1;
		defprop_flags |= DUK_DEFPROP_HAVE_SETTER;
		/* Leave 'setter' on stack */
	} else {
		duk_pop(ctx);
	}

	if (duk_get_prop_stridx(ctx, idx_in, DUK_STRIDX_ENUMERABLE)) {
		if (duk_to_boolean(ctx, -1)) {
			defprop_flags |= DUK_DEFPROP_HAVE_ENUMERABLE | DUK_DEFPROP_ENUMERABLE;
		} else {
			defprop_flags |= DUK_DEFPROP_HAVE_ENUMERABLE;
		}
	}
	duk_pop(ctx);

	if (duk_get_prop_stridx(ctx, idx_in, DUK_STRIDX_CONFIGURABLE)) {
		if (duk_to_boolean(ctx, -1)) {
			defprop_flags |= DUK_DEFPROP_HAVE_CONFIGURABLE | DUK_DEFPROP_CONFIGURABLE;
		} else {
			defprop_flags |= DUK_DEFPROP_HAVE_CONFIGURABLE;
		}
	}
	duk_pop(ctx);

	if (is_data_desc && is_acc_desc) {
		goto type_error;
	}

	*out_defprop_flags = defprop_flags;
	*out_idx_value = idx_value;
	*out_getter = getter;
	*out_setter = setter;

	/* [ ... value? getter? setter? ] */
	return;

 type_error:
	DUK_ERROR(thr, DUK_ERR_TYPE_ERROR, DUK_STR_INVALID_DESCRIPTOR);
}

/*
 *  Object.defineProperty() related helper  (E5 Section 15.2.3.6)
 *
 *  Inlines all [[DefineOwnProperty]] exotic behaviors.
 *
 *  Note: Ecmascript compliant [[DefineOwnProperty]](P, Desc, Throw) is not
 *  implemented directly, but Object.defineProperty() serves its purpose.
 *  We don't need the [[DefineOwnProperty]] internally and we don't have a
 *  property descriptor with 'missing values' so it's easier to avoid it
 *  entirely.
 *
 *  Note: this is only called for actual objects, not primitive values.
 *  This must support virtual properties for full objects (e.g. Strings)
 *  but not for plain values (e.g. strings).  Lightfuncs, even though
 *  primitive in a sense, are treated like objects and accepted as target
 *  values.
 */

/* XXX: this is a major target for size optimization */
DUK_INTERNAL
void duk_hobject_define_property_helper(duk_context *ctx,
                                        duk_uint_t defprop_flags,
                                        duk_hobject *obj,
                                        duk_hstring *key,
                                        duk_idx_t idx_value,
                                        duk_hobject *get,
                                        duk_hobject *set) {
	duk_hthread *thr = (duk_hthread *) ctx;
	duk_uint32_t arr_idx;
	duk_tval tv;
	duk_bool_t has_enumerable;
	duk_bool_t has_configurable;
	duk_bool_t has_writable;
	duk_bool_t has_value;
	duk_bool_t has_get;
	duk_bool_t has_set;
	duk_bool_t is_enumerable;
	duk_bool_t is_configurable;
	duk_bool_t is_writable;
	duk_bool_t throw_flag;
	duk_bool_t force_flag;
	duk_small_uint_t new_flags;
	duk_propdesc curr;
	duk_uint32_t arridx_new_array_length;  /* != 0 => post-update for array 'length' (used when key is an array index) */
	duk_uint32_t arrlen_old_len;
	duk_uint32_t arrlen_new_len;
	duk_bool_t pending_write_protect;

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(thr->heap != NULL);
	DUK_ASSERT(ctx != NULL);
	DUK_ASSERT(obj != NULL);
	DUK_ASSERT(key != NULL);
	/* idx_value may be < 0 (no value), set and get may be NULL */

	DUK_ASSERT_VALSTACK_SPACE(thr, DUK__VALSTACK_SPACE);

	/* All the flags fit in 16 bits, so will fit into duk_bool_t. */

	has_writable = (defprop_flags & DUK_DEFPROP_HAVE_WRITABLE);
	has_enumerable = (defprop_flags & DUK_DEFPROP_HAVE_ENUMERABLE);
	has_configurable = (defprop_flags & DUK_DEFPROP_HAVE_CONFIGURABLE);
	has_value = (defprop_flags & DUK_DEFPROP_HAVE_VALUE);
	has_get = (defprop_flags & DUK_DEFPROP_HAVE_GETTER);
	has_set = (defprop_flags & DUK_DEFPROP_HAVE_SETTER);
	is_writable = (defprop_flags & DUK_DEFPROP_WRITABLE);
	is_enumerable = (defprop_flags & DUK_DEFPROP_ENUMERABLE);
	is_configurable = (defprop_flags & DUK_DEFPROP_CONFIGURABLE);
	throw_flag = 1;   /* Object.defineProperty() calls [[DefineOwnProperty]] with Throw=true */
	force_flag = (defprop_flags & DUK_DEFPROP_FORCE);

	arr_idx = DUK_HSTRING_GET_ARRIDX_SLOW(key);

	arridx_new_array_length = 0;
	pending_write_protect = 0;
	arrlen_old_len = 0;
	arrlen_new_len = 0;

	DUK_DDD(DUK_DDDPRINT("has_enumerable=%ld is_enumerable=%ld "
	                     "has_configurable=%ld is_configurable=%ld "
	                     "has_writable=%ld is_writable=%ld "
	                     "has_value=%ld value=%!T "
	                     "has_get=%ld get=%p=%!O "
	                     "has_set=%ld set=%p=%!O "
	                     "arr_idx=%ld",
	                     (long) has_enumerable, (long) is_enumerable,
	                     (long) has_configurable, (long) is_configurable,
	                     (long) has_writable, (long) is_writable,
	                     (long) has_value, (duk_tval *) (idx_value >= 0 ? duk_get_tval(ctx, idx_value) : NULL),
	                     (long) has_get, (void *) get, (duk_heaphdr *) get,
	                     (long) has_set, (void *) set, (duk_heaphdr *) set,
	                     (long) arr_idx));

	/*
	 *  Array exotic behaviors can be implemented at this point.  The local variables
	 *  are essentially a 'value copy' of the input descriptor (Desc), which is modified
	 *  by the Array [[DefineOwnProperty]] (E5 Section 15.4.5.1).
	 */

	if (!DUK_HOBJECT_HAS_EXOTIC_ARRAY(obj)) {
		goto skip_array_exotic;
	}

	if (key == DUK_HTHREAD_STRING_LENGTH(thr)) {
		/* E5 Section 15.4.5.1, step 3, steps a - i are implemented here, j - n at the end */
		if (!has_value) {
			DUK_DDD(DUK_DDDPRINT("exotic array behavior for 'length', but no value in descriptor -> normal behavior"));
			goto skip_array_exotic;
		}

		DUK_DDD(DUK_DDDPRINT("exotic array behavior for 'length', value present in descriptor -> exotic behavior"));

		/*
		 *  Get old and new length
		 */

		/* Note: reuse 'curr' as a temp propdesc */
		arrlen_old_len = duk__get_old_array_length(thr, obj, &curr);

		duk_dup(ctx, idx_value);
		arrlen_new_len = duk__to_new_array_length_checked(thr);
		duk_replace(ctx, idx_value);  /* step 3.e: replace 'Desc.[[Value]]' */

		DUK_DDD(DUK_DDDPRINT("old_len=%ld, new_len=%ld", (long) arrlen_old_len, (long) arrlen_new_len));

		if (arrlen_new_len >= arrlen_old_len) {
			/* standard behavior, step 3.f.i */
			DUK_DDD(DUK_DDDPRINT("new length is same or higher as previous => standard behavior"));
			goto skip_array_exotic;
		}
		DUK_DDD(DUK_DDDPRINT("new length is smaller than previous => exotic post behavior"));

		/* XXX: consolidated algorithm step 15.f -> redundant? */
		if (!(curr.flags & DUK_PROPDESC_FLAG_WRITABLE) && !force_flag) {
			/* Note: 'curr' refers to 'length' propdesc */
			goto fail_not_writable_array_length;
		}

		/* steps 3.h and 3.i */
		if (has_writable && !is_writable) {
			DUK_DDD(DUK_DDDPRINT("desc writable is false, force it back to true, and flag pending write protect"));
			is_writable = 1;
			pending_write_protect = 1;
		}

		/* remaining actual steps are carried out if standard DefineOwnProperty succeeds */
	} else if (arr_idx != DUK__NO_ARRAY_INDEX) {
		/* XXX: any chance of unifying this with the 'length' key handling? */

		/* E5 Section 15.4.5.1, step 4 */
		duk_uint32_t old_len;

		/* Note: use 'curr' as a temp propdesc */
		old_len = duk__get_old_array_length(thr, obj, &curr);

		if (arr_idx >= old_len) {
			DUK_DDD(DUK_DDDPRINT("defineProperty requires array length update "
			                     "(arr_idx=%ld, old_len=%ld)",
			                     (long) arr_idx, (long) old_len));

			if (!(curr.flags & DUK_PROPDESC_FLAG_WRITABLE)) {
				/* Note: 'curr' refers to 'length' propdesc */
				goto fail_not_writable_array_length;
			}

			/* actual update happens once write has been completed without
			 * error below.
			 */
			DUK_ASSERT(arr_idx != 0xffffffffUL);
			arridx_new_array_length = arr_idx + 1;
		} else {
			DUK_DDD(DUK_DDDPRINT("defineProperty does not require length update "
			                     "(arr_idx=%ld, old_len=%ld) -> standard behavior",
			                     (long) arr_idx, (long) old_len));
		}
	}
 skip_array_exotic:

	/* XXX: There is currently no support for writing buffer object
	 * indexed elements here.  Attempt to do so will succeed and
	 * write a concrete property into the buffer object.  This should
	 * be fixed at some point but because buffers are a custom feature
	 * anyway, this is relatively unimportant.
	 */

	/*
	 *  Actual Object.defineProperty() default algorithm.
	 */

	/*
	 *  First check whether property exists; if not, simple case.  This covers
	 *  steps 1-4.
	 */

	if (!duk__get_own_property_desc_raw(thr, obj, key, arr_idx, &curr, DUK__DESC_FLAG_PUSH_VALUE)) {
		DUK_DDD(DUK_DDDPRINT("property does not exist"));

		if (!DUK_HOBJECT_HAS_EXTENSIBLE(obj) && !force_flag) {
			goto fail_not_extensible;
		}

		/* XXX: share final setting code for value and flags?  difficult because
		 * refcount code is different.  Share entry allocation?  But can't allocate
		 * until array index checked.
		 */

		/* steps 4.a and 4.b are tricky */
		if (has_set || has_get) {
			duk_int_t e_idx;

			DUK_DDD(DUK_DDDPRINT("create new accessor property"));

			DUK_ASSERT(has_set || set == NULL);
			DUK_ASSERT(has_get || get == NULL);
			DUK_ASSERT(!has_value);
			DUK_ASSERT(!has_writable);

			new_flags = DUK_PROPDESC_FLAG_ACCESSOR;  /* defaults, E5 Section 8.6.1, Table 7 */
			if (has_enumerable && is_enumerable) {
				new_flags |= DUK_PROPDESC_FLAG_ENUMERABLE;
			}
			if (has_configurable && is_configurable) {
				new_flags |= DUK_PROPDESC_FLAG_CONFIGURABLE;
			}

			if (arr_idx != DUK__NO_ARRAY_INDEX && DUK_HOBJECT_HAS_ARRAY_PART(obj)) {
				DUK_DDD(DUK_DDDPRINT("accessor cannot go to array part, abandon array"));
				duk__abandon_array_checked(thr, obj);
			}

			/* write to entry part */
			e_idx = duk__alloc_entry_checked(thr, obj, key);
			DUK_ASSERT(e_idx >= 0);

			DUK_HOBJECT_E_SET_VALUE_GETTER(thr->heap, obj, e_idx, get);
			DUK_HOBJECT_E_SET_VALUE_SETTER(thr->heap, obj, e_idx, set);
			DUK_HOBJECT_INCREF(thr, get);
			DUK_HOBJECT_INCREF(thr, set);

			DUK_HOBJECT_E_SET_FLAGS(thr->heap, obj, e_idx, new_flags);
			goto success_exotics;
		} else {
			duk_int_t e_idx;
			duk_tval *tv2;

			DUK_DDD(DUK_DDDPRINT("create new data property"));

			DUK_ASSERT(!has_set);
			DUK_ASSERT(!has_get);

			new_flags = 0;  /* defaults, E5 Section 8.6.1, Table 7 */
			if (has_writable && is_writable) {
				new_flags |= DUK_PROPDESC_FLAG_WRITABLE;
			}
			if (has_enumerable && is_enumerable) {
				new_flags |= DUK_PROPDESC_FLAG_ENUMERABLE;
			}
			if (has_configurable && is_configurable) {
				new_flags |= DUK_PROPDESC_FLAG_CONFIGURABLE;
			}
			if (has_value) {
				duk_tval *tv_tmp = duk_require_tval(ctx, idx_value);
				DUK_TVAL_SET_TVAL(&tv, tv_tmp);
			} else {
				DUK_TVAL_SET_UNDEFINED_ACTUAL(&tv);  /* default value */
			}

			if (arr_idx != DUK__NO_ARRAY_INDEX && DUK_HOBJECT_HAS_ARRAY_PART(obj)) {
				if (new_flags == DUK_PROPDESC_FLAGS_WEC) {
#if 0
					DUK_DDD(DUK_DDDPRINT("new data property attributes match array defaults, attempt to write to array part"));
					/* may become sparse...*/
#endif
					/* XXX: handling for array part missing now; this doesn't affect
					 * compliance but causes array entry writes using defineProperty()
					 * to always abandon array part.
					 */
				}
				DUK_DDD(DUK_DDDPRINT("new data property cannot go to array part, abandon array"));
				duk__abandon_array_checked(thr, obj);
				/* fall through */
			}

			/* write to entry part */
			e_idx = duk__alloc_entry_checked(thr, obj, key);
			DUK_ASSERT(e_idx >= 0);
			tv2 = DUK_HOBJECT_E_GET_VALUE_TVAL_PTR(thr->heap, obj, e_idx);
			DUK_TVAL_SET_TVAL(tv2, &tv);
			DUK_TVAL_INCREF(thr, tv2);

			DUK_HOBJECT_E_SET_FLAGS(thr->heap, obj, e_idx, new_flags);
			goto success_exotics;
		}
		DUK_UNREACHABLE();
	}

	/* we currently assume virtual properties are not configurable (as none of them are) */
	DUK_ASSERT((curr.e_idx >= 0 || curr.a_idx >= 0) || !(curr.flags & DUK_PROPDESC_FLAG_CONFIGURABLE));

	/* [obj key desc value get set curr_value] */

	/*
	 *  Property already exists.  Steps 5-6 detect whether any changes need
	 *  to be made.
	 */

	if (has_enumerable) {
		if (is_enumerable) {
			if (!(curr.flags & DUK_PROPDESC_FLAG_ENUMERABLE)) {
				goto need_check;
			}
		} else {
			if (curr.flags & DUK_PROPDESC_FLAG_ENUMERABLE) {
				goto need_check;
			}
		}
	}
	if (has_configurable) {
		if (is_configurable) {
			if (!(curr.flags & DUK_PROPDESC_FLAG_CONFIGURABLE)) {
				goto need_check;
			}
		} else {
			if (curr.flags & DUK_PROPDESC_FLAG_CONFIGURABLE) {
				goto need_check;
			}
		}
	}
	if (has_value) {
		duk_tval *tmp1;
		duk_tval *tmp2;

		/* attempt to change from accessor to data property */
		if (curr.flags & DUK_PROPDESC_FLAG_ACCESSOR) {
			goto need_check;
		}

		tmp1 = duk_require_tval(ctx, -1);         /* curr value */
		tmp2 = duk_require_tval(ctx, idx_value);  /* new value */
		if (!duk_js_samevalue(tmp1, tmp2)) {
			goto need_check;
		}
	}
	if (has_writable) {
		/* attempt to change from accessor to data property */
		if (curr.flags & DUK_PROPDESC_FLAG_ACCESSOR) {
			goto need_check;
		}

		if (is_writable) {
			if (!(curr.flags & DUK_PROPDESC_FLAG_WRITABLE)) {
				goto need_check;
			}
		} else {
			if (curr.flags & DUK_PROPDESC_FLAG_WRITABLE) {
				goto need_check;
			}
		}
	}
	if (has_set) {
		if (curr.flags & DUK_PROPDESC_FLAG_ACCESSOR) {
			if (set != curr.set) {
				goto need_check;
			}
		} else {
			goto need_check;
		}
	}
	if (has_get) {
		if (curr.flags & DUK_PROPDESC_FLAG_ACCESSOR) {
			if (get != curr.get) {
				goto need_check;
			}
		} else {
			goto need_check;
		}
	}

	/* property exists, either 'desc' is empty, or all values
	 * match (SameValue)
	 */
	goto success_no_exotics;

 need_check:

	/*
	 *  Some change(s) need to be made.  Steps 7-11.
	 */

	/* shared checks for all descriptor types */
	if (!(curr.flags & DUK_PROPDESC_FLAG_CONFIGURABLE) && !force_flag) {
		if (has_configurable && is_configurable) {
			goto fail_not_configurable;
		}
		if (has_enumerable) {
			if (curr.flags & DUK_PROPDESC_FLAG_ENUMERABLE) {
				if (!is_enumerable) {
					goto fail_not_configurable;
				}
			} else {
				if (is_enumerable) {
					goto fail_not_configurable;
				}
			}
		}
	}

	/* Reject attempt to change virtual properties: not part of the
	 * standard algorithm, applies currently to e.g. virtual index
	 * properties of buffer objects (which are virtual but writable).
	 * (Cannot "force" modification of a virtual property.)
	 */
	if (curr.flags & DUK_PROPDESC_FLAG_VIRTUAL) {
		goto fail_virtual;
	}

	/* descriptor type specific checks */
	if (has_set || has_get) {
		/* IsAccessorDescriptor(desc) == true */
		DUK_ASSERT(!has_writable);
		DUK_ASSERT(!has_value);

		if (curr.flags & DUK_PROPDESC_FLAG_ACCESSOR) {
			/* curr and desc are accessors */
			if (!(curr.flags & DUK_PROPDESC_FLAG_CONFIGURABLE) && !force_flag) {
				if (has_set && set != curr.set) {
					goto fail_not_configurable;
				}
				if (has_get && get != curr.get) {
					goto fail_not_configurable;
				}
			}
		} else {
			duk_bool_t rc;
			duk_tval tv_tmp;
			duk_tval *tv1;

			/* curr is data, desc is accessor */
			if (!(curr.flags & DUK_PROPDESC_FLAG_CONFIGURABLE) && !force_flag) {
				goto fail_not_configurable;
			}

			DUK_DDD(DUK_DDDPRINT("convert property to accessor property"));
			if (curr.a_idx >= 0) {
				DUK_DDD(DUK_DDDPRINT("property to convert is stored in an array entry, abandon array and re-lookup"));
				duk__abandon_array_checked(thr, obj);
				duk_pop(ctx);  /* remove old value */
				rc = duk__get_own_property_desc_raw(thr, obj, key, arr_idx, &curr, DUK__DESC_FLAG_PUSH_VALUE);
				DUK_UNREF(rc);
				DUK_ASSERT(rc != 0);
				DUK_ASSERT(curr.e_idx >= 0 && curr.a_idx < 0);
			}

			DUK_ASSERT(!DUK_HOBJECT_E_SLOT_IS_ACCESSOR(thr->heap, obj, curr.e_idx));

			tv1 = DUK_HOBJECT_E_GET_VALUE_TVAL_PTR(thr->heap, obj, curr.e_idx);
			DUK_TVAL_SET_TVAL(&tv_tmp, tv1);
			DUK_TVAL_SET_UNDEFINED_UNUSED(tv1);
			DUK_TVAL_DECREF(thr, &tv_tmp);

			DUK_HOBJECT_E_SET_VALUE_GETTER(thr->heap, obj, curr.e_idx, NULL);
			DUK_HOBJECT_E_SET_VALUE_SETTER(thr->heap, obj, curr.e_idx, NULL);
			DUK_HOBJECT_E_SLOT_CLEAR_WRITABLE(thr->heap, obj, curr.e_idx);
			DUK_HOBJECT_E_SLOT_SET_ACCESSOR(thr->heap, obj, curr.e_idx);

			DUK_DDD(DUK_DDDPRINT("flags after data->accessor conversion: 0x%02lx",
			                     (unsigned long) DUK_HOBJECT_E_GET_FLAGS(thr->heap, obj, curr.e_idx)));

			/* re-lookup to update curr.flags
			 * XXX: would be faster to update directly
			 */
			duk_pop(ctx);  /* remove old value */
			rc = duk__get_own_property_desc_raw(thr, obj, key, arr_idx, &curr, DUK__DESC_FLAG_PUSH_VALUE);
			DUK_UNREF(rc);
			DUK_ASSERT(rc != 0);
		}
	} else if (has_value || has_writable) {
		/* IsDataDescriptor(desc) == true */
		DUK_ASSERT(!has_set);
		DUK_ASSERT(!has_get);

		if (curr.flags & DUK_PROPDESC_FLAG_ACCESSOR) {
			duk_bool_t rc;
			duk_hobject *tmp;

			/* curr is accessor, desc is data */
			if (!(curr.flags & DUK_PROPDESC_FLAG_CONFIGURABLE) && !force_flag) {
				goto fail_not_configurable;
			}

			/* curr is accessor -> cannot be in array part */
			DUK_ASSERT(curr.e_idx >= 0 && curr.a_idx < 0);

			DUK_DDD(DUK_DDDPRINT("convert property to data property"));

			DUK_ASSERT(DUK_HOBJECT_E_SLOT_IS_ACCESSOR(thr->heap, obj, curr.e_idx));
			tmp = DUK_HOBJECT_E_GET_VALUE_GETTER(thr->heap, obj, curr.e_idx);
			DUK_UNREF(tmp);
			DUK_HOBJECT_E_SET_VALUE_GETTER(thr->heap, obj, curr.e_idx, NULL);
			DUK_HOBJECT_DECREF(thr, tmp);
			tmp = DUK_HOBJECT_E_GET_VALUE_SETTER(thr->heap, obj, curr.e_idx);
			DUK_UNREF(tmp);
			DUK_HOBJECT_E_SET_VALUE_SETTER(thr->heap, obj, curr.e_idx, NULL);
			DUK_HOBJECT_DECREF(thr, tmp);

			DUK_TVAL_SET_UNDEFINED_ACTUAL(DUK_HOBJECT_E_GET_VALUE_TVAL_PTR(thr->heap, obj, curr.e_idx));
			DUK_HOBJECT_E_SLOT_CLEAR_WRITABLE(thr->heap, obj, curr.e_idx);
			DUK_HOBJECT_E_SLOT_CLEAR_ACCESSOR(thr->heap, obj, curr.e_idx);

			DUK_DDD(DUK_DDDPRINT("flags after accessor->data conversion: 0x%02lx",
			                     (unsigned long) DUK_HOBJECT_E_GET_FLAGS(thr->heap, obj, curr.e_idx)));

			/* re-lookup to update curr.flags
			 * XXX: would be faster to update directly
			 */
			duk_pop(ctx);  /* remove old value */
			rc = duk__get_own_property_desc_raw(thr, obj, key, arr_idx, &curr, DUK__DESC_FLAG_PUSH_VALUE);
			DUK_UNREF(rc);
			DUK_ASSERT(rc != 0);
		} else {
			/* curr and desc are data */
			if (!(curr.flags & DUK_PROPDESC_FLAG_CONFIGURABLE) && !force_flag) {
				if (!(curr.flags & DUK_PROPDESC_FLAG_WRITABLE) && has_writable && is_writable) {
					goto fail_not_configurable;
				}
				/* Note: changing from writable to non-writable is OK */
				if (!(curr.flags & DUK_PROPDESC_FLAG_WRITABLE) && has_value) {
					duk_tval *tmp1 = duk_require_tval(ctx, -1);         /* curr value */
					duk_tval *tmp2 = duk_require_tval(ctx, idx_value);  /* new value */
					if (!duk_js_samevalue(tmp1, tmp2)) {
						goto fail_not_configurable;
					}
				}
			}
		}
	} else {
		/* IsGenericDescriptor(desc) == true; this means in practice that 'desc'
		 * only has [[Enumerable]] or [[Configurable]] flag updates, which are
		 * allowed at this point.
		 */

		DUK_ASSERT(!has_value && !has_writable && !has_get && !has_set);
	}

	/*
	 *  Start doing property attributes updates.  Steps 12-13.
	 *
	 *  Start by computing new attribute flags without writing yet.
	 *  Property type conversion is done above if necessary.
	 */

	new_flags = curr.flags;

	if (has_enumerable) {
		if (is_enumerable) {
			new_flags |= DUK_PROPDESC_FLAG_ENUMERABLE;
		} else {
			new_flags &= ~DUK_PROPDESC_FLAG_ENUMERABLE;
		}
	}
	if (has_configurable) {
		if (is_configurable) {
			new_flags |= DUK_PROPDESC_FLAG_CONFIGURABLE;
		} else {
			new_flags &= ~DUK_PROPDESC_FLAG_CONFIGURABLE;
		}
	}
	if (has_writable) {
		if (is_writable) {
			new_flags |= DUK_PROPDESC_FLAG_WRITABLE;
		} else {
			new_flags &= ~DUK_PROPDESC_FLAG_WRITABLE;
		}
	}

	/* XXX: write protect after flag? -> any chance of handling it here? */

	DUK_DDD(DUK_DDDPRINT("new flags that we want to write: 0x%02lx",
	                     (unsigned long) new_flags));

	/*
	 *  Check whether we need to abandon an array part (if it exists)
	 */

	if (curr.a_idx >= 0) {
		duk_bool_t rc;

		DUK_ASSERT(curr.e_idx < 0);

		if (new_flags == DUK_PROPDESC_FLAGS_WEC) {
			duk_tval *tv1, *tv2;
			duk_tval tv_tmp;

			DUK_DDD(DUK_DDDPRINT("array index, new property attributes match array defaults, update in-place"));

			DUK_ASSERT(curr.flags == DUK_PROPDESC_FLAGS_WEC);  /* must have been, since in array part */
			DUK_ASSERT(!has_set);
			DUK_ASSERT(!has_get);

			tv2 = duk_require_tval(ctx, idx_value);
			tv1 = DUK_HOBJECT_A_GET_VALUE_PTR(thr->heap, obj, curr.a_idx);
			DUK_TVAL_SET_TVAL(&tv_tmp, tv1);
			DUK_TVAL_SET_TVAL(tv1, tv2);
			DUK_TVAL_INCREF(thr, tv1);
			DUK_TVAL_DECREF(thr, &tv_tmp);
			goto success_exotics;
		}

		DUK_DDD(DUK_DDDPRINT("array index, new property attributes do not match array defaults, abandon array and re-lookup"));
		duk__abandon_array_checked(thr, obj);
		duk_pop(ctx);  /* remove old value */
		rc = duk__get_own_property_desc_raw(thr, obj, key, arr_idx, &curr, DUK__DESC_FLAG_PUSH_VALUE);
		DUK_UNREF(rc);
		DUK_ASSERT(rc != 0);
		DUK_ASSERT(curr.e_idx >= 0 && curr.a_idx < 0);
	}

	DUK_DDD(DUK_DDDPRINT("updating existing property in entry part"));

	/* array case is handled comprehensively above */
	DUK_ASSERT(curr.e_idx >= 0 && curr.a_idx < 0);

	DUK_DDD(DUK_DDDPRINT("update existing property attributes"));
	DUK_HOBJECT_E_SET_FLAGS(thr->heap, obj, curr.e_idx, new_flags);

	if (has_set) {
		duk_hobject *tmp;

		DUK_DDD(DUK_DDDPRINT("update existing property setter"));
		DUK_ASSERT(DUK_HOBJECT_E_SLOT_IS_ACCESSOR(thr->heap, obj, curr.e_idx));

		tmp = DUK_HOBJECT_E_GET_VALUE_SETTER(thr->heap, obj, curr.e_idx);
		DUK_UNREF(tmp);
		DUK_HOBJECT_E_SET_VALUE_SETTER(thr->heap, obj, curr.e_idx, set);
		DUK_HOBJECT_INCREF(thr, set);
		DUK_HOBJECT_DECREF(thr, tmp);
	}
	if (has_get) {
		duk_hobject *tmp;

		DUK_DDD(DUK_DDDPRINT("update existing property getter"));
		DUK_ASSERT(DUK_HOBJECT_E_SLOT_IS_ACCESSOR(thr->heap, obj, curr.e_idx));

		tmp = DUK_HOBJECT_E_GET_VALUE_GETTER(thr->heap, obj, curr.e_idx);
		DUK_UNREF(tmp);
		DUK_HOBJECT_E_SET_VALUE_GETTER(thr->heap, obj, curr.e_idx, get);
		DUK_HOBJECT_INCREF(thr, get);
		DUK_HOBJECT_DECREF(thr, tmp);
	}
	if (has_value) {
		duk_tval *tv1, *tv2;
		duk_tval tv_tmp;

		DUK_DDD(DUK_DDDPRINT("update existing property value"));
		DUK_ASSERT(!DUK_HOBJECT_E_SLOT_IS_ACCESSOR(thr->heap, obj, curr.e_idx));

		tv2 = duk_require_tval(ctx, idx_value);
		tv1 = DUK_HOBJECT_E_GET_VALUE_TVAL_PTR(thr->heap, obj, curr.e_idx);
		DUK_TVAL_SET_TVAL(&tv_tmp, tv1);
		DUK_TVAL_SET_TVAL(tv1, tv2);
		DUK_TVAL_INCREF(thr, tv1);
		DUK_TVAL_DECREF(thr, &tv_tmp);
	}

	/*
	 *  Standard algorithm succeeded without errors, check for exotic post-behaviors.
	 *
	 *  Arguments exotic behavior in E5 Section 10.6 occurs after the standard
	 *  [[DefineOwnProperty]] has completed successfully.
	 *
	 *  Array exotic behavior in E5 Section 15.4.5.1 is implemented partly
	 *  prior to the default [[DefineOwnProperty]], but:
	 *    - for an array index key (e.g. "10") the final 'length' update occurs here
	 *    - for 'length' key the element deletion and 'length' update occurs here
	 */

 success_exotics:

	/* [obj key desc value get set curr_value] */

	if (DUK_HOBJECT_HAS_EXOTIC_ARRAY(obj)) {
		if (arridx_new_array_length > 0) {
			duk_tval *tmp;
			duk_bool_t rc;

			/*
			 *  Note: zero works as a "no update" marker because the new length
			 *  can never be zero after a new property is written.
			 */

			/* E5 Section 15.4.5.1, steps 4.e.i - 4.e.ii */

			DUK_DDD(DUK_DDDPRINT("defineProperty successful, pending array length update to: %ld",
			                     (long) arridx_new_array_length));

			/* Note: reuse 'curr' */
			rc = duk__get_own_property_desc_raw(thr, obj, DUK_HTHREAD_STRING_LENGTH(thr), DUK__NO_ARRAY_INDEX, &curr, 0 /*flags*/);  /* don't push value */
			DUK_UNREF(rc);
			DUK_ASSERT(rc != 0);
			DUK_ASSERT(curr.e_idx >= 0);

			tmp = DUK_HOBJECT_E_GET_VALUE_TVAL_PTR(thr->heap, obj, curr.e_idx);
			DUK_ASSERT(DUK_TVAL_IS_NUMBER(tmp));
			DUK_TVAL_SET_NUMBER(tmp, (duk_double_t) arridx_new_array_length);  /* no need for decref/incref because value is a number */
		}
		if (key == DUK_HTHREAD_STRING_LENGTH(thr) && arrlen_new_len < arrlen_old_len) {
			/*
			 *  E5 Section 15.4.5.1, steps 3.k - 3.n.  The order at the end combines
			 *  the error case 3.l.iii and the success case 3.m-3.n.
			 *
			 *  Note: 'length' is always in entries part, so no array abandon issues for
			 *  'writable' update.
			 */

			/* XXX: investigate whether write protect can be handled above, if we
			 * just update length here while ignoring its protected status
			 */

			duk_tval *tmp;
			duk_uint32_t result_len;
			duk_bool_t rc;

			DUK_DDD(DUK_DDDPRINT("defineProperty successful, key is 'length', exotic array behavior, "
			                     "doing array element deletion and length update"));

			rc = duk__handle_put_array_length_smaller(thr, obj, arrlen_old_len, arrlen_new_len, force_flag, &result_len);

			/* update length (curr points to length, and we assume it's still valid) */
			DUK_ASSERT(result_len >= arrlen_new_len && result_len <= arrlen_old_len);

			DUK_ASSERT(curr.e_idx >= 0);
			DUK_ASSERT(!DUK_HOBJECT_E_SLOT_IS_ACCESSOR(thr->heap, obj, curr.e_idx));
			tmp = DUK_HOBJECT_E_GET_VALUE_TVAL_PTR(thr->heap, obj, curr.e_idx);
			DUK_ASSERT(DUK_TVAL_IS_NUMBER(tmp));
			DUK_TVAL_SET_NUMBER(tmp, (duk_double_t) result_len);  /* no decref needed for a number */
			DUK_ASSERT(DUK_TVAL_IS_NUMBER(tmp));

			if (pending_write_protect) {
				DUK_DDD(DUK_DDDPRINT("setting array length non-writable (pending writability update)"));
				DUK_HOBJECT_E_SLOT_CLEAR_WRITABLE(thr->heap, obj, curr.e_idx);
			}

			/*
			 *  XXX: shrink array allocation or entries compaction here?
			 */

			if (!rc) {
				goto fail_array_length_partial;
			}
		}
	} else if (arr_idx != DUK__NO_ARRAY_INDEX && DUK_HOBJECT_HAS_EXOTIC_ARGUMENTS(obj)) {
		duk_hobject *map;
		duk_hobject *varenv;

		DUK_ASSERT(arridx_new_array_length == 0);
		DUK_ASSERT(!DUK_HOBJECT_HAS_EXOTIC_ARRAY(obj));  /* traits are separate; in particular, arguments not an array */

		map = NULL;
		varenv = NULL;
		if (!duk__lookup_arguments_map(thr, obj, key, &curr, &map, &varenv)) {
			goto success_no_exotics;
		}
		DUK_ASSERT(map != NULL);
		DUK_ASSERT(varenv != NULL);

		/* [obj key desc value get set curr_value varname] */

		if (has_set || has_get) {
			/* = IsAccessorDescriptor(Desc) */
			DUK_DDD(DUK_DDDPRINT("defineProperty successful, key mapped to arguments 'map' "
			                     "changed to an accessor, delete arguments binding"));

			(void) duk_hobject_delprop_raw(thr, map, key, 0);  /* ignore result */
		} else {
			/* Note: this order matters (final value before deleting map entry must be done) */
			DUK_DDD(DUK_DDDPRINT("defineProperty successful, key mapped to arguments 'map', "
			                     "check for value update / binding deletion"));

			if (has_value) {
				duk_hstring *varname;

				DUK_DDD(DUK_DDDPRINT("defineProperty successful, key mapped to arguments 'map', "
				                     "update bound value (variable/argument)"));

				varname = duk_require_hstring(ctx, -1);
				DUK_ASSERT(varname != NULL);

				DUK_DDD(DUK_DDDPRINT("arguments object automatic putvar for a bound variable; "
				                     "key=%!O, varname=%!O, value=%!T",
				                     (duk_heaphdr *) key,
				                     (duk_heaphdr *) varname,
				                     (duk_tval *) duk_require_tval(ctx, idx_value)));

				/* strict flag for putvar comes from our caller (currently: fixed) */
				duk_js_putvar_envrec(thr, varenv, varname, duk_require_tval(ctx, idx_value), throw_flag);
			}
			if (has_writable && !is_writable) {
				DUK_DDD(DUK_DDDPRINT("defineProperty successful, key mapped to arguments 'map', "
				                     "changed to non-writable, delete arguments binding"));

				(void) duk_hobject_delprop_raw(thr, map, key, 0);  /* ignore result */
			}
		}

		/* 'varname' is in stack in this else branch, leaving an unbalanced stack below,
		 * but this doesn't matter now.
		 */
	}

 success_no_exotics:
	return;

 fail_virtual:
	DUK_ERROR(thr, DUK_ERR_TYPE_ERROR, DUK_STR_PROPERTY_IS_VIRTUAL);
	return;

 fail_not_writable_array_length:
	DUK_ERROR(thr, DUK_ERR_TYPE_ERROR, DUK_STR_ARRAY_LENGTH_NOT_WRITABLE);
	return;

 fail_not_extensible:
	DUK_ERROR(thr, DUK_ERR_TYPE_ERROR, DUK_STR_NOT_EXTENSIBLE);
	return;

 fail_not_configurable:
	DUK_ERROR(thr, DUK_ERR_TYPE_ERROR, DUK_STR_NOT_CONFIGURABLE);
	return;

 fail_array_length_partial:
	DUK_ERROR(thr, DUK_ERR_TYPE_ERROR, DUK_STR_ARRAY_LENGTH_WRITE_FAILED);
	return;
}

/*
 *  Object.prototype.hasOwnProperty() and Object.prototype.propertyIsEnumerable().
 */

DUK_INTERNAL duk_bool_t duk_hobject_object_ownprop_helper(duk_context *ctx, duk_small_uint_t required_desc_flags) {
	duk_hthread *thr = (duk_hthread *) ctx;
	duk_hstring *h_v;
	duk_hobject *h_obj;
	duk_propdesc desc;
	duk_bool_t ret;

	/* coercion order matters */
	h_v = duk_to_hstring(ctx, 0);
	DUK_ASSERT(h_v != NULL);

	h_obj = duk_push_this_coercible_to_object(ctx);
	DUK_ASSERT(h_obj != NULL);

	ret = duk__get_own_property_desc(thr, h_obj, h_v, &desc, 0 /*flags*/);  /* don't push value */

	duk_push_boolean(ctx, ret && ((desc.flags & required_desc_flags) == required_desc_flags));
	return 1;
}

/*
 *  Object.seal() and Object.freeze()  (E5 Sections 15.2.3.8 and 15.2.3.9)
 *
 *  Since the algorithms are similar, a helper provides both functions.
 *  Freezing is essentially sealing + making plain properties non-writable.
 *
 *  Note: virtual (non-concrete) properties which are non-configurable but
 *  writable would pose some problems, but such properties do not currently
 *  exist (all virtual properties are non-configurable and non-writable).
 *  If they did exist, the non-configurability does NOT prevent them from
 *  becoming non-writable.  However, this change should be recorded somehow
 *  so that it would turn up (e.g. when getting the property descriptor),
 *  requiring some additional flags in the object.
 */

DUK_INTERNAL void duk_hobject_object_seal_freeze_helper(duk_hthread *thr, duk_hobject *obj, duk_bool_t is_freeze) {
	duk_uint_fast32_t i;

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(thr->heap != NULL);
	DUK_ASSERT(obj != NULL);

	DUK_ASSERT_VALSTACK_SPACE(thr, DUK__VALSTACK_SPACE);

	/*
	 *  Abandon array part because all properties must become non-configurable.
	 *  Note that this is now done regardless of whether this is always the case
	 *  (skips check, but performance problem if caller would do this many times
	 *  for the same object; not likely).
	 */

	duk__abandon_array_checked(thr, obj);
	DUK_ASSERT(DUK_HOBJECT_GET_ASIZE(obj) == 0);

	for (i = 0; i < DUK_HOBJECT_GET_ENEXT(obj); i++) {
		duk_uint8_t *fp;

		/* since duk__abandon_array_checked() causes a resize, there should be no gaps in keys */
		DUK_ASSERT(DUK_HOBJECT_E_GET_KEY(thr->heap, obj, i) != NULL);

		/* avoid multiple computations of flags address; bypasses macros */
		fp = DUK_HOBJECT_E_GET_FLAGS_PTR(thr->heap, obj, i);
		if (is_freeze && !((*fp) & DUK_PROPDESC_FLAG_ACCESSOR)) {
			*fp &= ~(DUK_PROPDESC_FLAG_WRITABLE | DUK_PROPDESC_FLAG_CONFIGURABLE);
		} else {
			*fp &= ~DUK_PROPDESC_FLAG_CONFIGURABLE;
		}
	}

	DUK_HOBJECT_CLEAR_EXTENSIBLE(obj);

	/* no need to compact since we already did that in duk__abandon_array_checked()
	 * (regardless of whether an array part existed or not.
	 */

	return;
}

/*
 *  Object.isSealed() and Object.isFrozen()  (E5 Sections 15.2.3.11, 15.2.3.13)
 *
 *  Since the algorithms are similar, a helper provides both functions.
 *  Freezing is essentially sealing + making plain properties non-writable.
 *
 *  Note: all virtual (non-concrete) properties are currently non-configurable
 *  and non-writable (and there are no accessor virtual properties), so they don't
 *  need to be considered here now.
 */

DUK_INTERNAL duk_bool_t duk_hobject_object_is_sealed_frozen_helper(duk_hthread *thr, duk_hobject *obj, duk_bool_t is_frozen) {
	duk_uint_fast32_t i;

	DUK_ASSERT(obj != NULL);
	DUK_UNREF(thr);

	/* Note: no allocation pressure, no need to check refcounts etc */

	/* must not be extensible */
	if (DUK_HOBJECT_HAS_EXTENSIBLE(obj)) {
		return 0;
	}

	/* all virtual properties are non-configurable and non-writable */

	/* entry part must not contain any configurable properties, or
	 * writable properties (if is_frozen).
	 */
	for (i = 0; i < DUK_HOBJECT_GET_ENEXT(obj); i++) {
		duk_small_uint_t flags;

		if (!DUK_HOBJECT_E_GET_KEY(thr->heap, obj, i)) {
			continue;
		}

		/* avoid multiple computations of flags address; bypasses macros */
		flags = (duk_small_uint_t) DUK_HOBJECT_E_GET_FLAGS(thr->heap, obj, i);

		if (flags & DUK_PROPDESC_FLAG_CONFIGURABLE) {
			return 0;
		}
		if (is_frozen &&
		    !(flags & DUK_PROPDESC_FLAG_ACCESSOR) &&
		    (flags & DUK_PROPDESC_FLAG_WRITABLE)) {
			return 0;
		}
	}

	/* array part must not contain any non-unused properties, as they would
	 * be configurable and writable.
	 */
	for (i = 0; i < DUK_HOBJECT_GET_ASIZE(obj); i++) {
		duk_tval *tv = DUK_HOBJECT_A_GET_VALUE_PTR(thr->heap, obj, i);
		if (!DUK_TVAL_IS_UNDEFINED_UNUSED(tv)) {
			return 0;
		}
	}

	return 1;
}

/*
 *  Object.preventExtensions() and Object.isExtensible()  (E5 Sections 15.2.3.10, 15.2.3.13)
 *
 *  Not needed, implemented by macros DUK_HOBJECT_{HAS,CLEAR,SET}_EXTENSIBLE
 *  and the Object built-in bindings.
 */

/* Undefine local defines */

#undef DUK__NO_ARRAY_INDEX
#undef DUK__HASH_INITIAL
#undef DUK__HASH_PROBE_STEP
#undef DUK__HASH_UNUSED
#undef DUK__HASH_DELETED
#undef DUK__VALSTACK_SPACE
