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

#include <linux/slab.h>

#include "ntrdma_dev.h"
#include "ntrdma_vbell.h"

static void ntrdma_dev_vbell_work_cb(unsigned long ptrhld);

static struct workqueue_struct *ntrdma_workq;

int ntrdma_dev_vbell_init(struct ntrdma_dev *dev,
			  u32 vbell_count, u32 vbell_start)
{
	int rc, i;

	dev->vbell_enable = 0;
	spin_lock_init(&dev->vbell_self_lock);

	for (i = 0; i < NTB_MAX_IRQS; i++) {
		dev->vbell_work_data[i].dev = dev;
		dev->vbell_work_data[i].vec = i;
		tasklet_init(&dev->vbell_work[i],
				 ntrdma_dev_vbell_work_cb,
				 to_ptrhld(&dev->vbell_work_data[i]));
	}

	dev->vbell_count = vbell_count;
	dev->vbell_start = vbell_start;
	atomic_set(&dev->vbell_next, 0);

	dev->vbell_vec = kmalloc_node(vbell_count * sizeof(*dev->vbell_vec),
				      GFP_KERNEL, dev->node);
	if (!dev->vbell_vec) {
		ntrdma_err(dev, "failed to alloc vbell vec");
		rc = -ENOMEM;
		goto err_vec;
	}

	rc = ntc_export_buf_zalloc(&dev->vbell_buf, dev->ntc,
				vbell_count * sizeof(u32),
				GFP_KERNEL);
	if (rc < 0) {
		ntrdma_err(dev, "failed to alloc vbell buffer");
		goto err_buf;
	}

	for (i = 0; i < vbell_count; ++i)
		ntrdma_vbell_head_init(&dev->vbell_vec[i]);

	for (i = 0; i < NTRDMA_DEV_VBELL_COUNT; ++i)
		ntrdma_peer_vbell_init(&dev->peer_vbell[i]);

	dev->peer_vbell_count = 0;

	return 0;

err_buf:
	kfree(dev->vbell_vec);
err_vec:
	for (i = 0; i < NTB_MAX_IRQS; i++)
		tasklet_kill(&dev->vbell_work[i]);
	return rc;
}

void ntrdma_dev_vbell_deinit(struct ntrdma_dev *dev)
{
	int i;

	for (i = 0; i < NTB_MAX_IRQS; i++)
		tasklet_kill(&dev->vbell_work[i]);

	ntc_export_buf_free(&dev->vbell_buf);
	kfree(dev->vbell_vec);
}

u32 ntrdma_dev_vbell_next(struct ntrdma_dev *dev)
{
	u32 idx = atomic_inc_return(&dev->vbell_next) - 1;

	return dev->vbell_start + (idx % (dev->vbell_count - dev->vbell_start));
}

int ntrdma_dev_vbell_enable(struct ntrdma_dev *dev,
			const struct ntc_remote_buf_desc *vbell_ntc_buf_desc,
			u32 peer_vbell_count)
{
	int rc;
	struct ntc_remote_buf peer_vbell_buf;
	int i;

	if (unlikely(dev->vbell_enable)) {
		spin_lock_bh(&dev->vbell_self_lock);
		if (likely(dev->vbell_enable)) {
			spin_unlock_bh(&dev->vbell_self_lock);
			return 0;
		}
		spin_unlock_bh(&dev->vbell_self_lock);
	}

	rc = ntc_remote_buf_map(&peer_vbell_buf, dev->ntc,
				vbell_ntc_buf_desc);
	if (rc < 0) {
		ntrdma_err(dev, "Failed to map peer vbell buff");
		return rc;
	}

	spin_lock_bh(&dev->vbell_self_lock);

	if (unlikely(dev->vbell_enable)) {
		ntc_remote_buf_unmap(&peer_vbell_buf, dev->ntc);
		spin_unlock_bh(&dev->vbell_self_lock);
		return 0;
	}

	dev->vbell_enable = 1;

	for (i = 0; i < peer_vbell_count; ++i)
		ntrdma_peer_vbell_enable(&dev->peer_vbell[i]);

	dev->peer_vbell_buf = peer_vbell_buf;
	dev->peer_vbell_count = peer_vbell_count;

	for (i = 0; i < dev->vbell_count; ++i)
		ntrdma_vbell_head_enable(&dev->vbell_vec[i]);

	spin_unlock_bh(&dev->vbell_self_lock);

	return 0;
}

void ntrdma_dev_vbell_disable(struct ntrdma_dev *dev)
{
	int i;

	spin_lock_bh(&dev->vbell_self_lock);

	if (unlikely(!dev->vbell_enable)) {
		spin_unlock_bh(&dev->vbell_self_lock);
		return;
	}

	dev->vbell_enable = 0;

	for (i = 0; i < dev->vbell_count; ++i)
		ntrdma_vbell_head_disable(&dev->vbell_vec[i]);

	for (i = 0; i < dev->peer_vbell_count; ++i)
		ntrdma_peer_vbell_disable(&dev->peer_vbell[i]);

	ntc_remote_buf_unmap(&dev->peer_vbell_buf, dev->ntc);

	ntc_export_buf_reinit_by_zeroes(&dev->vbell_buf, 0,
			dev->vbell_count * sizeof(u32));

	dev->peer_vbell_count = 0;

 	spin_unlock_bh(&dev->vbell_self_lock);
}

void ntrdma_dev_vbell_reset(struct ntrdma_dev *dev)
{
	ntrdma_dbg(dev, "vbell reset not implemented\n");
}

static void ntrdma_dev_vbell_work(struct ntrdma_dev *dev, int vec)
{
	struct ntrdma_vbell_head *head;
	const u32 *vbell_buf;
	u32 vbell_val;
	int i;


	vbell_buf = ntc_export_buf_const_deref(&dev->vbell_buf,
			0, dev->vbell_count * sizeof(u32));
	for (i = 0; i < dev->vbell_count; ++i) {
		head = &dev->vbell_vec[i];
		vbell_val = READ_ONCE(vbell_buf[i]);
		if (READ_ONCE(head->seq) == vbell_val)
			continue;
		ntrdma_vbell_head_fire(head, vbell_val);
	}

	if (ntc_clear_signal(dev->ntc, vec))
		tasklet_schedule(&dev->vbell_work[vec]);
}

static void ntrdma_dev_vbell_work_cb(unsigned long ptrhld)
{
	struct vbell_work_data_s  *vbell_work_data = of_ptrhld(ptrhld);

	ntrdma_dev_vbell_work(vbell_work_data->dev, vbell_work_data->vec);
}

int ntrdma_dev_vbell_peer(struct ntrdma_dev *dev,
			struct ntc_dma_chan *chan, u32 idx)
{
	struct ntrdma_peer_vbell *peer_vbell = &dev->peer_vbell[idx];
	int rc = 0;

	TRACE_DEBUG("vbell peer idx %d\n", idx);

	spin_lock_bh(&peer_vbell->lock);

	if (unlikely(!peer_vbell->enabled)) {
		ntrdma_err(dev, "peer vbell disabled");
		rc = -EINVAL;
		goto exit_unlock;
	}

	rc = ntc_request_imm32(chan,
			&dev->peer_vbell_buf, idx * sizeof(u32),
			++peer_vbell->seq,
			true, NULL, NULL);

	if (unlikely(rc < 0))
		ntrdma_err(dev, "ntc_request_imm32 failed. rc=%d", rc);

exit_unlock:
	spin_unlock_bh(&peer_vbell->lock);

	return rc;
}

int ntrdma_dev_vbell_peer_direct(struct ntrdma_dev *dev, u32 idx)
{
	struct ntrdma_peer_vbell *peer_vbell = &dev->peer_vbell[idx];
	int rc = 0;

	TRACE_DEBUG("vbell peer idx %d\n", idx);

	spin_lock_bh(&peer_vbell->lock);

	if (unlikely(!peer_vbell->enabled)) {
		ntrdma_err(dev, "peer vbell disabled");
		rc = -EINVAL;
		goto exit_unlock;
	}

	rc = ntc_imm32(&dev->peer_vbell_buf, idx * sizeof(u32),
		++peer_vbell->seq);

	if (unlikely(rc < 0))
		ntrdma_err(dev, "ntc_imm32 failed. rc=%d", rc);

exit_unlock:
	spin_unlock_bh(&peer_vbell->lock);

	return rc;
}

static void vbell_tasklet_cb(void *cb_ctx)
{
	struct tasklet_struct *tasklet = cb_ctx;

	tasklet_schedule(tasklet);
}

void ntrdma_tasklet_vbell_init(struct ntrdma_dev *dev,
			struct ntrdma_vbell *vbell, u32 idx,
			struct tasklet_struct *tasklet)
{
	ntrdma_vbell_init(dev, vbell, idx, vbell_tasklet_cb, tasklet);
}

static void vbell_work_cb(void *cb_ctx)
{
	struct work_struct *work = cb_ctx;

	queue_work(ntrdma_workq, work);
}

void ntrdma_work_vbell_init(struct ntrdma_dev *dev,
			struct ntrdma_vbell *vbell, u32 idx,
			struct work_struct *work)
{
	ntrdma_vbell_init(dev, vbell, idx, vbell_work_cb, work);
}

static void vbell_napi_cb(void *cb_ctx)
{
	struct napi_struct *napi = cb_ctx;

	napi_schedule(napi);
}

void ntrdma_napi_vbell_init(struct ntrdma_dev *dev,
			struct ntrdma_vbell *vbell, u32 idx,
			struct napi_struct *napi)
{
	ntrdma_vbell_init(dev, vbell, idx, vbell_napi_cb, napi);
}

void ntrdma_tasklet_vbell_kill(struct ntrdma_vbell *vbell)
{
	struct tasklet_struct *tasklet = vbell->cb_ctx;

	ntrdma_vbell_del(vbell);
	tasklet_kill(tasklet);
}

void ntrdma_work_vbell_flush(struct ntrdma_vbell *vbell)
{
	struct work_struct *work = vbell->cb_ctx;

	ntrdma_vbell_disable(vbell);
	flush_work(work);
	flush_work(work);
}

void ntrdma_work_vbell_kill(struct ntrdma_vbell *vbell)
{
	struct work_struct *work = vbell->cb_ctx;

	ntrdma_vbell_del(vbell);
	flush_work(work);
	flush_work(work);
}

void ntrdma_napi_vbell_kill(struct ntrdma_vbell *vbell)
{
	struct napi_struct *napi = vbell->cb_ctx;

	ntrdma_vbell_del(vbell);
	netif_napi_del(napi);
}

int __init ntrdma_vbell_module_init(void)
{
	ntrdma_workq =
		alloc_workqueue("ntrdma-vbell", WQ_UNBOUND | WQ_MEM_RECLAIM |
				WQ_SYSFS, 0);
	if (!ntrdma_workq) {
		pr_err("%s failed to alloc work queue\n", __func__);
		return -ENOMEM;
	}

	return 0;
}

void ntrdma_vbell_module_deinit(void)
{
	if (ntrdma_workq) {
		destroy_workqueue(ntrdma_workq);
		ntrdma_workq = NULL;
	}
}
