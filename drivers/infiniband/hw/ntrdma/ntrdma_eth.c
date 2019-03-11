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

#include "ntrdma_dev.h"
#include "ntrdma_eth.h"
#include "ntrdma_hello.h"

#define MAX_WQES 4096
#define SKINFO_SIZE SKB_DATA_ALIGN(sizeof(struct skb_shared_info))

static const struct net_device_ops ntrdma_eth_net_ops;
static int ntrdma_eth_napi_poll(struct napi_struct *napi, int budget);
static void ntrdma_eth_vbell_cb(void *ctx);
static void ntrdma_eth_dma_cb(void *ctx);
static void ntrdma_eth_link_event(struct ntrdma_eth *eth);
static void ntrdma_eth_link_event(struct ntrdma_eth *eth);

int ntrdma_dev_eth_init(struct ntrdma_dev *dev,
			u32 vbell_idx,
			u32 rx_cap)
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

	eth->rx_cap = rx_cap;
	eth->rx_post = 0;
	eth->rx_prod = 0;
	eth->rx_cmpl = 0;

	eth->rx_buf = kmalloc_node(eth->rx_cap * sizeof(*eth->rx_buf),
				   GFP_KERNEL, dev->node);
	if (!eth->rx_buf) {
		rc = -ENOMEM;
		goto err_rx_buf;
	}

	eth->rx_wqe_buf_size = eth->rx_cap * sizeof(*eth->rx_wqe_buf);

	eth->rx_wqe_buf = kmalloc_node(eth->rx_wqe_buf_size,
				       GFP_KERNEL, dev->node);
	if (!eth->rx_wqe_buf) {
		rc = -ENOMEM;
		goto err_rx_wqe_buf;
	}

	eth->rx_wqe_buf_addr = ntc_buf_map(dev->ntc,
					   eth->rx_wqe_buf,
					   eth->rx_wqe_buf_size,
					   DMA_TO_DEVICE);
	if (!eth->rx_wqe_buf_addr) {
		rc = -EIO;
		goto err_rx_wqe_addr;
	}

	eth->rx_cqe_buf_size = eth->rx_cap * sizeof(*eth->rx_cqe_buf);

	eth->rx_cqe_buf = kmalloc_node(eth->rx_cqe_buf_size,
				       GFP_KERNEL, dev->node);
	if (!eth->rx_cqe_buf) {
		rc = -ENOMEM;
		goto err_rx_cqe_buf;
	}

	eth->rx_cqe_buf_addr = ntc_buf_map(dev->ntc,
					   eth->rx_cqe_buf,
					   eth->rx_cqe_buf_size,
					   DMA_FROM_DEVICE);
	if (!eth->rx_cqe_buf_addr) {
		rc = -EIO;
		goto err_rx_cqe_addr;
	}

	eth->rx_cons_buf = ntc_buf_alloc(dev->ntc,
					 sizeof(*eth->rx_cons_buf),
					 &eth->rx_cons_buf_addr,
					 dev->node);
	if (!eth->rx_cons_buf) {
		rc = -ENOMEM;
		goto err_rx_cons_buf;
	}

	*eth->rx_cons_buf = 0;

	eth->peer_tx_wqe_buf_addr = 0;
	eth->peer_tx_prod_buf_addr = 0;

	eth->tx_cap = 0;
	eth->tx_cons = 0;
	eth->tx_cmpl = 0;
	eth->tx_wqe_buf = NULL;
	eth->tx_wqe_buf_addr = 0;
	eth->tx_wqe_buf_size = 0;
	eth->tx_cqe_buf = NULL;
	eth->tx_cqe_buf_addr = 0;
	eth->tx_cqe_buf_size = 0;
	eth->tx_prod_buf = NULL;
	eth->tx_prod_buf_addr = 0;

	eth->peer_rx_cqe_buf_addr = 0;
	eth->peer_rx_cons_buf_addr = 0;
	eth->peer_vbell_idx = 0;

	spin_lock_init(&eth->rx_prod_lock);
	spin_lock_init(&eth->rx_cmpl_lock);
	spin_lock_init(&eth->tx_cons_lock);

	ntrdma_vbell_init(&eth->vbell, ntrdma_eth_vbell_cb, eth);
	eth->vbell_idx = vbell_idx;

	netif_napi_add(net, &eth->napi, ntrdma_eth_napi_poll,
		       NAPI_POLL_WEIGHT);

	rc = register_netdev(net);
	if (rc)
		goto err_register;

	return 0;

err_register:
	netif_napi_del(&eth->napi);
	ntc_buf_free(dev->ntc,
		     sizeof(*eth->rx_cons_buf),
		     eth->rx_cons_buf,
		     eth->rx_cons_buf_addr);
err_rx_cons_buf:
	ntc_buf_unmap(dev->ntc,
		      eth->rx_cqe_buf_addr,
		      eth->rx_cqe_buf_size,
		      DMA_FROM_DEVICE);
err_rx_cqe_addr:
	kfree(eth->rx_cqe_buf);
err_rx_cqe_buf:
	ntc_buf_unmap(dev->ntc,
		      eth->rx_wqe_buf_addr,
		      eth->rx_wqe_buf_size,
		      DMA_TO_DEVICE);
err_rx_wqe_addr:
	kfree(eth->rx_wqe_buf);
err_rx_wqe_buf:
	kfree(eth->rx_buf);
err_rx_buf:
	free_netdev(net);
err_net:
	return rc;
}

void ntrdma_dev_eth_deinit(struct ntrdma_dev *dev)
{
	struct ntrdma_eth *eth = dev->eth;
	struct net_device *net = eth->napi.dev;
	u32 start, pos, end, base;

	unregister_netdev(net);
	netif_napi_del(&eth->napi);

	ntc_buf_free(dev->ntc,
		     sizeof(*eth->rx_cons_buf),
		     eth->rx_cons_buf,
		     eth->rx_cons_buf_addr);
	ntc_buf_unmap(dev->ntc,
		      eth->rx_cqe_buf_addr,
		      eth->rx_cqe_buf_size,
		      DMA_FROM_DEVICE);
	kfree(eth->rx_cqe_buf);
	ntc_buf_unmap(dev->ntc,
		      eth->rx_wqe_buf_addr,
		      eth->rx_wqe_buf_size,
		      DMA_TO_DEVICE);
	kfree(eth->rx_wqe_buf);

	do {
		ntrdma_ring_consume(eth->rx_prod,
				    eth->rx_cmpl,
				    eth->rx_cap,
				    &start, &end, &base);

		for (pos = start; pos != end; ++pos)
			kfree(eth->rx_buf[pos]);

		eth->rx_cmpl = ntrdma_ring_update(end, base, eth->rx_cap);
	} while (start != end);

	kfree(eth->rx_buf);

	free_netdev(net);
}

int ntrdma_dev_eth_hello_info(struct ntrdma_dev *dev,
			       struct ntrdma_eth_hello_info *info)
{
	struct ntrdma_eth *eth = dev->eth;

	info->rx_cap = eth->rx_cap;
	info->rx_idx = eth->rx_cmpl;
	if (!eth->rx_cqe_buf_addr || !eth->rx_cons_buf_addr)
		return -EINVAL;

	info->rx_buf_addr = eth->rx_cqe_buf_addr;
	info->rx_idx_addr = eth->rx_cons_buf_addr;
	info->vbell_idx = eth->vbell_idx;

	return 0;
}

int ntrdma_dev_eth_hello_prep(struct ntrdma_dev *dev,
			      struct ntrdma_eth_hello_info *peer_info,
			      struct ntrdma_eth_hello_prep *prep)
{
	struct ntrdma_eth *eth = dev->eth;
	int rc;

	eth->peer_rx_cqe_buf_addr =
		ntc_peer_addr(dev->ntc, peer_info->rx_buf_addr);
	eth->peer_rx_cons_buf_addr =
		ntc_peer_addr(dev->ntc, peer_info->rx_idx_addr);

	if (peer_info->vbell_idx > MAX_VBELL_COUNT) {
		ntrdma_err(dev, "peer info suspected as garbage vbell_idx %u\n",
				peer_info->vbell_idx);
		rc = -ENOMEM;
		goto err_tx_wqe_buf;
	}

	eth->peer_vbell_idx = peer_info->vbell_idx;

	/* added protection with a big enough size, since rx_cap and
	 * rx_idx can hold ANY value, which would fail the kmalloc
	 */
	if (peer_info->rx_cap > MAX_WQES || peer_info->rx_idx > MAX_WQES) {
		ntrdma_err(dev, "peer info is suspected as garbage cap %u idx %u\n",
				peer_info->rx_cap, peer_info->rx_idx);
		rc = -ENOMEM;
		goto err_tx_wqe_buf;
	}
	eth->tx_cap = peer_info->rx_cap;
	eth->tx_cons = peer_info->rx_idx;
	eth->tx_cmpl = peer_info->rx_idx;

	eth->tx_wqe_buf_size = eth->tx_cap * sizeof(*eth->tx_wqe_buf);

	eth->tx_wqe_buf = kmalloc_node(eth->tx_wqe_buf_size,
				       GFP_KERNEL, dev->node);
	if (!eth->tx_wqe_buf) {
		rc = -ENOMEM;
		goto err_tx_wqe_buf;
	}

	eth->tx_wqe_buf_addr = ntc_buf_map(dev->ntc,
					   eth->tx_wqe_buf,
					   eth->tx_wqe_buf_size,
					   DMA_FROM_DEVICE);
	if (!eth->tx_wqe_buf_addr) {
		ntrdma_err(dev, "dma mapping failed\n");
		rc = -EIO;
		goto err_tx_wqe_addr;
	}

	eth->tx_cqe_buf_size = eth->tx_cap * sizeof(*eth->tx_cqe_buf);

	eth->tx_cqe_buf = kmalloc_node(eth->tx_cqe_buf_size,
				       GFP_KERNEL, dev->node);
	if (!eth->tx_cqe_buf) {
		rc = -ENOMEM;
		goto err_tx_cqe_buf;
	}

	eth->tx_cqe_buf_addr = ntc_buf_map(dev->ntc,
					   eth->tx_cqe_buf,
					   eth->tx_cqe_buf_size,
					   DMA_TO_DEVICE);
	if (!eth->tx_cqe_buf_addr) {
		ntrdma_err(dev, "dma mapping failed\n");
		rc = -EIO;
		goto err_tx_cqe_addr;
	}

	eth->tx_prod_buf = ntc_buf_alloc(dev->ntc,
					 sizeof(*eth->tx_prod_buf),
					 &eth->tx_prod_buf_addr,
					 dev->node);
	if (!eth->tx_prod_buf) {
		rc = -ENOMEM;
		goto err_tx_prod_buf;
	}

	*eth->tx_prod_buf = peer_info->rx_idx;

	prep->tx_buf_addr = eth->tx_wqe_buf_addr;
	prep->tx_idx_addr = eth->tx_prod_buf_addr;

	return 0;

	ntc_buf_free(dev->ntc,
		     sizeof(*eth->tx_prod_buf),
		     eth->tx_prod_buf,
		     eth->tx_prod_buf_addr);
err_tx_prod_buf:
	ntc_buf_unmap(dev->ntc,
		      eth->tx_cqe_buf_addr,
		      eth->tx_cqe_buf_size,
		      DMA_TO_DEVICE);
err_tx_cqe_addr:
	kfree(eth->tx_cqe_buf);
err_tx_cqe_buf:
	ntc_buf_unmap(dev->ntc,
		      eth->tx_wqe_buf_addr,
		      eth->tx_wqe_buf_size,
		      DMA_FROM_DEVICE);
err_tx_wqe_addr:
	kfree(eth->tx_wqe_buf);
err_tx_wqe_buf:
	return rc;
}

void ntrdma_dev_eth_hello_done(struct ntrdma_dev *dev,
			       struct ntrdma_eth_hello_prep *peer_prep)
{
	struct ntrdma_eth *eth = dev->eth;

	eth->peer_tx_wqe_buf_addr =
		ntc_peer_addr(dev->ntc, peer_prep->tx_buf_addr);
	eth->peer_tx_prod_buf_addr =
		ntc_peer_addr(dev->ntc, peer_prep->tx_idx_addr);
}

void ntrdma_dev_eth_enable(struct ntrdma_dev *dev)
{
	struct ntrdma_eth *eth = dev->eth;

	eth->ready = true;
	ntrdma_eth_link_event(eth);
}

void ntrdma_dev_eth_disable(struct ntrdma_dev *dev)
{
	struct ntrdma_eth *eth = dev->eth;

	eth->ready = false;
	ntrdma_eth_link_event(eth);
}

void ntrdma_dev_eth_reset(struct ntrdma_dev *dev)
{
	struct ntrdma_eth *eth = dev->eth;

	ntc_buf_free(dev->ntc,
		     sizeof(*eth->tx_prod_buf),
		     eth->tx_prod_buf,
		     eth->tx_prod_buf_addr);
	ntc_buf_unmap(dev->ntc,
		      eth->tx_cqe_buf_addr,
		      eth->tx_cqe_buf_size,
		      DMA_TO_DEVICE);
	kfree(eth->tx_cqe_buf);
	ntc_buf_unmap(dev->ntc,
		      eth->tx_wqe_buf_addr,
		      eth->tx_wqe_buf_size,
		      DMA_FROM_DEVICE);
	kfree(eth->tx_wqe_buf);

	*eth->rx_cons_buf = eth->rx_cmpl;
	eth->peer_rx_cqe_buf_addr = 0;
	eth->peer_rx_cons_buf_addr = 0;
	eth->peer_vbell_idx = 0;
}

static void ntrdma_eth_rx_fill(struct ntrdma_eth *eth)
{
	struct ntrdma_dev *dev = eth->dev;
	void *req, *buf;
	u64 src, dst;
	size_t off, len;
	u32 start, pos, end, base;

	spin_lock_bh(&eth->rx_prod_lock);
	{
		if (!eth->ready) {
			spin_unlock_bh(&eth->rx_prod_lock);
			return;
		}

		req = ntc_req_create(dev->ntc);
		if (WARN_ON(!req)) {
			spin_unlock_bh(&eth->rx_prod_lock);
			return;
		}

		ntrdma_ring_produce(eth->rx_prod,
				    eth->rx_cmpl,
				    eth->rx_cap,
				    &start, &end, &base);
		if (start == end) {
			spin_unlock_bh(&eth->rx_prod_lock);
			return;
		}

more:
		len = SKB_DATA_ALIGN(NET_SKB_PAD + NET_IP_ALIGN +
				     eth->napi.dev->hard_header_len +
				     eth->napi.dev->mtu);

		for (pos = start; pos != end; ++pos) {
			buf = kmalloc_node(len + SKINFO_SIZE,
					   GFP_ATOMIC, eth->dev->node);
			if (!buf)
				break;

			dst = ntc_buf_map(dev->ntc,
					  buf, len,
					  DMA_FROM_DEVICE);
			if (!dst) {
				kfree(buf);
				break;
			}

			eth->rx_buf[pos] = buf;
			eth->rx_wqe_buf[pos].addr = dst;
			eth->rx_wqe_buf[pos].len = len;
		}

		eth->rx_prod = ntrdma_ring_update(pos, base, eth->rx_cap);

		ntc_buf_sync_dev(dev->ntc,
				 eth->rx_wqe_buf_addr,
				 eth->rx_wqe_buf_size,
				 DMA_TO_DEVICE);

		off = start * sizeof(*eth->rx_wqe_buf);
		len = (pos - start) * sizeof(*eth->rx_wqe_buf);
		src = eth->rx_wqe_buf_addr + off;
		dst = eth->peer_tx_wqe_buf_addr + off;

		ntc_req_memcpy(dev->ntc, req,
			       dst, src, len,
			       true, NULL, NULL);

		ntrdma_ring_produce(eth->rx_prod,
				    eth->rx_cmpl,
				    eth->rx_cap,
				    &start, &end, &base);
		if (start != end)
			goto more;

		ntc_req_imm32(dev->ntc, req,
			      eth->peer_tx_prod_buf_addr,
			      eth->rx_prod,
			      true, NULL, NULL);

		ntrdma_dev_vbell_peer(dev, req, eth->peer_vbell_idx);
		ntc_req_signal(dev->ntc, req, NULL, NULL, NTB_DEFAULT_VEC(dev->ntc));
		ntc_req_submit(dev->ntc, req);
	}
	spin_unlock_bh(&eth->rx_prod_lock);
}

static int ntrdma_eth_napi_poll(struct napi_struct *napi, int budget)
{
	struct ntrdma_eth *eth = ntrdma_napi_eth(napi);
	struct ntrdma_dev *dev = eth->dev;
	struct sk_buff *skb;
	void *buf;
	u64 addr, rx_addr;
	size_t len, rx_len;
	u32 start, pos, end, base;
	int count;

	if (!eth->link) {
		napi_complete(&eth->napi);
		return 0;
	}

	ntrdma_dev_vbell_clear(dev, &eth->vbell, eth->vbell_idx);

	if (eth->tx_cons != *eth->tx_prod_buf) {
		netif_wake_queue(eth->napi.dev);
	}

	ntc_buf_sync_cpu(dev->ntc,
			 eth->rx_cqe_buf_addr,
			 eth->rx_cqe_buf_size,
			 DMA_FROM_DEVICE);

	ntrdma_ring_consume(*eth->rx_cons_buf,
			    eth->rx_cmpl,
			    eth->rx_cap,
			    &start, &end, &base);

	end = min_t(u32, end, start + budget);

	for (pos = start; pos < end; ++pos) {
		buf = eth->rx_buf[pos];

		addr = eth->rx_wqe_buf[pos].addr;
		len = eth->rx_wqe_buf[pos].len;

		ntc_buf_unmap(dev->ntc,
			      addr, len,
			      DMA_FROM_DEVICE);

		rx_addr = eth->rx_cqe_buf[pos].addr;
		rx_len = eth->rx_cqe_buf[pos].len;

		if (!rx_len || WARN_ON(rx_addr < addr) ||
		    WARN_ON(rx_addr + rx_len > addr + len)) {
			kfree(buf);
			eth->napi.dev->stats.rx_errors++;
			eth->napi.dev->stats.rx_length_errors++;
			continue;
		}

		skb = build_skb(buf, 0);
		if (!skb) {
			kfree(buf);
			eth->napi.dev->stats.rx_errors++;
			eth->napi.dev->stats.rx_dropped++;
		}

		skb_reserve(skb, rx_addr - addr);
		skb_put(skb, rx_len);

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
	void *req;
	u64 dst, src, tx_addr;
	size_t off, len, tx_off, tx_len;
	u32 pos, end, base;

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

	tx_addr = eth->tx_wqe_buf[pos].addr;
	tx_len = eth->tx_wqe_buf[pos].len;

	off = skb_headroom(skb);
	len = skb_headlen(skb);

	tx_off = off & (SMP_CACHE_BYTES - 1);

	if (eth->req) {
		req = eth->req;
	} else {
		req = ntc_req_create(dev->ntc);
		if (!req)
			return NETDEV_TX_BUSY;
		eth->req = req;
	}

	if (len + tx_off > tx_len) {
		eth->napi.dev->stats.tx_errors++;
		eth->napi.dev->stats.tx_dropped++;

		eth->tx_cqe_buf[pos].addr = tx_addr;
		eth->tx_cqe_buf[pos].len = 0;

		kfree_skb(skb);
	} else {
		dst = ntc_peer_addr(dev->ntc, tx_addr);
		src = ntc_buf_map(dev->ntc, skb->head,
				  skb_end_offset(skb),
				  DMA_TO_DEVICE);
		if (!src) {
			kfree_skb(skb);
			eth->napi.dev->stats.tx_errors++;
			eth->napi.dev->stats.tx_dropped++;
			goto skip;
		}

		/*
		 * save the mapped dma addr in the skb control buffer,
		 * so that it can be unmapped later in the dma callback.
		 */
		*(u64 *)skb->cb = src;

		ntc_req_memcpy(dev->ntc, req,
			       dst, src + off - tx_off, len + tx_off,
			       false, ntrdma_eth_dma_cb, skb);

		eth->napi.dev->stats.tx_packets++;
		eth->napi.dev->stats.tx_bytes += len;

		eth->tx_cqe_buf[pos].addr = tx_addr + tx_off;
		eth->tx_cqe_buf[pos].len = len;

		netdev_sent_queue(eth->napi.dev, len);
	}

	eth->tx_cons = ntrdma_ring_update(pos + 1, base, eth->tx_cap);

	if (!skb->xmit_more) {
		while (eth->tx_cmpl != eth->tx_cons) {
			ntrdma_ring_consume(eth->tx_cons,
					    eth->tx_cmpl,
					    eth->tx_cap,
					    &pos, &end, &base);

			off = pos * sizeof(*eth->tx_cqe_buf);
			len = (end - pos) * sizeof(*eth->tx_cqe_buf);
			dst = eth->peer_rx_cqe_buf_addr + off;
			src = eth->tx_cqe_buf_addr + off;

			ntc_req_memcpy(dev->ntc, req,
				       dst, src, len,
				       true, NULL, NULL);

			eth->tx_cmpl = ntrdma_ring_update(end, base,
							  eth->tx_cap);
		}

		ntc_req_imm32(dev->ntc, req,
			      eth->peer_rx_cons_buf_addr,
			      eth->tx_cmpl,
			      true, NULL, NULL);

		ntrdma_dev_vbell_peer(dev, req, eth->peer_vbell_idx);
		ntc_req_signal(dev->ntc, req, NULL, NULL, NTB_DEFAULT_VEC(dev->ntc));
		ntc_req_submit(dev->ntc, req);

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
	u64 src;

	/* retrieve the mapped addr from the skb control buffer */
	src = *(u64 *)skb->cb;

	ntc_buf_unmap(dev->ntc, src,
		      skb_end_offset(skb),
		      DMA_TO_DEVICE);

	consume_skb(skb);

	netdev_completed_queue(eth->napi.dev, 1, skb_headlen(skb));
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
