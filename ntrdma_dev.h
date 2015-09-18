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

#ifndef NTRDMA_DEV_H
#define NTRDMA_DEV_H

#include "ntrdma_os.h"
#include "ntrdma_ib.h"

#include "ntrdma_cb.h"
#include "ntrdma_mem.h"
#include "ntrdma_util.h"
#include "ntrdma_vbell.h"

struct ntrdma_peer_buf;

struct ntrdma_dev;
struct ntrdma_eth;
struct ntrdma_lpp;
struct ntrdma_pp;
struct ntrdma_vbell;
struct ntrdma_vbell_head;
struct ntrdma_dma_cb;

union ntrdma_cmd;
union ntrdma_rsp;

struct ntrdma_quiesce {
	NTRDMA_DECL_LIST_ENTRY		(entry);
	struct ntrdma_cb		cb;
};

struct ntrdma_req {
	struct ntrdma_dev		*dev;
};

struct ntrdma_dma_cb {
	NTRDMA_DECL_LIST_ENTRY		(entry);
	struct ntrdma_cb		cb;
};

struct ntrdma_dev_ops {
	void *(*port_device)(struct ntrdma_dev *dev);

	void (*port_enable)(struct ntrdma_dev *dev);
	void (*port_disable)(struct ntrdma_dev *dev);

	struct ntrdma_req *(*port_req_alloc)(struct ntrdma_dev *dev,
					     int cap_hint);
	void (*port_req_free)(struct ntrdma_req *req_impl);
	void (*port_req_submit)(struct ntrdma_req *req_impl);
	void (*port_req_memcpy)(struct ntrdma_req *req_impl,
				ntrdma_dma_addr_t dst,
				ntrdma_dma_addr_t src,
				ntrdma_size_t len);
	void (*port_req_imm32)(struct ntrdma_req *req_impl,
			       ntrdma_dma_addr_t dst,
			       ntrdma_u32_t val);
	void (*port_req_imm64)(struct ntrdma_req *req_impl,
			       ntrdma_dma_addr_t dst,
			       ntrdma_u64_t val);
	void (*port_req_signal)(struct ntrdma_req *req_impl);
	void (*port_req_fence)(struct ntrdma_req *req_impl);
	void (*port_req_callback)(struct ntrdma_req *req,
				  void (*cb)(void *cb_ctx),
				  void *cb_ctx);

	bool (*port_throttle)(struct ntrdma_dev *dev,
			      struct ntrdma_dma_cb *cb);
};

/* RDMA over PCIe NTB device */
struct ntrdma_dev {
	/* NOTE: .ibdev MUST be the first thing in ntrdma_dev!
	 *     (required by ib_alloc_device and ib_dealloc_device) */
	struct ib_device		ibdev;
#ifdef CONFIG_NTRDMA_DEBUGFS
	struct dentry			*debug;
#endif

	/* Numa node for driver memory */
	int				node;

	/* NTRDMA device operations that differ based on port type */
	const struct ntrdma_dev_ops	*ops;

	/* Base offset for writing to peer memory */
	ntrdma_dma_addr_t		peer_dram_base;

	/* virtual doorbells synchronization */

	int				vbell_enable;
	NTRDMA_DECL_SPL			(vbell_next_lock);
	NTRDMA_DECL_SPL			(vbell_self_lock);
	NTRDMA_DECL_SPL			(vbell_peer_lock);

	/* local virtual doorbells */

	ntrdma_u32_t			vbell_count;
	ntrdma_u32_t			vbell_start;
	ntrdma_u32_t			vbell_next;
	struct ntrdma_vbell_head	*vbell_vec;
	ntrdma_u32_t			*vbell_buf;
	ntrdma_size_t			vbell_buf_size;
	ntrdma_dma_addr_t		vbell_buf_dma;

	/* peer virtual doorbells */

#ifdef CONFIG_NTRDMA_VBELL_USE_SEQ
	ntrdma_u32_t			*vbell_peer_seq;
#endif
	ntrdma_dma_addr_t		peer_vbell_buf_dma;
	int				peer_vbell_count;

	/* commands to affect remote resources */

	ntrdma_bool_t			cmd_ready;

	/* command recv work */
	NTRDMA_DECL_MUT			(cmd_recv_lock);
	NTRDMA_DECL_DWI			(cmd_recv_work);
	struct ntrdma_vbell		cmd_recv_vbell;
	ntrdma_u32_t			cmd_recv_vbell_idx;

	/* command recv ring indices */
	ntrdma_u32_t			cmd_recv_cap;
	ntrdma_u32_t			cmd_recv_cons;

	/* command recv ring buffers and producer index */
	union ntrdma_cmd		*cmd_recv_buf;
	ntrdma_u32_t			*cmd_recv_prod_buf;
	ntrdma_dma_addr_t		cmd_recv_buf_dma;
	ntrdma_size_t			cmd_recv_buf_size;
	union ntrdma_rsp		*cmd_recv_rsp_buf;
	ntrdma_dma_addr_t		cmd_recv_rsp_buf_dma;
	ntrdma_size_t			cmd_recv_rsp_buf_size;
	ntrdma_dma_addr_t		peer_cmd_send_rsp_buf_dma;
	ntrdma_dma_addr_t		peer_cmd_send_cons_dma;
	ntrdma_u32_t			peer_cmd_send_vbell_idx;

	/* command recv work */
	NTRDMA_DECL_LIST_HEAD		(cmd_pend_list);
	NTRDMA_DECL_LIST_HEAD		(cmd_post_list);
	NTRDMA_DECL_CVH			(cmd_send_cond);
	NTRDMA_DECL_MUT			(cmd_send_lock);
	NTRDMA_DECL_DWI			(cmd_send_work);
	struct ntrdma_vbell		cmd_send_vbell;
	ntrdma_u32_t			cmd_send_vbell_idx;

	/* command send ring indices */
	ntrdma_u32_t			cmd_send_cap;
	ntrdma_u32_t			cmd_send_prod;
	ntrdma_u32_t			cmd_send_cmpl;

	/* command send ring buffers and consumer index */
	union ntrdma_cmd		*cmd_send_buf;
	ntrdma_dma_addr_t		cmd_send_buf_dma;
	ntrdma_size_t			cmd_send_buf_size;
	union ntrdma_rsp		*cmd_send_rsp_buf;
	ntrdma_u32_t			*cmd_send_cons_buf;
	ntrdma_dma_addr_t		cmd_send_rsp_buf_dma;
	ntrdma_size_t			cmd_send_rsp_buf_size;
	ntrdma_dma_addr_t		peer_cmd_recv_buf_dma;
	ntrdma_dma_addr_t		peer_cmd_recv_prod_dma;
	ntrdma_u32_t			peer_cmd_recv_vbell_idx;

	/* rdma resource synchronization state */

	int				res_enable;
	NTRDMA_DECL_MUT			(res_lock);
	NTRDMA_DECL_MUT			(rres_lock);

	/* completion queues are special */

	NTRDMA_DECL_LIST_HEAD		(cq_list);

	/* rdma local resources */

	NTRDMA_DECL_LIST_HEAD		(res_list);
	struct ntrdma_kvec		pd_vec;
	struct ntrdma_kvec		qp_vec;

	/* rdma remote resources */

	NTRDMA_DECL_LIST_HEAD		(rres_list);
	struct ntrdma_vec		rpd_vec;
	struct ntrdma_vec		rqp_vec;

	/* virtual ethernet device */
	struct ntrdma_eth		*eth;
};

#define ntrdma_ib_dev(__ibdev) \
	NTRDMA_CONTAINER_OF(__ibdev, struct ntrdma_dev, ibdev)

int ntrdma_dev_init(struct ntrdma_dev *dev);
void ntrdma_dev_deinit(struct ntrdma_dev *dev);

int ntrdma_dev_register(struct ntrdma_dev *dev);
void ntrdma_dev_unregister(struct ntrdma_dev *dev);

int ntrdma_dev_enable(struct ntrdma_dev *dev);
void ntrdma_dev_disable(struct ntrdma_dev *dev);

void ntrdma_dev_quiesce(struct ntrdma_dev *dev);

int ntrdma_dev_eth_init(struct ntrdma_dev *dev);
void ntrdma_dev_eth_deinit(struct ntrdma_dev *dev);

struct ntrdma_eth_conf_attr {
	ntrdma_u32_t			cap;
	ntrdma_u32_t			idx;
	ntrdma_dma_addr_t		buf_dma;
	ntrdma_dma_addr_t		idx_dma;
	ntrdma_u32_t			vbell_idx;
};

void ntrdma_dev_eth_conf_attr(struct ntrdma_dev *dev,
			      struct ntrdma_eth_conf_attr *attr);
int ntrdma_dev_eth_conf(struct ntrdma_dev *dev,
			struct ntrdma_eth_conf_attr *attr);
void ntrdma_dev_eth_deconf(struct ntrdma_dev *dev);

struct ntrdma_eth_enable_attr {
	ntrdma_dma_addr_t		buf_dma;
	ntrdma_dma_addr_t		idx_dma;
	ntrdma_u32_t			vbell_idx;
};

void ntrdma_dev_eth_enable_attr(struct ntrdma_dev *dev,
				struct ntrdma_eth_enable_attr *attr);
void ntrdma_dev_eth_enable(struct ntrdma_dev *dev,
			   struct ntrdma_eth_enable_attr *attr);
void ntrdma_dev_eth_disable(struct ntrdma_dev *dev);

static inline void *ntrdma_port_device(struct ntrdma_dev *dev)
{
	if (!dev->ops->port_device)
		return NULL;

	return dev->ops->port_device(dev);
}

static inline void ntrdma_port_enable(struct ntrdma_dev *dev)
{
	dev->ops->port_enable(dev);
}

static inline void ntrdma_port_disable(struct ntrdma_dev *dev)
{
	dev->ops->port_disable(dev);
}

static inline struct ntrdma_req *ntrdma_req_alloc(struct ntrdma_dev *dev,
						  int cap_hint)
{
	return dev->ops->port_req_alloc(dev, cap_hint);
}

static inline void ntrdma_req_free(struct ntrdma_req *req)
{
	req->dev->ops->port_req_free(req);
}

static inline void ntrdma_req_memcpy(struct ntrdma_req *req,
				     ntrdma_dma_addr_t dst,
				     ntrdma_dma_addr_t src,
				     ntrdma_size_t len)
{
	req->dev->ops->port_req_memcpy(req, dst, src, len);
}

static inline void ntrdma_req_imm32(struct ntrdma_req *req,
				    ntrdma_dma_addr_t dst,
				    ntrdma_u32_t val)
{
	req->dev->ops->port_req_imm32(req, dst, val);
}

static inline void ntrdma_req_imm64(struct ntrdma_req *req,
				    ntrdma_dma_addr_t dst,
				    ntrdma_u64_t val)
{
	req->dev->ops->port_req_imm64(req, dst, val);
}

static inline void ntrdma_req_fence(struct ntrdma_req *req)
{
	req->dev->ops->port_req_fence(req);
}

static inline void ntrdma_req_signal(struct ntrdma_req *req)
{
	req->dev->ops->port_req_signal(req);
}

static inline void ntrdma_req_submit(struct ntrdma_req *req)
{
	req->dev->ops->port_req_submit(req);
}

static inline void ntrdma_req_callback(struct ntrdma_req *req,
				       void (*cb)(void *cb_ctx),
				       void *cb_ctx)
{
	req->dev->ops->port_req_callback(req, cb, cb_ctx);
}

static inline bool ntrdma_throttle(struct ntrdma_dev *dev,
				   struct ntrdma_dma_cb *cb)
{
	if (!dev->ops->port_throttle)
		return false;

	return dev->ops->port_throttle(dev, cb);
}

#endif
