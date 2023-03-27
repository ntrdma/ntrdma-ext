/*
 * Copyright (c) 2014, 2015 EMC Corporation.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 *  * General Public License (GPL) Version 2, available from the file
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

static struct kmem_cache *ah_slab;
static struct kmem_cache *ibcq_slab;
static struct kmem_cache *pd_slab;
static struct kmem_cache *ibuctx_slab;

#define RDMA_DRIVER_NTRDMA 17

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

	if (!ah) {
		WARN_ON(1);
		return ERR_PTR(-ENOMEM);
	}

	ntrdma_create_ah_common(ah, ah_attr);
	return &ah->ibah;
}

/* not implemented / not required? */
static int ntrdma_destroy_ah(struct ib_ah *ibah)
{
	struct ntrdma_ah *ah = container_of(ibah, struct ntrdma_ah, ibah);

	kmem_cache_free(ah_slab, ah);
	return 0;
}

static inline void ntrdma_cq_release_cache_free(struct ntrdma_cq *cq)
{
	kmem_cache_free(ibcq_slab, cq->ibcq);
	kmem_cache_free(cq_slab, cq);
}

static inline void ntrdma_pd_release_cache_free(struct ntrdma_pd *pd)
{
	kmem_cache_free(pd_slab, pd);
}

static int ntrdma_destroy_qp(struct ib_qp *ibqp)
{
	return ntrdma_destroy_qp_common(ibqp);
}

struct ib_umem *ntrdma_ib_umem_get(struct ib_udata *ibudata,
		struct ib_ucontext *context, unsigned long addr, size_t size,
		int access, int dmasync)
{
	return ib_umem_get(context, addr, size, access, dmasync);
}

static int ntrdma_dereg_mr(struct ib_mr *ibmr)
{
	return ntrdma_dereg_mr_common(ibmr);
}

static struct ib_ucontext *ntrdma_alloc_ucontext(struct ib_device *ibdev,
		struct ib_udata *ibudata)
{
	struct ntrdma_dev *dev = ntrdma_ib_dev(ibdev);
	struct ntrdma_ucontext *ntctx;
	int rc;

	ntctx = kmem_cache_alloc_node(ibuctx_slab, GFP_KERNEL, dev->node);
	if (!ntctx) {
		ntrdma_err(dev, "kmem_cache_alloc_node failed");
		rc = -ENOMEM;
		goto err_ctx;
	}
	memset(ntctx, 0, sizeof(*ntctx));

	return &ntctx->ibucontext;

err_ctx:
	return ERR_PTR(rc);
}

static int ntrdma_dealloc_ucontext(struct ib_ucontext *ibuctx)
{
	struct ntrdma_ucontext *ntctx =
			container_of(ibuctx, struct ntrdma_ucontext, ibucontext);

	kmem_cache_free(ibuctx_slab, ntctx);
	return 0;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 19, 0)
int ntrdma_add_gid(struct ib_device *device, u8 port_num,
		unsigned int index, const union ib_gid *gid,
		const struct ib_gid_attr *attr, void **context)
#else
int ntrdma_add_gid(const struct ib_gid_attr *attr, void **context)
#endif
{
	return 0;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 19, 0)
int ntrdma_del_gid(struct ib_device *device, u8 port_num,
		unsigned int index, void **context)
#else
int ntrdma_del_gid(const struct ib_gid_attr *attr, void **context)
#endif
{
	return 0;
}


int ntrdma_process_mad(struct ib_device *device,
		int process_mad_flags,
		ntrdma_port_t port_num,
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

static int ntrdma_destroy_cq(struct ib_cq *ibcq)
{
	struct ntrdma_cq *cq;

	NTRDMA_IB_PERF_INIT;
	NTRDMA_IB_PERF_START;

	cq = ntrdma_destroy_cq_common(ibcq);

	ntrdma_cq_put(cq);

	NTRDMA_IB_PERF_END;

	return 0;
}

void ntrdma_pd_get(struct ntrdma_pd *pd)
{
	ntrdma_obj_get(&pd->obj);
}

void ntrdma_pd_put(struct ntrdma_pd *pd)
{
	ntrdma_obj_put(&pd->obj, ntrdma_pd_release);
}

static int ntrdma_dealloc_pd(struct ib_pd *ibpd)
{
	struct ntrdma_pd *pd;

	NTRDMA_IB_PERF_INIT;
	NTRDMA_IB_PERF_START;

	pd = ntrdma_dealloc_pd_common(ibpd);

	ntrdma_pd_put(pd);
	NTRDMA_IB_PERF_END;
	return 0;
}

static inline struct ntrdma_pd *ntrdma_new_pd(struct ib_pd *ibpd,
		struct ntrdma_dev *dev)
{
	struct ntrdma_pd *pd = kmem_cache_alloc_node(pd_slab, GFP_KERNEL,
			dev->node);
	if (pd)
		memset(pd, 0, sizeof(*pd));

	return pd;
}

static struct ib_pd *ntrdma_alloc_pd(struct ib_device *ibdev,
		struct ib_ucontext *ibuctx, struct ib_udata *ibudata)
{
	struct ntrdma_pd *pd = NULL;
	int rc;

	rc = ntrdma_alloc_pd_common(NULL, ibdev, ibuctx, ibudata, &pd);

	if (!rc)
		return &pd->ibpd;

	return ERR_PTR(rc);
}

static inline void ntrdma_dev_ib_deinit_common(struct ntrdma_dev *dev)
{
	ntrdma_cm_deinit(dev->ibdev.iwcm);
}

static inline struct ntrdma_cq *ntrdma_alloc_cq(struct ib_cq *ibcq,
		struct ntrdma_dev *dev)
{
	struct ntrdma_ib_cq *ntrdma_ib_cq =
			kmem_cache_alloc_node(ibcq_slab, GFP_KERNEL, dev->node);
	struct ntrdma_cq *cq =
			kmem_cache_alloc_node(cq_slab, GFP_KERNEL, dev->node);
	if (ntrdma_ib_cq) {
		memset(ntrdma_ib_cq, 0, sizeof(*ntrdma_ib_cq));
		ntrdma_ib_cq->cq = cq;
	}
	if (cq) {
		memset(cq, 0, sizeof(*cq));
		cq->ibcq = ntrdma_ib_cq;
	}

	return cq;
}

static struct ib_cq *ntrdma_create_cq(struct ib_device *ibdev,
		const struct ib_cq_init_attr *ibattr,
		struct ib_ucontext *ibuctx,
		struct ib_udata *ibudata)
{
	struct ntrdma_cq *cq = NULL;
	int rc;
	rc = ntrdma_create_cq_common(NULL, ibdev, ibattr, ibuctx, ibudata, &cq);

	if (!rc)
		return &cq->ibcq->ibcq;

	return ERR_PTR(rc);
}

static inline int ntrdma_set_ib_ops(struct ntrdma_dev *dev,
		struct ib_device *ibdev)
{
	/* not implemented / not required */
	ibdev->owner				= THIS_MODULE;
	ibdev->uverbs_abi_ver		= 1;
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
	ibdev->query_device			= ntrdma_query_device;
	ibdev->query_port			= ntrdma_query_port;

	/* completion queue */
	ibdev->create_cq			= ntrdma_create_cq;
	ibdev->destroy_cq			= ntrdma_destroy_cq;
	ibdev->poll_cq				= ntrdma_poll_cq;
	ibdev->req_notify_cq		= ntrdma_req_notify_cq;

	/* protection domain */
	ibdev->alloc_pd				= ntrdma_alloc_pd;
	ibdev->dealloc_pd			= ntrdma_dealloc_pd;

	/* memory region */
	ibdev->reg_user_mr			= ntrdma_reg_user_mr;
	ibdev->dereg_mr				= ntrdma_dereg_mr;

	/* queue pair */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 19, 0)
	ibdev->driver_id			= RDMA_DRIVER_NTRDMA;
	ibdev->create_qp			= ntrdma_create_qp;
	ibdev->query_qp				= ntrdma_query_qp;
	ibdev->modify_qp			= ntrdma_modify_qp;
	ibdev->destroy_qp			= ntrdma_destroy_qp;
	ibdev->post_send			= ntrdma_post_send;
	ibdev->post_recv			= ntrdma_post_recv;
	ibdev->get_link_layer		= ntrdma_get_link_layer;
	ibdev->add_gid				= ntrdma_add_gid;
	ibdev->del_gid				= ntrdma_del_gid;
	ibdev->process_mad			= ntrdma_process_mad;
#endif
	ibdev->iwcm = ntrdma_cm_init(ibdev->name, dev);
	return (ibdev->iwcm == NULL);
}

static inline int ntrdma_ib_register_device(struct ib_device *ibdev)
{
	return ib_register_device(ibdev, NULL);
}

void ntrdma_ib_module_deinit(void)
{
	ntrdma_deinit_slab(&qp_slab);
	ntrdma_deinit_slab(&ah_slab);
	ntrdma_deinit_slab(&ibcq_slab);
	ntrdma_deinit_slab(&cq_slab);
	ntrdma_deinit_slab(&pd_slab);
	ntrdma_deinit_slab(&ibuctx_slab);
}

static inline bool ntrdma_slab_init(void)
{
	return ((ah_slab = KMEM_CACHE(ntrdma_ah, 0)) &&
			(cq_slab = KMEM_CACHE(ntrdma_cq, 0)) &&
			(ibcq_slab = KMEM_CACHE(ntrdma_ib_cq, 0)) &&
				(pd_slab = KMEM_CACHE(ntrdma_pd, 0)) &&
				(qp_slab = KMEM_CACHE(ntrdma_qp, 0)) &&
				(ibuctx_slab = KMEM_CACHE(ib_ucontext, 0)));
}
