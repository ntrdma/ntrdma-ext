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

#include "ntrdma_dev.h"
#include "ntrdma_wr.h"
#include "ntrdma_mr.h"
#include "ntrdma_zip.h"

DECLARE_PER_CPU(struct ntrdma_dev_counters, dev_cnt);

static const struct ntrdma_wr_snd_sge *
snd_sg_list_skip_empty(const struct ntrdma_wr_snd_sge *snd_sge,
		const struct ntrdma_wr_snd_sge *snd_sg_end)
{
	u64 len;

	for (; snd_sge != snd_sg_end; snd_sge++) {
		if (snd_sge->key == NTRDMA_RESERVED_DMA_LEKY)
			len = snd_sge->snd_dma_buf.size;
		else
			len = snd_sge->len;
		if (len)
			break;
	}

	return snd_sge;
}

static const struct ntrdma_wr_rcv_sge *
rcv_sg_list_skip_empty(const struct ntrdma_wr_rcv_sge *rcv_sge,
		const struct ntrdma_wr_rcv_sge *rcv_sg_end)
{
	u64 len;

	for (; rcv_sge != rcv_sg_end; rcv_sge++) {
		if (rcv_sge->key == NTRDMA_RESERVED_DMA_LEKY)
			len = rcv_sge->desc.size;
		else
			len = rcv_sge->len;
		if (len)
			break;
	}

	return rcv_sge;
}

static const struct ntrdma_wr_rcv_sge *
rcv_sg_list_skip(const struct ntrdma_wr_rcv_sge *rcv_sge,
		const struct ntrdma_wr_rcv_sge *rcv_sg_end, u32 *offset_in_out)
{
	u64 len;
	u32 offset = *offset_in_out;

	for (; rcv_sge != rcv_sg_end; rcv_sge++) {
		if (rcv_sge->key == NTRDMA_RESERVED_DMA_LEKY)
			len = rcv_sge->desc.size;
		else
			len = rcv_sge->len;
		if (len > offset)
			break;
		offset -= len;
	}

	if (rcv_sge != rcv_sg_end)
		*offset_in_out = offset;

	return rcv_sge;
}

struct ntrdma_snd_cursor {
	struct ntrdma_dev *dev;
	const struct ntrdma_wr_snd_sge *snd_sge;
	const struct ntrdma_wr_snd_sge *snd_sg_end;
	u64 snd_rem;
	struct ntrdma_mr *mr;
	u32 mr_key;
	struct ntc_bidir_buf *mr_sge;
	struct ntc_bidir_buf *mr_sg_end;
	u64 next_io_off;
};

struct ntrdma_rcv_cursor {
	struct ntrdma_dev *dev;
	const struct ntrdma_wr_rcv_sge *rcv_sge;
	const struct ntrdma_wr_rcv_sge *rcv_sg_end;
	u64 rcv_rem;
	u32 rmr_key;
	struct ntrdma_rmr *rmr;
	struct ntc_remote_buf *rmr_sge;
	struct ntc_remote_buf *rmr_sg_end;
	u64 next_io_off;
	u32 offset;
};

struct ntrdma_lrcv_cursor {
	struct ntrdma_dev *dev;
	const struct ntrdma_wr_rcv_sge *lrcv_sge;
	const struct ntrdma_wr_rcv_sge *lrcv_sg_end;
	u64 lrcv_rem;
	u32 mr_key;
	struct ntrdma_mr *mr;
	struct ntc_bidir_buf *mr_sge;
	struct ntc_bidir_buf *mr_sg_end;
	u64 next_io_off;
};

static inline void
ntrdma_snd_cursor_init(struct ntrdma_snd_cursor *c,
		struct ntrdma_dev *dev,
		const struct ntrdma_wr_snd_sge *snd_sg_list,
		u32 snd_sg_count)
{
	c->dev = dev;
	c->snd_sge = snd_sg_list;
	c->snd_sg_end = snd_sg_list + snd_sg_count;
	c->snd_rem = 0;
	c->mr = NULL;
	c->mr_key = NTRDMA_RESERVED_DMA_LEKY;
	c->mr_sge = NULL;
	c->mr_sg_end = NULL;
	c->next_io_off = 0;
}

static inline void
ntrdma_rcv_cursor_init(struct ntrdma_rcv_cursor *c, struct ntrdma_dev *dev,
		const struct ntrdma_wr_rcv_sge *rcv_sg_list,
		u32 rcv_sg_count, u32 offset)
{
	c->dev = dev;
	c->rcv_sge = rcv_sg_list;
	c->rcv_sg_end = rcv_sg_list + rcv_sg_count;
	c->rcv_rem = 0;
	c->rmr = NULL;
	c->rmr_key = NTRDMA_RESERVED_DMA_LEKY;
	c->rmr_sge = NULL;
	c->rmr_sg_end = NULL;
	c->next_io_off = 0;
	c->offset = offset;
}

static inline void
ntrdma_lrcv_cursor_init(struct ntrdma_lrcv_cursor *c, struct ntrdma_dev *dev,
			const struct ntrdma_wr_rcv_sge *lrcv_sg_list,
			u32 lrcv_sg_count)
{
	c->dev = dev;
	c->lrcv_sge = lrcv_sg_list;
	c->lrcv_sg_end = lrcv_sg_list + lrcv_sg_count;
	c->lrcv_rem = 0;
	c->mr = NULL;
	c->mr_key = NTRDMA_RESERVED_DMA_LEKY;
	c->mr_sge = NULL;
	c->mr_sg_end = NULL;
	c->next_io_off = 0;
}

static inline void ntrdma_snd_cursor_deinit(struct ntrdma_snd_cursor *c)
{
	if (c->mr)
		/* FIXME: dma callback for put mr */
		ntrdma_mr_put(c->mr);
}

static inline void ntrdma_rcv_cursor_deinit(struct ntrdma_rcv_cursor *c)
{
	if (c->rmr)
		/* FIXME: dma callback for put rmr */
		ntrdma_rmr_put(c->rmr);
}

static inline void ntrdma_lrcv_cursor_deinit(struct ntrdma_lrcv_cursor *c)
{
	if (c->mr)
		/* FIXME: dma callback for put mr */
		ntrdma_mr_put(c->mr);
}

static inline bool
ntrdma_snd_cursor_update(struct ntrdma_snd_cursor *c, int *rc)
{
	if (c->snd_rem)
		goto next_io_off_update;

	c->snd_sge = snd_sg_list_skip_empty(c->snd_sge, c->snd_sg_end);
	if (c->snd_sge == c->snd_sg_end) {
		/* finished with snd work request */
		*rc = 0;
		return false;
	}

	if (c->mr && (c->mr_key != c->snd_sge->key)) {
		/* FIXME: dma callback for put mr */
		ntrdma_mr_put(c->mr);
		c->mr = NULL;
	}

	c->mr_key = c->snd_sge->key;

	if (!c->mr && (c->mr_key != NTRDMA_RESERVED_DMA_LEKY)) {
		/* Get a reference to snd mr */
		c->mr = ntrdma_dev_mr_look(c->dev, c->mr_key);
		if (!c->mr) {
			ntrdma_err(c->dev, "Invalid snd mr key");
			*rc = -EINVAL;
			return false;
		}
	}

	if (c->mr) {
		c->snd_rem = c->snd_sge->len;
		c->mr_sge = c->mr->sg_list;
		c->mr_sg_end = c->mr_sge + c->mr->sg_count;
		c->next_io_off = c->snd_sge->addr - c->mr->addr;
	} else {
		c->snd_rem = c->snd_sge->snd_dma_buf.size;
		c->next_io_off = 0;
	}

 next_io_off_update:
	if (c->mr) {
		for (; (c->mr_sge != c->mr_sg_end) &&
			     (c->next_io_off >= c->mr_sge->size); ++c->mr_sge)
			c->next_io_off -= c->mr_sge->size;

		if (c->mr_sge == c->mr_sg_end) {
			ntrdma_err(c->dev, "out of bounds of snd mr");
			*rc = -EINVAL;
			return false;
		}
	}

	return true;
}

static inline bool
ntrdma_rcv_cursor_update(struct ntrdma_rcv_cursor *c, int *rc)
{
	if (c->rcv_rem)
		goto next_io_off_update;

	c->rcv_sge = rcv_sg_list_skip(c->rcv_sge, c->rcv_sg_end, &c->offset);
	if (c->rcv_sge == c->rcv_sg_end) {
		ntrdma_err(c->dev, "Out of bounds rcv work request");
		*rc = -EINVAL;
		return false;
	}

	if (c->rmr && (c->rmr_key != c->rcv_sge->key)) {
		/* FIXME: dma callback for put rmr */
		ntrdma_rmr_put(c->rmr);
		c->rmr = NULL;
	}

	c->rmr_key = c->rcv_sge->key;

	if (!c->rmr && (c->rmr_key != NTRDMA_RESERVED_DMA_LEKY)) {
		/* Get a reference to rcv mr */
		c->rmr = ntrdma_dev_rmr_look(c->dev, c->rmr_key);
		if (!c->rmr) {
			ntrdma_err(c->dev, "Invalid rcv rmr key");
			*rc = -EINVAL;
			return false;
		}
	}

	if (c->rmr) {
		c->rcv_rem = c->rcv_sge->len - c->offset;
		c->rmr_sge = c->rmr->sg_list;
		c->rmr_sg_end = c->rmr_sge + c->rmr->sg_count;
		c->next_io_off = c->rcv_sge->addr + c->offset - c->rmr->addr;
	} else {
		c->rcv_rem = c->rcv_sge->desc.size - c->offset;
		c->next_io_off = c->offset;
	}
	c->offset = 0;

 next_io_off_update:
	if (c->rmr) {
		for (; (c->rmr_sge != c->rmr_sg_end) &&
			     (c->next_io_off >= c->rmr_sge->size); ++c->rmr_sge)
			c->next_io_off -= c->rmr_sge->size;

		if (c->rmr_sge == c->rmr_sg_end) {
			ntrdma_err(c->dev, "out of bounds of rcv rmr");
			*rc = -EINVAL;
			return false;
		}
	}

	return true;
}

static inline bool
ntrdma_lrcv_cursor_update(struct ntrdma_lrcv_cursor *c, int *rc)
{
	if (c->lrcv_rem)
		goto next_io_off_update;

	c->lrcv_sge = rcv_sg_list_skip_empty(c->lrcv_sge, c->lrcv_sg_end);
	if (c->lrcv_sge == c->lrcv_sg_end) {
		/* finished with local rcv work request */
		*rc = 0;
		return false;
	}

	if (c->mr && (c->mr_key != c->lrcv_sge->key)) {
		/* FIXME: dma callback for put rmr */
		ntrdma_mr_put(c->mr);
		c->mr = NULL;
	}

	c->mr_key = c->lrcv_sge->key;

	if (!c->mr && (c->mr_key != NTRDMA_RESERVED_DMA_LEKY)) {
		/* Get a reference to rcv mr */
		c->mr = ntrdma_dev_mr_look(c->dev, c->mr_key);
		if (!c->mr) {
			ntrdma_err(c->dev, "Invalid rcv rmr key");
			*rc = -EINVAL;
			return false;
		}
	}

	if (c->mr) {
		c->lrcv_rem = c->lrcv_sge->len;
		c->mr_sge = c->mr->sg_list;
		c->mr_sg_end = c->mr_sge + c->mr->sg_count;
		c->next_io_off = c->lrcv_sge->addr - c->mr->addr;
	} else {
		c->lrcv_rem = c->lrcv_sge->desc.size;
		c->next_io_off = 0;
	}

 next_io_off_update:
	if (c->mr) {
		for (; (c->mr_sge != c->mr_sg_end) &&
			     (c->next_io_off >= c->mr_sge->size); ++c->mr_sge)
			c->next_io_off -= c->mr_sge->size;

		if (c->mr_sge == c->mr_sg_end) {
			ntrdma_err(c->dev, "out of bounds of local rcv mr");
			*rc = -EINVAL;
			return false;
		}
	}

	return true;
}

static inline bool
ntrdma_snd_cursor_forward(struct ntrdma_snd_cursor *c, u64 len, int *rc)
{
	c->snd_rem -= len;

	if (c->snd_rem)
		c->next_io_off += len;
	else
		c->snd_sge++;

	return ntrdma_snd_cursor_update(c, rc);
}

static inline bool
ntrdma_rcv_cursor_forward(struct ntrdma_rcv_cursor *c, u64 len, int *rc)
{
	c->rcv_rem -= len;

	if (c->rcv_rem)
		c->next_io_off += len;
	else
		c->rcv_sge++;

	return ntrdma_rcv_cursor_update(c, rc);
}

static inline bool
ntrdma_lrcv_cursor_forward(struct ntrdma_lrcv_cursor *c, u64 len, int *rc)
{
	c->lrcv_rem -= len;

	if (c->lrcv_rem)
		c->next_io_off += len;
	else
		c->lrcv_sge++;

	return ntrdma_lrcv_cursor_update(c, rc);
}

static inline u64 ntrdma_snd_cursor_next_io_size(struct ntrdma_snd_cursor *c)
{
	if (c->mr)
		return min_t(u64, c->snd_rem, c->mr_sge->size - c->next_io_off);
	else
		return c->snd_rem;
}

static inline u64 ntrdma_rcv_cursor_next_io_size(struct ntrdma_rcv_cursor *c)
{
	if (c->rmr)
		return min_t(u64, c->rcv_rem,
			c->rmr_sge->size - c->next_io_off);
	else
		return c->rcv_rem;
}

static inline u64 ntrdma_lrcv_cursor_next_io_size(struct ntrdma_lrcv_cursor *c)
{
	if (c->mr)
		return min_t(u64, c->lrcv_rem,
			c->mr_sge->size - c->next_io_off);
	else
		return c->lrcv_rem;
}

static inline s64 ntrdma_cursor_next_io(struct ntrdma_dev *dev,
					struct ntrdma_rcv_cursor *rcv,
					struct ntrdma_snd_cursor *snd,
					struct dma_chan *req)
{
	s64 len = min_t(u64, ntrdma_snd_cursor_next_io_size(snd),
			ntrdma_rcv_cursor_next_io_size(rcv));
	struct ntc_dev *ntc;
	struct ntc_remote_buf *rcv_buf;
	struct ntc_remote_buf remote;
	int rc;

	if (len < 0)
		return -EINVAL;

	ntc = dev->ntc;

	if (!rcv->rmr) {
		rc = ntc_remote_buf_map(&remote, ntc, &rcv->rcv_sge->desc);
		if (rc < 0)
			return rc;
		TRACE("DMA copy %#lx bytes to remote at %#lx offset %#lx",
			(long)len, (long)rcv->rcv_sge->desc.chan_addr.value,
			(long)rcv->next_io_off);
		rcv_buf = &remote;
	} else
		rcv_buf = rcv->rmr_sge;

	if (snd->mr)
		rc = ntc_bidir_request_memcpy_unfenced(req,
						rcv_buf, rcv->next_io_off,
						snd->mr_sge, snd->next_io_off,
						len);
	else {
		TRACE("DMA copy %#lx bytes from %#lx offset %#lx\n",
			(long)len, (long)snd->snd_sge->snd_dma_buf.dma_addr,
			(long)snd->next_io_off);

		rc = ntc_request_memcpy_fenced(req, rcv_buf, rcv->next_io_off,
					&snd->snd_sge->snd_dma_buf,
					snd->next_io_off, len);
	}

	if (!rcv->rmr)
		ntc_remote_buf_unmap(&remote, ntc);

	if (rc < 0) {
		ntrdma_err(snd->dev,
			"ntc_request_memcpy (len=%lu) error %d",
			(long)len, -rc);
		return rc;
	} else
		return len;
}

static inline s64 ntrdma_lrcv_cursor_next_io_deref(struct ntrdma_lrcv_cursor *c)
{
	s64 len = ntrdma_lrcv_cursor_next_io_size(c);

	if (len < 0)
		return -EINVAL;

	if (c->mr)
		ntc_bidir_buf_deref(c->mr_sge, c->next_io_off, len);

	return len;
}

int ntrdma_zip_rdma(struct ntrdma_dev *dev, struct dma_chan *req, u32 *rdma_len,
		const struct ntrdma_wr_rcv_sge *rcv_sg_list,
		const struct ntrdma_wr_snd_sge *snd_sg_list,
		u32 rcv_sg_count, u32 snd_sg_count, u32 rcv_start_offset)
{
	u32 total_len = 0;
	s64 len;
	int rc = 0;
	struct ntrdma_snd_cursor snd_cursor;
	struct ntrdma_rcv_cursor rcv_cursor;

	ntrdma_snd_cursor_init(&snd_cursor, dev, snd_sg_list, snd_sg_count);
	ntrdma_rcv_cursor_init(&rcv_cursor, dev, rcv_sg_list, rcv_sg_count,
		rcv_start_offset);

	if (!ntrdma_snd_cursor_update(&snd_cursor, &rc) ||
		!ntrdma_rcv_cursor_update(&rcv_cursor, &rc))
		goto out;

	do {
		len = ntrdma_cursor_next_io(dev, &rcv_cursor, &snd_cursor, req);
		if (len < 0) {
			rc = len;
			break;
		}

		if (total_len + len != total_len + (u32)len) {
			/* total len would overflow u32 */
			rc = -EINVAL;
			ntrdma_err(dev, "total len would overflow u32");
			break;
		}
		total_len += len;

	} while (ntrdma_snd_cursor_forward(&snd_cursor, len, &rc) &&
		ntrdma_rcv_cursor_forward(&rcv_cursor, len, &rc));

out:
	ntrdma_snd_cursor_deinit(&snd_cursor);
	ntrdma_rcv_cursor_deinit(&rcv_cursor);

	if (!rc)
		*rdma_len = total_len;

	this_cpu_add(dev_cnt.qp_send_work_bytes, total_len);

	return rc;
}

int ntrdma_zip_sync(struct ntrdma_dev *dev,
		struct ntrdma_wr_rcv_sge *lrcv_sg_list, u32 lrcv_sg_count)
{
	struct ntrdma_lrcv_cursor lrcv_cursor;
	s64 len;
	int rc;

	ntrdma_lrcv_cursor_init(&lrcv_cursor, dev, lrcv_sg_list, lrcv_sg_count);
	if (!ntrdma_lrcv_cursor_update(&lrcv_cursor, &rc))
		goto out;

	do {
		len = ntrdma_lrcv_cursor_next_io_deref(&lrcv_cursor);
		if (len < 0) {
			rc = len;
			break;
		}
	} while (ntrdma_lrcv_cursor_forward(&lrcv_cursor, len, &rc));

out:
	ntrdma_lrcv_cursor_deinit(&lrcv_cursor);

	return rc;
}

