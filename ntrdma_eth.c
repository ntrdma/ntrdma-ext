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

#include <linux/etherdevice.h>
#include <linux/ethtool.h>

#include "ntrdma_map.h"
#include "ntrdma_dev.h"
#include "ntrdma_ring.h"

#define NTRDMA_ETH_CAP			0x400
#define NTRDMA_ETH_VBELL_IDX		4

struct ntrdma_eth_qe {
	ntrdma_u64_t			dma;
	ntrdma_u64_t			len;
};

struct ntrdma_eth {
	struct napi_struct		napi;
	struct ntrdma_dev		*dev;

	struct ntrdma_req		*req;

	ntrdma_bool_t			enable;
	ntrdma_bool_t			ready;
	ntrdma_bool_t			link;

	/* ethernet rx ring indices */

	ntrdma_u32_t			rx_cap;
	ntrdma_u32_t			rx_post;
	ntrdma_u32_t			rx_prod;
	ntrdma_u32_t			rx_cmpl;

	/* ethernet rx ring buffers */

	struct sk_buff			**rx_skb;
	struct ntrdma_eth_qe		*rx_wqe_buf;
	ntrdma_dma_addr_t		rx_wqe_buf_dma;
	ntrdma_size_t			rx_wqe_buf_size;
	ntrdma_u64_t			*rx_cqe_buf;
	ntrdma_dma_addr_t		rx_cqe_buf_dma;
	ntrdma_size_t			rx_cqe_buf_size;

	ntrdma_u32_t			*rx_cons_buf;
	ntrdma_dma_addr_t		rx_cons_buf_dma;

	ntrdma_dma_addr_t		peer_tx_wqe_buf_dma;
	ntrdma_dma_addr_t		peer_tx_prod_buf_dma;

	/* ethernet tx ring indices */

	ntrdma_u32_t			tx_cap;
	ntrdma_u32_t			tx_cons;
	ntrdma_u32_t			tx_cmpl;

	/* ethernet tx ring buffers */

	struct ntrdma_eth_qe		*tx_wqe_buf;
	ntrdma_dma_addr_t		tx_wqe_buf_dma;
	ntrdma_size_t			tx_wqe_buf_size;
	ntrdma_u64_t			*tx_cqe_buf;
	ntrdma_dma_addr_t		tx_cqe_buf_dma;
	ntrdma_size_t			tx_cqe_buf_size;

	ntrdma_u32_t			*tx_prod_buf;
	ntrdma_dma_addr_t		tx_prod_buf_dma;

	ntrdma_dma_addr_t		peer_rx_cqe_buf_dma;
	ntrdma_dma_addr_t		peer_rx_cons_buf_dma;
	ntrdma_u32_t			peer_vbell_idx;

	/* one at a time each: poster, producer, consumer, completer */

	NTRDMA_DECL_SPL			(rx_prod_lock);
	NTRDMA_DECL_SPL			(rx_cmpl_lock);
	NTRDMA_DECL_SPL			(tx_cons_lock);

	/* notify napi of rx tx ring availability */

	struct ntrdma_vbell		vbell;
	ntrdma_u32_t			vbell_idx;
};

#define ntrdma_napi_eth(__napi) \
	NTRDMA_CONTAINER_OF(__napi, struct ntrdma_eth, napi)
#define ntrdma_net_eth(__net) \
	netdev_priv(__net)

static const struct net_device_ops ntrdma_eth_net_ops;
static int ntrdma_eth_napi_poll(struct napi_struct *napi, int budget);
static void ntrdma_eth_vbell_cb(void *ctx);
static void ntrdma_eth_dma_cb(void *ctx);
static void ntrdma_eth_link_event(struct ntrdma_eth *eth);
static void ntrdma_eth_link_event(struct ntrdma_eth *eth);

int ntrdma_dev_eth_init(struct ntrdma_dev *dev)
{
	struct net_device *net;
	struct ntrdma_eth *eth;
	int rc;

	net = alloc_etherdev(sizeof(*eth));
	if (!net) {
		rc = -ENOMEM;
		goto err_net;
	}

	net->netdev_ops = &ntrdma_eth_net_ops;
	net->features = NETIF_F_HIGHDMA;
	random_ether_addr(net->perm_addr);
	memcpy(net->dev_addr, net->perm_addr, net->addr_len);

	eth = ntrdma_net_eth(net);
	dev->eth = eth;

	eth->dev = dev;
	eth->req = NULL;
	eth->enable = false;
	eth->ready = false;
	eth->link = false;

	eth->rx_cap = NTRDMA_ETH_CAP;
	eth->rx_post = 0;
	eth->rx_prod = 0;
	eth->rx_cmpl = 0;

	eth->rx_skb = ntrdma_malloc(eth->rx_cap * sizeof(*eth->rx_skb),
				    dev->node);
	if (!eth->rx_skb) {
		rc = -ENOMEM;
		goto err_rx_skb;
	}

	eth->rx_wqe_buf_size = eth->rx_cap * sizeof(*eth->rx_wqe_buf);

	eth->rx_wqe_buf = ntrdma_malloc(eth->rx_wqe_buf_size, dev->node);
	if (!eth->rx_wqe_buf) {
		rc = -ENOMEM;
		goto err_rx_wqe_buf;
	}

	eth->rx_wqe_buf_dma = ntrdma_dma_map(ntrdma_port_device(dev),
					     eth->rx_wqe_buf,
					     eth->rx_wqe_buf_size,
					     NTRDMA_DMA_TO_DEVICE);
	if (!eth->rx_wqe_buf_dma) {
		rc = -EIO;
		goto err_rx_wqe_dma;
	}

	eth->rx_cqe_buf_size = eth->rx_cap * sizeof(*eth->rx_cqe_buf);

	eth->rx_cqe_buf = ntrdma_malloc(eth->rx_cqe_buf_size, dev->node);
	if (!eth->rx_cqe_buf) {
		rc = -ENOMEM;
		goto err_rx_cqe_buf;
	}

	eth->rx_cqe_buf_dma = ntrdma_dma_map(ntrdma_port_device(dev),
					     eth->rx_cqe_buf,
					     eth->rx_cqe_buf_size,
					     NTRDMA_DMA_FROM_DEVICE);
	if (!eth->rx_cqe_buf_dma) {
		rc = -EIO;
		goto err_rx_cqe_dma;
	}

	eth->rx_cons_buf = ntrdma_malloc_coherent(ntrdma_port_device(dev),
						  sizeof(*eth->rx_cons_buf),
						  &eth->rx_cons_buf_dma,
						  dev->node);
	if (!eth->rx_cons_buf) {
		rc = -ENOMEM;
		goto err_rx_cons_buf;
	}

	*eth->rx_cons_buf = 0;

	eth->peer_tx_wqe_buf_dma = 0;
	eth->peer_tx_prod_buf_dma = 0;

	eth->tx_cap = 0;
	eth->tx_cons = 0;
	eth->tx_cmpl = 0;
	eth->tx_wqe_buf = NULL;
	eth->tx_wqe_buf_dma = 0;
	eth->tx_wqe_buf_size = 0;
	eth->tx_cqe_buf = NULL;
	eth->tx_cqe_buf_dma = 0;
	eth->tx_cqe_buf_size = 0;
	eth->tx_prod_buf = NULL;
	eth->tx_prod_buf_dma = 0;

	eth->peer_rx_cqe_buf_dma = 0;
	eth->peer_rx_cons_buf_dma = 0;
	eth->peer_vbell_idx = 0;

	ntrdma_spl_create(&eth->rx_prod_lock, "eth_rx_prod_lock");
	ntrdma_spl_create(&eth->rx_cmpl_lock, "eth_rx_cmpl_lock");
	ntrdma_spl_create(&eth->tx_cons_lock, "eth_tx_cons_lock");

	ntrdma_vbell_init(&eth->vbell, ntrdma_eth_vbell_cb, eth);
	eth->vbell_idx = NTRDMA_ETH_VBELL_IDX;

	netif_napi_add(net, &eth->napi, ntrdma_eth_napi_poll,
		       NAPI_POLL_WEIGHT);

	rc = register_netdev(net);
	if (rc)
		goto err_register;

	return 0;

err_register:
	netif_napi_del(&eth->napi);
	ntrdma_free_coherent(ntrdma_port_device(dev),
			     sizeof(*eth->rx_cons_buf),
			     eth->rx_cons_buf,
			     eth->rx_cons_buf_dma);
err_rx_cons_buf:
	ntrdma_dma_unmap(ntrdma_port_device(dev),
			 eth->rx_cqe_buf_dma,
			 eth->rx_cqe_buf_size,
			 NTRDMA_DMA_FROM_DEVICE);
err_rx_cqe_dma:
	ntrdma_free(eth->rx_cqe_buf);
err_rx_cqe_buf:
	ntrdma_dma_unmap(ntrdma_port_device(dev),
			 eth->rx_wqe_buf_dma,
			 eth->rx_wqe_buf_size,
			 NTRDMA_DMA_TO_DEVICE);
err_rx_wqe_dma:
	ntrdma_free(eth->rx_wqe_buf);
err_rx_wqe_buf:
	ntrdma_free(eth->rx_skb);
err_rx_skb:
	free_netdev(net);
err_net:
	return rc;
}

void ntrdma_dev_eth_deinit(struct ntrdma_dev *dev)
{
	struct ntrdma_eth *eth = dev->eth;
	struct net_device *net = eth->napi.dev;

	unregister_netdev(net);
	netif_napi_del(&eth->napi);

	ntrdma_free_coherent(ntrdma_port_device(dev),
			     sizeof(*eth->rx_cons_buf),
			     eth->rx_cons_buf,
			     eth->rx_cons_buf_dma);
	ntrdma_dma_unmap(ntrdma_port_device(dev),
			 eth->rx_cqe_buf_dma,
			 eth->rx_cqe_buf_size,
			 NTRDMA_DMA_FROM_DEVICE);
	ntrdma_free(eth->rx_cqe_buf);
	ntrdma_dma_unmap(ntrdma_port_device(dev),
			 eth->rx_wqe_buf_dma,
			 eth->rx_wqe_buf_size,
			 NTRDMA_DMA_TO_DEVICE);
	ntrdma_free(eth->rx_wqe_buf);
	ntrdma_free(eth->rx_skb);

	free_netdev(net);
}

void ntrdma_dev_eth_conf_attr(struct ntrdma_dev *dev,
			      struct ntrdma_eth_conf_attr *attr)
{
	struct ntrdma_eth *eth = dev->eth;

	attr->cap = eth->rx_cap;
	attr->idx = eth->rx_cmpl;
	attr->buf_dma = eth->rx_cqe_buf_dma;
	attr->idx_dma = eth->rx_cons_buf_dma;
	attr->vbell_idx = eth->vbell_idx;
}

int ntrdma_dev_eth_conf(struct ntrdma_dev *dev,
			struct ntrdma_eth_conf_attr *attr)
{
	struct ntrdma_eth *eth = dev->eth;
	int rc;

	eth->peer_rx_cqe_buf_dma = attr->buf_dma;
	eth->peer_rx_cons_buf_dma = attr->idx_dma;
	eth->peer_vbell_idx = attr->vbell_idx;

	eth->tx_cap = attr->cap;
	eth->tx_cons = attr->idx;
	eth->tx_cmpl = attr->idx;

	eth->tx_wqe_buf_size = eth->tx_cap * sizeof(*eth->tx_wqe_buf);

	eth->tx_wqe_buf = ntrdma_malloc(eth->tx_wqe_buf_size, dev->node);
	if (!eth->tx_wqe_buf) {
		rc = -ENOMEM;
		goto err_tx_wqe_buf;
	}

	eth->tx_wqe_buf_dma = ntrdma_dma_map(ntrdma_port_device(dev),
					     eth->tx_wqe_buf,
					     eth->tx_wqe_buf_size,
					     NTRDMA_DMA_FROM_DEVICE);
	if (!eth->tx_wqe_buf_dma) {
		rc = -EIO;
		goto err_tx_wqe_dma;
	}

	eth->tx_cqe_buf_size = eth->tx_cap * sizeof(*eth->tx_cqe_buf);

	eth->tx_cqe_buf = ntrdma_malloc(eth->tx_cqe_buf_size, dev->node);
	if (!eth->tx_cqe_buf) {
		rc = -ENOMEM;
		goto err_tx_cqe_buf;
	}

	eth->tx_cqe_buf_dma = ntrdma_dma_map(ntrdma_port_device(dev),
					     eth->tx_cqe_buf,
					     eth->tx_cqe_buf_size,
					     NTRDMA_DMA_TO_DEVICE);
	if (!eth->tx_cqe_buf_dma) {
		rc = -EIO;
		goto err_tx_cqe_dma;
	}

	eth->tx_prod_buf = ntrdma_malloc_coherent(ntrdma_port_device(dev),
						  sizeof(*eth->tx_prod_buf),
						  &eth->tx_prod_buf_dma,
						  dev->node);
	if (!eth->tx_prod_buf) {
		rc = -ENOMEM;
		goto err_tx_prod_buf;
	}

	*eth->tx_prod_buf = 0;


	return 0;

	ntrdma_free_coherent(ntrdma_port_device(dev),
			     sizeof(*eth->tx_prod_buf),
			     eth->tx_prod_buf,
			     eth->tx_prod_buf_dma);
err_tx_prod_buf:
	ntrdma_dma_unmap(ntrdma_port_device(dev),
			 eth->tx_cqe_buf_dma,
			 eth->tx_cqe_buf_size,
			 NTRDMA_DMA_TO_DEVICE);
err_tx_cqe_dma:
	ntrdma_free(eth->tx_cqe_buf);
err_tx_cqe_buf:
	ntrdma_dma_unmap(ntrdma_port_device(dev),
			 eth->tx_wqe_buf_dma,
			 eth->tx_wqe_buf_size,
			 NTRDMA_DMA_FROM_DEVICE);
err_tx_wqe_dma:
	ntrdma_free(eth->tx_wqe_buf);
err_tx_wqe_buf:
	return rc;
}

void ntrdma_dev_eth_deconf(struct ntrdma_dev *dev)
{
	struct ntrdma_eth *eth = dev->eth;

	ntrdma_free_coherent(ntrdma_port_device(dev),
			     sizeof(*eth->tx_prod_buf),
			     eth->tx_prod_buf,
			     eth->tx_prod_buf_dma);
	ntrdma_dma_unmap(ntrdma_port_device(dev),
			 eth->tx_cqe_buf_dma,
			 eth->tx_cqe_buf_size,
			 NTRDMA_DMA_TO_DEVICE);
	ntrdma_free(eth->tx_cqe_buf);
	ntrdma_dma_unmap(ntrdma_port_device(dev),
			 eth->tx_wqe_buf_dma,
			 eth->tx_wqe_buf_size,
			 NTRDMA_DMA_FROM_DEVICE);
	ntrdma_free(eth->tx_wqe_buf);

	eth->peer_rx_cqe_buf_dma = 0;
	eth->peer_rx_cons_buf_dma = 0;
	eth->peer_vbell_idx = 0;
}

void ntrdma_dev_eth_enable_attr(struct ntrdma_dev *dev,
				struct ntrdma_eth_enable_attr *attr)
{
	struct ntrdma_eth *eth = dev->eth;

	attr->buf_dma = eth->tx_wqe_buf_dma;
	attr->idx_dma = eth->tx_prod_buf_dma;
}

void ntrdma_dev_eth_enable(struct ntrdma_dev *dev,
			   struct ntrdma_eth_enable_attr *attr)
{
	struct ntrdma_eth *eth = dev->eth;

	eth->peer_tx_wqe_buf_dma = attr->buf_dma;
	eth->peer_tx_prod_buf_dma = attr->idx_dma;

	eth->ready = true;
	ntrdma_eth_link_event(eth);
}

void ntrdma_dev_eth_disable(struct ntrdma_dev *dev)
{
	struct ntrdma_eth *eth = dev->eth;

	eth->ready = false;
	ntrdma_eth_link_event(eth);

	/* FIXME: sync with anyting that might be running first? */

	eth->peer_tx_wqe_buf_dma = 0;
	eth->peer_tx_prod_buf_dma = 0;
}

static void ntrdma_eth_rx_fill(struct ntrdma_eth *eth)
{
	struct ntrdma_dev *dev = eth->dev;
	struct ntrdma_req *req;
	struct sk_buff *skb;
	ntrdma_dma_addr_t src, dst;
	ntrdma_size_t off, len;
	ntrdma_u32_t start, pos, end, base;

	ntrdma_spl_lock(&eth->rx_prod_lock);
	{
		if (!eth->ready) {
			ntrdma_spl_unlock(&eth->rx_prod_lock);
			return;
		}

		req = ntrdma_req_alloc(dev, 100);
		if (WARN_ON(!req)) {
			ntrdma_spl_unlock(&eth->rx_prod_lock);
			return;
		}

		ntrdma_ring_produce(eth->rx_prod,
				    eth->rx_cmpl,
				    eth->rx_cap,
				    &start, &end, &base);
		if (start == end) {
			ntrdma_spl_unlock(&eth->rx_prod_lock);
			return;
		}

more:
		len = eth->napi.dev->mtu + eth->napi.dev->hard_header_len;

		for (pos = start; pos != end; ++pos) {
			skb = napi_alloc_skb(&eth->napi, len);
			if (!skb)
				break;

			dst = ntrdma_dma_map(ntrdma_port_device(dev),
					     skb->data, len,
					     NTRDMA_DMA_FROM_DEVICE);
			if (!dst) {
				kfree_skb(skb);
				break;
			}

			eth->rx_skb[pos] = skb;
			eth->rx_wqe_buf[pos].dma = dst;
			eth->rx_wqe_buf[pos].len = len;
		}

		eth->rx_prod = ntrdma_ring_update(pos, base, eth->rx_cap);

		ntrdma_dma_sync_for_device(ntrdma_port_device(dev),
					   eth->rx_wqe_buf_dma,
					   eth->rx_wqe_buf_size,
					   NTRDMA_DMA_TO_DEVICE);

		off = start * sizeof(*eth->rx_wqe_buf);
		len = (end - start) * sizeof(*eth->rx_wqe_buf);
		src = eth->rx_wqe_buf_dma + off;
		dst = eth->peer_tx_wqe_buf_dma + off;

		ntrdma_req_memcpy(req, dst, src, len);

		ntrdma_ring_produce(eth->rx_prod,
				    eth->rx_cmpl,
				    eth->rx_cap,
				    &start, &end, &base);
		if (start != end)
			goto more;

		ntrdma_req_imm32(req, eth->peer_tx_prod_buf_dma,
				 eth->rx_prod);

		ntrdma_dev_vbell_peer(dev, req, eth->peer_vbell_idx);
		ntrdma_req_signal(req);
		ntrdma_req_submit(req);
	}
	ntrdma_spl_unlock(&eth->rx_prod_lock);
}

static int ntrdma_eth_napi_poll(struct napi_struct *napi, int budget)
{
	struct ntrdma_eth *eth = ntrdma_napi_eth(napi);
	struct ntrdma_dev *dev = eth->dev;
	struct sk_buff *skb;
	ntrdma_dma_addr_t rx_dma;
	ntrdma_size_t len, rx_len;
	ntrdma_u32_t start, pos, end, base;
	int count;

	if (!eth->link) {
		napi_complete(&eth->napi);
		return 0;
	}

	ntrdma_dev_vbell_clear(dev, &eth->vbell, eth->vbell_idx);

	if (eth->tx_cons != *eth->tx_prod_buf) {
		netif_wake_queue(eth->napi.dev);
	}

	ntrdma_dma_sync_for_cpu(ntrdma_port_device(dev),
				eth->rx_cqe_buf_dma,
				eth->rx_cqe_buf_size,
				NTRDMA_DMA_FROM_DEVICE);

	ntrdma_ring_consume(*eth->rx_cons_buf,
			    eth->rx_cmpl,
			    eth->rx_cap,
			    &start, &end, &base);

	end = ntrdma_min_t(ntrdma_u32_t, end, start + budget);

	for (pos = start; pos < end; ++pos) {
		skb = eth->rx_skb[pos];

		rx_dma = eth->rx_wqe_buf[pos].dma;
		rx_len = eth->rx_wqe_buf[pos].len;

		ntrdma_dma_unmap(ntrdma_port_device(dev),
				 rx_dma, rx_len,
				 NTRDMA_DMA_FROM_DEVICE);

		len = eth->rx_cqe_buf[pos];
		if (!len || WARN_ON(len > rx_len)) {
			kfree_skb(skb);
			eth->napi.dev->stats.rx_errors++;
			eth->napi.dev->stats.rx_length_errors++;
			continue;
		}

		skb_put(skb, len);
		skb->protocol = eth_type_trans(skb, eth->napi.dev);
		skb->ip_summed = CHECKSUM_NONE;

		if (netif_receive_skb(skb) == NET_RX_SUCCESS) {
			eth->napi.dev->stats.rx_packets++;
			eth->napi.dev->stats.rx_bytes += len;
		} else {
			eth->napi.dev->stats.rx_errors++;
			eth->napi.dev->stats.rx_dropped++;
		}
	}

	eth->rx_cmpl = ntrdma_ring_update(pos, base, eth->rx_cap);

	count = pos - start;

	if (count < budget) {
		napi_complete(&eth->napi);
		if (ntrdma_dev_vbell_add(dev, &eth->vbell,
					 eth->vbell_idx) == -EAGAIN)
			napi_reschedule(&eth->napi);
	}

	ntrdma_eth_rx_fill(eth);

	return count;
}

static netdev_tx_t ntrdma_eth_start_xmit(struct sk_buff *skb,
					 struct net_device *net)
{
	struct ntrdma_eth *eth = ntrdma_net_eth(net);
	struct ntrdma_dev *dev = eth->dev;
	struct ntrdma_req *req;
	ntrdma_dma_addr_t dst, src, tx_dma;
	ntrdma_size_t off, len, tx_len;
	ntrdma_u32_t pos, end, base;

	if (!eth->link) {
		kfree_skb(skb);
		eth->napi.dev->stats.tx_errors++;
		eth->napi.dev->stats.tx_carrier_errors++;
		return NETDEV_TX_OK;
	}

	ntrdma_ring_consume(*eth->tx_prod_buf,
			    eth->tx_cons,
			    eth->tx_cap,
			    &pos, &end, &base);

	if (pos == end) {
		kfree_skb(skb);
		eth->napi.dev->stats.tx_errors++;
		eth->napi.dev->stats.tx_fifo_errors++;
		goto skip;
	}

	tx_dma = eth->tx_wqe_buf[pos].dma;
	tx_len = eth->tx_wqe_buf[pos].len;

	len = skb_headlen(skb);

	if (eth->req) {
		req = eth->req;
	} else {
		req = ntrdma_req_alloc(dev, 100);
		if (!req)
			return NETDEV_TX_BUSY;
		eth->req = req;
	}

	if (len > tx_len) {
		kfree_skb(skb);
		eth->napi.dev->stats.tx_errors++;
		eth->napi.dev->stats.tx_dropped++;
		len = 0;
	} else {
		dst = dev->peer_dram_base + tx_dma;
		src = ntrdma_dma_map(ntrdma_port_device(dev),
				     skb->data, len,
				     NTRDMA_DMA_TO_DEVICE);
		if (!src) {
			kfree_skb(skb);
			eth->napi.dev->stats.tx_errors++;
			eth->napi.dev->stats.tx_dropped++;
			goto skip;
		}

		eth->napi.dev->stats.tx_packets++;
		eth->napi.dev->stats.tx_bytes += len;

		/*
		 * save the mapped dma addr in the skb control buffer,
		 * so that it can be unmapped later in the dma callback.
		 */
		*(ntrdma_dma_addr_t *)skb->cb = src;

		ntrdma_req_memcpy(req, dst, src, len);
		ntrdma_req_callback(req, ntrdma_eth_dma_cb, skb);
	}

	eth->tx_cqe_buf[pos] = len;

	eth->tx_cons = ntrdma_ring_update(pos + 1, base, eth->tx_cap);

	netdev_sent_queue(eth->napi.dev, len);

	if (!skb->xmit_more) {
		while (eth->tx_cmpl != eth->tx_cons) {
			ntrdma_ring_consume(eth->tx_cons,
					    eth->tx_cmpl,
					    eth->tx_cap,
					    &pos, &end, &base);

			off = pos * sizeof(*eth->tx_cqe_buf);
			len = (end - pos) * sizeof(*eth->tx_cqe_buf);
			dst = eth->peer_rx_cqe_buf_dma + off;
			src = eth->tx_cqe_buf_dma + off;

			ntrdma_req_memcpy(req, dst, src, len);

			eth->tx_cmpl = ntrdma_ring_update(end, base,
							  eth->tx_cap);
		}

		ntrdma_req_imm32(req, eth->peer_rx_cons_buf_dma,
				 eth->tx_cmpl);

		ntrdma_dev_vbell_peer(dev, req, eth->peer_vbell_idx);
		ntrdma_req_signal(req);
		ntrdma_req_submit(req);

		eth->req = NULL;
	}

skip:
	if (eth->tx_cons == *eth->tx_prod_buf) {
		netif_stop_queue(eth->napi.dev);
		napi_schedule(&eth->napi);
	}

	return NETDEV_TX_OK;
}

static void ntrdma_eth_vbell_cb(void *ctx)
{
	struct ntrdma_eth *eth = ctx;

	napi_schedule(&eth->napi);
}

static void ntrdma_eth_dma_cb(void *ctx)
{
	struct sk_buff *skb = ctx;
	struct ntrdma_eth *eth = ntrdma_net_eth(skb->dev);
	struct ntrdma_dev *dev = eth->dev;

	ntrdma_dma_addr_t src;
	ntrdma_size_t len;

	/* retrieve the mapped dma addr from the skb control buffer */
	src = *(ntrdma_dma_addr_t *)skb->cb;
	len = skb_headlen(skb);

	ntrdma_dma_unmap(ntrdma_port_device(dev),
			 src, len, NTRDMA_DMA_TO_DEVICE);

	consume_skb(skb);

	netdev_completed_queue(eth->napi.dev, 1, len);
}

static void ntrdma_eth_link_event(struct ntrdma_eth *eth)
{
	bool link = eth->enable && eth->ready;

	if (link == eth->link)
		return;

	if (link) {
		netif_stop_queue(eth->napi.dev);
		ntrdma_eth_rx_fill(eth);
		netif_carrier_on(eth->napi.dev);
		napi_enable(&eth->napi);
		napi_schedule(&eth->napi);
	} else {
		napi_disable(&eth->napi);
		netif_carrier_off(eth->napi.dev);
	}

	eth->link = link;
}

static int ntrdma_eth_open(struct net_device *net)
{
	struct ntrdma_eth *eth = ntrdma_net_eth(net);

	eth->enable = true;
	ntrdma_eth_link_event(eth);

	return 0;
}


static int ntrdma_eth_stop(struct net_device *net)
{
	struct ntrdma_eth *eth = ntrdma_net_eth(net);

	eth->enable = false;
	ntrdma_eth_link_event(eth);

	return 0;
}

static int ntrdma_eth_change_mtu(struct net_device *net, int mtu)
{
	/* like eth_change_mtu, but no upper limit */
	if (mtu < 68)
		return -EINVAL;

	net->mtu = mtu;

	return 0;
}

static const struct net_device_ops ntrdma_eth_net_ops = {
	.ndo_open = ntrdma_eth_open,
	.ndo_stop = ntrdma_eth_stop,
	.ndo_start_xmit = ntrdma_eth_start_xmit,
	.ndo_change_mtu = ntrdma_eth_change_mtu,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
};
