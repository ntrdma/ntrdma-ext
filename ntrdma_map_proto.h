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

#ifndef NTRDMA_MAP_PROTO_H
#define NTRDMA_MAP_PROTO_H

#ifdef NTRDMA_MAP_DEFINED
#error "there can be only one"
#else
#define NTRDMA_MAP_DEFINED
#endif

#include "ntrdma_os.h"

/* io mapping */
#define __iomem
void __iomem *ntrdma_io_map(ntrdma_phys_addr_t phys, ntrdma_size_t size);
void ntrdma_io_unmap(void __iomem *ptr);
ntrdma_u32_t ntrdma_io_read32(void __iomem *ptr);
void ntrdma_io_write32(void __iomem *ptr, ntrdma_u32_t val);
void ntrdma_io_write64(void __iomem *ptr, ntrdma_u64_t val);

/* dma mapping */
#define NTRDMA_DMA_TO_DEVICE		1
#define NTRDMA_DMA_FROM_DEVICE		2
#define NTRDMA_DMA_BIDIRECTIONAL	3
ntrdma_dma_addr_t ntrdma_dma_map(void *dev_impl, void *ptr,
				 ntrdma_size_t size, int dir);
void ntrdma_dma_unmap(void *dev_impl, ntrdma_dma_addr_t dma,
		      ntrdma_size_t size, int dir);
void ntrdma_dma_sync_for_cpu(void *dev_impl, ntrdma_dma_addr_t dma,
			     ntrdma_size_t size, int dir);
void ntrdma_dma_sync_for_device(void *dev_impl, ntrdma_dma_addr_t dma,
				ntrdma_size_t size, int dir);

/* dma coherent allocation */
void *ntrdma_malloc_coherent(void *dev_impl, ntrdma_size_t size,
			     ntrdma_dma_addr_t *dma, int node);
void ntrdma_free_coherent(void *dev_impl, ntrdma_size_t size,
			  void *ptr, ntrdma_dma_addr_t dma);

/* infiniband user memory mapping */
struct ntrdma_mr_sge;
struct ib_ucontext;

void *ntrdma_umem_get(struct ib_ucontext *ibctx,
		      unsigned long addr, ntrdma_size_t size,
		      int access, int dmasync);
void ntrdma_umem_put(void *umem);
int ntrdma_umem_sg_count(void *umem);
int ntrdma_umem_sg_init(void *umem, struct ntrdma_mr_sge *sgl, int cap);

#endif
