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

int ntrdma_vec_init(struct ntrdma_vec *vec, int cap, int node)
{
	vec->look = kzalloc_node(cap * sizeof(*vec->look),
				 GFP_KERNEL, node);
	if (!vec->look)
		goto err_look;

	vec->cap = cap;

	spin_lock_init(&vec->lock);

	return 0;

	//kfree(vec->look);
err_look:
	return -ENOMEM;
}

void ntrdma_vec_deinit(struct ntrdma_vec *vec)
{
	kfree(vec->look);
}

void *ntrdma_vec_look(struct ntrdma_vec *vec, u32 key)
{
	if (key < vec->cap)
		return vec->look[key];

	return NULL;
}

void ntrdma_vec_set(struct ntrdma_vec *vec, u32 key, void *elem)
{
	vec->look[key] = elem;
}

void ntrdma_vec_lock(struct ntrdma_vec *vec)
{
	spin_lock_bh(&vec->lock);
}

void ntrdma_vec_unlock(struct ntrdma_vec *vec)
{
	spin_unlock_bh(&vec->lock);
}

int ntrdma_vec_resize_larger(struct ntrdma_vec *vec, u32 cap, int node)
{
	struct ntrdma_vec vec_dst, vec_src;
	int rc;

	if (cap <= vec->cap)
		return -EINVAL;

	vec_src = *vec;

	rc = ntrdma_vec_init(&vec_dst, cap, node);
	if (rc)
		return -ENOMEM;

	memcpy(vec_dst.look, vec_src.look,
	       vec_src.cap * sizeof(*vec_src.look));

	*vec = vec_dst;

	ntrdma_vec_deinit(&vec_src);

	return 0;
}

int ntrdma_kvec_init(struct ntrdma_kvec *vec, u32 cap, int node)
{
	vec->keys = kzalloc_node(BITS_TO_LONGS_SIZE(cap),
				 GFP_KERNEL, node);
	if (!vec->keys)
		goto err_keys;

	vec->look = kzalloc_node(cap * sizeof(*vec->look),
				 GFP_KERNEL, node);
	if (!vec->look)
		goto err_look;

	vec->cap = cap;
	vec->next_key = 0;

	spin_lock_init(&vec->lock);

	return 0;

	//kfree(vec->look);
err_look:
	kfree(vec->keys);
err_keys:
	return -ENOMEM;
}

void ntrdma_kvec_deinit(struct ntrdma_kvec *vec)
{
	kfree(vec->look);
	kfree(vec->keys);
}

u32 ntrdma_kvec_reserve_key(struct ntrdma_kvec *vec, int node)
{
	u32 key;

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

void ntrdma_kvec_dispose_key(struct ntrdma_kvec *vec, u32 key)
{
	ntrdma_bits_clear(vec->keys, key);

	if (key < vec->next_key)
		vec->next_key = key;
}

void *ntrdma_kvec_look(struct ntrdma_kvec *vec, u32 key)
{
	if (key < vec->cap)
		return vec->look[key];

	return NULL;
}

void ntrdma_kvec_set(struct ntrdma_kvec *vec, u32 key, void *elem)
{
	vec->look[key] = elem;
}

void ntrdma_kvec_lock(struct ntrdma_kvec *vec)
{
	spin_lock_bh(&vec->lock);
}

void ntrdma_kvec_unlock(struct ntrdma_kvec *vec)
{
	spin_unlock_bh(&vec->lock);
}

int ntrdma_kvec_resize_larger(struct ntrdma_kvec *vec, u32 cap, int node)
{
	struct ntrdma_kvec vec_dst, vec_src;
	int rc;

	if (cap <= vec->cap)
		return -EINVAL;

	vec_src = *vec;

	rc = ntrdma_kvec_init(&vec_dst, cap, node);
	if (rc)
		goto err_dst;

	memcpy(vec_dst.keys, vec_src.keys,
	       BITS_TO_LONGS_SIZE(vec_src.cap));
	memcpy(vec_dst.look, vec_src.look,
	       vec_src.cap * sizeof(*vec_src.look));

	*vec = vec_dst;

	ntrdma_kvec_deinit(&vec_src);

	return 0;

err_dst:
	return -ENOMEM;
}

