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
#include <linux/ratelimit.h>
#include <linux/percpu.h>
#include "ntc.h"

#include <rdma/ib_verbs.h>

#include "ntrdma.h"
#include "ntrdma_util.h"
#include "ntrdma_vbell.h"

#define NTRDMA_DEV_VBELL_COUNT 0x400
#define NTRDMA_DEV_VBELL_START 0x8

#define NTRDMA_DEV_MAX_SGE 4096
#define NTRDMA_DEV_MAX_QP_WR 4096
#define NTRDMA_DEV_MAX_QP 4096
#define NTRDMA_DEV_MAX_CQE 0x400
#define NTRDMA_DEV_MAX_CQ 0x400
#define NTRDMA_DEV_MAX_MR 0x400
#define NTRDMA_DEV_MAX_PD 0x400
#define NTRDMA_DEV_MAX_INLINE_DATA 8

struct vbell_work_data_s {
	struct ntrdma_dev *dev;
	int vec;
};

struct ntrdma_dev_counters {
	u64 qp_send_work_bytes;
	u64 post_send_bytes;
	u64 post_send_wqes;
	u64 post_send_wqes_signalled;
	u64 post_send_wqes_ioctl;
	u64 post_send_wqes_ioctl_signalled;
	u64 tx_cqes;

	u64 cqes_notified;
	u64 cqes_polled_s;
	u64 cqes_polled_ns;
	u64 cqes_polled_ioctl_s;
	u64 cqes_polled_ioctl_ns;
	u64 cqes_armed;

	u64 poll_cq_count;
	u64 poll_cq_count_ioctl;
};

/* NTRDMA device local resources */
struct ntrdma_dev_res {
	struct mutex			res_lock;
	struct list_head		mr_list;
	struct list_head		qp_list;
	struct ntrdma_kvec		mr_vec;
	struct ntrdma_kvec		qp_vec;
	struct list_head		cq_list;
	struct list_head		pd_list;
	u32						pd_next_key;
};

struct ntrdma_dev_rres {
	struct mutex			rres_lock;
	struct list_head		rres_list;
	struct ntrdma_vec		rmr_vec;
	struct ntrdma_vec		rqp_vec;
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
	struct work_struct		ntc_link_reset_work;

	/* debugfs */
	struct dentry			*debug;
	struct mutex			debugfs_lock;

	/* protocol version */
	u32				version;
	u32				latest_version;

	/* virtual doorbells synchronization */
	int			vbell_enable; /* Protected by vbell_self_lock */
	struct tasklet_struct		vbell_work[NTB_MAX_IRQS];
	struct vbell_work_data_s	vbell_work_data[NTB_MAX_IRQS];
	spinlock_t			vbell_self_lock;

	/* local virtual doorbells */

	u32				vbell_count;
	u32				vbell_start;
	atomic_t			vbell_next;
	struct ntrdma_vbell_head	*vbell_vec;
	struct ntc_export_buf		vbell_buf;

	/* peer virtual doorbells */

	struct ntrdma_peer_vbell	peer_vbell[NTRDMA_DEV_VBELL_COUNT];
	struct ntc_remote_buf		peer_vbell_buf;
	int				peer_vbell_count;

	/* commands to affect remote resources */

	bool				cmd_send_ready;
	bool				cmd_recv_ready;

	/* command recv work */
	struct mutex			cmd_recv_lock;
	struct work_struct		cmd_recv_work;
	struct ntrdma_vbell		cmd_recv_vbell;

	/* command recv ring indices */
	u32				cmd_recv_cap;
	u32				cmd_recv_cons;

	/* command recv ring buffers and producer index */
	struct ntc_export_buf		cmd_recv_buf;
	struct ntc_local_buf		cmd_recv_rsp_buf;
	struct ntc_remote_buf		peer_cmd_send_rsp_buf;
	u64				peer_send_cons_shift;
	u32				peer_cmd_send_vbell_idx;

	/* command recv work */
	struct list_head	cmd_pend_list; /* Protected by cmd_send_lock */
	struct list_head	cmd_post_list; /* Protected by cmd_send_lock */
	struct mutex			cmd_send_lock;
	struct work_struct		cmd_send_work;
	struct ntrdma_vbell		cmd_send_vbell;

	/* command send ring indices */
	u32				cmd_send_cap;
	u32				cmd_send_prod;
	u32				cmd_send_cmpl;

	/* command send ring buffers and consumer index */
	struct ntc_local_buf		cmd_send_buf;
	struct ntc_export_buf		cmd_send_rsp_buf;
	struct ntc_remote_buf		peer_cmd_recv_buf;
	u64				peer_recv_prod_shift;
	u32				peer_cmd_recv_vbell_idx;
	int				is_cmd_hello_done;
	/* hello buffers */
	const u8 *hello_local_buf;
	int hello_local_buf_size;
	u8 __iomem *hello_peer_buf;
	int hello_peer_buf_size;

	/* rdma resource synchronization state */
	struct ntrdma_dev_res res;
	struct ntrdma_dev_rres rres;



	/* virtual ethernet device */
	struct ntrdma_eth		*eth;
	bool	is_cmd_prep;

	atomic_t qp_num;
	atomic_t cq_num;
	atomic_t mr_num;
	atomic_t pd_num;

	struct mutex cmd_modify_qp_recv_lock;
};

inline u32 ntrdma_dev_cmd_send_cons(struct ntrdma_dev *dev);
inline u32 ntrdma_dev_cmd_recv_prod(struct ntrdma_dev *dev);

#define ntrdma_ib_dev(__ibdev) \
	container_of(__ibdev, struct ntrdma_dev, ibdev)

#define ntrdma_ntc_link_reset_work_dev(__ws) \
	container_of(__ws, struct ntrdma_dev, ntc_link_reset_work)
#define ntrdma_cmd_send_work_dev(__ws) \
	container_of(__ws, struct ntrdma_dev, cmd_send_work)
#define ntrdma_cmd_recv_work_dev(__ws) \
	container_of(__ws, struct ntrdma_dev, cmd_recv_work)

#define ntrdma_dbg(__dev, ...)			\
	ntc_dbg((__dev)->ntc, ##__VA_ARGS__)

#define ntrdma_err(__dev, ...)			\
	ntc_err((__dev)->ntc, ##__VA_ARGS__)

#define ntrdma_info(__dev, ...)			\
	ntc_info((__dev)->ntc, ##__VA_ARGS__)

#define ntrdma_vdbg(__dev, ...)			\
	ntc_vdbg((__dev)->ntc, ##__VA_ARGS__)

#define ntrdma_info_ratelimited(__dev, __fmt, __args...)		\
	do {								\
		static DEFINE_RATELIMIT_STATE(_rs,			\
					DEFAULT_RATELIMIT_INTERVAL,	\
					DEFAULT_RATELIMIT_BURST);	\
		if (__ratelimit(&_rs))					\
			ntrdma_info(__dev, __fmt, ## __args);		\
	} while (0)

static inline void ntrdma_vbell_enable(struct ntrdma_vbell *vbell)
{
	struct ntrdma_dev *dev = vbell->dev;
	struct ntrdma_vbell_head *head = &dev->vbell_vec[vbell->idx];

	spin_lock_bh(&head->lock);
	vbell->enabled = true;
	spin_unlock_bh(&head->lock);
}

static inline void ntrdma_vbell_disable(struct ntrdma_vbell *vbell)
{
	struct ntrdma_dev *dev = vbell->dev;
	struct ntrdma_vbell_head *head = &dev->vbell_vec[vbell->idx];

	spin_lock_bh(&head->lock);
	vbell->enabled = false;
	spin_unlock_bh(&head->lock);
}

static inline void ntrdma_vbell_del(struct ntrdma_vbell *vbell)
{
	struct ntrdma_dev *dev = vbell->dev;
	struct ntrdma_vbell_head *head = &dev->vbell_vec[vbell->idx];

	spin_lock_bh(&head->lock);

	vbell->enabled = false;
	vbell->alive = false;

	if (!vbell->arm)
		goto unlock;
	list_del(&vbell->entry);
	vbell->arm = false;

 unlock:
	spin_unlock_bh(&head->lock);
}

static inline void ntrdma_vbell_clear(struct ntrdma_vbell *vbell)
{
	struct ntrdma_dev *dev = vbell->dev;
	struct ntrdma_vbell_head *head = &dev->vbell_vec[vbell->idx];

	spin_lock_bh(&head->lock);
	vbell->seq = head->seq;
	spin_unlock_bh(&head->lock);
}

static inline int _ntrdma_vbell_readd(const char *caller, int line,
				struct ntrdma_vbell *vbell)
{
	struct ntrdma_dev *dev = vbell->dev;
	struct ntrdma_vbell_head *head = &dev->vbell_vec[vbell->idx];
	int rc = 0;

	spin_lock_bh(&head->lock);

	if (unlikely(!vbell->alive)) {
		ntrdma_err(dev, "this vbell is dead @%s:%d", caller, line);
		rc = -EINVAL;
		goto unlock;
	}

	if (vbell->arm)
		goto unlock;

	if (vbell->seq != head->seq) {
		vbell->seq = head->seq;
		vbell->cb_fn(vbell->cb_ctx);
	} else {
		list_add_tail(&vbell->entry, &head->list);
		vbell->arm = true;
	}

 unlock:
	spin_unlock_bh(&head->lock);

	return rc;
}
#define ntrdma_vbell_readd(...) \
	_ntrdma_vbell_readd(__func__, __LINE__, ##__VA_ARGS__)

static inline int ntrdma_vbell_add_clear(struct ntrdma_vbell *vbell)
{
	struct ntrdma_dev *dev = vbell->dev;
	struct ntrdma_vbell_head *head = &dev->vbell_vec[vbell->idx];
	int rc = 0;

	spin_lock_bh(&head->lock);

	if (unlikely(!vbell->alive)) {
		ntrdma_err(dev, "this vbell is dead");
		rc = -EINVAL;
		goto unlock;
	}

	if (vbell->arm)
		goto unlock;

	vbell->seq = head->seq;
	list_add_tail(&vbell->entry, &head->list);
	vbell->arm = true;

 unlock:
	spin_unlock_bh(&head->lock);

	return rc;
}

static inline int _ntrdma_vbell_trigger(const char *caller, int line,
					struct ntrdma_vbell *vbell)
{
	struct ntrdma_dev *dev = vbell->dev;
	struct ntrdma_vbell_head *head = &dev->vbell_vec[vbell->idx];
	int rc = 0;

	spin_lock_bh(&head->lock);

	if (unlikely(!vbell->enabled)) {
		ntrdma_err(dev, "this vbell disabled @%s:%d", caller, line);
		rc = -EINVAL;
		goto unlock;
	}

	if (vbell->arm) {
		list_del(&vbell->entry);
		vbell->arm = false;
	}
	vbell->seq = head->seq;
	vbell->cb_fn(vbell->cb_ctx);

 unlock:
	spin_unlock_bh(&head->lock);

	return rc;
}
#define ntrdma_vbell_trigger(...) \
	_ntrdma_vbell_trigger(__func__, __LINE__, ##__VA_ARGS__)

static inline void ntrdma_vbell_head_fire_locked(struct ntrdma_vbell_head *head)
{
	struct ntrdma_vbell *vbell;
	struct ntrdma_vbell *vbell_tmp;

	list_for_each_entry_safe(vbell, vbell_tmp, &head->list, entry) {
		if (unlikely(!vbell->enabled))
			continue;
		list_del(&vbell->entry);
		vbell->arm = false;
		vbell->seq = head->seq;
		vbell->cb_fn(vbell->cb_ctx);
	}
}

static inline void ntrdma_vbell_head_fire(struct ntrdma_vbell_head *head,
					u32 vbell_val)
{
	spin_lock_bh(&head->lock);

	if (unlikely(!head->enabled))
		goto unlock;

	if (head->seq == vbell_val)
		goto unlock;

	WRITE_ONCE(head->seq, vbell_val);

	ntrdma_vbell_head_fire_locked(head);

 unlock:
	spin_unlock_bh(&head->lock);
}

static inline void ntrdma_vbell_head_enable(struct ntrdma_vbell_head *head)
{
	spin_lock_bh(&head->lock);

	head->enabled = 1;

	ntrdma_vbell_head_fire_locked(head);

	spin_unlock_bh(&head->lock);
}

static inline void ntrdma_vbell_head_disable(struct ntrdma_vbell_head *head)
{
	spin_lock_bh(&head->lock);

	head->enabled = 0;

	WRITE_ONCE(head->seq, 0);

	ntrdma_vbell_head_fire_locked(head);

	spin_unlock_bh(&head->lock);
}

#endif
