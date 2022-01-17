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

#ifdef NTRDMA_FULL_ETH
static struct kmem_cache *skb_cb_slab;
#endif

static const struct net_device_ops ntrdma_eth_net_ops;
static int ntrdma_eth_napi_poll(struct napi_struct *napi, int budget);
static void ntrdma_eth_link_event(struct ntrdma_eth *eth);
#ifdef NTRDMA_FULL_ETH
static void ntrdma_eth_dma_cb(void *ctx, const struct dmaengine_result *result);
static inline const u32 *ntrdma_eth_tx_prod_buf(struct ntrdma_eth *eth)
{
	return ntc_export_buf_const_deref(&eth->tx_prod_buf, 0, sizeof(u32));
}

inline u32 ntrdma_eth_tx_prod(struct ntrdma_eth *eth)
{
	const u32 *tx_prod_buf = ntrdma_eth_tx_prod_buf(eth);

	if (!tx_prod_buf)
		return 0;

	return READ_ONCE(*tx_prod_buf);
}

static inline const u32 *ntrdma_eth_rx_cons_buf(struct ntrdma_eth *eth)
{
	return ntc_export_buf_const_deref(&eth->rx_cons_buf, 0, sizeof(u32));
}

inline u32 ntrdma_eth_rx_cons(struct ntrdma_eth *eth)
{
	const u32 *rx_cons_buf = ntrdma_eth_rx_cons_buf(eth);

	if (!rx_cons_buf)
		return 0;

	return READ_ONCE(*rx_cons_buf);
}

static inline void ntrdma_dev_eth_rx_drain(struct ntrdma_dev *dev)
{
	struct ntrdma_eth *eth = dev->eth;
	u32 start, pos, end, base;

	do {
		ntrdma_ring_consume(eth->rx_prod,
				eth->rx_cmpl,
				eth->rx_cap,
				&start, &end, &base);

		for (pos = start; pos != end; ++pos)
			ntc_export_buf_free(&eth->rx_buf[pos]);

		eth->rx_cmpl = ntrdma_ring_update(end, base, eth->rx_cap);
	} while (start != end);
}
#endif
static inline int ntrdma_dev_eth_init_deinit(struct ntrdma_dev *dev,
			u32 vbell_idx,
			u32 rx_cap,
			int is_deinit)
{
	struct net_device *net;
	struct ntrdma_eth *eth;
	int rc;
#ifdef NTRDMA_FULL_ETH
	u32 rx_cons = 0;
#endif

	if (is_deinit)
		goto deinit;

	net = alloc_netdev_mqs(sizeof(*eth), "ntrdma%d", NET_NAME_UNKNOWN,
			ether_setup, 1, 1);

	if (!net) {
		ntrdma_err(dev, "alloc etherdec failed");
		rc = -ENOMEM;
		goto err_net;
	}

	net->netdev_ops = &ntrdma_eth_net_ops;
	net->features = NETIF_F_HIGHDMA;
	net->priv_flags |= IFF_LIVE_ADDR_CHANGE;
	net->type = ARPHRD_PPP;
#ifndef NTRDMA_FULL_ETH
	net->flags |= IFF_NOARP;
#endif
	random_ether_addr(net->perm_addr);
	memcpy(net->dev_addr, net->perm_addr, net->addr_len);

	eth = ntrdma_net_eth(net);
	dev->eth = eth;

	eth->dev = dev;
	ntc_init_dma_chan(&eth->dma_chan, dev->ntc, NTC_ETH_DMA_CHAN);
	eth->enable = false;
	eth->ready = false;
	eth->link = false;
#ifdef NTRDMA_FULL_ETH
	eth->rx_cap = rx_cap;
	eth->rx_post = 0;
	eth->rx_prod = 0;
	eth->rx_cmpl = 0;

	eth->rx_buf = kmalloc_node(eth->rx_cap * sizeof(*eth->rx_buf),
				   GFP_KERNEL, dev->node);
	if (!eth->rx_buf) {
		ntrdma_err(dev, "rx buffer alloc failed");
		rc = -ENOMEM;
		goto err_rx_buf;
	}

	rc = ntc_local_buf_zalloc(&eth->rx_wqe_buf, dev->ntc,
				eth->rx_cap *
				sizeof(struct ntc_remote_buf_desc),
				GFP_KERNEL);
	if (rc < 0) {
		ntrdma_err(dev, "rx wqe buffer alloc failed");
		goto err_rx_wqe_buf;
	}

	rc = ntc_export_buf_zalloc(&eth->rx_cqe_buf, dev->ntc,
				eth->rx_cap *
				sizeof(struct ntc_remote_buf_desc),
				GFP_KERNEL);
	if (rc < 0) {
		ntrdma_err(dev, "rx cqe buffer alloc failed");
		goto err_rx_cqe_buf;
	}

	rc = ntc_export_buf_zalloc_init(&eth->rx_cons_buf, dev->ntc,
					sizeof(u32),
					GFP_KERNEL, &rx_cons, sizeof(u32), 0);
	if (rc < 0) {
		ntrdma_err(dev, "rx cons buffer alloc failed");
		goto err_rx_cons_buf;
	}

	ntc_remote_buf_clear(&eth->peer_tx_wqe_buf);
	ntc_remote_buf_clear(&eth->peer_tx_prod_buf);

	eth->tx_cap = 0;
	eth->tx_cons = 0;
	eth->tx_cmpl = 0;
	ntc_export_buf_clear(&eth->tx_wqe_buf);
	ntc_local_buf_clear(&eth->tx_cqe_buf);
	ntc_export_buf_clear(&eth->tx_prod_buf);

	ntc_remote_buf_clear(&eth->peer_rx_cqe_buf);
	ntc_remote_buf_clear(&eth->peer_rx_cons_buf);
	eth->peer_vbell_idx = 0;
	eth->is_hello_done = false;
	eth->is_hello_prep = false;

	spin_lock_init(&eth->rx_prod_lock);
	spin_lock_init(&eth->rx_cmpl_lock);
	spin_lock_init(&eth->tx_cons_lock);

	ntrdma_napi_vbell_init(dev, &eth->vbell, vbell_idx, &eth->napi);
#endif
	netif_napi_add(net, &eth->napi, ntrdma_eth_napi_poll, NAPI_POLL_WEIGHT);

	rc = register_netdev(net);
	if (rc) {
		ntrdma_err(dev, "err_register rc %d\n", rc);
		goto err_register;
	}

	netif_carrier_off(net);
	netif_dormant_on(net);
	netif_tx_disable(net);

	return 0;
deinit:
	eth = dev->eth;
	net = eth->napi.dev;
	WARN(eth->is_hello_done, "eth deinit without hello undone");
	WARN(eth->is_hello_prep, "eth deinit without hello unprep");
	unregister_netdev(net);
err_register:
#ifdef NTRDMA_FULL_ETH
	ntrdma_napi_vbell_kill(&eth->vbell);
	ntc_export_buf_free(&eth->rx_cons_buf);
err_rx_cons_buf:
	ntc_export_buf_free(&eth->rx_cqe_buf);
err_rx_cqe_buf:
	ntc_local_buf_free(&eth->rx_wqe_buf, dev->ntc);
err_rx_wqe_buf:
	ntrdma_dev_eth_rx_drain(dev);
	kfree(eth->rx_buf);
err_rx_buf:
#endif
	free_netdev(net);
err_net:
	return rc;
}

int ntrdma_dev_eth_init(struct ntrdma_dev *dev,
			u32 vbell_idx,
			u32 rx_cap)
{
	if (unlikely(vbell_idx >= NTRDMA_DEV_VBELL_COUNT)) {
		ntrdma_err(dev, "invalid vbell_idx. idx %d >= %d",
			vbell_idx, NTRDMA_DEV_VBELL_COUNT);
		return -EINVAL;
	}

	return ntrdma_dev_eth_init_deinit(dev, vbell_idx, rx_cap, false);
}

void ntrdma_dev_eth_deinit(struct ntrdma_dev *dev)
{
	ntrdma_dev_eth_init_deinit(dev, 0, 0, true);
}

#ifdef NTRDMA_FULL_ETH
int ntrdma_dev_eth_hello_info(struct ntrdma_dev *dev,
			struct ntrdma_eth_hello_info __iomem *info)
{
	struct ntrdma_eth *eth = dev->eth;
	struct ntc_remote_buf_desc rx_cqe_buf_desc;
	struct ntc_remote_buf_desc rx_cons_buf_desc;

	iowrite32(eth->rx_cap, &info->rx_cap);
	iowrite32(eth->rx_cmpl, &info->rx_idx);
	if (!ntc_export_buf_valid(&eth->rx_cqe_buf) ||
		!ntc_export_buf_valid(&eth->rx_cons_buf)) {
		ntrdma_err(dev, "either cqe or cons buffers are not ready");
		return -EINVAL;
	}

	ntc_export_buf_make_desc(&rx_cqe_buf_desc, &eth->rx_cqe_buf);
	memcpy_toio(&info->rx_cqe_buf_desc, &rx_cqe_buf_desc,
		sizeof(rx_cqe_buf_desc));

	ntc_export_buf_make_desc(&rx_cons_buf_desc, &eth->rx_cons_buf);
	memcpy_toio(&info->rx_cons_buf_desc, &rx_cons_buf_desc,
		sizeof(rx_cons_buf_desc));

	iowrite32(eth->vbell.idx, &info->vbell_idx);

	return 0;
}

static inline
int ntrdma_dev_eth_hello_prep_unperp(struct ntrdma_dev *dev,
				const struct ntrdma_eth_hello_info *peer_info,
				struct ntrdma_eth_hello_prep __iomem *prep,
				int is_unperp)
{
	struct ntrdma_eth *eth = dev->eth;
	int rc;
	u32 tx_prod;

	if (is_unperp)
		goto unprep;

	if (peer_info->vbell_idx > NTRDMA_DEV_VBELL_COUNT) {
		ntrdma_err(dev, "peer info suspected as garbage vbell_idx %u\n",
				peer_info->vbell_idx);
		rc = -ENOMEM;
		goto err_peer_rx_cqe_buf;
	}

	/* added protection with a big enough size, since rx_cap and
	 * rx_idx can hold ANY value, which would fail the kmalloc
	 */
	if (peer_info->rx_cap > MAX_WQES || peer_info->rx_idx > MAX_WQES) {
		ntrdma_err(dev, "peer info is suspected as garbage cap %u idx %u\n",
				peer_info->rx_cap, peer_info->rx_idx);
		rc = -ENOMEM;
		goto err_peer_rx_cqe_buf;
	}


	rc = ntc_remote_buf_map(&eth->peer_rx_cqe_buf, dev->ntc,
				&peer_info->rx_cqe_buf_desc);
	if (rc < 0) {
		ntrdma_err(dev, "peer rx cqe buffer map failed");
		goto err_peer_rx_cqe_buf;
	}

	rc = ntc_remote_buf_map(&eth->peer_rx_cons_buf, dev->ntc,
				&peer_info->rx_cons_buf_desc);
	if (rc < 0) {
		ntrdma_err(dev, "peer rx cons buffer map failed");
		goto err_peer_rx_cons_buf;
	}

	eth->peer_vbell_idx = peer_info->vbell_idx;


	eth->tx_cap = peer_info->rx_cap;
	eth->tx_cons = peer_info->rx_idx;
	eth->tx_cmpl = peer_info->rx_idx;

	rc = ntc_export_buf_zalloc(&eth->tx_wqe_buf, dev->ntc,
				eth->tx_cap *
				sizeof(struct ntc_remote_buf_desc),
				GFP_KERNEL);
	if (rc < 0) {
		ntrdma_err(dev, "dma mapping failed\n");
		goto err_tx_wqe_buf;
	}

	rc = ntc_local_buf_zalloc(&eth->tx_cqe_buf, dev->ntc,
				eth->tx_cap *
				sizeof(struct ntc_remote_buf_desc),
				GFP_KERNEL);
	if (rc < 0) {
		ntrdma_err(dev, "tx cqe buffer alloc failed");
		goto err_tx_cqe_buf;
	}

	tx_prod = peer_info->rx_idx;
	rc = ntc_export_buf_zalloc_init(&eth->tx_prod_buf, dev->ntc,
					sizeof(u32), GFP_KERNEL,
					&tx_prod, sizeof(u32), 0);
	if (rc < 0) {
		ntrdma_err(dev, "tx prod buffer alloc failed");
		goto err_tx_prod_buf;
	}

	eth->is_hello_prep = true;

	return 0;
unprep:
	if (!eth->is_hello_prep)
		return 0;

	eth->is_hello_prep = false;
	ntc_export_buf_free(&eth->tx_prod_buf);
err_tx_prod_buf:
	ntc_local_buf_free(&eth->tx_cqe_buf, dev->ntc);
err_tx_cqe_buf:
	ntc_local_buf_clear(&eth->tx_cqe_buf);
	ntc_export_buf_free(&eth->tx_wqe_buf);
err_tx_wqe_buf:
	eth->peer_vbell_idx = 0;
	eth->tx_cap = 0;
	eth->tx_cons = 0;
	eth->tx_cmpl = 0;
	ntc_export_buf_clear(&eth->tx_wqe_buf);
	ntc_remote_buf_unmap(&eth->peer_rx_cons_buf, dev->ntc);
err_peer_rx_cons_buf:
	ntc_remote_buf_unmap(&eth->peer_rx_cqe_buf, dev->ntc);
err_peer_rx_cqe_buf:
	return rc;
}

int ntrdma_dev_eth_hello_prep(struct ntrdma_dev *dev,
			const struct ntrdma_eth_hello_info *peer_info,
			struct ntrdma_eth_hello_prep __iomem *prep)
{
	struct ntrdma_eth *eth = dev->eth;
	struct ntc_remote_buf_desc tx_wqe_buf_desc;
	struct ntc_remote_buf_desc tx_prod_buf_desc;
	int rc;

	rc = ntrdma_dev_eth_hello_prep_unperp(dev, peer_info, prep, false);
	if (rc)
		return rc;

	ntc_export_buf_make_desc(&tx_wqe_buf_desc, &eth->tx_wqe_buf);
	memcpy_toio(&prep->tx_wqe_buf_desc, &tx_wqe_buf_desc,
		sizeof(tx_wqe_buf_desc));

	ntc_export_buf_make_desc(&tx_prod_buf_desc, &eth->tx_prod_buf);
	memcpy_toio(&prep->tx_prod_buf_desc, &tx_prod_buf_desc,
		sizeof(tx_prod_buf_desc));

	return 0;
}

static inline
int ntrdma_dev_eth_hello_done_undone(struct ntrdma_dev *dev,
				const struct ntrdma_eth_hello_prep *peer_prep,
				int is_undone)
{
	struct ntrdma_eth *eth = dev->eth;
	u32 rx_cons;
	int rc;

	if (is_undone)
		goto undone;

	rc = ntc_remote_buf_map(&eth->peer_tx_wqe_buf, dev->ntc,
				&peer_prep->tx_wqe_buf_desc);
	if (rc < 0) {
		ntrdma_err(dev, "peer tx wqe buffer map failed");
		goto err_peer_tx_wqe_buf;
	}

	rc = ntc_remote_buf_map(&eth->peer_tx_prod_buf, dev->ntc,
				&peer_prep->tx_prod_buf_desc);
	if (rc < 0) {
		ntrdma_err(dev, "peer tx prod buffer map failed");
		goto err_peer_tx_prod_buf;
	}

	eth->is_hello_done = true;

	return 0;
undone:
	if (!eth->is_hello_done)
		return 0;

	eth->is_hello_done = false;
	WARN(eth->link == true,
			"OMG!!! eth hello undone while eth link is up");

	ntrdma_dev_eth_rx_drain(dev);
	rx_cons = eth->rx_cmpl;
	ntc_export_buf_reinit(&eth->rx_cons_buf, &rx_cons, 0, sizeof(rx_cons));

	ntc_remote_buf_unmap(&eth->peer_tx_prod_buf, dev->ntc);
err_peer_tx_prod_buf:
	ntc_remote_buf_unmap(&eth->peer_tx_wqe_buf, dev->ntc);
err_peer_tx_wqe_buf:
	return rc;
}


int ntrdma_dev_eth_hello_done(struct ntrdma_dev *dev,
			const struct ntrdma_eth_hello_prep *peer_prep)
{
	return ntrdma_dev_eth_hello_done_undone(dev, peer_prep, false);
}
#endif
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

#ifdef NTRDMA_FULL_ETH
void ntrdma_dev_eth_reset(struct ntrdma_dev *dev)
{
	ntrdma_dev_eth_hello_done_undone(dev, NULL, true);
	ntrdma_dev_eth_hello_prep_unperp(dev, NULL, NULL, true);
}
#endif

void ntrdma_dev_eth_quiesce(struct ntrdma_dev *dev)
{
	struct ntrdma_eth *eth = dev->eth;
	struct net_device *net = eth->napi.dev;

	netif_tx_disable(net);
	ntrdma_dev_eth_disable(dev);
}

#ifdef NTRDMA_FULL_ETH
static void ntrdma_eth_rx_fill(struct ntrdma_eth *eth)
{
	struct ntrdma_dev *dev = eth->dev;
	size_t off, len;
	u32 start, pos, end, base;
	struct ntc_remote_buf_desc *rx_wqe_buf;
	int rc;

	rc = 0;
	spin_lock_bh(&eth->rx_prod_lock);
	{
		if (!eth->ready) {
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

		rx_wqe_buf = ntc_local_buf_deref(&eth->rx_wqe_buf);

		for (pos = start; pos != end; ++pos) {
			rc = ntc_export_buf_zalloc(&eth->rx_buf[pos], dev->ntc,
						len + SKINFO_SIZE,
						GFP_ATOMIC);
			if (rc < 0) {
				ntrdma_err(dev, "alloc rx buff[%d] failed",
						pos);
				break;
			}

			rc = ntc_export_buf_make_partial_desc(&rx_wqe_buf[pos],
							&eth->rx_buf[pos],
							0, len);
			if (rc < 0) {
				ntc_export_buf_free(&eth->rx_buf[pos]);
				break;
			}
		}

		eth->rx_prod = ntrdma_ring_update(pos, base, eth->rx_cap);

		off = start * sizeof(*rx_wqe_buf);
		len = (pos - start) * sizeof(*rx_wqe_buf);
		rc = ntc_request_memcpy_fenced(eth->dma_chan,
					&eth->peer_tx_wqe_buf, off,
					&eth->rx_wqe_buf, off,
					len, NTC_DMA_WAIT);
		if (unlikely(rc < 0)) {
			ntrdma_err(dev,
				"ntc_request_memcpy failed. rc=%d", rc);
			goto dma_err;
		}

		ntrdma_ring_produce(eth->rx_prod,
				    eth->rx_cmpl,
				    eth->rx_cap,
				    &start, &end, &base);
		if (start != end)
			goto more;

		rc = ntc_request_imm32(eth->dma_chan,
				&eth->peer_tx_prod_buf, 0,
				eth->rx_prod, true, NULL, NULL);
		if (unlikely(rc < 0)) {
			ntrdma_err(dev, "ntc_request_imm32 failed. rc=%d", rc);
			goto dma_err;
		}

		rc = ntrdma_dev_vbell_peer(dev, eth->dma_chan,
					eth->peer_vbell_idx);
		if (unlikely(rc < 0)) {
			ntrdma_err(dev,
				"ntrdma_dev_vbell_peer failed. rc=%d", rc);
			goto dma_err;
		}

		rc = ntc_signal(dev->ntc);
		if (unlikely(rc < 0)) {
			ntrdma_err(dev, "ntc_signal failed. rc=%d", rc);
			goto dma_err;
		}

	 dma_err:
		ntc_req_submit(eth->dma_chan);
		if (unlikely(rc < 0))
			ntrdma_unrecoverable_err(dev);
	}
	spin_unlock_bh(&eth->rx_prod_lock);
}

static int ntrdma_eth_napi_poll(struct napi_struct *napi, int budget)
{
	struct ntrdma_eth *eth = ntrdma_napi_eth(napi);
	struct ntrdma_dev *dev = eth->dev;
	struct sk_buff *skb;
	void *buf;
	u64 rx_offset;
	u64 len, rx_len, buf_len;
	u32 start, pos, end, base;
	int count;
	const struct ntc_remote_buf_desc *rx_cqe_buf;
	struct ntc_remote_buf_desc desc;
	int rc;

	if (!eth->link) {
		napi_complete(&eth->napi);
		return 0;
	}

	ntrdma_vbell_clear(&eth->vbell);

	if (eth->tx_cons != ntrdma_eth_tx_prod(eth))
		netif_wake_queue(eth->napi.dev);

	ntrdma_ring_consume(ntrdma_eth_rx_cons(eth),
			eth->rx_cmpl,
			eth->rx_cap,
			&start, &end, &base);

	end = min_t(u32, end, start + budget);

	rx_cqe_buf = ntc_export_buf_const_deref(&eth->rx_cqe_buf,
						sizeof(*rx_cqe_buf) * start,
						sizeof(*rx_cqe_buf) *
						(end - start));
	/* Make it point to the start of eth->rx_cqe_buf. */
	rx_cqe_buf -= start;

	for (pos = start; pos < end; ++pos) {
		len = eth->rx_buf[pos].size;

		desc = READ_ONCE(rx_cqe_buf[pos]);
		rc = ntc_export_buf_get_part_params(&eth->rx_buf[pos],
						&desc, &rx_offset, &rx_len);
		buf_len = rx_offset + rx_len + SKINFO_SIZE;

		if (!rx_len || WARN_ON(rc < 0) ||
			(buf_len > eth->rx_buf[pos].size)) {
			ntc_export_buf_free(&eth->rx_buf[pos]);
			eth->napi.dev->stats.rx_errors++;
			eth->napi.dev->stats.rx_length_errors++;
			TRACE("POLL ERROR: len %lu buf_len %lu.\n",
				(long)len, (long)buf_len);
			TRACE("POLL ERROR: rx_len %lu rx_offset %lu.\n",
				(long)rx_len, (long)rx_offset);
			ntrdma_info(dev, "POLL ERROR: len %lu buf_len %lu.\n",
				(long)len, (long)buf_len);
			ntrdma_info(dev,
				"POLL ERROR: rx_len %lu rx_offset %lu.\n",
				(long)rx_len, (long)rx_offset);
			continue;
		}

		ntrdma_vdbg(dev, "len %lu buf_len %lu rx_len %lu rx_offset %lu",
			(long)len, (long)buf_len,
			(long)rx_len, (long)rx_offset);
		buf = kmalloc(buf_len, GFP_KERNEL);
		if (!buf) {
			ntc_export_buf_free(&eth->rx_buf[pos]);
			eth->napi.dev->stats.rx_errors++;
			eth->napi.dev->stats.rx_dropped++;
			continue;
		}
		memcpy(buf, ntc_export_buf_const_deref(&eth->rx_buf[pos], 0,
							buf_len), buf_len);
		ntc_export_buf_free(&eth->rx_buf[pos]);

		skb = build_skb(buf, 0);
		if (!skb) {
			kfree(buf);
			eth->napi.dev->stats.rx_errors++;
			eth->napi.dev->stats.rx_dropped++;
			continue;
		}

		skb_reserve(skb, rx_offset);
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
		ntrdma_vbell_readd(&eth->vbell);
	}

	ntrdma_eth_rx_fill(eth);

	return count;
}

struct ntrdma_skb_cb {
	struct ntc_dev *ntc;
	struct ntc_remote_buf dst;
	struct ntc_local_buf src;
};

static netdev_tx_t ntrdma_eth_start_xmit(struct sk_buff *skb,
					 struct net_device *net)
{
	struct ntrdma_eth *eth = ntrdma_net_eth(net);
	struct ntrdma_dev *dev = eth->dev;
	size_t off, len, tx_off;
	u32 pos, end, base;
	struct ntrdma_skb_cb *skb_ctx = NULL;
	bool skb_alloced = false;
	bool skb_ctx_dst_alloced = false;
	bool skb_ctx_src_alloced = false;
	const struct ntc_remote_buf_desc *tx_wqe_buf;
	struct ntc_remote_buf_desc tx_wqe;
	struct ntc_remote_buf_desc *tx_cqe_buf;
	struct ntc_remote_buf_desc tmp_desc;
	u32 tx_prod;
	int rc;

	if (!eth->link) {
		kfree_skb(skb);
		eth->napi.dev->stats.tx_errors++;
		eth->napi.dev->stats.tx_carrier_errors++;
		return NETDEV_TX_OK;
	}

	tx_prod = ntrdma_eth_tx_prod(eth);
	ntrdma_ring_consume(tx_prod,
			    eth->tx_cons,
			    eth->tx_cap,
			    &pos, &end, &base);

	if (pos == end) {
		kfree_skb(skb);
		eth->napi.dev->stats.tx_errors++;
		eth->napi.dev->stats.tx_fifo_errors++;
		goto done;
	}

	tx_wqe_buf = ntc_export_buf_const_deref(&eth->tx_wqe_buf,
						sizeof(*tx_wqe_buf) * pos,
						sizeof(*tx_wqe_buf));
	/* Make it point to the start of eth->tx_wqe_buf. */
	tx_wqe = READ_ONCE(*tx_wqe_buf);

	off = skb_headroom(skb);
	len = skb_headlen(skb);

	tx_off = off & (SMP_CACHE_BYTES - 1);

	if (len + tx_off > tx_wqe.size) {
		eth->napi.dev->stats.tx_errors++;
		eth->napi.dev->stats.tx_dropped++;

		tx_cqe_buf = ntc_local_buf_deref(&eth->tx_cqe_buf);
		tx_cqe_buf[pos] = tx_wqe;
		tx_cqe_buf[pos].size = 0;

		kfree_skb(skb);
	} else {
		skb_alloced = true;

		BUILD_BUG_ON(sizeof(struct ntrdma_skb_cb **) > sizeof(skb->cb));
		skb_ctx = *(struct ntrdma_skb_cb **)skb->cb =
			kmem_cache_alloc_node(skb_cb_slab, GFP_KERNEL,
					dev->node);
		if (!skb_ctx)
			goto err_alloc_skb_ctx;
		skb_ctx->ntc = dev->ntc;

		tmp_desc = tx_wqe;
		rc = ntc_remote_buf_desc_clip(&tmp_desc, 0, len + tx_off);
		if (rc < 0) {
			TRACE("XMIT: Bad size for remote map %lu.\n",
				(long)(len + tx_off));
			goto err_res_map;
		}

		rc = ntc_remote_buf_map(&skb_ctx->dst, dev->ntc,
					&tmp_desc);
		if (rc < 0) {
			ntrdma_err(dev, "failed to map dst buffer");
			goto err_res_map;
		}
		skb_ctx_dst_alloced = true;

		rc = ntc_local_buf_map_prealloced(&skb_ctx->src, dev->ntc,
						skb_end_offset(skb), skb->head);
		if (rc < 0) {
			ntrdma_err(dev, "failed to map src buffer");
			goto err_buf_map;
		}
		skb_ctx_src_alloced = true;

		rc = ntc_request_memcpy_with_cb(eth->dma_chan,
						&skb_ctx->dst, 0,
						&skb_ctx->src, off - tx_off,
						len + tx_off,
						ntrdma_eth_dma_cb, skb, NTC_DMA_WAIT);
		if (unlikely(rc < 0)) {
			ntrdma_err(dev,
				"ntc_request_memcpy failed. rc=%d", rc);
			goto err_memcpy;
		}

		eth->napi.dev->stats.tx_packets++;
		eth->napi.dev->stats.tx_bytes += len;

		tx_cqe_buf = ntc_local_buf_deref(&eth->tx_cqe_buf);
		tx_cqe_buf[pos] = tx_wqe;
		rc = ntc_remote_buf_desc_clip(&tx_cqe_buf[pos], tx_off, len);
		if (unlikely(rc < 0)) {
			ntrdma_err(dev, "ntc_remote_buf_desc_clip error");
			goto err_memcpy;
		}

		netdev_sent_queue(eth->napi.dev, len);
	}

	eth->tx_cons = ntrdma_ring_update(pos + 1, base, eth->tx_cap);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0)
	if (!netdev_xmit_more()) {
#else
	if (!skb->xmit_more) {
#endif
		while (eth->tx_cmpl != eth->tx_cons) {
			ntrdma_ring_consume(eth->tx_cons,
					    eth->tx_cmpl,
					    eth->tx_cap,
					    &pos, &end, &base);

			off = pos * sizeof(struct ntc_remote_buf_desc);
			len = (end - pos) * sizeof(struct ntc_remote_buf_desc);
			rc = ntc_request_memcpy_fenced(eth->dma_chan,
						&eth->peer_rx_cqe_buf, off,
						&eth->tx_cqe_buf, off,
						len, NTC_DMA_WAIT);
			if (unlikely(rc < 0)) {
				ntrdma_err(dev,
					"ntc_request_memcpy failed. rc=%d", rc);
				goto err_memcpy;
			}

			eth->tx_cmpl = ntrdma_ring_update(end, base,
							eth->tx_cap);
		}

		rc = ntc_request_imm32(eth->dma_chan,
				&eth->peer_rx_cons_buf, 0,
				eth->tx_cmpl, true, NULL, NULL);
		if (unlikely(rc < 0)) {
			ntrdma_err(dev, "ntc_request_imm32 failed. rc=%d", rc);
			goto err_memcpy;
		}

		rc = ntrdma_dev_vbell_peer(dev, eth->dma_chan,
					eth->peer_vbell_idx);
		if (unlikely(rc < 0)) {
			ntrdma_err(dev,
				"ntrdma_dev_vbell_peer failed. rc=%d", rc);
			goto err_memcpy;
		}

		rc = ntc_signal(dev->ntc);
		if (unlikely(rc < 0)) {
			ntrdma_err(dev, "ntc_signal failed. rc=%d", rc);
			goto err_memcpy;
		}

		ntc_req_submit(eth->dma_chan);
	}
	goto done;

err_memcpy:
	ntc_req_submit(eth->dma_chan);
	ntrdma_unrecoverable_err(dev);
	if (skb_ctx_src_alloced)
		ntc_local_buf_free(&skb_ctx->src, dev->ntc);
err_buf_map:
	if (skb_ctx_dst_alloced)
		ntc_remote_buf_unmap(&skb_ctx->dst, dev->ntc);
err_res_map:
	if (skb_ctx)
		kmem_cache_free(skb_cb_slab, skb_ctx);
err_alloc_skb_ctx:
	if (skb_alloced) {
		kfree_skb(skb);
		eth->napi.dev->stats.tx_errors++;
		eth->napi.dev->stats.tx_dropped++;
	}
done:
	if (eth->tx_cons == tx_prod) {
		netif_stop_queue(eth->napi.dev);
		napi_schedule(&eth->napi);
	}

	return NETDEV_TX_OK;
}

static void ntrdma_eth_dma_cb(void *ctx, const struct dmaengine_result *result)
{
	struct sk_buff *skb = ctx;
	struct ntrdma_eth *eth = ntrdma_net_eth(skb->dev);
	struct ntrdma_skb_cb *skb_ctx = *(struct ntrdma_skb_cb **)skb->cb;

	/* retrieve the mapped addr from the skb control buffer */

	ntc_local_buf_disown(&skb_ctx->src, skb_ctx->ntc);
	ntc_remote_buf_unmap(&skb_ctx->dst, skb_ctx->ntc);
	kmem_cache_free(skb_cb_slab, skb_ctx);

	consume_skb(skb);

	netdev_completed_queue(eth->napi.dev, 1, skb_headlen(skb));
}

#else
static int ntrdma_eth_napi_poll(struct napi_struct *napi, int budget)
{
	BUG();
	return 0;
}

static netdev_tx_t ntrdma_eth_start_xmit(struct sk_buff *skb,
					 struct net_device *net)
{
	BUG();
}
#endif
static void ntrdma_eth_link_event(struct ntrdma_eth *eth)
{
	bool link = eth->enable && eth->ready;

	if (link == eth->link)
		return;
#ifdef NTRDMA_FULL_ETH
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
#else
	netif_stop_queue(eth->napi.dev);

	if (link) {
		netif_carrier_on(eth->napi.dev);
	} else {
		netif_carrier_off(eth->napi.dev);
	}
#endif
	eth->link = link;
}

static int ntrdma_eth_open(struct net_device *net)
{
	struct ntrdma_eth *eth = ntrdma_net_eth(net);

	eth->enable = true;
	ntrdma_eth_link_event(eth);
	netif_dormant_off(net);

	return 0;
}

static int ntrdma_eth_stop(struct net_device *net)
{
	struct ntrdma_eth *eth = ntrdma_net_eth(net);

	eth->enable = false;
	ntrdma_eth_link_event(eth);
	netif_dormant_on(net);
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

#ifdef NTRDMA_FULL_ETH
int __init ntrdma_eth_module_init(void)
{
	if (!(skb_cb_slab = KMEM_CACHE(ntrdma_skb_cb, 0))) {
		ntrdma_eth_module_deinit();
		pr_err("%s failed to alloc slab\n", __func__);
		return -ENOMEM;
	}

	return 0;
}

void ntrdma_eth_module_deinit(void)
{
	ntrdma_deinit_slab(&skb_cb_slab);
}

#endif
