/*
 * Copyright (c) 2014, 2015 EMC Corporation.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef NTRDMA_UTIL_H
#define NTRDMA_UTIL_H

#include "ntrdma_os.h"

#define NTRDMA_PTR_OFFSET(ptr, off) \
	((void*)(((unsigned long)ptr) + off))

/* return the next power of two, including val */
long ntrdma_pow_two_up(long val);
/* return the previous power of two, including val */
long ntrdma_pow_two_down(long val);
/* return the next power of two, excluding val */
long ntrdma_pow_two_next(long val);
/* return the previous power of two, excluding val */
long ntrdma_pow_two_prev(long val);

/* find the first one bit in the bitset */
int ntrdma_bits_first(void *bits, int count);
/* find the next one bit in the bitset, starting at and including pos */
int ntrdma_bits_next(void *bits, int count, int pos);

/* find the first zero bit in the bitset */
int ntrdma_bits_first_zero(void *bits, int count);
/* find the next zero bit in the bitset, starting at and including pos */
int ntrdma_bits_next_zero(void *bits, int count, int pos);

/* set the bit at pos in the bitset */
void ntrdma_bits_set(void *bits, int pos);
/* clear the bit at pos in the bitset */
void ntrdma_bits_clear(void *bits, int pos);

/* for each position of a one bit in the bit set */
#define ntrdma_bits_for_each(pos, count, bits) \
	for (pos = ntrdma_bits_first(bits, count);		\
	     pos < count;					\
	     pos = ntrdma_bits_next(bits, count, pos + 1))

/* Resizable vector */
struct ntrdma_vec {
	/* Capacity of the vec */
	int				cap;
	/* Key indexed lookup of elements, cap elements long */
	void				**look;
	NTRDMA_DECL_SPL			(lock);
};

/* Allocate an empty vector with a capacity */
int ntrdma_vec_init(struct ntrdma_vec *vec, int cap, int node);
/* Destroy a vector */
void ntrdma_vec_deinit(struct ntrdma_vec *vec);
/* Look up an element in a vector */
void *ntrdma_vec_look(struct ntrdma_vec *vec, ntrdma_u32_t key);
/* Look up an element in a vector */
void ntrdma_vec_set(struct ntrdma_vec *vec, ntrdma_u32_t key, void *elem);
/* Lock vec access */
void ntrdma_vec_lock(struct ntrdma_vec *vec);
/* Unlock vec access */
void ntrdma_vec_unlock(struct ntrdma_vec *vec);

/* TODO: move to static in c file */
/* Resize a vector if cap is larger than the allocated capacity */
int ntrdma_vec_resize_larger(struct ntrdma_vec *vec, ntrdma_u32_t key, int node);
/* Resize a vector to ensure that key is within the capacity */
static inline
int ntrdma_vec_ensure_key(struct ntrdma_vec *vec, ntrdma_u32_t key, int node)
{
	if (key >= vec->cap)
		return ntrdma_vec_resize_larger(vec, ntrdma_pow_two_next(key), node);

	return 0;
}

/* Resizable vector with key reservation */
struct ntrdma_kvec {
	/* Capacity of the vec */
	int				cap;
	/* Next key to check when reserving */
	int				next_key;
	/* Bitset of available/used keys, cap bits long */
	unsigned long			*keys;
	/* Key indexed lookup of elements, cap elements long */
	void				**look;
	NTRDMA_DECL_SPL			(lock);
};

/* Allocate an empty vector with a capacity */
int ntrdma_kvec_init(struct ntrdma_kvec *vec, ntrdma_u32_t cap, int node);
/* Destroy a vector */
void ntrdma_kvec_deinit(struct ntrdma_kvec *vec);
/* Reserve the next available key */
ntrdma_u32_t ntrdma_kvec_reserve_key(struct ntrdma_kvec *vec, int node);
/* Dispose a key that no longer needs to be reserved */
void ntrdma_kvec_dispose_key(struct ntrdma_kvec *vec, ntrdma_u32_t key);
/* Look up an element in a vector */
void *ntrdma_kvec_look(struct ntrdma_kvec *vec, ntrdma_u32_t key);
/* Look up an element in a vector */
void ntrdma_kvec_set(struct ntrdma_kvec *vec, ntrdma_u32_t key, void *elem);
/* Lock kvec access */
void ntrdma_kvec_lock(struct ntrdma_kvec *vec);
/* Unlock kvec access */
void ntrdma_kvec_unlock(struct ntrdma_kvec *vec);

/* TODO: move to static in c file */
/* Resize a vector if cap is larger than the allocated capacity */
int ntrdma_kvec_resize_larger(struct ntrdma_kvec *vec, ntrdma_u32_t key, int node);
/* Resize a vector to twice as large */
static inline
int ntrdma_kvec_resize_double(struct ntrdma_kvec *vec, int node)
{
	return ntrdma_kvec_resize_larger(vec, vec->cap << 1, node);
}

#endif

