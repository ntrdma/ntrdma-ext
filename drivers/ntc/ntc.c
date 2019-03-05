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

#include <linux/ntc.h>

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

static struct bus_type ntc_bus;

int __ntc_register_driver(struct ntc_driver *driver, struct module *mod,
			  const char *mod_name)
{
	if (!ntc_driver_ops_is_valid(&driver->ops))
		return -EINVAL;

	pr_devel("register driver %s\n", driver->drv.name);

	driver->drv.bus = &ntc_bus;
	driver->drv.name = mod_name;
	driver->drv.owner = mod;

	return driver_register(&driver->drv);
}
EXPORT_SYMBOL(__ntc_register_driver);

void ntc_unregister_driver(struct ntc_driver *driver)
{
	pr_devel("unregister driver %s\n", driver->drv.name);

	driver_unregister(&driver->drv);
}
EXPORT_SYMBOL(ntc_unregister_driver);

int ntc_register_device(struct ntc_dev *ntc)
{
	if (!ntc->dev_ops)
		goto err;
	if (!ntc_dev_ops_is_valid(ntc->dev_ops))
		goto err;
	if (!ntc->map_ops)
		goto err;
	if (!ntc_map_ops_is_valid(ntc->map_ops))
		goto err;

	pr_devel("register device %s\n", dev_name(&ntc->dev));

	ntc->ctx_ops = NULL;

	ntc->dev.bus = &ntc_bus;

	return device_register(&ntc->dev);

err:
	if (!WARN_ON(!ntc->dev.release))
		ntc->dev.release(&ntc->dev);
	return -EINVAL;
}
EXPORT_SYMBOL(ntc_register_device);

void ntc_unregister_device(struct ntc_dev *ntc)
{
	pr_devel("unregister device %s\n", dev_name(&ntc->dev));

	device_unregister(&ntc->dev);

	ntc->dev.bus = NULL;
}
EXPORT_SYMBOL(ntc_unregister_device);

int ntc_set_ctx(struct ntc_dev *ntc, void *ctx,
		const struct ntc_ctx_ops *ctx_ops)
{
	if (ntc_get_ctx(ntc))
		return -EINVAL;

	if (!ctx_ops)
		return -EINVAL;
	if (!ntc_ctx_ops_is_valid(ctx_ops))
		return -EINVAL;

	dev_vdbg(&ntc->dev, "set ctx\n");

	dev_set_drvdata(&ntc->dev, ctx);
	wmb(); /* if ctx_ops is set, drvdata must be set */
	ntc->ctx_ops = ctx_ops;

	return 0;
}
EXPORT_SYMBOL(ntc_set_ctx);

void ntc_clear_ctx(struct ntc_dev *ntc)
{
	dev_vdbg(&ntc->dev, "clear ctx\n");

	ntc->ctx_ops = NULL;
	wmb(); /* if ctx_ops is set, drvdata must be set */
	dev_set_drvdata(&ntc->dev, NULL);
}
EXPORT_SYMBOL(ntc_clear_ctx);

int ntc_ctx_hello(struct ntc_dev *ntc, int phase,
		      void *in_buf, size_t in_size,
		      void *out_buf, size_t out_size)
{
	const struct ntc_ctx_ops *ctx_ops;
	void *ctx;

	dev_dbg(&ntc->dev, "hello phase %d\n", phase);

	ctx = ntc_get_ctx(ntc);
	rmb(); /* if ctx_ops is set, drvdata must be set */
	ctx_ops = ntc->ctx_ops;

	if (ctx_ops && ctx_ops->hello)
		return ctx_ops->hello(ctx, phase,
				      in_buf, in_size,
				      out_buf, out_size);

	if (phase || in_size || out_size)
		return -EINVAL;

	return 0;
}
EXPORT_SYMBOL(ntc_ctx_hello);

void ntc_ctx_enable(struct ntc_dev *ntc)
{
	const struct ntc_ctx_ops *ctx_ops;
	void *ctx;

	dev_dbg(&ntc->dev, "enable\n");

	ctx = ntc_get_ctx(ntc);
	rmb(); /* if ctx_ops is set, drvdata must be set */
	ctx_ops = ntc->ctx_ops;

	if (ctx_ops)
		ctx_ops->enable(ctx);
}
EXPORT_SYMBOL(ntc_ctx_enable);

void ntc_ctx_disable(struct ntc_dev *ntc)
{
	const struct ntc_ctx_ops *ctx_ops;
	void *ctx;

	dev_dbg(&ntc->dev, "disable\n");

	ctx = ntc_get_ctx(ntc);
	rmb(); /* if ctx_ops is set, drvdata must be set */
	ctx_ops = ntc->ctx_ops;

	if (ctx_ops)
		ctx_ops->disable(ctx);
}
EXPORT_SYMBOL(ntc_ctx_disable);

void ntc_ctx_quiesce(struct ntc_dev *ntc)
{
	const struct ntc_ctx_ops *ctx_ops;
	void *ctx;

	dev_dbg(&ntc->dev, "quiesce\n");

	ctx = ntc_get_ctx(ntc);
	rmb(); /* if ctx_ops is set, drvdata must be set */
	ctx_ops = ntc->ctx_ops;

	if (ctx_ops && ctx_ops->quiesce)
		ctx_ops->quiesce(ctx);
}
EXPORT_SYMBOL(ntc_ctx_quiesce);

void ntc_ctx_reset(struct ntc_dev *ntc)
{
	const struct ntc_ctx_ops *ctx_ops;
	void *ctx;

	dev_dbg(&ntc->dev, "reset\n");

	ctx = ntc_get_ctx(ntc);
	rmb(); /* if ctx_ops is set, drvdata must be set */
	ctx_ops = ntc->ctx_ops;

	if (ctx_ops && ctx_ops->reset)
		ctx_ops->reset(ctx);
}
EXPORT_SYMBOL(ntc_ctx_reset);

void ntc_ctx_signal(struct ntc_dev *ntc, int vec)
{
	const struct ntc_ctx_ops *ctx_ops;
	void *ctx;

	dev_vdbg(&ntc->dev, "signal\n");

	ctx = ntc_get_ctx(ntc);
	rmb(); /* if ctx_ops is set, drvdata must be set */
	ctx_ops = ntc->ctx_ops;

	if (ctx_ops && ctx_ops->signal)
		ctx_ops->signal(ctx, vec);
}
EXPORT_SYMBOL(ntc_ctx_signal);

static int ntc_probe(struct device *dev)
{
	struct ntc_dev *ntc = ntc_of_dev(dev);
	struct ntc_driver *driver = ntc_of_driver(dev->driver);
	int rc;

	dev_vdbg(dev, "probe\n");

	get_device(dev);
	rc = driver->ops.probe(driver, ntc);
	if (rc)
		put_device(dev);

	dev_vdbg(dev, "probe return %d\n", rc);

	return rc;
}

static int ntc_remove(struct device *dev)
{
	struct ntc_dev *ntc = ntc_of_dev(dev);
	struct ntc_driver *driver;

	dev_vdbg(dev, "remove\n");

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

static int __init ntc_driver_init(void)
{
	pr_info("%s: %s %s\n", DRIVER_NAME,
		DRIVER_DESCRIPTION, DRIVER_VERSION);
	return bus_register(&ntc_bus);
}
module_init(ntc_driver_init);

static void __exit ntc_driver_exit(void)
{
	bus_unregister(&ntc_bus);
}
module_exit(ntc_driver_exit);

