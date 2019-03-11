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

#ifndef NTRDMA_DEV_H
#define NTRDMA_DEV_H

#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/types.h>

#include <linux/ntc.h>

#include <rdma/ib_verbs.h>

#include "ntrdma.h"
#include "ntrdma_cb.h"
#include "ntrdma_util.h"
#include "ntrdma_vbell.h"

struct vbell_work_data_s {
	struct ntrdma_dev *dev;
	int vec;
};

/* RDMA over PCIe NTB device */
struct ntrdma_dev {
	/* NOTE: .ibdev MUST be the first thing in ntrdma_dev!
	 *     (required by ib_alloc_device and ib_dealloc_device) */
	struct ib_device		ibdev;

	/* NUMA node */
	int				node;

	/* The non-transparent channel back end implementation */
	struct ntc_dev			*ntc;

	/* debugfs */
	struct dentry			*debug;

	/* protocol version */
	u32				version;

	/* virtual doorbells synchronization */
	int				vbell_enable;
	struct tasklet_struct		vbell_work[NTB_MAX_IRQS];
	struct vbell_work_data_s	vbell_work_data[NTB_MAX_IRQS];
	spinlock_t			vbell_next_lock;
	spinlock_t			vbell_self_lock;
	spinlock_t			vbell_peer_lock;

	/* local virtual doorbells */

	u32				vbell_count;
	u32				vbell_start;
	u32				vbell_next;
	struct ntrdma_vbell_head	*vbell_vec;
	u32				*vbell_buf;
	size_t				vbell_buf_size;
	u64				vbell_buf_addr;

	/* peer virtual doorbells */

#ifdef CONFIG_NTRDMA_VBELL_USE_SEQ
	u32				*vbell_peer_seq;
#endif
	u64				peer_vbell_buf_addr;
	int				peer_vbell_count;

	/* commands to affect remote resources */

	bool				cmd_ready;

	/* command recv work */
	struct mutex			cmd_recv_lock;
	struct work_struct		cmd_recv_work;
	struct ntrdma_vbell		cmd_recv_vbell;
	u32				cmd_recv_vbell_idx;

	/* command recv ring indices */
	u32				cmd_recv_cap;
	u32				cmd_recv_cons;

	/* command recv ring buffers and producer index */
	union ntrdma_cmd		*cmd_recv_buf;
	u32				*cmd_recv_prod_buf;
	u64				cmd_recv_buf_addr;
	size_t				cmd_recv_buf_size;
	union ntrdma_rsp		*cmd_recv_rsp_buf;
	u64				cmd_recv_rsp_buf_addr;
	size_t				cmd_recv_rsp_buf_size;
	u64				peer_cmd_send_rsp_buf_addr;
	u64				peer_cmd_send_cons_addr;
	u32				peer_cmd_send_vbell_idx;

	/* command recv work */
	struct list_head		cmd_pend_list;
	struct list_head		cmd_post_list;
	wait_queue_head_t		cmd_send_cond;
	struct mutex			cmd_send_lock;
	struct work_struct		cmd_send_work;
	struct ntrdma_vbell		cmd_send_vbell;
	u32				cmd_send_vbell_idx;

	/* command send ring indices */
	u32				cmd_send_cap;
	u32				cmd_send_prod;
	u32				cmd_send_cmpl;

	/* command send ring buffers and consumer index */
	union ntrdma_cmd		*cmd_send_buf;
	u64				cmd_send_buf_addr;
	size_t				cmd_send_buf_size;
	union ntrdma_rsp		*cmd_send_rsp_buf;
	u32				*cmd_send_cons_buf;
	u64				cmd_send_rsp_buf_addr;
	size_t				cmd_send_rsp_buf_size;
	u64				peer_cmd_recv_buf_addr;
	u64				peer_cmd_recv_prod_addr;
	u32				peer_cmd_recv_vbell_idx;

	/* hello buffers */
	u8 *hello_local_buf;
	int hello_local_buf_size;
	u8 *hello_peer_buf;
	int hello_peer_buf_size;

	/* rdma resource synchronization state */

	int				res_enable;
	struct mutex			res_lock;
	struct mutex			rres_lock;

	/* local-only resources */

	struct list_head		cq_list;
	struct list_head		pd_list;
	u32				pd_next_key;

	/* rdma local resources */

	struct list_head		res_list;
	struct ntrdma_kvec		mr_vec;
	struct ntrdma_kvec		qp_vec;

	/* rdma remote resources */

	struct list_head		rres_list;
	struct ntrdma_vec		rmr_vec;
	struct ntrdma_vec		rqp_vec;

	/* virtual ethernet device */
	struct ntrdma_eth		*eth;
};

#define ntrdma_ib_dev(__ibdev) \
	container_of(__ibdev, struct ntrdma_dev, ibdev)

#define ntrdma_cmd_send_work_dev(__ws) \
	container_of(__ws, struct ntrdma_dev, cmd_send_work)
#define ntrdma_cmd_recv_work_dev(__ws) \
	container_of(__ws, struct ntrdma_dev, cmd_recv_work)

#define ntrdma_dbg(dev, args...) \
	dev_dbg(&(dev)->ntc->dev, ## args)

#define ntrdma_err(dev, args...) \
	dev_err(&(dev)->ntc->dev, ## args)

#define ntrdma_vdbg(dev, args...) \
	dev_vdbg(&(dev)->ntc->dev, ## args)

#endif
