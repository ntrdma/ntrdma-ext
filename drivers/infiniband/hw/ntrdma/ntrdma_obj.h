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

#ifndef NTRDMA_OBJ_H
#define NTRDMA_OBJ_H

#include <linux/list.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/kref.h>

#include "ntrdma.h"

struct ntrdma_obj {
	/* The ntrdma device to which this object belongs */
	struct ntrdma_dev		*dev;
	/* The entry in the device list for this type of object */
	struct list_head		dev_entry;
	struct kref kref;
};

#define ntrdma_obj_dev(obj) ((obj)->dev)
#define ntrdma_obj_dev_entry(obj) ((obj)->dev_entry)

/* Claim a reference to the object */
static inline void ntrdma_obj_get(struct ntrdma_obj *obj)
{
	int ret = kref_get_unless_zero(&obj->kref);

	WARN(!ret, "Should not happen ref is zero for obj %p", obj);
}

/* Relinquish a reference to the object */
static inline void ntrdma_obj_put(struct ntrdma_obj *obj,
		void (*obj_release)(struct kref *kref))
{
	kref_put(&obj->kref, obj_release);
}

static inline void ntrdma_obj_init(struct ntrdma_obj *obj,
				  struct ntrdma_dev *dev)
{
	obj->dev = dev;
	kref_init(&obj->kref);
}

#endif
