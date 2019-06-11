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

#include <linux/module.h>

#include <linux/in6.h>
#include <linux/slab.h>

#include <rdma/ib_user_verbs.h>

#include "ntrdma_dev.h"
#include "ntrdma_cmd.h"
#include "ntrdma_cq.h"
#include "ntrdma_mr.h"
#include "ntrdma_pd.h"
#include "ntrdma_qp.h"
#include "ntrdma_wr.h"

/* not implemented / not required? */
static int ntrdma_get_port_immutable(struct ib_device *ibdev,
				     u8 port,
				     struct ib_port_immutable *imm)
{
	struct ntrdma_dev *dev = ntrdma_ib_dev(ibdev);

	ntrdma_dbg(dev, "get immutable %u\n", port);

	/* These values do not appear to have any impact on the prototype, but
	 * specific values and actual capabilities may be a requirement for
	 * integration with other technologies enabled by the ofed framework.
	 */
	imm->pkey_tbl_len = 0;
	imm->gid_tbl_len = 0;
	imm->core_cap_flags = 0;
	imm->max_mad_size = 0;

	return 0;
}

/* not implemented / not required? */
static int ntrdma_query_pkey(struct ib_device *ibdev,
			     u8 port_num,
			     u16 index,
			     u16 *pkey)
{
	pr_debug("not implemented, returning %d\n", -ENOSYS);
	return -ENOSYS;
}

/* not implemented / not required? */
static int ntrdma_query_gid(struct ib_device *ibdev,
			    u8 port_num,
			    int index,
			    union ib_gid *ibgid)
{
	struct ntrdma_dev *dev = ntrdma_ib_dev(ibdev);
	struct in6_addr *addr = (void *)ibgid->raw;

	ntrdma_dbg(dev, "query gid port %u idx %d\n", port_num, index);
	ntrdma_dbg(dev, "cap eth ah? %d\n",
		   rdma_cap_eth_ah(ibdev, port_num));
	/*ntrdma_dbg(dev, "imm core cap flags %#x",
		   ibdev->port_immutable[port_num].core_cap_flags);*/

	/* Everything is "link local" since we don't have an interface */
	addr->s6_addr32[0] = htonl(0xfe800000);
	addr->s6_addr32[1] = 0;
	addr->s6_addr32[2] = 0;
	addr->s6_addr32[3] = 0;

	return 0;
}

/* not implemented / not required? */
static int ntrdma_create_ah(struct ib_ah *ibah,
				      struct rdma_ah_attr *ah_attr,
				      u32 flags,
				      struct ib_udata *udata)
{
	pr_debug("not implemented, returning %d\n", -ENOSYS);
	return -ENOSYS;
}

/* not implemented / not required? */
static void ntrdma_destroy_ah(struct ib_ah *ibah, u32 flags)
{
	pr_debug("not implemented, returning %d\n", -ENOSYS);
}

/* not implemented / not required? */
static struct ib_mr *ntrdma_get_dma_mr(struct ib_pd *ibpd,
				       int mr_access_flags)
{
	pr_debug("not implemented, returning %d\n", -ENOSYS);
	return ERR_PTR(-ENOSYS);
}

static int ntrdma_query_device(struct ib_device *ibdev,
			       struct ib_device_attr *ibattr,
			       struct ib_udata *ibudata)
{
	/* TODO: These values do not appear to have any impact on the prototype,
	 * but specific values and actual capabilities may be a requirement for
	 * integration with other technologies enabled by the ofed framework.
	 */
	memset(ibattr, 0, sizeof(*ibattr));
	ibattr->max_pd			= 0x100;
	ibattr->max_mr			= 0x100;
	ibattr->max_mr_size		= 0x80000000;
	ibattr->max_qp			= 0x100;
	ibattr->max_qp_wr		= 0x100;
	ibattr->max_cq			= 0x100;
	ibattr->max_cqe			= 0x100;
	ibattr->max_recv_sge		= 0x100;
	ibattr->max_sge_rd		= 0x100;
	ibattr->page_size_cap		= PAGE_SIZE;

	return 0;
}

static int ntrdma_query_port(struct ib_device *ibdev,
			     u8 port, struct ib_port_attr *ibattr)
{
	/* TODO: These values do not appear to have any impact on the prototype,
	 * but specific values and actual capabilities may be a requirement for
	 * integration with other technologies enabled by the ofed framework.
	 */
	memset(ibattr, 0, sizeof(*ibattr));
	ibattr->state			= IB_PORT_ACTIVE;
	ibattr->max_mtu			= IB_MTU_256;
	ibattr->active_mtu		= IB_MTU_256;

	return 0;
}

static struct ib_cq *ntrdma_create_cq(struct ib_device *ibdev,
				      const struct ib_cq_init_attr *ibattr,
				      struct ib_udata *ibudata)
{
	struct ntrdma_dev *dev = ntrdma_ib_dev(ibdev);
	struct ntrdma_cq *cq;
	u32 vbell_idx;
	int rc;

	cq = kmalloc_node(sizeof(*cq), GFP_KERNEL, dev->node);
	if (!cq) {
		rc = -ENOMEM;
		goto err_cq;
	}

	if (ibattr->comp_vector)
		vbell_idx = ibattr->comp_vector;
	else
		vbell_idx = ntrdma_dev_vbell_next(dev);

	rc = ntrdma_cq_init(cq, dev, vbell_idx);
	if (rc)
		goto err_init;

	rc = ntrdma_cq_add(cq);
	if (rc)
		goto err_add;

	ntrdma_dbg(dev, "added cq %p\n", cq);

	return &cq->ibcq;

	// ntrdma_cq_del(cq);
err_add:
	ntrdma_cq_deinit(cq);
err_init:
	kfree(cq);
err_cq:
	ntrdma_dbg(dev, "failed, returning err %d\n", rc);
	return ERR_PTR(rc);
}

static int ntrdma_destroy_cq(struct ib_cq *ibcq,
				struct ib_udata *ibudata)
{
	struct ntrdma_cq *cq = ntrdma_ib_cq(ibcq);

	/* TODO: what should be done about oustanding work completions? */

	ntrdma_cq_del(cq);
	ntrdma_cq_repo(cq);
	ntrdma_cq_deinit(cq);
	kfree(cq);

	return 0;
}

static inline int ntrdma_ib_wc_status_from_cqe(u32 op_status)
{
	switch (op_status) {
	case NTRDMA_WC_SUCCESS:
		return IB_WC_SUCCESS;
	case NTRDMA_WC_ERR_ABORTED:
		return IB_WC_WR_FLUSH_ERR;
	case NTRDMA_WC_ERR_CORRUPT:
		return IB_WC_BAD_RESP_ERR;
	case NTRDMA_WC_ERR_OPCODE:
		return IB_WC_REM_OP_ERR;
	case NTRDMA_WC_ERR_RECV_NOT_READY:
		return IB_WC_RESP_TIMEOUT_ERR;
	case NTRDMA_WC_ERR_RECV_MISSING:
		return IB_WC_REM_INV_REQ_ERR;
	case NTRDMA_WC_ERR_RECV_OVERFLOW:
	case NTRDMA_WC_ERR_RDMA_KEY:
	case NTRDMA_WC_ERR_RDMA_RANGE:
	case NTRDMA_WC_ERR_RDMA_ACCESS:
		return IB_WC_REM_ACCESS_ERR;
	}
	return IB_WC_FATAL_ERR;
}

static inline int ntrdma_ib_wc_opcode_from_cqe(u32 op_code)
{
	switch (op_code) {
	case NTRDMA_WR_RECV:
	case NTRDMA_WR_RECV_INV:
	case NTRDMA_WR_RECV_IMM:
		return IB_WC_RECV;
	case NTRDMA_WR_RECV_RDMA:
		return IB_WC_RECV_RDMA_WITH_IMM;
	case NTRDMA_WR_SEND:
	case NTRDMA_WR_SEND_INV:
	case NTRDMA_WR_SEND_IMM:
		return IB_WC_SEND;
	case NTRDMA_WR_SEND_RDMA:
	case NTRDMA_WR_RDMA_WRITE:
		return IB_WC_RDMA_WRITE;
	case NTRDMA_WR_RDMA_READ:
		return IB_WC_RDMA_READ;
	}
	return -1;
}

static inline int ntrdma_ib_wc_flags_from_cqe(u32 op_code)
{
	switch (op_code) {
	case NTRDMA_WR_RECV_IMM:
	case NTRDMA_WR_RECV_RDMA:
		return IB_WC_WITH_IMM;
	case NTRDMA_WR_RECV_INV:
		return IB_WC_WITH_INVALIDATE;
	}
	return 0;
}

static int ntrdma_ib_wc_from_cqe(struct ib_wc *ibwc,
				 struct ntrdma_qp *qp,
				 struct ntrdma_cqe *cqe)
{
	ibwc->wr_id = cqe->ulp_handle;

	ibwc->status = ntrdma_ib_wc_status_from_cqe(cqe->op_status);
	ibwc->opcode = ntrdma_ib_wc_opcode_from_cqe(cqe->op_code);

	ibwc->qp = &qp->ibqp;
	ibwc->vendor_err = cqe->op_status;
	ibwc->byte_len = cqe->rdma_len;
	ibwc->ex.imm_data = cqe->imm_data;
	ibwc->src_qp = qp->rqp_key;
	ibwc->wc_flags = 0;
	ibwc->wc_flags = ntrdma_ib_wc_flags_from_cqe(cqe->op_code);
	ibwc->pkey_index = 0;
	ibwc->slid = 0;
	ibwc->sl = 0;
	ibwc->dlid_path_bits = 0;
	ibwc->port_num = 0;

	return 0;
}

static int ntrdma_poll_cq(struct ib_cq *ibcq,
			  int howmany,
			  struct ib_wc *ibwc)
{
	struct ntrdma_cq *cq = ntrdma_ib_cq(ibcq);
	struct ntrdma_qp *qp;
	struct ntrdma_cqe *cqe, abort_cqe;
	u32 pos, end, base;
	int count = 0, rc = 0;

	/* lock for completions */
	ntrdma_cq_cmpl_start(cq);

	while (count < howmany) {
		/* get the next completing range in the next qp ring */
		rc = ntrdma_cq_cmpl_get(cq, &qp, &pos, &end, &base);
		if (rc)
			break;

		for (;;) {
			/* current entry in the ring, or aborted into abort_cqe */
			cqe = ntrdma_cq_cmpl_cqe(cq, &abort_cqe, pos);

			if (!ntrdma_wr_code_push_data(cqe->op_code) || 
					(cqe->flags & IB_SEND_SIGNALED)) {
				/* transform the entry into the work completion */
				rc = ntrdma_ib_wc_from_cqe(&ibwc[count], qp, cqe);
				if (rc)
					break;

				++count;
			}

			++pos;

			/* quit after the last completion */
			if (count == howmany || pos == end)
				break;
		}

		/* update the next completing range */
		ntrdma_cq_cmpl_put(cq, pos, base);
	}

	/* release lock for later completions */
	ntrdma_cq_cmpl_done(cq);

	if (count)
		return count;
	if (rc == -EAGAIN)
		return 0;
	return rc;
}

static int ntrdma_req_notify_cq(struct ib_cq *ibcq,
				enum ib_cq_notify_flags flags)
{
	struct ntrdma_cq *cq = ntrdma_ib_cq(ibcq);

	ntrdma_cq_arm(cq);

	return 0;
}

static int ntrdma_alloc_pd(struct ib_pd *ibpd,
				struct ib_udata *ibudata)
{
	struct ib_device *ibdev = ibpd->device;
	struct ntrdma_dev *dev = ntrdma_ib_dev(ibdev);
	struct ntrdma_pd *pd = ntrdma_ib_pd(ibpd);
	int rc;

	ntrdma_vdbg(dev, "called\n");

	rc = ntrdma_pd_init(pd, dev, dev->pd_next_key++);
	if (rc)
		goto err_init;

	ntrdma_vdbg(dev, "initialized pd %p\n", pd);

	rc = ntrdma_pd_add(pd);
	if (rc)
		goto err_add;

	ntrdma_dbg(dev, "added pd%d\n", pd->key);

	return 0;

	// ntrdma_pd_del(pd);
err_add:
	ntrdma_pd_deinit(pd);
err_init:
	ntrdma_dbg(dev, "failed, returning err %d\n", rc);
	return rc;
}

static void ntrdma_dealloc_pd(struct ib_pd *ibpd,
				struct ib_udata *ibudata)
{
	struct ntrdma_pd *pd = ntrdma_ib_pd(ibpd);

	/* TODO: pd should not have any mr or qp: wait, or fail? */

	ntrdma_pd_del(pd);
	ntrdma_pd_repo(pd);
	ntrdma_pd_deinit(pd);
}

static struct ib_qp *ntrdma_create_qp(struct ib_pd *ibpd,
				      struct ib_qp_init_attr *ibqp_attr,
				      struct ib_udata *ibudata)
{
	struct ntrdma_pd *pd = ntrdma_ib_pd(ibpd);
	struct ntrdma_dev *dev = ntrdma_pd_dev(pd);
	struct ntrdma_cq *recv_cq = ntrdma_ib_cq(ibqp_attr->recv_cq);
	struct ntrdma_cq *send_cq = ntrdma_ib_cq(ibqp_attr->send_cq);
	struct ntrdma_qp *qp;
	struct ntrdma_qp_init_attr qp_attr;
	int rc;

	qp = kmalloc_node(sizeof(*qp), GFP_KERNEL, dev->node);
	if (!qp) {
		rc = -ENOMEM;
		goto err_qp;
	}

	memset(qp, 0, sizeof(*qp));

	qp_attr.pd_key = pd->key;
	qp_attr.recv_wqe_cap = ibqp_attr->cap.max_recv_wr;
	qp_attr.recv_wqe_sg_cap = ibqp_attr->cap.max_recv_sge;
	qp_attr.send_wqe_cap = ibqp_attr->cap.max_send_wr;
	qp_attr.send_wqe_sg_cap = ibqp_attr->cap.max_send_sge;

	rc = ntrdma_qp_init(qp, dev, recv_cq, send_cq, &qp_attr);
	if (rc)
		goto err_init;

	rc = ntrdma_qp_add(qp);
	if (rc)
		goto err_add;

	qp->ibqp.qp_num = qp->res.key;
	qp->state = IB_QPS_RESET;

	ntrdma_dbg(dev, "added qp%d\n", qp->res.key);

	return &qp->ibqp;

	// ntrdma_qp_del(qp);
err_add:
	ntrdma_qp_deinit(qp);
err_init:
	kfree(qp);
err_qp:
	ntrdma_dbg(dev, "failed, returning err %d\n", rc);
	return ERR_PTR(rc);
}

#define NTRDMA_IBQP_MASK_FAKE_SUPPORTED ( \
					  IB_QP_PKEY_INDEX		| \
					  IB_QP_AV			| \
					  IB_QP_PATH_MTU		| \
					  IB_QP_TIMEOUT			| \
					  IB_QP_RETRY_CNT		| \
					  IB_QP_RNR_RETRY		| \
					  IB_QP_RQ_PSN			| \
					  IB_QP_MAX_QP_RD_ATOMIC	| \
					  IB_QP_MIN_RNR_TIMER		| \
					  IB_QP_SQ_PSN			| \
					  IB_QP_MAX_DEST_RD_ATOMIC	| \
					  0 )

#define NTRDMA_IBQP_MASK_SUPPORTED	( \
					  IB_QP_STATE			| \
					  IB_QP_CUR_STATE		| \
					  IB_QP_ACCESS_FLAGS		| \
					  IB_QP_PORT			| \
					  IB_QP_DEST_QPN		| \
					  NTRDMA_IBQP_MASK_FAKE_SUPPORTED | \
					  0 )

static u32 ntrdma_ib_state(int ib_state)
{
	switch(ib_state) {
	case IB_QPS_RESET:
		return NTRDMA_QPS_RESET;
	case IB_QPS_INIT:
		return NTRDMA_QPS_INIT;
	case IB_QPS_RTR:
		return NTRDMA_QPS_RECV_READY;
	case IB_QPS_RTS:
		return NTRDMA_QPS_SEND_READY;
	case IB_QPS_SQD:
		return NTRDMA_QPS_SEND_DRAIN;
	case IB_QPS_ERR:
		return NTRDMA_QPS_ERROR;
	}
	return NTRDMA_QPS_ERROR;
}

static int ntrdma_query_qp(struct ib_qp *ibqp,
			   struct ib_qp_attr *ibqp_attr,
			   int ibqp_mask,
			   struct ib_qp_init_attr *ibqp_init_attr)
{
	struct ntrdma_qp *qp = ntrdma_ib_qp(ibqp);
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);

	ntrdma_dbg(dev, "ibqp_mask %#x\n", ibqp_mask);

	memset(ibqp_attr, 0, sizeof(*ibqp_attr));
	memset(ibqp_init_attr, 0, sizeof(*ibqp_init_attr));

	/* Note: qperf v0.4.9 passes mask 0, looking for max_inline_data.
	 *
	 * NTRDMA max_inline_data is zero.
	 *
	 * We just need to return success here to make qperf happy.
	 * But be careful and fail here if we get an unexpected mask.
	 */

	if (ibqp_mask)
		return -EINVAL;

	return 0;
}

static int ntrdma_modify_qp(struct ib_qp *ibqp,
			    struct ib_qp_attr *ibqp_attr,
			    int ibqp_mask,
			    struct ib_udata *ibudata)
{
	struct ntrdma_dev *dev;
	struct ntrdma_qp *qp;
	int rc, cur_state, new_state;

	qp = ntrdma_ib_qp(ibqp);
	dev = ntrdma_qp_dev(qp);

	ntrdma_res_lock(&qp->res);
	{
		cur_state = ibqp_mask & IB_QP_CUR_STATE ?
			ibqp_attr->cur_qp_state : qp->state;
		new_state = ibqp_mask & IB_QP_STATE ?
			ibqp_attr->qp_state : cur_state;

		ntrdma_dbg(dev, "state %d -> %d\n", cur_state, new_state);

		if (cur_state != qp->state ||
		    ibqp_mask & ~NTRDMA_IBQP_MASK_SUPPORTED ||
		    !ib_modify_qp_is_ok(cur_state, new_state,
					IB_QPT_RC, ibqp_mask)) {
			ntrdma_dbg(dev, "invalid modify\n");
			rc = -EINVAL;
			goto err_attr;
		}

		if (ibqp_mask & IB_QP_STATE)
			qp->state = ntrdma_ib_state(new_state);

		if (ibqp_mask & IB_QP_ACCESS_FLAGS)
			qp->access = ibqp_attr->qp_access_flags;

		if (ibqp_mask & IB_QP_DEST_QPN)
			qp->rqp_key = ibqp_attr->dest_qp_num;

		mutex_lock(&dev->res_lock);
		{
			rc = ntrdma_qp_modify(qp);
			/* FIXME: unchecked */
			ntrdma_dev_cmd_submit(dev);
		}
		mutex_unlock(&dev->res_lock);

		ntrdma_res_wait_cmds(&qp->res);
	}
	ntrdma_res_unlock(&qp->res);

	return 0;

err_attr:
	ntrdma_res_unlock(&qp->res);
	return rc;
}

static int ntrdma_destroy_qp(struct ib_qp *ibqp,
				struct ib_udata *udata)
{
	struct ntrdma_qp *qp = ntrdma_ib_qp(ibqp);

	/* TODO: what should be done about oustanding work requests? */

	ntrdma_qp_del(qp);
	ntrdma_qp_repo(qp);
	ntrdma_qp_deinit(qp);
	kfree(qp);

	return 0;
}

static int ntrdma_ib_send_to_wqe(struct ntrdma_dev *dev,
				 struct ntrdma_send_wqe *wqe,
				 const struct ib_send_wr *ibwr,
				 int sg_cap)
{
	int i;

	wqe->ulp_handle = ibwr->wr_id;
	wqe->flags = ibwr->send_flags;

	switch (ibwr->opcode) {
	case IB_WR_SEND:
		wqe->op_code = NTRDMA_WR_SEND;
		wqe->op_status = 0;
		wqe->recv_key = ~(u32)0;
		wqe->rdma_key = 0;
		wqe->rdma_len = 0;
		wqe->rdma_addr = 0;
		wqe->imm_data = 0;
		break;
	case IB_WR_SEND_WITH_IMM:
		wqe->op_code = NTRDMA_WR_SEND_IMM;
		wqe->op_status = 0;
		wqe->recv_key = ~(u32)0;
		wqe->rdma_key = 0;
		wqe->rdma_len = 0;
		wqe->rdma_addr = 0;
		wqe->imm_data = ibwr->ex.imm_data;
		break;
	case IB_WR_RDMA_WRITE_WITH_IMM:
		wqe->op_code = NTRDMA_WR_SEND_RDMA;
		wqe->op_status = 0;
		wqe->recv_key = ~(u32)0;
		wqe->rdma_key = rdma_wr(ibwr)->rkey;
		wqe->rdma_len = 0;
		wqe->rdma_addr = rdma_wr(ibwr)->remote_addr;
		wqe->imm_data = ibwr->ex.imm_data;
		break;
	case IB_WR_RDMA_WRITE:
		wqe->op_code = NTRDMA_WR_RDMA_WRITE;
		wqe->op_status = 0;
		wqe->recv_key = ~(u32)0;
		wqe->rdma_key = rdma_wr(ibwr)->rkey;
		wqe->rdma_len = 0;
		wqe->rdma_addr = rdma_wr(ibwr)->remote_addr;
		wqe->imm_data = 0;
		break;
	case IB_WR_RDMA_READ:
		wqe->op_code = NTRDMA_WR_RDMA_READ;
		wqe->op_status = 0;
		wqe->recv_key = ~(u32)0;
		wqe->rdma_key = rdma_wr(ibwr)->rkey;
		wqe->rdma_len = 0;
		wqe->rdma_addr = rdma_wr(ibwr)->remote_addr;
		wqe->imm_data = 0;
		break;
	default:
		ntrdma_dbg(dev, "unsupported send opcode %d\n",
			   ibwr->opcode);
		return -EINVAL;
	}

	if (ibwr->num_sge > sg_cap) {
		ntrdma_dbg(dev, "too many num_sge %d\n",
			   ibwr->num_sge);
		return -EINVAL;
	}

	wqe->sg_count = ibwr->num_sge;
	for (i = 0; i < ibwr->num_sge; ++i) {
		wqe->sg_list[i].addr = ibwr->sg_list[i].addr;
		wqe->sg_list[i].len = ibwr->sg_list[i].length;
		wqe->sg_list[i].key = ibwr->sg_list[i].lkey;
	}

	return 0;
}

static int ntrdma_post_send(struct ib_qp *ibqp,
			    const struct ib_send_wr *ibwr,
			    const struct ib_send_wr **bad)
{
	struct ntrdma_qp *qp = ntrdma_ib_qp(ibqp);
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);
	struct ntrdma_send_wqe *wqe;
	u32 pos, end, base;
	int rc;

	/* verify the qp state and lock for posting sends */
	rc = ntrdma_qp_send_post_start(qp);
	if (rc)
		goto out;

	while (ibwr) {
		/* get the next posting range in the ring */
		ntrdma_qp_send_post_get(qp, &pos, &end, &base);

		if (pos == end) {
			/* posting too many oustanding requests */
			ntrdma_dbg(dev, "posting too many sends\n");
			rc = -EINVAL;
			break;
		}

		for (;;) {
			/* current entry in the ring */
			wqe = ntrdma_qp_send_wqe(qp, pos);

			/* transform work request into the entry */
			rc = ntrdma_ib_send_to_wqe(dev, wqe, ibwr,
						   qp->send_wqe_sg_cap);
			if (rc)
				break;

			ibwr = ibwr->next;
			++pos;

			/* quit after the last request or end of range */
			if (!ibwr || pos == end)
				break;
		}

		/* update the next posting range */
		ntrdma_qp_send_post_put(qp, pos, base);

		if (rc)
			break;
	}

	/* release lock for state change or posting later sends */
	ntrdma_qp_send_post_done(qp);

out:
	*bad = ibwr;
	return rc;
}

static int ntrdma_ib_recv_to_wqe(struct ntrdma_recv_wqe *wqe,
				 const struct ib_recv_wr *ibwr,
				 int sg_cap)
{
	int i;

	wqe->ulp_handle = ibwr->wr_id;

	wqe->op_code = NTRDMA_WR_RECV;

	if (ibwr->num_sge > sg_cap)
		return -EINVAL;

	wqe->sg_count = ibwr->num_sge;
	for (i = 0; i < ibwr->num_sge; ++i) {
		wqe->sg_list[i].addr = ibwr->sg_list[i].addr;
		wqe->sg_list[i].len = ibwr->sg_list[i].length;
		wqe->sg_list[i].key = ibwr->sg_list[i].lkey;
	}

	return 0;
}

static int ntrdma_post_recv(struct ib_qp *ibqp,
			    const struct ib_recv_wr *ibwr,
			    const struct ib_recv_wr **bad)
{
	struct ntrdma_qp *qp = ntrdma_ib_qp(ibqp);
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);
	struct ntrdma_recv_wqe *wqe;
	u32 pos, end, base;
	int rc;

	/* verify the qp state and lock for posting recvs */
	rc = ntrdma_qp_recv_post_start(qp);
	if (rc)
		goto out;

	while (ibwr) {
		/* get the next posting range in the ring */
		ntrdma_qp_recv_post_get(qp, &pos, &end, &base);

		if (pos == end) {
			/* posting too many oustanding requests */
			ntrdma_dbg(dev, "posting too many recvs\n");
			rc = -EINVAL;
			break;
		}

		for (;;) {
			/* current entry in the ring */
			wqe = ntrdma_qp_recv_wqe(qp, pos);

			/* transform work request to queue entry */
			rc = ntrdma_ib_recv_to_wqe(wqe, ibwr,
						   qp->recv_wqe_sg_cap);
			if (rc)
				break;

			ibwr = ibwr->next;
			++pos;

			/* quit after the last request or end of range */
			if (!ibwr || pos == end)
				break;
		}

		/* update the next posting range */
		ntrdma_qp_recv_post_put(qp, pos, base);

		if (rc)
			break;
	}

	/* release lock for state change or posting later recvs */
	ntrdma_qp_recv_post_done(qp);

out:
	*bad = ibwr;
	return rc;
}

static struct ib_mr *ntrdma_reg_user_mr(struct ib_pd *ibpd,
					u64 start,
					u64 length,
					u64 virt_addr,
					int mr_access_flags,
					struct ib_udata *ibudata)
{
	struct ntrdma_pd *pd = ntrdma_ib_pd(ibpd);
	struct ntrdma_dev *dev = ntrdma_pd_dev(pd);
	struct ntrdma_mr *mr;
	void *umem;
	int rc, i, count;

	ntrdma_vdbg(dev, "called\n");

	umem = ntc_umem_get(dev->ntc, ibudata, start, length, mr_access_flags, false);

	if (IS_ERR(umem)) {
		rc = PTR_ERR(umem);
		goto err_umem;
	}

	count = ntc_umem_count(dev->ntc, umem);
	if (count < 0) {
		rc = count;
		goto err_mr;
	}

	mr = kmalloc_node(sizeof(*mr) + count * sizeof(*mr->sg_list),
			  GFP_KERNEL, dev->node);
	if (!mr) {
		rc = -ENOMEM;
		goto err_mr;
	}

	rc = ntrdma_mr_init(mr, dev, umem,
			    pd->key, mr_access_flags,
			    virt_addr, length, count);
	if (rc)
		goto err_init;

	rc = ntrdma_mr_add(mr);
	if (rc)
		goto err_add;

	mr->ibmr.lkey = mr->ibmr.rkey = mr->res.key;

	ntrdma_dbg(dev, "added mr%d\n", mr->res.key);
	ntrdma_dbg(dev, "addr %llx\n", mr->addr);
	ntrdma_dbg(dev, "access %x\n", mr->access);
	ntrdma_dbg(dev, "count %x\n", mr->sg_count);

	for (i = 0; i < count; ++i)
		ntrdma_dbg(dev, "sgl[%x] dma %llx len %llx\n",
			   i, mr->sg_list[i].addr, mr->sg_list[i].len);

	return &mr->ibmr;

	// ntrdma_mr_del(mr);
err_add:
	ntrdma_mr_deinit(mr);
err_init:
	kfree(mr);
err_mr:
	ntc_umem_put(dev->ntc, umem);
err_umem:
	ntrdma_dbg(dev, "failed, returning err %d\n", rc);
	return ERR_PTR(rc);
}

static int ntrdma_dereg_mr(struct ib_mr *ibmr,
				struct ib_udata *udata)
{
	struct ntrdma_mr *mr = ntrdma_ib_mr(ibmr);
	struct ntrdma_dev *dev = ntrdma_mr_dev(mr);
	void *umem;

	umem = mr->umem;

	ntrdma_mr_del(mr);
	ntrdma_mr_repo(mr);
	kfree(mr);
	ntc_umem_put(dev->ntc, umem);

	return 0;
}

static int ntrdma_alloc_ucontext(struct ib_ucontext *ibuctx,
				 struct ib_udata *ibudata)
{
	// Nothing to do here
	return 0;
}

static void ntrdma_dealloc_ucontext(struct ib_ucontext *ibuctx)
{
	// Nothing to do here
}

int ntrdma_dev_ib_init(struct ntrdma_dev *dev)
{
	struct ib_device *ibdev = &dev->ibdev;
	int rc;

	ibdev->owner			= THIS_MODULE;
	ibdev->node_type		= RDMA_NODE_IB_CA;
	/* TODO: maybe this should be the number of virtual doorbells */
	ibdev->num_comp_vectors		= 1;

	ibdev->dev.parent = ntc_map_dev(dev->ntc);

	ibdev->uverbs_abi_ver		= 1;
	ibdev->phys_port_cnt		= 1;

	ibdev->uverbs_cmd_mask		=
		(1ull << IB_USER_VERBS_CMD_GET_CONTEXT)			|
		(1ull << IB_USER_VERBS_CMD_QUERY_DEVICE)		|
		(1ull << IB_USER_VERBS_CMD_QUERY_PORT)			|
		(1ull << IB_USER_VERBS_CMD_ALLOC_PD)			|
		(1ull << IB_USER_VERBS_CMD_DEALLOC_PD)			|
		(1ull << IB_USER_VERBS_CMD_REG_MR)			|
		(1ull << IB_USER_VERBS_CMD_DEREG_MR)			|
		(1ull << IB_USER_VERBS_CMD_CREATE_COMP_CHANNEL)		|
		(1ull << IB_USER_VERBS_CMD_REQ_NOTIFY_CQ)		|
		(1ull << IB_USER_VERBS_CMD_CREATE_CQ)			|
		(1ull << IB_USER_VERBS_CMD_DESTROY_CQ)			|
		(1ull << IB_USER_VERBS_CMD_POLL_CQ)			|
		(1ull << IB_USER_VERBS_CMD_CREATE_QP)			|
		(1ull << IB_USER_VERBS_CMD_QUERY_QP)			|
		(1ull << IB_USER_VERBS_CMD_MODIFY_QP)			|
		(1ull << IB_USER_VERBS_CMD_DESTROY_QP)			|
		(1ull << IB_USER_VERBS_CMD_POST_SEND)			|
		(1ull << IB_USER_VERBS_CMD_POST_RECV)			|
		0ull;

	/* not implemented / not required */
	ibdev->ops.get_port_immutable	= ntrdma_get_port_immutable;
	ibdev->ops.query_pkey		= ntrdma_query_pkey;
	ibdev->ops.query_gid		= ntrdma_query_gid;
	ibdev->ops.create_ah		= ntrdma_create_ah;
	ibdev->ops.destroy_ah		= ntrdma_destroy_ah;
	ibdev->ops.get_dma_mr		= ntrdma_get_dma_mr;

	/* userspace context */
	ibdev->ops.alloc_ucontext	= ntrdma_alloc_ucontext;
	ibdev->ops.dealloc_ucontext	= ntrdma_dealloc_ucontext;

	/* device and port queries */
	ibdev->ops.query_device		= ntrdma_query_device;
	ibdev->ops.query_port		= ntrdma_query_port;

	/* completion queue */
	ibdev->ops.create_cq		= ntrdma_create_cq;
	ibdev->ops.destroy_cq		= ntrdma_destroy_cq;
	ibdev->ops.poll_cq		= ntrdma_poll_cq;
	ibdev->ops.req_notify_cq	= ntrdma_req_notify_cq;

	/* protection domain */
	ibdev->ops.alloc_pd		= ntrdma_alloc_pd;
	ibdev->ops.dealloc_pd		= ntrdma_dealloc_pd;

	/* memory region */
	ibdev->ops.reg_user_mr		= ntrdma_reg_user_mr;
	ibdev->ops.dereg_mr		= ntrdma_dereg_mr;

	/* queue pair */
	ibdev->ops.create_qp		= ntrdma_create_qp;
	ibdev->ops.query_qp		= ntrdma_query_qp;
	ibdev->ops.modify_qp		= ntrdma_modify_qp;
	ibdev->ops.destroy_qp		= ntrdma_destroy_qp;
	ibdev->ops.post_send		= ntrdma_post_send;
	ibdev->ops.post_recv		= ntrdma_post_recv;


	rc = ib_register_device(ibdev, "ntrdma_%d");
	if (rc)
		goto err_ib;

	return 0;

err_ib:
	return rc;
}

void ntrdma_dev_ib_deinit(struct ntrdma_dev *dev)
{
	ib_unregister_device(&dev->ibdev);
}
