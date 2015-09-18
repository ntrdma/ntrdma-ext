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

#define VERBOSE_DEBUG

#include "ntrdma_dev.h"
#include "ntrdma_util.h"
#include "ntrdma_res.h"
#include "ntrdma_cmd.h"

int ntrdma_dev_res_init(struct ntrdma_dev *dev)
{
	int err;

	/* rdma resource synchronization state */

	dev->res_enable = 0;

	ntrdma_mut_create(&dev->res_lock, "ntrdma_res_lock");
	ntrdma_mut_create(&dev->rres_lock, "ntrdma_rres_lock");

	/* rdma local resources */

	ntrdma_list_init(&dev->cq_list);
	ntrdma_list_init(&dev->res_list);

	err = ntrdma_kvec_init(&dev->pd_vec, NTRDMA_RES_VEC_INIT_CAP, dev->node);
	if (err) {
		ntrdma_dbg(dev, "pd vec failed\n");
		goto err_pd;
	}

	err = ntrdma_kvec_init(&dev->qp_vec, NTRDMA_RES_VEC_INIT_CAP, dev->node);
	if (err) {
		ntrdma_dbg(dev, "qp vec failed\n");
		goto err_qp;
	}

	/* rdma remote resources */

	ntrdma_list_init(&dev->rres_list);

	err = ntrdma_vec_init(&dev->rpd_vec, NTRDMA_RES_VEC_INIT_CAP, dev->node);
	if (err) {
		ntrdma_dbg(dev, "rpd vec failed\n");
		goto err_rpd;
	}

	err = ntrdma_vec_init(&dev->rqp_vec, NTRDMA_RES_VEC_INIT_CAP, dev->node);
	if (err) {
		ntrdma_dbg(dev, "rqp vec failed\n");
		goto err_rqp;
	}

	return 0;

	//ntrdma_vec_deinit(&dev->rqp_vec);
err_rqp:
	ntrdma_vec_deinit(&dev->rpd_vec);
err_rpd:
	ntrdma_kvec_deinit(&dev->qp_vec);
err_qp:
	ntrdma_kvec_deinit(&dev->pd_vec);
err_pd:
	ntrdma_mut_destroy(&dev->rres_lock);
	ntrdma_mut_destroy(&dev->res_lock);
	return err;
}

void ntrdma_dev_res_deinit(struct ntrdma_dev *dev)
{
	ntrdma_vec_deinit(&dev->rqp_vec);
	ntrdma_vec_deinit(&dev->rpd_vec);
	ntrdma_kvec_deinit(&dev->qp_vec);
	ntrdma_kvec_deinit(&dev->pd_vec);
	ntrdma_mut_destroy(&dev->rres_lock);
	ntrdma_mut_destroy(&dev->res_lock);
}

void ntrdma_dev_res_enable(struct ntrdma_dev *dev)
{
	struct ntrdma_res *res;
	int rc;

	ntrdma_mut_lock(&dev->res_lock);
	{
		dev->res_enable = 1;

		ntrdma_list_for_each_entry(res, &dev->res_list,
					   struct ntrdma_res, obj.dev_entry) {
			rc = res->enable(res);
			/* FIXME: unchecked */
		}

		ntrdma_dev_cmd_finish(dev);
	}
	ntrdma_mut_unlock(&dev->res_lock);

	return;
}

void ntrdma_dev_res_disable(struct ntrdma_dev *dev)
{
	struct ntrdma_res *res;

	ntrdma_mut_lock(&dev->res_lock);
	{
		dev->res_enable = 0;

		ntrdma_list_for_each_entry_reverse(res, &dev->res_list,
						   struct ntrdma_res, obj.dev_entry) {
			res->reset(res);
		}
	}
	ntrdma_mut_unlock(&dev->res_lock);
}

struct ntrdma_res *ntrdma_dev_res_look(struct ntrdma_dev *dev,
				       struct ntrdma_kvec *vec, int key)
{
	struct ntrdma_res *res;

	ntrdma_kvec_lock(vec);
	{
		res = ntrdma_kvec_look(vec, key);
		if (res)
			ntrdma_res_get(res);
	}
	ntrdma_kvec_unlock(vec);

	return res;
}

void ntrdma_dev_rres_reset(struct ntrdma_dev *dev)
{
	struct ntrdma_rres *rres, *rres_next;

	ntrdma_mut_lock(&dev->rres_lock);
	{
		ntrdma_list_for_each_entry_safe_reverse(rres, rres_next, &dev->rres_list,
							struct ntrdma_rres, obj.dev_entry) {

			ntrdma_vec_lock(rres->vec);
			{
				ntrdma_vec_set(rres->vec, rres->key, NULL);
			}
			ntrdma_vec_unlock(rres->vec);

			ntrdma_rres_put(rres);
			ntrdma_rres_repo(rres);
			rres->free(rres);
		}
		ntrdma_list_init(&dev->rres_list);
	}
	ntrdma_mut_unlock(&dev->rres_lock);
}

struct ntrdma_rres *ntrdma_dev_rres_look(struct ntrdma_dev *dev,
					 struct ntrdma_vec *vec, int key)
{
	struct ntrdma_rres *rres;

	ntrdma_vec_lock(vec);
	{
		rres = ntrdma_vec_look(vec, key);
		if (rres)
			ntrdma_rres_get(rres);
	}
	ntrdma_vec_unlock(vec);

	return rres;
}

int ntrdma_res_init(struct ntrdma_res *res,
		    struct ntrdma_dev *dev, struct ntrdma_kvec *vec,
		    int (*enable)(struct ntrdma_res *res),
		    int (*disable)(struct ntrdma_res *res),
		    void (*reset)(struct ntrdma_res *res))
{
	int rc;

	rc = ntrdma_obj_init(&res->obj, dev);
	if (rc)
		goto err_obj;

	res->vec = vec;

	res->enable = enable;
	res->disable = disable;
	res->reset = reset;

	ntrdma_mut_create(&res->lock, "res_lock");
	ntrdma_mre_create(&res->cmds_done, "res_cmds_done");

	ntrdma_mut_lock(&dev->res_lock);
	{
		res->key = ntrdma_kvec_reserve_key(res->vec, dev->node);
		if (res->key < 0) {
			rc = -ENOMEM;
			goto err_key;
		}
	}
	ntrdma_mut_unlock(&dev->res_lock);

	return 0;

err_key:
	ntrdma_mut_unlock(&dev->res_lock);
	ntrdma_mre_destroy(&res->cmds_done);
	ntrdma_mut_destroy(&res->lock);
	ntrdma_obj_deinit(&res->obj);
err_obj:
	return rc;
}

void ntrdma_res_deinit(struct ntrdma_res *res)
{
	struct ntrdma_dev *dev = ntrdma_res_dev(res);

	ntrdma_mut_lock(&dev->res_lock);
	{
		ntrdma_kvec_dispose_key(res->vec, res->key);
	}
	ntrdma_mut_unlock(&dev->res_lock);

	ntrdma_mre_destroy(&res->cmds_done);
	ntrdma_mut_destroy(&res->lock);
	ntrdma_obj_deinit(&res->obj);
}

int ntrdma_res_add(struct ntrdma_res *res)
{
	struct ntrdma_dev *dev = ntrdma_res_dev(res);
	int rc;

	ntrdma_res_get(res);

	ntrdma_vdbg(dev, "resource obtained\n");

	ntrdma_res_lock(res);
	ntrdma_mut_lock(&dev->res_lock);
	{
		ntrdma_kvec_lock(res->vec);
		{
			ntrdma_kvec_set(res->vec, res->key, res);
		}
		ntrdma_kvec_unlock(res->vec);

		ntrdma_list_add_tail(&dev->res_list, &res->obj.dev_entry);

		ntrdma_vdbg(dev, "resource added\n");

		if (dev->res_enable) {
			ntrdma_vdbg(dev, "resource commands initiated\n");
			rc = res->enable(res);
			/* FIXME: unchecked */
			ntrdma_dev_cmd_submit(dev);
		} else {
			ntrdma_vdbg(dev, "no commands\n");
			ntrdma_res_done_cmds(res);
		}
	}
	ntrdma_mut_unlock(&dev->res_lock);

	ntrdma_vdbg(dev, "wait for commands\n");
	ntrdma_res_wait_cmds(res);
	ntrdma_vdbg(dev, "done waiting\n");
	ntrdma_res_unlock(res);

	return 0;
}

void ntrdma_res_del(struct ntrdma_res *res)
{
	struct ntrdma_dev *dev = ntrdma_res_dev(res);
	int rc;

	ntrdma_res_lock(res);
	ntrdma_mut_lock(&dev->res_lock);
	{
		if (dev->res_enable) {
			rc = res->disable(res);
			/* FIXME: unchecked */
			ntrdma_dev_cmd_submit(dev);
		} else {
			ntrdma_res_done_cmds(res);
		}

		ntrdma_list_del(&res->obj.dev_entry);

		ntrdma_kvec_lock(res->vec);
		{
			ntrdma_kvec_set(res->vec, res->key, NULL);
		}
		ntrdma_kvec_unlock(res->vec);
	}
	ntrdma_mut_unlock(&dev->res_lock);

	ntrdma_res_wait_cmds(res);
	ntrdma_res_unlock(res);

	ntrdma_res_put(res);
}

int ntrdma_rres_init(struct ntrdma_rres *rres,
		     struct ntrdma_dev *dev, struct ntrdma_vec *vec,
		     void (*free)(struct ntrdma_rres *rres))
{
	int rc;

	rc = ntrdma_obj_init(&rres->obj, dev);
	if (rc)
		return rc;

	rres->vec = vec;
	rres->free = free;
	rres->key = 0;

	return 0;
}

void ntrdma_rres_deinit(struct ntrdma_rres *rres)
{
	ntrdma_obj_deinit(&rres->obj);
}

int ntrdma_rres_add(struct ntrdma_rres *rres, ntrdma_u32_t key)
{
	struct ntrdma_dev *dev = ntrdma_rres_dev(rres);
	int rc;

	ntrdma_mut_lock(&dev->rres_lock);
	{
		ntrdma_rres_get(rres);

		rc = ntrdma_vec_ensure_key(rres->vec, key, dev->node);
		if (rc)
			goto err_key;

		ntrdma_vec_lock(rres->vec);
		{
			ntrdma_vec_set(rres->vec, rres->key, rres);
		}
		ntrdma_vec_unlock(rres->vec);

		ntrdma_list_add_tail(&dev->rres_list, &rres->obj.dev_entry);
	}
	ntrdma_mut_unlock(&dev->rres_lock);

	return 0;

err_key:
	ntrdma_rres_put(rres);
	ntrdma_mut_unlock(&dev->rres_lock);
	return rc;
}

void ntrdma_rres_del(struct ntrdma_rres *rres)
{
	struct ntrdma_dev *dev = ntrdma_rres_dev(rres);

	ntrdma_mut_lock(&dev->rres_lock);
	{
		ntrdma_list_del(&rres->obj.dev_entry);

		ntrdma_vec_lock(rres->vec);
		{
			ntrdma_vec_set(rres->vec, rres->key, NULL);
		}
		ntrdma_vec_unlock(rres->vec);
	}
	ntrdma_mut_unlock(&dev->rres_lock);

	ntrdma_rres_put(rres);
}

