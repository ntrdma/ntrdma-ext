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

#ifndef NTRDMA_DMA_IMPL_H
#define NTRDMA_DMA_IMPL_H

#ifdef __cplusplus
extern "C" {
#endif

/* NTRDMA DMA minimal sub-interface to underlying DMA implementation */

#include "ntrdma_os.h"

/* Dma completion callback status codes */
#define NTRDMA_DMA_SUCCESS		0
#define NTRDMA_DMA_ERROR		1
#define NTRDMA_DMA_ABORTED		2
#define NTRDMA_DMA_TIMEOUT		3

/* Parameters for one dma memcpy operation */
struct ntrdma_dma_sge {
	ntrdma_dma_addr_t	dst;
	ntrdma_dma_addr_t	src;
	ntrdma_size_t		len;
	unsigned long		flags;
};

#define NTRDMA_DMA_REQ_FENCE		1

/* Stop dma and abort all pending requests that have not started */
void ntrdma_dma_impl_abort_all(void *dma_impl);

/* Allocate a dma request, to be initialized and then submitted
 *     (returns simple impl-defined req, not ntrdma_dma_req decorator) */
void *ntrdma_dma_impl_alloc_req(void *dma_impl,
				void (*cb)(void *cb_ctx),
				void *cb_ctx,
				int cap_hint);

/* Append a number of copy operations to the request
 *     (impl need not coalesce sges, will be handled by decorator) */
int ntrdma_dma_impl_req_append(void *dma_req,
			       struct ntrdma_dma_sge *sgl,
			       int count);

/* Append one copy operation to the request
 *     (impl need not coalesce sges, will be handled by decorator) */
int ntrdma_dma_impl_req_append_one(void *dma_req,
				   ntrdma_dma_addr_t dst,
				   ntrdma_dma_addr_t src,
				   ntrdma_size_t len,
				   unsigned long flags);

#ifndef CONFIG_NTRDMA_IMM_BUF
/* Append one immediate data operation to the request */
int ntrdma_dma_impl_req_append_imm(void *dma_req,
				   ntrdma_dma_addr_t dst,
				   void *src,
				   ntrdma_size_t len,
				   unsigned long flags);
#endif

static inline int ntrdma_dma_impl_append_sgl(void *dma_req,
					     struct ntrdma_dma_sge *sgl,
					     int count)
{
	return ntrdma_dma_impl_req_append(dma_req, sgl, count);
}

static inline int ntrdma_dma_impl_append_sge(void *dma_req,
					     struct ntrdma_dma_sge *sge)
{
	return ntrdma_dma_impl_req_append_one(dma_req,
					      sge->dst,
					      sge->src,
					      sge->len,
					      sge->flags);
}

/* Submit a dma request for processing */
void ntrdma_dma_impl_submit_req(void *dma_req);

static inline void ntrdma_dma_impl_submit(void *dma_impl,
					  void *dma_req)
{
	ntrdma_dma_impl_submit_req(dma_req);
}

/* Dealloc a dma request without submitting */
void ntrdma_dma_impl_free_req(void *dma_req);

static inline void ntrdma_dma_impl_free(void *dma_impl,
					void *dma_req)
{
	ntrdma_dma_impl_free_req(dma_req);
}

#ifdef __cplusplus
}
#endif

#endif
