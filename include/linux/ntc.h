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

#include <linux/device.h>
#include <linux/dma-direction.h>
#include <linux/rwlock.h>
#include <rdma/ib_verbs.h>

#define NTB_MAX_IRQS (64)

struct ntc_driver;
struct ntc_dev;

struct ib_ucontext;

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
	void (*enable)(void *ctx);
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
	struct device *(*map_dev)(struct ntc_dev *ntc);

	int (*link_disable)(struct ntc_dev *ntc);
	int (*link_enable)(struct ntc_dev *ntc);
	int (*link_reset)(struct ntc_dev *ntc);

	u64 (*peer_addr)(struct ntc_dev *ntc, u64 addr);

	void *(*req_create)(struct ntc_dev *ntc);
	void (*req_cancel)(struct ntc_dev *ntc, void *req);
	int (*req_submit)(struct ntc_dev *ntc, void *req);

	int (*req_memcpy)(struct ntc_dev *ntc, void *req,
			  u64 dst, u64 src, u64 len, bool fence,
			  void (*cb)(void *cb_ctx), void *cb_ctx);
	int (*req_imm32)(struct ntc_dev *ntc, void *req,
			 u64 dst, u32 val, bool fence,
			 void (*cb)(void *cb_ctx), void *cb_ctx);
	int (*req_imm64)(struct ntc_dev *ntc, void *req,
			 u64 dst, u64 val, bool fence,
			 void (*cb)(void *cb_ctx), void *cb_ctx);
	int (*req_signal)(struct ntc_dev *ntc, void *req,
			  void (*cb)(void *cb_ctx), void *cb_ctx, int vec);
	int (*clear_signal)(struct ntc_dev *ntc, int vec);
	int (*max_peer_irqs)(struct ntc_dev *ntc);
	void *(*local_hello_buf)(struct ntc_dev *ntc, int *size);
	void *(*peer_hello_buf)(struct ntc_dev *ntc, int *size);
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

/** struct ntc_sge - ntc memory mapping scatter gather entry
 * @addr		Channel mapped address.
 * @len			Length of entry.
 */
struct ntc_sge {
	u64 addr;
	u64 len;
};

/** struct ntc_map_ops - ntc memory mapping operations
 * @buf_alloc		See ntc_buf_alloc().
 * @buf_free		See ntc_buf_free().
 * @buf_map		See ntc_buf_map().
 * @buf_unmap		See ntc_buf_unmap().
 * @buf_sync_cpu	See ntc_buf_sync_cpu().
 * @buf_sync_dev	See ntc_buf_sync_dev().
 * @umem_get		See ntc_umem_get().
 * @umem_put		See ntc_umem_put().
 * @umem_sgl		See ntc_umem_sgl().
 * @umem_count		See ntc_umem_count().
 */
struct ntc_map_ops {
	void *(*buf_alloc)(struct ntc_dev *ntc, u64 size, u64 *addr, gfp_t gfp);
	void (*buf_free)(struct ntc_dev *ntc, u64 size, void *buf, u64 addr);

	u64 (*buf_map)(struct ntc_dev *ntc, void *buf, u64 size,
		       enum dma_data_direction dir);
	void (*buf_unmap)(struct ntc_dev *ntc, u64 addr, u64 size,
			  enum dma_data_direction dir);
	void (*buf_sync_cpu)(struct ntc_dev *ntc, u64 addr, u64 size,
			     enum dma_data_direction dir);
	void (*buf_sync_dev)(struct ntc_dev *ntc, u64 addr, u64 size,
			     enum dma_data_direction dir);

	void *(*umem_get)(struct ib_udata *udata,
			  unsigned long uaddr, size_t size,
			  int access, int dmasync);
	void (*umem_put)(struct ntc_dev *ntc, void *umem);
	int (*umem_sgl)(struct ntc_dev *ntc, void *umem,
			struct ntc_sge *sgl, int count);
	int (*umem_count)(struct ntc_dev *ntc, void *umem);
};

/* Virtual mapping ops provided by ntc_virt library module */
extern struct ntc_map_ops ntc_virt_map_ops;

/* Hardware DMA mapping ops provided by ntc_phys library module */
extern struct ntc_map_ops ntc_phys_map_ops;

static inline int ntc_map_ops_is_valid(const struct ntc_map_ops *ops)
{
	/* commented callbacks are not required: */
	return
		ops->buf_alloc				&&
		ops->buf_free				&&

		ops->buf_map				&&
		ops->buf_unmap				&&
		/* ops->buf_sync_cpu			&& */
		/* ops->buf_sync_dev			&& */

		ops->umem_get				&&
		ops->umem_put				&&
		ops->umem_sgl				&&
		1;
}

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
void ntc_ctx_enable(struct ntc_dev *ntc);

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
static inline struct device *ntc_map_dev(struct ntc_dev *ntc)
{
	if (!ntc->dev_ops->map_dev)
		return &ntc->dev;

	return ntc->dev_ops->map_dev(ntc);
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
static inline int ntc_link_disable(struct ntc_dev *ntc)
{
	return ntc->dev_ops->link_disable(ntc);
}

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
static inline int ntc_link_reset(struct ntc_dev *ntc)
{
	return ntc->dev_ops->link_reset(ntc);
}

/**
 * ntc_peer_addr() - transform a channel-mapped address into a peer address
 * @ntc:	Device context.
 * @addr:	Channel-mapped address of a remote buffer.
 *
 * Transform a channel-mapped address of a remote buffer, mapped by the peer,
 * into a peer address to be used as the destination of a channel request.
 *
 * The peer allocates the buffer with ntc_buf_alloc(), maps a kernel space
 * buffer with ntc_buf_map(), or maps a user space buffer with ntc_umem_get().
 * Then, the remote peer communicates the channel-mapped address of the buffer
 * across the channel.  The driver on side of the connection must then
 * translate the peer's channel-mapped address into a peer address.  The
 * translated peer address may then be used as the destination address of a
 * channel request.
 *
 * Return: The translated peer address.
 */
static inline u64 ntc_peer_addr(struct ntc_dev *ntc, u64 addr)
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
 * See ntc_buf_alloc() and ntc_buf_map().
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
				 u64 dst, u64 src, u64 len, bool fence,
				 void (*cb)(void *cb_ctx), void *cb_ctx)
{
	return ntc->dev_ops->req_memcpy(ntc, req,
					dst, src, len, fence,
					cb, cb_ctx);
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
				u64 dst, u32 val, bool fence,
				void (*cb)(void *cb_ctx), void *cb_ctx)
{
	return ntc->dev_ops->req_imm32(ntc, req,
				       dst, val, fence,
				       cb, cb_ctx);
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
				u64 dst, u64 val, bool fence,
				void (*cb)(void *cb_ctx), void *cb_ctx)
{
	return ntc->dev_ops->req_imm64(ntc, req,
				       dst, val, fence,
				       cb, cb_ctx);
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

/**
 * ntc_buf_alloc() - allocate a coherent channel-mapped memory buffer
 * @ntc:	Device context.
 * @size:	Size of the buffer to allocate.
 * @addr:	OUT - channel-mapped buffer address.
 * @gfp:	Allocation flags from gfp.h.
 *
 * Allocate a coherent channel-mapped memory buffer, to use as the source of a
 * channel request, or the destination of a channel request by the peer.  The
 * channel-mapped address of the buffer is returned as an output parameter.
 *
 * This allocates a coherent buffer that does not need to be explicitly synced,
 * but there may be additional hardware overhead due to maintenaning cache
 * coherency between all local and remote accesses to the buffer.
 *
 * Return: A pointer to the channel-mapped buffer, or NULL.
 */
static inline void *ntc_buf_alloc(struct ntc_dev *ntc, u64 size,
				  u64 *addr, gfp_t gfp)
{
	return ntc->map_ops->buf_alloc(ntc, size, addr, gfp);
}

/**
 * ntc_buf_free() - free a coherent channel-mapped memory buffer
 * @ntc:	Device context.
 * @size:	Size of the buffer to allocate.
 * @buf:	Pointer to the channel-mapped buffer.
 * @addr:	Channel-mapped buffer address.
 *
 * Free a coherent channel-mapped memory buffer, allocated by ntc_buf_alloc().
 */
static inline void ntc_buf_free(struct ntc_dev *ntc, u64 size,
				void *buf, u64 addr)
{
	ntc->map_ops->buf_free(ntc, size, buf, addr);
}

/**
 * ntc_buf_map() - channel-map a kernel allocated memory buffer
 * @ntc:	Device context.
 * @buf:	Pointer to the kernel memory buffer.
 * @size:	Size of the buffer to map.
 * @dir:	Direction of data movement.
 *
 * Channel-map a kernel allocated memory buffer, to use as the source of a
 * channel request, or the destination of a channel request by the peer.  The
 * direction specifies whether the buffer will be used as a source,
 * destination, or both.
 *
 * Data movement and access must be properly synchronized, to be portable.  See
 * ntc_buf_sync_cpu() and ntc_buf_sync_dev().
 *
 * This function is modelled based on dma_map_single(), however the channel may
 * provide a mapping that is different from dma in its implementation.
 *
 * Return: The channel-mapped buffer address, or Zero.
 */
static inline u64 ntc_buf_map(struct ntc_dev *ntc, void *buf, u64 size,
			      enum dma_data_direction dir)
{
	return ntc->map_ops->buf_map(ntc, buf, size, dir);
}

/**
 * ntc_buf_unmap() - unmap a channel-mapped kernel memory buffer
 * @ntc:	Device context.
 * @addr:	Channel-mapped address of the buffer.
 * @size:	Size of the mapped buffer.
 * @dir:	Direction of data movement.
 *
 * Unmap a channel-mapped memory buffer, previously mapped by ntc_buf_map().
 *
 * Unmapping the buffer implies a final sync of data into the buffer, for use
 * by the cpu.  When unmapping, the driver need not explicitly call
 * ntc_buf_sync_cpu() to sync the data in the buffer.
 */
static inline void ntc_buf_unmap(struct ntc_dev *ntc, u64 addr, u64 size,
				 enum dma_data_direction dir)
{
	ntc->map_ops->buf_unmap(ntc, addr, size, dir);
}

/**
 * ntc_buf_sync_cpu() - sync a channel-mapped buffer for cpu access
 * @ntc:	Device context.
 * @addr:	Channel-mapped address of the buffer.
 * @size:	Size of the mapped buffer.
 * @dir:	Direction of data movement.
 *
 * Sync a channel-mapped memory buffer, previously mapped by ntc_buf_map(), for
 * cpu access to the data.  This ensures that any data written by the peer into
 * the buffer, before the sync, is available for the cpu, after the sync.
 */
static inline void ntc_buf_sync_cpu(struct ntc_dev *ntc, u64 addr, u64 size,
				    enum dma_data_direction dir)
{
	if (ntc->map_ops->buf_sync_cpu)
		ntc->map_ops->buf_sync_cpu(ntc, addr, size, dir);
}

/**
 * ntc_buf_sync_dev() - sync a channel-mapped buffer for device access
 * @ntc:	Device context.
 * @addr:	Channel-mapped address of the buffer.
 * @size:	Size of the mapped buffer.
 * @dir:	Direction of data movement.
 *
 * Sync a channel-mapped memory buffer, previously mapped by ntc_buf_map(), for
 * device access to the data.  This ensures that any data written by the by the
 * cpu into the buffer, before the sync, is available for the channel device,
 * after the sync.
 */
static inline void ntc_buf_sync_dev(struct ntc_dev *ntc, u64 addr, u64 size,
				    enum dma_data_direction dir)
{
	if (ntc->map_ops->buf_sync_dev)
		ntc->map_ops->buf_sync_dev(ntc, addr, size, dir);
}

/**
 * ntc_umem_get() - channel-map a user allocated memory buffer
 * @ntc:        Device context.
 * @udata:	Userspace memory context.
 * @uaddr:	User allocated memory virtual address.
 * @size:	Size of the user memory buffer.
 * @access:	Infiniband access flags for ib_umem_get().
 * @dmasync:	Infiniband dmasync option for ib_umem_get().
 *
 * Channel-map a user allocated memory buffer, to use as the source of a
 * channel request, or the destination of a channel request by the peer.
 *
 * The access flags specify whether the buffer will be used as a source,
 * destination, or both.  The dmasync option specifies that in-flight dma
 * should be flushed when the memory is written, to prevent the write from
 * modifying the contents of the source buffer in the middle of the dma
 * operation.
 *
 * Data movement and access must be properly synchronized, to be portable.  See
 * ntc_buf_sync_cpu() and ntc_buf_sync_dev().
 *
 * This function is modelled based on ib_umem_get(), however the channel may
 * provide a mapping that is different from ib_umem in its implementation.
 *
 * Return: An object repreenting the memory mapping, or an error pointer.
 *
 * FIXME: I wish there was a way
 * to make this more generic, without rewriting ib_umem_get().
 */
static inline void *ntc_umem_get(struct ntc_dev *ntc, struct ib_udata *udata,
				 unsigned long uaddr, size_t size,
				 int access, int dmasync)
{
	return ntc->map_ops->umem_get(udata, uaddr,
				      size, access, dmasync);
}

/**
 * ntc_umem_put() - unmap a channel-mapped user memory buffer
 * @ntc:	Device context.
 * @umem:	Object representing the memory mapping.
 *
 * Unmap a channel-mapped user memory buffer, previously mapped by
 * ntc_umem_get().
 */
static inline void ntc_umem_put(struct ntc_dev *ntc, void *umem)
{
	ntc->map_ops->umem_put(ntc, umem);
}

/**
 * ntc_umem_sgl() - store discontiguous ranges in a scatter gather list
 * @ntc:	Device context.
 * @umem:	Object representing the memory mapping.
 * @sgl:	Scatter gather list to receive the entries.
 * @count:	Capacity of the scatter gather list.
 *
 * Count the number of discontiguous ranges in the channel mapping, so that a
 * scatter gather list of the exact length can be allocated.
 *
 * Return: The number of entries stored in the list, or an error number.
 */
static inline int ntc_umem_sgl(struct ntc_dev *ntc, void *umem,
			       struct ntc_sge *sgl, int count)
{
	return ntc->map_ops->umem_sgl(ntc, umem, sgl, count);
}

/**
 * ntc_umem_count() - count discontiguous ranges in the channel mapping
 * @ntc:	Device context.
 * @umem:	Object representing the memory mapping.
 *
 * Count the number of discontiguous ranges in the channel mapping, so that a
 * scatter gather list of the exact length can be allocated.
 *
 * Return: The number of discontiguous ranges, or an error number.
 */
static inline int ntc_umem_count(struct ntc_dev *ntc, void *umem)
{
	if (!ntc->map_ops->umem_count)
		return ntc_umem_sgl(ntc, umem, NULL, 0);

	return ntc->map_ops->umem_count(ntc, umem);
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
