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

#ifndef NTRDMA_MEM_LINUX_H
#define NTRDMA_MEM_LINUX_H

#ifdef NTRDMA_MEM_DEFINED
#error "there can be only one"
#else
#define NTRDMA_MEM_DEFINED
#endif

#ifdef __cplusplus
#error "linux api is not c++"
#endif

#include <linux/slab.h>

#define ntrdma_malloc(size, node) \
	kmalloc_node(size, GFP_KERNEL, node)
#define ntrdma_zalloc(size, node) \
	kzalloc_node(size, GFP_KERNEL, node)
#define ntrdma_free(ptr) \
	kfree(ptr)

#define ntrdma_malloc_atomic(size, node) \
	kmalloc_node(size, GFP_ATOMIC, node)
#define ntrdma_free_atomic(ptr) \
	kfree(ptr)

#endif
