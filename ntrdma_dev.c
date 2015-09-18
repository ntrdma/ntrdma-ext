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

#include "ntrdma_os.h"
#include "ntrdma_mem.h"

#include "ntrdma_dev.h"
#include "ntrdma_cmd.h"
#include "ntrdma_vbell.h"
#include "ntrdma_ping.h"
#include "ntrdma_port.h"
#include "ntrdma_ib.h"

#ifdef NTRDMA_CONFIG_ETH
#include "ntrdma_eth.h"
#endif

#include "ntrdma_cq.h"
#include "ntrdma_pd.h"
#include "ntrdma_mr.h"
#include "ntrdma_qp.h"

#define NTRDMA_DEV_VBELL_COUNT 0x400
#define NTRDMA_DEV_VBELL_START 0x8

#define NTRDMA_DEV_CMD_RECV_VBELL_IDX 0
#define NTRDMA_DEV_CMD_SEND_VBELL_IDX 1
#define NTRDMA_DEV_CMD_SEND_CAP 0x10

int ntrdma_dev_init(struct ntrdma_dev *dev)
{
	int err;

	err = ntrdma_dev_vbell_init(dev,
				    NTRDMA_DEV_VBELL_COUNT,
				    NTRDMA_DEV_VBELL_START);
	if (err) {
		ntrdma_dbg(dev, "vbell failed\n");
		goto err_vbell;
	}

	err = ntrdma_dev_cmd_init(dev,
				  NTRDMA_DEV_CMD_RECV_VBELL_IDX,
				  NTRDMA_DEV_CMD_SEND_VBELL_IDX,
				  NTRDMA_DEV_CMD_SEND_CAP);
	if (err) {
		ntrdma_dbg(dev, "cmd failed\n");
		goto err_cmd;
	}

	err = ntrdma_dev_res_init(dev);
	if (err) {
		ntrdma_dbg(dev, "res failed\n");
		goto err_res;
	}

#ifdef CONFIG_NTRDMA_ETH
	err = ntrdma_dev_eth_init(dev);
	if (err) {
		ntrdma_dbg(dev, "eth failed\n");
		goto err_eth;
	}
#else
	dev->eth = NULL;
#endif

	err = ntrdma_ib_init(dev);
	if (err) {
		ntrdma_dbg(dev, "ib failed\n");
		goto err_ib;
	}

	ntrdma_debugfs_dev_add(dev);

	return 0;

	//ntrdma_ib_deinit(dev);
err_ib:
#ifdef CONFIG_NTRDMA_ETH
	ntrdma_dev_eth_deinit(dev);
err_eth:
#endif
	ntrdma_dev_res_deinit(dev);
err_res:
	ntrdma_dev_cmd_deinit(dev);
err_cmd:
	ntrdma_dev_vbell_deinit(dev);
err_vbell:
	ntrdma_dbg(dev, "failed, returning err %d\n", err);
	return err;
}

int ntrdma_dev_enable(struct ntrdma_dev *dev)
{
	ntrdma_dev_res_enable(dev);
	return 0;
}

void ntrdma_dev_disable(struct ntrdma_dev *dev)
{
	ntrdma_dbg(dev, "not implemented\n");
}

void ntrdma_dev_deinit(struct ntrdma_dev *dev)
{
	ntrdma_debugfs_dev_del(dev);
	ntrdma_ib_deinit(dev);
#ifdef CONFIG_NTRDMA_ETH
	ntrdma_dev_eth_deinit(dev);
#endif
	ntrdma_dev_res_deinit(dev);
	ntrdma_dev_cmd_deinit(dev);
	ntrdma_dev_vbell_deinit(dev);
}

void ntrdma_dev_quiesce(struct ntrdma_dev *dev)
{
	ntrdma_dbg(dev, "not implemented\n");
}

