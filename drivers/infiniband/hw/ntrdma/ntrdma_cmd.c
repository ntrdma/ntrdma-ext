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
#include "ntrdma_hello.h"
#include "ntrdma_util.h"
#include "ntrdma_res.h"
#include "ntrdma_cmd.h"
#include "ntrdma_ring.h"

#include "ntrdma_cq.h"
#include "ntrdma_pd.h"
#include "ntrdma_mr.h"
#include "ntrdma_qp.h"

#define NTRDMA_RES_VBELL		1
#define NTRDMA_RRES_VBELL		0
#define MAX_CMDS 16

static void ntrdma_cmd_send_work(struct ntrdma_dev *dev);
static void ntrdma_cmd_send_work_cb(struct work_struct *ws);
static void ntrdma_cmd_send_vbell_cb(void *ctx);

static int ntrdma_cmd_recv(struct ntrdma_dev *dev, union ntrdma_cmd *cmd,
			   union ntrdma_rsp *rsp, void *req);

static void ntrdma_cmd_recv_work(struct ntrdma_dev *dev);
static void ntrdma_cmd_recv_work_cb(struct work_struct *ws);
static void ntrdma_cmd_recv_vbell_cb(void *ctx);

static inline bool ntrdma_cmd_done(struct ntrdma_dev *dev)
{
	return list_empty(&dev->cmd_post_list) &&
		list_empty(&dev->cmd_pend_list);
}

int ntrdma_dev_cmd_init(struct ntrdma_dev *dev,
			u32 recv_vbell_idx,
			u32 send_vbell_idx,
			u32 send_cap)
{
	int rc;

	dev->cmd_ready = 0;

	/* recv work */
	mutex_init(&dev->cmd_recv_lock);
	INIT_WORK(&dev->cmd_recv_work,
		  ntrdma_cmd_recv_work_cb);
	ntrdma_vbell_init(&dev->cmd_recv_vbell,
			  ntrdma_cmd_recv_vbell_cb, dev);
	dev->cmd_recv_vbell_idx = recv_vbell_idx;

	/* allocated in conf phase */
	dev->cmd_recv_cap = 0;
	dev->cmd_recv_cons = 0;
	dev->cmd_recv_buf = NULL;
	dev->cmd_recv_prod_buf = NULL;
	dev->cmd_recv_buf_addr = 0;
	dev->cmd_recv_buf_size = 0;
	dev->cmd_recv_rsp_buf = NULL;
	dev->cmd_recv_rsp_buf_addr = 0;
	dev->cmd_recv_rsp_buf_size = 0;

	/* assigned in ready phase */
	dev->peer_cmd_send_rsp_buf_addr = 0;
	dev->peer_cmd_send_cons_addr = 0;
	/* assigned in conf phase */
	dev->peer_cmd_send_vbell_idx = 0;

	/* send work */
	INIT_LIST_HEAD(&dev->cmd_pend_list);
	INIT_LIST_HEAD(&dev->cmd_post_list);
	init_waitqueue_head(&dev->cmd_send_cond);
	mutex_init(&dev->cmd_send_lock);
	INIT_WORK(&dev->cmd_send_work,
		  ntrdma_cmd_send_work_cb);
	ntrdma_vbell_init(&dev->cmd_send_vbell,
			  ntrdma_cmd_send_vbell_cb, dev);
	dev->cmd_send_vbell_idx = send_vbell_idx;

	/* allocate send buffers */
	dev->cmd_send_cap = send_cap;
	dev->cmd_send_prod = 0;
	dev->cmd_send_cmpl = 0;

	dev->cmd_send_buf_size = dev->cmd_send_cap * sizeof(union ntrdma_cmd);

	dev->cmd_send_buf = kzalloc_node(dev->cmd_send_buf_size,
					 GFP_KERNEL, dev->node);
	if (!dev->cmd_send_buf) {
		rc = -ENOMEM;
		goto err_send_buf;
	}

	dev->cmd_send_buf_addr = ntc_buf_map(dev->ntc,
					     dev->cmd_send_buf,
					     dev->cmd_send_buf_size,
					     DMA_TO_DEVICE);
	if (!dev->cmd_send_buf_addr) {
		ntrdma_err(dev, "dma mapping failed\n");
		rc = -EIO;
		goto err_send_buf_addr;
	}

	dev->cmd_send_rsp_buf_size = dev->cmd_send_cap * sizeof(union ntrdma_rsp)
		+ sizeof(*dev->cmd_send_cons_buf); /* for cmd_send_cons_buf */

	dev->cmd_send_rsp_buf = kzalloc_node(dev->cmd_send_rsp_buf_size,
					     GFP_KERNEL, dev->node);
	if (!dev->cmd_send_rsp_buf) {
		rc = -ENOMEM;
		goto err_send_rsp_buf;
	}

	dev->cmd_send_cons_buf = (void *)&dev->cmd_send_rsp_buf[dev->cmd_send_cap];

	*dev->cmd_send_cons_buf = 0;

	dev->cmd_send_rsp_buf_addr = ntc_buf_map(dev->ntc,
						 dev->cmd_send_rsp_buf,
						 dev->cmd_send_rsp_buf_size,
						 DMA_FROM_DEVICE);
	if (!dev->cmd_send_rsp_buf_addr) {
		ntrdma_err(dev, "dma mapping failed\n");
		rc = -EIO;
		goto err_send_rsp_buf_addr;
	}

	/* assigned in conf phase */
	dev->peer_cmd_recv_buf_addr = 0;
	dev->peer_cmd_recv_prod_addr = 0;
	dev->peer_cmd_recv_vbell_idx = 0;

	return 0;

err_send_rsp_buf_addr:
	kfree(dev->cmd_send_rsp_buf);
err_send_rsp_buf:
	ntc_buf_unmap(dev->ntc,
		      dev->cmd_send_buf_addr, dev->cmd_send_buf_size,
		      DMA_TO_DEVICE);
err_send_buf_addr:
	kfree(dev->cmd_send_buf);
err_send_buf:
	return rc;
}

void ntrdma_dev_cmd_deinit(struct ntrdma_dev *dev)
{
	ntc_buf_unmap(dev->ntc,
		      dev->cmd_send_rsp_buf_addr, dev->cmd_send_rsp_buf_size,
		      DMA_FROM_DEVICE);
	kfree(dev->cmd_send_rsp_buf);
	ntc_buf_unmap(dev->ntc,
		      dev->cmd_send_buf_addr, dev->cmd_send_buf_size,
		      DMA_TO_DEVICE);
	kfree(dev->cmd_send_buf);
}

void ntrdma_dev_cmd_hello_info(struct ntrdma_dev *dev,
			       struct ntrdma_cmd_hello_info *info)
{
	info->send_rsp_buf_addr = dev->cmd_send_rsp_buf_addr;
	info->send_cons_addr =
		dev->cmd_send_rsp_buf_addr +
		dev->cmd_send_cap * sizeof(union ntrdma_rsp);
	info->send_cap = dev->cmd_send_cap;
	info->send_idx = *dev->cmd_send_cons_buf;
	info->send_vbell_idx = dev->cmd_send_vbell_idx;
	info->recv_vbell_idx = dev->cmd_recv_vbell_idx;
}

int ntrdma_dev_cmd_hello_prep(struct ntrdma_dev *dev,
			      struct ntrdma_cmd_hello_info *peer_info,
			      struct ntrdma_cmd_hello_prep *prep)
{
	int rc;

	dev->peer_cmd_send_rsp_buf_addr =
		ntc_peer_addr(dev->ntc, peer_info->send_rsp_buf_addr);
	dev->peer_cmd_send_cons_addr =
		ntc_peer_addr(dev->ntc, peer_info->send_cons_addr);
	dev->peer_cmd_send_vbell_idx = peer_info->send_vbell_idx;
	dev->peer_cmd_recv_vbell_idx = peer_info->recv_vbell_idx;

	if (peer_info->send_cap > MAX_CMDS || peer_info->send_idx > MAX_CMDS) {
		ntrdma_err(dev, "peer info is suspected as garbage cap %d, idx %d\n",
				peer_info->send_cap, peer_info->send_idx);
		rc = -ENOMEM;
		goto err_recv_buf;
	}

	/* allocate local recv ring for peer send */
	dev->cmd_recv_cap = peer_info->send_cap;
	dev->cmd_recv_cons = peer_info->send_idx;

	dev->cmd_recv_buf_size = dev->cmd_recv_cap * sizeof(union ntrdma_cmd)
		+ sizeof(*dev->cmd_recv_prod_buf); /* for cmd_recv_prod_buf */

	dev->cmd_recv_buf = kzalloc_node(dev->cmd_recv_buf_size,
					 GFP_KERNEL, dev->node);
	if (!dev->cmd_recv_buf) {
		rc = -ENOMEM;
		goto err_recv_buf;
	}

	dev->cmd_recv_prod_buf = (void *)&dev->cmd_recv_buf[dev->cmd_recv_cap];

	*dev->cmd_recv_prod_buf = 0;

	dev->cmd_recv_buf_addr = ntc_buf_map(dev->ntc,
					     dev->cmd_recv_buf,
					     dev->cmd_recv_buf_size,
					     DMA_FROM_DEVICE);
	if (!dev->cmd_recv_buf_addr) {
		ntrdma_err(dev, "dma mapping failed\n");
		rc = -EIO;
		goto err_recv_buf_addr;
	}

	dev->cmd_recv_rsp_buf_size = dev->cmd_recv_cap * sizeof(union ntrdma_rsp);

	dev->cmd_recv_rsp_buf = kzalloc_node(dev->cmd_recv_rsp_buf_size,
					     GFP_KERNEL, dev->node);
	if (!dev->cmd_recv_rsp_buf) {
		rc = -ENOMEM;
		goto err_recv_rsp_buf;
	}

	dev->cmd_recv_rsp_buf_addr = ntc_buf_map(dev->ntc,
						 dev->cmd_recv_rsp_buf,
						 dev->cmd_recv_rsp_buf_size,
						 DMA_TO_DEVICE);
	if (!dev->cmd_recv_rsp_buf_addr) {
		ntrdma_err(dev, "dma mapping failed\n");
		rc = -EIO;
		goto err_recv_rsp_buf_addr;
	}

	prep->recv_buf_addr = dev->cmd_recv_buf_addr;
	prep->recv_prod_addr =
		dev->cmd_recv_buf_addr +
		dev->cmd_recv_cap * sizeof(union ntrdma_cmd);

	return 0;

err_recv_rsp_buf_addr:
	kfree(dev->cmd_recv_rsp_buf);
err_recv_rsp_buf:
	ntc_buf_unmap(dev->ntc,
		      dev->cmd_recv_buf_addr, dev->cmd_recv_buf_size,
		      DMA_FROM_DEVICE);
err_recv_buf_addr:
	kfree(dev->cmd_recv_buf);
err_recv_buf:
	dev->peer_cmd_send_rsp_buf_addr = 0;
	dev->peer_cmd_send_cons_addr = 0;
	dev->peer_cmd_send_vbell_idx = 0;
	dev->cmd_recv_cap = 0;
	dev->cmd_recv_cons = 0;
	dev->cmd_recv_buf_size = 0;
	dev->cmd_recv_buf = NULL;
	dev->cmd_recv_prod_buf = NULL;
	dev->cmd_recv_buf_addr = 0;
	dev->cmd_recv_rsp_buf_size = 0;
	dev->cmd_recv_rsp_buf = NULL;
	dev->cmd_recv_rsp_buf_addr = 0;
	dev->peer_cmd_recv_vbell_idx = 0;
	return rc;
}

void ntrdma_dev_cmd_hello_done(struct ntrdma_dev *dev,
			       struct ntrdma_cmd_hello_prep *peer_prep)
{
	dev->peer_cmd_recv_buf_addr =
		ntc_peer_addr(dev->ntc, peer_prep->recv_buf_addr);
	dev->peer_cmd_recv_prod_addr =
		ntc_peer_addr(dev->ntc, peer_prep->recv_prod_addr);
}

void ntrdma_dev_cmd_reset(struct ntrdma_dev *dev)
{
	ntc_buf_unmap(dev->ntc,
		      dev->cmd_recv_rsp_buf_addr,
		      dev->cmd_recv_rsp_buf_size,
		      DMA_TO_DEVICE);
	kfree(dev->cmd_recv_rsp_buf);
	ntc_buf_unmap(dev->ntc,
		      dev->cmd_recv_buf_addr,
		      dev->cmd_recv_buf_size,
		      DMA_FROM_DEVICE);
	kfree(dev->cmd_recv_buf);

	dev->peer_cmd_send_rsp_buf_addr = 0;
	dev->peer_cmd_send_cons_addr = 0;
	dev->peer_cmd_send_vbell_idx = 0;
	dev->cmd_recv_cap = 0;
	dev->cmd_recv_cons = 0;
	dev->cmd_recv_buf_size = 0;
	dev->cmd_recv_buf = NULL;
	dev->cmd_recv_prod_buf = NULL;
	dev->cmd_recv_buf_addr = 0;
	dev->cmd_recv_rsp_buf_size = 0;
	dev->cmd_recv_rsp_buf = NULL;
	dev->cmd_recv_rsp_buf_addr = 0;
	dev->peer_cmd_recv_vbell_idx = 0;
}

void ntrdma_dev_cmd_enable(struct ntrdma_dev *dev)
{
	mutex_lock(&dev->cmd_send_lock);
	mutex_lock(&dev->cmd_recv_lock);
	{
		dev->cmd_ready = 1;
	}
	mutex_unlock(&dev->cmd_recv_lock);
	mutex_unlock(&dev->cmd_send_lock);

	ntrdma_cmd_recv_work(dev);
	ntrdma_cmd_send_work(dev);
}

void ntrdma_dev_cmd_disable(struct ntrdma_dev *dev)
{
	mutex_lock(&dev->cmd_send_lock);
	mutex_lock(&dev->cmd_recv_lock);
	{
		dev->cmd_ready = 0;
	}
	mutex_unlock(&dev->cmd_recv_lock);
	mutex_unlock(&dev->cmd_send_lock);
}

void ntrdma_dev_cmd_add(struct ntrdma_dev *dev, struct ntrdma_cmd_cb *cb)
{
	mutex_lock(&dev->cmd_send_lock);
	{
		list_add_tail(&cb->dev_entry, &dev->cmd_pend_list);
	}
	mutex_unlock(&dev->cmd_send_lock);
}

void ntrdma_dev_cmd_submit(struct ntrdma_dev *dev)
{
	schedule_work(&dev->cmd_send_work);
}

void ntrdma_dev_cmd_finish(struct ntrdma_dev *dev)
{
	mutex_lock(&dev->cmd_send_lock);
	{
		wait_event_cmd(dev->cmd_send_cond, ntrdma_cmd_done(dev),
			       mutex_unlock(&dev->cmd_send_lock),
			       mutex_lock(&dev->cmd_send_lock));
	}
	mutex_unlock(&dev->cmd_send_lock);
}

static inline void ntrdma_cmd_send_vbell_clear(struct ntrdma_dev *dev)
{
	ntrdma_dev_vbell_clear(dev, &dev->cmd_send_vbell,
			       dev->cmd_send_vbell_idx);
}

static inline int ntrdma_cmd_send_vbell_add(struct ntrdma_dev *dev)
{
	return ntrdma_dev_vbell_add(dev, &dev->cmd_send_vbell,
				    dev->cmd_send_vbell_idx);
}

static void ntrdma_cmd_send_work(struct ntrdma_dev *dev)
{
	void *req;
	struct ntrdma_cmd_cb *cb;
	u32 start, pos, end, base;
	u64 dst, src;
	size_t off, len;
	bool more = false;
	int rc;

	req = ntc_req_create(dev->ntc);
	if (!req)
		return; /* FIXME: no req, now what? */

	/* sync the ring buf for the cpu */

	ntc_buf_sync_cpu(dev->ntc,
			 dev->cmd_send_rsp_buf_addr,
			 dev->cmd_send_rsp_buf_size,
			 DMA_FROM_DEVICE);

	mutex_lock(&dev->cmd_send_lock);
	{
		ntrdma_cmd_send_vbell_clear(dev);

		/* Complete commands that have a response */

		ntrdma_ring_consume(*dev->cmd_send_cons_buf, dev->cmd_send_cmpl,
				    dev->cmd_send_cap, &start, &end, &base);
		ntrdma_vdbg(dev, "rsp start %d end %d\n", start, end);
		for (pos = start; pos < end; ++pos) {
			cb = list_first_entry(&dev->cmd_post_list,
					      struct ntrdma_cmd_cb,
					      dev_entry);

			list_del(&cb->dev_entry);

			ntrdma_vdbg(dev, "rsp cmpl pos %d\n", pos);
			rc = cb->rsp_cmpl(cb, &dev->cmd_send_rsp_buf[pos], req);
			if (rc)
				; /* FIXME: command failed, now what? */
		}

		if (pos != start) {
			dev->cmd_send_cmpl = ntrdma_ring_update(pos, base,
								dev->cmd_send_cap);
			more = true;
		}

		/* Issue some more pending commands */

		ntrdma_ring_produce(dev->cmd_send_prod,
				    dev->cmd_send_cmpl,
				    dev->cmd_send_cap,
				    &start, &end, &base);
		ntrdma_vdbg(dev, "cmd start %d end %d\n", start, end);
		for (pos = start; pos < end; ++pos) {
			if (list_empty(&dev->cmd_pend_list))
				break;

			cb = list_first_entry(&dev->cmd_pend_list,
					      struct ntrdma_cmd_cb,
					      dev_entry);

			list_move_tail(&cb->dev_entry, &dev->cmd_post_list);

			ntrdma_vdbg(dev, "cmd prep pos %d\n", pos);
			rc = cb->cmd_prep(cb, &dev->cmd_send_buf[pos], req);
			if (rc)
				; /* FIXME: command failed, now what? */
		}

		if (pos != start) {
			ntrdma_vdbg(dev, "cmd copy start %d pos %d\n", start, pos);

			dev->cmd_send_prod = ntrdma_ring_update(pos, base,
								dev->cmd_send_cap);
			more = true;

			/* sync the ring buf for the device */

			ntc_buf_sync_dev(dev->ntc,
					 dev->cmd_send_buf_addr,
					 dev->cmd_send_buf_size,
					 DMA_TO_DEVICE);

			/* copy the portion of the ring buf */

			off = start * sizeof(union ntrdma_cmd);
			len = (pos - start) * sizeof(union ntrdma_cmd);
			dst = dev->peer_cmd_recv_buf_addr + off;
			src = dev->cmd_send_buf_addr + off;

			ntc_req_memcpy(dev->ntc, req,
				       dst, src, len,
				       true, NULL, NULL);

			/* update the producer index on the peer */

			ntc_req_imm32(dev->ntc, req,
				      dev->peer_cmd_recv_prod_addr,
				      dev->cmd_send_prod,
				      true, NULL, NULL);

			/* update the vbell and signal the peer */

			ntrdma_dev_vbell_peer(dev, req,
					      dev->peer_cmd_recv_vbell_idx);
			ntc_req_signal(dev->ntc, req, NULL, NULL, NTB_DEFAULT_VEC(dev->ntc));
			ntc_req_submit(dev->ntc, req);
		} else {
			ntc_req_cancel(dev->ntc, req);
		}

		if (!ntrdma_cmd_done(dev)) {
			if (more || ntrdma_cmd_send_vbell_add(dev) == -EAGAIN)
				schedule_work(&dev->cmd_send_work);
		}
	}
	mutex_unlock(&dev->cmd_send_lock);
}

static void ntrdma_cmd_send_work_cb(struct work_struct *ws)
{
	struct ntrdma_dev *dev = ntrdma_cmd_send_work_dev(ws);

	ntrdma_cmd_send_work(dev);
}

static void ntrdma_cmd_send_vbell_cb(void *ctx)
{
	struct ntrdma_dev *dev = ctx;

	schedule_work(&dev->cmd_send_work);
}

static int ntrdma_cmd_recv_none(struct ntrdma_dev *dev, u32 cmd_op,
				struct ntrdma_rsp_hdr *rsp)
{
	ntrdma_dbg(dev, "called\n");

	rsp->op = cmd_op;
	rsp->status = 0;

	return 0;
}

static int ntrdma_cmd_recv_mr_create(struct ntrdma_dev *dev,
				     struct ntrdma_cmd_mr_create *cmd,
				     struct ntrdma_rsp_mr_status *rsp)
{
	struct ntrdma_rmr *rmr;
	u32 i, count;
	int rc;

	ntrdma_dbg(dev, "called\n");

	rsp->hdr.op = cmd->op;
	rsp->mr_key = cmd->mr_key;

	rmr = kmalloc_node(sizeof(*rmr) + cmd->sg_cap * sizeof(*rmr->sg_list),
			   GFP_KERNEL, dev->node);
	if (!rmr) {
		rc = -ENOMEM;
		goto err_rmr;
	}

	rc = ntrdma_rmr_init(rmr, dev, cmd->pd_key, cmd->access,
			     cmd->mr_addr, cmd->mr_len, cmd->sg_cap,
			     cmd->mr_key);
	if (rc)
		goto err_init;

	count = cmd->sg_count;

	memcpy(rmr->sg_list, cmd->sg_list,
	       count * sizeof(*cmd->sg_list));

	for (i = 0; i < count; ++i)
		rmr->sg_list[i].addr =
			ntc_peer_addr(dev->ntc,
				      rmr->sg_list[i].addr);

	rc = ntrdma_rmr_add(rmr);
	if (rc)
		goto err_add;

	rsp->hdr.status = 0;
	return 0;

err_add:
	ntrdma_rmr_deinit(rmr);
err_init:
	kfree(rmr);
err_rmr:
	rsp->hdr.status = ~0;
	return rc;
}

static int ntrdma_cmd_recv_mr_delete(struct ntrdma_dev *dev,
				     struct ntrdma_cmd_mr_delete *cmd,
				     struct ntrdma_rsp_mr_status *rsp)
{
	struct ntrdma_rmr *rmr;
	int rc;

	ntrdma_dbg(dev, "called\n");

	rsp->hdr.op = cmd->op;
	rsp->mr_key = cmd->mr_key;

	rmr = ntrdma_dev_rmr_look(dev, cmd->mr_key);
	if (!rmr) {
		rc = -EINVAL;
		goto err_rmr;
	}

	ntrdma_rmr_del(rmr);
	ntrdma_rmr_put(rmr);
	ntrdma_rmr_repo(rmr);
	ntrdma_rmr_deinit(rmr);
	kfree(rmr);

	rsp->hdr.status = 0;
	return 0;

err_rmr:
	rsp->hdr.status = ~0;
	return rc;
}

static int ntrdma_cmd_recv_mr_append(struct ntrdma_dev *dev,
				     struct ntrdma_cmd_mr_append *cmd,
				     struct ntrdma_rsp_mr_status *rsp)
{
	struct ntrdma_rmr *rmr;
	u32 i, pos, count;
	int rc;

	ntrdma_dbg(dev, "called\n");

	rsp->hdr.op = cmd->op;
	rsp->mr_key = cmd->mr_key;

	rmr = ntrdma_dev_rmr_look(dev, cmd->mr_key);
	if (!rmr) {
		rc = -EINVAL;
		goto err_rmr;
	}

	pos = cmd->sg_pos;
	count = cmd->sg_count;

	memcpy(&rmr->sg_list[pos], cmd->sg_list,
	       count * sizeof(*cmd->sg_list));

	count += pos;
	for (i = pos; i < count; ++i)
		rmr->sg_list[i].addr =
			ntc_peer_addr(dev->ntc,
				      rmr->sg_list[i].addr);

	ntrdma_rmr_put(rmr);

	rsp->hdr.status = 0;
	return 0;

err_rmr:
	rsp->hdr.status = ~0;
	return rc;
}

static int ntrdma_cmd_recv_qp_create(struct ntrdma_dev *dev,
				     struct ntrdma_cmd_qp_create *cmd,
				     struct ntrdma_rsp_qp_create *rsp)
{
	struct ntrdma_rqp *rqp;
	struct ntrdma_rqp_init_attr attr;
	int rc;

	ntrdma_dbg(dev, "called\n");

	rsp->hdr.op = cmd->op;
	rsp->qp_key = cmd->qp_key;

	rqp = kmalloc_node(sizeof(*rqp), GFP_KERNEL, dev->node);
	if (!rqp) {
		rc = -ENOMEM;
		goto err_rqp;
	}

	attr.pd_key = cmd->pd_key;
	attr.recv_wqe_idx = cmd->recv_ring_idx;
	attr.recv_wqe_cap = cmd->recv_wqe_cap;
	attr.recv_wqe_sg_cap = cmd->recv_wqe_sg_cap;
	attr.send_wqe_idx = cmd->send_ring_idx;
	attr.send_wqe_cap = cmd->send_wqe_cap;
	attr.send_wqe_sg_cap = cmd->send_wqe_sg_cap;
	attr.peer_send_cqe_buf_addr =
		ntc_peer_addr(dev->ntc, cmd->send_cqe_buf_addr);
	attr.peer_send_cons_addr =
		ntc_peer_addr(dev->ntc, cmd->send_cons_addr);
	attr.peer_cmpl_vbell_idx = cmd->cmpl_vbell_idx;

	rc = ntrdma_rqp_init(rqp, dev, &attr, cmd->qp_key);
	if (rc)
		goto err_init;

	rsp->recv_wqe_buf_addr = rqp->recv_wqe_buf_addr;
	rsp->recv_prod_addr = rqp->recv_wqe_buf_addr +
		rqp->recv_cap * rqp->recv_wqe_size;
	rsp->send_wqe_buf_addr = rqp->send_wqe_buf_addr;
	rsp->send_prod_addr = rqp->send_wqe_buf_addr +
		rqp->send_cap * rqp->send_wqe_size;
	rsp->send_vbell_idx = rqp->send_vbell_idx;

	rc = ntrdma_rqp_add(rqp);
	if (rc)
		goto err_add;

	rsp->hdr.status = 0;
	return 0;

err_add:
	ntrdma_rqp_deinit(rqp);
err_init:
	kfree(rqp);
err_rqp:
	rsp->hdr.status = ~0;
	rsp->recv_wqe_buf_addr = 0;
	rsp->recv_prod_addr = 0;
	rsp->send_wqe_buf_addr = 0;
	rsp->send_prod_addr = 0;
	rsp->send_vbell_idx = 0;
	return rc;
}

static int ntrdma_cmd_recv_qp_delete(struct ntrdma_dev *dev,
				     struct ntrdma_cmd_qp_delete *cmd,
				     struct ntrdma_rsp_qp_status *rsp)
{
	struct ntrdma_rqp *rqp;
	int rc;

	ntrdma_dbg(dev, "called\n");

	rsp->hdr.op = cmd->op;
	rsp->qp_key = cmd->qp_key;

	rqp = ntrdma_dev_rqp_look(dev, cmd->qp_key);
	if (!rqp) {
		rc = -EINVAL;
		goto err_rqp;
	}

	ntrdma_rqp_del(rqp);
	ntrdma_rqp_put(rqp);
	ntrdma_rqp_repo(rqp);
	ntrdma_rqp_deinit(rqp);
	kfree(rqp);

	rsp->hdr.status = 0;
	return 0;

err_rqp:
	rsp->hdr.status = ~0;
	return rc;
}

static int ntrdma_cmd_recv_qp_modify(struct ntrdma_dev *dev,
				     struct ntrdma_cmd_qp_modify *cmd,
				     struct ntrdma_rsp_qp_status *rsp)
{
	struct ntrdma_rqp *rqp;
	int rc;

	ntrdma_dbg(dev, "called\n");

	rsp->hdr.op = cmd->op;
	rsp->qp_key = cmd->qp_key;

	rqp = ntrdma_dev_rqp_look(dev, cmd->qp_key);
	if (!rqp) {
		rc = -EINVAL;
		goto err_rqp;
	}

	rqp->state = cmd->state;
	rqp->recv_error = false;
	rqp->send_error = false;
	rqp->access = cmd->access;

	//rqp->access = cmd->access; /* TODO: qp access flags */
	rqp->qp_key = cmd->dest_qp_key;

	tasklet_schedule(&rqp->send_work);
	ntrdma_rqp_put(rqp);

	rsp->hdr.status = 0;
	return 0;

err_rqp:
	rsp->hdr.status = ~0;
	return rc;
}

static int ntrdma_cmd_recv(struct ntrdma_dev *dev, union ntrdma_cmd *cmd,
			   union ntrdma_rsp *rsp, void *req)
{
	switch (cmd->op) {
	case NTRDMA_CMD_NONE:
		return ntrdma_cmd_recv_none(dev, cmd->op, &rsp->hdr);
	case NTRDMA_CMD_MR_CREATE:
		return ntrdma_cmd_recv_mr_create(dev, &cmd->mr_create,
						 &rsp->mr_create);
	case NTRDMA_CMD_MR_DELETE:
		return ntrdma_cmd_recv_mr_delete(dev, &cmd->mr_delete,
						 &rsp->mr_delete);
	case NTRDMA_CMD_MR_APPEND:
		return ntrdma_cmd_recv_mr_append(dev, &cmd->mr_append,
						 &rsp->mr_append);
	case NTRDMA_CMD_QP_CREATE:
		return ntrdma_cmd_recv_qp_create(dev, &cmd->qp_create,
						 &rsp->qp_create);
	case NTRDMA_CMD_QP_DELETE:
		return ntrdma_cmd_recv_qp_delete(dev, &cmd->qp_delete,
						 &rsp->qp_delete);
	case NTRDMA_CMD_QP_MODIFY:
		return ntrdma_cmd_recv_qp_modify(dev, &cmd->qp_modify,
						 &rsp->qp_modify);
	}

	ntrdma_dbg(dev, "unhandled recv cmd op %u\n", cmd->op);

	return -EINVAL;
}

static inline void ntrdma_cmd_recv_vbell_clear(struct ntrdma_dev *dev)
{
	ntrdma_vdbg(dev, "called\n");
	ntrdma_dev_vbell_clear(dev, &dev->cmd_recv_vbell,
			       dev->cmd_recv_vbell_idx);
}

static inline int ntrdma_cmd_recv_vbell_add(struct ntrdma_dev *dev)
{
	ntrdma_vdbg(dev, "called\n");
	return ntrdma_dev_vbell_add(dev, &dev->cmd_recv_vbell,
				    dev->cmd_recv_vbell_idx);
}

static void ntrdma_cmd_recv_work(struct ntrdma_dev *dev)
{
	void *req;
	u32 start, pos, end, base;
	u64 dst, src;
	size_t off, len;
	int rc;

	ntrdma_vdbg(dev, "called\n");

	req = ntc_req_create(dev->ntc);
	if (!req)
		return; /* FIXME: no req, now what? */

	/* sync the ring buf for the cpu */

	ntc_buf_sync_cpu(dev->ntc,
			 dev->cmd_recv_buf_addr,
			 dev->cmd_recv_buf_size,
			 DMA_FROM_DEVICE);

	mutex_lock(&dev->cmd_recv_lock);
	{
		ntrdma_cmd_recv_vbell_clear(dev);

		/* Process commands */

		ntrdma_ring_consume(*dev->cmd_recv_prod_buf,
				    dev->cmd_recv_cons,
				    dev->cmd_recv_cap,
				    &start, &end, &base);
		ntrdma_vdbg(dev, "cmd start %d end %d\n", start, end);
		for (pos = start; pos < end; ++pos) {
			ntrdma_vdbg(dev, "cmd recv pos %d\n", pos);
			rc = ntrdma_cmd_recv(dev,
					     &dev->cmd_recv_buf[pos],
					     &dev->cmd_recv_rsp_buf[pos],
					     req);
			if (rc)
				; /* FIXME: command failed, now what? */
		}

		if (pos != start) {
			ntrdma_vdbg(dev, "rsp copy start %d pos %d\n",
				    start, pos);

			dev->cmd_recv_cons = ntrdma_ring_update(pos, base,
								dev->cmd_recv_cap);

			/* sync the ring buf for the device */

			ntc_buf_sync_dev(dev->ntc,
					 dev->cmd_recv_rsp_buf_addr,
					 dev->cmd_recv_rsp_buf_size,
					 DMA_TO_DEVICE);

			/* copy the portion of the ring buf */

			off = start * sizeof(union ntrdma_rsp);
			len = (pos - start) * sizeof(union ntrdma_rsp);
			dst = dev->peer_cmd_send_rsp_buf_addr + off;
			src = dev->cmd_recv_rsp_buf_addr + off;

			ntc_req_memcpy(dev->ntc, req,
				       dst, src, len,
				       true, NULL, NULL);

			/* update the producer index on the peer */

			ntc_req_imm32(dev->ntc, req,
				      dev->peer_cmd_send_cons_addr,
				      dev->cmd_recv_cons,
				      true, NULL, NULL);

			/* update the vbell and signal the peer */

			ntrdma_dev_vbell_peer(dev, req,
					      dev->peer_cmd_send_vbell_idx);
			ntc_req_signal(dev->ntc, req, NULL, NULL, NTB_DEFAULT_VEC(dev->ntc));

			ntc_req_submit(dev->ntc, req);
			schedule_work(&dev->cmd_recv_work);
		} else {
			ntc_req_cancel(dev->ntc, req);
			if (ntrdma_cmd_recv_vbell_add(dev) == -EAGAIN)
				schedule_work(&dev->cmd_recv_work);
		}
	}
	mutex_unlock(&dev->cmd_recv_lock);
}

static void ntrdma_cmd_recv_work_cb(struct work_struct *ws)
{
	struct ntrdma_dev *dev = ntrdma_cmd_recv_work_dev(ws);

	ntrdma_cmd_recv_work(dev);
}

static void ntrdma_cmd_recv_vbell_cb(void *ctx)
{
	struct ntrdma_dev *dev = ctx;

	schedule_work(&dev->cmd_recv_work);
}

