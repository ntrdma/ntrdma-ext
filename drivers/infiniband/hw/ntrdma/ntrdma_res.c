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
#include "ntrdma_util.h"
#include "ntrdma_res.h"
#include "ntrdma_cmd.h"
#include "ntrdma_qp.h"
#include "ntrdma_mr.h"

int ntrdma_dev_res_init(struct ntrdma_dev *dev)
{
	int rc;

	/* rdma resource synchronization state */
	mutex_init(&dev->res.lock);
	mutex_init(&dev->rres.lock);

	/* rdma local resources */

	dev->res.pd_next_key = 0;
	INIT_LIST_HEAD(&dev->res.pd_list);
	INIT_LIST_HEAD(&dev->res.cq_list);
	INIT_LIST_HEAD(&dev->res.mr_list);
	INIT_LIST_HEAD(&dev->res.qp_list);

	rc = ntrdma_kvec_init(&dev->res.mr_vec,
			NTRDMA_RES_VEC_INIT_CAP, NTRDMA_MR_VEC_PREALLOCATED,
			dev->node);

	if (rc) {
		ntrdma_err(dev, "mr vec failed\n");
		goto err_mr;
	}

	rc = ntrdma_kvec_init(&dev->res.qp_vec,
			NTRDMA_RES_VEC_INIT_CAP, NTRDMA_QP_VEC_PREALLOCATED,
			dev->node);

	if (rc) {
		ntrdma_err(dev, "qp vec failed\n");
		goto err_qp;
	}

	/* rdma remote resources */

	INIT_LIST_HEAD(&dev->rres.list);

	rc = ntrdma_vec_init(&dev->rres.rmr_vec,
			NTRDMA_RES_VEC_INIT_CAP,
			dev->node);

	if (rc) {
		ntrdma_err(dev, "rmr vec failed\n");
		goto err_rmr;
	}

	rc = ntrdma_vec_init(&dev->rres.rqp_vec,
			NTRDMA_RES_VEC_INIT_CAP,
			dev->node);

	if (rc) {
		ntrdma_err(dev, "rqp vec failed\n");
		goto err_rqp;
	}

	return 0;

err_rqp:
	ntrdma_vec_deinit(&dev->rres.rmr_vec);
err_rmr:
	ntrdma_kvec_deinit(&dev->res.qp_vec);
err_qp:
	ntrdma_kvec_deinit(&dev->res.mr_vec);
err_mr:
	return rc;
}

void ntrdma_dev_res_deinit(struct ntrdma_dev *dev)
{
	ntrdma_vec_deinit(&dev->rres.rqp_vec);
	ntrdma_vec_deinit(&dev->rres.rmr_vec);
	ntrdma_kvec_deinit(&dev->res.qp_vec);
	ntrdma_kvec_deinit(&dev->res.mr_vec);
}

inline int ntrdma_dev_cmd_submit(struct ntrdma_dev *dev)
{
	return ntrdma_vbell_trigger(&dev->cmd_send.vbell);
}

inline bool ntrdma_cmd_cb_unlink(struct ntrdma_dev *dev,
				struct ntrdma_cmd_cb *cb)
{
	bool result;

	mutex_lock(&dev->cmd_send.lock);
	if (cb->in_list) {
		list_del(&cb->dev_entry);
		cb->in_list = false;
		result = true;
	} else
		result = false;
	complete_all(&cb->cmds_done);
	mutex_unlock(&dev->cmd_send.lock);

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
		ntrdma_err(dev, "ntrdma res cmd timeout %ld id %d cb %p %pf\n",
				timeout, cb->cmd_id, cb, cb->cmd_prep);

	return rc;
}

int ntrdma_dev_res_enable(struct ntrdma_dev *dev)
{
	struct ntrdma_qp *qp;
	struct ntrdma_mr *mr;
	unsigned long timeout = msecs_to_jiffies(CMD_TIMEOUT_MSEC);
	bool need_unlink;
	int rc = 0;
	int r;

	TRACE("resources enabled\n");

	mutex_lock(&dev->res.lock);

	list_for_each_entry(mr, &dev->res.mr_list, res.obj.dev_entry) {
		timeout = max_t(typeof(timeout), timeout, mr->res.timeout);
		ntrdma_mr_enable(mr);
	}
	list_for_each_entry(qp, &dev->res.qp_list, res.obj.dev_entry) {
		timeout = max_t(typeof(timeout), timeout, qp->res.timeout);
		ntrdma_qp_enable(qp);
	}

	if (ntrdma_dev_cmd_submit(dev) < 0) {
		need_unlink = true;
		goto unlock;
	}
	need_unlink = false;

	list_for_each_entry(mr, &dev->res.mr_list, res.obj.dev_entry) {
		r = ntrdma_cmd_cb_wait_out(dev, &mr->enable_mrcb.cb, &timeout);
		rc = rc ? : r;
	}
	list_for_each_entry(qp, &dev->res.qp_list, res.obj.dev_entry) {
		r = ntrdma_cmd_cb_wait_out(dev, &qp->enable_qpcb.cb, &timeout);
		rc = rc ? : r;
	}

 unlock:
	mutex_unlock(&dev->res.lock);

	if (need_unlink) {
		/*FIXME shouldn't we proteect mr/qp lists with res lock ?*/
		list_for_each_entry(mr, &dev->res.mr_list, res.obj.dev_entry)
			ntrdma_cmd_cb_unlink(dev, &mr->enable_mrcb.cb);

		list_for_each_entry(qp, &dev->res.qp_list, res.obj.dev_entry)
			ntrdma_cmd_cb_unlink(dev, &mr->enable_mrcb.cb);
	}

	return rc;
}

void ntrdma_dev_res_disable(struct ntrdma_dev *dev)
{
	struct ntrdma_qp *qp;

	ntrdma_vdbg(dev, "res disable starting ...");

	mutex_lock(&dev->res.lock);

	list_for_each_entry_reverse(qp, &dev->res.qp_list, res.obj.dev_entry) {
		ntrdma_qp_reset(qp);
	}
	mutex_unlock(&dev->res.lock);

	ntrdma_vdbg(dev, "res disable done");
}

void ntrdma_dev_res_reset(struct ntrdma_dev *dev)
{
	ntrdma_err(dev, "not implemented\n");
}

struct ntrdma_res *ntrdma_res_look(struct ntrdma_kvec *vec, u32 key)
{
	struct ntrdma_res *res;
	struct ntrdma_rcu_kvec *rkvec;

	rcu_read_lock();

	rkvec = rcu_dereference(vec->rkvec);
	if (!rkvec) {
		rcu_read_unlock();
		return NULL;
	}
	if (key < rkvec->cap) {
		res = rkvec->look[key];
		if (res)
			ntrdma_res_get(res);
	} else
		res = NULL;

	rcu_read_unlock();

	return res;
}

void ntrdma_dev_rres_reset(struct ntrdma_dev *dev)
{
	struct ntrdma_rres *rres, *rres_next;

	mutex_lock(&dev->rres.lock);
	list_for_each_entry_safe_reverse(rres, rres_next,
			&dev->rres.list,
			obj.dev_entry) {
		ntrdma_rres_remove_unsafe(rres);
		rres->free(rres);
	}
	INIT_LIST_HEAD(&dev->rres.list);
	mutex_unlock(&dev->rres.lock);
}

struct ntrdma_rres *ntrdma_rres_look(struct ntrdma_vec *vec, u32 key)
{
	struct ntrdma_rcu_vec *rvec;
	struct ntrdma_rres *rres;

	rcu_read_lock();

	rvec = rcu_dereference(vec->rvec);
	if (!rvec) {
		rcu_read_unlock();
		return NULL;
	}
	if (key < rvec->cap) {
		rres = rvec->look[key];
		if (rres)
			ntrdma_rres_get(rres);
	} else
		rres = NULL;

	rcu_read_unlock();

	return rres;
}

void ntrdma_res_init(struct ntrdma_res *res,
		struct ntrdma_dev *dev,
		int (*enable)(struct ntrdma_res *res,
			struct ntrdma_cmd_cb *cb),
		int (*disable)(struct ntrdma_res *res,
				struct ntrdma_cmd_cb *cb))
{
	ntrdma_obj_init(&res->obj, dev);

	res->enable = enable;
	res->disable = disable;

	res->timeout = msecs_to_jiffies(CMD_TIMEOUT_MSEC);

	mutex_init(&res->lock);
}

int ntrdma_res_add(struct ntrdma_res *res, struct ntrdma_cmd_cb *cb,
		struct list_head *res_list, struct ntrdma_kvec *res_vec)
{
	struct ntrdma_dev *dev = ntrdma_res_dev(res);
	int rc = 0;

	mutex_lock(&dev->res.lock);
	ntrdma_res_lock(res);

	ntrdma_kvec_set_key(dev->node, res_vec, res->key, res);

	list_add_tail(&res->obj.dev_entry, res_list);

	rc = res->enable(res, cb);

	ntrdma_res_unlock(res);
	mutex_unlock(&dev->res.lock);

	if (rc) {
		complete_all(&cb->cmds_done);
		return 0;
	}

	rc = ntrdma_dev_cmd_submit(dev);
	if (unlikely(rc)) {
		ntrdma_cmd_cb_unlink(dev, cb);
		return 0;
	}

	rc = ntrdma_res_wait_cmds(dev, cb, res->timeout);
	if (unlikely(rc)) {
		list_del(&res->obj.dev_entry);
		ntrdma_kvec_set_key(dev->node, res_vec, res->key, NULL);
		ntrdma_err(dev, "res %d, timeout %u [msec] cb %p\n",
				res->key, jiffies_to_msecs(res->timeout), cb);
		ntrdma_unrecoverable_err(dev);
	}

	return rc;
}

void ntrdma_res_del(struct ntrdma_res *res, struct ntrdma_cmd_cb *cb,
		struct ntrdma_kvec *res_vec)
{
	struct ntrdma_dev *dev = ntrdma_res_dev(res);
	int rc = 0;

	mutex_lock(&dev->res.lock);
	ntrdma_res_lock(res);
	rc = res->disable(res, cb);
	list_del(&res->obj.dev_entry);
	ntrdma_kvec_set_key(dev->node, res_vec, res->key, NULL);
	ntrdma_res_unlock(res);
	mutex_unlock(&dev->res.lock);

	if (unlikely(rc)) {
		complete_all(&cb->cmds_done);
		return;
	}

	rc = ntrdma_dev_cmd_submit(dev);
	if (unlikely(rc)) {
		ntrdma_cmd_cb_unlink(dev, cb);
		return;
	}

	rc = ntrdma_res_wait_cmds(dev, cb, res->timeout);
	if (unlikely(rc)) {
		ntrdma_dbg(dev, "wait cmd failed after %ld\n", res->timeout);
		ntrdma_unrecoverable_err(dev);
	}
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

	rc = ntrdma_vec_set(rres->vec, rres->key, rres, dev->node);
	if (rc < 0)
		return rc;

	mutex_lock(&dev->rres.lock);
	rres->in_rres_list = true;
	list_add_tail(&rres->obj.dev_entry, &dev->rres.list);
	mutex_unlock(&dev->rres.lock);

	return 0;
}

void ntrdma_rres_remove_unsafe(struct ntrdma_rres *rres)
{
	struct ntrdma_dev *dev = ntrdma_rres_dev(rres);
	ntrdma_vec_set(rres->vec, rres->key, NULL, dev->node);
	rres->key = ~0;
	if (rres->in_rres_list) {
		rres->in_rres_list = false;
		list_del(&rres->obj.dev_entry);
	}

}

void ntrdma_rres_remove(struct ntrdma_rres *rres)
{
	struct ntrdma_dev *dev = ntrdma_rres_dev(rres);

	mutex_lock(&dev->rres.lock);
	ntrdma_rres_remove_unsafe(rres);
	mutex_unlock(&dev->rres.lock);
}

