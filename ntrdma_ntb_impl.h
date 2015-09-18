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

#ifndef NTRDMA_NTB_IMPL_H
#define NTRDMA_NTB_IMPL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "ntrdma_os.h"

struct ntb_dev;

/* Callbacks from ntb impl to ntb client (ntrdma):
 *     link_event   - notify of a change in link status
 *     db_event     - notify of a doorbell interrupt received
 */
struct ntrdma_ntb_impl_ops {
	void				(*link_event)(void *ctx);
	void				(*db_event)(void *ctx);
};

/* get numa node closest to the ntb device */
int ntrdma_ntb_impl_node(void *ntb_impl);

/* get internal os or sim device handle to assign to ib_device->dma_device */
void *ntrdma_ntb_impl_get_device(void *ntb_impl);

/* get the context (rdma device) for this ntb device */
void *ntrdma_ntb_impl_get_ctx(void *ntb_impl);

/* register context (a new rdma device) and callbacks for this ntb device */
void ntrdma_ntb_impl_set_ctx(void *ntb_impl, void *ctx,
			     const struct ntrdma_ntb_impl_ops *ops);

/* enable link training */
void ntrdma_ntb_link_enable(void *ntb_impl);

/* reset and hold down the link until further notice */
void ntrdma_ntb_link_disable(void *ntb_impl);

/* get the current link status, width, and speed, return true if the link is up
 * AND it is done training.
 *
 * NOTE: impl may exit training before reaching target link and speed.  For
 * instance, if there is a problem training the link, the ntb impl can leave
 * the link at a lower width and/or speed, exit the training state machine, and
 * report that the link is up.  The output parameters here will indicate the
 * final link width and speed, regardless of the target.
 *
 * NOTE: caller can observe the width and speed while the link is training.  It
 * is therefore recommended that ntb impl call back to indicate a link event
 * for any change in link status, including width and speed, and always assign
 * good values to the output parameters (if provided by the caller).  While the
 * link is training, this should return false (and assign the output
 * parameters).  When the link is stopped training, and the link us up, this
 * should return true (and assign the output parameters).
 */
int ntrdma_ntb_impl_link_is_up(void *ntb_impl);

/* read the local ntb scratchpad register */
ntrdma_u32_t ntrdma_ntb_impl_spad_read(void *ntb_impl, int idx);

/* write the local ntb scratchpad register */
void ntrdma_ntb_impl_spad_write(void *ntb_impl, int idx,
				ntrdma_u32_t val);

/* read the peer ntb scratchpad register */
ntrdma_u32_t ntrdma_ntb_impl_peer_spad_read(void *ntb_impl, int idx);

/* write the peer ntb scratchpad register */
void ntrdma_ntb_impl_peer_spad_write(void *ntb_impl, int idx,
				     ntrdma_u32_t val);

/* get the dma address of the start of peer dram */
ntrdma_dma_addr_t ntrdma_ntb_impl_peer_dram_dma(void *ntb_impl);

/* get the dma address of the start of peer msi */
ntrdma_dma_addr_t ntrdma_ntb_impl_peer_msi_dma(void *ntb_impl);

/* get the dma address and value for the "doorbell" msi interrupt register. */
void ntrdma_ntb_impl_msi_dma(void *ntb_impl,
			     ntrdma_dma_addr_t *addr,
			     ntrdma_u32_t *val);

/* get the dma address of the peer ntb scratchpad register */
ntrdma_dma_addr_t ntrdma_ntb_impl_peer_spad_dma(void *ntb_impl, int idx);

#ifdef __cplusplus
}
#endif

#endif
