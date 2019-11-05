/*
 * Copyright (c) 2014-2019 Dell EMC, Inc.  All rights reserved.
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

#ifndef __NTRDMA_IOCTL_H__
#define __NTRDMA_IOCTL_H__

#ifdef __KERNEL__

#include <rdma/ib_verbs.h>
#include <linux/ioctl.h>

typedef u64 ntrdma_u64;
typedef u32 ntrdma_u32;
typedef u16 ntrdma_u16;
typedef u8  ntrdma_u8;
typedef struct ib_sge ntrdma_sge;

#else

#include <infiniband/driver.h>
#include <sys/ioctl.h>

typedef uint64_t ntrdma_u64;
typedef uint32_t ntrdma_u32;
typedef uint16_t ntrdma_u16;
typedef uint8_t  ntrdma_u8;
typedef struct ibv_sge ntrdma_sge;

#endif

#define NTRDMA_IOCTL_IF_HELPER_IMPL(SDESC, ...)			\
typedef char ntrdma_ioctl_if_desc[sizeof(SDESC)];		\
								\
static inline							\
void ntrdma_ioctl_if_init_desc(ntrdma_ioctl_if_desc *desc)	\
{								\
	memcpy(*desc, SDESC, sizeof(SDESC));			\
}								\
								\
static inline							\
bool ntrdma_ioctl_if_check_desc(ntrdma_ioctl_if_desc *desc)	\
{								\
	return !memcmp(*desc, SDESC, sizeof(SDESC));		\
}								\
								\
__VA_ARGS__

#define NTRDMA_IOCTL_IF_STRINGIFY_IMPL(X) #X
#define NTRDMA_IOCTL_IF_STRINGIFY(X) NTRDMA_IOCTL_IF_STRINGIFY_IMPL(X)

#define NTRDMA_IOCTL_IF_HELPER(DESC) \
	NTRDMA_IOCTL_IF_HELPER_IMPL(NTRDMA_IOCTL_IF_STRINGIFY((DESC)), DESC)

#define NTRDMA_IOCTL_IF					\
							\
struct ntrdma_create_qp_ext {				\
	ntrdma_ioctl_if_desc		desc;		\
	ntrdma_u64			send_page_ptr;	\
};							\
							\
struct ntrdma_create_qp_resp_ext {			\
	int qpfd;					\
};							\
							\
struct ntrdma_create_cq_ext {				\
	ntrdma_ioctl_if_desc		desc;		\
	ntrdma_u64			poll_page_ptr;	\
};							\
							\
struct ntrdma_create_cq_resp_ext {			\
	int cqfd;					\
};							\
							\
typedef enum {						\
	NTRDMA_SMALL_ENUM_MIN = 0,			\
	NTRDMA_SMALL_ENUM_MAX = 1 << 7,			\
} ntrdma_small_enum;					\
							\
struct ntrdma_send_wqe {				\
	ntrdma_u64			ulp_handle;	\
	ntrdma_u16			op_code;	\
	ntrdma_u16			op_status;	\
	ntrdma_u32			recv_key;	\
	ntrdma_sge			rdma_sge;	\
	ntrdma_u32			imm_data;	\
	ntrdma_u32			flags;		\
	union {						\
		ntrdma_u32		sg_count;	\
		ntrdma_u32		inline_len;	\
	};						\
};							\
							\
struct ntrdma_snd_hdr {					\
	ntrdma_u32			wqe_counter;	\
	ntrdma_u32			first_wqe_size;	\
};							\
							\
struct ntrdma_poll_hdr {				\
	ntrdma_u32			wc_counter;	\
};							\
							\
struct ntrdma_ibv_wc {					\
	ntrdma_u64			wr_id;		\
	ntrdma_small_enum		status;		\
	ntrdma_small_enum		opcode;		\
	ntrdma_u32			vendor_err;	\
	ntrdma_u32			byte_len;	\
	union {						\
		ntrdma_u32		imm_data;	\
		ntrdma_u32	invalidated_rkey;	\
	};						\
	ntrdma_u32			qp_num;		\
	ntrdma_u32			src_qp;		\
	unsigned int			wc_flags;	\
	ntrdma_u16			pkey_index;	\
	ntrdma_u16			slid;		\
	ntrdma_u8			sl;		\
	ntrdma_u8			dlid_path_bits;	\
};							\
							\
enum {							\
	NTRDMA_IOCTL_SEND = _IO('N', 0x30),		\
	NTRDMA_IOCTL_POLL = _IO('N', 0x31),		\
};

NTRDMA_IOCTL_IF_HELPER(NTRDMA_IOCTL_IF);

#endif
