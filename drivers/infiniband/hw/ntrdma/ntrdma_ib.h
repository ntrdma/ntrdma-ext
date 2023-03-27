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
#define NTRDMA_PKEY_DEFAULT 0xffff
#define NTRDMA_GIB_TBL_LEN 1
#define NTRDMA_PKEY_TBL_LEN 2

#define DELL_VENDOR_ID 0x1028
#define NOT_SUPPORTED 0
/* TODO: remove this macro when adding to enum rdma_driver_id
 * in include/uapi/rdma/rdma_user_ioctl_cmds.h
 */
#define NTRDMA_IB_PERF_PRINT_ABOVE_MILI_SECS_LATENCY_CONSTANT 400
#define NTRDMA_IB_PERF_INIT unsigned long ___ts_ = 0; \
				unsigned long ___tt_ = 0; \
				unsigned int ___perf_ = 0
#define NTRDMA_IB_PERF_START {___ts_ = jiffies; }
#define NTRDMA_IB_PERF_END do {\
			___tt_ = jiffies - ___ts_; \
			___perf_ = jiffies_to_msecs(___tt_);\
			if (time_after(jiffies, ___ts_ + \
					NTRDMA_IB_PERF_PRINT_ABOVE_MILI_SECS_LATENCY_CONSTANT)) { \
				TRACE(\
						"performance time of method %s: %u miliseconds", \
						__func__, \
						___perf_); \
			} \
	} while (0)

#define qp_enum_to_string(QP_ENUM) \
						QP_ENUM == IB_QPS_RESET ? "IB_QPS_RESET" :\
						QP_ENUM == IB_QPS_INIT ? "IB_QPS_INIT" :\
						QP_ENUM == IB_QPS_RTR ? "IB_QPS_RTR" :\
						QP_ENUM == IB_QPS_RTS ? "IB_QPS_RTS" :\
						QP_ENUM == IB_QPS_SQD ? "IB_QPS_SQD" :\
						QP_ENUM == IB_QPS_SQE ? "IB_QPS_SQE" :\
						QP_ENUM == IB_QPS_ERR ? "IB_QPS_ERR" :\
						"Undefined state number"\

static int ntrdma_qp_file_release(struct inode *inode, struct file *filp);
static long ntrdma_qp_file_ioctl(struct file *filp, unsigned int cmd,
				unsigned long arg);
static int ntrdma_cq_file_release(struct inode *inode, struct file *filp);
static long ntrdma_cq_file_ioctl(struct file *filp, unsigned int cmd,
				unsigned long arg);

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)
static struct kmem_cache *qp_slab;
#endif
static struct kmem_cache *cq_slab;

struct ntrdma_ucontext {
	struct ib_ucontext ibucontext;
};

static const struct file_operations ntrdma_qp_fops = {
	.owner		= THIS_MODULE,
	.release	= ntrdma_qp_file_release,
	.unlocked_ioctl	= ntrdma_qp_file_ioctl,
	.compat_ioctl	= ntrdma_qp_file_ioctl,
};

static const struct file_operations ntrdma_cq_fops = {
	.owner		= THIS_MODULE,
	.release	= ntrdma_cq_file_release,
	.unlocked_ioctl	= ntrdma_cq_file_ioctl,
	.compat_ioctl	= ntrdma_cq_file_ioctl,
};

struct ntrdma_ah {
	struct ib_ah ibah;
	struct rdma_ah_attr attr;
};

static inline void ntrdma_create_ah_common(struct ntrdma_ah *ah,
		struct rdma_ah_attr *ah_attr)
{
	ah->attr = *ah_attr;
}

static inline int ntrdma_destroy_qp_common(struct ib_qp *ibqp)
{
	struct ntrdma_qp *qp = ntrdma_ib_qp(ibqp);
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);
	struct ntrdma_qp_cmd_cb qpcb;
	unsigned long t0, t1, t2, t3, t4;
	int key;

	NTRDMA_IB_PERF_INIT;
	NTRDMA_IB_PERF_START;

	key = qp ? qp->res.key : -1;
	ntrdma_dbg(dev, "QP %p (QP %d)\n", qp, key);

	if (unlikely(qp->send_cmpl != qp->send_post)) {
		ntrdma_dbg(dev,
				"Destroy QP %p (%d) while send cmpl %d send post %d send prod %d send cap %d\n",
				qp, qp->res.key, qp->send_cmpl,
				qp->send_post, qp->send_prod, qp->send_cap);
	}
	t0 = jiffies;
	ntrdma_debugfs_qp_del(qp);
	t1 = jiffies;
	memset(&qpcb, 0, sizeof(qpcb));

	init_completion(&qpcb.cb.cmds_done);
	ntrdma_res_del(&qp->res, &qpcb.cb, &dev->res.qp_vec);
	t2 = jiffies;
	ntc_dma_flush(qp->dma_chan);
	t3 = jiffies;
	ntrdma_cm_qp_shutdown(qp);
	ntrdma_qp_put(qp);
	t4 = jiffies;
	NTRDMA_IB_PERF_END;
	if (t4 - t0 > 100)
		ntrdma_info(dev,
				"#NTRDMAPERF -  put %ld, flush %ld, res_del %ld, debugfs %ld\n",
				t4 - t3, t3 - t2, t2 - t1, t1 - t0);
	return 0;
}

static inline int ntrdma_dereg_mr_common(struct ib_mr *ibmr)
{
	struct ntrdma_mr *mr = ntrdma_ib_mr(ibmr);
	struct ntrdma_dev *dev = ntrdma_mr_dev(mr);
	struct ntrdma_mr_cmd_cb mrcb;
	struct completion done;
	unsigned long t0, t1, t2, t3, t4;

	NTRDMA_IB_PERF_INIT;
	NTRDMA_IB_PERF_START;

	ntrdma_dbg(dev, "dereg MR %p (key %d)\n", mr, mr->res.key);
	t0 = jiffies;
	ntrdma_debugfs_mr_del(mr);
	t1 = jiffies;

	memset(&mrcb, 0, sizeof(mrcb));
	init_completion(&mrcb.cb.cmds_done);
	ntrdma_res_del(&mr->res, &mrcb.cb, &dev->res.mr_vec);
	t2 = jiffies;

	init_completion(&done);
	mr->done = &done;

	ntrdma_mr_put(mr);

	wait_for_completion(&done);
	t3 = jiffies;
	ntc_flush_dma_channels(dev->ntc);
	t4 = jiffies;


	NTRDMA_IB_PERF_END;
	if (t4 - t0 > 100)
		ntrdma_info(dev,
				"#NTRDMAPERF -  flush %ld, put %ld, res_del %ld, debugfs %ld\n",
				t4 - t3, t3 - t2, t2 - t1, t1 - t0);
	return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 13, 0)
typedef u32 ntrdma_port_t;
#else
typedef u8 ntrdma_port_t;
#endif

struct net_device *ntrdma_get_netdev(struct ib_device *ibdev, ntrdma_port_t port_num);
static int ntrdma_get_port_immutable(struct ib_device *ibdev, ntrdma_port_t port,
		struct ib_port_immutable *imm);
static int ntrdma_query_pkey(struct ib_device *ibdev, ntrdma_port_t port_num, u16 index,
		u16 *pkey);
static int ntrdma_query_gid(struct ib_device *ibdev, ntrdma_port_t port_num, int index,
		union ib_gid *ibgid);
static struct ib_mr *ntrdma_get_dma_mr(struct ib_pd *ibpd,
		int mr_access_flags);
static int ntrdma_query_device(struct ib_device *ibdev,
		struct ib_device_attr *ibattr, struct ib_udata *ibudata);
static int ntrdma_query_port(struct ib_device *ibdev, ntrdma_port_t port,
		struct ib_port_attr *ibattr);
enum rdma_link_layer ntrdma_get_link_layer(struct ib_device *device,
		ntrdma_port_t port_num);

static void ntrdma_cq_release(struct kref *kref);
static void ntrdma_pd_release(struct kref *kref);

static inline struct ntrdma_cq *ntrdma_destroy_cq_common(struct ib_cq *ibcq)
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
	mutex_lock(&dev->res.lock);
	list_del(&cq->obj.dev_entry);
	mutex_unlock(&dev->res.lock);

	ntrdma_cq_vbell_kill(cq);
	return cq;
}

static int ntrdma_poll_cq(struct ib_cq *ibcq, int howmany, struct ib_wc *ibwc);
static int ntrdma_req_notify_cq(struct ib_cq *ibcq,
		enum ib_cq_notify_flags flags);
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)
struct ib_qp *ntrdma_create_qp(struct ib_pd *ibpd,
		struct ib_qp_init_attr *ibqp_attr, struct ib_udata *ibudata);
#else
int ntrdma_create_qp(struct ib_qp *ibqp,
		struct ib_qp_init_attr *ibqp_attr, struct ib_udata *ibudata);
#endif
static int ntrdma_query_qp(struct ib_qp *ibqp, struct ib_qp_attr *ibqp_attr,
		int ibqp_mask, struct ib_qp_init_attr *ibqp_init_attr);
int ntrdma_modify_qp(struct ib_qp *ibqp, struct ib_qp_attr *ibqp_attr,
		int ibqp_mask, struct ib_udata *ibudata);
static int ntrdma_post_send(struct ib_qp *ibqp,
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 19, 0)
			struct ib_send_wr *ibwr,
			struct ib_send_wr **bad);
#else
			const struct ib_send_wr *ibwr,
			const struct ib_send_wr **bad);
#endif
static int ntrdma_post_recv(struct ib_qp *ibqp,
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 19, 0)
		struct ib_recv_wr *ibwr,
		struct ib_recv_wr **bad);
#else
		const struct ib_recv_wr *ibwr,
		const struct ib_recv_wr **bad);
#endif
static struct ib_mr *ntrdma_reg_user_mr(struct ib_pd *ibpd, u64 start,
		u64 length, u64 virt_addr, int mr_access_flags,
		struct ib_udata *ibudata);

static inline struct ntrdma_pd *ntrdma_dealloc_pd_common(struct ib_pd *ibpd)
{
	struct ntrdma_pd *pd = ntrdma_ib_pd(ibpd);
	struct ntrdma_dev *dev = ntrdma_pd_dev(pd);

	mutex_lock(&dev->res.lock);
	list_del(&pd->obj.dev_entry);
	mutex_unlock(&dev->res.lock);

	return pd;
}

int ntrdma_alloc_pd_common(struct ib_pd *ibpd, struct ib_device *ibdev,
		struct ib_ucontext *ibuctx, struct ib_udata *ibudata,
		struct ntrdma_pd **pd);
static int ntrdma_create_cq_common(struct ib_cq *ibcq, struct ib_device *ibdev,
		const struct ib_cq_init_attr *ibattr, struct ib_ucontext *ibuctx,
		struct ib_udata *ibudata, struct ntrdma_cq **cq);
