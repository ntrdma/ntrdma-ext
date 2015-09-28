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

#include "ntrdma.h"

struct ntrdma_obj {
	/* The ntrdma device to which this object belongs */
	struct ntrdma_dev		*dev;
	/* The entry in the device list for this type of object */
	struct list_head		dev_entry;

	/* The number of reference holders of this object */
	unsigned int			ref_count;
	/* Broadcast if the ref count reaches zero */
	wait_queue_head_t		ref_cond;
	/* Synchronize access to ref count */
	spinlock_t			ref_lock;
};

#define ntrdma_obj_dev(obj) ((obj)->dev)
#define ntrdma_obj_dev_entry(obj) ((obj)->dev_entry)

static inline int ntrdma_obj_init(struct ntrdma_obj *obj,
				  struct ntrdma_dev *dev)
{
	obj->dev = dev;
	obj->ref_count = 0;
	init_waitqueue_head(&obj->ref_cond);
	spin_lock_init(&obj->ref_lock);

	return 0;
}

static inline void ntrdma_obj_deinit(struct ntrdma_obj *obj)
{
}

/* Claim a reference to the object */
static inline void ntrdma_obj_get(struct ntrdma_obj *obj)
{
	spin_lock_bh(&obj->ref_lock);
	{
		++obj->ref_count;
	}
	spin_unlock_bh(&obj->ref_lock);
}

/* Relinquish a reference to the object */
static inline void ntrdma_obj_put(struct ntrdma_obj *obj)
{
	spin_lock_bh(&obj->ref_lock);
	{
		if (!--obj->ref_count)
			wake_up_all(&obj->ref_cond);
	}
	spin_unlock_bh(&obj->ref_lock);
}

/* Repossess an object.  Wait for references to be relinquished. */
static inline void ntrdma_obj_repo(struct ntrdma_obj *obj)
{
	spin_lock_bh(&obj->ref_lock);
	{
		wait_event_cmd(obj->ref_cond, !obj->ref_count,
			       spin_unlock_bh(&obj->ref_lock),
			       spin_lock_bh(&obj->ref_lock));
	}
	spin_unlock_bh(&obj->ref_lock);
}

#endif
