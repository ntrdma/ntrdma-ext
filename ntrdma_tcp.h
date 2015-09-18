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

#ifndef NTRDMA_TCP_H
#define NTRDMA_TCP_H

#include <linux/mutex.h>
#include <linux/net.h>
#include <linux/workqueue.h>

#include "ntrdma_dev.h"
#include "ntrdma_res.h"

#define NTRDMA_TCP_CONN_DELAY		msecs_to_jiffies(2000)

#define NTRDMA_TCP_VBELL_COUNT		0x20
#define NTRDMA_TCP_CMD_CAP		0x20
#define NTRDMA_TCP_PP_CAP		0x20
#define NTRDMA_TCP_PP_LEN		PAGE_SIZE

#define NTRDMA_TCP_MSG_DATA		BIT(0)
#define NTRDMA_TCP_MSG_SIGNAL		BIT(1)
#define NTRDMA_TCP_MSG_IMM		BIT(2)

#define NTRDMA_TCP_INFO_TAG		U64_C(0x73c384cd2a6727db)
#define NTRDMA_TCP_MSG_TAG		U64_C(0xe4ee6eb8e653fa98)

struct ntrdma_tcp_dev {
	/* NOTE: .dev.ibdev MUST be the first thing in ntrdma_tcp_dev!
	 *      (required by ib_alloc_device and ib_dealloc_device) */
	struct ntrdma_dev		dev;

	struct task_struct		*task;
	int				(*task_fn)(void *ctx);

	struct socket			*sock;
	struct sockaddr			saddr;

	NTRDMA_DECL_LIST_HEAD		(req_list);
	NTRDMA_DECL_SPL			(req_lock);
	NTRDMA_DECL_DWI			(req_work);
};

#define dev_tdev(__dev) NTRDMA_CONTAINER_OF(__dev, struct ntrdma_tcp_dev, dev)

struct ntrdma_tcp_msg {
	ntrdma_u64_t			start_tag;

	ntrdma_u32_t			flags;
	ntrdma_u32_t			data_len;
	ntrdma_u64_t			data_dst;

	ntrdma_u64_t			end_tag;
};

struct ntrdma_tcp_init {
	ntrdma_u64_t			start_tag;

	/* virtual doorbell */
	ntrdma_u64_t			vbell_dma;
	ntrdma_u32_t			vbell_flags; /* TODO: bool/seq, hw bell */
	ntrdma_u32_t			vbell_count;

	/* resource commands */
	ntrdma_u32_t			cmd_send_cap;
	ntrdma_u32_t			cmd_send_idx;
	ntrdma_u64_t			cmd_send_rsp_buf_dma;
	ntrdma_u64_t			cmd_send_cons_dma;
	ntrdma_u32_t			cmd_send_vbell_idx;
	ntrdma_u32_t			cmd_recv_vbell_idx;

	/* ethernet frames */
	ntrdma_u32_t			eth_rx_cap;
	ntrdma_u32_t			eth_rx_idx;
	ntrdma_u64_t			eth_rx_cqe_buf_dma;
	ntrdma_u64_t			eth_rx_cons_buf_dma;
	ntrdma_u32_t			eth_rx_vbell_idx;

	ntrdma_u64_t			end_tag;
};

struct ntrdma_tcp_conf {
	ntrdma_u64_t			start_tag;

	/* resource commands */
	ntrdma_u64_t			cmd_recv_buf_dma;
	ntrdma_u64_t			cmd_recv_prod_dma;

	/* ethernet frames */
	ntrdma_u64_t			eth_tx_wqe_buf_dma;
	ntrdma_u64_t			eth_tx_prod_buf_dma;
	ntrdma_u32_t			eth_tx_vbell_idx;

	ntrdma_u64_t			end_tag;
};

struct ntrdma_tcp_req {
	struct ntrdma_req		req;
	struct ntrdma_tcp_dev		*tdev;
	NTRDMA_DECL_LIST_ENTRY		(entry);
	NTRDMA_DECL_LIST_HEAD		(imm_list);
	NTRDMA_DECL_LIST_HEAD		(op_list);
	NTRDMA_DECL_LIST_HEAD		(cb_list);
};

#define req_treq(__req) NTRDMA_CONTAINER_OF(__req, struct ntrdma_tcp_req, req)

struct ntrdma_tcp_req_imm {
	NTRDMA_DECL_LIST_ENTRY		(entry);
	ntrdma_dma_addr_t		dma;
	ntrdma_size_t			len;
};

struct ntrdma_tcp_req_op {
	NTRDMA_DECL_LIST_ENTRY		(entry);
	struct ntrdma_tcp_msg		msg;
	ntrdma_u64_t			data_src;
};

#endif
