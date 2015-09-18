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

#ifndef NTRDMA_QP_H
#define NTRDMA_QP_H

#include "ntrdma_res.h"

struct ntrdma_dev;
struct ntrdma_req;
struct ntrdma_cq;
struct ntrdma_pd;
struct ntrdma_qp;
struct ntrdma_rpd;
struct ntrdma_rqp;

struct ntrdma_send_wqe;
struct ntrdma_recv_wqe;
struct ntrdma_cqe;

enum ntrdma_qp_state {
	NTRDMA_QPS_ERROR		= -1,

	NTRDMA_QPS_RESET		= 0,
	NTRDMA_QPS_INIT,
	NTRDMA_QPS_RECV_READY,
	NTRDMA_QPS_SEND_DRAIN, /* Note: unused, no QP UD this version */
	NTRDMA_QPS_SEND_READY,
};

/* Queue Pair Entry in Completion Queue */
struct ntrdma_poll {
	NTRDMA_DECL_LIST_ENTRY	(cq_entry);

	struct ntrdma_cqe	*(*poll_cqe)(struct ntrdma_poll *poll,
					     struct ntrdma_cqe *abort_cqe,
					     ntrdma_u32_t pos);

	int			(*poll_start_and_get)(struct ntrdma_poll *poll,
						      struct ntrdma_qp **qp,
						      ntrdma_u32_t *pos,
						      ntrdma_u32_t *end,
						      ntrdma_u32_t *base);

	void			(*poll_put_and_done)(struct ntrdma_poll *poll,
						     ntrdma_u32_t pos,
						     ntrdma_u32_t base);
};

/* Queue Pair */
struct ntrdma_qp {
	/* Ofed qp structure */
	struct ib_qp			ibqp;
#ifdef CONFIG_NTRDMA_DEBUGFS
	struct dentry			*debug;
#endif

	/* Ntrdma resource bookkeeping structure */
	struct ntrdma_res		res;
	/* Protection domain to which this queue pair is associated */
	struct ntrdma_pd		*pd;

	/* Completion queue for recv requests */
	struct ntrdma_cq		*recv_cq;
	struct ntrdma_poll		recv_poll;

	/* Completion queue for send requests */
	struct ntrdma_cq		*send_cq;
	struct ntrdma_poll		send_poll;

	/* The current ib_qp_state of the queue pair */
	int				state;

	/* The behavior within the queue pair state */
	int				recv_error;
	int				recv_abort;
	int				send_error;
	int				send_abort;

	/* key of connected remote queue pair, or -1 if not connected */
	int				rqp_key;

	/* rings are prepared for copying to the peer */
	bool				ring_ready;

	/* sizes and capacities of single work queue entries */
	int				send_wqe_sg_cap;
	int				recv_wqe_sg_cap;
	ntrdma_size_t			send_wqe_size;
	ntrdma_size_t			recv_wqe_size;

	/* send ring indices */
	ntrdma_u32_t			send_cap;
	ntrdma_u32_t			send_post;
	ntrdma_u32_t			send_prod;
	ntrdma_u32_t			send_cmpl;

	/* send ring buffers and consumer index */
	ntrdma_u8_t			*send_wqe_buf;
	ntrdma_dma_addr_t		send_wqe_buf_dma;
	ntrdma_size_t			send_wqe_buf_size;
	ntrdma_u8_t			*send_cqe_buf;
	ntrdma_u32_t			*send_cons_buf;
	ntrdma_dma_addr_t		send_cqe_buf_dma;
	ntrdma_size_t			send_cqe_buf_size;
	ntrdma_dma_addr_t		peer_send_wqe_buf_dma;
	ntrdma_dma_addr_t		peer_send_prod_dma;
	ntrdma_u32_t			peer_send_vbell_idx;

	/* recv ring indices */
	ntrdma_u32_t			recv_cap;
	ntrdma_u32_t			recv_post;
	ntrdma_u32_t			recv_prod;
	ntrdma_u32_t			recv_cons;
	ntrdma_u32_t			recv_cmpl;

	/* recv ring buffers */
	ntrdma_u8_t			*recv_wqe_buf;
	ntrdma_dma_addr_t		recv_wqe_buf_dma;
	ntrdma_size_t			recv_wqe_buf_size;
	ntrdma_u8_t			*recv_cqe_buf;
	ntrdma_size_t			recv_cqe_buf_size;
	ntrdma_dma_addr_t		peer_recv_wqe_buf_dma;
	ntrdma_dma_addr_t		peer_recv_prod_dma;

	/* at most one poster, producer, or completer at a time */
	NTRDMA_DECL_MUT			(send_post_lock);
	NTRDMA_DECL_SPL			(send_prod_lock);
	NTRDMA_DECL_MUT			(send_cmpl_lock);

	/* at most one poster, producer, consumer, or completer at a time */
	NTRDMA_DECL_MUT			(recv_post_lock);
	NTRDMA_DECL_SPL			(recv_prod_lock);
	NTRDMA_DECL_SPL			(recv_cons_lock);
	NTRDMA_DECL_MUT			(recv_cmpl_lock);

	NTRDMA_DECL_DPC			(send_work);

	bool				throttle;
	struct ntrdma_dma_cb		throttle_cb;
};

#define ntrdma_qp_dev(__qp) (ntrdma_res_dev(&(__qp)->res))
#define ntrdma_res_qp(__res) \
	NTRDMA_CONTAINER_OF(__res, struct ntrdma_qp, res)
#define ntrdma_ib_qp(__ib) \
	NTRDMA_CONTAINER_OF(__ib, struct ntrdma_qp, ibqp)
#define ntrdma_send_poll_qp(__poll) \
	NTRDMA_CONTAINER_OF(__poll, struct ntrdma_qp, send_poll)
#define ntrdma_recv_poll_qp(__poll) \
	NTRDMA_CONTAINER_OF(__poll, struct ntrdma_qp, recv_poll)

struct ntrdma_qp_init_attr {
	ntrdma_u32_t recv_wqe_cap;
	ntrdma_u32_t recv_wqe_sg_cap;
	ntrdma_u32_t send_wqe_cap;
	ntrdma_u32_t send_wqe_sg_cap;
};

int ntrdma_qp_init(struct ntrdma_qp *qp, struct ntrdma_pd *pd,
		   struct ntrdma_cq *recv_cq, struct ntrdma_cq *send_cq,
		   struct ntrdma_qp_init_attr *attr);

void ntrdma_qp_deinit(struct ntrdma_qp *qp);

int ntrdma_qp_modify(struct ntrdma_qp *qp);

static inline int ntrdma_qp_add(struct ntrdma_qp *qp)
{
	ntrdma_debugfs_qp_add(qp);
	return ntrdma_res_add(&qp->res);
}

static inline void ntrdma_qp_del(struct ntrdma_qp *qp)
{
	ntrdma_res_del(&qp->res);
	ntrdma_debugfs_qp_del(qp);
}

static inline void ntrdma_qp_get(struct ntrdma_qp *qp)
{
	ntrdma_res_get(&qp->res);
}

static inline void ntrdma_qp_put(struct ntrdma_qp *qp)
{
	ntrdma_res_put(&qp->res);
}

static inline void ntrdma_qp_repo(struct ntrdma_qp *qp)
{
	//FIXME: missing a put (missing in rqp work)
	//ntrdma_res_repo(&qp->res);
}

struct ntrdma_recv_wqe *ntrdma_qp_recv_wqe(struct ntrdma_qp *qp,
					   ntrdma_u32_t pos);
struct ntrdma_cqe *ntrdma_qp_recv_cqe(struct ntrdma_qp *qp,
				      ntrdma_u32_t pos);
struct ntrdma_send_wqe *ntrdma_qp_send_wqe(struct ntrdma_qp *qp,
					   ntrdma_u32_t pos);
struct ntrdma_cqe *ntrdma_qp_send_cqe(struct ntrdma_qp *qp,
				      ntrdma_u32_t pos);

int ntrdma_qp_recv_post_start(struct ntrdma_qp *qp);
void ntrdma_qp_recv_post_done(struct ntrdma_qp *qp);
void ntrdma_qp_recv_post_get(struct ntrdma_qp *qp,
			     ntrdma_u32_t *pos, ntrdma_u32_t *end,
			     ntrdma_u32_t *base);
void ntrdma_qp_recv_post_put(struct ntrdma_qp *qp,
			     ntrdma_u32_t pos, ntrdma_u32_t base);

int ntrdma_qp_send_post_start(struct ntrdma_qp *qp);
void ntrdma_qp_send_post_done(struct ntrdma_qp *qp);
void ntrdma_qp_send_post_get(struct ntrdma_qp *qp,
			     ntrdma_u32_t *pos, ntrdma_u32_t *end,
			     ntrdma_u32_t *base);
void ntrdma_qp_send_post_put(struct ntrdma_qp *qp,
			     ntrdma_u32_t pos, ntrdma_u32_t base);

/* Remote Queue Pair */
struct ntrdma_rqp {
#ifdef CONFIG_NTRDMA_DEBUGFS
	struct dentry			*debug;
#endif

	/* Ntrdma remote resource bookkeeping structure */
	struct ntrdma_rres		rres;

	/* Remote protection domain to which this remote queue pair belongs */
	struct ntrdma_rpd		*rpd;

	/* The current state of the queue pair */
	int				state;

	/* The behavior within the queue pair state */
	int				recv_error;
	int				send_error;

	/* key of connected local queue pair, or -1 if not connected */
	int				qp_key;

	/* sizes and capacities of single work queue entries */
	int				send_wqe_sg_cap;
	int				recv_wqe_sg_cap;
	ntrdma_size_t			send_wqe_size;
	ntrdma_size_t			recv_wqe_size;

	/* send ring indices */
	ntrdma_u32_t			send_cap;
	ntrdma_u32_t			send_cons;

	/* send ring buffers and consumer index */
	ntrdma_u8_t			*send_wqe_buf;
	ntrdma_u32_t			*send_prod_buf;
	ntrdma_dma_addr_t		send_wqe_buf_dma;
	ntrdma_size_t			send_wqe_buf_size;
	ntrdma_u8_t			*send_cqe_buf;
	ntrdma_dma_addr_t		send_cqe_buf_dma;
	ntrdma_size_t			send_cqe_buf_size;
	ntrdma_dma_addr_t		peer_send_cqe_buf_dma;
	ntrdma_dma_addr_t		peer_send_cons_dma;
	ntrdma_u32_t			peer_cmpl_vbell_idx;

	/* recv ring indices */
	ntrdma_u32_t			recv_cap;
	ntrdma_u32_t			recv_cons;

	/* recv ring buffers and producer index */
	ntrdma_u32_t			*recv_prod_buf;
	ntrdma_u8_t			*recv_wqe_buf;
	ntrdma_dma_addr_t		recv_wqe_buf_dma;
	ntrdma_size_t			recv_wqe_buf_size;

	/* allow one consumer at a time */
	NTRDMA_DECL_SPL			(send_cons_lock);
	NTRDMA_DECL_SPL			(recv_cons_lock);

	/* work request processing */
	struct ntrdma_vbell		send_vbell;
	ntrdma_u32_t			send_vbell_idx;
	NTRDMA_DECL_DPC			(send_work);

	bool				throttle;
	struct ntrdma_dma_cb		throttle_cb;
};

#define ntrdma_rqp_dev(__rqp) \
	ntrdma_rres_dev(&(__rqp)->rres)
#define ntrdma_rres_rqp(__rres) \
	NTRDMA_CONTAINER_OF(__rres, struct ntrdma_rqp, rres)

struct ntrdma_rqp_init_attr {
	ntrdma_u32_t recv_wqe_idx;
	ntrdma_u32_t recv_wqe_cap;
	ntrdma_u32_t recv_wqe_sg_cap;
	ntrdma_u32_t send_wqe_idx;
	ntrdma_u32_t send_wqe_cap;
	ntrdma_u32_t send_wqe_sg_cap;
	ntrdma_dma_addr_t peer_send_cqe_buf_dma;
	ntrdma_dma_addr_t peer_send_cons_dma;
	ntrdma_u32_t peer_cmpl_vbell_idx;
};

int ntrdma_rqp_init(struct ntrdma_rqp *rqp, struct ntrdma_rpd *rpd,
		    struct ntrdma_rqp_init_attr *attr);

void ntrdma_rqp_deinit(struct ntrdma_rqp *rqp);

static inline int ntrdma_rqp_add(struct ntrdma_rqp *rqp, int key)
{
	ntrdma_debugfs_rqp_add(rqp);
	return ntrdma_rres_add(&rqp->rres, key);
}

void ntrdma_rqp_del(struct ntrdma_rqp *rqp);

static inline void ntrdma_rqp_get(struct ntrdma_rqp *rqp)
{
	ntrdma_rres_get(&rqp->rres);
}

static inline void ntrdma_rqp_put(struct ntrdma_rqp *rqp)
{
	ntrdma_rres_put(&rqp->rres);
}

static inline void ntrdma_rqp_repo(struct ntrdma_rqp *rqp)
{
	//FIXME: missing a put (missing in post send)
	//ntrdma_rres_repo(&rqp->rres);
}

struct ntrdma_recv_wqe *ntrdma_rqp_recv_wqe(struct ntrdma_rqp *rqp,
					    ntrdma_u32_t pos);
struct ntrdma_send_wqe *ntrdma_rqp_send_wqe(struct ntrdma_rqp *rqp,
					    ntrdma_u32_t pos);
struct ntrdma_cqe *ntrdma_rqp_send_cqe(struct ntrdma_rqp *rqp,
				       ntrdma_u32_t pos);

struct ntrdma_qp *ntrdma_dev_qp_look(struct ntrdma_dev *dev, int key);
struct ntrdma_rqp *ntrdma_dev_rqp_look(struct ntrdma_dev *dev, int key);

#endif
