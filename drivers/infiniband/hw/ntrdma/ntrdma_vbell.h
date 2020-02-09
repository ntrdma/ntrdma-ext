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

#include <linux/workqueue.h>
#include <linux/netdevice.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/types.h>

struct ntrdma_dev;

struct ntrdma_vbell {
	struct ntrdma_dev	*dev;
	u32			idx;    /* Index in the device. */
	struct list_head	entry;  /* Protected by head->lock. */
	void	(*cb_fn)(void *cb_ctx); /* Called under head->lock. */
	void			*cb_ctx;
	u32			seq;    /* Protected by head->lock. */
	bool			arm;    /* Protected by head->lock. */
	bool			enabled;/* Protected by head->lock. */
	bool 			alive;	/* Protected by head->lock. */
};

struct ntrdma_vbell_head {
	spinlock_t			lock;
	struct list_head		list;	/* Protected by lock. */
	u32				seq;	/* Write protected by lock. */
	bool 				enabled;/* Protected by lock. */
};

struct ntrdma_peer_vbell {
	spinlock_t			lock;
	u32				seq;	/* Protected by lock. */
	bool 				enabled;/* Protected by lock. */
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

int ntrdma_dev_vbell_peer(struct ntrdma_dev *dev,
			struct ntc_dma_chan *chan, u32 idx);
int ntrdma_dev_vbell_peer_direct(struct ntrdma_dev *dev, u32 idx);

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
	vbell->enabled = true;
	vbell->alive = true;
}

void ntrdma_tasklet_vbell_init(struct ntrdma_dev *dev,
			struct ntrdma_vbell *vbell, u32 idx,
			struct tasklet_struct *tasklet);

void ntrdma_work_vbell_init(struct ntrdma_dev *dev,
			struct ntrdma_vbell *vbell, u32 idx,
			struct work_struct *work);

void ntrdma_napi_vbell_init(struct ntrdma_dev *dev,
			struct ntrdma_vbell *vbell, u32 idx,
			struct napi_struct *napi);

void ntrdma_tasklet_vbell_kill(struct ntrdma_vbell *vbell);
void ntrdma_work_vbell_flush(struct ntrdma_vbell *vbell);
void ntrdma_work_vbell_kill(struct ntrdma_vbell *vbell);
void ntrdma_napi_vbell_kill(struct ntrdma_vbell *vbell);

static inline void ntrdma_vbell_head_init(struct ntrdma_vbell_head *head)
{
	INIT_LIST_HEAD(&head->list);
	head->seq = 0;
	spin_lock_init(&head->lock);
	head->enabled = 0;
}

static inline void ntrdma_peer_vbell_init(struct ntrdma_peer_vbell *peer_vbell)
{
	peer_vbell->seq = 0;
	spin_lock_init(&peer_vbell->lock);
	peer_vbell->enabled = 0;
}

static inline void ntrdma_peer_vbell_enable(struct ntrdma_peer_vbell *peer_vbell)
{
	spin_lock_bh(&peer_vbell->lock);
	peer_vbell->enabled = 1;
	spin_unlock_bh(&peer_vbell->lock);
}

static inline void ntrdma_peer_vbell_disable(struct ntrdma_peer_vbell *peer_vbell)
{
	spin_lock_bh(&peer_vbell->lock);
	peer_vbell->enabled = 0;
	spin_unlock_bh(&peer_vbell->lock);
}

#endif
