/*!
 * \file	sccp_vector.h
 * \brief       SCCP Vector Header
 * \note	Vector Code derived from Asterisk 12 "vector.h"
 * 		Copyright (C) 2013, Digium, Inc.
 * \author 	David M. Lee, II <dlee@digium.com>
 * \note	This program is free software and may be modified and distributed under the terms of the GNU Public License.
 *		See the LICENSE file at the top of the source tree.
 */
#pragma once

/* A vector is a variable length array, with properties that can be useful when order doesn't matter. */

#define SCCP_VECTOR(name, type)                                                                                                                                                                                                 \
	struct name {                                                                                                                                                                                                           \
		type * elems;                                                                                                                                                                                                   \
		size_t max;                                                                                                                                                                                                     \
		size_t current;                                                                                                                                                                                                 \
	}

SCCP_VECTOR(sccp_vector_int, int);

SCCP_VECTOR(sccp_vector_string, char *);

#define SCCP_VECTOR_RW(name, type)                                                                                                                                                                                              \
	struct name {                                                                                                                                                                                                           \
		type *       elems;                                                                                                                                                                                             \
		size_t       max;                                                                                                                                                                                               \
		size_t       current;                                                                                                                                                                                           \
		pbx_rwlock_t lock;                                                                                                                                                                                              \
	}

#define SCCP_VECTOR_INIT(vec, size)                                                                                                                                                                                             \
	({                                                                                                                                                                                                                      \
		size_t __size     = (size);                                                                                                                                                                                     \
		size_t alloc_size = __size * sizeof(*((vec)->elems));                                                                                                                                                           \
		(vec)->elems      = alloc_size ? (typeof(((vec)->elems)))ast_calloc(1, alloc_size) : NULL;                                                                                                                      \
		(vec)->current    = 0;                                                                                                                                                                                          \
		if ((vec)->elems) {                                                                                                                                                                                             \
			(vec)->max = __size;                                                                                                                                                                                    \
		} else {                                                                                                                                                                                                        \
			(vec)->max = 0;                                                                                                                                                                                         \
		}                                                                                                                                                                                                               \
		(alloc_size == 0 || (vec)->elems != NULL) ? 0 : -1;                                                                                                                                                             \
	})

/* With size 0 no space is allocated until the first append. */
#define SCCP_VECTOR_RW_INIT(vec, size)                                                                                                                                                                                          \
	({                                                                                                                                                                                                                      \
		int __sccp_vector_res = -1;                                                                                                                                                                                     \
		if (SCCP_VECTOR_INIT(vec, size) == 0) {                                                                                                                                                                         \
			__sccp_vector_res = pbx_rwlock_init(&(vec)->lock);                                                                                                                                                      \
		}                                                                                                                                                                                                               \
		__sccp_vector_res;                                                                                                                                                                                              \
	})

/* If any code to free the elements of this vector needs to be run, that should be done prior to this call. */
#define SCCP_VECTOR_FREE(vec)                                                                                                                                                                                                   \
	do {                                                                                                                                                                                                                    \
		ast_free((vec)->elems);                                                                                                                                                                                         \
		(vec)->elems   = NULL;                                                                                                                                                                                          \
		(vec)->max     = 0;                                                                                                                                                                                             \
		(vec)->current = 0;                                                                                                                                                                                             \
	} while (0)

/* If any code to free the elements of this vector need to be run, that should be done prior to this call. */
#define SCCP_VECTOR_PTR_FREE(vec)                                                                                                                                                                                               \
	do {                                                                                                                                                                                                                    \
		SCCP_VECTOR_FREE(vec);                                                                                                                                                                                          \
		ast_free(vec);                                                                                                                                                                                                  \
	} while (0)

/*
 * Deallocates this locked vector
 * If any code to free the elements of this vector need to be run, that should be done prior to this call.
 */
#define SCCP_VECTOR_RW_FREE(vec)                                                                                                                                                                                                \
	do {                                                                                                                                                                                                                    \
		SCCP_VECTOR_FREE(vec);                                                                                                                                                                                          \
		pbx_rwlock_destroy(&(vec)->lock);                                                                                                                                                                               \
	} while (0)

/*
 * Deallocates this locked vector pointer.
 * If any code to free the elements of this vector need to be run, that should be done prior to this call.
 */
#define SCCP_VECTOR_RW_PTR_FREE(vec)                                                                                                                                                                                            \
	do {                                                                                                                                                                                                                    \
		SCCP_VECTOR_RW_FREE(vec);                                                                                                                                                                                       \
		ast_free(vec);                                                                                                                                                                                                  \
	} while (0)

#define __sccp_make_room(idx, vec)                                                                                                                                                                                              \
	({                                                                                                                                                                                                                      \
		int __sccp_vector_res1 = 0;                                                                                                                                                                                     \
 \
		do {                                                                                                                                                                                                            \
			if ((idx) >= (vec)->max) {                                                                                                                                                                              \
				size_t               new_max   = ((idx) + 1) * 2;                                                                                                                                               \
				typeof((vec)->elems) new_elems = (typeof((vec)->elems))ast_calloc(1, new_max * sizeof(*new_elems));                                                                                             \
				if (new_elems) {                                                                                                                                                                                \
					if ((vec)->elems) {                                                                                                                                                                     \
						memcpy(new_elems, (vec)->elems, (vec)->current * sizeof(*new_elems));                                                                                                           \
						ast_free((vec)->elems);                                                                                                                                                         \
					}                                                                                                                                                                                       \
					(vec)->elems = new_elems;                                                                                                                                                               \
					(vec)->max   = new_max;                                                                                                                                                                 \
				} else {                                                                                                                                                                                        \
					__sccp_vector_res1 = -1;                                                                                                                                                                \
					break;                                                                                                                                                                                  \
				}                                                                                                                                                                                               \
			}                                                                                                                                                                                                       \
		} while (0);                                                                                                                                                                                                    \
		__sccp_vector_res1;                                                                                                                                                                                             \
	})

#define SCCP_VECTOR_APPEND(vec, elem)                                                                                                                                                                                           \
	({                                                                                                                                                                                                                      \
		int __sccp_vector_res = 0;                                                                                                                                                                                      \
		do {                                                                                                                                                                                                            \
			if (__sccp_make_room((vec)->current, vec) != 0) {                                                                                                                                                       \
				__sccp_vector_res = -1;                                                                                                                                                                         \
				break;                                                                                                                                                                                          \
			}                                                                                                                                                                                                       \
			(vec)->elems[(vec)->current++] = (elem);                                                                                                                                                                \
		} while (0);                                                                                                                                                                                                    \
		__sccp_vector_res;                                                                                                                                                                                              \
	})

#define SCCP_VECTOR_REPLACE(vec, idx, elem)                                                                                                                                                                                     \
	({                                                                                                                                                                                                                      \
		int __sccp_vector_res = 0;                                                                                                                                                                                      \
		do {                                                                                                                                                                                                            \
			if (__sccp_make_room((idx), vec) != 0) {                                                                                                                                                                \
				__sccp_vector_res = -1;                                                                                                                                                                         \
				break;                                                                                                                                                                                          \
			}                                                                                                                                                                                                       \
			(vec)->elems[(idx)] = (elem);                                                                                                                                                                           \
			if (((idx) + 1) > (vec)->current) {                                                                                                                                                                     \
				(vec)->current = (idx) + 1;                                                                                                                                                                     \
			}                                                                                                                                                                                                       \
		} while (0);                                                                                                                                                                                                    \
		__sccp_vector_res;                                                                                                                                                                                              \
	})

#define SCCP_VECTOR_INSERT_AT(vec, idx, elem)                                                                                                                                                                                   \
	({                                                                                                                                                                                                                      \
		int    __sccp_vector_res = 0;                                                                                                                                                                                   \
		size_t __move;                                                                                                                                                                                                  \
		do {                                                                                                                                                                                                            \
			if (__sccp_make_room(((idx) > (vec)->current ? (idx) : (vec)->current), vec) != 0) {                                                                                                                    \
				__sccp_vector_res = -1;                                                                                                                                                                         \
				break;                                                                                                                                                                                          \
			}                                                                                                                                                                                                       \
			if ((vec)->current > 0 && (idx) < (vec)->current) {                                                                                                                                                     \
				__move = ((vec)->current - (idx)) * sizeof(typeof((vec)->elems[0]));                                                                                                                            \
				memmove(&(vec)->elems[(idx) + 1], &(vec)->elems[(idx)], __move);                                                                                                                                \
			}                                                                                                                                                                                                       \
			(vec)->elems[(idx)] = (elem);                                                                                                                                                                           \
			(vec)->current      = ((idx) > (vec)->current ? (idx) : (vec)->current) + 1;                                                                                                                            \
		} while (0);                                                                                                                                                                                                    \
		__sccp_vector_res;                                                                                                                                                                                              \
	})

#define SCCP_VECTOR_ADD_SORTED(vec, elem, cmp)                                                                                                                                                                                  \
	({                                                                                                                                                                                                                      \
		int    __sccp_vector_res = 0;                                                                                                                                                                                   \
		size_t __sccp_vector_idx = (vec)->current;                                                                                                                                                                      \
		do {                                                                                                                                                                                                            \
			if (__sccp_make_room((vec)->current, vec) != 0) {                                                                                                                                                       \
				__sccp_vector_res = -1;                                                                                                                                                                         \
				break;                                                                                                                                                                                          \
			}                                                                                                                                                                                                       \
			while (__idx > 0 && (cmp((vec)->elems[__sccp_vector_idx - 1], elem) > 0)) {                                                                                                                             \
				(vec)->elems[__idx] = (vec)->elems[__sccp_vector_idx - 1];                                                                                                                                      \
				__idx--;                                                                                                                                                                                        \
			}                                                                                                                                                                                                       \
			(vec)->elems[__sccp_vector_idx] = elem;                                                                                                                                                                 \
			(vec)->current++;                                                                                                                                                                                       \
		} while (0);                                                                                                                                                                                                    \
		__sccp_vector_res;                                                                                                                                                                                              \
	})

/* Unless preserve_order is set, elements may be reordered so the removal takes constant time. */
#define SCCP_VECTOR_REMOVE(vec, idx, preserve_order)                                                                                                                                                                            \
	({                                                                                                                                                                                                                      \
		typeof((vec)->elems[0]) __sccp_vector_res1;                                                                                                                                                                     \
		size_t                  __sccp_vector_idx1 = (idx);                                                                                                                                                             \
		ast_assert(__sccp_vector_idx1 < (vec)->current);                                                                                                                                                                \
		__sccp_vector_res1 = (vec)->elems[__sccp_vector_idx1];                                                                                                                                                          \
		if ((preserve_order)) {                                                                                                                                                                                         \
			size_t __move;                                                                                                                                                                                          \
			__move = ((vec)->current - (__sccp_vector_idx1)-1) * sizeof(typeof((vec)->elems[0]));                                                                                                                   \
			memmove(&(vec)->elems[__sccp_vector_idx1], &(vec)->elems[__sccp_vector_idx1 + 1], __move);                                                                                                              \
			(vec)->current--;                                                                                                                                                                                       \
		} else {                                                                                                                                                                                                        \
			(vec)->elems[__sccp_vector_idx1] = (vec)->elems[--(vec)->current];                                                                                                                                      \
		};                                                                                                                                                                                                              \
		__sccp_vector_res1;                                                                                                                                                                                             \
	})

#define SCCP_VECTOR_REMOVE_UNORDERED(vec, idx) SCCP_VECTOR_REMOVE(vec, idx, 0)

#define SCCP_VECTOR_REMOVE_ORDERED(vec, idx) SCCP_VECTOR_REMOVE(vec, idx, 1)

#define SCCP_VECTOR_REMOVE_CMP_UNORDERED(vec, value, cmp, cleanup)                                                                                                                                                              \
	({                                                                                                                                                                                                                      \
		int           __sccp_vector_res = -1;                                                                                                                                                                           \
		size_t        __sccp_vector_idx;                                                                                                                                                                                \
		typeof(value) __value = (value);                                                                                                                                                                                \
		for (__sccp_vector_idx = 0; __sccp_vector_idx < (vec)->current; ++__sccp_vector_idx) {                                                                                                                          \
			if (cmp((vec)->elems[__sccp_vector_idx], __value)) {                                                                                                                                                    \
				cleanup((vec)->elems[__sccp_vector_idx]);                                                                                                                                                       \
				SCCP_VECTOR_REMOVE_UNORDERED((vec), __sccp_vector_idx);                                                                                                                                         \
				__sccp_vector_res = 0;                                                                                                                                                                          \
				break;                                                                                                                                                                                          \
			}                                                                                                                                                                                                       \
		}                                                                                                                                                                                                               \
		__sccp_vector_res;                                                                                                                                                                                              \
	})

#define SCCP_VECTOR_REMOVE_CMP_ORDERED(vec, value, cmp, cleanup)                                                                                                                                                                \
	({                                                                                                                                                                                                                      \
		int           __sccp_vector_res = -1;                                                                                                                                                                           \
		size_t        __sccp_vector_idx;                                                                                                                                                                                \
		typeof(value) __value = (value);                                                                                                                                                                                \
		for (__sccp_vector_idx = 0; __sccp_vector_idx < (vec)->current; ++__sccp_vector_idx) {                                                                                                                          \
			if (cmp((vec)->elems[__sccp_vector_idx], __value)) {                                                                                                                                                    \
				cleanup((vec)->elems[__sccp_vector_idx]);                                                                                                                                                       \
				SCCP_VECTOR_REMOVE_ORDERED((vec), __sccp_vector_idx);                                                                                                                                           \
				__sccp_vector_res = 0;                                                                                                                                                                          \
				break;                                                                                                                                                                                          \
			}                                                                                                                                                                                                       \
		}                                                                                                                                                                                                               \
		__sccp_vector_res;                                                                                                                                                                                              \
	})

#define SCCP_VECTOR_ELEM_DEFAULT_CMP(elem, value) ((elem) == (value))

#define SCCP_VECTOR_ELEM_CLEANUP_NOOP(elem)

#define SCCP_VECTOR_REMOVE_ELEM_UNORDERED(vec, elem, cleanup) ({ SCCP_VECTOR_REMOVE_CMP_UNORDERED((vec), (elem), SCCP_VECTOR_ELEM_DEFAULT_CMP, cleanup); })

#define SCCP_VECTOR_REMOVE_ELEM_ORDERED(vec, elem, cleanup) ({ SCCP_VECTOR_REMOVE_CMP_ORDERED((vec), (elem), SCCP_VECTOR_ELEM_DEFAULT_CMP, cleanup); })

#define SCCP_VECTOR_SIZE(vec) (vec)->current

#define SCCP_VECTOR_RESET(vec, cleanup)                                                                                                                                                                                         \
	({                                                                                                                                                                                                                      \
		SCCP_VECTOR_CALLBACK_VOID(vec, cleanup);                                                                                                                                                                        \
		(vec)->current = 0;                                                                                                                                                                                             \
	})

#define SCCP_VECTOR_GET_ADDR(vec, idx)                                                                                                                                                                                          \
	({                                                                                                                                                                                                                      \
		size_t __sccp_vector_idx = (idx);                                                                                                                                                                               \
		ast_assert(__sccp_vector_idx < (vec)->current);                                                                                                                                                                 \
		&(vec)->elems[__sccp_vector_idx];                                                                                                                                                                               \
	})

#define SCCP_VECTOR_GET(vec, idx)                                                                                                                                                                                               \
	({                                                                                                                                                                                                                      \
		size_t __sccp_vector_idx = (idx);                                                                                                                                                                               \
		ast_assert(__sccp_vector_idx < (vec)->current);                                                                                                                                                                 \
		(vec)->elems[__sccp_vector_idx];                                                                                                                                                                                \
	})

#define SCCP_VECTOR_GET_CMP(vec, value, cmp)                                                                                                                                                                                    \
	({                                                                                                                                                                                                                      \
		void *        __sccp_vector_res = NULL;                                                                                                                                                                         \
		size_t        __sccp_vector_idx;                                                                                                                                                                                \
		typeof(value) __value = (value);                                                                                                                                                                                \
		for (__sccp_vector_idx = 0; __sccp_vector_idx < (vec)->current; ++__sccp_vector_idx) {                                                                                                                          \
			if (cmp((vec)->elems[__sccp_vector_idx], __value)) {                                                                                                                                                    \
				__sccp_vector_res = &(vec)->elems[__sccp_vector_idx];                                                                                                                                           \
				break;                                                                                                                                                                                          \
			}                                                                                                                                                                                                       \
		}                                                                                                                                                                                                               \
		__sccp_vector_res;                                                                                                                                                                                              \
	})

/* Returns CMP_MATCH always. */
#define SCCP_VECTOR_MATCH_ALL(element) (CMP_MATCH)

#define SCCP_VECTOR_CALLBACK(vec, callback, default_value, ...)                                                                                                                                                                 \
	({                                                                                                                                                                                                                      \
		size_t                  __sccp_vector_idx;                                                                                                                                                                      \
		typeof((vec)->elems[0]) __sccp_vector_res = default_value;                                                                                                                                                      \
		for (__sccp_vector_idx = 0; __sccp_vector_idx < (vec)->current; __sccp_vector_idx++) {                                                                                                                          \
			int rc = callback((vec)->elems[__sccp_vector_idx], ##__VA_ARGS__);                                                                                                                                      \
			if (rc & CMP_MATCH) {                                                                                                                                                                                   \
				__sccp_vector_res = (vec)->elems[__sccp_vector_idx];                                                                                                                                            \
				break;                                                                                                                                                                                          \
			}                                                                                                                                                                                                       \
			if (rc & CMP_STOP) {                                                                                                                                                                                    \
				break;                                                                                                                                                                                          \
			}                                                                                                                                                                                                       \
		}                                                                                                                                                                                                               \
		__sccp_vector_res;                                                                                                                                                                                              \
	})

/*
 * Free the returned vector with SCCP_VECTOR_PTR_FREE(). Its type must come from the same declaration as the original vector:
 * SCCP_VECTOR(mytype, char *) vector_1; struct mytype *vector_2 = SCCP_VECTOR_CALLBACK_MULTIPLE(&vector_1, callback);
 * (two separate SCCP_VECTOR(, char *) declarations are different struct types and do not assign).
 */
#define SCCP_VECTOR_CALLBACK_MULTIPLE(vec, callback, ...)                                                                                                                                                                       \
	({                                                                                                                                                                                                                      \
		size_t        __sccp_vector_idx;                                                                                                                                                                                \
		typeof((vec)) new_vec = NULL;                                                                                                                                                                                   \
		do {                                                                                                                                                                                                            \
			new_vec = (typeof((vec)))ast_malloc(sizeof(*new_vec));                                                                                                                                                  \
			if (!new_vec) {                                                                                                                                                                                         \
				break;                                                                                                                                                                                          \
			}                                                                                                                                                                                                       \
			if (SCCP_VECTOR_INIT(new_vec, SCCP_VECTOR_SIZE((vec))) != 0) {                                                                                                                                          \
				ast_free(new_vec);                                                                                                                                                                              \
				new_vec = NULL;                                                                                                                                                                                 \
				break;                                                                                                                                                                                          \
			}                                                                                                                                                                                                       \
			for (__sccp_vector_idx = 0; __sccp_vector_idx < (vec)->current; __sccp_vector_idx++) {                                                                                                                  \
				int rc = callback((vec)->elems[__sccp_vector_idx], ##__VA_ARGS__);                                                                                                                              \
				if (rc & CMP_MATCH) {                                                                                                                                                                           \
					SCCP_VECTOR_APPEND(new_vec, (vec)->elems[__sccp_vector_idx]);                                                                                                                           \
				}                                                                                                                                                                                               \
				if (rc & CMP_STOP) {                                                                                                                                                                            \
					break;                                                                                                                                                                                  \
				}                                                                                                                                                                                               \
			}                                                                                                                                                                                                       \
		} while (0);                                                                                                                                                                                                    \
		new_vec;                                                                                                                                                                                                        \
	})

#define SCCP_VECTOR_CALLBACK_VOID(vec, callback, ...)                                                                                                                                                                           \
	({                                                                                                                                                                                                                      \
		size_t __sccp_vector_idx;                                                                                                                                                                                       \
		for (__sccp_vector_idx = 0; __sccp_vector_idx < (vec)->current; __sccp_vector_idx++) {                                                                                                                          \
			callback((vec)->elems[__sccp_vector_idx], ##__VA_ARGS__);                                                                                                                                               \
		}                                                                                                                                                                                                               \
	})

#define SCCP_VECTOR_RW_RDLOCK(vec)                 pbx_rwlock_rdlock(&(vec)->lock)
#define SCCP_VECTOR_RW_WRLOCK(vec)                 pbx_rwlock_wrlock(&(vec)->lock)
#define SCCP_VECTOR_RW_UNLOCK(vec)                 pbx_rwlock_unlock(&(vec)->lock)
#define SCCP_VECTOR_RW_RDLOCK_TRY(vec)             pbx_rwlock_tryrdlock(&(vec)->lock)
#define SCCP_VECTOR_RW_WRLOCK_TRY(vec)             pbx_rwlock_trywrlock(&(vec)->lock)
#define SCCP_VECTOR_RW_RDLOCK_TIMED(vec, timespec) pbx_rwlock_timedrdlock(&(vec)->lock, timespec)
#define SCCP_VECTOR_RW_WRLOCK_TIMED(vec, timespec) pbx_rwlock_timedwrlock(&(vec)->lock, timespec)
