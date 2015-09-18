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

#ifndef NTRDMA_DMA_H
#define NTRDMA_DMA_H

#include "ntrdma_dev.h"
#include "ntrdma_ntb.h"
#include "ntrdma_sg.h"

#include "ntrdma_dma_impl.h"

#ifdef CONFIG_NTRDMA_IMM_BUF
#define NTRDMA_DMA_IMM_SIZE		0x80
#endif

struct ntrdma_ntb_dev;
struct ntrdma_dma_req;
struct ntrdma_sge_iter;

struct ntrdma_dma_req {
	struct ntrdma_req		req;
	NTRDMA_DECL_LIST_ENTRY		(entry);

	/* The next memcpy sge not yet added to req_impl */
	int				next_valid;
	struct ntrdma_dma_sge		next_sge;

	/* The dma_impl request, where sge are added */
	void				*req_impl;

#ifdef CONFIG_NTRDMA_IMM_BUF
	/* Small buffer for immediate data */
	ntrdma_u8_t			*imm_buf;
	ntrdma_dma_addr_t		imm_dma;
	ntrdma_size_t			imm_size;
	ntrdma_size_t			imm_off;
#endif

	/* Callbacks */
	NTRDMA_DECL_LIST_HEAD		(cb_list);

	int dma_count;
};

#define req_dreq(__req) NTRDMA_CONTAINER_OF(__req, struct ntrdma_dma_req, req)

/* initialize the dma subcomponent */
int ntrdma_dma_init(struct ntrdma_ntb_dev *dev);
void ntrdma_dma_deinit(struct ntrdma_ntb_dev *dev);

/* Free a dma request without submitting it */
void ntrdma_dma_req_free(struct ntrdma_dma_req *req);

/* Submit a dma request which will be freed on completion */
void ntrdma_dma_req_submit(struct ntrdma_dma_req *req);

/* Allocate a dma request */
struct ntrdma_dma_req *ntrdma_dma_req_alloc(struct ntrdma_ntb_dev *dev,
					    int hint);

/* Memcpy from a source to destination dma address */
void ntrdma_dma_req_memcpy(struct ntrdma_dma_req *req,
			   ntrdma_dma_addr_t dst,
			   ntrdma_dma_addr_t src,
			   ntrdma_size_t len);

/* Write a small amount of immediate data */
void ntrdma_dma_req_imm32(struct ntrdma_dma_req *req,
			  ntrdma_dma_addr_t dst,
			  ntrdma_u32_t val);

/* Write a small amount of immediate data */
void ntrdma_dma_req_imm64(struct ntrdma_dma_req *req,
			  ntrdma_dma_addr_t dst,
			  ntrdma_u64_t val);

/* Add a fence between dependent memcpy operations. */
void ntrdma_dma_req_fence(struct ntrdma_dma_req *req);

/* Add a dma completion callback to the list for this request. */
void ntrdma_dma_req_callback(struct ntrdma_dma_req *req,
			     struct ntrdma_dma_cb *cb);

/* Throttle dma requests if there are many are outstanding. */
bool ntrdma_dma_throttle(struct ntrdma_ntb_dev *dev,
			 struct ntrdma_dma_cb *cb);

#endif
