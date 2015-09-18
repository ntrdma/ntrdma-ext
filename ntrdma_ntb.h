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

#ifndef NTRDMA_NTB_H
#define NTRDMA_NTB_H

#include "ntrdma_dev.h"
#include "ntrdma_dma.h"

struct ntrdma_peer_info;

struct ntrdma_ntb_dev {
	/* NOTE: .dev.ibdev MUST be the first thing in ntrdma_ntb_dev!
	 *      (required by ib_alloc_device and ib_dealloc_device) */
	struct ntrdma_dev		dev;

	/* handles into the minimal ntb and dma api */
	void				*ntb_impl;
	void				*dma_impl;

	ntrdma_phys_addr_t		peer_msi_base;
	ntrdma_dma_addr_t		peer_msi_dma;
	ntrdma_u32_t			peer_msi_val;
	int				peer_msi_count;

	/* pingpong messages */
	int				ping_run;
	int				ping_flags;
	ntrdma_u16_t			ping_seq;
	ntrdma_u16_t			ping_msg;
	ntrdma_u32_t			ping_val;
	int				ping_miss;
	NTRDMA_DECL_TIM			(ping_pong);
	NTRDMA_DECL_TIM			(ping_poll);
	NTRDMA_DECL_SPL			(ping_lock);

	/* port state machine */

	int				port_version;
	int				port_link;
	int				port_lstate;
	int				port_rstate;
	NTRDMA_DECL_MUT			(port_lock);
	NTRDMA_DECL_DWI			(port_work);
	NTRDMA_DECL_DWI			(port_link_work);

	/* port version defined info */

	ntrdma_dma_addr_t		peer_info_read_dma;
	struct ntrdma_peer_info		*peer_info_read_buf;

	ntrdma_dma_addr_t		peer_info_write_dma;
	struct ntrdma_peer_info __iomem	*peer_info_write_io;

	/* dma requests */

	NTRDMA_DECL_LIST_HEAD		(dma_req_free);
	NTRDMA_DECL_LIST_HEAD		(dma_req_sent);
	NTRDMA_DECL_SPL			(dma_lock);

	int				dma_hwm;
	int				dma_lwm;
	int				dma_out;

	NTRDMA_DECL_LIST_HEAD		(throttle_list);
	NTRDMA_DECL_SPL			(throttle_lock);
};

#define dev_ndev(__dev) NTRDMA_CONTAINER_OF(__dev, struct ntrdma_ntb_dev, dev)

#endif
