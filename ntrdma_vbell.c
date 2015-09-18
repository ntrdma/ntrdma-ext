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

#include "ntrdma_os.h"
#include "ntrdma_map.h"
#include "ntrdma_dev.h"
#include "ntrdma_vbell.h"

int ntrdma_dev_vbell_init(struct ntrdma_dev *dev,
			  ntrdma_u32_t vbell_count, ntrdma_u32_t vbell_start)
{
	int rc, i;

	dev->vbell_enable = 0;
	ntrdma_spl_create(&dev->vbell_next_lock, "vbell_next_lock");
	ntrdma_spl_create(&dev->vbell_self_lock, "vbell_self_lock");
	ntrdma_spl_create(&dev->vbell_peer_lock, "vbell_peer_lock");

	dev->vbell_count = vbell_count;
	dev->vbell_start = vbell_start;
	dev->vbell_next = vbell_start;

	dev->vbell_vec = ntrdma_malloc(vbell_count * sizeof(*dev->vbell_vec), dev->node);
	if (!dev->vbell_vec) {
		rc = -ENOMEM;
		goto err_vec;
	}

	dev->vbell_buf_size = vbell_count * sizeof(*dev->vbell_buf);
	dev->vbell_buf = ntrdma_malloc_coherent(ntrdma_port_device(dev),
						dev->vbell_buf_size, &dev->vbell_buf_dma, dev->node);
	if (!dev->vbell_buf) {
		rc = -ENOMEM;
		goto err_buf;
	}

	for (i = 0; i < vbell_count; ++i) {
		ntrdma_vbell_head_init(&dev->vbell_vec[i]);
		dev->vbell_buf[i] = 0;
	}

#ifdef CONFIG_NTRDMA_VBELL_USE_SEQ
	dev->vbell_peer_seq = NULL;
#endif
	dev->peer_vbell_buf_dma = 0;
	dev->peer_vbell_count = 0;

	return 0;

err_buf:
	ntrdma_free(dev->vbell_vec);
err_vec:
	ntrdma_spl_destroy(&dev->vbell_peer_lock);
	ntrdma_spl_destroy(&dev->vbell_self_lock);
	ntrdma_spl_destroy(&dev->vbell_next_lock);
	return rc;
}

void ntrdma_dev_vbell_deinit(struct ntrdma_dev *dev)
{
	ntrdma_free_coherent(ntrdma_port_device(dev),
			     dev->vbell_buf_size, dev->vbell_buf, dev->vbell_buf_dma);
	ntrdma_free(dev->vbell_vec);
	ntrdma_spl_destroy(&dev->vbell_peer_lock);
	ntrdma_spl_destroy(&dev->vbell_self_lock);
	ntrdma_spl_destroy(&dev->vbell_next_lock);
}

ntrdma_u32_t ntrdma_dev_vbell_next(struct ntrdma_dev *dev)
{
	ntrdma_u32_t idx;

	ntrdma_spl_lock(&dev->vbell_next_lock);
	{
		idx = dev->vbell_next;

		if (dev->vbell_count < ++dev->vbell_next)
			dev->vbell_next = dev->vbell_start;
	}
	ntrdma_spl_unlock(&dev->vbell_next_lock);

	return idx;
}

int ntrdma_dev_vbell_enable(struct ntrdma_dev *dev,
			    ntrdma_dma_addr_t peer_vbell_buf_dma,
			    ntrdma_u32_t peer_vbell_count)
{
#ifdef CONFIG_NTRDMA_VBELL_USE_SEQ
	int rc, i;
#endif

	ntrdma_spl_lock(&dev->vbell_self_lock);
	ntrdma_spl_lock(&dev->vbell_peer_lock);
	{
		dev->vbell_enable = 1;

#ifdef CONFIG_NTRDMA_VBELL_USE_SEQ
		dev->vbell_peer_seq = ntrdma_malloc(peer_vbell_count *
						    sizeof(*dev->vbell_peer_seq), dev->node);
		if (!dev->vbell_peer_seq) {
			rc = -ENOMEM;
			goto err_seq;
		}

		for (i = 0; i < peer_vbell_count; ++i) {
			dev->vbell_peer_seq[i] = 0;
		}
#endif

		dev->peer_vbell_buf_dma = peer_vbell_buf_dma;
		dev->peer_vbell_count = peer_vbell_count;
	}
	ntrdma_spl_unlock(&dev->vbell_peer_lock);
	ntrdma_spl_unlock(&dev->vbell_self_lock);

	return 0;

#ifdef CONFIG_NTRDMA_VBELL_USE_SEQ
err_seq:
	dev->vbell_enable = 0;
	ntrdma_spl_unlock(&dev->vbell_peer_lock);
	ntrdma_spl_unlock(&dev->vbell_self_lock);
	return rc;
#endif
}

void ntrdma_dev_vbell_disable(struct ntrdma_dev *dev)
{
	int i;

	ntrdma_spl_lock(&dev->vbell_self_lock);
	ntrdma_spl_lock(&dev->vbell_peer_lock);
	{
		dev->vbell_enable = 0;

		for (i = 0; i < dev->vbell_count; ++i) {
			ntrdma_vbell_head_reset(&dev->vbell_vec[i]);
			dev->vbell_buf[i] = 0;
		}

#ifdef CONFIG_NTRDMA_VBELL_USE_SEQ
		ntrdma_free(dev->vbell_peer_seq);
		dev->vbell_peer_seq = NULL;
#endif
		dev->peer_vbell_buf_dma = 0;
		dev->peer_vbell_count = 0;
	}
	ntrdma_spl_unlock(&dev->vbell_peer_lock);
	ntrdma_spl_unlock(&dev->vbell_self_lock);
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

void ntrdma_dev_vbell_event(struct ntrdma_dev *dev)
{
	struct ntrdma_vbell_head *head;
	ntrdma_u32_t *buf;
	int i;

	ntrdma_spl_lock(&dev->vbell_self_lock);
	{
		for (i = 0; i < dev->vbell_count; ++i) {
			head = &dev->vbell_vec[i];
			buf = &dev->vbell_buf[i];
			if (ntrdma_vbell_cond(head->seq, *buf)) {
				ntrdma_vbell_update_head(head->seq, *buf);
				ntrdma_vbell_head_fire(head);
			}
		}
	}
	ntrdma_spl_unlock(&dev->vbell_self_lock);
}

void ntrdma_dev_vbell_del(struct ntrdma_dev *dev,
			  struct ntrdma_vbell *vbell, ntrdma_u32_t idx)
{
	ntrdma_spl_lock(&dev->vbell_self_lock);
	{
		ntrdma_vbell_del(vbell);
	}
	ntrdma_spl_unlock(&dev->vbell_self_lock);
}

void ntrdma_dev_vbell_clear(struct ntrdma_dev *dev,
			    struct ntrdma_vbell *vbell, ntrdma_u32_t idx)
{
	ntrdma_spl_lock(&dev->vbell_self_lock);
	{
		ntrdma_vbell_clear(&dev->vbell_vec[idx], vbell);
	}
	ntrdma_spl_unlock(&dev->vbell_self_lock);
}

int ntrdma_dev_vbell_add(struct ntrdma_dev *dev,
			 struct ntrdma_vbell *vbell, ntrdma_u32_t idx)
{
	int rc;

	ntrdma_spl_lock(&dev->vbell_self_lock);
	{
		if (!dev->vbell_enable)
			rc = -EINVAL;
		else
			rc = ntrdma_vbell_add(&dev->vbell_vec[idx], vbell);
	}
	ntrdma_spl_unlock(&dev->vbell_self_lock);

	return rc;
}

int ntrdma_dev_vbell_add_clear(struct ntrdma_dev *dev,
			       struct ntrdma_vbell *vbell, ntrdma_u32_t idx)
{
	int rc;

	ntrdma_spl_lock(&dev->vbell_self_lock);
	{
		if (!dev->vbell_enable)
			rc = -EINVAL;
		else
			rc = ntrdma_vbell_add_clear(&dev->vbell_vec[idx], vbell);
	}
	ntrdma_spl_unlock(&dev->vbell_self_lock);

	return rc;
}

void ntrdma_dev_vbell_peer(struct ntrdma_dev *dev,
			   struct ntrdma_req *req, ntrdma_u32_t idx)
{
	ntrdma_dma_addr_t dst;
	ntrdma_u32_t val;

	ntrdma_req_fence(req);

	ntrdma_spl_lock(&dev->vbell_self_lock);
	{
		if (dev->vbell_enable) {
			dst = dev->peer_vbell_buf_dma + idx * sizeof(ntrdma_u32_t);
			val = ntrdma_vbell_update_peer(dev, idx);

			ntrdma_req_imm32(req, dst, val);
		}
	}
	ntrdma_spl_unlock(&dev->vbell_self_lock);
}

