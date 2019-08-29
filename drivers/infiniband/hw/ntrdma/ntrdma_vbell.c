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

int ntrdma_dev_vbell_init(struct ntrdma_dev *dev,
			  u32 vbell_count, u32 vbell_start)
{
	int rc, i;

	dev->vbell_enable = 0;
	spin_lock_init(&dev->vbell_next_lock);
	spin_lock_init(&dev->vbell_self_lock);
	spin_lock_init(&dev->vbell_peer_lock);

	for (i = 0; i < NTB_MAX_IRQS; i++) {
		dev->vbell_work_data[i].dev = dev;
		dev->vbell_work_data[i].vec = i;
		tasklet_init(&dev->vbell_work[i],
				 ntrdma_dev_vbell_work_cb,
				 to_ptrhld(&dev->vbell_work_data[i]));
	}

	dev->vbell_count = vbell_count;
	dev->vbell_start = vbell_start;
	dev->vbell_next = vbell_start;

	dev->vbell_vec = kmalloc_node(vbell_count * sizeof(*dev->vbell_vec),
				      GFP_KERNEL, dev->node);
	if (!dev->vbell_vec) {
		rc = -ENOMEM;
		goto err_vec;
	}

	rc = ntc_bidir_buf_zalloc(&dev->vbell_buf, dev->ntc,
				vbell_count * sizeof(u32),
				GFP_KERNEL);
	if (rc < 0)
		goto err_buf;

	for (i = 0; i < vbell_count; ++i)
		ntrdma_vbell_head_init(&dev->vbell_vec[i]);

#ifdef CONFIG_NTRDMA_VBELL_USE_SEQ
	dev->vbell_peer_seq = NULL;
#endif
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

	ntc_bidir_buf_free(&dev->vbell_buf);
	kfree(dev->vbell_vec);
}

u32 ntrdma_dev_vbell_next(struct ntrdma_dev *dev)
{
	u32 idx;

	spin_lock_bh(&dev->vbell_next_lock);
	{
		idx = dev->vbell_next;

		if (dev->vbell_count <= ++dev->vbell_next)
			dev->vbell_next = dev->vbell_start;
	}
	spin_unlock_bh(&dev->vbell_next_lock);

	return idx;
}

int ntrdma_dev_vbell_enable(struct ntrdma_dev *dev,
			const struct ntc_remote_buf_desc *vbell_ntc_buf_desc,
			u32 peer_vbell_count)
{
	int rc;
#ifdef CONFIG_NTRDMA_VBELL_USE_SEQ
	int i;
#endif
	struct ntc_remote_buf peer_vbell_buf;

	rc = ntc_remote_buf_map(&peer_vbell_buf, dev->ntc,
				vbell_ntc_buf_desc);
	if (rc < 0)
		goto err_map;

	spin_lock_bh(&dev->vbell_self_lock);
	spin_lock_bh(&dev->vbell_peer_lock);
	{
		dev->vbell_enable = 1;

#ifdef CONFIG_NTRDMA_VBELL_USE_SEQ
		dev->vbell_peer_seq = kmalloc_node(peer_vbell_count *
						   sizeof(*dev->vbell_peer_seq),
						   GFP_KERNEL, dev->node);
		if (!dev->vbell_peer_seq) {
			rc = -ENOMEM;
			goto err_seq;
		}

		for (i = 0; i < peer_vbell_count; ++i) {
			dev->vbell_peer_seq[i] = 0;
		}
#endif

		dev->peer_vbell_buf = peer_vbell_buf;
		dev->peer_vbell_count = peer_vbell_count;
	}
	spin_unlock_bh(&dev->vbell_peer_lock);
	spin_unlock_bh(&dev->vbell_self_lock);

	return 0;

#ifdef CONFIG_NTRDMA_VBELL_USE_SEQ
err_seq:
	dev->vbell_enable = 0;
	spin_unlock_bh(&dev->vbell_peer_lock);
	spin_unlock_bh(&dev->vbell_self_lock);
	ntc_remote_buf_unmap(&peer_vbell_buf);
#endif
err_map:
	return rc;
}

void ntrdma_dev_vbell_disable(struct ntrdma_dev *dev)
{
	int i;
	u32 *vbell_buf;

	ntc_remote_buf_unmap(&dev->peer_vbell_buf);

	spin_lock_bh(&dev->vbell_self_lock);
	spin_lock_bh(&dev->vbell_peer_lock);
	{
		dev->vbell_enable = 0;
		vbell_buf = ntc_bidir_buf_deref(&dev->vbell_buf, 0,
						dev->vbell_count * sizeof(u32));
		for (i = 0; i < dev->vbell_count; ++i) {
			ntrdma_vbell_head_reset(&dev->vbell_vec[i]);
			vbell_buf[i] = 0;
		}
		ntc_bidir_buf_unref(&dev->vbell_buf, 0,
				dev->vbell_count * sizeof(u32));

#ifdef CONFIG_NTRDMA_VBELL_USE_SEQ
		kfree(dev->vbell_peer_seq);
		dev->vbell_peer_seq = NULL;
#endif
		dev->peer_vbell_count = 0;
	}
	spin_unlock_bh(&dev->vbell_peer_lock);
	spin_unlock_bh(&dev->vbell_self_lock);
}

void ntrdma_dev_vbell_reset(struct ntrdma_dev *dev)
{
	ntrdma_dbg(dev, "vbell reset not implemented\n");
}

#ifdef CONFIG_NTRDMA_VBELL_USE_SEQ

#define ntrdma_vbell_cond(head_seq, buf_seq) \
	((head_seq) != (buf_seq))

#define ntrdma_vbell_update_head(head_seq, buf_seq) \
	((head_seq) = (buf_seq))

#define ntrdma_vbell_update_peer(dev, idx) \
	(++(dev)->vbell_peer_seq[(idx)])

#else

#define ntrdma_vbell_cond(head_seq, buf_seq) \
	(buf_seq)

#define ntrdma_vbell_update_head(head_seq, buf_seq) \
	((buf_seq = 0), ++(head_seq))

#define ntrdma_vbell_update_peer(dev, idx) \
	(1)

#endif

static void ntrdma_dev_vbell_work(struct ntrdma_dev *dev, int vec)
{
	struct ntrdma_vbell_head *head;
	const u32 *vbell_buf;
	u32 vbell_val;
	int i;

	ntrdma_vdbg(dev, "vbell work\n");

	spin_lock_bh(&dev->vbell_self_lock);
	{
		vbell_buf = ntc_bidir_buf_const_deref(&dev->vbell_buf, 0,
						dev->vbell_count * sizeof(u32));
		for (i = 0; i < dev->vbell_count; ++i) {
			head = &dev->vbell_vec[i];
			vbell_val = vbell_buf[i];
			if (ntrdma_vbell_cond(head->seq, vbell_val)) {
				ntrdma_vbell_update_head(head->seq, vbell_val);
				ntrdma_vbell_head_fire(head);
			}
		}
	}
	spin_unlock_bh(&dev->vbell_self_lock);

	if (ntc_clear_signal(dev->ntc, vec))
		tasklet_schedule(&dev->vbell_work[vec]);
}

static void ntrdma_dev_vbell_work_cb(unsigned long ptrhld)
{
	struct vbell_work_data_s  *vbell_work_data = of_ptrhld(ptrhld);

	ntrdma_dev_vbell_work(vbell_work_data->dev, vbell_work_data->vec);
}

void ntrdma_dev_vbell_event(struct ntrdma_dev *dev, int vec)
{
	ntrdma_vdbg(dev, "vbell event on vec %d\n", vec);

	tasklet_schedule(&dev->vbell_work[vec]);
}

void ntrdma_dev_vbell_del(struct ntrdma_dev *dev,
			  struct ntrdma_vbell *vbell)
{
	spin_lock_bh(&dev->vbell_self_lock);
	{
		ntrdma_vbell_del(vbell);
	}
	spin_unlock_bh(&dev->vbell_self_lock);
}

void ntrdma_dev_vbell_clear(struct ntrdma_dev *dev,
			    struct ntrdma_vbell *vbell, u32 idx)
{
	if (unlikely(idx >= NTRDMA_DEV_VBELL_COUNT))
		return;

	spin_lock_bh(&dev->vbell_self_lock);
	{
		ntrdma_vbell_clear(&dev->vbell_vec[idx], vbell);
	}
	spin_unlock_bh(&dev->vbell_self_lock);
}

int ntrdma_dev_vbell_add(struct ntrdma_dev *dev,
			 struct ntrdma_vbell *vbell, u32 idx)
{
	int rc;

	if (unlikely(idx >= NTRDMA_DEV_VBELL_COUNT)) {
		rc = -EINVAL;
		ntrdma_err(dev,
				"vbell add failed, idx %d >= %d\n",
				idx, NTRDMA_DEV_VBELL_COUNT);
		goto err;
	}

	spin_lock_bh(&dev->vbell_self_lock);
	{
		if (!dev->vbell_enable) {
			rc = -EINVAL;
			ntrdma_err(dev,
					"vbell add failed, vbell not enabled\n");
			goto err_unlock;
		}

		rc = ntrdma_vbell_add(&dev->vbell_vec[idx], vbell);
	}
err_unlock:
	spin_unlock_bh(&dev->vbell_self_lock);
err:
	return rc;
}

int ntrdma_dev_vbell_add_clear(struct ntrdma_dev *dev,
			       struct ntrdma_vbell *vbell, u32 idx)
{
	int rc;

	spin_lock_bh(&dev->vbell_self_lock);
	{
		if (!dev->vbell_enable)
			rc = -EINVAL;
		else
			rc = ntrdma_vbell_add_clear(&dev->vbell_vec[idx], vbell);
	}
	spin_unlock_bh(&dev->vbell_self_lock);

	return rc;
}

void ntrdma_dev_vbell_peer(struct ntrdma_dev *dev,
			   void *req, u32 idx)
{
	int rc;

	spin_lock_bh(&dev->vbell_self_lock);
	{
		if (dev->vbell_enable) {
			rc = ntc_request_imm32(req,
					&dev->peer_vbell_buf, idx * sizeof(u32),
					ntrdma_vbell_update_peer(dev, idx),
					true, NULL, NULL);
			if (rc < 0)
				ntrdma_err(dev,
					"ntc_request_imm32 failed. rc=%d\n",
					rc);
		}
	}
	spin_unlock_bh(&dev->vbell_self_lock);
}

