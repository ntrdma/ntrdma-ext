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

#include "ntrdma_dev.h"
#include "ntrdma_cq.h"
#include "ntrdma_qp.h"

static void ntrdma_cq_cue_work(unsigned long ptrhld);
static void ntrdma_cq_vbell_cb(void *ctx);

DECLARE_PER_CPU(struct ntrdma_dev_counters, dev_cnt);

int ntrdma_cq_init(struct ntrdma_cq *cq, struct ntrdma_dev *dev, int vbell_idx)
{
	int rc;

	rc = ntrdma_obj_init(&cq->obj, dev);
	if (rc)
		goto err_obj;

	cq->arm = 0;
	cq->need_cue = false;
	spin_lock_init(&cq->arm_lock);

	INIT_LIST_HEAD(&cq->poll_list);
	mutex_init(&cq->poll_lock);

	tasklet_init(&cq->cue_work,
		     ntrdma_cq_cue_work,
		     to_ptrhld(cq));
	ntrdma_vbell_init(&cq->vbell, ntrdma_cq_vbell_cb, cq);
	cq->vbell_idx = vbell_idx;

	return 0;

err_obj:
	return rc;
}

void ntrdma_cq_deinit(struct ntrdma_cq *cq)
{
	ntrdma_obj_deinit(&cq->obj);
}

int ntrdma_cq_add(struct ntrdma_cq *cq)
{
	struct ntrdma_dev *dev = ntrdma_cq_dev(cq);

	ntrdma_debugfs_cq_add(cq);
	ntrdma_cq_get(cq);

	mutex_lock(&dev->res_lock);
	{
		list_add_tail(&cq->obj.dev_entry, &dev->cq_list);
	}
	mutex_unlock(&dev->res_lock);

	return 0;
}

void ntrdma_cq_del(struct ntrdma_cq *cq)
{
	struct ntrdma_dev *dev = ntrdma_cq_dev(cq);

	spin_lock_bh(&cq->arm_lock);
	{
		cq->arm = 0;
		cq->need_cue = false;
		ntrdma_dev_vbell_del(dev, &cq->vbell);
	}
	spin_unlock_bh(&cq->arm_lock);

	tasklet_kill(&cq->cue_work);

	mutex_lock(&dev->res_lock);
	{
		list_del(&cq->obj.dev_entry);
	}
	mutex_unlock(&dev->res_lock);

	ntrdma_cq_put(cq);
	ntrdma_cq_repo(cq);
	ntrdma_debugfs_cq_del(cq);
}

void ntrdma_cq_arm(struct ntrdma_cq *cq)
{
	struct ntrdma_dev *dev = ntrdma_cq_dev(cq);
	bool need_cue;
	unsigned int arm;

	this_cpu_inc(dev_cnt.cqes_armed);

	spin_lock_bh(&cq->arm_lock);

	ntrdma_dev_vbell_add_clear(dev, &cq->vbell, cq->vbell_idx);
	++cq->arm;
	arm = cq->arm;
	need_cue = cq->need_cue;
	cq->need_cue = false;

	spin_unlock_bh(&cq->arm_lock);

	TRACE("cq %p arm %d need cue %d vbell idx %d\n",
			cq, arm, need_cue, cq->vbell_idx);

	if (need_cue)
		ntrdma_cq_cue(cq);
}

void _ntrdma_cq_cue(struct ntrdma_cq *cq, const char *f)
{
	unsigned int arm;

	spin_lock_bh(&cq->arm_lock);
	arm = cq->arm;

	if (!cq->arm)
		cq->need_cue = true;

	for (; cq->arm; --cq->arm) {
		/*TODO: do we realy need to run this in spinlock???*/
		cq->ibcq.comp_handler(&cq->ibcq, cq->ibcq.cq_context);
	}
	spin_unlock_bh(&cq->arm_lock);

	this_cpu_add(dev_cnt.cqes_notified, arm);
	TRACE("cq %p arm %d %s\n", cq, arm, f);
}

void ntrdma_cq_add_poll(struct ntrdma_cq *cq, struct ntrdma_poll *poll)
{
	mutex_lock(&cq->poll_lock);
	{
		list_add_tail(&poll->cq_entry, &cq->poll_list);
	}
	mutex_unlock(&cq->poll_lock);
}

void ntrdma_cq_del_poll(struct ntrdma_cq *cq, struct ntrdma_poll *poll)
{
	mutex_lock(&cq->poll_lock);
	{
		list_del(&poll->cq_entry);
	}
	mutex_unlock(&cq->poll_lock);
}

void ntrdma_cq_cmpl_start(struct ntrdma_cq *cq)
{
	mutex_lock(&cq->poll_lock);
}

void ntrdma_cq_cmpl_done(struct ntrdma_cq *cq)
{
	mutex_unlock(&cq->poll_lock);
}

int ntrdma_cq_cmpl_get(struct ntrdma_cq *cq, struct ntrdma_qp **qp,
		       u32 *pos, u32 *end, u32 *base)
{
	struct ntrdma_poll *poll;
	int rc = -EAGAIN;

	list_for_each_entry(poll, &cq->poll_list, cq_entry) {
		rc = poll->poll_start_and_get(poll, qp, pos, end, base);
		if (rc == -EAGAIN && qp && *qp) {
			if (rc == -EAGAIN)
				continue;
		}

		/* move the head to after the entry (rotate the list) */
		list_move(&cq->poll_list, &poll->cq_entry);
		break;
	}

	return rc;
}

void ntrdma_cq_cmpl_put(struct ntrdma_cq *cq,
			u32 pos, u32 base)
{
	struct ntrdma_poll *poll;

	poll = list_last_entry(&cq->poll_list, struct ntrdma_poll, cq_entry);

	poll->poll_put_and_done(poll, pos, base);
}

void ntrdma_cq_cmpl_cqe(struct ntrdma_cq *cq,
			struct ntrdma_cqe *outcqe, u32 pos)
{
	struct ntrdma_poll *poll;

	poll = list_last_entry(&cq->poll_list, struct ntrdma_poll, cq_entry);

	return poll->poll_cqe(poll, outcqe, pos);
}

static void ntrdma_cq_cue_work(unsigned long ptrhld)
{
	struct ntrdma_cq *cq = of_ptrhld(ptrhld);

	ntrdma_cq_cue(cq);
}

static void ntrdma_cq_vbell_cb(void *ctx)
{
	struct ntrdma_cq *cq = ctx;

	tasklet_schedule(&cq->cue_work);
}
