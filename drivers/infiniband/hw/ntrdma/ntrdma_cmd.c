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
#include "ntrdma_cm.h"

#define NTRDMA_RES_VBELL		1
#define NTRDMA_RRES_VBELL		0
#define MAX_CMDS 16

static void ntrdma_cmd_send_work(struct ntrdma_dev *dev);
static void ntrdma_cmd_send_work_cb(struct work_struct *ws);

static int ntrdma_cmd_recv(struct ntrdma_dev *dev, const union ntrdma_cmd *cmd,
			union ntrdma_rsp *rsp);

static void ntrdma_cmd_recv_work(struct ntrdma_dev *dev);
static void ntrdma_cmd_recv_work_cb(struct work_struct *ws);

static inline bool ntrdma_cmd_done(struct ntrdma_dev *dev)
{
	return list_empty(&dev->cmd_send.post_list) &&
		list_empty(&dev->cmd_send.pend_list);
}

static inline const u32 *ntrdma_dev_cmd_send_cons_buf(struct ntrdma_dev *dev)
{
	return ntc_export_buf_const_deref(&dev->cmd_send.rsp_buf,
					sizeof(union ntrdma_rsp) *
					dev->cmd_send.cap,
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

	ntc_export_buf_reinit(&dev->cmd_send.rsp_buf, &cmd_send_cons,
			sizeof(union ntrdma_rsp) * dev->cmd_send.cap,
			sizeof(cmd_send_cons));
}

static inline const u32 *ntrdma_dev_cmd_recv_prod_buf(struct ntrdma_dev *dev)
{
	return ntc_export_buf_const_deref(&dev->cmd_recv.buf,
					sizeof(union ntrdma_cmd) *
					dev->cmd_recv.cap,
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

	dev->cmd_send.ready = false;
	dev->cmd_recv.ready = false;

	/* recv work */
	mutex_init(&dev->cmd_recv.lock);

	ntrdma_work_vbell_init(dev, &dev->cmd_recv.vbell, recv_vbell_idx,
			&dev->cmd_recv.work);

	/* allocated in conf phase */
	dev->cmd_recv.cap = 0;
	dev->cmd_recv.cons = 0;
	ntc_export_buf_clear(&dev->cmd_recv.buf);
	ntc_local_buf_clear(&dev->cmd_recv.rsp_buf);
	dev->is_cmd_hello_done = false;
	dev->is_cmd_prep = false;

	/* assigned in ready phase */
	ntc_remote_buf_clear(&dev->cmd_recv.peer_cmd_send_rsp_buf);
	/* assigned in conf phase */
	dev->cmd_recv.peer_cmd_send_vbell_idx = 0;

	/* send work */
	INIT_LIST_HEAD(&dev->cmd_send.pend_list);
	INIT_LIST_HEAD(&dev->cmd_send.post_list);
	mutex_init(&dev->cmd_send.lock);
	ntrdma_work_vbell_init(dev, &dev->cmd_send.vbell, send_vbell_idx,
			&dev->cmd_send.work);

	/* allocate send buffers */
	dev->cmd_send.cap = send_cap;
	dev->cmd_send.prod = 0;
	dev->cmd_send.cmpl = 0;

	rc = ntc_local_buf_zalloc(&dev->cmd_send.buf, dev->ntc,
				dev->cmd_send.cap * sizeof(union ntrdma_cmd),
				GFP_KERNEL);
	if (rc < 0) {
		ntrdma_err(dev, "dma mapping failed\n");
		goto err_send_buf;
	}

	rc = ntc_export_buf_zalloc(&dev->cmd_send.rsp_buf, dev->ntc,
				dev->cmd_send.cap * sizeof(union ntrdma_rsp)
				+ sizeof(u32), /* for cmd_send_cons */
				GFP_KERNEL);
	if (rc < 0) {
		ntrdma_err(dev, "dma mapping failed\n");
		goto err_send_rsp_buf;
	}

	INIT_WORK(&dev->cmd_send.work,
			ntrdma_cmd_send_work_cb);
	INIT_WORK(&dev->cmd_recv.work,
			ntrdma_cmd_recv_work_cb);
	return 0;
deinit:
	WARN(dev->is_cmd_hello_done, "Deinit while cmd hello not undone");
	WARN(dev->is_cmd_prep, "Deinit while cmd prep not unprep");
	ntrdma_work_vbell_kill(&dev->cmd_send.vbell);
	ntrdma_work_vbell_kill(&dev->cmd_recv.vbell);
	ntc_export_buf_free(&dev->cmd_send.rsp_buf);
err_send_rsp_buf:
	ntc_local_buf_free(&dev->cmd_send.buf, dev->ntc);
err_send_buf:
	return rc;
}

int ntrdma_dev_cmd_init(struct ntrdma_dev *dev,
			u32 recv_vbell_idx,
			u32 send_vbell_idx,
			u32 send_cap)
{
	if (unlikely(recv_vbell_idx >= NTRDMA_DEV_VBELL_COUNT)) {
		ntrdma_err(dev, "invalid recv_vbell_idx. idx %d >= %d",
			recv_vbell_idx, NTRDMA_DEV_VBELL_COUNT);
		return -EINVAL;
	}

	if (unlikely(send_vbell_idx >= NTRDMA_DEV_VBELL_COUNT)) {
		ntrdma_err(dev, "invalid send_vbell_idx. idx %d >= %d",
			send_vbell_idx, NTRDMA_DEV_VBELL_COUNT);
		return -EINVAL;
	}

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

	rc = ntc_remote_buf_map(&dev->cmd_send.peer_cmd_recv_buf, dev->ntc,
				&peer_prep->recv_buf_desc);
	if (rc < 0) {
		ntrdma_err(dev, "peer cmd recv buff map failed");
		goto err_peer_cmd_recv_buf;
	}

	dev->cmd_send.peer_recv_prod_shift = peer_prep->recv_prod_shift;

	dev->is_cmd_hello_done = true;

	return 0;
undone:
	if (!dev->is_cmd_hello_done)
		return 0;

	dev->is_cmd_hello_done = false;
	ntc_remote_buf_unmap(&dev->cmd_send.peer_cmd_recv_buf, dev->ntc);
err_peer_cmd_recv_buf:
	return rc;
}

void ntrdma_dev_cmd_deinit(struct ntrdma_dev *dev)
{
	ntrdma_dev_cmd_init_deinit(dev, 0, 0, 0, true);
}

void ntrdma_dev_cmd_hello_info(struct ntrdma_dev *dev,
			struct ntrdma_cmd_hello_info __iomem *info)
{
	struct ntc_remote_buf_desc send_rsp_buf_desc;

	ntc_export_buf_make_desc(&send_rsp_buf_desc, &dev->cmd_send.rsp_buf);
	memcpy_toio(&info->send_rsp_buf_desc, &send_rsp_buf_desc,
		sizeof(send_rsp_buf_desc));

	iowrite64(dev->cmd_send.cap * sizeof(union ntrdma_rsp),
		&info->send_cons_shift);
	iowrite32(dev->cmd_send.cap, &info->send_cap);
	iowrite32(ntrdma_dev_cmd_send_cons(dev), &info->send_idx);
	iowrite32(dev->cmd_send.vbell.idx, &info->send_vbell_idx);
	iowrite32(dev->cmd_recv.vbell.idx, &info->recv_vbell_idx);
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

	rc = ntc_remote_buf_map(&dev->cmd_recv.peer_cmd_send_rsp_buf, dev->ntc,
				&peer_info->send_rsp_buf_desc);
	if (rc < 0) {
		ntrdma_err(dev, "peer_cmd_send_rsp_buf map failed");
		goto err_peer_cmd_send_rsp_buf;
	}

	dev->cmd_recv.peer_send_cons_shift = peer_info->send_cons_shift;

	dev->cmd_recv.peer_cmd_send_vbell_idx = peer_info->send_vbell_idx;
	dev->cmd_send.peer_cmd_recv_vbell_idx = peer_info->recv_vbell_idx;

	/* allocate local recv ring for peer send */
	dev->cmd_recv.cap = peer_info->send_cap;
	dev->cmd_recv.cons = peer_info->send_idx;

	rc = ntc_export_buf_zalloc(&dev->cmd_recv.buf, dev->ntc,
				dev->cmd_recv.cap * sizeof(union ntrdma_cmd)
				+ sizeof(u32), /* for cmd_recv_prod */
				GFP_KERNEL);
	if (rc < 0) {
		ntrdma_err(dev, "cmd recv buffer dma mapping failed\n");
		goto err_recv_buf;
	}

	rc = ntc_local_buf_zalloc(&dev->cmd_recv.rsp_buf, dev->ntc,
				dev->cmd_recv.cap * sizeof(union ntrdma_rsp),
				GFP_KERNEL);
	if (rc < 0) {
		ntrdma_err(dev, "cmd recv resp buffer dma mapping failed\n");
		goto err_recv_rsp_buf;
	}

	dev->is_cmd_prep = true;

	return 0;
deinit:
	if (!dev->is_cmd_prep)
		return 0;
	dev->is_cmd_prep = false;
	ntc_local_buf_free(&dev->cmd_recv.rsp_buf, dev->ntc);
err_recv_rsp_buf:
	ntc_export_buf_free(&dev->cmd_recv.buf);
err_recv_buf:
	ntrdma_dev_clear_cmd_send_cons(dev);
	dev->cmd_send.cmpl = 0;
	dev->cmd_send.prod = 0;
	dev->cmd_recv.cons = 0;
	dev->cmd_recv.cap = 0;
	dev->cmd_recv.peer_cmd_send_vbell_idx = 0;
	dev->cmd_send.peer_cmd_recv_vbell_idx = 0;
	ntc_remote_buf_unmap(&dev->cmd_recv.peer_cmd_send_rsp_buf, dev->ntc);
err_peer_cmd_send_rsp_buf:
err_sanity:
	return rc;
}

int ntrdma_dev_cmd_hello_prep(struct ntrdma_dev *dev,
			const struct ntrdma_cmd_hello_info *peer_info,
			struct ntrdma_cmd_hello_prep __iomem *prep)
{
	struct ntc_remote_buf_desc recv_buf_desc;
	int rc;

	rc = ntrdma_dev_cmd_hello_prep_unprep(dev, peer_info, false);
	if (rc)
		return rc;

	ntc_export_buf_make_desc(&recv_buf_desc, &dev->cmd_recv.buf);
	memcpy_toio(&prep->recv_buf_desc, &recv_buf_desc,
		sizeof(recv_buf_desc));

	iowrite64(dev->cmd_recv.cap * sizeof(union ntrdma_cmd),
		&prep->recv_prod_shift);

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

	ntrdma_info(dev, "cmd quiesce starting ...");

	ntrdma_work_vbell_flush(&dev->cmd_send.vbell);
	ntrdma_work_vbell_flush(&dev->cmd_recv.vbell);
	ntrdma_vbell_enable(&dev->cmd_recv.vbell);
	ntrdma_vbell_enable(&dev->cmd_send.vbell);

	/* now lets cancel all posted cmds
	 * (cmds sent but did not replied)
	 */



	list_for_each_entry_safe(cb, cb_tmp,
			&dev->cmd_send.post_list, dev_entry) {

		list_del(&cb->dev_entry);
		cb->in_list = false;
		ntrdma_info(dev, "cmd quiesce: aborting post %ps",
			cb->rsp_cmpl);
		complete_all(&cb->cmds_done);
	}

	/* now lets cancel all pending cmds
	 * (cmds did not sent yet)
	 */

	/*FIXME we should lock here (cmd_send_lock)
	 * might be a race with ntrdma_cmd_cb_unlink
	 */
	list_for_each_entry_safe(cb, cb_tmp,
			&dev->cmd_send.pend_list, dev_entry) {

		list_del(&cb->dev_entry);
		cb->in_list = false;
		ntrdma_info(dev, "cmd quiesce: aborting pend %ps",
			cb->rsp_cmpl);
		complete_all(&cb->cmds_done);
	}

	ntrdma_info(dev, "cmd quiesce done");
}

void ntrdma_dev_cmd_reset(struct ntrdma_dev *dev)
{
	ntrdma_dev_cmd_hello_done_undone(dev, NULL, true);
	ntrdma_dev_cmd_hello_prep_unprep(dev, NULL, true);
}

void ntrdma_dev_cmd_enable(struct ntrdma_dev *dev)
{
	mutex_lock(&dev->cmd_send.lock);
	dev->cmd_send.ready = true;
	mutex_unlock(&dev->cmd_send.lock);

	mutex_lock(&dev->cmd_recv.lock);
	dev->cmd_recv.ready = true;
	mutex_unlock(&dev->cmd_recv.lock);


	ntrdma_vbell_trigger(&dev->cmd_recv.vbell);
	ntrdma_vbell_trigger(&dev->cmd_send.vbell);
}

void ntrdma_dev_cmd_disable(struct ntrdma_dev *dev)
{
	/* Verify cmd recv not running any more */
	mutex_lock(&dev->cmd_recv.lock);
	dev->cmd_recv.ready = false;
	mutex_unlock(&dev->cmd_recv.lock);

	/* Verify cmd send not running any more */
	mutex_lock(&dev->cmd_send.lock);
	dev->cmd_send.ready = false;
	mutex_unlock(&dev->cmd_send.lock);
}

void ntrdma_dev_cmd_add_unsafe(struct ntrdma_dev *dev, struct ntrdma_cmd_cb *cb)
{
	WARN(!mutex_is_locked(&dev->cmd_send.lock),
			"Entered %s, without locking cmd_send_lock\n",
			__func__);

	cb->in_list = true;
	list_add_tail(&cb->dev_entry, &dev->cmd_send.pend_list);
}

int ntrdma_dev_cmd_add(struct ntrdma_dev *dev, struct ntrdma_cmd_cb *cb)
{
	int rc = -1;

	mutex_lock(&dev->cmd_send.lock);
	if (dev->cmd_send.ready) {
		ntrdma_dev_cmd_add_unsafe(dev, cb);
		rc = 0;
	}
	mutex_unlock(&dev->cmd_send.lock);

	return rc;
}

static void ntrdma_cmd_send_work(struct ntrdma_dev *dev)
{
	struct ntrdma_cmd_cb *cb;
	u32 start, pos, end, base;
	size_t off, len;
	bool more = false;
	int rc;
	union ntrdma_cmd *cmd_send_buf;
	const union ntrdma_rsp *cmd_send_rsp_buf;
	u32 cmd_id;


	ntrdma_vbell_clear(&dev->cmd_send.vbell);

	/* Complete commands that have a response */
	ntrdma_ring_consume(ntrdma_dev_cmd_send_cons(dev),
			dev->cmd_send.cmpl,
			dev->cmd_send.cap, &start, &end, &base);
	ntrdma_vdbg(dev, "rsp start %d end %d\n", start, end);

	cmd_send_buf = ntc_local_buf_deref(&dev->cmd_send.buf);

	cmd_send_rsp_buf =
			ntc_export_buf_const_deref(&dev->cmd_send.rsp_buf,
					sizeof(*cmd_send_rsp_buf) *
					start,
					sizeof(*cmd_send_rsp_buf) *
					(end - start));
	/* Make it point to the start of dev->cmd_send.cmd_send_rsp_buf. */
	cmd_send_rsp_buf -= start;

	for (pos = start; pos < end; ++pos) {

		cmd_id = READ_ONCE(cmd_send_rsp_buf[pos].hdr.cmd_id);

		if (unlikely(pos != cmd_id)) {
			ntrdma_err(dev,
					"rsp cmd id %d != pos %d, link down\n",
					cmd_id, pos);
			ntc_link_disable(dev->ntc);
		}

		if (unlikely(list_empty(&dev->cmd_send.post_list))) {
			ntrdma_info(dev, "Skipping timeouted command");
			continue;
		}

		cb = list_first_entry(&dev->cmd_send.post_list,
				struct ntrdma_cmd_cb,
				dev_entry);

		if (unlikely(cb->cmd_id != pos)) {
			ntrdma_info(dev, "Skipping timeouted command");
			continue;
		}

		list_del(&cb->dev_entry);
		cb->in_list = false;

		ntrdma_vdbg(dev, "rsp cmpl pos %d cmd_id %d", pos,
				cmd_id);

		TRACE("CMD: respond received for %p (func) pos %u\n",
				cb->rsp_cmpl, pos);

		rc = cb->rsp_cmpl(cb, &cmd_send_rsp_buf[pos]);
		if (rc)
			ntrdma_err(dev,
					"%ps failed rc = %d", cb->rsp_cmpl,
					rc);
		/* FIXME: command failed, now what? */
	}

	if (pos != start) {
		dev->cmd_send.cmpl = ntrdma_ring_update(pos, base,
				dev->cmd_send.cap);
		more = true;
	}

	/* Issue some more pending commands */
	ntrdma_ring_produce(dev->cmd_send.prod,
			dev->cmd_send.cmpl,
			dev->cmd_send.cap,
			&start, &end, &base);
	ntrdma_vdbg(dev, "cmd start %d end %d\n", start, end);
	for (pos = start; pos < end; ++pos) {
		if (list_empty(&dev->cmd_send.pend_list))
			break;
		cb = list_first_entry(&dev->cmd_send.pend_list,
				struct ntrdma_cmd_cb,
				dev_entry);
		cb->cmd_id = pos;

		list_move_tail(&cb->dev_entry, &dev->cmd_send.post_list);

		ntrdma_vdbg(dev, "cmd prep pos %d\n", pos);

		TRACE("CMD: post cmd by %p (func) pos %u\n",
				cb->cmd_prep, pos);

		cmd_send_buf[pos].hdr.cmd_id = pos;
		cmd_send_buf[pos].hdr.cb_p = (u64)cb;
		rc = cb->cmd_prep(cb, &cmd_send_buf[pos]);
		if (rc)
			ntrdma_err(dev,
					"%ps failed rc = %d", cb->cmd_prep,
					rc);
		/* FIXME: command failed, now what? */
	}

	rc = 0;
	if (pos != start) {
		ntrdma_vdbg(dev, "cmd copy start %d pos %d\n", start, pos);

		dev->cmd_send.prod = ntrdma_ring_update(pos, base,
				dev->cmd_send.cap);
		more = true;

		/* copy the portion of the ring buf */

		off = start * sizeof(union ntrdma_cmd);
		len = (pos - start) * sizeof(union ntrdma_cmd);
		rc = ntc_memcpy(&dev->cmd_send.peer_cmd_recv_buf, off,
				&dev->cmd_send.buf, off, len);
		if (unlikely(rc < 0)) {
			ntrdma_err(dev,
					"ntc_memcpy failed. rc=%d", rc);
			goto dma_err;
		}

		/* update the producer index on the peer */
		rc = ntc_imm32(&dev->cmd_send.peer_cmd_recv_buf,
				dev->cmd_send.peer_recv_prod_shift,
				dev->cmd_send.prod);
		if (unlikely(rc < 0)) {
			ntrdma_err(dev,
					"ntc_imm32 failed. rc=%d", rc);
			goto dma_err;
		}

		/* update the vbell and signal the peer */
		rc = ntrdma_dev_vbell_peer_direct(dev,
				dev->cmd_send.peer_cmd_recv_vbell_idx);
		if (unlikely(rc < 0)) {
			ntrdma_err(dev,
					"ntrdma_dev_vbell_peer_direct: rc=%d",
					rc);
			goto dma_err;
		}

		rc = ntc_signal(dev->ntc, NTB_DEFAULT_VEC(dev->ntc));
		if (unlikely(rc < 0)) {
			ntrdma_err(dev, "ntc_signal failed. rc=%d",
					rc);
			goto dma_err;
		}

		TRACE("CMD: Send %d cmds to pos %u vbell %u\n",
				(pos - start), start,
				dev->cmd_send.peer_cmd_recv_vbell_idx);
	}

	 dma_err:
		if (unlikely(rc < 0))
			ntrdma_unrecoverable_err(dev);
		else if (!ntrdma_cmd_done(dev)) {
			if (more)
				ntrdma_vbell_trigger(&dev->cmd_send.vbell);
			else
				ntrdma_vbell_readd(&dev->cmd_send.vbell);
		}
}

static void ntrdma_cmd_send_work_cb(struct work_struct *ws)
{
	struct ntrdma_dev *dev = ntrdma_cmd_send_work_dev(ws);

	mutex_lock(&dev->cmd_send.lock);

	if (!dev->cmd_send.ready) {
		mutex_unlock(&dev->cmd_send.lock);
		ntrdma_info(dev, "cmd not ready yet to send");
		return;
	}

	ntrdma_cmd_send_work(dev);
	mutex_unlock(&dev->cmd_send.lock);
}

static int ntrdma_cmd_recv_none(struct ntrdma_dev *dev,
				const struct ntrdma_cmd_hdr *_cmd,
				struct ntrdma_rsp_hdr *rsp)
{
	struct ntrdma_cmd_hdr cmd;

	cmd = READ_ONCE(*_cmd);
	rsp->cmd_id = cmd.cmd_id;

	ntrdma_vdbg(dev, "called\n");

	rsp->op = NTRDMA_CMD_NONE;
	rsp->status = 0;

	return 0;
}

static int ntrdma_sanity_mr_create(struct ntrdma_dev *dev,
				const struct ntrdma_cmd_mr_create *cmd)
{
	/* sanity checks for values received from peer */
	if (cmd->sg_cap > (IB_MR_LIMIT_BYTES >> PAGE_SHIFT) ||
		cmd->sg_count > NTRDMA_CMD_MR_CREATE_SG_CAP ||
		cmd->sg_count > cmd->sg_cap) {

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
				const struct ntrdma_cmd_mr_append *cmd,
				struct ntrdma_rmr *rmr)
{
	/* sanity checks for values received from peer */
	if (cmd->sg_pos > (IB_MR_LIMIT_BYTES >> PAGE_SHIFT) ||
		cmd->sg_count > NTRDMA_CMD_MR_APPEND_SG_CAP ||
		cmd->sg_pos + cmd->sg_count < cmd->sg_pos ||
		cmd->sg_pos + cmd->sg_count > rmr->sg_count) {
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
				const struct ntrdma_cmd_mr_create *_cmd,
				struct ntrdma_rsp_mr_status *rsp)
{
	struct ntrdma_cmd_mr_create cmd;
	struct ntc_remote_buf_desc sg_desc_list;
	struct ntrdma_rmr *rmr;
	int i;
	u32 count;
	int rc;

	cmd = READ_ONCE(*_cmd);
	rsp->hdr.cmd_id = cmd.hdr.cmd_id;

	count = cmd.sg_count;

	ntrdma_vdbg(dev,
			"called mr len %llx  mr addr %llx mr key %x sg_count %d\n",
			cmd.mr_len, cmd.mr_addr, cmd.mr_key, count);

	rsp->hdr.op = NTRDMA_CMD_MR_CREATE;
	rsp->mr_key = cmd.mr_key;

	rc = ntrdma_sanity_mr_create(dev, &cmd);
	if (rc) {
		ntrdma_err(dev, "sanity failed, rc %d\n", rc);
		goto err_sanity;
	}

	rmr = kmalloc_node(sizeof(*rmr) + cmd.sg_cap * sizeof(*rmr->sg_list),
			   GFP_KERNEL, dev->node);
	if (!rmr) {
		ntrdma_err(dev, "rmr alloc failed for size %ld",
			sizeof(*rmr) + cmd.sg_cap * sizeof(*rmr->sg_list));
		rc = -ENOMEM;
		goto err_rmr;
	}

	ntrdma_rmr_init(rmr, dev, cmd.pd_key, cmd.access,
			cmd.mr_addr, cmd.mr_len, cmd.sg_cap,
			cmd.mr_key);

	for (i = 0; i < count; ++i) {
		sg_desc_list = READ_ONCE(_cmd->sg_desc_list[i]);

		rc = ntc_remote_buf_map(&rmr->sg_list[i], dev->ntc,
					&sg_desc_list);
		if (rc < 0) {
			ntrdma_err(dev, "rmr (%p) sg_list[%d] map failed",
					rmr, i);
			goto err_map;
		}
	}

	rc = ntrdma_rmr_add(rmr);
	if (rc) {
		ntrdma_err(dev, "failed to add RMR %p rc %d\n", rmr, rc);
		goto err_add;
	}

	rsp->hdr.status = 0;
	return 0;

err_add:
err_map:
	for (--i; i >= 0; i--)
		ntc_remote_buf_unmap(&rmr->sg_list[i], dev->ntc);
	kfree(rmr);
err_rmr:
err_sanity:
	rsp->hdr.status = ~0;
	return rc;
}

static int ntrdma_cmd_recv_mr_delete(struct ntrdma_dev *dev,
				const struct ntrdma_cmd_mr_delete *_cmd,
				struct ntrdma_rsp_mr_status *rsp)
{
	struct ntrdma_cmd_mr_delete cmd;
	struct ntrdma_rmr *rmr;
	struct completion done;
	int rc;

	cmd = READ_ONCE(*_cmd);
	rsp->hdr.cmd_id = cmd.hdr.cmd_id;

	ntrdma_vdbg(dev, "called\n");

	rsp->hdr.op = NTRDMA_CMD_MR_DELETE;
	rsp->mr_key = cmd.mr_key;

	rmr = ntrdma_dev_rmr_look(dev, cmd.mr_key);
	if (!rmr) {
		ntrdma_err(dev, "rmr lock failed for key %d\n",
				cmd.mr_key);
		rc = -EINVAL;
		goto err_rmr;
	}

	ntrdma_rres_remove(&rmr->rres);

	init_completion(&done);
	rmr->done = &done;

	ntrdma_rmr_put(rmr);
	ntrdma_rmr_put(rmr);

	wait_for_completion(&done);
	ntc_flush_dma_channels(dev->ntc);

	rsp->hdr.status = 0;
	return 0;

err_rmr:
	rsp->hdr.status = ~0;
	return rc;
}

static int ntrdma_cmd_recv_mr_append(struct ntrdma_dev *dev,
				const struct ntrdma_cmd_mr_append *_cmd,
				struct ntrdma_rsp_mr_status *rsp)
{
	struct ntrdma_cmd_mr_append cmd;
	struct ntc_remote_buf_desc sg_desc_list;
	struct ntrdma_rmr *rmr;
	int i;
	u32 pos, count;
	int rc;

	cmd = READ_ONCE(*_cmd);
	rsp->hdr.cmd_id = cmd.hdr.cmd_id;

	pos = cmd.sg_pos;
	count = cmd.sg_count;

	ntrdma_vdbg(dev, "called sg count %d sg pos %d\n", count, pos);

	rsp->hdr.op = NTRDMA_CMD_MR_APPEND;
	rsp->mr_key = cmd.mr_key;

	rmr = ntrdma_dev_rmr_look(dev, cmd.mr_key);
	if (!rmr) {
		ntrdma_err(dev, "rmr look failed for lock %d\n",
				cmd.mr_key);
		rc = -EINVAL;
		goto err_rmr;
	}

	rc = ntrdma_sanity_mr_append(dev, &cmd, rmr);
	if (rc) {
		ntrdma_err(dev, "sanity failed, rc %d\n", rc);
		goto err_sanity;
	}

	for (i = 0; i < count; ++i) {
		if (ntc_remote_buf_valid(&rmr->sg_list[pos + i])) {
			ntrdma_err(dev, "rmr %p sg_list[%d] invalid",
					rmr, pos + i);
			rc = -EINVAL;
			goto err_map;
		}

		sg_desc_list = READ_ONCE(_cmd->sg_desc_list[i]);

		rc = ntc_remote_buf_map(&rmr->sg_list[pos + i], dev->ntc,
					&sg_desc_list);
		if (rc < 0) {
			ntrdma_err(dev, "rmr %p sg_list[%d] map fail rc %d",
					rmr, pos + i, rc);
			goto err_map;
		}
	}

	ntrdma_rmr_put(rmr);

	rsp->hdr.status = 0;
	return 0;
err_map:
	for (--i; i >= 0 ; i--) {
		ntc_remote_buf_unmap(&rmr->sg_list[pos + i], dev->ntc);
	}

err_sanity:
	ntrdma_rmr_put(rmr);
err_rmr:
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
				const struct ntrdma_cmd_qp_create *_cmd,
				struct ntrdma_rsp_qp_create *rsp)
{
	struct ntrdma_cmd_qp_create cmd;
	struct ntrdma_rqp *rqp;
	struct ntrdma_rqp_init_attr attr;
	int rc;

	cmd = READ_ONCE(*_cmd);
	rsp->hdr.cmd_id = cmd.hdr.cmd_id;

	ntrdma_vdbg(dev,
			"called qp_key %d vbell %d\n",
			 cmd.qp_key, cmd.cmpl_vbell_idx);

	TRACE("peer QP %d create received, recv cap: %d send cap %d\n",
			cmd.qp_key, cmd.recv_wqe_cap, cmd.send_wqe_cap);

	rsp->hdr.op = NTRDMA_CMD_QP_CREATE;
	rsp->qp_key = cmd.qp_key;

	rc = ntrdma_qp_create_sanity(dev, &cmd);
	if (rc) {
		ntrdma_err(dev, "sanity failed rc=%d\n", rc);
		goto err_sanity;
	}

	rqp = ntrdma_alloc_rqp(GFP_KERNEL, dev);
	if (!rqp) {
		ntrdma_err(dev, "RQP %d alloc failed", cmd.qp_key);
		rc = -ENOMEM;
		goto err_rqp;
	}

	mutex_init(&rqp->lock);

	attr.pd_key = cmd.pd_key;
	attr.recv_wqe_idx = cmd.recv_ring_idx;
	attr.recv_wqe_cap = cmd.recv_wqe_cap;
	attr.recv_wqe_sg_cap = cmd.recv_wqe_sg_cap;
	attr.send_wqe_idx = cmd.send_ring_idx;
	attr.send_wqe_cap = cmd.send_wqe_cap;
	attr.send_wqe_sg_cap = cmd.send_wqe_sg_cap;
	attr.send_wqe_inline_cap = cmd.send_wqe_inline_cap;

	rc = ntc_remote_buf_map(&attr.peer_send_cqe_buf, dev->ntc,
				&cmd.send_cqe_buf_desc);
	if (rc < 0)
		goto err_peer_send_cqe_buf;

	attr.peer_send_cons_shift = cmd.send_cons_shift;

	attr.peer_cmpl_vbell_idx = cmd.cmpl_vbell_idx;

	rc = ntrdma_rqp_init(rqp, dev, &attr, cmd.qp_key);
	if (rc) {
		ntrdma_err(dev, "RQP %d init failed", cmd.qp_key);
		goto err_init;
	}

	ntc_export_buf_make_desc(&rsp->recv_wqe_buf_desc, &rqp->recv_wqe_buf);
	rsp->recv_prod_shift = rqp->recv_cap * rqp->recv_wqe_size;

	ntc_export_buf_make_desc(&rsp->send_wqe_buf_desc, &rqp->send_wqe_buf);

	rsp->send_prod_shift = rqp->send_cap * rqp->send_wqe_size;

	rsp->send_vbell_idx = rqp->send_vbell.idx;

	rc = ntrdma_rqp_add(rqp);
	if (rc) {
		ntrdma_err(dev, "RQP %d add failed rc = %d\n", cmd.qp_key, rc);
		goto err_add;
	}

	rsp->hdr.status = 0;
	ntrdma_dbg(dev, "success %d\n", rc);
	return 0;

err_add:
	ntrdma_rqp_deinit(rqp);
	ntc_remote_buf_unmap(&attr.peer_send_cqe_buf, dev->ntc);
err_peer_send_cqe_buf:
err_init:
	ntrdma_free_rqp(rqp);
err_rqp:
err_sanity:
	rsp->hdr.status = ~0;
	ntc_remote_buf_desc_clear(&rsp->recv_wqe_buf_desc);
	rsp->recv_prod_shift = 0;
	ntc_remote_buf_desc_clear(&rsp->send_wqe_buf_desc);
	rsp->send_prod_shift = 0;
	rsp->send_vbell_idx = 0;
	ntrdma_dbg(dev, "ntrdma_cmd_recv_qp_create failed %d\n", rc);
	return rc;
}

static int ntrdma_cmd_recv_qp_delete(struct ntrdma_dev *dev,
				const struct ntrdma_cmd_qp_delete *_cmd,
				struct ntrdma_rsp_qp_status *rsp)
{
	struct ntrdma_cmd_qp_delete cmd;
	struct ntrdma_rqp *rqp;
	struct ntrdma_qp *qp;
	int rc;

	cmd = READ_ONCE(*_cmd);
	rsp->hdr.cmd_id = cmd.hdr.cmd_id;

	ntrdma_dbg(dev, "deleting  RQP %d\n", cmd.qp_key);

	rsp->hdr.op = NTRDMA_CMD_QP_DELETE;
	rsp->qp_key = cmd.qp_key;

	rqp = ntrdma_dev_rqp_look_and_get(dev, cmd.qp_key);
	if (!rqp) {
		ntrdma_err(dev, "rqp look failed key %d\n", cmd.qp_key);
		rc = -EINVAL;
		goto err_rqp;
	}
	qp = ntrdma_dev_qp_look_and_get(dev, rqp->qp_key);

	ntrdma_vdbg(dev, "stall QP %d\n", qp ? qp->res.key : -1);


	if (qp) {
		ntrdma_res_lock(&qp->res);
		ntrdma_qp_send_stall(qp, rqp);
		qp->rqp_key = -1;
		ntrdma_cm_kill(qp);
		ntrdma_res_unlock(&qp->res);
		ntrdma_qp_put(qp);
	} else {
		ntrdma_qp_send_stall(NULL, rqp);
	}

	ntrdma_rres_remove(&rqp->rres);
	ntrdma_rqp_del(rqp);
	ntrdma_rqp_put(rqp);
	ntrdma_rqp_put(rqp);

	rsp->hdr.status = 0;
	return 0;

err_rqp:
	rsp->hdr.status = ~0;
	return rc;
}

int _ntrmda_rqp_modify_local(struct ntrdma_dev *dev,
		u32 src_qp_key, u32 access,
		u32 new_state, u32 dest_qp_key, const char *caller)
{
	struct ntrdma_rqp *rqp;
	u32 qp_according_to_rqp;
	struct ntrdma_qp *qp;

	rqp = ntrdma_dev_rqp_look_and_get(dev, src_qp_key);
	if (!rqp) {
		ntrdma_err(dev, "ntrdma_dev_rqp_look failed RQP %d from %s",
				src_qp_key, caller);
		return -EINVAL;
	}

	mutex_lock(&rqp->lock);

	rqp->state = new_state;

	if (access)
		rqp->access = access;

	if (!is_state_error(new_state)) {
		rqp->qp_key = dest_qp_key;
		ntrdma_dbg(dev, "RQP %d got QP %d from %s\n",
				rqp->rres.key, rqp->qp_key, caller);
	}

	ntrdma_vbell_trigger(&rqp->send_vbell);
	qp_according_to_rqp = rqp->qp_key;
	mutex_unlock(&rqp->lock);
	ntrdma_rqp_put(rqp);

	if (is_state_error(new_state) &&
				(qp_according_to_rqp == dest_qp_key)) {
		qp = ntrdma_dev_qp_look_and_get(dev, dest_qp_key);
		ntrdma_dbg(dev, "qp %p (QP %d) state changed to %d from %s", qp,
				dest_qp_key, new_state, caller);
		if (!qp) {
			ntrdma_dbg(dev,
					"ntrdma_dev_qp_look failed QP %d (RQP %d) from %s",
					dest_qp_key, src_qp_key, caller);
			return 0;
		}

		ntrdma_res_lock(&qp->res);

		if (new_state > IB_QPS_RTS && qp->cm_id &&
				new_state != atomic_read(&qp->state)) {
			ntrdma_dbg(dev,
					"QP %d state modified to %d, firing disconnect event caller %s\n",
					qp->res.key, new_state, caller);
			ntrdma_cm_kill(qp);
		}

		ntrdma_dbg(dev, "QP %d state modified %d -> %d by RQP %d caller %s\n",
				qp->res.key, atomic_read(&qp->state),
				new_state, src_qp_key, caller);

		atomic_set(&qp->state, new_state);
		ntrdma_res_unlock(&qp->res);
		ntrdma_qp_put(qp);
	}

	return 0;
}

int ntrdma_cmd_recv_qp_modify(struct ntrdma_dev *dev,
				const struct ntrdma_cmd_qp_modify *_cmd,
				struct ntrdma_rsp_qp_status *rsp)
{
	struct ntrdma_cmd_qp_modify cmd;
	int rc;

	cmd = READ_ONCE(*_cmd);
	rsp->hdr.cmd_id = cmd.hdr.cmd_id;

	rsp->hdr.op = NTRDMA_CMD_QP_MODIFY;
	rsp->qp_key = cmd.src_qp_key;

	ntrdma_vdbg(dev, "enter state %d qp key %d\n",
			cmd.state, cmd.src_qp_key);

	/* sanity check */
	if (cmd.access > MAX_SUM_ACCESS_FLAGS || !is_state_valid(cmd.state)) {
		ntrdma_err(dev, "Sanity failure %d %d", cmd.access, cmd.state);
		return -EINVAL;
	}

	rc = ntrmda_rqp_modify_local(dev,
			_cmd->src_qp_key, _cmd->access,
			_cmd->state, _cmd->dest_qp_key);

	rsp->hdr.status = rc;

	return rc;
}

int ntrdma_cmd_recv_cm(struct ntrdma_dev *dev,
		const union ntrdma_cmd *cmd,
		union ntrdma_rsp *rsp);

static int ntrdma_cmd_recv(struct ntrdma_dev *dev, const union ntrdma_cmd *cmd,
			union ntrdma_rsp *rsp)
{
	u32 op;
	int rc;

	op = READ_ONCE(cmd->hdr.op);

	ntrdma_dbg(dev, "CMD: received: op %d cb %p cmd id %u\n",
			op, (void *)cmd->hdr.cb_p, cmd->hdr.cmd_id);

	switch (op) {
	case NTRDMA_CMD_NONE:
		rc = ntrdma_cmd_recv_none(dev, &cmd->hdr, &rsp->hdr);
		break;
	case NTRDMA_CMD_MR_CREATE:
		rc = ntrdma_cmd_recv_mr_create(dev, &cmd->mr_create,
						 &rsp->mr_create);
		break;
	case NTRDMA_CMD_MR_DELETE:
		rc = ntrdma_cmd_recv_mr_delete(dev, &cmd->mr_delete,
						 &rsp->mr_delete);
		break;
	case NTRDMA_CMD_MR_APPEND:
		rc = ntrdma_cmd_recv_mr_append(dev, &cmd->mr_append,
						 &rsp->mr_append);
		break;
	case NTRDMA_CMD_QP_CREATE:
		rc = ntrdma_cmd_recv_qp_create(dev, &cmd->qp_create,
						 &rsp->qp_create);
		break;
	case NTRDMA_CMD_QP_DELETE:
		rc = ntrdma_cmd_recv_qp_delete(dev, &cmd->qp_delete,
						 &rsp->qp_delete);
		break;
	case NTRDMA_CMD_QP_MODIFY:
		rc = ntrdma_cmd_recv_qp_modify(dev, &cmd->qp_modify,
						 &rsp->qp_modify);
		break;
	case NTRDMA_CMD_IW_CM:
		rc = ntrdma_cmd_recv_cm(dev, cmd, rsp);
		break;
	default:
		ntrdma_err(dev, "unhandled recv cmd op %u\n", op);
		rc = -EINVAL;
		break;
	}

	ntrdma_dbg(dev, "CMD: received: op %d cb %p done rc %d\n",
			op, (void *)cmd->hdr.cb_p, rc);
	return rc;



}

static void ntrdma_cmd_recv_work(struct ntrdma_dev *dev)
{
	u32 start, pos, end, base;
	size_t off, len;
	int rc;
	const union ntrdma_cmd *cmd_recv_buf;
	union ntrdma_rsp *cmd_recv_rsp_buf;

	ntrdma_vdbg(dev, "called\n");

	ntrdma_vbell_clear(&dev->cmd_recv.vbell);

	cmd_recv_rsp_buf = ntc_local_buf_deref(&dev->cmd_recv.rsp_buf);

	/* Process commands */
	ntrdma_ring_consume(ntrdma_dev_cmd_recv_prod(dev),
			dev->cmd_recv.cons,
			dev->cmd_recv.cap,
			&start, &end, &base);

	ntrdma_vdbg(dev, "cmd start %d end %d\n", start, end);

	cmd_recv_buf =
			ntc_export_buf_const_deref(&dev->cmd_recv.buf,
					sizeof(*cmd_recv_buf) * start,
					sizeof(*cmd_recv_buf) *
					(end - start));
	/* Make it point to the start of dev->cmd_recv_buf. */
	cmd_recv_buf -= start;

	for (pos = start; pos < end; ++pos) {
		ntrdma_vdbg(dev, "cmd recv pos %d\n", pos);
		rc = ntrdma_cmd_recv(dev,
				&cmd_recv_buf[pos],
				&cmd_recv_rsp_buf[pos]);
		if (rc)
			ntrdma_err(dev,
					"ntrdma_cmd_recv failed rc %d op %d, cmd id %d\n",
					rc, cmd_recv_buf[pos].hdr.op,
					cmd_recv_buf[pos].hdr.cmd_id);
	}

	rc = 0;
	if (pos != start) {
		ntrdma_vdbg(dev, "rsp copy start %d pos %d\n",
				start, pos);

		dev->cmd_recv.cons = ntrdma_ring_update(pos, base,
				dev->cmd_recv.cap);

		/* copy the portion of the ring buf */

		TRACE("CMD: send reply for %d cmds to pos %d\n",
				(pos - start), start);

		off = start * sizeof(union ntrdma_rsp);
		len = (pos - start) * sizeof(union ntrdma_rsp);
		rc = ntc_memcpy(&dev->cmd_recv.peer_cmd_send_rsp_buf, off,
				&dev->cmd_recv.rsp_buf, off, len);
		if (unlikely(rc < 0)) {
			ntrdma_err(dev,
					"ntc_memcpy failed. rc=%d", rc);
			goto dma_err;
		}

		/* update the producer index on the peer */
		rc = ntc_imm32(&dev->cmd_recv.peer_cmd_send_rsp_buf,
				dev->cmd_recv.peer_send_cons_shift,
				dev->cmd_recv.cons);
		if (unlikely(rc < 0)) {
			ntrdma_err(dev,
					"ntc_imm32 failed. rc=%d", rc);
			goto dma_err;
		}

		/* update the vbell and signal the peer */

		rc = ntrdma_dev_vbell_peer_direct(dev,
				dev->cmd_recv.peer_cmd_send_vbell_idx);
		if (unlikely(rc < 0)) {
			ntrdma_err(dev,
					"ntrdma_dev_vbell_peer_direct: rc=%d",
					rc);
			goto dma_err;
		}

		rc = ntc_signal(dev->ntc, NTB_DEFAULT_VEC(dev->ntc));
		if (unlikely(rc < 0)) {
			ntrdma_err(dev, "ntc_signal failed. rc=%d",
					rc);
			goto dma_err;
		}

		ntrdma_vbell_trigger(&dev->cmd_recv.vbell);
	} else
		ntrdma_vbell_readd(&dev->cmd_recv.vbell);

	dma_err:
	if (unlikely(rc < 0))
		ntrdma_unrecoverable_err(dev);
}

static void ntrdma_cmd_recv_work_cb(struct work_struct *ws)
{
	struct ntrdma_dev *dev = ntrdma_cmd_recv_work_dev(ws);

	mutex_lock(&dev->cmd_recv.lock);

	if (!dev->cmd_recv.ready) {
		ntrdma_info(dev, "cmd not ready yet to recv");
		mutex_unlock(&dev->cmd_recv.lock);
		return;
	}

	ntrdma_cmd_recv_work(dev);

	mutex_unlock(&dev->cmd_recv.lock);
}
