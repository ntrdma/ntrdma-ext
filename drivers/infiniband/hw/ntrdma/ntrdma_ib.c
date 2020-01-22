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

#include "ntrdma_ioctl.h"

#include <linux/module.h>
#include <linux/in6.h>
#include <linux/slab.h>
#include <linux/random.h>
#include <linux/file.h>
#include <linux/anon_inodes.h>

#include <rdma/ib_user_verbs.h>
#include <rdma/ib_mad.h>
#include <rdma/ib_verbs.h>
#include <rdma/ib_umem.h>

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

static int ntrdma_qp_file_release(struct inode *inode, struct file *filp);
static long ntrdma_qp_file_ioctl(struct file *filp, unsigned int cmd,
				unsigned long arg);

static int ntrdma_cq_file_release(struct inode *inode, struct file *filp);
static long ntrdma_cq_file_ioctl(struct file *filp, unsigned int cmd,
				unsigned long arg);

static struct kmem_cache *ah_slab;
static struct kmem_cache *cq_slab;
static struct kmem_cache *pd_slab;
static struct kmem_cache *qp_slab;
static struct kmem_cache *ibuctx_slab;

static struct file_operations ntrdma_qp_fops = {
	.owner		= THIS_MODULE,
	.release	= ntrdma_qp_file_release,
	.unlocked_ioctl	= ntrdma_qp_file_ioctl,
	.compat_ioctl	= ntrdma_qp_file_ioctl,
};

static struct file_operations ntrdma_cq_fops = {
	.owner		= THIS_MODULE,
	.release	= ntrdma_cq_file_release,
	.unlocked_ioctl	= ntrdma_cq_file_ioctl,
	.compat_ioctl	= ntrdma_cq_file_ioctl,
};

void ntrdma_free_qp(struct ntrdma_qp *qp)
{
	kmem_cache_free(qp_slab, qp);
}

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
	struct ntrdma_ah *ah = kmem_cache_zalloc(ah_slab, GFP_ATOMIC);

	if (!ah)
		return ERR_PTR(-ENOMEM);

	ah->attr = *ah_attr;

	return &ah->ibah;

}

/* not implemented / not required? */
static int ntrdma_destroy_ah(struct ib_ah *ibah)
{
	struct ntrdma_ah *ah = container_of(ibah, struct ntrdma_ah, ibah);

	kmem_cache_free(ah_slab, ah);

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
	struct ntrdma_create_cq_ext inbuf;
	struct ntrdma_create_cq_resp_ext outbuf;
	struct ntrdma_cq *cq;
	u32 vbell_idx;
	struct file *file;
	int flags;
	int rc;

	if (atomic_inc_return(&dev->cq_num) >= NTRDMA_DEV_MAX_CQ) {
		ntrdma_err(dev, "beyond supported number %d\n",
				NTRDMA_DEV_MAX_CQ);
		rc = -ETOOMANYREFS;
		goto err_cq;
	}

	if (ibattr->comp_vector)
		vbell_idx = ibattr->comp_vector;
	else
		vbell_idx = ntrdma_dev_vbell_next(dev);

	if (unlikely(vbell_idx >= NTRDMA_DEV_VBELL_COUNT)) {
		ntrdma_err(dev, "invalid vbell_idx. idx %d >= %d",
			vbell_idx, NTRDMA_DEV_VBELL_COUNT);
		rc = -EINVAL;
		goto err_cq;
	}

	cq = kmem_cache_alloc_node(cq_slab, GFP_KERNEL, dev->node);
	if (!cq) {
		ntrdma_err(dev, "kmem_cache_alloc_node failed\n");
		rc = -ENOMEM;
		goto err_cq;
	}

	memset(cq, 0, sizeof(*cq));

	ntrdma_cq_init(cq, dev);

	if (ibudata && ibudata->inlen >= sizeof(inbuf)) {
		if (copy_from_user(&inbuf, ibudata->inbuf, sizeof(inbuf))) {
			ntrdma_cq_err(cq, "copy_from_user failed");
			ntrdma_cq_put(cq); /* The initial ref */
			rc = -EFAULT;
			goto err_cq;
		}

		if (!ntrdma_ioctl_if_check_desc(&inbuf.desc)) {
			ntrdma_cq_err(cq, "BAD inbuf.desc:\n%s", inbuf.desc);
			goto bad_ntrdma_ioctl_if;
		}

		if (!inbuf.poll_page_ptr) {
			ntrdma_cq_err(cq, "inbuf.poll_page_ptr is NULL");
			ntrdma_cq_put(cq); /* The initial ref */
			rc = -EINVAL;
			goto err_cq;
		}

		rc = get_user_pages_fast(inbuf.poll_page_ptr, 1, 1,
					&cq->poll_page);
		if (rc < 0) {
			ntrdma_cq_err(cq, "get_user_pages_fast failed: %d", rc);
			ntrdma_cq_put(cq); /* The initial ref */
			goto err_cq;
		}
	}

	if (ibudata && ibudata->outlen >= sizeof(outbuf)) {
		flags = O_RDWR | O_CLOEXEC;

		outbuf.cqfd = get_unused_fd_flags(flags);
		if (outbuf.cqfd < 0) {
			ntrdma_cq_err(cq, "get_unused_fd_flags failed: %d",
				outbuf.cqfd);
			ntrdma_cq_put(cq); /* The initial ref */
			rc = outbuf.cqfd;
			goto err_cq;
		}

		file = anon_inode_getfile("ntrdma_cq", &ntrdma_cq_fops, cq,
					flags);
		if (IS_ERR(file)) {
			ntrdma_cq_err(cq, "anon_inode_getfile failed: %ld",
				PTR_ERR(file));
			ntrdma_cq_put(cq); /* The initial ref */
			put_unused_fd(outbuf.cqfd);
			return (void *)file;
		}
		/*
		 * Ref taken below will be released in ntrdma_cq_file_release().
		 */
		ntrdma_cq_get(cq);

		if (copy_to_user(ibudata->outbuf, &outbuf, sizeof(outbuf))) {
			ntrdma_cq_err(cq, "copy_to_user failed");
			fput(file); /* Close the file. */
			ntrdma_cq_put(cq); /* The initial ref */
			put_unused_fd(outbuf.cqfd);
			rc = -EFAULT;
			goto err_cq;
		}

		fd_install(outbuf.cqfd, file);
		/* outbuf.cqfd now points to file */
	}

 bad_ntrdma_ioctl_if:

	/*
	 * Init vbell before adding to list,
	 * so that dirty vbell doesn't go off from ntrdma_cq_arm_resync().
	 */
	ntrdma_cq_vbell_init(cq, vbell_idx);

	mutex_lock(&dev->res_lock);
	list_add_tail(&cq->obj.dev_entry, &dev->cq_list);
	mutex_unlock(&dev->res_lock);

	ntrdma_debugfs_cq_add(cq);

	ntrdma_dbg(dev,
		"added cq %p (%d/%d) ib cq %p vbell idx %d c\n",
		cq, atomic_read(&dev->cq_num), NTRDMA_DEV_MAX_CQ,
		&cq->ibcq, vbell_idx);

	cq->ibcq_valid = true;

	return &cq->ibcq;

err_cq:
	atomic_dec(&dev->cq_num);
	ntrdma_err(dev, "failed, returning err %d\n", rc);
	return ERR_PTR(rc);
}

static void ntrdma_cq_release(struct kref *kref)
{
	struct ntrdma_obj *obj = container_of(kref, struct ntrdma_obj, kref);
	struct ntrdma_cq *cq = container_of(obj, struct ntrdma_cq, obj);
	struct ntrdma_dev *dev = ntrdma_cq_dev(cq);

	if (cq->poll_page) {
		put_page(cq->poll_page);
		cq->poll_page = NULL;
	}

	ntrdma_debugfs_cq_del(cq);
	kmem_cache_free(cq_slab, cq);
	atomic_dec(&dev->cq_num);
}

static int ntrdma_destroy_cq(struct ib_cq *ibcq)
{
	struct ntrdma_cq *cq = ntrdma_ib_cq(ibcq);
	struct ntrdma_dev *dev = ntrdma_cq_dev(cq);

	spin_lock_bh(&cq->arm_lock);
	cq->ibcq_valid = false;
	spin_unlock_bh(&cq->arm_lock);

	/*
	 * Remove from list before killing vbell,
	 * so that killed vbell does not go off from ntrdma_cq_arm_resync().
	 */
	mutex_lock(&dev->res_lock);
	list_del(&cq->obj.dev_entry);
	mutex_unlock(&dev->res_lock);

	ntrdma_cq_vbell_kill(cq);

	ntrdma_cq_put(cq);

	return 0;
}

void ntrdma_cq_put(struct ntrdma_cq *cq)
{
	ntrdma_obj_put(&cq->obj, ntrdma_cq_release);
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
	case NTRDMA_WC_ERR_LOC_PORT:
		return IB_WC_LOC_PROT_ERR;
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
	case IB_WR_SEND:
	case NTRDMA_WR_SEND_INV:
	case IB_WR_SEND_WITH_IMM:
		return IB_WC_SEND;
	case IB_WR_RDMA_WRITE_WITH_IMM:
	case IB_WR_RDMA_WRITE:
		return IB_WC_RDMA_WRITE;
	case IB_WR_RDMA_READ:
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

static inline void ntrdma_qp_start_measure(struct ntrdma_qp *qp,
		struct ntrdma_send_wqe *wqe, u32 pos)
{
	if (!ntrdma_wr_code_push_data(wqe->op_code) ||
						(wqe->flags & IB_SEND_SIGNALED))
		ntrdma_qp_set_stats(qp, pos);
}


static inline cycles_t ntrdma_qp_stop_measure(struct ntrdma_qp *qp, u32 pos)
{
	return ntrdma_qp_get_diff_cycles(qp, pos);
}

static void ntrdma_ib_wc_from_cqe(struct ib_wc *ibwc,
				struct ntrdma_qp *qp,
				const struct ntrdma_cqe *cqe)
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
}
#define SHIFT_SAVE_BITS 10
static int ntrdma_poll_cq(struct ib_cq *ibcq,
			  int howmany,
			  struct ib_wc *ibwc)
{
	struct ntrdma_cq *cq = ntrdma_ib_cq(ibcq);
	struct ntrdma_qp *qp;
	struct ntrdma_cqe cqe;
	u32 pos, end, base;
	int count_s = 0, count_ns = 0, count_gsi = 0, rc = 0;
	int count = 0;

	/* lock for completions */
	mutex_lock(&cq->poll_lock);

	this_cpu_inc(dev_cnt.poll_cq_count);

	while (count < howmany) {
		/* get the next completing range in the next qp ring */
		rc = ntrdma_cq_cmpl_get(cq, &qp, &pos, &end, &base);
		if (rc)
			break;

		for (;;) {
			/* current entry in the ring, or aborted */
			ntrdma_cq_cmpl_cqe(cq, &cqe, pos);

			/* completion should be generated for post send with
			 * IB_SEND_SIGNALED flag
			 */
			if (!ntrdma_wr_code_push_data(cqe.op_code) ||
					(cqe.flags & IB_SEND_SIGNALED)) {
				/* transform the entry into the work completion */
				ntrdma_ib_wc_from_cqe(&ibwc[count], qp, &cqe);
				TRACE_DATA(
						"OPCODE %d(%d): wrid %llu QP %d status %d pos %u end %u flags %d\n",
						ibwc[count].opcode,
						cqe.op_code,
						ibwc[count].wr_id,
						qp->res.key,
						ibwc[count].status,
						pos,
						end,
						cqe.flags);

				/* SHIFT 10 >> for saving bits, postponing wrap-around... */
				this_cpu_add(dev_cnt.accum_latency,
				(u64)ntrdma_qp_stop_measure(qp, pos) >> SHIFT_SAVE_BITS);
				if (qp->qp_type != IB_QPT_GSI) {
					if (cqe.flags & IB_SEND_SIGNALED)
						++count_s;
					else
						++count_ns;
				} else
					++count_gsi;
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
	mutex_unlock(&cq->poll_lock);

	if (count) {
		this_cpu_add(dev_cnt.cqes_polled_s, count_s);
		this_cpu_add(dev_cnt.cqes_polled_ns, count_ns);
		return (count);
	}
	if ((rc == -EAGAIN) || (rc > 0))
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

static void ntrdma_wc_from_cqe(struct ntrdma_ibv_wc *wc,
			struct ntrdma_qp *qp,
			const struct ntrdma_cqe *cqe)
{
	u16 op_code = cqe->op_code;
	u16 op_status = cqe->op_status;

	wc->wr_id = cqe->ulp_handle;

	wc->status = ntrdma_ib_wc_status_from_cqe(op_status);
	wc->opcode = ntrdma_ib_wc_opcode_from_cqe(op_code);

	wc->qp_num = qp->res.key; /* == qp->ibqp.qp_num when valid */
	wc->vendor_err = op_status;
	wc->byte_len = cqe->rdma_len;
	wc->imm_data = cqe->imm_data;
	wc->src_qp = qp->rqp_key;
	wc->wc_flags = ntrdma_ib_wc_flags_from_cqe(op_code);
	wc->pkey_index = 0;
	wc->slid = 0;
	wc->sl = 0;
	wc->dlid_path_bits = 0;
}

static inline int ntrdma_cq_process_poll_ioctl(struct ntrdma_cq *cq)
{
	struct ntrdma_qp *qp;
	struct ntrdma_cqe cqe;
	u32 pos, end, base;
	int count_s = 0, count_ns = 0, rc = 0;
	int count = 0;
	struct ntrdma_poll_hdr *hdr;
	struct ntrdma_ibv_wc *wc;
	u32 howmany;

	if (!cq->poll_page)
		return -EINVAL;

	hdr = page_address(cq->poll_page);
	howmany = READ_ONCE(hdr->wc_counter);

	if (howmany > (PAGE_SIZE - sizeof(*hdr)) / sizeof(*wc))
		return -EINVAL;

	wc = (void *)(hdr + 1);

	/* lock for completions */
	mutex_lock(&cq->poll_lock);
	this_cpu_inc(dev_cnt.poll_cq_count_ioctl);

	while (count < howmany) {
		/* get the next completing range in the next qp ring */
		rc = ntrdma_cq_cmpl_get(cq, &qp, &pos, &end, &base);
		if (rc < 0)
			break;

		for (;;) {
			/* current entry in the ring, or aborted */
			ntrdma_cq_cmpl_cqe(cq, &cqe, pos);

			/*
			 * completions should be generated for post send
			 * with IB_SEND_SIGNALED flag
			 */
			if (!ntrdma_wr_code_push_data(cqe.op_code) ||
					(cqe.flags & IB_SEND_SIGNALED)) {
				/* transform the entry into work completion */
				ntrdma_wc_from_cqe(&wc[count], qp, &cqe);
				TRACE_DATA(
					"OPCODE %d(%d): wrid %llu QP %d status %d pos %u end %u flags %d\n",
					wc[count].opcode,
					cqe.op_code,
					wc[count].wr_id,
					qp->res.key,
					wc[count].status,
					pos,
					end,
					cqe.flags);
				if (qp->qp_type != IB_QPT_GSI) {
					if (cqe.flags & IB_SEND_SIGNALED)
						++count_s;
					else
						++count_ns;
				}
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
	mutex_unlock(&cq->poll_lock);

	hdr->wc_counter = count;

	if (count) {
		this_cpu_add(dev_cnt.cqes_polled_ioctl_s, count_s);
		this_cpu_add(dev_cnt.cqes_polled_ioctl_ns, count_ns);
		return 0;
	}
	if ((rc == -EAGAIN) || (rc > 0))
		return 0;
	return rc;
}

static long ntrdma_cq_file_ioctl(struct file *filp, unsigned int cmd,
			unsigned long arg)
{
	struct ntrdma_cq *cq = filp->private_data;

	switch (cmd) {
	case NTRDMA_IOCTL_POLL:
		return ntrdma_cq_process_poll_ioctl(cq);
	default:
		return -EINVAL;
	}
}

static int ntrdma_cq_file_release(struct inode *inode, struct file *filp)
{
	struct ntrdma_cq *cq = filp->private_data;

	if (cq->poll_page) {
		put_page(cq->poll_page);
		cq->poll_page = NULL;
	}

	ntrdma_cq_put(cq);

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
		ntrdma_err(dev, "beyond supported number %d\n",
				NTRDMA_DEV_MAX_PD);
		rc = -ETOOMANYREFS;
		goto err_pd;
	}

	pd = kmem_cache_alloc_node(pd_slab, GFP_KERNEL, dev->node);
	if (!pd) {
		rc = -ENOMEM;
		ntrdma_err(dev, "kmem_cache_alloc_node failed\n");
		goto err_pd;
	}

	ntrdma_vdbg(dev, "allocated pd %p\n", pd);

	ntrdma_pd_init(pd, dev, dev->pd_next_key++);

	ntrdma_vdbg(dev, "initialized pd %p\n", pd);

	mutex_lock(&dev->res_lock);
	list_add_tail(&pd->obj.dev_entry, &dev->pd_list);
	mutex_unlock(&dev->res_lock);

	ntrdma_dbg(dev, "added pd key=%d", pd->key);

	return &pd->ibpd;

err_pd:
	atomic_dec(&dev->pd_num);
	ntrdma_err(dev, "failed, returning err %d\n", rc);
	return ERR_PTR(rc);
}
static void ntrdma_pd_release(struct kref *kref)
{
	struct ntrdma_obj *obj = container_of(kref, struct ntrdma_obj, kref);
	struct ntrdma_pd *pd = container_of(obj, struct ntrdma_pd, obj);
	struct ntrdma_dev *dev = ntrdma_pd_dev(pd);

	kmem_cache_free(pd_slab, pd);
	atomic_dec(&dev->pd_num);
}

static int ntrdma_dealloc_pd(struct ib_pd *ibpd)
{
	struct ntrdma_pd *pd = ntrdma_ib_pd(ibpd);
	struct ntrdma_dev *dev = ntrdma_pd_dev(pd);

	mutex_lock(&dev->res_lock);
	list_del(&pd->obj.dev_entry);
	mutex_unlock(&dev->res_lock);

	ntrdma_pd_put(pd);

	return 0;
}

void ntrdma_pd_put(struct ntrdma_pd *pd)
{
	ntrdma_obj_put(&pd->obj, ntrdma_pd_release);
}

static struct ib_qp *ntrdma_create_qp(struct ib_pd *ibpd,
				      struct ib_qp_init_attr *ibqp_attr,
				      struct ib_udata *ibudata)
{
	struct ntrdma_pd *pd = ntrdma_ib_pd(ibpd);
	struct ntrdma_dev *dev = ntrdma_pd_dev(pd);
	struct ntrdma_cq *recv_cq = ntrdma_ib_cq(ibqp_attr->recv_cq);
	struct ntrdma_cq *send_cq = ntrdma_ib_cq(ibqp_attr->send_cq);
	struct ntrdma_create_qp_ext inbuf;
	struct ntrdma_create_qp_resp_ext outbuf;
	struct ntrdma_qp *qp;
	struct ntrdma_qp_init_attr qp_attr;
	struct file *file;
	struct ntrdma_qp_cmd_cb qpcb;
	int flags;
	int rc;

	if (atomic_inc_return(&dev->qp_num) >= NTRDMA_DEV_MAX_QP) {
		ntrdma_err(dev, "beyond supported number %d\n",
				NTRDMA_DEV_MAX_QP);
		rc = -ETOOMANYREFS;
		goto err_qp;
	}

	qp = kmem_cache_alloc_node(qp_slab, GFP_KERNEL, dev->node);
	if (!qp) {
		rc = -ENOMEM;
		ntrdma_err(dev, "kmem_cache_alloc_node failed\n");
		goto err_qp;
	}

	memset(qp, 0, sizeof(*qp));

	qp->qp_type = ibqp_attr->qp_type;
	init_completion(&qp->enable_qpcb.cb.cmds_done);

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

	if (ibqp_attr->cap.max_inline_data > NTRDMA_DEV_MAX_INLINE_DATA)
		qp_attr.send_wqe_inline_cap = NTRDMA_DEV_MAX_INLINE_DATA;
	else
		qp_attr.send_wqe_inline_cap = ibqp_attr->cap.max_inline_data;

	ntrdma_dbg(dev, "max inline data was set to %d\n",
			qp_attr.send_wqe_inline_cap);
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
	if (rc) {
		ntrdma_err(dev, "ntrdma_qp_init failed rc %d\n", rc);
		goto err_init;
	}

	memset(&qpcb, 0, sizeof(qpcb));
	init_completion(&qpcb.cb.cmds_done);
	rc = ntrdma_res_add(&qp->res, &qpcb.cb, &dev->qp_list, &dev->qp_vec);
	if (rc) {
		ntrdma_err(dev, "ntrdma_qp_add failed %d\n", rc);
		goto err_add;
	}

	qp->ibqp.qp_num = qp->res.key;
	atomic_set(&qp->state, IB_QPS_RESET);

	ntrdma_info(dev, "added qp%d type %d (%d/%d)\n",
		qp->res.key, ibqp_attr->qp_type,
		atomic_read(&dev->qp_num), NTRDMA_DEV_MAX_QP);

	if (ibudata && ibudata->inlen >= sizeof(inbuf)) {
		if (copy_from_user(&inbuf, ibudata->inbuf, sizeof(inbuf))) {
			ntrdma_qp_err(qp, "copy_from_user failed");
			ntrdma_qp_put(qp); /* The initial ref */
			return ERR_PTR(-EFAULT);
		}

		if (!ntrdma_ioctl_if_check_desc(&inbuf.desc)) {
			ntrdma_qp_err(qp, "BAD inbuf.desc:\n%s", inbuf.desc);
			goto bad_ntrdma_ioctl_if;
		}

		if (!inbuf.send_page_ptr) {
			ntrdma_qp_err(qp, "inbuf.send_page_ptr is NULL");
			ntrdma_qp_put(qp); /* The initial ref */
			return ERR_PTR(-EINVAL);
		}

		rc = get_user_pages_fast(inbuf.send_page_ptr, 1, 1,
					&qp->send_page);
		if (rc < 0) {
			ntrdma_qp_err(qp, "get_user_pages_fast failed: %d", rc);
			ntrdma_qp_put(qp); /* The initial ref */
			return ERR_PTR(rc);
		}
	}
	ntrdma_info(dev, "added qp%d type %d (%d/%d)\n",
			qp->res.key, ibqp_attr->qp_type,
			atomic_read(&dev->qp_num), NTRDMA_DEV_MAX_QP);

	if (ibudata && ibudata->outlen >= sizeof(outbuf)) {
		flags = O_RDWR | O_CLOEXEC;

		outbuf.qpfd = get_unused_fd_flags(flags);
		if (outbuf.qpfd < 0) {
			ntrdma_qp_err(qp, "get_unused_fd_flags failed: %d",
				outbuf.qpfd);
			ntrdma_qp_put(qp); /* The initial ref */
			return ERR_PTR(outbuf.qpfd);
		}

		file = anon_inode_getfile("ntrdma_qp", &ntrdma_qp_fops, qp,
					flags);
		if (IS_ERR(file)) {
			ntrdma_qp_err(qp, "anon_inode_getfile failed: %ld",
				PTR_ERR(file));
			ntrdma_qp_put(qp); /* The initial ref */
			put_unused_fd(outbuf.qpfd);
			return (void *)file;
		}
		/*
		 * Ref taken below will be released in ntrdma_qp_file_release().
		 */
		ntrdma_qp_get(qp);

		if (copy_to_user(ibudata->outbuf, &outbuf, sizeof(outbuf))) {
			ntrdma_qp_err(qp, "copy_to_user failed");
			fput(file); /* Close the file. */
			ntrdma_qp_put(qp); /* The initial ref */
			put_unused_fd(outbuf.qpfd);
			return ERR_PTR(-EFAULT);
		}

		fd_install(outbuf.qpfd, file);
		/* outbuf.qpfd now points to file */
	}

 bad_ntrdma_ioctl_if:
	ntrdma_debugfs_qp_add(qp);

	ntrdma_qp_dbg(qp, "added qp%d type %d",
			qp->res.key, ibqp_attr->qp_type);

	return &qp->ibqp;

err_add:
	ntrdma_qp_deinit(qp, dev);
err_init:
	kmem_cache_free(qp_slab, qp);
err_qp:
	atomic_dec(&dev->qp_num);
	ntrdma_err(dev, "failed, returning err %d\n", rc);
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
	if (!ibqp_mask)
		return 0;

	if (!(ibqp_mask & (IB_QP_STATE | IB_QP_DEST_QPN))) {
		ntrdma_err(dev, "Not supported ibqp mask %d\n", ibqp_mask);
		return -EINVAL;
	}

	if (ibqp_mask & IB_QP_STATE)
		ibqp_attr->qp_state = atomic_read(&qp->state);

	if (ibqp_mask & IB_QP_DEST_QPN)
		ibqp_attr->dest_qp_num = qp->rqp_key;

	ibqp_attr->cap.max_inline_data = qp->send_wqe_inline_cap;

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
	struct ntrdma_qp *qp = ntrdma_ib_qp(ibqp);
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);
	int rc, cur_state, new_state;
	bool success = true;

	ntrdma_modify_qp_debug(ibqp, ibqp_attr, ibqp_mask, ibudata);
	ntrdma_res_lock(&qp->res);
	do {
		cur_state = atomic_read(&qp->state);
		new_state = ibqp_mask & IB_QP_STATE ?
				ibqp_attr->qp_state : cur_state;

		ntrdma_info(dev, "QP %d state %d (%d) -> %d (ibqp_mask 0x%x)\n",
				qp->res.key, cur_state,
				atomic_read(&qp->state), new_state, ibqp_mask);

		if ((ibqp_mask & IB_QP_CUR_STATE) &&
				(ibqp_attr->cur_qp_state !=
						atomic_read(&qp->state))) {
			ntrdma_err(dev,
					"%s: unexpected current state %d %d\n",
					__func__, cur_state,
					atomic_read(&qp->state));
			rc = -EINVAL;
			goto unlock_exit;
		}

		if (ibqp_mask & ~NTRDMA_IBQP_MASK_SUPPORTED) {
			ntrdma_err(dev, "%s: unsupported QP mask %x\n",
					__func__, ibqp_mask &
					~NTRDMA_IBQP_MASK_SUPPORTED);
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

		if ((ibqp->qp_type != IB_QPT_GSI) && !ntc_is_link_up(dev->ntc)
				&& is_state_send_ready(new_state)) {
			ntrdma_err(dev,
					"link is not up, cannot move to ready qp %d\n",
					qp->res.key);
			rc = -EAGAIN;
			goto unlock_exit;
		}

		if (ibqp_mask & IB_QP_STATE) {
			ntrdma_vdbg(dev, "qp %p state %d -> %d\n",
					qp, cur_state, new_state);

			success =
				atomic_cmpxchg(&qp->state, cur_state,
					new_state) == cur_state;
			TRACE(
				"try qp %d move from state %d to state %d (success %d), s_a %d, s_aing %d, r_a %d, r_aing %d\n",
				qp->res.key, cur_state, new_state, success,
				qp->send_abort, qp->send_aborting,
				qp->recv_abort, qp->recv_aborting);
			if (success) {
				if (!is_state_error(new_state)) {
					qp->send_aborting = false;
					qp->send_abort = false;
				}
				if (new_state != IB_QPS_ERR) {
					qp->recv_aborting = false;
					qp->recv_abort = false;
				}
			}
		}
	} while (!success);

	if (ibqp_mask & IB_QP_ACCESS_FLAGS)
		qp->access = ibqp_attr->qp_access_flags;

	if (ibqp_mask & IB_QP_DEST_QPN)
		qp->rqp_key = ibqp_attr->dest_qp_num;

	/* For GSI and MSI QPs we used reserved key 1 and 2 */
	if (ibqp->qp_type == IB_QPT_GSI || ibqp->qp_type == IB_QPT_SMI)
		qp->rqp_key = ibqp->qp_type;

	rc = ntrdma_qp_modify(qp);

	if (unlikely(rc))
		ntrdma_err(dev, "ntrdma_qp_modify: failed rc = %d", rc);

unlock_exit:
	ntrdma_res_unlock(&qp->res);
	return rc;
}

static int ntrdma_destroy_qp(struct ib_qp *ibqp)
{
	struct ntrdma_qp *qp = ntrdma_ib_qp(ibqp);
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);
	struct ntrdma_qp_cmd_cb qpcb;

	TRACE("qp %p (res key %d)\n", qp, qp ? qp->res.key : -1);

	if (unlikely(qp->send_cmpl != qp->send_post)) {
		ntrdma_info(dev,
				"Destroy QP %p (%d) while send cmpl %d send post %d send prod %d send cap %d\n",
				qp, qp->res.key, qp->send_cmpl,
				qp->send_post, qp->send_prod, qp->send_cap);
	}

	ntrdma_debugfs_qp_del(qp);

	memset(&qpcb, 0, sizeof(qpcb));
	init_completion(&qpcb.cb.cmds_done);
	ntrdma_res_del(&qp->res, &qpcb.cb, &dev->qp_vec);

	ntc_dma_flush(qp->dma_chan);

	ntrdma_qp_put(qp);

	return 0;
}

#define NUM_SUPPORTED_INLINE_SGE 1
static inline int ntrdma_ib_send_to_inline_wqe(struct ntrdma_qp *qp,
					struct ntrdma_send_wqe *wqe,
					struct ib_sge *sg_list)
{
	bool from_user = (qp->qp_type != IB_QPT_GSI);
	u64 tail_size;
	u64 entry_size;
	u64 available_size;
	int i;

	for ((i = 0), (tail_size = 0), (available_size = wqe->inline_len);
	     tail_size < available_size; (i++), (tail_size += entry_size)) {
		entry_size = sg_list[i].length;
		if (!entry_size)
			continue;
		if (from_user) {
			if (copy_from_user(snd_inline_data(wqe) + tail_size,
						(void __user *)sg_list[i].addr,
						entry_size)) {
				ntrdma_qp_err(qp, "copy from user failed");
				return -EFAULT;
			}
		} else
			memcpy(snd_inline_data(wqe) + tail_size,
				phys_to_virt(sg_list[i].addr), entry_size);
	}

	this_cpu_add(dev_cnt.post_send_bytes, available_size);

	TRACE_DATA(
		"OPCODE %d: flags %x addr %llx QP %d inline len %lld wrid %llu",
		wqe->op_code, wqe->flags, wqe->rdma_sge.addr, qp->res.key,
		available_size, wqe->ulp_handle);

	if (qp->qp_type != IB_QPT_GSI) {
		if (wqe->flags & IB_SEND_SIGNALED)
			this_cpu_inc(dev_cnt.post_send_wqes_signalled);
		this_cpu_inc(dev_cnt.post_send_wqes);
	}

	return 0;
}

static inline
bool ntrdma_send_is_non_signaled_write(const struct ntrdma_send_wqe *wqe)
{
	return (wqe->op_code == IB_WR_RDMA_WRITE) &&
		!(wqe->flags & IB_SEND_SIGNALED);
}

static inline bool ntrdma_send_opcode_is_rdma(int opcode)
{
	switch (opcode) {
	case IB_WR_RDMA_WRITE_WITH_IMM:
	case IB_WR_RDMA_WRITE:
	case IB_WR_RDMA_READ:
		return true;
	default:
		return false;
	}
}

static inline int ntrdma_ib_send_to_wqe(struct ntrdma_qp *qp,
					struct ntrdma_send_wqe *wqe,
					struct ib_send_wr *ibwr)
{
	bool is_rdma = ntrdma_send_opcode_is_rdma(ibwr->opcode);
	bool from_user;
	u64 tail_size;
	u64 entry_size;
	u64 available_size;
	int i;

	wqe->ulp_handle = ibwr->wr_id;
	wqe->op_code = ibwr->opcode;
	wqe->op_status = 0;
	wqe->recv_key = ~(u32)0;
	wqe->rdma_sge.addr = (is_rdma ? rdma_wr(ibwr)->remote_addr : 0);
	wqe->rdma_sge.lkey = (is_rdma ? rdma_wr(ibwr)->rkey : 0);
	wqe->rdma_sge.length = 0;
	wqe->imm_data = ibwr->ex.imm_data;
	wqe->flags = ibwr->send_flags;

	if (is_rdma) {
		from_user = (qp->qp_type != IB_QPT_GSI);
		if (unlikely(from_user !=
				(wqe->rdma_sge.lkey !=
					NTRDMA_RESERVED_DMA_LEKY))) {
			ntrdma_qp_err(qp, "from_user %d but key reserved %d",
				from_user,
				(wqe->rdma_sge.lkey ==
					NTRDMA_RESERVED_DMA_LEKY));
			return -EINVAL;
		}
	}

	if (ntrdma_send_wqe_is_inline(wqe)) {
		available_size = qp->send_wqe_inline_cap;
		for ((i = 0), (tail_size = 0); i < ibwr->num_sge; i++) {
			entry_size = ibwr->sg_list[i].length;
			if (!entry_size)
				continue;
			if (tail_size + entry_size < tail_size)
				return -ENOMEM;
			tail_size += entry_size;
			if (available_size < tail_size)
				return -ENOMEM;
		}
		wqe->inline_len = tail_size;
	} else
		wqe->sg_count = ibwr->num_sge;

	return 0;
}

static inline int ntrdma_ib_send_to_wqe_sgl(struct ntrdma_qp *qp,
					struct ntrdma_send_wqe *wqe,
					struct ib_sge *sg_list)
{
	bool from_user = (qp->qp_type != IB_QPT_GSI);
	int sg_count = wqe->sg_count;
	struct ib_sge *sge;
	struct ib_sge *wqe_snd_sge;
	int i;

	for (i = 0; i < sg_count; ++i) {
		sge = &sg_list[i];
		wqe_snd_sge = snd_sg_list(i, wqe);
		*wqe_snd_sge = *sge;

		if (unlikely(from_user != !ntrdma_ib_sge_reserved(sge))) {
			ntrdma_qp_err(qp, "from_user %d but key reserved %d",
				from_user, ntrdma_ib_sge_reserved(sge));
			return -EINVAL;
		}

		this_cpu_add(dev_cnt.post_send_bytes, sge->length);
	}

	TRACE_DATA(
		"OPCODE %d: flags %x, addr %llx QP %d num sges %d wrid %llu",
		wqe->op_code, wqe->flags, wqe->rdma_sge.addr, qp->res.key,
		sg_count, wqe->ulp_handle);

	if (qp->qp_type != IB_QPT_GSI) {
		if (wqe->flags & IB_SEND_SIGNALED)
			this_cpu_add(dev_cnt.post_send_wqes_signalled, sg_count);
		this_cpu_add(dev_cnt.post_send_wqes, sg_count);
	}

	return 0;
}

/*
 * ntrdma_post_send_wqe():
 *
 * On error, return the (negative) error value.
 * If the wqe was sent, return 1.
 * Otherwise, return 0.
 */
static inline int ntrdma_post_send_wqe(struct ntrdma_qp *qp,
				struct ntrdma_send_wqe *wqe,
				struct ib_sge *sg_list)
{
	int rc;

	if (ntrdma_send_wqe_is_inline(wqe))
		rc = ntrdma_ib_send_to_inline_wqe(qp, wqe, sg_list);
	else
		rc = ntrdma_ib_send_to_wqe_sgl(qp, wqe, sg_list);

	if (rc < 0)
		return rc;

	if ((wqe->flags & IB_SEND_SIGNALED) ||
		(wqe->op_code != IB_WR_RDMA_WRITE))
		return 0;

	rc = ntrdma_qp_rdma_write(qp, wqe);
	if (unlikely(rc < 0))
		return rc;

	return 1;
}

static inline void ntrdma_qp_additional_work(struct ntrdma_qp *qp,
					bool has_deferred_work,
					bool had_immediate_work) {
	bool reschedule;

	if (has_deferred_work)
		do {
			reschedule = ntrdma_qp_send_work(qp);
			ntc_req_submit(qp->dma_chan);
		} while (reschedule);
	else if (had_immediate_work)
		ntc_req_submit(qp->dma_chan);
}

static inline int ntrdma_post_send_locked(struct ntrdma_qp *qp,
					struct ib_send_wr *ibwr,
					struct ib_send_wr **bad,
					bool *had_immediate_work,
					bool *has_deferred_work)
{
	struct ntrdma_send_wqe *wqe;
	u32 pos, end, base;
	struct ib_sge *sg_list;
	int rc = 0;

	while (ibwr) {
		/* get the next posting range in the ring */
		if (!ntrdma_qp_send_post_get(qp, &pos, &end, &base)) {
			rc = -EAGAIN;
			break;
		}

		wqe = NULL;
		for (;;) {
			/* current entry in the ring */
			if (!wqe)
				wqe = ntrdma_qp_send_wqe(qp, pos);

			if (unlikely((unsigned)ibwr->num_sge >
					(unsigned)qp->send_wqe_sg_cap)) {
				ntrdma_qp_dbg(qp, "too many sges %d", ibwr->num_sge);
				rc = -EINVAL;
				break;
			}

			if (unlikely((unsigned)ibwr->opcode > (unsigned)
					NTRDMA_SEND_WR_MAX_SUPPORTED)) {
				rc = -EINVAL;
				break;
			}

			sg_list = ibwr->sg_list;
			rc = ntrdma_ib_send_to_wqe(qp, wqe, ibwr);
			if (rc < 0)
				break;

			ntrdma_qp_start_measure(qp, wqe, pos);

			rc = ntrdma_post_send_wqe(qp, wqe, sg_list);
			if (rc < 0)
				break;

			if (rc > 0) {
				*had_immediate_work = true;
				/* pos and wqe can be reused. */
			} else {
				wqe = NULL;
				++pos;
				*has_deferred_work = true;
			}


			ibwr = ibwr->next;

			/* quit after the last request or end of range */
			if (!ibwr || pos == end)
				break;
		}
		/* update the next posting range */
		ntrdma_qp_send_post_put(qp, pos, base);

		if (rc < 0)
			break;
	}

	*bad = ibwr;

	if (rc > 0)
		rc = 0;

	return rc;
}

static int ntrdma_post_send(struct ib_qp *ibqp,
			struct ib_send_wr *ibwr,
			struct ib_send_wr **bad)
{
	DEFINE_NTC_FUNC_PERF_TRACKER(perf, 1 << 20);
	struct ntrdma_qp *qp = ntrdma_ib_qp(ibqp);
	bool had_immediate_work = false;
	bool has_deferred_work = false;
	int rc;

	ntrdma_qp_send_post_lock(qp);

	if (likely(ntrdma_qp_is_send_ready(qp))) {
		rc = ntrdma_post_send_locked(qp, ibwr, bad,
					&had_immediate_work,
					&has_deferred_work);
		ntrdma_qp_additional_work(qp, has_deferred_work,
					had_immediate_work);
	} else {
		ntrdma_qp_err(qp, "qp %d state %d", qp->res.key,
			atomic_read(&qp->state));
		rc = -EINVAL;
	}

	ntrdma_qp_send_post_unlock(qp);

	NTRDMA_PERF_MEASURE(perf);

	return rc;
}

static int ntrdma_ib_recv_to_wqe(struct ntrdma_dev *dev,
				struct ntrdma_recv_wqe *wqe,
				struct ib_recv_wr *ibwr,
				int sg_cap)
{
	int i;
	struct ntrdma_wr_rcv_sge *rcv_sge;
	struct ntrdma_wr_rcv_sge_shadow *shadow;
	u32 key;
	struct ib_sge *sg_list;
	struct ntrdma_mr *mr;
	int rc;

	wqe->ulp_handle = ibwr->wr_id;

	wqe->op_code = NTRDMA_WR_RECV;

	if (ibwr->num_sge > sg_cap)
		return -EINVAL;

	wqe->sg_count = ibwr->num_sge;
	for (i = 0; i < ibwr->num_sge; ++i) {
		rcv_sge = &wqe->rcv_sg_list[i];
		sg_list = &ibwr->sg_list[i];

		key = sg_list->lkey;

		if (!ntrdma_ib_sge_reserved(sg_list)) {
			mr = ntrdma_dev_mr_look(dev, key);
			if (mr) {
				if (mr->access & IB_ACCESS_REMOTE_WRITE) {
					rcv_sge->shadow = NULL;
					rcv_sge->sge = *sg_list;
					/* FIXME: dma callback for put mr */
					ntrdma_mr_put(mr);
					continue;
				} else
					ntrdma_mr_put(mr);
			}
		}

		rcv_sge->shadow =
			shadow = ntrdma_zalloc_sge_shadow(GFP_KERNEL, dev);
		if (!shadow) {
			rc = -ENOMEM;
			ntrdma_err(dev, "FAILED to alloc shadow");
			goto err;
		}
		shadow->local_key = key;

		if (!ntrdma_ib_sge_reserved(sg_list))
			shadow->local_addr = sg_list->addr;
		else
			ntc_local_buf_map_dma(&shadow->rcv_dma_buf,
					sg_list->length,
					sg_list->addr);

		rc = ntc_export_buf_alloc(&shadow->exp_buf,
					dev->ntc,
					sg_list->length,
					GFP_KERNEL);
		if (rc < 0) {
			ntrdma_err(dev, "FAILED %d", -rc);
			goto err;
		}
		ntc_export_buf_make_desc(&rcv_sge->exp_buf_desc,
					&shadow->exp_buf);
		ntrdma_info(dev,
			"Allocating rcv %s buffer size %#x @DMA %#llx",
			ntrdma_ib_sge_reserved(sg_list) ?
			"DMA" : "MR", sg_list->length,
			shadow->exp_buf.dma_addr);
	}

	return 0;

 err:
	wqe->sg_count = i;
	ntrdma_recv_wqe_cleanup(wqe);

	return rc;
}

static int ntrdma_post_recv(struct ib_qp *ibqp,
			    struct ib_recv_wr *ibwr,
			    struct ib_recv_wr **bad)
{
	DEFINE_NTC_FUNC_PERF_TRACKER(perf, 1 << 10);
	struct ntrdma_qp *qp = ntrdma_ib_qp(ibqp);
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);
	struct ntrdma_recv_wqe *wqe;
	u32 pos, end, base;
	int rc = 0;

	/* verify the qp state and lock for posting recvs */
	mutex_lock(&qp->recv_post_lock);
	if (!is_state_out_of_reset(atomic_read(&qp->state))) {
		ntrdma_qp_err(qp, "qp %d state %d", qp->res.key,
			atomic_read(&qp->state));
		rc = -EINVAL;
		goto out;
	}

	while (ibwr) {
		/* get the next posting range in the ring */
		if (!ntrdma_qp_recv_post_get(qp, &pos, &end, &base)) {
			/* posting too many oustanding requests */
			ntrdma_qp_dbg(qp, "posting too many recvs");
			rc = -EINVAL;
			break;
		}

		for (;;) {
			/* current entry in the ring */
			wqe = ntrdma_qp_recv_wqe(qp, pos);

			/* transform work request to queue entry */
			rc = ntrdma_ib_recv_to_wqe(dev, wqe, ibwr,
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
	if (dev->res_enable) /* WHY?? */
		ntrdma_qp_recv_work(qp);

out:
	mutex_unlock(&qp->recv_post_lock);

	*bad = ibwr;

	NTRDMA_PERF_MEASURE(perf);

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
	struct ib_umem *ib_umem;
	unsigned long dma_addr = 0;
	unsigned long dma_len;
	struct ntrdma_mr_cmd_cb mrcb;
	int rc, i, count;

	ntrdma_info(dev, "called user addr %llx len %lld: (%d/%d)",
		virt_addr, length, atomic_read(&dev->mr_num),
		NTRDMA_DEV_MAX_MR);


	if (length > IB_MR_LIMIT_BYTES) {
		ntrdma_err(dev, "reg_user_mr with not supported length %lld\n",
				length);
		rc = -EINVAL;
		goto err_len;
	}

	if (atomic_inc_return(&dev->mr_num) >= NTRDMA_DEV_MAX_MR) {
		ntrdma_err(dev, "reg_user_mr beyond supported number %d\n",
				NTRDMA_DEV_MAX_MR);
		rc = -ETOOMANYREFS;
		goto err_umem;
	}

	if (!is_canonical(virt_addr)) {
		rc = -EINVAL;
		ntrdma_err(dev, "reg_user_mr with non canonical addr (corrupted?)\n");
		goto err_umem;
	}

	rc = ntc_get_io_pfn_segment(current->mm, start, length,
				mr_access_flags & IB_ACCESS_REMOTE_WRITE,
				&dma_addr, &dma_len);
	if ((rc >= 0) && (dma_len >= length)) {
		ib_umem = NULL;
		count = 1;
		ntrdma_info(dev, "MAPPED ADDR %#llx TO DMA %#lx LEN %#llx",
			start, dma_addr, length);
	} else {
		ib_umem = ib_umem_get(pd->ibpd.uobject->context, start, length,
				mr_access_flags, false);

		if (IS_ERR(ib_umem)) {
			rc = PTR_ERR(ib_umem);
			ntrdma_err(dev, "reg_user_mr failed on ib_umem %d\n",
				rc);
			goto err_umem;
		}

		count = ntc_umem_count(dev->ntc, ib_umem);
		if (count < 0) {
			rc = count;
			goto err_mr;
		}
	}

	mr = kmalloc_node(sizeof(*mr) + count * sizeof(mr->sg_list[0]),
			GFP_KERNEL, dev->node);
	if (!mr) {
		rc = -ENOMEM;
		ntrdma_err(dev, "reg_user_mr failed on kmalloc_node %d\n", rc);
		goto err_mr;
	}

	init_completion(&mr->enable_mrcb.cb.cmds_done);

	mr->ib_umem = ib_umem;
	mr->pd_key = pd->key;
	mr->access = mr_access_flags;
	mr->addr = virt_addr;
	mr->len = length;
	mr->sg_count = count;
	mr->done = NULL;

	if (!ib_umem) {
		rc = ntc_mr_buf_map_dma(&mr->sg_list[0], dev->ntc, length,
					dma_addr, mr_access_flags);
		if (rc < 0)
			goto err_init;
	}

	rc = ntrdma_mr_init(mr, dev);
	if (rc < 0) {
		ntrdma_err(dev,
				"reg_user_mr failed on ntrdma_mr_init %d\n",
				rc);
		goto err_init;
	}

	memset(&mrcb, 0, sizeof(mrcb));
	init_completion(&mrcb.cb.cmds_done);
	rc = ntrdma_res_add(&mr->res, &mrcb.cb, &dev->mr_list, &dev->mr_vec);
	if (rc < 0) {
		ntrdma_mr_put(mr);
		ntrdma_err(dev, "reg_user_mr failed on ntrdma_res_add %d", rc);
		return ERR_PTR(rc);
	}

	mr->ibmr.lkey = mr->ibmr.rkey = mr->res.key;

	ntrdma_dbg(dev, "added mr%d\n", mr->res.key);
	ntrdma_dbg(dev, "addr %llx\n", mr->addr);
	ntrdma_dbg(dev, "access %x\n", mr->access);
	ntrdma_dbg(dev, "count %x\n", mr->sg_count);

	for (i = 0; i < count; ++i) {
		ntrdma_vdbg(dev, "sgl[%d] dma_addr %llx len %#llx\n", i,
			mr->sg_list[i].dma_addr,
			mr->sg_list[i].size);
	}

	ntrdma_debugfs_mr_add(mr);

	return &mr->ibmr;

	// ntrdma_mr_del(mr);
err_init:
	kfree(mr);
err_mr:
	if (ib_umem)
		ib_umem_release(ib_umem);
err_umem:
	atomic_dec(&dev->mr_num);
err_len:
	ntrdma_err(dev, "failed, returning err %d\n", rc);
	return ERR_PTR(rc);
}

static void mr_release(struct kref *kref)
{
	struct ntrdma_obj *obj = container_of(kref, struct ntrdma_obj, kref);
	struct ntrdma_res *res = container_of(obj, struct ntrdma_res, obj);
	struct ntrdma_mr *mr = container_of(res, struct ntrdma_mr, res);
	struct ntrdma_dev *dev = ntrdma_mr_dev(mr);
	struct completion *done = mr->done;

	TRACE("dev %p, mr %p (res key %d)\n",
			dev, mr, mr->res.key);
	ntrdma_mr_deinit(mr, dev);
	if (mr->ib_umem)
		ib_umem_release(mr->ib_umem);
	WARN(!ntrdma_list_is_entry_poisoned(&obj->dev_entry),
		"Free list element while in the list, obj %p, res %p, mr %p (key %d)\n",
		obj, res, mr, mr->res.key);
	kfree(mr);
	atomic_dec(&dev->mr_num);

	if (done)
		complete_all(done);
}


void ntrdma_mr_put(struct ntrdma_mr *mr)
{
	ntrdma_res_put(&mr->res, mr_release);
}

static int ntrdma_dereg_mr(struct ib_mr *ibmr)
{
	struct ntrdma_mr *mr = ntrdma_ib_mr(ibmr);
	struct ntrdma_dev *dev = ntrdma_mr_dev(mr);
	struct ntrdma_mr_cmd_cb mrcb;
	struct completion done;

	ntrdma_debugfs_mr_del(mr);

	memset(&mrcb, 0, sizeof(mrcb));
	init_completion(&mrcb.cb.cmds_done);
	ntrdma_res_del(&mr->res, &mrcb.cb, &dev->mr_vec);

	init_completion(&done);
	mr->done = &done;

	ntrdma_mr_put(mr);

	wait_for_completion(&done);
	ntc_flush_dma_channels(dev->ntc);

	return 0;
}

static struct ib_ucontext *ntrdma_alloc_ucontext(struct ib_device *ibdev,
						 struct ib_udata *ibudata)
{
	struct ntrdma_dev *dev = ntrdma_ib_dev(ibdev);
	struct ib_ucontext *ibuctx;
	int rc;

	ibuctx = kmem_cache_alloc_node(ibuctx_slab, GFP_KERNEL, dev->node);
	if (!ibuctx) {
		rc = -ENOMEM;
		goto err_ctx;
	}

	return ibuctx;

err_ctx:
	return ERR_PTR(rc);
}

static int ntrdma_dealloc_ucontext(struct ib_ucontext *ibuctx)
{
	kmem_cache_free(ibuctx_slab, ibuctx);

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

static int ntrdma_qp_file_release(struct inode *inode, struct file *filp)
{
	struct ntrdma_qp *qp = filp->private_data;

	if (qp->send_page) {
		put_page(qp->send_page);
		qp->send_page = NULL;
	}

	ntc_dma_flush(qp->dma_chan);

	ntrdma_qp_put(qp);

	return 0;
}

static inline int ntrdma_validate_post_send_wqe(struct ntrdma_qp *qp,
						struct ntrdma_send_wqe *wqe,
						u32 wqe_size)
{
	struct ib_sge *wqe_snd_sge;
	int i;
	u64 available_size = 0;

	wqe->op_status = 0;
	wqe->recv_key = ~(u32)0;
	wqe->rdma_sge.length = 0;

	if (unlikely((wqe->rdma_sge.lkey ==
				NTRDMA_RESERVED_DMA_LEKY))) {
		ntrdma_qp_err(qp,
				"QP %d, wrid 0x%llx invalid  lkey %u",
						qp->res.key,
						wqe->ulp_handle,
						wqe->rdma_sge.lkey);
		return -EINVAL;
	}

	if (unlikely(((unsigned int)wqe->op_code > (unsigned int)
				NTRDMA_SEND_WR_MAX_SUPPORTED))) {
		ntrdma_qp_err(qp,
				"QP %d, wrid 0x%llx invalid opcode %u ",
				qp->res.key, wqe->ulp_handle,
				(unsigned int)wqe->op_code);
		return -EINVAL;
	}

	if (ntrdma_send_wqe_is_inline(wqe)) {
		if (unlikely(wqe->inline_len != wqe_size - sizeof(*wqe))) {
			ntrdma_qp_err(qp,
				"QP %d wrid 0x%llx inline len %d, size %d wqe size %ld",
				qp->res.key, wqe->ulp_handle,
				wqe->inline_len, wqe_size,
				sizeof(*wqe));
			return -EINVAL;
		}
		available_size = wqe->inline_len;
		TRACE_DATA(
			"OPCODE %d: flags %x addr %llx QP %d inline len %lld wrid %llu",
			wqe->op_code, wqe->flags, wqe->rdma_sge.addr, qp->res.key,
			available_size, wqe->ulp_handle);

		if (qp->qp_type != IB_QPT_GSI) {
			if (wqe->flags & IB_SEND_SIGNALED)
				this_cpu_inc(dev_cnt.post_send_wqes_ioctl_signalled);
			this_cpu_inc(dev_cnt.post_send_wqes_ioctl);
		}
	} else {
		if (unlikely(wqe->sg_count * sizeof(struct ib_sge) !=
				wqe_size - sizeof(*wqe))) {
			ntrdma_qp_err(qp,
				"QP %d wrid 0x%llx sg_count %d ib_sge size %ld, size %d wqe size %ld",
				qp->res.key, wqe->ulp_handle,
				wqe->sg_count, sizeof(struct ib_sge),
				wqe_size, sizeof(*wqe));
			return -EINVAL;
		}

		for (i = 0; i < wqe->sg_count; i++) {
			wqe_snd_sge = snd_sg_list(i, wqe);
			if (unlikely(wqe_snd_sge->lkey ==
					NTRDMA_RESERVED_DMA_LEKY)) {
				ntrdma_qp_err(qp,
						"QP %d wrid 0x%llx lkey %d ",
						qp->res.key, wqe->ulp_handle,
						wqe_snd_sge->lkey);
				return -EINVAL;
			}
			available_size += wqe_snd_sge->length;
		}
		TRACE_DATA(
			"OPCODE %d: flags %x, addr %llx QP %d num sges %d wrid %llu",
			wqe->op_code, wqe->flags, wqe->rdma_sge.addr, qp->res.key,
			wqe->sg_count, wqe->ulp_handle);

		if (qp->qp_type != IB_QPT_GSI) {
			if (wqe->flags & IB_SEND_SIGNALED)
				this_cpu_inc(dev_cnt.post_send_wqes_ioctl_signalled);
			this_cpu_inc(dev_cnt.post_send_wqes_ioctl);
		}
	}

	this_cpu_add(dev_cnt.post_send_bytes, available_size);
	return 0;
}

static inline int ntrdma_qp_process_send_ioctl_locked(struct ntrdma_qp *qp,
						void volatile *_uptr,
						bool *had_immediate_work,
						bool *has_deferred_work)
{
	struct ntrdma_send_wqe *wqe;
	u32 pos, end, base;
	size_t max_size = sizeof(struct ntrdma_send_wqe) +
		max_t(size_t, qp->send_wqe_inline_cap,
			qp->send_wqe_sg_cap * sizeof(struct ib_sge));
	void volatile *uptr = _uptr;
	struct ntrdma_snd_hdr hdr;
	u32 wqe_size;
	u32 next_wqe_size;
	int i = 0;
	int rc = 0;

	memcpy(&hdr, (void *)uptr, sizeof(hdr));
	uptr += sizeof(hdr);
	wqe_size = hdr.first_wqe_size;

	while ((rc >= 0) && (i < hdr.wqe_counter)) {
		/* get the next posting range in the ring */
		if (!ntrdma_qp_send_post_get(qp, &pos, &end, &base)) {
			rc = -EAGAIN;
			break;
		}

		wqe = NULL;
		for (; i < hdr.wqe_counter; i++) {
			/* current entry in the ring */
			if (!wqe)
				wqe = ntrdma_qp_send_wqe(qp, pos);

			if (unlikely(wqe_size > max_size)) {
				ntrdma_qp_err(qp, "wqe_size %d max_size %zu",
					wqe_size, max_size);
				rc = -EINVAL;
				break;
			}

			memcpy(wqe, (void *)uptr, wqe_size);
			uptr += wqe_size;
			next_wqe_size = wqe->recv_key;

			rc = ntrdma_validate_post_send_wqe(qp, wqe, wqe_size);
			if (rc < 0)
				break;
			wqe_size = next_wqe_size;

			if (!(wqe->flags & IB_SEND_SIGNALED) &&
				(wqe->op_code == IB_WR_RDMA_WRITE)) {
				rc = ntrdma_qp_rdma_write(qp, wqe);
				if (unlikely(rc < 0)) {
					ntrdma_qp_err(qp,
						"ntrdma_qp_rdma_write returned %d, QP %d pos %d wrid 0x%llx flags %x opcode %d",
						rc, qp->res.key, pos,
						wqe->ulp_handle, wqe->flags,
						wqe->op_code);
					break;
				}

				*had_immediate_work = true;
				/* pos and wqe can be reused. */
			} else {
				wqe = NULL;
				++pos;
				*has_deferred_work = true;
			}
		}

		/* update the next posting range */
		ntrdma_qp_send_post_put(qp, pos, base);
	}

	if (rc < 0) {
		ntrdma_qp_err(qp, "rc %d, QP %d", rc, qp->res.key);
		*(u32 volatile *)_uptr = i;
	} else
		rc = 0;
	return rc;
}

static inline int ntrdma_qp_process_send_ioctl(struct ntrdma_qp *qp)
{
	DEFINE_NTC_FUNC_PERF_TRACKER(perf, 1 << 20);
	bool had_immediate_work = false;
	bool has_deferred_work = false;
	void volatile *_uptr;
	int rc;

	if (unlikely(!qp->send_page))
		return -EINVAL;
	_uptr = page_address(qp->send_page);

	ntrdma_qp_send_post_lock(qp);

	if (likely(ntrdma_qp_is_send_ready(qp))) {
		rc = ntrdma_qp_process_send_ioctl_locked(qp, _uptr,
							&had_immediate_work,
							&has_deferred_work);
		ntrdma_qp_additional_work(qp, has_deferred_work,
					had_immediate_work);
	} else {
		ntrdma_qp_err(qp, "qp %d state %d", qp->res.key,
			atomic_read(&qp->state));
		rc = -EINVAL;
	}

	ntrdma_qp_send_post_unlock(qp);

	NTRDMA_PERF_MEASURE(perf);

	if (unlikely(rc < 0) && (rc != -EAGAIN))
		ntrdma_qp_err(qp, "returning %d on QP %d",
				rc, qp->res.key);

	return rc;
}

static long ntrdma_qp_file_ioctl(struct file *filp, unsigned int cmd,
			unsigned long arg)
{
	struct ntrdma_qp *qp = filp->private_data;

	switch (cmd) {
	case NTRDMA_IOCTL_SEND:
		return ntrdma_qp_process_send_ioctl(qp);
	default:
		return -EINVAL;
	}
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

	ibdev->dev.parent = dev->ntc->ntb_dev;

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
	ntrdma_info(dev, "NTRDMA IB dev deinit");
	ib_unregister_device(&dev->ibdev);
}

int __init ntrdma_ib_module_init(void)
{
	compiletime_assert((IB_WR_SEND == NTRDMA_WR_SEND) &&
			(IB_WR_SEND_WITH_IMM == NTRDMA_WR_SEND_IMM) &&
			(IB_WR_RDMA_WRITE_WITH_IMM == NTRDMA_WR_SEND_RDMA) &&
			(IB_WR_RDMA_WRITE == NTRDMA_WR_RDMA_WRITE) &&
			(IB_WR_RDMA_READ == NTRDMA_WR_RDMA_READ),
			"IB_WR and NTRDMA_WR enums must match for supported");

	if (!((ah_slab = KMEM_CACHE(ntrdma_ah, 0)) &&
			(cq_slab = KMEM_CACHE(ntrdma_cq, 0)) &&
			(pd_slab = KMEM_CACHE(ntrdma_pd, 0)) &&
			(qp_slab = KMEM_CACHE(ntrdma_qp, 0)) &&
			(ibuctx_slab = KMEM_CACHE(ib_ucontext, 0)))) {
		ntrdma_ib_module_deinit();
		return -ENOMEM;
	}

	return 0;
}

void ntrdma_ib_module_deinit(void)
{
	ntrdma_deinit_slab(&ah_slab);
	ntrdma_deinit_slab(&cq_slab);
	ntrdma_deinit_slab(&pd_slab);
	ntrdma_deinit_slab(&qp_slab);
	ntrdma_deinit_slab(&ibuctx_slab);
}
