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

#include <linux/debugfs.h>
#include <linux/cpumask.h>

#include "ntrdma_dev.h"
#include "ntrdma_eth.h"
#include "ntrdma_cq.h"
#include "ntrdma_mr.h"
#include "ntrdma_qp.h"
#include "ntrdma_wr.h"

#define NTRDMA_DEBUGFS_NAME_MAX 0x20

static const struct file_operations ntrdma_debugfs_dev_info_ops;
static const struct file_operations ntrdma_debugfs_dev_perf_ops;
static const struct file_operations ntrdma_debugfs_dev_cmd_send_ops;
static const struct file_operations ntrdma_debugfs_dev_cmd_send_rsp_ops;
static const struct file_operations ntrdma_debugfs_dev_cmd_recv_ops;
static const struct file_operations ntrdma_debugfs_dev_cmd_recv_rsp_ops;
static const struct file_operations ntrdma_debugfs_dev_vbell_ops;
static const struct file_operations ntrdma_debugfs_cq_info_ops;
static const struct file_operations ntrdma_debugfs_mr_info_ops;
static const struct file_operations ntrdma_debugfs_qp_info_ops;
static const struct file_operations ntrdma_debugfs_qp_send_wqe_ops;
static const struct file_operations ntrdma_debugfs_qp_send_cqe_ops;
static const struct file_operations ntrdma_debugfs_qp_recv_wqe_ops;
static const struct file_operations ntrdma_debugfs_qp_recv_cqe_ops;
static const struct file_operations ntrdma_debugfs_rmr_info_ops;
static const struct file_operations ntrdma_debugfs_rqp_info_ops;
static const struct file_operations ntrdma_debugfs_rqp_send_wqe_ops;
static const struct file_operations ntrdma_debugfs_rqp_send_cqe_ops;
static const struct file_operations ntrdma_debugfs_rqp_recv_wqe_ops;
static const struct file_operations ntrdma_debugfs_dev_vbell_peer_ops;

static struct dentry *debug;
DECLARE_PER_CPU(struct ntrdma_dev_counters, dev_cnt);

static inline
void ntrdma_debugfs_print_rmr_sg_list(struct seq_file *s, const char *pre,
				struct ntc_remote_buf *sg_list,
				u32 sg_count)
{
	u32 i;

	for (i = 0; i < sg_count; ++i)
		seq_printf(s,
			"%ssg_list[%u] dma_addr %#llx size %#llx\n",
			pre, i,
			sg_list[i].dma_addr, sg_list[i].size);
}

static inline
void ntrdma_debugfs_print_wr_snd_sg_list(struct seq_file *s, const char *pre,
					const struct ntrdma_wr_snd_sge *sg_list,
					u32 sg_count)
{
	u32 i;

	for (i = 0; i < sg_count; ++i)
		seq_printf(s, "%ssnd_sg_list[%u] addr %#llx len %#x key %u\n",
			   pre, i, sg_list[i].addr, sg_list[i].len, sg_list[i].key);
}

static inline
void ntrdma_debugfs_print_wr_rcv_sg_list(struct seq_file *s, const char *pre,
					const struct ntrdma_wr_rcv_sge *sg_list,
					u32 sg_count)
{
	const struct ntrdma_wr_rcv_sge *sge;
	struct ntrdma_wr_rcv_sge_shadow *shadow;
	u32 i;

	for (i = 0; i < sg_count; ++i) {
		sge = &sg_list[i];
		shadow = sge->shadow;

		if (!shadow)
			seq_printf(s,
				"%srcv_sg_list[%u] addr %#llx len %#x key %u\n",
				pre, i, sge->addr, sge->len, sge->key);
		else
			seq_printf(s,
				"%srcv_sg_list[%u] exp_buf_desc.size %#llx\n",
				pre, i, sge->exp_buf_desc.size);
	}
}

static inline
void ntrdma_debugfs_print_recv_wqe(struct seq_file *s, const char *pre,
				const struct ntrdma_recv_wqe *wqe, u32 wqe_i)
{
	seq_printf(s, "%srecv_wqe[%u] ulp %#llx code %#x stat %u sg_count %u\n",
		   pre, wqe_i, wqe->ulp_handle, wqe->op_code,
		   wqe->op_status, wqe->sg_count);
}

static inline
void ntrdma_debugfs_print_send_wqe(struct seq_file *s, const char *pre,
				const struct ntrdma_send_wqe *wqe, u32 wqe_i)
{
	seq_printf(s, "%ssend_wqe[%u] ulp %#llx code %#x stat %u\n",
		   pre, wqe_i, wqe->ulp_handle, wqe->op_code,
		   wqe->op_status);
	seq_printf(s, "%s\trecv %u rkey %u addr %#llx imm %#x sg_count %u\n",
		   pre, wqe->recv_key, wqe->rdma_key, wqe->rdma_addr,
		   wqe->imm_data, wqe->sg_count);
}

static inline void ntrdma_debugfs_print_cqe(struct seq_file *s,
					const char *pre,
					const struct ntrdma_cqe *cqe, u32 cqe_i)
{
	seq_printf(s, "%scqe[%u] ulp %#llx code %#x stat %u\n",
		   pre, cqe_i, cqe->ulp_handle, cqe->op_code,
		   cqe->op_status);
	seq_printf(s, "%s\tlen %#x imm %#x\n",
		   pre, cqe->rdma_len, cqe->imm_data);
}

void ntrdma_debugfs_init(void)
{
	if (debugfs_initialized())
		debug = debugfs_create_dir(KBUILD_MODNAME, NULL);
}

void ntrdma_debugfs_deinit(void)
{
	debugfs_remove_recursive(debug);
}

void ntrdma_debugfs_dev_add(struct ntrdma_dev *dev)
{
	if (!debug) {
		dev->debug = NULL;
		return;
	}

	dev->debug = debugfs_create_dir(dev_name(&dev->ibdev.dev), debug);
	if (!dev->debug)
		return;

	debugfs_create_file("info", 0400, dev->debug,
			    dev, &ntrdma_debugfs_dev_info_ops);
	debugfs_create_file("perf", 0400, dev->debug,
			    dev, &ntrdma_debugfs_dev_perf_ops);
	debugfs_create_file("cmd_send_buf", 0400, dev->debug,
			    dev, &ntrdma_debugfs_dev_cmd_send_ops);
	debugfs_create_file("cmd_send_rsp_buf", 0400, dev->debug,
			    dev, &ntrdma_debugfs_dev_cmd_send_rsp_ops);
	debugfs_create_file("cmd_recv_buf", 0400, dev->debug,
			    dev, &ntrdma_debugfs_dev_cmd_recv_ops);
	debugfs_create_file("cmd_recv_rsp_buf", 0400, dev->debug,
			    dev, &ntrdma_debugfs_dev_cmd_recv_rsp_ops);
	debugfs_create_file("vbell_buf", 0400, dev->debug,
			    dev, &ntrdma_debugfs_dev_vbell_ops);
	debugfs_create_file("vbell_peer_seq", S_IRUSR, dev->debug,
			    dev, &ntrdma_debugfs_dev_vbell_peer_ops);
}

void ntrdma_debugfs_dev_del(struct ntrdma_dev *dev)
{
	debugfs_remove_recursive(dev->debug);
}

void ntrdma_debugfs_cq_add(struct ntrdma_cq *cq)
{
	struct ntrdma_dev *dev = ntrdma_cq_dev(cq);
	char buf[NTRDMA_DEBUGFS_NAME_MAX];

	if (!dev->debug) {
		cq->debug = NULL;
		return;
	}

	snprintf(buf, NTRDMA_DEBUGFS_NAME_MAX, "cq%#llx", (u64)cq);

	cq->debug = debugfs_create_dir(buf, dev->debug);
	if (!cq->debug)
		return;

	debugfs_create_file("info", S_IRUSR, cq->debug,
			    cq, &ntrdma_debugfs_cq_info_ops);
}

void ntrdma_debugfs_cq_del(struct ntrdma_cq *cq)
{
	debugfs_remove_recursive(cq->debug);
}

void ntrdma_debugfs_mr_add(struct ntrdma_mr *mr)
{
	struct ntrdma_dev *dev = ntrdma_mr_dev(mr);
	char buf[NTRDMA_DEBUGFS_NAME_MAX];

	if (!dev->debug) {
		mr->debug = NULL;
		return;
	}

	snprintf(buf, NTRDMA_DEBUGFS_NAME_MAX, "mr%d", mr->res.key);

	mr->debug = debugfs_create_dir(buf, dev->debug);
	if (!mr->debug)
		return;

	debugfs_create_file("info", S_IRUSR, mr->debug,
			    mr, &ntrdma_debugfs_mr_info_ops);
}

void ntrdma_debugfs_mr_del(struct ntrdma_mr *mr)
{
	debugfs_remove_recursive(mr->debug);
}

void ntrdma_debugfs_rmr_add(struct ntrdma_rmr *rmr)
{
	struct ntrdma_dev *dev = ntrdma_rmr_dev(rmr);
	char buf[NTRDMA_DEBUGFS_NAME_MAX];

	if (!dev->debug) {
		rmr->debug = NULL;
		return;
	}

	snprintf(buf, NTRDMA_DEBUGFS_NAME_MAX, "rmr%d", rmr->rres.key);

	rmr->debug = debugfs_create_dir(buf, dev->debug);
	if (!rmr->debug)
		return;

	debugfs_create_file("info", S_IRUSR, rmr->debug,
			    rmr, &ntrdma_debugfs_rmr_info_ops);
}

void ntrdma_debugfs_rmr_del(struct ntrdma_rmr *rmr)
{
	debugfs_remove_recursive(rmr->debug);
}

void ntrdma_debugfs_qp_add(struct ntrdma_qp *qp)
{
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);
	char buf[NTRDMA_DEBUGFS_NAME_MAX];

	if (!dev->debug) {
		qp->debug = NULL;
		return;
	}

	snprintf(buf, NTRDMA_DEBUGFS_NAME_MAX, "qp%d", qp->res.key);

	qp->debug = debugfs_create_dir(buf, dev->debug);
	if (!qp->debug)
		return;

	debugfs_create_file("info", S_IRUSR, qp->debug,
			    qp, &ntrdma_debugfs_qp_info_ops);
	debugfs_create_file("send_wqe_buf", S_IRUSR, qp->debug,
			    qp, &ntrdma_debugfs_qp_send_wqe_ops);
	debugfs_create_file("send_cqe_buf", S_IRUSR, qp->debug,
			    qp, &ntrdma_debugfs_qp_send_cqe_ops);
	debugfs_create_file("recv_wqe_buf", S_IRUSR, qp->debug,
			    qp, &ntrdma_debugfs_qp_recv_wqe_ops);
	debugfs_create_file("recv_cqe_buf", S_IRUSR, qp->debug,
			    qp, &ntrdma_debugfs_qp_recv_cqe_ops);
}

void ntrdma_debugfs_qp_del(struct ntrdma_qp *qp)
{
	debugfs_remove_recursive(qp->debug);
}

void ntrdma_debugfs_rqp_add(struct ntrdma_rqp *rqp)
{
	struct ntrdma_dev *dev = ntrdma_rqp_dev(rqp);
	char buf[NTRDMA_DEBUGFS_NAME_MAX];

	if (!dev->debug) {
		rqp->debug = NULL;
		return;
	}

	snprintf(buf, NTRDMA_DEBUGFS_NAME_MAX, "rqp%d", rqp->rres.key);

	rqp->debug = debugfs_create_dir(buf, dev->debug);
	if (!rqp->debug)
		return;

	debugfs_create_file("info", S_IRUSR, rqp->debug,
			    rqp, &ntrdma_debugfs_rqp_info_ops);
	debugfs_create_file("send_wqe_buf", S_IRUSR, rqp->debug,
			    rqp, &ntrdma_debugfs_rqp_send_wqe_ops);
	debugfs_create_file("send_cqe_buf", S_IRUSR, rqp->debug,
			    rqp, &ntrdma_debugfs_rqp_send_cqe_ops);
	debugfs_create_file("recv_wqe_buf", S_IRUSR, rqp->debug,
			    rqp, &ntrdma_debugfs_rqp_recv_wqe_ops);
}

void ntrdma_debugfs_rqp_del(struct ntrdma_rqp *rqp)
{
	debugfs_remove_recursive(rqp->debug);
}

static int ntrdma_debugfs_dev_info_show(struct seq_file *s, void *v)
{
	struct ntrdma_dev *dev = s->private;
	struct ntrdma_eth *eth = dev->eth;

	seq_printf(s, "vbell_enable %d\n",dev->vbell_enable);
	seq_printf(s, "vbell_count %u\n", dev->vbell_count);
	seq_printf(s, "vbell_start %u\n", dev->vbell_next);
	seq_printf(s, "vbell_next %u\n", dev->vbell_next);
	seq_printf(s, "vbell_buf.dma_addr %#llx\n",
		dev->vbell_buf.dma_addr);
	seq_printf(s, "peer_vbell_buf.dma_addr %#llx\n",
			dev->peer_vbell_buf.dma_addr);
	seq_printf(s, "peer_vbell_count %u\n", dev->peer_vbell_count);

	seq_printf(s, "cmd_ready %d\n", dev->cmd_ready);
	seq_printf(s, "cmd_send_cap %u\n", dev->cmd_send_cap);
	seq_printf(s, "cmd_send_prod %u\n", dev->cmd_send_prod);
	seq_printf(s, "cmd_send_cons %u\n", ntrdma_dev_cmd_send_cons(dev));
	seq_printf(s, "cmd_send_cmpl %u\n", dev->cmd_send_cmpl);

	seq_printf(s, "cmd_send_buf.dma_addr %#llx\n",
		   dev->cmd_send_buf.dma_addr);
	seq_printf(s, "cmd_send_rsp_buf.dma_addr %#llx\n",
		dev->cmd_send_rsp_buf.dma_addr);
	seq_printf(s, "peer_cmd_recv_buf.dma_addr %#llx\n",
		dev->peer_cmd_recv_buf.dma_addr);
	seq_printf(s, "peer_recv_prod_shift %#llx\n",
		dev->peer_recv_prod_shift);
	seq_printf(s, "peer_cmd_recv_vbell_idx %u\n",
		   dev->peer_cmd_recv_vbell_idx);

	seq_printf(s, "cmd_send_vbell_idx %u\n", dev->cmd_send_vbell_idx);
	seq_printf(s, "cmd_send_vbell_seq %u\n", dev->cmd_send_vbell.seq);
	seq_printf(s, "cmd_send_vbell_arm %d\n", dev->cmd_send_vbell.arm);

	seq_printf(s, "cmd_recv_cap %u\n", dev->cmd_recv_cap);
	seq_printf(s, "cmd_recv_prod %u\n", ntrdma_dev_cmd_recv_prod(dev));
	seq_printf(s, "cmd_recv_cons %u\n", dev->cmd_recv_cons);

	seq_printf(s, "cmd_recv_buf.dma_addr %#llx\n",
		dev->cmd_recv_buf.dma_addr);
	seq_printf(s, "cmd_recv_rsp_buf.dma_addr %#llx\n",
		   dev->cmd_recv_rsp_buf.dma_addr);
	seq_printf(s, "peer_cmd_send_rsp_buf.dma_addr %#llx\n",
		dev->peer_cmd_send_rsp_buf.dma_addr);
	seq_printf(s, "peer_send_cons_shift %#llx\n",
		   dev->peer_send_cons_shift);
	seq_printf(s, "peer_cmd_send_vbell_idx %u\n",
		   dev->peer_cmd_send_vbell_idx);

	seq_printf(s, "cmd_recv_vbell_idx %u\n", dev->cmd_recv_vbell_idx);
	seq_printf(s, "cmd_recv_vbell_seq %u\n", dev->cmd_recv_vbell.seq);
	seq_printf(s, "cmd_recv_vbell_arm %d\n", dev->cmd_recv_vbell.arm);

	seq_printf(s, "res_enable %d\n", dev->res_enable);

	seq_printf(s, "eth_enable %d\n", eth->enable);
	seq_printf(s, "eth_ready %d\n", eth->ready);
	seq_printf(s, "eth_link %d\n", eth->link);

	seq_printf(s, "eth_rx_cap %#x\n", eth->rx_cap);
	seq_printf(s, "eth_rx_post %#x\n", eth->rx_post);
	seq_printf(s, "eth_rx_prod %#x\n", eth->rx_prod);
	seq_printf(s, "eth_rx_cmpl %#x\n", eth->rx_cmpl);

	seq_printf(s, "eth_rx_wqe_buf.dma_addr %#llx\n",
		eth->rx_wqe_buf.dma_addr);
	seq_printf(s, "eth_rx_wqe_buf.size %#llx\n", (u64)eth->rx_wqe_buf.size);

	seq_printf(s, "eth_rx_cqe_buf.dma_addr %#llx\n",
		eth->rx_cqe_buf.dma_addr);
	seq_printf(s, "eth_rx_cqe_buf.size %#llx\n", eth->rx_cqe_buf.size);

	seq_printf(s, "eth_rx_cons_buf %#x\n", ntrdma_eth_rx_cons(eth));
	seq_printf(s, "eth_rx_cons_buf.dma_addr %#llx\n",
		eth->rx_cons_buf.dma_addr);

	seq_printf(s, "peer_eth_tx_wqe_buf.dma_addr %#llx\n",
		eth->peer_tx_wqe_buf.dma_addr);
	seq_printf(s, "peer_eth_tx_prod_buf.dma_addr %#llx\n",
		eth->peer_tx_prod_buf.dma_addr);

	seq_printf(s, "eth_tx_cap %#x\n", eth->tx_cap);
	seq_printf(s, "eth_tx_cons %#x\n", eth->tx_cons);
	seq_printf(s, "eth_tx_cmpl %#x\n", eth->tx_cmpl);

	seq_printf(s, "eth_tx_wqe_buf.dma_addr %#llx\n",
		eth->tx_wqe_buf.dma_addr);
	seq_printf(s, "eth_tx_wqe_buf.size %#llx\n", eth->tx_wqe_buf.size);

	seq_printf(s, "eth_tx_cqe_buf.dma_addr %#llx\n",
		eth->tx_cqe_buf.dma_addr);
	seq_printf(s, "eth_tx_cqe_buf.size %#llx\n", (u64)eth->tx_cqe_buf.size);

	seq_printf(s, "eth_tx_prod_buf %#x\n",
		ntrdma_eth_tx_prod(eth));
	seq_printf(s, "eth_tx_prod_buf.dma_addr %#llx\n",
		eth->tx_prod_buf.dma_addr);

	seq_printf(s, "peer_eth_rx_cqe_buf.dma_addr %#llx\n",
		eth->peer_rx_cqe_buf.dma_addr);
	seq_printf(s, "peer_eth_rx_cons_buf.dma_addr %#llx\n",
		eth->peer_rx_cons_buf.dma_addr);

	seq_printf(s, "peer_eth_vbell_idx %#x\n", eth->peer_vbell_idx);
	seq_printf(s, "eth_vbell_idx %u\n", eth->vbell_idx);
	seq_printf(s, "eth_vbell_seq %u\n", eth->vbell.seq);
	seq_printf(s, "eth_vbell_arm %d\n", eth->vbell.arm);

	return 0;
}


static int ntrdma_debugfs_dev_perf_show(struct seq_file *s, void *v)
{
	int i;

	int num_cpus = num_online_cpus();

	for (i = 0; i < num_cpus; i++) {
		seq_printf(s, "*** CPU #%d ***\n", i);
		seq_printf(s, "post_send_bytes %llu\n",
				per_cpu(dev_cnt.post_send_bytes, i));
		seq_printf(s, "post_send_wqes %llu\n",
				per_cpu(dev_cnt.post_send_wqes, i));
		seq_printf(s, "post_send_wqes_signalled %llu\n",
				per_cpu(dev_cnt.post_send_wqes_signalled, i));
		seq_printf(s, "qp_send_work_bytes %llu\n",
				per_cpu(dev_cnt.qp_send_work_bytes, i));
		seq_printf(s, "tx_cqes %llu\n",
				per_cpu(dev_cnt.tx_cqes, i));
		seq_printf(s, "cqes_notified %llu\n",
				per_cpu(dev_cnt.cqes_notified, i));
		seq_printf(s, "cqes_polled %llu\n",
				per_cpu(dev_cnt.cqes_polled, i));
		seq_printf(s, "cqes_armed %llu\n",
				per_cpu(dev_cnt.cqes_armed, i));
		seq_puts(s, "***********\n");
	}

	return 0;
}

static int ntrdma_debugfs_dev_info_open(struct inode *inode, struct file *file)
{
	return single_open(file, ntrdma_debugfs_dev_info_show,
			inode->i_private);
}

static int ntrdma_debugfs_dev_perf_open(struct inode *inode, struct file *file)
{
	return single_open(file, ntrdma_debugfs_dev_perf_show,
			inode->i_private);
}


static const struct file_operations ntrdma_debugfs_dev_info_ops = {
	.open = ntrdma_debugfs_dev_info_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};


static const struct file_operations ntrdma_debugfs_dev_perf_ops = {
	.open = ntrdma_debugfs_dev_perf_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int ntrdma_debugfs_dev_cmd_send_show(struct seq_file *s, void *v)
{
	struct ntrdma_dev *dev = s->private;

	return ntc_local_buf_seq_write(s, &dev->cmd_send_buf);
}

static int ntrdma_debugfs_dev_cmd_send_open(struct inode *inode, struct file *file)
{
	return single_open(file, ntrdma_debugfs_dev_cmd_send_show, inode->i_private);
}

static const struct file_operations ntrdma_debugfs_dev_cmd_send_ops = {
	.open = ntrdma_debugfs_dev_cmd_send_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int ntrdma_debugfs_dev_cmd_send_rsp_show(struct seq_file *s, void *v)
{
	struct ntrdma_dev *dev = s->private;

	return ntc_export_buf_seq_write(s, &dev->cmd_send_rsp_buf);
}

static int ntrdma_debugfs_dev_cmd_send_rsp_open(struct inode *inode, struct file *file)
{
	return single_open(file, ntrdma_debugfs_dev_cmd_send_rsp_show, inode->i_private);
}

static const struct file_operations ntrdma_debugfs_dev_cmd_send_rsp_ops = {
	.open = ntrdma_debugfs_dev_cmd_send_rsp_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int ntrdma_debugfs_dev_cmd_recv_show(struct seq_file *s, void *v)
{
	struct ntrdma_dev *dev = s->private;

	return ntc_export_buf_seq_write(s, &dev->cmd_recv_buf);
}

static int ntrdma_debugfs_dev_cmd_recv_open(struct inode *inode, struct file *file)
{
	return single_open(file, ntrdma_debugfs_dev_cmd_recv_show, inode->i_private);
}

static const struct file_operations ntrdma_debugfs_dev_cmd_recv_ops = {
	.open = ntrdma_debugfs_dev_cmd_recv_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int ntrdma_debugfs_dev_cmd_recv_rsp_show(struct seq_file *s, void *v)
{
	struct ntrdma_dev *dev = s->private;

	return ntc_local_buf_seq_write(s, &dev->cmd_send_buf);
}

static int ntrdma_debugfs_dev_cmd_recv_rsp_open(struct inode *inode, struct file *file)
{
	return single_open(file, ntrdma_debugfs_dev_cmd_recv_rsp_show, inode->i_private);
}

static const struct file_operations ntrdma_debugfs_dev_cmd_recv_rsp_ops = {
	.open = ntrdma_debugfs_dev_cmd_recv_rsp_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int ntrdma_debugfs_dev_vbell_show(struct seq_file *s, void *v)
{
	struct ntrdma_dev *dev = s->private;
	u32 i, count = dev->vbell_count;
	const u32 *vbell_buf;

	if (count > dev->vbell_buf.size / sizeof(u32))
		count = dev->vbell_buf.size / sizeof(u32);

	vbell_buf = ntc_export_buf_const_deref(&dev->vbell_buf,
					0, count * sizeof(u32));
	if (!vbell_buf)
		return 0;

	for (i = 0; i < count; ++i)
		seq_printf(s, "%u: %u\n", i, vbell_buf[i]);

	return 0;
}

static int ntrdma_debugfs_dev_vbell_open(struct inode *inode, struct file *file)
{
	return single_open(file, ntrdma_debugfs_dev_vbell_show, inode->i_private);
}

static const struct file_operations ntrdma_debugfs_dev_vbell_ops = {
	.open = ntrdma_debugfs_dev_vbell_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int ntrdma_debugfs_dev_vbell_peer_show(struct seq_file *s, void *v)
{
	struct ntrdma_dev *dev = s->private;
	u32 i, count = dev->peer_vbell_count;

	for (i = 0; i < count; ++i)
		seq_printf(s, "%u: %u\n", i, dev->vbell_peer_seq[i]);

	return 0;
}

static int ntrdma_debugfs_dev_vbell_peer_open(struct inode *inode, struct file *file)
{
	return single_open(file, ntrdma_debugfs_dev_vbell_peer_show, inode->i_private);
}

static const struct file_operations ntrdma_debugfs_dev_vbell_peer_ops = {
	.open = ntrdma_debugfs_dev_vbell_peer_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int ntrdma_debugfs_cq_info_show(struct seq_file *s, void *v)
{
	struct ntrdma_cq *cq = s->private;

	seq_printf(s, "arm %u\n", cq->arm);
	seq_printf(s, "vbell_arm %u\n", cq->vbell.arm);
	seq_printf(s, "vbell_idx %u\n", cq->vbell_idx);

	return 0;
}

static int ntrdma_debugfs_cq_info_open(struct inode *inode, struct file *file)
{
	return single_open(file, ntrdma_debugfs_cq_info_show, inode->i_private);
}

static const struct file_operations ntrdma_debugfs_cq_info_ops = {
	.open = ntrdma_debugfs_cq_info_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int ntrdma_debugfs_mr_info_show(struct seq_file *s, void *v)
{
	struct ntrdma_mr *mr = s->private;
	u32 i;

	seq_printf(s, "addr %#llx\n", mr->addr);
	seq_printf(s, "len %#llx\n", mr->len);
	seq_printf(s, "access %#x\n", mr->access);
	seq_printf(s, "sg_count %u\n", mr->sg_count);

	for (i = 0; i < mr->sg_count; ++i)
		seq_printf(s, "sg_list[%u] dma_addr %llx len %#llx\n", i,
			mr->sg_list[i].dma_addr,
			mr->sg_list[i].size);

	return 0;
}

static int ntrdma_debugfs_mr_info_open(struct inode *inode, struct file *file)
{
	return single_open(file, ntrdma_debugfs_mr_info_show, inode->i_private);
}

static const struct file_operations ntrdma_debugfs_mr_info_ops = {
	.open = ntrdma_debugfs_mr_info_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int ntrdma_debugfs_rmr_info(struct seq_file *s,
				u64 addr, u64 len,
				u32 access, u32 sg_count,
				struct ntc_remote_buf *sg_list)
{
	seq_printf(s, "addr %#llx\n", addr);
	seq_printf(s, "len %#llx\n", len);
	seq_printf(s, "access %#x\n", access);
	seq_printf(s, "sg_count %u\n", sg_count);
	ntrdma_debugfs_print_rmr_sg_list(s, "",
					sg_list, sg_count);

	return 0;
}

static int ntrdma_debugfs_rmr_info_show(struct seq_file *s, void *v)
{
	struct ntrdma_rmr *rmr = s->private;

	return ntrdma_debugfs_rmr_info(s, rmr->addr, rmr->len, rmr->access,
				rmr->sg_count, rmr->sg_list);
}

static int ntrdma_debugfs_rmr_info_open(struct inode *inode, struct file *file)
{
	return single_open(file, ntrdma_debugfs_rmr_info_show, inode->i_private);
}

static const struct file_operations ntrdma_debugfs_rmr_info_ops = {
	.open = ntrdma_debugfs_rmr_info_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int ntrdma_debugfs_qp_info_show(struct seq_file *s, void *v)
{
	struct ntrdma_qp *qp = s->private;

	seq_printf(s, "state %d\n", atomic_read(&qp->state));
	seq_printf(s, "recv_abort %u\n", qp->recv_abort);
	seq_printf(s, "send_abort %u\n", qp->send_abort);
	seq_printf(s, "rqp_key %u\n", qp->rqp_key);
	seq_printf(s, "send_wqe_sg_cap %u\n", qp->send_wqe_sg_cap);
	seq_printf(s, "recv_wqe_sg_cap %u\n", qp->recv_wqe_sg_cap);
	seq_printf(s, "send_wqe_size %#zx\n", qp->send_wqe_size);
	seq_printf(s, "recv_wqe_size %#zx\n", qp->recv_wqe_size);

	seq_printf(s, "send_cap %u\n", qp->send_cap);
	seq_printf(s, "send_post %u\n", qp->send_post);
	seq_printf(s, "send_prod %u\n", qp->send_prod);
	seq_printf(s, "send_cons %u\n", ntrdma_qp_send_cons(qp));
	seq_printf(s, "send_cmpl %u\n", qp->send_cmpl);
	seq_printf(s, "send_wqe_buf.dma_addr %#llx\n",
		qp->send_wqe_buf.dma_addr);
	seq_printf(s, "send_wqe_buf.size %#llx\n", (u64)qp->send_wqe_buf.size);
	seq_printf(s, "send_cqe_buf.dma_addr %#llx\n",
		qp->send_cqe_buf.dma_addr);
	seq_printf(s, "send_cqe_buf.size %#llx\n", qp->send_cqe_buf.size);
	seq_printf(s, "peer_send_wqe_buf.dma_addr %#llx\n",
		qp->peer_send_wqe_buf.dma_addr);
	seq_printf(s, "peer_send_prod_shift %#llx\n", qp->peer_send_prod_shift);
	seq_printf(s, "peer_send_vbell_idx %u\n", qp->peer_send_vbell_idx);

	seq_printf(s, "recv_cap %u\n", qp->recv_cap);
	seq_printf(s, "recv_post %u\n", qp->recv_post);
	seq_printf(s, "recv_prod %u\n", qp->recv_prod);
	seq_printf(s, "recv_cons %u\n", qp->recv_cons);
	seq_printf(s, "recv_cmpl %u\n", qp->recv_cmpl);
	seq_printf(s, "recv_wqe_buf.dma_addr %#llx\n",
		qp->recv_wqe_buf.dma_addr);
	seq_printf(s, "recv_wqe_buf.size %#llx\n", (u64)qp->recv_wqe_buf.size);
	seq_printf(s, "recv_cqe_buf_size %#zx\n", qp->recv_cqe_buf_size);
	seq_printf(s, "peer_recv_wqe_buf.dma_addr %#llx\n",
		qp->peer_recv_wqe_buf.dma_addr);
	seq_printf(s, "peer_recv_prod_shift %#llx\n", qp->peer_recv_prod_shift);

	return 0;
}

static int ntrdma_debugfs_qp_info_open(struct inode *inode, struct file *file)
{
	return single_open(file, ntrdma_debugfs_qp_info_show, inode->i_private);
}

static const struct file_operations ntrdma_debugfs_qp_info_ops = {
	.open = ntrdma_debugfs_qp_info_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int ntrdma_debugfs_qp_send_wqe_show(struct seq_file *s, void *v)
{
	struct ntrdma_qp *qp = s->private;
	const struct ntrdma_send_wqe *wqe;
	u32 i, count = qp->send_cap;
	u32 sg_count;

	for (i = 0; i < count; ++i) {
		wqe = ntrdma_qp_send_wqe(qp, i);

		ntrdma_debugfs_print_send_wqe(s, "",
					      wqe, i);

		sg_count = min_t(u32, wqe->sg_count, qp->send_wqe_sg_cap);

		ntrdma_debugfs_print_wr_snd_sg_list(s, "\t",
				snd_sg_list(0, wqe), sg_count);
	}

	return 0;
}

static int ntrdma_debugfs_qp_send_wqe_open(struct inode *inode, struct file *file)
{
	return single_open(file, ntrdma_debugfs_qp_send_wqe_show, inode->i_private);
}

static const struct file_operations ntrdma_debugfs_qp_send_wqe_ops = {
	.open = ntrdma_debugfs_qp_send_wqe_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int ntrdma_debugfs_qp_send_cqe_show(struct seq_file *s, void *v)
{
	struct ntrdma_qp *qp = s->private;
	const struct ntrdma_cqe *cqe;
	u32 i, count = qp->send_cap;

	for (i = 0; i < count; ++i) {
		cqe = ntrdma_qp_send_cqe(qp, i);
		ntrdma_debugfs_print_cqe(s, "", cqe, i);
	}

	return 0;
}

static int ntrdma_debugfs_qp_send_cqe_open(struct inode *inode, struct file *file)
{
	return single_open(file, ntrdma_debugfs_qp_send_cqe_show, inode->i_private);
}

static const struct file_operations ntrdma_debugfs_qp_send_cqe_ops = {
	.open = ntrdma_debugfs_qp_send_cqe_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int ntrdma_debugfs_qp_recv_wqe_show(struct seq_file *s, void *v)
{
	struct ntrdma_qp *qp = s->private;
	const struct ntrdma_recv_wqe *wqe;
	u32 i, count = qp->recv_cap;
	u32 sg_count;

	for (i = 0; i < count; ++i) {
		wqe = ntrdma_qp_recv_wqe(qp, i);

		ntrdma_debugfs_print_recv_wqe(s, "",
					      wqe, i);

		sg_count = min_t(u32, wqe->sg_count, qp->recv_wqe_sg_cap);

		ntrdma_debugfs_print_wr_rcv_sg_list(s, "\t",
						wqe->rcv_sg_list, sg_count);
	}

	return 0;
}

static int ntrdma_debugfs_qp_recv_wqe_open(struct inode *inode, struct file *file)
{
	return single_open(file, ntrdma_debugfs_qp_recv_wqe_show, inode->i_private);
}

static const struct file_operations ntrdma_debugfs_qp_recv_wqe_ops = {
	.open = ntrdma_debugfs_qp_recv_wqe_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int ntrdma_debugfs_qp_recv_cqe_show(struct seq_file *s, void *v)
{
	struct ntrdma_qp *qp = s->private;
	const struct ntrdma_cqe *cqe;
	u32 i, count = qp->recv_cap;

	for (i = 0; i < count; ++i) {
		cqe = ntrdma_qp_recv_cqe(qp, i);
		ntrdma_debugfs_print_cqe(s, "", cqe, i);
	}

	return 0;
}

static int ntrdma_debugfs_qp_recv_cqe_open(struct inode *inode, struct file *file)
{
	return single_open(file, ntrdma_debugfs_qp_recv_cqe_show, inode->i_private);
}

static const struct file_operations ntrdma_debugfs_qp_recv_cqe_ops = {
	.open = ntrdma_debugfs_qp_recv_cqe_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int ntrdma_debugfs_rqp_info_show(struct seq_file *s, void *v)
{
	struct ntrdma_rqp *rqp = s->private;

	seq_printf(s, "state %d\n", rqp->state);
	seq_printf(s, "qp_key %u\n", rqp->qp_key);
	seq_printf(s, "send_wqe_sg_cap %u\n", rqp->send_wqe_sg_cap);
	seq_printf(s, "recv_wqe_sg_cap %u\n", rqp->recv_wqe_sg_cap);
	seq_printf(s, "send_wqe_size %#zx\n", rqp->send_wqe_size);
	seq_printf(s, "recv_wqe_size %#zx\n", rqp->recv_wqe_size);

	seq_printf(s, "send_cap %u\n", rqp->send_cap);
	seq_printf(s, "send_prod %u\n", ntrdma_rqp_send_prod(rqp));
	seq_printf(s, "send_cons %u\n", rqp->send_cons);
	seq_printf(s, "send_wqe_buf.dma_addr %#llx\n",
		rqp->send_wqe_buf.dma_addr);
	seq_printf(s, "send_wqe_buf.size %#llx\n", rqp->send_wqe_buf.size);
	seq_printf(s, "send_cqe_buf.dma_addr %#llx\n",
		rqp->send_cqe_buf.dma_addr);
	seq_printf(s, "send_cqe_buf.size %#llx\n", (u64)rqp->send_cqe_buf.size);
	seq_printf(s, "peer_send_cqe_buf.dma_addr %#llx\n",
		rqp->peer_send_cqe_buf.dma_addr);
	seq_printf(s, "peer_send_cons_shift %#llx\n",
		rqp->peer_send_cons_shift);
	seq_printf(s, "peer_cmpl_vbell_idx %u\n", rqp->peer_cmpl_vbell_idx);

	seq_printf(s, "recv_cap %u\n", rqp->recv_cap);
	seq_printf(s, "recv_prod %u\n", ntrdma_rqp_recv_prod(rqp));
	seq_printf(s, "recv_cons %u\n", rqp->recv_cons);
	seq_printf(s, "recv_wqe_buf.dma_addr %#llx\n",
		rqp->recv_wqe_buf.dma_addr);
	seq_printf(s, "recv_wqe_buf.size %#llx\n", rqp->recv_wqe_buf.size);

	seq_printf(s, "send_vbell_arm %u\n", rqp->send_vbell.arm);
	seq_printf(s, "send_vbell_idx %u\n", rqp->send_vbell_idx);

	return 0;
}

static int ntrdma_debugfs_rqp_info_open(struct inode *inode, struct file *file)
{
	return single_open(file, ntrdma_debugfs_rqp_info_show, inode->i_private);
}

static const struct file_operations ntrdma_debugfs_rqp_info_ops = {
	.open = ntrdma_debugfs_rqp_info_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int ntrdma_debugfs_rqp_send_wqe_show(struct seq_file *s, void *v)
{
	struct ntrdma_rqp *rqp = s->private;
	const struct ntrdma_send_wqe *wqe;
	u32 i, count = rqp->send_cap;
	u32 sg_count;

	for (i = 0; i < count; ++i) {
		wqe = ntrdma_rqp_send_wqe(rqp, i);

		ntrdma_debugfs_print_send_wqe(s, "",
					      wqe, i);

		sg_count = min_t(u32, wqe->sg_count, rqp->send_wqe_sg_cap);

		ntrdma_debugfs_print_wr_snd_sg_list(s, "\t",
				snd_sg_list(0, wqe), sg_count);
	}

	return 0;
}

static int ntrdma_debugfs_rqp_send_wqe_open(struct inode *inode, struct file *file)
{
	return single_open(file, ntrdma_debugfs_rqp_send_wqe_show, inode->i_private);
}

static const struct file_operations ntrdma_debugfs_rqp_send_wqe_ops = {
	.open = ntrdma_debugfs_rqp_send_wqe_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int ntrdma_debugfs_rqp_send_cqe_show(struct seq_file *s, void *v)
{
	struct ntrdma_rqp *rqp = s->private;
	const struct ntrdma_cqe *cqe;
	u32 i, count = rqp->send_cap;

	for (i = 0; i < count; ++i) {
		cqe = ntrdma_rqp_send_cqe(rqp, i);
		ntrdma_debugfs_print_cqe(s, "", cqe, i);
	}

	return 0;
}

static int ntrdma_debugfs_rqp_send_cqe_open(struct inode *inode, struct file *file)
{
	return single_open(file, ntrdma_debugfs_rqp_send_cqe_show, inode->i_private);
}

static const struct file_operations ntrdma_debugfs_rqp_send_cqe_ops = {
	.open = ntrdma_debugfs_rqp_send_cqe_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int ntrdma_debugfs_rqp_recv_wqe_show(struct seq_file *s, void *v)
{
	struct ntrdma_rqp *rqp = s->private;
	const struct ntrdma_recv_wqe *wqe;
	u32 i, count = rqp->recv_cap;
	u32 sg_count;

	for (i = 0; i < count; ++i) {
		wqe = ntrdma_rqp_recv_wqe(rqp, i);

		ntrdma_debugfs_print_recv_wqe(s, "",
					      wqe, i);

		sg_count = min_t(u32, wqe->sg_count, rqp->recv_wqe_sg_cap);

		ntrdma_debugfs_print_wr_rcv_sg_list(s, "\t",
						wqe->rcv_sg_list, sg_count);
	}

	return 0;
}

static int ntrdma_debugfs_rqp_recv_wqe_open(struct inode *inode, struct file *file)
{
	return single_open(file, ntrdma_debugfs_rqp_recv_wqe_show, inode->i_private);
}

static const struct file_operations ntrdma_debugfs_rqp_recv_wqe_ops = {
	.open = ntrdma_debugfs_rqp_recv_wqe_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

