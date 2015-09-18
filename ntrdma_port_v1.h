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

#ifndef NTRDMA_PORT_V1_H
#define NTRDMA_PORT_V1_H

#include "ntrdma_port.h"
#include "ntrdma_ring.h"
#include "ntrdma_res.h"

#define NTRDMA_V1_VER			1
#define NTRDMA_V1_TAG			0xec7b5719

/* Protocol version one peer info structure. */
struct ntrdma_v1_info {

	/* ------------------------------ */
	/* initialization phase           */
	/* ------------------------------ */

	/* interrupt messages */
	ntrdma_u64_t			msi_dma;
	ntrdma_u32_t			msi_val;
	ntrdma_u32_t			msi_count; /* TODO: multiple vectors */

	/* virtual doorbell */
	ntrdma_u64_t			vbell_dma;
	ntrdma_u32_t			vbell_flags; /* TODO: bool/seq, hw bell */
	ntrdma_u32_t			vbell_count;

	/* resource commands */
	ntrdma_u32_t			cmd_send_cap;
	ntrdma_u32_t			cmd_send_idx;
	ntrdma_u64_t			cmd_send_rsp_buf_dma;
	ntrdma_u64_t			cmd_send_cons_dma;
	ntrdma_u32_t			cmd_send_vbell_idx;
	ntrdma_u32_t			cmd_recv_vbell_idx;

	/* ethernet frames */
	ntrdma_u32_t			eth_rx_cap;
	ntrdma_u32_t			eth_rx_idx;
	ntrdma_u64_t			eth_rx_cqe_buf_dma;
	ntrdma_u64_t			eth_rx_cons_buf_dma;
	ntrdma_u32_t			eth_rx_vbell_idx;

	/* initialization phase end tag */
	ntrdma_u32_t			v1_init_tag;

	/* ------------------------------ */
	/* configuration phase            */
	/* ------------------------------ */

	/* resource commands */
	ntrdma_u64_t			cmd_recv_buf_dma;
	ntrdma_u64_t			cmd_recv_prod_dma;

	/* ethernet frames */
	ntrdma_u64_t			eth_tx_wqe_buf_dma;
	ntrdma_u64_t			eth_tx_prod_buf_dma;
	ntrdma_u32_t			eth_tx_vbell_idx;

	/* configuration phase end tag */
	ntrdma_u32_t			v1_conf_tag;
};

void ntrdma_v1_reset(struct ntrdma_ntb_dev *dev);

void ntrdma_v1_prep(struct ntrdma_ntb_dev *dev,
		    struct ntrdma_v1_info __iomem *info);

void ntrdma_v1_init(struct ntrdma_ntb_dev *dev,
		    struct ntrdma_v1_info __iomem *info,
		    struct ntrdma_v1_info *peer_info);

void ntrdma_v1_conf(struct ntrdma_ntb_dev *dev,
		    struct ntrdma_v1_info *peer_info);

#endif
