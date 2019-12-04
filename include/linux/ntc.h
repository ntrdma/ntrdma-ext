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

#ifndef _NTC_H_
#define _NTC_H_

/* Non-Transparent Channel */

#include <linux/ntc_mm.h>
#include <linux/device.h>
#include <linux/dmaengine.h>
#include <linux/dma-direction.h>
#include <linux/rwlock.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <rdma/ib_verbs.h>

#include "linux/ntc_trace.h"
#define NTB_MAX_IRQS (64)
#define SYNC_RESET 1
#define ASYNC_RESET 0

#define NTC_INFO_MW_IDX 0
#define NTC_DRAM_MW_IDX 1
#define NTC_MAX_NUM_MWS 2

#define NTC_MAX_DMA_CHANS 8

struct ntc_driver;
struct ntc_dev;

struct ib_ucontext;
struct ib_umem;

#ifndef iowrite64
#ifdef writeq
#define iowrite64 writeq
#else
#define iowrite64 __lame_iowrite64
static inline void __lame_iowrite64(u64 val, void __iomem *ptr)
{
	iowrite32(val, ptr);
	iowrite32(val >> 32, ptr + sizeof(u32));
}
#endif
#endif

enum ntc_dma_chan_type {
	NTC_DEV_DMA_CHAN,
	NTC_ETH_DMA_CHAN,
	NTC_QP_DMA_CHAN,
	NTC_NUM_DMA_CHAN_TYPES
};

/**
 * struct ntc_driver_ops - ntc driver operations
 * @probe:		Notify driver of a new device.
 * @remove:		Notify driver to remove a device.
 */
struct ntc_driver_ops {
	int (*probe)(struct ntc_driver *driver, struct ntc_dev *ntc);
	void (*remove)(struct ntc_driver *driver, struct ntc_dev *ntc);
};

static inline int ntc_driver_ops_is_valid(const struct ntc_driver_ops *ops)
{
	/* commented callbacks are not required: */
	return
		ops->probe			&&
		ops->remove			&&
		1;
}

/**
 * struct ntc_ctx_ops - ntc driver context operations
 * @hello:		See ntc_ctx_hello().
 * @enable:		See ntc_ctx_enable().
 * @disable:		See ntc_ctx_disable().
 * @quiesce:		See ntc_ctx_quiesce().
 * @reset:		See ntc_ctx_reset().
 * @signal:		See ntc_ctx_signal().
 */
struct ntc_ctx_ops {
	int (*hello)(void *ctx, int phase,
			 void *in_buf, size_t in_size,
			 void *out_buf, size_t out_size);
	int (*enable)(void *ctx);
	void (*disable)(void *ctx);
	void (*quiesce)(void *ctx);
	void (*reset)(void *ctx);
	void (*signal)(void *ctx, int vec);
};

static inline int ntc_ctx_ops_is_valid(const struct ntc_ctx_ops *ops)
{
	/* commented callbacks are not required: */
	return
		ops->hello			&&
		ops->enable			&&
		ops->disable			&&
		/* ops->quiesce			&& */
		/* ops->reset			&& */
		/* ops->signal			&& */
		1;
}

union ntc_chan_addr {
	struct {
		u64 offset:62;
		u8  mw_idx:1;
		u8  one:1;
	};
	u64 value;
};

struct ntc_own_mw {
	struct ntc_dev *ntc;
	struct device *ntb_dev;
	dma_addr_t base;
	void *base_ptr;
	resource_size_t size;
	struct ntc_mm mm;
	u8 mw_idx;
	bool is_flat;
};

struct ntc_peer_mw {
	struct ntc_dev *ntc;
	phys_addr_t base;
	resource_size_t size;
	void __iomem *base_ptr;
	u8 mw_idx;
};

struct ntc_local_buf {
	u64 size:62;
	bool owned:1;
	bool mapped:1;
	void *ptr;
	dma_addr_t dma_addr;
};

struct ntc_export_buf {
	u64 size;
	void *ptr;
	struct ntc_own_mw *own_mw;
	dma_addr_t dma_addr;
};

struct ntc_mr_buf {
	u64 size;
	struct ntc_own_mw *own_mw;
	dma_addr_t dma_addr;
	int mr_access_flags;
};

struct ntc_remote_buf_desc {
	union ntc_chan_addr chan_addr;
	u64 size;
};

struct ntc_remote_buf {
	u64 size;
	dma_addr_t dma_addr;
};

struct ntc_dma_chan {
	struct ntc_dev *ntc;
	struct dma_chan *chan;
	dma_cookie_t last_cookie;
	dma_cookie_t submit_cookie;
	u32 submit_counter;
};

/**
 * struct ntc_driver - driver interested in ntc devices
 * @drv:		Linux driver object.
 * @ops:		See &ntc_driver_ops.
 */
struct ntc_driver {
	struct device_driver		drv;
	const struct ntc_driver_ops	ops;
};

#define ntc_of_driver(__drv) container_of(__drv, struct ntc_driver, drv)

/**
 * struct ntc_device - ntc device
 * @dev:		Linux device object.
 * @ctx_ops:		See &ntc_ctx_ops.
 */
struct ntc_dev {
	struct device			dev;
	struct device			*ntb_dev;
	struct ntc_dma_chan		dma_chan[NTC_MAX_DMA_CHANS];
	atomic_t		dma_chan_rr_index[NTC_NUM_DMA_CHAN_TYPES];
	struct device			*dma_engine_dev;
	const struct ntc_ctx_ops	*ctx_ops;
	struct ntc_own_mw		own_mws[NTC_MAX_NUM_MWS];
	struct ntc_peer_mw		peer_mws[NTC_MAX_NUM_MWS];

	int				peer_irq_num;

	/* negotiated protocol version for ntc */
	int				version;
	u32				latest_version;

	bool				link_is_up;
};

#define ntc_of_dev(__dev) container_of(__dev, struct ntc_dev, dev)

static inline int ntc_max_peer_irqs(struct ntc_dev *ntc)
{
	return ntc->peer_irq_num;
}

const void *ntc_local_hello_buf(struct ntc_dev *ntc, int *size);
void *ntc_peer_hello_buf(struct ntc_dev *ntc, int *size);

static inline int ntc_door_bell_arbitrator(struct ntc_dev *ntc)
{
	return smp_processor_id();
}

#define NTB_DEFAULT_VEC(__NTC) ntc_door_bell_arbitrator(__NTC)

struct bus_type *ntc_bus_ptr(void);

/**
 * ntc_register_driver() - register a driver for interest in ntc devices
 * @driver:	Client context.
 *
 * The driver will be added to the list of drivers interested in ntc devices.
 * The driver will be notified of any ntc devices that are not already
 * associated with a driver, or if ntc devices are registered later.
 *
 * Return: Zero if the driver is registered, otherwise an error number.
 */
#define ntc_register_driver(driver) \
	__ntc_register_driver(driver, THIS_MODULE, KBUILD_MODNAME)

static inline int __ntc_register_driver(struct ntc_driver *driver,
					struct module *mod,
					const char *mod_name)
{
	if (!ntc_driver_ops_is_valid(&driver->ops))
		return -EINVAL;

	pr_devel("register driver %s\n", driver->drv.name);

	driver->drv.bus = ntc_bus_ptr();
	driver->drv.name = mod_name;
	driver->drv.owner = mod;

	return driver_register(&driver->drv);
}

/**
 * ntc_unregister_driver() - unregister a driver for interest in ntc devices
 * @driver:	Client context.
 *
 * The driver will be removed from the list of drivers interested in ntc
 * devices.  If any ntc devices are associated with the driver, the driver will
 * be notified to remove those devices.
 */
static inline void ntc_unregister_driver(struct ntc_driver *driver)
{
	pr_devel("unregister driver %s\n", driver->drv.name);

	driver_unregister(&driver->drv);
}

#define module_ntc_driver(__ntc_driver) \
	module_driver(__ntc_driver, ntc_register_driver, \
		      ntc_unregister_driver)

/**
 * ntc_get_ctx() - get the driver context associated with an ntc device
 * @ntc:	Device context.
 *
 * Get the driver context associated with an ntc device.
 *
 * Return: The associated context, or NULL.
 */
static inline void *ntc_get_ctx(struct ntc_dev *ntc)
{
	return dev_get_drvdata(&ntc->dev);
}

/**
 * ntc_set_ctx() - associate a driver context with an ntc device
 * @ntc:	Device context.
 * @ctx:	Driver context.
 * @ctx_ops:	Driver context operations.
 *
 * Associate a driver context and operations with a ntc device.  The context is
 * provided by the driver driver, and the driver may associate a different
 * context with each ntc device.
 *
 * Return: Zero if the context is associated, otherwise an error number.
 */
static inline int ntc_set_ctx(struct ntc_dev *ntc, void *ctx,
			const struct ntc_ctx_ops *ctx_ops)
{
	if (ntc_get_ctx(ntc))
		return -EINVAL;

	if (!ctx_ops)
		return -EINVAL;
	if (!ntc_ctx_ops_is_valid(ctx_ops))
		return -EINVAL;

	dev_vdbg(&ntc->dev, "set ctx\n");

	dev_set_drvdata(&ntc->dev, ctx);
	wmb(); /* if ctx_ops is set, drvdata must be set */
	ntc->ctx_ops = ctx_ops;

	return 0;
}

/**
 * ntc_clear_ctx() - disassociate any driver context from an ntc device
 * @ntc:	Device context.
 *
 * Clear any association that may exist between a driver context and the ntc
 * device.
 */
static inline void ntc_clear_ctx(struct ntc_dev *ntc)
{
	dev_vdbg(&ntc->dev, "clear ctx\n");

	ntc->ctx_ops = NULL;
	wmb(); /* if ctx_ops is set, drvdata must be set */
	dev_set_drvdata(&ntc->dev, NULL);
}

/**
 * ntc_is_link_up() - check the link
 * @ntc:	Device context.
 *
 * check logical link status
 *
 * Return: Zero on success, otherwise an error number.
 */
static inline int ntc_is_link_up(struct ntc_dev *ntc)
{
	return ntc->link_is_up;
}

/**
 * ntc_query_version() - check the link
 * @ntc:	Device context.
 *
 * check logical link status
 *
 * Return: Zero on success, otherwise an error number.
 */
static inline int ntc_query_version(struct ntc_dev *ntc)
{
	return ntc->latest_version;
}

/**
 * ntc_link_disable() - disable the link
 * @ntc:	Device context.
 *
 * Disable the link, tear down any active connection, and disable negotiating a
 * connection with the peer.
 *
 * Return: Zero on success, otherwise an error number.
 */
int _ntc_link_disable(struct ntc_dev *ntc, const char *f);
#define ntc_link_disable(_ntc) _ntc_link_disable(_ntc, __func__)

/**
 * ntc_link_enable() - enable the link
 * @ntc:	Device context.
 *
 * Enable the link to negotiate a connection with the peer.
 *
 * Return: Zero on success, otherwise an error number.
 */
int ntc_link_enable(struct ntc_dev *ntc);

/**
 * ntc_link_reset() - reset the link
 * @ntc:	Device context.
 * @wait	Sync reset.
 *
 * Tear down any active connection and coordinate renegotiating the connection
 * with the peer.
 *
 * Return: Zero on success, otherwise an error number.
 */
int _ntc_link_reset(struct ntc_dev *ntc, bool wait, const char *f);
#define ntc_link_reset(_ntc, _wait) _ntc_link_reset(_ntc, _wait, __func__)

static inline struct ntc_peer_mw *ntc_peer_mw(struct ntc_dev *ntc,
					const union ntc_chan_addr *chan_addr)
{
	if (unlikely(chan_addr->mw_idx >= (u8)NTC_MAX_NUM_MWS)) {
		dev_err(&ntc->dev, "Unsupported MW %d", chan_addr->mw_idx);
		return NULL;
	}

	return &ntc->peer_mws[chan_addr->mw_idx];
}

/**
 * ntc_peer_addr() - transform a channel-mapped address into a peer address
 * @ntc:	Device context.
 * @addr:	Channel-mapped address of a remote buffer.
 *
 * Transform a channel-mapped address of a remote buffer, mapped by the peer,
 * into a peer address to be used as the destination of a channel request.
 *
 * The peer allocates and maps the buffer with ntc_export_buf_alloc()
 * or ntc_mr_buf_map_dma().
 * Then, the remote peer communicates the channel-mapped address of the buffer
 * across the channel.  The driver on side of the connection must then
 * translate the peer's channel-mapped address into a peer address.  The
 * translated peer address may then be used as the destination address of a
 * channel request.
 *
 * Return: The translated peer address.
 */
static inline phys_addr_t ntc_peer_addr(struct ntc_dev *ntc,
					const union ntc_chan_addr *chan_addr)
{
	struct ntc_peer_mw *peer_mw = ntc_peer_mw(ntc, chan_addr);
	u64 offset = chan_addr->offset;

	if (unlikely(!peer_mw))
		return 0;

	if (unlikely(offset >= peer_mw->size)) {
		dev_err(&ntc->dev, "offset %#llx is beyond memory size %#llx\n",
			offset, peer_mw->size);
		return 0;
	}

	return peer_mw->base + offset;
}

void ntc_init_dma_chan(struct ntc_dma_chan **dma_chan,
		struct ntc_dev *ntc, enum ntc_dma_chan_type type);

static inline struct device *ntc_dma_chan_dev(struct ntc_dma_chan *dma_chan)
{
	return dma_chan->chan->device->dev;
}

static inline void ntc_dma_chan_tx_status(struct ntc_dma_chan *chan)
{
	struct dma_tx_state txstate;

	chan->submit_counter = 0;
	txstate.used = 0;
	dmaengine_tx_status(chan->chan, chan->submit_cookie,
			&txstate);
	chan->submit_cookie = txstate.used;
}

/**
 * ntc_req_submit() - submit a channel request
 * @ntc:	Device context.
 * @chan:	Channel request context.
 *
 * Submit a channel request for processing.  Ownership of the request is passed
 * from the caller back to the channel, and the caller no longer holds a valid
 * reference to the request.
 *
 * Some channel implementations may submit operations as soon as they are
 * appended to a request, therefore the driver must assume that it is possible
 * for operations to begin as soon as they are appended to the request, prior
 * to submitting the request.  The submit function exists to provide a portable
 * interface to use the varying underlying channel implementations most
 * efficiently.
 *
 * Return: Zero on success, othewise an error number.
 */
static inline int ntc_req_submit(struct ntc_dev *ntc, struct ntc_dma_chan *chan)
{
	dma_async_issue_pending(chan->chan);

	if (++chan->submit_counter >= 0xff)
		ntc_dma_chan_tx_status(chan);

	return 0;
}

/**
 * ntc_req_memcpy() - append a buffer to buffer memory copy operation
 * @chan:	Channel request context.
 * @dst:	Destination channel address.
 * @src:	Source channel address.
 * @len:	Number of bytes to copy.
 * @fence:	Fence after this operation.
 * @cb:		Callback after this operation.
 * @cb_ctx:	Callback context.
 *
 * Append an asynchronous memory copy operation from a local source to a remote
 * destination buffer.  The amount of data is detemined by len.
 *
 * The destination address refers to a remote buffer, by the channel-mapped and
 * peer translated address.  See ntc_peer_addr().
 *
 * The source address refers to a local buffer by its channel-mapped address.
 *
 * A fence implies that processing of following operations must not proceed
 * until the completion of this memory copy operation (i.e. fence-after, not
 * fence-before).  This meaning of fence is derived from DMA_PREP_FENCE of the
 * dmaengine drivers.
 *
 * The callback, if specified, is called on completion of the request, with the
 * callback context as its only parameter.  The callback does not indicate
 * success or failure of the memory copy operation, only that the operation has
 * completed.  To determine success or failure of the operation, upper layer
 * drivers may implement an acknowledgment protocol, but that is out of the
 * scope of this interface.
 *
 * Return: Zero on success, othewise an error number.
 */
static inline int ntc_req_memcpy(struct ntc_dma_chan *chan,
				dma_addr_t dst, dma_addr_t src, u64 len,
				bool fence, void (*cb)(void *cb_ctx),
				void *cb_ctx)
{
	struct dma_async_tx_descriptor *tx;
	int flags = fence ? DMA_PREP_FENCE : 0;

	if (unlikely(!len))
		return -EINVAL;

	do { /* not a loop */

		tx = dmaengine_prep_dma_memcpy(chan->chan, dst, src, len,
					flags);
		if (tx)
			break;

		pr_warn("DMA ring is full for len %#llx. taking status.", len);
		ntc_dma_chan_tx_status(chan);

		tx = dmaengine_prep_dma_memcpy(chan->chan, dst, src, len,
					flags);
		if (tx)
			break;

		pr_warn("DMA ring still full. len %#llx. waiting", len);
		/* Busy waiting */
		dma_sync_wait(chan->chan, chan->last_cookie);

		tx = dmaengine_prep_dma_memcpy(chan->chan, dst, src, len,
					flags);
		if (tx)
			break;

		pr_err("DMA ring still full. len %#llx. Give up.", len);
		return -ENOMEM;
	} while (false);

	tx->callback = cb;
	tx->callback_param = cb_ctx;

	this_cpu_add(chan->chan->local->bytes_transferred, len);
	this_cpu_inc(chan->chan->local->memcpy_count);

	if (dma_submit_error(chan->last_cookie = dmaengine_submit(tx)))
		return -EIO;

	return 0;
}

static inline bool ntc_segment_valid(u64 buf_size, u64 buf_offset, u64 len)
{
	if (unlikely(buf_offset >= buf_size))
		return false;
	if (unlikely(len > buf_size))
		return false;
	if (unlikely(buf_offset + len > buf_size))
		return false;

	return true;
}

static inline
int _ntc_request_memcpy(struct ntc_dma_chan *chan,
			dma_addr_t dst_start, u64 dst_size, u64 dst_offset,
			dma_addr_t src_start, u64 src_size, u64 src_offset,
			u64 len, bool fence,
			void (*cb)(void *cb_ctx), void *cb_ctx)
{
	return ntc_req_memcpy(chan,
			dst_start + dst_offset, src_start + src_offset, len,
			fence, cb, cb_ctx);
}

static inline
void ntc_prepare_to_copy(struct ntc_dma_chan *chan, dma_addr_t dma_addr, u64 len)
{
	dma_sync_single_for_device(ntc_dma_chan_dev(chan), dma_addr, len,
				DMA_TO_DEVICE);
}

static inline int ntc_request_memcpy_with_cb(struct ntc_dma_chan *chan,
					const struct ntc_remote_buf *dst,
					u64 dst_offset,
					const struct ntc_local_buf *src,
					u64 src_offset,
					u64 len,
					void (*cb)(void *cb_ctx), void *cb_ctx)
{
	if (unlikely(len == 0))
		return 0;

	if (unlikely(!ntc_segment_valid(src->size, src_offset, len)))
		return -EINVAL;

	if (unlikely(!ntc_segment_valid(dst->size, dst_offset, len)))
		return -EINVAL;

	ntc_prepare_to_copy(chan, src->dma_addr + src_offset, len);

	return _ntc_request_memcpy(chan,
				dst->dma_addr, dst->size, dst_offset,
				src->dma_addr, src->size, src_offset,
				len, false, cb, cb_ctx);
}

static inline int ntc_request_memcpy(struct ntc_dma_chan *chan,
				const struct ntc_remote_buf *dst,
				u64 dst_offset,
				const struct ntc_local_buf *src,
				u64 src_offset,
				u64 len, bool fence)
{
	if (unlikely(!ntc_segment_valid(src->size, src_offset, len)))
		return -EINVAL;

	if (unlikely(!ntc_segment_valid(dst->size, dst_offset, len)))
		return -EINVAL;

	ntc_prepare_to_copy(chan, src->dma_addr + src_offset, len);

	return _ntc_request_memcpy(chan,
				dst->dma_addr, dst->size, dst_offset,
				src->dma_addr, src->size, src_offset,
				len, fence, NULL, NULL);
}

static inline int ntc_request_memcpy_fenced(struct ntc_dma_chan *chan,
					const struct ntc_remote_buf *dst,
					u64 dst_offset,
					const struct ntc_local_buf *src,
					u64 src_offset,
					u64 len)
{
	return ntc_request_memcpy(chan, dst, dst_offset, src, src_offset, len,
				true);
}

static inline int ntc_request_memcpy_unfenced(struct ntc_dma_chan *chan,
					const struct ntc_remote_buf *dst,
					u64 dst_offset,
					const struct ntc_local_buf *src,
					u64 src_offset,
					u64 len)
{
	return ntc_request_memcpy(chan, dst, dst_offset, src, src_offset, len,
				false);
}

static inline
int ntc_mr_request_memcpy_unfenced(struct ntc_dma_chan *chan,
				const struct ntc_remote_buf *dst,
				u64 dst_offset,
				const struct ntc_mr_buf *src,
				u64 src_offset,
				u64 len)
{
	if (unlikely(!ntc_segment_valid(src->size, src_offset, len)))
		return -EINVAL;

	if (unlikely(!ntc_segment_valid(dst->size, dst_offset, len)))
		return -EINVAL;

	ntc_prepare_to_copy(chan, src->dma_addr + src_offset, len);

	return _ntc_request_memcpy(chan,
				dst->dma_addr, dst->size, dst_offset,
				src->dma_addr, src->size, src_offset,
				len, false, NULL, NULL);
}

int ntc_req_imm(struct ntc_dma_chan *chan,
		u64 dst, const void *ptr, size_t len, bool fence,
		void (*cb)(void *cb_ctx), void *cb_ctx);

/**
 * ntc_req_imm32() - append a 32 bit immediate data write operation
 * @chan:	Channel request context.
 * @dst:	Destination channel address.
 * @val:	Immediate data value.
 * @fence:	Fence after this operation.
 * @cb:		Callback after this operation.
 * @cb_ctx:	Callback context.
 *
 * Append a 32 bit immedidate data write operation to a remote destination
 * buffer.  The immedate data value is determined by val, and the implicit
 * length is four bytes.
 *
 * The upper layer should attempt to minimize the frequency of immediate data
 * requests.  Immediate data requests may be implemented as small operations
 * processed as dma requests, and many small dma requests may impact the
 * throughput performance of the channel.
 *
 * Please see ntc_req_memcpy() for a complete description of other parameters.
 *
 * Return: Zero on success, othewise an error number.
 */
static inline int ntc_req_imm32(struct ntc_dma_chan *chan,
				dma_addr_t dst, u32 val,
				bool fence, void (*cb)(void *cb_ctx),
				void *cb_ctx)
{
	return ntc_req_imm(chan, dst, &val, sizeof(val),
			fence, cb, cb_ctx);
}

static inline int _ntc_request_imm32(struct ntc_dma_chan *chan,
				dma_addr_t dst_start,
				u64 dst_size, u64 dst_offset,
				u32 val, bool fence,
				void (*cb)(void *cb_ctx), void *cb_ctx)
{
	if (unlikely(!ntc_segment_valid(dst_size, dst_offset, sizeof(u32))))
		return -EINVAL;

	return ntc_req_imm32(chan, dst_start + dst_offset, val,
			fence, cb, cb_ctx);
}

static inline int ntc_request_imm32(struct ntc_dma_chan *chan,
				const struct ntc_remote_buf *dst,
				u64 dst_offset,
				u32 val, bool fence,
				void (*cb)(void *cb_ctx), void *cb_ctx)
{
	return _ntc_request_imm32(chan,
				dst->dma_addr, dst->size, dst_offset,
				val, fence, cb, cb_ctx);
}

/**
 * ntc_req_imm64() - append a 64 bit immediate data write operation
 * @chan:	Channel request context.
 * @dst:	Destination channel address.
 * @val:	Immediate data value.
 * @fence:	Fence after this operation.
 * @cb:		Callback after this operation.
 * @cb_ctx:	Callback context.
 *
 * Append a 64 bit immedidate data write operation to a remote destination
 * buffer.  The immedate data value is determined by val, and the implicit
 * length is eight bytes.
 *
 * The upper layer should attempt to minimize the frequency of immediate data
 * requests.  Immediate data requests may be implemented as small operations
 * processed as dma requests, and many small dma requests may impact the
 * throughput performance of the channel.
 *
 * Please see ntc_req_memcpy() for a complete description of other parameters.
 *
 * Return: Zero on success, othewise an error number.
 */
static inline int ntc_req_imm64(struct ntc_dma_chan *chan,
				dma_addr_t dst, u64 val,
				bool fence, void (*cb)(void *cb_ctx),
				void *cb_ctx)
{
	return ntc_req_imm(chan, dst, &val, sizeof(val),
			fence, cb, cb_ctx);
}

static inline int _ntc_request_imm64(struct ntc_dma_chan *chan,
				dma_addr_t dst_start,
				u64 dst_size, u64 dst_offset,
				u64 val, bool fence,
				void (*cb)(void *cb_ctx), void *cb_ctx)
{
	if (unlikely(!ntc_segment_valid(dst_size, dst_offset, sizeof(u64))))
		return -EINVAL;

	return ntc_req_imm64(chan, dst_start + dst_offset, val,
			fence, cb, cb_ctx);
}

static inline int ntc_request_imm64(struct ntc_dma_chan *chan,
				const struct ntc_remote_buf *dst,
				u64 dst_offset,
				u64 val, bool fence,
				void (*cb)(void *cb_ctx), void *cb_ctx)
{
	return _ntc_request_imm64(chan,
				dst->dma_addr, dst->size, dst_offset,
				val, fence, cb, cb_ctx);
}

static inline
int ntc_mr_request_memcpy_unfenced_imm(struct ntc_dma_chan *chan,
				const struct ntc_remote_buf *dst,
				u64 dst_offset,
				const void *src_data,
				u32 len)
{
	if (unlikely(!ntc_segment_valid(dst->size, dst_offset, len)))
		return -EINVAL;

	return ntc_req_imm(chan, dst->dma_addr + dst_offset, src_data, len,
			false, NULL, NULL);
}

/**
 * ntc_req_signal() - append an operation to signal the peer
 * @ntc:	Device context.
 * @chan:	Channel request context.
 * @cb:		Callback after this operation.
 * @cb_ctx:	Callback context.
 *
 * Append an operation to signal the peer.  The peer driver will be receive
 * ntc_signal_event() after processing this operation.
 *
 * The channel implementation may coalesce interrupts to the peer driver,
 * therefore the upper layer must not rely on a one-to-one correspondence of
 * signals requested, and signal events received.  The only guarantee is that
 * at least one signal event will be received after the last signal requested.
 *
 * The upper layer should attempt to minimize the frequency of signal requests.
 * Signal requests may be implemented as small operations processed as dma
 * requests, and many small dma requests may impact the throughput performance
 * of the channel.  Furthermore, the channel may not coalesce interrupts, and a
 * high frequency of interrupts may impact the scheduling performance of the
 * peer.
 *
 * Please see ntc_req_memcpy() for a complete description of other parameters.
 *
 * Return: Zero on success, othewise an error number.
 */
int ntc_req_signal(struct ntc_dev *ntc, struct ntc_dma_chan *chan,
		void (*cb)(void *cb_ctx), void *cb_ctx, int vec);

static inline int ntc_phys_local_buf_map(struct ntc_local_buf *buf,
					struct ntc_dev *ntc)
{
	struct device *dev = ntc->dma_engine_dev;

	buf->dma_addr = dma_map_single(dev, buf->ptr, buf->size, DMA_TO_DEVICE);
	if (unlikely(dma_mapping_error(dev, buf->dma_addr)))
		return -EIO;

	return 0;
}

static inline void ntc_phys_local_buf_unmap(struct ntc_local_buf *buf,
					struct ntc_dev *ntc)
{
	dma_unmap_single(ntc->dma_engine_dev, buf->dma_addr, buf->size,
			DMA_TO_DEVICE);
}

static inline void ntc_local_buf_clear(struct ntc_local_buf *buf)
{
	if (buf->owned)
		buf->ptr = NULL;
	buf->dma_addr = 0;
}

/**
 * ntc_local_buf_alloc() - allocate a local memory buffer,
 *                         capable of DMA to the DMA engine.
 * @buf:	OUTPUT buffer.
 * @ntc:	Device context.
 * @size:	Size of the buffer to allocate.
 * @gfp:	Allocation flags from gfp.h.
 *
 * Return: zero on success, negative error value on failure.
 */
static inline int ntc_local_buf_alloc(struct ntc_local_buf *buf,
				struct ntc_dev *ntc, u64 size, gfp_t gfp)
{
	int rc;

	buf->ptr = NULL;
	buf->dma_addr = 0;

	buf->size = size;
	if (unlikely(buf->size != size))
		return -EINVAL;

	buf->owned = true;
	buf->mapped = true;

	buf->ptr = kmalloc_node(buf->size, gfp, dev_to_node(&ntc->dev));
	if (unlikely(!buf->ptr)) {
		ntc_local_buf_clear(buf);
		return -ENOMEM;
	}

	rc = ntc_phys_local_buf_map(buf, ntc);

	if (unlikely(rc < 0)) {
		kfree(buf->ptr);
		ntc_local_buf_clear(buf);
	}

	return rc;
}

/**
 * ntc_local_buf_zalloc() - allocate a local memory buffer,
 *                          capable of DMA to the DMA engine,
 *                          and clear the buffer's contents.
 * @buf:	OUTPUT buffer.
 * @ntc:	Device context.
 * @size:	Size of the buffer to allocate.
 * @gfp:	Allocation flags from gfp.h.
 *
 * Return: zero on success, negative error value on failure.
 */
static inline int ntc_local_buf_zalloc(struct ntc_local_buf *buf,
				struct ntc_dev *ntc, u64 size, gfp_t gfp)
{
	return ntc_local_buf_alloc(buf, ntc, size, gfp | __GFP_ZERO);
}

/**
 * ntc_local_buf_map_prealloced() - prepare a local memory buffer to be
 *                                  capable of DMA to the DMA engine.
 * @buf:	OUTPUT buffer.
 * @ntc:	Device context.
 * @size:	Size of the buffer.
 * @ptr:	Pointer to the preallocated buffer.
 *
 * Return: zero on success, negative error value on failure.
 */
static inline int ntc_local_buf_map_prealloced(struct ntc_local_buf *buf,
					struct ntc_dev *ntc,
					u64 size, void *ptr)
{
	int rc;

	buf->ptr = NULL;
	buf->dma_addr = 0;

	buf->size = size;
	if (unlikely(buf->size != size))
		return -EINVAL;

	buf->owned = false;
	buf->mapped = true;

	buf->ptr = ptr;

	rc = ntc_phys_local_buf_map(buf, ntc);

	if (unlikely(rc < 0))
		ntc_local_buf_clear(buf);

	return rc;
}

/**
 * ntc_local_buf_map_dma() - create local memory buffer by DMA address.
 *
 * @buf:	OUTPUT buffer.
 * @ntc:	Device context.
 * @size:	Size of the buffer.
 * @dma_addr:	DMA address of the buffer.
 */
static inline int ntc_local_buf_map_dma(struct ntc_local_buf *buf,
					u64 size, dma_addr_t dma_addr)
{
	buf->ptr = NULL;
	buf->dma_addr = 0;

	buf->size = size;
	if (unlikely(buf->size != size))
		return -EINVAL;

	buf->owned = false;
	buf->mapped = false;
	buf->ptr = phys_to_virt(dma_addr);
	buf->dma_addr = dma_addr;

	return 0;
}

/**
 * ntc_local_buf_free() - unmap and, if necessary, free the local buffer.
 *
 * @buf:	Local buffer.
 */
static inline void ntc_local_buf_free(struct ntc_local_buf *buf,
				struct ntc_dev *ntc)
{
	if (buf->mapped)
		ntc_phys_local_buf_unmap(buf, ntc);

	if (buf->owned)
		kfree(buf->ptr);

	ntc_local_buf_clear(buf);
}

/**
 * ntc_local_buf_disown() - Mark the buffer memory as not owned
 *                          by the buffer and return pointer to it.
 *                          This memory must be kfree()-d.
 *
 * @buf:	Local buffer.
 *
 * After this, the memory will not be freed by ntc_local_buf_free().
 *
 * Return:	Pointer to the buffer's memory.
 */
static inline void *ntc_local_buf_disown(struct ntc_local_buf *buf,
					struct ntc_dev *ntc)
{
	void *ptr = buf->ptr;

	buf->owned = false;
	ntc_local_buf_free(buf, ntc);

	return ptr;
}

/**
 * ntc_local_buf_seq_write() - Write out the buffer's contents.
 * @s:		The first argument of seq_write().
 * @buf:	Local buffer.
 *
 * Return:	The result of seq_write().
 */
static inline int ntc_local_buf_seq_write(struct seq_file *s,
					struct ntc_local_buf *buf)
{
	const void *p;

	p = buf->ptr;

	if (!p)
		return 0;

	return seq_write(s, p, buf->size);
}

/**
 * ntc_local_buf_deref() - Retrieve pointer to the buffer's memory.
 * @buf:	Local buffer.
 *
 * Return:	The pointer to the buffer's memory.
 */
static inline void *ntc_local_buf_deref(struct ntc_local_buf *buf)
{
	return buf->ptr;
}

/**
 * ntc_local_buf_const_deref() - Retrieve const pointer to the buffer's memory.
 * @buf:	Local buffer.
 *
 * Return:	The pointer to the buffer's memory.
 */
static inline const void *
ntc_local_buf_const_deref(const struct ntc_local_buf *buf)
{
	return buf->ptr;
}

static inline bool ntc_chan_addr_valid(union ntc_chan_addr *chan_addr)
{
	return !!chan_addr->value;
}

/**
 * ntc_export_buf_valid() - Check whether the buffer is valid.
 * @buf:	Export buffer.
 *
 * Return:	true iff valid.
 */
static inline bool ntc_export_buf_valid(struct ntc_export_buf *buf)
{
	return !!buf->dma_addr;
}

static inline void ntc_export_buf_clear(struct ntc_export_buf *buf)
{
	buf->ptr = NULL;
	buf->dma_addr = 0;
}

/**
 * ntc_export_buf_alloc() - allocate a memory buffer in an NTB window,
 *                          to which the peer can write.
 * @buf:	OUTPUT buffer.
 * @ntc:	Device context.
 * @size:	Size of the buffer to allocate.
 * @gfp:	Allocation flags from gfp.h.
 *
 * Return: zero on success, negative error value on failure.
 */
static inline int _ntc_export_buf_alloc(const char *caller, int line,
					struct ntc_export_buf *buf,
					struct ntc_dev *ntc,
					u64 size, gfp_t gfp)
{
	int rc;
	dma_addr_t dma_addr;
	bool is_flat = ntc->own_mws[NTC_DRAM_MW_IDX].is_flat;

	buf->ptr = NULL;
	buf->dma_addr = 0;

	buf->size = size;

	if (!is_flat) {
		buf->own_mw = &ntc->own_mws[NTC_INFO_MW_IDX];
		buf->ptr = ntc_mm_alloc(caller, line,
					&buf->own_mw->mm, size, gfp);
		if (unlikely(IS_ERR(buf->ptr))) {
			rc = PTR_ERR(buf->ptr);
			buf->ptr = NULL;
			return rc;
		}

		dma_addr = buf->own_mw->base +
			(buf->ptr - buf->own_mw->base_ptr);
	} else {
		buf->own_mw = &ntc->own_mws[NTC_DRAM_MW_IDX];
		buf->ptr = kmalloc_node(size, gfp, dev_to_node(&ntc->dev));
		if (unlikely(!buf->ptr))
			return -ENOMEM;

		dma_addr = dma_map_single(ntc->ntb_dev, buf->ptr, size,
					DMA_FROM_DEVICE);
		if (unlikely(dma_mapping_error(ntc->ntb_dev, dma_addr))) {
			kfree(buf->ptr);
			return -EIO;
		}
	}

	buf->dma_addr = dma_addr;

	return 0;
}

#define ntc_export_buf_alloc(...) \
	_ntc_export_buf_alloc(__func__, __LINE__, ##__VA_ARGS__)

/**
 * ntc_export_buf_zalloc() - allocate a memory buffer in an NTB window,
 *                           to which the peer can write,
 *                           and clear the buffer's contents.
 * @buf:	OUTPUT buffer.
 * @ntc:	Device context.
 * @size:	Size of the buffer to allocate.
 * @gfp:	Allocation flags from gfp.h.
 *
 * Calls ntc_export_buf_alloc().
 *
 * Return: zero on success, negative error value on failure.
 */
static inline int _ntc_export_buf_zalloc(const char *caller, int line,
					struct ntc_export_buf *buf,
					struct ntc_dev *ntc,
					u64 size, gfp_t gfp)
{
	return _ntc_export_buf_alloc(caller, line,
				buf, ntc, size, gfp | __GFP_ZERO);
}

#define ntc_export_buf_zalloc(...) \
	_ntc_export_buf_zalloc(__func__, __LINE__, ##__VA_ARGS__)

/**
 * ntc_export_buf_alloc_init() - allocate a memory buffer in an NTB window,
 *                               to which the peer can write,
 *                               and initialize the buffer's contents.
 * @buf:	OUTPUT buffer.
 * @ntc:	Device context.
 * @size:	Size of the buffer to allocate.
 * @gfp:	Allocation flags from gfp.h.
 * @init_buf:	Pointer to initialization data.
 * @init_buf_size: Size of initialization data.
 * @init_buf_offset: Offset of the initialized data in the buffer.
 *
 * Calls ntc_export_buf_alloc().
 *
 * Return: zero on success, negative error value on failure.
 */
static inline int _ntc_export_buf_alloc_init(const char *caller, int line,
					struct ntc_export_buf *buf,
					struct ntc_dev *ntc,
					u64 size, gfp_t gfp,
					void *init_buf, u64 init_buf_size,
					u64 init_buf_offset)
{
	int rc;


	if (unlikely(!ntc_segment_valid(size, init_buf_offset, init_buf_size)))
		return -EINVAL;

	rc = _ntc_export_buf_alloc(caller, line, buf, ntc, size, gfp);
	if (unlikely(rc < 0))
		return rc;

	memcpy(buf->ptr + init_buf_offset, init_buf, init_buf_size);

	return rc;
}

#define ntc_export_buf_alloc_init(...) \
	_ntc_export_buf_alloc_init(__func__, __LINE__, ##__VA_ARGS__)


/**
 * ntc_export_buf_zalloc_init() - allocate a memory buffer in an NTB window,
 *                                to which the peer can write,
 *                                initialize the buffer's contents,
 *                                and clear the rest of the buffer.
 * @buf:	OUTPUT buffer.
 * @ntc:	Device context.
 * @size:	Size of the buffer to allocate.
 * @gfp:	Allocation flags from gfp.h.
 * @init_buf:	Pointer to initialization data.
 * @init_buf_size: Size of initialization data.
 * @init_buf_offset: Offset of the initialized data in the buffer.
 *
 * Calls ntc_export_buf_alloc().
 *
 * Return: zero on success, negative error value on failure.
 */
static inline int _ntc_export_buf_zalloc_init(const char *caller, int line,
					struct ntc_export_buf *buf,
					struct ntc_dev *ntc,
					u64 size, gfp_t gfp,
					void *init_buf, u64 init_buf_size,
					u64 init_buf_offset)
{
	int rc;

	if (unlikely(!ntc_segment_valid(size, init_buf_offset, init_buf_size)))
		return -EINVAL;

	rc = _ntc_export_buf_zalloc(caller, line, buf, ntc, size, gfp);
	if (unlikely(rc < 0))
		return rc;

	memcpy(buf->ptr + init_buf_offset, init_buf, init_buf_size);

	return rc;
}

#define ntc_export_buf_zalloc_init(...) \
	_ntc_export_buf_zalloc_init(__func__, __LINE__, ##__VA_ARGS__)

/**
 * ntc_export_buf_free() - unmap and, if necessary, free the buffer
 *                         created by ntc_export_buf_alloc().
 * @buf:	Export buffer.
 */
static inline void ntc_export_buf_free(struct ntc_export_buf *buf)
{
	bool is_flat = buf->own_mw->ntc->own_mws[NTC_DRAM_MW_IDX].is_flat;

	if (buf->dma_addr && is_flat)
		dma_unmap_single(buf->own_mw->ntb_dev, buf->dma_addr,
				buf->size, DMA_FROM_DEVICE);

	if (buf->ptr) {
		if (!is_flat)
			ntc_mm_free(&buf->own_mw->mm, buf->ptr, buf->size);
		else
			kfree(buf->ptr);
	}

	ntc_export_buf_clear(buf);
}

/**
 * ntc_export_buf_make_desc() - Make serializable description of buffer.
 *
 * @desc:	OUTPUT buffer description, which can be sent to peer.
 * @buf:	Export buffer.
 */
static inline void ntc_export_buf_make_desc(struct ntc_remote_buf_desc *desc,
					struct ntc_export_buf *buf)
{
	if (buf->dma_addr) {
		desc->chan_addr.mw_idx = buf->own_mw->mw_idx;
		desc->chan_addr.one = 1;
		desc->chan_addr.offset = buf->dma_addr - buf->own_mw->base;
		desc->size = buf->size;
	} else
		memset(desc, 0, sizeof(*desc));
}

/**
 * ntc_export_buf_make_partial_desc() - Make description for a part of a buffer.
 *
 * @desc:	OUTPUT buffer description, which can be sent to peer.
 * @buf:	Export buffer.
 * @offset:	Offset in the buffer.
 * @len:	Length of the part.
 */
static inline int
ntc_export_buf_make_partial_desc(struct ntc_remote_buf_desc *desc,
				const struct ntc_export_buf *buf,
				u64 offset, u64 len)
{
	if (unlikely(!buf->dma_addr))
		return -EINVAL;

	if (unlikely(!ntc_segment_valid(buf->size, offset, len)))
		return -EINVAL;

	desc->chan_addr.mw_idx = buf->own_mw->mw_idx;
	desc->chan_addr.one = 1;
	desc->chan_addr.offset = buf->dma_addr - buf->own_mw->base + offset;
	desc->size = len;

	return 0;
}

/**
 * ntc_export_buf_get_part_params() - Retrieve offset & len for part of buffer.
 *
 * @desc:	Partial buffer description.
 * @buf:	Export buffer.
 * @offset:	OUTPUT offset in the buffer.
 * @len:	OUTPUT length of the part.
 *
 * Returns 0 on success, negative value on failure.
 */
static inline int
ntc_export_buf_get_part_params(const struct ntc_export_buf *buf,
			const struct ntc_remote_buf_desc *desc,
			u64 *offset, u64 *len)
{
	s64 soffset;

	soffset = desc->chan_addr.offset - (buf->dma_addr - buf->own_mw->base);

	*offset = soffset;
	*len = desc->size;

	if (unlikely(!buf->dma_addr))
		return -EINVAL;

	if (unlikely(soffset < 0))
		return -EINVAL;

	if (unlikely(!ntc_segment_valid(buf->size, soffset, desc->size)))
		return -EINVAL;

	if (unlikely(buf->own_mw->mw_idx != desc->chan_addr.mw_idx))
		return -EINVAL;

	return 0;
}

/**
 * ntc_export_buf_const_deref() - Retrieve const pointer into buffer's memory.
 * @buf:	Export buffer.
 * @offset:	Offset to the segment to be accessed.
 * @len:	Length of the segment to be accessed.
 *
 * Any necessary synchronization will be made before returning the pointer.
 *
 * Return:	The const pointer into the buffer's memory at the given offset,
 *		or NULL on error.
 */
static inline const void *ntc_export_buf_const_deref(struct ntc_export_buf *buf,
						u64 offset, u64 len)
{
	if (unlikely(!ntc_segment_valid(buf->size, offset, len)))
		return NULL;
	if (unlikely(!buf->dma_addr))
		return NULL;

	dma_sync_single_for_cpu(buf->own_mw->ntb_dev,
				buf->dma_addr + offset, len,
				DMA_FROM_DEVICE);

	return buf->ptr + offset;
}

static inline int ntc_export_buf_reinit(struct ntc_export_buf *buf,
					void *init_buf, u64 offset, u64 len)
{
	if (unlikely(!ntc_segment_valid(buf->size, offset, len)))
		return -EINVAL;

	if (unlikely(!buf->dma_addr))
		return -EINVAL;

	memcpy(buf->ptr + offset, init_buf, len);

	dma_sync_single_for_device(buf->own_mw->ntb_dev,
				buf->dma_addr + offset, len,
				DMA_FROM_DEVICE);

	return 0;
}

static inline int ntc_export_buf_reinit_by_zeroes(struct ntc_export_buf *buf,
						u64 offset, u64 len)
{
	if (unlikely(!ntc_segment_valid(buf->size, offset, len)))
		return -EINVAL;

	if (unlikely(!buf->dma_addr))
		return -EINVAL;

	memset(buf->ptr + offset, 0, len);

	dma_sync_single_for_device(buf->own_mw->ntb_dev,
				buf->dma_addr + offset, len,
				DMA_FROM_DEVICE);

	return 0;
}

/**
 * ntc_export_buf_seq_write() - Write out the buffer's contents.
 * @s:		The first argument of seq_write().
 * @buf:	Export buffer.
 *
 * Return:	The result of seq_write().
 */
static inline int ntc_export_buf_seq_write(struct seq_file *s,
					struct ntc_export_buf *buf)
{
	const void *p;

	p = ntc_export_buf_const_deref(buf, 0, buf->size);

	if (!p)
		return 0;

	return seq_write(s, p, buf->size);
}

/**
 * ntc_mr_buf_valid() - Check whether the buffer is valid.
 * @buf:	Buffer created by ntc_mr_buf_map_dma().
 *
 * Return:	true iff valid.
 */
static inline bool ntc_mr_buf_valid(struct ntc_mr_buf *buf)
{
	return !!buf->dma_addr;
}

static inline void ntc_mr_buf_clear(struct ntc_mr_buf *buf)
{
	buf->dma_addr = 0;
}

/**
 * ntc_mr_buf_map_dma() - prepare a memory buffer in an NTB window,
 *                           to which the peer can write,
 *                           and make it capable of DMA to the DMA engine.
 * @buf:	OUTPUT buffer.
 * @ntc:	Device context.
 * @size:	Size of the buffer.
 * @dma_addr:	buffer's DMA address.
 */
static inline int ntc_mr_buf_map_dma(struct ntc_mr_buf *buf,
				struct ntc_dev *ntc,
				u64 size, dma_addr_t dma_addr,
				int mr_access_flags)
{
	int i;
	struct ntc_own_mw *own_mw;

	buf->size = size;
	buf->mr_access_flags = mr_access_flags;
	buf->dma_addr = dma_addr;

	for (i = 0; i < NTC_MAX_NUM_MWS; i++) {
		own_mw = &ntc->own_mws[i];
		if (ntc_segment_valid(own_mw->size,
					dma_addr - own_mw->base, size))
			break;
	}

	buf->own_mw = own_mw;

	if (!(mr_access_flags & IB_ACCESS_REMOTE_WRITE))
		return 0;

	if (unlikely(i == NTC_MAX_NUM_MWS)) {
		dev_err(&ntc->dev, "addr %#llx:%#llx beyond memory windows",
			dma_addr, dma_addr + size);
		return -EINVAL;
	}

	return 0;
}

/**
 * ntc_mr_buf_clear_sgl() - clear each buffer in the array of buffers
  *                         created by ntc_mr_buf_map_dma().
 * @sgl:	Array of buffers created by ntc_mr_buf_map_dma().
 * @count:	Size of the array.
 */
static inline void ntc_mr_buf_clear_sgl(struct ntc_mr_buf *sgl, int count)
{
	int i;

	for (i = 0; i < count; i++)
		ntc_mr_buf_clear(&sgl[i]);
}

/**
 * ntc_mr_buf_make_desc() - Make serializable description of buffer.
 *
 * @desc:	OUTPUT buffer description, which can be sent to peer.
 * @buf:	Buffer created by ntc_mr_buf_map_dma().
 */
static inline void ntc_mr_buf_make_desc(struct ntc_remote_buf_desc *desc,
					struct ntc_mr_buf *buf)
{
	desc->size = buf->size;

	if (!buf->dma_addr)
		goto empty;

	if (!(buf->mr_access_flags & IB_ACCESS_REMOTE_WRITE))
		goto empty;

	desc->chan_addr.mw_idx = buf->own_mw->mw_idx;
	desc->chan_addr.one = 1;
	desc->chan_addr.offset = buf->dma_addr - buf->own_mw->base;

	return;

 empty:
	desc->chan_addr.value = 0;
}

/**
 * ntc_mr_buf_sync() - Sync the buffer's memory after receiving data.
 * @buf:	Buffer created by ntc_mr_buf_map_dma().
 * @offset:	Offset to the segment to be accessed.
 * @len:	Length of the segment to be accessed.
 */
static inline void ntc_mr_buf_sync(struct ntc_mr_buf *buf, u64 offset, u64 len)
{
	if (unlikely(!ntc_segment_valid(buf->size, offset, len)))
		return;

	dma_sync_single_for_cpu(buf->own_mw->ntb_dev,
				buf->dma_addr + offset,
				len, DMA_FROM_DEVICE);
}

static inline void ntc_remote_buf_desc_clear(struct ntc_remote_buf_desc *desc)
{
	memset(desc, 0, sizeof(*desc));
}

static inline int
ntc_remote_buf_desc_clip(struct ntc_remote_buf_desc *desc, u64 offset, u64 len)
{
	if (unlikely(!ntc_segment_valid(desc->size, offset, len)))
		return -EINVAL;

	desc->chan_addr.offset += offset;
	desc->size = len;

	return 0;
}

static inline void ntc_remote_buf_clear(struct ntc_remote_buf *buf)
{
	buf->dma_addr = 0;
}

static inline bool ntc_remote_buf_valid(struct ntc_remote_buf *buf)
{
	return !!buf->dma_addr;
}

/**
 * ntc_remote_buf_map() - map a remote buffer via an NTB window,
 *                        to write to the peer,
 *                        and make it capable of DMA from the DMA engine.
 * @buf:	OUTPUT remote buffer.
 * @ntc:	Device context.
 * @desc:	The remote buffer description created by
 *		ntc_export_buf_make_desc() or ntc_mr_buf_make_desc().
 *
 * Return: zero on success, negative error value on failure.
 */
static inline int ntc_remote_buf_map(struct ntc_remote_buf *buf,
				struct ntc_dev *ntc,
				const struct ntc_remote_buf_desc *desc)
{
	struct device *dev = ntc->dma_engine_dev;
	phys_addr_t ptr;
	dma_addr_t dma_addr;

	ntc_remote_buf_clear(buf);

	if (!desc->chan_addr.value)
		return 0;

	ptr = ntc_peer_addr(ntc, &desc->chan_addr);
	if (unlikely(!ptr))
		return -EIO;

	dma_addr = dma_map_resource(dev, ptr, desc->size, DMA_FROM_DEVICE, 0);
	if (unlikely(dma_mapping_error(dev, dma_addr)))
		return -EIO;

	buf->dma_addr = dma_addr;
	buf->size = desc->size;

	return 0;
}

/**
 * ntc_remote_buf_map_phys() - map a remote buffer via an NTB window,
 *                        to write to the peer,
 *                        and make it capable of DMA from the DMA engine.
 * @buf:	OUTPUT remote buffer.
 * @ntc:	Device context.
 * @ptr:	The physical (PCIe) address of the remote buffer in NTB window.
 * @size:	Size of the buffer to map.
 *
 * Return: zero on success, negative error value on failure.
 */
static inline int ntc_remote_buf_map_phys(struct ntc_remote_buf *buf,
					struct ntc_dev *ntc,
					phys_addr_t ptr, u64 size)
{
	struct device *dev = ntc->dma_engine_dev;
	dma_addr_t dma_addr;

	ntc_remote_buf_clear(buf);

	if (unlikely(!ptr))
		return -EIO;

	dma_addr = dma_map_resource(dev, ptr, size, DMA_FROM_DEVICE, 0);
	if (unlikely(dma_mapping_error(dev, dma_addr)))
		return -EIO;

	buf->dma_addr = dma_addr;
	buf->size = size;

	return 0;
}

/**
 * ntc_remote_buf_map_phys() - unmap a remote buffer.
 * @buf:	remote buffer created by
 *		ntc_remote_buf_map() or ntc_remote_buf_map_phys().
 */
static inline void ntc_remote_buf_unmap(struct ntc_remote_buf *buf,
					struct ntc_dev *ntc)
{
	struct device *dev = ntc->dma_engine_dev;

	if (buf->dma_addr)
		dma_unmap_resource(dev, buf->dma_addr, buf->size,
				DMA_FROM_DEVICE, 0);

	ntc_remote_buf_clear(buf);
}

/**
 * ntc_umem_sgl() - store discontiguous ranges in a scatter gather list
 * @ntc:	Device context.
 * @ib_umem:	Object representing the memory mapping.
 * @sgl:	Scatter gather ntc_mr_buf list to receive the entries.
 * @count:	Capacity of the scatter gather list.
 *
 * Count the number of discontiguous ranges in the channel mapping, so that a
 * scatter gather list of the exact length can be allocated.
 *
 * Return: The number of entries stored in the list, or an error number.
 */
int ntc_umem_sgl(struct ntc_dev *ntc, struct ib_umem *ib_umem,
		struct ntc_mr_buf *sgl, int count, int mr_access_flags);

/**
 * ntc_umem_count() - count discontiguous ranges in the channel mapping
 * @ntc:	Device context.
 * @ib_umem:	Object representing the memory mapping.
 *
 * Count the number of discontiguous ranges in the channel mapping, so that a
 * scatter gather list of the exact length can be allocated.
 *
 * Return: The number of discontiguous ranges, or an error number.
 */
static inline int ntc_umem_count(struct ntc_dev *ntc, struct ib_umem *ib_umem)
{
	return ntc_umem_sgl(ntc, ib_umem, NULL, 0, 0);
}

/**
 * ntc_clear_signal() - clear the signal asserting event
 * @ntc:	Device context.
 *
 * Clear the signal that's asserted for events (interrupts perhaps).
 *
 * Return: 1 for events waiting or 0 for no events.
 */
int ntc_clear_signal(struct ntc_dev *ntc, int vec);

#endif
