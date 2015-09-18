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
#include "ntrdma_dev.h"
#include "ntrdma_dma.h"

#define NTRDMA_DMA_HWM (1<<14)
#define NTRDMA_DMA_LWM (1<<13)

static void ntrdma_dma_incr_out(struct ntrdma_ntb_dev *dev);
static void ntrdma_dma_decr_out(struct ntrdma_ntb_dev *dev, int amount);
static inline void ntrdma_dma_req_free_common(struct ntrdma_ntb_dev *dev,
					      struct ntrdma_dma_req *req);

int ntrdma_dma_init(struct ntrdma_ntb_dev *dev)
{
	ntrdma_list_init(&dev->dma_req_free);
	ntrdma_list_init(&dev->dma_req_sent);
	ntrdma_spl_create(&dev->dma_lock, "ntrdma_dma_spl");

	dev->dma_hwm = NTRDMA_DMA_HWM;
	dev->dma_lwm = NTRDMA_DMA_LWM;
	dev->dma_out = 0;

	ntrdma_list_init(&dev->throttle_list);
	ntrdma_spl_create(&dev->throttle_lock, "throttle_lock");

	return 0;
}

void ntrdma_dma_deinit(struct ntrdma_ntb_dev *dev)
{
	ntrdma_spl_destroy(&dev->dma_lock);
}

static inline void ntrdma_dma_req_free_common(struct ntrdma_ntb_dev *dev,
					      struct ntrdma_dma_req *req)
{
	ntrdma_list_add_tail(&dev->dma_req_free, &req->entry);
}

void ntrdma_dma_req_free(struct ntrdma_dma_req *req)
{
	struct ntrdma_ntb_dev *dev;

	dev = dev_ndev(req->req.dev);

	ntrdma_vdbg(&dev->dev, "req %p\n", req);

	ntrdma_dma_impl_free_req(req->req_impl);

	ntrdma_spl_lock(&dev->dma_lock);
	{
		ntrdma_dma_req_free_common(dev, req);
	}
	ntrdma_spl_unlock(&dev->dma_lock);
}

static void ntrdma_dma_req_complete(void *ctx)
{
	struct ntrdma_ntb_dev *dev;
	struct ntrdma_dma_req *req;
	struct ntrdma_dma_cb *cb;

	req = ctx;
	dev = dev_ndev(req->req.dev);

	ntrdma_dma_decr_out(dev, req->dma_count);

	ntrdma_list_for_each_entry(cb, &req->cb_list,
				   struct ntrdma_dma_cb, entry)
		ntrdma_cb_call(&cb->cb);

	ntrdma_spl_lock(&dev->dma_lock);
	{
		ntrdma_list_del(&req->entry);
		ntrdma_dma_req_free_common(dev, req);
	}
	ntrdma_spl_unlock(&dev->dma_lock);
}

void ntrdma_dma_req_submit(struct ntrdma_dma_req *req)
{
	struct ntrdma_ntb_dev *dev;

	dev = dev_ndev(req->req.dev);

	if (req->next_valid) {
		++req->dma_count;
		ntrdma_dma_incr_out(dev);
		ntrdma_dma_impl_append_sge(req->req_impl, &req->next_sge);
	}

	ntrdma_spl_lock(&dev->dma_lock);
	{
		ntrdma_list_add_tail(&dev->dma_req_sent, &req->entry);
	}
	ntrdma_spl_unlock(&dev->dma_lock);

	ntrdma_dma_impl_submit_req(req->req_impl);
}

struct ntrdma_dma_req *ntrdma_dma_req_alloc(struct ntrdma_ntb_dev *dev, int hint)
{
	struct ntrdma_dma_req *req;

	ntrdma_spl_lock(&dev->dma_lock);
	{
		req = ntrdma_list_remove_first_entry(&dev->dma_req_free,
						     struct ntrdma_dma_req, entry);
	}
	ntrdma_spl_unlock(&dev->dma_lock);

	if (!req) {
		req = ntrdma_malloc_atomic(sizeof(*req), dev->dev.node);

		if (!req)
			goto err_req;

		req->req.dev = &dev->dev;

#ifdef CONFIG_NTRDMA_IMM_BUF
		req->imm_buf = ntrdma_malloc_coherent(
						      ntrdma_port_device(&dev->dev),
						      NTRDMA_DMA_IMM_SIZE, &req->imm_dma);
		if (!req->imm_buf) {
			ntrdma_free(req);
			goto err_req;
		}

		req->imm_size = NTRDMA_DMA_IMM_SIZE;
#endif
	}

	req->next_valid = 0;

	req->req_impl = ntrdma_dma_impl_alloc_req(dev->dma_impl,
						  ntrdma_dma_req_complete, req, hint);
	if (!req->req_impl)
		goto err_impl;

	ntrdma_list_init(&req->cb_list);
	req->dma_count = 0;

#ifdef CONFIG_NTRDMA_IMM_BUF
	req->imm_off = 0;
#endif

	ntrdma_vdbg(req->req.dev, "req %p\n", req);

	return req;

err_impl:
	ntrdma_spl_lock(&dev->dma_lock);
	{
		ntrdma_list_add_tail(&dev->dma_req_free, &req->entry);
	}
	ntrdma_spl_unlock(&dev->dma_lock);
err_req:
	return NULL;
}

void ntrdma_dma_req_memcpy(struct ntrdma_dma_req *req, ntrdma_dma_addr_t dst,
			   ntrdma_dma_addr_t src, ntrdma_size_t len)
{
	struct ntrdma_ntb_dev *dev = dev_ndev(req->req.dev);

	ntrdma_vdbg(req->req.dev, "req %p dst %llx src %llx len %lx\n", req, dst, src, len);

#ifdef CONFIG_NTRDMA_SIMULATION
	if (dst >> 48 == src >> 48) {
		ntrdma_pr("GOING TO ASSERT: dst %llx src %llx\n", dst, src);
		NTRDMA_HARD_ASSERT(0);
	}
#endif

	if (req->next_valid) {
		++req->dma_count;
		ntrdma_dma_incr_out(dev);
		ntrdma_dma_impl_append_sge(req->req_impl, &req->next_sge);
	}

	req->next_sge.dst = dst;
	req->next_sge.src = src;
	req->next_sge.len = len;
	req->next_sge.flags = 0;
	req->next_valid = 1;
}

#ifdef CONFIG_NTRDMA_IMM_BUF

void ntrdma_dma_req_imm32(struct ntrdma_dma_req *req,
			  ntrdma_dma_addr_t dst, ntrdma_u32_t val)
{
	ntrdma_dma_addr_t src;

	ntrdma_vdbg(req->req.dev, "req %p dst %#llx val %#x\n", req, dst, val);

	if (req->imm_size < req->imm_off + sizeof(val))
		return;

	*(ntrdma_u32_t *)(req->imm_buf + req->imm_off) = val;
	src = req->imm_dma + req->imm_off;

	ntrdma_dma_req_memcpy(req, dst, src, sizeof(val));

	req->imm_off += sizeof(val);
}

void ntrdma_dma_req_imm64(struct ntrdma_dma_req *req,
			  ntrdma_dma_addr_t dst, ntrdma_u64_t val)
{
	ntrdma_dma_addr_t src;

	ntrdma_vdbg(req->req.dev, "req %p dst %#llx val %#llx\n", req, dst, val);

	if (req->imm_size < req->imm_off + sizeof(val))
		return;

	*(ntrdma_u64_t *)(req->imm_buf + req->imm_off) = val;
	src = req->imm_dma + req->imm_off;

	ntrdma_dma_req_memcpy(req, dst, src, sizeof(val));

	req->imm_off += sizeof(val);
}

#else

void ntrdma_dma_req_imm32(struct ntrdma_dma_req *req,
			  ntrdma_dma_addr_t dst, ntrdma_u32_t val)
{
	struct ntrdma_ntb_dev *dev = dev_ndev(req->req.dev);

	ntrdma_vdbg(req->req.dev, "req %p dst %#llx val %#x\n", req, dst, val);

	if (req->next_valid) {
		++req->dma_count;
		ntrdma_dma_incr_out(dev);
		ntrdma_dma_impl_append_sge(req->req_impl, &req->next_sge);
		req->next_valid = 0;
	}

	++req->dma_count;
	ntrdma_dma_incr_out(dev);
	ntrdma_dma_impl_req_append_imm(req->req_impl,
				       dst, &val, sizeof(val),
				       NTRDMA_DMA_REQ_FENCE);
}

void ntrdma_dma_req_imm64(struct ntrdma_dma_req *req,
			  ntrdma_dma_addr_t dst, ntrdma_u64_t val)
{
	struct ntrdma_ntb_dev *dev = dev_ndev(req->req.dev);

	ntrdma_vdbg(req->req.dev, "req %p dst %#llx val %#llx\n", req, dst, val);

	if (req->next_valid) {
		++req->dma_count;
		ntrdma_dma_incr_out(dev);
		ntrdma_dma_impl_append_sge(req->req_impl, &req->next_sge);
		req->next_valid = 0;
	}

	++req->dma_count;
	ntrdma_dma_incr_out(dev);
	ntrdma_dma_impl_req_append_imm(req->req_impl,
				       dst, &val, sizeof(val),
				       NTRDMA_DMA_REQ_FENCE);
}

#endif

/* Add a fence between dependent memcpy operations. */
void ntrdma_dma_req_fence(struct ntrdma_dma_req *req)
{
	req->next_sge.flags |= NTRDMA_DMA_REQ_FENCE;
}

/* Add a dma completion callback to the list for this request. */
void ntrdma_dma_req_callback(struct ntrdma_dma_req *req, struct ntrdma_dma_cb *cb)
{
	ntrdma_list_add_tail(&req->cb_list, &cb->entry);
}

/* Throttle dma requests if there are many are outstanding. */
bool ntrdma_dma_throttle(struct ntrdma_ntb_dev *dev, struct ntrdma_dma_cb *cb)
{
	bool throttle;

	ntrdma_spl_lock(&dev->throttle_lock);
	{
		throttle = dev->dma_out >= dev->dma_hwm;
		if (!throttle)
			goto out;

		ntrdma_vdbg(&dev->dev, "throttle cb %p\n", cb);
		ntrdma_list_add_tail(&dev->throttle_list, &cb->entry);

	}
out:
	ntrdma_spl_unlock(&dev->throttle_lock);

	return throttle;
}

static void ntrdma_dma_incr_out(struct ntrdma_ntb_dev *dev)
{
	ntrdma_spl_lock(&dev->throttle_lock);
	{
		++dev->dma_out;
	}
	ntrdma_spl_unlock(&dev->throttle_lock);
}

static void ntrdma_dma_decr_out(struct ntrdma_ntb_dev *dev, int amount)
{
	struct ntrdma_dma_cb *cb;
	bool throttle;

	ntrdma_spl_lock(&dev->throttle_lock);
	{
		dev->dma_out -= amount;

		throttle = dev->dma_out >= dev->dma_lwm;
		if (throttle)
			goto out;

		ntrdma_list_for_each_entry(cb, &dev->throttle_list,
					   struct ntrdma_dma_cb, entry) {
			ntrdma_vdbg(&dev->dev, "unthrottle cb %p\n", cb);
			ntrdma_cb_call(&cb->cb);
		}

		ntrdma_list_init(&dev->throttle_list);
	}
out:
	ntrdma_spl_unlock(&dev->throttle_lock);
}
