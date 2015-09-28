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
#include "ntrdma_res.h"
#include "ntrdma_pd.h"

struct ntrdma_pd_cmd_cb {
	struct ntrdma_cmd_cb cb;
	struct ntrdma_pd *pd;
};

#define ntrdma_cmd_cb_pdcb(__cb) \
	container_of(__cb, struct ntrdma_pd_cmd_cb, cb)

static int ntrdma_pd_enable_prep(struct ntrdma_cmd_cb *cb,
				 union ntrdma_cmd *cmd, struct ntrdma_req *req);
static int ntrdma_pd_enable_cmpl(struct ntrdma_cmd_cb *cb,
				 union ntrdma_rsp *rsp, struct ntrdma_req *req);
static int ntrdma_pd_disable_prep(struct ntrdma_cmd_cb *cb,
				  union ntrdma_cmd *cmd, struct ntrdma_req *req);
static int ntrdma_pd_disable_cmpl(struct ntrdma_cmd_cb *cb,
				  union ntrdma_rsp *rsp, struct ntrdma_req *req);

static int ntrdma_pd_enable(struct ntrdma_res *res);
static int ntrdma_pd_disable(struct ntrdma_res *res);

static void ntrdma_rpd_free(struct ntrdma_rres *rres);

int ntrdma_pd_init(struct ntrdma_pd *pd, struct ntrdma_dev *dev)
{
	int rc;

	rc = ntrdma_kvec_init(&pd->mr_vec, NTRDMA_RES_VEC_INIT_CAP, dev->node);
	if (rc)
		goto err_mr;

	rc = ntrdma_res_init(&pd->res, dev, &dev->pd_vec,
			     ntrdma_pd_enable, ntrdma_pd_disable, NULL);
	if (rc)
		goto err_res;

	return 0;

err_res:
	ntrdma_kvec_deinit(&pd->mr_vec);
err_mr:
	return rc;
}

void ntrdma_pd_deinit(struct ntrdma_pd *pd)
{
	ntrdma_res_deinit(&pd->res);
	ntrdma_kvec_deinit(&pd->mr_vec);
}

static int ntrdma_pd_enable(struct ntrdma_res *res)
{
	struct ntrdma_dev *dev = ntrdma_res_dev(res);
	struct ntrdma_pd *pd = ntrdma_res_pd(res);
	struct ntrdma_pd_cmd_cb *pdcb;
	int rc;

	ntrdma_res_start_cmds(&pd->res);

	pdcb = kmalloc_node(sizeof(*pdcb), GFP_KERNEL, dev->node);
	if (!pdcb) {
		rc = -ENOMEM;
		goto err;
	}

	pdcb->cb.cmd_prep = ntrdma_pd_enable_prep;
	pdcb->cb.rsp_cmpl = ntrdma_pd_enable_cmpl;
	pdcb->pd = pd;

	ntrdma_dev_cmd_add(dev, &pdcb->cb);

	return 0;

err:
	ntrdma_res_done_cmds(&pd->res);
	return rc;
}

static int ntrdma_pd_enable_prep(struct ntrdma_cmd_cb *cb,
				 union ntrdma_cmd *cmd, struct ntrdma_req *req)
{
	struct ntrdma_pd_cmd_cb *pdcb = ntrdma_cmd_cb_pdcb(cb);
	struct ntrdma_pd *pd = pdcb->pd;

	cmd->pd_create.op = NTRDMA_CMD_PD_CREATE;
	cmd->pd_create.pd_key = pd->res.key;

	return 0;
}

static int ntrdma_pd_enable_cmpl(struct ntrdma_cmd_cb *cb,
				 union ntrdma_rsp *rsp, struct ntrdma_req *req)
{
	struct ntrdma_pd_cmd_cb *pdcb = ntrdma_cmd_cb_pdcb(cb);
	struct ntrdma_pd *pd = pdcb->pd;
	int rc;

	if (!rsp || rsp->pd_status.status) {
		rc = -EIO;
		goto err;
	}

	ntrdma_res_done_cmds(&pd->res);
	kfree(pdcb);

	return 0;

err:
	ntrdma_res_done_cmds(&pd->res);
	return rc;
}

static int ntrdma_pd_disable(struct ntrdma_res *res)
{
	struct ntrdma_dev *dev = ntrdma_res_dev(res);
	struct ntrdma_pd *pd = ntrdma_res_pd(res);
	struct ntrdma_pd_cmd_cb *pdcb;
	int rc;

	ntrdma_res_start_cmds(&pd->res);

	pdcb = kmalloc_node(sizeof(*pdcb), GFP_KERNEL, dev->node);
	if (!pdcb) {
		rc = -ENOMEM;
		goto err;
	}

	pdcb->cb.cmd_prep = ntrdma_pd_disable_prep;
	pdcb->cb.rsp_cmpl = ntrdma_pd_disable_cmpl;
	pdcb->pd = pd;

	ntrdma_dev_cmd_add(dev, &pdcb->cb);

	return 0;

err:
	ntrdma_res_done_cmds(&pd->res);
	return rc;
}

static int ntrdma_pd_disable_prep(struct ntrdma_cmd_cb *cb,
				  union ntrdma_cmd *cmd, struct ntrdma_req *req)
{
	struct ntrdma_pd_cmd_cb *pdcb = ntrdma_cmd_cb_pdcb(cb);
	struct ntrdma_pd *pd = pdcb->pd;

	cmd->pd_create.op = NTRDMA_CMD_PD_DELETE;
	cmd->pd_create.pd_key = pd->res.key;

	return 0;
}

static int ntrdma_pd_disable_cmpl(struct ntrdma_cmd_cb *cb,
				  union ntrdma_rsp *rsp, struct ntrdma_req *req)
{
	struct ntrdma_pd_cmd_cb *pdcb = ntrdma_cmd_cb_pdcb(cb);
	struct ntrdma_pd *pd = pdcb->pd;
	int rc;

	if (!rsp || rsp->pd_status.status) {
		rc = -EIO;
		goto err;
	}

	ntrdma_res_done_cmds(&pd->res);
	kfree(pdcb);

	return 0;

err:
	ntrdma_res_done_cmds(&pd->res);
	return rc;
}

int ntrdma_rpd_init(struct ntrdma_rpd *rpd, struct ntrdma_dev *dev)
{
	int rc;

	rc = ntrdma_vec_init(&rpd->rmr_vec, NTRDMA_RES_VEC_INIT_CAP, dev->node);
	if (rc)
		goto err_rmr;

	rc = ntrdma_rres_init(&rpd->rres, dev, &dev->rpd_vec, ntrdma_rpd_free);
	if (rc)
		goto err_rres;

	return 0;

err_rres:
	ntrdma_vec_deinit(&rpd->rmr_vec);
err_rmr:
	return rc;
}

void ntrdma_rpd_deinit(struct ntrdma_rpd *rpd)
{
	ntrdma_vec_deinit(&rpd->rmr_vec);
	ntrdma_rres_deinit(&rpd->rres);
}

static void ntrdma_rpd_free(struct ntrdma_rres *rres)
{
	struct ntrdma_rpd *rpd = ntrdma_rres_rpd(rres);

	ntrdma_rpd_del(rpd);
	ntrdma_rpd_deinit(rpd);
	kfree(rpd);
}

struct ntrdma_pd *ntrdma_dev_pd_look(struct ntrdma_dev *dev, int key)
{
	struct ntrdma_res *res;

	res = ntrdma_dev_res_look(dev, &dev->pd_vec, key);
	if (!res)
		return NULL;

	return ntrdma_res_pd(res);
}

struct ntrdma_rpd *ntrdma_dev_rpd_look(struct ntrdma_dev *dev, int key)
{
	struct ntrdma_rres *rres;

	rres = ntrdma_dev_rres_look(dev, &dev->rpd_vec, key);
	if (!rres)
		return NULL;

	return ntrdma_rres_rpd(rres);
}

