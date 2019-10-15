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

#ifndef NTRDMA_SG_H
#define NTRDMA_SG_H

#include <linux/ntc.h>
#include <linux/err.h>
#include <linux/types.h>

#include "ntrdma.h"

struct ntrdma_wr_snd_sge {
	u32				key;
	union {
		/* key != NTRDMA_RESERVED_DMA_LEKY */
		struct {
			u64		addr;
			u32		len;
		};
		/* key == NTRDMA_RESERVED_DMA_LEKY */
		struct ntc_local_buf	snd_dma_buf;
	};
};

struct ntrdma_wr_rcv_sge_shadow {
	u32				local_key;
	union {
		/* local_key != NTRDMA_RESERVED_DMA_LEKY */
		u64			local_addr;
		/* local_key == NTRDMA_RESERVED_DMA_LEKY */
		struct ntc_local_buf	rcv_dma_buf;
	};
	struct ntc_export_buf		exp_buf;
};

struct ntrdma_wr_rcv_sge {
	struct ntrdma_wr_rcv_sge_shadow *shadow;
	union {
		/* shadow == NULL */
		/* sge.lkey != NTRDMA_RESERVED_DMA_LEKY */
		struct ib_sge sge;
		/* shadow != NULL: the desc of shadow->exp_buf */
		struct ntc_remote_buf_desc exp_buf_desc;
	};
};

static inline bool ntrdma_ib_sge_reserved(const struct ib_sge *sge)
{
	return sge->lkey == NTRDMA_RESERVED_DMA_LEKY;
}

static inline u64 ntrdma_wr_rcv_sge_len(const struct ntrdma_wr_rcv_sge *sge)
{
	if (!sge->shadow)
		return sge->sge.length;
	else
		return sge->exp_buf_desc.size;
}

static inline
u32 ntrdma_wr_rcv_sge_remote_key(const struct ntrdma_wr_rcv_sge *sge)
{
	if (!sge->shadow)
		return sge->sge.lkey;
	else
		return NTRDMA_RESERVED_DMA_LEKY;
}

struct ntrdma_wr_rcv_sge_shadow *
ntrdma_zalloc_sge_shadow(gfp_t gfp, struct ntrdma_dev *dev);
void ntrdma_free_sge_shadow(struct ntrdma_wr_rcv_sge_shadow *shadow);

#endif
