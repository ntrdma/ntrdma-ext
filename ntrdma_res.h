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

#ifndef NTRDMA_RES_H
#define NTRDMA_RES_H

#include "ntrdma_os.h"
#include "ntrdma_obj.h"
#include "ntrdma_dev.h"

#define NTRDMA_RES_VEC_INIT_CAP		0x10

struct ntrdma_dev;
struct ntrdma_res;
struct ntrdma_rres;
struct ntrdma_vec;
struct ntrdma_kvec;

int ntrdma_dev_res_init(struct ntrdma_dev *dev);
void ntrdma_dev_res_deinit(struct ntrdma_dev *dev);

void ntrdma_dev_res_enable(struct ntrdma_dev *dev);
void ntrdma_dev_res_disable(struct ntrdma_dev *dev);

struct ntrdma_res *ntrdma_dev_res_look(struct ntrdma_dev *dev,
				       struct ntrdma_kvec *vec, int key);

void ntrdma_dev_rres_reset(struct ntrdma_dev *dev);

struct ntrdma_rres *ntrdma_dev_rres_look(struct ntrdma_dev *dev,
					 struct ntrdma_vec *vec, int key);

/* Local rdma resource */
struct ntrdma_res {
	/* The resource is an ntrdma object */
	struct ntrdma_obj		obj;

	/* The vector to which this resource is added */
	struct ntrdma_kvec		*vec;

	/* Initiate commands to create the remote resource */
	int			(*enable)(struct ntrdma_res *res);
	/* Initiate commands to delete the remote resource */
	int			(*disable)(struct ntrdma_res *res);
	/* Invalidate any remote resource information */
	void			(*reset)(struct ntrdma_res *res);

	/* The key identifies this resource */
	ntrdma_u32_t			key;

	/* Synchronize operations affecting the local resource */
	NTRDMA_DECL_MUT			(lock);
	/* Wait on commands affecting the remote resource */
	NTRDMA_DECL_MRE			(cmds_done);
};

#define ntrdma_res_dev(res) ntrdma_obj_dev(&(res)->obj)

int ntrdma_res_init(struct ntrdma_res *res,
		    struct ntrdma_dev *dev,
		    struct ntrdma_kvec *vec,
		    int (*enable)(struct ntrdma_res *res),
		    int (*disable)(struct ntrdma_res *res),
		    void (*reset)(struct ntrdma_res *res));

void ntrdma_res_deinit(struct ntrdma_res *res);

int ntrdma_res_add(struct ntrdma_res *res);
void ntrdma_res_del(struct ntrdma_res *res);

static inline void ntrdma_res_lock(struct ntrdma_res *res)
{
	ntrdma_mut_lock(&res->lock);
}

static inline void ntrdma_res_unlock(struct ntrdma_res *res)
{
	ntrdma_mut_unlock(&res->lock);
}

static inline void ntrdma_res_start_cmds(struct ntrdma_res *res)
{
	ntrdma_vdbg(ntrdma_res_dev(res), "start commands\n");
	ntrdma_mre_reset(&res->cmds_done);
}

static inline void ntrdma_res_done_cmds(struct ntrdma_res *res)
{
	ntrdma_vdbg(ntrdma_res_dev(res), "done commands\n");
	ntrdma_mre_broadcast(&res->cmds_done);
}

static inline void ntrdma_res_wait_cmds(struct ntrdma_res *res)
{
	ntrdma_vdbg(ntrdma_res_dev(res), "wait commands\n");
	ntrdma_mre_wait(&res->cmds_done);
}

static inline void ntrdma_res_get(struct ntrdma_res *res)
{
	ntrdma_obj_get(&res->obj);
}

static inline void ntrdma_res_put(struct ntrdma_res *res)
{
	ntrdma_obj_put(&res->obj);
}

static inline void ntrdma_res_repo(struct ntrdma_res *res)
{
	ntrdma_obj_repo(&res->obj);
}

/* Remote rdma resource */
struct ntrdma_rres {
	/* The resource is an ntrdma object */
	struct ntrdma_obj		obj;

	/* The vector to which this remote resource is added */
	struct ntrdma_vec		*vec;

	/* Free this remote resource on reset */
	void			(*free)(struct ntrdma_rres *rres);

	/* The key identifies this remote resource */
	ntrdma_u32_t			key;
};

#define ntrdma_rres_dev(rres) ntrdma_obj_dev(&(rres)->obj)

int ntrdma_rres_init(struct ntrdma_rres *rres,
		     struct ntrdma_dev *dev,
		     struct ntrdma_vec *vec,
		     void (*free)(struct ntrdma_rres *rres));

void ntrdma_rres_deinit(struct ntrdma_rres *rres);

int ntrdma_rres_add(struct ntrdma_rres *rres, ntrdma_u32_t key);
void ntrdma_rres_del(struct ntrdma_rres *rres);

static inline void ntrdma_rres_get(struct ntrdma_rres *rres)
{
	ntrdma_obj_get(&rres->obj);
}

static inline void ntrdma_rres_put(struct ntrdma_rres *rres)
{
	ntrdma_obj_put(&rres->obj);
}

static inline void ntrdma_rres_repo(struct ntrdma_rres *rres)
{
	ntrdma_obj_repo(&rres->obj);
}

#endif
