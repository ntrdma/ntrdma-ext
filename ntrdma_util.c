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

#include "ntrdma_os.h"
#include "ntrdma_mem.h"
#include "ntrdma_util.h"

long ntrdma_pow_two_up(long val)
{
	return ntrdma_pow_two_next(val - 1);
}

long ntrdma_pow_two_down(long val)
{
	return 1l << ntrdma_bsr_long(val);
}

long ntrdma_pow_two_next(long val)
{
	return 1l << (ntrdma_bsr_long(val) + 1);
}

long ntrdma_pow_two_prev(long val)
{
	return ntrdma_pow_two_down(val - 1);
}

int ntrdma_bits_first(void *bits, int count)
{
	int pos = 0;
	unsigned long val;
	unsigned long *ptr = bits;

	while (pos < count) {
		val = *ptr;
		if (val) {
			pos += ntrdma_bsf_long(val);
			break;
		}
		pos += NTRDMA_BITS_PER_LONG;
		++ptr;
	}

	if (pos < count)
		return pos;

	return count;
}

int ntrdma_bits_next(void *bits, int count, int pos)
{
	int off;
	unsigned long val;
	unsigned long *ptr = bits;

	if (pos < count) {
		ptr += pos / NTRDMA_BITS_PER_LONG;
		off = pos % NTRDMA_BITS_PER_LONG;
		val = *ptr >> off;
		if (val) {
			pos += ntrdma_bsf_long(val);
		} else {
			pos += NTRDMA_BITS_PER_LONG - off;

			while (pos < count) {
				val = *ptr;
				if (val) {
					pos += ntrdma_bsf_long(val);
					break;
				}
				pos += NTRDMA_BITS_PER_LONG;
				++ptr;
			}
		}

		if (pos < count)
			return pos;
	}

	return count;
}

int ntrdma_bits_first_zero(void *bits, int count)
{
	int pos = 0;
	unsigned long val;
	unsigned long *ptr = bits;

	while (pos < count) {
		val = ~*ptr;
		if (val) {
			pos += ntrdma_bsf_long(val);
			break;
		}
		pos += NTRDMA_BITS_PER_LONG;
		++ptr;
	}

	if (pos < count)
		return pos;

	return count;
}

int ntrdma_bits_next_zero(void *bits, int count, int pos)
{
	int off;
	unsigned long val;
	unsigned long *ptr = bits;

	if (pos < count) {
		ptr += pos / NTRDMA_BITS_PER_LONG;
		off = pos % NTRDMA_BITS_PER_LONG;
		val = (~*ptr) >> off;
		if (val) {
			pos += ntrdma_bsf_long(val);
		} else {
			pos += NTRDMA_BITS_PER_LONG - off;

			while (pos < count) {
				val = ~*ptr;
				if (val) {
					pos += ntrdma_bsf_long(val);
					break;
				}
				pos += NTRDMA_BITS_PER_LONG;
				++ptr;
			}
		}

		if (pos < count)
			return pos;
	}

	return count;
}

void ntrdma_bits_set(void *bits, int pos)
{
	unsigned long *ptr = bits;

	ptr[pos / NTRDMA_BITS_PER_LONG] |= 1l << (pos % NTRDMA_BITS_PER_LONG);
}

void ntrdma_bits_clear(void *bits, int pos)
{
	unsigned long *ptr = bits;

	ptr[pos / NTRDMA_BITS_PER_LONG] &= ~(1l << (pos % NTRDMA_BITS_PER_LONG));
}

int ntrdma_vec_init(struct ntrdma_vec *vec, int cap, int node)
{
	vec->look = ntrdma_zalloc(cap * sizeof(*vec->look), node);
	if (!vec->look)
		goto err_look;

	vec->cap = cap;

	ntrdma_spl_create(&vec->lock, "vec_lock");

	return 0;

	//ntrdma_free(vec->look);
err_look:
	return -ENOMEM;
}

void ntrdma_vec_deinit(struct ntrdma_vec *vec)
{
	ntrdma_free(vec->look);
}

void *ntrdma_vec_look(struct ntrdma_vec *vec, ntrdma_u32_t key)
{
	NTRDMA_HARD_ASSERT(0 <= key);

	if (key < vec->cap)
		return vec->look[key];

	return NULL;
}

void ntrdma_vec_set(struct ntrdma_vec *vec, ntrdma_u32_t key, void *elem)
{
	NTRDMA_HARD_ASSERT(0 <= key);
	NTRDMA_HARD_ASSERT(key < vec->cap);

	vec->look[key] = elem;
}

void ntrdma_vec_lock(struct ntrdma_vec *vec)
{
	ntrdma_spl_lock(&vec->lock);
}

void ntrdma_vec_unlock(struct ntrdma_vec *vec)
{
	ntrdma_spl_unlock(&vec->lock);
}

int ntrdma_vec_resize_larger(struct ntrdma_vec *vec, ntrdma_u32_t cap, int node)
{
	struct ntrdma_vec vec_dst, vec_src;
	int err;

	NTRDMA_HARD_ASSERT(cap > vec->cap);

	vec_src = *vec;

	err = ntrdma_vec_init(&vec_dst, cap, node);
	if (err)
		goto err_dst;

	ntrdma_memcpy(vec_dst.look, vec_src.look, vec_src.cap * sizeof(*vec_src.look));

	*vec = vec_dst;

	ntrdma_vec_deinit(&vec_src);

	return 0;

err_dst:
	return -ENOMEM;
}

int ntrdma_kvec_init(struct ntrdma_kvec *vec, ntrdma_u32_t cap, int node)
{
	vec->keys = ntrdma_zalloc(NTRDMA_BITS_TO_BYTES(cap), node);
	if (!vec->keys)
		goto err_keys;

	vec->look = ntrdma_zalloc(cap * sizeof(*vec->look), node);
	if (!vec->look)
		goto err_look;

	vec->cap = cap;
	vec->next_key = 0;

	ntrdma_spl_create(&vec->lock, "kvec_lock");

	return 0;

	//ntrdma_free(vec->look);
err_look:
	ntrdma_free(vec->keys);
err_keys:
	return -ENOMEM;
}

void ntrdma_kvec_deinit(struct ntrdma_kvec *vec)
{
	ntrdma_free(vec->look);
	ntrdma_free(vec->keys);
}

ntrdma_u32_t ntrdma_kvec_reserve_key(struct ntrdma_kvec *vec, int node)
{
	ntrdma_u32_t key;

	key = ntrdma_bits_next_zero(vec->keys, vec->cap, vec->next_key);
	if (key < 0)
		goto err_key;

	if (key == vec->cap)
		ntrdma_kvec_resize_double(vec, node);

	ntrdma_bits_set(vec->keys, key);

	vec->next_key = key + 1;

err_key:
	return key;
}

void ntrdma_kvec_dispose_key(struct ntrdma_kvec *vec, ntrdma_u32_t key)
{
	ntrdma_bits_clear(vec->keys, key);

	if (key < vec->next_key)
		vec->next_key = key;
}

void *ntrdma_kvec_look(struct ntrdma_kvec *vec, ntrdma_u32_t key)
{
	NTRDMA_HARD_ASSERT(0 <= key);

	if (key < vec->cap)
		return vec->look[key];

	return NULL;
}

void ntrdma_kvec_set(struct ntrdma_kvec *vec, ntrdma_u32_t key, void *elem)
{
	NTRDMA_HARD_ASSERT(0 <= key);
	NTRDMA_HARD_ASSERT(key < vec->cap);

	vec->look[key] = elem;
}

void ntrdma_kvec_lock(struct ntrdma_kvec *vec)
{
	ntrdma_spl_lock(&vec->lock);
}

void ntrdma_kvec_unlock(struct ntrdma_kvec *vec)
{
	ntrdma_spl_unlock(&vec->lock);
}

int ntrdma_kvec_resize_larger(struct ntrdma_kvec *vec, ntrdma_u32_t cap, int node)
{
	struct ntrdma_kvec vec_dst, vec_src;
	int err;

	NTRDMA_HARD_ASSERT(cap > vec->cap);

	vec_src = *vec;

	err = ntrdma_kvec_init(&vec_dst, cap, node);
	if (err)
		goto err_dst;

	ntrdma_memcpy(vec_dst.keys, vec_src.keys, NTRDMA_BITS_TO_BYTES(vec_src.cap));
	ntrdma_memcpy(vec_dst.look, vec_src.look, vec_src.cap * sizeof(*vec_src.look));

	*vec = vec_dst;

	ntrdma_kvec_deinit(&vec_src);

	return 0;

err_dst:
	return -ENOMEM;
}

