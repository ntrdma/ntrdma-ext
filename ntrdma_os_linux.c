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

#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/pci.h>
#include <linux/dmaengine.h>
#include <linux/ntb.h>

#include "ntrdma_debugfs.h"
#include "ntrdma_ntb_probe.h"
#include "ntrdma_ntb.h"
#include "ntrdma_port.h"
#include "ntrdma_vbell.h"
#include "ntrdma_mem.h"
#include "ntrdma_ntb_impl.h"
#include "ntrdma_dma_impl.h"

#define DRIVER_NAME "ntrdma"
#define DRIVER_VERSION  "0.1"
#define DRIVER_RELDATE  "December 10, 2014"

MODULE_AUTHOR("Allen Hubbe");
MODULE_DESCRIPTION("RDMA Provider over PCIe NTB and DMA");
MODULE_LICENSE("GPL v2");
MODULE_VERSION(DRIVER_VERSION);

struct ntb_ctx_ops ntrdma_ntb_ops;

int ntrdma_ntb_impl_node(void *ntb_impl)
{
	struct ntb_dev *ntb = ntb_impl;

	return dev_to_node(&ntb->dev);
}

void *ntrdma_ntb_impl_get_device(void *ntb_impl)
{
	struct ntb_dev *ntb = ntb_impl;

	return &ntb->pdev->dev;
}

struct ntb_ctx_foo {
	void *ctx;
	const struct ntrdma_ntb_impl_ops *ops;
	NTRDMA_DECL_DPC(link_event_dpc);
	NTRDMA_DECL_DPC(db_event_dpc);
};

void *ntrdma_ntb_impl_get_ctx(void *ntb_impl)
{
	struct ntb_dev *ntb = ntb_impl;
	struct ntb_ctx_foo *foo = ntb->ctx;

	return foo->ctx;
}

static NTRDMA_DECL_DPC_CB(ntrdma_link_event_dpc, ptrhld)
{
	struct ntb_ctx_foo *foo = NTRDMA_CAST_DPC_CTX(ptrhld);

	foo->ops->link_event(foo->ctx);
}

static NTRDMA_DECL_DPC_CB(ntrdma_db_event_dpc, ptrhld)
{
	struct ntb_ctx_foo *foo = NTRDMA_CAST_DPC_CTX(ptrhld);

	foo->ops->db_event(foo->ctx);
}

static void ntrdma_link_event_isr(void *ctx)
{
	struct ntb_ctx_foo *foo = ctx;

	ntrdma_dpc_fire(&foo->link_event_dpc);
}

static void ntrdma_db_event_isr(void *ctx, int vec)
{
	struct ntb_ctx_foo *foo = ctx;

	ntrdma_dpc_fire(&foo->db_event_dpc);
}

void ntrdma_ntb_impl_set_ctx(void *ntb_impl, void *ctx,
			     const struct ntrdma_ntb_impl_ops *ops)
{
	struct ntb_dev *ntb = ntb_impl;
	struct ntb_ctx_foo *foo;

	if (ops) {
		foo = ntrdma_malloc(sizeof(*foo), dev_to_node(&ntb->dev));

		dev_dbg(&ntb->dev, "set ntb %p ctx foo %p\n", ntb, foo);

		foo->ctx = ctx;
		foo->ops = ops;

		ntrdma_dpc_create(&foo->link_event_dpc, "foo_link",
				  ntrdma_link_event_dpc, foo);
		ntrdma_dpc_create(&foo->db_event_dpc, "foo_db",
				  ntrdma_db_event_dpc, foo);

		ntb_set_ctx(ntb, foo, &ntrdma_ntb_ops);
	} else {
		foo = ntb->ctx;

		dev_dbg(&ntb->dev, "clear ntb %p ctx foo %p\n", ntb, foo);

		ntb_clear_ctx(ntb);

		ntrdma_free(foo);
	}
}

void ntrdma_ntb_link_enable(void *ntb_impl)
{
	ntb_link_enable(ntb_impl, NTB_SPEED_AUTO, NTB_WIDTH_AUTO);
}

void ntrdma_ntb_link_disable(void *ntb_impl)
{
	ntb_link_disable(ntb_impl);
}

int ntrdma_ntb_impl_link_is_up(void *ntb_impl)
{
	return ntb_link_is_up(ntb_impl, NULL, NULL);
}

ntrdma_u32_t ntrdma_ntb_impl_spad_read(void *ntb_impl, int idx)
{
	return ntb_spad_read(ntb_impl, idx);
}

void ntrdma_ntb_impl_spad_write(void *ntb_impl, int idx, ntrdma_u32_t val)
{
	ntb_spad_write(ntb_impl, idx, val);
}

ntrdma_u32_t ntrdma_ntb_impl_peer_spad_read(void *ntb_impl, int idx)
{
	return ntb_peer_spad_read(ntb_impl, idx);
}

void ntrdma_ntb_impl_peer_spad_write(void *ntb_impl, int idx, ntrdma_u32_t val)
{
	ntb_peer_spad_write(ntb_impl, idx, val);
}

ntrdma_dma_addr_t ntrdma_ntb_impl_peer_dram_dma(void *ntb_impl)
{
	phys_addr_t base;
	ntb_mw_get_range(ntb_impl, 1, &base, NULL, NULL, NULL);
	return base;
}

ntrdma_dma_addr_t ntrdma_ntb_impl_peer_msi_dma(void *ntb_impl)
{
	return ntrdma_ntb_impl_peer_dram_dma(ntb_impl);
}

void ntrdma_ntb_impl_msi_dma(void *ntb_impl,
			     ntrdma_dma_addr_t *addr,
			     ntrdma_u32_t *val)
{
	struct ntb_dev *ntb = ntb_impl;
	u32 addr_lo, addr_hi, data;

	/* FIXME: add support for 64-bit MSI header, and MSI-X.  Until it is
	 * fixed here, we need to disable MSI-X in the NTB HW driver.
	 */

	pci_read_config_dword(ntb->pdev,
			      ntb->pdev->msi_cap + PCI_MSI_ADDRESS_LO,
			      &addr_lo);

	/* FIXME: msi header layout differs for 32/64 bit addr width. */
	addr_hi = 0;

	pci_read_config_dword(ntb->pdev,
			      ntb->pdev->msi_cap + PCI_MSI_DATA_32,
			      &data);

	*addr = (u64)addr_lo | (u64)addr_hi << 32;
	*val = data;

	dev_dbg(&ntb->dev, "msi addr %#llx data %#x\n", *addr, *val);
}

ntrdma_dma_addr_t ntrdma_ntb_impl_peer_spad_dma(void *ntb_impl, int idx)
{
	ntrdma_pr("not implemented\n");
	return 0;
}

void ntrdma_dma_impl_abort_all(void *dma_impl)
{
	ntrdma_pr("not implemented\n");
}

struct dma_req_foo {
	void *dma_impl;
	void (*cb)(void *cb_ctx);
	void *cb_ctx;
};

void *ntrdma_dma_impl_alloc_req(void *dma_impl,
				void (*cb)(void *cb_ctx),
				void *cb_ctx,
				int cap_hint)
{
	struct dma_chan *chan = dma_impl;
	struct dma_req_foo *req;

	req = ntrdma_malloc(sizeof(*req),
			    dev_to_node(&chan->dev->device));
	req->dma_impl = chan;
	req->cb = cb;
	req->cb_ctx = cb_ctx;

	return req;
}

int ntrdma_dma_impl_req_append(void *dma_req,
			       struct ntrdma_dma_sge *sgl,
			       int count)
{
	return ntrdma_dma_impl_req_append_one(dma_req,
					      sgl->dst,
					      sgl->src,
					      sgl->len,
					      sgl->flags);
}

int ntrdma_dma_impl_req_append_one(void *dma_req,
				   ntrdma_dma_addr_t dst,
				   ntrdma_dma_addr_t src,
				   ntrdma_size_t len,
				   unsigned long flags)
{
	struct dma_req_foo *req = dma_req;
	struct dma_chan *chan;
	struct dma_async_tx_descriptor *tx;

	chan = req->dma_impl;

	/* FIXME: somewhere is submitting with len zero!!! */
	if (!len)
		return 0;

	tx = chan->device->device_prep_dma_memcpy(chan,
						  dst, src,
						  len, flags);
	/* FIXME: we can run out of tx.  this is bad news!!! */
	if (WARN_ON(!tx)) {
		pr_warn("no tx for dst %#llx src %#llx len %#zx flags %#lx\n",
			dst, src, len, flags);
		return -ENOMEM;
	}

	dmaengine_submit(tx);

	return 0;
}

#ifndef CONFIG_NTRDMA_IMM_BUF
int ntrdma_dma_impl_req_append_imm(void *dma_req,
				   ntrdma_dma_addr_t dst,
				   void *src,
				   ntrdma_size_t len,
				   unsigned long flags)
{
	struct dma_req_foo *req = dma_req;
	struct dma_chan *chan;
	struct dma_async_tx_descriptor *tx;

	chan = req->dma_impl;

	tx = chan->device->device_prep_dma_imm(chan,
					       dst, src,
					       len, flags);

	dmaengine_submit(tx);

	return 0;
}
#endif

void ntrdma_dma_impl_submit_req(void *dma_req)
{
	struct dma_req_foo *req = dma_req;
	struct dma_chan *chan;
	struct dma_async_tx_descriptor *tx;

	chan = req->dma_impl;

	tx = chan->device->device_prep_dma_interrupt(chan, 0);

	tx->callback = req->cb;
	tx->callback_param = req->cb_ctx;

	dmaengine_submit(tx);

	dma_async_issue_pending(chan);

	ntrdma_free(req);
}

void ntrdma_dma_impl_free_req(void *dma_req)
{
	struct dma_req_foo *req = dma_req;

	ntrdma_free(req);
}

static struct pci_bus *ntrdma_ascend_bus(struct pci_bus *bus)
{
	while(bus->parent)
		bus = bus->parent;

	return bus;
}

static bool ntrdma_dma_filter_fn(struct dma_chan *chan,
				 void *filter_param)
{
	struct pci_dev *pdev;

	pdev = to_pci_dev(chan->dev->device.parent);

	return ntrdma_ascend_bus(pdev->bus) == filter_param;
}

static int ntrdma_os_probe(struct ntb_client *self,
			   struct ntb_dev *ntb)
{
	struct dma_chan *dma;
	dma_cap_mask_t mask;
	phys_addr_t mw_base;
	resource_size_t mw_size;
	int rc;

	dev_dbg(&ntb->dev, "enter\n");

	/* clear the warning flags */
	ntb_db_is_unsafe(ntb);
	ntb_spad_is_unsafe(ntb);

	dma_cap_zero(mask);
	dma_cap_set(DMA_MEMCPY, mask);

	dma = dma_request_channel(mask, ntrdma_dma_filter_fn,
				  ntrdma_ascend_bus(ntb->pdev->bus));
	if (!dma) {
		dev_dbg(&ntb->dev, "failed to get dma channel\n");
		return -ENODEV;
	}

	ntb_mw_clear_trans(ntb, 0);

	ntb_mw_get_range(ntb, 1, &mw_base, &mw_size, NULL, NULL);
	ntb_mw_set_trans(ntb, 1, 0, mw_size);
	dev_dbg(&ntb->dev, "mw %d base %#llx size %#llx trans %llx for RDMA\n",
		1, mw_base, mw_size, 0ull);

	ntb_db_is_unsafe(ntb);
	ntb_spad_is_unsafe(ntb);

	rc = ntrdma_ntb_probe(ntb, dma);
	if (rc) {
		dma_release_channel(dma);
		return rc;
	}

	return 0;
}

static void ntrdma_os_remove(struct ntb_client *self, struct ntb_dev *ntb)
{
	struct ntrdma_ntb_dev *dev = ntrdma_ntb_impl_get_ctx(ntb);
	struct dma_chan *dma = dev->dma_impl;

	ntrdma_ntb_remove(ntb);
	dma_release_channel(dma);
}

struct ntb_ctx_ops ntrdma_ntb_ops = {
	.link_event = ntrdma_link_event_isr,
	.db_event = ntrdma_db_event_isr,
};

struct ntb_client ntrdma_ntb_client = {
	.ops = {
		.probe = ntrdma_os_probe,
		.remove = ntrdma_os_remove,
	},
};

static int __init ntrdma_init(void)
{
	ntrdma_debugfs_init();
	ntb_register_client(&ntrdma_ntb_client);
	return 0;
}
module_init(ntrdma_init);

static void __exit ntrdma_exit(void)
{
	ntb_unregister_client(&ntrdma_ntb_client);
	ntrdma_debugfs_deinit();
}
module_exit(ntrdma_exit);
