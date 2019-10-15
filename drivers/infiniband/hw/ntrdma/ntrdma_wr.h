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

#ifndef NTRDMA_WR_H
#define NTRDMA_WR_H

#include "ntrdma_sg.h"

/* work and completion queue entry op_code values */
#define NTRDMA_WR_RECV			0 /* post recv and consumed by send */
#define NTRDMA_WR_RECV_INV		1 /* recv consumed by send with inv */
#define NTRDMA_WR_RECV_IMM		2 /* recv consumed by send with imm */
#define NTRDMA_WR_RECV_RDMA		3 /* recv consumed by write with imm */
#define NTRDMA_WR_SEND			4 /* post send */
#define NTRDMA_WR_SEND_INV		5 /* post send with inv */
#define NTRDMA_WR_SEND_IMM		6 /* post send with imm */
#define NTRDMA_WR_SEND_RDMA		7 /* post write with imm */
#define NTRDMA_WR_RDMA_WRITE		8 /* post rdma write */
#define NTRDMA_WR_RDMA_READ		9 /* post rdma read */

/* completion queue entry op_status values */
#define NTRDMA_WC_SUCCESS		0
#define NTRDMA_WC_ERR_ABORTED		1
#define NTRDMA_WC_ERR_CORRUPT		2
#define NTRDMA_WC_ERR_OPCODE		3
#define NTRDMA_WC_ERR_RECV_NOT_READY	4
#define NTRDMA_WC_ERR_RECV_MISSING	5
#define NTRDMA_WC_ERR_RECV_OVERFLOW	6
#define NTRDMA_WC_ERR_RDMA_KEY		7
#define NTRDMA_WC_ERR_RDMA_RANGE	8
#define NTRDMA_WC_ERR_RDMA_ACCESS	9
#define NTRDMA_WC_ERR_LOC_PORT		10


struct ntrdma_send_wqe {
	u64				ulp_handle;
	u16				op_code;
	u16				op_status;
	u32				recv_key;
	struct ib_sge			rdma_sge;
	u32				imm_data;

	union {
		u32	sg_count;
		u8 inline_len;
	};

	int				flags;
};

struct ntrdma_recv_wqe {
	u64				ulp_handle;
	u16				op_code;
	u16				op_status;
	u32				sg_count;
	struct ntrdma_wr_rcv_sge	rcv_sg_list[];
};

struct ntrdma_cqe {
	u64				ulp_handle;
	u16				op_code;
	u16				op_status;
	u32				rdma_len;
	u32				imm_data;
	int				flags;
};


static inline size_t ntrdma_wqe_snd_sg_list_size(u32 sg_cap)
{
	return sg_cap * sizeof(struct ntrdma_wr_snd_sge);
}

static inline size_t ntrdma_wqe_snd_inline_size(u32 inline_cap)
{
	return inline_cap;
}

static inline struct ntrdma_wr_snd_sge *
snd_sg_list(int index, struct ntrdma_send_wqe *wqe)
{
	void *past_wqe = wqe + 1;
	struct ntrdma_wr_snd_sge *array = past_wqe;

	return &array[index];
}

static inline const struct ntrdma_wr_snd_sge *
const_snd_sg_list(int index, const struct ntrdma_send_wqe *wqe)
{
	const void *past_wqe = wqe + 1;
	const struct ntrdma_wr_snd_sge *array = past_wqe;

	return &array[index];
}

static inline u8 *snd_inline_data(struct ntrdma_send_wqe *wqe)
{
	void *past_wqe = wqe + 1;

	return past_wqe;
}

static inline size_t ntrdma_wqe_rcv_sg_list_size(u32 sg_cap)
{
	return sg_cap * sizeof(struct ntrdma_wr_rcv_sge);
}

static inline size_t ntrdma_send_wqe_size(u32 sg_cap, u32 inline_cap)
{
	return sizeof(struct ntrdma_send_wqe) +
			max(ntrdma_wqe_snd_sg_list_size(sg_cap),
			ntrdma_wqe_snd_inline_size(inline_cap));
}

static inline size_t ntrdma_recv_wqe_size(u32 sg_cap)
{
	return sizeof(struct ntrdma_recv_wqe) +
		ntrdma_wqe_rcv_sg_list_size(sg_cap);
}

static inline bool ntrdma_wr_status_no_recv(u16 op_status)
{
	switch (op_status) {
	case NTRDMA_WC_ERR_RECV_NOT_READY:
	case NTRDMA_WC_ERR_RECV_MISSING:
		return true;
	}
	return false;
}

static inline bool ntrdma_wr_code_is_recv(u16 op_code)
{
	switch (op_code) {
	case NTRDMA_WR_RECV:
	case NTRDMA_WR_RECV_INV:
	case NTRDMA_WR_RECV_IMM:
	case NTRDMA_WR_RECV_RDMA:
		return true;
	}

	return false;
}

static inline bool ntrdma_wr_code_is_send(u16 op_code)
{
	switch (op_code) {
	case NTRDMA_WR_SEND:
	case NTRDMA_WR_SEND_INV:
	case NTRDMA_WR_SEND_IMM:
	case NTRDMA_WR_SEND_RDMA:
		return true;
	}

	return false;
}

static inline bool ntrdma_wr_code_is_rdma(u16 op_code)
{
	switch (op_code) {
	case NTRDMA_WR_SEND_RDMA:
	case NTRDMA_WR_RDMA_WRITE:
	case NTRDMA_WR_RDMA_READ:
		return true;
	}
	return false;
}

static inline bool ntrdma_wr_code_push_data(u16 op_code)
{
	switch (op_code) {
	case NTRDMA_WR_SEND:
	case NTRDMA_WR_SEND_INV:
	case NTRDMA_WR_SEND_IMM:
	case NTRDMA_WR_SEND_RDMA:
	case NTRDMA_WR_RDMA_WRITE:
		return true;
	}
	return false;
}

static inline bool ntrdma_wr_code_pull_data(u16 op_code)
{
	return op_code == NTRDMA_WR_RDMA_READ;
}

int ntrdma_recv_wqe_sync(struct ntrdma_dev *dev, struct ntrdma_recv_wqe *wqe);
void ntrdma_recv_wqe_cleanup(struct ntrdma_recv_wqe *wqe);

#endif
