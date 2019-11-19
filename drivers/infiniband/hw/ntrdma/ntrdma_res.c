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
#include "ntrdma_qp.h"
#include "ntrdma_mr.h"
#include <linux/ntc_trace.h>

int ntrdma_dev_res_init(struct ntrdma_dev *dev)
{
	int rc;

	/* rdma resource synchronization state */

	dev->res_enable = 0;

	mutex_init(&dev->res_lock);
	mutex_init(&dev->rres_lock);

	/* rdma local resources */

	dev->pd_next_key = 0;
	INIT_LIST_HEAD(&dev->pd_list);
	INIT_LIST_HEAD(&dev->cq_list);
	INIT_LIST_HEAD(&dev->mr_list);
	INIT_LIST_HEAD(&dev->qp_list);

	rc = ntrdma_kvec_init(&dev->mr_vec,
			NTRDMA_RES_VEC_INIT_CAP,
			dev->node, 0);

	if (rc) {
		ntrdma_dbg(dev, "mr vec failed\n");
		goto err_mr;
	}

	rc = ntrdma_kvec_init(&dev->qp_vec,
			NTRDMA_RES_VEC_INIT_CAP,
			dev->node, 2);

	if (rc) {
		ntrdma_dbg(dev, "qp vec failed\n");
		goto err_qp;
	}

	/* rdma remote resources */

	INIT_LIST_HEAD(&dev->rres_list);

	rc = ntrdma_vec_init(&dev->rmr_vec,
			NTRDMA_RES_VEC_INIT_CAP,
			dev->node);

	if (rc) {
		ntrdma_dbg(dev, "rmr vec failed\n");
		goto err_rmr;
	}

	rc = ntrdma_vec_init(&dev->rqp_vec,
			NTRDMA_RES_VEC_INIT_CAP,
			dev->node);

	if (rc) {
		ntrdma_dbg(dev, "rqp vec failed\n");
		goto err_rqp;
	}

	return 0;

	//ntrdma_vec_deinit(&dev->rqp_vec);
err_rqp:
	ntrdma_vec_deinit(&dev->rmr_vec);
err_rmr:
	ntrdma_kvec_deinit(&dev->qp_vec);
err_qp:
	ntrdma_kvec_deinit(&dev->mr_vec);
err_mr:
	return rc;
}

void ntrdma_dev_res_deinit(struct ntrdma_dev *dev)
{
	ntrdma_vec_deinit(&dev->rqp_vec);
	ntrdma_vec_deinit(&dev->rmr_vec);
	ntrdma_kvec_deinit(&dev->qp_vec);
	ntrdma_kvec_deinit(&dev->mr_vec);
}

static inline bool ntrdma_cmd_cb_unlink(struct ntrdma_dev *dev,
					struct ntrdma_cmd_cb *cb)
{
	bool result;

	mutex_lock(&dev->cmd_send_lock);
	if (cb->in_list) {
		list_del(&cb->dev_entry);
		cb->in_list = false;
		result = true;
	} else
		result = false;
	mutex_unlock(&dev->cmd_send_lock);

	return result;
}

static inline int ntrdma_cmd_cb_wait_out(struct ntrdma_dev *dev,
					struct ntrdma_cmd_cb *cb,
					unsigned long *timeout)
{
	unsigned long t = *timeout;

	if (t)
		t = wait_for_completion_timeout(&cb->cmds_done, t);

	if (!t && ntrdma_cmd_cb_unlink(dev, cb)) {
		*timeout = 0;
		return -ETIME;
	}

	*timeout = t + 1;
	return 0;
}

inline int ntrdma_res_wait_cmds(struct ntrdma_dev *dev,
				struct ntrdma_cmd_cb *cb,
				unsigned long timeout)
{
	int rc;

	rc = ntrdma_cmd_cb_wait_out(dev, cb, &timeout);

	if (rc < 0)
		ntrdma_err(dev, "ntrdma res cmd timeout");

	return rc;
}

int ntrdma_dev_res_enable(struct ntrdma_dev *dev)
{
	struct ntrdma_qp *qp;
	struct ntrdma_mr *mr;
	unsigned long timeout = CMD_TIMEOUT_MSEC;
	int rc = 0;
	int r;

	TRACE("resources enabled\n");

	mutex_lock(&dev->res_lock);

	dev->res_enable = 0;

	list_for_each_entry(mr, &dev->mr_list, res.obj.dev_entry) {
		timeout = max_t(typeof(timeout), timeout, mr->res.timeout);
		ntrdma_mr_enable(mr);
	}
	list_for_each_entry(qp, &dev->qp_list, res.obj.dev_entry) {
		timeout = max_t(typeof(timeout), timeout, qp->res.timeout);
		ntrdma_qp_enable(qp);
	}

	ntrdma_dev_cmd_submit(dev);

	list_for_each_entry(mr, &dev->mr_list, res.obj.dev_entry) {
		r = ntrdma_cmd_cb_wait_out(dev, &mr->enable_mrcb.cb, &timeout);
		rc = rc ? : r;
	}
	list_for_each_entry(qp, &dev->qp_list, res.obj.dev_entry) {
		r = ntrdma_cmd_cb_wait_out(dev, &qp->enable_qpcb.cb, &timeout);
		rc = rc ? : r;
	}

	if (likely(!rc))
		dev->res_enable = 1;
	else
		ntrdma_err(dev, "ntrdma cmd timeout in %s", __func__);

	mutex_unlock(&dev->res_lock);

	return rc;
}

void ntrdma_dev_res_disable(struct ntrdma_dev *dev)
{
	struct ntrdma_qp *qp;

	pr_info("res disable starting ...\n");

	mutex_lock(&dev->res_lock);
	dev->res_enable = 0;

	list_for_each_entry_reverse(qp, &dev->qp_list, res.obj.dev_entry) {
		ntrdma_qp_reset(qp);
	}
	mutex_unlock(&dev->res_lock);

	pr_info("res disable done\n");
}

void ntrdma_dev_res_reset(struct ntrdma_dev *dev)
{
	ntrdma_dbg(dev, "not implemented\n");
}

struct ntrdma_res *ntrdma_dev_res_look(struct ntrdma_dev *dev,
				       struct ntrdma_kvec *vec, int key)
{
	struct ntrdma_res *res;

	ntrdma_kvec_read_lock(vec);

	res = ntrdma_kvec_look(vec, key);
	if (res)
		ntrdma_res_get(res);

	ntrdma_kvec_read_unlock(vec);

	return res;
}

void ntrdma_dev_rres_reset(struct ntrdma_dev *dev)
{
	struct ntrdma_rres *rres, *rres_next;

	mutex_lock(&dev->rres_lock);
	{
		list_for_each_entry_safe_reverse(rres, rres_next,
						 &dev->rres_list,
						 obj.dev_entry) {
			ntrdma_vec_lock(rres->vec);
			{
				ntrdma_vec_set(rres->vec, rres->key, NULL);
			}
			ntrdma_vec_unlock(rres->vec);

			rres->free(rres);
		}
		INIT_LIST_HEAD(&dev->rres_list);
	}
	mutex_unlock(&dev->rres_lock);
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
		void (*enable)(struct ntrdma_res *res,
			struct ntrdma_cmd_cb *cb),
		void (*disable)(struct ntrdma_res *res,
				struct ntrdma_cmd_cb *cb),
		int reserved_key)
{
	int rc;

	ntrdma_obj_init(&res->obj, dev);

	res->vec = vec;

	res->enable = enable;
	res->disable = disable;

	res->timeout = msecs_to_jiffies(CMD_TIMEOUT_MSEC);

	mutex_init(&res->lock);

	if (reserved_key == -1) {
		mutex_lock(&dev->res_lock);
		{
			res->key = ntrdma_kvec_reserve_key(res->vec, dev->node);
			if (res->key < 0) {
				rc = -ENOMEM;
				goto err_key;
			}
		}
		mutex_unlock(&dev->res_lock);
	} else {
		res->key = reserved_key;
	}
	return 0;

err_key:
	mutex_unlock(&dev->res_lock);
	return rc;
}

void ntrdma_res_deinit(struct ntrdma_res *res)
{
	struct ntrdma_dev *dev = ntrdma_res_dev(res);

	mutex_lock(&dev->res_lock);
	{
		ntrdma_kvec_dispose_key(res->vec, res->key);
	}
	mutex_unlock(&dev->res_lock);
}

int ntrdma_res_add(struct ntrdma_res *res, struct ntrdma_cmd_cb *cb,
		struct list_head *res_list)
{
	struct ntrdma_dev *dev = ntrdma_res_dev(res);
	int rc;

	ntrdma_vdbg(dev, "resource obtained\n");

	ntrdma_res_lock(res);
	mutex_lock(&dev->res_lock);
	{
		ntrdma_kvec_write_lock(res->vec);
		{
			ntrdma_kvec_set(res->vec, res->key, res);
		}
		ntrdma_kvec_write_unlock(res->vec);

		list_add_tail(&res->obj.dev_entry, res_list);

		ntrdma_vdbg(dev, "resource added\n");

		if (dev->res_enable) {
			ntrdma_vdbg(dev, "resource commands initiated\n");
			res->enable(res, cb);
			ntrdma_dev_cmd_submit(dev);
		} else {
			ntrdma_vdbg(dev, "no commands\n");
			complete_all(&cb->cmds_done);
		}
	}
	mutex_unlock(&dev->res_lock);

	ntrdma_vdbg(dev, "wait for commands\n");
	rc = ntrdma_res_wait_cmds(dev, cb, res->timeout);
	ntrdma_res_unlock(res);

	if (rc)
		ntrdma_unrecoverable_err(dev);

	ntrdma_vdbg(dev, "done waiting\n");

	return rc;
}

void ntrdma_res_del(struct ntrdma_res *res, struct ntrdma_cmd_cb *cb)
{
	struct ntrdma_dev *dev = ntrdma_res_dev(res);
	int rc;

	ntrdma_res_lock(res);
	mutex_lock(&dev->res_lock);
	{
		if (dev->res_enable) {
			res->disable(res, cb);
			ntrdma_dev_cmd_submit(dev);
		} else {
			complete_all(&cb->cmds_done);
		}

		list_del(&res->obj.dev_entry);

		ntrdma_kvec_write_lock(res->vec);
		{
			ntrdma_kvec_set(res->vec, res->key, NULL);
		}
		ntrdma_kvec_write_unlock(res->vec);
	}
	mutex_unlock(&dev->res_lock);

	rc = ntrdma_res_wait_cmds(dev, cb, res->timeout);
	ntrdma_res_unlock(res);

	if (rc)
		ntrdma_unrecoverable_err(dev);
}

void ntrdma_res_put(struct ntrdma_res *res,
		void (*obj_release)(struct kref *kref))
{
	ntrdma_obj_put(&res->obj, obj_release);
}

void ntrdma_rres_init(struct ntrdma_rres *rres,
		struct ntrdma_dev *dev, struct ntrdma_vec *vec,
		void (*free)(struct ntrdma_rres *rres),
		u32 key)
{
	ntrdma_obj_init(&rres->obj, dev);

	rres->vec = vec;
	rres->free = free;
	rres->key = key;
}

int ntrdma_rres_add(struct ntrdma_rres *rres)
{
	struct ntrdma_dev *dev = ntrdma_rres_dev(rres);
	int rc;

	mutex_lock(&dev->rres_lock);
	{
		rc = ntrdma_vec_ensure_key(rres->vec, rres->key, dev->node);
		if (rc)
			goto err_key;

		ntrdma_vec_lock(rres->vec);
		{
			ntrdma_vec_set(rres->vec, rres->key, rres);
		}
		ntrdma_vec_unlock(rres->vec);

		list_add_tail(&rres->obj.dev_entry, &dev->rres_list);
	}
	mutex_unlock(&dev->rres_lock);

	return 0;

err_key:
	mutex_unlock(&dev->rres_lock);
	return rc;
}

void ntrdma_rres_remove_unsafe(struct ntrdma_rres *rres)
{
	list_del(&rres->obj.dev_entry);

	ntrdma_vec_lock(rres->vec);
	ntrdma_vec_set(rres->vec, rres->key, NULL);
	rres->key = ~0;
	ntrdma_vec_unlock(rres->vec);
}

void ntrdma_rres_remove(struct ntrdma_rres *rres)
{
	struct ntrdma_dev *dev = ntrdma_rres_dev(rres);

	mutex_lock(&dev->rres_lock);
	ntrdma_rres_remove_unsafe(rres);
	mutex_unlock(&dev->rres_lock);
}

