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

#include <linux/cpumask.h>
#include "ntrdma_cmd.h"
#include "ntrdma_vbell.h"

#include "ntrdma.h"
#include "ntrdma_dev.h"
#include "ntrdma_qp.h"
#include "ntrdma_cm.h"

#define NTRDMA_DEV_VBELL_START 0x8

#define NTRDMA_DEV_CMD_RECV_VBELL_IDX 0
#define NTRDMA_DEV_CMD_SEND_VBELL_IDX 1
#define NTRDMA_DEV_CMD_SEND_CAP 0x10

#define NTRDMA_DEV_ETH_VBELL_IDX 2
#define NTRDMA_DEV_ETH_RX_CAP 0x100


#define ntrdma_of_qp_close_work(__ws) \
	container_of(__ws, struct ntrdma_dev, qp_close_work)


DEFINE_PER_CPU(struct ntrdma_dev_counters, dev_cnt);
EXPORT_PER_CPU_SYMBOL(dev_cnt);
static void ntrdma_ntc_link_reset_cb(struct work_struct *ws);

static int ntrdma_ntc_hello(void *ctx, int phase,
				void *in_buf, size_t in_size,
				void *out_buf, size_t out_size)
{
	struct ntrdma_dev *dev = ctx;

	return ntrdma_dev_hello(dev, phase);
}

static int ntrdma_ntc_enable(void *ctx)
{
	struct ntrdma_dev *dev = ctx;

	return ntrdma_dev_enable(dev);
}

static void ntrdma_ntc_disable(void *ctx)
{
	struct ntrdma_dev *dev = ctx;

	ntrdma_dev_disable(dev);
}

static void ntrdma_ntc_quiesce(void *ctx)
{
	struct ntrdma_dev *dev = ctx;

	ntrdma_dev_quiesce(dev);
}

static void ntrdma_ntc_reset(void *ctx)
{
	struct ntrdma_dev *dev = ctx;

	ntrdma_dev_reset(dev);
}

static void ntrdma_ntc_signal(void *ctx, int vec)
{
	struct ntrdma_dev *dev = ctx;

	tasklet_schedule(&dev->vbell_work[vec]);
}

static struct ntc_ctx_ops ntrdma_ntc_ctx_ops = {
	.hello = ntrdma_ntc_hello,
	.enable = ntrdma_ntc_enable,
	.disable = ntrdma_ntc_disable,
	.quiesce = ntrdma_ntc_quiesce,
	.reset = ntrdma_ntc_reset,
	.signal = ntrdma_ntc_signal,
};

int ntrdma_dev_init(struct ntrdma_dev *dev, struct ntc_dev *ntc)
{
	int rc;
	int i = 0;
	int num_cpus;

	dev->ntc = ntc;

	/* Must be before device is registered (in ntrdma_dev_ib_init) */
	mutex_init(&dev->debugfs_lock);

	rc = ntrdma_dev_vbell_init(dev,
				   NTRDMA_DEV_VBELL_COUNT,
				   NTRDMA_DEV_VBELL_START);
	if (rc)
		goto err_vbell;

	rc = ntrdma_dev_cmd_init(dev,
				 NTRDMA_DEV_CMD_RECV_VBELL_IDX,
				 NTRDMA_DEV_CMD_SEND_VBELL_IDX,
				 NTRDMA_DEV_CMD_SEND_CAP);
	if (rc)
		goto err_cmd;

	rc = ntrdma_dev_eth_init(dev,
				 NTRDMA_DEV_ETH_VBELL_IDX,
				 NTRDMA_DEV_ETH_RX_CAP);
	if (rc)
		goto err_eth;

	rc = ntrdma_dev_res_init(dev);
	if (rc)
		goto err_res;

	rc = ntrdma_dev_ib_init(dev);
	if (rc)
		goto err_ib;

	rc = ntc_set_ctx(ntc, dev, &ntrdma_ntc_ctx_ops);
	if (rc)
		goto err_ntc;

	rc = ntrdma_dev_hello_init(dev, ntc);
	if (rc)
		goto err_hello;

	/* counters */
	num_cpus = num_online_cpus();

	for (i = 0; i < num_cpus; i++)
		memset(per_cpu_ptr(&dev_cnt, i), 0,
				sizeof(struct ntrdma_dev_counters));

	ntrdma_debugfs_dev_add(dev);

	atomic_set(&dev->qp_num, 0);
	atomic_set(&dev->cq_num, 0);
	atomic_set(&dev->mr_num, 0);
	atomic_set(&dev->pd_num, 0);
	INIT_WORK(&dev->ntc_link_reset_work, ntrdma_ntc_link_reset_cb);

	return 0;

err_hello:
	ntrdma_dev_hello_deinit(dev);
err_ntc:
	ntrdma_dev_ib_deinit(dev);
err_ib:
	ntrdma_dev_eth_deinit(dev);
err_eth:
	ntrdma_dev_res_deinit(dev);
err_res:
	ntrdma_dev_cmd_deinit(dev);
err_cmd:
	ntrdma_dev_vbell_deinit(dev);
err_vbell:
	return rc;
}

int ntrdma_dev_hello_init(struct ntrdma_dev *dev, struct ntc_dev *ntc)
{
	dev->hello_local_buf = ntc_local_hello_buf(ntc, &dev->hello_local_buf_size);
	dev->hello_peer_buf = ntc_peer_hello_buf(ntc, &dev->hello_peer_buf_size);

	ntrdma_dbg(dev, "local %p size %d peer %p size %d\n",
				dev->hello_local_buf, dev->hello_local_buf_size,
				dev->hello_peer_buf, dev->hello_peer_buf_size);

	return !(dev->hello_local_buf && dev->hello_peer_buf &&
			dev->hello_local_buf_size > 0 && dev->hello_peer_buf_size > 0);
}

void ntrdma_dev_hello_deinit(struct ntrdma_dev *dev)
{
	dev->hello_local_buf = NULL;
	dev->hello_peer_buf = NULL;
	dev->hello_local_buf_size = 0;
	dev->hello_peer_buf_size = 0;
}

void ntrdma_dev_deinit(struct ntrdma_dev *dev)
{
	ntrdma_info(dev, "Starting dev deinit...\n");
	ntrdma_debugfs_dev_del(dev);
	ntrdma_dev_eth_deinit(dev);
	ntrdma_dev_res_deinit(dev);
	ntrdma_dev_cmd_deinit(dev);
	ntrdma_dev_vbell_deinit(dev);
	cancel_work_sync(&dev->ntc_link_reset_work);
	ntrdma_info(dev, "Dev deinit finished\n");
}

int ntrdma_dev_enable(struct ntrdma_dev *dev)
{
	int rc;

	ntrdma_dev_eth_enable(dev);
	ntrdma_dev_cmd_enable(dev);
	rc = ntrdma_dev_res_enable(dev);

	return rc;
}

void ntrdma_dev_disable(struct ntrdma_dev *dev)
{
	ntrdma_dev_cmd_disable(dev);
	ntrdma_dev_eth_disable(dev);
}

void ntrdma_dev_quiesce(struct ntrdma_dev *dev)
{
	ntrdma_dev_vbell_disable(dev);
	ntrdma_dev_eth_quiesce(dev);
	/* resource disable should block from new commands to be submitted */
	ntrdma_dev_res_disable(dev);
	ntrdma_cm_shutdown(dev); /*FIXME should be moved inside ntrdma_dev_res_disable*/
	/* cmd quiesce should block till all in progress commands completed */
	ntrdma_dev_cmd_quiesce(dev);
}

void ntrdma_dev_reset(struct ntrdma_dev *dev)
{
	ntrdma_dev_eth_reset(dev);
	ntrdma_dev_rres_reset(dev);
	ntrdma_dev_cmd_reset(dev);
	ntrdma_dev_vbell_reset(dev);
}

static void ntrdma_ntc_link_reset_cb(struct work_struct *ws)
{
	struct ntrdma_dev *dev;

	dev = ntrdma_ntc_link_reset_work_dev(ws);
	ntc_link_reset(dev->ntc, ASYNC_RESET);
}

void _ntrdma_unrecoverable_err(struct ntrdma_dev *dev,
		const char *f)
{
	ntrdma_err(dev, "unrecoverable error called by %s ", f);
	ntrdma_err(dev, "disabling NTB link...\n");
	ntc_link_disable(dev->ntc);
	ntrdma_err(dev, "NTB link disabled, resetting NTC state machine\n");
	schedule_work(&dev->ntc_link_reset_work);
	ntrdma_err(dev, "NTC state machine reset finished\n");
}


