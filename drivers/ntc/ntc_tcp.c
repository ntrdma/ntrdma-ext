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

#include <linux/kernel.h>
#include <linux/module.h>

#include <linux/in.h>
#include <linux/in6.h>
#include <linux/inet.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <net/net_namespace.h>
#include <linux/tcp.h>

#include <linux/ntc.h>

#define DRIVER_NAME			"ntc_tcp"
#define DRIVER_DESCRIPTION		"NTC TCP Channel Back End"

#define DRIVER_LICENSE			"Dual BSD/GPL"
#define DRIVER_VERSION			"0.2"
#define DRIVER_RELDATE			"2 October 2015"
#define DRIVER_AUTHOR			"Allen Hubbe <Allen.Hubbe@emc.com>"

MODULE_LICENSE(DRIVER_LICENSE);
MODULE_VERSION(DRIVER_VERSION);
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESCRIPTION);

#define NTC_TCP_CONNECT_DELAY		msecs_to_jiffies(2000)
#define NTC_TCP_ACCEPT_DELAY		msecs_to_jiffies(700)
#define NTC_TCP_MAGIC_INFO		U64_C(0x73c984cd2a6727db)
#define NTC_TCP_MAGIC_MSG		U64_C(0xe4ee6b08e653fa98)

struct ntc_tcp_dev {
	struct ntc_dev			ntc;

	struct task_struct		*task;
	int				(*task_fn)(void *ctx);

	struct socket			*sock;
	struct sockaddr			saddr;

	struct work_struct		req_work;
	struct list_head		op_list;
	spinlock_t			op_lock;
};

#define ntc_tcp_down_cast(__ntc) \
	container_of(__ntc, struct ntc_tcp_dev, ntc)

#define ntc_tcp_of_req_work(__ws) \
	container_of(__ws, struct ntc_tcp_dev, req_work)

struct ntc_tcp_op {
	struct list_head		entry;
	bool				imm;
	u64				dst;
	u64				src;
	u64				len;
	void				(*cb)(void *cb_ctx);
	void				*cb_ctx;
};

struct ntc_tcp_msg {
	u64				magic_start;
	u64				dst;
	u64				len;
	u64				magic_end;
};

static const struct ntc_dev_ops ntc_tcp_dev_ops;

static struct ntc_tcp_dev *ntc_tcp_create(char *config, int config_i);
static void ntc_tcp_destroy(struct ntc_tcp_dev *dev);
static void ntc_tcp_req_work(struct work_struct *ws);
static int ntc_tcp_server(void *ctx);
static int ntc_tcp_client(void *ctx);

#define NTC_TCP_MAX			10

static struct ntc_tcp_dev *ntc_tcp_devices[NTC_TCP_MAX];

char *ntc_tcp_config[NTC_TCP_MAX];
static int ntc_tcp_count;

module_param_array_named(config, ntc_tcp_config,
			 charp, &ntc_tcp_count, 0440);
MODULE_PARM_DESC(config, "[s|c]:<ip>:<port>[,...]");

static int ntc_tcp_nodelay = 1;

module_param_named(nodelay, ntc_tcp_nodelay, int, 0440);
MODULE_PARM_DESC(nodelay, "0|1");

static void ntc_tcp_release(struct device *__dev)
{
	struct ntc_tcp_dev *dev = ntc_tcp_down_cast(ntc_of_dev(__dev));

	send_sig(SIGINT, dev->task, 1);
	kthread_stop(dev->task);
	put_task_struct(dev->task);
	kfree(dev);
}

static struct ntc_tcp_dev *ntc_tcp_create(char *config, int config_i)
{
	struct ntc_tcp_dev *dev;
	const char *pos = config;
	int rc;

	u16 port;
	union {
		struct sockaddr *p;
		struct sockaddr_in *in;
		struct sockaddr_in6 *in6;
	} saddr;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		goto err_dev;

	dev->ntc.dev_ops = &ntc_tcp_dev_ops;
	dev->ntc.map_ops = &ntc_virt_map_ops;

	INIT_WORK(&dev->req_work, ntc_tcp_req_work);
	INIT_LIST_HEAD(&dev->op_list);
	spin_lock_init(&dev->op_lock);

	pos = skip_spaces(pos);
	switch (*pos++) {
	case 'c':
		dev->task_fn = ntc_tcp_client;
		break;
	case 's':
		dev->task_fn = ntc_tcp_server;
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

	saddr.p = &dev->saddr;

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

	dev->task = kthread_create(dev->task_fn, dev, "ntc_tcp%d",
				   config_i);
	if (IS_ERR(dev->task)) {
		pr_info("failed to create task for ntc_tcp%d\n", config_i);
		goto err_kthread;
	}
	get_task_struct(dev->task);

	dev_set_name(&dev->ntc.dev, "ntc_tcp%d", config_i);
	dev->ntc.dev.release = ntc_tcp_release;

	rc = ntc_register_device(&dev->ntc);
	if (rc) {
		pr_info("failed to register ntc_tcp%d\n", config_i);
		goto err_register;
	}

	return dev;

err_register:
	kthread_stop(dev->task);
	put_task_struct(dev->task);
err_kthread:
err_config:
	kfree(dev);
err_dev:
	return NULL;
}

static void ntc_tcp_destroy(struct ntc_tcp_dev *dev)
{
	ntc_unregister_device(&dev->ntc);
}

static int ntc_tcp_send(struct socket *sock, void *buf, size_t len)
{
	struct msghdr msg;
	struct kvec iov;
	int rc;

	if (!sock)
		return -EIO;

	while (len > 0) {
		memset(&msg, 0, sizeof(msg));
		iov.iov_base = buf;
		iov.iov_len = len;

		rc = kernel_sendmsg(sock, &msg, &iov, 1, len);
		if (rc < 0)
			return rc;

		buf += rc;
		len -= rc;
	}

	return 0;
}

static int ntc_tcp_recv(struct socket *sock, void *buf, size_t len)
{
	struct msghdr msg;
	struct kvec iov;
	int rc;

	if (!sock)
		return -EIO;

	while (len > 0) {
		memset(&msg, 0, sizeof(msg));
		iov.iov_base = buf;
		iov.iov_len = len;

		rc = kernel_recvmsg(sock, &msg, &iov, 1, len, 0);
		if (rc < 0)
			return rc;

		buf += rc;
		len -= rc;
	}

	wmb(); /* write data in the order it is copied out of the channel */

	return 0;
}

static int ntc_tcp_send_msg(struct ntc_tcp_dev *dev,
			    struct ntc_tcp_op *op)
{
	struct ntc_tcp_msg msg;
	struct socket *sock = dev->sock;
	void *src;
	int rc;

	msg.magic_start = NTC_TCP_MAGIC_MSG;
	msg.dst = op->dst;
	msg.len = op->len;
	msg.magic_end = NTC_TCP_MAGIC_MSG;

	rc = ntc_tcp_send(sock, &msg, sizeof(msg));
	if (rc)
		return rc;

	if (!op->len)
		return 0;

	if (op->imm)
		src = &op->src;
	else
		src = (void *)(unsigned long)op->src;

	return ntc_tcp_send(sock, src, op->len);
}

static int ntc_tcp_recv_msg(struct ntc_tcp_dev *dev)
{
	struct socket *sock = dev->sock;
	struct ntc_tcp_msg msg;
	void *dst;
	int rc;

	rc = ntc_tcp_recv(sock, &msg, sizeof(msg));
	if (rc)
		return rc;

	if (msg.magic_start != NTC_TCP_MAGIC_MSG)
		return -EIO;
	if (msg.magic_end != NTC_TCP_MAGIC_MSG)
		return -EIO;

	if (!msg.len) {
		ntc_ctx_signal(&dev->ntc, NTB_DEFAULT_VEC);
		return 0;
	}

	dst = (void *)(unsigned long)msg.dst;

	return ntc_tcp_recv(sock, dst, msg.len);
}

#define NTC_TCP_HELLO_MAGIC		0x70cfa228

struct ntc_tcp_hello_msg {
	u32				magic;
	u32				phase;
	u32				data_size;
	u32				next_size;
};

static int ntc_tcp_hello(struct ntc_tcp_dev *dev, struct socket *sock)
{
	struct ntc_tcp_hello_msg msg;
	void *in_buf = NULL, *out_buf = NULL;
	size_t in_size = 0, out_size = 0;
	ssize_t next_size;
	int rc = 0, phase = 0;

	for (;;) {
		dev_dbg(&dev->ntc.dev, "begin phase %d\n", phase);

		dev_dbg(&dev->ntc.dev, "phase %d in_size %#zx out_size %#zx\n",
			phase, in_size, out_size);
		next_size = ntc_ctx_hello(&dev->ntc, phase,
					  in_buf, in_size,
					  out_buf, out_size);
		if (next_size < 0) {
			rc = next_size;
			break;
		}

		dev_dbg(&dev->ntc.dev, "phase %d next_size %#zx\n",
			phase, next_size);

		if (!next_size && !out_size)
			break;

		dev_dbg(&dev->ntc.dev, "phase %d send msg\n", phase);

		msg.magic = NTC_TCP_HELLO_MAGIC;
		msg.phase = phase;
		msg.data_size = out_size;
		msg.next_size = next_size;

		rc = ntc_tcp_send(sock, &msg, sizeof(msg));
		if (rc)
			break;

		dev_dbg(&dev->ntc.dev, "phase %d send data\n", phase);

		rc = ntc_tcp_send(sock, out_buf, out_size);
		if (rc)
			break;

		dev_dbg(&dev->ntc.dev, "phase %d+1 recv msg\n",
			phase);

		rc = ntc_tcp_recv(sock, &msg, sizeof(msg));
		if (rc)
			break;

		dev_dbg(&dev->ntc.dev, "phase %d+1 validate msg\n",
			phase);

		if (msg.magic != NTC_TCP_HELLO_MAGIC ||
		    msg.phase != phase) {
			rc = -EINVAL;
			break;
		}

		dev_dbg(&dev->ntc.dev, "phase %d+1 in_size %#zx\n",
			phase, in_size);

		in_size = msg.data_size;

		kfree(in_buf);
		in_buf = kmalloc(in_size, GFP_KERNEL);
		if (in_size && !in_buf) {
			rc = -ENOMEM;
			break;
		}

		dev_dbg(&dev->ntc.dev, "phase %d+1 out_size %#zx\n",
			phase, in_size);

		out_size = msg.next_size;

		kfree(out_buf);
		out_buf = kmalloc(out_size, GFP_KERNEL);
		if (out_size && !out_buf) {
			rc = -ENOMEM;
			break;
		}

		dev_dbg(&dev->ntc.dev, "phase %d+1 recv data\n",
			phase);

		rc = ntc_tcp_recv(sock, in_buf, in_size);
		if (rc)
			break;

		dev_dbg(&dev->ntc.dev, "next phase %d+1\n", phase);

		++phase;
	}

	kfree(in_buf);
	kfree(out_buf);

	return rc;
}

static int ntc_tcp_process(struct ntc_tcp_dev *dev,
			   struct socket *sock)
{
	int rc;

	rc = ntc_tcp_hello(dev, sock);
	if (rc)
		return rc;

	dev->sock = sock;
	ntc_ctx_enable(&dev->ntc);

	while (!kthread_should_stop()) {
		rc = ntc_tcp_recv_msg(dev);
		if (rc)
			break;
	}

	dev->sock = NULL;
	ntc_ctx_disable(&dev->ntc);
	ntc_ctx_quiesce(&dev->ntc);
	ntc_ctx_reset(&dev->ntc);

	return rc;
}

static void ntc_tcp_req_work(struct work_struct *ws)
{
	struct ntc_tcp_dev *dev = ntc_tcp_of_req_work(ws);
	struct list_head op_list;
	struct ntc_tcp_op *op, *next;
	unsigned long irqflags;

	spin_lock_irqsave(&dev->op_lock, irqflags);
	list_replace_init(&dev->op_list, &op_list);
	spin_unlock_irqrestore(&dev->op_lock, irqflags);

	list_for_each_entry_safe(op, next, &op_list, entry) {
		ntc_tcp_send_msg(dev, op);

		if (op->cb)
			op->cb(op->cb_ctx);

		kfree(op);
	}
}

static int ntc_tcp_server(void *ctx)
{
	struct ntc_tcp_dev *dev = ctx;
	struct socket *listen_sock, *sock;
	int rc;
	int flag = 1;

	int reps = 3; /* TODO: delme */

	pr_info("server %pISpc\n", &dev->saddr);

	pr_info("server %pISpc create\n", &dev->saddr);
	rc = sock_create_kern(&init_net, dev->saddr.sa_family,
			      SOCK_STREAM, IPPROTO_TCP, &listen_sock);
	if (rc)
		goto err_sock;

	if (ntc_tcp_nodelay) {
		rc = kernel_setsockopt(listen_sock, IPPROTO_TCP, TCP_NODELAY,
				       (char *)&flag, sizeof(flag));
		if (rc)
			goto err_sock;
	}

	pr_info("server %pISpc bind\n", &dev->saddr);
	rc = kernel_bind(listen_sock, &dev->saddr, sizeof(dev->saddr));
	if (rc)
		goto err_bind;

	pr_info("server %pISpc listen\n", &dev->saddr);
	rc = kernel_listen(listen_sock, 1);
	if (rc)
		goto err_bind;

	while (!kthread_should_stop()) {
		if (!reps) /* TODO: delme */
			break;
		--reps;

		pr_info("server %pISpc accept\n", &dev->saddr);
		rc = kernel_accept(listen_sock, &sock, SOCK_NONBLOCK);
		while (rc == -EAGAIN && !kthread_should_stop()) {
			schedule_timeout_interruptible(NTC_TCP_ACCEPT_DELAY);
			rc = kernel_accept(listen_sock, &sock, SOCK_NONBLOCK);
		}
		if (rc)
			goto err_bind;

		if (ntc_tcp_nodelay) {
			rc = kernel_setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
					       (char *)&flag, sizeof(flag));
			if (rc)
				goto err_bind;
		}

		pr_info("server %pISpc process\n", &dev->saddr);
		rc = ntc_tcp_process(dev, sock);
		if (rc)
			goto err_mesg;

		pr_info("server %pISpc shutdown\n", &dev->saddr);
		kernel_sock_shutdown(sock, SHUT_RDWR);
		sock_release(sock);
	}

	sock_release(listen_sock);

	pr_info("server %pISpc exiting normally\n", &dev->saddr);

	return 0;

err_mesg:
	kernel_sock_shutdown(sock, SHUT_RDWR);
	sock_release(sock);
err_bind:
	sock_release(listen_sock);
err_sock:
	pr_info("server %pISpc exiting rc:%d\n", &dev->saddr, rc);
	return rc;
}

static int ntc_tcp_client(void *ctx)
{
	struct ntc_tcp_dev *dev = ctx;
	struct socket *sock;
	int rc = 0;
	int flag = 1;

	pr_info("client %pISpc\n", &dev->saddr);

	while (!kthread_should_stop()) {
		pr_info("client %pISpc create\n", &dev->saddr);
		rc = sock_create_kern(&init_net, dev->saddr.sa_family,
				      SOCK_STREAM, IPPROTO_TCP, &sock);
		if (rc)
			goto err_sock;

		if (ntc_tcp_nodelay) {
			rc = kernel_setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
					       (char *)&flag, sizeof(flag));
			if (rc)
				goto err_sock;
		}

		pr_info("client %pISpc connect\n", &dev->saddr);
		rc = kernel_connect(sock, &dev->saddr,
				    sizeof(dev->saddr), 0);

		while (rc == -ECONNREFUSED && !kthread_should_stop()) {
			schedule_timeout_interruptible(NTC_TCP_CONNECT_DELAY);
			rc = kernel_connect(sock, &dev->saddr,
					    sizeof(dev->saddr), 0);
		}
		if (rc)
			goto err_conn;

		pr_info("client %pISpc process\n", &dev->saddr);
		rc = ntc_tcp_process(dev, sock);
		if (rc)
			goto err_mesg;

		pr_info("client %pISpc shutdown\n", &dev->saddr);
		kernel_sock_shutdown(sock, SHUT_RDWR);
		sock_release(sock);
	}

	pr_info("client %pISpc exiting normally\n", &dev->saddr);

	return 0;

err_mesg:
	kernel_sock_shutdown(sock, SHUT_RDWR);
err_conn:
	sock_release(sock);
err_sock:
	pr_info("client %pISpc exiting rc:%d\n", &dev->saddr, rc);
	return rc;
}

static int ntc_tcp_link_enable(struct ntc_dev *ntc)
{
	struct ntc_tcp_dev *dev = ntc_tcp_down_cast(ntc);

	wake_up_process(dev->task);

	return 0;
}

static int ntc_tcp_link_disable(struct ntc_dev *ntc)
{
	struct ntc_tcp_dev *dev = ntc_tcp_down_cast(ntc);
	struct socket *sock = dev->sock;

	if (sock)
		kernel_sock_shutdown(sock, SHUT_RDWR);

	return 0;
}

static int ntc_tcp_link_reset(struct ntc_dev *ntc)
{
	pr_debug("not implemented\n");

	return 0;
}

static void *ntc_tcp_req_create(struct ntc_dev *ntc)
{
	struct list_head *op_list;

	op_list = kmalloc(sizeof(*op_list), GFP_ATOMIC);
	if (op_list)
		INIT_LIST_HEAD(op_list);

	return op_list;
}

static void ntc_tcp_req_cancel(struct ntc_dev *ntc, void *req)
{
	struct list_head *op_list = req;
	struct ntc_tcp_op *op, *next;

	list_for_each_entry_safe(op, next, op_list, entry)
		kfree(op);

	kfree(op_list);
}

static int ntc_tcp_req_submit(struct ntc_dev *ntc, void *req)
{
	struct ntc_tcp_dev *dev = ntc_tcp_down_cast(ntc);
	struct list_head *op_list = req;
	unsigned long irqflags;

	spin_lock_irqsave(&dev->op_lock, irqflags);
	list_splice_tail(op_list, &dev->op_list);
	spin_unlock_irqrestore(&dev->op_lock, irqflags);

	kfree(op_list);

	schedule_work(&dev->req_work);

	return 0;
}

static int ntc_tcp_req_memcpy(struct ntc_dev *ntc, void *req,
			      u64 dst, u64 src, u64 len, bool fence,
			      void (*cb)(void *cb_ctx), void *cb_ctx)
{
	struct list_head *op_list = req;
	struct ntc_tcp_op *op;

	if (!len)
		return -EINVAL;

	op = kmalloc(sizeof(*op), GFP_ATOMIC);
	if (!op)
		return -ENOMEM;

	op->imm = false;
	op->dst = dst;
	op->src = src;
	op->len = len;
	op->cb = cb;
	op->cb_ctx = cb_ctx;

	list_add_tail(&op->entry, op_list);

	return 0;
}

static int ntc_tcp_req_imm32(struct ntc_dev *ntc, void *req,
			     u64 dst, u32 val, bool fence,
			     void (*cb)(void *cb_ctx), void *cb_ctx)
{
	struct list_head *op_list = req;
	struct ntc_tcp_op *op;

	op = kmalloc(sizeof(*op), GFP_ATOMIC);
	if (!op)
		return -ENOMEM;

	op->imm = true;
	op->dst = dst;
	op->src = val;
	op->len = sizeof(val);
	op->cb = cb;
	op->cb_ctx = cb_ctx;

	list_add_tail(&op->entry, op_list);

	return 0;
}

static int ntc_tcp_req_imm64(struct ntc_dev *ntc, void *req,
			     u64 dst, u64 val, bool fence,
			     void (*cb)(void *cb_ctx), void *cb_ctx)
{
	struct list_head *op_list = req;
	struct ntc_tcp_op *op;

	op = kmalloc(sizeof(*op), GFP_ATOMIC);
	if (!op)
		return -ENOMEM;

	op->imm = true;
	op->dst = dst;
	op->src = val;
	op->len = sizeof(val);
	op->cb = cb;
	op->cb_ctx = cb_ctx;

	list_add_tail(&op->entry, op_list);

	return 0;
}

static int ntc_tcp_req_signal(struct ntc_dev *ntc, void *req,
			      void (*cb)(void *cb_ctx), void *cb_ctx, int vec)
{
	struct list_head *op_list = req;
	struct ntc_tcp_op *op;

	op = kmalloc(sizeof(*op), GFP_ATOMIC);
	if (!op)
		return -ENOMEM;

	op->imm = false;
	op->dst = 0;
	op->src = 0;
	op->len = 0;
	op->cb = cb;
	op->cb_ctx = cb_ctx;

	list_add_tail(&op->entry, op_list);

	return 0;
}

static const struct ntc_dev_ops ntc_tcp_dev_ops = {
	.link_disable			= ntc_tcp_link_disable,
	.link_enable			= ntc_tcp_link_enable,
	.link_reset			= ntc_tcp_link_reset,
	.req_create			= ntc_tcp_req_create,
	.req_cancel			= ntc_tcp_req_cancel,
	.req_submit			= ntc_tcp_req_submit,
	.req_memcpy			= ntc_tcp_req_memcpy,
	.req_imm32			= ntc_tcp_req_imm32,
	.req_imm64			= ntc_tcp_req_imm64,
	.req_signal			= ntc_tcp_req_signal,
};

static int __init ntc_tcp_init(void)
{
	int i;

	for (i = 0; i < ntc_tcp_count; ++i)
		ntc_tcp_devices[i] = ntc_tcp_create(ntc_tcp_config[i], i);

	return 0;
}
module_init(ntc_tcp_init);

static void __exit ntc_tcp_exit(void)
{
	int i;

	for (i = 0; i < ntc_tcp_count; ++i)
		if (ntc_tcp_devices[i])
			ntc_tcp_destroy(ntc_tcp_devices[i]);
}
module_exit(ntc_tcp_exit);

