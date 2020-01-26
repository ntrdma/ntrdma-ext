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

#ifndef NTRDMA_RES_H
#define NTRDMA_RES_H

#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/poison.h>

#include "ntrdma_obj.h"
#include "ntrdma_dev.h"

#define NTRDMA_RES_VEC_INIT_CAP		512
#define NTRDMA_QP_VEC_PREALLOCATED	2
#define NTRDMA_MR_VEC_PREALLOCATED	0

struct ntrdma_dev;
struct ntrdma_cmd_cb;
struct ntrdma_vec;
struct ntrdma_kvec;

struct ntrdma_res *ntrdma_res_look(struct ntrdma_kvec *vec, u32 key);

void ntrdma_dev_rres_reset(struct ntrdma_dev *dev);

struct ntrdma_rres *ntrdma_rres_look(struct ntrdma_vec *vec, u32 key);

/* Local rdma resource */
struct ntrdma_res {
	/* The resource is an ntrdma object */
	struct ntrdma_obj		obj;

	/* Initiate commands to create the remote resource */
	void (*enable)(struct ntrdma_res *res,
		struct ntrdma_cmd_cb *cb);
	/* Initiate commands to delete the remote resource */
	void (*disable)(struct ntrdma_res *res,
			struct ntrdma_cmd_cb *cb);

	/* The key identifies this resource */
	u32				key;

	/* Synchronize operations affecting the local resource */
	struct mutex			lock;
	unsigned long			timeout;
};

#define ntrdma_res_dev(res) ntrdma_obj_dev(&(res)->obj)

void ntrdma_res_init(struct ntrdma_res *res,
		struct ntrdma_dev *dev,
		void (*enable)(struct ntrdma_res *res,
			struct ntrdma_cmd_cb *cb),
		void (*disable)(struct ntrdma_res *res,
				struct ntrdma_cmd_cb *cb));

int ntrdma_res_add(struct ntrdma_res *res, struct ntrdma_cmd_cb *cb,
		struct list_head *res_list, struct ntrdma_kvec *res_vec);
void ntrdma_res_del(struct ntrdma_res *res, struct ntrdma_cmd_cb *cb,
		struct ntrdma_kvec *res_vec);

static inline void ntrdma_res_lock(struct ntrdma_res *res)
{
	mutex_lock(&res->lock);
}

static inline void ntrdma_res_unlock(struct ntrdma_res *res)
{
	mutex_unlock(&res->lock);
}

inline int ntrdma_res_wait_cmds(struct ntrdma_dev *dev,
				struct ntrdma_cmd_cb *cb,
				unsigned long timeout);

static inline void ntrdma_res_get(struct ntrdma_res *res)
{
	ntrdma_obj_get(&res->obj);
}

void ntrdma_res_put(struct ntrdma_res *res,
		void (*obj_release)(struct kref *kref));

/* Remote rdma resource */
struct ntrdma_rres {
	/* The resource is an ntrdma object */
	struct ntrdma_obj	obj; /* Entry protected by dev->rres_lock. */

	bool			in_rres_list; /* Protected by dev->rres_lock. */

	/* The vector to which this remote resource is added */
	struct ntrdma_vec		*vec;

	/* Free this remote resource on reset */
	void (*free)(struct ntrdma_rres *rres);

	/* The key identifies this remote resource */
	u32				key;
};

#define ntrdma_rres_dev(rres) ntrdma_obj_dev(&(rres)->obj)

void ntrdma_rres_init(struct ntrdma_rres *rres,
		     struct ntrdma_dev *dev,
		     struct ntrdma_vec *vec,
		     void (*free)(struct ntrdma_rres *rres),
		     u32 key);

int ntrdma_rres_add(struct ntrdma_rres *rres);
void ntrdma_rres_remove(struct ntrdma_rres *rres);
void ntrdma_rres_remove_unsafe(struct ntrdma_rres *rres);

static inline void ntrdma_rres_get(struct ntrdma_rres *rres)
{
	ntrdma_obj_get(&rres->obj);
}

static inline void ntrdma_rres_put(struct ntrdma_rres *rres,
		void (*release)(struct kref *))
{
	ntrdma_obj_put(&rres->obj, release);
}

static inline bool ntrdma_list_is_entry_poisoned(struct list_head *entry)
{
	if (entry->next == LIST_POISON1 && entry->prev == LIST_POISON2)
		return true;
	return false;
}

#endif
