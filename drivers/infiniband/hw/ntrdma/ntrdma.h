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

#ifndef NTRDMA_H
#define NTRDMA_H

#include <linux/kernel.h>
#include "ntc.h"
#include "ntrdma_file.h"

/* #define NTRDMA_QP_DEBUG */

#define NTRDMA_PERF_MEASURE(name) do {					\
		if (ntrdma_measure_perf) NTC_PERF_MEASURE(name);	\
	} while (false)

#define NTRDMA_RESERVED_DMA_LEKY 0xFFFF

#define to_ptrhld(ptr) ((unsigned long)(void *)(ptr))
#define of_ptrhld(ptr) ((void *)(ptr))

struct dentry;

struct ntrdma_dev;
struct ntrdma_eth;

struct ntrdma_obj;
struct ntrdma_res;
struct ntrdma_rres;

struct ntrdma_cq;
struct ntrdma_mr;
struct ntrdma_pd;
struct ntrdma_qp;

struct ntrdma_rmr;
struct ntrdma_rpd;
struct ntrdma_rqp;

struct ntrdma_cqe;
struct ntrdma_poll;
struct ntrdma_recv_wqe;
struct ntrdma_send_wqe;

struct ntrdma_vbell;
struct ntrdma_vbell_head;

union ntrdma_cmd;
union ntrdma_rsp;

int __init ntrdma_vbell_module_init(void);
void ntrdma_vbell_module_deinit(void);

int __init ntrdma_qp_module_init(void);
void ntrdma_qp_module_deinit(void);

int __init ntrdma_ib_module_init(void);
void ntrdma_ib_module_deinit(void);
#ifdef NTRDMA_FULL_ETH
int __init ntrdma_eth_module_init(void);
void ntrdma_eth_module_deinit(void);
void ntrdma_dev_eth_reset(struct ntrdma_dev *dev);
#else
static inline
int __init ntrdma_eth_module_init(void)
{
	return 0;
}
static inline
void ntrdma_eth_module_deinit(void) {}
static inline
void ntrdma_dev_eth_reset(struct ntrdma_dev *dev) {}
#endif

int __init ntrdma_debugfs_init(void);
void ntrdma_debugfs_deinit(void);

void ntrdma_debugfs_dev_add(struct ntrdma_dev *dev);
void ntrdma_debugfs_dev_del(struct ntrdma_dev *dev);
void ntrdma_debugfs_cq_add(struct ntrdma_cq *cq);
void ntrdma_debugfs_cq_del(struct ntrdma_cq *cq);
void ntrdma_debugfs_pd_add(struct ntrdma_pd *pd);
void ntrdma_debugfs_pd_del(struct ntrdma_pd *pd);
void ntrdma_debugfs_mr_add(struct ntrdma_mr *mr);
void ntrdma_debugfs_mr_del(struct ntrdma_mr *mr);
void ntrdma_debugfs_qp_add(struct ntrdma_qp *qp);
void ntrdma_debugfs_qp_del(struct ntrdma_qp *qp);
void ntrdma_debugfs_rpd_add(struct ntrdma_rpd *rpd);
void ntrdma_debugfs_rpd_del(struct ntrdma_rpd *rpd);
void ntrdma_debugfs_rmr_add(struct ntrdma_rmr *rmr);
void ntrdma_debugfs_rmr_del(struct ntrdma_rmr *rmr);
void ntrdma_debugfs_rqp_add(struct ntrdma_rqp *rqp);
void ntrdma_debugfs_rqp_del(struct ntrdma_rqp *rqp);

int ntrdma_dev_init(struct ntrdma_dev *dev,
		      struct ntc_dev *ntc);
void ntrdma_dev_deinit(struct ntrdma_dev *dev);

int ntrdma_dev_hello_init(struct ntrdma_dev *dev, struct ntc_dev *ntc);
void ntrdma_dev_hello_deinit(struct ntrdma_dev *dev);

int ntrdma_dev_ib_init(struct ntrdma_dev *dev);
void ntrdma_dev_ib_deinit(struct ntrdma_dev *dev);

int ntrdma_dev_eth_init(struct ntrdma_dev *dev,
			  u32 vbell_idx, u32 rx_cap);
void ntrdma_dev_eth_deinit(struct ntrdma_dev *dev);

int ntrdma_dev_hello(struct ntrdma_dev *dev, int phase);

int ntrdma_dev_res_init(struct ntrdma_dev *dev);
void ntrdma_dev_res_deinit(struct ntrdma_dev *dev);

int ntrdma_dev_enable(struct ntrdma_dev *dev);
void ntrdma_dev_eth_enable(struct ntrdma_dev *dev);
void ntrdma_dev_cmd_enable(struct ntrdma_dev *dev);
int ntrdma_dev_res_enable(struct ntrdma_dev *dev);

void ntrdma_dev_disable(struct ntrdma_dev *dev);
void ntrdma_dev_eth_disable(struct ntrdma_dev *dev);
void ntrdma_dev_cmd_disable(struct ntrdma_dev *dev);
void ntrdma_dev_res_disable(struct ntrdma_dev *dev);

void ntrdma_dev_quiesce(struct ntrdma_dev *dev);
void ntrdma_dev_reset(struct ntrdma_dev *dev);
void ntrdma_dev_eth_quiesce(struct ntrdma_dev *dev);
void ntrdma_dev_cmd_reset(struct ntrdma_dev *dev);
void ntrdma_dev_cmd_quiesce(struct ntrdma_dev *dev);
void ntrdma_dev_res_reset(struct ntrdma_dev *dev);
void ntrdma_dev_rres_reset(struct ntrdma_dev *dev);
void ntrdma_dev_vbell_reset(struct ntrdma_dev *dev);
void _ntrdma_unrecoverable_err(struct ntrdma_dev *dev, const char *f);
#define ntrdma_unrecoverable_err(_dev) _ntrdma_unrecoverable_err(_dev, __func__)
#endif
