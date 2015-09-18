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

#include "ntrdma_map.h"
#include "ntrdma_ntb.h"
#include "ntrdma_port.h"
#include "ntrdma_ping.h"

#include "ntrdma_port_v1.h"

#include "ntrdma_ntb_impl.h"

#define NTRDMA_MIN_VER			NTRDMA_V1_VER
#define NTRDMA_MAX_VER			NTRDMA_V1_VER

struct ntrdma_peer_info {
	struct ntrdma_peer_info_hdr	hdr;
	union {
		struct ntrdma_v1_info	v1;
	};
};

static void ntrdma_port_reset(struct ntrdma_ntb_dev *dev);
static void ntrdma_port_spad(struct ntrdma_ntb_dev *dev);
static void ntrdma_port_prep(struct ntrdma_ntb_dev *dev);
static void ntrdma_port_init(struct ntrdma_ntb_dev *dev);
static void ntrdma_port_conf(struct ntrdma_ntb_dev *dev);

static void ntrdma_port_recv(struct ntrdma_ntb_dev *dev, int recv_event);

static void ntrdma_port_work(struct ntrdma_ntb_dev *dev);
static NTRDMA_DECL_DWI_CB(ntrdma_port_work_cb, ctx);

static void ntrdma_port_link_work(struct ntrdma_ntb_dev *dev);
static NTRDMA_DECL_DWI_CB(ntrdma_port_link_work_cb, ptrhld);

int ntrdma_ntb_port_init(struct ntrdma_ntb_dev *dev)
{
	int err;

	dev->port_version = NTRDMA_VERSION_NONE;
	dev->port_link = 0;
	dev->port_lstate = NTRDMA_RESET_ACK;
	dev->port_rstate = NTRDMA_QUIESCE;

	dev->peer_info_read_buf =
		ntrdma_malloc_coherent(ntrdma_port_device(&dev->dev),
				       sizeof(*dev->peer_info_read_buf),
				       &dev->peer_info_read_dma,
				       dev->dev.node);
	if (!dev->peer_info_read_buf) {
		err = -ENOMEM;
		goto err_buf;
	}
	ntrdma_dbg(&dev->dev, "port info read buf %llx dma %llx\n",
		   (unsigned long long)dev->peer_info_read_buf,
		   (unsigned long long)dev->peer_info_read_dma);

	dev->peer_info_write_dma = 0;
	dev->peer_info_write_io = NULL;

	ntrdma_mut_create(&dev->port_lock, "port_lock");
	ntrdma_dwi_create(&dev->port_work, "port_work",
			  ntrdma_port_work_cb, dev);
	ntrdma_dwi_create(&dev->port_link_work, "port_link_work",
			  ntrdma_port_link_work_cb, dev);

	return 0;

err_buf:
	return err;
}

void ntrdma_ntb_port_deinit(struct ntrdma_ntb_dev *dev)
{
	ntrdma_mut_destroy(&dev->port_lock);
	ntrdma_dwi_destroy(&dev->port_work);
	ntrdma_free_coherent(ntrdma_port_device(&dev->dev),
			     sizeof(*dev->peer_info_read_buf),
			     dev->peer_info_read_buf,
			     dev->peer_info_read_dma);
}

void ntrdma_port_recv_event(struct ntrdma_ntb_dev *dev)
{
	ntrdma_dwi_fire(&dev->port_work);
}

void ntrdma_port_link_event(struct ntrdma_ntb_dev *dev)
{
	ntrdma_dwi_fire(&dev->port_link_work);
}

void ntrdma_port_send(struct ntrdma_ntb_dev *dev, int send_event)
{
	int flags;

	if (send_event >= NTRDMA_READY_CONF)
		flags = NTRDMA_PING_FROM_MEM;
	else
		flags = NTRDMA_PING_FROM_SPAD;

	if (send_event >= NTRDMA_READY_INIT)
		flags |= NTRDMA_PING_TO_MEM;

	if (send_event <= NTRDMA_READY_CONF)
		flags |= NTRDMA_PING_TO_SPAD;

	dev->port_lstate = send_event;

	ntrdma_ping_send(dev, send_event, flags);
}

void ntrdma_port_quiesce(struct ntrdma_ntb_dev *dev)
{
	ntrdma_port_send(dev, NTRDMA_QUIESCE);
	ntrdma_dev_quiesce(&dev->dev);
	ntrdma_port_send(dev, NTRDMA_RESET_REQ);
}

ntrdma_u32_t ntrdma_port_info_read_pingval(struct ntrdma_ntb_dev *dev)
{
	return dev->peer_info_read_buf->hdr.pingval;
}

void ntrdma_port_info_write_pingval(struct ntrdma_ntb_dev *dev,
				    ntrdma_u32_t val)
{
	ntrdma_io_write32(&dev->peer_info_write_io->hdr.pingval, val);
}

static void ntrdma_port_reset(struct ntrdma_ntb_dev *dev)
{
	switch (dev->port_version) {
	case NTRDMA_V1_VER:
		ntrdma_v1_reset(dev);
		break;
	default:
		ntrdma_dbg(&dev->dev, "no version for reset\n");
		ntrdma_port_send(dev, NTRDMA_RESET_ACK);
	}
}

static void ntrdma_port_spad(struct ntrdma_ntb_dev *dev)
{
	ntrdma_u32_t val;

	val = ntrdma_version_code(NTRDMA_MIN_VER, NTRDMA_MAX_VER);
	ntrdma_ntb_impl_peer_spad_write(dev->ntb_impl, 1, val);
	ntrdma_dbg(&dev->dev, "wrote spad 1 %x (version code)\n", val);

	val = (ntrdma_u32_t)dev->peer_info_read_dma;
	ntrdma_ntb_impl_peer_spad_write(dev->ntb_impl, 2, val);
	ntrdma_dbg(&dev->dev, "wrote spad 2 %x (addr lower)\n", val);

	val = (ntrdma_u32_t)(dev->peer_info_read_dma >> 32);
	ntrdma_ntb_impl_peer_spad_write(dev->ntb_impl, 3, val);
	ntrdma_dbg(&dev->dev, "wrote spad 3 %x (addr upper)\n", val);

	/* Note: no outstanding operations, so send message immediately */
	ntrdma_port_send(dev, NTRDMA_READY_SPAD);
}

static void ntrdma_port_prep(struct ntrdma_ntb_dev *dev)
{
	struct ntrdma_peer_info __iomem *info;
	ntrdma_u64_t addr;
	ntrdma_u32_t val, ver;

	ver = ntrdma_version_code(NTRDMA_MIN_VER, NTRDMA_MAX_VER);

	val = ntrdma_ntb_impl_spad_read(dev->ntb_impl, 1);
	ntrdma_dbg(&dev->dev, "read spad 1 %x (version code)\n", val);

	dev->port_version = ntrdma_version_choose(val, ver);
	ntrdma_dbg(&dev->dev, "choose version %d\n",
		   dev->port_version);

	val = ntrdma_ntb_impl_spad_read(dev->ntb_impl, 2);
	ntrdma_dbg(&dev->dev, "read spad 2 %x (addr lower)\n", val);

	addr = (ntrdma_u64_t)val;

	val = ntrdma_ntb_impl_spad_read(dev->ntb_impl, 3);
	ntrdma_dbg(&dev->dev, "read spad 3 %x (addr upper)\n", val);

	addr |= ((ntrdma_u64_t)val) << 32;
	ntrdma_dbg(&dev->dev, "peer addr %llx\n", addr);

	dev->peer_info_write_dma = dev->dev.peer_dram_base + addr;
	ntrdma_dbg(&dev->dev, "peer info dma %llx\n",
		   dev->peer_info_write_dma);

	info = ntrdma_io_map(dev->peer_info_write_dma,
			     sizeof(*dev->peer_info_write_io));
	if (!info) {
		ntrdma_port_quiesce(dev);
		return;
	}

	dev->peer_info_write_io = info;

	switch (dev->port_version) {
	case NTRDMA_V1_VER:
		ntrdma_io_write32(&info->hdr.version_tag, NTRDMA_V1_TAG);
		ntrdma_v1_prep(dev, &info->v1);
		break;
	default:
		ntrdma_dbg(&dev->dev, "no version for prep\n");
		ntrdma_port_quiesce(dev);
	}
}

static void ntrdma_port_init(struct ntrdma_ntb_dev *dev)
{
	struct ntrdma_peer_info __iomem *info;
	struct ntrdma_peer_info *peer_info;

	info = dev->peer_info_write_io;
	peer_info = dev->peer_info_read_buf;

	switch (dev->port_version) {
	case NTRDMA_V1_VER:
		if (peer_info->hdr.version_tag != NTRDMA_V1_TAG) {
			ntrdma_port_quiesce(dev);
			break;
		}
		ntrdma_v1_init(dev, &info->v1, &peer_info->v1);
		break;
	default:
		ntrdma_dbg(&dev->dev, "no version for init\n");
		ntrdma_port_quiesce(dev);
	}
}

static void ntrdma_port_conf(struct ntrdma_ntb_dev *dev)
{
	struct ntrdma_peer_info *peer_info;

	peer_info = dev->peer_info_read_buf;

	switch (dev->port_version) {
	case NTRDMA_V1_VER:
		if (peer_info->hdr.version_tag != NTRDMA_V1_TAG) {
			ntrdma_port_quiesce(dev);
			break;
		}
		ntrdma_v1_conf(dev, &peer_info->v1);
		break;
	default:
		ntrdma_dbg(&dev->dev, "no version for conf\n");
		ntrdma_port_quiesce(dev);
	}
}

static void ntrdma_port_recv(struct ntrdma_ntb_dev *dev, int recv_event)
{
	if (dev->port_lstate > NTRDMA_RESET_ACK &&
	    dev->port_rstate > recv_event) {
		ntrdma_dbg(&dev->dev, "invalid recv_event for port state\n");
		ntrdma_port_quiesce(dev);
	}

	else switch (dev->port_lstate) {

	case NTRDMA_QUIESCE:
		ntrdma_vdbg(&dev->dev, "NTRDMA_QUIESCE\n");
		break;

	case NTRDMA_RESET_REQ:
		ntrdma_vdbg(&dev->dev, "NTRDMA_RESET_REQ\n");
		switch (recv_event) {
		case NTRDMA_RESET_REQ:
		case NTRDMA_RESET_ACK:
			ntrdma_port_reset(dev);
		}

		if (dev->port_lstate != NTRDMA_RESET_ACK)
			break;

	case NTRDMA_RESET_ACK:
		ntrdma_vdbg(&dev->dev, "NTRDMA_RESET_ACK\n");
		switch (recv_event) {
		case NTRDMA_QUIESCE:
		case NTRDMA_RESET_REQ:
			break;
		case NTRDMA_RESET_ACK:
		case NTRDMA_READY_SPAD:
			ntrdma_port_spad(dev);
			break;
		default:
			ntrdma_port_quiesce(dev);
		}

		if (dev->port_lstate != NTRDMA_READY_SPAD)
			break;

	case NTRDMA_READY_SPAD:
		ntrdma_vdbg(&dev->dev, "NTRDMA_READY_SPAD\n");
		switch (recv_event) {
		case NTRDMA_RESET_ACK:
			break;
		case NTRDMA_READY_SPAD:
		case NTRDMA_READY_INIT:
			ntrdma_port_prep(dev);
			break;
		default:
			ntrdma_port_quiesce(dev);
		}

		if (dev->port_lstate != NTRDMA_READY_INIT)
			break;

	case NTRDMA_READY_INIT:
		ntrdma_vdbg(&dev->dev, "NTRDMA_READY_INIT\n");
		switch (recv_event) {
		case NTRDMA_READY_SPAD:
			break;
		case NTRDMA_READY_INIT:
		case NTRDMA_READY_CONF:
			ntrdma_port_init(dev);
			break;
		default:
			ntrdma_port_quiesce(dev);
		}

		if (dev->port_lstate != NTRDMA_READY_CONF)
			break;

	case NTRDMA_READY_CONF:
		ntrdma_vdbg(&dev->dev, "NTRDMA_READY_SPAD\n");
		switch (recv_event) {
		case NTRDMA_READY_INIT:
			break;
		case NTRDMA_READY_CONF:
		case NTRDMA_READY_PORT:
			ntrdma_port_conf(dev);
			break;
		default:
			ntrdma_port_quiesce(dev);
		}

		if (dev->port_lstate != NTRDMA_READY_PORT)
			break;

	case NTRDMA_READY_PORT:
		ntrdma_vdbg(&dev->dev, "NTRDMA_READY_SPAD\n");
		switch (recv_event) {
		case NTRDMA_READY_CONF:
			break;
		case NTRDMA_READY_PORT:
			ntrdma_dev_enable(&dev->dev);
			break;
		default:
			ntrdma_port_quiesce(dev);
		}

		break;

	default:
		ntrdma_dbg(&dev->dev, "bad port lstate %d\n",
			   dev->port_lstate);
		ntrdma_port_quiesce(dev);
	}

	dev->port_rstate = recv_event;

	ntrdma_mut_unlock(&dev->port_lock);
}

static void ntrdma_port_work(struct ntrdma_ntb_dev *dev)
{
	int recv_event;

	ntrdma_mut_lock(&dev->port_lock);
	{
		recv_event = ntrdma_ping_recv(dev);

		ntrdma_port_recv(dev, recv_event);
	}
	ntrdma_mut_unlock(&dev->port_lock);
}

static NTRDMA_DECL_DWI_CB(ntrdma_port_work_cb, ctx)
{
	struct ntrdma_ntb_dev *dev = NTRDMA_CAST_DWI_CTX(ctx);

	ntrdma_port_work(dev);
}

static void ntrdma_port_link_work(struct ntrdma_ntb_dev *dev)
{
	int link_event;

	ntrdma_mut_lock(&dev->port_lock);

	link_event = !!ntrdma_ntb_impl_link_is_up(dev->ntb_impl);
	ntrdma_dbg(&dev->dev, "impl link event %d\n", link_event);
	ntrdma_dbg(&dev->dev, "port link state %d\n", dev->port_link);

	if (link_event != dev->port_link) {
		if (link_event) {
			ntrdma_dbg(&dev->dev,
				   "port transition to link up\n");

			dev->dev.peer_dram_base =
				ntrdma_ntb_impl_peer_dram_dma(dev->ntb_impl);
			ntrdma_dbg(&dev->dev, "peer dram dma %llx\n",
				   dev->dev.peer_dram_base);

			dev->peer_msi_base =
				ntrdma_ntb_impl_peer_msi_dma(dev->ntb_impl);
			ntrdma_dbg(&dev->dev, "peer msi dma %llx\n",
				   dev->peer_msi_base);

			ntrdma_port_send(dev, dev->port_lstate);
			ntrdma_ping_start(dev);
		} else {
			ntrdma_dbg(&dev->dev,
				   "port transition to link down\n");

			dev->dev.peer_dram_base = 0;
			dev->peer_msi_base = 0;

			ntrdma_ping_stop(dev);
			ntrdma_port_quiesce(dev);
		}
		dev->port_link = link_event;
	}

	ntrdma_mut_unlock(&dev->port_lock);
}

static NTRDMA_DECL_DWI_CB(ntrdma_port_link_work_cb, ptrhld)
{
	struct ntrdma_ntb_dev *dev = NTRDMA_CAST_DWI_CTX(ptrhld);

	ntrdma_port_link_work(dev);
}
