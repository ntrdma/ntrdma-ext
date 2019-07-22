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
#include <linux/random.h>

#include <rdma/ib_user_verbs.h>
#include <linux/ntc_trace.h>
#include <rdma/ib_mad.h>
#include <rdma/ib_verbs.h>

#include "ntrdma_dev.h"
#include "ntrdma_cmd.h"
#include "ntrdma_cq.h"
#include "ntrdma_mr.h"
#include "ntrdma_pd.h"
#include "ntrdma_qp.h"
#include "ntrdma_wr.h"
#include "ntrdma_eth.h"

#define NTRDMA_PKEY_DEFAULT 0xffff
#define NTRDMA_GIB_TBL_LEN 1
#define NTRDMA_PKEY_TBL_LEN 2

#define DELL_VENDOR_ID 0x1028
#define NOT_SUPPORTED 0

DECLARE_PER_CPU(struct ntrdma_dev_counters, dev_cnt);

struct net_device *ntrdma_get_netdev(struct ib_device *ibdev,
						 u8 port_num)
{
	struct ntrdma_dev *dev = ntrdma_ib_dev(ibdev);
	struct net_device *ndev = ntrdma_get_net(dev);

	dev_hold(ndev);

	return ndev;
}

/* not implemented / not required? */
static int ntrdma_get_port_immutable(struct ib_device *ibdev,
				     u8 port,
				     struct ib_port_immutable *imm)
{
	imm->pkey_tbl_len = NTRDMA_PKEY_TBL_LEN;
	imm->gid_tbl_len = 8;
	imm->core_cap_flags =
			RDMA_CORE_CAP_PROT_ROCE |
			RDMA_CORE_PORT_IBA_ROCE_UDP_ENCAP;
	imm->max_mad_size = IB_MGMT_MAD_SIZE;

	return 0;
}

/* not implemented / not required? */
static int ntrdma_query_pkey(struct ib_device *ibdev,
			     u8 port_num,
			     u16 index,
			     u16 *pkey)
{
	if (index > NTRDMA_PKEY_TBL_LEN)
		return -EINVAL;

	*pkey = NTRDMA_PKEY_DEFAULT;

	return 0;
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
	ntrdma_dbg(dev, "imm core cap flags %#x",
		   ibdev->port_immutable[port_num].core_cap_flags);

	/* Everything is "link local" since we don't have an interface */
	addr->s6_addr32[0] = htonl(0xfe800000);
	addr->s6_addr32[1] = 0;
	addr->s6_addr32[2] = 0;
	addr->s6_addr32[3] = 0;

	return 0;
}

struct ntrdma_ah {
	struct ib_ah ibah;
	struct rdma_ah_attr attr;
};

/* not implemented / not required? */
/* if required, one needs to implement:
 * Perform path query to the Subnet Administrator (SA)
	Out of band connection to the remote node, for example: using socket
 * Using well-known values, for example: this can be done in a static
    subnet (which all of the addresses are predefined) or using
    multicast groups
 */
static struct ib_ah *ntrdma_create_ah(struct ib_pd *ibpd,
				      struct rdma_ah_attr *ah_attr,
				      struct ib_udata *udata)
{
	struct ntrdma_ah *ah = kzalloc(sizeof(*ah), GFP_ATOMIC);

	if (!ah)
		return ERR_PTR(-ENOMEM);

	ah->attr = *ah_attr;

	return &ah->ibah;

}

/* not implemented / not required? */
static int ntrdma_destroy_ah(struct ib_ah *ibah)
{
	struct ntrdma_ah *ah = container_of(ibah, struct ntrdma_ah, ibah);

	kfree(ah);
	return 0;
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
	struct ntrdma_dev *dev = ntrdma_ib_dev(ibdev);

	memset(ibattr, 0, sizeof(*ibattr));

	/* ntrdma versioning */
	ibattr->fw_ver = dev->latest_version;

	/* from the mlx web, most of the times, its equal to node guid.*/
	ibattr->sys_image_guid = ibdev->node_guid;

	/* Size (in bytes) of the largest contiguous memory block
	 * that can be registered by this device
	 */
	ibattr->max_mr_size = IB_MR_LIMIT_BYTES; // 1GB currently ntrdma_cmd.c

	/* Memory page size supported by this device */
	ibattr->page_size_cap = PAGE_SIZE;

	ibattr->vendor_id =  DELL_VENDOR_ID; /* Dell */

	ibattr->hw_ver = ntc_query_version(dev->ntc);

	/* Maximum number of QPs, of UD/UC/RC transport types,
	 * supported by this device
	 * Currently no such limit is enforced
	 */
	ibattr->max_qp = NTRDMA_DEV_MAX_QP;

	/* limit currently no limit on qp's size which is set
	 * from the user of verbs
	 */

	/* Max outstanding wqe on any send/recvieve queue */
	ibattr->max_qp_wr = NTRDMA_DEV_MAX_QP_WR;

	ibattr->device_cap_flags = IB_DEVICE_LOCAL_DMA_LKEY;

	/* Maximum number of scatter/gather entries per Send
	 * or Receive Work Request, in a QP other than RD,
	 * supported by this device
	 */
	ibattr->max_sge = NTRDMA_DEV_MAX_SGE;

	/* Maximum number of CQs supported by this device */
	ibattr->max_cq = NTRDMA_DEV_MAX_CQ;

	/* Maximum number of entries in each CQ supported by this device */
	ibattr->max_cqe = NTRDMA_DEV_MAX_CQE;

	/* Maximum number of MRs supported by this device */
	ibattr->max_mr = NTRDMA_DEV_MAX_MR;

	/* Maximum number of PDs  supported by this device */
	ibattr->max_pd = NTRDMA_DEV_MAX_PD;

	/* Atomic operations aren't supported at all */
	ibattr->atomic_cap = NOT_SUPPORTED; //IBV_ATOMIC_NONE;

	/* Not supporting srq */
	ibattr->max_srq = NOT_SUPPORTED;
	ibattr->max_srq_wr = NOT_SUPPORTED; // outstanding wqe in SRQ
	ibattr->max_srq_sge = NOT_SUPPORTED; // max sge per SRQ wqe

	return 0;
}

static int ntrdma_query_port(struct ib_device *ibdev,
			     u8 port, struct ib_port_attr *ibattr)
{
	struct ntrdma_dev *dev = ntrdma_ib_dev(ibdev);

	memset(ibattr, 0, sizeof(*ibattr));

	ibattr->subnet_prefix = 0;

	/* TODO: it should reflect the state of the logical-link, ntc_ntb_msi,
	 * once hello phase is finished
	 */
	ibattr->state = ntc_is_link_up(dev->ntc) ? IB_PORT_ACTIVE
			: IB_PORT_DOWN;

	/* dma/pcie fragmentation? */
	ibattr->max_mtu = IB_MTU_256;
	ibattr->active_mtu = IB_MTU_256;

	/* ntrdma define */
	ibattr->gid_tbl_len = NTRDMA_GIB_TBL_LEN;

	/* ntrdma define */
	ibattr->pkey_tbl_len = NTRDMA_PKEY_TBL_LEN;

	/*  lid of the Subnet Manager (SM) - do we have a subnet notion?*/
	ibattr->sm_lid = 0;

	/* port unique id within the subnet */
	ibattr->lid = 0;

	/* sl of the Subnet Manager (SM) */
	ibattr->sm_sl = 0;

	/*
	 * The active link width of this port. There isn't any enumeration
	 * of this value, and the numeric value can be one of the following:
	 *	1 - 1x
	 *	2 - 4x
	 *	4 - 8x
	 *	8 - 12x
	 *
	 *	we may call
	 *	u64 (*link_is_up)(struct ntb_dev *ntb,
	 *  enum ntb_speed *speed, enum ntb_width *width);
	 *
	 *  currently leaving at 1x, is this equal to the pcie lanes width?
	 */
	ibattr->active_width = IB_WIDTH_1X;
	/*
	 * The active link speed of this port. There isn't any enumeration
	 * of this value, and the numeric value can be one of the following:
	 * 1 - 2.5 Gbps
	 * 2 - 5.0 Gbps
	 * 4 - 10.0 Gbps
	 * 8 - 10.0 Gbps
	 * 16 - 14.0 Gbps
	 * 32 - 25.0 Gbps
	 *
	 * u64 (*link_is_up)(struct ntb_dev *ntb,
	 *		  enum ntb_speed *speed, enum ntb_width *width);
	 */
	ibattr->active_speed = IB_SPEED_DDR; //

	return 0;
}

static struct ib_cq *ntrdma_create_cq(struct ib_device *ibdev,
		const struct ib_cq_init_attr *ibattr,
		struct ib_ucontext *ibuctx,
		struct ib_udata *ibudata)
{
	struct ntrdma_dev *dev = ntrdma_ib_dev(ibdev);
	struct ntrdma_cq *cq;
	u32 vbell_idx;
	int rc;

	if (atomic_inc_return(&dev->cq_num) >= NTRDMA_DEV_MAX_CQ) {
		rc = -ETOOMANYREFS;
		goto err_cq;
	}

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

	ntrdma_dbg(dev, "added cq %p ib cq %p\n", cq, &cq->ibcq);

	return &cq->ibcq;

	// ntrdma_cq_del(cq);
err_add:
	ntrdma_cq_deinit(cq);
err_init:
	kfree(cq);
err_cq:
	atomic_dec(&dev->cq_num);
	ntrdma_dbg(dev, "failed, returning err %d\n", rc);
	return ERR_PTR(rc);
}

static int ntrdma_destroy_cq(struct ib_cq *ibcq)
{
	struct ntrdma_cq *cq = ntrdma_ib_cq(ibcq);
	struct ntrdma_dev *dev = NULL;

	if (!cq) {
		ntrdma_err(dev, "ntrdma_destroy_cq failed, destroying NULL cq\n");
		return -EFAULT;
	}
	dev = ntrdma_cq_dev(cq);
	/* TODO: what should be done about oustanding work completions? */

	ntrdma_cq_del(cq);
	ntrdma_cq_repo(cq);
	ntrdma_cq_deinit(cq);
	kfree(cq);
	atomic_dec(&dev->cq_num);
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

			/*complition should be generated for post send with IB_SEND_SIGNALED flag*/
			if (!ntrdma_wr_code_push_data(cqe->op_code) ||
					(cqe->flags & IB_SEND_SIGNALED)) {
				/* transform the entry into the work completion */
				rc = ntrdma_ib_wc_from_cqe(&ibwc[count], qp, cqe);
				if (rc)
					break;

				TRACE("OPCODE %d(%d): wrid %llu QP %d status %d pos %u end %u\n",
						ibwc[count].opcode,
						cqe->op_code,
						ibwc[count].wr_id,
						qp->res.key,
						ibwc[count].status,
						pos,
						end);

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

	if (count) {
		this_cpu_add(dev_cnt.cqes_polled, count);
		return count;
	}
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

static struct ib_pd *ntrdma_alloc_pd(struct ib_device *ibdev,
				     struct ib_ucontext *ibuctx,
				     struct ib_udata *ibudata)
{
	struct ntrdma_dev *dev = ntrdma_ib_dev(ibdev);
	struct ntrdma_pd *pd;
	int rc;

	ntrdma_vdbg(dev, "called\n");

	if (atomic_inc_return(&dev->pd_num) >= NTRDMA_DEV_MAX_PD) {
		rc = -ETOOMANYREFS;
		goto err_pd;
	}

	pd = kmalloc_node(sizeof(*pd), GFP_KERNEL, dev->node);
	if (!pd) {
		rc = -ENOMEM;
		goto err_pd;
	}

	ntrdma_vdbg(dev, "allocated pd %p\n", pd);

	rc = ntrdma_pd_init(pd, dev, dev->pd_next_key++);
	if (rc)
		goto err_init;

	ntrdma_vdbg(dev, "initialized pd %p\n", pd);

	rc = ntrdma_pd_add(pd);
	if (rc)
		goto err_add;

	ntrdma_dbg(dev, "added pd%d\n", pd->key);

	return &pd->ibpd;

	// ntrdma_pd_del(pd);
err_add:
	ntrdma_pd_deinit(pd);
err_init:
	kfree(pd);
err_pd:
	atomic_dec(&dev->pd_num);
	ntrdma_dbg(dev, "failed, returning err %d\n", rc);
	return ERR_PTR(rc);
}

static int ntrdma_dealloc_pd(struct ib_pd *ibpd)
{
	struct ntrdma_pd *pd = ntrdma_ib_pd(ibpd);
	struct ntrdma_dev *dev = NULL;

	if (!pd) {
		ntrdma_err(dev, "ntrdma_dealloc_pd failed, dealloc NULL pd\n");
		return 0;
	}

	dev = ntrdma_pd_dev(pd);
	/* TODO: pd should not have any mr or qp: wait, or fail? */

	ntrdma_pd_del(pd);
	ntrdma_pd_repo(pd);
	ntrdma_pd_deinit(pd);
	kfree(pd);
	atomic_dec(&dev->pd_num);
	return 0;
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

	if (atomic_inc_return(&dev->qp_num) >= NTRDMA_DEV_MAX_QP) {
		rc = -ETOOMANYREFS;
		goto err_qp;
	}

	qp = kmalloc_node(sizeof(*qp), GFP_KERNEL, dev->node);
	if (!qp) {
		rc = -ENOMEM;
		goto err_qp;
	}

	memset(qp, 0, sizeof(*qp));

	qp_attr.pd_key = pd->key;

	/* Avoiding zero size alloc in case max_*_wr is 0 */
	qp_attr.recv_wqe_cap = (!ibqp_attr->cap.max_recv_wr) ?
				1 : ibqp_attr->cap.max_recv_wr;
	qp_attr.recv_wqe_sg_cap = ibqp_attr->cap.max_recv_sge;

	/* Avoiding zero size alloc in case max_*_wr is 0 */
	qp_attr.send_wqe_cap = (!ibqp_attr->cap.max_send_wr) ?
				1 : ibqp_attr->cap.max_send_wr;
	qp_attr.send_wqe_sg_cap = ibqp_attr->cap.max_send_sge;
	qp_attr.qp_type = ibqp_attr->qp_type;

	if (qp_attr.recv_wqe_cap > NTRDMA_DEV_MAX_QP_WR ||
		qp_attr.send_wqe_cap > NTRDMA_DEV_MAX_QP_WR ||
		qp_attr.send_wqe_sg_cap > NTRDMA_DEV_MAX_SGE ||
		qp_attr.recv_wqe_sg_cap > NTRDMA_DEV_MAX_SGE) {

		ntrdma_err(dev, "send_wqe(cap=%u,sg=%u), recv_wqe(cap=%u,sg=%u)\n",
				 qp_attr.send_wqe_cap, qp_attr.send_wqe_sg_cap,
				 qp_attr.recv_wqe_cap, qp_attr.recv_wqe_sg_cap);
		rc = -ETOOMANYREFS;
		goto err_init;
	}

	rc = ntrdma_qp_init(qp, dev, recv_cq, send_cq, &qp_attr);
	if (rc)
		goto err_init;

	rc = ntrdma_qp_add(qp);
	if (rc)
		goto err_add;

	qp->ibqp.qp_num = qp->res.key;
	qp->state = IB_QPS_RESET;

	ntrdma_dbg(dev, "added qp%d type %d\n",
			qp->res.key, ibqp_attr->qp_type);

	return &qp->ibqp;

	// ntrdma_qp_del(qp);
err_add:
	ntrdma_qp_deinit(qp);
err_init:
	kfree(qp);
err_qp:
	atomic_dec(&dev->qp_num);
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
					  IB_QP_QKEY			| \
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

static void ntrdma_modify_qp_debug(struct ib_qp *ibqp,
		struct ib_qp_attr *ibqp_attr,
		int ibqp_mask,
		struct ib_udata *ibudata)
{
	struct ntrdma_dev *dev;
	struct ntrdma_qp *qp;

	qp = ntrdma_ib_qp(ibqp);
	dev = ntrdma_qp_dev(qp);

	if (ibqp_mask & IB_QP_STATE) {
		ntrdma_vdbg(dev, "IB_QP_STATE %d\n",
				ibqp_attr->qp_state);
	}

	if (ibqp_mask & IB_QP_CUR_STATE) {
		ntrdma_vdbg(dev, "IB_QP_CUR_STATE %d\n",
				ibqp_attr->cur_qp_state);
	}

	if (ibqp_mask & IB_QP_EN_SQD_ASYNC_NOTIFY) {
		ntrdma_vdbg(dev, "IB_QP_EN_SQD_ASYNC_NOTIFY %d\n",
				ibqp_attr->en_sqd_async_notify);
	}

	if (ibqp_mask & IB_QP_ACCESS_FLAGS) {
		ntrdma_vdbg(dev, "IB_QP_ACCESS_FLAGS %d\n",
				ibqp_attr->qp_access_flags);
	}

	if (ibqp_mask & IB_QP_PKEY_INDEX) {
		ntrdma_vdbg(dev, "IB_QP_PKEY_INDEX %d\n",
				ibqp_attr->pkey_index);
	}

	if (ibqp_mask & IB_QP_PORT) {
		ntrdma_vdbg(dev, "IB_QP_PORT %d\n",
				ibqp_attr->port_num);
	}

	if (ibqp_mask & IB_QP_QKEY) {
		ntrdma_vdbg(dev, "IB_QP_QKEY %d\n",
				ibqp_attr->qkey);
	}

	if (ibqp_mask & IB_QP_AV) {
		ntrdma_vdbg(dev, "IB_QP_AV\n");
	}

	if (ibqp_mask & IB_QP_PATH_MTU) {
		ntrdma_vdbg(dev, "IB_QP_PATH_MTU %d\n",
				ibqp_attr->path_mtu);
	}

	if (ibqp_mask & IB_QP_TIMEOUT) {
		ntrdma_vdbg(dev, "IB_QP_TIMEOUT %d\n",
				ibqp_attr->timeout);
	}

	if (ibqp_mask & IB_QP_RETRY_CNT) {
		ntrdma_vdbg(dev, "IB_QP_RETRY_CNT %d\n",
				ibqp_attr->retry_cnt);
	}

	if (ibqp_mask & IB_QP_RNR_RETRY) {
		ntrdma_vdbg(dev, "IB_QP_RNR_RETRY %d\n",
				ibqp_attr->rnr_retry);
	}

	if (ibqp_mask & IB_QP_RQ_PSN) {
		ntrdma_vdbg(dev, "IB_QP_RQ_PSN %d\n",
				ibqp_attr->rq_psn);
	}

	if (ibqp_mask & IB_QP_MAX_QP_RD_ATOMIC) {
		ntrdma_vdbg(dev, "IB_QP_MAX_QP_RD_ATOMIC %d\n",
				ibqp_attr->max_rd_atomic);
	}

	if (ibqp_mask & IB_QP_ALT_PATH)
		ntrdma_vdbg(dev, "IB_QP_ALT_PATH\n");

	if (ibqp_mask & IB_QP_MIN_RNR_TIMER) {
		ntrdma_vdbg(dev, "IB_QP_MIN_RNR_TIMER %d\n",
				ibqp_attr->min_rnr_timer);
	}

	if (ibqp_mask & IB_QP_SQ_PSN) {
		ntrdma_vdbg(dev, "IB_QP_SQ_PSN %d\n",
				ibqp_attr->sq_psn);
	}

	if (ibqp_mask & IB_QP_MAX_DEST_RD_ATOMIC) {
		ntrdma_vdbg(dev, "IB_QP_MAX_DEST_RD_ATOMIC %d\n",
				ibqp_attr->max_dest_rd_atomic);
	}

	if (ibqp_mask & IB_QP_PATH_MIG_STATE) {
		ntrdma_vdbg(dev, "IB_QP_PATH_MIG_STATE %d\n",
				ibqp_attr->path_mig_state);
	}

	if (ibqp_mask & IB_QP_CAP) {
		ntrdma_vdbg(dev, "IB_QP_CAP\n");
	}

	if (ibqp_mask & IB_QP_DEST_QPN) {
		ntrdma_vdbg(dev, "IB_QP_DEST_QPN %d\n",
				ibqp_attr->dest_qp_num);
	}
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

	ntrdma_modify_qp_debug(ibqp, ibqp_attr, ibqp_mask, ibudata);
	ntrdma_res_lock(&qp->res);

	cur_state = ibqp_mask & IB_QP_CUR_STATE ?
			ibqp_attr->cur_qp_state : qp->state;
	new_state = ibqp_mask & IB_QP_STATE ?
			ibqp_attr->qp_state : cur_state;

	ntrdma_dbg(dev, "QP type %d state %d -> %d\n",
			ibqp->qp_type, cur_state, new_state);

	if (cur_state != qp->state) {
		ntrdma_err(dev,
				"%s: unexpected current state %d %d\n",
				__func__, cur_state, qp->state);
		rc = -EINVAL;
		goto unlock_exit;
	}

	if (ibqp_mask & ~NTRDMA_IBQP_MASK_SUPPORTED) {
		ntrdma_err(dev, "%s: unsupported QP mask %x\n",
				__func__,
				ibqp_mask & ~NTRDMA_IBQP_MASK_SUPPORTED);
		rc = -EINVAL;
		goto unlock_exit;
	}

	rc = ib_modify_qp_is_ok(cur_state, new_state,
			ibqp->qp_type, ibqp_mask,
			IB_LINK_LAYER_UNSPECIFIED);

	if (!rc) {
		ntrdma_err(dev, "ib_modify_qp_is_ok failed\n");
		rc = -EINVAL;
		goto unlock_exit;
	}

	if (ibqp_mask & IB_QP_STATE) {
		ntrdma_vdbg(dev, "qp %p state %d -> %d\n",
				qp, qp->state,
				ntrdma_ib_state(new_state));

		qp->state = ntrdma_ib_state(new_state);
	}

	if (ibqp_mask & IB_QP_ACCESS_FLAGS)
		qp->access = ibqp_attr->qp_access_flags;

	if (ibqp_mask & IB_QP_DEST_QPN)
		qp->rqp_key = ibqp_attr->dest_qp_num;

	/* For GSI and MSI QPs we used reserved key 1 and 2 */
	if (ibqp->qp_type == IB_QPT_GSI || ibqp->qp_type == IB_QPT_SMI)
		qp->rqp_key = ibqp->qp_type;

	rc = 0;

	mutex_lock(&dev->res_lock);

	if (dev->res_enable) {
		rc = ntrdma_qp_modify(qp);
		if (!WARN(rc, "ntrdma_qp_modify: failed rc = %d", rc))
			ntrdma_dev_cmd_submit(dev);

		mutex_unlock(&dev->res_lock);

		rc = ntrdma_res_wait_cmds(&qp->res);

		if (rc) {
			ntrdma_err(dev,
					"ntrdma qp modify cmd timeout after %lu msec",
					qp->res.timeout);
			ntrdma_unrecoverable_err(dev);
		}
	} else {
		mutex_unlock(&dev->res_lock);
	}

unlock_exit:
	ntrdma_res_unlock(&qp->res);
	return rc;
}

static int ntrdma_destroy_qp(struct ib_qp *ibqp)
{
	struct ntrdma_dev *dev = NULL;
	struct ntrdma_qp *qp = ntrdma_ib_qp(ibqp);

	if (!qp) {
		ntrdma_err(dev, "trying to destroy a NULL ptr qp\n");
		return -EFAULT;
	}

	dev = ntrdma_qp_dev(qp);
	/* TODO: what should be done about oustanding work requests? */

	ntrdma_qp_del(qp);
	ntrdma_qp_repo(qp);
	ntrdma_qp_deinit(qp);
	kfree(qp);

	if (dev)
		atomic_dec(&dev->qp_num);

	return 0;
}

static int ntrdma_ib_send_to_wqe(struct ntrdma_dev *dev,
		struct ntrdma_send_wqe *wqe,
		struct ib_send_wr *ibwr,
		struct ntrdma_qp *qp)
{
	int i;
	int sg_cap = qp->send_wqe_sg_cap;

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
		this_cpu_add(dev_cnt.post_send_bytes, wqe->sg_list[i].len);
	}

	if (wqe->flags & IB_SEND_SIGNALED)
		this_cpu_add(dev_cnt.post_send_wqes_signalled, wqe->sg_count);

	this_cpu_add(dev_cnt.post_send_wqes, wqe->sg_count);

	return 0;
}

static int ntrdma_post_send(struct ib_qp *ibqp,
			    struct ib_send_wr *ibwr,
			    struct ib_send_wr **bad)
{
	struct ntrdma_qp *qp = ntrdma_ib_qp(ibqp);
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);
	struct ntrdma_send_wqe *wqe;
	u32 pos, end, base;
	int rc;

	/* verify the qp state and lock for posting sends */
	rc = ntrdma_qp_send_post_start(qp);
	if (rc) {
		ntrdma_err(dev,
				"ntrdma_qp_send_post_start failed %d qp %d\n",
				rc, qp->res.key);
		goto out;
	}

	while (ibwr) {
		/* get the next posting range in the ring */
		ntrdma_qp_send_post_get(qp, &pos, &end, &base);

		if (pos == end) {
			ntrdma_err(dev,
					"posting too many sends QP %d\n",
					qp->res.key);
			rc = -EINVAL;
			break;
		}

		for (;;) {
			/* current entry in the ring */
			wqe = ntrdma_qp_send_wqe(qp, pos);

			/* transform work request into the entry */
			rc = ntrdma_ib_send_to_wqe(dev, wqe, ibwr, qp);

			TRACE("OPCODE %d: flags %x, addr %llx, rc = %d QP %d num sges %d pos %d wr_id %llu\n",
					ibwr->opcode, ibwr->send_flags,
					wqe->rdma_addr, rc, qp->res.key,
					ibwr->num_sge, pos, ibwr->wr_id);

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
				 struct ib_recv_wr *ibwr,
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
			    struct ib_recv_wr *ibwr,
			    struct ib_recv_wr **bad)
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
			TRACE("OPCODE %d: wrid %llu QP %d, rc = %d\n",
					wqe->op_code, ibwr->wr_id,
					qp->res.key, rc);
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

	ntrdma_vdbg(dev, "called user addr %llx len %lld:\n",
			virt_addr, length);

	if (length > IB_MR_LIMIT_BYTES) {
		ntrdma_err(dev, "reg_user_mr with not supported length %lld\n",
				length);
		rc = -EINVAL;
		goto err_len;
	}

	if (atomic_inc_return(&dev->mr_num) >= NTRDMA_DEV_MAX_MR) {
		rc = -ETOOMANYREFS;
		goto err_umem;
	}

	if (!is_canonical(virt_addr)) {
		rc = -EINVAL;
		ntrdma_err(dev, "reg_user_mr with non canonical addr (corrupted?)\n");
		goto err_umem;
	}

	umem = ntc_umem_get(dev->ntc, pd->ibpd.uobject->context,
			    start, length, mr_access_flags, false);

	if (IS_ERR(umem)) {
		rc = PTR_ERR(umem);
		goto err_umem;
	}

	count = ntc_umem_count(dev->ntc, umem);
	if (count < 0) {
		rc = count;
		goto err_mr;
	}

	/*
	 * multiple count by 2 because we need save 2 SGLs,
	 * one for IOAT and one for NTB.
	 */

	mr = kmalloc_node(sizeof(*mr) + 2 * count * sizeof(*mr->sg_list),
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

	for (i = 0; i < count; ++i) {
		ntrdma_vdbg(dev, "local sgl[%d] dma %#llx len %#llx\n",
				i, mr->local_dma[i].addr,
				mr->local_dma[i].len);
		ntrdma_vdbg(dev, "remote sgl[%d] dma %#llx len %#llx\n",
				i, mr->remote_dma[i].addr,
				mr->remote_dma[i].len);
	}

	return &mr->ibmr;

	// ntrdma_mr_del(mr);
err_add:
	ntrdma_mr_deinit(mr);
err_init:
	kfree(mr);
err_mr:
	ntc_umem_put(dev->ntc, umem);
err_umem:
	atomic_dec(&dev->mr_num);
err_len:
	ntrdma_dbg(dev, "failed, returning err %d\n", rc);
	return ERR_PTR(rc);
}

static int ntrdma_dereg_mr(struct ib_mr *ibmr)
{
	struct ntrdma_mr *mr = ntrdma_ib_mr(ibmr);
	struct ntrdma_dev *dev = ntrdma_mr_dev(mr);
	void *umem;

	umem = mr->umem;

	ntrdma_mr_del(mr);
	ntrdma_mr_repo(mr);
	kfree(mr);
	ntc_umem_put(dev->ntc, umem);
	atomic_dec(&dev->mr_num);
	return 0;
}

static struct ib_ucontext *ntrdma_alloc_ucontext(struct ib_device *ibdev,
						 struct ib_udata *ibudata)
{
	struct ntrdma_dev *dev = ntrdma_ib_dev(ibdev);
	struct ib_ucontext *ibuctx;
	int rc;

	ibuctx = kmalloc_node(sizeof(*ibuctx), GFP_KERNEL, dev->node);
	if (!ibuctx) {
		rc = -ENOMEM;
		goto err_ctx;
	}

	return ibuctx;

	// kfree(ibuctx);
err_ctx:
	return ERR_PTR(rc);
}

static int ntrdma_dealloc_ucontext(struct ib_ucontext *ibuctx)
{
	kfree(ibuctx);

	return 0;
}

static void ntrdma_set_node_guid(__be64 *guid)
{
	prandom_bytes(guid, sizeof(*guid));
}

enum rdma_link_layer	ntrdma_get_link_layer(struct ib_device *device,
		u8 port_num)
{
	return IB_LINK_LAYER_ETHERNET;
}

int ntrdma_add_gid(struct ib_device *device, u8 port_num,
		unsigned int index, const union ib_gid *gid,
		const struct ib_gid_attr *attr, void **context)
{
	return 0;
}

int ntrdma_del_gid(struct ib_device *device, u8 port_num,
		unsigned int index, void **context)
{
	return 0;
}

int ntrdma_process_mad(struct ib_device *device,
		int process_mad_flags,
		u8 port_num,
		const struct ib_wc *in_wc,
		const struct ib_grh *in_grh,
		const struct ib_mad_hdr *in_mad,
		size_t in_mad_size,
		struct ib_mad_hdr *out_mad,
		size_t *out_mad_size,
		u16 *out_mad_pkey_index)
{
	TRACE("RDMA CM MAD received: class %d\n", in_mad->mgmt_class);

	return IB_MAD_RESULT_SUCCESS;
}
int ntrdma_dev_ib_init(struct ntrdma_dev *dev)
{
	struct ib_device *ibdev = &dev->ibdev;
	int rc;

	strlcpy(ibdev->name, "ntrdma_%d", IB_DEVICE_NAME_MAX);

	ntrdma_set_node_guid(&ibdev->node_guid);

	ibdev->owner			= THIS_MODULE;
	ibdev->node_type		= RDMA_NODE_IB_CA;
	/* TODO: maybe this should be the number of virtual doorbells */
	ibdev->num_comp_vectors		= 1;

	ibdev->dev.parent = ntc_map_dev(dev->ntc, NTB_DEV_ACCESS);

	ibdev->uverbs_abi_ver		= 1;
	ibdev->phys_port_cnt		= 1;
	ibdev->local_dma_lkey = NTRDMA_RESERVED_DMA_LEKY;


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
	ibdev->get_netdev			= ntrdma_get_netdev;
	ibdev->get_port_immutable	= ntrdma_get_port_immutable;
	ibdev->query_pkey			= ntrdma_query_pkey;
	ibdev->query_gid			= ntrdma_query_gid;
	ibdev->create_ah			= ntrdma_create_ah;
	ibdev->destroy_ah			= ntrdma_destroy_ah;
	ibdev->get_dma_mr			= ntrdma_get_dma_mr;

	/* userspace context */
	ibdev->alloc_ucontext		= ntrdma_alloc_ucontext;
	ibdev->dealloc_ucontext		= ntrdma_dealloc_ucontext;

	/* device and port queries */
	ibdev->query_device		= ntrdma_query_device;
	ibdev->query_port		= ntrdma_query_port;

	/* completion queue */
	ibdev->create_cq		= ntrdma_create_cq;
	ibdev->destroy_cq		= ntrdma_destroy_cq;
	ibdev->poll_cq			= ntrdma_poll_cq;
	ibdev->req_notify_cq		= ntrdma_req_notify_cq;

	/* protection domain */
	ibdev->alloc_pd			= ntrdma_alloc_pd;
	ibdev->dealloc_pd		= ntrdma_dealloc_pd;

	/* memory region */
	ibdev->reg_user_mr		= ntrdma_reg_user_mr;
	ibdev->dereg_mr			= ntrdma_dereg_mr;

	/* queue pair */
	ibdev->create_qp		= ntrdma_create_qp;
	ibdev->query_qp			= ntrdma_query_qp;
	ibdev->modify_qp		= ntrdma_modify_qp;
	ibdev->destroy_qp		= ntrdma_destroy_qp;
	ibdev->post_send		= ntrdma_post_send;
	ibdev->post_recv		= ntrdma_post_recv;
	ibdev->get_link_layer	= ntrdma_get_link_layer;
	ibdev->add_gid			= ntrdma_add_gid;
	ibdev->del_gid			= ntrdma_del_gid;
	ibdev->process_mad		= ntrdma_process_mad;

	rc = ib_register_device(ibdev, NULL);
	if (rc)
		goto err_ib;

	return 0;

err_ib:
	return rc;
}

void ntrdma_dev_ib_deinit(struct ntrdma_dev *dev)
{
	pr_info("NTRDMA IB dev deinit\n");
	ib_unregister_device(&dev->ibdev);
}
