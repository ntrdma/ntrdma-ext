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

#ifndef NTRDMA_MAP_PHYS_H
#define NTRDMA_MAP_PHYS_H

#ifdef NTRDMA_MAP_DEFINED
#error "there can be only one"
#else
#define NTRDMA_MAP_DEFINED
#endif

#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>

#include "ntrdma_ib.h"
#include "ntrdma_sg.h"

#define ntrdma_io_map(phys, size)	ioremap(phys, size)
#define ntrdma_io_unmap(ptr)		iounmap(ptr)
#define ntrdma_io_read32(ptr)		ioread32(ptr)
#define ntrdma_io_read64(ptr)		ioread64(ptr)
#define ntrdma_io_write32(ptr, val)	iowrite32(val, ptr)
#define ntrdma_io_write64(ptr, val)	iowrite64(val, ptr)

#ifndef ioread64
#ifdef readq
#define ioread64 readq
#else
#define ioread64 __lame_ioread64
static inline u64 __lame_ioread64(const void __iomem* ptr)
{
	return (u64)ioread32(ptr) | (u64)ioread32(ptr + sizeof(u32))
}
#endif
#endif

#ifndef iowrite64
#ifdef writeq
#define iowrite64 writeq
#else
#define iowrite64 __lame_iowrite64
static inline void __lame_iowrite64(u64 val, void __iomem *ptr)
{
	iowrite32(val, ptr);
	iowrite32(val >> 32, ptr + sizeof(u32));
}
#endif
#endif

#define NTRDMA_DMA_TO_DEVICE		DMA_TO_DEVICE
#define NTRDMA_DMA_FROM_DEVICE		DMA_FROM_DEVICE
#define NTRDMA_DMA_BIDIRECTIONAL	DMA_BIDIRECTIONAL
#define ntrdma_dma_map(dev_impl, ptr, size, dir) \
	dma_map_single(dev_impl, ptr, size, dir)
#define ntrdma_dma_unmap(dev_impl, dma, size, dir) \
	dma_unmap_single(dev_impl, dma, size, dir)
#define ntrdma_dma_sync_for_cpu(dev_impl, dma, size, dir) \
	dma_sync_single_for_cpu(dev_impl, dma, size, dir)
#define ntrdma_dma_sync_for_device(dev_impl, dma, size, dir) \
	dma_sync_single_for_cpu(dev_impl, dma, size, dir)

#define ntrdma_malloc_coherent(dev_impl, size, dma, node) \
	dma_alloc_coherent(dev_impl, size, dma, GFP_ATOMIC)
#define ntrdma_free_coherent(dev_impl, size, ptr, dma) \
	dma_free_coherent(dev_impl, size, ptr, dma)

#define ntrdma_umem_get			ib_umem_get
#define ntrdma_umem_put			ib_umem_release

#ifdef CONFIG_NTRDMA_UMEM_HAS_CHUNKS

#define NTRDMA_UMEM_EXTRA_DECL \
	struct ib_umem_chunk *chunk;

#define NTRDMA_UMEM_FOR_EACH \
	list_for_each_entry(chunk, &ibmem->chunk_list, list)	\
	for_each_sg(chunk->page_list, sg, chunk->nents, i)
#else

#define NTRDMA_UMEM_EXTRA_DECL

#define NTRDMA_UMEM_FOR_EACH \
	for_each_sg(ibmem->sg_head.sgl, sg, ibmem->sg_head.nents, i)
#endif

#define NTRDMA_UMEM_DMA_ADDR_LEN \
	dma_addr = sg_dma_address(sg);				\
	dma_len = sg_dma_len(sg);				\
	for (;;) {						\
		next = sg_next(sg);				\
		if (!next)					\
		break;						\
		if (sg_dma_address(next) != dma_addr + dma_len)	\
		break;						\
		dma_len += sg_dma_len(next);			\
		sg = next;					\
		++i;						\
	}

static inline int ntrdma_umem_sg_count(void *umem)
{
	struct ib_umem *ibmem = umem;
	NTRDMA_UMEM_EXTRA_DECL
	struct scatterlist *sg, *next;
	int i, dma_count = 0;
	dma_addr_t dma_addr;
	size_t dma_len;

	NTRDMA_UMEM_FOR_EACH {
		NTRDMA_UMEM_DMA_ADDR_LEN
		++dma_count;
	}

	return dma_count;
}

static inline int ntrdma_umem_sg_init(void *umem,
				      struct ntrdma_mr_sge *sg_list,
				      int sg_count)
{
	struct ib_umem *ibmem = umem;
	NTRDMA_UMEM_EXTRA_DECL
	struct scatterlist *sg, *next;
	int i, dma_count = 0;
	dma_addr_t dma_addr;
	size_t dma_len;

	NTRDMA_UMEM_FOR_EACH {
		if (dma_count >= sg_count)
			return -EINVAL;
		NTRDMA_UMEM_DMA_ADDR_LEN
		sg_list[dma_count].dma = dma_addr;
		sg_list[dma_count].len = dma_len;
		++dma_count;
	}

	return dma_count;
}

#endif
