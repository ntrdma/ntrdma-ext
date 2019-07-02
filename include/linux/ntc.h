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
#include <linux/dma-direction.h>
#include <linux/rwlock.h>

#include "linux/ntc_trace.h"
#define NTB_MAX_IRQS (64)

struct ntc_driver;
struct ntc_dev;

struct ib_ucontext;
struct ib_umem;

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
 * @clear_signal:	See ntc_ctx_clear_signal().
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

enum ntc_dma_access {
	NTB_DEV_ACCESS = 0,
	IOAT_DEV_ACCESS
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

/**
 * struct ntc_dev_ops - ntc device operations
 * @map_dev:		See ntc_map_dev().
 * @link_disable:	See ntc_link_disable().
 * @link_enable:	See ntc_link_enable().
 * @link_reset:		See ntc_link_reset().
 * @peer_addr		See ntc_peer_addr().
 * @req_create:		See ntc_req_create().
 * @req_cancel:		See ntc_req_cancel().
 * @req_submit:		See ntc_req_submit().
 * @req_memcpy:		See ntc_req_memcpy().
 * @req_imm32:		See ntc_req_imm32().
 * @req_imm64:		See ntc_req_imm64().
 * @req_signal:		See ntc_req_signal().
 * @clear_signal:	See ntc_clear_signal().
 */
struct ntc_dev_ops {
	struct device *(*map_dev)(struct ntc_dev *ntc,
			enum ntc_dma_access dma_dev);
	int (*is_link_up)(struct ntc_dev *ntc);
	int (*link_disable)(struct ntc_dev *ntc);
	int (*link_enable)(struct ntc_dev *ntc);
	int (*link_reset)(struct ntc_dev *ntc);

	phys_addr_t (*peer_addr)(struct ntc_dev *ntc, u64 dma_addr_local);

	void *(*req_create)(struct ntc_dev *ntc);
	void (*req_cancel)(struct ntc_dev *ntc, void *req);
	int (*req_submit)(struct ntc_dev *ntc, void *req);

	int (*req_memcpy)(struct ntc_dev *ntc, void *req,
			dma_addr_t dst, dma_addr_t src, u64 len, bool fence,
			void (*cb)(void *cb_ctx), void *cb_ctx);
	int (*req_imm32)(struct ntc_dev *ntc, void *req,
			dma_addr_t dst, u32 val, bool fence,
			void (*cb)(void *cb_ctx), void *cb_ctx);
	int (*req_imm64)(struct ntc_dev *ntc, void *req,
			dma_addr_t dst, u64 val, bool fence,
			void (*cb)(void *cb_ctx), void *cb_ctx);
	int (*req_signal)(struct ntc_dev *ntc, void *req,
			  void (*cb)(void *cb_ctx), void *cb_ctx, int vec);
	int (*clear_signal)(struct ntc_dev *ntc, int vec);
	int (*max_peer_irqs)(struct ntc_dev *ntc);
	void *(*local_hello_buf)(struct ntc_dev *ntc, int *size);
	void *(*peer_hello_buf)(struct ntc_dev *ntc, int *size);
	u32 (*query_version)(struct ntc_dev *ntc);
};

static inline int ntc_dev_ops_is_valid(const struct ntc_dev_ops *ops)
{
	/* commented callbacks are not required: */
	return
		/* ops->map_dev				&& */
		ops->link_disable			&&
		ops->link_enable			&&
		ops->link_reset				&&

		/* ops->peer_addr			&& */

		ops->req_create				&&
		ops->req_cancel				&&
		ops->req_submit				&&

		ops->req_memcpy				&&
		ops->req_imm32				&&
		ops->req_imm64				&&
		ops->req_signal				&&
		/* ops->clear_signal			&& */
		1;
}

struct ntc_local_buf {
	struct ntc_dev *ntc;
	struct device *dma_engine_dev;
	u64 size;
	bool owned;
	bool mapped;

	void *ptr;
	dma_addr_t dma_addr;
};

struct ntc_export_buf {
	struct ntc_dev *ntc;
	struct device *ntb_dev;
	u64 size;
	gfp_t gfp;

	void *ptr;
	u64 chan_addr;
};

struct ntc_bidir_buf {
	struct ntc_dev *ntc;
	struct device *ntb_dev;
	struct device *dma_engine_dev;
	u64 size;
	gfp_t gfp;

	void *ptr;
	u64 chan_addr;
	dma_addr_t dma_addr;
};

struct ntc_remote_buf_desc {
	u64 chan_addr;
	u64 size;
};

struct ntc_remote_buf {
	struct ntc_dev *ntc;
	struct device *dma_engine_dev;
	u64 size;

	phys_addr_t ptr;
	dma_addr_t dma_addr;
};

/** struct ntc_map_ops - ntc memory mapping operations
 * See usage below.
 */
struct ntc_map_ops {
	int (*local_buf_alloc)(struct ntc_local_buf *buf, gfp_t gfp);
	void (*local_buf_free)(struct ntc_local_buf *buf);
	int (*local_buf_map)(struct ntc_local_buf *buf);
	void (*local_buf_unmap)(struct ntc_local_buf *buf);
	void (*local_buf_prepare_to_copy)(const struct ntc_local_buf *buf,
					u64 offset, u64 len);

	int (*export_buf_alloc)(struct ntc_export_buf *buf);
	void (*export_buf_free)(struct ntc_export_buf *buf);
	const void *(*export_buf_const_deref)(struct ntc_export_buf *buf,
					u64 offset, u64 len);

	int (*bidir_buf_alloc)(struct ntc_bidir_buf *buf);
	void (*bidir_buf_free)(struct ntc_bidir_buf *buf);
	void *(*bidir_buf_deref)(struct ntc_bidir_buf *buf,
				u64 offset, u64 len);
	void (*bidir_buf_unref)(struct ntc_bidir_buf *buf,
				u64 offset, u64 len);
	const void *(*bidir_buf_const_deref)(struct ntc_bidir_buf *buf,
					u64 offset, u64 len);
	void (*bidir_buf_prepare_to_copy)(const struct ntc_bidir_buf *buf,
					u64 offset, u64 len);

	int (*remote_buf_map)(struct ntc_remote_buf *buf,
			const struct ntc_remote_buf_desc *desc);
	int (*remote_buf_map_phys)(struct ntc_remote_buf *buf,
				phys_addr_t ptr, u64 size);
	void (*remote_buf_unmap)(struct ntc_remote_buf *buf);
};

/* Virtual mapping ops provided by ntc_virt library module */
extern struct ntc_map_ops ntc_virt_map_ops;

/* Hardware DMA mapping ops provided by ntc_phys library module */
extern struct ntc_map_ops ntc_phys_map_ops;

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
 * @ops:		See &ntc_dev_ops.
 * @ctx_ops:		See &ntc_ctx_ops.
 */
struct ntc_dev {
	struct device			dev;
	const struct ntc_dev_ops	*dev_ops;
	const struct ntc_map_ops	*map_ops;
	const struct ntc_ctx_ops	*ctx_ops;
};

#define ntc_of_dev(__dev) container_of(__dev, struct ntc_dev, dev)

static inline int ntc_max_peer_irqs(struct ntc_dev *ntc)
{
	if (!ntc->dev_ops->max_peer_irqs)
			return 1;

	return ntc->dev_ops->max_peer_irqs(ntc);
}

static inline void *ntc_local_hello_buf(struct ntc_dev *ntc, int *size)
{
	return ntc->dev_ops->local_hello_buf(ntc, size);
}

static inline void *ntc_peer_hello_buf(struct ntc_dev *ntc, int *size)
{
	return ntc->dev_ops->peer_hello_buf(ntc, size);
}

static inline int ntc_door_bell_arbitrator(struct ntc_dev *ntc)
{
	static int counter;
	counter++;
	/*FIXME actualy we want to use the number of peer CPUs but for now we assume its the same */
	counter = counter % min(ntc_max_peer_irqs(ntc), (int)num_online_cpus());
	return counter;

}

#define NTB_DEFAULT_VEC(__NTC) ntc_door_bell_arbitrator(__NTC)
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

int __ntc_register_driver(struct ntc_driver *driver, struct module *mod,
			  const char *mod_name);

/**
 * ntc_unregister_driver() - unregister a driver for interest in ntc devices
 * @driver:	Client context.
 *
 * The driver will be removed from the list of drivers interested in ntc
 * devices.  If any ntc devices are associated with the driver, the driver will
 * be notified to remove those devices.
 */
void ntc_unregister_driver(struct ntc_driver *driver);

#define module_ntc_driver(__ntc_driver) \
	module_driver(__ntc_driver, ntc_register_driver, \
		      ntc_unregister_driver)

/**
 * ntc_register_device() - register a ntc device
 * @ntc:	Device context.
 *
 * The device will be added to the list of ntc devices.  If any drivers are
 * interested in ntc devices, each driver will be notified of the ntc device,
 * until at most one driver accepts the device.
 *
 * Return: Zero if the device is registered, otherwise an error number.
 */
int ntc_register_device(struct ntc_dev *ntc);

/**
 * ntc_unregister_device() - unregister a ntc device
 * @ntc:	Device context.
 *
 * The device will be removed from the list of ntc devices.  If the ntc device
 * is associated with a driver, the driver will be notified to remove the
 * device.
 */
void ntc_unregister_device(struct ntc_dev *ntc);

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
int ntc_set_ctx(struct ntc_dev *ntc, void *ctx,
		const struct ntc_ctx_ops *ctx_ops);

/**
 * ntc_clear_ctx() - disassociate any driver context from an ntc device
 * @ntc:	Device context.
 *
 * Clear any association that may exist between a driver context and the ntc
 * device.
 */
void ntc_clear_ctx(struct ntc_dev *ntc);

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
 * ntc_ctx_hello() - exchange data for upper layer protocol initialization
 * @ntc:	Device context.
 * @phase:	Hello phase number.
 * @in_buf:	Hello input buffer.
 * @in_buf:	Hello input buffer size.
 * @out_buf:	Hello output buffer.
 * @out_buf:	Hello output buffer size.
 *
 * In each phase, provide an input buffer with data from the peer, and an
 * output buffer with data for the peer.  The inputs are from the peer previous
 * phase outputs, and the outputs are for the peer next phase inputs.  Return
 * the size of a buffer for the peer next phase outputs, and the local phase
 * after next inputs.
 *
 * The zero'th phase has no inputs or outputs, and returns the size of the
 * buffer for the peer phase one outputs, which will be the local phase two
 * inputs.
 *
 * The first phase has only outputs, which will be the peer phase two inputs.
 * Phase one returns the size of the buffer for peer phase two outputs, which
 * will be the local phase three inputs.  The second and later phases have both
 * inputs and outputs, until the final phases.
 *
 * If a phase returns zero, then the next phase has no outputs.  The next phase
 * is the last phase.  Likewise, the last phase must return zero, or an error
 * code, only.
 *
 * When the next phase (logically, after the last phase) has no inputs or
 * outputs, initialization is complete.  After initialization is complete on
 * both sides of the channel, a link event will be indicated to the upper
 * layer, indicating that the link is up.
 *
 * The input buffer size is the capacity of the buffer, although the peer may
 * have written less data than the buffer size.  Likewise, the output buffer
 * size is the capacity of the buffer on the peer.  It is the responsibility of
 * the upper layer to detect and handle any cases where the data in the buffer
 * is less than the capacity of the buffer.
 *
 * Return: Input buffer size for phase after next, zero, or an error number.
 */
int ntc_ctx_hello(struct ntc_dev *ntc, int phase,
		      void *in_buf, size_t in_size,
		      void *out_buf, size_t out_size);

/**
 * ntc_ctx_enable() - notify driver that the link is active
 * @ntc:	Device context.
 *
 * Notify the driver context that the link is active.  The driver may begin
 * issuing requests.
 */
int ntc_ctx_enable(struct ntc_dev *ntc);

/**
 * ntc_ctx_disable() - notify driver that the link is not active
 * @ntc:	Device context.
 *
 * Notify the driver context that the link is not active.  The driver should
 * stop issuing requests.  See ntc_ctx_quiesce().
 */
void ntc_ctx_disable(struct ntc_dev *ntc);

/**
 * ntc_ctx_quiesce() - make sure driver is not issuing any new requests
 * @ntc:	Device context.
 *
 * Make sure the driver context is not issuing any new requests.  The driver
 * should block and wait for any asynchronous operations to stop.  It is not
 * necessary to wait for previously issued requests to complete, just that no
 * new requests will be issued after this function returns.
 */
void ntc_ctx_quiesce(struct ntc_dev *ntc);

/**
 * ntc_ctx_reset() - safely deallocate upper layer protocol structures
 * @ntc:	Device context.
 *
 * Call the upper layer to deallocate data structures that the peer may have
 * been writing.  When calling this function, the channel must guarantee to the
 * upper layer that the peer is no longer using any of the locally channel
 * mapped buffers.  Either the channel has obtained acknowledgment from the
 * peer for a protocol reset, or the channel can prevent misbehavior from a
 * peer in an unknown state.
 *
 * It is important that the upper layer does not immediately deallocate
 * structures on a link event, because while the channel link may report to be
 * down, a hardware link may still be active.  Immediately deallocating data
 * buffers could result in memory corruption, as it may be used by the peer
 * after it is freed locally.  A coordinated protocol reset is therefore
 * necessary to prevent use after free conditions from occurring.
 */
void ntc_ctx_reset(struct ntc_dev *ntc);

/**
 * ntc_ctx_signal() - notify driver context of a signal event
 * @ntc:	Device context.
 * @vector:	Interrupt vector number.
 *
 * Notify the driver context of a signal event triggered by the peer.
 */
void ntc_ctx_signal(struct ntc_dev *ntc, int vec);

/**
 * ntc_map_dev() - get the device to use for channel mapping
 * @ntc:	Device context.
 *
 * Get the device to use for channel mapping, which may be different from
 * &ntc->dev.  This is the device to pass to map_ops.
 *
 * Return: The device for channel mapping.
 */
static inline struct device *ntc_map_dev(struct ntc_dev *ntc,
		enum ntc_dma_access dma_dev)
{
	if (!ntc->dev_ops->map_dev)
		return &ntc->dev;

	return ntc->dev_ops->map_dev(ntc, dma_dev);
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
	if (!ntc->dev_ops->is_link_up)
		return 0;

	return ntc->dev_ops->is_link_up(ntc);
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
	if (!ntc->dev_ops->query_version)
		return -EINVAL;
	return ntc->dev_ops->query_version(ntc);
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
static inline int _ntc_link_disable(struct ntc_dev *ntc, const char *f)
{
	pr_info("NTC link disable by upper layer (%s)\n",
			f);
	TRACE("NTC link disable by upper layer (%s)\n",
			f);

	return ntc->dev_ops->link_disable(ntc);
}
#define ntc_link_disable(_ntc) _ntc_link_disable(_ntc, __func__)

/**
 * ntc_link_enable() - enable the link
 * @ntc:	Device context.
 *
 * Enable the link to negotiate a connection with the peer.
 *
 * Return: Zero on success, otherwise an error number.
 */
static inline int ntc_link_enable(struct ntc_dev *ntc)
{
	return ntc->dev_ops->link_enable(ntc);
}

/**
 * ntc_link_reset() - reset the link
 * @ntc:	Device context.
 *
 * Tear down any active connection and coordinate renegotiating the connection
 * with the peer.
 *
 * Return: Zero on success, otherwise an error number.
 */
static inline int _ntc_link_reset(struct ntc_dev *ntc, const char *f)
{
	int ret;

	pr_info("NTC link resetting by upper layer (%s)...\n",
			f);
	ret = ntc->dev_ops->link_reset(ntc);
	pr_info("NTC link reset done (%s)\n", f);

	return ret;
}

#define ntc_link_reset(_ntc) _ntc_link_reset(_ntc, __func__)

/**
 * ntc_peer_addr() - transform a channel-mapped address into a peer address
 * @ntc:	Device context.
 * @addr:	Channel-mapped address of a remote buffer.
 *
 * Transform a channel-mapped address of a remote buffer, mapped by the peer,
 * into a peer address to be used as the destination of a channel request.
 *
 * The peer allocates and maps the buffer with ntc_export_buf_alloc(),
 * ntc_bidir_buf_alloc() or ntc_bidir_buf_make_prealloced().
 * Then, the remote peer communicates the channel-mapped address of the buffer
 * across the channel.  The driver on side of the connection must then
 * translate the peer's channel-mapped address into a peer address.  The
 * translated peer address may then be used as the destination address of a
 * channel request.
 *
 * Return: The translated peer address.
 */
static inline phys_addr_t ntc_peer_addr(struct ntc_dev *ntc, u64 addr)
{
	if (!ntc->dev_ops->peer_addr)
		return addr;

	return ntc->dev_ops->peer_addr(ntc, addr);
}

/**
 * ntc_req_create() - create a channel request context
 * @ntc:	Device context.
 *
 * Create and return a channel request context, for data operations over the
 * channel.  Ownership of the request is passed to the caller, and the request
 * may be appended with operations to copy buffered or immediate data, until it
 * is either canceled or submitted.
 *
 * The different submit and cancel functions are provided, to support different
 * channel implementations.  Some implementations may submit operations as soon
 * as they are appended to a request, while others may store operations in the
 * request, to be submitted at once as a batch of operations.  Therefore, the
 * driver must assume that operations may begin as soon as they are appended,
 * prior to submitting the request, and must not assume that canceling a
 * request actually prevents operations from being carried out.  The different
 * submit and cancel functions exist to provide a portable interface to use the
 * varying underlying channel implementations most efficiently.
 *
 * Return: A channel request context, otherwise an error pointer.
 */
static inline void *ntc_req_create(struct ntc_dev *ntc)
{
	return ntc->dev_ops->req_create(ntc);
}

/**
 * ntc_req_cancel() - cancel a channel request
 * @ntc:	Device context.
 * @req:	Channel request context.
 *
 * Cancel a channel request instead of submitting it.  Ownership of the request
 * is passed from the caller back to the channel, and the caller no longer
 * holds a valid reference to the request.
 *
 * Some channel implementations may submit operations as soon as they are
 * appended to a request, therefore the driver must not assume that canceling a
 * request actually prevents operations from being carried out.  The cancel
 * function exists to provide a portable interface to use the varying
 * underlying channel implementations most efficiently.
 */
static inline void ntc_req_cancel(struct ntc_dev *ntc, void *req)
{
	return ntc->dev_ops->req_cancel(ntc, req);
}

/**
 * ntc_req_submit() - submit a channel request
 * @ntc:	Device context.
 * @req:	Channel request context.
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
static inline int ntc_req_submit(struct ntc_dev *ntc, void *req)
{
	return ntc->dev_ops->req_submit(ntc, req);
}

/**
 * ntc_req_memcpy() - append a buffer to buffer memory copy operation
 * @ntc:	Device context.
 * @req:	Channel request context.
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
static inline int ntc_req_memcpy(struct ntc_dev *ntc, void *req,
				dma_addr_t dst, dma_addr_t src, u64 len,
				bool fence,
				void (*cb)(void *cb_ctx), void *cb_ctx)
{
	return ntc->dev_ops->req_memcpy(ntc, req,
					dst, src, len, fence,
					cb, cb_ctx);
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

static inline int _ntc_request_memcpy(struct ntc_dev *ntc, void *req,
				dma_addr_t dst_start,
				u64 dst_size, u64 dst_offset,
				dma_addr_t src_start,
				u64 src_size, u64 src_offset,
				u64 len, bool fence,
				void (*cb)(void *cb_ctx), void *cb_ctx)
{
	return ntc_req_memcpy(ntc, req,
			dst_start + dst_offset, src_start + src_offset, len,
			fence, cb, cb_ctx);
}

static inline int ntc_request_memcpy_with_cb(void *req,
					const struct ntc_remote_buf *dst,
					u64 dst_offset,
					const struct ntc_local_buf *src,
					u64 src_offset,
					u64 len,
					void (*cb)(void *cb_ctx), void *cb_ctx)
{
	if (unlikely(len == 0))
		return 0;

	if (!ntc_segment_valid(src->size, src_offset, len))
		return -EINVAL;

	if (!ntc_segment_valid(dst->size, dst_offset, len))
		return -EINVAL;

	if (src->ntc->map_ops->local_buf_prepare_to_copy)
		src->ntc->map_ops->local_buf_prepare_to_copy(src, src_offset,
							len);

	return _ntc_request_memcpy(src->ntc, req,
				dst->dma_addr, dst->size, dst_offset,
				src->dma_addr, src->size, src_offset,
				len, false, cb, cb_ctx);
}

static inline int ntc_request_memcpy_fenced(void *req,
					const struct ntc_remote_buf *dst,
					u64 dst_offset,
					const struct ntc_local_buf *src,
					u64 src_offset,
					u64 len)
{
	if (!ntc_segment_valid(src->size, src_offset, len))
		return -EINVAL;

	if (!ntc_segment_valid(dst->size, dst_offset, len))
		return -EINVAL;

	if (src->ntc->map_ops->local_buf_prepare_to_copy)
		src->ntc->map_ops->local_buf_prepare_to_copy(src, src_offset,
							len);

	return _ntc_request_memcpy(src->ntc, req,
				dst->dma_addr, dst->size, dst_offset,
				src->dma_addr, src->size, src_offset,
				len, true, NULL, NULL);
}

static inline
int ntc_bidir_request_memcpy_unfenced(void *req,
				const struct ntc_remote_buf *dst,
				u64 dst_offset,
				const struct ntc_bidir_buf *src,
				u64 src_offset,
				u64 len)
{
	if (!ntc_segment_valid(src->size, src_offset, len))
		return -EINVAL;

	if (!ntc_segment_valid(dst->size, dst_offset, len))
		return -EINVAL;

	if (src->ntc->map_ops->bidir_buf_prepare_to_copy)
		src->ntc->map_ops->bidir_buf_prepare_to_copy(src, src_offset,
							len);

	return _ntc_request_memcpy(src->ntc, req,
				dst->dma_addr, dst->size, dst_offset,
				src->dma_addr, src->size, src_offset,
				len, false, NULL, NULL);
}

/**
 * ntc_req_imm32() - append a 32 bit immediate data write operation
 * @ntc:	Device context.
 * @req:	Channel request context.
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
static inline int ntc_req_imm32(struct ntc_dev *ntc, void *req,
				dma_addr_t dst, u32 val, bool fence,
				void (*cb)(void *cb_ctx), void *cb_ctx)
{
	return ntc->dev_ops->req_imm32(ntc, req,
				       dst, val, fence,
				       cb, cb_ctx);
}

static inline int _ntc_request_imm32(struct ntc_dev *ntc, void *req,
				dma_addr_t dst_start,
				u64 dst_size, u64 dst_offset,
				u32 val, bool fence,
				void (*cb)(void *cb_ctx), void *cb_ctx)
{
	if (!ntc_segment_valid(dst_size, dst_offset, sizeof(u32)))
		return -EINVAL;

	return ntc_req_imm32(ntc, req, dst_start + dst_offset, val,
			fence, cb, cb_ctx);
}

static inline int ntc_request_imm32(void *req,
				const struct ntc_remote_buf *dst,
				u64 dst_offset,
				u32 val, bool fence,
				void (*cb)(void *cb_ctx), void *cb_ctx)
{
	return _ntc_request_imm32(dst->ntc, req,
				dst->dma_addr, dst->size, dst_offset,
				val, fence, cb, cb_ctx);
}

/**
 * ntc_req_imm64() - append a 64 bit immediate data write operation
 * @ntc:	Device context.
 * @req:	Channel request context.
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
static inline int ntc_req_imm64(struct ntc_dev *ntc, void *req,
				dma_addr_t dst, u64 val, bool fence,
				void (*cb)(void *cb_ctx), void *cb_ctx)
{
	return ntc->dev_ops->req_imm64(ntc, req,
				       dst, val, fence,
				       cb, cb_ctx);
}

static inline int _ntc_request_imm64(struct ntc_dev *ntc, void *req,
				dma_addr_t dst_start,
				u64 dst_size, u64 dst_offset,
				u64 val, bool fence,
				void (*cb)(void *cb_ctx), void *cb_ctx)
{
	if (!ntc_segment_valid(dst_size, dst_offset, sizeof(u64)))
		return -EINVAL;

	return ntc_req_imm64(ntc, req, dst_start + dst_offset, val,
			fence, cb, cb_ctx);
}

static inline int ntc_request_imm64(void *req,
				const struct ntc_remote_buf *dst,
				u64 dst_offset,
				u64 val, bool fence,
				void (*cb)(void *cb_ctx), void *cb_ctx)
{
	return _ntc_request_imm64(dst->ntc, req,
				dst->dma_addr, dst->size, dst_offset,
				val, fence, cb, cb_ctx);
}

/**
 * ntc_req_signal() - append an operation to signal the peer
 * @ntc:	Device context.
 * @req:	Channel request context.
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
static inline int ntc_req_signal(struct ntc_dev *ntc, void *req,
				 void (*cb)(void *cb_ctx), void *cb_ctx, int vec)
{
	return ntc->dev_ops->req_signal(ntc, req,
					cb, cb_ctx, vec);
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

	buf->ntc = ntc;
	buf->dma_engine_dev = ntc_map_dev(ntc, IOAT_DEV_ACCESS);
	buf->size = size;
	buf->owned = true;
	buf->mapped = true;

	ntc_local_buf_clear(buf);

	if (!buf->ntc->map_ops->local_buf_alloc)
		return -ENOENT;

	rc = buf->ntc->map_ops->local_buf_alloc(buf, gfp);

	if (rc < 0) {
		ntc_local_buf_clear(buf);
		return rc;
	}

	if (!buf->ntc->map_ops->local_buf_map)
		return 0;

	rc = buf->ntc->map_ops->local_buf_map(buf);

	if (rc < 0) {
		if (buf->ntc->map_ops->local_buf_free)
			buf->ntc->map_ops->local_buf_free(buf);
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

	buf->ntc = ntc;
	buf->dma_engine_dev = ntc_map_dev(ntc, IOAT_DEV_ACCESS);
	buf->size = size;
	buf->owned = false;
	buf->mapped = true;

	ntc_local_buf_clear(buf);

	buf->ptr = ptr;

	if (!buf->ntc->map_ops->local_buf_map)
		return 0;

	rc = buf->ntc->map_ops->local_buf_map(buf);

	if (rc < 0)
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
static inline void ntc_local_buf_map_dma(struct ntc_local_buf *buf,
					struct ntc_dev *ntc,
					u64 size, dma_addr_t dma_addr)
{
	buf->ntc = ntc;
	buf->dma_engine_dev = ntc_map_dev(ntc, IOAT_DEV_ACCESS);
	buf->size = size;
	buf->owned = false;
	buf->mapped = false;
	buf->ptr = NULL;
	buf->dma_addr = dma_addr;
}

/**
 * ntc_local_buf_free() - unmap and, if necessary, free the local buffer.
 *
 * @buf:	Local buffer.
 */
static inline void ntc_local_buf_free(struct ntc_local_buf *buf)
{
	if (buf->mapped && buf->ntc->map_ops->local_buf_unmap)
		buf->ntc->map_ops->local_buf_unmap(buf);

	if (buf->owned && buf->ntc->map_ops->local_buf_free)
		buf->ntc->map_ops->local_buf_free(buf);

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
static inline void *ntc_local_buf_disown(struct ntc_local_buf *buf)
{
	void *ptr = buf->ptr;

	buf->owned = false;
	ntc_local_buf_free(buf);

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
 * ntc_export_buf_valid() - Check whether the buffer is valid.
 * @buf:	Export buffer.
 *
 * Return:	true iff valid.
 */
static inline bool ntc_export_buf_valid(struct ntc_export_buf *buf)
{
	return !!buf->chan_addr;
}

static inline void ntc_export_buf_clear(struct ntc_export_buf *buf)
{
	if (buf->gfp)
		buf->ptr = NULL;
	buf->chan_addr = 0;
}

/**
 * ntc_export_buf_alloc() - allocate a memory buffer in an NTB window,
 *                          to which the peer can write.
 * @buf:	OUTPUT buffer.
 * @ntc:	Device context.
 * @size:	Size of the buffer to allocate.
 * @gfp:	Allocation flags from gfp.h, or zero.
 * @buf->ptr:	If gfp is zero, buf->ptr must be preset
 *		to the pointer to preallocated buffer.
 *		No memory allocation occurs in this case.
 *
 * Return: zero on success, negative error value on failure.
 */
static inline int ntc_export_buf_alloc(struct ntc_export_buf *buf,
				struct ntc_dev *ntc,
				u64 size, gfp_t gfp)
{
	int rc;

	buf->ntc = ntc;
	buf->ntb_dev = ntc_map_dev(ntc, NTB_DEV_ACCESS);
	buf->size = size;
	buf->gfp = gfp;

	ntc_export_buf_clear(buf);

	if (!buf->ntc->map_ops->export_buf_alloc)
		return -ENOENT;

	rc = buf->ntc->map_ops->export_buf_alloc(buf);

	if (rc < 0)
		ntc_export_buf_clear(buf);

	return rc;
}

/**
 * ntc_export_buf_zalloc() - allocate a memory buffer in an NTB window,
 *                           to which the peer can write,
 *                           and clear the buffer's contents.
 * @buf:	OUTPUT buffer.
 * @ntc:	Device context.
 * @size:	Size of the buffer to allocate.
 * @gfp:	Allocation flags from gfp.h, or zero.
 * @buf->ptr:	If gfp is zero, buf->ptr must be preset
 *		to the pointer to preallocated buffer.
 *		No memory allocation occurs in this case.
 *
 * Calls ntc_export_buf_alloc().
 *
 * Return: zero on success, negative error value on failure.
 */
static inline int ntc_export_buf_zalloc(struct ntc_export_buf *buf,
					struct ntc_dev *ntc,
					u64 size, gfp_t gfp)
{
	gfp_t new_gfp;
	int rc;

	if (gfp)
		new_gfp = gfp | __GFP_ZERO;
	else {
		new_gfp = 0;
		if (size && buf->ptr)
			memset(buf->ptr, 0, size);
	}

	rc = ntc_export_buf_alloc(buf, ntc, size, new_gfp);
	buf->gfp = gfp;

	return rc;
}

/**
 * ntc_export_buf_alloc_init() - allocate a memory buffer in an NTB window,
 *                               to which the peer can write,
 *                               and initialize the buffer's contents.
 * @buf:	OUTPUT buffer.
 * @ntc:	Device context.
 * @size:	Size of the buffer to allocate.
 * @gfp:	Allocation flags from gfp.h, or zero.
 * @buf->ptr:	If gfp is zero, buf->ptr must be preset
 *		to the pointer to preallocated buffer.
 *		No memory allocation occurs in this case.
 * @init_buf:	Pointer to initialization data.
 * @init_buf_size: Size of initialization data.
 * @init_buf_offset: Offset of the initialized data in the buffer.
 *
 * Calls ntc_export_buf_alloc().
 *
 * Return: zero on success, negative error value on failure.
 */
static inline int ntc_export_buf_alloc_init(struct ntc_export_buf *buf,
					struct ntc_dev *ntc,
					u64 size, gfp_t gfp,
					void *init_buf, u64 init_buf_size,
					u64 init_buf_offset)
{
	int rc;


	if (!ntc_segment_valid(size, init_buf_offset, init_buf_size))
		return -EINVAL;

	rc = ntc_export_buf_alloc(buf, ntc, size, gfp);
	if (rc < 0)
		return rc;

	memcpy(buf->ptr + init_buf_offset, init_buf, init_buf_size);

	return rc;
}

/**
 * ntc_export_buf_zalloc_init() - allocate a memory buffer in an NTB window,
 *                                to which the peer can write,
 *                                initialize the buffer's contents,
 *                                and clear the rest of the buffer.
 * @buf:	OUTPUT buffer.
 * @ntc:	Device context.
 * @size:	Size of the buffer to allocate.
 * @gfp:	Allocation flags from gfp.h, or zero.
 * @buf->ptr:	If gfp is zero, buf->ptr must be preset
 *		to the pointer to preallocated buffer.
 *		No memory allocation occurs in this case.
 * @init_buf:	Pointer to initialization data.
 * @init_buf_size: Size of initialization data.
 * @init_buf_offset: Offset of the initialized data in the buffer.
 *
 * Calls ntc_export_buf_alloc().
 *
 * Return: zero on success, negative error value on failure.
 */
static inline int ntc_export_buf_zalloc_init(struct ntc_export_buf *buf,
					struct ntc_dev *ntc,
					u64 size, gfp_t gfp,
					void *init_buf, u64 init_buf_size,
					u64 init_buf_offset)
{
	int rc;

	if (!ntc_segment_valid(size, init_buf_offset, init_buf_size))
		return -EINVAL;

	rc = ntc_export_buf_zalloc(buf, ntc, size, gfp);
	if (rc < 0)
		return rc;

	memcpy(buf->ptr + init_buf_offset, init_buf, init_buf_size);

	return rc;
}

/**
 * ntc_export_buf_free() - unmap and, if necessary, free the buffer
 *                         created by ntc_export_buf_alloc().
 * @buf:	Export buffer.
 */
static inline void ntc_export_buf_free(struct ntc_export_buf *buf)
{
	if (buf->ptr)
		buf->ntc->map_ops->export_buf_free(buf);

	ntc_export_buf_clear(buf);
}

/**
 * ntc_export_buf_disown() - Mark the buffer memory as not owned
 *                           by the buffer and return pointer to it.
 *                           This memory must be kfree()-d.
 *
 * @buf:	Export buffer.
 *
 * After this, the memory will not be freed by ntc_export_buf_free().
 *
 * Return:	Pointer to the buffer's memory.
 */
static inline void *ntc_export_buf_disown(struct ntc_export_buf *buf)
{
	void *ptr = buf->ptr;

	buf->gfp = 0;
	ntc_export_buf_free(buf);

	return ptr;
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
	if (buf->ptr) {
		desc->chan_addr = buf->chan_addr;
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
	if (!buf->ptr)
		return -EINVAL;

	if (!ntc_segment_valid(buf->size, offset, len))
		return -EINVAL;

	desc->chan_addr = buf->chan_addr + offset;
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

	soffset = desc->chan_addr - buf->chan_addr;

	*offset = soffset;
	*len = desc->size;

	if (soffset < 0)
		return -EINVAL;

	if (!ntc_segment_valid(buf->size, soffset, desc->size))
		return -EINVAL;

	if (!buf->ptr)
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
	if (!ntc_segment_valid(buf->size, offset, len))
		return NULL;
	if (unlikely(!buf->ptr))
		return NULL;

	if (buf->ntc->map_ops->export_buf_const_deref)
		return buf->ntc->map_ops->export_buf_const_deref(buf, offset,
								len);
	else
		return buf->ptr + offset;
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
 * ntc_bidir_buf_valid() - Check whether the buffer is valid.
 * @buf:	Buffer created by ntc_bidir_buf_alloc().
 *
 * Return:	true iff valid.
 */
static inline bool ntc_bidir_buf_valid(struct ntc_bidir_buf *buf)
{
	return !!buf->chan_addr;
}

static inline void ntc_bidir_buf_clear(struct ntc_bidir_buf *buf)
{
	if (buf->gfp)
		buf->ptr = NULL;
	buf->chan_addr = 0;
}

/**
 * ntc_bidir_buf_alloc() - allocate a memory buffer in an NTB window,
 *                         to which the peer can write,
 *                         and make it capable of DMA to the DMA engine.
 * @buf:	OUTPUT buffer.
 * @ntc:	Device context.
 * @size:	Size of the buffer to allocate.
 * @gfp:	Allocation flags from gfp.h, or zero.
 * @buf->ptr:	If gfp is zero, buf->ptr must be preset
 *		to the pointer to preallocated buffer.
 *		No memory allocation occurs in this case.
 *
 * Return: zero on success, negative error value on failure.
 */
static inline int ntc_bidir_buf_alloc(struct ntc_bidir_buf *buf,
				struct ntc_dev *ntc,
				u64 size, gfp_t gfp)
{
	int rc;

	buf->ntc = ntc;
	buf->ntb_dev = ntc_map_dev(ntc, NTB_DEV_ACCESS);
	buf->dma_engine_dev = ntc_map_dev(ntc, IOAT_DEV_ACCESS);
	buf->size = size;
	buf->gfp = gfp;

	ntc_bidir_buf_clear(buf);

	if (!buf->ntc->map_ops->bidir_buf_alloc)
		return -ENOENT;

	rc = buf->ntc->map_ops->bidir_buf_alloc(buf);

	if (rc < 0)
		ntc_bidir_buf_clear(buf);

	return rc;
}

/**
 * ntc_bidir_buf_zalloc() - allocate a memory buffer in an NTB window,
 *                          to which the peer can write,
 *                          clear the buffer's contents,
 *                          and make it capable of DMA to the DMA engine.
 * @buf:	OUTPUT buffer.
 * @ntc:	Device context.
 * @size:	Size of the buffer to allocate.
 * @gfp:	Allocation flags from gfp.h, or zero.
 * @buf->ptr:	If gfp is zero, buf->ptr must be preset
 *		to the pointer to preallocated buffer.
 *		No memory allocation occurs in this case.
 *
 * Calls ntc_bidir_buf_alloc().
 *
 * Return: zero on success, negative error value on failure.
 */
static inline int ntc_bidir_buf_zalloc(struct ntc_bidir_buf *buf,
					struct ntc_dev *ntc,
					u64 size, gfp_t gfp)
{
	gfp_t new_gfp;
	int rc;

	if (gfp)
		new_gfp = gfp | __GFP_ZERO;
	else {
		new_gfp = 0;
		if (size && buf->ptr)
			memset(buf->ptr, 0, size);
	}

	rc = ntc_bidir_buf_alloc(buf, ntc, size, new_gfp);
	buf->gfp = gfp;

	return rc;
}

/**
 * ntc_bidir_buf_alloc_init() - allocate a memory buffer in an NTB window,
 *                              to which the peer can write,
 *                              initialize the buffer's contents,
 *                              and make it capable of DMA to the DMA engine.
 * @buf:	OUTPUT buffer.
 * @ntc:	Device context.
 * @size:	Size of the buffer to allocate.
 * @gfp:	Allocation flags from gfp.h, or zero.
 * @buf->ptr:	If gfp is zero, buf->ptr must be preset
 *		to the pointer to preallocated buffer.
 *		No memory allocation occurs in this case.
 * @init_buf:	Pointer to initialization data.
 * @init_buf_size: Size of initialization data.
 * @init_buf_offset: Offset of the initialized data in the buffer.
 *
 * Calls ntc_bidir_buf_alloc().
 *
 * Return: zero on success, negative error value on failure.
 */
static inline int ntc_bidir_buf_alloc_init(struct ntc_bidir_buf *buf,
					struct ntc_dev *ntc,
					u64 size, gfp_t gfp,
					void *init_buf, u64 init_buf_size,
					u64 init_buf_offset)
{
	int rc;

	if (!ntc_segment_valid(size, init_buf_offset, init_buf_size))
		return -EINVAL;

	rc = ntc_bidir_buf_alloc(buf, ntc, size, gfp);
	if (rc < 0)
		return rc;

	memcpy(buf->ptr + init_buf_offset, init_buf, init_buf_size);

	return rc;
}

/**
 * ntc_bidir_buf_zalloc_init() - allocate a memory buffer in an NTB window,
 *                               to which the peer can write,
 *                               initialize the buffer's contents,
 *                               clear the rest of the buffer,
 *                               and make it capable of DMA to the DMA engine.
 * @buf:	OUTPUT buffer.
 * @ntc:	Device context.
 * @size:	Size of the buffer to allocate.
 * @gfp:	Allocation flags from gfp.h, or zero.
 * @buf->ptr:	If gfp is zero, buf->ptr must be preset
 *		to the pointer to preallocated buffer.
 *		No memory allocation occurs in this case.
 * @init_buf:	Pointer to initialization data.
 * @init_buf_size: Size of initialization data.
 * @init_buf_offset: Offset of the initialized data in the buffer.
 *
 * Calls ntc_bidir_buf_alloc().
 *
 * Return: zero on success, negative error value on failure.
 */
static inline int ntc_bidir_buf_zalloc_init(struct ntc_bidir_buf *buf,
					struct ntc_dev *ntc,
					u64 size, gfp_t gfp,
					void *init_buf, u64 init_buf_size,
					u64 init_buf_offset)
{
	int rc;

	if (!ntc_segment_valid(size, init_buf_offset, init_buf_size))
		return -EINVAL;

	rc = ntc_bidir_buf_zalloc(buf, ntc, size, gfp);
	if (rc < 0)
		return rc;

	memcpy(buf->ptr + init_buf_offset, init_buf, init_buf_size);

	return rc;
}

/**
 * ntc_bidir_buf_make_prealloced() - prepare a memory buffer in an NTB window,
 *                                   to which the peer can write,
 *                                 and make it capable of DMA to the DMA engine.
 * @buf:	OUTPUT buffer.
 * @ntc:	Device context.
 * @buf->size:	Preset to size of the buffer.
 * @buf->ptr:	Preset to the pointer to preallocated buffer.
 *		No memory allocation occurs.
 *
 * Calls ntc_bidir_buf_alloc().
 *
 * Return: zero on success, negative error value on failure.
 */
static inline int ntc_bidir_buf_make_prealloced(struct ntc_bidir_buf *buf,
						struct ntc_dev *ntc)
{
	return ntc_bidir_buf_alloc(buf, ntc, buf->size, 0);
}

/**
 * ntc_bidir_buf_free() - unmap and, if necessary, free the buffer
 *                        created by ntc_bidir_buf_alloc().
 * @buf:	Buffer created by ntc_bidir_buf_alloc().
 */
static inline void ntc_bidir_buf_free(struct ntc_bidir_buf *buf)
{
	if (buf->ptr)
		buf->ntc->map_ops->bidir_buf_free(buf);

	ntc_bidir_buf_clear(buf);
}

/**
 * ntc_bidir_buf_free_sgl() - unmap and, if necessary, free each buffer
 *                            in the array of buffers
 *                            created by ntc_bidir_buf_alloc().
 * @sgl:	Array of buffers created by ntc_bidir_buf_alloc().
 * @count:	Size of the array.
 */
static inline void ntc_bidir_buf_free_sgl(struct ntc_bidir_buf *sgl, int count)
{
	int i;

	for (i = 0; i < count; i++)
		ntc_bidir_buf_free(&sgl[i]);
}

/**
 * ntc_bidir_buf_make_prealloced_sgl() - prepare memory buffer in an NTB window,
 *                                       to which the peer can write,
 *                                       make it capable of DMA to DMA engine --
 *                                       for each buffer in the array.
 * @sgl:	Array of buffers to be created.
 * @count:	Size of the array.
 * @ntc:	Device context.
 * @sgl[i]->size: Preset to size of the buffer.
 * @sgl[i]->ptr: Preset to the pointer to preallocated buffer.
 *		 No memory allocation occurs.
 *
 * Calls ntc_bidir_buf_make_prealloced().
 *
 * Return: zero on success, negative error value on failure.
 */
static inline int ntc_bidir_buf_make_prealloced_sgl(struct ntc_bidir_buf *sgl,
						int count, struct ntc_dev *ntc)
{
	int i, rc;

	for (i = 0; i < count; i++) {
		rc = ntc_bidir_buf_make_prealloced(&sgl[i], ntc);
		if (rc < 0)
			goto err;
	}

	return 0;
 err:
	ntc_bidir_buf_free_sgl(sgl, i);
	return rc;
}

/**
 * ntc_bidir_buf_make_desc() - Make serializable description of buffer.
 *
 * @desc:	OUTPUT buffer description, which can be sent to peer.
 * @buf:	Buffer created by ntc_bidir_buf_alloc().
 */
static inline void ntc_bidir_buf_make_desc(struct ntc_remote_buf_desc *desc,
					struct ntc_bidir_buf *buf)
{
	if (buf->ptr) {
		desc->chan_addr = buf->chan_addr;
		desc->size = buf->size;
	} else
		memset(desc, 0, sizeof(*desc));
}

/**
 * ntc_bidir_buf_deref() - Retrieve pointer into buffer's memory.
 * @buf:	Buffer created by ntc_bidir_buf_alloc().
 * @offset:	Offset to the segment to be accessed.
 * @len:	Length of the segment to be accessed.
 *
 * Any necessary synchronization will be made before returning the pointer.
 * After finishing the work with the buffer,
 * ntc_bidir_buf_unref() must be called.
 *
 * Return:	The pointer into the buffer's memory at the given offset,
 *		or NULL on error.
 */
static inline void *ntc_bidir_buf_deref(struct ntc_bidir_buf *buf,
					u64 offset, u64 len)
{
	if (!ntc_segment_valid(buf->size, offset, len))
		return NULL;
	if (unlikely(!buf->ptr))
		return NULL;

	if (buf->ntc->map_ops->bidir_buf_deref)
		return buf->ntc->map_ops->bidir_buf_deref(buf, offset, len);
	else
		return buf->ptr + offset;
}

/**
 * ntc_bidir_buf_unref() - Finish working with ntc_bidir_buf_deref() pointer.
 * @buf:	Buffer created by ntc_bidir_buf_alloc().
 * @offset:	Offset to the segment to be accessed.
 * @len:	Length of the segment to be accessed.
 *
 * Any necessary synchronization will be made before returning.
 */
static inline void ntc_bidir_buf_unref(struct ntc_bidir_buf *buf,
				u64 offset, u64 len)
{
	if (!ntc_segment_valid(buf->size, offset, len))
		return;
	if (unlikely(!buf->ptr))
		return;

	if (buf->ntc->map_ops->bidir_buf_unref)
		buf->ntc->map_ops->bidir_buf_unref(buf, offset, len);
}

/**
 * ntc_bidir_buf_deref() - Retrieve pointer to buffer's memory.
 * @buf:	Buffer created by ntc_bidir_buf_alloc().
 *
 * Any necessary synchronization will be made before returning the pointer.
 * After finishing the work with the buffer,
 * ntc_bidir_buf_unref() must be called.
 *
 * Return:	The pointer to the buffer's memory, or NULL on error.
 */
static inline void *ntc_bidir_buf_full_deref(struct ntc_bidir_buf *buf)
{
	return ntc_bidir_buf_deref(buf, 0, buf->size);
}

/**
 * ntc_bidir_buf_const_deref() - Retrieve const pointer into buffer's memory.
 * @buf:	Buffer created by ntc_bidir_buf_alloc().
 * @offset:	Offset to the segment to be accessed.
 * @len:	Length of the segment to be accessed.
 *
 * Any necessary synchronization will be made before returning the pointer.
 *
 * Return:	The const pointer into the buffer's memory at the given offset,
 *		or NULL on error.
 */
static inline const void *ntc_bidir_buf_const_deref(struct ntc_bidir_buf *buf,
						u64 offset, u64 len)
{
	if (!ntc_segment_valid(buf->size, offset, len))
		return NULL;
	if (unlikely(!buf->ptr))
		return NULL;

	if (buf->ntc->map_ops->bidir_buf_deref)
		return buf->ntc->map_ops->bidir_buf_const_deref(buf, offset,
								len);
	else
		return buf->ptr + offset;
}

/**
 * ntc_bidir_buf_seq_write() - Write out the buffer's contents.
 * @s:		The first argument of seq_write().
 * @buf:	Buffer created by ntc_bidir_buf_alloc().
 *
 * Return:	The result of seq_write().
 */
static inline int ntc_bidir_buf_seq_write(struct seq_file *s,
					struct ntc_bidir_buf *buf)
{
	const void *p;

	p = ntc_bidir_buf_const_deref(buf, 0, buf->size);

	if (!p)
		return 0;

	return seq_write(s, p, buf->size);
}

static inline void ntc_remote_buf_desc_clear(struct ntc_remote_buf_desc *desc)
{
	memset(desc, 0, sizeof(*desc));
}

static inline int
ntc_remote_buf_desc_clip(struct ntc_remote_buf_desc *desc, u64 offset, u64 len)
{
	if (!ntc_segment_valid(desc->size, offset, len))
		return -EINVAL;

	desc->chan_addr += offset;
	desc->size = len;

	return 0;
}

static inline void ntc_remote_buf_clear(struct ntc_remote_buf *buf)
{
	buf->ptr = 0;
	buf->dma_addr = 0;
}

/**
 * ntc_remote_buf_map() - map a remote buffer via an NTB window,
 *                        to write to the peer,
 *                        and make it capable of DMA from the DMA engine.
 * @buf:	OUTPUT remote buffer.
 * @ntc:	Device context.
 * @desc:	The remote buffer description created by
 *		ntc_export_buf_make_desc() or ntc_bidir_buf_make_desc().
 *
 * Return: zero on success, negative error value on failure.
 */
static inline int ntc_remote_buf_map(struct ntc_remote_buf *buf,
				struct ntc_dev *ntc,
				const struct ntc_remote_buf_desc *desc)
{
	int rc;

	ntc_remote_buf_clear(buf);

	buf->ntc = ntc;
	buf->dma_engine_dev = ntc_map_dev(ntc, IOAT_DEV_ACCESS);

	if (!buf->ntc->map_ops->remote_buf_map)
		return -ENOENT;

	rc = buf->ntc->map_ops->remote_buf_map(buf, desc);
	if (rc < 0)
		ntc_remote_buf_clear(buf);

	return rc;
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
	int rc;

	ntc_remote_buf_clear(buf);

	buf->ntc = ntc;
	buf->dma_engine_dev = ntc_map_dev(ntc, IOAT_DEV_ACCESS);

	if (!buf->ntc->map_ops->remote_buf_map_phys)
		return -ENOENT;

	rc = buf->ntc->map_ops->remote_buf_map_phys(buf, ptr, size);
	if (rc < 0)
		ntc_remote_buf_clear(buf);

	return rc;
}

/**
 * ntc_remote_buf_map_phys() - unmap a remote buffer.
 * @buf:	remote buffer created by
 *		ntc_remote_buf_map() or ntc_remote_buf_map_phys().
 */
static inline void ntc_remote_buf_unmap(struct ntc_remote_buf *buf)
{
	if (buf->dma_addr)
		buf->ntc->map_ops->remote_buf_unmap(buf);

	ntc_remote_buf_clear(buf);
}

/**
 * ntc_umem_sgl() - store discontiguous ranges in a scatter gather list
 * @ntc:	Device context.
 * @ib_umem:	Object representing the memory mapping.
 * @sgl:	Scatter gather ntc_bidir_buf list to receive the entries.
 * @count:	Capacity of the scatter gather list.
 *
 * Count the number of discontiguous ranges in the channel mapping, so that a
 * scatter gather list of the exact length can be allocated.
 *
 * Return: The number of entries stored in the list, or an error number.
 */
int ntc_umem_sgl(struct ntc_dev *ntc, struct ib_umem *ib_umem,
		struct ntc_bidir_buf *sgl, int count);

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
	return ntc_umem_sgl(ntc, ib_umem, NULL, 0);
}

/**
 * ntc_clear_signal() - clear the signal asserting event
 * @ntc:	Device context.
 *
 * Clear the signal that's asserted for events (interrupts perhaps).
 *
 * Return: 1 for events waiting or 0 for no events.
 */
static inline int ntc_clear_signal(struct ntc_dev *ntc, int vec)
{
	if (!ntc->dev_ops->clear_signal)
		return 0;

	return ntc->dev_ops->clear_signal(ntc, vec);
}

#endif
