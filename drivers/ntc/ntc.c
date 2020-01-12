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
#include <linux/kernel.h>
#include <linux/module.h>

#include "ntc.h"
#include <linux/scatterlist.h>
#include <rdma/ib_umem.h>

#include "ntc_internal.h"

#define DRIVER_NAME			"ntc"
#define DRIVER_DESCRIPTION		"NTC Driver Framework"

#define DRIVER_LICENSE			"Dual BSD/GPL"
#define DRIVER_VERSION			"0.2"
#define DRIVER_RELDATE			"2 October 2015"
#define DRIVER_AUTHOR			"Allen Hubbe <Allen.Hubbe@emc.com>"

MODULE_LICENSE(DRIVER_LICENSE);
MODULE_VERSION(DRIVER_VERSION);
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESCRIPTION);

void ntc_flush_dma_channels(struct ntc_dev *ntc)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(ntc->dma_chan); i++)
		if (ntc->dma_chan[i].chan) {
			ntc_info(ntc, "flushing DMA channel %d", i);
			ntc_dma_flush(&ntc->dma_chan[i]);
		} else
			break;

	ntc_info(ntc, "All DMA channels flushed");
}
EXPORT_SYMBOL(ntc_flush_dma_channels);

int ntc_umem_sgl(struct ntc_dev *ntc, struct ib_umem *ib_umem,
		struct ntc_mr_buf *sgl, int count, int mr_access_flags)
{
	struct scatterlist *sg, *next;
	dma_addr_t dma_addr;
	size_t dma_len, offset, total_len;
	int i, n;

	offset = ib_umem_offset(ib_umem);
	total_len = 0;
	n = 0;
	for_each_sg(ib_umem->sg_head.sgl, sg, ib_umem->sg_head.nents, i) {
		/* dma_addr is start DMA addr of the contiguous range */
		dma_addr = sg_dma_address(sg);
		/* dma_len accumulates the length of the contiguous range */
		dma_len = sg_dma_len(sg);

		TRACE("ntc_umem_sgl: dma_addr %#llx access %d",
			dma_addr, mr_access_flags);

		for (; i + 1 < ib_umem->sg_head.nents; ++i) {
			next = sg_next(sg);
			if (!next)
				break;
			if (sg_dma_address(next) != dma_addr + dma_len)
				break;
			dma_len += sg_dma_len(next);
			sg = next;
		}

		if (dma_len <= offset) {
			offset -= dma_len;
			continue;
		}

		if (offset) {
			dma_addr += offset;
			dma_len -= offset;
			offset = 0;
		}

		total_len += dma_len;
		if (total_len > ib_umem->length) {
			dma_len -= total_len - ib_umem->length;
			total_len = ib_umem->length;
		}

		TRACE("ntc_umem_sgl: dma_len %#llx", (u64)dma_len);
		if (sgl && (n < count)) {
			if (ntc_mr_buf_map_dma(&sgl[n], ntc, dma_len,
						dma_addr, mr_access_flags) < 0)
				break;
		}

		++n;

		if (total_len == ib_umem->length)
			break;
	}

	return n;
}
EXPORT_SYMBOL(ntc_umem_sgl);

static int ntc_probe(struct device *dev)
{
	struct ntc_dev *ntc = ntc_of_dev(dev);
	struct ntc_driver *driver = ntc_of_driver(dev->driver);
	int rc;

	ntc_vdbg(ntc, "probe");

	get_device(dev);
	rc = driver->ops.probe(driver, ntc);
	if (rc)
		put_device(dev);

	ntc_vdbg(ntc, "probe return %d", rc);

	return rc;
}

static int ntc_remove(struct device *dev)
{
	struct ntc_dev *ntc = ntc_of_dev(dev);
	struct ntc_driver *driver;

	ntc_vdbg(ntc, "remove");

	if (dev->driver) {
		driver = ntc_of_driver(dev->driver);

		driver->ops.remove(driver, ntc);
		put_device(dev);
	}

	return 0;
}

static struct bus_type ntc_bus = {
	.name = "ntc",
	.probe = ntc_probe,
	.remove = ntc_remove,
};

struct bus_type *ntc_bus_ptr(void)
{
	return &ntc_bus;
}
EXPORT_SYMBOL(ntc_bus_ptr);

static
struct ntc_dma_chan *ntc_req_rr(struct ntc_dev *ntc,
				enum ntc_dma_chan_type type)
{
	int old_index;
	int i;

	do {
		old_index = atomic_read(&ntc->dma_chan_rr_index[type]);
		i = old_index + 1;

		if ((i >= ARRAY_SIZE(ntc->dma_chan)) || !ntc->dma_chan[i].chan)
			i = 0;

	} while (atomic_cmpxchg(&ntc->dma_chan_rr_index[type], old_index, i) !=
		old_index);

	ntc_info(ntc, "ntc_req_rr for type %d returns dma_chan #%d", type, i);

	return &ntc->dma_chan[i];
}

void ntc_init_dma_chan(struct ntc_dma_chan **dma_chan,
		struct ntc_dev *ntc, enum ntc_dma_chan_type type)
{
	*dma_chan = ntc_req_rr(ntc, type);
}
EXPORT_SYMBOL(ntc_init_dma_chan);

static int __init ntc_driver_init(void)
{
	int rc;

	pr_info("%s: %s %s init\n", DRIVER_NAME,
		DRIVER_DESCRIPTION, DRIVER_VERSION);
	rc = bus_register(&ntc_bus);
	if (rc < 0)
		return rc;

	rc = ntc_init();
	if (rc < 0)
		bus_unregister(&ntc_bus);

	return rc;
}
module_init(ntc_driver_init);

static void __exit ntc_driver_exit(void)
{
	ntc_exit();
	bus_unregister(&ntc_bus);
	pr_info("%s: %s %s exit\n", DRIVER_NAME,
			DRIVER_DESCRIPTION, DRIVER_VERSION);
}
module_exit(ntc_driver_exit);

