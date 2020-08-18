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
#include "ntrdma_file.h"
#include "ntrdma_dev.h"

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

#define DRIVER_NAME "ntrdma"
#define DRIVER_VERSION  "0.2"
#define DRIVER_RELDATE  "20 October 2015"

#define MAX_LEN 2
#define IS_UP(buf) (buf[0] == '1')
#define IS_DOWN(buf) (buf[0] == '0')

MODULE_AUTHOR("Allen Hubbe");
MODULE_DESCRIPTION("RDMA Driver for PCIe NTB and DMA");
MODULE_LICENSE("GPL v2");
MODULE_VERSION(DRIVER_VERSION);
#ifdef NTRDMA_GIT_HASH
MODULE_INFO(githash, TOSTRING(NTRDMA_GIT_HASH));
#endif


static ssize_t link_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct ib_device *ibdev = container_of(dev, struct ib_device, dev);
	struct ntrdma_dev *ntrdma_dev = ntrdma_ib_dev(ibdev);
	struct ntc_dev *ntc = ntrdma_dev->ntc;

	return snprintf(buf, MAX_LEN, "%d\n", ntc_is_link_up(ntc));
}

static ssize_t link_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	int rc = 0;

	struct ib_device *ibdev = container_of(dev, struct ib_device, dev);
	struct ntrdma_dev *ntrdma_dev = ntrdma_ib_dev(ibdev);
	struct ntc_dev *ntc = ntrdma_dev->ntc;

	if (!ntc) {
		pr_err("sysfs: failed to store sysfs link file, ntc is NULL\n");
		return -ENODEV;
	}

	if (strnlen(buf, MAX_LEN+1) != MAX_LEN) {
		pr_err("sysfs: wrong param %s\n", buf);
		return -EINVAL;
	}

	if (IS_DOWN(buf) && ntc_is_link_up(ntc)) {
		pr_debug("sysfs: changed link state to %s\n", buf);
		rc = ntc_link_disable(ntc);
	} else if (IS_UP(buf) && !ntc_is_link_up(ntc)) {
		pr_debug("sysfs: changed link state to %s\n", buf);
		rc = ntc_link_enable(ntc);
	} else {
		pr_err("sysfs: link already %s\n", buf);
	}

	if (rc)
		pr_err("sysfs: could not set link state (%s) by ntc, rc %d\n",
				buf, rc);

	return count;
}

static struct device_attribute attr = {
	.attr = {
		.name = "link",
		.mode = 0660,/*S_IWUSR | S_IRUGO,*/
	},
	.show = link_show,
	.store = link_store,
};

static int ntrdma_probe(struct ntc_driver *self,
			struct ntc_dev *ntc)
{
	struct ntrdma_dev *dev;
	int rc;

	pr_devel("probe ntc %s\n", dev_name(&ntc->dev));
#ifdef NTRDMA_GIT_HASH
	pr_info("Probe ntrdma - git hash %s", TOSTRING(NTRDMA_GIT_HASH));
#endif
	ntc_link_disable(ntc);

	dev = (void *)ib_alloc_device(sizeof(*dev));
	if (!dev)
		return -ENOMEM;

	rc = ntrdma_dev_init(dev, ntc);
	if (rc)
		goto err_init;

	ntc_link_enable(ntc);

	rc = device_create_file((struct device *)&dev->ibdev.dev, &attr);
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
	device_remove_file((struct device *)&dev->ibdev.dev, &attr);
	ntrdma_dev_ib_deinit(dev);
	ntc_link_disable(ntc);
	ntc_link_reset(dev->ntc, SYNC_RESET);
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

static void ntrdma_deinit(void)
{
	ntrdma_debugfs_deinit();
	ntrdma_eth_module_deinit();
	ntrdma_ib_module_deinit();
	ntrdma_qp_module_deinit();
	ntrdma_vbell_module_deinit();
}

static int __init ntrdma_init(void)
{
	int rc;

	pr_info("NTRDMA module init\n");

	rc = ntrdma_vbell_module_init();
	if (rc < 0)
		goto err;

	rc = ntrdma_qp_module_init();
	if (rc < 0)
		goto err;

	rc = ntrdma_ib_module_init();
	if (rc < 0)
		goto err;

	rc = ntrdma_eth_module_init();
	if (rc < 0)
		goto err;

	rc = ntrdma_debugfs_init();
	if (rc < 0)
		goto err;

	rc = ntc_register_driver(&ntrdma_driver);
	if (rc < 0)
		goto err;

	rc = ntrdma_file_register();
	if (rc < 0)
		goto err;

	return 0;

 err:
	ntrdma_deinit();
	return rc;
}
module_init(ntrdma_init);

static __exit void ntrdma_exit(void)
{
	ntrdma_file_unregister();
	ntc_unregister_driver(&ntrdma_driver);
	ntrdma_deinit();
	pr_info("NTRDMA module exit\n");
}
module_exit(ntrdma_exit);
