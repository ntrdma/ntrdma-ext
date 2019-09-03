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

DECLARE_PER_CPU(struct ntrdma_dev_counters, dev_cnt);

#define NTRDMA_QP_BATCH_SIZE 0x10

struct ntrdma_qp_cmd_cb {
	struct ntrdma_cmd_cb cb;
	struct ntrdma_qp *qp;
};

#define ntrdma_cmd_cb_qpcb(__cb) \
	container_of(__cb, struct ntrdma_qp_cmd_cb, cb)

static int ntrdma_qp_modify_prep(struct ntrdma_cmd_cb *cb,
				 union ntrdma_cmd *cmd, struct dma_chan *req);
static int ntrdma_qp_modify_cmpl(struct ntrdma_cmd_cb *cb,
				const union ntrdma_rsp *rsp,
				struct dma_chan *req);
static int ntrdma_qp_enable_prep(struct ntrdma_cmd_cb *cb,
				 union ntrdma_cmd *cmd, struct dma_chan *req);
static int ntrdma_qp_enable_cmpl(struct ntrdma_cmd_cb *cb,
				const union ntrdma_rsp *rsp,
				struct dma_chan *req);
static int ntrdma_qp_disable_prep(struct ntrdma_cmd_cb *cb,
				  union ntrdma_cmd *cmd, struct dma_chan *req);
static int ntrdma_qp_disable_cmpl(struct ntrdma_cmd_cb *cb,
				const union ntrdma_rsp *rsp,
				struct dma_chan *req);

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
static
const struct ntrdma_cqe *ntrdma_qp_poll_recv_cqe(struct ntrdma_poll *poll,
						struct ntrdma_cqe *abort_cqe,
						u32 pos);

static int ntrdma_qp_poll_send_start_and_get(struct ntrdma_poll *poll,
					     struct ntrdma_qp **pqp, u32 *pos, u32 *end,
					     u32 *base);
static void ntrdma_qp_poll_send_put_and_done(struct ntrdma_poll *poll,
					     u32 pos, u32 base);
static
const struct ntrdma_cqe *ntrdma_qp_poll_send_cqe(struct ntrdma_poll *poll,
						struct ntrdma_cqe *abort_cqe,
						u32 pos);

static void ntrdma_qp_recv_work(struct ntrdma_qp *qp);
static void ntrdma_qp_send_work(struct ntrdma_qp *qp);
static void ntrdma_rqp_send_work(struct ntrdma_rqp *rqp);

static void ntrdma_qp_work_cb(unsigned long ptrhld);
static void ntrdma_rqp_work_cb(unsigned long ptrhld);
static void ntrdma_rqp_vbell_cb(void *ctx);
static void ntrdma_send_fail(struct ntrdma_cqe *cqe,
			const struct ntrdma_send_wqe *wqe, int op_status);

#define move_to_err_state(qp) move_to_err_state_d(qp, __func__, __LINE__)

static inline const u32 *ntrdma_qp_send_cons_buf(struct ntrdma_qp *qp)
{
	return ntc_bidir_buf_const_deref(&qp->send_cqe_buf,
					qp->send_cap *
					sizeof(struct ntrdma_cqe),
					sizeof(u32));
}

inline u32 ntrdma_qp_send_cons(struct ntrdma_qp *qp)
{
	const u32 *send_cons_buf = ntrdma_qp_send_cons_buf(qp);

	if (!send_cons_buf)
		return 0;

	return *send_cons_buf;
}

static inline void ntrdma_qp_set_send_cons(struct ntrdma_qp *qp, u32 send_cons)
{
	u32 *send_cons_buf;

	send_cons_buf = ntc_bidir_buf_deref(&qp->send_cqe_buf,
					qp->send_cap *
					sizeof(struct ntrdma_cqe),
					sizeof(u32));

	if (!send_cons_buf)
		return;

	*send_cons_buf = send_cons;

	ntc_bidir_buf_unref(&qp->send_cqe_buf,
			qp->send_cap *
			sizeof(struct ntrdma_cqe),
			sizeof(u32));
}

static inline const u32 *ntrdma_rqp_send_prod_buf(struct ntrdma_rqp *rqp)
{
	return ntc_export_buf_const_deref(&rqp->send_wqe_buf,
					rqp->send_cap * rqp->send_wqe_size,
					sizeof(u32));
}

inline u32 ntrdma_rqp_send_prod(struct ntrdma_rqp *rqp)
{
	const u32 *send_prod_buf = ntrdma_rqp_send_prod_buf(rqp);

	if (!send_prod_buf)
		return 0;

	return *send_prod_buf;
}

static inline const u32 *ntrdma_rqp_recv_prod_buf(struct ntrdma_rqp *rqp)
{
	return ntc_export_buf_const_deref(&rqp->recv_wqe_buf,
					rqp->recv_cap * rqp->recv_wqe_size,
					sizeof(u32));
}

inline u32 ntrdma_rqp_recv_prod(struct ntrdma_rqp *rqp)
{
	const u32 *recv_prod_buf = ntrdma_rqp_recv_prod_buf(rqp);

	if (!recv_prod_buf)
		return 0;

	return *recv_prod_buf;
}

static inline int ntrdma_qp_init_deinit(struct ntrdma_qp *qp,
		struct ntrdma_dev *dev,
		struct ntrdma_cq *recv_cq, struct ntrdma_cq *send_cq,
		struct ntrdma_qp_init_attr *attr,
		int is_deinit)
{
	int rc;
	int reserved_key = -1;
	u32 send_cons = 0;
	u64 send_cqes_total_size;

	if (is_deinit)
		goto deinit;

	if (attr->qp_type == IB_QPT_GSI || attr->qp_type == IB_QPT_SMI)
		reserved_key = attr->qp_type;


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

	atomic_set(&qp->state, 0);
	qp->recv_abort = false;
	qp->recv_aborting = false;
	qp->recv_abort_first = false;
	qp->send_abort = false;
	qp->send_aborting = false;
	qp->send_abort_first = false;
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
	rc = ntc_local_buf_zalloc(&qp->send_wqe_buf, dev->ntc,
				qp->send_cap * qp->send_wqe_size, GFP_KERNEL);
	if (rc < 0)
		goto err_send_wqe_buf;

	/* set up the send completion queue buffer */
	send_cqes_total_size = qp->send_cap * sizeof(struct ntrdma_cqe);
	rc = ntc_bidir_buf_zalloc_init(&qp->send_cqe_buf, dev->ntc,
				send_cqes_total_size
				+ sizeof(u32), /* for send_cons */
				GFP_KERNEL, &send_cons, sizeof(u32),
				send_cqes_total_size);
	if (rc < 0)
		goto err_send_cqe_buf;

	/* peer rqp send queue is zero until enabled */
	ntc_remote_buf_clear(&qp->peer_send_wqe_buf);
	qp->peer_send_vbell_idx = 0;

	/* set up the recv work ring */
	qp->recv_cap = attr->recv_wqe_cap;
	qp->recv_post = 0;
	qp->recv_prod = 0;
	qp->recv_cons = 0;
	qp->recv_cmpl = 0;

	/* set up the recv work queue buffer */
	rc = ntc_local_buf_zalloc(&qp->recv_wqe_buf, dev->ntc,
				qp->recv_cap * qp->recv_wqe_size, GFP_KERNEL);
	if (rc < 0)
		goto err_recv_wqe_buf;

	/* set up the recv completion queue buffer */
	qp->recv_cqe_buf_size = qp->recv_cap * sizeof(struct ntrdma_cqe);

	qp->recv_cqe_buf = kzalloc_node(qp->recv_cqe_buf_size,
					GFP_KERNEL, dev->node);
	if (!qp->recv_cqe_buf) {
		rc = -ENOMEM;
		goto err_recv_cqe_buf;
	}

	/* peer rqp recv queue is zero until enabled */
	ntc_remote_buf_clear(&qp->peer_recv_wqe_buf);

	/* initialize synchronization */
	mutex_init(&qp->send_post_lock);
	spin_lock_init(&qp->send_post_slock);
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
	ntc_local_buf_free(&qp->recv_wqe_buf);
err_recv_wqe_buf:
	ntc_bidir_buf_free(&qp->send_cqe_buf);
err_send_cqe_buf:
	ntc_local_buf_free(&qp->send_wqe_buf);
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
	return ntc_local_buf_deref(&qp->send_wqe_buf) + pos * qp->send_wqe_size;
}

const struct ntrdma_cqe *ntrdma_qp_send_cqe(struct ntrdma_qp *qp, u32 pos)
{
	return ntc_bidir_buf_const_deref(&qp->send_cqe_buf,
					pos * sizeof(struct ntrdma_cqe),
					sizeof(struct ntrdma_cqe));
}

struct ntrdma_recv_wqe *ntrdma_qp_recv_wqe(struct ntrdma_qp *qp,
					   u32 pos)
{
	return ntc_local_buf_deref(&qp->recv_wqe_buf) + pos * qp->recv_wqe_size;
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
				 union ntrdma_cmd *cmd, struct dma_chan *req)
{
	struct ntrdma_qp_cmd_cb *qpcb = ntrdma_cmd_cb_qpcb(cb);
	struct ntrdma_qp *qp = qpcb->qp;
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);

	ntrdma_vdbg(dev, "called\n");

	ntrdma_qp_recv_work(qp);

	cmd->qp_modify.hdr.op = NTRDMA_CMD_QP_MODIFY;
	cmd->qp_modify.qp_key = qp->res.key;
	cmd->qp_modify.access = qp->access;
	cmd->qp_modify.state = atomic_read(&qp->state);
	cmd->qp_modify.dest_qp_key = qp->rqp_key;

	return 0;
}

static int ntrdma_qp_modify_cmpl(struct ntrdma_cmd_cb *cb,
				const union ntrdma_rsp *rsp,
				struct dma_chan *req)
{
	struct ntrdma_qp_cmd_cb *qpcb = ntrdma_cmd_cb_qpcb(cb);
	struct ntrdma_qp *qp = qpcb->qp;
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);
	int rc;

	ntrdma_vdbg(dev, "called\n");
	if (!rsp) {
		ntrdma_err(dev, "qp %d modify aborted\n",
				qp->res.key);
		ntrdma_res_done_cmds(&qp->res);
		return -EIO;
	}

	if (rsp->hdr.status) {
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

	if (qp->ibqp.qp_type == IB_QPT_GSI) {
		qp->recv_prod = qp->recv_cmpl;
		TRACE("Enabling GSI QP post %u prod %u cons %u cmpl %u\n",
				qp->recv_post, qp->recv_prod,
				qp->recv_cons, qp->recv_cmpl);
	}

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
				 union ntrdma_cmd *cmd, struct dma_chan *req)
{
	struct ntrdma_qp_cmd_cb *qpcb = ntrdma_cmd_cb_qpcb(cb);
	struct ntrdma_qp *qp = qpcb->qp;
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);

	ntrdma_vdbg(dev, "called\n");
	TRACE("qp_enable prep: qp %d\n", qp->res.key);

	cmd->qp_create.hdr.op = NTRDMA_CMD_QP_CREATE;
	cmd->qp_create.qp_key = qp->res.key;
	cmd->qp_create.pd_key = qp->pd_key;
	cmd->qp_create.qp_type = 0; /* TODO: just RC for now */
	cmd->qp_create.recv_wqe_cap = qp->recv_cap;
	cmd->qp_create.recv_wqe_sg_cap = qp->recv_wqe_sg_cap;
	cmd->qp_create.recv_ring_idx = qp->recv_cons;
	cmd->qp_create.send_wqe_cap = qp->send_cap;
	cmd->qp_create.send_wqe_sg_cap = qp->send_wqe_sg_cap;
	cmd->qp_create.send_ring_idx = ntrdma_qp_send_cons(qp);
	ntc_bidir_buf_make_desc(&cmd->qp_create.send_cqe_buf_desc,
				&qp->send_cqe_buf);
	cmd->qp_create.send_cons_shift =
		qp->send_cap * sizeof(struct ntrdma_cqe);
	cmd->qp_create.cmpl_vbell_idx = qp->send_cq->vbell_idx;

	return 0;
}

static int ntrdma_qp_enable_disable_cmpl_common(struct ntrdma_qp *qp,
						const struct ntrdma_dev *dev,
						const union ntrdma_rsp *rsp,
						int is_disable)
{
	int rc;

	if (is_disable)
		goto disable;

	rc = ntc_remote_buf_map(&qp->peer_recv_wqe_buf, dev->ntc,
				&rsp->qp_create.recv_wqe_buf_desc);
	if (rc < 0)
		goto err_peer_recv_wqe_buf;

	qp->peer_recv_prod_shift = rsp->qp_create.recv_prod_shift;

	rc = ntc_remote_buf_map(&qp->peer_send_wqe_buf, dev->ntc,
				&rsp->qp_create.send_wqe_buf_desc);
	if (rc < 0)
		goto err_peer_send_wqe_buf;

	qp->peer_send_prod_shift = rsp->qp_create.send_prod_shift;

	qp->peer_send_vbell_idx = rsp->qp_create.send_vbell_idx;


	return 0;
disable:
	ntc_remote_buf_unmap(&qp->peer_send_wqe_buf);
err_peer_send_wqe_buf:
	ntc_remote_buf_unmap(&qp->peer_recv_wqe_buf);
err_peer_recv_wqe_buf:
	ntrdma_res_done_cmds(&qp->res);
	return rc;
}

static int ntrdma_qp_enable_cmpl(struct ntrdma_cmd_cb *cb,
				const union ntrdma_rsp *rsp,
				struct dma_chan *req)
{
	struct ntrdma_qp_cmd_cb *qpcb = ntrdma_cmd_cb_qpcb(cb);
	struct ntrdma_qp *qp = qpcb->qp;
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);
	int rc;

	ntrdma_vdbg(dev, "called\n");

	TRACE("qp_enable cmpl: %d\n", qp->res.key);

	if (!rsp || rsp->hdr.status)
		return -EIO;

	rc = ntrdma_qp_enable_disable_cmpl_common(qp, dev, rsp, false);
	if (rc)
		return rc;

	if (is_state_out_of_reset(atomic_read(&qp->state))) {
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
	struct ntrdma_rqp *rqp = NULL;
	struct ntrdma_qp_cmd_cb *qpcb;
	int rc;

	if (qp && dev)
		rqp = ntrdma_dev_rqp_look_and_get(dev, qp->rqp_key);
	TRACE("qp %p rqp %p - key %d", qp, rqp, qp ? qp->res.key : rqp ?
			rqp->rres.key : -1);
	ntrdma_qp_send_stall(qp, rqp);
	if (rqp)
		ntrdma_rqp_put(rqp);

	ntrdma_res_start_cmds(&qp->res);

	qpcb = kmalloc_node(sizeof(*qpcb),
			    GFP_KERNEL, dev->node);
	if (!qpcb) {
		rc = -ENOMEM;
		goto err;
	}
	WARN(qp->ibqp.qp_type == IB_QPT_GSI, "try to delete qp type IB_QPT_GSI");
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
				  union ntrdma_cmd *cmd, struct dma_chan *req)
{
	struct ntrdma_qp_cmd_cb *qpcb = ntrdma_cmd_cb_qpcb(cb);
	struct ntrdma_qp *qp = qpcb->qp;
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);

	ntrdma_vdbg(dev, "called\n");

	cmd->qp_delete.hdr.op = NTRDMA_CMD_QP_DELETE;
	cmd->qp_delete.qp_key = qp->res.key;

	return 0;
}

static int ntrdma_qp_disable_cmpl(struct ntrdma_cmd_cb *cb,
				const union ntrdma_rsp *rsp,
				struct dma_chan *req)
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
	struct ntrdma_qp *qp;
	struct ntrdma_dev *dev;
	struct ntrdma_rqp *rqp = NULL;
	int start, end, base;

	qp = ntrdma_res_qp(res);
	dev = ntrdma_qp_dev(qp);
	if (dev) {
		rqp = ntrdma_dev_rqp_look_and_get(dev, qp->rqp_key);
		if (rqp) {
			rqp->state = IB_QPS_ERR;
			ntrdma_rqp_put(rqp);
		}
	}
	TRACE("qp reset %p (res key: %d) rqp %p\n", qp, qp->res.key, rqp);
	spin_lock_bh(&qp->recv_prod_lock);
	{
		if (qp->ibqp.qp_type != IB_QPT_GSI)
			move_to_err_state(qp);
		ntc_remote_buf_clear(&qp->peer_recv_wqe_buf);
	}
	spin_unlock_bh(&qp->recv_prod_lock);

	spin_lock_bh(&qp->send_prod_lock);
	{
		ntc_remote_buf_clear(&qp->peer_send_wqe_buf);
		qp->peer_send_vbell_idx = 0;
	}
	spin_unlock_bh(&qp->send_prod_lock);
	/* GSI is always active, we can move it to SQE only if we generate
	 * error completion, here we check if there are completion waiting
	 * if yet we move it to aborting and will generate the completion
	 * later, if not it is transparent, and no need for the aborting
	 */
	if (qp->ibqp.qp_type == IB_QPT_GSI) {
		ntrdma_qp_send_cmpl_start(qp);
		ntrdma_ring_consume(qp->send_abort ? qp->send_post :
				ntrdma_qp_send_cons(qp), qp->send_cmpl,
				qp->send_cap, &start, &end, &base);
		ntrdma_qp_send_cmpl_done(qp);
		if (start != end)
			qp->send_aborting = true;
	} else
		qp->send_aborting = true;
	/* GSI can have only send abort (in SQE state) */
	if (qp->ibqp.qp_type != IB_QPT_GSI)
		qp->recv_aborting = true;
}

static void ntrdma_rqp_free(struct ntrdma_rres *rres)
{
	struct ntrdma_rqp *rqp = ntrdma_rres_rqp(rres);

	ntrdma_rqp_del(rqp);
	ntrdma_rres_del_unsafe(&rqp->rres);
	ntrdma_rqp_deinit(rqp);
	kfree(rqp);
}

static inline int ntrdma_rqp_init_deinit(struct ntrdma_rqp *rqp,
		struct ntrdma_dev *dev,
		struct ntrdma_rqp_init_attr *attr,
		u32 key, int is_deinit)
{
	int rc;
	u32 send_prod;
	u64 send_wqes_total_size;
	u32 recv_prod;
	u64 recv_wqes_total_size;

	if (is_deinit)
		goto deinit;

	rc = ntrdma_rres_init(&rqp->rres, dev, &dev->rqp_vec,
			      ntrdma_rqp_free, key);
	if (rc)
		goto err_rres;

	rqp->pd_key = attr->pd_key;
	rqp->access = 0;

	rqp->state = 0;
	rqp->qp_key = -1;

	rqp->send_wqe_sg_cap = attr->send_wqe_sg_cap;
	rqp->recv_wqe_sg_cap = attr->recv_wqe_sg_cap;
	rqp->send_wqe_size = ntrdma_send_wqe_size(rqp->send_wqe_sg_cap);
	rqp->recv_wqe_size = ntrdma_recv_wqe_size(rqp->recv_wqe_sg_cap);

	/* set up the send work ring */

	rqp->send_cap = attr->send_wqe_cap;
	rqp->send_cons = attr->send_wqe_idx;

	/* set up the send work queue buffer */
	send_prod = attr->send_wqe_idx;
	send_wqes_total_size = rqp->send_cap * rqp->send_wqe_size;
	rc = ntc_export_buf_zalloc_init(&rqp->send_wqe_buf, dev->ntc,
					send_wqes_total_size
					+ sizeof(u32), /* for send_prod */
					GFP_KERNEL, &send_prod, sizeof(u32),
					send_wqes_total_size);
	if (rc < 0)
		goto err_send_wqe_buf;

	/* set up the send completion queue buffer */
	rc = ntc_local_buf_zalloc(&rqp->send_cqe_buf, dev->ntc,
				rqp->send_cap * sizeof(struct ntrdma_cqe),
				GFP_KERNEL);
	if (rc < 0)
		goto err_send_cqe_buf;

	/* peer qp send queue info is provided */

	rqp->peer_send_cqe_buf = attr->peer_send_cqe_buf;
	rqp->peer_send_cons_shift = attr->peer_send_cons_shift;
	rqp->peer_cmpl_vbell_idx = attr->peer_cmpl_vbell_idx;

	/* set up the recv work ring */

	rqp->recv_cap = attr->recv_wqe_cap;
	rqp->recv_cons = attr->recv_wqe_idx;

	/* set up the recv work queue buffer */
	recv_prod = attr->recv_wqe_idx;
	recv_wqes_total_size = rqp->recv_cap * rqp->recv_wqe_size;
	rc = ntc_export_buf_zalloc_init(&rqp->recv_wqe_buf, dev->ntc,
					recv_wqes_total_size
					+ sizeof(u32), /* for recv_prod */
					GFP_KERNEL, &recv_prod, sizeof(u32),
					recv_wqes_total_size);
	if (rc < 0)
		goto err_recv_wqe_buf;

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
	ntc_export_buf_free(&rqp->recv_wqe_buf);
err_recv_wqe_buf:
	ntc_local_buf_free(&rqp->send_cqe_buf);
err_send_cqe_buf:
	ntc_export_buf_free(&rqp->send_wqe_buf);
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

	rqp->state = IB_QPS_RESET;

	ntrdma_dev_vbell_del(dev, &rqp->send_vbell);

	tasklet_kill(&rqp->send_work);

	ntrdma_debugfs_rqp_del(rqp);
}

const struct ntrdma_recv_wqe *ntrdma_rqp_recv_wqe(struct ntrdma_rqp *rqp,
						u32 pos)
{
	return ntc_export_buf_const_deref(&rqp->recv_wqe_buf,
					pos * rqp->recv_wqe_size,
					rqp->recv_wqe_size);
}

const struct ntrdma_send_wqe *ntrdma_rqp_send_wqe(struct ntrdma_rqp *rqp,
						u32 pos)
{
	return ntc_export_buf_const_deref(&rqp->send_wqe_buf,
					pos * rqp->send_wqe_size,
					rqp->send_wqe_size);
}

struct ntrdma_cqe *ntrdma_rqp_send_cqe(struct ntrdma_rqp *rqp,
				       u32 pos)
{
	return ntc_local_buf_deref(&rqp->send_cqe_buf) +
		pos * sizeof(struct ntrdma_cqe);
}

static void ntrdma_send_fail(struct ntrdma_cqe *cqe,
			const struct ntrdma_send_wqe *wqe, int op_status)
{
	cqe->ulp_handle = wqe->ulp_handle;
	cqe->op_code = wqe->op_code;
	cqe->op_status = op_status;
	cqe->rdma_len = 0;
	cqe->imm_data = 0;
	cqe->flags = wqe->flags;
}

static void ntrdma_send_done(struct ntrdma_cqe *cqe,
			const struct ntrdma_send_wqe *wqe,
			u32 rdma_len)
{
	cqe->ulp_handle = wqe->ulp_handle;
	cqe->op_code = wqe->op_code;
	cqe->op_status = NTRDMA_WC_SUCCESS;
	cqe->rdma_len = rdma_len;
	cqe->imm_data = 0;
	cqe->flags = wqe->flags;
}

void ntrdma_recv_wqe_sync(struct ntrdma_recv_wqe *wqe)
{
	int i;

	for (i = 0; i < wqe->sg_count; ++i) {
		if (wqe->rcv_sg_list[i].key != NTRDMA_RESERVED_DMA_LEKY)
			continue;

		TRACE("DMA memcpy %#x bytes to %#lx virt %p\n",
			(int)wqe->rcv_sg_list[i].exp_buf.size,
			(long)wqe->rcv_sg_list[i].rcv_dma_buf.dma_addr,
			phys_to_virt(wqe->rcv_sg_list[i].rcv_dma_buf.dma_addr));

		memcpy(phys_to_virt(wqe->rcv_sg_list[i].rcv_dma_buf.dma_addr),
			wqe->rcv_sg_list[i].exp_buf.ptr,
			wqe->rcv_sg_list[i].exp_buf.size);
	}
}

void ntrdma_recv_wqe_cleanup(struct ntrdma_recv_wqe *wqe)
{
	int i;

	for (i = 0; i < wqe->sg_count; ++i)
		if (wqe->rcv_sg_list[i].key == NTRDMA_RESERVED_DMA_LEKY)
			ntc_export_buf_free(&wqe->rcv_sg_list[i].exp_buf);

	wqe->sg_count = 0;
}

static void ntrdma_recv_fail(struct ntrdma_cqe *recv_cqe,
			struct ntrdma_recv_wqe *recv_wqe, int op_status)
{
	recv_cqe->ulp_handle = recv_wqe->ulp_handle;
	recv_cqe->op_code = recv_wqe->op_code;
	recv_cqe->op_status = op_status;
	recv_cqe->rdma_len = 0;
	recv_cqe->imm_data = 0;
	ntrdma_recv_wqe_cleanup(recv_wqe);
}

static u16 ntrdma_send_recv_opcode(const struct ntrdma_send_wqe *send_wqe)
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

static u32 ntrdma_send_recv_len(const struct ntrdma_send_wqe *send_wqe)
{
	switch (send_wqe->op_code) {
	case NTRDMA_WR_SEND:
	case NTRDMA_WR_SEND_INV:
	case NTRDMA_WR_SEND_IMM:
		return send_wqe->rdma_len;
	}
	return 0;
}

static u32 ntrdma_send_recv_imm(const struct ntrdma_send_wqe *send_wqe)
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
			const struct ntrdma_send_wqe *send_wqe)
{
	recv_cqe->ulp_handle = recv_wqe->ulp_handle;
	recv_cqe->op_code = ntrdma_send_recv_opcode(send_wqe);
	recv_cqe->op_status = NTRDMA_WC_SUCCESS;
	recv_cqe->rdma_len = ntrdma_send_recv_len(send_wqe);
	recv_cqe->imm_data = ntrdma_send_recv_imm(send_wqe);
	ntrdma_recv_wqe_sync(recv_wqe);
	ntrdma_recv_wqe_cleanup(recv_wqe);
}

int ntrdma_qp_recv_post_start(struct ntrdma_qp *qp)
{
	struct ntrdma_dev *dev;

	mutex_lock(&qp->recv_post_lock);
	if (!is_state_out_of_reset(atomic_read(&qp->state))) {
		dev = ntrdma_qp_dev(qp);
		ntrdma_err(dev, "qp %d state %d\n", qp->res.key,
				atomic_read(&qp->state));
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
	struct ntrdma_dev *dev;

	spin_lock_bh(&qp->recv_prod_lock);
	if (!is_state_recv_ready(atomic_read(&qp->state))) {
		dev = ntrdma_qp_dev(qp);
		ntrdma_info(dev, "qp %d state %d will retry", qp->res.key,
			atomic_read(&qp->state));
		spin_unlock_bh(&qp->recv_prod_lock);
		return -EAGAIN;
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
	struct ntrdma_dev *dev;

	spin_lock_bh(&qp->recv_cons_lock);
	if (!is_state_recv_ready(atomic_read(&qp->state))) {
		dev = ntrdma_qp_dev(qp);
		ntrdma_err(dev, "qp %d state %d\n", qp->res.key,
				atomic_read(&qp->state));
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
	u32 recv_cons;

	if (qp->recv_abort) {
		recv_cons = qp->recv_post;
		qp->recv_cons = recv_cons;
	} else
		recv_cons = qp->recv_cons;
	ntrdma_ring_consume(recv_cons, qp->recv_cmpl, qp->recv_cap,
			pos, end, base);

	if (qp->recv_aborting && (*pos == *end) && !qp->recv_abort) {
		qp->recv_abort = true;
		qp->recv_abort_first = true;
		TRACE(
				"qp %p (res key %d): move from aborting to abort, pos = end = %d, cons %d, post %d\n",
				qp, qp->res.key, *pos, recv_cons,
				qp->recv_post);
	}
	if (qp->recv_aborting && (*pos != *end))
		TRACE(
				"qp %p (res key: %d): recv_bort %d, post %d, cons %d, cmpl %d, cap %d, pos %d, end %d, base %d\n",
				qp, qp->res.key, qp->recv_abort,
				qp->recv_post, qp->recv_cons, qp->recv_cmpl,
				qp->recv_cap, *pos, *end, *base);
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

	/* TODO need to put this lock in rdma_core and not here */
	if (qp->ibqp.qp_type != IB_QPT_GSI)
		mutex_lock(&qp->send_post_lock);
	else
		spin_lock(&qp->send_post_slock);

	if (!is_state_send_ready(atomic_read(&qp->state))) {
		ntrdma_err(dev, "qp %d state %d\n", qp->res.key,
				atomic_read(&qp->state));
		if (qp->ibqp.qp_type != IB_QPT_GSI)
			mutex_unlock(&qp->send_post_lock);
		else
			spin_unlock(&qp->send_post_slock);
		ntrdma_dbg(dev, "invalid qp %d state %u\n", qp->res.key,
				atomic_read(&qp->state));
		return -EINVAL;
	}

	return 0;
}

void ntrdma_qp_send_post_done(struct ntrdma_qp *qp)
{
	tasklet_schedule(&qp->send_work);
	if (qp->ibqp.qp_type != IB_QPT_GSI)
		mutex_unlock(&qp->send_post_lock);
	else
		spin_unlock(&qp->send_post_slock);
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
	struct ntrdma_dev *dev;

	spin_lock_bh(&qp->send_prod_lock);
	if (atomic_read(&qp->state) != IB_QPS_RTS) {
		dev = ntrdma_qp_dev(qp);
		ntrdma_info(dev, "qp %d state %d will retry", qp->res.key,
			atomic_read(&qp->state));
		spin_unlock_bh(&qp->send_prod_lock);
		return -EAGAIN;
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
	if (qp->send_abort) {
		TRACE("qp %d (already in abort)\n", qp->res.key);
		send_cons = qp->send_post;
		ntrdma_qp_set_send_cons(qp, send_cons);
	}
	else
		send_cons = ntrdma_qp_send_cons(qp);

	ntrdma_ring_consume(send_cons, qp->send_cmpl, qp->send_cap,
			    pos, end, base);
	if (qp->send_aborting && (*pos == *end) && !qp->send_abort) {
		qp->send_abort = true;
		qp->send_abort_first = true;
		TRACE(
				"qp %p (res key %d): move from aborting to abort, pos = end = %d, cons %d, post %d\n",
				qp, qp->res.key, *pos, send_cons,
				qp->send_post);
	}
	if (qp->send_aborting && (*pos != *end))
		TRACE("qp %p (res key: %d): send_abort %d, post %d, cons %d, cmpl %d, cap %d, pos %d, end %d, base %d\n",
			qp, qp->res.key, qp->send_abort, qp->send_post,
			ntrdma_qp_send_cons(qp), qp->send_cmpl, qp->send_cap,
			*pos, *end, *base);
}

static void ntrdma_qp_send_cmpl_put(struct ntrdma_qp *qp,
				    u32 pos, u32 base)
{
	qp->send_cmpl = ntrdma_ring_update(pos, base, qp->send_cap);
}

static int ntrdma_rqp_recv_cons_start(struct ntrdma_rqp *rqp)
{
	struct ntrdma_dev *dev;

	spin_lock_bh(&rqp->recv_cons_lock);
	if (!is_state_recv_ready(rqp->state)) {
		dev = ntrdma_rqp_dev(rqp);
		ntrdma_err(dev, "rqp %d state %d\n", rqp->rres.key, rqp->state);
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
	u32 recv_prod = ntrdma_rqp_recv_prod(rqp);

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
	struct ntrdma_dev *dev;

	spin_lock_bh(&rqp->send_cons_lock);
	if (!is_state_send_ready(rqp->state)) {
		dev = ntrdma_rqp_dev(rqp);
		ntrdma_info(dev, "rqp %d state %d will retry",
			rqp->rres.key, rqp->state);
		spin_unlock_bh(&rqp->send_cons_lock);
		return -EAGAIN;
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
	u32 send_prod = ntrdma_rqp_send_prod(rqp);

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
	int rc = 0;
	struct ntrdma_qp *qp = ntrdma_recv_poll_qp(poll);
	u32 pos, end, base;

	ntrdma_qp_recv_cmpl_start(qp);
	ntrdma_qp_recv_cmpl_get(qp, &pos, &end, &base);

	if (pos == end) {
		/* update once qp->recv_cons */
		if (qp->recv_abort_first)
			ntrdma_qp_recv_cmpl_get(qp, &pos, &end, &base);
		if (pos == end) {
			ntrdma_qp_recv_cmpl_done(qp);
			rc = -EAGAIN;
		}
	}

	end = min_t(u32, end, pos + NTRDMA_QP_BATCH_SIZE);

	*poll_qp = qp;
	*poll_pos = pos;
	*poll_end = end;
	*poll_base = base;

	return rc;
}

static void ntrdma_qp_poll_recv_put_and_done(struct ntrdma_poll *poll,
					     u32 pos, u32 base)
{
	struct ntrdma_qp *qp = ntrdma_recv_poll_qp(poll);

	ntrdma_qp_recv_cmpl_put(qp, pos, base);
	ntrdma_qp_recv_cmpl_done(qp);
}

static
const struct ntrdma_cqe *ntrdma_qp_poll_recv_cqe(struct ntrdma_poll *poll,
						struct ntrdma_cqe *abort_cqe,
						u32 pos)
{
	struct ntrdma_qp *qp = ntrdma_recv_poll_qp(poll);
	const struct ntrdma_cqe *cqe = ntrdma_qp_recv_cqe(qp, pos);
	struct ntrdma_recv_wqe *wqe = ntrdma_qp_recv_wqe(qp, pos);
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);
	int opt;

	if (qp->recv_abort) {
		opt = qp->recv_abort_first ? NTRDMA_WC_ERR_LOC_PORT :
				NTRDMA_WC_ERR_ABORTED;
		ntrdma_recv_fail(abort_cqe, wqe, NTRDMA_WC_ERR_ABORTED);
		qp->recv_abort_first = false;
		ntrdma_info(dev, "fail completion opt %d QP %d\n",
				opt, qp->res.key);
		return abort_cqe;
	}
	if (wqe->op_status) {
		qp->recv_aborting = true;
		ntrdma_recv_fail(abort_cqe, wqe, wqe->op_status);
		TRACE("qp %d wqe status %d\n", qp->res.key, wqe->op_status);
		return abort_cqe;
	}
	if (cqe->op_status) {
		qp->recv_aborting = true;
		TRACE("qp %d cqe status %d\n", qp->res.key, cqe->op_status);
	}

	return cqe;
}

static int ntrdma_qp_poll_send_start_and_get(struct ntrdma_poll *poll,
					     struct ntrdma_qp **poll_qp, u32 *poll_pos,
					     u32 *poll_end, u32 *poll_base)
{
	int rc = 0;
	struct ntrdma_qp *qp = ntrdma_send_poll_qp(poll);
	u32 pos, end, base;

	ntrdma_qp_send_cmpl_start(qp);
	ntrdma_qp_send_cmpl_get(qp, &pos, &end, &base);

	if (pos == end) {
		/* In this cae we update once the qp->send_cons_buf */
		if (qp->send_abort_first)
			ntrdma_qp_send_cmpl_get(qp, &pos, &end, &base);
		if (pos == end) {
			ntrdma_qp_send_cmpl_done(qp);
			rc =  -EAGAIN;
		}
	}

	end = min_t(u32, end, pos + NTRDMA_QP_BATCH_SIZE);

	*poll_qp = qp;
	*poll_pos = pos;
	*poll_end = end;
	*poll_base = base;

	return rc;
}

static void ntrdma_qp_poll_send_put_and_done(struct ntrdma_poll *poll,
					     u32 pos, u32 base)
{
	struct ntrdma_qp *qp = ntrdma_send_poll_qp(poll);

	ntrdma_qp_send_cmpl_put(qp, pos, base);
	ntrdma_qp_send_cmpl_done(qp);
}

static
const struct ntrdma_cqe *ntrdma_qp_poll_send_cqe(struct ntrdma_poll *poll,
						struct ntrdma_cqe *abort_cqe,
						u32 pos)
{
	struct ntrdma_qp *qp = ntrdma_send_poll_qp(poll);
	const struct ntrdma_cqe *cqe = ntrdma_qp_send_cqe(qp, pos);
	const struct ntrdma_send_wqe *wqe = ntrdma_qp_send_wqe(qp, pos);
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);
	int opt;

	if (qp->send_abort) {
		opt = qp->send_abort_first ? NTRDMA_WC_ERR_LOC_PORT :
				NTRDMA_WC_ERR_ABORTED;
		ntrdma_send_fail(abort_cqe, wqe, opt);
		qp->send_abort_first = false;
		ntrdma_info(dev, "fail completion opt %d QP %d\n",
				opt, qp->res.key);
		return abort_cqe;
	}

	if (wqe->op_status) {
		/* TODO: should not happen if we are here and not in aborting
		 * already
		 */
		qp->send_aborting = true;
		ntrdma_send_fail(abort_cqe, wqe, wqe->op_status);
		TRACE("qp %d wqe->op_status %d, move to abort\n", qp->res.key,
				wqe->op_status);
		return abort_cqe;
	}

	if (cqe->op_status) {
		qp->send_aborting = true;
		TRACE("qp %d, cqe->op_status %d, move to abort\n",
				qp->res.key, cqe->op_status);
	}

	return cqe;
}

static void ntrdma_qp_recv_work(struct ntrdma_qp *qp)
{
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);
	struct dma_chan *req;
	u32 start, end, base;
	size_t off, len;
	int rc;

	/* verify the qp state and lock for producing recvs */
	rc = ntrdma_qp_recv_prod_start(qp);
	if (rc) {
		if (qp->ibqp.qp_type != IB_QPT_GSI)
			qp->recv_aborting = true;
		return;
	}

	/* get the next producing range in the ring */
	ntrdma_qp_recv_prod_get(qp, &start, &end, &base);

	if (start == end)
		goto out;

	req = ntc_req_create(dev->ntc);
	if (!req)
		goto out;

	for (;;) {
		ntrdma_qp_recv_prod_put(qp, end, base);

		/* send the portion of the ring */
		off = start * qp->recv_wqe_size;
		len = (end - start) * qp->recv_wqe_size;
		rc = ntc_request_memcpy_fenced(req,
					&qp->peer_recv_wqe_buf, off,
					&qp->recv_wqe_buf, off,
					len);

		TRACE("QP %d start %u end %u\n",
				qp->res.key, start, end);

		if (rc < 0) {
			ntrdma_err(dev,
				"ntc_request_memcpy (len=%zu) error %d",
				len, -rc);
			break;
		}

		ntrdma_qp_recv_prod_get(qp, &start, &end, &base);
		if (start == end)
			break;
	}

	/* send the prod idx */
	rc = ntc_request_imm32(req,
			&qp->peer_recv_wqe_buf, qp->peer_recv_prod_shift,
			qp->recv_prod, true, NULL, NULL);
	if (rc < 0)
		ntrdma_err(dev, "ntc_request_imm32 failed. rc=%d\n", rc);

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
	struct dma_chan *req;
	struct ntrdma_send_wqe *wqe;
	const struct ntrdma_recv_wqe *recv_wqe = NULL;
	u32 recv_wqe_recv_pos = 0;
	struct ntrdma_wr_rcv_sge rdma_sge;
	u32 start, pos, end, base;
	u32 recv_pos, recv_end, recv_base;
	u32 rcv_start_offset;
	u32 rdma_len;
	size_t off, len;
	int rc;
	bool abort = false;

	/* verify the qp state and lock for producing sends */
	rc = ntrdma_qp_send_prod_start(qp);
	if (rc) {
		ntrdma_err(dev, "ntrdma_qp_send_prod_start failed rc = %d qp %d(%p)\n",
				rc,qp->res.key, qp);
		return;
	}

	/* get the next producing range in the send ring */
	ntrdma_qp_send_prod_get(qp, &start, &end, &base);
	if (qp->ibqp.qp_type == IB_QPT_GSI)
		ntrdma_info(dev, "qp %d, start %d, end %d, base %d\n",
				qp->res.key, start, end, base);
	/* quit if there is no send work to do */
	if (start == end) {
		ntrdma_qp_send_prod_done(qp);
		return;
	}

	/* limit the range to batch size */
	end = min_t(u32, end, start + NTRDMA_QP_BATCH_SIZE);

	/* On GSI qp we do not change qp state in reset, since it cannot move
	 * to SQE state without error completion, we move it to aborting
	 * and on first send (here) we will provide the error completion
	 * and move to error state
	 */
	if (qp->send_abort || qp->send_aborting) {
		ntrdma_err(dev, "qp %d in abort process\n", qp->res.key);
		goto err_rqp;
	}
	/* sending requires a connected rqp */
	rqp = ntrdma_dev_rqp_look_and_get(dev, qp->rqp_key);
	if (!rqp) {
		ntrdma_err(dev, "ntrdma_dev_rqp_look failed %d key %d\n",
				rc, qp->rqp_key);
		goto err_rqp;
	}


	/* connected rqp must be ready to receive */
	rc = ntrdma_rqp_recv_cons_start(rqp);
	if (rc) {
		ntrdma_err(dev, "ntrdma_rqp_recv_cons_start failed %d\n", rc);
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
				ntrdma_info_ratelimited(dev,
						"send but no recv QP %d, pos %u end %u base %u prod %u cons %u\n",
						qp->res.key,
						recv_pos, recv_end, recv_base,
						ntrdma_rqp_recv_prod(rqp),
						rqp->recv_cons);

#ifdef CONFIG_NTRDMA_RETRY_RECV
				--pos;
				tasklet_schedule(&qp->send_work);
#else
				if (!wqe->op_status)
					wqe->op_status = NTRDMA_WC_ERR_RECV_MISSING;

				abort = true;
				move_to_err_state(qp);
#endif
				break;
			}

			recv_wqe_recv_pos = recv_pos;
			recv_wqe = ntrdma_rqp_recv_wqe(rqp, recv_pos++);

			if (recv_wqe->op_status) {
				if (!wqe->op_status) {
					ntrdma_err(dev,
							"wqe->op_status %d recv_wqe->op_status %d recv_pos %u qp %d\n",
							wqe->op_status,
							recv_wqe->op_status,
							recv_pos,
							qp->res.key);
					wqe->op_status = recv_wqe->op_status;
				}

				abort = true;
				move_to_err_state(qp);
				break;
			}

			if (recv_pos == recv_end) {
				ntrdma_rqp_recv_cons_put(rqp, recv_pos, recv_base);
				ntrdma_rqp_recv_cons_get(rqp, &recv_pos, &recv_end, &recv_base);
			}
		}

		if (wqe->op_status) {
			ntrdma_err(dev, "op status %d qp %d\n",
					wqe->op_status,
					qp->res.key);

			abort = true;
			move_to_err_state(qp);
			break;
		}

		if (ntrdma_wr_code_push_data(wqe->op_code)) {
			if (ntrdma_wr_code_is_rdma(wqe->op_code)) {
				if (wqe->rdma_key != NTRDMA_RESERVED_DMA_LEKY) {
					rdma_sge.addr = wqe->rdma_addr;
					rdma_sge.len = ~(u32)0;
					rdma_sge.key = wqe->rdma_key;
					/* From send to RDMA address. */
					rc = ntrdma_zip_rdma(dev, req,
							&rdma_len,
							&rdma_sge,
							wqe->snd_sg_list,
							1, wqe->sg_count, 0);
				} else
					rc = -EINVAL;
			} else {
				if (qp->ibqp.qp_type == IB_QPT_GSI)
					rcv_start_offset =
						sizeof(struct ib_grh);
				else
					rcv_start_offset = 0;
				/* This goes from send to post recv */
				rc = ntrdma_zip_rdma(dev, req, &rdma_len,
						recv_wqe->rcv_sg_list,
						wqe->snd_sg_list,
						recv_wqe->sg_count,
						wqe->sg_count,
						rcv_start_offset);
			}
			if (rc) {
				ntrdma_err(dev,
						"ntrdma_zip_rdma failed %d qp %d\n",
						rc, qp->res.key);

				wqe->op_status = NTRDMA_WC_ERR_RDMA_RANGE;
				abort = true;
				move_to_err_state(qp);
				break;
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

	if (abort)
		goto err_memcpy;

	if (unlikely(pos == start)) {
		goto done;
	}

	/* send the portion of the ring */
	off = start * qp->send_wqe_size;
	len = (pos - start) * qp->send_wqe_size;
	rc = ntc_request_memcpy_fenced(req,
				&qp->peer_send_wqe_buf, off,
				&qp->send_wqe_buf, off, len);
	if (rc < 0) {
		ntrdma_err(dev,
			"ntc_request_memcpy (len=%zu) error %d", len, -rc);

		abort = true;
		move_to_err_state(qp);
		goto err_memcpy;
	}

	this_cpu_add(dev_cnt.qp_send_work_bytes, len);

	/* send the prod idx */
	rc = ntc_request_imm32(req,
			&qp->peer_send_wqe_buf, qp->peer_send_prod_shift,
			qp->send_prod, true, NULL, NULL);
	if (rc < 0) {
		ntrdma_err(dev, "ntc_request_imm32 failed. rc=%d\n", rc);

		abort = true;
		move_to_err_state(qp);
		goto err_memcpy;
	}
	/* update the vbell and signal the peer */
	/* TODO: return value is ignored! */
	ntrdma_dev_vbell_peer(dev, req,
			qp->peer_send_vbell_idx);

	/* TODO: return value is ignored! */
	ntc_req_signal(dev->ntc, req, NULL, NULL, NTB_DEFAULT_VEC(dev->ntc));

	TRACE("start %u pos %u QP %d RQP %d prod %u peer vbell idx %d\n",
			start, pos,
			qp->res.key, rqp->qp_key,
			qp->send_prod,
			qp->peer_send_vbell_idx);

	/* submit the request */
	/* TODO: return value is ignored! */
	ntc_req_submit(dev->ntc, req);

	/* release lock for state change or producing later sends */
done:
	ntrdma_qp_send_prod_done(qp);
	return;
err_memcpy:
	ntrdma_qp_send_prod_done(qp);
	ntrdma_err(dev, "err_memcpy - rc = %d on qp %p", rc, qp);
	ntrdma_unrecoverable_err(dev);
	return;

err_recv:
	ntrdma_rqp_put(rqp);
err_rqp:
	if (qp->ibqp.qp_type == IB_QPT_GSI) {
		atomic_set(&qp->state, IB_QPS_SQE);
		qp->send_aborting = true;
		qp->send_abort = false;
		qp->send_abort_first = false;
		ntrdma_qp_send_prod_put(qp, end, base);
		ntrdma_qp_send_prod_done(qp);
		ntrdma_cq_cue(qp->send_cq);
	} else
		ntrdma_qp_send_prod_done(qp);
	ntrdma_err(dev, "err_rqp QP %d aborting = %d qp %p, cq %p end %d\n",
			qp->send_aborting, qp->res.key, qp, qp->send_cq, end);
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
	struct dma_chan *req;
	struct ntrdma_cqe *cqe, *recv_cqe = NULL;
	const struct ntrdma_send_wqe *wqe;
	struct ntrdma_recv_wqe *recv_wqe = NULL;
	struct ntrdma_wr_rcv_sge rdma_sge;
	u32 start, pos, end, base;
	u32 recv_pos, recv_end, recv_base;
	size_t off, len;
	bool cue_recv = false;
	int rc;
	bool do_signal = false;
	bool abort = false;

	/* verify the rqp state and lock for consuming sends */
	rc = ntrdma_rqp_send_cons_start(rqp);
	if (rc) {
		if (rc != -EAGAIN)
			ntrdma_err(dev,
				"ntrdma_rqp_send_cons_start error %d rqp=%p",
				-rc, rqp);
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
		/* TODO: if not EAGAIN ??? */
		return;
	}

	/* limit the range to batch size */
	end = min_t(u32, end, start + NTRDMA_QP_BATCH_SIZE);

	/* sending requires a connected qp */
	qp = ntrdma_dev_qp_look_and_get(dev, rqp->qp_key);
	if (!qp) {
		ntrdma_err(dev, "ntrdma_dev_qp_look failed qp key %d\n",
				rqp->qp_key);
		goto err_qp;
	}
	/* FIXME: need to complete the send with error??? */

	/* connected qp must be ready to receive */
	rc = ntrdma_qp_recv_cons_start(qp);
	if (rc) {
		ntrdma_err(dev, "ntrdma_qp_recv_cons_start failed qp key %d\n",
				rqp->qp_key);
		goto err_recv;
	}
	/* FIXME: need to complete the send with error??? */

	/* get the next consuming range in the recv ring */
	ntrdma_qp_recv_cons_get(qp, &recv_pos, &recv_end, &recv_base);

	req = ntc_req_create(dev->ntc);

	for (pos = start;;) {
		cqe = ntrdma_rqp_send_cqe(rqp, pos);
		wqe = ntrdma_rqp_send_wqe(rqp, pos++);

		if (wqe->flags & IB_SEND_SIGNALED)
			do_signal = true;

		if (ntrdma_wr_code_is_send(wqe->op_code)) {
			if (ntrdma_wr_status_no_recv(wqe->op_status)) {
				ntrdma_send_fail(cqe, wqe, wqe->op_status);
				ntrdma_err(dev, "WQE with error %d received on  qp %d\n",
						wqe->op_status,
						rqp->qp_key);
				abort = true;
				move_to_err_state(qp);
				break;
			}

			if (WARN_ON(wqe->recv_key != recv_pos + recv_base)) {
				if (!wqe->op_status)
					ntrdma_send_fail(cqe, wqe, NTRDMA_WC_ERR_CORRUPT);
				else
					ntrdma_send_fail(cqe, wqe, wqe->op_status);

				abort = true;
				move_to_err_state(qp);
				ntrdma_err(dev,
						"Error %s %d qp key %d, move to error state\n",
						__func__, __LINE__,
						rqp->qp_key);
				break;
			}

			if (recv_pos == recv_end) {
				if (WARN_ON(!wqe->op_status))
					ntrdma_send_fail(cqe, wqe, NTRDMA_WC_ERR_RECV_MISSING);
				else
					ntrdma_send_fail(cqe, wqe, wqe->op_status);

				abort = true;
				move_to_err_state(qp);
				ntrdma_err(dev,
						"Error %s %d qp key %d, move to error state\n",
						__func__, __LINE__,
						rqp->qp_key);
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

				abort = true;
				move_to_err_state(qp);
				ntrdma_err(dev, "Error %s %d qp key %d\n",
						__func__, __LINE__,
						rqp->qp_key);
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

			abort = true;
			move_to_err_state(qp);
			ntrdma_err(dev, "Error wqe op status %d  pos %u QP %d\n",
					wqe->op_status, pos, qp->res.key);
			break;
		}

		if (ntrdma_wr_code_push_data(wqe->op_code)) {
			if (ntrdma_wr_code_is_rdma(wqe->op_code)) {
				if (wqe->rdma_key != NTRDMA_RESERVED_DMA_LEKY) {
					rdma_sge.addr = wqe->rdma_addr;
					rdma_sge.len = wqe->rdma_len;
					rdma_sge.key = wqe->rdma_key;

					rc = ntrdma_zip_sync(dev, &rdma_sge, 1);
				} else
					rc = -EINVAL;
			} else {
				/* TODO: sync less than sg_count using rdma_len */
				rc = ntrdma_zip_sync(dev, recv_wqe->rcv_sg_list,
						     recv_wqe->sg_count);
			}

			WARN(rc, "ntrdma_zip_sync failed rc = %d qp key %d\n",
					rc, rqp->qp_key);
			/* FIXME: handle send sync error */

			if (ntrdma_wr_code_is_send(wqe->op_code))
				ntrdma_recv_done(recv_cqe, recv_wqe, wqe);

			ntrdma_send_done(cqe, wqe, wqe->rdma_len);
		}

		if (ntrdma_wr_code_pull_data(wqe->op_code)) {
			/* We do not support RDMA read. */
			rc = -EINVAL;
			ntrdma_send_fail(cqe, wqe, NTRDMA_WC_ERR_RDMA_ACCESS);
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
	/* TODO: What to do if went out of the loop with break on error?? */
	ntrdma_qp_recv_cons_put(qp, recv_pos, recv_base);
	ntrdma_qp_recv_cons_done(qp);

	if (cue_recv)
		ntrdma_cq_cue(qp->recv_cq);
	/* TODO: What if not? */

	ntrdma_qp_put(qp);

	ntrdma_rqp_send_cons_put(rqp, pos, base);

	if (abort) {
		ntrdma_err(dev, " failed to compete\n");
		goto err_memcpy;
	}

	/* send the portion of the ring */
	off = start * sizeof(struct ntrdma_cqe);
	len = (pos - start) * sizeof(struct ntrdma_cqe);
	rc = ntc_request_memcpy_fenced(req,
				&rqp->peer_send_cqe_buf, off,
				&rqp->send_cqe_buf, off, len);
	if (rc < 0) {
		ntrdma_err(dev,
			"ntc_request_memcpy (len=%zu) error %d qp key %d",
			len, -rc, rqp->qp_key);

		goto err_memcpy;
	}

	this_cpu_add(dev_cnt.tx_cqes, pos - start);
	/* send the cons idx */
	rc = ntc_request_imm32(req,
			&rqp->peer_send_cqe_buf, rqp->peer_send_cons_shift,
			rqp->send_cons, true, NULL, NULL);
	if (rc < 0) {
		ntrdma_err(dev, "ntc_request_imm32 failed. rc=%d\n", rc);
		goto err_memcpy;
	}

	if (do_signal) {
		/* update the vbell and signal the peer */
		ntrdma_dev_vbell_peer(dev, req,
				rqp->peer_cmpl_vbell_idx);
		ntc_req_signal(dev->ntc, req, NULL, NULL,
				NTB_DEFAULT_VEC(dev->ntc));

		TRACE("Signal QP %d RQP %d cons %u start %u pos %u peer vbell idx %d\n",
				qp->res.key, rqp->qp_key,
				rqp->send_cons,
				start,
				pos,
				rqp->peer_cmpl_vbell_idx);
	}
	/* submit the request */
	/* TODO: return cpde? */
	ntc_req_submit(dev->ntc, req);

	/* release lock for state change or consuming later sends */
	ntrdma_rqp_send_cons_done(rqp);
	return;

err_recv:
	ntrdma_qp_put(qp);
err_qp:
	ntrdma_rqp_send_cons_done(rqp);
	ntrdma_err(dev, "%s Failed qp key %d\n",
			__func__, rqp->qp_key);
	return;
err_memcpy:
	ntrdma_rqp_send_cons_done(rqp);
	ntrdma_err(dev, "%s Failed qp key %d\n",
			__func__, rqp->qp_key);
	ntrdma_unrecoverable_err(dev);
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

struct ntrdma_qp *ntrdma_dev_qp_look_and_get(struct ntrdma_dev *dev, int key)
{
	struct ntrdma_res *res;

	res = ntrdma_dev_res_look(dev, &dev->qp_vec, key);
	if (!res)
		return NULL;

	return ntrdma_res_qp(res);
}

struct ntrdma_rqp *ntrdma_dev_rqp_look_and_get(struct ntrdma_dev *dev, int key)
{
	struct ntrdma_rres *rres;

	rres = ntrdma_dev_rres_look(dev, &dev->rqp_vec, key);
	if (!rres)
		return NULL;

	return ntrdma_rres_rqp(rres);
}

void ntrdma_qp_send_stall(struct ntrdma_qp *qp, struct ntrdma_rqp *rqp)
{
	int rc;

	if (!qp && !rqp)
		return;
	if (qp) {
		TRACE("qp %p (res key %d)\n", qp, qp->res.key);
		rc = ntrdma_qp_send_prod_start(qp);
		if (!rc) {
			move_to_err_state(qp);
			qp->send_aborting = true;
			qp->recv_aborting = true;
			ntrdma_qp_send_prod_done(qp);
			TRACE("qp %d - aborting\n", qp->res.key);
		}
	}
	if (!rqp)
		return;
	TRACE("rqp %p (rres key %d)\n", rqp, rqp->rres.key);

	/* Just to sync */
	rc = ntrdma_rqp_send_cons_start(rqp);
	if (!rc)
		ntrdma_rqp_send_cons_done(rqp);
}


