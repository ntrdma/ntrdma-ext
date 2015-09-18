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
#include "ntrdma_wr.h"

static NTRDMA_DECL_DPC_CB(ntrdma_cq_cue_work, ptrhld);
static void ntrdma_cq_vbell_cb(void *ctx);

int ntrdma_cq_init(struct ntrdma_cq *cq, struct ntrdma_dev *dev, int vbell_idx)
{
	int rc;

	rc = ntrdma_obj_init(&cq->obj, dev);
	if (rc)
		goto err_obj;

	cq->arm = 0;
	ntrdma_spl_create(&cq->arm_lock, "cq_arm_lock");

	ntrdma_list_init(&cq->poll_list);
	ntrdma_mut_create(&cq->poll_lock, "cq_poll_lock");

	ntrdma_dpc_create(&cq->cue_work, "cq_cue_work",
			  ntrdma_cq_cue_work, cq);

	ntrdma_vbell_init(&cq->vbell, ntrdma_cq_vbell_cb, cq);
	cq->vbell_idx = vbell_idx;

	return 0;

err_obj:
	return rc;
}

void ntrdma_cq_deinit(struct ntrdma_cq *cq)
{
	ntrdma_mut_destroy(&cq->poll_lock);
	ntrdma_obj_deinit(&cq->obj);
}

int ntrdma_cq_add(struct ntrdma_cq *cq)
{
	struct ntrdma_dev *dev = ntrdma_cq_dev(cq);

	ntrdma_debugfs_cq_add(cq);
	ntrdma_cq_get(cq);

	ntrdma_mut_lock(&dev->res_lock);
	{
		ntrdma_list_add_tail(&dev->cq_list, &cq->obj.dev_entry);
	}
	ntrdma_mut_unlock(&dev->res_lock);

	return 0;
}

void ntrdma_cq_del(struct ntrdma_cq *cq)
{
	struct ntrdma_dev *dev = ntrdma_cq_dev(cq);

	ntrdma_spl_lock(&cq->arm_lock);
	{
		cq->arm = 0;
		ntrdma_dev_vbell_del(dev, &cq->vbell, cq->vbell_idx);
	}
	ntrdma_spl_unlock(&cq->arm_lock);

	ntrdma_mut_lock(&dev->res_lock);
	{
		ntrdma_list_del(&cq->obj.dev_entry);
	}
	ntrdma_mut_unlock(&dev->res_lock);

	ntrdma_cq_put(cq);
	ntrdma_cq_repo(cq);
	ntrdma_debugfs_cq_del(cq);
}

void ntrdma_cq_arm(struct ntrdma_cq *cq)
{
	struct ntrdma_dev *dev = ntrdma_cq_dev(cq);

	ntrdma_spl_lock(&cq->arm_lock);
	{
		ntrdma_dev_vbell_add_clear(dev, &cq->vbell, cq->vbell_idx);
		++cq->arm;
	}
	ntrdma_spl_unlock(&cq->arm_lock);
}

void ntrdma_cq_cue(struct ntrdma_cq *cq)
{
	ntrdma_spl_lock(&cq->arm_lock);
	{
		for (; cq->arm; --cq->arm)
			cq->ibcq.comp_handler(&cq->ibcq, cq->ibcq.cq_context);
	}
	ntrdma_spl_unlock(&cq->arm_lock);
}

void ntrdma_cq_add_poll(struct ntrdma_cq *cq, struct ntrdma_poll *poll)
{
	ntrdma_mut_lock(&cq->poll_lock);
	{
		ntrdma_list_add_tail(&cq->poll_list, &poll->cq_entry);
	}
	ntrdma_mut_unlock(&cq->poll_lock);
}

void ntrdma_cq_del_poll(struct ntrdma_cq *cq, struct ntrdma_poll *poll)
{
	ntrdma_mut_lock(&cq->poll_lock);
	{
		ntrdma_list_del(&poll->cq_entry);
	}
	ntrdma_mut_unlock(&cq->poll_lock);
}

void ntrdma_cq_cmpl_start(struct ntrdma_cq *cq)
{
	ntrdma_mut_lock(&cq->poll_lock);
}

void ntrdma_cq_cmpl_done(struct ntrdma_cq *cq)
{
	ntrdma_mut_unlock(&cq->poll_lock);
}

int ntrdma_cq_cmpl_get(struct ntrdma_cq *cq, struct ntrdma_qp **qp,
		       ntrdma_u32_t *pos, ntrdma_u32_t *end, ntrdma_u32_t *base)
{
	struct ntrdma_poll *poll;
	int rc = 0;

	ntrdma_list_for_each_entry(poll, &cq->poll_list,
				   struct ntrdma_poll, cq_entry) {
		rc = poll->poll_start_and_get(poll, qp, pos, end, base);

		if (rc == -EAGAIN)
			continue;

		/* rotate the active poll to the last position */
		ntrdma_list_rotate_tail(&cq->poll_list, &poll->cq_entry);

		if (rc)
			break;

		/* leave spl locked on success */
		return 0;
	}

	return rc;
}

void ntrdma_cq_cmpl_put(struct ntrdma_cq *cq,
			ntrdma_u32_t pos, ntrdma_u32_t base)
{
	struct ntrdma_poll *poll;

	/* TODO: ok to use fast last entry here (guaranteed nonempty list) */
	poll = ntrdma_list_last_entry(&cq->poll_list,
				      struct ntrdma_poll, cq_entry);

	poll->poll_put_and_done(poll, pos, base);
}

struct ntrdma_cqe *ntrdma_cq_cmpl_cqe(struct ntrdma_cq *cq,
				      struct ntrdma_cqe *abort_cqe, ntrdma_u32_t pos)
{
	struct ntrdma_poll *poll;

	/* TODO: ok to use fast last entry here (guaranteed nonempty list) */
	poll = ntrdma_list_last_entry(&cq->poll_list,
				      struct ntrdma_poll, cq_entry);

	return poll->poll_cqe(poll, abort_cqe, pos);
}

static NTRDMA_DECL_DPC_CB(ntrdma_cq_cue_work, ptrhld)
{
	struct ntrdma_cq *cq = NTRDMA_CAST_DPC_CTX(ptrhld);

	ntrdma_cq_cue(cq);
}

static void ntrdma_cq_vbell_cb(void *ctx)
{
	struct ntrdma_cq *cq = ctx;

	ntrdma_dpc_fire(&cq->cue_work);
}

