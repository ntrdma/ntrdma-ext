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

#include "ntrdma_os.h"
#include "ntrdma_mem.h"

#include "ntrdma_dev.h"
#include "ntrdma_ntb.h"
#include "ntrdma_port.h"
#include "ntrdma_port_v1.h"
#include "ntrdma_ping.h"
#include "ntrdma_dma.h"

#include "ntrdma_ntb_impl.h"

static void *ntrdma_ntb_port_device(struct ntrdma_dev *dev);
static void ntrdma_ntb_port_enable(struct ntrdma_dev *dev);
static void ntrdma_ntb_port_disable(struct ntrdma_dev *dev);
static struct ntrdma_req *ntrdma_ntb_port_req_alloc(struct ntrdma_dev *dev,
						    int cap_hint);
static void ntrdma_ntb_port_req_free(struct ntrdma_req *req);
static void ntrdma_ntb_port_req_submit(struct ntrdma_req *req);
static void ntrdma_ntb_port_req_memcpy(struct ntrdma_req *req,
				       ntrdma_dma_addr_t dst,
				       ntrdma_dma_addr_t src,
				       ntrdma_size_t len);
static void ntrdma_ntb_port_req_imm32(struct ntrdma_req *req,
				      ntrdma_dma_addr_t dst,
				      ntrdma_u32_t val);
static void ntrdma_ntb_port_req_imm64(struct ntrdma_req *req,
				      ntrdma_dma_addr_t dst,
				      ntrdma_u64_t val);
static void ntrdma_ntb_port_req_signal(struct ntrdma_req *req);
static void ntrdma_ntb_port_req_fence(struct ntrdma_req *req);
static void ntrdma_ntb_port_req_callback(struct ntrdma_req *req,
					 void (*cb)(void *cb_ctx),
					 void *cb_ctx);
static bool ntrdma_ntb_port_throttle(struct ntrdma_dev *dev,
				     struct ntrdma_dma_cb *cb);

static const struct ntrdma_dev_ops ntrdma_ntb_dev_ops = {
	.port_device			= ntrdma_ntb_port_device,
	.port_enable			= ntrdma_ntb_port_enable,
	.port_disable			= ntrdma_ntb_port_disable,
	.port_req_alloc			= ntrdma_ntb_port_req_alloc,
	.port_req_free			= ntrdma_ntb_port_req_free,
	.port_req_submit		= ntrdma_ntb_port_req_submit,
	.port_req_memcpy		= ntrdma_ntb_port_req_memcpy,
	.port_req_imm32			= ntrdma_ntb_port_req_imm32,
	.port_req_imm64			= ntrdma_ntb_port_req_imm64,
	.port_req_signal		= ntrdma_ntb_port_req_signal,
	.port_req_fence			= ntrdma_ntb_port_req_fence,
	.port_req_callback		= ntrdma_ntb_port_req_callback,
	.port_throttle			= ntrdma_ntb_port_throttle,
};

static void ntrdma_ntb_link_event(void *ctx);
static void ntrdma_ntb_bell_event(void *ctx);

static const struct ntrdma_ntb_impl_ops ntrdma_ntb_impl_ops = {
	.link_event			= ntrdma_ntb_link_event,
	.db_event			= ntrdma_ntb_bell_event,
};

int ntrdma_ntb_probe(void *ntb_impl, void *dma_impl)
{
	struct ntrdma_ntb_dev *dev;
	int err;

	ntrdma_ntb_link_disable(ntb_impl);

	/* Note: because of the way ib_alloc_device works, a direct cast is used
	 * here instead of container_of.  The containing structure MUST be the same
	 * pointer as the ib_device member, which MUST be the first member.
	 */
	dev = (void *)ib_alloc_device(sizeof(*dev));
	if (!dev) {
		ntrdma_pr("malloc failed\n");
		err = -ENOMEM;
		goto err_dev;
	}

	dev->dev.node = ntrdma_ntb_impl_node(ntb_impl);

	dev->dev.ops = &ntrdma_ntb_dev_ops;
	dev->ntb_impl = ntb_impl;
	dev->dma_impl = dma_impl;

	ntrdma_dbg(&dev->dev, "dev %p ntb_impl %p dma_impl %p\n",
		   dev, ntb_impl, dma_impl);

	err = ntrdma_dma_init(dev);
	if (err) {
		ntrdma_dbg(&dev->dev, "dma failed\n");
		goto err_dma;
	}

	err = ntrdma_ping_init(dev);
	if (err) {
		ntrdma_dbg(&dev->dev, "ping failed\n");
		goto err_ping;
	}

	err = ntrdma_ntb_port_init(dev);
	if (err) {
		ntrdma_dbg(&dev->dev, "port failed\n");
		goto err_port;
	}

	err = ntrdma_dev_init(&dev->dev);
	if (err) {
		ntrdma_dbg(&dev->dev, "init failed\n");
		goto err_init;
	}

	ntrdma_ntb_impl_set_ctx(dev->ntb_impl, dev, &ntrdma_ntb_impl_ops);

	/* TODO: register device here instead of in dev_init */

	ntrdma_ntb_link_enable(dev->ntb_impl);
	ntrdma_ntb_link_event(dev);

	return 0;

	//ntrdma_dev_deinit(dev);
err_init:
	ntrdma_ntb_port_deinit(dev);
err_port:
	ntrdma_ping_deinit(dev);
err_ping:
	ntrdma_dma_deinit(dev);
err_dma:
	ib_dealloc_device((void *)dev);
err_dev:
	ntrdma_pr("failed, returning err %d\n", err);
	return err;
}

void ntrdma_ntb_remove(void *ntb_impl)
{
	struct ntrdma_ntb_dev *dev;

	dev = ntrdma_ntb_impl_get_ctx(ntb_impl);

	ntrdma_dbg(&dev->dev, "ntb_impl %p\n", ntb_impl);
	ntrdma_dbg(&dev->dev, "dev %p ntb_impl %p dma_impl %p\n", dev,
		   dev->ntb_impl, dev->dma_impl);

	/* TODO: unregister device here instead of in dev_deinit */

	ntrdma_ntb_link_disable(dev->ntb_impl);
	ntrdma_dev_quiesce(&dev->dev);
	ntrdma_ntb_impl_set_ctx(dev->ntb_impl, NULL, NULL);

	ntrdma_dev_deinit(&dev->dev);
	ntrdma_ntb_port_deinit(dev);
	ntrdma_ping_deinit(dev);
	ntrdma_dma_deinit(dev);
	ib_dealloc_device((void *)dev);
}

static void *ntrdma_ntb_port_device(struct ntrdma_dev *dev)
{
	return ntrdma_ntb_impl_get_device(dev_ndev(dev)->ntb_impl);
}

static void ntrdma_ntb_port_enable(struct ntrdma_dev *dev)
{
	ntrdma_ntb_link_enable(dev_ndev(dev)->ntb_impl);
}

static void ntrdma_ntb_port_disable(struct ntrdma_dev *dev)
{
	ntrdma_ntb_link_disable(dev_ndev(dev)->ntb_impl);
}

static struct ntrdma_req *ntrdma_ntb_port_req_alloc(struct ntrdma_dev *dev,
						    int cap_hint)
{
	struct ntrdma_dma_req *dreq;

	dreq = ntrdma_dma_req_alloc(dev_ndev(dev), cap_hint);

	return &dreq->req;
}

static void ntrdma_ntb_port_req_free(struct ntrdma_req *req)
{
	ntrdma_dma_req_free(req_dreq(req));
}

static void ntrdma_ntb_port_req_submit(struct ntrdma_req *req)
{
	ntrdma_dma_req_submit(req_dreq(req));
}

static void ntrdma_ntb_port_req_memcpy(struct ntrdma_req *req,
				       ntrdma_dma_addr_t dst,
				       ntrdma_dma_addr_t src,
				       ntrdma_size_t len)
{
	ntrdma_dma_req_memcpy(req_dreq(req), dst, src, len);
}

static void ntrdma_ntb_port_req_imm32(struct ntrdma_req *req,
				      ntrdma_dma_addr_t dst,
				      ntrdma_u32_t val)
{
	ntrdma_dma_req_imm32(req_dreq(req), dst, val);
}

static void ntrdma_ntb_port_req_imm64(struct ntrdma_req *req,
				      ntrdma_dma_addr_t dst,
				      ntrdma_u64_t val)
{
	ntrdma_dma_req_imm64(req_dreq(req), dst, val);
}

static void ntrdma_ntb_port_req_signal(struct ntrdma_req *req)
{
	struct ntrdma_ntb_dev *ndev = dev_ndev(req->dev);

	ntrdma_ntb_port_req_fence(req);
	ntrdma_ntb_port_req_imm32(req, ndev->peer_msi_dma,
				  ndev->peer_msi_val);
}

static void ntrdma_ntb_port_req_fence(struct ntrdma_req *req)
{
	ntrdma_dma_req_fence(req_dreq(req));
}

static void ntrdma_ntb_port_req_callback(struct ntrdma_req *req,
					 void (*cb)(void *cb_ctx),
					 void *cb_ctx)
{
	struct ntrdma_dma_cb *dma_cb;

	dma_cb = ntrdma_malloc_atomic(sizeof(*dma_cb), req->dev->node);
	if (WARN_ON(!dma_cb))
		return;

	ntrdma_cb_init(&dma_cb->cb, cb, cb_ctx);

	ntrdma_dma_req_callback(req_dreq(req), dma_cb);
}

static bool ntrdma_ntb_port_throttle(struct ntrdma_dev *dev,
				     struct ntrdma_dma_cb *cb)
{
	return ntrdma_dma_throttle(dev_ndev(dev), cb);
}

static void ntrdma_ntb_link_event(void *ctx)
{
	struct ntrdma_ntb_dev *dev = ctx;

	ntrdma_port_link_event(dev);
}

static void ntrdma_ntb_bell_event(void *ctx)
{
	struct ntrdma_ntb_dev *dev = ctx;

	ntrdma_dev_vbell_event(&dev->dev);
}
