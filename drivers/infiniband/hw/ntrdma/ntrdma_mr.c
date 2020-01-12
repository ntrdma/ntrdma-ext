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
#include "ntrdma_cmd.h"
#include "ntrdma_pd.h"
#include "ntrdma_mr.h"
#include "ntrdma_res.h"

#define ntrdma_cmd_cb_mrcb(__cb) \
	container_of(__cb, struct ntrdma_mr_cmd_cb, cb)

static int ntrdma_mr_enable_prep(struct ntrdma_cmd_cb *cb,
				union ntrdma_cmd *cmd);
static int ntrdma_mr_enable_cmpl(struct ntrdma_cmd_cb *cb,
				const union ntrdma_rsp *rsp);
static int ntrdma_mr_disable_prep(struct ntrdma_cmd_cb *cb,
				union ntrdma_cmd *cmd);
static int ntrdma_mr_disable_cmpl(struct ntrdma_cmd_cb *cb,
				const union ntrdma_rsp *rsp);

static void ntrdma_mr_enable_cb(struct ntrdma_res *res,
				struct ntrdma_cmd_cb *cb);
static void ntrdma_mr_disable_cb(struct ntrdma_res *res,
				struct ntrdma_cmd_cb *cb);

static void ntrdma_rmr_free(struct ntrdma_rres *rres);

int ntrdma_mr_init(struct ntrdma_mr *mr, struct ntrdma_dev *dev)
{
	int count;
	int rc;

	if (mr->ib_umem) {
		count = ntc_umem_sgl(dev->ntc, mr->ib_umem,
				mr->sg_list, mr->sg_count, mr->access);
		if (count != mr->sg_count) {
			rc = -EFAULT;
			goto err;
		}
	} else
		count = mr->sg_count;

	ntrdma_res_init(&mr->res, dev,
			ntrdma_mr_enable_cb, ntrdma_mr_disable_cb);

	rc = ntrdma_kvec_reserve_key(&dev->mr_vec, dev->node);
	if (rc < 0)
		goto err;
	mr->res.key = rc;

	return 0;

 err:
	ntc_mr_buf_clear_sgl(mr->sg_list, count);

	return rc;
}

void ntrdma_mr_deinit(struct ntrdma_mr *mr, struct ntrdma_dev *dev)
{
	ntrdma_kvec_dispose_key(&dev->mr_vec, mr->res.key);
	ntc_mr_buf_clear_sgl(mr->sg_list, mr->sg_count);
}

static void ntrdma_mr_enable_cb(struct ntrdma_res *res,
				struct ntrdma_cmd_cb *cb)
{
	struct ntrdma_dev *dev = ntrdma_res_dev(res);
	struct ntrdma_mr *mr = ntrdma_res_mr(res);
	struct ntrdma_mr_cmd_cb *mrcb;
	u32 count;

	mrcb = container_of(cb, struct ntrdma_mr_cmd_cb, cb);

	count = mr->sg_count;
	if (count > NTRDMA_CMD_MR_CREATE_SG_CAP)
		count = NTRDMA_CMD_MR_CREATE_SG_CAP;

	mrcb->cb.cmd_prep = ntrdma_mr_enable_prep;
	mrcb->cb.rsp_cmpl = ntrdma_mr_enable_cmpl;
	mrcb->mr = mr;
	mrcb->sg_pos = 0;
	mrcb->sg_count = count;

	ntrdma_dev_cmd_add(dev, &mrcb->cb);
}

void ntrdma_mr_enable(struct ntrdma_mr *mr)
{
	reinit_completion(&mr->enable_mrcb.cb.cmds_done);
	ntrdma_mr_enable_cb(&mr->res, &mr->enable_mrcb.cb);
}

static int ntrdma_mr_enable_prep(struct ntrdma_cmd_cb *cb,
				union ntrdma_cmd *cmd)
{
	struct ntrdma_mr_cmd_cb *mrcb = ntrdma_cmd_cb_mrcb(cb);
	struct ntrdma_mr *mr = mrcb->mr;
	int i;

	TRACE("mr_enable prep: key=%d sg_pos=%d sg_count=%d",
		mr->res.key, mrcb->sg_pos, mrcb->sg_count);

	if (mrcb->sg_pos == 0) {
		cmd->mr_create.hdr.op = NTRDMA_CMD_MR_CREATE;
		cmd->mr_create.mr_key = mr->res.key;
		cmd->mr_create.pd_key = mr->pd_key;
		cmd->mr_create.access = mr->access;
		cmd->mr_create.mr_addr = mr->addr;
		cmd->mr_create.mr_len = mr->len;

		cmd->mr_create.sg_cap = mr->sg_count;
		cmd->mr_create.sg_count = mrcb->sg_count;

		for (i = 0; i < mrcb->sg_count; ++i)
			ntc_mr_buf_make_desc(&cmd->mr_create.sg_desc_list[i],
					&mr->sg_list[i]);
	} else {
		cmd->mr_append.hdr.op = NTRDMA_CMD_MR_APPEND;
		cmd->mr_append.mr_key = mr->res.key;

		cmd->mr_append.sg_pos = mrcb->sg_pos;
		cmd->mr_append.sg_count = mrcb->sg_count;

		for (i = 0; i < mrcb->sg_count; ++i)
			ntc_mr_buf_make_desc(&cmd->mr_append.sg_desc_list[i],
					&mr->sg_list[mrcb->sg_pos + i]);
	}

	return 0;
}

static int ntrdma_mr_enable_cmpl(struct ntrdma_cmd_cb *cb,
				const union ntrdma_rsp *rsp)
{
	struct ntrdma_mr_cmd_cb *mrcb = ntrdma_cmd_cb_mrcb(cb);
	struct ntrdma_mr *mr = mrcb->mr;
	struct ntrdma_dev *dev = ntrdma_res_dev(&mr->res);
	u32 end, count;
	int rc;

	TRACE("mr_enable cmpl: %d\n", mr->res.key);

	if (unlikely(READ_ONCE(rsp->hdr.status))) {
		rc = -EIO;
		goto out;
	} else
		rc = 0;

	end = mrcb->sg_pos + mrcb->sg_count;
	if (end != mr->sg_count) {
		if (unlikely(rc < 0))
			return rc;

		count = mr->sg_count - end;
		if (count > NTRDMA_CMD_MR_APPEND_SG_CAP)
			count = NTRDMA_CMD_MR_APPEND_SG_CAP;

		mrcb->sg_pos = end;
		mrcb->sg_count = count;

		ntrdma_dev_cmd_add_unsafe(dev, &mrcb->cb);
		return 0;
	}

 out:
	complete_all(&cb->cmds_done);

	return rc;
}

static void ntrdma_mr_disable_cb(struct ntrdma_res *res,
				struct ntrdma_cmd_cb *cb)
{
	struct ntrdma_dev *dev = ntrdma_res_dev(res);
	struct ntrdma_mr *mr = ntrdma_res_mr(res);
	struct ntrdma_mr_cmd_cb *mrcb;

	mrcb = container_of(cb, struct ntrdma_mr_cmd_cb, cb);

	mrcb->cb.cmd_prep = ntrdma_mr_disable_prep;
	mrcb->cb.rsp_cmpl = ntrdma_mr_disable_cmpl;
	mrcb->mr = mr;
	mrcb->sg_pos = 0;
	mrcb->sg_count = 0;

	ntrdma_dev_cmd_add(dev, &mrcb->cb);
}

static int ntrdma_mr_disable_prep(struct ntrdma_cmd_cb *cb,
				union ntrdma_cmd *cmd)
{
	struct ntrdma_mr_cmd_cb *mrcb = ntrdma_cmd_cb_mrcb(cb);
	struct ntrdma_mr *mr = mrcb->mr;

	cmd->mr_delete.hdr.op = NTRDMA_CMD_MR_DELETE;
	cmd->mr_delete.mr_key = mr->res.key;

	return 0;
}

static int ntrdma_mr_disable_cmpl(struct ntrdma_cmd_cb *cb,
				const union ntrdma_rsp *rsp)
{
	complete_all(&cb->cmds_done);

	if (unlikely(READ_ONCE(rsp->hdr.status)))
		return -EIO;

	return 0;
}

void ntrdma_rmr_init(struct ntrdma_rmr *rmr,
		    struct ntrdma_dev *dev,
		    u32 pd_key, u32 access,
		    u64 addr, u64 len,
		    u32 sg_count, u32 key)
{
	rmr->done = NULL;
	rmr->pd_key = pd_key;
	rmr->access = access;

	rmr->addr = addr;
	rmr->len = len;

	rmr->sg_count = sg_count;

	memset(rmr->sg_list, 0,
		sg_count * sizeof(*rmr->sg_list));

	ntrdma_rres_init(&rmr->rres, dev, &dev->rmr_vec,
			ntrdma_rmr_free, key);
}

static void ntrdma_rmr_release(struct kref *kref)
{
	struct ntrdma_obj *obj = container_of(kref, struct ntrdma_obj, kref);
	struct ntrdma_rres *rres = container_of(obj, struct ntrdma_rres, obj);
	struct ntrdma_rmr *rmr = container_of(rres, struct ntrdma_rmr, rres);
	struct ntrdma_dev *dev = ntrdma_rres_dev(rres);
	struct completion *done = rmr->done;
	int i;

	ntrdma_debugfs_rmr_del(rmr);

	for (i = 0; i < rmr->sg_count; i++)
		ntc_remote_buf_unmap(&rmr->sg_list[i], dev->ntc);

	kfree(rmr);

	if (done)
		complete_all(done);
}

void ntrdma_rmr_put(struct ntrdma_rmr *rmr)
{
	ntrdma_rres_put(&rmr->rres, ntrdma_rmr_release);
}

static void ntrdma_rmr_free(struct ntrdma_rres *rres)
{
	struct ntrdma_rmr *rmr = ntrdma_rres_rmr(rres);

	ntrdma_rmr_put(rmr);
	/* SYNC ref == 0 ?*/
}

struct ntrdma_mr *ntrdma_dev_mr_look(struct ntrdma_dev *dev, u32 key)
{
	struct ntrdma_res *res;

	res = ntrdma_res_look(&dev->mr_vec, key);
	if (!res)
		return NULL;

	return ntrdma_res_mr(res);
}

struct ntrdma_rmr *ntrdma_dev_rmr_look(struct ntrdma_dev *dev, u32 key)
{
	struct ntrdma_rres *rres;

	rres = ntrdma_rres_look(&dev->rmr_vec, key);
	if (!rres)
		return NULL;

	return ntrdma_rres_rmr(rres);
}
