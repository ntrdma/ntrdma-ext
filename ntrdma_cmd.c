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

#include "ntrdma_map.h"

#include "ntrdma_dev.h"
#include "ntrdma_util.h"
#include "ntrdma_res.h"
#include "ntrdma_cmd.h"
#include "ntrdma_ring.h"

#include "ntrdma_cq.h"
#include "ntrdma_pd.h"
#include "ntrdma_mr.h"
#include "ntrdma_qp.h"

#define NTRDMA_RES_VBELL		1
#define NTRDMA_RRES_VBELL		0

static void ntrdma_cmd_send_work(struct ntrdma_dev *dev);
static NTRDMA_DECL_DWI_CB(ntrdma_cmd_send_work_cb, ptrhld);
static void ntrdma_cmd_send_vbell_cb(void *ctx);

static int ntrdma_cmd_recv(struct ntrdma_dev *dev, union ntrdma_cmd *cmd,
			   union ntrdma_rsp *rsp, struct ntrdma_req *req);

static void ntrdma_cmd_recv_work(struct ntrdma_dev *dev);
static NTRDMA_DECL_DWI_CB(ntrdma_cmd_recv_work_cb, ptrhld);
static void ntrdma_cmd_recv_vbell_cb(void *ctx);

static inline bool ntrdma_cmd_done(struct ntrdma_dev *dev)
{
	return ntrdma_list_empty(&dev->cmd_post_list) &&
		ntrdma_list_empty(&dev->cmd_pend_list);
}

int ntrdma_dev_cmd_init(struct ntrdma_dev *dev,
			ntrdma_u32_t recv_vbell_idx,
			ntrdma_u32_t send_vbell_idx,
			ntrdma_u32_t send_cap)
{
	int rc;

	dev->cmd_ready = 0;

	/* recv work */
	ntrdma_mut_create(&dev->cmd_recv_lock, "cmd_recv_lock");
	ntrdma_dwi_create(&dev->cmd_recv_work, "cmd_recv_work",
			  ntrdma_cmd_recv_work_cb, dev);
	ntrdma_vbell_init(&dev->cmd_recv_vbell,
			  ntrdma_cmd_recv_vbell_cb, dev);
	dev->cmd_recv_vbell_idx = recv_vbell_idx;

	/* allocated in conf phase */
	dev->cmd_recv_cap = 0;
	dev->cmd_recv_cons = 0;
	dev->cmd_recv_buf = NULL;
	dev->cmd_recv_prod_buf = NULL;
	dev->cmd_recv_buf_dma = 0;
	dev->cmd_recv_buf_size = 0;
	dev->cmd_recv_rsp_buf = NULL;
	dev->cmd_recv_rsp_buf_dma = 0;
	dev->cmd_recv_rsp_buf_size = 0;

	/* assigned in ready phase */
	dev->peer_cmd_send_rsp_buf_dma = 0;
	dev->peer_cmd_send_cons_dma = 0;
	/* assigned in conf phase */
	dev->peer_cmd_send_vbell_idx = 0;

	/* send work */
	ntrdma_list_init(&dev->cmd_pend_list);
	ntrdma_list_init(&dev->cmd_post_list);
	ntrdma_cvh_create(&dev->cmd_send_cond, "cmd_send_cond");
	ntrdma_mut_create(&dev->cmd_send_lock, "cmd_send_lock");
	ntrdma_dwi_create(&dev->cmd_send_work, "cmd_send_work",
			  ntrdma_cmd_send_work_cb, dev);
	ntrdma_vbell_init(&dev->cmd_send_vbell,
			  ntrdma_cmd_send_vbell_cb, dev);
	dev->cmd_send_vbell_idx = send_vbell_idx;

	/* allocate send buffers */
	dev->cmd_send_cap = send_cap;
	dev->cmd_send_prod = 0;
	dev->cmd_send_cmpl = 0;

	dev->cmd_send_buf_size = dev->cmd_send_cap * sizeof(union ntrdma_cmd);

	dev->cmd_send_buf = ntrdma_zalloc(dev->cmd_send_buf_size, dev->node);
	if (!dev->cmd_send_buf) {
		rc = -ENOMEM;
		goto err_send_buf;
	}

	dev->cmd_send_buf_dma = ntrdma_dma_map(ntrdma_port_device(dev),
					       dev->cmd_send_buf,
					       dev->cmd_send_buf_size,
					       NTRDMA_DMA_TO_DEVICE);
	if (!dev->cmd_send_buf_dma) {
		rc = -EIO;
		goto err_send_buf_dma;
	}

	dev->cmd_send_rsp_buf_size = dev->cmd_send_cap * sizeof(union ntrdma_rsp)
		+ sizeof(*dev->cmd_send_cons_buf); /* for cmd_send_cons_buf */

	dev->cmd_send_rsp_buf = ntrdma_zalloc(dev->cmd_send_rsp_buf_size, dev->node);
	if (!dev->cmd_send_rsp_buf) {
		rc = -ENOMEM;
		goto err_send_rsp_buf;
	}

	dev->cmd_send_cons_buf = (void *)&dev->cmd_send_rsp_buf[dev->cmd_send_cap];

	*dev->cmd_send_cons_buf = 0;

	dev->cmd_send_rsp_buf_dma = ntrdma_dma_map(ntrdma_port_device(dev),
						   dev->cmd_send_rsp_buf,
						   dev->cmd_send_rsp_buf_size,
						   NTRDMA_DMA_FROM_DEVICE);
	if (!dev->cmd_send_rsp_buf_dma) {
		rc = -EIO;
		goto err_send_rsp_buf_dma;
	}

	/* assigned in conf phase */
	dev->peer_cmd_recv_buf_dma = 0;
	dev->peer_cmd_recv_prod_dma = 0;
	dev->peer_cmd_recv_vbell_idx = 0;

	return 0;

err_send_rsp_buf_dma:
	ntrdma_free(dev->cmd_send_rsp_buf);
err_send_rsp_buf:
	ntrdma_dma_unmap(ntrdma_port_device(dev),
			 dev->cmd_send_buf_dma, dev->cmd_send_buf_size,
			 NTRDMA_DMA_TO_DEVICE);
err_send_buf_dma:
	ntrdma_free(dev->cmd_send_buf);
err_send_buf:
	ntrdma_dwi_destroy(&dev->cmd_send_work);
	ntrdma_mut_destroy(&dev->cmd_send_lock);
	ntrdma_cvh_destroy(&dev->cmd_send_cond);
	ntrdma_dwi_destroy(&dev->cmd_recv_work);
	ntrdma_mut_destroy(&dev->cmd_recv_lock);
	return rc;
}

void ntrdma_dev_cmd_deinit(struct ntrdma_dev *dev)
{
	ntrdma_dwi_destroy(&dev->cmd_recv_work);
	ntrdma_dwi_destroy(&dev->cmd_send_work);
	ntrdma_cvh_destroy(&dev->cmd_send_cond);
	ntrdma_mut_destroy(&dev->cmd_send_lock);
	ntrdma_dma_unmap(ntrdma_port_device(dev),
			 dev->cmd_send_rsp_buf_dma, dev->cmd_send_rsp_buf_size,
			 NTRDMA_DMA_FROM_DEVICE);
	ntrdma_free(dev->cmd_send_rsp_buf);
	ntrdma_dma_unmap(ntrdma_port_device(dev),
			 dev->cmd_send_buf_dma, dev->cmd_send_buf_size,
			 NTRDMA_DMA_TO_DEVICE);
	ntrdma_free(dev->cmd_send_buf);
	ntrdma_dwi_destroy(&dev->cmd_send_work);
	ntrdma_mut_destroy(&dev->cmd_send_lock);
	ntrdma_cvh_destroy(&dev->cmd_send_cond);
	ntrdma_dwi_destroy(&dev->cmd_recv_work);
	ntrdma_mut_destroy(&dev->cmd_recv_lock);
}

int ntrdma_dev_cmd_conf(struct ntrdma_dev *dev,
			ntrdma_dma_addr_t peer_send_rsp_buf_dma,
			ntrdma_dma_addr_t peer_send_cons_dma,
			ntrdma_u32_t peer_send_vbell_idx,
			ntrdma_u32_t peer_recv_vbell_idx,
			ntrdma_u32_t recv_cap,
			ntrdma_u32_t recv_idx)
{
	int rc;

	dev->peer_cmd_send_rsp_buf_dma = peer_send_rsp_buf_dma;
	dev->peer_cmd_send_cons_dma = peer_send_cons_dma;
	dev->peer_cmd_send_vbell_idx = peer_send_vbell_idx;

	/* allocate recv buffers */
	dev->cmd_recv_cap = recv_cap;
	dev->cmd_recv_cons = recv_idx;

	dev->cmd_recv_buf_size = dev->cmd_recv_cap * sizeof(union ntrdma_cmd)
		+ sizeof(*dev->cmd_recv_prod_buf); /* for cmd_recv_prod_buf */

	dev->cmd_recv_buf = ntrdma_zalloc(dev->cmd_recv_buf_size, dev->node);
	if (!dev->cmd_recv_buf) {
		rc = -ENOMEM;
		goto err_recv_buf;
	}

	dev->cmd_recv_prod_buf = (void *)&dev->cmd_recv_buf[dev->cmd_recv_cap];

	*dev->cmd_recv_prod_buf = 0;

	dev->cmd_recv_buf_dma = ntrdma_dma_map(ntrdma_port_device(dev),
					       dev->cmd_recv_buf,
					       dev->cmd_recv_buf_size,
					       NTRDMA_DMA_FROM_DEVICE);
	if (!dev->cmd_recv_buf_dma) {
		rc = -EIO;
		goto err_recv_buf_dma;
	}

	dev->cmd_recv_rsp_buf_size = dev->cmd_recv_cap * sizeof(union ntrdma_rsp);

	dev->cmd_recv_rsp_buf = ntrdma_zalloc(dev->cmd_recv_rsp_buf_size, dev->node);
	if (!dev->cmd_recv_rsp_buf) {
		rc = -ENOMEM;
		goto err_recv_rsp_buf;
	}

	dev->cmd_recv_rsp_buf_dma = ntrdma_dma_map(ntrdma_port_device(dev),
						   dev->cmd_recv_rsp_buf,
						   dev->cmd_recv_rsp_buf_size,
						   NTRDMA_DMA_TO_DEVICE);
	if (!dev->cmd_recv_rsp_buf_dma) {
		rc = -EIO;
		goto err_recv_rsp_buf_dma;
	}

	dev->peer_cmd_recv_vbell_idx = peer_recv_vbell_idx;

	return 0;

err_recv_rsp_buf_dma:
	ntrdma_free(dev->cmd_recv_rsp_buf);
err_recv_rsp_buf:
	ntrdma_dma_unmap(ntrdma_port_device(dev),
			 dev->cmd_recv_buf_dma, dev->cmd_recv_buf_size,
			 NTRDMA_DMA_FROM_DEVICE);
err_recv_buf_dma:
	ntrdma_free(dev->cmd_recv_buf);
err_recv_buf:
	dev->peer_cmd_send_rsp_buf_dma = 0;
	dev->peer_cmd_send_cons_dma = 0;
	dev->peer_cmd_send_vbell_idx = 0;
	dev->cmd_recv_cap = 0;
	dev->cmd_recv_cons = 0;
	dev->cmd_recv_buf_size = 0;
	dev->cmd_recv_buf = NULL;
	dev->cmd_recv_prod_buf = NULL;
	dev->cmd_recv_buf_dma = 0;
	dev->cmd_recv_rsp_buf_size = 0;
	dev->cmd_recv_rsp_buf = NULL;
	dev->cmd_recv_rsp_buf_dma = 0;
	dev->peer_cmd_recv_vbell_idx = 0;
	return rc;
}

void ntrdma_dev_cmd_deconf(struct ntrdma_dev *dev)
{
	ntrdma_dma_unmap(ntrdma_port_device(dev),
			 dev->cmd_recv_rsp_buf_dma, dev->cmd_recv_rsp_buf_size,
			 NTRDMA_DMA_TO_DEVICE);
	ntrdma_free(dev->cmd_recv_rsp_buf);
	ntrdma_dma_unmap(ntrdma_port_device(dev),
			 dev->cmd_recv_buf_dma, dev->cmd_recv_buf_size,
			 NTRDMA_DMA_FROM_DEVICE);
	ntrdma_free(dev->cmd_recv_buf);

	dev->peer_cmd_send_rsp_buf_dma = 0;
	dev->peer_cmd_send_cons_dma = 0;
	dev->peer_cmd_send_vbell_idx = 0;
	dev->cmd_recv_cap = 0;
	dev->cmd_recv_cons = 0;
	dev->cmd_recv_buf_size = 0;
	dev->cmd_recv_buf = NULL;
	dev->cmd_recv_prod_buf = NULL;
	dev->cmd_recv_buf_dma = 0;
	dev->cmd_recv_rsp_buf_size = 0;
	dev->cmd_recv_rsp_buf = NULL;
	dev->cmd_recv_rsp_buf_dma = 0;
	dev->peer_cmd_recv_vbell_idx = 0;
}

void ntrdma_dev_cmd_enable(struct ntrdma_dev *dev,
			   ntrdma_dma_addr_t peer_recv_buf_dma,
			   ntrdma_dma_addr_t peer_recv_prod_dma)
{
	dev->peer_cmd_recv_buf_dma = peer_recv_buf_dma;
	dev->peer_cmd_recv_prod_dma = peer_recv_prod_dma;

	ntrdma_mut_lock(&dev->cmd_send_lock);
	ntrdma_mut_lock(&dev->cmd_recv_lock);
	{
		dev->cmd_ready = 1;
	}
	ntrdma_mut_unlock(&dev->cmd_recv_lock);
	ntrdma_mut_unlock(&dev->cmd_send_lock);

	ntrdma_cmd_recv_work(dev);
	ntrdma_cmd_send_work(dev);
}

void ntrdma_dev_cmd_disable(struct ntrdma_dev *dev)
{
	ntrdma_mut_lock(&dev->cmd_send_lock);
	ntrdma_mut_lock(&dev->cmd_recv_lock);
	{
		dev->cmd_ready = 0;
	}
	ntrdma_mut_unlock(&dev->cmd_recv_lock);
	ntrdma_mut_unlock(&dev->cmd_send_lock);
}

void ntrdma_dev_cmd_add(struct ntrdma_dev *dev, struct ntrdma_cmd_cb *cb)
{
	ntrdma_vdbg(dev, "called\n");
	ntrdma_mut_lock(&dev->cmd_send_lock);
	{
		ntrdma_list_add_tail(&dev->cmd_pend_list, &cb->dev_entry);
	}
	ntrdma_mut_unlock(&dev->cmd_send_lock);
}

void ntrdma_dev_cmd_submit(struct ntrdma_dev *dev)
{
	ntrdma_vdbg(dev, "called\n");
	ntrdma_dwi_fire(&dev->cmd_send_work);
}

void ntrdma_dev_cmd_finish(struct ntrdma_dev *dev)
{
	ntrdma_vdbg(dev, "called\n");
	ntrdma_mut_lock(&dev->cmd_send_lock);
	{
		ntrdma_cvh_mut_wait(&dev->cmd_send_cond,
				    &dev->cmd_send_lock,
				    ntrdma_cmd_done(dev));
	}
	ntrdma_mut_unlock(&dev->cmd_send_lock);
}

static inline void ntrdma_cmd_send_vbell_clear(struct ntrdma_dev *dev)
{
	ntrdma_vdbg(dev, "called\n");
	ntrdma_dev_vbell_clear(dev, &dev->cmd_send_vbell,
			       dev->cmd_send_vbell_idx);
}

static inline int ntrdma_cmd_send_vbell_add(struct ntrdma_dev *dev)
{
	ntrdma_vdbg(dev, "called\n");
	return ntrdma_dev_vbell_add(dev, &dev->cmd_send_vbell,
				    dev->cmd_send_vbell_idx);
}

static void ntrdma_cmd_send_work(struct ntrdma_dev *dev)
{
	struct ntrdma_req *req;
	struct ntrdma_cmd_cb *cb;
	ntrdma_u32_t start, pos, end, base;
	ntrdma_dma_addr_t dst, src;
	ntrdma_size_t off, len;
	bool more = false;
	int rc;

	ntrdma_vdbg(dev, "called\n");

	req = ntrdma_req_alloc(dev, 100);
	if (!req)
		return; /* FIXME: no req, now what? */

	/* sync the ring buf for the cpu */

	ntrdma_dma_sync_for_cpu(ntrdma_port_device(dev),
				dev->cmd_send_rsp_buf_dma,
				dev->cmd_send_rsp_buf_size,
				NTRDMA_DMA_FROM_DEVICE);

	ntrdma_mut_lock(&dev->cmd_send_lock);
	{
		ntrdma_cmd_send_vbell_clear(dev);

		/* Complete commands that have a response */

		ntrdma_ring_consume(*dev->cmd_send_cons_buf, dev->cmd_send_cmpl,
				    dev->cmd_send_cap, &start, &end, &base);
		ntrdma_vdbg(dev, "rsp start %d end %d\n", start, end);
		for (pos = start; pos < end; ++pos) {
			cb = ntrdma_list_remove_first_entry(&dev->cmd_post_list,
							    struct ntrdma_cmd_cb,
							    dev_entry);
			if (!cb)
				; /* FIXME: detected a bug, now what? just crash below */

			ntrdma_vdbg(dev, "rsp cmpl pos %d\n", pos);
			rc = cb->rsp_cmpl(cb, &dev->cmd_send_rsp_buf[pos], req);
			if (rc)
				; /* FIXME: command failed, now what? */
		}

		if (pos != start) {
			dev->cmd_send_cmpl = ntrdma_ring_update(pos, base,
								dev->cmd_send_cap);
			more = true;
		}

		/* Issue some more pending commands */

		ntrdma_ring_produce(dev->cmd_send_prod,
				    dev->cmd_send_cmpl,
				    dev->cmd_send_cap,
				    &start, &end, &base);
		ntrdma_vdbg(dev, "cmd start %d end %d\n", start, end);
		for (pos = start; pos < end; ++pos) {
			cb = ntrdma_list_move_first_entry(&dev->cmd_post_list,
							  &dev->cmd_pend_list,
							  struct ntrdma_cmd_cb,
							  dev_entry);
			if (!cb)
				break;

			ntrdma_vdbg(dev, "cmd prep pos %d\n", pos);
			rc = cb->cmd_prep(cb, &dev->cmd_send_buf[pos], req);
			if (rc)
				; /* FIXME: command failed, now what? */
		}

		if (pos != start) {
			ntrdma_vdbg(dev, "cmd copy start %d pos %d\n", start, pos);

			dev->cmd_send_prod = ntrdma_ring_update(pos, base,
								dev->cmd_send_cap);
			more = true;

			/* sync the ring buf for the device */

			ntrdma_dma_sync_for_device(ntrdma_port_device(dev),
						   dev->cmd_send_buf_dma,
						   dev->cmd_send_buf_size,
						   NTRDMA_DMA_TO_DEVICE);

			/* copy the portion of the ring buf */

			off = start * sizeof(union ntrdma_cmd);
			len = (pos - start) * sizeof(union ntrdma_cmd);
			dst = dev->peer_cmd_recv_buf_dma + off;
			src = dev->cmd_send_buf_dma + off;

			ntrdma_req_memcpy(req, dst, src, len);

			/* update the producer index on the peer */

			ntrdma_req_fence(req);
			ntrdma_req_imm32(req, dev->peer_cmd_recv_prod_dma,
					 dev->cmd_send_prod);

			/* update the vbell and signal the peer */

			ntrdma_dev_vbell_peer(dev, req,
					      dev->peer_cmd_recv_vbell_idx);
			ntrdma_req_signal(req);
			ntrdma_req_submit(req);
		} else {
			ntrdma_req_free(req);
		}

		if (!ntrdma_cmd_done(dev)) {
			if (more || ntrdma_cmd_send_vbell_add(dev) == -EAGAIN)
				ntrdma_dwi_fire(&dev->cmd_send_work);
		}
	}
	ntrdma_mut_unlock(&dev->cmd_send_lock);
}

static NTRDMA_DECL_DWI_CB(ntrdma_cmd_send_work_cb, ptrhld)
{
	struct ntrdma_dev *dev = NTRDMA_CAST_DWI_CTX(ptrhld);

	ntrdma_cmd_send_work(dev);
}

static void ntrdma_cmd_send_vbell_cb(void *ctx)
{
	struct ntrdma_dev *dev = ctx;

	ntrdma_dwi_fire(&dev->cmd_send_work);
}

static int ntrdma_cmd_recv_none(struct ntrdma_dev *dev,
				struct ntrdma_cmd_hdr *cmd,
				struct ntrdma_cmd_hdr *rsp)
{
	ntrdma_dbg(dev, "called\n");

	*rsp = *cmd;

	return 0;
}

static int ntrdma_cmd_recv_pd_create(struct ntrdma_dev *dev,
				     struct ntrdma_cmd_hdr *cmd,
				     struct ntrdma_rsp_pd_status *rsp)
{
	struct ntrdma_rpd *rpd;
	int rc;

	ntrdma_dbg(dev, "called\n");

	rsp->hdr = *cmd;

	rpd = ntrdma_malloc(sizeof(*rpd), dev->node);
	if (!rpd) {
		rc = -ENOMEM;
		goto err_rpd;
	}

	rc = ntrdma_rpd_init(rpd, dev);
	if (rc)
		goto err_init;

	rc = ntrdma_rpd_add(rpd, cmd->pd_key);
	if (rc)
		goto err_add;

	rsp->status = 0;
	return 0;

err_add:
	ntrdma_rpd_deinit(rpd);
err_init:
	ntrdma_free(rpd);
err_rpd:
	rsp->status = ~0;
	return rc;
}

static int ntrdma_cmd_recv_pd_delete(struct ntrdma_dev *dev,
				     struct ntrdma_cmd_hdr *cmd,
				     struct ntrdma_rsp_pd_status *rsp)
{
	struct ntrdma_rpd *rpd;
	int rc;

	ntrdma_dbg(dev, "called\n");

	rsp->hdr = *cmd;

	rpd = ntrdma_dev_rpd_look(dev, cmd->pd_key);
	if (!rpd) {
		rc = -EINVAL;
		goto err_rpd;
	}

	ntrdma_rpd_del(rpd);
	ntrdma_rpd_put(rpd);
	ntrdma_rpd_repo(rpd);
	ntrdma_free(rpd);

	rsp->status = 0;
	return 0;

err_rpd:
	rsp->status = ~0;
	return rc;
}

static int ntrdma_cmd_recv_mr_create(struct ntrdma_dev *dev,
				     struct ntrdma_cmd_mr_create *cmd,
				     struct ntrdma_rsp_mr_status *rsp)
{
	struct ntrdma_rpd *rpd;
	struct ntrdma_rmr *rmr;
	ntrdma_u32_t i, count;
	int rc;

	ntrdma_dbg(dev, "called\n");

	rsp->hdr = cmd->hdr;
	rsp->mr_key = cmd->mr_key;

	rpd = ntrdma_dev_rpd_look(dev, cmd->hdr.pd_key);
	if (!rpd) {
		rc = -EINVAL;
		goto err_rpd;
	}

	rmr = ntrdma_malloc(sizeof(*rmr) +
			    cmd->sg_cap * sizeof(*rmr->sg_list),
			    dev->node);
	if (!rmr) {
		rc = -ENOMEM;
		goto err_rmr;
	}

	rc = ntrdma_rmr_init(rmr, rpd,
			     cmd->mr_addr, cmd->mr_len,
			     cmd->access, cmd->sg_cap);
	if (rc)
		goto err_init;

	count = cmd->sg_count;

	ntrdma_memcpy(rmr->sg_list, cmd->sg_list,
		      count * sizeof(struct ntrdma_mr_sge));

	for (i = 0; i < count; ++i)
		rmr->sg_list[i].dma += dev->peer_dram_base;

	rc = ntrdma_rmr_add(rmr, cmd->mr_key);
	if (rc)
		goto err_add;

	ntrdma_rpd_put(rpd);

	rsp->status = 0;
	return 0;

err_add:
	ntrdma_rmr_deinit(rmr);
err_init:
	ntrdma_free(rmr);
err_rmr:
	ntrdma_rpd_put(rpd);
err_rpd:
	rsp->status = ~0;
	return rc;
}

static int ntrdma_cmd_recv_mr_delete(struct ntrdma_dev *dev,
				     struct ntrdma_cmd_mr_delete *cmd,
				     struct ntrdma_rsp_mr_status *rsp)
{
	struct ntrdma_rpd *rpd;
	struct ntrdma_rmr *rmr;
	int rc;

	ntrdma_dbg(dev, "called\n");

	rsp->hdr = cmd->hdr;
	rsp->mr_key = cmd->mr_key;

	rpd = ntrdma_dev_rpd_look(dev, cmd->hdr.pd_key);
	if (!rpd) {
		rc = -EINVAL;
		goto err_rpd;
	}

	rmr = ntrdma_dev_rmr_look(rpd, cmd->mr_key);
	if (!rmr) {
		rc = -EINVAL;
		goto err_rmr;
	}

	ntrdma_rmr_del(rmr);
	ntrdma_rmr_put(rmr);
	ntrdma_rmr_repo(rmr);
	ntrdma_rmr_deinit(rmr);
	ntrdma_free(rmr);
	ntrdma_rpd_put(rpd);

	rsp->status = 0;
	return 0;

err_rmr:
	ntrdma_rpd_put(rpd);
err_rpd:
	rsp->status = ~0;
	return rc;
}

static int ntrdma_cmd_recv_mr_append(struct ntrdma_dev *dev,
				     struct ntrdma_cmd_mr_append *cmd,
				     struct ntrdma_rsp_mr_status *rsp)
{
	struct ntrdma_rpd *rpd;
	struct ntrdma_rmr *rmr;
	ntrdma_u32_t i, pos, count;
	int rc;

	ntrdma_dbg(dev, "called\n");

	rsp->hdr = cmd->hdr;
	rsp->mr_key = cmd->mr_key;

	rpd = ntrdma_dev_rpd_look(dev, cmd->hdr.pd_key);
	if (!rpd) {
		rc = -EINVAL;
		goto err_rpd;
	}

	rmr = ntrdma_dev_rmr_look(rpd, cmd->mr_key);
	if (!rmr) {
		rc = -EINVAL;
		goto err_rmr;
	}

	pos = cmd->sg_pos;
	count = cmd->sg_count;

	ntrdma_memcpy(&rmr->sg_list[pos], cmd->sg_list,
		      count * sizeof(struct ntrdma_mr_sge));

	count += pos;
	for (i = pos; i < count; ++i)
		rmr->sg_list[i].dma += dev->peer_dram_base;

	ntrdma_rmr_put(rmr);
	ntrdma_rpd_put(rpd);

	rsp->status = 0;
	return 0;

err_rmr:
	ntrdma_rpd_put(rpd);
err_rpd:
	rsp->status = ~0;
	return rc;
}

static int ntrdma_cmd_recv_qp_create(struct ntrdma_dev *dev,
				     struct ntrdma_cmd_qp_create *cmd,
				     struct ntrdma_rsp_qp_create *rsp)
{
	struct ntrdma_rpd *rpd;
	struct ntrdma_rqp *rqp;
	struct ntrdma_rqp_init_attr attr;
	int rc;

	ntrdma_dbg(dev, "called\n");

	rsp->hdr = cmd->hdr;
	rsp->qp_key = cmd->qp_key;

	rpd = ntrdma_dev_rpd_look(dev, cmd->hdr.pd_key);
	if (!rpd) {
		rc = -EINVAL;
		goto err_rpd;
	}

	rqp = ntrdma_malloc(sizeof(*rqp), dev->node);
	if (!rqp) {
		rc = -ENOMEM;
		goto err_rqp;
	}

	attr.recv_wqe_idx = cmd->recv_ring_idx;
	attr.recv_wqe_cap = cmd->recv_wqe_cap;
	attr.recv_wqe_sg_cap = cmd->recv_wqe_sg_cap;
	attr.send_wqe_idx = cmd->send_ring_idx;
	attr.send_wqe_cap = cmd->send_wqe_cap;
	attr.send_wqe_sg_cap = cmd->send_wqe_sg_cap;
	attr.peer_send_cqe_buf_dma =
		dev->peer_dram_base + cmd->send_cqe_buf_dma;
	attr.peer_send_cons_dma =
		dev->peer_dram_base + cmd->send_cons_dma;
	attr.peer_cmpl_vbell_idx = cmd->cmpl_vbell_idx;

	rc = ntrdma_rqp_init(rqp, rpd, &attr);
	if (rc)
		goto err_init;

	rsp->recv_wqe_buf_dma = rqp->recv_wqe_buf_dma;
	rsp->recv_prod_dma = rqp->recv_wqe_buf_dma +
		rqp->recv_cap * rqp->recv_wqe_size;
	rsp->send_wqe_buf_dma = rqp->send_wqe_buf_dma;
	rsp->send_prod_dma = rqp->send_wqe_buf_dma +
		rqp->send_cap * rqp->send_wqe_size;
	rsp->send_vbell_idx = rqp->send_vbell_idx;

	rc = ntrdma_rqp_add(rqp, cmd->qp_key);
	if (rc)
		goto err_add;

	ntrdma_rpd_put(rpd);

	rsp->status = 0;
	return 0;

err_add:
	ntrdma_rqp_deinit(rqp);
err_init:
	ntrdma_free(rqp);
err_rqp:
	ntrdma_rpd_put(rpd);
err_rpd:
	rsp->status = ~0;
	rsp->recv_wqe_buf_dma = 0;
	rsp->recv_prod_dma = 0;
	rsp->send_wqe_buf_dma = 0;
	rsp->send_prod_dma = 0;
	rsp->send_vbell_idx = 0;
	return rc;
}

static int ntrdma_cmd_recv_qp_delete(struct ntrdma_dev *dev,
				     struct ntrdma_cmd_qp_delete *cmd,
				     struct ntrdma_rsp_qp_status *rsp)
{
	struct ntrdma_rqp *rqp;
	int rc;

	ntrdma_dbg(dev, "called\n");

	rsp->hdr = cmd->hdr;
	rsp->qp_key = cmd->qp_key;

	rqp = ntrdma_dev_rqp_look(dev, cmd->qp_key);
	if (!rqp) {
		rc = -EINVAL;
		goto err_rqp;
	}

	ntrdma_rqp_del(rqp);
	ntrdma_rqp_put(rqp);
	ntrdma_rqp_repo(rqp);
	ntrdma_rqp_deinit(rqp);
	ntrdma_free(rqp);

	rsp->status = 0;
	return 0;

err_rqp:
	rsp->status = ~0;
	return rc;
}

static int ntrdma_cmd_recv_qp_modify(struct ntrdma_dev *dev,
				     struct ntrdma_cmd_qp_modify *cmd,
				     struct ntrdma_rsp_qp_status *rsp)
{
	struct ntrdma_rqp *rqp;
	int rc;

	ntrdma_dbg(dev, "called\n");

	rsp->hdr = cmd->hdr;
	rsp->qp_key = cmd->qp_key;

	rqp = ntrdma_dev_rqp_look(dev, cmd->qp_key);
	if (!rqp) {
		rc = -EINVAL;
		goto err_rqp;
	}

	rqp->state = cmd->state;
	rqp->recv_error = false;
	rqp->send_error = false;

	//rqp->access = cmd->access; /* TODO: qp access flags */
	rqp->qp_key = cmd->dest_qp_key;

	ntrdma_dpc_fire(&rqp->send_work);
	ntrdma_rqp_put(rqp);

	rsp->status = 0;
	return 0;

err_rqp:
	rsp->status = ~0;
	return rc;
}

static int ntrdma_cmd_recv(struct ntrdma_dev *dev, union ntrdma_cmd *cmd,
			   union ntrdma_rsp *rsp, struct ntrdma_req *req)
{
	switch (cmd->hdr.op) {
	case NTRDMA_CMD_NONE:
		return ntrdma_cmd_recv_none(dev, &cmd->hdr, &rsp->hdr);
	case NTRDMA_CMD_PD_CREATE:
		return ntrdma_cmd_recv_pd_create(dev, &cmd->pd_create,
						 &rsp->pd_status);
	case NTRDMA_CMD_PD_DELETE:
		return ntrdma_cmd_recv_pd_delete(dev, &cmd->pd_delete,
						 &rsp->pd_status);
	case NTRDMA_CMD_MR_CREATE:
		return ntrdma_cmd_recv_mr_create(dev, &cmd->mr_create,
						 &rsp->mr_status);
	case NTRDMA_CMD_MR_DELETE:
		return ntrdma_cmd_recv_mr_delete(dev, &cmd->mr_delete,
						 &rsp->mr_status);
	case NTRDMA_CMD_MR_APPEND:
		return ntrdma_cmd_recv_mr_append(dev, &cmd->mr_append,
						 &rsp->mr_status);
	case NTRDMA_CMD_QP_CREATE:
		return ntrdma_cmd_recv_qp_create(dev, &cmd->qp_create,
						 &rsp->qp_create);
	case NTRDMA_CMD_QP_DELETE:
		return ntrdma_cmd_recv_qp_delete(dev, &cmd->qp_delete,
						 &rsp->qp_status);
	case NTRDMA_CMD_QP_MODIFY:
		return ntrdma_cmd_recv_qp_modify(dev, &cmd->qp_modify,
						 &rsp->qp_status);
	}

	ntrdma_dbg(dev, "unhandled recv cmd hdr op %u\n", cmd->hdr.op);

	return -EINVAL;
}

static inline void ntrdma_cmd_recv_vbell_clear(struct ntrdma_dev *dev)
{
	ntrdma_vdbg(dev, "called\n");
	ntrdma_dev_vbell_clear(dev, &dev->cmd_recv_vbell,
			       dev->cmd_recv_vbell_idx);
}

static inline int ntrdma_cmd_recv_vbell_add(struct ntrdma_dev *dev)
{
	ntrdma_vdbg(dev, "called\n");
	return ntrdma_dev_vbell_add(dev, &dev->cmd_recv_vbell,
				    dev->cmd_recv_vbell_idx);
}

static void ntrdma_cmd_recv_work(struct ntrdma_dev *dev)
{
	struct ntrdma_req *req;
	ntrdma_u32_t start, pos, end, base;
	ntrdma_dma_addr_t dst, src;
	ntrdma_size_t off, len;
	int rc;

	ntrdma_vdbg(dev, "called\n");

	req = ntrdma_req_alloc(dev, 100);
	if (!req)
		return; /* FIXME: no req, now what? */

	/* sync the ring buf for the cpu */

	ntrdma_dma_sync_for_cpu(ntrdma_port_device(dev),
				dev->cmd_recv_buf_dma,
				dev->cmd_recv_buf_size,
				NTRDMA_DMA_FROM_DEVICE);

	ntrdma_mut_lock(&dev->cmd_recv_lock);
	{
		ntrdma_cmd_recv_vbell_clear(dev);

		/* Process commands */

		ntrdma_ring_consume(*dev->cmd_recv_prod_buf,
				    dev->cmd_recv_cons,
				    dev->cmd_recv_cap,
				    &start, &end, &base);
		ntrdma_vdbg(dev, "cmd start %d end %d\n", start, end);
		for (pos = start; pos < end; ++pos) {
			ntrdma_vdbg(dev, "cmd recv pos %d\n", pos);
			rc = ntrdma_cmd_recv(dev,
					     &dev->cmd_recv_buf[pos],
					     &dev->cmd_recv_rsp_buf[pos],
					     req);
			if (rc)
				; /* FIXME: command failed, now what? */
		}

		if (pos != start) {
			ntrdma_vdbg(dev, "rsp copy start %d pos %d\n",
				    start, pos);

			dev->cmd_recv_cons = ntrdma_ring_update(pos, base,
								dev->cmd_recv_cap);

			/* sync the ring buf for the device */

			ntrdma_dma_sync_for_device(ntrdma_port_device(dev),
						   dev->cmd_recv_rsp_buf_dma,
						   dev->cmd_recv_rsp_buf_size,
						   NTRDMA_DMA_TO_DEVICE);

			/* copy the portion of the ring buf */

			off = start * sizeof(union ntrdma_rsp);
			len = (pos - start) * sizeof(union ntrdma_rsp);
			dst = dev->peer_cmd_send_rsp_buf_dma + off;
			src = dev->cmd_recv_rsp_buf_dma + off;

			ntrdma_req_memcpy(req, dst, src, len);

			/* update the producer index on the peer */

			ntrdma_req_fence(req);
			ntrdma_req_imm32(req, dev->peer_cmd_send_cons_dma,
					 dev->cmd_recv_cons);

			/* update the vbell and signal the peer */

			ntrdma_dev_vbell_peer(dev, req,
					      dev->peer_cmd_send_vbell_idx);
			ntrdma_req_signal(req);

			ntrdma_req_submit(req);
			ntrdma_dwi_fire(&dev->cmd_recv_work);
		} else {
			ntrdma_req_free(req);
			if (ntrdma_cmd_recv_vbell_add(dev) == -EAGAIN)
				ntrdma_dwi_fire(&dev->cmd_recv_work);
		}
	}
	ntrdma_mut_unlock(&dev->cmd_recv_lock);
}

static NTRDMA_DECL_DWI_CB(ntrdma_cmd_recv_work_cb, ptrhld)
{
	struct ntrdma_dev *dev = NTRDMA_CAST_DWI_CTX(ptrhld);

	ntrdma_cmd_recv_work(dev);
}

static void ntrdma_cmd_recv_vbell_cb(void *ctx)
{
	struct ntrdma_dev *dev = ctx;

	ntrdma_dwi_fire(&dev->cmd_recv_work);
}

