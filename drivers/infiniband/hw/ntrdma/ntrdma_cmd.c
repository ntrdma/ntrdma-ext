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
#include "ntrdma_hello.h"
#include "ntrdma_util.h"
#include "ntrdma_res.h"
#include "ntrdma_cmd.h"
#include "ntrdma_ring.h"

#include "ntrdma_cq.h"
#include "ntrdma_pd.h"
#include "ntrdma_mr.h"
#include "ntrdma_qp.h"
#include "ntrdma_wr.h"
#include "linux/ntc_trace.h"

#define NTRDMA_RES_VBELL		1
#define NTRDMA_RRES_VBELL		0
#define MAX_CMDS 16

static void ntrdma_cmd_send_work(struct ntrdma_dev *dev);
static void ntrdma_cmd_send_work_cb(struct work_struct *ws);
static void ntrdma_cmd_send_vbell_cb(void *ctx);

static int ntrdma_cmd_recv(struct ntrdma_dev *dev, const union ntrdma_cmd *cmd,
			   union ntrdma_rsp *rsp, struct dma_chan *req);

static void ntrdma_cmd_recv_work(struct ntrdma_dev *dev);
static void ntrdma_cmd_recv_work_cb(struct work_struct *ws);
static void ntrdma_cmd_recv_vbell_cb(void *ctx);

static inline bool ntrdma_cmd_done(struct ntrdma_dev *dev)
{
	return list_empty(&dev->cmd_post_list) &&
		list_empty(&dev->cmd_pend_list);
}

static inline const u32 *ntrdma_dev_cmd_send_cons_buf(struct ntrdma_dev *dev)
{
	return ntc_export_buf_const_deref(&dev->cmd_send_rsp_buf,
					sizeof(union ntrdma_rsp) *
					dev->cmd_send_cap,
					sizeof(u32));
}

inline u32 ntrdma_dev_cmd_send_cons(struct ntrdma_dev *dev)
{
	const u32 *cmd_send_cons_buf = ntrdma_dev_cmd_send_cons_buf(dev);

	if (!cmd_send_cons_buf)
		return 0;

	return *cmd_send_cons_buf;
}

static inline void ntrdma_dev_clear_cmd_send_cons(struct ntrdma_dev *dev)
{
	u32 cmd_send_cons = 0;

	ntc_export_buf_reinit(&dev->cmd_send_rsp_buf, &cmd_send_cons,
			sizeof(union ntrdma_rsp) * dev->cmd_send_cap,
			sizeof(cmd_send_cons));
}

static inline const u32 *ntrdma_dev_cmd_recv_prod_buf(struct ntrdma_dev *dev)
{
	return ntc_export_buf_const_deref(&dev->cmd_recv_buf,
					sizeof(union ntrdma_cmd) *
					dev->cmd_recv_cap,
					sizeof(u32));
}

inline u32 ntrdma_dev_cmd_recv_prod(struct ntrdma_dev *dev)
{
	const u32 *cmd_recv_prod_buf = ntrdma_dev_cmd_recv_prod_buf(dev);

	if (!cmd_recv_prod_buf)
		return 0;

	return *cmd_recv_prod_buf;
}

static inline int ntrdma_dev_cmd_init_deinit(struct ntrdma_dev *dev,
			u32 recv_vbell_idx,
			u32 send_vbell_idx,
			u32 send_cap, int is_deinit)
{
	int rc;

	if (is_deinit)
		goto deinit;

	dev->cmd_ready = 0;

	/* recv work */
	mutex_init(&dev->cmd_recv_lock);

	ntrdma_vbell_init(&dev->cmd_recv_vbell,
			  ntrdma_cmd_recv_vbell_cb, dev);
	dev->cmd_recv_vbell_idx = recv_vbell_idx;

	/* allocated in conf phase */
	dev->cmd_recv_cap = 0;
	dev->cmd_recv_cons = 0;
	ntc_export_buf_clear(&dev->cmd_recv_buf);
	ntc_local_buf_clear(&dev->cmd_recv_rsp_buf);
	dev->is_cmd_hello_done = false;
	dev->is_cmd_prep = false;

	/* assigned in ready phase */
	ntc_remote_buf_clear(&dev->peer_cmd_send_rsp_buf);
	/* assigned in conf phase */
	dev->peer_cmd_send_vbell_idx = 0;

	/* send work */
	INIT_LIST_HEAD(&dev->cmd_pend_list);
	INIT_LIST_HEAD(&dev->cmd_post_list);
	init_waitqueue_head(&dev->cmd_send_cond);
	mutex_init(&dev->cmd_send_lock);
	ntrdma_vbell_init(&dev->cmd_send_vbell,
			  ntrdma_cmd_send_vbell_cb, dev);
	dev->cmd_send_vbell_idx = send_vbell_idx;

	/* allocate send buffers */
	dev->cmd_send_cap = send_cap;
	dev->cmd_send_prod = 0;
	dev->cmd_send_cmpl = 0;

	rc = ntc_local_buf_zalloc(&dev->cmd_send_buf, dev->ntc,
				dev->cmd_send_cap * sizeof(union ntrdma_cmd),
				GFP_KERNEL);
	if (rc < 0) {
		ntrdma_err(dev, "dma mapping failed\n");
		goto err_send_buf;
	}

	rc = ntc_export_buf_zalloc(&dev->cmd_send_rsp_buf, dev->ntc,
				dev->cmd_send_cap * sizeof(union ntrdma_rsp)
				+ sizeof(u32), /* for cmd_send_cons */
				GFP_KERNEL);
	if (rc < 0) {
		ntrdma_err(dev, "dma mapping failed\n");
		goto err_send_rsp_buf;
	}

	INIT_WORK(&dev->cmd_send_work,
			ntrdma_cmd_send_work_cb);
	INIT_WORK(&dev->cmd_recv_work,
			ntrdma_cmd_recv_work_cb);
	return 0;
deinit:
	WARN(dev->is_cmd_hello_done, "Deinit while cmd hello not undone");
	WARN(dev->is_cmd_prep, "Deinit while cmd prep not unprep");
	cancel_work_sync(&dev->cmd_send_work);
	cancel_work_sync(&dev->cmd_recv_work);
	ntc_export_buf_free(&dev->cmd_send_rsp_buf);
err_send_rsp_buf:
	ntc_local_buf_free(&dev->cmd_send_buf);
err_send_buf:
	return rc;
}

int ntrdma_dev_cmd_init(struct ntrdma_dev *dev,
			u32 recv_vbell_idx,
			u32 send_vbell_idx,
			u32 send_cap)
{
	return ntrdma_dev_cmd_init_deinit(dev, recv_vbell_idx,
			send_vbell_idx, send_cap, false);
}

static inline
int ntrdma_dev_cmd_hello_done_undone(struct ntrdma_dev *dev,
				const struct ntrdma_cmd_hello_prep *peer_prep,
				int is_undone)
{
	int rc;

	if (is_undone)
		goto undone;

	rc = ntc_remote_buf_map(&dev->peer_cmd_recv_buf, dev->ntc,
				&peer_prep->recv_buf_desc);
	if (rc < 0)
		goto err_peer_cmd_recv_buf;

	dev->peer_recv_prod_shift = peer_prep->recv_prod_shift;

	dev->is_cmd_hello_done = true;

	return 0;
undone:
	if (!dev->is_cmd_hello_done)
		return 0;

	dev->is_cmd_hello_done = false;
	ntc_remote_buf_unmap(&dev->peer_cmd_recv_buf);
err_peer_cmd_recv_buf:
	return rc;
}

void ntrdma_dev_cmd_deinit(struct ntrdma_dev *dev)
{
	ntrdma_dev_cmd_init_deinit(dev, 0, 0, 0, true);
}

void ntrdma_dev_cmd_hello_info(struct ntrdma_dev *dev,
			       struct ntrdma_cmd_hello_info *info)
{
	ntc_export_buf_make_desc(&info->send_rsp_buf_desc,
				&dev->cmd_send_rsp_buf);
	info->send_cons_shift = dev->cmd_send_cap * sizeof(union ntrdma_rsp);
	info->send_cap = dev->cmd_send_cap;
	info->send_idx = ntrdma_dev_cmd_send_cons(dev);
	info->send_vbell_idx = dev->cmd_send_vbell_idx;
	info->recv_vbell_idx = dev->cmd_recv_vbell_idx;
}

static inline
int ntrdma_dev_cmd_hello_prep_unprep(struct ntrdma_dev *dev,
				const struct ntrdma_cmd_hello_info *peer_info,
				int is_unprep)
{
	int rc;

	if (is_unprep)
		goto deinit;

	if (peer_info->send_vbell_idx > NTRDMA_DEV_VBELL_COUNT ||
		peer_info->recv_vbell_idx > NTRDMA_DEV_VBELL_COUNT) {
		ntrdma_err(dev,
				"peer info vbell corrupted? send vbell idx %u, recv vbell idx %u\n",
				peer_info->send_vbell_idx,
				peer_info->recv_vbell_idx);
		rc = -EIO;
		goto err_sanity;
	}

	if (peer_info->send_cap > MAX_CMDS) {
		ntrdma_err(dev,
				"peer info corrupted? cap %d, idx %d\n",
				peer_info->send_cap, peer_info->send_idx);
		rc = -EIO;
		goto err_sanity;
	}

	rc = ntc_remote_buf_map(&dev->peer_cmd_send_rsp_buf, dev->ntc,
				&peer_info->send_rsp_buf_desc);
	if (rc < 0)
		goto err_peer_cmd_send_rsp_buf;

	dev->peer_send_cons_shift = peer_info->send_cons_shift;

	dev->peer_cmd_send_vbell_idx = peer_info->send_vbell_idx;
	dev->peer_cmd_recv_vbell_idx = peer_info->recv_vbell_idx;

	/* allocate local recv ring for peer send */
	dev->cmd_recv_cap = peer_info->send_cap;
	dev->cmd_recv_cons = peer_info->send_idx;

	rc = ntc_export_buf_zalloc(&dev->cmd_recv_buf, dev->ntc,
				dev->cmd_recv_cap * sizeof(union ntrdma_cmd)
				+ sizeof(u32), /* for cmd_recv_prod */
				GFP_KERNEL);
	if (rc < 0) {
		ntrdma_err(dev, "dma mapping failed\n");
		goto err_recv_buf;
	}

	rc = ntc_local_buf_zalloc(&dev->cmd_recv_rsp_buf, dev->ntc,
				dev->cmd_recv_cap * sizeof(union ntrdma_rsp),
				GFP_KERNEL);
	if (rc < 0) {
		ntrdma_err(dev, "dma mapping failed\n");
		goto err_recv_rsp_buf;
	}

	dev->is_cmd_prep = true;

	return 0;
deinit:
	if (!dev->is_cmd_prep)
		return 0;
	dev->is_cmd_prep = false;
	ntc_local_buf_free(&dev->cmd_recv_rsp_buf);
err_recv_rsp_buf:
	ntc_export_buf_free(&dev->cmd_recv_buf);
err_recv_buf:
	ntrdma_dev_clear_cmd_send_cons(dev);
	dev->cmd_send_cmpl = 0;
	dev->cmd_send_prod = 0;
	dev->cmd_recv_cons = 0;
	dev->cmd_recv_cap = 0;
	dev->peer_cmd_send_vbell_idx = 0;
	dev->peer_cmd_recv_vbell_idx = 0;
	ntc_remote_buf_unmap(&dev->peer_cmd_send_rsp_buf);
err_peer_cmd_send_rsp_buf:
err_sanity:
	return rc;
}

int ntrdma_dev_cmd_hello_prep(struct ntrdma_dev *dev,
			const struct ntrdma_cmd_hello_info *peer_info,
			struct ntrdma_cmd_hello_prep *prep)
{
	int rc;

	rc = ntrdma_dev_cmd_hello_prep_unprep(dev, peer_info, false);
	if (rc)
		return rc;

	ntc_export_buf_make_desc(&prep->recv_buf_desc, &dev->cmd_recv_buf);

	prep->recv_prod_shift = dev->cmd_recv_cap * sizeof(union ntrdma_cmd);

	return 0;
}

int ntrdma_dev_cmd_hello_done(struct ntrdma_dev *dev,
			const struct ntrdma_cmd_hello_prep *peer_prep)
{
	return ntrdma_dev_cmd_hello_done_undone(dev, peer_prep, false);
}
void ntrdma_dev_cmd_quiesce(struct ntrdma_dev *dev)
{
	struct ntrdma_cmd_cb *cb_tmp, *cb;

	pr_info("cmd quiesce starting ...\n");

	cancel_work_sync(&dev->cmd_send_work);
	cancel_work_sync(&dev->cmd_recv_work);

	/* now lets cancel all posted cmds
	 * (cmds sent but did not replied)
	 */


	list_for_each_entry_safe(cb, cb_tmp,
			&dev->cmd_post_list, dev_entry) {

		list_del(&cb->dev_entry);
		pr_info("cmd quiesce: aborting post %ps\n",
				cb->rsp_cmpl);
		cb->rsp_cmpl(cb, NULL, NULL);
	}

	/* now lets cancel all pending cmds
	 * (cmds did not sent yet)
	 */

	list_for_each_entry_safe(cb, cb_tmp,
			&dev->cmd_pend_list, dev_entry) {

		list_del(&cb->dev_entry);
		pr_info("cmd quiesce: aborting pend %ps\n",
				cb->rsp_cmpl);
		cb->rsp_cmpl(cb, NULL, NULL);
	}

	pr_info("cmd quiesce done\n");
}

void ntrdma_dev_cmd_reset(struct ntrdma_dev *dev)
{
	ntrdma_dev_cmd_hello_done_undone(dev, NULL, true);
	ntrdma_dev_cmd_hello_prep_unprep(dev, NULL, true);
}

void ntrdma_dev_cmd_enable(struct ntrdma_dev *dev)
{
	mutex_lock(&dev->cmd_send_lock);
	mutex_lock(&dev->cmd_recv_lock);
	{
		dev->cmd_ready = 1;
	}
	mutex_unlock(&dev->cmd_recv_lock);
	mutex_unlock(&dev->cmd_send_lock);

	ntrdma_cmd_recv_work(dev);
	ntrdma_cmd_send_work(dev);
}

void ntrdma_dev_cmd_disable(struct ntrdma_dev *dev)
{
	mutex_lock(&dev->cmd_send_lock);
	mutex_lock(&dev->cmd_recv_lock);
	{
		dev->cmd_ready = 0;
	}
	mutex_unlock(&dev->cmd_recv_lock);
	mutex_unlock(&dev->cmd_send_lock);
}

void ntrdma_dev_cmd_add_unsafe(struct ntrdma_dev *dev, struct ntrdma_cmd_cb *cb)
{
	WARN(!mutex_is_locked(&dev->cmd_send_lock),
			"Entered %s, without locking cmd_send_lock\n",
			__func__);

	list_add_tail(&cb->dev_entry, &dev->cmd_pend_list);
}

void ntrdma_dev_cmd_add(struct ntrdma_dev *dev, struct ntrdma_cmd_cb *cb)
{
	mutex_lock(&dev->cmd_send_lock);
	ntrdma_dev_cmd_add_unsafe(dev, cb);
	mutex_unlock(&dev->cmd_send_lock);
}

void ntrdma_dev_cmd_submit(struct ntrdma_dev *dev)
{
	schedule_work(&dev->cmd_send_work);
}

int ntrdma_dev_cmd_finish(struct ntrdma_dev *dev)
{
	int ret;

	ret = wait_event_timeout(dev->cmd_send_cond,
			ntrdma_cmd_done(dev), CMD_TIMEOUT_MSEC);

	if (!ret) {
		ntrdma_err(dev,
				"TIMEOUT: waiting for all pending commands to complete, after %d msec\n",
				CMD_TIMEOUT_MSEC);
		return -ETIME;
	}
	return 0;

}

static inline void ntrdma_cmd_send_vbell_clear(struct ntrdma_dev *dev)
{
	ntrdma_dev_vbell_clear(dev, &dev->cmd_send_vbell,
			       dev->cmd_send_vbell_idx);
}

static inline int ntrdma_cmd_send_vbell_add(struct ntrdma_dev *dev)
{
	return ntrdma_dev_vbell_add(dev, &dev->cmd_send_vbell,
				    dev->cmd_send_vbell_idx);
}

static void ntrdma_cmd_send_work(struct ntrdma_dev *dev)
{
	struct dma_chan *req;
	struct ntrdma_cmd_cb *cb;
	u32 start, pos, end, base;
	size_t off, len;
	bool more = false;
	int rc;
	union ntrdma_cmd *cmd_send_buf;
	const union ntrdma_rsp *cmd_send_rsp_buf;

	req = ntc_req_create(dev->ntc);
	if (!req)
		return; /* FIXME: no req, now what? */

	mutex_lock(&dev->cmd_send_lock);
	{
		ntrdma_cmd_send_vbell_clear(dev);

		/* Complete commands that have a response */
		ntrdma_ring_consume(ntrdma_dev_cmd_send_cons(dev),
				dev->cmd_send_cmpl,
				dev->cmd_send_cap, &start, &end, &base);
		ntrdma_vdbg(dev, "rsp start %d end %d\n", start, end);

		cmd_send_buf = ntc_local_buf_deref(&dev->cmd_send_buf);

		cmd_send_rsp_buf =
			ntc_export_buf_const_deref(&dev->cmd_send_rsp_buf,
						sizeof(*cmd_send_rsp_buf) *
						start,
						sizeof(*cmd_send_rsp_buf) *
						(end - start));
		/* Make it point to the start of dev->cmd_send_rsp_buf. */
		cmd_send_rsp_buf -= start;

		for (pos = start; pos < end; ++pos) {

			if (WARN(list_empty(&dev->cmd_post_list),
					"Corruption pos %d end %d but list is empty cons %u cmpl %u",
					pos, end, ntrdma_dev_cmd_send_cons(dev),
					dev->cmd_send_cmpl)) {
				break;
			}

			if (unlikely(pos !=
				cmd_send_rsp_buf[pos].hdr.cmd_id)) {
				ntrdma_err(dev,
					"rsp cmd id %d != pos %d, link down\n",
					cmd_send_rsp_buf[pos].hdr.cmd_id, pos);
				ntc_link_disable(dev->ntc);
			}

			cb = list_first_entry(&dev->cmd_post_list,
					struct ntrdma_cmd_cb,
					dev_entry);

			list_del(&cb->dev_entry);

			ntrdma_vdbg(dev,
					"rsp cmpl pos %d cmd_id %d\n", pos,
					cmd_send_rsp_buf[pos].hdr.cmd_id);

			TRACE("CMD: respond received for %ps pos %u\n",
				cb->rsp_cmpl, pos);

			rc = cb->rsp_cmpl(cb, &cmd_send_rsp_buf[pos], req);
			WARN(rc, "%ps failed rc = %d", cb->rsp_cmpl, rc);
			/* FIXME: command failed, now what? */
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
			if (list_empty(&dev->cmd_pend_list))
				break;
			cb = list_first_entry(&dev->cmd_pend_list,
					struct ntrdma_cmd_cb,
					dev_entry);

			list_move_tail(&cb->dev_entry, &dev->cmd_post_list);

			ntrdma_vdbg(dev, "cmd prep pos %d\n", pos);

			TRACE("CMD: post cmd by %ps pos %u\n",
				cb->cmd_prep, pos);

			cmd_send_buf[pos].hdr.cmd_id = pos;
			rc = cb->cmd_prep(cb, &cmd_send_buf[pos], req);
			WARN(rc, "%ps failed rc = %d", cb->cmd_prep, rc);
			/* FIXME: command failed, now what? */
		}

		if (pos != start) {
			ntrdma_vdbg(dev, "cmd copy start %d pos %d\n", start, pos);

			dev->cmd_send_prod = ntrdma_ring_update(pos, base,
								dev->cmd_send_cap);
			more = true;

			/* copy the portion of the ring buf */

			off = start * sizeof(union ntrdma_cmd);
			len = (pos - start) * sizeof(union ntrdma_cmd);
			rc = ntc_request_memcpy_fenced(req,
						&dev->peer_cmd_recv_buf, off,
						&dev->cmd_send_buf, off,
						len);
			if (rc < 0)
				ntrdma_err(dev,
					"ntc_request_memcpy (len=%zu) error %d",
					len, -rc);

			/* update the producer index on the peer */
			rc = ntc_request_imm32(req, &dev->peer_cmd_recv_buf,
					dev->peer_recv_prod_shift,
					dev->cmd_send_prod, true, NULL, NULL);
			if (rc < 0)
				ntrdma_err(dev,
					"ntc_request_imm32 failed. rc=%d\n",
					rc);

			/* update the vbell and signal the peer */
			ntrdma_dev_vbell_peer(dev, req,
					dev->peer_cmd_recv_vbell_idx);

			ntc_req_signal(dev->ntc, req, NULL, NULL, NTB_DEFAULT_VEC(dev->ntc));
			ntc_req_submit(dev->ntc, req);

			TRACE("CMD: Send %d cmds to pos %u vbell %u\n",
				(pos - start), start,
				dev->peer_cmd_recv_vbell_idx);

		} else {
			ntc_req_cancel(dev->ntc, req);
		}

		if (!ntrdma_cmd_done(dev)) {
			if (more || ntrdma_cmd_send_vbell_add(dev) == -EAGAIN)
				schedule_work(&dev->cmd_send_work);
		}
	}
	mutex_unlock(&dev->cmd_send_lock);

	wake_up(&dev->cmd_send_cond);
}

static void ntrdma_cmd_send_work_cb(struct work_struct *ws)
{
	struct ntrdma_dev *dev = ntrdma_cmd_send_work_dev(ws);

	if (!dev->cmd_ready) {
		pr_info("cmd not ready yet to send\n");
		return;
	}

	ntrdma_cmd_send_work(dev);
}

static void ntrdma_cmd_send_vbell_cb(void *ctx)
{
	struct ntrdma_dev *dev = ctx;

	schedule_work(&dev->cmd_send_work);
}

static int ntrdma_cmd_recv_none(struct ntrdma_dev *dev, u32 cmd_op,
				struct ntrdma_rsp_hdr *rsp)
{
	ntrdma_vdbg(dev, "called\n");

	rsp->op = cmd_op;
	rsp->status = 0;

	return 0;
}

static int ntrdma_sanity_mr_create(struct ntrdma_dev *dev,
				const struct ntrdma_cmd_mr_create *cmd)
{
	/* sanity checks for values received from peer */
	if (cmd->sg_cap > (IB_MR_LIMIT_BYTES >> PAGE_SHIFT) ||
		cmd->sg_count > NTRDMA_CMD_MR_CREATE_SG_CAP) {

		ntrdma_err(dev,
				"Invalid sg_cap %u(max %llu) sg_count %u (max %lu)\n",
				cmd->sg_cap, (IB_MR_LIMIT_BYTES >> PAGE_SHIFT),
				cmd->sg_count, NTRDMA_CMD_MR_CREATE_SG_CAP);

		return -EINVAL;
	}

	if (cmd->mr_len > IB_MR_LIMIT_BYTES) {
		ntrdma_err(dev, "mr len %llu is beyond 1GB limit (corrupted?)\n",
				cmd->mr_len);
		return -EINVAL;
	}

	if (!IS_ALIGNED(cmd->mr_addr, INTEL_ALIGN)) {
		ntrdma_err(dev, "mr addr 0x%llx is not aligned (corrupted?)\n",
				cmd->mr_addr);
		return -EINVAL;
	}

	if (cmd->access > MAX_SUM_ACCESS_FLAGS) {
		ntrdma_err(dev, "mr access flags 0x%x may be corrupted?\n",
				cmd->access);
		return -EINVAL;
	}

	return 0;
}

static int ntrdma_sanity_mr_append(struct ntrdma_dev *dev,
				const struct ntrdma_cmd_mr_append *cmd)
{
	/* sanity checks for values received from peer */
	if (cmd->sg_pos > (IB_MR_LIMIT_BYTES >> PAGE_SHIFT) ||
		cmd->sg_count > NTRDMA_CMD_MR_APPEND_SG_CAP) {
		ntrdma_err(dev, 
				"Invalid sg pos %u(%llu) sg_count %u(%lu)\n",
				cmd->sg_pos,
				(IB_MR_LIMIT_BYTES >> PAGE_SHIFT),
				cmd->sg_count,
				NTRDMA_CMD_MR_APPEND_SG_CAP);
		return -EINVAL;
	}

	return 0;
}
static int ntrdma_cmd_recv_mr_create(struct ntrdma_dev *dev,
				const struct ntrdma_cmd_mr_create *cmd,
				struct ntrdma_rsp_mr_status *rsp)
{
	struct ntrdma_rmr *rmr;
	u32 i, count;
	int rc;

	ntrdma_vdbg(dev,
			"called mr len %llx  mr addr %llx mr key %x sg_count %d\n",
			cmd->mr_len, cmd->mr_addr, cmd->mr_key, cmd->sg_count);

	rsp->hdr.op = cmd->hdr.op;
	rsp->mr_key = cmd->mr_key;

	rc = ntrdma_sanity_mr_create(dev, cmd);
	if (rc) {
		ntrdma_err(dev, "sanity failed, rc %d\n", rc);
		goto err_sanity;
	}

	rmr = kmalloc_node(sizeof(*rmr) + cmd->sg_cap * sizeof(*rmr->sg_list),
			   GFP_KERNEL, dev->node);
	if (!rmr) {
		rc = -ENOMEM;
		goto err_rmr;
	}

	rc = ntrdma_rmr_init(rmr, dev, cmd->pd_key, cmd->access,
			cmd->mr_addr, cmd->mr_len, cmd->sg_cap,
			cmd->mr_key);
	if (rc) {
		ntrdma_err(dev, "rmr init failed, rc %d\n",
						rc);
		goto err_init;
	}

	count = cmd->sg_count;

	for (i = 0; i < count; ++i) {
		rc = ntc_remote_buf_map(&rmr->sg_list[i], dev->ntc,
					&cmd->sg_desc_list[i]);
		if (rc < 0)
			goto err_map;
	}

	rc = ntrdma_rmr_add(rmr);
	if (rc) {
		ntrdma_err(dev, "failed to add RMR %p rc %d\n", rmr, rc);
		goto err_add;
	}

	rsp->hdr.status = 0;
	return 0;

err_add:
	ntrdma_rmr_deinit(rmr); /* Not sure we need this */
err_map:
	for (i--; i >= 0; i--)
		ntc_remote_buf_unmap(&rmr->sg_list[i]);
err_init:
	kfree(rmr);
err_rmr:
err_sanity:
	rsp->hdr.status = ~0;
	return rc;
}

static int ntrdma_cmd_recv_mr_delete(struct ntrdma_dev *dev,
				const struct ntrdma_cmd_mr_delete *cmd,
				struct ntrdma_rsp_mr_status *rsp)
{
	struct ntrdma_rmr *rmr;
	int rc;
	int i;

	ntrdma_vdbg(dev, "called\n");

	rsp->hdr.op = cmd->hdr.op;
	rsp->mr_key = cmd->mr_key;

	rmr = ntrdma_dev_rmr_look(dev, cmd->mr_key);
	if (!rmr) {
		ntrdma_err(dev, "rmr lock failed for key %d\n",
				cmd->mr_key);
		rc = -EINVAL;
		goto err_rmr;
	}

	for (i = 0; i < rmr->sg_count; i++)
		ntc_remote_buf_unmap(&rmr->sg_list[i]);

	ntrdma_rmr_del(rmr);
	ntrdma_rres_del(&rmr->rres);
	ntrdma_rmr_put(rmr);
	ntrdma_rmr_repo(rmr);
	ntrdma_rmr_deinit(rmr);
	kfree(rmr);

	rsp->hdr.status = 0;
	return 0;

err_rmr:
	rsp->hdr.status = ~0;
	return rc;
}

static int ntrdma_cmd_recv_mr_append(struct ntrdma_dev *dev,
				const struct ntrdma_cmd_mr_append *cmd,
				struct ntrdma_rsp_mr_status *rsp)
{
	struct ntrdma_rmr *rmr;
	u32 i, pos, count;
	int rc;

	ntrdma_vdbg(dev, "called sg count %d sg pos %d\n",
			cmd->sg_count, cmd->sg_pos);

	rsp->hdr.op = cmd->hdr.op;
	rsp->mr_key = cmd->mr_key;

	rc = ntrdma_sanity_mr_append(dev, cmd);
	if (rc) {
		ntrdma_err(dev, "sanity failed, rc %d\n", rc);
		goto err_sanity;
	}

	rmr = ntrdma_dev_rmr_look(dev, cmd->mr_key);
	if (!rmr) {
		ntrdma_err(dev, "rmr look failed for lock %d\n",
				cmd->mr_key);
		rc = -EINVAL;
		goto err_rmr;
	}

	pos = cmd->sg_pos;
	count = cmd->sg_count;

	/* TODO: this should be validated if its not corruption */

	for (i = 0; i < count; ++i) {
		rc = ntc_remote_buf_map(&rmr->sg_list[pos + i], dev->ntc,
					&cmd->sg_desc_list[i]);
		if (rc < 0)
			goto err_map;
	}

	ntrdma_rmr_put(rmr);

	rsp->hdr.status = 0;
	return 0;
err_map:
	for (--i; i >= 0 ; i--) {
		ntc_remote_buf_unmap(&rmr->sg_list[pos + i]);
	}

	ntrdma_rmr_put(rmr);
err_rmr:
err_sanity:
	rsp->hdr.status = ~0;
	return rc;
}


#define MAX_WQE_SG_CAP 128
#define MAX_WQE_CAP 4096
#define QP_NUM_TYPES 3

static int ntrdma_qp_create_sanity(struct ntrdma_dev *dev,
				const struct ntrdma_cmd_qp_create *cmd)
{
	/* currently 3 types: IBV_QPT_ RC/UC/UD */
	if (cmd->qp_type > QP_NUM_TYPES) {
		ntrdma_err(dev, "qp type has wrong type?\n");
		return -EINVAL;
	}

	if (cmd->send_wqe_cap > MAX_WQE_CAP ||
		cmd->send_wqe_sg_cap > MAX_WQE_SG_CAP ||
		cmd->send_ring_idx >= 2 * cmd->send_wqe_cap) {
		ntrdma_err(dev, "send wqe_cap %u, wqe_sg_cap %u, idx %u\n",
				cmd->send_wqe_cap, cmd->send_wqe_sg_cap,
				cmd->send_ring_idx);
				return -EINVAL;
	}

	if (cmd->recv_wqe_cap > MAX_WQE_CAP ||
		cmd->recv_wqe_sg_cap > MAX_WQE_SG_CAP ||
		cmd->recv_ring_idx >= 2 * cmd->recv_wqe_cap) {
		ntrdma_err(dev, "recv wqe_cap %u, wqe_sg_cap %u, idx %u\n",
				cmd->recv_wqe_cap, cmd->recv_wqe_sg_cap,
				cmd->recv_ring_idx);
				return -EINVAL;
	}

	if (cmd->cmpl_vbell_idx > NTRDMA_DEV_VBELL_COUNT) {
		ntrdma_err(dev, "cmpl_vbell_idx %u\n", cmd->cmpl_vbell_idx);
		return -EINVAL;
	}

	return 0;
}

static int ntrdma_cmd_recv_qp_create(struct ntrdma_dev *dev,
				const struct ntrdma_cmd_qp_create *cmd,
				struct ntrdma_rsp_qp_create *rsp)
{
	struct ntrdma_rqp *rqp;
	struct ntrdma_rqp_init_attr attr;
	int rc;

	ntrdma_vdbg(dev,
			"called qp_key %d vbell %d\n",
			 cmd->qp_key, cmd->cmpl_vbell_idx);

	TRACE("peer QP %d create received, recv cap: %d send cap %d\n",
			cmd->qp_key, cmd->recv_wqe_cap, cmd->send_wqe_cap);

	rsp->hdr.op = cmd->hdr.op;
	rsp->qp_key = cmd->qp_key;

	rc = ntrdma_qp_create_sanity(dev, cmd);
	if (rc) {
		ntrdma_err(dev, "sanity failed rc=%d\n", rc);
		goto err_sanity;
	}

	rqp = kmalloc_node(sizeof(*rqp), GFP_KERNEL, dev->node);
	if (!rqp) {
		rc = -ENOMEM;
		goto err_rqp;
	}

	attr.pd_key = cmd->pd_key;
	attr.recv_wqe_idx = cmd->recv_ring_idx;
	attr.recv_wqe_cap = cmd->recv_wqe_cap;
	attr.recv_wqe_sg_cap = cmd->recv_wqe_sg_cap;
	attr.send_wqe_idx = cmd->send_ring_idx;
	attr.send_wqe_cap = cmd->send_wqe_cap;
	attr.send_wqe_sg_cap = cmd->send_wqe_sg_cap;

	rc = ntc_remote_buf_map(&attr.peer_send_cqe_buf, dev->ntc,
				&cmd->send_cqe_buf_desc);
	if (rc < 0)
		goto err_peer_send_cqe_buf;

	attr.peer_send_cons_shift = cmd->send_cons_shift;

	attr.peer_cmpl_vbell_idx = cmd->cmpl_vbell_idx;

	rc = ntrdma_rqp_init(rqp, dev, &attr, cmd->qp_key);
	if (rc)
		goto err_init;

	ntc_export_buf_make_desc(&rsp->recv_wqe_buf_desc, &rqp->recv_wqe_buf);
	rsp->recv_prod_shift = rqp->recv_cap * rqp->recv_wqe_size;

	ntc_export_buf_make_desc(&rsp->send_wqe_buf_desc, &rqp->send_wqe_buf);

	rsp->send_prod_shift = rqp->send_cap * rqp->send_wqe_size;

	rsp->send_vbell_idx = rqp->send_vbell_idx;

	rc = ntrdma_rqp_add(rqp);
	if (rc) {
		ntrdma_err(dev, "rqp add failed rc = %d\n", rc);
		goto err_add;
	}

	rsp->hdr.status = 0;
	return 0;

err_add:
	ntrdma_rqp_deinit(rqp);
	ntc_remote_buf_unmap(&attr.peer_send_cqe_buf);
err_peer_send_cqe_buf:
err_init:
	kfree(rqp);
err_rqp:
err_sanity:
	rsp->hdr.status = ~0;
	ntc_remote_buf_desc_clear(&rsp->recv_wqe_buf_desc);
	rsp->recv_prod_shift = 0;
	ntc_remote_buf_desc_clear(&rsp->send_wqe_buf_desc);
	rsp->send_prod_shift = 0;
	rsp->send_vbell_idx = 0;
	return rc;
}

static int ntrdma_cmd_recv_qp_delete(struct ntrdma_dev *dev,
				const struct ntrdma_cmd_qp_delete *cmd,
				struct ntrdma_rsp_qp_status *rsp)
{
	struct ntrdma_rqp *rqp;
	struct ntrdma_qp *qp;
	int rc;

	ntrdma_vdbg(dev, "called\n");

	rsp->hdr.op = cmd->hdr.op;
	rsp->qp_key = cmd->qp_key;

	rqp = ntrdma_dev_rqp_look_and_get(dev, cmd->qp_key);
	if (!rqp) {
		ntrdma_err(dev, "rqp look failed key %d\n", cmd->qp_key);
		rc = -EINVAL;
		goto err_rqp;
	}
	qp = ntrdma_dev_qp_look_and_get(dev, rqp->qp_key);
	TRACE("stall qp %p (res key %d)\n", qp, qp ? qp->res.key : -1);
	ntrdma_qp_send_stall(qp, rqp);
	if (qp)
		ntrdma_qp_put(qp);

	ntc_remote_buf_unmap(&rqp->peer_send_cqe_buf);

	ntrdma_rqp_del(rqp);
	ntrdma_rres_del(&rqp->rres);
	ntrdma_rqp_put(rqp);
	ntrdma_rqp_repo(rqp);
	ntrdma_rqp_deinit(rqp);
	kfree(rqp);

	rsp->hdr.status = 0;
	return 0;

err_rqp:
	rsp->hdr.status = ~0;
	return rc;
}

static int ntrdma_cmd_recv_qp_modify(struct ntrdma_dev *dev,
				const struct ntrdma_cmd_qp_modify *cmd,
				struct ntrdma_rsp_qp_status *rsp)
{
	struct ntrdma_rqp *rqp;
	struct ntrdma_qp *qp;
	int rc;

	ntrdma_vdbg(dev, "enter state %d qp key %d\n",
			cmd->state, cmd->qp_key);

	rsp->hdr.op = cmd->hdr.op;
	rsp->qp_key = cmd->qp_key;

	/* sanity check */
	if (cmd->access > MAX_SUM_ACCESS_FLAGS || !is_state_valid(cmd->state)) {
		ntrdma_err(dev,
				"Sanity failure %d %d\n",
				cmd->access, cmd->state);

		rc = -EINVAL;
		goto err_sanity;
	}

	rqp = ntrdma_dev_rqp_look_and_get(dev, cmd->qp_key);
	if (!rqp) {
		ntrdma_err(dev, "ntrdma_dev_rqp_look failed key %d\n",
				cmd->qp_key);
		rc = -EINVAL;
		goto err_rqp;
	}
	rqp->state = cmd->state;
	rqp->access = cmd->access;

	//rqp->access = cmd->access; /* TODO: qp access flags */
	rqp->qp_key = cmd->dest_qp_key;

	tasklet_schedule(&rqp->send_work);
	ntrdma_rqp_put(rqp);

	if (is_state_error(cmd->state)) {
		qp = ntrdma_dev_qp_look_and_get(dev, cmd->dest_qp_key);
		TRACE("qp %p (%d) state changed to %d\n",
				qp, cmd->dest_qp_key, cmd->state);
		if (!qp) {
			ntrdma_info(dev,
					"ntrdma_dev_qp_look failed key %d (rqp key %d)\n",
					cmd->dest_qp_key, cmd->qp_key);
			rc = 0;
			goto err_qp;
		}
		atomic_set(&qp->state, cmd->state);
		ntrdma_qp_put(qp);
	}

err_qp:
	rsp->hdr.status = 0;
	return 0;

err_rqp:
err_sanity:
	rsp->hdr.status = ~0;
	return rc;
}

static int ntrdma_cmd_recv(struct ntrdma_dev *dev, const union ntrdma_cmd *cmd,
			union ntrdma_rsp *rsp, struct dma_chan *req)
{
	TRACE("CMD: received: op %d\n",
			cmd->hdr.op);

	rsp->hdr.cmd_id = cmd->hdr.cmd_id;

	switch (cmd->hdr.op) {
	case NTRDMA_CMD_NONE:
		return ntrdma_cmd_recv_none(dev, cmd->hdr.op, &rsp->hdr);
	case NTRDMA_CMD_MR_CREATE:
		return ntrdma_cmd_recv_mr_create(dev, &cmd->mr_create,
						 &rsp->mr_create);
	case NTRDMA_CMD_MR_DELETE:
		return ntrdma_cmd_recv_mr_delete(dev, &cmd->mr_delete,
						 &rsp->mr_delete);
	case NTRDMA_CMD_MR_APPEND:
		return ntrdma_cmd_recv_mr_append(dev, &cmd->mr_append,
						 &rsp->mr_append);
	case NTRDMA_CMD_QP_CREATE:
		return ntrdma_cmd_recv_qp_create(dev, &cmd->qp_create,
						 &rsp->qp_create);
	case NTRDMA_CMD_QP_DELETE:
		return ntrdma_cmd_recv_qp_delete(dev, &cmd->qp_delete,
						 &rsp->qp_delete);
	case NTRDMA_CMD_QP_MODIFY:
		return ntrdma_cmd_recv_qp_modify(dev, &cmd->qp_modify,
						 &rsp->qp_modify);
	}

	ntrdma_err(dev, "unhandled recv cmd op %u\n", cmd->hdr.op);

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
	struct dma_chan *req;
	u32 start, pos, end, base;
	size_t off, len;
	int rc;
	const union ntrdma_cmd *cmd_recv_buf;
	union ntrdma_rsp *cmd_recv_rsp_buf;

	ntrdma_vdbg(dev, "called\n");

	req = ntc_req_create(dev->ntc);
	if (!req)
		return; /* FIXME: no req, now what? */

	mutex_lock(&dev->cmd_recv_lock);
	{
		ntrdma_cmd_recv_vbell_clear(dev);

		cmd_recv_rsp_buf = ntc_local_buf_deref(&dev->cmd_recv_rsp_buf);

		/* Process commands */
		ntrdma_ring_consume(ntrdma_dev_cmd_recv_prod(dev),
				dev->cmd_recv_cons,
				dev->cmd_recv_cap,
				&start, &end, &base);

		ntrdma_vdbg(dev, "cmd start %d end %d\n", start, end);

		cmd_recv_buf =
			ntc_export_buf_const_deref(&dev->cmd_recv_buf,
						sizeof(*cmd_recv_buf) * start,
						sizeof(*cmd_recv_buf) *
						(end - start));
		/* Make it point to the start of dev->cmd_recv_buf. */
		cmd_recv_buf -= start;

		for (pos = start; pos < end; ++pos) {
			ntrdma_vdbg(dev, "cmd recv pos %d\n", pos);
			rc = ntrdma_cmd_recv(dev,
					&cmd_recv_buf[pos],
					&cmd_recv_rsp_buf[pos],
					req);
			WARN(rc, "ntrdma_cmd_recv failed and unhandled FIXME\n");
		}

		if (pos != start) {
			ntrdma_vdbg(dev, "rsp copy start %d pos %d\n",
				    start, pos);

			dev->cmd_recv_cons = ntrdma_ring_update(pos, base,
								dev->cmd_recv_cap);

			/* copy the portion of the ring buf */

			TRACE("CMD: send reply for %d cmds to pos %d\n",
						(pos - start), start);

			off = start * sizeof(union ntrdma_rsp);
			len = (pos - start) * sizeof(union ntrdma_rsp);
			rc = ntc_request_memcpy_fenced(req,
						&dev->peer_cmd_send_rsp_buf,
						off,
						&dev->cmd_recv_rsp_buf, off,
						len);
			if (rc < 0)
				ntrdma_err(dev,
					"ntc_request_memcpy (len=%zu) error %d",
					len, -rc);

			/* update the producer index on the peer */
			rc = ntc_request_imm32(req,
					&dev->peer_cmd_send_rsp_buf,
					dev->peer_send_cons_shift,
					dev->cmd_recv_cons, true, NULL, NULL);
			if (rc < 0)
				ntrdma_err(dev,
					"ntc_request_imm32 failed. rc=%d\n",
					rc);

			/* update the vbell and signal the peer */

			ntrdma_dev_vbell_peer(dev, req,
					      dev->peer_cmd_send_vbell_idx);
			ntc_req_signal(dev->ntc, req, NULL, NULL, NTB_DEFAULT_VEC(dev->ntc));

			ntc_req_submit(dev->ntc, req);
			schedule_work(&dev->cmd_recv_work);
		} else {
			ntc_req_cancel(dev->ntc, req);
			if (ntrdma_cmd_recv_vbell_add(dev) == -EAGAIN)
				schedule_work(&dev->cmd_recv_work);
		}
	}
	mutex_unlock(&dev->cmd_recv_lock);
}

static void ntrdma_cmd_recv_work_cb(struct work_struct *ws)
{
	struct ntrdma_dev *dev = ntrdma_cmd_recv_work_dev(ws);

	if (!dev->cmd_ready) {
		pr_info("cmd not ready yet to recv\n");
		return;
	}

	ntrdma_cmd_recv_work(dev);
}

static void ntrdma_cmd_recv_vbell_cb(void *ctx)
{
	struct ntrdma_dev *dev = ctx;

	schedule_work(&dev->cmd_recv_work);
}

