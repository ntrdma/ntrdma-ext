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

#include <linux/in.h>
#include <linux/in6.h>
#include <linux/inet.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <net/net_namespace.h>

#include "ntrdma_os.h"
#include "ntrdma_mem.h"
#include "ntrdma_map.h"

#include "ntrdma_dev.h"
#include "ntrdma_cmd.h"
#include "ntrdma_tcp.h"

#define NTRDMA_TCP_CONNECT_DELAY	msecs_to_jiffies(1700)
#define NTRDMA_TCP_ACCEPT_DELAY		msecs_to_jiffies(700)

static struct ntrdma_tcp_dev *ntrdma_tcp_create(char *config, int config_i);
static void ntrdma_tcp_destroy(struct ntrdma_tcp_dev * tdev);
NTRDMA_DECL_DWI_CB(ntrdma_tcp_req_work, ptrhld);
static int ntrdma_tcp_server(void *ctx);
static int ntrdma_tcp_client(void *ctx);

static void ntrdma_tcp_port_enable(struct ntrdma_dev *dev);
static void ntrdma_tcp_port_disable(struct ntrdma_dev *dev);
static struct ntrdma_req *ntrdma_tcp_port_req_alloc(struct ntrdma_dev *dev,
						    int cap_hint);
static void ntrdma_tcp_port_req_free(struct ntrdma_req *req);
static void ntrdma_tcp_port_req_submit(struct ntrdma_req *req);
static void ntrdma_tcp_port_req_memcpy(struct ntrdma_req *req,
				       ntrdma_dma_addr_t dst,
				       ntrdma_dma_addr_t src,
				       ntrdma_size_t len);
static void ntrdma_tcp_port_req_imm32(struct ntrdma_req *req,
				      ntrdma_dma_addr_t dst,
				      ntrdma_u32_t val);
static void ntrdma_tcp_port_req_imm64(struct ntrdma_req *req,
				      ntrdma_dma_addr_t dst,
				      ntrdma_u64_t val);
static void ntrdma_tcp_port_req_signal(struct ntrdma_req *req);
static void ntrdma_tcp_port_req_fence(struct ntrdma_req *req);
static void ntrdma_tcp_port_req_callback(struct ntrdma_req *req,
					 void (*cb)(void *cb_ctx),
					 void *cb_ctx);

static const struct ntrdma_dev_ops ntrdma_tcp_dev_ops = {
	.port_enable			= ntrdma_tcp_port_enable,
	.port_disable			= ntrdma_tcp_port_disable,
	.port_req_alloc			= ntrdma_tcp_port_req_alloc,
	.port_req_free			= ntrdma_tcp_port_req_free,
	.port_req_submit		= ntrdma_tcp_port_req_submit,
	.port_req_memcpy		= ntrdma_tcp_port_req_memcpy,
	.port_req_imm32			= ntrdma_tcp_port_req_imm32,
	.port_req_imm64			= ntrdma_tcp_port_req_imm64,
	.port_req_signal		= ntrdma_tcp_port_req_signal,
	.port_req_fence			= ntrdma_tcp_port_req_fence,
	.port_req_callback		= ntrdma_tcp_port_req_callback,
};

#define NTRDMA_TCP_MAX		10

static struct ntrdma_tcp_dev *ntrdma_tcp_devices[NTRDMA_TCP_MAX];

char *ntrdma_tcp_config[NTRDMA_TCP_MAX];
static int ntrdma_tcp_count;

module_param_array_named(config, ntrdma_tcp_config,
			 charp, &ntrdma_tcp_count, 0440);
MODULE_PARM_DESC(config, "[s|c]:<ip>:<port>[,...]");

static struct ntrdma_tcp_dev *ntrdma_tcp_create(char *config, int config_i)
{
	struct ntrdma_tcp_dev *tdev;
	const char *pos = config;
	int rc;

	u16 port;
	union {
		struct sockaddr *p;
		struct sockaddr_in *in;
		struct sockaddr_in6 *in6;
	} saddr;

	tdev = (void *)ib_alloc_device(sizeof(*tdev));
	if (!tdev)
		goto err_tdev;

	tdev->dev.node = NUMA_NO_NODE;

	tdev->dev.ops = &ntrdma_tcp_dev_ops;

	ntrdma_list_init(&tdev->req_list);
	ntrdma_spl_create(&tdev->req_lock, "req_lock");
	ntrdma_dwi_create(&tdev->req_work, "req_work",
			  ntrdma_tcp_req_work, tdev);

	pos = skip_spaces(pos);
	switch (*pos++) {
	case 'c':
		tdev->task_fn = ntrdma_tcp_client;
		break;
	case 's':
		tdev->task_fn = ntrdma_tcp_server;
		break;
	default:
		pr_info("failed to parse cs in '%s'\n", config);
		goto err_config;
	}

	pos = skip_spaces(pos);
	if (*pos++ != ':') {
		pr_info("missing separating ':' after cs in '%s'\n", config);
		goto err_config;
	}

	saddr.p = &tdev->saddr;

	pos = skip_spaces(pos);
	if (*pos == '[') {
		saddr.in6->sin6_family = AF_INET6;
		rc = in6_pton(pos + 1, -1,
			      (void *)&saddr.in6->sin6_addr,
			      -1, &pos);
		if (rc != 1) {
			pr_info("failed to parse in6 addr in '%s'\n", config);
			goto err_config;
		}
		if (*pos++ != ']') {
			pr_info("missing trailing ']' in '%s'\n", config);
			goto err_config;
		}
		pos = skip_spaces(pos);
		if (*pos++ != ':') {
			pr_info("missing separating ':' after addr in '%s'\n",
				config);
			goto err_config;
		}
		pos = skip_spaces(pos);
		rc = kstrtou16(pos, 0, &port);
		if (rc) {
			pr_info("failed to parse port in '%s'\n", config);
			goto err_config;
		}
		saddr.in6->sin6_port = cpu_to_be16(port);
	} else {
		saddr.in->sin_family = AF_INET;
		rc = in4_pton(pos, -1,
			      (void *)&saddr.in->sin_addr,
			      -1, &pos);
		if (rc != 1) {
			pr_info("failed to parse in4 addr in '%s'\n", config);
			goto err_config;
		}
		pos = skip_spaces(pos);
		if (*pos++ != ':') {
			pr_info("missing separating ':' after addr in '%s'\n",
				config);
			goto err_config;
		}
		pos = skip_spaces(pos);
		rc = kstrtou16(pos, 0, &port);
		if (rc) {
			pr_info("failed to parse port in '%s'\n", config);
			goto err_config;
		}
		saddr.in->sin_port = htons(port);
	}

	tdev->task = kthread_create(tdev->task_fn, tdev, "ntrdma_tcp%d",
				    config_i);
	if (IS_ERR(tdev->task)) {
		pr_info("failed to create task for ntrdma_tcp%d\n", config_i);
		goto err_kthread;
	}
	get_task_struct(tdev->task);

	rc = ntrdma_dev_init(&tdev->dev);
	if (rc) {
		pr_info("failed to register ntrdma_tcp%d\n", config_i);
		goto err_register;
	}

	wake_up_process(tdev->task);

	return tdev;

err_register:
	kthread_stop(tdev->task);
	put_task_struct(tdev->task);
err_kthread:
err_config:
	ib_dealloc_device((void *)tdev);
err_tdev:
	return NULL;
}

static void ntrdma_tcp_destroy(struct ntrdma_tcp_dev * tdev)
{
	send_sig(SIGINT, tdev->task, 1);
	kthread_stop(tdev->task);
	put_task_struct(tdev->task);
	ib_dealloc_device((void *)tdev);
}

static int ntrdma_tcp_send(struct socket *sock, void *buf, size_t len)
{
	struct msghdr msg;
	struct kvec iov;
	int rc;

	if (!sock)
		return -EIO;

	do {
		memset(&msg, 0, sizeof(msg));
		iov.iov_base = buf;
		iov.iov_len = len;

		rc = kernel_sendmsg(sock, &msg, &iov, 1, len);
		if (rc < 0)
			return rc;

		buf += rc;
		len -= rc;
	} while (len > 0);

	return 0;
}

static int ntrdma_tcp_recv(struct socket *sock, void *buf, size_t len)
{
	struct msghdr msg;
	struct kvec iov;
	int rc;

	if (!sock)
		return -EIO;

	do {
		memset(&msg, 0, sizeof(msg));
		iov.iov_base = buf;
		iov.iov_len = len;

		rc = kernel_recvmsg(sock, &msg, &iov, 1, len, 0);
		if (rc < 0)
			return rc;

		buf += rc;
		len -= rc;
	} while(len > 0);

	return 0;
}

static int ntrdma_tcp_send_msg(struct ntrdma_tcp_dev *tdev,
			       struct ntrdma_tcp_req_op *op)
{
	struct socket *sock = tdev->sock;
	void *src;
	int rc;

	ntrdma_vdbg(&tdev->dev, "send msg\n");

	rc = ntrdma_tcp_send(sock, &op->msg, sizeof(op->msg));
	if (rc)
		return rc;

	if (op->msg.flags & NTRDMA_TCP_MSG_SIGNAL)
		ntrdma_vdbg(&tdev->dev, "send signal\n");

	if (op->msg.flags & NTRDMA_TCP_MSG_DATA) {
		ntrdma_vdbg(&tdev->dev,
			    "send data %s %#llx dst %#llx len %#x\n",
			    (op->msg.flags & NTRDMA_TCP_MSG_IMM ?
			     "imm" : "src"),
			    op->data_src,
			    op->msg.data_dst,
			    op->msg.data_len);

		if (op->msg.flags & NTRDMA_TCP_MSG_IMM)
			src = &op->data_src;
		else
			src = (void *)(unsigned long)op->data_src;

		rc = ntrdma_tcp_send(sock, src, op->msg.data_len);
		if (rc) {
			ntrdma_dbg(&tdev->dev, "tcp send data failed\n");
			return rc;
		}
	}

	return 0;
}

static int ntrdma_tcp_recv_msg(struct ntrdma_tcp_dev *tdev)
{
	struct socket *sock = tdev->sock;
	struct ntrdma_tcp_msg msg;
	void *dst;
	int rc;

	rc = ntrdma_tcp_recv(sock, &msg, sizeof(msg));
	if (rc)
		return rc;

	ntrdma_vdbg(&tdev->dev, "recv msg\n");

	if (msg.start_tag != NTRDMA_TCP_MSG_TAG)
		return -EIO;
	if (msg.end_tag != NTRDMA_TCP_MSG_TAG)
		return -EIO;

	ntrdma_vdbg(&tdev->dev, "recv tags verified\n");

	/* TODO: pat the watchdog */

	if (msg.flags & NTRDMA_TCP_MSG_SIGNAL) {
		ntrdma_vdbg(&tdev->dev, "recv signal\n");

		ntrdma_dev_vbell_event(&tdev->dev);
	}

	if (msg.flags & NTRDMA_TCP_MSG_DATA) {
		ntrdma_vdbg(&tdev->dev, "recv data dst %#llx len %#x\n",
			    msg.data_dst, msg.data_len);


		dst = (void *)(unsigned long)msg.data_dst;
		rc = ntrdma_tcp_recv(sock, dst, msg.data_len);
		if (rc) {
			ntrdma_dbg(&tdev->dev, "tcp recv data failed\n");
			return rc;
		}
	}

	return 0;
}

static int ntrdma_tcp_hello(struct ntrdma_tcp_dev *tdev, struct socket *sock)
{
	union {
		struct ntrdma_tcp_init init;
		struct ntrdma_tcp_conf conf;
	} u;
	int rc;

	memset(&u.init, 0, sizeof(u.init));

	pr_info("hello %pISpc\n", &tdev->saddr);

	u.init.start_tag = NTRDMA_TCP_INFO_TAG;

	u.init.vbell_dma = tdev->dev.vbell_buf_dma;
	u.init.vbell_flags = 0;
	u.init.vbell_count = tdev->dev.vbell_count;

	u.init.cmd_send_cap = tdev->dev.cmd_send_cap;
	u.init.cmd_send_idx = *tdev->dev.cmd_send_cons_buf;
	u.init.cmd_send_rsp_buf_dma = tdev->dev.cmd_send_rsp_buf_dma;
	u.init.cmd_send_cons_dma = tdev->dev.cmd_send_rsp_buf_dma +
		tdev->dev.cmd_send_cap * sizeof(union ntrdma_rsp);
	u.init.cmd_send_vbell_idx = tdev->dev.cmd_send_vbell_idx;
	u.init.cmd_recv_vbell_idx = tdev->dev.cmd_recv_vbell_idx;

#ifdef CONFIG_NTRDMA_ETH
	{
		struct ntrdma_eth_conf_attr attr;

		ntrdma_dev_eth_conf_attr(&tdev->dev, &attr);

		u.init.eth_rx_cap = attr.cap;
		u.init.eth_rx_idx = attr.idx;
		u.init.eth_rx_cqe_buf_dma = attr.buf_dma;
		u.init.eth_rx_cons_buf_dma = attr.idx_dma;
		u.init.eth_rx_vbell_idx = attr.vbell_idx;
	}
#else
	u.init.eth_rx_cap = 0;
	u.init.eth_rx_idx = 0;
	u.init.eth_rx_cqe_buf_dma = 0;
	u.init.eth_rx_cons_buf_dma = 0;
	u.init.eth_rx_vbell_idx = 0;
#endif

	u.init.end_tag = NTRDMA_TCP_INFO_TAG;

	pr_info("hello %pISpc sendmsg\n", &tdev->saddr);
	rc = ntrdma_tcp_send(sock, &u.init, sizeof(u.init));
	if (rc)
		return rc;

	memset(&u.init, 0, sizeof(u.init));

	pr_info("hello %pISpc recvmsg\n", &tdev->saddr);
	rc = ntrdma_tcp_recv(sock, &u.init, sizeof(u.init));
	if (rc)
		return rc;

	if (u.init.start_tag != NTRDMA_TCP_INFO_TAG)
		return -EINVAL;
	if (u.init.end_tag != NTRDMA_TCP_INFO_TAG)
		return -EINVAL;

	pr_info("hello %pISpc tags verified\n", &tdev->saddr);

	ntrdma_dev_vbell_enable(&tdev->dev,
				u.init.vbell_dma, u.init.vbell_count);

	ntrdma_dev_cmd_conf(&tdev->dev,
			    u.init.cmd_send_rsp_buf_dma,
			    u.init.cmd_send_cons_dma,
			    u.init.cmd_send_vbell_idx,
			    u.init.cmd_recv_vbell_idx,
			    u.init.cmd_send_cap,
			    u.init.cmd_send_idx);

#ifdef CONFIG_NTRDMA_ETH
	{
		struct ntrdma_eth_conf_attr attr;

		attr.idx = u.init.eth_rx_idx;
		attr.cap = u.init.eth_rx_cap;
		attr.buf_dma = u.init.eth_rx_cqe_buf_dma;
		attr.idx_dma = u.init.eth_rx_cons_buf_dma;
		attr.vbell_idx = u.init.eth_rx_vbell_idx;

		ntrdma_dev_eth_conf(&tdev->dev, &attr);
	}
#endif

	memset(&u.conf, 0, sizeof(u.conf));

	u.conf.start_tag = NTRDMA_TCP_INFO_TAG;

	u.conf.cmd_recv_buf_dma = tdev->dev.cmd_recv_buf_dma;
	u.conf.cmd_recv_prod_dma = tdev->dev.cmd_recv_buf_dma +
		tdev->dev.cmd_recv_cap * sizeof(union ntrdma_cmd);

#ifdef CONFIG_NTRDMA_ETH
	{
		struct ntrdma_eth_enable_attr attr;

		ntrdma_dev_eth_enable_attr(&tdev->dev, &attr);

		u.conf.eth_tx_wqe_buf_dma = attr.buf_dma;
		u.conf.eth_tx_prod_buf_dma = attr.idx_dma;
		u.conf.eth_tx_vbell_idx = attr.vbell_idx;
	}
#else
	u.conf.eth_recv_buf_dma = 0;
	u.conf.eth_recv_prod_dma = 0;
#endif

	u.conf.end_tag = NTRDMA_TCP_INFO_TAG;

	pr_info("hello %pISpc sendmsg\n", &tdev->saddr);
	rc = ntrdma_tcp_send(sock, &u.conf, sizeof(u.conf));
	if (rc)
		return rc;

	memset(&u.conf, 0, sizeof(u.conf));

	pr_info("hello %pISpc recvmsg\n", &tdev->saddr);
	rc = ntrdma_tcp_recv(sock, &u.conf, sizeof(u.conf));
	if (rc)
		return rc;

	if (u.conf.start_tag != NTRDMA_TCP_INFO_TAG)
		return -EINVAL;
	if (u.conf.end_tag != NTRDMA_TCP_INFO_TAG)
		return -EINVAL;

	ntrdma_dev_cmd_enable(&tdev->dev,
			      u.conf.cmd_recv_buf_dma,
			      u.conf.cmd_recv_prod_dma);

#ifdef CONFIG_NTRDMA_ETH
	{
		struct ntrdma_eth_enable_attr attr;

		attr.buf_dma = u.conf.eth_tx_wqe_buf_dma;
		attr.idx_dma = u.conf.eth_tx_prod_buf_dma;
		attr.vbell_idx = u.conf.eth_tx_vbell_idx;

		ntrdma_dev_eth_enable(&tdev->dev, &attr);
	}
#endif
	pr_info("hello %pISpc tags verified\n", &tdev->saddr);

	return 0;
}

static int ntrdma_tcp_process(struct ntrdma_tcp_dev *tdev,
			      struct socket *sock)
{
	int rc;

	rc = ntrdma_tcp_hello(tdev, sock);
	if (rc)
		return rc;

	tdev->sock = sock;
	ntrdma_dev_enable(&tdev->dev);

	while (!kthread_should_stop()) {
		rc = ntrdma_tcp_recv_msg(tdev);
		if (rc)
			return rc;
	}

	ntrdma_dev_disable(&tdev->dev);
	tdev->sock = NULL;

	return 0;
}

NTRDMA_DECL_DWI_CB(ntrdma_tcp_req_work, ptrhld)
{
	struct ntrdma_tcp_dev *tdev = NTRDMA_CAST_DWI_CTX(ptrhld);
	struct ntrdma_tcp_req *treq;
	union {
		struct ntrdma_tcp_req_op *op;
		struct ntrdma_dma_cb *cb;
	} u;
	int rc;

	ntrdma_spl_lock(&tdev->req_lock);
	treq = ntrdma_list_remove_first_entry(&tdev->req_list,
					      struct ntrdma_tcp_req, entry);
	ntrdma_spl_unlock(&tdev->req_lock);

	while(treq) {
		ntrdma_list_for_each_entry(u.op, &treq->op_list,
					   struct ntrdma_tcp_req_op, entry) {
			rc = ntrdma_tcp_send_msg(tdev, u.op);
			if (rc)
				break;
		}

		ntrdma_list_for_each_entry(u.cb, &treq->cb_list,
					   struct ntrdma_dma_cb, entry)
			ntrdma_cb_call(&u.cb->cb);

		ntrdma_spl_lock(&tdev->req_lock);
		treq = ntrdma_list_remove_first_entry(&tdev->req_list,
						      struct ntrdma_tcp_req,
						      entry);
		ntrdma_spl_unlock(&tdev->req_lock);
	}
}

static int ntrdma_tcp_server(void *ctx)
{
	struct ntrdma_tcp_dev *tdev = ctx;
	struct socket *listen_sock, *sock;
	int rc;

	int reps = 3; /* TODO: delme */

	pr_info("server %pISpc\n", &tdev->saddr);

	pr_info("server %pISpc create\n", &tdev->saddr);
	rc = sock_create_kern(&init_net, tdev->saddr.sa_family,
			      SOCK_STREAM, IPPROTO_TCP, &listen_sock);
	if (rc)
		goto err_sock;

	pr_info("server %pISpc bind\n", &tdev->saddr);
	rc = kernel_bind(listen_sock, &tdev->saddr, sizeof(tdev->saddr));
	if (rc)
		goto err_bind;

	pr_info("server %pISpc listen\n", &tdev->saddr);
	rc = kernel_listen(listen_sock, 1);
	if (rc)
		goto err_bind;

	while (!kthread_should_stop()) {

		if (!reps) /* TODO: delme */
			break;
		--reps;

		pr_info("server %pISpc accept\n", &tdev->saddr);
		rc = kernel_accept(listen_sock, &sock, SOCK_NONBLOCK);
		while (rc == -EAGAIN && !kthread_should_stop()) {
			schedule_timeout_interruptible(NTRDMA_TCP_ACCEPT_DELAY);
			rc = kernel_accept(listen_sock, &sock, SOCK_NONBLOCK);
		}
		if (rc)
			goto err_bind;

		pr_info("server %pISpc process\n", &tdev->saddr);
		rc = ntrdma_tcp_process(tdev, sock);
		if (rc)
			goto err_mesg;

		pr_info("server %pISpc shutdown\n", &tdev->saddr);
		kernel_sock_shutdown(sock, SHUT_RDWR);
		sock_release(sock);
	}

	sock_release(listen_sock);

	pr_info("server %pISpc exiting normally\n", &tdev->saddr);

	return 0;

err_mesg:
	kernel_sock_shutdown(sock, SHUT_RDWR);
	sock_release(sock);
err_bind:
	sock_release(listen_sock);
err_sock:
	pr_info("server %pISpc exiting rc:%d\n", &tdev->saddr, rc);
	return rc;
}

static int ntrdma_tcp_client(void *ctx)
{
	struct ntrdma_tcp_dev *tdev = ctx;
	struct socket *sock;
	int rc = 0;

	int reps = 3; /* TODO: delme */

	pr_info("client %pISpc\n", &tdev->saddr);

	while (!kthread_should_stop()) {

		if (!reps) /* TODO: delme */
			break;
		--reps;

		pr_info("client %pISpc create\n", &tdev->saddr);
		rc = sock_create_kern(&init_net, tdev->saddr.sa_family,
				      SOCK_STREAM, IPPROTO_TCP, &sock);
		if (rc)
			goto err_sock;

		pr_info("client %pISpc connect\n", &tdev->saddr);
		rc = kernel_connect(sock, &tdev->saddr,
				    sizeof(tdev->saddr), 0);
		while (rc == -ECONNREFUSED && !kthread_should_stop()) {
			schedule_timeout_interruptible(NTRDMA_TCP_CONNECT_DELAY);
			rc = kernel_connect(sock, &tdev->saddr,
					    sizeof(tdev->saddr), 0);
		}
		if (rc)
			goto err_conn;

		pr_info("client %pISpc process\n", &tdev->saddr);
		rc = ntrdma_tcp_process(tdev, sock);
		if (rc)
			goto err_mesg;

		pr_info("client %pISpc shutdown\n", &tdev->saddr);
		kernel_sock_shutdown(sock, SHUT_RDWR);
		sock_release(sock);
	}

	pr_info("client %pISpc exiting normally\n", &tdev->saddr);

	return 0;

err_mesg:
	kernel_sock_shutdown(sock, SHUT_RDWR);
err_conn:
	sock_release(sock);
err_sock:
	pr_info("client %pISpc exiting rc:%d\n", &tdev->saddr, rc);
	return rc;
}

int ntrdma_tcp_probe(char *conn_str)
{
	return 0;
}

static void ntrdma_tcp_port_enable(struct ntrdma_dev *dev)
{
	pr_debug("not implemented\n");
}

static void ntrdma_tcp_port_disable(struct ntrdma_dev *dev)
{
	pr_debug("not implemented\n");
}

static struct ntrdma_req *ntrdma_tcp_port_req_alloc(struct ntrdma_dev *dev,
						    int cap_hint)
{
	struct ntrdma_tcp_req *treq;

	treq = ntrdma_malloc(sizeof(*treq), dev->node);
	if (!treq)
		return NULL;

	treq->req.dev = dev;
	treq->tdev = dev_tdev(dev);
	ntrdma_list_init(&treq->imm_list);
	ntrdma_list_init(&treq->op_list);
	ntrdma_list_init(&treq->cb_list);

	return &treq->req;
}

static void ntrdma_tcp_port_req_free(struct ntrdma_req *req)
{
	struct ntrdma_tcp_req *treq = req_treq(req);
	union {
		struct ntrdma_tcp_req_imm *imm;
		struct ntrdma_tcp_req_op *op;
	} u;

	while ((u.imm = ntrdma_list_remove_first_entry(&treq->imm_list,
						       struct ntrdma_tcp_req_imm,
						       entry)))
		ntrdma_free_coherent(ntrdma_port_device(req->dev),
				     sizeof(*u.imm) + u.imm->len,
				     u.imm, u.imm->dma);

	while ((u.op = ntrdma_list_remove_first_entry(&treq->op_list,
						      struct ntrdma_tcp_req_op,
						      entry)))
		ntrdma_free(u.op);

	ntrdma_free(treq);
}

static void ntrdma_tcp_port_req_submit(struct ntrdma_req *req)
{
	struct ntrdma_tcp_req *treq = req_treq(req);
	struct ntrdma_tcp_dev *tdev = treq->tdev;

	ntrdma_spl_lock(&tdev->req_lock);
	{
		ntrdma_list_add_tail(&tdev->req_list, &treq->entry);
		ntrdma_dwi_fire(&tdev->req_work);
	}
	ntrdma_spl_unlock(&tdev->req_lock);
}

static void ntrdma_tcp_port_req_memcpy(struct ntrdma_req *req,
				       ntrdma_dma_addr_t dst,
				       ntrdma_dma_addr_t src,
				       ntrdma_size_t len)
{
	struct ntrdma_tcp_req *treq = req_treq(req);
	struct ntrdma_tcp_req_op *op;

	op = ntrdma_malloc(sizeof(*op), req->dev->node);
	if (!op)
		return;

	op->msg.start_tag = NTRDMA_TCP_MSG_TAG;
	op->msg.end_tag = NTRDMA_TCP_MSG_TAG;

	op->msg.flags = NTRDMA_TCP_MSG_DATA;
	op->msg.data_len = len;
	op->msg.data_dst = dst;
	op->data_src = src;

	ntrdma_list_add_tail(&treq->op_list, &op->entry);
}

static void ntrdma_tcp_port_req_imm32(struct ntrdma_req *req,
				      ntrdma_dma_addr_t dst,
				      ntrdma_u32_t val)
{
	struct ntrdma_tcp_req *treq = req_treq(req);
	struct ntrdma_tcp_req_op *op;

	op = ntrdma_malloc(sizeof(*op), req->dev->node);
	if (!op)
		return;

	op->msg.start_tag = NTRDMA_TCP_MSG_TAG;
	op->msg.end_tag = NTRDMA_TCP_MSG_TAG;

	op->msg.flags = NTRDMA_TCP_MSG_DATA | NTRDMA_TCP_MSG_IMM;
	op->msg.data_len = sizeof(val);
	op->msg.data_dst = dst;
	op->data_src = val;

	ntrdma_list_add_tail(&treq->op_list, &op->entry);
}

static void ntrdma_tcp_port_req_imm64(struct ntrdma_req *req,
				      ntrdma_dma_addr_t dst,
				      ntrdma_u64_t val)
{
	struct ntrdma_tcp_req *treq = req_treq(req);
	struct ntrdma_tcp_req_op *op;

	op = ntrdma_malloc(sizeof(*op), req->dev->node);
	if (!op)
		return;

	op->msg.start_tag = NTRDMA_TCP_MSG_TAG;
	op->msg.end_tag = NTRDMA_TCP_MSG_TAG;

	op->msg.flags = NTRDMA_TCP_MSG_DATA | NTRDMA_TCP_MSG_IMM;
	op->msg.data_len = sizeof(val);
	op->msg.data_dst = dst;
	op->data_src = val;

	ntrdma_list_add_tail(&treq->op_list, &op->entry);
}

static void ntrdma_tcp_port_req_signal(struct ntrdma_req *req)
{
	struct ntrdma_tcp_req *treq = req_treq(req);
	struct ntrdma_tcp_req_op *op;

	op = ntrdma_malloc(sizeof(*op), req->dev->node);
	if (!op)
		return;

	op->msg.start_tag = NTRDMA_TCP_MSG_TAG;
	op->msg.end_tag = NTRDMA_TCP_MSG_TAG;

	op->msg.flags = NTRDMA_TCP_MSG_SIGNAL;
	op->msg.data_len = 0;
	op->msg.data_dst = 0;
	op->data_src = 0;

	ntrdma_list_add_tail(&treq->op_list, &op->entry);
}

static void ntrdma_tcp_port_req_fence(struct ntrdma_req *req)
{
	/* The TCP stream provides in order delivery. */
}

static void ntrdma_tcp_port_req_callback(struct ntrdma_req *req,
					 void (*cb_fn)(void *cb_ctx),
					 void *cb_ctx)
{
	struct ntrdma_tcp_req *treq = req_treq(req);
	struct ntrdma_dma_cb *dma_cb;

	dma_cb = ntrdma_malloc(sizeof(*dma_cb), req->dev->node);
	if (WARN_ON(!dma_cb))
		return;

	ntrdma_cb_init(&dma_cb->cb, cb_fn, cb_ctx);

	ntrdma_list_add_tail(&treq->cb_list, &dma_cb->entry);
}

static int __init ntrdma_tcp_init(void)
{
	int i;

	ntrdma_debugfs_init();

	if (ntrdma_tcp_count > NTRDMA_TCP_MAX)
		ntrdma_tcp_count = NTRDMA_TCP_MAX;

	for (i = 0; i < ntrdma_tcp_count; ++i)
		ntrdma_tcp_devices[i] = ntrdma_tcp_create(ntrdma_tcp_config[i],
							  i);

	return 0;
}
module_init(ntrdma_tcp_init);

static void __exit ntrdma_tcp_exit(void)
{
	int i;

	for (i = 0; i < ntrdma_tcp_count; ++i)
		if (ntrdma_tcp_devices[i])
			ntrdma_tcp_destroy(ntrdma_tcp_devices[i]);

	ntrdma_debugfs_deinit();
}
module_exit(ntrdma_tcp_exit);

MODULE_LICENSE("Dual BSD/GPL");
