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

#include "ntrdma.h"
#include "ntrdma_dev.h"

#define DRIVER_NAME "ntrdma"
#define DRIVER_VERSION  "0.2"
#define DRIVER_RELDATE  "20 October 2015"

MODULE_AUTHOR("Allen Hubbe");
MODULE_DESCRIPTION("RDMA Driver for PCIe NTB and DMA");
MODULE_LICENSE("GPL v2");
MODULE_VERSION(DRIVER_VERSION);

static int ntrdma_probe(struct ntc_driver *self,
			struct ntc_dev *ntc)
{
	struct ntrdma_dev *dev;
	int rc;

	pr_devel("probe ntc %s\n", dev_name(&ntc->dev));

	ntc_link_disable(ntc);

	dev = ib_alloc_device(ntrdma_dev, ibdev);
	if (!dev)
		return -ENOMEM;

	rc = ntrdma_dev_init(dev, ntc);
	if (rc)
		goto err_init;

	ntc_link_enable(ntc);

	return 0;

	ntrdma_dev_deinit(dev);
err_init:
	ib_dealloc_device((void *)dev);
	return rc;
}

static void ntrdma_remove(struct ntc_driver *self, struct ntc_dev *ntc)
{
	struct ntrdma_dev *dev = ntc_get_ctx(ntc);

	pr_devel("remove ntc %s\n", dev_name(&ntc->dev));

	ntc_link_disable(ntc);
	ntrdma_dev_deinit(dev);
	ib_dealloc_device((void *)dev);
}

struct ntc_driver ntrdma_driver = {
	.drv = {
		.name = KBUILD_MODNAME,
	},
	.ops = {
		.probe = ntrdma_probe,
		.remove = ntrdma_remove,
	},
};

static int __init ntrdma_init(void)
{
	ntrdma_debugfs_init();
	ntc_register_driver(&ntrdma_driver);
	return 0;
}
module_init(ntrdma_init);

static void __exit ntrdma_exit(void)
{
	ntc_unregister_driver(&ntrdma_driver);
	ntrdma_debugfs_deinit();
}
module_exit(ntrdma_exit);
