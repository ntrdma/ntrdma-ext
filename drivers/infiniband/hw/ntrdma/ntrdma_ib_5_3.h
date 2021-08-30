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
#include "ntrdma_ib.h"

/* The RDMA_DRIVER_NTRDMA value depends on the rdma-core library and not on kernel version
 * Old 2.2 had value of 17, but it conflicts with new values so from version 2.71 it moved
 * to 19 */
#define RDMA_DRIVER_NTRDMA 19

/* not implemented / not required? */
/* if required, one needs to implement:
 * Perform path query to the Subnet Administrator (SA)
	Out of band connection to the remote node, for example: using socket
 * Using well-known values, for example: this can be done in a static
    subnet (which all of the addresses are predefined) or using
    multicast groups
 */
static int ntrdma_create_ah(struct ib_ah *ibah, struct rdma_ah_attr *ah_attr,
		u32 flags, struct ib_udata *udata)
{
	struct ntrdma_ah *ah = container_of(ibah, struct ntrdma_ah, ibah);

	ntrdma_create_ah_common(ah, ah_attr);
	return 0;
}

/* not implemented / not required? */
static void ntrdma_destroy_ah(struct ib_ah *ibah, u32 flags)
{
}

static inline void ntrdma_cq_release_cache_free(struct ntrdma_cq *cq)
{
	kmem_cache_free(cq_slab, cq);
}

static inline void ntrdma_pd_release_cache_free(struct ntrdma_pd *pd)
{
}

static int ntrdma_destroy_qp(struct ib_qp *ibqp, struct ib_udata *udata)
{
	return ntrdma_destroy_qp_common(ibqp);
}

struct ib_umem *ntrdma_ib_umem_get(struct ib_udata *ibudata,
		struct ib_ucontext *context, unsigned long addr, size_t size,
		int access, int dmasync)
{
	return ib_umem_get(ibudata, addr, size, access, dmasync);
}

static int ntrdma_dereg_mr(struct ib_mr *ibmr, struct ib_udata *udata)
{
	return ntrdma_dereg_mr_common(ibmr);
}

static int ntrdma_alloc_ucontext(struct ib_ucontext *ctx,
		struct ib_udata *udata)
{
	return 0;
}

static void ntrdma_dealloc_ucontext(struct ib_ucontext *ibuctx)
{
}

int ntrdma_add_gid(const struct ib_gid_attr *attr, void **context)
{
	return 0;
}

int ntrdma_del_gid(const struct ib_gid_attr *attr, void **context)
{
	return 0;
}

int ntrdma_process_mad(struct ib_device *device,
		int process_mad_flags,
		u8 port_num,
		const struct ib_wc *in_wc,
		const struct ib_grh *in_grh,
		const struct ib_mad *in_mad,
		struct ib_mad *out_mad,
		size_t *out_mad_size,
		u16 *out_mad_pkey_index)
{
	TRACE("RDMA CM MAD received: class %d\n", in_mad->mad_hdr.mgmt_class);
	return IB_MAD_RESULT_SUCCESS;
}

void ntrdma_disassociate_ucontext(struct ib_ucontext *ibcontext)
{
}

static void ntrdma_destroy_cq(struct ib_cq *ibcq, struct ib_udata *udata)
{
	struct ntrdma_cq *cq;

	NTRDMA_IB_PERF_INIT;
	NTRDMA_IB_PERF_START;

	cq = ntrdma_destroy_cq_common(ibcq);

	ntrdma_cq_put(cq);

	NTRDMA_IB_PERF_END;
}

void ntrdma_pd_get(struct ntrdma_pd *pd)
{
}

void ntrdma_pd_put(struct ntrdma_pd *pd)
{
}

static void ntrdma_dealloc_pd(struct ib_pd *ibpd, struct ib_udata *udata)
{
	struct ntrdma_pd *pd;

	NTRDMA_IB_PERF_INIT;
	NTRDMA_IB_PERF_START;

	pd = ntrdma_dealloc_pd_common(ibpd);
	ntrdma_pd_release(&pd->obj.kref);

	NTRDMA_IB_PERF_END;
}

static inline struct ntrdma_pd *ntrdma_new_pd(struct ib_pd *ibpd,
		struct ntrdma_dev *dev)
{
	struct ntrdma_pd *pd = container_of(ibpd, struct ntrdma_pd, ibpd);

	return pd;
}

static int ntrdma_alloc_pd(struct ib_pd *ibpd, struct ib_udata *ibudata)
{
	struct ntrdma_pd *pd;

	return ntrdma_alloc_pd_common(ibpd, ibpd->device, NULL, ibudata, &pd);
}

static inline void ntrdma_dev_ib_deinit_common(struct ntrdma_dev *dev)
{
	kmem_cache_destroy(dev->cmid_node_slab);
}

static inline struct ntrdma_cq *ntrdma_alloc_cq(struct ib_cq *ibcq,
		struct ntrdma_dev *dev)
{
	struct ntrdma_ib_cq *ntrdma_ib_cq =
			container_of(ibcq, struct ntrdma_ib_cq, ibcq);
	struct ntrdma_cq *cq =
			kmem_cache_alloc_node(cq_slab, GFP_KERNEL, dev->node);

	if (ntrdma_ib_cq) {
		ntrdma_ib_cq->cq = cq;
	}
	if (cq) {
		memset(cq, 0, sizeof(*cq));
		cq->ibcq = ntrdma_ib_cq;
	}

	return cq;
}

static int ntrdma_create_cq(struct ib_cq *ibcq,
		const struct ib_cq_init_attr *ibattr,
		struct ib_udata *ibudata)
{
	struct ntrdma_cq *cq = NULL;

	return ntrdma_create_cq_common(ibcq, ibcq->device, ibattr, NULL, ibudata,
			&cq);
}

static const struct ib_device_ops ntrdma_dev_ops = {
	.owner			= THIS_MODULE,
	.driver_id		= RDMA_DRIVER_NTRDMA,
	.uverbs_abi_ver	= 1,

	/* not implemented / not required */
	.get_netdev		= ntrdma_get_netdev,
	.get_port_immutable	= ntrdma_get_port_immutable,
	.query_pkey		= ntrdma_query_pkey,
	.query_gid		= ntrdma_query_gid,
	.create_ah		= ntrdma_create_ah,
	.destroy_ah		= ntrdma_destroy_ah,
	.get_dma_mr		= ntrdma_get_dma_mr,

	/* userspace context */
	.alloc_ucontext	= ntrdma_alloc_ucontext,
	.dealloc_ucontext	= ntrdma_dealloc_ucontext,
	.disassociate_ucontext = ntrdma_disassociate_ucontext,

	/* device and port queries */
	.query_device	= ntrdma_query_device,
	.query_port		= ntrdma_query_port,

	/* completion queue */
	.create_cq		= ntrdma_create_cq,
	.destroy_cq		= ntrdma_destroy_cq,
	.poll_cq		= ntrdma_poll_cq,
	.req_notify_cq	= ntrdma_req_notify_cq,

	/* protection domain */
	.alloc_pd		= ntrdma_alloc_pd,
	.dealloc_pd		= ntrdma_dealloc_pd,

	/* memory region */
	.reg_user_mr	= ntrdma_reg_user_mr,
	.dereg_mr		= ntrdma_dereg_mr,

	/* queue pair */
	.create_qp		= ntrdma_create_qp,
	.query_qp		= ntrdma_query_qp,
	.modify_qp		= ntrdma_modify_qp,
	.destroy_qp		= ntrdma_destroy_qp,
	.post_send		= ntrdma_post_send,
	.post_recv		= ntrdma_post_recv,
	.get_link_layer	= ntrdma_get_link_layer,
	.add_gid		= ntrdma_add_gid,
	.del_gid		= ntrdma_del_gid,
	.process_mad	= ntrdma_process_mad,

	INIT_RDMA_OBJ_SIZE(ib_ah, ntrdma_ah, ibah),
	INIT_RDMA_OBJ_SIZE(ib_pd, ntrdma_pd, ibpd),
	INIT_RDMA_OBJ_SIZE(ib_cq, ntrdma_ib_cq, ibcq),
	INIT_RDMA_OBJ_SIZE(ib_ucontext, ntrdma_ucontext, ibucontext),
};

static inline int ntrdma_ib_register_device(struct ib_device *ibdev)
{
	return ib_register_device(ibdev, "ntrdma_%d");
}

static inline int ntrdma_set_ib_ops(struct ntrdma_dev *dev,
		struct ib_device *ibdev)
{
	ib_set_device_ops(ibdev, &ntrdma_dev_ops);
	ntrdma_cm_init(ibdev);
	return 0;
}

void ntrdma_ib_module_deinit(void)
{
	ntrdma_deinit_slab(&qp_slab);
	ntrdma_deinit_slab(&cq_slab);
}

static inline bool ntrdma_slab_init(void)
{
	return ((qp_slab = KMEM_CACHE(ntrdma_qp, 0)) &&
			(cq_slab = KMEM_CACHE(ntrdma_cq, 0)));
}
