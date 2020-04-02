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

#ifndef NTRDMA_ZIP_H
#define NTRDMA_ZIP_H

#include "ntrdma_sg.h"

struct ntrdma_pd;
struct ntrdma_rpd;

/* Append dma requests from snd entries in pd to rcv entries in rpd */
/* Called on snd side. */
int ntrdma_zip_rdma(struct ntrdma_dev *dev, struct ntc_dma_chan *chan,
		u32 *rdma_len,
		const struct ntrdma_wr_rcv_sge *rcv_sg_list,
		const struct ib_sge *snd_sg_list,
		u32 rcv_sg_count, u32 snd_sg_count, u32 rcv_start_offset,
		u64 wrid, int dma_wait);

int ntrdma_zip_rdma_imm(struct ntrdma_dev *dev, struct ntc_dma_chan *chan,
			const struct ntrdma_wr_rcv_sge *rcv_sg_list,
			const void *snd_data,
			u32 rcv_sg_count, u32 snd_data_size,
			u32 rcv_start_offset, u64 wrid);

/* Sync incoming data to dst entries in pd for the cpu */
/* Called on rcv side. */
int ntrdma_zip_sync(struct ntrdma_dev *dev,
		struct ntrdma_wr_rcv_sge *lrcv_sg_list, u32 lrcv_sg_count);

int ntrdma_zip_memcpy(struct ntrdma_dev *dev, u32 target_key, u64 target_addr,
		void *src, u64 size);

#endif

