/*
 * Copyright (c) 2019 Dell Technologies.  All rights reserved.
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

#ifndef _NTC_INTERNAL_H_
#define _NTC_INTERNAL_H_

#include "ntc.h"

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
static inline int ntc_ctx_hello(struct ntc_dev *ntc, int phase,
				void *in_buf, size_t in_size,
				void *out_buf, size_t out_size)
{
	const struct ntc_ctx_ops *ctx_ops;
	void *ctx;

	ntc_dbg(ntc, "hello phase %d", phase);

	ctx = ntc_get_ctx(ntc);
	rmb(); /* if ctx_ops is set, drvdata must be set */
	ctx_ops = ntc->ctx_ops;

	if (ctx_ops && ctx_ops->hello)
		return ctx_ops->hello(ctx, phase,
				      in_buf, in_size,
				      out_buf, out_size);

	if (phase || in_size || out_size)
		return -EINVAL;

	return 0;
}

/**
 * ntc_ctx_enable() - notify driver that the link is active
 * @ntc:	Device context.
 *
 * Notify the driver context that the link is active.  The driver may begin
 * issuing requests.
 */
static inline int ntc_ctx_enable(struct ntc_dev *ntc)
{
	const struct ntc_ctx_ops *ctx_ops;
	void *ctx;
	int ret = 0;

	ntc_dbg(ntc, "enable");

	ctx = ntc_get_ctx(ntc);
	rmb(); /* if ctx_ops is set, drvdata must be set */
	ctx_ops = ntc->ctx_ops;

	if (ctx_ops)
		ret = ctx_ops->enable(ctx);

	return ret;
}

/**
 * ntc_ctx_disable() - notify driver that the link is not active
 * @ntc:	Device context.
 *
 * Notify the driver context that the link is not active.  The driver should
 * stop issuing requests.  See ntc_ctx_quiesce().
 */
static inline void ntc_ctx_disable(struct ntc_dev *ntc)
{
	const struct ntc_ctx_ops *ctx_ops;
	void *ctx;

	ntc_dbg(ntc, "disable");

	ctx = ntc_get_ctx(ntc);
	rmb(); /* if ctx_ops is set, drvdata must be set */
	ctx_ops = ntc->ctx_ops;

	if (ctx_ops)
		ctx_ops->disable(ctx);
}

/**
 * ntc_ctx_quiesce() - make sure driver is not issuing any new requests
 * @ntc:	Device context.
 *
 * Make sure the driver context is not issuing any new requests.  The driver
 * should block and wait for any asynchronous operations to stop.  It is not
 * necessary to wait for previously issued requests to complete, just that no
 * new requests will be issued after this function returns.
 */
static inline void ntc_ctx_quiesce(struct ntc_dev *ntc)
{
	const struct ntc_ctx_ops *ctx_ops;
	void *ctx;

	ntc_dbg(ntc, "quiesce");

	ctx = ntc_get_ctx(ntc);
	rmb(); /* if ctx_ops is set, drvdata must be set */
	ctx_ops = ntc->ctx_ops;

	if (ctx_ops && ctx_ops->quiesce)
		ctx_ops->quiesce(ctx);
}

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
static inline void ntc_ctx_reset(struct ntc_dev *ntc)
{
	const struct ntc_ctx_ops *ctx_ops;
	void *ctx;

	ntc_dbg(ntc, "reset");

	ctx = ntc_get_ctx(ntc);
	rmb(); /* if ctx_ops is set, drvdata must be set */
	ctx_ops = ntc->ctx_ops;

	if (ctx_ops && ctx_ops->reset)
		ctx_ops->reset(ctx);
}

/**
 * ntc_ctx_signal() - notify driver context of a signal event
 * @ntc:	Device context.
 * @vector:	Interrupt vector number.
 *
 * Notify the driver context of a signal event triggered by the peer.
 */
static inline void ntc_ctx_signal(struct ntc_dev *ntc, int vec)
{
	const struct ntc_ctx_ops *ctx_ops;
	void *ctx;

	ntc_vdbg(ntc, "signal");

	ctx = ntc_get_ctx(ntc);
	rmb(); /* if ctx_ops is set, drvdata must be set */
	ctx_ops = ntc->ctx_ops;

	if (ctx_ops && ctx_ops->signal)
		ctx_ops->signal(ctx, vec);
}

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
static inline int ntc_register_device(struct ntc_dev *ntc)
{
	pr_devel("register device %s\n", dev_name(&ntc->dev));

	ntc->ctx_ops = NULL;

	ntc->dev.bus = ntc_bus_ptr();

	return device_register(&ntc->dev);
}

/**
 * ntc_unregister_device() - unregister a ntc device
 * @ntc:	Device context.
 *
 * The device will be removed from the list of ntc devices.  If the ntc device
 * is associated with a driver, the driver will be notified to remove the
 * device.
 */
static inline void ntc_unregister_device(struct ntc_dev *ntc)
{
	pr_devel("unregister device %s\n", dev_name(&ntc->dev));

	device_unregister(&ntc->dev);

	ntc->dev.bus = NULL;
}

int __init ntc_init(void);
void __exit ntc_exit(void);

#endif
