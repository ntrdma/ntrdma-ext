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

#include <rdma/ib_verbs.h>
#include <asm/tsc.h>
#include "ntrdma_cmd.h"
#include "ntrdma_ring.h"
#include "ntrdma_res.h"

struct ntrdma_dev;
struct ntrdma_qp;

/* Queue Pair Entry in Completion Queue */
struct ntrdma_poll {
	struct list_head		cq_entry;

	void (*poll_cqe)(struct ntrdma_poll *poll,
			struct ntrdma_cqe *outcqe, u32 pos);

	int (*poll_start_and_get)(struct ntrdma_poll *poll,
				  struct ntrdma_qp **qp,
				  u32 *pos, u32 *end, u32 *base);

	void (*poll_put_and_done)(struct ntrdma_poll *poll,
				  u32 pos, u32 base);
};

struct ntrdma_qp_cmd_cb {
	struct ntrdma_cmd_cb cb;
	struct ntrdma_qp *qp;
};

#include <rdma/iw_cm.h>

/* Queue Pair */
struct ntrdma_qp {
	/* Ofed qp structure */
	struct ib_qp			ibqp;

	struct ntrdma_qp_cmd_cb		enable_qpcb;

	/* debugfs */
	struct dentry			*debug;

	/* Ntrdma resource bookkeeping structure */
	struct ntrdma_res		res;

	/* Completion queue for recv requests */
	struct ntrdma_cq		*recv_cq;
	struct ntrdma_poll		recv_poll;

	/* Completion queue for send requests */
	struct ntrdma_cq		*send_cq;
	struct ntrdma_poll		send_poll;

	struct ntc_dma_chan		*dma_chan;

	struct page			*send_page;

	/* Same as ibqp.qp_type */
	u32				qp_type;

	/* Protection domain key */
	u32				pd_key;
	/* Queue pair access flags */
	u32			access;

	/* The current ib_qp_state of the queue pair */
	atomic_t		state;

	/* The behavior within the queue pair state */
	int			recv_abort;
	int			recv_aborting;
	int			recv_abort_first;
	int			send_abort;    /* wrap up mode, set error
						* completion for all wqe that
						* did not get completion yet
						*/
	int			send_aborting; /* Entering to wrap up mode,
						* make sure all wqe that have
						* pending completion will
						* process it, before the
						* send_abort set
						*/
	int			send_abort_first;

	/* key of connected remote queue pair, or -1 if not connected */
	u32			rqp_key;

	/* sizes and capacities of single work queue entries */
	int			send_wqe_sg_cap;
	int			send_wqe_inline_cap;
	int			recv_wqe_sg_cap;
	size_t		send_wqe_size;
	size_t		recv_wqe_size;

	/* send ring indices */
	u32				send_cap;
	u32				send_post;
	u32				send_prod;
	u32				send_cmpl;

	/* send ring buffers and consumer index */
	struct ntc_local_buf		send_wqe_buf;
	struct ntc_export_buf		send_cqe_buf;
	struct ntc_remote_buf		peer_send_wqe_buf;
	u64				peer_send_prod_shift;
	u32				peer_send_vbell_idx;

	/* recv ring indices */
	u32				recv_cap;
	u32				recv_post;
	u32				recv_prod;
	u32				recv_cons;
	u32				recv_cmpl;

	/* recv ring buffers */
	struct ntc_local_buf		recv_wqe_buf;
	u8				*recv_cqe_buf;
	size_t			recv_cqe_buf_size;
	struct ntc_remote_buf		peer_recv_wqe_buf;
	u64				peer_recv_prod_shift;

	/* at most one poster, producer, or completer at a time */
	spinlock_t		send_post_slock;
	spinlock_t		send_prod_lock;
	struct mutex	send_cmpl_lock;

	/* at most one poster, producer, consumer, or completer at a time */
	struct mutex	recv_post_lock;
	spinlock_t		recv_prod_lock;
	spinlock_t		recv_cons_lock;
	struct mutex	recv_cmpl_lock;
	struct iw_cm_id *cm_id;
	struct mutex cm_lock;
	int ntrdma_cm_state;
	struct work_struct qp_work;
};

inline u32 ntrdma_qp_send_cons(struct ntrdma_qp *qp);

#define ntrdma_qp_dev(__qp) (ntrdma_res_dev(&(__qp)->res))
#define ntrdma_res_qp(__res) \
	container_of(__res, struct ntrdma_qp, res)
#define ntrdma_ib_qp(__ib) \
	container_of(__ib, struct ntrdma_qp, ibqp)
#define ntrdma_send_poll_qp(__poll) \
	container_of(__poll, struct ntrdma_qp, send_poll)
#define ntrdma_recv_poll_qp(__poll) \
	container_of(__poll, struct ntrdma_qp, recv_poll)

#define ntrdma_qp_dbg(__qp, __args...)			\
	ntrdma_dbg(ntrdma_qp_dev(__qp), ## __args)

#define ntrdma_qp_vdbg(__qp, __args...)			\
	ntrdma_vdbg(ntrdma_qp_dev(__qp), ## __args)

#define ntrdma_qp_err(__qp, __fmt, __args...)			\
	ntrdma_err(ntrdma_qp_dev(__qp), __fmt, ## __args)

#define ntrdma_qp_info(__qp, __fmt, __args...)			\
	ntrdma_info(ntrdma_qp_dev(__qp), __fmt, ## __args)

#define ntrdma_qp_info_ratelimited(__qp, __fmt, __args...)		\
	ntrdma_info_ratelimited(ntrdma_qp_dev(__qp), __fmt, ## __args)

struct ntrdma_qp_init_attr {
	u32 pd_key;
	u32 recv_wqe_cap;
	u32 recv_wqe_sg_cap;
	u32 send_wqe_cap;
	u32 send_wqe_inline_cap;
	u32 send_wqe_sg_cap;
	u32 qp_type;
};

int ntrdma_qp_init(struct ntrdma_qp *qp, struct ntrdma_dev *dev,
		   struct ntrdma_cq *recv_cq, struct ntrdma_cq *send_cq,
		   struct ntrdma_qp_init_attr *attr);

void ntrdma_qp_deinit(struct ntrdma_qp *qp, struct ntrdma_dev *dev);

int ntrdma_modify_qp_remote(struct ntrdma_qp *qp);
#ifdef NTRDMA_QP_DEBUG
#define ntrdma_qp_get(qp) \
	ntrdma_dbg(ntrdma_qp_dev(qp), "calling ntrdma_qp_get. qp %p QP %d - caller: %s , line:%u\n", qp, qp->res.key, __func__, __LINE__); \
	_ntrdma_qp_get(qp)
#else
#define ntrdma_qp_get(qp) \
	_ntrdma_qp_get(qp)

#endif
static inline void _ntrdma_qp_get(struct ntrdma_qp *qp)
{
	ntrdma_res_get(&qp->res);
}

#ifdef NTRDMA_QP_DEBUG
#define ntrdma_qp_put(qp) \
		ntrdma_dbg(ntrdma_qp_dev(qp), "calling ntrdma_qp_put. qp %p QP %d - caller: %s , line:%u\n", qp, qp->res.key, __func__, __LINE__); \
		_ntrdma_qp_put(qp)
#else
#define ntrdma_qp_put(qp) \
		_ntrdma_qp_put(qp)
#endif

void _ntrdma_qp_put(struct ntrdma_qp *qp);

void ntrdma_qp_send_stall(struct ntrdma_qp *qp, struct ntrdma_rqp *rqp, const char *from, int line);

inline struct ntrdma_recv_wqe *ntrdma_qp_recv_wqe(struct ntrdma_qp *qp,
						u32 pos);
inline struct ntrdma_cqe *ntrdma_qp_recv_cqe(struct ntrdma_qp *qp,
					u32 pos);
inline struct ntrdma_send_wqe *ntrdma_qp_send_wqe(struct ntrdma_qp *qp,
						u32 pos);

inline void ntrdma_qp_set_stats(struct ntrdma_qp *qp, u32 pos);

inline
const struct ntrdma_cqe *ntrdma_qp_send_cqe(struct ntrdma_qp *qp, u32 pos);

static inline bool ntrdma_qp_recv_post_get(struct ntrdma_qp *qp,
					u32 *pos, u32 *end, u32 *base)
{
	return ntrdma_ring_produce(qp->recv_post, qp->recv_cmpl, qp->recv_cap,
				pos, end, base);
}

static inline void ntrdma_qp_recv_post_put(struct ntrdma_qp *qp,
					u32 pos, u32 base)
{
	qp->recv_post = ntrdma_ring_update(pos, base, qp->recv_cap);
}

bool ntrdma_qp_send_work(struct ntrdma_qp *qp);

static inline bool ntrdma_qp_send_post_get(struct ntrdma_qp *qp,
					u32 *pos, u32 *end, u32 *base)
{
	return ntrdma_ring_produce(qp->send_post, qp->send_cmpl, qp->send_cap,
				pos, end, base);
}

static inline void ntrdma_qp_send_post_put(struct ntrdma_qp *qp,
					u32 pos, u32 base)
{
	qp->send_post = ntrdma_ring_update(pos, base, qp->send_cap);
}

static inline void ntrdma_qp_recv_prod_get(struct ntrdma_qp *qp,
					u32 *pos, u32 *end, u32 *base)
{
	ntrdma_ring_consume(qp->recv_post, qp->recv_prod, qp->recv_cap,
			pos, end, base);
}

static inline void ntrdma_qp_recv_prod_put(struct ntrdma_qp *qp,
					u32 pos, u32 base)
{
	qp->recv_prod = ntrdma_ring_update(pos, base, qp->recv_cap);
}

void ntrdma_qp_enable(struct ntrdma_qp *qp);
void ntrdma_qp_reset(struct ntrdma_qp *qp);

/* Remote Queue Pair */
struct ntrdma_rqp {
	/* debugfs */
	struct dentry			*debug;

	/* Ntrdma remote resource bookkeeping structure */
	struct ntrdma_rres		rres;

	/* Protectoin domain key */
	u32				pd_key;
	/* Queue pair access flags */
	u32				access;

	/* The current state of the queue pair */
	int				state;

	/* key of connected local queue pair, or -1 if not connected */
	u32				qp_key;

	/* sizes and capacities of single work queue entries */
	int				send_wqe_sg_cap;
	int				send_wqe_inline_cap;
	int				recv_wqe_sg_cap;
	size_t				send_wqe_size;
	size_t				recv_wqe_size;

	/* send ring indices */
	u32				send_cap;
	u32				send_cons;

	/* send ring buffers and consumer index */
	struct ntc_export_buf		send_wqe_buf;
	struct ntc_local_buf		send_cqe_buf;
	struct ntc_remote_buf		peer_send_cqe_buf;
	u64				peer_send_cons_shift;
	u32				peer_cmpl_vbell_idx;

	/* recv ring indices */
	u32				recv_cap;
	u32				recv_cons;

	/* recv ring buffers and producer index */
	struct ntc_export_buf		recv_wqe_buf;

	/* allow one consumer at a time */
	spinlock_t			send_cons_lock;
	spinlock_t			recv_cons_lock;

	/* work request processing */
	struct ntrdma_vbell		send_vbell;
	struct tasklet_struct		send_work;

	struct mutex lock;
};

inline u32 ntrdma_rqp_send_prod(struct ntrdma_rqp *rqp);
inline u32 ntrdma_rqp_recv_prod(struct ntrdma_rqp *rqp);

#define ntrdma_rqp_dev(__rqp) \
	ntrdma_rres_dev(&(__rqp)->rres)
#define ntrdma_rres_rqp(__rres) \
	container_of(__rres, struct ntrdma_rqp, rres)

#define ntrdma_rqp_dbg(__qp, __args...)			\
	ntrdma_dbg(ntrdma_rqp_dev(__qp), ## __args)

#define ntrdma_rqp_vdbg(__qp, __args...)			\
	ntrdma_vdbg(ntrdma_rqp_dev(__qp), ## __args)

#define ntrdma_rqp_err(__qp, __fmt, __args...)			\
	ntrdma_err(ntrdma_rqp_dev(__qp), __fmt, ## __args)

#define ntrdma_rqp_info(__qp, __fmt, __args...)			\
	ntrdma_info(ntrdma_rqp_dev(__qp), __fmt, ## __args)

#define ntrdma_rqp_info_ratelimited(__qp, __fmt, __args...)		\
	ntrdma_info_ratelimited(ntrdma_rqp_dev(__qp), __fmt, ## __args)

struct ntrdma_rqp_init_attr {
	u32 pd_key;
	u32 recv_wqe_idx;
	u32 recv_wqe_cap;
	u32 recv_wqe_sg_cap;
	u32 send_wqe_idx;
	u32 send_wqe_cap;
	u32 send_wqe_sg_cap;
	u32 send_wqe_inline_cap;
	u32 peer_cmpl_vbell_idx;
	struct ntc_remote_buf peer_send_cqe_buf;
	u64 peer_send_cons_shift;
};

int ntrdma_rqp_init(struct ntrdma_rqp *rqp, struct ntrdma_dev *dev,
		    struct ntrdma_rqp_init_attr *attr, u32 key);

void ntrdma_rqp_deinit(struct ntrdma_rqp *rqp);

static inline int ntrdma_rqp_add(struct ntrdma_rqp *rqp)
{
	ntrdma_debugfs_rqp_add(rqp);
	return ntrdma_rres_add(&rqp->rres);
}

void ntrdma_rqp_del(struct ntrdma_rqp *rqp);

static inline void ntrdma_rqp_get(struct ntrdma_rqp *rqp)
{
	ntrdma_rres_get(&rqp->rres);
}

inline void ntrdma_rqp_put(struct ntrdma_rqp *rqp);

inline inline const struct ntrdma_recv_wqe *ntrdma_rqp_recv_wqe(struct ntrdma_rqp *rqp,
								u32 pos);
const struct ntrdma_send_wqe *ntrdma_rqp_send_wqe(struct ntrdma_rqp *rqp,
						u32 pos);
inline struct ntrdma_cqe *ntrdma_rqp_send_cqe(struct ntrdma_rqp *rqp,
					u32 pos);

struct ntrdma_qp *_ntrdma_dev_qp_look_and_get(struct ntrdma_dev *dev, u32 key);
#ifdef NTRDMA_QP_DEBUG
static inline struct ntrdma_qp *__ntrdma_dev_qp_look_and_get(struct ntrdma_dev *dev, int key, const char *func, const int line)
{
	ntrdma_dbg(dev, "calling ntrdma_dev_qp_look_and_get. QP %d - caller: %s , line:%u\n", key, func, line);
	return _ntrdma_dev_qp_look_and_get(dev, key);
}
#define ntrdma_dev_qp_look_and_get(dev, key) \
		__ntrdma_dev_qp_look_and_get(dev, key, __func__, __LINE__)
#else
#define ntrdma_dev_qp_look_and_get(dev, key) \
		_ntrdma_dev_qp_look_and_get(dev, key)
#endif

struct ntrdma_rqp *_ntrdma_dev_rqp_look_and_get(struct ntrdma_dev *dev, u32 key);
#undef NTRDMA_QP_DEBUG

#ifdef NTRDMA_QP_DEBUG
static inline struct ntrdma_rqp *__ntrdma_dev_rqp_look_and_get(struct ntrdma_dev *dev, int key, const char *func, const int line)
{
		ntrdma_dbg(dev, "calling ntrdma_dev_rqp_look_and_get. QP %d - caller: %s , line:%u\n", key, func, line);
		return _ntrdma_dev_rqp_look_and_get(dev, key);
}
#define ntrdma_dev_rqp_look_and_get(dev, key) \
	__ntrdma_dev_rqp_look_and_get(dev, key, __func__, __LINE__)
#else
#define ntrdma_dev_rqp_look_and_get(dev, key) \
		_ntrdma_dev_rqp_look_and_get(dev, key)
#endif

static inline bool is_state_valid(int state)
{
	return ((state >= IB_QPS_RESET) && (state <= IB_QPS_ERR));
}

static inline bool is_state_send_ready(int state)
{
	return ((state == IB_QPS_RTR) || (state == IB_QPS_RTS));
}

static inline bool is_state_recv_ready(int state)
{
	return ((state == IB_QPS_RTR) || (state == IB_QPS_RTS) ||
			(state == IB_QPS_SQE));
}

static inline bool is_state_error(int state)
{
	return ((state == IB_QPS_ERR) || (state == IB_QPS_SQE));
}

static inline bool is_state_out_of_reset(int state)
{
	return (is_state_valid(state) && !is_state_error(state) &&
			(state != IB_QPS_RESET));
}

static inline bool ntrdma_qp_is_send_ready(struct ntrdma_qp *qp)
{
	return is_state_send_ready(atomic_read(&qp->state));
}

static inline void move_to_err_state_d(struct ntrdma_qp *qp, const char *s,
		int line)
{
	TRACE("Move QP %d to err state from %s, line %d\n",
			qp->res.key, s, line);
	atomic_set(&qp->state, IB_QPS_ERR);
}

struct ntrdma_rqp *ntrdma_alloc_rqp(gfp_t gfp, struct ntrdma_dev *dev);
void ntrdma_free_rqp(struct ntrdma_rqp *rqp);

inline int ntrdma_qp_rdma_write(struct ntrdma_qp *qp,
				struct ntrdma_send_wqe *wqe);

void ntrdma_qp_recv_work(struct ntrdma_qp *qp);

#endif
