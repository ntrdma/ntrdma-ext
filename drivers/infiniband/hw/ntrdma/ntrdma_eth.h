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

#ifndef NTRDMA_ETH_H
#define NTRDMA_ETH_H

#include <linux/etherdevice.h>
#include <linux/ethtool.h>

#include "ntrdma.h"
#include "ntrdma_ring.h"

struct ntrdma_eth_qe {
	u64				addr;
	u64				len;
};

struct ntrdma_eth {
	struct napi_struct		napi;
	struct ntrdma_dev		*dev;

	void				*req;

	bool				enable;
	bool				ready; /* FIXME: dev enable common */
	bool				link;

	/* ethernet rx ring indices */

	u32				rx_cap;
	u32				rx_post;
	u32				rx_prod;
	u32				rx_cmpl;

	/* ethernet rx ring buffers */

	void				**rx_buf;
	struct ntrdma_eth_qe		*rx_wqe_buf;
	u64				rx_wqe_buf_dma_addr;
	size_t				rx_wqe_buf_size;
	struct ntrdma_eth_qe		*rx_cqe_buf;
	u64				rx_cqe_buf_addr;
	size_t				rx_cqe_buf_size;

	u32				*rx_cons_buf;
	u64				rx_cons_buf_addr;

	u64				peer_tx_wqe_buf_dma_addr;
	u64				peer_tx_prod_buf_dma_addr;

	/* ethernet tx ring indices */

	u32				tx_cap;
	u32				tx_cons;
	u32				tx_cmpl;

	/* ethernet tx ring buffers */

	struct ntrdma_eth_qe		*tx_wqe_buf;
	u64				tx_wqe_buf_addr;
	size_t				tx_wqe_buf_size;
	struct ntrdma_eth_qe		*tx_cqe_buf;
	u64				tx_cqe_buf_addr;
	size_t				tx_cqe_buf_size;

	u32				*tx_prod_buf;
	u64				tx_prod_buf_addr;

	u64				peer_rx_cqe_buf_dma_addr;
	u64				peer_rx_cons_buf_dma_addr;
	u32				peer_vbell_idx;

	/* one at a time each: poster, producer, consumer, completer */

	spinlock_t			rx_prod_lock;
	spinlock_t			rx_cmpl_lock;
	spinlock_t			tx_cons_lock;

	/* notify napi of rx tx ring availability */

	struct ntrdma_vbell		vbell;
	u32				vbell_idx;
};

#define ntrdma_napi_eth(__napi) \
	container_of(__napi, struct ntrdma_eth, napi)
#define ntrdma_net_eth(__net) \
	netdev_priv(__net)

#endif
