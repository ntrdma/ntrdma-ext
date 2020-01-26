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

#include <linux/slab.h>

#include "ntrdma_util.h"

#define BITS_TO_LONGS_SIZE(bits) \
	(BITS_TO_LONGS(bits) * sizeof(long))

#define MAX_VEC_CAP 0x800000U
#define MAX_KVEC_CAP 0x800000U

int ntrdma_vec_init(struct ntrdma_vec *vec, u32 cap, int node)
{
	if (cap == 0)
		return -EINVAL;

	if (cap >= MAX_VEC_CAP)
		return -ENOMEM;

	vec->look = kzalloc_node(cap * sizeof(*vec->look), GFP_KERNEL, node);
	if (!vec->look)
		return -ENOMEM;

	vec->cap = cap;

	rwlock_init(&vec->lock);

	return 0;
}

void ntrdma_vec_deinit(struct ntrdma_vec *vec)
{
	kfree(vec->look);
}

int ntrdma_vec_resize_larger(struct ntrdma_vec *vec, u32 cap, int node)
{
	void **look;

	if (cap == 0)
		return -EINVAL;

	if (cap >= MAX_VEC_CAP)
		return -ENOMEM;

	look = kzalloc_node(cap * sizeof(*look), GFP_KERNEL, node);
	if (!look)
		return -ENOMEM;

	write_lock_bh(&vec->lock);
	if (cap > vec->cap) {
		memcpy(look, vec->look, vec->cap * sizeof(*vec->look));
		kfree(vec->look);
		vec->look = look;

		vec->cap = cap;
	} else
		kfree(look);
	write_unlock_bh(&vec->lock);

	return 0;
}

int ntrdma_kvec_init(struct ntrdma_kvec *vec, u32 cap, u32 num_reserved_keys,
		int node)
{
	u32 i;

	if (cap == 0)
		return -EINVAL;

	if (cap <= num_reserved_keys)
		return -EINVAL;

	if (cap >= MAX_KVEC_CAP)
		return -ENOMEM;

	vec->keys = kzalloc_node(BITS_TO_LONGS_SIZE(cap), GFP_KERNEL, node);
	if (!vec->keys)
		return -ENOMEM;

	vec->look = kzalloc_node(cap * sizeof(*vec->look), GFP_KERNEL, node);
	if (!vec->look) {
		kfree(vec->keys);
		return -ENOMEM;
	}

	for (i = 0; i < num_reserved_keys; i++)
		__set_bit(i, vec->keys);

	vec->cap = cap;
	vec->num_reserved_keys = num_reserved_keys;
	vec->next_key = num_reserved_keys;

	rwlock_init(&vec->lock);

	return 0;
}

void ntrdma_kvec_deinit(struct ntrdma_kvec *vec)
{
	kfree(vec->look);
	kfree(vec->keys);
}

static int ntrdma_kvec_resize_larger(struct ntrdma_kvec *vec, u32 cap, int node)
{
	unsigned long *keys;
	void **look;

	if (cap == 0)
		return -EINVAL;

	if (cap >= MAX_KVEC_CAP)
		return -ENOMEM;

	keys = kzalloc_node(BITS_TO_LONGS_SIZE(cap), GFP_KERNEL, node);
	if (!keys)
		return -ENOMEM;

	look = kzalloc_node(cap * sizeof(*look), GFP_KERNEL, node);
	if (!look) {
		kfree(keys);
		return -ENOMEM;
	}

	write_lock_bh(&vec->lock);
	if (cap > vec->cap) {
		memcpy(keys, vec->keys, BITS_TO_LONGS_SIZE(vec->cap));
		kfree(vec->keys);
		vec->keys = keys;

		memcpy(look, vec->look, vec->cap * sizeof(*vec->look));
		kfree(vec->look);
		vec->look = look;

		vec->cap = cap;
	} else {
		kfree(keys);
		kfree(look);
	}
	write_unlock_bh(&vec->lock);

	return 0;
}

int ntrdma_kvec_reserve_key(struct ntrdma_kvec *vec, int node)
{
	u32 key;
	int rc;

 again:
	write_lock_bh(&vec->lock);

	key = find_next_zero_bit(vec->keys, vec->cap, vec->next_key);
	if (key == vec->cap) {
		write_unlock_bh(&vec->lock);
		rc = ntrdma_kvec_resize_larger(vec, key << 1, node);
		if (rc < 0)
			return rc;
		goto again;
	}

	__set_bit(key, vec->keys);

	vec->next_key = (key + 1 == vec->cap) ? vec->num_reserved_keys : key + 1;

	write_unlock_bh(&vec->lock);

	return key;
}
