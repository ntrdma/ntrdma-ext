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

#ifndef NTRDMA_PD_H
#define NTRDMA_PD_H

#include "ntrdma_res.h"

/* Protection Domain */
struct ntrdma_pd {
	/* Ofed pd structure */
	struct ib_pd			ibpd;
#ifdef CONFIG_NTRDMA_DEBUGFS
	struct dentry			*debug;
#endif

	/* Ntrdma resource bookkeeping structure */
	struct ntrdma_res		res;
	/* efficient lookup of mr that belong to this pd */
	struct ntrdma_kvec		mr_vec;
};

#define ntrdma_pd_dev(__pd) ntrdma_res_dev(&(__pd)->res)
#define ntrdma_res_pd(__res) \
	NTRDMA_CONTAINER_OF(__res, struct ntrdma_pd, res)
#define ntrdma_ib_pd(__ib) \
	NTRDMA_CONTAINER_OF(__ib, struct ntrdma_pd, ibpd)

/* Remote Protection Domain */
struct ntrdma_rpd {
#ifdef CONFIG_NTRDMA_DEBUGFS
	struct dentry			*debug;
#endif

	/* Ntrdma remote resource bookkeeping structure */
	struct ntrdma_rres		rres;
	/* efficient lookup of rmr that belong to this rpd */
	struct ntrdma_vec		rmr_vec;
};

#define ntrdma_rpd_dev(__rpd) \
	ntrdma_res_dev(&(__rpd)->rres)
#define ntrdma_rres_rpd(__rres) \
	NTRDMA_CONTAINER_OF(__rres, struct ntrdma_rpd, rres)

int ntrdma_pd_init(struct ntrdma_pd *pd, struct ntrdma_dev *dev);
void ntrdma_pd_deinit(struct ntrdma_pd *pd);

static inline int ntrdma_pd_add(struct ntrdma_pd *pd)
{
	ntrdma_debugfs_pd_add(pd);
	return ntrdma_res_add(&pd->res);
}

static inline void ntrdma_pd_del(struct ntrdma_pd *pd)
{
	ntrdma_res_del(&pd->res);
	ntrdma_debugfs_pd_del(pd);
}

static inline void ntrdma_pd_get(struct ntrdma_pd *pd)
{
	ntrdma_res_get(&pd->res);
}

static inline void ntrdma_pd_put(struct ntrdma_pd *pd)
{
	ntrdma_res_put(&pd->res);
}

static inline void ntrdma_pd_repo(struct ntrdma_pd *pd)
{
	/* FIXME: missing a put somewhere */
	//ntrdma_res_repo(&pd->res);
}

int ntrdma_rpd_init(struct ntrdma_rpd *rpd, struct ntrdma_dev *dev);
void ntrdma_rpd_deinit(struct ntrdma_rpd *rpd);

static inline int ntrdma_rpd_add(struct ntrdma_rpd *rpd, ntrdma_u32_t key)
{
	ntrdma_debugfs_rpd_add(rpd);
	return ntrdma_rres_add(&rpd->rres, key);
}

static inline void ntrdma_rpd_del(struct ntrdma_rpd *rpd)
{
	ntrdma_rres_del(&rpd->rres);
	ntrdma_debugfs_rpd_del(rpd);
}

static inline void ntrdma_rpd_get(struct ntrdma_rpd *rpd)
{
	ntrdma_rres_get(&rpd->rres);
}

static inline void ntrdma_rpd_put(struct ntrdma_rpd *rpd)
{
	ntrdma_rres_put(&rpd->rres);
}

static inline void ntrdma_rpd_repo(struct ntrdma_rpd *rpd)
{
	/* FIXME: missing a put somewhere */
	//ntrdma_rres_repo(&rpd->rres);
}

struct ntrdma_pd *ntrdma_dev_pd_look(struct ntrdma_dev *dev, int key);
struct ntrdma_rpd *ntrdma_dev_rpd_look(struct ntrdma_dev *dev, int key);

#endif
