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

#include "ntrdma_ntb_impl.h"

#include "ntrdma_ntb.h"
#include "ntrdma_ping.h"
#include "ntrdma_port.h"

static void ntrdma_ping_pong(struct ntrdma_ntb_dev *dev);
static void ntrdma_ping_poll(struct ntrdma_ntb_dev *dev);

static ntrdma_u32_t ntrdma_ping_read(struct ntrdma_ntb_dev *dev);
static void ntrdma_ping_write_peer(struct ntrdma_ntb_dev *dev, ntrdma_u32_t val);

static NTRDMA_DECL_TIM_CB(ntrdma_ping_pong_work, ptrhld);
static NTRDMA_DECL_TIM_CB(ntrdma_ping_poll_work, ptrhld);

int ntrdma_ping_init(struct ntrdma_ntb_dev *dev)
{
	dev->ping_run = 0;
	dev->ping_flags = 0;
	dev->ping_seq = 0;
	dev->ping_msg = 0;
	dev->ping_val = 0;
	dev->ping_miss = NTRDMA_PING_MISS_MAX;

	ntrdma_tim_create(&dev->ping_pong, "ntrdma_ping_pong",
			  ntrdma_ping_pong_work, dev);

	ntrdma_tim_create(&dev->ping_poll, "ntrdma_ping_poll",
			  ntrdma_ping_poll_work, dev);

	ntrdma_spl_create(&dev->ping_lock, "ntrdma_ping_lock");

	return 0;
}

void ntrdma_ping_deinit(struct ntrdma_ntb_dev *dev)
{
	ntrdma_tim_destroy(&dev->ping_pong);
	ntrdma_tim_destroy(&dev->ping_poll);
}

void ntrdma_ping_start(struct ntrdma_ntb_dev *dev)
{
	ntrdma_spl_lock(&dev->ping_lock);
	if (!dev->ping_run) {
		dev->ping_run = 1;

		ntrdma_ping_pong(dev);

		dev->ping_miss = NTRDMA_PING_MISS_MAX;
		dev->ping_val = ntrdma_ping_read(dev);

		ntrdma_tim_fire(&dev->ping_poll, NTRDMA_PING_POLL_MS);
	}
	ntrdma_spl_unlock(&dev->ping_lock);
}

void ntrdma_ping_stop(struct ntrdma_ntb_dev *dev)
{
	ntrdma_spl_lock(&dev->ping_lock);
	if (dev->ping_run) {
		dev->ping_run = 0;

		ntrdma_tim_cancel_async(&dev->ping_pong);

		ntrdma_tim_cancel_async(&dev->ping_poll);
	}
	ntrdma_spl_unlock(&dev->ping_lock);
}

void ntrdma_ping_send(struct ntrdma_ntb_dev *dev, int msg, int flags)
{
	NTRDMA_HARD_ASSERT(!(msg >> 16));

	ntrdma_spl_lock(&dev->ping_lock);
	{
		dev->ping_msg = msg;

		dev->ping_flags |= flags;

		ntrdma_dbg(&dev->dev, "send msg %d\n", msg);
		ntrdma_ping_pong(dev);

		dev->ping_flags = flags;
	}
	ntrdma_spl_unlock(&dev->ping_lock);
}

int ntrdma_ping_recv(struct ntrdma_ntb_dev *dev)
{
	int msg;

	ntrdma_spl_lock(&dev->ping_lock);
	{
		if (dev->ping_miss < NTRDMA_PING_MISS_MAX)
			msg = ntrdma_pingval_state(dev->ping_val);
		else
			msg = 0;
	}
	ntrdma_spl_unlock(&dev->ping_lock);

	ntrdma_dbg(&dev->dev, "recv msg %d\n", msg);
	return msg;
}

static void ntrdma_ping_pong(struct ntrdma_ntb_dev *dev)
{
	ntrdma_u32_t val;

	if (dev->ping_run) {
		ntrdma_vdbg(&dev->dev, "run\n");
		val = ntrdma_pingval(dev->ping_msg, ++dev->ping_seq);

		ntrdma_vdbg(&dev->dev, "write pingval %x\n", val);
		ntrdma_ping_write_peer(dev, val);

		ntrdma_tim_fire(&dev->ping_pong, NTRDMA_PING_PONG_MS);
	}
}

static void ntrdma_ping_poll(struct ntrdma_ntb_dev *dev)
{
	ntrdma_u32_t val;
	int msg_old, msg_new;

	if (dev->ping_run) {
		ntrdma_vdbg(&dev->dev, "run\n");

		val = ntrdma_ping_read(dev);
		ntrdma_vdbg(&dev->dev, "peer pingval %x\n", val);
		ntrdma_vdbg(&dev->dev, "prev pingval %x\n", dev->ping_val);

		if (val != dev->ping_val) {
			ntrdma_vdbg(&dev->dev, "ping hit\n");
			msg_old = ntrdma_pingval_state(dev->ping_val);
			msg_new = ntrdma_pingval_state(val);

			dev->ping_miss = 0;
			dev->ping_val = val;

			if (msg_old != msg_new) {
				ntrdma_dbg(&dev->dev, "peer updated msg %d\n", msg_new);
				ntrdma_port_recv_event(dev);
			}

		} else if (dev->ping_miss < NTRDMA_PING_MISS_MAX) {
			ntrdma_dbg(&dev->dev, "ping miss %d\n", dev->ping_miss + 1);
			if (++dev->ping_miss == NTRDMA_PING_MISS_MAX) {
				ntrdma_dbg(&dev->dev, "too many misses\n");
				ntrdma_port_recv_event(dev);
			}
		}

		ntrdma_tim_fire(&dev->ping_poll, NTRDMA_PING_POLL_MS);
	}
}

static ntrdma_u32_t ntrdma_ping_read(struct ntrdma_ntb_dev *dev)
{
	if (dev->ping_flags & NTRDMA_PING_FROM_MEM)
		return ntrdma_port_info_read_pingval(dev);
	if (dev->ping_flags & NTRDMA_PING_FROM_SPAD)
		return ntrdma_ntb_impl_spad_read(dev->ntb_impl, 0);
	return 0;
}

static void ntrdma_ping_write_peer(struct ntrdma_ntb_dev *dev, ntrdma_u32_t val)
{
	if (dev->ping_flags & NTRDMA_PING_TO_MEM)
		ntrdma_port_info_write_pingval(dev, val);
	if (dev->ping_flags & NTRDMA_PING_TO_SPAD)
		ntrdma_ntb_impl_peer_spad_write(dev->ntb_impl, 0, val);
}

static NTRDMA_DECL_TIM_CB(ntrdma_ping_pong_work, ptrhld)
{
	struct ntrdma_ntb_dev *dev = NTRDMA_CAST_TIM_CTX(ptrhld);

	ntrdma_spl_lock(&dev->ping_lock);
	{
		ntrdma_ping_pong(dev);
	}
	ntrdma_spl_unlock(&dev->ping_lock);
}

static NTRDMA_DECL_TIM_CB(ntrdma_ping_poll_work, ptrhld)
{
	struct ntrdma_ntb_dev *dev = NTRDMA_CAST_TIM_CTX(ptrhld);

	ntrdma_spl_lock(&dev->ping_lock);
	{
		ntrdma_ping_poll(dev);
	}
	ntrdma_spl_unlock(&dev->ping_lock);
}

