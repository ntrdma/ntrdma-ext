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

struct ntrdma_dev;

struct ntrdma_vbell {
	struct ntrdma_dev	*dev;
	u32			idx;    /* Index in the device. */
	struct list_head	entry;  /* Protected by dev->vbell_self_lock. */
	void	(*cb_fn)(void *cb_ctx); /* Called under dev->vbell_self_lock. */
	void			*cb_ctx;
	u32			seq;    /* Protected by dev->vbell_self_lock. */
	bool			arm;    /* Protected by dev->vbell_self_lock. */
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

void ntrdma_dev_vbell_peer(struct ntrdma_dev *dev,
			struct ntc_dma_chan *chan, u32 idx);

static inline void ntrdma_vbell_init(struct ntrdma_dev *dev,
				struct ntrdma_vbell *vbell, u32 idx,
				void (*cb_fn)(void *cb_ctx), void *cb_ctx)
{
	vbell->dev = dev;
	vbell->idx = idx;
	vbell->cb_fn = cb_fn;
	vbell->cb_ctx = cb_ctx;
	vbell->seq = ~0;
	vbell->arm = false;
}

static inline void ntrdma_vbell_head_init(struct ntrdma_vbell_head *head)
{
	INIT_LIST_HEAD(&head->list);
	head->seq = 0;
}

/*
 * Must run under dev->vbell_self_lock.
 */
static inline void ntrdma_vbell_head_fire(struct ntrdma_vbell_head *head)
{
	struct ntrdma_vbell *vbell;

	list_for_each_entry(vbell, &head->list, entry) {
		vbell->arm = false;
		vbell->cb_fn(vbell->cb_ctx);
	}

	INIT_LIST_HEAD(&head->list);
}

/*
 * Must run under dev->vbell_self_lock.
 */
static inline void ntrdma_vbell_head_event(struct ntrdma_vbell_head *head)
{
	++head->seq;
	ntrdma_vbell_head_fire(head);
}

/*
 * Must run under dev->vbell_self_lock.
 */
static inline void ntrdma_vbell_head_reset(struct ntrdma_vbell_head *head)
{
	head->seq = 0;
	ntrdma_vbell_head_fire(head);
}

#endif
