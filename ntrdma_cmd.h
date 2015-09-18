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

#ifndef NTRDMA_CMD_H
#define NTRDMA_CMD_H

#include "ntrdma_os.h"
#include "ntrdma_sg.h"

struct ntrdma_dev;
struct ntrdma_req;

/* Command ring capacity */

#define NTRDMA_CMD_CAP			0x10

/* Command and response size in bytes */

#define NTRDMA_CMD_SIZE			0x100
#define NTRDMA_RSP_SIZE			0x40

/* Command op codes */

#define NTRDMA_CMD_NONE			0

#define NTRDMA_CMD_PD			0x04
#define NTRDMA_CMD_PD_CREATE		(NTRDMA_CMD_PD + 0)
#define NTRDMA_CMD_PD_DELETE		(NTRDMA_CMD_PD + 1)

#define NTRDMA_CMD_MR			0x08
#define NTRDMA_CMD_MR_CREATE		(NTRDMA_CMD_MR + 0)
#define NTRDMA_CMD_MR_DELETE		(NTRDMA_CMD_MR + 1)
#define NTRDMA_CMD_MR_APPEND		(NTRDMA_CMD_MR + 2)

#define NTRDMA_CMD_QP			0x0c
#define NTRDMA_CMD_QP_CREATE		(NTRDMA_CMD_QP + 0)
#define NTRDMA_CMD_QP_DELETE		(NTRDMA_CMD_QP + 1)
#define NTRDMA_CMD_QP_MODIFY		(NTRDMA_CMD_QP + 2)

/* Header for commands and responses */
struct ntrdma_cmd_hdr {
	ntrdma_u32_t			op;
	ntrdma_u32_t			pd_key;
};

/* Protection domain command is simply the header */

/* Create or delete protection domain response */
struct ntrdma_rsp_pd_status {
	struct ntrdma_cmd_hdr		hdr;
	ntrdma_u32_t			status;
};

/* Create memory region command */
struct ntrdma_cmd_mr_create {
	struct ntrdma_cmd_hdr		hdr;
	ntrdma_u32_t			mr_key;
	ntrdma_u32_t			access;
	ntrdma_u64_t			mr_addr;
	ntrdma_u64_t			mr_len;
	ntrdma_u32_t			sg_cap;
	ntrdma_u32_t			sg_count;
	struct ntrdma_mr_sge		sg_list[];
};

/* Delete memory region command */
struct ntrdma_cmd_mr_delete {
	struct ntrdma_cmd_hdr		hdr;
	ntrdma_u32_t			mr_key;
};

/* Append to memory region command */
struct ntrdma_cmd_mr_append {
	struct ntrdma_cmd_hdr		hdr;
	ntrdma_u32_t			mr_key;
	ntrdma_u32_t			sg_cap;
	ntrdma_u32_t			sg_pos;
	ntrdma_u32_t			sg_count;
	struct ntrdma_mr_sge		sg_list[];
};

/* Create, delete or append memory region response */
struct ntrdma_rsp_mr_status {
	struct ntrdma_cmd_hdr		hdr;
	ntrdma_u32_t			mr_key;
	ntrdma_u32_t			status;
};

/* Create queue pair command */
struct ntrdma_cmd_qp_create {
	struct ntrdma_cmd_hdr		hdr;
	ntrdma_u32_t			qp_key;
	ntrdma_u32_t			qp_type;
	ntrdma_u32_t			recv_wqe_cap;
	ntrdma_u32_t			recv_wqe_sg_cap;
	ntrdma_u32_t			recv_ring_idx;
	ntrdma_u32_t			send_wqe_cap;
	ntrdma_u32_t			send_wqe_sg_cap;
	ntrdma_u32_t			send_ring_idx;
	ntrdma_u64_t			send_cqe_buf_dma;
	ntrdma_u64_t			send_cons_dma;
	ntrdma_u32_t			cmpl_vbell_idx;
};

/* Delete a queue pair command */
struct ntrdma_cmd_qp_delete {
	struct ntrdma_cmd_hdr		hdr;
	ntrdma_u32_t			qp_key;
};

/* Modify queue pair command */
struct ntrdma_cmd_qp_modify {
	struct ntrdma_cmd_hdr		hdr;
	ntrdma_u32_t			qp_key;
	ntrdma_u32_t			state;
	ntrdma_u32_t			access;
	ntrdma_u32_t			dest_qp_key;
};

/* Create queue pair response */
struct ntrdma_rsp_qp_create {
	struct ntrdma_cmd_hdr		hdr;
	ntrdma_u32_t			qp_key;
	ntrdma_u32_t			status;
	ntrdma_dma_addr_t		recv_wqe_buf_dma;
	ntrdma_dma_addr_t		recv_prod_dma;
	ntrdma_dma_addr_t		send_wqe_buf_dma;
	ntrdma_dma_addr_t		send_prod_dma;
	ntrdma_u32_t			send_vbell_idx;
};

/* Delete or modify queue pair response */
struct ntrdma_rsp_qp_status {
	struct ntrdma_cmd_hdr		hdr;
	ntrdma_u32_t			qp_key;
	ntrdma_u32_t			status;
};

/* Command union */
union ntrdma_cmd {
	struct ntrdma_cmd_hdr		hdr;
	struct ntrdma_cmd_hdr		pd_create;
	struct ntrdma_cmd_hdr		pd_delete;
	struct ntrdma_cmd_mr_create	mr_create;
	struct ntrdma_cmd_mr_delete	mr_delete;
	struct ntrdma_cmd_mr_append	mr_append;
	struct ntrdma_cmd_qp_create	qp_create;
	struct ntrdma_cmd_qp_delete	qp_delete;
	struct ntrdma_cmd_qp_modify	qp_modify;
	ntrdma_u8_t			buf[NTRDMA_CMD_SIZE];
};

/* Response union */
union ntrdma_rsp {
	struct ntrdma_cmd_hdr		hdr;
	struct ntrdma_rsp_pd_status	pd_status;
	struct ntrdma_rsp_mr_status	mr_status;
	struct ntrdma_rsp_qp_status	qp_status;
	struct ntrdma_rsp_qp_create	qp_create;
	ntrdma_u8_t			buf[NTRDMA_RSP_SIZE];
};

#define NTRDMA_CMD_MR_CREATE_SG_CAP \
	((sizeof(union ntrdma_cmd) - sizeof(struct ntrdma_cmd_mr_create)) \
	 / sizeof(struct ntrdma_mr_sge))

#define NTRDMA_CMD_MR_APPEND_SG_CAP \
	((sizeof(union ntrdma_cmd) - sizeof(struct ntrdma_cmd_mr_append)) \
	 / sizeof(struct ntrdma_mr_sge))

struct ntrdma_cmd_cb {
	/* entry in the device cmd pending or posted list */
	NTRDMA_DECL_LIST_ENTRY		(dev_entry);

	/* prepare a command in-place in the ring buffer */
	int				(*cmd_prep)(struct ntrdma_cmd_cb *cb,
						    union ntrdma_cmd *cmd,
						    struct ntrdma_req *req);

	/* complete and free the command following a response */
	int				(*rsp_cmpl)(struct ntrdma_cmd_cb *cb,
						    union ntrdma_rsp *rsp,
						    struct ntrdma_req *req);
};

int ntrdma_dev_cmd_init(struct ntrdma_dev *dev,
			ntrdma_u32_t recv_vbell_idx,
			ntrdma_u32_t send_vbell_idx,
			ntrdma_u32_t send_cap);

void ntrdma_dev_cmd_deinit(struct ntrdma_dev *dev);

int ntrdma_dev_cmd_conf(struct ntrdma_dev *dev,
			ntrdma_dma_addr_t peer_send_rsp_buf_dma,
			ntrdma_dma_addr_t peer_send_cons_dma,
			ntrdma_u32_t peer_send_vbell_idx,
			ntrdma_u32_t peer_recv_vbell_idx,
			ntrdma_u32_t recv_cap,
			ntrdma_u32_t recv_idx);

void ntrdma_dev_cmd_deconf(struct ntrdma_dev *dev);

void ntrdma_dev_cmd_enable(struct ntrdma_dev *dev,
			   ntrdma_dma_addr_t peer_recv_buf_dma,
			   ntrdma_dma_addr_t peer_recv_prod_dma);

void ntrdma_dev_cmd_disable(struct ntrdma_dev *dev);

void ntrdma_dev_cmd_add(struct ntrdma_dev *dev, struct ntrdma_cmd_cb *cb);
void ntrdma_dev_cmd_submit(struct ntrdma_dev *dev);
void ntrdma_dev_cmd_finish(struct ntrdma_dev *dev);

#endif
