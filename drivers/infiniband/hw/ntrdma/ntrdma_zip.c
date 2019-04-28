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


struct dma_res_unmap_ctx {
	struct ntrdma_dev *dev;
	u64 dma_addr;
	u64 len;
};

void dma_res_unmap_cb(void *cb_ctx)
{
	struct dma_res_unmap_ctx *ctx = cb_ctx;

	ntc_resource_unmap(ctx->dev->ntc,
			ctx->dma_addr,
			ctx->len,
			DMA_FROM_DEVICE,
			IOAT_DEV_ACCESS);

	kfree(ctx);
}

int ntrdma_zip_rdma(struct ntrdma_dev *dev, void *req, u32 *rdma_len,
		struct ntrdma_wr_sge *dst_sg_list,
		struct ntrdma_wr_sge *src_sg_list,
		u32 dst_sg_count, u32 src_sg_count,
		bool rdma_read)
{
	struct ntrdma_mr *mr = NULL;
	struct ntrdma_rmr *rmr = NULL;
	u32 total_len = 0;
	u32 src_i = 0, dst_i = 0;
	u32 src_off = 0, dst_off = 0;
	u32 mr_i = 0, rmr_i = 0;
	u64 mr_off = 0, rmr_off = 0;
	u64 dst, src;
	size_t len;
	int rc;
	bool is_dma_mr = false;
	bool is_dma_rmr = false;
	u64 dst_len, src_len;

	for (;;) {
		/* Advance the source work request entry */
		while (src_off == src_sg_list[src_i].len) {
			if (mr) {
				/* FIXME: dma callback for put mr */
				ntrdma_mr_put(mr);
				mr = NULL;
			}

			src_off = 0;

			if (++src_i == src_sg_count) {
				if (!rdma_read) {
					/* finished with src work request */
					*rdma_len = total_len;
					rc = 0;
					goto out;
				}
				break;
			}
		}

		/* Advance the destination work request entry */
		while (dst_off == dst_sg_list[dst_i].len) {
			if (rmr) {
				/* FIXME: dma callback for put rmr */
				ntrdma_rmr_put(rmr);
				rmr = NULL;
			}

			dst_off = 0;

			if (++dst_i == dst_sg_count) {
				if (rdma_read) {
					/* finished with dst work request */
					*rdma_len = total_len;
					rc = 0;
					goto out;
				}
				break;
			}
		}

		if (src_sg_list[src_i].key == NTRDMA_RESERVED_DMA_LEKY) {
			is_dma_mr = true;
		}

		/* Get a reference to the source memory region */
		if (!mr && !is_dma_mr) {
			if (src_i == src_sg_count) {
				ntrdma_err(dev,
						"Error out of bounds src work request %d\n",
						src_i);
				rc = -EINVAL;
				goto err;
			}

			mr = ntrdma_dev_mr_look(dev, src_sg_list[src_i].key);
			if (!mr) {
				ntrdma_err(dev,
						"Error invalid mr key for source SG list %x sg idx %d/%d\n",
						src_sg_list[src_i].key, src_i, src_sg_count);
				rc = -EINVAL;
				goto err;
			}

			mr_i = 0;
			mr_off = src_sg_list[src_i].addr - mr->addr;
		}

		/* Advance the source memory region entry */
		while (!is_dma_mr && mr_off >= mr->sg_list[mr_i].len) {
			mr_off -= mr->local_dma[mr_i].len;

			if (++mr_i == mr->sg_count) {
				ntrdma_err(dev,
						"out of bounds of source memory region, sg_count %d mr_i %d\n",
						mr->sg_count, mr_i);
				rc = -EINVAL;
				goto err;
			}

			ntc_buf_sync_dev(dev->ntc,
					mr->local_dma[mr_i].addr,
					mr->local_dma[mr_i].len,
					DMA_BIDIRECTIONAL,
					IOAT_DEV_ACCESS);
		}

		if (is_dma_mr) {
			ntc_buf_sync_dev(dev->ntc,
					src_sg_list[src_i].addr,
					src_sg_list[src_i].len,
					DMA_BIDIRECTIONAL,
					IOAT_DEV_ACCESS);
		}

		if (dst_sg_list[dst_i].key == NTRDMA_RESERVED_DMA_LEKY)
			is_dma_rmr = true;

		/* Get a reference to the destination memory region */
		if (!rmr && !is_dma_rmr) {
			if (dst_i == dst_sg_count) {
				ntrdma_err(dev,
						"Error out of bounds dst work request %d\n",
						dst_i);
				rc = -EINVAL;
				goto err;
			}

			rmr = ntrdma_dev_rmr_look(dev, dst_sg_list[dst_i].key);
			if (!rmr) {
				ntrdma_err(dev,
						"Error invalid rmr key for destination %u\n",
						dst_sg_list[dst_i].key);
				rc = -EINVAL;
				goto err;
			}

			rmr_i = 0;
			rmr_off = dst_sg_list[dst_i].addr - rmr->addr;
		}

		/* Advance the destination memory region entry */
		while (!is_dma_rmr && rmr_off >= rmr->sg_list[rmr_i].len) {
			rmr_off -= rmr->sg_list[rmr_i].len;

			if (++rmr_i == rmr->sg_count) {
				ntrdma_err(dev,
						"Error out of bounds of destination memory region %d\n",
						rmr_i);
				rc = -EINVAL;
				goto err;
			}
		}

		/* Now we have resolved one source and destination */
		if (!is_dma_rmr) {
			dst = rmr->sg_list[rmr_i].addr + rmr_off;
			dst_len = min_t(size_t,
					dst_sg_list[dst_i].len - dst_off,
					rmr->sg_list[rmr_i].len - rmr_off);

			src = mr->local_dma[mr_i].addr + mr_off;
			src_len = min_t(size_t,
					src_sg_list[src_i].len - src_off,
					mr->local_dma[mr_i].len - mr_off);
			len = min_t(size_t, dst_len, src_len);
			ntc_req_memcpy(dev->ntc, req,
					dst, src, len,
					false, NULL, NULL);
		} else {
			struct dma_res_unmap_ctx *ctx;
			u64 peer_phys = ntc_peer_addr(dev->ntc,
					dst_sg_list[dst_i].addr + dst_off);

			ctx = kmalloc_node(sizeof(*ctx), GFP_ATOMIC,
					dev_to_node(&dev->ntc->dev));

			if (!ctx) {
				ntrdma_err(dev,
						"Failed map ctx for DMA MR\n");
				rc = -EINVAL;
				goto err;
			}

			dst_len = dst_sg_list[dst_i].len - dst_off;

			dst = ntc_resource_map(dev->ntc,
					peer_phys,
					dst_len,
					DMA_FROM_DEVICE,
					IOAT_DEV_ACCESS);

			if (!dst) {
				kfree(ctx);
				ntrdma_err(dev,
						"Failed resource map %#llx len %llu\n",
						peer_phys, dst_len);
				rc = -EINVAL;
				goto err;
			}

			src = src_sg_list[src_i].addr;
			src_len = src_sg_list[src_i].len;
			len = min_t(size_t, dst_len, src_len);

			ctx->dev = dev;
			ctx->dma_addr = dst;
			ctx->len = dst_len;

			ntc_req_memcpy(dev->ntc, req,
					dst, src, len,
					false, dma_res_unmap_cb, ctx);
		}

		dev_vdbg(&dev->ntc->dev, "request memcpy dst %llx src %llx len %zu\n",
								 dst, src, len);

		/* Advance the offsets and continue to the next */

		src_off += len;
		dst_off += len;
		mr_off += len;
		rmr_off += len;

		/* len fits in u32, guaranteed by min */
		if (total_len > total_len + (u32)len) {
			/* total len would overflow u32 */
			rc = -EINVAL;
			ntrdma_err(dev,
					"Error total len would overflow u32 %u\n",
					total_len);
			goto err;
		}

		total_len += len;

		is_dma_mr = false;
		is_dma_rmr = false;
	}

err:
	if (mr) {
		/* FIXME: dma callback for put mr */
		ntrdma_mr_put(mr);
	}
out:
	if (rmr) {
		/* FIXME: dma callback for put rmr */
		ntrdma_rmr_put(rmr);
	}

	return rc;
}

int ntrdma_zip_sync(struct ntrdma_dev *dev,
		    struct ntrdma_wr_sge *dst_sg_list,
		    u32 dst_sg_count)
{
	struct ntrdma_mr *mr = NULL;
	u32 sg_i = 0;
	u32 sg_off = 0;
	u32 mr_i = 0;
	u64 mr_off = 0;
	u64 dma;
	size_t len;
	int rc;
	bool is_dma_mr = false;

	for (;;) {
		/* Advance the work request entry */
		while (sg_off == dst_sg_list[sg_i].len) {
			if (mr) {
				ntrdma_mr_put(mr);
				mr = NULL;
			}

			sg_off = 0;

			if (++sg_i == dst_sg_count) {
				/* finished with sg list */
				rc = 0;
				goto out;
			}
		}

		if (dst_sg_list[sg_i].key == NTRDMA_RESERVED_DMA_LEKY)
			is_dma_mr = true;

		/* Get a reference to the memory region */
		if (!is_dma_mr && !mr) {
			mr = ntrdma_dev_mr_look(dev, dst_sg_list[sg_i].key);
			if (!mr) {
				ntrdma_err(dev, "Invalid MR key %u\n",
						dst_sg_list[sg_i].key);
				rc = -EINVAL;
				goto err;
			}

			mr_i = 0;
			mr_off = dst_sg_list[sg_i].addr - mr->addr;
		}

		/* Advance the memory region entry */
		while (!is_dma_mr && mr_off >= mr->local_dma[mr_i].len) {
			mr_off -= mr->local_dma[mr_i].len;

			if (++mr_i == mr->sg_count) {
				ntrdma_err(dev,
						"out of bounds of the memory region sg_count %d\n",
						mr->sg_count);
				rc = -EINVAL;
				goto err;
			}

			ntc_buf_sync_cpu(dev->ntc,
					 mr->local_dma[mr_i].addr,
					 mr->local_dma[mr_i].len,
					 DMA_BIDIRECTIONAL,
					 NTB_DEV_ACCESS);
		}

		/* Now we have resolved one range to sync */

		if (is_dma_mr) {
			ntc_buf_sync_cpu(dev->ntc,
					dst_sg_list[sg_i].addr,
					dst_sg_list[sg_i].len,
					DMA_BIDIRECTIONAL,
					NTB_DEV_ACCESS);
		}

		if (!is_dma_mr) {
			dma = mr->local_dma[mr_i].addr + mr_off;
			len = min_t(size_t,
					dst_sg_list[sg_i].len - sg_off,
					mr->local_dma[mr_i].len - mr_off);
		} else {
			dma = dst_sg_list[sg_i].addr;
			len = dst_sg_list[sg_i].len - sg_off;

		}

		/* Advance the offsets and continue to the next */

		sg_off += len;
		mr_off += len;

		is_dma_mr = false;
	}

err:
	if (mr)
		ntrdma_mr_put(mr);
out:
	return rc;
}

