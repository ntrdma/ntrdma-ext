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
#include <linux/ntc.h>

#include <rdma/ib_verbs.h>

#include "ntrdma.h"
#include "ntrdma_util.h"
#include "ntrdma_vbell.h"

#define NTRDMA_DEV_VBELL_COUNT 0x400
#define NTRDMA_DEV_VBELL_START 0x8

#define NTRDMA_DEV_MAX_SGE 4096
#define NTRDMA_DEV_MAX_QP_WR 4096
#define NTRDMA_DEV_MAX_QP 4096
#define NTRDMA_DEV_MAX_CQE 0x100
#define NTRDMA_DEV_MAX_CQ 0x100
#define NTRDMA_DEV_MAX_MR 0x400
#define NTRDMA_DEV_MAX_PD 0x100
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
	u64 tx_cqes;

	u64 cqes_notified;
	u64 cqes_polled;
	u64 cqes_armed;
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

	struct ntc_dma_chan		*dma_chan;

	/* debugfs */
	struct dentry			*debug;
	struct mutex			debugfs_lock;

	/* protocol version */
	u32				version;
	u32				latest_version;

	/* virtual doorbells synchronization */
	int				vbell_enable;
	struct tasklet_struct		vbell_work[NTB_MAX_IRQS];
	struct vbell_work_data_s	vbell_work_data[NTB_MAX_IRQS];
	spinlock_t			vbell_next_lock;
	spinlock_t			vbell_self_lock;

	/* local virtual doorbells */

	u32				vbell_count;
	u32				vbell_start;
	u32			vbell_next; /* Protected by vbell_next_lock */
	struct ntrdma_vbell_head	*vbell_vec;
	struct ntc_export_buf		vbell_buf;

	/* peer virtual doorbells */

	u32				*vbell_peer_seq;
	struct ntc_remote_buf		peer_vbell_buf;
	int				peer_vbell_count;

	/* commands to affect remote resources */

	bool				cmd_ready;

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
	struct list_head		cmd_pend_list;
	struct list_head		cmd_post_list;
	wait_queue_head_t		cmd_send_cond;
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
	bool	is_cmd_prep;

	atomic_t qp_num;
	atomic_t cq_num;
	atomic_t mr_num;
	atomic_t pd_num;
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

#define ntrdma_dbg(__dev, __args...) \
	dev_dbg(&(__dev)->ntc->dev, ## __args)

#define ntrdma_err(__dev, __fmt, __args...) \
	dev_err(&(__dev)->ntc->dev, "%s: %d: ERROR: " \
			__fmt, __func__, __LINE__, ## __args)

#define ntrdma_info(__dev, __fmt, __args...) \
	dev_info(&(__dev)->ntc->dev, "%s: %d: " \
			__fmt, __func__, __LINE__, ## __args)

#define ntrdma_info_ratelimited(__dev, __fmt, __args...)	\
	do {													\
		static DEFINE_RATELIMIT_STATE(_rs,					\
				DEFAULT_RATELIMIT_INTERVAL,					\
				DEFAULT_RATELIMIT_BURST);					\
				if (__ratelimit(&_rs))						\
					ntrdma_info(__dev, __fmt, ## __args);		\
	} while (0)

#define ntrdma_vdbg(__dev, __args...) \
	dev_vdbg(&(__dev)->ntc->dev, ## __args)

static inline void ntrdma_vbell_del(struct ntrdma_vbell *vbell)
{
	struct ntrdma_dev *dev = vbell->dev;

	spin_lock_bh(&dev->vbell_self_lock);

	if (!vbell->arm)
		goto unlock;
	list_del(&vbell->entry);
	vbell->arm = false;

 unlock:
	spin_unlock_bh(&dev->vbell_self_lock);
}

static inline void ntrdma_vbell_clear(struct ntrdma_vbell *vbell)
{
	struct ntrdma_dev *dev = vbell->dev;
	struct ntrdma_vbell_head *head = &dev->vbell_vec[vbell->idx];

	spin_lock_bh(&dev->vbell_self_lock);
	vbell->seq = head->seq;
	spin_unlock_bh(&dev->vbell_self_lock);
}

static inline int ntrdma_vbell_add(struct ntrdma_vbell *vbell)
{
	struct ntrdma_dev *dev = vbell->dev;
	struct ntrdma_vbell_head *head = &dev->vbell_vec[vbell->idx];
	int rc = 0;

	spin_lock_bh(&dev->vbell_self_lock);

	if (unlikely(!dev->vbell_enable)) {
		rc = -EINVAL;
		ntrdma_err(dev, "vbell disabled");
		TRACE_DATA("vbell disabled");
		goto unlock;
	}

	if (vbell->arm)
		goto unlock;

	if (vbell->seq != head->seq) {
		vbell->seq = head->seq;
		rc = -EAGAIN;
		goto unlock;
	}

	list_add_tail(&vbell->entry, &head->list);
	vbell->arm = true;

 unlock:
	spin_unlock_bh(&dev->vbell_self_lock);

	return rc;
}

static inline int ntrdma_vbell_readd(struct ntrdma_vbell *vbell)
{
	struct ntrdma_dev *dev = vbell->dev;
	struct ntrdma_vbell_head *head = &dev->vbell_vec[vbell->idx];
	int rc = 0;

	spin_lock_bh(&dev->vbell_self_lock);

	if (unlikely(!dev->vbell_enable)) {
		ntrdma_err(dev, "vbell disabled");
		TRACE_DATA("vbell disabled");
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
	spin_unlock_bh(&dev->vbell_self_lock);

	return rc;
}

static inline int ntrdma_vbell_add_clear(struct ntrdma_vbell *vbell)
{
	struct ntrdma_dev *dev = vbell->dev;
	struct ntrdma_vbell_head *head = &dev->vbell_vec[vbell->idx];
	int rc = 0;

	spin_lock_bh(&dev->vbell_self_lock);

	if (unlikely(!dev->vbell_enable)) {
		ntrdma_err(dev, "vbell disabled");
		TRACE_DATA("vbell disabled");
		rc = -EINVAL;
		goto unlock;
	}

	if (vbell->arm)
		goto unlock;

	vbell->seq = head->seq;
	list_add_tail(&vbell->entry, &head->list);
	vbell->arm = true;

 unlock:
	spin_unlock_bh(&dev->vbell_self_lock);

	return rc;
}

static inline void ntrdma_dev_vbell_event(struct ntrdma_dev *dev, int vec)
{
	ntrdma_vdbg(dev, "vbell event on vec %d\n", vec);

	tasklet_schedule(&dev->vbell_work[vec]);
}

#endif
