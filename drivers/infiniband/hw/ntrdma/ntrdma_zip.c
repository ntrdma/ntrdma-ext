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
#define CREATE_TRACE_POINTS
#include "ntrdma-trace.h"

DECLARE_PER_CPU(struct ntrdma_dev_counters, dev_cnt);

static const struct ib_sge *
snd_sg_list_skip_empty(const struct ib_sge *snd_sge,
		const struct ib_sge *snd_sg_end)
{
	for (; snd_sge != snd_sg_end; snd_sge++) {
		if (snd_sge->length)
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
		len = ntrdma_wr_rcv_sge_len(rcv_sge);
		if (len)
			break;
	}

	return rcv_sge;
}

struct ntrdma_snd_cursor {
	struct ntrdma_dev *dev;
	const struct ib_sge *snd_sge;
	const struct ib_sge *snd_sg_end;
	u64 snd_rem;
	struct ntrdma_mr *mr;
	u32 mr_key;
	struct ntc_mr_buf *mr_sge;
	struct ntc_mr_buf *mr_sg_end;
	u64 next_io_off;
};

struct ntrdma_rcv_cursor {
	struct ntrdma_dev *dev;
	const struct ntrdma_wr_rcv_sge *rcv_sge;
	struct ntrdma_wr_rcv_sge rcv_sge_copy;
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
	struct ntc_mr_buf *mr_sge;
	struct ntc_mr_buf *mr_sg_end;
	u64 next_io_off;
};

static inline void
ntrdma_snd_cursor_init(struct ntrdma_snd_cursor *c,
		struct ntrdma_dev *dev,
		const struct ib_sge *snd_sg_list,
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
	if (rcv_sg_count)
		c->rcv_sge_copy = READ_ONCE(*c->rcv_sge);
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

	if (c->mr && (c->mr_key != c->snd_sge->lkey)) {
		/* FIXME: dma callback for put mr */
		ntrdma_mr_put(c->mr);
		c->mr = NULL;
	}

	c->mr_key = c->snd_sge->lkey;

	if (!c->mr && (c->mr_key != NTRDMA_RESERVED_DMA_LEKY)) {
		/* Get a reference to snd mr */
		c->mr = ntrdma_dev_mr_look(c->dev, c->mr_key);
		if (!c->mr) {
			ntrdma_err(c->dev, "Invalid snd mr key\n");
			*rc = -EINVAL;
			return false;
		}
	}

	c->snd_rem = c->snd_sge->length;
	if (c->mr) {
		c->mr_sge = c->mr->sg_list;
		c->mr_sg_end = c->mr_sge + c->mr->sg_count;
		c->next_io_off = c->snd_sge->addr - c->mr->addr;
	} else
		c->next_io_off = 0;

 next_io_off_update:
	if (c->mr) {
		for (; (c->mr_sge != c->mr_sg_end) &&
			     (c->next_io_off >= c->mr_sge->size); ++c->mr_sge)
			c->next_io_off -= c->mr_sge->size;

		if (c->mr_sge == c->mr_sg_end) {
			ntrdma_err(c->dev, "out of bounds of snd mr\n");
			*rc = -EINVAL;
			return false;
		}
	}

	return true;
}

static inline bool
ntrdma_rcv_cursor_update(struct ntrdma_rcv_cursor *c, int *rc)
{
	u64 len;
	u32 remote_key;

	if (c->rcv_rem)
		goto next_io_off_update;

	for (; c->rcv_sge != c->rcv_sg_end;) {
		len = ntrdma_wr_rcv_sge_len(&c->rcv_sge_copy);
		if (len > c->offset)
			break;
		c->offset -= len;
		c->rcv_sge++;
		if (c->rcv_sge != c->rcv_sg_end)
			c->rcv_sge_copy = READ_ONCE(*c->rcv_sge);
	}

	if (c->rcv_sge == c->rcv_sg_end) {
		ntrdma_err(c->dev, "Out of bounds rcv work request\n");
		*rc = -EINVAL;
		return false;
	}

	remote_key = ntrdma_wr_rcv_sge_remote_key(&c->rcv_sge_copy);
	if (c->rmr && (c->rmr_key != remote_key)) {
		/* FIXME: dma callback for put rmr */
		ntrdma_rmr_put(c->rmr);
		c->rmr = NULL;
	}
	c->rmr_key = remote_key;

	if (!c->rmr && (remote_key != NTRDMA_RESERVED_DMA_LEKY)) {
		/* Get a reference to rcv mr */
		c->rmr = ntrdma_dev_rmr_look(c->dev, remote_key);
		if (!c->rmr) {
			ntrdma_err(c->dev, "Invalid rcv rmr key\n");
			*rc = -EINVAL;
			return false;
		}
	}

	c->rcv_rem = ntrdma_wr_rcv_sge_len(&c->rcv_sge_copy) - c->offset;
	if (c->rmr) {
		c->rmr_sge = c->rmr->sg_list;
		c->rmr_sg_end = c->rmr_sge + c->rmr->sg_count;
		c->next_io_off = c->rcv_sge_copy.sge.addr +
			c->offset - c->rmr->addr;
	} else
		c->next_io_off = c->offset;
	c->offset = 0;

 next_io_off_update:
	if (c->rmr) {
		for (; (c->rmr_sge != c->rmr_sg_end) &&
			     (c->next_io_off >= c->rmr_sge->size); ++c->rmr_sge)
			c->next_io_off -= c->rmr_sge->size;

		if (c->rmr_sge == c->rmr_sg_end) {
			ntrdma_err(c->dev, "out of bounds of rcv rmr\n");
			*rc = -EINVAL;
			return false;
		}
	}

	return true;
}

static inline bool
ntrdma_lrcv_cursor_update(struct ntrdma_lrcv_cursor *c, int *rc)
{
	u32 remote_key;

	if (c->lrcv_rem)
		goto next_io_off_update;

	c->lrcv_sge = rcv_sg_list_skip_empty(c->lrcv_sge, c->lrcv_sg_end);
	if (c->lrcv_sge == c->lrcv_sg_end) {
		/* finished with local rcv work request */
		*rc = 0;
		return false;
	}

	remote_key = ntrdma_wr_rcv_sge_remote_key(c->lrcv_sge);
	if (c->mr && (c->mr_key != remote_key)) {
		/* FIXME: dma callback for put rmr */
		ntrdma_mr_put(c->mr);
		c->mr = NULL;
	}
	c->mr_key = remote_key;

	if (!c->mr && (remote_key != NTRDMA_RESERVED_DMA_LEKY)) {
		/* Get a reference to rcv mr */
		c->mr = ntrdma_dev_mr_look(c->dev, remote_key);
		if (!c->mr) {
			ntrdma_err(c->dev, "Invalid rcv rmr key");
			*rc = -EINVAL;
			return false;
		}
	}

	c->lrcv_rem = ntrdma_wr_rcv_sge_len(c->lrcv_sge);
	if (c->mr) {
		c->mr_sge = c->mr->sg_list;
		c->mr_sg_end = c->mr_sge + c->mr->sg_count;
		c->next_io_off = c->lrcv_sge->sge.addr - c->mr->addr;
	} else
		c->next_io_off = 0;

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
	else {
		c->rcv_sge++;
		if (c->rcv_sge != c->rcv_sg_end)
			c->rcv_sge_copy = READ_ONCE(*c->rcv_sge);
	}

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
					struct ntc_dma_chan *chan,
					u64 wrid, int dma_wait)
{
	s64 len = min_t(u64, ntrdma_snd_cursor_next_io_size(snd),
			ntrdma_rcv_cursor_next_io_size(rcv));
	struct ntc_dev *ntc;
	struct ntc_remote_buf *rcv_buf;
	struct ntc_remote_buf remote;
	struct ntc_local_buf snd_dma_buf;
	int rc;

	if (len < 0) {
		ntrdma_err(dev, "invalid len %lld", len);
		return -EINVAL;
	}

	ntc = dev->ntc;

	if (!rcv->rmr) {
		rc = ntc_remote_buf_map(&remote, ntc,
					&rcv->rcv_sge->exp_buf_desc);
		if (rc < 0)
			return rc;
		TRACE("DMA copy %#lx bytes to remote at %#lx offset %#lx\n",
			(long)len,
			(long)rcv->rcv_sge->exp_buf_desc.chan_addr.value,
			(long)rcv->next_io_off);
		rcv_buf = &remote;
	} else
		rcv_buf = rcv->rmr_sge;

	if (snd->mr) {
		rc = ntc_mr_request_memcpy_unfenced(chan,
						rcv_buf, rcv->next_io_off,
						snd->mr_sge, snd->next_io_off,
						len, dma_wait);
		trace_dma_cpy(wrid, snd->snd_sge->addr + snd->next_io_off,
				(u64)snd->mr_sge->dma_addr + snd->next_io_off,
				rcv->rcv_sge->exp_buf_desc.chan_addr.value +
					rcv->next_io_off,
				(u64)rcv_buf->dma_addr + rcv->next_io_off,
				len, rc);
	} else {
		TRACE("DMA copy %#lx bytes from %#lx offset %#lx\n",
			(long)len, (long)snd->snd_sge->addr,
			(long)snd->next_io_off);

		ntc_local_buf_map_dma(&snd_dma_buf,
				snd->snd_sge->length, snd->snd_sge->addr);

		rc = ntc_request_memcpy_fenced(chan, rcv_buf, rcv->next_io_off,
					&snd_dma_buf,
					snd->next_io_off, len, dma_wait);
		trace_dma_cpy(wrid, (u64)snd_dma_buf.ptr + snd->next_io_off,
				snd->snd_sge->addr + snd->next_io_off,
				rcv->rcv_sge->exp_buf_desc.chan_addr.value +
					rcv->next_io_off,
				(u64)rcv_buf->dma_addr + rcv->next_io_off,
				len, rc);
	}

	if (!rcv->rmr)
		ntc_remote_buf_unmap(&remote, ntc);

	if (unlikely(rc < 0)) {
		if (rc != -EAGAIN) {
			ntrdma_err(snd->dev,
					"ntc_request_memcpy (len=%lu) error %d\n",
					(long)len, -rc);
			ntrdma_unrecoverable_err(snd->dev);
		}
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
		ntc_mr_buf_sync(c->mr_sge, c->next_io_off, len);

	return len;
}

int ntrdma_zip_rdma(struct ntrdma_dev *dev, struct ntc_dma_chan *chan,
		u32 *rdma_len,
		const struct ntrdma_wr_rcv_sge *rcv_sg_list,
		const struct ib_sge *snd_sg_list,
		u32 rcv_sg_count, u32 snd_sg_count, u32 rcv_start_offset,
		u64 wrid, int  dma_wait)
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
		len = ntrdma_cursor_next_io(dev, &rcv_cursor, &snd_cursor,
				chan, wrid, dma_wait);
		if (len < 0) {
			rc = len;
			break;
		}

		if (total_len + len != total_len + (u32)len) {
			/* total len would overflow u32 */
			rc = -EINVAL;
			ntrdma_err(dev, "total len would overflow u32\n");
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

static inline s64 ntrdma_cursor_next_imm_io(struct ntrdma_dev *dev,
					struct ntrdma_rcv_cursor *rcv,
					const void *snd_data,
					u32 snd_data_size,
					struct ntc_dma_chan *chan,
					u64 wrid)
{
	s64 len = min_t(u64, snd_data_size,
			ntrdma_rcv_cursor_next_io_size(rcv));
	struct ntc_dev *ntc;
	struct ntc_remote_buf *rcv_buf;
	struct ntc_remote_buf remote;
	int rc;

	if (unlikely(len < 0)) {
		ntrdma_err(dev, "invalid len %lld", len);
		return -EINVAL;
	}

	ntc = dev->ntc;

	if (!rcv->rmr) {
		rc = ntc_remote_buf_map(&remote, ntc,
					&rcv->rcv_sge->exp_buf_desc);
		if (rc < 0) {
			ntrdma_err(dev, "failed to map buffer");
			return rc;
		}
		TRACE("DMA copy %#lx bytes to remote at %#lx offset %#lx\n",
			(long)len,
			(long)rcv->rcv_sge->exp_buf_desc.chan_addr.value,
			(long)rcv->next_io_off);
		rcv_buf = &remote;
	} else
		rcv_buf = rcv->rmr_sge;

	rc = ntc_mr_request_memcpy_unfenced_imm(chan, rcv_buf, rcv->next_io_off,
						snd_data, len, wrid);

	trace_imm_dma_cpy(wrid, *((u64 *)snd_data),
			rcv->rcv_sge->exp_buf_desc.chan_addr.value +
				rcv->next_io_off,
			(u64)rcv_buf->dma_addr + rcv->next_io_off,
			len, rc);
	if (!rcv->rmr)
		ntc_remote_buf_unmap(&remote, ntc);

	if (unlikely(rc < 0)) {
		ntrdma_err(dev, "ntc_req_imm (len=%lu) error %d\n",
			(long)len, -rc);
		ntrdma_unrecoverable_err(dev);
		return rc;
	} else
		return len;
}

int ntrdma_zip_rdma_imm(struct ntrdma_dev *dev, struct ntc_dma_chan *chan,
			const struct ntrdma_wr_rcv_sge *rcv_sg_list,
			const void *snd_data,
			u32 rcv_sg_count, u32 snd_data_size,
			u32 rcv_start_offset, u64 wrid)
{
	u32 total_len = 0;
	s64 len;
	int rc = 0;
	struct ntrdma_rcv_cursor rcv_cursor;

	if (unlikely(!snd_data_size))
		return 0;

	ntrdma_rcv_cursor_init(&rcv_cursor, dev, rcv_sg_list, rcv_sg_count,
			rcv_start_offset);

	if (!ntrdma_rcv_cursor_update(&rcv_cursor, &rc))
		goto out;

	do {
		len = ntrdma_cursor_next_imm_io(dev, &rcv_cursor,
						snd_data, snd_data_size, chan,
						wrid);
		if (len < 0) {
			rc = len;
			break;
		}

		snd_data += len;
		snd_data_size -= len;
		total_len += len;

	} while ((snd_data_size > 0) &&
		ntrdma_rcv_cursor_forward(&rcv_cursor, len, &rc));

out:
	ntrdma_rcv_cursor_deinit(&rcv_cursor);

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

int ntrdma_zip_memcpy(struct ntrdma_dev *dev, u32 target_key, u64 target_addr,
		void *src, u64 size)
{
	struct ntrdma_mr *mr;
	struct ntc_mr_buf *mr_sge;
	struct ntc_mr_buf *mr_sg_end;
	u64 next_io_off;
	u64 next_io_size;

	if (!size)
		return 0;

	mr = ntrdma_dev_mr_look(dev, target_key);
	if (!mr) {
		ntrdma_err(dev, "Invalid mr key %d", target_key);
		return -EINVAL;
	}

	mr_sge = mr->sg_list;
	mr_sg_end = mr_sge + mr->sg_count;
	next_io_off = target_addr - mr->addr;
	for (; (mr_sge != mr_sg_end) && (next_io_off >= mr_sge->size); ++mr_sge)
		next_io_off -= mr_sge->size;

	for (; (mr_sge != mr_sg_end) && size; ++mr_sge) {
		next_io_size = min_t(u64, size, mr_sge->size - next_io_off);
		memcpy(phys_to_virt(mr_sge->dma_addr), src, next_io_size);
		src += next_io_size;
		size -= next_io_size;
		next_io_off = 0;
	}

	/* FIXME: dma callback for put mr */
	ntrdma_mr_put(mr);

	if (size) {
		ntrdma_err(dev, "out of bounds of local rcv mr");
		return  -EINVAL;
	}

	return 0;
}
