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
#include <linux/stddef.h>
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

static struct kmem_cache *rqp_slab;
static struct kmem_cache *shadow_slab;

#define ntrdma_cmd_cb_qpcb(__cb) \
	container_of(__cb, struct ntrdma_qp_cmd_cb, cb)

static int ntrdma_qp_modify_prep(struct ntrdma_cmd_cb *cb,
				union ntrdma_cmd *cmd);
static int ntrdma_qp_modify_cmpl(struct ntrdma_cmd_cb *cb,
				const union ntrdma_rsp *rsp);
static int ntrdma_qp_enable_prep(struct ntrdma_cmd_cb *cb,
				union ntrdma_cmd *cmd);
static int ntrdma_qp_enable_cmpl(struct ntrdma_cmd_cb *cb,
				const union ntrdma_rsp *rsp);
static int ntrdma_qp_disable_prep(struct ntrdma_cmd_cb *cb,
				union ntrdma_cmd *cmd);
static int ntrdma_qp_disable_cmpl(struct ntrdma_cmd_cb *cb,
				const union ntrdma_rsp *rsp);

static void ntrdma_qp_enable_cb(struct ntrdma_res *res,
				struct ntrdma_cmd_cb *cb);
static void ntrdma_qp_disable_cb(struct ntrdma_res *res,
				struct ntrdma_cmd_cb *cb);

static void ntrdma_rqp_free(struct ntrdma_rres *rres);

static void ntrdma_qp_send_cmpl_get(struct ntrdma_qp *qp,
				    u32 *pos, u32 *end, u32 *base);

static int ntrdma_qp_poll_recv_start_and_get(struct ntrdma_poll *poll,
					     struct ntrdma_qp **pqp, u32 *pos, u32 *end,
					     u32 *base);
static void ntrdma_qp_poll_recv_put_and_done(struct ntrdma_poll *poll,
					     u32 pos, u32 base);
static void ntrdma_qp_poll_recv_cqe(struct ntrdma_poll *poll,
				struct ntrdma_cqe *outcqe, u32 pos);

static int ntrdma_qp_poll_send_start_and_get(struct ntrdma_poll *poll,
					     struct ntrdma_qp **pqp, u32 *pos, u32 *end,
					     u32 *base);
static void ntrdma_qp_poll_send_put_and_done(struct ntrdma_poll *poll,
					     u32 pos, u32 base);
static void ntrdma_qp_poll_send_cqe(struct ntrdma_poll *poll,
				struct ntrdma_cqe *outcqe, u32 pos);

static void ntrdma_rqp_send_work(struct ntrdma_rqp *rqp);

static void ntrdma_rqp_work_cb(unsigned long ptrhld);

void ntrdma_free_qp(struct ntrdma_qp *qp);

#define move_to_err_state(qp) move_to_err_state_d(qp, __func__, __LINE__)

static inline const u32 *ntrdma_qp_send_cons_buf(struct ntrdma_qp *qp)
{
	return ntc_export_buf_const_deref(&qp->send_cqe_buf,
					qp->send_cap *
					sizeof(struct ntrdma_cqe),
					sizeof(u32));
}

inline u32 ntrdma_qp_send_cons(struct ntrdma_qp *qp)
{
	const u32 *send_cons_buf = ntrdma_qp_send_cons_buf(qp);

	if (!send_cons_buf)
		return 0;

	return READ_ONCE(*send_cons_buf);
}

static inline void ntrdma_qp_set_send_cons(struct ntrdma_qp *qp, u32 send_cons)
{
	ntc_export_buf_reinit(&qp->send_cqe_buf, &send_cons,
			qp->send_cap * sizeof(struct ntrdma_cqe), sizeof(u32));
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

	return READ_ONCE(*send_prod_buf);
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

	return READ_ONCE(*recv_prod_buf);
}

static inline int ntrdma_qp_init_deinit(struct ntrdma_qp *qp,
		struct ntrdma_dev *dev,
		struct ntrdma_cq *recv_cq, struct ntrdma_cq *send_cq,
		struct ntrdma_qp_init_attr *attr,
		int is_deinit)
{
	int rc = 0;
	u32 send_cons = 0;
	u64 send_cqes_total_size;
	u32 pos;

	if (is_deinit)
		goto deinit;

	ntrdma_res_init(&qp->res, dev,
			ntrdma_qp_enable_cb, ntrdma_qp_disable_cb);

	if (attr->qp_type == IB_QPT_GSI || attr->qp_type == IB_QPT_SMI) {
		BUILD_BUG_ON(NTRDMA_QP_VEC_PREALLOCATED <= IB_QPT_GSI);
		BUILD_BUG_ON(NTRDMA_QP_VEC_PREALLOCATED <= IB_QPT_SMI);
		qp->res.key = attr->qp_type;
	} else {
		rc = ntrdma_kvec_reserve_key(&dev->qp_vec, dev->node);
		if (rc < 0)
			goto err_res;
		qp->res.key = rc;
	}

	ntc_init_dma_chan(&qp->dma_chan, dev->ntc, NTC_QP_DMA_CHAN);

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

	atomic_set(&qp->state, IB_QPS_RESET);
	qp->recv_abort = false;
	qp->recv_aborting = false;
	qp->recv_abort_first = false;
	qp->send_abort = false;
	qp->send_aborting = false;
	qp->send_abort_first = false;
	qp->access = 0;

	qp->rqp_key = -1;

	qp->send_wqe_sg_cap = attr->send_wqe_sg_cap;

	ntrdma_dbg(dev, "qp init: inline data cap %u\n",
			attr->send_wqe_inline_cap);
	qp->send_wqe_inline_cap = attr->send_wqe_inline_cap;
	qp->recv_wqe_sg_cap = attr->recv_wqe_sg_cap;
	qp->send_wqe_size = ntrdma_send_wqe_size(qp->send_wqe_sg_cap,
			attr->send_wqe_inline_cap);
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

	/* set up the send work queue buffer for statistics (latency calc) */
	qp->send_wqe_cycles_buf = kzalloc_node(qp->send_cap * sizeof(cycles_t),
						GFP_KERNEL, dev->node);
	if (!qp->send_wqe_cycles_buf)
		goto err_cycles_buf;

	/* set up the send completion queue buffer */
	send_cqes_total_size = qp->send_cap * sizeof(struct ntrdma_cqe);
	rc = ntc_export_buf_zalloc_init(&qp->send_cqe_buf, dev->ntc,
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

	return 0;

deinit:
	ntrdma_cq_del_poll(qp->send_cq, &qp->send_poll);
	ntrdma_cq_del_poll(qp->recv_cq, &qp->recv_poll);
	kfree(qp->recv_cqe_buf);
	qp->recv_cqe_buf = 0;
err_recv_cqe_buf:
	for (pos = 0; pos < qp->recv_cap; pos++)
		ntrdma_recv_wqe_cleanup(ntrdma_qp_recv_wqe(qp, pos));

	ntc_local_buf_free(&qp->recv_wqe_buf, dev->ntc);
err_recv_wqe_buf:
	ntc_export_buf_free(&qp->send_cqe_buf);
err_send_cqe_buf:
	kfree(qp->send_wqe_cycles_buf);
err_cycles_buf:
	ntc_local_buf_free(&qp->send_wqe_buf, dev->ntc);
err_send_wqe_buf:
	ntrdma_cq_put(qp->send_cq);
	ntrdma_cq_put(qp->recv_cq);
err_res:
	ntrdma_kvec_dispose_key(&dev->qp_vec, qp->res.key);
	if (qp->send_page) {
		put_page(qp->send_page);
		qp->send_page = NULL;
	}

	return rc;
}

int ntrdma_qp_init(struct ntrdma_qp *qp, struct ntrdma_dev *dev,
		   struct ntrdma_cq *recv_cq, struct ntrdma_cq *send_cq,
		   struct ntrdma_qp_init_attr *attr)
{
	return ntrdma_qp_init_deinit(qp, dev, recv_cq, send_cq, attr, false);
}

void ntrdma_qp_deinit(struct ntrdma_qp *qp, struct ntrdma_dev *dev)
{
	ntrdma_qp_init_deinit(qp, dev, NULL, NULL, NULL, true);
}

static void ntrdma_qp_release(struct kref *kref)
{
	struct ntrdma_obj *obj = container_of(kref, struct ntrdma_obj, kref);
	struct ntrdma_res *res = container_of(obj, struct ntrdma_res, obj);
	struct ntrdma_qp *qp = container_of(res, struct ntrdma_qp, res);
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);

	ntrdma_qp_deinit(qp, dev);
	WARN(!ntrdma_list_is_entry_poisoned(&obj->dev_entry),
		"Free list element while in the list, obj %p, res %p, qp %p (key %d)\n",
		obj, res, qp, qp->res.key);
	ntrdma_free_qp(qp);
	atomic_dec(&dev->qp_num);
}

void ntrdma_qp_put(struct ntrdma_qp *qp)
{
	ntrdma_res_put(&qp->res, ntrdma_qp_release);
}

inline struct ntrdma_send_wqe *ntrdma_qp_send_wqe(struct ntrdma_qp *qp,
					u32 pos)
{
	return ntc_local_buf_deref(&qp->send_wqe_buf) + pos * qp->send_wqe_size;
}

inline void ntrdma_qp_set_stats(struct ntrdma_qp *qp, u32 pos)
{
	qp->send_wqe_cycles_buf[pos] = get_cycles();
}

inline cycles_t ntrdma_qp_get_diff_cycles(struct ntrdma_qp *qp, u32 pos)
{
	return get_cycles() - qp->send_wqe_cycles_buf[pos];
}

inline
const struct ntrdma_cqe *ntrdma_qp_send_cqe(struct ntrdma_qp *qp, u32 pos)
{
	return ntc_export_buf_const_deref(&qp->send_cqe_buf,
					pos * sizeof(struct ntrdma_cqe),
					sizeof(struct ntrdma_cqe));
}

inline struct ntrdma_recv_wqe *ntrdma_qp_recv_wqe(struct ntrdma_qp *qp,
						u32 pos)
{
	return ntc_local_buf_deref(&qp->recv_wqe_buf) + pos * qp->recv_wqe_size;
}

inline struct ntrdma_cqe *ntrdma_qp_recv_cqe(struct ntrdma_qp *qp,
					u32 pos)
{
	return (void *)(qp->recv_cqe_buf + pos * sizeof(struct ntrdma_cqe));
}

int ntrdma_qp_modify(struct ntrdma_qp *qp)
{
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);
	struct ntrdma_qp_cmd_cb qpcb = {
		.cb = {
			.cmd_prep = ntrdma_qp_modify_prep,
			.rsp_cmpl = ntrdma_qp_modify_cmpl,
		},
		.qp = qp,
	};

	if (!dev->res_enable)
		return 0;

	init_completion(&qpcb.cb.cmds_done);

	mutex_lock(&dev->res_lock);

	ntrdma_dev_cmd_add(dev, &qpcb.cb);

	mutex_unlock(&dev->res_lock);

	ntrdma_dev_cmd_submit(dev);

	return ntrdma_res_wait_cmds(dev, &qpcb.cb, qp->res.timeout);
}

static int ntrdma_qp_modify_prep(struct ntrdma_cmd_cb *cb,
				union ntrdma_cmd *cmd)
{
	struct ntrdma_qp_cmd_cb *qpcb = ntrdma_cmd_cb_qpcb(cb);
	struct ntrdma_qp *qp = qpcb->qp;
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);

	ntrdma_vdbg(dev, "called\n");

	ntrdma_qp_recv_work(qp);

	cmd->qp_modify.hdr.op = NTRDMA_CMD_QP_MODIFY;
	cmd->qp_modify.src_qp_key = qp->res.key;
	cmd->qp_modify.access = qp->access;
	cmd->qp_modify.state = atomic_read(&qp->state);
	cmd->qp_modify.dest_qp_key = qp->rqp_key;

	return 0;
}

static int ntrdma_qp_modify_cmpl(struct ntrdma_cmd_cb *cb,
				const union ntrdma_rsp *rsp)
{
	struct ntrdma_qp_cmd_cb *qpcb = ntrdma_cmd_cb_qpcb(cb);
	struct ntrdma_qp *qp = qpcb->qp;
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);
	u32 status;
	int rc;

	ntrdma_vdbg(dev, "called\n");

	status = READ_ONCE(rsp->hdr.status);
	if (unlikely(status)) {
		ntrdma_err(dev, "rsp %p status %d", rsp, status);
		rc = -EIO;

		ntrdma_qp_recv_work(qp);

		goto out;
	}

	rc = 0;

 out:
	complete_all(&cb->cmds_done);

	return rc;
}

static void ntrdma_qp_enable_cb(struct ntrdma_res *res,
				struct ntrdma_cmd_cb *cb)
{
	struct ntrdma_dev *dev = ntrdma_res_dev(res);
	struct ntrdma_qp *qp = ntrdma_res_qp(res);
	struct ntrdma_qp_cmd_cb *qpcb;

	if (qp->ibqp.qp_type == IB_QPT_GSI) {
		qp->recv_prod = qp->recv_cons = qp->recv_cmpl;
		TRACE("Enabling GSI QP post %u prod %u cons %u cmpl %u\n",
				qp->recv_post, qp->recv_prod,
				qp->recv_cons, qp->recv_cmpl);
	}

	qpcb = container_of(cb, struct ntrdma_qp_cmd_cb, cb);

	qpcb->cb.cmd_prep = ntrdma_qp_enable_prep;
	qpcb->cb.rsp_cmpl = ntrdma_qp_enable_cmpl;
	qpcb->qp = qp;

	ntrdma_dev_cmd_add(dev, &qpcb->cb);
}

void ntrdma_qp_enable(struct ntrdma_qp *qp)
{
	reinit_completion(&qp->enable_qpcb.cb.cmds_done);
	ntrdma_qp_enable_cb(&qp->res, &qp->enable_qpcb.cb);
}

static int ntrdma_qp_enable_prep(struct ntrdma_cmd_cb *cb,
				union ntrdma_cmd *cmd)
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
	cmd->qp_create.send_wqe_inline_cap = qp->send_wqe_inline_cap;
	cmd->qp_create.send_ring_idx = ntrdma_qp_send_cons(qp);
	ntc_export_buf_make_desc(&cmd->qp_create.send_cqe_buf_desc,
				&qp->send_cqe_buf);
	cmd->qp_create.send_cons_shift =
		qp->send_cap * sizeof(struct ntrdma_cqe);
	cmd->qp_create.cmpl_vbell_idx = qp->send_cq->vbell.idx;

	return 0;
}

static int ntrdma_qp_enable_disable_cmpl_common(struct ntrdma_qp *qp,
						const struct ntrdma_dev *dev,
						const union ntrdma_rsp *rsp,
						int is_disable)
{
	int rc;

	if (is_disable) {
		rc = 0;
		goto disable;
	}

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
	ntc_remote_buf_unmap(&qp->peer_send_wqe_buf, dev->ntc);
err_peer_send_wqe_buf:
	ntc_remote_buf_unmap(&qp->peer_recv_wqe_buf, dev->ntc);
err_peer_recv_wqe_buf:
	return rc;
}

static int ntrdma_qp_enable_cmpl(struct ntrdma_cmd_cb *cb,
				const union ntrdma_rsp *_rsp)
{
	struct ntrdma_qp_cmd_cb *qpcb = ntrdma_cmd_cb_qpcb(cb);
	struct ntrdma_qp *qp = qpcb->qp;
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);
	union ntrdma_rsp rsp;
	int rc;

	ntrdma_vdbg(dev, "called\n");

	TRACE("qp_enable cmpl: %d\n", qp->res.key);

	rsp = READ_ONCE(*_rsp);
	if (unlikely(rsp.hdr.status)) {
		rc = -EIO;
		goto out;
	}

	rc = ntrdma_qp_enable_disable_cmpl_common(qp, dev, &rsp, false);
	if (unlikely(rc))
		goto out;

	if (is_state_out_of_reset(atomic_read(&qp->state))) {
		qpcb->cb.cmd_prep = ntrdma_qp_modify_prep;
		qpcb->cb.rsp_cmpl = ntrdma_qp_modify_cmpl;
		ntrdma_dev_cmd_add_unsafe(dev, &qpcb->cb);
		return 0;
	}

	ntrdma_qp_recv_work(qp);
	rc = 0;

 out:
	complete_all(&cb->cmds_done);

	return rc;
}

static void ntrdma_qp_disable_cb(struct ntrdma_res *res,
				struct ntrdma_cmd_cb *cb)
{
	struct ntrdma_dev *dev = ntrdma_res_dev(res);
	struct ntrdma_qp *qp = ntrdma_res_qp(res);
	struct ntrdma_rqp *rqp = NULL;
	struct ntrdma_qp_cmd_cb *qpcb;

	if (qp && dev)
		rqp = ntrdma_dev_rqp_look_and_get(dev, qp->rqp_key);
	TRACE("qp %p (key %d) rqp %p (key %d)\n", qp, qp ? qp->res.key : -1,
			rqp, rqp ? rqp->rres.key : -1);
	ntrdma_qp_send_stall(qp, rqp);
	if (rqp) {
		rqp->qp_key = -1;
		ntrdma_rqp_put(rqp);
	}

	qpcb = container_of(cb, struct ntrdma_qp_cmd_cb, cb);

	if (unlikely(qp->ibqp.qp_type == IB_QPT_GSI))
		ntrdma_info(dev, "deleting QP type IB_QPT_GSI\n");

	qpcb->cb.cmd_prep = ntrdma_qp_disable_prep;
	qpcb->cb.rsp_cmpl = ntrdma_qp_disable_cmpl;
	qpcb->qp = qp;

	ntrdma_dev_cmd_add(dev, &qpcb->cb);
}

static int ntrdma_qp_disable_prep(struct ntrdma_cmd_cb *cb,
				union ntrdma_cmd *cmd)
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
				const union ntrdma_rsp *rsp)
{
	struct ntrdma_qp_cmd_cb *qpcb = ntrdma_cmd_cb_qpcb(cb);
	struct ntrdma_qp *qp = qpcb->qp;
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);

	ntrdma_vdbg(dev, "called\n");

	ntrdma_qp_enable_disable_cmpl_common(qp, dev, NULL, true);

	complete_all(&cb->cmds_done);

	if (READ_ONCE(rsp->hdr.status))
		return -EIO;

	return 0;
}

void ntrdma_qp_reset(struct ntrdma_qp *qp)
{
	struct ntrdma_dev *dev;
	struct ntrdma_rqp *rqp = NULL;
	int start_cmpl, start_cons, end, base;
	bool need_cue = false;
	unsigned long irqflags = 0;

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
		mutex_lock(&qp->send_cmpl_lock);
		/* TODO: warn if qp state < SEND_DRAIN */

		spin_lock_irqsave(&qp->send_post_slock, irqflags); /* Potential deadlock? */
		ntrdma_ring_consume(qp->send_post, qp->send_cmpl,
				qp->send_cap, &start_cmpl, &end, &base);
		ntrdma_ring_consume(qp->send_post, ntrdma_qp_send_cons(qp),
				qp->send_cap, &start_cons, &end, &base);
		ntrdma_info(dev,
				"QP %d, start %d, end %d, base %d: send_abort %d, send_post %d, cons %d, send_cmpl %d, send_prod %d\n",
				qp->res.key, start_cmpl, end, base,
				qp->send_abort, qp->send_post,
				ntrdma_qp_send_cons(qp), qp->send_cmpl,
				qp->send_prod);
		if (start_cons != end) {
			qp->send_aborting = true;
			move_to_err_state(qp);
			need_cue = true;
		} else if (start_cmpl != start_cons) {
			need_cue = true;
		}
		spin_unlock_irqrestore(&qp->send_post_slock, irqflags);
		mutex_unlock(&qp->send_cmpl_lock);

		if (need_cue)
			ntrdma_cq_cue(qp->send_cq);
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
	ntrdma_rqp_deinit(rqp);
	ntrdma_free_rqp(rqp);
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
	u32 send_vbell_idx;

	if (is_deinit)
		goto deinit;

	ntrdma_rres_init(&rqp->rres, dev, &dev->rqp_vec,
			      ntrdma_rqp_free, key);

	rqp->pd_key = attr->pd_key;
	rqp->access = 0;

	rqp->state = 0;
	rqp->qp_key = -1;

	rqp->send_wqe_sg_cap = attr->send_wqe_sg_cap;
	rqp->send_wqe_inline_cap = attr->send_wqe_inline_cap;
	rqp->recv_wqe_sg_cap = attr->recv_wqe_sg_cap;

	rqp->send_wqe_size = ntrdma_send_wqe_size(rqp->send_wqe_sg_cap,
			rqp->send_wqe_inline_cap);
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

	send_vbell_idx = ntrdma_dev_vbell_next(dev);
	if (unlikely(send_vbell_idx >= NTRDMA_DEV_VBELL_COUNT)) {
		ntrdma_err(dev, "invalid send_vbell_idx. idx %d >= %d",
			send_vbell_idx, NTRDMA_DEV_VBELL_COUNT);
		rc = -EINVAL;
		goto err_vbell_idx;
	}

	ntrdma_tasklet_vbell_init(dev, &rqp->send_vbell, send_vbell_idx,
				&rqp->send_work);

	tasklet_init(&rqp->send_work,
		     ntrdma_rqp_work_cb,
		     to_ptrhld(rqp));

	return 0;
deinit:
	tasklet_kill(&rqp->send_work);
err_vbell_idx:
	ntc_export_buf_free(&rqp->recv_wqe_buf);
err_recv_wqe_buf:
	ntc_local_buf_free(&rqp->send_cqe_buf, dev->ntc);
err_send_cqe_buf:
	ntc_export_buf_free(&rqp->send_wqe_buf);
err_send_wqe_buf:
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
	rqp->state = IB_QPS_RESET;

	ntrdma_vbell_del(&rqp->send_vbell);

	tasklet_kill(&rqp->send_work);

	ntrdma_debugfs_rqp_del(rqp);
}

static void ntrdma_rqp_release(struct kref *kref)
{
	struct ntrdma_obj *obj = container_of(kref, struct ntrdma_obj, kref);
	struct ntrdma_rres *rres = container_of(obj, struct ntrdma_rres, obj);
	struct ntrdma_rqp *rqp = container_of(rres, struct ntrdma_rqp, rres);
	struct ntrdma_dev *dev = ntrdma_rqp_dev(rqp);

	ntc_remote_buf_unmap(&rqp->peer_send_cqe_buf, dev->ntc);

	ntrdma_rqp_deinit(rqp);
	ntrdma_free_rqp(rqp);
}

inline void ntrdma_rqp_put(struct ntrdma_rqp *rqp)
{
	ntrdma_rres_put(&rqp->rres, ntrdma_rqp_release);
}

inline const struct ntrdma_recv_wqe *ntrdma_rqp_recv_wqe(struct ntrdma_rqp *rqp,
							u32 pos)
{
	return ntc_export_buf_const_deref(&rqp->recv_wqe_buf,
					pos * rqp->recv_wqe_size,
					rqp->recv_wqe_size);
}

inline const struct ntrdma_send_wqe *ntrdma_rqp_send_wqe(struct ntrdma_rqp *rqp,
							u32 pos)
{
	return ntc_export_buf_const_deref(&rqp->send_wqe_buf,
					pos * rqp->send_wqe_size,
					rqp->send_wqe_size);
}

inline struct ntrdma_cqe *ntrdma_rqp_send_cqe(struct ntrdma_rqp *rqp,
					u32 pos)
{
	return ntc_local_buf_deref(&rqp->send_cqe_buf) +
		pos * sizeof(struct ntrdma_cqe);
}

static inline void ntrdma_send_fail(struct ntrdma_cqe *cqe,
			const struct ntrdma_send_wqe *wqe, int op_status)
{
	cqe->ulp_handle = wqe->ulp_handle;
	cqe->op_code = wqe->op_code;
	cqe->op_status = op_status;
	cqe->rdma_len = 0;
	cqe->imm_data = 0;
	cqe->flags = wqe->flags;
}

static inline void ntrdma_send_done(struct ntrdma_cqe *cqe,
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

int ntrdma_recv_wqe_sync(struct ntrdma_dev *dev, struct ntrdma_recv_wqe *wqe)
{
	int rc, rc1;
	int i;
	struct ntrdma_wr_rcv_sge *rcv_sge;
	struct ntrdma_wr_rcv_sge_shadow *shadow;

	rc = 0;

	for (i = 0; i < wqe->sg_count; ++i) {
		rcv_sge = &wqe->rcv_sg_list[i];

		shadow = rcv_sge->shadow;
		if (!shadow)
			continue;

		if (shadow->local_key == NTRDMA_RESERVED_DMA_LEKY)
			memcpy(shadow->rcv_dma_buf.ptr,
				shadow->exp_buf.ptr, shadow->exp_buf.size);
		else {
			rc1 = ntrdma_zip_memcpy(dev,
						shadow->local_key,
						shadow->local_addr,
						shadow->exp_buf.ptr,
						shadow->exp_buf.size);
			rc = min_t(int, rc, rc1);
		}
	}

	return rc;
}

void ntrdma_recv_wqe_cleanup(struct ntrdma_recv_wqe *wqe)
{
	int i;
	struct ntrdma_wr_rcv_sge *rcv_sge;
	struct ntrdma_wr_rcv_sge_shadow *shadow;

	for (i = 0; i < wqe->sg_count; ++i) {
		rcv_sge = &wqe->rcv_sg_list[i];
		shadow = rcv_sge->shadow;
		if (!shadow)
			continue;
		rcv_sge->shadow = NULL;
		ntc_export_buf_free(&shadow->exp_buf);
		ntrdma_free_sge_shadow(shadow);
	}

	wqe->sg_count = 0;
}

static inline void ntrdma_recv_fail(struct ntrdma_cqe *recv_cqe,
				struct ntrdma_recv_wqe *recv_wqe, int op_status)
{
	recv_cqe->ulp_handle = recv_wqe->ulp_handle;
	recv_cqe->op_code = recv_wqe->op_code;
	recv_cqe->op_status = op_status;
	recv_cqe->rdma_len = 0;
	recv_cqe->imm_data = 0;
	ntrdma_recv_wqe_cleanup(recv_wqe);
}

static inline
u16 ntrdma_send_recv_opcode(const struct ntrdma_send_wqe *send_wqe)
{
	switch (send_wqe->op_code) {
	case NTRDMA_WR_SEND_INV:
		return NTRDMA_WR_RECV_INV;
	case IB_WR_SEND_WITH_IMM:
		return NTRDMA_WR_RECV_IMM;
	case IB_WR_RDMA_WRITE_WITH_IMM:
		return NTRDMA_WR_RECV_RDMA;
	}
	return NTRDMA_WR_RECV;
}

static inline u32 ntrdma_send_recv_len(const struct ntrdma_send_wqe *send_wqe)
{
	switch (send_wqe->op_code) {
	case IB_WR_SEND:
	case NTRDMA_WR_SEND_INV:
	case IB_WR_SEND_WITH_IMM:
		return send_wqe->rdma_sge.length;
	}
	return 0;
}

static inline u32 ntrdma_send_recv_imm(const struct ntrdma_send_wqe *send_wqe)
{
	switch (send_wqe->op_code) {
	case NTRDMA_WR_SEND_INV:
	case IB_WR_SEND_WITH_IMM:
	case IB_WR_RDMA_WRITE_WITH_IMM:
		return send_wqe->imm_data;
	}
	return 0;
}

static inline int ntrdma_recv_done(struct ntrdma_dev *dev,
				struct ntrdma_cqe *recv_cqe,
				struct ntrdma_recv_wqe *recv_wqe,
				const struct ntrdma_send_wqe *send_wqe)
{
	int rc;

	recv_cqe->ulp_handle = recv_wqe->ulp_handle;
	recv_cqe->op_code = ntrdma_send_recv_opcode(send_wqe);
	recv_cqe->op_status = NTRDMA_WC_SUCCESS;
	recv_cqe->rdma_len = ntrdma_send_recv_len(send_wqe);
	recv_cqe->imm_data = ntrdma_send_recv_imm(send_wqe);
	rc = ntrdma_recv_wqe_sync(dev, recv_wqe);
	ntrdma_recv_wqe_cleanup(recv_wqe);

	return rc;
}

static inline void ntrdma_qp_recv_cons_get(struct ntrdma_qp *qp,
					u32 *pos, u32 *end, u32 *base)
{
	ntrdma_ring_consume(qp->recv_prod, qp->recv_cons, qp->recv_cap,
			    pos, end, base);
}

static inline void ntrdma_qp_recv_cons_put(struct ntrdma_qp *qp,
					u32 pos, u32 base)
{
	wmb(); /* write recv completions before index */
	qp->recv_cons = ntrdma_ring_update(pos, base, qp->recv_cap);
}

static inline void ntrdma_qp_recv_cmpl_get(struct ntrdma_qp *qp,
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

static inline void ntrdma_qp_recv_cmpl_put(struct ntrdma_qp *qp,
					u32 pos, u32 base)
{
	qp->recv_cmpl = ntrdma_ring_update(pos, base, qp->recv_cap);
}

static inline void ntrdma_qp_send_prod_get(struct ntrdma_qp *qp,
					u32 *pos, u32 *end, u32 *base)
{
	ntrdma_ring_consume(qp->send_post, qp->send_prod, qp->send_cap,
			    pos, end, base);
}

static inline void ntrdma_qp_send_prod_put(struct ntrdma_qp *qp,
					u32 pos, u32 base)
{
	qp->send_prod = ntrdma_ring_update(pos, base, qp->send_cap);
}

static void ntrdma_qp_send_cmpl_get(struct ntrdma_qp *qp,
				    u32 *pos, u32 *end, u32 *base)
{
	u32 send_cons;

	/* during abort, short circuit prod and cons: abort to post idx */
	if (qp->send_abort) {
		TRACE("qp %d (already in abort)\n", qp->res.key);
		send_cons = qp->send_post;
		qp->send_prod = qp->send_post;
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

static inline void ntrdma_qp_send_cmpl_put(struct ntrdma_qp *qp,
				    u32 pos, u32 base)
{
	qp->send_cmpl = ntrdma_ring_update(pos, base, qp->send_cap);
}

static inline void ntrdma_rqp_recv_cons_get(struct ntrdma_rqp *rqp,
					u32 *pos, u32 *end, u32 *base)
{
	u32 recv_prod = ntrdma_rqp_recv_prod(rqp);

	ntrdma_ring_consume(recv_prod, rqp->recv_cons, rqp->recv_cap,
			    pos, end, base);
}

static inline void ntrdma_rqp_recv_cons_put(struct ntrdma_rqp *rqp,
					u32 pos, u32 base)
{
	rqp->recv_cons = ntrdma_ring_update(pos, base, rqp->recv_cap);
}

static inline void ntrdma_rqp_send_cons_get(struct ntrdma_rqp *rqp,
					u32 *pos, u32 *end, u32 *base)
{
	u32 send_prod = ntrdma_rqp_send_prod(rqp);

	ntrdma_ring_consume(send_prod, rqp->send_cons, rqp->send_cap,
			    pos, end, base);
}

static inline void ntrdma_rqp_send_cons_put(struct ntrdma_rqp *rqp,
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

	mutex_lock(&qp->recv_cmpl_lock);
	/* TODO: warn if qp state < INIT */

	ntrdma_qp_recv_cmpl_get(qp, &pos, &end, &base);

	if (pos == end) {
		/* update once qp->recv_cons */
		if (qp->recv_abort_first)
			ntrdma_qp_recv_cmpl_get(qp, &pos, &end, &base);
		if (pos == end) {
			mutex_unlock(&qp->recv_cmpl_lock);
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
	mutex_unlock(&qp->recv_cmpl_lock);
}

static void ntrdma_qp_poll_recv_cqe(struct ntrdma_poll *poll,
				struct ntrdma_cqe *outcqe, u32 pos)
{
	struct ntrdma_qp *qp = ntrdma_recv_poll_qp(poll);
	const struct ntrdma_cqe *cqe = ntrdma_qp_recv_cqe(qp, pos);
	struct ntrdma_recv_wqe *wqe = ntrdma_qp_recv_wqe(qp, pos);
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);
	int opt;

	if (qp->recv_abort) {
		if (qp->recv_abort_first)
			ntrdma_info(dev, "fail completion QP %d", qp->res.key);
		opt = qp->recv_abort_first ? NTRDMA_WC_ERR_LOC_PORT :
				NTRDMA_WC_ERR_ABORTED;
		ntrdma_recv_fail(outcqe, wqe, opt);
		qp->recv_abort_first = false;
		return;
	}
	if (wqe->op_status) {
		qp->recv_aborting = true;
		ntrdma_recv_fail(outcqe, wqe, wqe->op_status);
		TRACE("qp %d wqe status %d\n", qp->res.key, wqe->op_status);
		return;
	}
	*outcqe = READ_ONCE(*cqe);
	if (outcqe->op_status) {
		qp->recv_aborting = true;
		TRACE("qp %d cqe status %d\n", qp->res.key, outcqe->op_status);
	}
}

static int ntrdma_qp_poll_send_start_and_get(struct ntrdma_poll *poll,
					     struct ntrdma_qp **poll_qp, u32 *poll_pos,
					     u32 *poll_end, u32 *poll_base)
{
	int rc = 0;
	struct ntrdma_qp *qp = ntrdma_send_poll_qp(poll);
	u32 pos, end, base;

	mutex_lock(&qp->send_cmpl_lock);
	/* TODO: warn if qp state < SEND_DRAIN */

	ntrdma_qp_send_cmpl_get(qp, &pos, &end, &base);

	if (pos == end) {
		/* In this cae we update once the qp->send_cons_buf */
		if (qp->send_abort_first)
			ntrdma_qp_send_cmpl_get(qp, &pos, &end, &base);
		if (pos == end) {
			mutex_unlock(&qp->send_cmpl_lock);
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
	mutex_unlock(&qp->send_cmpl_lock);
}

static void ntrdma_qp_poll_send_cqe(struct ntrdma_poll *poll,
				struct ntrdma_cqe *outcqe, u32 pos)
{
	struct ntrdma_qp *qp = ntrdma_send_poll_qp(poll);
	const struct ntrdma_cqe *cqe = ntrdma_qp_send_cqe(qp, pos);
	const struct ntrdma_send_wqe *wqe = ntrdma_qp_send_wqe(qp, pos);
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);
	int opt;

	if (qp->send_abort) {
		if (qp->send_abort_first)
			ntrdma_info(dev, "fail completion QP %d", qp->res.key);
		opt = qp->send_abort_first ? NTRDMA_WC_ERR_LOC_PORT :
				NTRDMA_WC_ERR_ABORTED;
		ntrdma_send_fail(outcqe, wqe, opt);
		qp->send_abort_first = false;
		return;
	}

	if (wqe->op_status) {
		/* TODO: should not happen if we are here and not in aborting
		 * already
		 */
		qp->send_aborting = true;
		ntrdma_send_fail(outcqe, wqe, wqe->op_status);
		TRACE("qp %d wqe->op_status %d, move to abort\n", qp->res.key,
				wqe->op_status);
		return;
	}

	*outcqe = READ_ONCE(*cqe);
	if (outcqe->op_status) {
		qp->send_aborting = true;
		TRACE("qp %d, cqe->op_status %d, move to abort\n",
			qp->res.key, outcqe->op_status);
	}
}

void ntrdma_qp_recv_work(struct ntrdma_qp *qp)
{
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);
	u32 start, end, base;
	size_t off, len;
	int rc = 0;

	/* verify the qp state and lock for producing recvs */
	spin_lock_bh(&qp->recv_prod_lock);
	if (!is_state_recv_ready(atomic_read(&qp->state))) {
		ntrdma_qp_info(qp, "qp %d state %d will retry", qp->res.key,
			atomic_read(&qp->state));

		if (qp->ibqp.qp_type != IB_QPT_GSI)
			qp->recv_aborting = true;

		goto unlock;
	}

	/* get the next producing range in the ring */
	ntrdma_qp_recv_prod_get(qp, &start, &end, &base);

	if (start == end)
		goto out;

	for (;;) {
		ntrdma_qp_recv_prod_put(qp, end, base);

		/* send the portion of the ring */
		off = start * qp->recv_wqe_size;
		len = (end - start) * qp->recv_wqe_size;
		rc = ntc_request_memcpy_fenced(qp->dma_chan,
					&qp->peer_recv_wqe_buf, off,
					&qp->recv_wqe_buf, off,
					len);
		if (unlikely(rc < 0)) {
			ntrdma_err(dev, "ntc_request_memcpy failed. rc=%d", rc);
			goto out;
		}

		TRACE("QP %d start %u end %u\n",
				qp->res.key, start, end);

		ntrdma_qp_recv_prod_get(qp, &start, &end, &base);
		if (start == end)
			break;
	}

	/* send the prod idx */
	rc = ntc_request_imm32(qp->dma_chan,
			&qp->peer_recv_wqe_buf, qp->peer_recv_prod_shift,
			qp->recv_prod, true, NULL, NULL);
	if (unlikely(rc < 0)) {
		ntrdma_err(dev, "ntc_request_imm32 failed. rc=%d", rc);
		goto out;
	}

	/* submit the request */
	ntc_req_submit(qp->dma_chan);

out:
	if (unlikely(rc < 0))
		ntrdma_unrecoverable_err(dev);

unlock:
	/* release lock for state change or producing later recvs */
	spin_unlock_bh(&qp->recv_prod_lock);
}

static inline bool check_recv_wqe_sanity(struct ntrdma_rqp *rqp,
					const struct ntrdma_recv_wqe *recv_wqe)
{
	return sizeof(*recv_wqe) +
		recv_wqe->sg_count * (u64)sizeof(recv_wqe->rcv_sg_list[0]) <=
		rqp->recv_wqe_size;
}

inline int ntrdma_qp_rdma_write(struct ntrdma_qp *qp,
				struct ntrdma_send_wqe *wqe)
{
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);
	struct ntrdma_wr_rcv_sge rdma_sge;
	u32 rdma_len;
	int rc;

	if (unlikely(ntrdma_ib_sge_reserved(&wqe->rdma_sge)))
		return -EINVAL;

	rdma_sge.shadow = NULL;
	rdma_sge.sge = wqe->rdma_sge;
	rdma_sge.sge.length = ~(u32)0;

	if (wqe->flags & IB_SEND_INLINE) {
		rdma_len = wqe->inline_len;
		rc = ntrdma_zip_rdma_imm(dev, qp->dma_chan, &rdma_sge,
					wqe + 1, 1, wqe->inline_len, 0);
	} else
		rc = ntrdma_zip_rdma(dev, qp->dma_chan, &rdma_len, &rdma_sge,
				const_snd_sg_list(0, wqe), 1, wqe->sg_count, 0);

	if (likely(rc >= 0))
		wqe->rdma_sge.length = rdma_len;

	return rc;
}

bool ntrdma_qp_send_work(struct ntrdma_qp *qp)
{
	DEFINE_NTC_FUNC_PERF_TRACKER(perf, 1 << 15);
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);
	struct ntrdma_rqp *rqp;
	struct ntrdma_send_wqe *wqe;
	const struct ntrdma_recv_wqe *_recv_wqe = NULL;
	struct ntrdma_recv_wqe recv_wqe;
	u32 start, pos, end, base;
	u32 recv_pos, recv_end, recv_base;
	u32 rcv_start_offset;
	u32 rdma_len;
	size_t off, len;
	int rc = 0;
	bool abort = false;
	bool reschedule = false;

	/* verify the qp state and lock for producing sends */
	spin_lock_bh(&qp->send_prod_lock);

	/* get the next producing range in the send ring */
	ntrdma_qp_send_prod_get(qp, &start, &end, &base);
	if (qp->ibqp.qp_type == IB_QPT_GSI)
		ntrdma_qp_info(qp, "qp %d, start %d, end %d, base %d",
				qp->res.key, start, end, base);
	/* quit if there is no send work to do */
	if (start == end)
		goto done;

	/* limit the range to batch size */
	/* end = min_t(u32, end, start + NTRDMA_QP_BATCH_SIZE); */

	/* On GSI qp we do not change qp state in reset, since it cannot move
	 * to SQE state without error completion, we move it to aborting
	 * and on first send (here) we will provide the error completion
	 * and move to error state
	 */
	if (qp->send_abort || qp->send_aborting) {
		ntrdma_qp_err(qp, "qp %d in abort process", qp->res.key);
		goto err_rqp;
	}
	/* sending requires a connected rqp */
	rqp = ntrdma_dev_rqp_look_and_get(dev, qp->rqp_key);
	if (!rqp) {
		ntrdma_qp_err(qp, "ntrdma_dev_rqp_look failed key %d",
			qp->rqp_key);
		goto err_rqp;
	}


	/* connected rqp must be ready to receive */
	spin_lock_bh(&rqp->recv_cons_lock);
	if (!is_state_recv_ready(rqp->state)) {
		ntrdma_rqp_err(rqp, "rqp %d state %d",
			rqp->rres.key, rqp->state);
		spin_unlock_bh(&rqp->recv_cons_lock);
		goto err_recv;
	}

	/* get the next consuming range in the recv ring */
	ntrdma_rqp_recv_cons_get(rqp, &recv_pos, &recv_end, &recv_base);

	for (pos = start;;) {
		wqe = ntrdma_qp_send_wqe(qp, pos++);

		if (ntrdma_wr_code_is_send(wqe->op_code)) {

			wqe->recv_key = recv_pos + recv_base;

			if (recv_pos == recv_end) {
				ntrdma_qp_info_ratelimited(qp,
						"send but no recv QP %d, pos %u end %u base %u prod %u cons %u\n",
						qp->res.key,
						recv_pos, recv_end, recv_base,
						ntrdma_rqp_recv_prod(rqp),
						rqp->recv_cons);

#ifdef CONFIG_NTRDMA_RETRY_RECV
				--pos;
				reschedule = true;
#else
				if (!wqe->op_status)
					wqe->op_status = NTRDMA_WC_ERR_RECV_MISSING;

				abort = true;
#endif
				break;
			}

			_recv_wqe = ntrdma_rqp_recv_wqe(rqp, recv_pos++);
			recv_wqe = READ_ONCE(*_recv_wqe);

			if (recv_wqe.op_status) {
				if (!wqe->op_status) {
					ntrdma_qp_err(qp,
							"wqe->op_status %d recv_wqe.op_status %d recv_pos %u qp %d\n",
							wqe->op_status,
							recv_wqe.op_status,
							recv_pos,
							qp->res.key);
					wqe->op_status = recv_wqe.op_status;
				}

				abort = true;
				break;
			}

			if (recv_pos == recv_end) {
				ntrdma_rqp_recv_cons_put(rqp, recv_pos, recv_base);
				ntrdma_rqp_recv_cons_get(rqp, &recv_pos, &recv_end, &recv_base);
			}
		}

		if (wqe->op_status) {
			ntrdma_qp_err(qp, "op status %d qp %d",
				wqe->op_status, qp->res.key);

			abort = true;
			break;
		}

		if (ntrdma_wr_code_push_data(wqe->op_code)) {
			if (ntrdma_wr_code_is_rdma(wqe->op_code))
				rc = ntrdma_qp_rdma_write(qp, wqe);
			else if (check_recv_wqe_sanity(rqp, &recv_wqe)) {

				if (qp->ibqp.qp_type == IB_QPT_GSI)
					rcv_start_offset =
						sizeof(struct ib_grh);
				else
					rcv_start_offset = 0;

				/* This goes from send to post recv */
				rc = ntrdma_zip_rdma(dev, qp->dma_chan,
						&rdma_len,
						_recv_wqe->rcv_sg_list,
						const_snd_sg_list(0, wqe),
						recv_wqe.sg_count,
						wqe->sg_count,
						rcv_start_offset);
				if (rc >= 0)
					wqe->rdma_sge.length = rdma_len;
			} else
				rc = -EINVAL;
			if (rc) {
				ntrdma_qp_err(qp,
					"ntrdma_zip_rdma failed %d qp %d",
					rc, qp->res.key);

				wqe->op_status = NTRDMA_WC_ERR_RDMA_RANGE;
				abort = true;
				break;
			}
		}

		if (pos == end) {
			reschedule = true;
			break;
		}
	}
	ntrdma_rqp_recv_cons_put(rqp, recv_pos, recv_base);
	spin_unlock_bh(&rqp->recv_cons_lock);
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
	rc = ntc_request_memcpy_fenced(qp->dma_chan,
				&qp->peer_send_wqe_buf, off,
				&qp->send_wqe_buf, off, len);
	if (unlikely(rc < 0)) {
		ntrdma_qp_err(qp, "ntc_request_memcpy failed. rc=%d", rc);
		abort = true;
		goto err_memcpy;
	}

	this_cpu_add(dev_cnt.qp_send_work_bytes, len);

	/* send the prod idx */
	rc = ntc_request_imm32(qp->dma_chan,
			&qp->peer_send_wqe_buf, qp->peer_send_prod_shift,
			qp->send_prod, true, NULL, NULL);
	if (unlikely(rc < 0)) {
		ntrdma_qp_err(qp, "ntc_request_imm32 failed. rc=%d", rc);
		abort = true;
		goto err_memcpy;
	}
	/* update the vbell and signal the peer */
	/* TODO: return value is ignored! */
	rc = ntrdma_dev_vbell_peer(dev, qp->dma_chan, qp->peer_send_vbell_idx);
	if (unlikely(rc < 0)) {
		ntrdma_err(dev, "ntrdma_dev_vbell_peer failed. rc=%d", rc);
		goto err_memcpy;
	}

	rc = ntc_req_signal(dev->ntc, qp->dma_chan, NULL, NULL,
			NTB_DEFAULT_VEC(dev->ntc));
	if (unlikely(rc < 0)) {
		ntrdma_err(dev, "ntc_req_signal failed. rc=%d", rc);
		goto err_memcpy;
	}

	TRACE_DATA(
		"start %u pos %u QP %d RQP %d prod %u peer vbell idx %d (recv_pos %d, recv_base %d)\n",
		start, pos, qp->res.key, qp->rqp_key, qp->send_prod,
		qp->peer_send_vbell_idx, recv_pos, recv_base);

	/* release lock for state change or producing later sends */
done:
	spin_unlock_bh(&qp->send_prod_lock);
	goto out;
err_memcpy:
	spin_unlock_bh(&qp->send_prod_lock);
	ntrdma_qp_err(qp, "err_memcpy - rc = %d on qp %p", rc, qp);
	ntrdma_unrecoverable_err(dev);
	goto out;

err_recv:
	ntrdma_rqp_put(rqp);
err_rqp:
	if (qp->ibqp.qp_type == IB_QPT_GSI) {
		atomic_set(&qp->state, IB_QPS_SQE);
		qp->send_aborting = true;
		qp->send_abort = false;
		qp->send_abort_first = false;
		/* We know no other ntrdma_post_send is running */
		ntrdma_qp_send_prod_put(qp, end, base);
		spin_unlock_bh(&qp->send_prod_lock);
		ntrdma_cq_cue(qp->send_cq);
	} else
		spin_unlock_bh(&qp->send_prod_lock);

	ntrdma_qp_err(qp, "err_rqp QP %d aborting = %d qp %p, cq %p end %d\n",
		qp->send_aborting, qp->res.key, qp, qp->send_cq, end);

 out:
	NTRDMA_PERF_MEASURE(perf);

	return reschedule;
}

static void ntrdma_rqp_send_work(struct ntrdma_rqp *rqp)
{
	struct ntrdma_dev *dev = ntrdma_rqp_dev(rqp);
	struct ntrdma_qp *qp;
	struct ntrdma_cqe *cqe, *recv_cqe = NULL;
	const struct ntrdma_send_wqe *_wqe;
	struct ntrdma_send_wqe wqe;
	struct ntrdma_recv_wqe *recv_wqe = NULL;
	struct ntrdma_wr_rcv_sge rdma_sge;
	u32 start, pos, end, base;
	u32 recv_pos, recv_end, recv_base;
	size_t off, len;
	bool cue_recv = false;
	int rc = 0;
	bool do_signal = false;
	bool abort = false;

	/* verify the rqp state and lock for consuming sends */
	spin_lock_bh(&rqp->send_cons_lock);
	if (!is_state_send_ready(rqp->state)) {
		ntrdma_rqp_info(rqp, "rqp %d state %d will retry",
			rqp->rres.key, rqp->state);
		spin_unlock_bh(&rqp->send_cons_lock);
		return;
	}

	ntrdma_vbell_clear(&rqp->send_vbell);

	/* get the next consuming range in the send ring */
	ntrdma_rqp_send_cons_get(rqp, &start, &end, &base);

	/* quit if there is no send work to do */
	if (start == end) {
		rc = ntrdma_vbell_add(&rqp->send_vbell);
		if (rc == -EAGAIN)
			tasklet_schedule(&rqp->send_work);
		spin_unlock_bh(&rqp->send_cons_lock);
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
	rc = 0;
	spin_lock_bh(&qp->recv_cons_lock);
	if (!is_state_recv_ready(atomic_read(&qp->state))) {
		ntrdma_qp_err(qp, "qp %d state %d\n", qp->res.key,
				atomic_read(&qp->state));
		spin_unlock_bh(&qp->recv_cons_lock);
		rc = -EINVAL;
		goto err_recv;
	}
	/* FIXME: need to complete the send with error??? */

	/* get the next consuming range in the recv ring */
	ntrdma_qp_recv_cons_get(qp, &recv_pos, &recv_end, &recv_base);

	for (pos = start;;) {
		cqe = ntrdma_rqp_send_cqe(rqp, pos);
		_wqe = ntrdma_rqp_send_wqe(rqp, pos++);
		wqe = READ_ONCE(*_wqe);

		if (wqe.flags & IB_SEND_SIGNALED)
			do_signal = true;

		if (ntrdma_wr_code_is_send(wqe.op_code)) {
			if (ntrdma_wr_status_no_recv(wqe.op_status)) {
				ntrdma_send_fail(cqe, &wqe, wqe.op_status);
				ntrdma_err(dev, "WQE with error %d received on  qp %d\n",
					wqe.op_status, rqp->qp_key);
				abort = true;
				break;
			}

			if (WARN_ON(wqe.recv_key != recv_pos + recv_base)) {
				if (!wqe.op_status)
					ntrdma_send_fail(cqe, &wqe, NTRDMA_WC_ERR_CORRUPT);
				else
					ntrdma_send_fail(cqe, &wqe, wqe.op_status);

				abort = true;
				ntrdma_err(dev,
						"qp %d, recv_key %d, recv_pos %d, recv_base %d, wqe_op_status %d move to error state\n",
						rqp->qp_key, wqe.recv_key,
						recv_pos, recv_base,
						wqe.op_status);
				break;
			}

			if (recv_pos == recv_end) {
				if (WARN_ON(!wqe.op_status))
					ntrdma_send_fail(cqe, &wqe, NTRDMA_WC_ERR_RECV_MISSING);
				else
					ntrdma_send_fail(cqe, &wqe, wqe.op_status);

				abort = true;
				ntrdma_err(dev,
						"qp %d, recv_pos = recv_end = %d, wqe_op_status %d move to error state\n",
						rqp->qp_key, recv_pos,
						wqe.op_status);
				break;
			}

			cue_recv = true;
			recv_cqe = ntrdma_qp_recv_cqe(qp, recv_pos);
			recv_wqe = ntrdma_qp_recv_wqe(qp, recv_pos++);

			if (recv_wqe->op_status) {
				if (WARN_ON(!wqe.op_status))
					ntrdma_send_fail(cqe, &wqe, recv_wqe->op_status);
				else
					ntrdma_send_fail(cqe, &wqe, wqe.op_status);

				ntrdma_recv_fail(recv_cqe, recv_wqe, recv_wqe->op_status);

				abort = true;
				ntrdma_err(dev, "qp %d, recv_wqe_op_status %d\n",
						rqp->qp_key,
						recv_wqe->op_status);
				break;
			}
		} else {
			recv_wqe = NULL;
			recv_cqe = NULL;
		}

		if (wqe.op_status) {
			ntrdma_send_fail(cqe, &wqe, wqe.op_status);

			if (ntrdma_wr_code_is_send(wqe.op_code))
				ntrdma_recv_fail(recv_cqe, recv_wqe, wqe.op_status);

			abort = true;
			ntrdma_err(dev, "Error wqe op status %d  pos %u QP %d\n",
					wqe.op_status, pos, qp->res.key);
			break;
		}

		if (ntrdma_wr_code_push_data(wqe.op_code)) {
			if (ntrdma_wr_code_is_rdma(wqe.op_code)) {
				if (!ntrdma_ib_sge_reserved(&wqe.rdma_sge)) {
					rdma_sge.shadow = NULL;
					rdma_sge.sge = wqe.rdma_sge;
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

			if (ntrdma_wr_code_is_send(wqe.op_code)) {
				rc = ntrdma_recv_done(dev,
						recv_cqe, recv_wqe, &wqe);
				WARN(rc, "ntrdma_recv_done failed rc = %d", rc);
				/* FIXME: handle recv sync error */
			}

			ntrdma_send_done(cqe, &wqe, wqe.rdma_sge.length);
		}

		if (ntrdma_wr_code_pull_data(wqe.op_code)) {
			/* We do not support RDMA read. */
			rc = -EINVAL;
			ntrdma_send_fail(cqe, &wqe, NTRDMA_WC_ERR_RDMA_ACCESS);
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

	spin_unlock_bh(&qp->recv_cons_lock);

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
	rc = ntc_request_memcpy_fenced(qp->dma_chan,
				&rqp->peer_send_cqe_buf, off,
				&rqp->send_cqe_buf, off, len);
	if (unlikely(rc < 0)) {
		ntrdma_err(dev, "ntc_request_memcpy failed. rc=%d", rc);
		goto err_memcpy;
	}

	this_cpu_add(dev_cnt.tx_cqes, pos - start);
	/* send the cons idx */
	rc = ntc_request_imm32(qp->dma_chan,
			&rqp->peer_send_cqe_buf, rqp->peer_send_cons_shift,
			rqp->send_cons, true, NULL, NULL);
	if (unlikely(rc < 0)) {
		ntrdma_err(dev, "ntc_request_imm32 failed. rc=%d", rc);
		goto err_memcpy;
	}

	if (do_signal) {
		/* update the vbell and signal the peer */
		rc = ntrdma_dev_vbell_peer(dev, qp->dma_chan,
					rqp->peer_cmpl_vbell_idx);
		if (unlikely(rc < 0)) {
			ntrdma_err(dev, "ntrdma_dev_vbell_peer failed. rc=%d",
				rc);
			goto err_memcpy;
		}

		rc = ntc_req_signal(dev->ntc, qp->dma_chan, NULL, NULL,
				NTB_DEFAULT_VEC(dev->ntc));
		if (unlikely(rc < 0)) {
			ntrdma_err(dev, "ntc_req_signal failed. rc=%d", rc);
			goto err_memcpy;
		}

		TRACE_DATA(
				"Signal QP %d RQP %d cons %u start %u pos %u peer vbell idx %d\n",
				qp->res.key, qp->rqp_key,
				rqp->send_cons,
				start,
				pos,
				rqp->peer_cmpl_vbell_idx);
	}
	/* submit the request */
	/* TODO: return cpde? */
	ntc_req_submit(qp->dma_chan);

	/* release lock for state change or consuming later sends */
	spin_unlock_bh(&rqp->send_cons_lock);
	return;

err_recv:
	ntrdma_qp_put(qp);
err_qp:
	spin_unlock_bh(&rqp->send_cons_lock);
	ntrdma_err(dev, "%s Failed qp key %d\n",
			__func__, rqp->qp_key);
	return;
err_memcpy:
	spin_unlock_bh(&rqp->send_cons_lock);
	ntrdma_err(dev, "%s Failed qp key %d\n",
			__func__, rqp->qp_key);
	ntrdma_unrecoverable_err(dev);
}

static void ntrdma_rqp_work_cb(unsigned long ptrhld)
{
	struct ntrdma_rqp *rqp = of_ptrhld(ptrhld);

	ntrdma_rqp_send_work(rqp);
}

struct ntrdma_qp *ntrdma_dev_qp_look_and_get(struct ntrdma_dev *dev, u32 key)
{
	struct ntrdma_res *res;

	res = ntrdma_res_look(&dev->qp_vec, key);
	if (!res)
		return NULL;

	return ntrdma_res_qp(res);
}

struct ntrdma_rqp *ntrdma_dev_rqp_look_and_get(struct ntrdma_dev *dev, u32 key)
{
	struct ntrdma_rres *rres;

	rres = ntrdma_rres_look(&dev->rqp_vec, key);
	if (!rres)
		return NULL;

	return ntrdma_rres_rqp(rres);
}

void ntrdma_qp_send_stall(struct ntrdma_qp *qp, struct ntrdma_rqp *rqp)
{
	if (!qp && !rqp)
		return;
	if (qp) {
		TRACE("qp %p (res key %d)\n", qp, qp->res.key);

		spin_lock_bh(&qp->send_prod_lock);

		if (!ntrdma_qp_is_send_ready(qp))
			ntrdma_qp_info(qp, "qp %d state %d",
				qp->res.key, atomic_read(&qp->state));
		else {
			move_to_err_state(qp);
			qp->send_aborting = true;
			qp->recv_aborting = true;
			TRACE("qp %d - aborting\n", qp->res.key);
		}

		spin_unlock_bh(&qp->send_prod_lock);
	}
	if (!rqp)
		return;
	TRACE("rqp %p (rres key %d)\n", rqp, rqp->rres.key);

	/* Just to sync */
	spin_lock_bh(&rqp->send_cons_lock);
	spin_unlock_bh(&rqp->send_cons_lock);
}

struct ntrdma_wr_rcv_sge_shadow *
ntrdma_zalloc_sge_shadow(gfp_t gfp, struct ntrdma_dev *dev)
{
	return kmem_cache_alloc_node(shadow_slab, gfp | __GFP_ZERO, dev->node);
}

void ntrdma_free_sge_shadow(struct ntrdma_wr_rcv_sge_shadow *shadow)
{
	kmem_cache_free(shadow_slab, shadow);
}

struct ntrdma_rqp *ntrdma_alloc_rqp(gfp_t gfp, struct ntrdma_dev *dev)
{
	return kmem_cache_alloc_node(rqp_slab, gfp, dev->node);
}

void ntrdma_free_rqp(struct ntrdma_rqp *rqp)
{
	kmem_cache_free(rqp_slab, rqp);
}


int __init ntrdma_qp_module_init(void)
{
	if (!((rqp_slab = KMEM_CACHE(ntrdma_rqp, 0)) &&
			(shadow_slab = KMEM_CACHE(ntrdma_wr_rcv_sge_shadow,
						0)))) {
		ntrdma_qp_module_deinit();
		return -ENOMEM;
	}

	return 0;
}

void ntrdma_qp_module_deinit(void)
{
	ntrdma_deinit_slab(&rqp_slab);
	ntrdma_deinit_slab(&shadow_slab);
}
