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

#ifndef NTRDMA_VBELL_H
#define NTRDMA_VBELL_H

#include <linux/list.h>
#include <linux/types.h>

#include "ntrdma_cb.h"

struct ntrdma_dev;

struct ntrdma_vbell {
	struct list_head		entry;
	struct ntrdma_cb		cb;
	u32				seq;
	bool				arm;
};

struct ntrdma_vbell_head {
	struct list_head		list;
	u32				seq;
};

int ntrdma_dev_vbell_init(struct ntrdma_dev *dev,
			  u32 vbell_count,
			  u32 vbell_start);

void ntrdma_dev_vbell_deinit(struct ntrdma_dev *dev);

u32 ntrdma_dev_vbell_next(struct ntrdma_dev *dev);

int ntrdma_dev_vbell_enable(struct ntrdma_dev *dev,
			const struct ntc_remote_buf_desc *vbell_ntc_buf_desc,
			u32 peer_vbell_count);

void ntrdma_dev_vbell_disable(struct ntrdma_dev *dev);

void ntrdma_dev_vbell_event(struct ntrdma_dev *dev, int vec);

void ntrdma_dev_vbell_del(struct ntrdma_dev *dev,
			  struct ntrdma_vbell *vbell);

void ntrdma_dev_vbell_clear(struct ntrdma_dev *dev,
			    struct ntrdma_vbell *vbell,
			    u32 idx);

int ntrdma_dev_vbell_add(struct ntrdma_dev *dev,
			 struct ntrdma_vbell *vbell,
			 u32 idx);

int ntrdma_dev_vbell_add_clear(struct ntrdma_dev *dev,
			       struct ntrdma_vbell *vbell,
			       u32 idx);

void ntrdma_dev_vbell_peer(struct ntrdma_dev *dev,
			   void *req, u32 idx);

static inline void ntrdma_vbell_init(struct ntrdma_vbell *vbell,
				     void (*cb_fn)(void *cb_ctx),
				     void *cb_ctx)
{
	ntrdma_cb_init(&vbell->cb, cb_fn, cb_ctx);
	vbell->seq = ~0;
	vbell->arm = false;
}

static inline void ntrdma_vbell_del(struct ntrdma_vbell *vbell)
{
	if (!vbell->arm)
		return;

	list_del(&vbell->entry);
	vbell->arm = false;
}

static inline void ntrdma_vbell_clear(struct ntrdma_vbell_head *head,
				      struct ntrdma_vbell *vbell)
{
	vbell->seq = head->seq;
}

static inline int ntrdma_vbell_add(struct ntrdma_vbell_head *head,
				   struct ntrdma_vbell *vbell)
{
	if (vbell->arm)
		return 0;

	if (vbell->seq != head->seq) {
		ntrdma_vbell_clear(head, vbell);
		return -EAGAIN;
	}

	list_add_tail(&vbell->entry, &head->list);
	vbell->arm = true;

	return 0;
}

static inline int ntrdma_vbell_add_clear(struct ntrdma_vbell_head *head,
					 struct ntrdma_vbell *vbell)
{
	if (vbell->arm)
		return 0;

	ntrdma_vbell_clear(head, vbell);
	list_add_tail(&vbell->entry, &head->list);
	vbell->arm = true;

	return 0;
}

static inline void ntrdma_vbell_fire(struct ntrdma_vbell *vbell)
{
	vbell->arm = false;
	ntrdma_cb_call(&vbell->cb);
}

static inline void ntrdma_vbell_head_init(struct ntrdma_vbell_head *head)
{
	INIT_LIST_HEAD(&head->list);
	head->seq = 0;
}

static inline void ntrdma_vbell_head_fire(struct ntrdma_vbell_head *head)
{
	struct ntrdma_vbell *vbell;

	list_for_each_entry(vbell, &head->list, entry)
		ntrdma_vbell_fire(vbell);

	INIT_LIST_HEAD(&head->list);
}

static inline void ntrdma_vbell_head_event(struct ntrdma_vbell_head *head)
{
	++head->seq;
	ntrdma_vbell_head_fire(head);
}

static inline void ntrdma_vbell_head_reset(struct ntrdma_vbell_head *head)
{
	head->seq = 0;
	ntrdma_vbell_head_fire(head);
}

#endif
