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

/* minimal ntb/dma/ib apis to be implemented by wrapper or simulator */
#include "ntrdma_ntb_impl.h"
#include "ntrdma_dma_impl.h"
#include "ntrdma_ib_impl.h"
#include "ntrdma_sg.h"

#include <linux/pci.h>
#include <linux/ntb.h>

void ntrdma_dma_impl_submit_req(void *dma_req)
{
}

void ntrdma_dma_impl_flush(void *dma_impl)
{
}


void *ntrdma_dma_impl_alloc_req(void *dma_impl,
				void (*cb)(void *cb_ctx, int status),
				void *cb_ctx,
				int cap_hint)
{
	return NULL;
}

int ntrdma_dma_impl_req_append_one(void *dma_req,
				   ntrdma_dma_addr_t dst, ntrdma_dma_addr_t src,
				   ntrdma_size_t len, unsigned long flags)
{
	return 0;
}

void ntrdma_dma_impl_free_req(void *dma_req)
{
}

void ntrdma_ntb_impl_msi_dma(void *ntb_impl, 
			     ntrdma_dma_addr_t *addr,
			     ntrdma_u32_t *val)
{
}

int ntrdma_umem_sgl_count(struct ib_umem *umem)
{
	return 1;
}

int ntrdma_umem_sgl_init(struct ib_umem *umem, struct ntrdma_mr_sge *sgl, int count)
{
	return 1;
}
