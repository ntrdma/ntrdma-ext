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

#ifndef NTRDMA_CQ_H
#define NTRDMA_CQ_H

#include <linux/list.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include <rdma/ib_verbs.h>

#include "ntrdma_obj.h"
#include "ntrdma_vbell.h"

/* Completion Queue */
struct ntrdma_cq {
	/* Ofed cq structure */
	struct ib_cq			ibcq;

	/* debugfs */
	struct dentry			*debug;

	/* Ntrdma cq is a local-only object */
	struct ntrdma_obj		obj;

	/* number of application requests to be notified */
	unsigned int			arm;

	/* synchronize arming and cuing the completion queue */
	spinlock_t			arm_lock;

	/* poll entries for polling queue pairs */
	struct list_head		poll_list;

	/* synchronize poll_list modification and polling */
	struct mutex			poll_lock;

	/* work completion notification */
	struct tasklet_struct		cue_work;
	struct ntrdma_vbell		vbell;
	u32				vbell_idx;
	bool				need_cue;
};

#define ntrdma_ib_cq(__ibcq) \
	container_of(__ibcq, struct ntrdma_cq, ibcq)

#define ntrdma_cq_dev(__cq) \
	ntrdma_obj_dev(&(__cq)->obj)

int ntrdma_cq_init(struct ntrdma_cq *cq, struct ntrdma_dev *dev, int vbell_idx);
void ntrdma_cq_deinit(struct ntrdma_cq *cq);
int ntrdma_cq_add(struct ntrdma_cq *cq);
void ntrdma_cq_del(struct ntrdma_cq *cq);

static inline void ntrdma_cq_get(struct ntrdma_cq *cq)
{
	ntrdma_obj_get(&cq->obj);
}

static inline void ntrdma_cq_put(struct ntrdma_cq *cq)
{
	ntrdma_obj_put(&cq->obj);
}

static inline void ntrdma_cq_repo(struct ntrdma_cq *cq)
{
	ntrdma_obj_repo(&cq->obj);
}

void ntrdma_cq_arm(struct ntrdma_cq *cq);
#define ntrdma_cq_cue(__cq) _ntrdma_cq_cue(__cq, __func__)
void _ntrdma_cq_cue(struct ntrdma_cq *cq, const char *f);

void ntrdma_cq_add_poll(struct ntrdma_cq *cq, struct ntrdma_poll *poll);
void ntrdma_cq_del_poll(struct ntrdma_cq *cq, struct ntrdma_poll *poll);

void ntrdma_cq_cmpl_start(struct ntrdma_cq *cq);
void ntrdma_cq_cmpl_done(struct ntrdma_cq *cq);
int ntrdma_cq_cmpl_get(struct ntrdma_cq *cq, struct ntrdma_qp **qp,
		       u32 *pos, u32 *end, u32 *base);
void ntrdma_cq_cmpl_put(struct ntrdma_cq *cq,
			u32 pos, u32 base);
void ntrdma_cq_cmpl_cqe(struct ntrdma_cq *cq,
			struct ntrdma_cqe *outcqe, u32 pos);

#endif
