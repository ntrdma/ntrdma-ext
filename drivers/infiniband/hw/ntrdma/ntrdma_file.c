/*
 * Copyright (c) 2019, Dell Technologies, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include "ntrdma_file.h"

#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/module.h>

#define NTRDMA_MINORS		(1U << MINORBITS)

#define NTRDMA_FILE_IOCTL_BASE 'F'

#define NTRDMA_FILEIOCTL_REG		_IOWR(NTRDMA_FILE_IOCTL_BASE, 0x30, u32)
#define NTRDMA_FILEIOCTL_SEND		_IOWR(NTRDMA_FILE_IOCTL_BASE, 0x31, u32)

static int ntrdma_file_open(struct inode *inode, struct file *filp);
static int ntrdma_file_release(struct inode *inode, struct file *filp);
static long ntrdma_file_ioctl(struct file *filp, unsigned int cmd,
			unsigned long arg);

static bool ntrdma_chrdev_registered;
static int ntrdma_char_major;
module_param(ntrdma_char_major, int, 0);
static struct class *ntrdma_class;
struct device *ntrdma_dev;

static const struct file_operations ntrdma_file_ops = {
	.owner		= THIS_MODULE,
	.open		= ntrdma_file_open,
	.release	= ntrdma_file_release,
	.unlocked_ioctl	= ntrdma_file_ioctl,
	.compat_ioctl	= ntrdma_file_ioctl,
};

bool ntrdma_measure_perf;
static ssize_t show_measure_perf(struct device *dev,
				struct device_attribute *attr,
				char *buf);
static ssize_t write_measure_perf(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count);
static DEVICE_ATTR(measure_perf, S_IRUGO | S_IWUSR,
		show_measure_perf, write_measure_perf);

bool ntrdma_print_debug;
static ssize_t show_print_debug(struct device *dev,
				struct device_attribute *attr,
				char *buf);
static ssize_t write_print_debug(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count);
static DEVICE_ATTR(print_debug, S_IRUGO | S_IWUSR,
		show_print_debug, write_print_debug);

static struct attribute *ntrdma_file_attrs[] = {
	&dev_attr_measure_perf.attr,
	&dev_attr_print_debug.attr,
	NULL,
};

static struct attribute_group ntrdma_file_attrs_group = {
	.attrs		= ntrdma_file_attrs,
};

static const struct attribute_group *ntrdma_file_attrs_groups[] = {
	&ntrdma_file_attrs_group,
	NULL,
};

struct ntrdma_common_data {
	struct page *common_page;
	void volatile *common_ptr;
};

static int ntrdma_file_open(struct inode *inode, struct file *filp)
{
	struct ntrdma_common_data *common_data;

	common_data = kmalloc(sizeof(*common_data), GFP_KERNEL);
	if (!common_data)
		return -ENOMEM;

	common_data->common_page = NULL;
	common_data->common_ptr = NULL;

	filp->private_data = common_data;

	return 0;
}

static int ntrdma_file_release(struct inode *inode, struct file *filp)
{
	struct ntrdma_common_data *common_data = filp->private_data;

	if (common_data->common_page)
		put_page(common_data->common_page);

	kfree(common_data);

	return 0;
}

static long ntrdma_file_ioctl(struct file *filp, unsigned int cmd,
			unsigned long arg)
{
	struct ntrdma_common_data *common_data = filp->private_data;
	u8 data[64];
	int i;
	int rc;

	switch (cmd) {
	case NTRDMA_FILEIOCTL_REG:
		if (common_data->common_page)
			return -EINVAL;
		rc = get_user_pages_fast(arg, 1, 1, &common_data->common_page);
		if (rc >= 0)
			common_data->common_ptr =
				page_address(common_data->common_page);
		return rc;
	case NTRDMA_FILEIOCTL_SEND:
		if (!common_data->common_page)
			return -EINVAL;
		memcpy(data, (void *)common_data->common_ptr, sizeof(data));
		for (i = 0; i < sizeof(data); i++)
			data[i]++;
		memcpy((void *)common_data->common_ptr + 1024,
			data, sizeof(data));
		return 0;
	default:
		return -EINVAL;
	}
	return 0;
}

static ssize_t show_measure_perf(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	return sprintf(buf, "%d\n", ntrdma_measure_perf ? 1 : 0);
}

static ssize_t write_measure_perf(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	int rc;
	int val = 0;

	rc = sscanf(buf, "%i", &val);
	if (rc != 1)
		return -EINVAL;

	ntrdma_measure_perf = !!val;

	return count;
}

static ssize_t show_print_debug(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	return sprintf(buf, "%d\n", ntrdma_print_debug ? 1 : 0);
}

static ssize_t write_print_debug(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	int rc;
	int val = 0;

	rc = sscanf(buf, "%i", &val);
	if (rc != 1)
		return -EINVAL;

	ntrdma_print_debug = !!val;

	return count;
}

int __init ntrdma_file_register(void)
{
	int rc;

	rc = __register_chrdev(ntrdma_char_major, 0, NTRDMA_MINORS, "ntrdma",
			&ntrdma_file_ops);
	if (rc < 0)
		goto err_out;
	else {
		if (rc > 0)
			ntrdma_char_major = rc;

		ntrdma_chrdev_registered = true;
	}

	ntrdma_class = class_create(THIS_MODULE, "ntrdma");
	if (IS_ERR(ntrdma_class)) {
		rc = PTR_ERR(ntrdma_class);
		ntrdma_class = NULL;
		goto err_out;
	}

	ntrdma_dev = device_create_with_groups(ntrdma_class, NULL,
					MKDEV(ntrdma_char_major, 0),
					NULL, ntrdma_file_attrs_groups,
					"ntrdma");
	if (IS_ERR(ntrdma_dev)) {
		rc = PTR_ERR(ntrdma_dev);
		ntrdma_dev = NULL;
		goto err_out;
	}


	return 0;
 err_out:
	ntrdma_file_unregister();
	return rc;
}

void ntrdma_file_unregister(void)
{
	if (ntrdma_dev) {
		device_destroy(ntrdma_class, MKDEV(ntrdma_char_major, 0));
		ntrdma_dev = NULL;
	}

	if (ntrdma_class) {
		class_destroy(ntrdma_class);
		ntrdma_class = NULL;
	}

	if (ntrdma_chrdev_registered) {
		__unregister_chrdev(ntrdma_char_major, 0, NTRDMA_MINORS,
				"ntrdma");
		ntrdma_chrdev_registered = false;
	}
}
