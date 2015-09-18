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

#ifndef NTRDMA_DEBUGFS_H
#define NTRDMA_DEBUGFS_H

struct ntrdma_dev;
struct ntrdma_cq;
struct ntrdma_pd;
struct ntrdma_mr;
struct ntrdma_qp;
struct ntrdma_rpd;
struct ntrdma_rmr;
struct ntrdma_rqp;

#ifdef CONFIG_NTRDMA_DEBUGFS
struct dentry;
void ntrdma_debugfs_init(void);
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
#else
static inline void ntrdma_debugfs_init(void) {}
static inline void ntrdma_debugfs_deinit(void) {}
static inline void ntrdma_debugfs_dev_add(struct ntrdma_dev *dev) {}
static inline void ntrdma_debugfs_dev_del(struct ntrdma_dev *dev) {}
static inline void ntrdma_debugfs_cq_add(struct ntrdma_dev *dev) {}
static inline void ntrdma_debugfs_cq_del(struct ntrdma_dev *dev) {}
static inline void ntrdma_debugfs_pd_add(struct ntrdma_dev *dev) {}
static inline void ntrdma_debugfs_pd_del(struct ntrdma_dev *dev) {}
static inline void ntrdma_debugfs_mr_add(struct ntrdma_dev *dev) {}
static inline void ntrdma_debugfs_mr_del(struct ntrdma_dev *dev) {}
static inline void ntrdma_debugfs_qp_add(struct ntrdma_dev *dev) {}
static inline void ntrdma_debugfs_qp_del(struct ntrdma_dev *dev) {}
static inline void ntrdma_debugfs_rpd_add(struct ntrdma_dev *dev) {}
static inline void ntrdma_debugfs_rpd_del(struct ntrdma_dev *dev) {}
static inline void ntrdma_debugfs_rmr_add(struct ntrdma_dev *dev) {}
static inline void ntrdma_debugfs_rmr_del(struct ntrdma_dev *dev) {}
static inline void ntrdma_debugfs_rqp_add(struct ntrdma_dev *dev) {}
static inline void ntrdma_debugfs_rqp_del(struct ntrdma_dev *dev) {}
#endif

#endif
