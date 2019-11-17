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

struct ntrdma_eth {
	struct napi_struct		napi;
	struct ntrdma_dev		*dev;

	struct ntc_dma_chan		*dma_chan;

	bool				enable;
	bool				ready; /* FIXME: dev enable common */
	bool				link;

	/* ethernet rx ring indices */

	u32				rx_cap;
	u32				rx_post;
	u32				rx_prod;
	u32				rx_cmpl;

	/* ethernet rx ring buffers */

	struct ntc_export_buf		*rx_buf;
	struct ntc_local_buf		rx_wqe_buf;
	struct ntc_export_buf		rx_cqe_buf;

	struct ntc_export_buf		rx_cons_buf;

	struct ntc_remote_buf		peer_tx_wqe_buf;
	struct ntc_remote_buf		peer_tx_prod_buf;

	/* ethernet tx ring indices */

	u32				tx_cap;
	u32				tx_cons;
	u32				tx_cmpl;

	/* ethernet tx ring buffers */

	struct ntc_export_buf		tx_wqe_buf;
	struct ntc_local_buf		tx_cqe_buf;

	struct ntc_export_buf		tx_prod_buf;

	struct ntc_remote_buf		peer_rx_cqe_buf;
	struct ntc_remote_buf		peer_rx_cons_buf;
	u32				peer_vbell_idx;

	/* one at a time each: poster, producer, consumer, completer */

	spinlock_t			rx_prod_lock;
	spinlock_t			rx_cmpl_lock;
	spinlock_t			tx_cons_lock;

	/* notify napi of rx tx ring availability */

	struct ntrdma_vbell		vbell;
	bool	is_hello_done;
	bool	is_hello_prep;
};

inline u32 ntrdma_eth_tx_prod(struct ntrdma_eth *eth);
inline u32 ntrdma_eth_rx_cons(struct ntrdma_eth *eth);

#define ntrdma_napi_eth(__napi) \
	container_of(__napi, struct ntrdma_eth, napi)
#define ntrdma_net_eth(__net) \
	netdev_priv(__net)

static inline struct net_device *ntrdma_get_net(struct ntrdma_dev *dev)
{
	struct ntrdma_eth *eth = dev->eth;

	return eth->napi.dev;
}
#endif
