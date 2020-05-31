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
	struct ntrdma_rcu_vec *rvec;

	if (cap == 0)
		return -EINVAL;

	if (cap >= MAX_VEC_CAP)
		return -ENOMEM;
	rvec = kzalloc_node(sizeof(*vec->rvec), GFP_KERNEL, node);
	if (!rvec) {
		pr_err("Failed to alloc rvec, size %zd\n", sizeof(*vec->rvec));
		vec->rvec = NULL;
		return -ENOMEM;
	}

	rvec->look = kzalloc_node(cap * sizeof(*vec->rvec->look),
			GFP_KERNEL, node);
	if (!rvec->look) {
		pr_err("Failed to alloc rvec->look, cap %d, size %zd\n", cap, sizeof(*vec->rvec->look));
		kfree(rvec);
		vec->rvec = NULL;
		return -ENOMEM;
	}

	rvec->cap = cap;
	rcu_assign_pointer(vec->rvec, rvec);

	mutex_init(&vec->lock);

	return 0;
}

void ntrdma_vec_deinit(struct ntrdma_vec *vec)
{
	rcu_barrier();
	kfree(vec->rvec->look);
	kfree(vec->rvec);
}

int ntrdma_vec_copy_assign(struct ntrdma_vec *vec, u32 cap, int node, u32 key,
		void *val)
{
	struct ntrdma_rcu_vec *rvec;

	if (cap == 0)
		return -EINVAL;

	if (cap >= MAX_VEC_CAP)
		return -ENOMEM;

	rvec = kzalloc_node(sizeof(*vec->rvec), GFP_KERNEL, node);
	if (!rvec) {
		return -ENOMEM;
	}

	rvec->look = kzalloc_node(cap * sizeof(*rvec->look), GFP_KERNEL, node);

	if (!rvec->look) {
		kfree(rvec);
		return -ENOMEM;
	}

	memcpy(rvec->look, vec->rvec->look,
			vec->rvec->cap * sizeof(*vec->rvec->look));
	rvec->look[key] = val;
	rvec->cap = cap;

	rcu_assign_pointer(vec->rvec, rvec);

	return 0;
}

int ntrdma_kvec_init(struct ntrdma_kvec *vec, u32 cap, u32 num_reserved_keys,
		int node)
{
	struct ntrdma_rcu_kvec *rkvec;
	u32 i;

	if (cap == 0)
		return -EINVAL;

	if (cap <= num_reserved_keys)
		return -EINVAL;

	if (cap >= MAX_KVEC_CAP)
		return -ENOMEM;

	rkvec = kzalloc_node(sizeof(*vec->rkvec), GFP_KERNEL, node);
	if (!rkvec) {
		pr_err("Failed to alloc rkvec, size %zd\n", sizeof(*vec->rkvec));
		vec->rkvec = NULL;
		return -ENOMEM;
	}

	rkvec->keys = kzalloc_node(BITS_TO_LONGS_SIZE(cap), GFP_KERNEL, node);
	if (!rkvec->keys) {
		pr_err("Failed to alloc keys, size %zd\n", BITS_TO_LONGS_SIZE(cap));
		kfree(rkvec);
		vec->rkvec = NULL;
		return -ENOMEM;
	}

	rkvec->look = kzalloc_node(cap * sizeof(*rkvec->look), GFP_KERNEL, node);
	if (!rkvec->look) {
		pr_err("Failed to alloc look, size %zd\n", cap * sizeof(*rkvec->look));
		kfree(rkvec->keys);
		kfree(rkvec);
		vec->rkvec = NULL;
		return -ENOMEM;
	}

	for (i = 0; i < num_reserved_keys; i++)
		__set_bit(i, rkvec->keys);

	rkvec->cap = cap;
	rkvec->num_reserved_keys = num_reserved_keys;
	rkvec->next_key = num_reserved_keys;

	rcu_assign_pointer(vec->rkvec, rkvec);
	mutex_lock(&vec->lock);

	return 0;
}

void ntrdma_kvec_deinit(struct ntrdma_kvec *vec)
{
	rcu_barrier();
	kfree(vec->rkvec->look);
	kfree(vec->rkvec->keys);
	kfree(vec->rkvec);
}

int ntrdma_kvec_new_copy(struct ntrdma_kvec *vec, u32 cap,
		int node, struct ntrdma_rcu_kvec **ret_vec)
{
	struct ntrdma_rcu_kvec *rkvec;

	if (cap == 0)
		return -EINVAL;

	if (cap >= MAX_KVEC_CAP)
		return -ENOMEM;

	rkvec = kzalloc_node(sizeof(*vec->rkvec), GFP_KERNEL, node);
	if (!rkvec) {
		pr_err("Failed to alloc rkvec, size %zd\n", sizeof(*vec->rkvec));
		return -ENOMEM;
	}

	rkvec->keys = kzalloc_node(BITS_TO_LONGS_SIZE(cap), GFP_KERNEL, node);
	if (!rkvec->keys) {
		pr_err("Failed to alloc keys, size %zd\n", BITS_TO_LONGS_SIZE(cap));
		kfree(rkvec);
		return -ENOMEM;
	}

	rkvec->look = kzalloc_node(cap * sizeof(*rkvec->look), GFP_KERNEL, node);
	if (!rkvec->look) {
		pr_err("Failed to alloc look, size %zd\n", cap * sizeof(*rkvec->look));
		kfree(rkvec->keys);
		kfree(rkvec);
		return -ENOMEM;
	}

	memcpy(rkvec->keys, vec->rkvec->keys, BITS_TO_LONGS_SIZE(rkvec->cap));
	memcpy(rkvec->look, vec->rkvec->look, vec->rkvec->cap * sizeof(*rkvec->look));
	rkvec->cap = cap;
	rkvec->next_key = vec->rkvec->next_key;
	rkvec->num_reserved_keys = vec->rkvec->num_reserved_keys;

	*ret_vec = rkvec;
	return 0;
}

int ntrdma_kvec_reserve_key(struct ntrdma_kvec *vec, int node)
{
	u32 key;
	u32 cap;
	int rc;
	struct ntrdma_rcu_kvec *old_rkvec;
	struct ntrdma_rcu_kvec *new_rkvec;

	mutex_lock(&vec->lock);

	old_rkvec = vec->rkvec;
	key = find_next_zero_bit(old_rkvec->keys, old_rkvec->cap,
			old_rkvec->next_key);
	cap = old_rkvec->cap;
	if (key >= old_rkvec->cap) {
		cap = key << 1;
	}

	rc = ntrdma_kvec_new_copy(vec, cap, node, &new_rkvec);
	if (rc < 0) {
		key = rc;
		goto out;
	}

	__set_bit(key, new_rkvec->keys);
	new_rkvec->next_key = (key + 1 == new_rkvec->cap) ? new_rkvec->num_reserved_keys : key + 1;

	rcu_assign_pointer(vec->rkvec, new_rkvec);
	call_rcu(&old_rkvec->rcu, remove_rcu_kvec);
out:
	mutex_unlock(&vec->lock);

	return key;
}
