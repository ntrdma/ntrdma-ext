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

#include <linux/slab.h>

#include "ntrdma_dev.h"
#include "ntrdma_cmd.h"
#include "ntrdma_res.h"
#include "ntrdma_pd.h"

int ntrdma_pd_init(struct ntrdma_pd *pd, struct ntrdma_dev *dev, u32 key)
{
	pd->key = key;

	return ntrdma_obj_init(&pd->obj, dev);
}

void ntrdma_pd_deinit(struct ntrdma_pd *pd)
{
	ntrdma_obj_deinit(&pd->obj);
}

int ntrdma_pd_add(struct ntrdma_pd *pd)
{
	struct ntrdma_dev *dev = ntrdma_pd_dev(pd);

	ntrdma_pd_get(pd);

	mutex_lock(&dev->res_lock);
	{
		list_add_tail(&pd->obj.dev_entry, &dev->pd_list);
	}
	mutex_unlock(&dev->res_lock);

	return 0;
}

void ntrdma_pd_del(struct ntrdma_pd *pd)
{
	struct ntrdma_dev *dev = ntrdma_pd_dev(pd);

	mutex_lock(&dev->res_lock);
	{
		list_del(&pd->obj.dev_entry);
	}
	mutex_unlock(&dev->res_lock);

	ntrdma_pd_put(pd);
	ntrdma_pd_repo(pd);
}
