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
#include <linux/kobject.h>
#include <linux/sysfs.h>
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

static int sysfs_link_disable;

static ssize_t link_disable_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	return sprintf(buf, "%d\n", sysfs_link_disable);
}

static ssize_t link_disable_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct ntc_dev *ntc = (struct ntc_dev *)dev;

	int rc = sscanf(buf, "%du", &sysfs_link_disable);

	pr_devel("sysfs entry was set by user to %d rc %d\n",
			sysfs_link_disable, rc);

	if (ntc && rc > 0) {
		rc = (!sysfs_link_disable) ? ntc_link_enable(ntc) :
				ntc_link_disable(ntc);

		if (rc)
			pr_err("could not set link state (%d) by ntc, rc %d\n",
				sysfs_link_disable, rc);
	}
	return count;
}

static struct device_attribute attr = {
	.attr = {
		.name = "link_disable",
		.mode = 0660,/*S_IWUSR | S_IRUGO,*/
	},
	.show = link_disable_show,
	.store = link_disable_store,
};

static int ntrdma_probe(struct ntc_driver *self,
			struct ntc_dev *ntc)
{
	struct ntrdma_dev *dev;
	int rc;

	pr_devel("probe ntc %s\n", dev_name(&ntc->dev));

	ntc_link_disable(ntc);

	dev = (void *)ib_alloc_device(sizeof(*dev));
	if (!dev)
		return -ENOMEM;

	rc = ntrdma_dev_init(dev, ntc);
	if (rc)
		goto err_init;

	ntc_link_enable(ntc);

	rc = device_create_file((struct device *)ntc, &attr);
	if (rc)
		pr_err("failed to create sysfs entry rc = %d\n", rc);


	return 0;

err_init:
	ib_dealloc_device((void *)dev);
	return rc;
}

static void ntrdma_remove(struct ntc_driver *self, struct ntc_dev *ntc)
{
	struct ntrdma_dev *dev = ntc_get_ctx(ntc);
	pr_devel("remove ntc %s\n", dev_name(&ntc->dev));
	device_remove_file((struct device *)ntc, &attr);
	ntrdma_dev_ib_deinit(dev);
	ntc_link_disable(ntc);
	ntc_link_reset(dev->ntc);
	/* Prevent callbacks from the lower layer */
	ntc_clear_ctx(dev->ntc);
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
	pr_info("NTRDMA module init\n");
	ntrdma_debugfs_init();
	ntc_register_driver(&ntrdma_driver);
	return 0;
}
module_init(ntrdma_init);

static void __exit ntrdma_exit(void)
{
	ntc_unregister_driver(&ntrdma_driver);
	ntrdma_debugfs_deinit();
	pr_info("NTRDMA module exit\n");
}
module_exit(ntrdma_exit);
