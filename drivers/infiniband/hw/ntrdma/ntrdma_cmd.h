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

#include <linux/ntc.h>
#include <linux/types.h>

#include "ntrdma_sg.h"

struct ntrdma_dev;
struct ntrdma_req;

/* Command and response size in bytes */

#define NTRDMA_CMD_SIZE			0x100
#define NTRDMA_RSP_SIZE			0x40

/* Command op codes */

#define NTRDMA_CMD_NONE			0

#define NTRDMA_CMD_MR			0x08
#define NTRDMA_CMD_MR_CREATE		(NTRDMA_CMD_MR + 0)
#define NTRDMA_CMD_MR_DELETE		(NTRDMA_CMD_MR + 1)
#define NTRDMA_CMD_MR_APPEND		(NTRDMA_CMD_MR + 2)

#define NTRDMA_CMD_QP			0x0c
#define NTRDMA_CMD_QP_CREATE		(NTRDMA_CMD_QP + 0)
#define NTRDMA_CMD_QP_DELETE		(NTRDMA_CMD_QP + 1)
#define NTRDMA_CMD_QP_MODIFY		(NTRDMA_CMD_QP + 2)

struct ntrdma_rsp_hdr {
	u32				op;
	u32				status;
};

/* Create memory region command */
struct ntrdma_cmd_mr_create {
	u32				op;
	u32				mr_key;
	u32				pd_key;
	u32				access;
	u64				mr_addr;
	u64				mr_len;
	u32				sg_cap;
	u32				sg_count;
	struct ntc_sge			sg_list[];
};

/* Delete memory region command */
struct ntrdma_cmd_mr_delete {
	u32				op;
	u32				mr_key;
};

/* Append to memory region command */
struct ntrdma_cmd_mr_append {
	u32				op;
	u32				mr_key;
	u32				sg_pos;
	u32				sg_count;
	struct ntc_sge			sg_list[];
};

/* Create, delete or append memory region response */
struct ntrdma_rsp_mr_status {
	struct ntrdma_rsp_hdr		hdr;
	u32				mr_key;
};

/* Create queue pair command */
struct ntrdma_cmd_qp_create {
	u32				op;
	u32				qp_key;
	u32				pd_key;
	u32				qp_type;
	u32				recv_wqe_cap;
	u32				recv_wqe_sg_cap;
	u32				recv_ring_idx;
	u32				send_wqe_cap;
	u32				send_wqe_sg_cap;
	u32				send_ring_idx;
	u64				send_cqe_buf_addr;
	size_t			send_cqe_buf_size;
	u64				send_cons_addr;
	size_t			send_cons_size;
	u32				cmpl_vbell_idx;
};

/* Delete a queue pair command */
struct ntrdma_cmd_qp_delete {
	u32				op;
	u32				qp_key;
};

/* Modify queue pair command */
struct ntrdma_cmd_qp_modify {
	u32				op;
	u32				qp_key;
	u32				access;
	u32				state;
	u32				dest_qp_key;
};

/* Create queue pair response */
struct ntrdma_rsp_qp_create {
	struct ntrdma_rsp_hdr		hdr;
	u32				qp_key;
	u32				send_vbell_idx;
	u64				recv_wqe_buf_addr;
	size_t			recv_wqe_buf_size;
	u64				recv_prod_addr;
	size_t			recv_prod_size;
	u64				send_wqe_buf_addr;
	size_t			send_wqe_buf_size;
	u64				send_prod_addr;
	size_t			send_prod_size;
};

/* Delete or modify queue pair response */
struct ntrdma_rsp_qp_status {
	struct ntrdma_rsp_hdr		hdr;
	u32				qp_key;
};

/* Command union */
union ntrdma_cmd {
	u32				op;
	struct ntrdma_cmd_mr_create	mr_create;
	struct ntrdma_cmd_mr_delete	mr_delete;
	struct ntrdma_cmd_mr_append	mr_append;
	struct ntrdma_cmd_qp_create	qp_create;
	struct ntrdma_cmd_qp_delete	qp_delete;
	struct ntrdma_cmd_qp_modify	qp_modify;
	u8				buf[NTRDMA_CMD_SIZE];
};

/* Response union */
union ntrdma_rsp {
	struct ntrdma_rsp_hdr		hdr;
	struct ntrdma_rsp_mr_status	mr_create;
	struct ntrdma_rsp_mr_status	mr_delete;
	struct ntrdma_rsp_mr_status	mr_append;
	struct ntrdma_rsp_qp_create	qp_create;
	struct ntrdma_rsp_qp_status	qp_delete;
	struct ntrdma_rsp_qp_status	qp_modify;
	u8				buf[NTRDMA_RSP_SIZE];
};

#define NTRDMA_CMD_MR_CREATE_SG_CAP \
	((sizeof(union ntrdma_cmd) - sizeof(struct ntrdma_cmd_mr_create)) \
	 / sizeof(struct ntc_sge))

#define NTRDMA_CMD_MR_APPEND_SG_CAP \
	((sizeof(union ntrdma_cmd) - sizeof(struct ntrdma_cmd_mr_append)) \
	 / sizeof(struct ntc_sge))

struct ntrdma_cmd_cb {
	/* entry in the device cmd pending or posted list */
	struct list_head		dev_entry;

	/* prepare a command in-place in the ring buffer */
	int (*cmd_prep)(struct ntrdma_cmd_cb *cb,
			union ntrdma_cmd *cmd,
			struct ntrdma_req *req);

	/* complete and free the command following a response */
	int (*rsp_cmpl)(struct ntrdma_cmd_cb *cb,
			union ntrdma_rsp *rsp,
			struct ntrdma_req *req);
};

int ntrdma_dev_cmd_init(struct ntrdma_dev *dev,
			u32 recv_vbell_idx,
			u32 send_vbell_idx,
			u32 send_cap);

void ntrdma_dev_cmd_deinit(struct ntrdma_dev *dev);

void ntrdma_dev_cmd_add(struct ntrdma_dev *dev, struct ntrdma_cmd_cb *cb);
void ntrdma_dev_cmd_add_unsafe(struct ntrdma_dev *dev,
		struct ntrdma_cmd_cb *cb);
void ntrdma_dev_cmd_submit(struct ntrdma_dev *dev);
void ntrdma_dev_cmd_finish(struct ntrdma_dev *dev);

#endif
