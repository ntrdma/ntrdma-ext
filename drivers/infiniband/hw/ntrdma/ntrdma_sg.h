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

#ifndef NTRDEV_SG_H
#define NTRDEV_SG_H

#include <linux/err.h>
#include <linux/types.h>

struct ntrdma_wr_sge {
	u64				addr;
	u32				len;
	u32				key;
};

static inline int ntrdma_wr_sg_list_find(struct ntrdma_wr_sge *sg_list,
					 u32 sg_count,
					 u64 sg_off,
					 u32 *sg_pos,
					 u64 *sg_rem)
{
	u32 i = 0;

	for (i = 0; ; ++i) {
		if (i >= sg_count)
			return -EINVAL;

		if (sg_off < sg_list[i].len)
			break;

		sg_off -= sg_list[i].len;
	}

	*sg_pos = i;
	*sg_rem = sg_off;

	return 0;
}

static inline int ntrdma_wr_sg_list_len(struct ntrdma_wr_sge *sg_list,
					u32 sg_count,
					u32 *len)
{
	u32 i, cur, acc = 0;

	for (i = 0; i < sg_count; ++i) {
		cur = acc + sg_list[i].len;
		if (cur < acc)
			return -EINVAL;
		acc = cur;
	}

	*len = acc;

	return 0;
}

static inline int ntrdma_wr_sg_list_trunc(struct ntrdma_wr_sge *sg_list,
					  u32 sg_count,
					  u32 len)
{
	u32 i, cur, acc = 0;

	for (i = 0; acc < len && i < sg_count; ++i) {
		cur = acc + sg_list[i].len;
		if (cur < acc)
			return -EINVAL;
		acc = cur;
	}

	if (acc < len)
		return -EINVAL;

	if (acc > len)
		sg_list[i - 1].len -= acc - len;

	return i;
}

#endif
