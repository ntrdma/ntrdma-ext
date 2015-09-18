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

#include "ntrdma_os.h"
#include "ntrdma_cb.h"

#define NTRDMA_VBELL_NEXT_START		0x8

struct ntrdma_dev;
struct ntrdma_req;

struct ntrdma_vbell {
	NTRDMA_DECL_LIST_ENTRY		(entry);
	struct ntrdma_cb		cb;
	ntrdma_u32_t			seq;
	ntrdma_bool_t			arm;
};

struct ntrdma_vbell_head {
	NTRDMA_DECL_LIST_HEAD		(list);
	ntrdma_u32_t			seq;
};

int ntrdma_dev_vbell_init(struct ntrdma_dev *dev,
			  ntrdma_u32_t vbell_count,
			  ntrdma_u32_t vbell_start);

void ntrdma_dev_vbell_deinit(struct ntrdma_dev *dev);

ntrdma_u32_t ntrdma_dev_vbell_next(struct ntrdma_dev *dev);

int ntrdma_dev_vbell_enable(struct ntrdma_dev *dev,
			    ntrdma_dma_addr_t peer_vbell_buf_dma,
			    ntrdma_u32_t peer_vbell_count);

void ntrdma_dev_vbell_disable(struct ntrdma_dev *dev);

void ntrdma_dev_vbell_event(struct ntrdma_dev *dev);

void ntrdma_dev_vbell_del(struct ntrdma_dev *dev,
			  struct ntrdma_vbell *vbell,
			  ntrdma_u32_t idx);

void ntrdma_dev_vbell_clear(struct ntrdma_dev *dev,
			    struct ntrdma_vbell *vbell,
			    ntrdma_u32_t idx);

int ntrdma_dev_vbell_add(struct ntrdma_dev *dev,
			 struct ntrdma_vbell *vbell,
			 ntrdma_u32_t idx);

int ntrdma_dev_vbell_add_clear(struct ntrdma_dev *dev,
			       struct ntrdma_vbell *vbell,
			       ntrdma_u32_t idx);

void ntrdma_dev_vbell_peer(struct ntrdma_dev *dev,
			   struct ntrdma_req *req, ntrdma_u32_t idx);

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

	ntrdma_list_del(&vbell->entry);
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

	ntrdma_list_add_tail(&head->list, &vbell->entry);
	vbell->arm = true;

	return 0;
}

static inline int ntrdma_vbell_add_clear(struct ntrdma_vbell_head *head,
					 struct ntrdma_vbell *vbell)
{
	if (vbell->arm)
		return 0;

	ntrdma_vbell_clear(head, vbell);
	ntrdma_list_add_tail(&head->list, &vbell->entry);
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
	ntrdma_list_init(&head->list);
	head->seq = 0;
}

static inline void ntrdma_vbell_head_fire(struct ntrdma_vbell_head *head)
{
	struct ntrdma_vbell *vbell;

	ntrdma_list_for_each_entry(vbell, &head->list,
				   struct ntrdma_vbell, entry)
		ntrdma_vbell_fire(vbell);

	ntrdma_list_init(&head->list);
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
