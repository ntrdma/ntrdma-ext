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

#include "ntrdma_dev.h"
#include "ntrdma_cmd.h"
#include "ntrdma_pd.h"
#include "ntrdma_mr.h"

struct ntrdma_mr_cmd_cb {
	struct ntrdma_cmd_cb cb;
	struct ntrdma_mr *mr;
	u32 sg_pos, sg_count;
};

#define ntrdma_cmd_cb_mrcb(__cb) \
	container_of(__cb, struct ntrdma_mr_cmd_cb, cb)

static int ntrdma_mr_append_prep(struct ntrdma_cmd_cb *cb,
				 union ntrdma_cmd *cmd, struct ntrdma_req *req);
static int ntrdma_mr_enable_prep(struct ntrdma_cmd_cb *cb,
				 union ntrdma_cmd *cmd, struct ntrdma_req *req);
static int ntrdma_mr_enable_cmpl(struct ntrdma_cmd_cb *cb,
				 union ntrdma_rsp *rsp, struct ntrdma_req *req);
static int ntrdma_mr_disable_prep(struct ntrdma_cmd_cb *cb,
				  union ntrdma_cmd *cmd, struct ntrdma_req *req);
static int ntrdma_mr_disable_cmpl(struct ntrdma_cmd_cb *cb,
				  union ntrdma_rsp *rsp, struct ntrdma_req *req);

static int ntrdma_mr_enable(struct ntrdma_res *res);
static int ntrdma_mr_disable(struct ntrdma_res *res);

static void ntrdma_rmr_free(struct ntrdma_rres *rres);

int ntrdma_mr_init(struct ntrdma_mr *mr,
		   struct ntrdma_dev *dev,
		   void *umem,
		   u32 pd_key, u32 access,
		   u64 addr, u64 len,
		   u32 sg_count)
{
	int rc;

	mr->umem = umem;

	mr->pd_key = pd_key;
	mr->access = access;

	mr->addr = addr;
	mr->len = len;

	mr->sg_count = ntc_umem_sgl(dev->ntc, umem, mr->sg_list, sg_count);
	if (mr->sg_count != sg_count) {
		rc = -ENOMEM;
		goto err;
	}

	if (sg_count)
		WARN_ON((mr->sg_list[0].addr ^ addr) & (PAGE_SIZE - 1));

	rc = ntrdma_res_init(&mr->res, dev, &dev->mr_vec,
			     ntrdma_mr_enable, ntrdma_mr_disable, NULL);
	if (rc)
		goto err;

	return 0;

err:
	return rc;
}

void ntrdma_mr_deinit(struct ntrdma_mr *mr)
{
	ntrdma_res_deinit(&mr->res);
}

static int ntrdma_mr_enable(struct ntrdma_res *res)
{
	struct ntrdma_dev *dev = ntrdma_res_dev(res);
	struct ntrdma_mr *mr = ntrdma_res_mr(res);
	struct ntrdma_mr_cmd_cb *mrcb;
	u32 pos = 0, end, count;
	int rc;

	ntrdma_res_start_cmds(&mr->res);

	mrcb = kmalloc_node(sizeof(*mrcb), GFP_KERNEL, dev->node);
	if (!mrcb) {
		rc = -ENOMEM;
		goto err_create;
	}

	count = mr->sg_count;
	if (count > NTRDMA_CMD_MR_CREATE_SG_CAP)
		count = NTRDMA_CMD_MR_CREATE_SG_CAP;
	pos = 0;
	end = count;

	mrcb->cb.cmd_prep = ntrdma_mr_enable_prep;
	mrcb->cb.rsp_cmpl = ntrdma_mr_enable_cmpl;
	mrcb->mr = mr;
	mrcb->sg_pos = pos;
	mrcb->sg_count = count;

	ntrdma_dev_cmd_add(dev, &mrcb->cb);

	while (end < mr->sg_count) {
		mrcb = kmalloc_node(sizeof(*mrcb), GFP_KERNEL, dev->node);
		if (!mrcb) {
			rc = -ENOMEM;
			goto err_append;
		}

		count = mr->sg_count - end;
		if (count > NTRDMA_CMD_MR_APPEND_SG_CAP)
			count = NTRDMA_CMD_MR_APPEND_SG_CAP;
		pos = end;
		end = pos + count;

		mrcb->cb.cmd_prep = ntrdma_mr_append_prep;
		mrcb->cb.rsp_cmpl = ntrdma_mr_enable_cmpl;
		mrcb->mr = mr;
		mrcb->sg_pos = pos;
		mrcb->sg_count = count;

		ntrdma_dev_cmd_add(dev, &mrcb->cb);
	}

	return 0;

err_create:
	ntrdma_res_done_cmds(&mr->res);
err_append:
	return rc;
}

static int ntrdma_mr_append_prep(struct ntrdma_cmd_cb *cb,
				 union ntrdma_cmd *cmd, struct ntrdma_req *req)
{
	struct ntrdma_mr_cmd_cb *mrcb = ntrdma_cmd_cb_mrcb(cb);
	struct ntrdma_mr *mr = mrcb->mr;

	cmd->mr_append.op = NTRDMA_CMD_MR_APPEND;
	cmd->mr_append.mr_key = mr->res.key;

	cmd->mr_append.sg_pos = mrcb->sg_pos;
	cmd->mr_append.sg_count = mrcb->sg_count;

	memcpy(cmd->mr_append.sg_list, &mr->sg_list[mrcb->sg_pos],
	       mrcb->sg_count * sizeof(*mr->sg_list));

	return 0;
}

static int ntrdma_mr_enable_prep(struct ntrdma_cmd_cb *cb,
				 union ntrdma_cmd *cmd, struct ntrdma_req *req)
{
	struct ntrdma_mr_cmd_cb *mrcb = ntrdma_cmd_cb_mrcb(cb);
	struct ntrdma_mr *mr = mrcb->mr;

	cmd->mr_create.op = NTRDMA_CMD_MR_CREATE;
	cmd->mr_create.mr_key = mr->res.key;
	cmd->mr_create.pd_key = mr->pd_key;
	cmd->mr_create.access = mr->access;
	cmd->mr_create.mr_addr = mr->addr;
	cmd->mr_create.mr_len = mr->len;

	cmd->mr_create.sg_cap = mr->sg_count;
	cmd->mr_create.sg_count = mrcb->sg_count;

	memcpy(cmd->mr_create.sg_list, mr->sg_list,
	       mrcb->sg_count * sizeof(*mr->sg_list));

	return 0;
}

static int ntrdma_mr_enable_cmpl(struct ntrdma_cmd_cb *cb,
				 union ntrdma_rsp *rsp, struct ntrdma_req *req)
{
	struct ntrdma_mr_cmd_cb *mrcb = ntrdma_cmd_cb_mrcb(cb);
	struct ntrdma_mr *mr = mrcb->mr;
	int rc;

	if (!rsp || rsp->hdr.status) {
		rc = -EIO;
		goto err;
	}

	if (mrcb->sg_pos + mrcb->sg_count == mr->sg_count)
		ntrdma_res_done_cmds(&mr->res);
	kfree(mrcb);

	return 0;

err:
	if (mrcb->sg_pos + mrcb->sg_count == mr->sg_count)
		ntrdma_res_done_cmds(&mr->res);
	return rc;
}

static int ntrdma_mr_disable(struct ntrdma_res *res)
{
	struct ntrdma_dev *dev = ntrdma_res_dev(res);
	struct ntrdma_mr *mr = ntrdma_res_mr(res);
	struct ntrdma_mr_cmd_cb *mrcb;
	int rc;

	ntrdma_res_start_cmds(&mr->res);

	mrcb = kmalloc_node(sizeof(*mrcb), GFP_KERNEL, dev->node);
	if (!mrcb) {
		rc = -ENOMEM;
		goto err;
	}

	mrcb->cb.cmd_prep = ntrdma_mr_disable_prep;
	mrcb->cb.rsp_cmpl = ntrdma_mr_disable_cmpl;
	mrcb->mr = mr;
	mrcb->sg_pos = 0;
	mrcb->sg_count = 0;

	ntrdma_dev_cmd_add(dev, &mrcb->cb);

	return 0;

err:
	ntrdma_res_done_cmds(&mr->res);
	return rc;
}

static int ntrdma_mr_disable_prep(struct ntrdma_cmd_cb *cb,
				  union ntrdma_cmd *cmd, struct ntrdma_req *req)
{
	struct ntrdma_mr_cmd_cb *mrcb = ntrdma_cmd_cb_mrcb(cb);
	struct ntrdma_mr *mr = mrcb->mr;

	cmd->mr_delete.op = NTRDMA_CMD_MR_DELETE;
	cmd->mr_delete.mr_key = mr->res.key;

	return 0;
}

static int ntrdma_mr_disable_cmpl(struct ntrdma_cmd_cb *cb,
				  union ntrdma_rsp *rsp, struct ntrdma_req *req)
{
	struct ntrdma_mr_cmd_cb *mrcb = ntrdma_cmd_cb_mrcb(cb);
	struct ntrdma_mr *mr = mrcb->mr;
	int rc;

	if (!rsp || rsp->hdr.status) {
		rc = -EIO;
		goto err;
	}

	ntrdma_res_done_cmds(&mr->res);
	kfree(mrcb);

	return 0;

err:
	ntrdma_res_done_cmds(&mr->res);
	return rc;
}

int ntrdma_rmr_init(struct ntrdma_rmr *rmr,
		    struct ntrdma_dev *dev,
		    u32 pd_key, u32 access,
		    u64 addr, u64 len,
		    u32 sg_count)
{
	rmr->pd_key = pd_key;
	rmr->access = access;

	rmr->addr = addr;
	rmr->len = len;

	rmr->sg_count = sg_count;

	memset(rmr->sg_list, 0, sg_count * sizeof(*rmr->sg_list));

	return ntrdma_rres_init(&rmr->rres, dev, &dev->rmr_vec,
				ntrdma_rmr_free);
}

void ntrdma_rmr_deinit(struct ntrdma_rmr *rmr)
{
	ntrdma_rres_deinit(&rmr->rres);
}

static void ntrdma_rmr_free(struct ntrdma_rres *rres)
{
	struct ntrdma_rmr *rmr = ntrdma_rres_rmr(rres);

	ntrdma_rmr_del(rmr);
	ntrdma_rmr_deinit(rmr);
	kfree(rmr);
}

struct ntrdma_mr *ntrdma_dev_mr_look(struct ntrdma_dev *dev, int key)
{
	struct ntrdma_res *res;

	res = ntrdma_dev_res_look(dev, &dev->mr_vec, key);
	if (!res)
		return NULL;

	return ntrdma_res_mr(res);
}

struct ntrdma_rmr *ntrdma_dev_rmr_look(struct ntrdma_dev *dev, int key)
{
	struct ntrdma_rres *rres;

	rres = ntrdma_dev_rres_look(dev, &dev->rmr_vec, key);
	if (!rres)
		return NULL;

	return ntrdma_rres_rmr(rres);
}

