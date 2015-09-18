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

#ifndef NTRDMA_MR_H
#define NTRDMA_MR_H

#include "ntrdma_os.h"
#include "ntrdma_ib.h"

#include "ntrdma_res.h"
#include "ntrdma_sg.h"

struct ntrdma_pd;
struct ntrdma_rpd;

struct ntrdma_mr_data {
};

/* Memory Region */
struct ntrdma_mr {
	/* Ofed mr structure */
	struct ib_mr			ibmr;
#ifdef CONFIG_NTRDMA_DEBUGFS
	struct dentry			*debug;
#endif

	/* Ntrdma resource bookkeeping structure */
	struct ntrdma_res		res;
	/* Protection domain to which this memory region belongs */
	struct ntrdma_pd		*pd;

	void				*umem;

	ntrdma_u64_t			addr;
	ntrdma_u64_t			len;

	ntrdma_u32_t			access;

	ntrdma_u32_t			sg_count;
	struct ntrdma_mr_sge		sg_list[];
};

#define ntrdma_mr_dev(__mr) ntrdma_res_dev(&(__mr)->res)
#define ntrdma_res_mr(__res) \
	NTRDMA_CONTAINER_OF(__res, struct ntrdma_mr, res)
#define ntrdma_ib_mr(__ib) \
	NTRDMA_CONTAINER_OF(__ib, struct ntrdma_mr, ibmr)

/* Remote Memory Region */
struct ntrdma_rmr {
#ifdef CONFIG_NTRDMA_DEBUGFS
	struct dentry			*debug;
#endif

	/* Ntrdma remote resource bookkeeping structure */
	struct ntrdma_rres		rres;
	/* Remote protection domain to which this remote memory region belongs */
	struct ntrdma_rpd		*rpd;

	ntrdma_u64_t			addr;
	ntrdma_u64_t			len;

	ntrdma_u32_t			access;

	ntrdma_u32_t			sg_count;
	struct ntrdma_mr_sge		sg_list[];
};

#define ntrdma_rmr_dev(__rmr) ntrdma_rres_dev(&(__rmr)->rres)
#define ntrdma_rres_rmr(__rres) \
	NTRDMA_CONTAINER_OF(__rres, struct ntrdma_rmr, rres)

int ntrdma_mr_init(struct ntrdma_mr *mr, struct ntrdma_pd *pd,
		   void *umem, ntrdma_u64_t addr, ntrdma_u64_t len,
		   ntrdma_u32_t access, ntrdma_u32_t sg_count);

void ntrdma_mr_deinit(struct ntrdma_mr *mr);

static inline int ntrdma_mr_add(struct ntrdma_mr *mr)
{
	ntrdma_debugfs_mr_add(mr);
	return ntrdma_res_add(&mr->res);
}

static inline void ntrdma_mr_del(struct ntrdma_mr *mr)
{
	ntrdma_res_del(&mr->res);
	ntrdma_debugfs_mr_del(mr);
}

static inline void ntrdma_mr_get(struct ntrdma_mr *mr)
{
	ntrdma_res_get(&mr->res);
}

static inline void ntrdma_mr_put(struct ntrdma_mr *mr)
{
	ntrdma_res_put(&mr->res);
}

static inline void ntrdma_mr_repo(struct ntrdma_mr *mr)
{
	/* FIXME: missing a put */
	//ntrdma_res_repo(&mr->res);
}

int ntrdma_rmr_init(struct ntrdma_rmr *rmr, struct ntrdma_rpd *rpd,
		    ntrdma_u64_t addr, ntrdma_u64_t len,
		    ntrdma_u32_t access, ntrdma_u32_t sg_count);

void ntrdma_rmr_deinit(struct ntrdma_rmr *rmr);

static inline int ntrdma_rmr_add(struct ntrdma_rmr *rmr, ntrdma_u32_t key)
{
	ntrdma_debugfs_rmr_add(rmr);
	return ntrdma_rres_add(&rmr->rres, key);
}

static inline void ntrdma_rmr_del(struct ntrdma_rmr *rmr)
{
	ntrdma_rres_del(&rmr->rres);
	ntrdma_debugfs_rmr_del(rmr);
}

static inline void ntrdma_rmr_get(struct ntrdma_rmr *rmr)
{
	ntrdma_rres_get(&rmr->rres);
}

static inline void ntrdma_rmr_put(struct ntrdma_rmr *rmr)
{
	ntrdma_rres_put(&rmr->rres);
}

static inline void ntrdma_rmr_repo(struct ntrdma_rmr *rmr)
{
	/* FIXME: missing a put */
	//ntrdma_rres_repo(&rmr->rres);
}

struct ntrdma_mr *ntrdma_dev_mr_look(struct ntrdma_pd *pd, int key);
struct ntrdma_rmr *ntrdma_dev_rmr_look(struct ntrdma_rpd *rpd, int key);

#endif
