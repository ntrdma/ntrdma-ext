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

#ifndef NTRDMA_IB_IMPL_H
#define NTRDMA_IB_IMPL_H

#ifdef NTRDMA_IB_DEFINED
#error "there can be only one"
#else
#define NTRDMA_IB_DEFINED
#endif

#define IB_DEVICE_NAME_MAX 64

struct ib_device;
struct ib_umem;

struct ib_uobject {
	struct ib_ucontext *context;
};

struct ib_sge {
	int lkey;
	ntrdma_u64_t addr;
	ntrdma_u64_t length;
};

union ib_gid {
	ntrdma_u8_t		raw[16];
	struct {
		ntrdma_u64_t	subnet_prefix;
		ntrdma_u64_t	interface_id;
	} global;
};

struct ib_udata {
};

struct ib_ah_attr {
};

enum rdma_node_type {
	RDMA_NODE_IB_CA,
};

enum rdma_transport_type {
	RDMA_TRANSPORT_IB,
	RDMA_TRANSPORT_IWARP,
	RDMA_TRANSPORT_USNIC,
	RDMA_TRANSPORT_USNIC_UDP
};

enum rdma_link_layer {
	IB_LINK_LAYER_UNSPECIFIED,
	IB_LINK_LAYER_INFINIBAND,
	IB_LINK_LAYER_ETHERNET,
};

struct ib_device_attr {
	ntrdma_u64_t max_mr_size;
	int max_pd;
	int max_mr;
	int max_qp;
	int max_qp_wr;
	int max_cq;
	int max_cqe;
	int max_sge;
	int max_sge_rd;
	int page_size_cap;
};

enum ib_port_state {
	IB_PORT_DOWN,
};

enum ib_mtu {
	IB_MTU_256,
};

struct ib_port_attr {
	enum ib_port_state state;
	enum ib_mtu max_mtu;
	enum ib_mtu active_mtu;
	int         gid_tbl_len;
	ntrdma_u32_t         port_cap_flags;
	ntrdma_u32_t         max_msg_sz;
	ntrdma_u32_t         bad_pkey_cntr;
	ntrdma_u32_t         qkey_viol_cntr;
	ntrdma_u16_t         pkey_tbl_len;
	ntrdma_u16_t         lid;
	ntrdma_u16_t         sm_lid;
	ntrdma_u8_t         lmc;
	ntrdma_u8_t          max_vl_num;
	ntrdma_u8_t          sm_sl;
	ntrdma_u8_t          subnet_timeout;
	ntrdma_u8_t          init_type_reply;
	ntrdma_u8_t          active_width;
	ntrdma_u8_t          active_speed;
	ntrdma_u8_t                      phys_state;
};

struct ib_ucontext {
};

struct ib_pd {
	struct ib_device *device;
	struct ib_uobject *uobject;
};

struct ib_mr {
	struct ib_device *device;
	int lkey, rkey;
};

enum ib_cq_notify_flags {
	IB_CQ_SOLICITED,
};

struct ib_cq_init_attr {
	int cqe;
	int comp_vector;
};

struct ib_cq {
	struct ib_device *device;
	void *cq_context;
	void (*comp_handler)(struct ib_cq*, void*);
};

struct ib_qp_cap {
	int	max_send_wr;
	int	max_recv_wr;
	int	max_send_sge;
	int	max_recv_sge;
	int	max_inline_data;
};

struct ib_qp_init_attr {
	struct ib_qp_cap cap;
	struct ib_cq *recv_cq;
	struct ib_cq *send_cq;
};

enum ib_qp_type {
	IB_QPT_RC,
};

enum ib_qp_state {
	IB_QPS_RESET,
	IB_QPS_INIT,
	IB_QPS_RTR,
	IB_QPS_RTS,
};

enum ib_qp_attr_mask {
	IB_QP_STATE			= 1 << 0,
	IB_QP_CUR_STATE		= 1 << 1,
	IB_QP_EN_SQD_ASYNC_NOTIFY	= 1 << 2,
	IB_QP_ACCESS_FLAGS		= 1 << 3,
	IB_QP_PKEY_INDEX		= 1 << 4,
	IB_QP_PORT			= 1 << 5,
	IB_QP_QKEY			= 1 << 6,
	IB_QP_AV			= 1 << 7,
	IB_QP_PATH_MTU			= 1 << 8,
	IB_QP_TIMEOUT			= 1 << 9,
	IB_QP_RETRY_CNT		= 1 << 10,
	IB_QP_RNR_RETRY		= 1 << 11,
	IB_QP_RQ_PSN			= 1 << 12,
	IB_QP_MAX_QP_RD_ATOMIC		= 1 << 13,
	IB_QP_ALT_PATH			= 1 << 14,
	IB_QP_MIN_RNR_TIMER		= 1 << 15,
	IB_QP_SQ_PSN			= 1 << 16,
	IB_QP_MAX_DEST_RD_ATOMIC	= 1 << 17,
	IB_QP_PATH_MIG_STATE		= 1 << 18,
	IB_QP_CAP			= 1 << 19,
	IB_QP_DEST_QPN			= 1 << 20
};

struct ib_qp_attr {
	enum ib_qp_state cur_qp_state;
	enum ib_qp_state qp_state;
	int dest_qp_num;
};

struct ib_qp {
	struct ib_device *device;
	struct ib_cq *recv_cq;
	struct ib_cq *send_cq;
	int qp_num;
};

enum ib_wr_opcode {
	IB_WR_RDMA_WRITE,
	IB_WR_RDMA_WRITE_WITH_IMM,
	IB_WR_SEND,
	IB_WR_SEND_WITH_IMM,
	IB_WR_RDMA_READ,
	IB_WR_ATOMIC_CMP_AND_SWP,
	IB_WR_ATOMIC_FETCH_AND_ADD
};

struct ib_send_wr {
	struct ib_send_wr *next;
	ntrdma_u64_t wr_id;
	struct ib_sge *sg_list;
	int num_sge;
	enum ib_wr_opcode opcode;
	union {
		struct {
			ntrdma_u64_t remote_addr;
			ntrdma_u32_t rkey;
		} rdma;
	} wr;
};

struct ib_recv_wr {
	struct ib_recv_wr *next;
	unsigned long wr_id;
	struct ib_sge *sg_list;
	int num_sge;
};

enum ib_wc_opcode {
	IB_WC_SEND,
	IB_WC_RDMA_WRITE,
	IB_WC_RDMA_READ,
	IB_WC_COMP_SWAP,
	IB_WC_FETCH_ADD,
	IB_WC_BIND_MW,
	IB_WC_LSO,
	IB_WC_LOCAL_INV,
	IB_WC_FAST_REG_MR,
	IB_WC_MASKED_COMP_SWAP,
	IB_WC_MASKED_FETCH_ADD,
	IB_WC_RECV          = 1 << 7,
	IB_WC_RECV_RDMA_WITH_IMM
};

enum ib_wc_status {
	IB_WC_SUCCESS,
};

struct ib_wc {
	unsigned long wr_id;
	enum ib_wc_status status;
	enum ib_wc_opcode opcode;
	ntrdma_u32_t vendor_err;
	ntrdma_u32_t byte_len;
	struct ib_qp *qp;
	union {
		ntrdma_u32_t imm_data;
		ntrdma_u32_t invalidate_rkey;
	} ex;
	ntrdma_u32_t src_qp;
	int wc_flags;
	ntrdma_u16_t pkey_index;
	ntrdma_u16_t slid;
	ntrdma_u8_t  sl;
	ntrdma_u8_t dlid_path_bits;
	ntrdma_u8_t port_num;
};

enum ib_event_type {
	IB_EVENT_CQ_ERR,
	IB_EVENT_QP_FATAL,
	IB_EVENT_QP_REQ_ERR,
	IB_EVENT_QP_ACCESS_ERR,
	IB_EVENT_COMM_EST,
	IB_EVENT_SQ_DRAINED,
	IB_EVENT_PATH_MIG,
	IB_EVENT_PATH_MIG_ERR,
	IB_EVENT_DEVICE_FATAL,
	IB_EVENT_PORT_ACTIVE,
	IB_EVENT_PORT_ERR,
	IB_EVENT_LID_CHANGE,
	IB_EVENT_PKEY_CHANGE,
	IB_EVENT_SM_CHANGE,
	IB_EVENT_SRQ_ERR,
	IB_EVENT_SRQ_LIMIT_REACHED,
	IB_EVENT_QP_LAST_WQE_REACHED,
	IB_EVENT_CLIENT_REREGISTER,
	IB_EVENT_GID_CHANGE,
};

struct ib_event {
	struct ib_device    *device;
	union {
		struct ib_cq    *cq;
		struct ib_qp    *qp;
		ntrdma_u8_t     port_num;
	} element;
	enum ib_event_type  event;
};

struct ib_device {
	void *owner;
	char name[IB_DEVICE_NAME_MAX];
	enum rdma_node_type node_type;
	int num_comp_vectors;
	void *dma_device;
	int uverbs_abi_ver;
	ntrdma_u64_t uverbs_cmd_mask;

	int (*query_device)(struct ib_device *device, struct ib_device_attr *device_attr, struct ib_udata *udata);
	int (*query_port)(struct ib_device *device, ntrdma_u8_t port_num, struct ib_port_attr *port_attr);
	enum rdma_link_layer (*get_link_layer)(struct ib_device *device, ntrdma_u8_t port_num);
	int (*query_gid)(struct ib_device *device, ntrdma_u8_t port_num, int index, union ib_gid *gid);
	int (*query_pkey)(struct ib_device *device, ntrdma_u8_t port_num, ntrdma_u16_t index, ntrdma_u16_t *pkey);
	struct ib_ucontext * (*alloc_ucontext)(struct ib_device *device, struct ib_udata *udata);
	int (*dealloc_ucontext)(struct ib_ucontext *context);
	struct ib_pd * (*alloc_pd)(struct ib_device *device, struct ib_ucontext *context, struct ib_udata *udata);
	int (*dealloc_pd)(struct ib_pd *pd);
	struct ib_ah * (*create_ah)(struct ib_pd *pd, struct ib_ah_attr *ah_attr);
	int (*modify_ah)(struct ib_ah *ah, struct ib_ah_attr *ah_attr);
	int (*query_ah)(struct ib_ah *ah, struct ib_ah_attr *ah_attr);
	int (*destroy_ah)(struct ib_ah *ah);
	struct ib_qp * (*create_qp)(struct ib_pd *pd, struct ib_qp_init_attr *qp_init_attr, struct ib_udata *udata);
	int (*modify_qp)(struct ib_qp *qp, struct ib_qp_attr *qp_attr, int qp_attr_mask, struct ib_udata *udata);
	int (*query_qp)(struct ib_qp *qp, struct ib_qp_attr *qp_attr, int qp_attr_mask, struct ib_qp_init_attr *qp_init_attr);
	int (*destroy_qp)(struct ib_qp *qp);
	int (*post_send)(struct ib_qp *qp, struct ib_send_wr *send_wr, struct ib_send_wr **bad_send_wr);
	int (*post_recv)(struct ib_qp *qp, struct ib_recv_wr *recv_wr, struct ib_recv_wr **bad_recv_wr);
	struct ib_cq * (*create_cq)(struct ib_device *device, const struct ib_cq_init_attr *attr, struct ib_ucontext *context, struct ib_udata *udata);
	int (*modify_cq)(struct ib_cq *cq, ntrdma_u16_t cq_count, ntrdma_u16_t cq_period);
	int (*destroy_cq)(struct ib_cq *cq);
	int (*resize_cq)(struct ib_cq *cq, int cqe, struct ib_udata *udata);
	int (*poll_cq)(struct ib_cq *cq, int num_entries, struct ib_wc *wc);
	int (*peek_cq)(struct ib_cq *cq, int wc_cnt);
	int (*req_notify_cq)(struct ib_cq *cq, enum ib_cq_notify_flags flags);
	int (*req_ncomp_notif)(struct ib_cq *cq, int wc_cnt);
	struct ib_mr * (*reg_user_mr)(struct ib_pd *pd, ntrdma_u64_t start, ntrdma_u64_t length, ntrdma_u64_t virt_addr, int mr_access_flags, struct ib_udata *udata);
	struct ib_mr * (*get_dma_mr)(struct ib_pd *pd, int mr_access_flags);
	int (*dereg_mr)(struct ib_mr *mr);
};

enum {
	IB_USER_VERBS_CMD_GET_CONTEXT,
	IB_USER_VERBS_CMD_QUERY_DEVICE,
	IB_USER_VERBS_CMD_QUERY_PORT,
	IB_USER_VERBS_CMD_ALLOC_PD,
	IB_USER_VERBS_CMD_DEALLOC_PD,
	IB_USER_VERBS_CMD_CREATE_AH,
	IB_USER_VERBS_CMD_MODIFY_AH,
	IB_USER_VERBS_CMD_QUERY_AH,
	IB_USER_VERBS_CMD_DESTROY_AH,
	IB_USER_VERBS_CMD_REG_MR,
	IB_USER_VERBS_CMD_REG_SMR,
	IB_USER_VERBS_CMD_REREG_MR,
	IB_USER_VERBS_CMD_QUERY_MR,
	IB_USER_VERBS_CMD_DEREG_MR,
	IB_USER_VERBS_CMD_ALLOC_MW,
	IB_USER_VERBS_CMD_BIND_MW,
	IB_USER_VERBS_CMD_DEALLOC_MW,
	IB_USER_VERBS_CMD_CREATE_COMP_CHANNEL,
	IB_USER_VERBS_CMD_CREATE_CQ,
	IB_USER_VERBS_CMD_RESIZE_CQ,
	IB_USER_VERBS_CMD_DESTROY_CQ,
	IB_USER_VERBS_CMD_POLL_CQ,
	IB_USER_VERBS_CMD_PEEK_CQ,
	IB_USER_VERBS_CMD_REQ_NOTIFY_CQ,
	IB_USER_VERBS_CMD_CREATE_QP,
	IB_USER_VERBS_CMD_QUERY_QP,
	IB_USER_VERBS_CMD_MODIFY_QP,
	IB_USER_VERBS_CMD_DESTROY_QP,
	IB_USER_VERBS_CMD_POST_SEND,
	IB_USER_VERBS_CMD_POST_RECV,
	IB_USER_VERBS_CMD_ATTACH_MCAST,
	IB_USER_VERBS_CMD_DETACH_MCAST,
	IB_USER_VERBS_CMD_CREATE_SRQ,
	IB_USER_VERBS_CMD_MODIFY_SRQ,
	IB_USER_VERBS_CMD_QUERY_SRQ,
	IB_USER_VERBS_CMD_DESTROY_SRQ,
	IB_USER_VERBS_CMD_POST_SRQ_RECV
};

struct ib_device *ib_alloc_device(ntrdma_size_t size);
void ib_dealloc_device(struct ib_device *device);
int ib_register_device(struct ib_device *device, void *port_callback);
void ib_unregister_device(struct ib_device *device);

int ib_modify_qp_is_ok(enum ib_qp_state cur_state,
		       enum ib_qp_state next_state, enum ib_qp_type type,
		       enum ib_qp_attr_mask mask, enum rdma_link_layer ll);

void ib_dispatch_event(struct ib_event *event);

struct ib_umem *ib_umem_get(struct ib_ucontext *context,
			    unsigned long addr, ntrdma_size_t size,
			    int access, int dmasync);

void ib_umem_release(struct ib_umem *umem);

#endif
