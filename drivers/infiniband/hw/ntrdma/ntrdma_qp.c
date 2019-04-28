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

#include "ntrdma_cmd.h"
#include "ntrdma_sg.h"
#include "ntrdma_wr.h"

#include "ntrdma_ring.h"

#include "ntrdma_dev.h"
#include "ntrdma_pd.h"
#include "ntrdma_mr.h"
#include "ntrdma_qp.h"
#include "ntrdma_cq.h"
#include "ntrdma_zip.h"

#include <linux/ntc_trace.h>

#define NTRDMA_QP_BATCH_SIZE 0x10

struct ntrdma_qp_cmd_cb {
	struct ntrdma_cmd_cb cb;
	struct ntrdma_qp *qp;
};

#define ntrdma_cmd_cb_qpcb(__cb) \
	container_of(__cb, struct ntrdma_qp_cmd_cb, cb)

static int ntrdma_qp_modify_prep(struct ntrdma_cmd_cb *cb,
				 union ntrdma_cmd *cmd, struct ntrdma_req *req);
static int ntrdma_qp_modify_cmpl(struct ntrdma_cmd_cb *cb,
				 union ntrdma_rsp *rsp, struct ntrdma_req *req);
static int ntrdma_qp_enable_prep(struct ntrdma_cmd_cb *cb,
				 union ntrdma_cmd *cmd, struct ntrdma_req *req);
static int ntrdma_qp_enable_cmpl(struct ntrdma_cmd_cb *cb,
				 union ntrdma_rsp *rsp, struct ntrdma_req *req);
static int ntrdma_qp_disable_prep(struct ntrdma_cmd_cb *cb,
				  union ntrdma_cmd *cmd, struct ntrdma_req *req);
static int ntrdma_qp_disable_cmpl(struct ntrdma_cmd_cb *cb,
				  union ntrdma_rsp *rsp, struct ntrdma_req *req);

static int ntrdma_qp_enable(struct ntrdma_res *res);
static int ntrdma_qp_disable(struct ntrdma_res *res);
static void ntrdma_qp_reset(struct ntrdma_res *res);

static void ntrdma_rqp_free(struct ntrdma_rres *rres);

static int ntrdma_qp_recv_prod_start(struct ntrdma_qp *qp);
static void ntrdma_qp_recv_prod_done(struct ntrdma_qp *qp);
static void ntrdma_qp_recv_prod_get(struct ntrdma_qp *qp,
				    u32 *pos, u32 *end, u32 *base);
static void ntrdma_qp_recv_prod_put(struct ntrdma_qp *qp,
				    u32 pos, u32 base);

static int ntrdma_qp_recv_cons_start(struct ntrdma_qp *qp);
static void ntrdma_qp_recv_cons_done(struct ntrdma_qp *qp);
static void ntrdma_qp_recv_cons_get(struct ntrdma_qp *qp,
				    u32 *pos, u32 *end, u32 *base);
static void ntrdma_qp_recv_cons_put(struct ntrdma_qp *qp,
				    u32 pos, u32 base);

static void ntrdma_qp_recv_cmpl_start(struct ntrdma_qp *qp);
static void ntrdma_qp_recv_cmpl_done(struct ntrdma_qp *qp);
static void ntrdma_qp_recv_cmpl_get(struct ntrdma_qp *qp,
				    u32 *pos, u32 *end, u32 *base);
static void ntrdma_qp_recv_cmpl_put(struct ntrdma_qp *qp,
				    u32 pos, u32 base);

static int ntrdma_qp_send_prod_start(struct ntrdma_qp *qp);
static void ntrdma_qp_send_prod_done(struct ntrdma_qp *qp);
static void ntrdma_qp_send_prod_get(struct ntrdma_qp *qp,
				    u32 *pos, u32 *end, u32 *base);
static void ntrdma_qp_send_prod_put(struct ntrdma_qp *qp,
				    u32 pos, u32 base);

static void ntrdma_qp_send_cmpl_start(struct ntrdma_qp *qp);
static void ntrdma_qp_send_cmpl_done(struct ntrdma_qp *qp);
static void ntrdma_qp_send_cmpl_get(struct ntrdma_qp *qp,
				    u32 *pos, u32 *end, u32 *base);
static void ntrdma_qp_send_cmpl_put(struct ntrdma_qp *qp,
				    u32 pos, u32 base);

static int ntrdma_rqp_recv_cons_start(struct ntrdma_rqp *rqp);
static void ntrdma_rqp_recv_cons_done(struct ntrdma_rqp *rqp);
static void ntrdma_rqp_recv_cons_get(struct ntrdma_rqp *rqp,
				     u32 *pos, u32 *end, u32 *base);
static void ntrdma_rqp_recv_cons_put(struct ntrdma_rqp *rqp,
				     u32 pos, u32 base);

static int ntrdma_rqp_send_cons_start(struct ntrdma_rqp *rqp);
static void ntrdma_rqp_send_cons_done(struct ntrdma_rqp *rqp);
static void ntrdma_rqp_send_cons_get(struct ntrdma_rqp *rqp,
				     u32 *pos, u32 *end, u32 *base);
static void ntrdma_rqp_send_cons_put(struct ntrdma_rqp *rqp,
				     u32 pos, u32 base);

static int ntrdma_qp_poll_recv_start_and_get(struct ntrdma_poll *poll,
					     struct ntrdma_qp **pqp, u32 *pos, u32 *end,
					     u32 *base);
static void ntrdma_qp_poll_recv_put_and_done(struct ntrdma_poll *poll,
					     u32 pos, u32 base);
static struct ntrdma_cqe *ntrdma_qp_poll_recv_cqe(struct ntrdma_poll *poll,
						  struct ntrdma_cqe *abort_cqe, u32 pos);

static int ntrdma_qp_poll_send_start_and_get(struct ntrdma_poll *poll,
					     struct ntrdma_qp **pqp, u32 *pos, u32 *end,
					     u32 *base);
static void ntrdma_qp_poll_send_put_and_done(struct ntrdma_poll *poll,
					     u32 pos, u32 base);
static struct ntrdma_cqe *ntrdma_qp_poll_send_cqe(struct ntrdma_poll *poll,
						  struct ntrdma_cqe *abort_cqe, u32 pos);

static void ntrdma_qp_recv_work(struct ntrdma_qp *qp);
static void ntrdma_qp_send_work(struct ntrdma_qp *qp);
static void ntrdma_rqp_send_work(struct ntrdma_rqp *rqp);

static void ntrdma_qp_work_cb(unsigned long ptrhld);
static void ntrdma_rqp_work_cb(unsigned long ptrhld);
static void ntrdma_rqp_vbell_cb(void *ctx);

static inline int ntrdma_qp_init_deinit(struct ntrdma_qp *qp,
		struct ntrdma_dev *dev,
		struct ntrdma_cq *recv_cq, struct ntrdma_cq *send_cq,
		struct ntrdma_qp_init_attr *attr,
		int is_deinit)
{
	int rc;
	int reserved_key = -1;

	if (is_deinit)
		goto deinit;

	rc = ntrdma_res_init(&qp->res, dev, &dev->qp_vec,
			ntrdma_qp_enable,
			ntrdma_qp_disable,
			ntrdma_qp_reset,
			reserved_key);
	if (rc)
		goto err_res;

	ntrdma_cq_get(recv_cq);
	qp->recv_cq = recv_cq;
	qp->recv_poll.poll_cqe = ntrdma_qp_poll_recv_cqe;
	qp->recv_poll.poll_start_and_get = ntrdma_qp_poll_recv_start_and_get;
	qp->recv_poll.poll_put_and_done = ntrdma_qp_poll_recv_put_and_done;

	ntrdma_cq_get(send_cq);
	qp->send_cq = send_cq;
	qp->send_poll.poll_cqe = ntrdma_qp_poll_send_cqe;
	qp->send_poll.poll_start_and_get = ntrdma_qp_poll_send_start_and_get;
	qp->send_poll.poll_put_and_done = ntrdma_qp_poll_send_put_and_done;

	qp->state = 0;
	qp->recv_error = false;
	qp->recv_abort = false;
	qp->send_error = false;
	qp->send_abort = false;
	qp->access = 0;

	qp->rqp_key = -1;

	qp->send_wqe_sg_cap = attr->send_wqe_sg_cap;
	qp->recv_wqe_sg_cap = attr->recv_wqe_sg_cap;
	qp->send_wqe_size = ntrdma_send_wqe_size(qp->send_wqe_sg_cap);
	qp->recv_wqe_size = ntrdma_recv_wqe_size(qp->recv_wqe_sg_cap);

	/* set up the send work ring */

	qp->send_cap = attr->send_wqe_cap;
	qp->send_post = 0;
	qp->send_prod = 0;
	qp->send_cmpl = 0;

	/* set up the send work queue buffer */

	qp->send_wqe_buf_size = qp->send_cap * qp->send_wqe_size;

	qp->send_wqe_buf = kzalloc_node(qp->send_wqe_buf_size,
					GFP_KERNEL, dev->node);
	if (!qp->send_wqe_buf) {
		rc = -ENOMEM;
		goto err_send_wqe_buf;
	}

	/*Accessed by local (DMA)*/
	qp->send_wqe_buf_addr = ntc_buf_map(dev->ntc,
			qp->send_wqe_buf,
			qp->send_wqe_buf_size,
			DMA_TO_DEVICE,
			IOAT_DEV_ACCESS);

	if (!qp->send_wqe_buf_addr) {
		rc = -EIO;
		goto err_send_wqe_buf_addr;
	}

	/* set up the send completion queue buffer */

	qp->send_cqe_buf_size = qp->send_cap * sizeof(struct ntrdma_cqe)
		+ sizeof(*qp->send_cons_buf); /* space at end for cons buf */

	qp->send_cqe_buf = kzalloc_node(qp->send_cqe_buf_size,
					GFP_KERNEL, dev->node);
	if (!qp->send_cqe_buf) {
		rc = -ENOMEM;
		goto err_send_cqe_buf;
	}

	qp->send_cons_buf = (void *)(qp->send_cqe_buf
				     + qp->send_cap * sizeof(struct ntrdma_cqe));

	*qp->send_cons_buf = 0;

	qp->send_cqe_buf_addr = ntc_buf_map(dev->ntc,
					    qp->send_cqe_buf,
					    qp->send_cqe_buf_size,
					    DMA_FROM_DEVICE,
						NTB_DEV_ACCESS);
	if (!qp->send_cqe_buf_addr) {
		rc = -EIO;
		goto err_send_cqe_buf_addr;
	}

	/* peer rqp send queue is zero until enabled */

	qp->peer_send_wqe_buf_dma_addr = 0;
	qp->peer_send_prod_dma_addr = 0;
	qp->peer_send_vbell_idx = 0;

	/* set up the recv work ring */

	qp->recv_cap = attr->recv_wqe_cap;
	qp->recv_post = 0;
	qp->recv_prod = 0;
	qp->recv_cons = 0;
	qp->recv_cmpl = 0;

	/* set up the recv work queue buffer */

	qp->recv_wqe_buf_size = qp->recv_cap * qp->recv_wqe_size;

	qp->recv_wqe_buf = kzalloc_node(qp->recv_wqe_buf_size,
					GFP_KERNEL, dev->node);
	if (!qp->recv_wqe_buf) {
		rc = -ENOMEM;
		goto err_recv_wqe_buf;
	}

	qp->recv_wqe_buf_addr = ntc_buf_map(dev->ntc,
					    qp->recv_wqe_buf,
					    qp->recv_wqe_buf_size,
					    DMA_TO_DEVICE,
						IOAT_DEV_ACCESS);
	if (!qp->recv_wqe_buf_addr) {
		rc = -EIO;
		goto err_recv_wqe_buf_addr;
	}

	/* set up the recv completion queue buffer */

	qp->recv_cqe_buf_size = qp->recv_cap * sizeof(struct ntrdma_cqe);

	qp->recv_cqe_buf = kzalloc_node(qp->recv_cqe_buf_size,
					GFP_KERNEL, dev->node);
	if (!qp->recv_cqe_buf) {
		rc = -ENOMEM;
		goto err_recv_cqe_buf;
	}

	/* peer rqp recv queue is zero until enabled */

	qp->peer_recv_wqe_buf_dma_addr = 0;
	qp->peer_recv_prod_dma_addr = 0;

	/* initialize synchronization */

	mutex_init(&qp->send_post_lock);
	spin_lock_init(&qp->send_prod_lock);
	mutex_init(&qp->send_cmpl_lock);
	mutex_init(&qp->recv_post_lock);
	spin_lock_init(&qp->recv_prod_lock);
	spin_lock_init(&qp->recv_cons_lock);
	mutex_init(&qp->recv_cmpl_lock);

	/* add qp to completion queues for polling */

	/* TODO: add these during qp modify */
	ntrdma_cq_add_poll(qp->recv_cq, &qp->recv_poll);
	ntrdma_cq_add_poll(qp->send_cq, &qp->send_poll);

	tasklet_init(&qp->send_work,
		     ntrdma_qp_work_cb,
		     to_ptrhld(qp));

	return 0;

deinit:
	tasklet_kill(&qp->send_work);
	ntrdma_cq_del_poll(qp->send_cq, &qp->send_poll);
	ntrdma_cq_del_poll(qp->recv_cq, &qp->recv_poll);
	kfree(qp->recv_cqe_buf);
	qp->recv_cqe_buf = 0;
err_recv_cqe_buf:
	ntc_buf_unmap(dev->ntc,
			qp->recv_wqe_buf_addr,
			qp->recv_wqe_buf_size,
			DMA_TO_DEVICE,
			IOAT_DEV_ACCESS);
	qp->recv_wqe_buf_addr = 0;
err_recv_wqe_buf_addr:
	kfree(qp->recv_wqe_buf);
	qp->recv_wqe_buf = 0;
err_recv_wqe_buf:
	ntc_buf_unmap(dev->ntc,
			qp->send_cqe_buf_addr,
			qp->send_cqe_buf_size,
			DMA_FROM_DEVICE,
			NTB_DEV_ACCESS);
	qp->send_cqe_buf_addr = 0;
err_send_cqe_buf_addr:
	kfree(qp->send_cqe_buf);
	qp->send_cqe_buf = 0;
err_send_cqe_buf:
	ntc_buf_unmap(dev->ntc,
		      qp->send_wqe_buf_addr,
		      qp->send_wqe_buf_size,
		      DMA_TO_DEVICE,
			  IOAT_DEV_ACCESS);
	qp->send_wqe_buf_addr = 0;
err_send_wqe_buf_addr:
	kfree(qp->send_wqe_buf);
	qp->send_wqe_buf = 0;
err_send_wqe_buf:
	ntrdma_cq_put(qp->send_cq);
	ntrdma_cq_put(qp->recv_cq);
	ntrdma_res_deinit(&qp->res);
err_res:
	return rc;
}

int ntrdma_qp_init(struct ntrdma_qp *qp, struct ntrdma_dev *dev,
		   struct ntrdma_cq *recv_cq, struct ntrdma_cq *send_cq,
		   struct ntrdma_qp_init_attr *attr)
{
	return ntrdma_qp_init_deinit(qp, dev, recv_cq, send_cq, attr, false);
}

void ntrdma_qp_deinit(struct ntrdma_qp *qp)
{
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);

	ntrdma_qp_init_deinit(qp, dev, NULL, NULL, NULL, true);
}

struct ntrdma_send_wqe *ntrdma_qp_send_wqe(struct ntrdma_qp *qp,
					   u32 pos)
{
	return (void *)(qp->send_wqe_buf + pos * qp->send_wqe_size);
}

struct ntrdma_cqe *ntrdma_qp_send_cqe(struct ntrdma_qp *qp,
				      u32 pos)
{
	return (void *)(qp->send_cqe_buf + pos * sizeof(struct ntrdma_cqe));
}

struct ntrdma_recv_wqe *ntrdma_qp_recv_wqe(struct ntrdma_qp *qp,
					   u32 pos)
{
	return (void *)(qp->recv_wqe_buf + pos * qp->recv_wqe_size);
}

struct ntrdma_cqe *ntrdma_qp_recv_cqe(struct ntrdma_qp *qp,
				      u32 pos)
{
	return (void *)(qp->recv_cqe_buf + pos * sizeof(struct ntrdma_cqe));
}

int ntrdma_qp_modify(struct ntrdma_qp *qp)
{
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);
	struct ntrdma_qp_cmd_cb *qpcb;
	int rc;

	ntrdma_res_start_cmds(&qp->res);

	qpcb = kmalloc_node(sizeof(*qpcb),
			    GFP_KERNEL, dev->node);
	if (!qpcb) {
		rc = -ENOMEM;
		goto err;
	}

	qpcb->cb.cmd_prep = ntrdma_qp_modify_prep;
	qpcb->cb.rsp_cmpl = ntrdma_qp_modify_cmpl;
	qpcb->qp = qp;

	ntrdma_dev_cmd_add(dev, &qpcb->cb);

	return 0;

err:
	ntrdma_res_done_cmds(&qp->res);
	return rc;
}

static int ntrdma_qp_modify_prep(struct ntrdma_cmd_cb *cb,
				 union ntrdma_cmd *cmd, struct ntrdma_req *req)
{
	struct ntrdma_qp_cmd_cb *qpcb = ntrdma_cmd_cb_qpcb(cb);
	struct ntrdma_qp *qp = qpcb->qp;
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);

	ntrdma_vdbg(dev, "called\n");

	ntrdma_qp_recv_work(qp);

	cmd->qp_modify.op = NTRDMA_CMD_QP_MODIFY;
	cmd->qp_modify.qp_key = qp->res.key;
	cmd->qp_modify.access = qp->access;
	cmd->qp_modify.state = qp->state;
	cmd->qp_modify.dest_qp_key = qp->rqp_key;

	return 0;
}

static int ntrdma_qp_modify_cmpl(struct ntrdma_cmd_cb *cb,
				 union ntrdma_rsp *rsp, struct ntrdma_req *req)
{
	struct ntrdma_qp_cmd_cb *qpcb = ntrdma_cmd_cb_qpcb(cb);
	struct ntrdma_qp *qp = qpcb->qp;
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);
	int rc;

	ntrdma_vdbg(dev, "called\n");

	if (!rsp || rsp->hdr.status) {
		ntrdma_err(dev, "rsp %p status %d\n",
				rsp, rsp->hdr.status);
		rc = -EIO;
		goto err;
	}

	ntrdma_res_done_cmds(&qp->res);
	kfree(qpcb);

	return 0;

err:
	ntrdma_res_done_cmds(&qp->res);

	ntrdma_qp_recv_work(qp);
	return rc;
}

static int ntrdma_qp_enable(struct ntrdma_res *res)
{
	struct ntrdma_dev *dev = ntrdma_res_dev(res);
	struct ntrdma_qp *qp = ntrdma_res_qp(res);
	struct ntrdma_qp_cmd_cb *qpcb;
	int rc;

	ntrdma_res_start_cmds(&qp->res);

	qpcb = kmalloc_node(sizeof(*qpcb),
			    GFP_KERNEL, dev->node);
	if (!qpcb) {
		rc = -ENOMEM;
		goto err;
	}

	qpcb->cb.cmd_prep = ntrdma_qp_enable_prep;
	qpcb->cb.rsp_cmpl = ntrdma_qp_enable_cmpl;
	qpcb->qp = qp;

	ntrdma_dev_cmd_add(dev, &qpcb->cb);

	return 0;

err:
	ntrdma_res_done_cmds(&qp->res);
	return rc;
}

static int ntrdma_qp_enable_prep(struct ntrdma_cmd_cb *cb,
				 union ntrdma_cmd *cmd, struct ntrdma_req *req)
{
	struct ntrdma_qp_cmd_cb *qpcb = ntrdma_cmd_cb_qpcb(cb);
	struct ntrdma_qp *qp = qpcb->qp;
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);

	ntrdma_vdbg(dev, "called\n");
	TRACE("qp_enable prep: %d", qp->res.key);

	cmd->qp_create.op = NTRDMA_CMD_QP_CREATE;
	cmd->qp_create.qp_key = qp->res.key;
	cmd->qp_create.pd_key = qp->pd_key;
	cmd->qp_create.qp_type = 0; /* TODO: just RC for now */
	cmd->qp_create.recv_wqe_cap = qp->recv_cap;
	cmd->qp_create.recv_wqe_sg_cap = qp->recv_wqe_sg_cap;
	cmd->qp_create.recv_ring_idx = qp->recv_cons;
	cmd->qp_create.send_wqe_cap = qp->send_cap;
	cmd->qp_create.send_wqe_sg_cap = qp->send_wqe_sg_cap;
	cmd->qp_create.send_ring_idx = *qp->send_cons_buf;
	cmd->qp_create.send_cqe_buf_addr = qp->send_cqe_buf_addr;
	cmd->qp_create.send_cons_addr = qp->send_cqe_buf_addr
		+ qp->send_cap * sizeof(struct ntrdma_cqe);
	cmd->qp_create.cmpl_vbell_idx = qp->send_cq->vbell_idx;

	return 0;
}

static int ntrdma_qp_enable_disable_cmpl_common(struct ntrdma_qp *qp,
		struct ntrdma_dev *dev, union ntrdma_rsp *rsp,
		int is_disable)
{
	int rc;
	u64 peer_recv_wqe_buf_phys_addr;
	u64 peer_recv_prod_phys_addr;
	u64 peer_send_wqe_buf_phys_addr;
	u64 peer_send_prod_phys_addr;

	if (is_disable)
		goto disable;

	peer_recv_wqe_buf_phys_addr =
		ntc_peer_addr(dev->ntc, rsp->qp_create.recv_wqe_buf_addr);
	peer_recv_prod_phys_addr =
		ntc_peer_addr(dev->ntc, rsp->qp_create.recv_prod_addr);
	peer_send_wqe_buf_phys_addr =
		ntc_peer_addr(dev->ntc, rsp->qp_create.send_wqe_buf_addr);
	peer_send_prod_phys_addr =
		ntc_peer_addr(dev->ntc, rsp->qp_create.send_prod_addr);

	qp->peer_recv_wqe_buf_dma_addr =
		ntc_resource_map(dev->ntc,
				peer_recv_wqe_buf_phys_addr,
				qp->recv_wqe_buf_size,
				DMA_FROM_DEVICE,
				IOAT_DEV_ACCESS);
	if (unlikely(!qp->peer_recv_wqe_buf_dma_addr)) {
		rc = -EIO;
		goto err_peer_recv_wqe_buf_dma_addr;
	}

	qp->peer_recv_prod_dma_addr =
		ntc_resource_map(dev->ntc,
				peer_recv_prod_phys_addr,
				sizeof(u32),
				DMA_FROM_DEVICE,
				IOAT_DEV_ACCESS);
	if (unlikely(!qp->peer_recv_prod_dma_addr)) {
		rc = -EIO;
		goto err_peer_recv_prod_dma_addr;
	}

	qp->peer_send_wqe_buf_dma_addr =
		ntc_resource_map(dev->ntc,
				peer_send_wqe_buf_phys_addr,
				qp->send_wqe_buf_size,
				DMA_FROM_DEVICE,
				IOAT_DEV_ACCESS);
	if (unlikely(!qp->peer_send_wqe_buf_dma_addr)) {
		rc = -EIO;
		goto err_peer_send_wqe_buf_dma_addr;
	}

	qp->peer_send_prod_dma_addr =
		ntc_resource_map(dev->ntc,
				peer_send_prod_phys_addr,
				sizeof(u32),
				DMA_FROM_DEVICE,
				IOAT_DEV_ACCESS);
	if (unlikely(!qp->peer_send_prod_dma_addr)) {
		rc = -EIO;
		goto err_peer_send_prod_dma_addr;
	}

	qp->peer_send_vbell_idx = rsp->qp_create.send_vbell_idx;


	return 0;
disable:
	ntc_resource_unmap(dev->ntc,
			qp->peer_send_prod_dma_addr,
			sizeof(u32),
			DMA_FROM_DEVICE,
			IOAT_DEV_ACCESS);
	qp->peer_send_prod_dma_addr = 0;
err_peer_send_prod_dma_addr:
	ntc_resource_unmap(dev->ntc,
			qp->peer_send_wqe_buf_dma_addr,
			qp->send_wqe_buf_size,
			DMA_FROM_DEVICE,
			IOAT_DEV_ACCESS);
	qp->peer_send_wqe_buf_dma_addr = 0;
err_peer_send_wqe_buf_dma_addr:
	ntc_resource_unmap(dev->ntc,
			qp->peer_recv_prod_dma_addr,
			sizeof(u32),
			DMA_FROM_DEVICE,
			IOAT_DEV_ACCESS);
	qp->peer_recv_prod_dma_addr = 0;
err_peer_recv_prod_dma_addr:
	ntc_resource_unmap(dev->ntc,
			qp->peer_recv_wqe_buf_dma_addr,
			qp->recv_wqe_buf_size,
			DMA_FROM_DEVICE,
			IOAT_DEV_ACCESS);
	qp->peer_recv_wqe_buf_dma_addr = 0;
err_peer_recv_wqe_buf_dma_addr:
	ntrdma_res_done_cmds(&qp->res);
	return rc;
}

static int ntrdma_qp_enable_cmpl(struct ntrdma_cmd_cb *cb,
				 union ntrdma_rsp *rsp, struct ntrdma_req *req)
{
	struct ntrdma_qp_cmd_cb *qpcb = ntrdma_cmd_cb_qpcb(cb);
	struct ntrdma_qp *qp = qpcb->qp;
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);
	int rc;

	ntrdma_vdbg(dev, "called\n");

	TRACE("qp_enable cmpl: %d", qp->res.key);

	if (!rsp || rsp->hdr.status)
		return -EIO;

	rc = ntrdma_qp_enable_disable_cmpl_common(qp, dev, rsp, false);
	if (rc)
		return rc;

	if (qp->state > NTRDMA_QPS_RESET) {
		qpcb->cb.cmd_prep = ntrdma_qp_modify_prep;
		qpcb->cb.rsp_cmpl = ntrdma_qp_modify_cmpl;
		ntrdma_dev_cmd_add_unsafe(dev, &qpcb->cb);
	} else {
		ntrdma_res_done_cmds(&qp->res);
		kfree(qpcb);
		ntrdma_qp_recv_work(qp);
	}



	return 0;
}

static int ntrdma_qp_disable(struct ntrdma_res *res)
{
	struct ntrdma_dev *dev = ntrdma_res_dev(res);
	struct ntrdma_qp *qp = ntrdma_res_qp(res);
	struct ntrdma_qp_cmd_cb *qpcb;
	int rc;

	spin_lock_bh(&qp->recv_prod_lock);
	qp->recv_error = true;
	spin_unlock_bh(&qp->recv_prod_lock);

	spin_lock_bh(&qp->send_prod_lock);
	qp->send_error = true;
	spin_unlock_bh(&qp->send_prod_lock);

	ntrdma_res_start_cmds(&qp->res);

	qpcb = kmalloc_node(sizeof(*qpcb),
			    GFP_KERNEL, dev->node);
	if (!qpcb) {
		rc = -ENOMEM;
		goto err;
	}

	qpcb->cb.cmd_prep = ntrdma_qp_disable_prep;
	qpcb->cb.rsp_cmpl = ntrdma_qp_disable_cmpl;
	qpcb->qp = qp;

	ntrdma_dev_cmd_add(dev, &qpcb->cb);

	return 0;

err:
	ntrdma_res_done_cmds(&qp->res);
	return rc;
}

static int ntrdma_qp_disable_prep(struct ntrdma_cmd_cb *cb,
				  union ntrdma_cmd *cmd, struct ntrdma_req *req)
{
	struct ntrdma_qp_cmd_cb *qpcb = ntrdma_cmd_cb_qpcb(cb);
	struct ntrdma_qp *qp = qpcb->qp;
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);

	ntrdma_vdbg(dev, "called\n");

	cmd->qp_delete.op = NTRDMA_CMD_QP_DELETE;
	cmd->qp_delete.qp_key = qp->res.key;

	return 0;
}

static int ntrdma_qp_disable_cmpl(struct ntrdma_cmd_cb *cb,
				  union ntrdma_rsp *rsp, struct ntrdma_req *req)
{
	struct ntrdma_qp_cmd_cb *qpcb = ntrdma_cmd_cb_qpcb(cb);
	struct ntrdma_qp *qp = qpcb->qp;
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);
	int rc;

	ntrdma_vdbg(dev, "called\n");

	if (!rsp || rsp->hdr.status) {
		rc = -EIO;
		goto err;
	}

	ntrdma_qp_enable_disable_cmpl_common(qp, dev, NULL, true);

	ntrdma_res_done_cmds(&qp->res);
	kfree(qpcb);

	return 0;

err:
	ntrdma_res_done_cmds(&qp->res);
	return rc;
}

static void ntrdma_qp_reset(struct ntrdma_res *res)
{
	struct ntrdma_qp *qp = ntrdma_res_qp(res);

	spin_lock_bh(&qp->recv_prod_lock);
	{
		qp->recv_error = true;
		qp->peer_recv_wqe_buf_dma_addr = 0;
		qp->peer_recv_prod_dma_addr = 0;
	}
	spin_unlock_bh(&qp->recv_prod_lock);

	spin_lock_bh(&qp->send_prod_lock);
	{
		qp->send_error = true;
		qp->peer_send_wqe_buf_dma_addr = 0;
		qp->peer_send_prod_dma_addr = 0;
		qp->peer_send_vbell_idx = 0;
	}
	spin_unlock_bh(&qp->send_prod_lock);
}

static void ntrdma_rqp_free(struct ntrdma_rres *rres)
{
	struct ntrdma_rqp *rqp = ntrdma_rres_rqp(rres);

	ntrdma_rqp_del(rqp);
	ntrdma_rqp_deinit(rqp);
	kfree(rqp);
}

static inline int ntrdma_rqp_init_deinit(struct ntrdma_rqp *rqp,
		struct ntrdma_dev *dev,
		struct ntrdma_rqp_init_attr *attr,
		u32 key, int is_deinit)
{
	int rc;

	if (is_deinit)
		goto deinit;

	rc = ntrdma_rres_init(&rqp->rres, dev, &dev->rqp_vec,
			      ntrdma_rqp_free, key);
	if (rc)
		goto err_rres;

	rqp->pd_key = attr->pd_key;
	rqp->access = 0;

	rqp->state = 0;
	rqp->recv_error = 0;
	rqp->send_error = 0;

	rqp->qp_key = -1;

	rqp->send_wqe_sg_cap = attr->send_wqe_sg_cap;
	rqp->recv_wqe_sg_cap = attr->recv_wqe_sg_cap;
	rqp->send_wqe_size = ntrdma_send_wqe_size(rqp->send_wqe_sg_cap);
	rqp->recv_wqe_size = ntrdma_recv_wqe_size(rqp->recv_wqe_sg_cap);

	/* set up the send work ring */

	rqp->send_cap = attr->send_wqe_cap;
	rqp->send_cons = attr->send_wqe_idx;

	/* set up the send work queue buffer */

	rqp->send_wqe_buf_size = rqp->send_cap * rqp->send_wqe_size
		+ sizeof(*rqp->send_prod_buf); /* space at end for prod buf */

	rqp->send_wqe_buf = kzalloc_node(rqp->send_wqe_buf_size,
					 GFP_KERNEL, dev->node);
	if (!rqp->send_wqe_buf) {
		rc = -ENOMEM;
		goto err_send_wqe_buf;
	}

	rqp->send_prod_buf = (void *)(rqp->send_wqe_buf
				      + rqp->send_cap * rqp->send_wqe_size);

	*rqp->send_prod_buf = attr->send_wqe_idx;

	/* Accessed by remote (NTB)*/
	rqp->send_wqe_buf_addr = ntc_buf_map(dev->ntc,
			rqp->send_wqe_buf,
			rqp->send_wqe_buf_size,
			DMA_FROM_DEVICE,
			NTB_DEV_ACCESS);

	if (!rqp->send_wqe_buf_addr) {
		rc = -EIO;
		goto err_send_wqe_buf_addr;
	}

	/* set up the send completion queue buffer */

	rqp->send_cqe_buf_size = rqp->send_cap * sizeof(struct ntrdma_cqe);

	rqp->send_cqe_buf = kzalloc_node(rqp->send_cqe_buf_size,
					 GFP_KERNEL, dev->node);
	if (!rqp->send_cqe_buf) {
		rc = -ENOMEM;
		goto err_send_cqe_buf;
	}

	rqp->send_cqe_buf_addr = ntc_buf_map(dev->ntc,
					     rqp->send_cqe_buf,
					     rqp->send_cqe_buf_size,
					     DMA_TO_DEVICE,
						 IOAT_DEV_ACCESS);
	if (!rqp->send_cqe_buf_addr) {
		rc = -EIO;
		goto err_send_cqe_buf_addr;
	}

	/* peer qp send queue info is provided */

	rqp->peer_send_cqe_buf_dma_addr = attr->peer_send_cqe_buf_dma_addr;
	rqp->peer_send_cons_dma_addr = attr->peer_send_cons_dma_addr;
	rqp->peer_cmpl_vbell_idx = attr->peer_cmpl_vbell_idx;

	/* set up the recv work ring */

	rqp->recv_cap = attr->recv_wqe_cap;
	rqp->recv_cons = attr->recv_wqe_idx;

	/* set up the recv work queue buffer */

	rqp->recv_wqe_buf_size = rqp->recv_cap * rqp->recv_wqe_size
		+ sizeof(*rqp->recv_prod_buf); /* space at end for prod buf */

	rqp->recv_wqe_buf = kzalloc_node(rqp->recv_wqe_buf_size,
					 GFP_KERNEL, dev->node);
	if (!rqp->recv_wqe_buf) {
		rc = -ENOMEM;
		goto err_recv_wqe_buf;
	}

	rqp->recv_prod_buf = (u32 *)(rqp->recv_wqe_buf
				     + rqp->recv_cap * rqp->recv_wqe_size);

	*rqp->recv_prod_buf = attr->recv_wqe_idx;

	rqp->recv_wqe_buf_addr = ntc_buf_map(dev->ntc,
					     rqp->recv_wqe_buf,
					     rqp->recv_wqe_buf_size,
					     DMA_FROM_DEVICE,
						 NTB_DEV_ACCESS);
	if (!rqp->recv_wqe_buf_addr) {
		rc = -EIO;
		goto err_recv_wqe_buf_addr;
	}

	/* initialize synchronization */

	spin_lock_init(&rqp->send_cons_lock);
	spin_lock_init(&rqp->recv_cons_lock);

	/* initialize send work processing */

	ntrdma_vbell_init(&rqp->send_vbell,
			  ntrdma_rqp_vbell_cb, rqp);
	rqp->send_vbell_idx = ntrdma_dev_vbell_next(dev);

	tasklet_init(&rqp->send_work,
		     ntrdma_rqp_work_cb,
		     to_ptrhld(rqp));

	return 0;
deinit:
	tasklet_kill(&rqp->send_work);
	ntc_buf_unmap(dev->ntc,
			rqp->recv_wqe_buf_addr,
			rqp->recv_wqe_buf_size,
			DMA_FROM_DEVICE,
			NTB_DEV_ACCESS);
	rqp->recv_wqe_buf_addr = 0;
err_recv_wqe_buf_addr:
	kfree(rqp->recv_wqe_buf);
	rqp->recv_wqe_buf = 0;
err_recv_wqe_buf:
	ntc_buf_unmap(dev->ntc,
			rqp->send_cqe_buf_addr,
			rqp->send_cqe_buf_size,
			DMA_TO_DEVICE,
			IOAT_DEV_ACCESS);
	rqp->send_cqe_buf_addr = 0;
err_send_cqe_buf_addr:
	kfree(rqp->send_cqe_buf);
	rqp->send_cqe_buf = 0;
err_send_cqe_buf:
	ntc_buf_unmap(dev->ntc,
			rqp->send_wqe_buf_addr,
			rqp->send_wqe_buf_size,
			DMA_FROM_DEVICE,
			NTB_DEV_ACCESS);
	rqp->send_wqe_buf_addr = 0;
err_send_wqe_buf_addr:
	kfree(rqp->send_wqe_buf);
	rqp->send_wqe_buf = 0;
err_send_wqe_buf:
	ntrdma_rres_deinit(&rqp->rres);
err_rres:
	return rc;
}

int ntrdma_rqp_init(struct ntrdma_rqp *rqp, struct ntrdma_dev *dev,
		    struct ntrdma_rqp_init_attr *attr, u32 key)
{
	return ntrdma_rqp_init_deinit(rqp, dev, attr, key, false);
}

void ntrdma_rqp_deinit(struct ntrdma_rqp *rqp)
{
	struct ntrdma_dev *dev = ntrdma_rqp_dev(rqp);

	ntrdma_rqp_init_deinit(rqp, dev, NULL, 0, true);
}

void ntrdma_rqp_del(struct ntrdma_rqp *rqp)
{
	struct ntrdma_dev *dev = ntrdma_rqp_dev(rqp);

	rqp->state = NTRDMA_QPS_RESET;

	ntrdma_dev_vbell_del(dev, &rqp->send_vbell);

	tasklet_kill(&rqp->send_work);

	ntrdma_rres_del(&rqp->rres);
	ntrdma_debugfs_rqp_del(rqp);
}

struct ntrdma_recv_wqe *ntrdma_rqp_recv_wqe(struct ntrdma_rqp *rqp,
					    u32 pos)
{
	return (void *)(rqp->recv_wqe_buf + pos * rqp->recv_wqe_size);
}

struct ntrdma_send_wqe *ntrdma_rqp_send_wqe(struct ntrdma_rqp *rqp,
					    u32 pos)
{
	return (void *)(rqp->send_wqe_buf + pos * rqp->send_wqe_size);
}

struct ntrdma_cqe *ntrdma_rqp_send_cqe(struct ntrdma_rqp *rqp,
				       u32 pos)
{
	return (void *)(rqp->send_cqe_buf + pos * sizeof(struct ntrdma_cqe));
}

static void ntrdma_send_fail(struct ntrdma_cqe *cqe,
			     struct ntrdma_send_wqe *wqe, int op_status)
{
	cqe->ulp_handle = wqe->ulp_handle;
	cqe->op_code = wqe->op_code;
	cqe->op_status = op_status;
	cqe->rdma_len = 0;
	cqe->imm_data = 0;
}

static void ntrdma_send_done(struct ntrdma_cqe *cqe,
			     struct ntrdma_send_wqe *wqe,
			     u32 rdma_len)
{
	cqe->ulp_handle = wqe->ulp_handle;
	cqe->op_code = wqe->op_code;
	cqe->op_status = NTRDMA_WC_SUCCESS;
	cqe->rdma_len = rdma_len;
	cqe->imm_data = 0;
	cqe->flags = wqe->flags;
}

static void ntrdma_recv_fail(struct ntrdma_cqe *recv_cqe,
			     struct ntrdma_recv_wqe *recv_wqe, int op_status)
{
	recv_cqe->ulp_handle = recv_wqe->ulp_handle;
	recv_cqe->op_code = recv_wqe->op_code;
	recv_cqe->op_status = op_status;
	recv_cqe->rdma_len = 0;
	recv_cqe->imm_data = 0;
}

static u16 ntrdma_send_recv_opcode(struct ntrdma_send_wqe *send_wqe)
{
	switch (send_wqe->op_code) {
	case NTRDMA_WR_SEND_INV:
		return NTRDMA_WR_RECV_INV;
	case NTRDMA_WR_SEND_IMM:
		return NTRDMA_WR_RECV_IMM;
	case NTRDMA_WR_SEND_RDMA:
		return NTRDMA_WR_RECV_RDMA;
	}
	return NTRDMA_WR_RECV;
}

static u32 ntrdma_send_recv_len(struct ntrdma_send_wqe *send_wqe)
{
	switch (send_wqe->op_code) {
	case NTRDMA_WR_SEND:
	case NTRDMA_WR_SEND_INV:
	case NTRDMA_WR_SEND_IMM:
		return send_wqe->rdma_len;
	}
	return 0;
}

static u32 ntrdma_send_recv_imm(struct ntrdma_send_wqe *send_wqe)
{
	switch (send_wqe->op_code) {
	case NTRDMA_WR_SEND_INV:
	case NTRDMA_WR_SEND_IMM:
	case NTRDMA_WR_SEND_RDMA:
		return send_wqe->imm_data;
	}
	return 0;
}

static void ntrdma_recv_done(struct ntrdma_cqe *recv_cqe,
			     struct ntrdma_recv_wqe *recv_wqe,
			     struct ntrdma_send_wqe *send_wqe)
{
	recv_cqe->ulp_handle = recv_wqe->ulp_handle;
	recv_cqe->op_code = ntrdma_send_recv_opcode(send_wqe);
	recv_cqe->op_status = NTRDMA_WC_SUCCESS;
	recv_cqe->rdma_len = ntrdma_send_recv_len(send_wqe);
	recv_cqe->imm_data = ntrdma_send_recv_imm(send_wqe);
}

int ntrdma_qp_recv_post_start(struct ntrdma_qp *qp)
{
	mutex_lock(&qp->recv_post_lock);

	if (qp->state < NTRDMA_QPS_INIT) {
		mutex_unlock(&qp->recv_post_lock);
		return -EINVAL;
	}

	return 0;
}

void ntrdma_qp_recv_post_done(struct ntrdma_qp *qp)
{
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);

	if (dev->res_enable)
		ntrdma_qp_recv_work(qp);

	mutex_unlock(&qp->recv_post_lock);
}

void ntrdma_qp_recv_post_get(struct ntrdma_qp *qp,
			     u32 *pos, u32 *end, u32 *base)
{
	ntrdma_ring_produce(qp->recv_post, qp->recv_cmpl, qp->recv_cap,
			    pos, end, base);
}

void ntrdma_qp_recv_post_put(struct ntrdma_qp *qp,
			     u32 pos, u32 base)
{
	qp->recv_post = ntrdma_ring_update(pos, base, qp->recv_cap);
}

static int ntrdma_qp_recv_prod_start(struct ntrdma_qp *qp)
{
	spin_lock_bh(&qp->recv_prod_lock);

	if (qp->state < NTRDMA_QPS_RECV_READY) {
		spin_unlock_bh(&qp->recv_prod_lock);
		return -EAGAIN;
	}

	if (qp->recv_error) {
		spin_unlock_bh(&qp->recv_prod_lock);
		return -EINVAL;
	}

	return 0;
}

static void ntrdma_qp_recv_prod_done(struct ntrdma_qp *qp)
{
	spin_unlock_bh(&qp->recv_prod_lock);
}

static void ntrdma_qp_recv_prod_get(struct ntrdma_qp *qp,
				    u32 *pos, u32 *end, u32 *base)
{
	ntrdma_ring_consume(qp->recv_post, qp->recv_prod, qp->recv_cap,
			    pos, end, base);
}

static void ntrdma_qp_recv_prod_put(struct ntrdma_qp *qp,
				    u32 pos, u32 base)
{
	qp->recv_prod = ntrdma_ring_update(pos, base, qp->recv_cap);
}

static int ntrdma_qp_recv_cons_start(struct ntrdma_qp *qp)
{
	spin_lock_bh(&qp->recv_cons_lock);

	if (qp->state < NTRDMA_QPS_RECV_READY) {
		spin_unlock_bh(&qp->recv_cons_lock);
		return -EINVAL;
	}

	return 0;
}

static void ntrdma_qp_recv_cons_done(struct ntrdma_qp *qp)
{
	spin_unlock_bh(&qp->recv_cons_lock);
}

static void ntrdma_qp_recv_cons_get(struct ntrdma_qp *qp,
				    u32 *pos, u32 *end, u32 *base)
{
	ntrdma_ring_consume(qp->recv_prod, qp->recv_cons, qp->recv_cap,
			    pos, end, base);
}

static void ntrdma_qp_recv_cons_put(struct ntrdma_qp *qp,
				    u32 pos, u32 base)
{
	wmb(); /* write recv completions before index */
	qp->recv_cons = ntrdma_ring_update(pos, base, qp->recv_cap);
}

static void ntrdma_qp_recv_cmpl_start(struct ntrdma_qp *qp)
{
	mutex_lock(&qp->recv_cmpl_lock);

	/* TODO: warn if qp state < INIT */
}

static void ntrdma_qp_recv_cmpl_done(struct ntrdma_qp *qp)
{
	mutex_unlock(&qp->recv_cmpl_lock);
}

static void ntrdma_qp_recv_cmpl_get(struct ntrdma_qp *qp,
				    u32 *pos, u32 *end, u32 *base)
{
	ntrdma_ring_consume(qp->recv_abort ? qp->recv_post : qp->recv_cons,
			    qp->recv_cmpl, qp->recv_cap, pos, end, base);
	rmb(); /* read index before recv completions */
}

static void ntrdma_qp_recv_cmpl_put(struct ntrdma_qp *qp,
				    u32 pos, u32 base)
{
	qp->recv_cmpl = ntrdma_ring_update(pos, base, qp->recv_cap);
}

int ntrdma_qp_send_post_start(struct ntrdma_qp *qp)
{
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);

	mutex_lock(&qp->send_post_lock);

	if (qp->state < NTRDMA_QPS_SEND_DRAIN) {
		mutex_unlock(&qp->send_post_lock);
		ntrdma_dbg(dev, "invalid qp state %u\n", qp->state);
		return -EINVAL;
	}

	return 0;
}

void ntrdma_qp_send_post_done(struct ntrdma_qp *qp)
{
	tasklet_schedule(&qp->send_work);
	mutex_unlock(&qp->send_post_lock);
}

void ntrdma_qp_send_post_get(struct ntrdma_qp *qp,
			     u32 *pos, u32 *end, u32 *base)
{
	ntrdma_ring_produce(qp->send_post, qp->send_cmpl, qp->send_cap,
			    pos, end, base);
}

void ntrdma_qp_send_post_put(struct ntrdma_qp *qp,
			     u32 pos, u32 base)
{
	qp->send_post = ntrdma_ring_update(pos, base, qp->send_cap);
}

static int ntrdma_qp_send_prod_start(struct ntrdma_qp *qp)
{
	spin_lock_bh(&qp->send_prod_lock);

	if (qp->state < NTRDMA_QPS_SEND_READY) {
		spin_unlock_bh(&qp->send_prod_lock);
		return -EAGAIN;
	}

	if (qp->send_error) {
		spin_unlock_bh(&qp->send_prod_lock);
		return -EINVAL;
	}

	return 0;
}

static void ntrdma_qp_send_prod_done(struct ntrdma_qp *qp)
{
	spin_unlock_bh(&qp->send_prod_lock);
}

static void ntrdma_qp_send_prod_get(struct ntrdma_qp *qp,
				    u32 *pos, u32 *end, u32 *base)
{
	ntrdma_ring_consume(qp->send_post, qp->send_prod, qp->send_cap,
			    pos, end, base);
}

static void ntrdma_qp_send_prod_put(struct ntrdma_qp *qp,
				    u32 pos, u32 base)
{
	qp->send_prod = ntrdma_ring_update(pos, base, qp->send_cap);
}

static void ntrdma_qp_send_cmpl_start(struct ntrdma_qp *qp)
{
	mutex_lock(&qp->send_cmpl_lock);

	/* TODO: warn if qp state < SEND_DRAIN */
}

static void ntrdma_qp_send_cmpl_done(struct ntrdma_qp *qp)
{
	mutex_unlock(&qp->send_cmpl_lock);
}

static void ntrdma_qp_send_cmpl_get(struct ntrdma_qp *qp,
				    u32 *pos, u32 *end, u32 *base)
{
	u32 send_cons;

	/* during abort, short circuit prod and cons: abort to post idx */
	if (qp->send_abort)
		send_cons = qp->send_post;
	else
		send_cons = *qp->send_cons_buf;

	ntrdma_ring_consume(send_cons, qp->send_cmpl, qp->send_cap,
			    pos, end, base);
}

static void ntrdma_qp_send_cmpl_put(struct ntrdma_qp *qp,
				    u32 pos, u32 base)
{
	qp->send_cmpl = ntrdma_ring_update(pos, base, qp->send_cap);
}

static int ntrdma_rqp_recv_cons_start(struct ntrdma_rqp *rqp)
{
	spin_lock_bh(&rqp->recv_cons_lock);

	if (rqp->state < NTRDMA_QPS_RECV_READY) {
		spin_unlock_bh(&rqp->recv_cons_lock);
		return -EINVAL;
	}

	if (rqp->recv_error) {
		spin_unlock_bh(&rqp->recv_cons_lock);
		return -EINVAL;
	}

	return 0;
}

static void ntrdma_rqp_recv_cons_done(struct ntrdma_rqp *rqp)
{
	spin_unlock_bh(&rqp->recv_cons_lock);
}

static void ntrdma_rqp_recv_cons_get(struct ntrdma_rqp *rqp,
				     u32 *pos, u32 *end, u32 *base)
{
	u32 recv_prod = *rqp->recv_prod_buf;

	ntrdma_ring_consume(recv_prod, rqp->recv_cons, rqp->recv_cap,
			    pos, end, base);
}

static void ntrdma_rqp_recv_cons_put(struct ntrdma_rqp *rqp,
				     u32 pos, u32 base)
{
	rqp->recv_cons = ntrdma_ring_update(pos, base, rqp->recv_cap);
}

static int ntrdma_rqp_send_cons_start(struct ntrdma_rqp *rqp)
{
	spin_lock_bh(&rqp->send_cons_lock);

	if (rqp->state < NTRDMA_QPS_SEND_READY) {
		spin_unlock_bh(&rqp->send_cons_lock);
		return -EAGAIN;
	}

	if (rqp->send_error) {
		spin_unlock_bh(&rqp->send_cons_lock);
		return -EINVAL;
	}

	return 0;
}

static void ntrdma_rqp_send_cons_done(struct ntrdma_rqp *rqp)
{
	spin_unlock_bh(&rqp->send_cons_lock);
}

static void ntrdma_rqp_send_cons_get(struct ntrdma_rqp *rqp,
				     u32 *pos, u32 *end, u32 *base)
{
	u32 send_prod = *rqp->send_prod_buf;

	ntrdma_ring_consume(send_prod, rqp->send_cons, rqp->send_cap,
			    pos, end, base);
}

static void ntrdma_rqp_send_cons_put(struct ntrdma_rqp *rqp,
				     u32 pos, u32 base)
{
	rqp->send_cons = ntrdma_ring_update(pos, base, rqp->send_cap);
}

static int ntrdma_qp_poll_recv_start_and_get(struct ntrdma_poll *poll,
					     struct ntrdma_qp **poll_qp, u32 *poll_pos,
					     u32 *poll_end, u32 *poll_base)
{
	struct ntrdma_qp *qp = ntrdma_recv_poll_qp(poll);
	u32 pos, end, base;

	ntrdma_qp_recv_cmpl_start(qp);
	ntrdma_qp_recv_cmpl_get(qp, &pos, &end, &base);

	if (pos == end) {
		ntrdma_qp_recv_cmpl_done(qp);
		return -EAGAIN;
	}

	end = min_t(u32, end, pos + NTRDMA_QP_BATCH_SIZE);

	*poll_qp = qp;
	*poll_pos = pos;
	*poll_end = end;
	*poll_base = base;

	return 0;
}

static void ntrdma_qp_poll_recv_put_and_done(struct ntrdma_poll *poll,
					     u32 pos, u32 base)
{
	struct ntrdma_qp *qp = ntrdma_recv_poll_qp(poll);

	ntrdma_qp_recv_cmpl_put(qp, pos, base);
	ntrdma_qp_recv_cmpl_done(qp);
}

static struct ntrdma_cqe *ntrdma_qp_poll_recv_cqe(struct ntrdma_poll *poll,
						  struct ntrdma_cqe *abort_cqe, u32 pos)
{
	struct ntrdma_qp *qp = ntrdma_recv_poll_qp(poll);
	struct ntrdma_cqe *cqe = ntrdma_qp_recv_cqe(qp, pos);
	struct ntrdma_recv_wqe *wqe = ntrdma_qp_recv_wqe(qp, pos);

	if (qp->recv_abort) {
		ntrdma_recv_fail(abort_cqe, wqe, NTRDMA_WC_ERR_ABORTED);
		return abort_cqe;
	}

	if (wqe->op_status) {
		qp->recv_abort = true;
		ntrdma_recv_fail(abort_cqe, wqe, wqe->op_status);
		return abort_cqe;
	}

	if (cqe->op_status)
		qp->recv_abort = true;

	return cqe;
}

static int ntrdma_qp_poll_send_start_and_get(struct ntrdma_poll *poll,
					     struct ntrdma_qp **poll_qp, u32 *poll_pos,
					     u32 *poll_end, u32 *poll_base)
{
	struct ntrdma_qp *qp = ntrdma_send_poll_qp(poll);
	u32 pos, end, base;

	ntrdma_qp_send_cmpl_start(qp);
	ntrdma_qp_send_cmpl_get(qp, &pos, &end, &base);

	if (pos == end) {
		ntrdma_qp_send_cmpl_done(qp);
		return -EAGAIN;
	}

	end = min_t(u32, end, pos + NTRDMA_QP_BATCH_SIZE);

	*poll_qp = qp;
	*poll_pos = pos;
	*poll_end = end;
	*poll_base = base;

	return 0;
}

static void ntrdma_qp_poll_send_put_and_done(struct ntrdma_poll *poll,
					     u32 pos, u32 base)
{
	struct ntrdma_qp *qp = ntrdma_send_poll_qp(poll);

	ntrdma_qp_send_cmpl_put(qp, pos, base);
	ntrdma_qp_send_cmpl_done(qp);
}

static struct ntrdma_cqe *ntrdma_qp_poll_send_cqe(struct ntrdma_poll *poll,
						  struct ntrdma_cqe *abort_cqe, u32 pos)
{
	struct ntrdma_qp *qp = ntrdma_send_poll_qp(poll);
	struct ntrdma_cqe *cqe = ntrdma_qp_send_cqe(qp, pos);
	struct ntrdma_send_wqe *wqe = ntrdma_qp_send_wqe(qp, pos);

	if (qp->send_abort) {
		ntrdma_send_fail(abort_cqe, wqe, NTRDMA_WC_ERR_ABORTED);
		return abort_cqe;
	}

	if (wqe->op_status) {
		qp->send_abort = true;
		ntrdma_send_fail(abort_cqe, wqe, wqe->op_status);
		return abort_cqe;
	}

	if (cqe->op_status)
		qp->send_abort = true;

	return cqe;
}

static void ntrdma_qp_recv_work(struct ntrdma_qp *qp)
{
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);
	struct ntrdma_req *req;
	u32 start, end, base;
	u64 dst, src;
	size_t off, len;
	int rc;

	/* verify the qp state and lock for producing recvs */
	rc = ntrdma_qp_recv_prod_start(qp);
	if (rc)
		return;

	/* get the next producing range in the ring */
	ntrdma_qp_recv_prod_get(qp, &start, &end, &base);

	if (start == end)
		goto out;

	req = ntc_req_create(dev->ntc);
	if (!req)
		goto out;

	/* sync the ring buffer for the device */
	ntc_buf_sync_dev(dev->ntc,
			 qp->recv_wqe_buf_addr,
			 qp->recv_wqe_buf_size,
			 DMA_TO_DEVICE,
			 IOAT_DEV_ACCESS);

	for (;;) {
		ntrdma_qp_recv_prod_put(qp, end, base);

		/* send the portion of the ring */
		off = start * qp->recv_wqe_size;
		len = (end - start) * qp->recv_wqe_size;
		src = qp->recv_wqe_buf_addr + off;
		dst = qp->peer_recv_wqe_buf_dma_addr + off;

		ntc_req_memcpy(dev->ntc, req,
			       dst, src, len,
			       true, NULL, NULL);

		ntrdma_qp_recv_prod_get(qp, &start, &end, &base);
		if (start == end)
			break;
	}

	/* send the prod idx */
	ntc_req_imm32(dev->ntc, req,
		      qp->peer_recv_prod_dma_addr,
		      qp->recv_prod,
		      true, NULL, NULL);

	/* submit the request */
	ntc_req_submit(dev->ntc, req);

out:
	/* release lock for state change or producing later recvs */
	ntrdma_qp_recv_prod_done(qp);
}

static void ntrdma_qp_send_work(struct ntrdma_qp *qp)
{
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);
	struct ntrdma_rqp *rqp;
	struct ntrdma_req *req;
	struct ntrdma_send_wqe *wqe;
	struct ntrdma_recv_wqe *recv_wqe = NULL;
	struct ntrdma_wr_sge rdma_sge;
	u32 start, pos, end, base;
	u32 recv_pos, recv_end, recv_base;
	u32 rdma_len;
	u64 dst, src;
	size_t off, len;
	int rc;

	/* verify the qp state and lock for producing sends */
	rc = ntrdma_qp_send_prod_start(qp);
	if (rc) {
		ntrdma_err(dev,
				"ntrdma_qp_send_prod_start failed rc = %d qp %d(%p)\n",
				rc,qp->res.key, qp);
		return;
	}

	/* get the next producing range in the send ring */
	ntrdma_qp_send_prod_get(qp, &start, &end, &base);

	/* quit if there is no send work to do */
	if (start == end) {
		ntrdma_qp_send_prod_done(qp);
		return;
	}

	/* limit the range to batch size */
	end = min_t(u32, end, start + NTRDMA_QP_BATCH_SIZE);

	/* sending requires a connected rqp */
	rqp = ntrdma_dev_rqp_look(dev, qp->rqp_key);
	if (!rqp) {
		ntrdma_err(dev, "ntrdma_dev_rqp_look failed %d key %d\n",
				rc, qp->rqp_key);
		goto err_rqp;
	}
	/* FIXME: need to complete the send with error */


	/* connected rqp must be ready to receive */
	rc = ntrdma_rqp_recv_cons_start(rqp);
	if (rc) {
		ntrdma_err(dev, "ntrdma_rqp_recv_cons_start failed %d\n",
				rc);
		goto err_recv;
	}

	/* get the next consuming range in the recv ring */
	ntrdma_rqp_recv_cons_get(rqp, &recv_pos, &recv_end, &recv_base);

	req = ntc_req_create(dev->ntc);

	for (pos = start;;) {
		wqe = ntrdma_qp_send_wqe(qp, pos++);

		if (ntrdma_wr_code_is_send(wqe->op_code)) {

			wqe->recv_key = recv_pos + recv_base;

			if (recv_pos == recv_end) {
				ntrdma_dbg(dev, "send but no recv pos %u end %u base %u "
					   "prod %u cons %u\n", recv_pos, recv_end, recv_base,
					   *rqp->recv_prod_buf, rqp->recv_cons);
#ifdef CONFIG_NTRDMA_RETRY_RECV
				--pos;
				tasklet_schedule(&qp->send_work);
#else
				if (!wqe->op_status)
					wqe->op_status = NTRDMA_WC_ERR_RECV_MISSING;

				qp->send_error = true;
				rqp->recv_error = true;
#endif
				break;
			}

			recv_wqe = ntrdma_rqp_recv_wqe(rqp, recv_pos++);

			if (recv_wqe->op_status) {
				if (!wqe->op_status) {
					ntrdma_err(dev,
							"wqe->op_status %d recv_wqe->op_status %d recv_pos %u\n",
							wqe->op_status,
							recv_wqe->op_status,
							recv_pos);
					wqe->op_status = recv_wqe->op_status;
				}

				qp->send_error = true;
				rqp->recv_error = true;
				break;
			}

			if (recv_pos == recv_end) {
				ntrdma_rqp_recv_cons_put(rqp, recv_pos, recv_base);
				ntrdma_rqp_recv_cons_get(rqp, &recv_pos, &recv_end, &recv_base);
			}
		}

		if (wqe->op_status) {
			ntrdma_err(dev, "op status %d\n",
					wqe->op_status);
			qp->send_error = true;
			rqp->recv_error = true;
			break;
		}

		if (ntrdma_wr_code_push_data(wqe->op_code)) {
			if (ntrdma_wr_code_is_rdma(wqe->op_code)) {
				rdma_sge.addr = wqe->rdma_addr;
				rdma_sge.len = ~(u32)0;
				rdma_sge.key = wqe->rdma_key;

				rc = ntrdma_zip_rdma(dev, req, &rdma_len,
						     &rdma_sge, wqe->sg_list,
						     1, wqe->sg_count,
						     false);
			} else {
				rc = ntrdma_zip_rdma(dev, req, &rdma_len,
						     recv_wqe->sg_list, wqe->sg_list,
						     recv_wqe->sg_count, wqe->sg_count,
						     false);
			}
			if (rc) {
				ntrdma_err(dev,
						"ntrdma_zip_rdma failed %d qp %d\n",
						rc, qp->res.key);

				/* FIXME: use rc to indicate the real error */
				wqe->op_status = NTRDMA_WC_ERR_RDMA_RANGE;
				qp->send_error = true;
				rqp->recv_error = true;
			}

			wqe->rdma_len = rdma_len;
		}

		if (pos == end) {
			tasklet_schedule(&qp->send_work);
			break;
		}
	}

	ntrdma_rqp_recv_cons_put(rqp, recv_pos, recv_base);
	ntrdma_rqp_recv_cons_done(rqp);
	ntrdma_rqp_put(rqp);

	ntrdma_qp_send_prod_put(qp, pos, base);

	/* sync the ring buffer for the device */
	ntc_buf_sync_dev(dev->ntc,
			 qp->send_wqe_buf_addr,
			 qp->send_wqe_buf_size,
			 DMA_TO_DEVICE,
			 IOAT_DEV_ACCESS);

	/* send the portion of the ring */
	off = start * qp->send_wqe_size;
	len = (pos - start) * qp->send_wqe_size;
	src = qp->send_wqe_buf_addr + off;
	dst = qp->peer_send_wqe_buf_dma_addr + off;

	ntc_req_memcpy(dev->ntc, req,
		       dst, src, len,
		       true, NULL, NULL);

	/* send the prod idx */
	ntc_req_imm32(dev->ntc, req,
		      qp->peer_send_prod_dma_addr,
		      qp->send_prod,
		      true, NULL, NULL);

	/* update the vbell and signal the peer */
	ntrdma_dev_vbell_peer(dev, req,
			      qp->peer_send_vbell_idx);
	ntc_req_signal(dev->ntc, req, NULL, NULL, NTB_DEFAULT_VEC(dev->ntc));

	/* submit the request */
	ntc_req_submit(dev->ntc, req);

	/* release lock for state change or producing later sends */
	ntrdma_qp_send_prod_done(qp);
	return;

err_recv:
	ntrdma_rqp_put(rqp);
err_rqp:
	ntrdma_qp_send_prod_done(qp);
}

static inline void ntrdma_rqp_send_vbell_clear(struct ntrdma_dev *dev,
					       struct ntrdma_rqp *rqp)
{
	ntrdma_dev_vbell_clear(dev, &rqp->send_vbell,
			       rqp->send_vbell_idx);
}

static inline int ntrdma_rqp_send_vbell_add(struct ntrdma_dev *dev,
					    struct ntrdma_rqp *rqp)
{
	return ntrdma_dev_vbell_add(dev, &rqp->send_vbell,
				    rqp->send_vbell_idx);
}

static void ntrdma_rqp_send_work(struct ntrdma_rqp *rqp)
{
	struct ntrdma_dev *dev = ntrdma_rqp_dev(rqp);
	struct ntrdma_qp *qp;
	struct ntrdma_req *req;
	struct ntrdma_cqe *cqe, *recv_cqe = NULL;
	struct ntrdma_send_wqe *wqe;
	struct ntrdma_recv_wqe *recv_wqe = NULL;
	struct ntrdma_wr_sge rdma_sge;
	u32 start, pos, end, base;
	u32 recv_pos, recv_end, recv_base;
	u32 rdma_len;
	u64 dst, src;
	size_t off, len;
	bool cue_recv = false;
	int rc;

	/* verify the rqp state and lock for consuming sends */
	rc = ntrdma_rqp_send_cons_start(rqp);
	if (rc) {
		ntrdma_err(dev,
				"ntrdma_rqp_send_cons_start failed rc = %d qp_key %d(%p)\n",
				rc, rqp->qp_key, rqp);
		return;
	}

	ntrdma_rqp_send_vbell_clear(dev, rqp);

	/* get the next consuming range in the send ring */
	ntrdma_rqp_send_cons_get(rqp, &start, &end, &base);

	/* quit if there is no send work to do */
	if (start == end) {
		rc = ntrdma_rqp_send_vbell_add(dev, rqp);
		if (rc == -EAGAIN)
			tasklet_schedule(&rqp->send_work);
		ntrdma_rqp_send_cons_done(rqp);
		return;
	}

	/* limit the range to batch size */
	end = min_t(u32, end, start + NTRDMA_QP_BATCH_SIZE);

	/* sending requires a connected qp */
	qp = ntrdma_dev_qp_look(dev, rqp->qp_key);
	if (!qp) {
		ntrdma_err(dev, "ntrdma_dev_qp_look failed\n");
		goto err_qp;
	}
	/* FIXME: need to complete the send with error */

	/* connected qp must be ready to receive */
	rc = ntrdma_qp_recv_cons_start(qp);
	if (rc) {
		ntrdma_err(dev, "ntrdma_qp_recv_cons_start failed\n");
		goto err_recv;
	}
	/* FIXME: need to complete the send with error */

	/* get the next consuming range in the recv ring */
	ntrdma_qp_recv_cons_get(qp, &recv_pos, &recv_end, &recv_base);

	req = ntc_req_create(dev->ntc);

	for (pos = start;;) {
		cqe = ntrdma_rqp_send_cqe(rqp, pos);
		wqe = ntrdma_rqp_send_wqe(rqp, pos++);

		if (ntrdma_wr_code_is_send(wqe->op_code)) {
			if (ntrdma_wr_status_no_recv(wqe->op_status)) {
				ntrdma_send_fail(cqe, wqe, wqe->op_status);
				ntrdma_err(dev, "Error %s %d\n",
						__func__, __LINE__);
				qp->send_error = true;
				rqp->recv_error = true;
				break;
			}

			if (WARN_ON(wqe->recv_key != recv_pos + recv_base)) {
				if (!wqe->op_status)
					ntrdma_send_fail(cqe, wqe, NTRDMA_WC_ERR_CORRUPT);
				else
					ntrdma_send_fail(cqe, wqe, wqe->op_status);

				qp->send_error = true;
				rqp->recv_error = true;
				ntrdma_err(dev, "Error %s %d\n",
						__func__, __LINE__);
				break;
			}

			if (recv_pos == recv_end) {
				if (WARN_ON(!wqe->op_status))
					ntrdma_send_fail(cqe, wqe, NTRDMA_WC_ERR_RECV_MISSING);
				else
					ntrdma_send_fail(cqe, wqe, wqe->op_status);

				rqp->send_error = true;
				qp->recv_error = true;
				ntrdma_err(dev, "Error %s %d\n",
						__func__, __LINE__);
				break;
			}

			cue_recv = true;
			recv_cqe = ntrdma_qp_recv_cqe(qp, recv_pos);
			recv_wqe = ntrdma_qp_recv_wqe(qp, recv_pos++);

			if (recv_wqe->op_status) {
				if (WARN_ON(!wqe->op_status))
					ntrdma_send_fail(cqe, wqe, recv_wqe->op_status);
				else
					ntrdma_send_fail(cqe, wqe, wqe->op_status);

				ntrdma_recv_fail(recv_cqe, recv_wqe, recv_wqe->op_status);

				qp->send_error = true;
				rqp->recv_error = true;
				ntrdma_err(dev, "Error %s %d\n",
						__func__, __LINE__);
				break;
			}
		} else {
			recv_wqe = NULL;
			recv_cqe = NULL;
		}

		if (wqe->op_status) {
			ntrdma_send_fail(cqe, wqe, wqe->op_status);

			if (ntrdma_wr_code_is_send(wqe->op_code))
				ntrdma_recv_fail(recv_cqe, recv_wqe, wqe->op_status);

			qp->send_error = true;
			rqp->recv_error = true;
			ntrdma_err(dev, "Error wqe op status %d  pos %u\n",
					wqe->op_status, pos);
			break;
		}

		if (ntrdma_wr_code_push_data(wqe->op_code)) {
			if (ntrdma_wr_code_is_rdma(wqe->op_code)) {
				rdma_sge.addr = wqe->rdma_addr;
				rdma_sge.len = wqe->rdma_len;
				rdma_sge.key = wqe->rdma_key;

				rc = ntrdma_zip_sync(dev, &rdma_sge, 1);
			} else {
				/* TODO: sync less than sg_count using rdma_len */
				rc = ntrdma_zip_sync(dev, recv_wqe->sg_list,
						     recv_wqe->sg_count);
			}

			WARN(rc, "ntrdma_zip_sync failed rc = %d", rc);
			/* FIXME: handle send sync error */

			if (ntrdma_wr_code_is_send(wqe->op_code))
				ntrdma_recv_done(recv_cqe, recv_wqe, wqe);

			ntrdma_send_done(cqe, wqe, wqe->rdma_len);
		}

		if (ntrdma_wr_code_pull_data(wqe->op_code)) {
			rdma_sge.addr = wqe->rdma_addr;
			rdma_sge.len = ~(u32)0;
			rdma_sge.key = wqe->rdma_key;

			rc = ntrdma_zip_rdma(dev, req, &rdma_len,
					     wqe->sg_list, &rdma_sge,
					     wqe->sg_count, 1,
					     true);

			WARN_ON(rc);
			/* FIXME: handle rdma read error */

			ntrdma_send_done(cqe, wqe, rdma_len);
		}

		if (recv_pos == recv_end) {
			ntrdma_qp_recv_cons_put(qp, recv_pos, recv_base);
			ntrdma_qp_recv_cons_get(qp, &recv_pos,
						&recv_end, &recv_base);
		}

		if (pos == end) {
			tasklet_schedule(&rqp->send_work);
			break;
		}
	}

	ntrdma_qp_recv_cons_put(qp, recv_pos, recv_base);
	ntrdma_qp_recv_cons_done(qp);

	if (cue_recv)
		ntrdma_cq_cue(qp->recv_cq);

	ntrdma_qp_put(qp);

	ntrdma_rqp_send_cons_put(rqp, pos, base);

	/* sync the ring buffer for the device */
	ntc_buf_sync_dev(dev->ntc,
			 rqp->send_cqe_buf_addr,
			 rqp->send_cqe_buf_size,
			 DMA_TO_DEVICE,
			 IOAT_DEV_ACCESS);

	/* send the portion of the ring */
	off = start * sizeof(struct ntrdma_cqe);
	len = (pos - start) * sizeof(struct ntrdma_cqe);
	src = rqp->send_cqe_buf_addr + off;
	dst = rqp->peer_send_cqe_buf_dma_addr + off;

	ntc_req_memcpy(dev->ntc, req,
		       dst, src, len,
		       true, NULL, NULL);

	/* send the cons idx */
	ntc_req_imm32(dev->ntc, req,
		      rqp->peer_send_cons_dma_addr,
		      rqp->send_cons,
		      true, NULL, NULL);

	if (cqe->flags & IB_SEND_SIGNALED) {
		/* update the vbell and signal the peer */
		ntrdma_dev_vbell_peer(dev, req,
					  rqp->peer_cmpl_vbell_idx);
		ntc_req_signal(dev->ntc, req, NULL, NULL,
			       NTB_DEFAULT_VEC(dev->ntc));
	}
	/* submit the request */
	ntc_req_submit(dev->ntc, req);

	/* release lock for state change or consuming later sends */
	ntrdma_rqp_send_cons_done(rqp);
	return;

err_recv:
	ntrdma_qp_put(qp);
err_qp:
	ntrdma_rqp_send_cons_done(rqp);
	ntrdma_err(dev, "%s Failed\n", __func__);
}

static void ntrdma_qp_work_cb(unsigned long ptrhld)
{
	struct ntrdma_qp *qp = of_ptrhld(ptrhld);

	ntrdma_qp_send_work(qp);
}

static void ntrdma_rqp_work_cb(unsigned long ptrhld)
{
	struct ntrdma_rqp *rqp = of_ptrhld(ptrhld);

	ntrdma_rqp_send_work(rqp);
}

static void ntrdma_rqp_vbell_cb(void *ctx)
{
	struct ntrdma_rqp *rqp = ctx;

	tasklet_schedule(&rqp->send_work);
}

struct ntrdma_qp *ntrdma_dev_qp_look(struct ntrdma_dev *dev, int key)
{
	struct ntrdma_res *res;

	res = ntrdma_dev_res_look(dev, &dev->qp_vec, key);
	if (!res)
		return NULL;

	return ntrdma_res_qp(res);
}

struct ntrdma_rqp *ntrdma_dev_rqp_look(struct ntrdma_dev *dev, int key)
{
	struct ntrdma_rres *rres;

	rres = ntrdma_dev_rres_look(dev, &dev->rqp_vec, key);
	if (!rres)
		return NULL;

	return ntrdma_rres_rqp(rres);
}

