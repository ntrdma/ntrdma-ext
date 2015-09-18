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
#include "ntrdma_cmd.h"
#include "ntrdma_res.h"
#include "ntrdma_ntb.h"
#include "ntrdma_ping.h"
#include "ntrdma_vbell.h"
#include "ntrdma_port_v1.h"

#include "ntrdma_ntb_impl.h"

void ntrdma_v1_reset(struct ntrdma_ntb_dev *dev)
{
	ntrdma_dev_rres_reset(&dev->dev);
	ntrdma_port_send(dev, NTRDMA_RESET_ACK);
}

/* TODO: call this one init */
void ntrdma_v1_prep(struct ntrdma_ntb_dev *dev,
		    struct ntrdma_v1_info __iomem *info)
{
	ntrdma_dma_addr_t msi_dma;
	ntrdma_u32_t msi_val;

	ntrdma_ntb_impl_msi_dma(dev->ntb_impl, &msi_dma, &msi_val);

	/* interrupt messages */
	ntrdma_io_write64(&info->msi_dma, msi_dma);
	ntrdma_io_write32(&info->msi_val, msi_val);
	ntrdma_io_write32(&info->msi_count, 1);

	/* virtual doorbell */
	ntrdma_io_write64(&info->vbell_dma, dev->dev.vbell_buf_dma);
	ntrdma_io_write32(&info->vbell_flags, 0);
	ntrdma_io_write32(&info->vbell_count, dev->dev.vbell_count);

	/* resource commands */
	ntrdma_io_write32(&info->cmd_send_cap,
			  dev->dev.cmd_send_cap);
	ntrdma_io_write32(&info->cmd_send_idx,
			  *dev->dev.cmd_send_cons_buf);
	ntrdma_io_write64(&info->cmd_send_rsp_buf_dma,
			  dev->dev.cmd_send_rsp_buf_dma);
	ntrdma_io_write64(&info->cmd_send_cons_dma,
			  dev->dev.cmd_send_rsp_buf_dma +
			  dev->dev.cmd_send_cap * sizeof(union ntrdma_rsp));
	ntrdma_io_write32(&info->cmd_send_vbell_idx,
			  dev->dev.cmd_send_vbell_idx);
	ntrdma_io_write32(&info->cmd_recv_vbell_idx,
			  dev->dev.cmd_recv_vbell_idx);

#ifdef CONFIG_NTRDMA_ETH
	/* ethernet frames */
	{
		struct ntrdma_eth_conf_attr attr;

		ntrdma_dev_eth_conf_attr(&dev->dev, &attr);

		ntrdma_io_write32(&info->eth_rx_cap, attr.cap);
		ntrdma_io_write32(&info->eth_rx_idx, attr.idx);
		ntrdma_io_write64(&info->eth_rx_cqe_buf_dma, attr.buf_dma);
		ntrdma_io_write64(&info->eth_rx_cons_buf_dma, attr.idx_dma);
		ntrdma_io_write32(&info->eth_rx_vbell_idx, attr.vbell_idx);
	}
#else
	ntrdma_io_write32(&info->eth_rx_cap, 0);
	ntrdma_io_write32(&info->eth_rx_idx, 0);
	ntrdma_io_write64(&info->eth_rx_cqe_buf_dma, 0);
	ntrdma_io_write64(&info->eth_rx_cons_buf_dma, 0);
	ntrdma_io_write32(&info->eth_rx_vbell_idx, 0);
#endif

	/* phase end tag */
	ntrdma_io_write32(&info->v1_init_tag, NTRDMA_V1_TAG);

	ntrdma_port_send(dev, NTRDMA_READY_INIT);
}

/* TODO: call this one conf */
void ntrdma_v1_init(struct ntrdma_ntb_dev *dev,
		    struct ntrdma_v1_info __iomem *info,
		    struct ntrdma_v1_info *peer_info)
{
	ntrdma_dma_addr_t msi_base, dram_base;
	int rc;

	if (peer_info->v1_init_tag != NTRDMA_V1_TAG) {
		ntrdma_port_quiesce(dev);
		return;
	}

	msi_base = dev->peer_msi_base;
	dram_base = dev->dev.peer_dram_base;

	/* interrupt messages */
	dev->peer_msi_dma = msi_base + peer_info->msi_dma;
	dev->peer_msi_val = peer_info->msi_val;
	dev->peer_msi_count = peer_info->msi_count;

	/* virtual doorbell */
	/* TODO: vbell conf/deconf ? */
	rc = ntrdma_dev_vbell_enable(&dev->dev,
				     dram_base + peer_info->vbell_dma,
				     peer_info->vbell_count);
	if (rc) {
		ntrdma_port_quiesce(dev);
		return;
	}

	/* resource commands */
	rc = ntrdma_dev_cmd_conf(&dev->dev,
				 dram_base + peer_info->cmd_send_rsp_buf_dma,
				 dram_base + peer_info->cmd_send_cons_dma,
				 peer_info->cmd_send_vbell_idx,
				 peer_info->cmd_recv_vbell_idx,
				 peer_info->cmd_send_cap,
				 peer_info->cmd_send_idx);
	if (rc) {
		ntrdma_port_quiesce(dev);
		return;
	}

	ntrdma_io_write64(&info->cmd_recv_buf_dma,
			  dev->dev.cmd_recv_buf_dma);
	ntrdma_io_write64(&info->cmd_recv_prod_dma,
			  dev->dev.cmd_recv_buf_dma +
			  dev->dev.cmd_recv_cap * sizeof(union ntrdma_cmd));

#ifdef CONFIG_NTRDMA_ETH
	/* ethernet frames */
	{
		struct ntrdma_eth_conf_attr attr;

		attr.idx = peer_info->eth_rx_idx;
		attr.cap = peer_info->eth_rx_cap;
		attr.buf_dma = dram_base + peer_info->eth_rx_cqe_buf_dma;
		attr.idx_dma = dram_base + peer_info->eth_rx_cons_buf_dma;
		attr.vbell_idx = peer_info->eth_rx_vbell_idx;

		rc = ntrdma_dev_eth_conf(&dev->dev, &attr);
		if (rc) {
			ntrdma_port_quiesce(dev);
			return;
		}
	}

	{
		struct ntrdma_eth_enable_attr attr;

		ntrdma_dev_eth_enable_attr(&dev->dev, &attr);

		ntrdma_io_write64(&info->eth_tx_wqe_buf_dma, attr.buf_dma);
		ntrdma_io_write64(&info->eth_tx_prod_buf_dma, attr.idx_dma);
		ntrdma_io_write32(&info->eth_tx_vbell_idx, attr.vbell_idx);
	}
#else
	ntrdma_io_write64(&info->eth_recv_buf_dma, 0);
	ntrdma_io_write64(&info->eth_recv_prod_dma, 0);
#endif

	/* phase end tag */
	ntrdma_io_write32(&info->v1_conf_tag, NTRDMA_V1_TAG);

	ntrdma_port_send(dev, NTRDMA_READY_CONF);
}

/* TODO: call this one ready */
void ntrdma_v1_conf(struct ntrdma_ntb_dev *dev,
		    struct ntrdma_v1_info *peer_info)
{
	ntrdma_dma_addr_t msi_base, dram_base;

	/* ------------------------------ */
	/* configuration phase action     */
	/* ------------------------------ */

	if (peer_info->v1_conf_tag != NTRDMA_V1_TAG) {
		ntrdma_port_quiesce(dev);
		return;
	}

	msi_base = dev->peer_msi_base;
	dram_base = dev->dev.peer_dram_base;

	/* resource commands */
	ntrdma_dev_cmd_enable(&dev->dev,
			      dram_base + peer_info->cmd_recv_buf_dma,
			      dram_base + peer_info->cmd_recv_prod_dma);

#ifdef CONFIG_NTRDMA_ETH
	/* ethernet frames */
	{
		struct ntrdma_eth_enable_attr attr;

		attr.buf_dma = dram_base + peer_info->eth_tx_wqe_buf_dma;
		attr.idx_dma = dram_base + peer_info->eth_tx_prod_buf_dma;
		attr.vbell_idx = peer_info->eth_tx_vbell_idx;

		ntrdma_dev_eth_enable(&dev->dev, &attr);
	}
#endif

	ntrdma_port_send(dev, NTRDMA_READY_PORT);
}
