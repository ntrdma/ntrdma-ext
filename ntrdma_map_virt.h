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

#ifndef NTRDMA_MAP_VIRT_H
#define NTRDMA_MAP_VIRT_H

#include <linux/scatterlist.h>
#include <linux/vmalloc.h>

#include "ntrdma_mem.h"
#include "ntrdma_ib.h"
#include "ntrdma_sg.h"

#define NTRDMA_DMA_TO_DEVICE		0
#define NTRDMA_DMA_FROM_DEVICE		0
#define NTRDMA_DMA_BIDIRECTIONAL	0

static inline
ntrdma_dma_addr_t ntrdma_dma_map(void *dev_impl, void *ptr,
				 ntrdma_size_t size, int dir)
{
	return (unsigned long)ptr;
}

static inline
void ntrdma_dma_unmap(void *dev_impl, ntrdma_dma_addr_t dma,
		      ntrdma_size_t size, int dir)
{
}

static inline
void ntrdma_dma_sync_for_cpu(void *dev_impl, ntrdma_dma_addr_t dma,
			     ntrdma_size_t size, int dir)
{
}

static inline
void ntrdma_dma_sync_for_device(void *dev_impl, ntrdma_dma_addr_t dma,
				ntrdma_size_t size, int dir)
{
}

static inline
void *ntrdma_malloc_coherent(void *dev_impl, ntrdma_size_t size,
			     ntrdma_dma_addr_t *dma, int node)
{
	void *ptr;

	ptr = ntrdma_malloc(size, node);
	*dma = (unsigned long)ptr;

	return ptr;
}

static inline
void ntrdma_free_coherent(void *dev_impl, ntrdma_size_t size,
			  void *ptr, ntrdma_dma_addr_t dma)
{
	ntrdma_free(ptr);
}

struct ntrdma_vmem {
	void *addr;
	size_t size;
	void *kvaddr;
	unsigned long npages;
	struct page *pages[];
};

static inline
void *ntrdma_umem_get(struct ib_ucontext *ibctx, unsigned long addr,
		      ntrdma_size_t size, int access, int dmasync)
{
	struct ntrdma_vmem *vmem;
	unsigned long npages, locked, lock_limit;
	int readonly, ret, i;

	if (!size)
		return ERR_PTR(-EINVAL);

	if (addr + size < addr || PAGE_ALIGN(addr + size) < addr + size)
		return ERR_PTR(-EINVAL);

	if (!can_do_mlock())
		return ERR_PTR(-EPERM);

	npages = (PAGE_ALIGN(addr + size) - PAGE_ALIGN(addr)) >> PAGE_SHIFT;
	if (!npages)
		return ERR_PTR(-EINVAL);

	readonly = !(access &
		     (IB_ACCESS_LOCAL_WRITE   | IB_ACCESS_REMOTE_WRITE |
		      IB_ACCESS_REMOTE_ATOMIC | IB_ACCESS_MW_BIND));

	vmem = kmalloc(sizeof(*vmem) + npages * sizeof(*vmem->pages), GFP_KERNEL);
	if (!vmem)
		return ERR_PTR(-ENOMEM);

	vmem->npages = npages;

	down_write(&current->mm->mmap_sem);

	locked     = npages + current->mm->pinned_vm;
	lock_limit = rlimit(RLIMIT_MEMLOCK) >> PAGE_SHIFT;

	if ((locked > lock_limit) && !capable(CAP_IPC_LOCK)) {
		ret = -ENOMEM;
		goto err_pages;
	}

	ret = get_user_pages(current, current->mm, addr & PAGE_MASK,
			     npages, 1, readonly, vmem->pages, NULL);
	up_write(&current->mm->mmap_sem);
	if (ret < 0)
		goto err_pages;

	vmem->kvaddr = vmap(vmem->pages, npages, VM_MAP, PAGE_KERNEL);
	if (!vmem->kvaddr) {
		ret = -ENOMEM;
		goto err_vmap;
	}

	vmem->addr = vmem->kvaddr + (addr & ~PAGE_MASK);
	vmem->size = size;

	return vmem;

err_vmap:
	for (i = 0; i < npages; ++i)
		put_page(vmem->pages[i]);
err_pages:
	ntrdma_free(vmem);
	return ERR_PTR(ret);
}

static inline
void ntrdma_umem_put(void *umem)
{
	struct ntrdma_vmem *vmem = umem;
	int i, npages = vmem->npages;

	vunmap(vmem->kvaddr);
	for (i = 0; i < npages; ++i)
		put_page(vmem->pages[i]);
	ntrdma_free(vmem);
}

static inline
int ntrdma_umem_sg_count(void *umem)
{
	return 1;
}

static inline
int ntrdma_umem_sg_init(void *umem,
			struct ntrdma_mr_sge *sg_list,
			int sg_count)
{
	struct ntrdma_vmem *vmem = umem;

	if (sg_count < 1)
		return -EINVAL;

	sg_list[0].dma = (unsigned long)vmem->addr;
	sg_list[0].len = (unsigned long)vmem->size;

	return 1;
}

#endif
