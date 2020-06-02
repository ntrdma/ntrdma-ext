/*
 * ntrdma_cm.c
 *
 *  Created on: Jan 1, 2020
 *      Author: leonidr
 */


#include "ntrdma_cm.h"
#include "ntrdma_cmd.h"
#include "ntrdma_qp.h"

#define ntrdma_cmd_cb_iw_cm_req_cb(__cb) \
	container_of(__cb, struct ntrdma_iw_cm_req, cb)

enum ntrdma_iw_cm_op {
	NTRDMA_IW_CM_REQ,
	NTRDMA_IW_CM_REP,
	NTRDMA_IW_CM_REJECT
};

struct ntrdma_iw_cm_req {
	struct ntrdma_cmd_cb cb;
	enum ntrdma_iw_cm_op op;
	int qpn;
	int remote_port;
	int local_port;
	u32 local_addr[4];
	u32 remote_addr[4];
	const void *priv;
	int priv_len;
	int ird;
	int status;
	int sin_family;
};

struct ntrdma_iw_cm_cmd {
	struct ntrdma_cmd_hdr hdr;
	enum ntrdma_iw_cm_op cm_op;
	int qpn;
	int remote_port;
	int local_port;
	u32 remote_addr[4];
	u32 local_addr[4];
	int ird;
	int priv_len;
	int status;
	int sin_family;
};

void ntrdma_qp_recv_work(struct ntrdma_qp *qp);


int ntrmda_rqp_modify(struct ntrdma_dev *dev,
		u32 src_qp_key, u32 access,
		u32 new_state, u32 dest_qp_key,
		const char *caller);


static inline void ntrdma_copy_ip_ntohl(u32 *dst, unsigned char *src)
{
	*dst++ = ntohl(*src++);
	*dst++ = ntohl(*src++);
	*dst++ = ntohl(*src++);
	*dst = ntohl(*src);
}

static inline void ntrdma_copy_ip_htonl(unsigned char *dst, u32 *src)
{
	*dst++ = htonl(*src++);
	*dst++ = htonl(*src++);
	*dst++ = htonl(*src++);
	*dst = htonl(*src);
}
static void dump_iw_cm_id_nodes(struct ntrdma_dev *dev)
{
	struct ntrdma_iw_cm_id_node *node;
	struct iw_cm_id *cm_id;
	struct sockaddr_in *rsin;
	struct sockaddr_in *lsin;
	struct ntrdma_iw_cm *ntrdma_iwcm = ntrdma_iw_cm_from_ntrdma_dev(dev);

	read_lock(&ntrdma_iwcm->slock);
	list_for_each_entry(node, &ntrdma_iwcm->ntrdma_iw_cm_list, head) {
		cm_id = node->cm_id;
		rsin = (struct sockaddr_in *)&cm_id->remote_addr;
		lsin = (struct sockaddr_in *)&cm_id->local_addr;

		pr_info("%pISpc:\t%pISpc\t\t QP %d  node [%p] cm id [%p]\n"
				, lsin, rsin, node->qpn, node, cm_id);

	}
	read_unlock(&ntrdma_iwcm->slock);

}

static struct ntrdma_iw_cm_id_node *
find_iw_cm_id_node(struct ntrdma_dev *dev, int src_port, int dst_port)
{
	struct ntrdma_iw_cm *ntrdma_iwcm = ntrdma_iw_cm_from_ntrdma_dev(dev);
	struct ntrdma_iw_cm_id_node *node = NULL;
	struct iw_cm_id *cm_id;
	struct sockaddr_in *src_sin;
	struct sockaddr_in *dst_sin;
	int is_found = 0;

	read_lock(&ntrdma_iwcm->slock);
	list_for_each_entry(node, &ntrdma_iwcm->ntrdma_iw_cm_list, head) {
		cm_id = node->cm_id;
		src_sin = (struct sockaddr_in *)&cm_id->local_addr;
		dst_sin = (struct sockaddr_in *)&cm_id->remote_addr;

		if (src_sin->sin_port == src_port && dst_sin->sin_port == dst_port) {
			is_found = 1;
			break;
		}
	}
	read_unlock(&ntrdma_iwcm->slock);

	if (unlikely(!is_found))
		return NULL;

	return node;
}

static struct ntrdma_iw_cm_id_node *
find_iw_cm_id_listener_node(struct ntrdma_dev *dev, int src_port)
{
	return find_iw_cm_id_node(dev, src_port, 0);
}

static int
store_iw_cm_id(struct ntrdma_dev *dev,
		int qpn,
		struct iw_cm_id *cm_id)
{
	struct ntrdma_iw_cm *ntrdma_iwcm = ntrdma_iw_cm_from_ntrdma_dev(dev);
	struct ntrdma_iw_cm_id_node *node;
	int local_port;
	int remote_port;

	local_port = ((struct sockaddr_in *)&cm_id->local_addr)->sin_port;
	remote_port = ((struct sockaddr_in *)&cm_id->remote_addr)->sin_port;
	node = find_iw_cm_id_node(dev, local_port, remote_port);
	if (unlikely(node)) {
		ntrdma_err(dev, "Trying store cm_id with local port %d  remote port %d while exist\n",
				ntohs(local_port), ntohs(remote_port));
		dump_iw_cm_id_nodes(dev);
		return -EINVAL;
	}

	node = kmem_cache_alloc(ntrdma_iwcm->cmid_node_slab, GFP_KERNEL);
	if (!node) {
		ntrdma_err(dev, "NTRDMA create listen alloc failed \n");
		return -ENOMEM;
	}

	node->cm_id = cm_id;
	node->qpn = qpn;
	INIT_LIST_HEAD(&node->head);

	write_lock(&ntrdma_iwcm->slock);
	list_add_tail(&node->head, &ntrdma_iwcm->ntrdma_iw_cm_list);
	write_unlock(&ntrdma_iwcm->slock);

	/* inc cm_id_priv->refcount */
	cm_id->add_ref(cm_id);
	cm_id->provider_data = node;

	ntrdma_dbg(dev, "node %p QP %d cm_id %p\n",
			node, node->qpn, cm_id);

	return 0;
}

static void
discard_iw_cm_id(struct ntrdma_dev *dev,
		struct ntrdma_iw_cm_id_node *node)
{
	struct ntrdma_iw_cm *ntrdma_iwcm = ntrdma_iw_cm_from_ntrdma_dev(dev);

	ntrdma_dbg(dev, "node %p qpn %d mc_id %p\n",
			node, node->qpn, node->cm_id);

	write_lock(&ntrdma_iwcm->slock);
	list_del(&node->head);
	write_unlock(&ntrdma_iwcm->slock);

	/*dec cm_id_priv->refcount and free if last*/
	node->cm_id->rem_ref(node->cm_id);
	kmem_cache_free(ntrdma_iwcm->cmid_node_slab, node);
}

static struct ib_qp *ntrdma_get_qp(struct ib_device *ibdev, int qpn)
{
	struct ntrdma_qp *qp;
	struct ntrdma_dev *dev;

	dev = ntrdma_ib_dev(ibdev);
	qp = ntrdma_dev_qp_look_and_get(dev, qpn);
	if (!qp) {
		return NULL;
	}
	ntrdma_qp_put(qp);
	return &qp->ibqp;
}

void ntrdma_cm_fire_abort(struct ntrdma_qp *qp)
{
	int ret = 0;
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);
	struct iw_cm_id *cm_id = qp->cm_id;

	struct iw_cm_event event = {
		.event = IW_CM_EVENT_CONNECT_REPLY,
		.status = -ECONNRESET,
	};

	ntrdma_dbg(dev, "firing abort for QP %d cm_id %p\n",
			qp->res.key, cm_id);

	if (unlikely(!cm_id || !cm_id->event_handler)) {
		return;
	}

	/* Assuming qp->cm_lock locked by the caller */
	qp->ntrdma_cm_state = NTRDMA_CM_STATE_IDLE;
	ret = cm_id->event_handler(cm_id, &event);

	if (ret)
		ntrdma_err(dev, "abort event to failed with return value: %d\n", ret);
}

static int ntrdma_fire_reject(struct iw_cm_id *cm_id, void *pdata, u8 pdata_len)
{

	int ret = 0;
	struct ib_device *ibdev = cm_id->device;
	struct ntrdma_dev *dev = ntrdma_ib_dev(ibdev);

	struct iw_cm_event event = {
		.event = IW_CM_EVENT_CONNECT_REPLY,
		.status = -ECONNREFUSED,
		.private_data = pdata,
		.private_data_len = pdata_len,
	};


	if (unlikely (!cm_id || !cm_id->event_handler)) {
		ret = -1;
		goto exit;
	}

	ret = cm_id->event_handler(cm_id, &event);

	if (ret) {
		ntrdma_err(dev, "reject event to failed with return value: %d\n", ret);
	}
exit:
	return ret;
}

static int ntrdma_fire_conn_rep(struct iw_cm_id *cm_id,
		int status,
		int priv_len,
		void *priv_data,
		int ird)
{
	struct iw_cm_event event = {
			.event = IW_CM_EVENT_CONNECT_REPLY,
			.status = status,
			.ird = ird,
	};

	if (priv_len && priv_data) {
		event.private_data = priv_data;
		event.private_data_len = priv_len;
	}

	memcpy(&event.local_addr, &cm_id->m_local_addr, sizeof(event.local_addr));
	memcpy(&event.remote_addr, &cm_id->m_remote_addr, sizeof(event.remote_addr));

	return cm_id->event_handler(cm_id, &event);
}

static int ntrdma_fire_conn_est(struct iw_cm_id *cm_id, int ird)
{
	struct iw_cm_event event = {
			.event = IW_CM_EVENT_ESTABLISHED,
			.status = 0,
			.ird = ird,
	};

	return cm_id->event_handler(cm_id, &event);
}

static inline
void ntrdma_cm_fire_close(struct ntrdma_qp *qp)
{
	int ret;
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);
	struct iw_cm_id *cm_id = qp->cm_id;
	struct iw_cm_event event = {
			.event = IW_CM_EVENT_CLOSE,
	};

	if (!cm_id || !cm_id->event_handler)
		return;

	ntrdma_dbg(dev, "NTRDMA CW firing close event QP %d\n",
			qp->res.key);

	ret = cm_id->event_handler(cm_id, &event);
	if (ret) {
		ntrdma_err(dev, "close event failed QP %d ret %d\n",
				qp->res.key, ret);
	}

	qp->cm_id = NULL;

	/*dec cm_id_priv->refcount and free if last*/
	cm_id->rem_ref(cm_id);
}

static inline
int ntrdma_cm_fire_disconnect(struct ntrdma_qp *qp)
{
	int ret = 0;
	struct iw_cm_id *cm_id = qp->cm_id;
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);

	struct iw_cm_event event = {
			.event = IW_CM_EVENT_DISCONNECT,
	};

	if (!cm_id || !cm_id->event_handler) {
		ret = -EINVAL;
		goto out;
	}

	ntrdma_dbg(dev, "NTRDMA CW firing disconnect event QP %d\n",
			qp->res.key);


	ret = cm_id->event_handler(cm_id, &event);

	if (ret) {
		ntrdma_err(dev, "disconnect event to QP %d failed %d\n",
				qp->res.key, ret);
	}

out:
	return ret;
}

void ntrdma_cm_kill(struct ntrdma_qp *qp)
{
		int err;

		mutex_lock(&qp->cm_lock);

		if (qp->ntrdma_cm_state != NTRDMA_CM_STATE_ESTABLISHED)
			goto unlock_out;
		qp->ntrdma_cm_state = NTRDMA_CM_STATE_KILLING;
		err = ntrdma_cm_fire_disconnect(qp);
		if (unlikely(err))
			goto unlock_out;

		ntrdma_cm_fire_close(qp);

unlock_out:
		mutex_unlock(&qp->cm_lock);
}

static int ntrdma_iw_cm_gen_perp(struct ntrdma_cmd_cb *cb,
				union ntrdma_cmd *cmd)
{
	struct ntrdma_iw_cm_req *req_cb = ntrdma_cmd_cb_iw_cm_req_cb(cb);
	struct ntrdma_iw_cm_cmd *my_cmd = (struct ntrdma_iw_cm_cmd *)cmd;
	void *cmd_priv = my_cmd + 1;

	BUILD_BUG_ON(sizeof(struct ntrdma_iw_cm_cmd) > sizeof(union ntrdma_cmd));

	my_cmd->hdr.op = NTRDMA_CMD_IW_CM;
	my_cmd->cm_op = req_cb->op;
	my_cmd->qpn =  req_cb->qpn;
	my_cmd->remote_port = req_cb->remote_port;
	my_cmd->local_port = req_cb->local_port;
	my_cmd->status = req_cb->status;
	memcpy(my_cmd->local_addr, req_cb->local_addr, sizeof(req_cb->local_addr));
	memcpy(my_cmd->remote_addr, req_cb->remote_addr, sizeof(req_cb->remote_addr));

	if (req_cb->priv_len)
		memcpy(cmd_priv, req_cb->priv, req_cb->priv_len);
	my_cmd->priv_len =  req_cb->priv_len;
	my_cmd->ird =  req_cb->ird;

	return 0;
}

static int ntrdma_iw_cm_gen_cmpl(struct ntrdma_cmd_cb *cb,
				const union ntrdma_rsp *rsp)
{
	u32 status;
	int rc;

	status = READ_ONCE(rsp->hdr.status);
	if (unlikely(status)) {
		pr_err("NTRDMA CM returned status %u\n", status);
		rc = -EIO;
		goto out;
	}

	rc = 0;

 out:
	complete_all(&cb->cmds_done);

	return rc;
}

static int ntrdma_cmd_send(struct ntrdma_dev *dev,
		struct ntrdma_iw_cm_req *qpcb)
{
	int rc;

	init_completion(&qpcb->cb.cmds_done);

	mutex_lock(&dev->res_lock);

	if (!dev->res_enable) {
		ntrdma_err(dev, "Command not sent, resource not enabled");
		mutex_unlock(&dev->res_lock);
		return -1;
	}

	ntrdma_dev_cmd_add(dev, &qpcb->cb);
	mutex_unlock(&dev->res_lock);

	rc = ntrdma_dev_cmd_submit(dev);
	if (unlikely(rc < 0)) {
		ntrdma_cmd_cb_unlink(dev, &qpcb->cb);
		ntrdma_err(dev, "Command not sent, ntrdma_dev_cmd_submit failed\n");
	}

	/*TODO consider make it async , if we changing to async qpcb should be dynamicly allocated*/
	return ntrdma_res_wait_cmds(dev, &qpcb->cb, msecs_to_jiffies(CMD_TIMEOUT_MSEC));
}

static int ntrdma_cmd_send_rep(struct ntrdma_dev *dev,
		struct sockaddr_storage *local_addr,
		struct sockaddr_storage *remote_addr,
		int qpn,
		int priv_len,
		const void *priv,
		int ird,
		int status)
{
	struct sockaddr_in *rsin = (struct sockaddr_in *)local_addr;
	struct sockaddr_in *lsin = (struct sockaddr_in *)remote_addr;

	struct ntrdma_iw_cm_req qpcb = {
			.cb = {
					.cmd_prep = ntrdma_iw_cm_gen_perp,
					.rsp_cmpl = ntrdma_iw_cm_gen_cmpl,
			},
			.op = NTRDMA_IW_CM_REP,
			.qpn = qpn,
			.remote_port = rsin->sin_port,
			.local_port = lsin->sin_port,
			.ird = ird,
			.status = status,
	};

	if (priv_len && priv) {
		qpcb.priv_len = priv_len;
		qpcb.priv = priv;
	}

	return ntrdma_cmd_send(dev, &qpcb);
}

static int ntrdma_cm_handle_connect_req(struct ntrdma_dev *dev,
		struct ntrdma_iw_cm_cmd *req_cmd)
{
	struct ntrdma_iw_cm_id_node *iw_cm_node;
	struct sockaddr_in *laddr, *raddr;
	struct sockaddr_in6 *laddr6, *raddr6;
	struct iw_cm_event event = {
			.event = IW_CM_EVENT_CONNECT_REQUEST,
			.status = 0,
			.private_data = req_cmd->priv_len?(req_cmd + 1):NULL,
			.private_data_len = req_cmd->priv_len,
			.provider_data = (void *)((unsigned long)req_cmd->qpn),
			.ird = req_cmd->ird,
	};

	laddr = (struct sockaddr_in *)&event.local_addr;
	raddr = (struct sockaddr_in *)&event.remote_addr;

	raddr->sin_port = req_cmd->local_port;
	laddr->sin_port = req_cmd->remote_port;

	if (req_cmd->sin_family == AF_INET6) {
		laddr->sin_family = AF_INET6;
		raddr->sin_family = AF_INET6;
		laddr6 = (struct sockaddr_in6 *) laddr;
		raddr6 = (struct sockaddr_in6 *) raddr;
		ntrdma_copy_ip_htonl((unsigned char *)&laddr6->sin6_addr.s6_addr, (u32 *)req_cmd->local_addr);
		ntrdma_copy_ip_htonl((unsigned char *)&raddr6->sin6_addr.s6_addr, (u32 *)req_cmd->remote_addr);
	} else {
		laddr->sin_family = AF_INET;
		raddr->sin_family = AF_INET;
		raddr->sin_addr.s_addr = htonl(req_cmd->local_addr[0]);
		laddr->sin_addr.s_addr =  htonl(req_cmd->remote_addr[0]);
	}

	iw_cm_node = find_iw_cm_id_listener_node(dev, req_cmd->remote_port);

	if (!iw_cm_node) {
		ntrdma_err(dev, "Listener port not found");
		dump_iw_cm_id_nodes(dev);
		ntrdma_cmd_send_rep(dev,
				&event.local_addr,
				&event.remote_addr,
				req_cmd->qpn, 0, NULL, 0,
				-ENETUNREACH); /* FIXME handle return value */
		return 0;
	}

	ntrdma_dbg(dev, "Connection request: %pISpc -> %pISpc\n",
			laddr, raddr);

	return iw_cm_node->cm_id->event_handler(iw_cm_node->cm_id, &event);
}

static int ntrdma_cm_handle_reject(struct ntrdma_dev *dev,
		struct ntrdma_iw_cm_cmd *my_cmd)
{
	int status = 0 ;
	struct ntrdma_iw_cm_id_node *iw_cm_node;

	iw_cm_node = find_iw_cm_id_node(dev, my_cmd->local_port, my_cmd->remote_port);
	if (!iw_cm_node) {
		ntrdma_err(dev, "iw cm node for port %d  QP %d not found in reject handler\n",
				ntohs(my_cmd->local_port), my_cmd->qpn);
		return -ENETUNREACH;
	}

	ntrdma_dbg(dev, "RDMA_CM: reject cm_id %p QP %d\n",
			iw_cm_node->cm_id, my_cmd->qpn);

	status = ntrdma_fire_reject(iw_cm_node->cm_id, 0, my_cmd->priv_len);

	if (unlikely(status)) {
		ntrdma_err(dev, "firing event IW_CM_EVENT_CONNECT_REPLY at reject and returned error %d\n",
				status);
	}

	return status;
}

static int ntrdma_cm_handle_rep(struct ntrdma_dev *dev,
		struct ntrdma_iw_cm_cmd *my_cmd)
{
	int rc = 0;
	int status;
	struct ntrdma_iw_cm_id_node *iw_cm_node;
	struct ib_qp *ibqp;
	struct ntrdma_qp *ntrdma_qp;
	struct iw_cm_id *cm_id;
	int qpn;

	struct ib_qp_attr attr = {
			.qp_state = IB_QPS_RTS,
			.dest_qp_num = my_cmd->qpn,
	};

	/*Now we need to find the request cm_id to fire event */
	iw_cm_node = find_iw_cm_id_node(dev, my_cmd->local_port, my_cmd->remote_port);
	if (!iw_cm_node) {
		ntrdma_err(dev, "iw cm node for port %d not found\n", ntohs(my_cmd->local_port));
		dump_iw_cm_id_nodes(dev);
		return -ENETUNREACH;
	}

	cm_id = iw_cm_node->cm_id;
	qpn = iw_cm_node->qpn;

	ntrdma_qp = ntrdma_dev_qp_look_and_get(dev, qpn);
	if (unlikely(!ntrdma_qp)) {
		ntrdma_err(dev, "QP %d  node %p local port %d from connection reply not found\n",
				qpn, iw_cm_node, ntohs(my_cmd->local_port));
		dump_iw_cm_id_nodes(dev);
		goto exit;
	}

	ntrdma_dbg(dev, "Reply for port %d node %p from RQP %d status %d\n",
			my_cmd->local_port, iw_cm_node, my_cmd->qpn, my_cmd->status);

	/* FIXME handle bad status */

	mutex_lock(&ntrdma_qp->cm_lock);

	if (ntrdma_qp->ntrdma_cm_state != NTRDMA_CM_STATE_CONNECTING) {
		rc = 1;
		ntrdma_dbg(dev, "qp %d Handle reply failed on wrong state. port: %d\n",
				my_cmd->qpn, my_cmd->local_port);
		goto cleanup;
	}

	discard_iw_cm_id(dev, iw_cm_node);

	ibqp = ntrdma_get_qp(&dev->ibdev, qpn);

	rc = ntrmda_rqp_modify(dev,
			my_cmd->qpn, 0,
			IB_QPS_RTS,
			ntrdma_qp->res.key,
			__func__);

	if (rc) {
		ntrdma_err(dev, "ntrdma_cmd_recv_qp_modify_internal failed. QPN: %d\n",
				my_cmd->qpn);
	}
/*FIXME should we handle it*/
	rc = ntrdma_modify_qp_internal(ibqp, &attr,
			IB_QP_STATE|IB_QP_DEST_QPN, NULL, false, __func__);
	if (rc) {
		ntrdma_err(dev, "ntrdma_modify_qp_internal failed. QPN: %d\n",
				attr.dest_qp_num);
	}
/*FIXME should we handle it*/

	ntrdma_qp_recv_work(ntrdma_qp);

	/*TODO - timer this out- so it will only be called after the
	 * server peer have received an ack
	 */


	status = ntrdma_fire_conn_rep(cm_id, my_cmd->status,
			my_cmd->priv_len, my_cmd + 1, my_cmd->ird);

	if (unlikely(status)) {
		ntrdma_err(dev, "firing event IW_CM_EVENT_CONNECT_REPLY and returned error %d\n",
				status);
	}

	ntrdma_qp->ntrdma_cm_state = NTRDMA_CM_STATE_ESTABLISHED;



cleanup:
	ntrdma_qp_put(ntrdma_qp);
	mutex_unlock(&ntrdma_qp->cm_lock);
exit:
	return rc;
}

static void ntrdma_cm_qp_shutdown(struct ntrdma_qp *qp)
{
	struct ntrdma_dev *dev = ntrdma_qp_dev(qp);

	ntrdma_dbg(dev, "CM shutdown QP %d state %d\n",
			qp->res.key, qp->ntrdma_cm_state);

	mutex_lock(&qp->cm_lock);
	if (qp->ntrdma_cm_state == NTRDMA_CM_STATE_IDLE) {
		qp->ntrdma_cm_state = NTRDMA_CM_STATE_KILLING;
		goto exit_unlock;
	}

	if (qp->ntrdma_cm_state != NTRDMA_CM_STATE_CONNECTING)
		goto exit_unlock;

	discard_iw_cm_id(dev, qp->cm_id->provider_data);
	ntrdma_cm_fire_abort(qp);

exit_unlock:
	mutex_unlock(&qp->cm_lock);
}


void ntrdma_cm_shutdown(struct ntrdma_dev *dev)
{
	struct ntrdma_qp *qp;

	mutex_lock(&dev->res_lock);
	list_for_each_entry(qp, &dev->qp_list, res.obj.dev_entry) {
		ntrdma_cm_qp_shutdown(qp);
	}
	mutex_unlock(&dev->res_lock);
}

int ntrdma_cmd_recv_cm(struct ntrdma_dev *dev,
		const union ntrdma_cmd *cmd,
		union ntrdma_rsp *rsp)
{
	struct ntrdma_iw_cm_cmd *my_cmd = (struct ntrdma_iw_cm_cmd *)cmd;
	switch (my_cmd->cm_op) {
	case NTRDMA_IW_CM_REQ:
		ntrdma_dbg(dev, "Received iw cm connection request qpn %d  local %d remote %d\n",
				my_cmd->qpn, ntohs(my_cmd->local_port), ntohs(my_cmd->remote_port));
		rsp->hdr.status = ntrdma_cm_handle_connect_req(dev, my_cmd);
		break;
	case NTRDMA_IW_CM_REP:
		ntrdma_dbg(dev, "Received iw cm connection reply RPQ %d  port %d -> %d\n",
				my_cmd->qpn, ntohs(my_cmd->local_port),
				ntohs(my_cmd->remote_port));
		rsp->hdr.status = ntrdma_cm_handle_rep(dev, my_cmd);
		break;
	case NTRDMA_IW_CM_REJECT:
		ntrdma_dbg(dev, "Received iw cm connection reject\n");
		rsp->hdr.status = ntrdma_cm_handle_reject(dev, my_cmd);
		break;
	default:
		ntrdma_dbg(dev, "Received unknown IW CM OP %d\n",
				my_cmd->cm_op);
		rsp->hdr.status = ~0;
		break;
	}

	rsp->hdr.cmd_id = cmd->hdr.cmd_id;
	return 0;
}

static void ntrdma_cm_add_ref(struct ib_qp *ibqp)
{
	struct ntrdma_qp *qp = ntrdma_ib_qp(ibqp);

	ntrdma_qp_get(qp);
}

/*Might be called by upprt layer with spinlock irqdisable*/
static void ntrdma_cm_rem_ref(struct ib_qp *ibqp)
{
	struct ntrdma_qp *qp = ntrdma_ib_qp(ibqp);

	ntrdma_qp_put(qp);
}

static int ntrdma_create_listen(struct iw_cm_id *cm_id, int backlog)
{
	struct ib_device *ibdev = cm_id->device;
	struct ntrdma_dev *dev = ntrdma_ib_dev(ibdev);
	struct sockaddr_in *sin = (struct sockaddr_in *)&cm_id->local_addr;

	ntrdma_dbg(dev, "Waiting for a connections on %pISpc (%d)\n",
		&cm_id->local_addr, ntohs(sin->sin_port));

	return store_iw_cm_id(dev, -1, cm_id);
}

static int ntrdma_connect(struct iw_cm_id *cm_id, struct iw_cm_conn_param *conn_param)
{
	struct ib_device *ibdev = cm_id->device;
	struct ntrdma_dev *dev = ntrdma_ib_dev(ibdev);
	struct sockaddr_in *rsin = (struct sockaddr_in *)&cm_id->remote_addr;
	struct sockaddr_in *lsin = (struct sockaddr_in *)&cm_id->local_addr;
	struct sockaddr_in6 *lsin6;
	struct sockaddr_in6 *rsin6;
	struct ib_qp *ib_qp;
	struct ntrdma_qp *ntrdma_qp;
	int rc;
	const size_t private_data_max_len =
			sizeof(const union ntrdma_cmd) -
			sizeof(struct ntrdma_iw_cm_cmd);

	struct ntrdma_iw_cm_req qpcb = {
			.cb = {
					.cmd_prep = ntrdma_iw_cm_gen_perp,
					.rsp_cmpl = ntrdma_iw_cm_gen_cmpl,
			},
			.op = NTRDMA_IW_CM_REQ,
			.qpn = conn_param->qpn,
			.remote_port = rsin->sin_port,
			.local_port = lsin->sin_port,
			.local_addr[0] = ntohl(lsin->sin_addr.s_addr),
			.remote_addr[0] = ntohl(rsin->sin_addr.s_addr),
			.priv_len = conn_param->private_data_len,
			.priv = conn_param->private_data,
			.ird = conn_param->ird,
	};

	if (lsin->sin_family == AF_INET6) {
		qpcb.sin_family = AF_INET6;
		lsin6 = (struct sockaddr_in6 *)lsin;
		rsin6 = (struct sockaddr_in6 *)rsin;
		ntrdma_copy_ip_ntohl((u32 *)&qpcb.local_addr, (unsigned char *)&lsin6->sin6_addr.s6_addr);
		ntrdma_copy_ip_ntohl((u32 *)&qpcb.remote_addr, (unsigned char *)&rsin6->sin6_addr.s6_addr);
	} else {
		qpcb.sin_family = AF_INET;
	}

	ib_qp = ntrdma_get_qp(ibdev, conn_param->qpn);
	ntrdma_qp = ntrdma_ib_qp(ib_qp);

	mutex_lock(&ntrdma_qp->cm_lock);

	if (ntrdma_qp->ntrdma_cm_state != NTRDMA_CM_STATE_IDLE) {
		ntrdma_err(dev, "connect QP %d while state %d\n",
				ntrdma_qp->res.key, ntrdma_qp->ntrdma_cm_state);
		rc = -EINVAL;
		goto err_state;
	}

	/*Client waiting for reply*/
	ntrdma_qp->ntrdma_cm_state = NTRDMA_CM_STATE_CONNECTING;

	if (ntrdma_qp->cm_id)
		ntrdma_qp->cm_id->rem_ref(ntrdma_qp->cm_id);

	ntrdma_qp->cm_id = cm_id;
	cm_id->add_ref(cm_id);

	if (conn_param->private_data_len > private_data_max_len) {
		ntrdma_err(dev, "Private data size not supported %d > %lu\n",
				conn_param->private_data_len,
				private_data_max_len);
		rc = -EINVAL;
		goto err_priv;
	}

	rc = store_iw_cm_id(dev, conn_param->qpn, cm_id);
	if (rc) {
		ntrdma_err(dev,
				"NTRDMA CM DEBUG connect %pISpc -> %pISpc failed on storing id. QPN: %d cm id %p\n",
				&cm_id->local_addr,
				&cm_id->remote_addr,
				conn_param->qpn,
				cm_id);

		goto err_store;
	}

	mutex_unlock(&ntrdma_qp->cm_lock);

	ntrdma_dbg(dev,
			"Connect: I want QP %d local %pISpc remote %pISpc priv data len %u priv data %p node %p cm_id %p\n",
			conn_param->qpn, &cm_id->local_addr,
			&cm_id->remote_addr,
			conn_param->private_data_len,
			conn_param->private_data,
			cm_id->provider_data,
			cm_id);

	rc = ntrdma_cmd_send(dev, &qpcb);
	if (rc) {
		ntrdma_err(dev,
				"NTRDMA CM DEBUG connect failed on sending request. QPN: %d\n",
				conn_param->qpn);

		/* There might be a race when cmd reply times out while connection already accepted by peer */
		mutex_lock(&ntrdma_qp->cm_lock);
		if (ntrdma_qp->ntrdma_cm_state == NTRDMA_CM_STATE_CONNECTING)
			goto err_send;
		mutex_unlock(&ntrdma_qp->cm_lock);
	}


	return 0;

err_send:
	discard_iw_cm_id(dev, cm_id->provider_data);
err_store:
err_priv:
	ntrdma_qp->ntrdma_cm_state = NTRDMA_CM_STATE_IDLE;
err_state:
	mutex_unlock(&ntrdma_qp->cm_lock);
	return rc;
}

static int ntrdma_accept(struct iw_cm_id *cm_id, struct iw_cm_conn_param *conn_param)
{
	struct ib_device *ibdev = cm_id->device;
	struct ntrdma_dev *dev = ntrdma_ib_dev(ibdev);
	struct ib_qp *ib_qp;
	struct ntrdma_qp *ntrdma_qp;
	int dest_qp_num = (unsigned long)cm_id->provider_data;
	int rc = 0;

	struct ib_qp_attr attr = {
			.qp_state = IB_QPS_RTS,
			.dest_qp_num = dest_qp_num,
	};

	ib_qp = ntrdma_get_qp(ibdev, conn_param->qpn);
	ntrdma_qp = ntrdma_ib_qp(ib_qp);

	mutex_lock(&ntrdma_qp->cm_lock);

	/* inc cm_id_priv->refcount */
	cm_id->add_ref(cm_id); /*FIXME should be removed in case of error*/
	ntrdma_qp->cm_id = cm_id;

	ntrdma_dbg(dev, "Accept: I want QP %d to RQP %d %pISpc priv data len %u priv data %p\n",
			conn_param->qpn,
			dest_qp_num,
			&cm_id->remote_addr,
			conn_param->private_data_len,
			conn_param->private_data);

	if (ntrdma_modify_qp_internal(ib_qp, &attr,
			IB_QP_STATE|IB_QP_DEST_QPN, NULL, false, __func__)) {
		ntrdma_err(dev,
				"NTRDMA CM ERR accepting QP %d encountred error on modify qp state most likely link is down\n",
				conn_param->qpn);
	}

	ntrdma_qp_recv_work(ntrdma_qp);

	mutex_unlock(&ntrdma_qp->cm_lock);

	rc = ntrdma_cmd_send_rep(dev,
			&cm_id->local_addr,
			&cm_id->remote_addr,
			conn_param->qpn,
			conn_param->private_data_len,
			conn_param->private_data,
			conn_param->ird, 0);
	if (rc) {
		ntrdma_err(dev, "NTRDMA CM ERR accepting QP %d encountred error on send reply\n",
				conn_param->qpn);
		return rc;
	}

	/* Modify rqp */
	rc = ntrmda_rqp_modify(dev,
			dest_qp_num, 0,
			IB_QPS_RTS,
			ntrdma_qp->res.key,
			__func__);

	if (rc) {
		ntrdma_err(dev,
				"NTRDMA CM ERR accepting QP %d encountred error on modify rqp state\n",
				conn_param->qpn);
	}

	mutex_lock(&ntrdma_qp->cm_lock);

	if (ntrdma_qp->ntrdma_cm_state != NTRDMA_CM_STATE_IDLE) {
		ntrdma_err(dev, "NTRDMA CM state %d while expected %d\n",
				ntrdma_qp->ntrdma_cm_state, NTRDMA_CM_STATE_IDLE);
		mutex_unlock(&ntrdma_qp->cm_lock);
		/*FIXME what should we do now ?*/
		return -EINVAL;
	}

	ntrdma_qp->ntrdma_cm_state = NTRDMA_CM_STATE_ESTABLISHED;

	if (ntrdma_fire_conn_est(cm_id, conn_param->ird)) {
		ntrdma_err(dev, "NTRDMA CM ERR accepting QP %d encountred error on fire establish\n",
				conn_param->qpn);
	}

	mutex_unlock(&ntrdma_qp->cm_lock);
	return rc;
}


static int ntrdma_reject(struct iw_cm_id *cm_id, const void *pdata, u8 pdata_len)
{
	struct ib_device *ibdev = cm_id->device;
	struct ntrdma_dev *dev = ntrdma_ib_dev(ibdev);
	struct sockaddr_in *rsin = (struct sockaddr_in *)&cm_id->local_addr;
	struct sockaddr_in *lsin = (struct sockaddr_in *)&cm_id->remote_addr;
	struct sockaddr_in6 *rsin6, *lsin6;

	struct ntrdma_iw_cm_req rejmsg = {
			.cb = {
					.cmd_prep = ntrdma_iw_cm_gen_perp,
					.rsp_cmpl = ntrdma_iw_cm_gen_cmpl,
			},
			.op = NTRDMA_IW_CM_REJECT,
			.qpn = -1,
			.remote_port = rsin->sin_port,
			.local_port = lsin->sin_port,
			.local_addr[0] = ntohl(lsin->sin_addr.s_addr),
			.remote_addr[0] = ntohl(rsin->sin_addr.s_addr),
			.priv_len = pdata_len,
			.priv = pdata,
			.ird = -1,
	};

	if (rsin->sin_family == AF_INET6) {
		rejmsg.sin_family = AF_INET6;
		rsin6 = (struct sockaddr_in6 *)rsin;
		lsin6 = (struct sockaddr_in6 *)lsin;
		ntrdma_copy_ip_ntohl((u32 *)rejmsg.local_addr, (unsigned char *)&rsin6->sin6_addr.s6_addr);
		ntrdma_copy_ip_ntohl((u32 *)rejmsg.remote_addr, (unsigned char *)&lsin6->sin6_addr.s6_addr);
	} else {
		rejmsg.sin_family = AF_INET;
	}

	ntrdma_dbg(dev, "NTRDMA CM rejecting pdata len %u on\n", pdata_len);

	return ntrdma_cmd_send(dev, &rejmsg);
}

static int ntrdma_destroy_listen(struct iw_cm_id *cm_id)
{
	struct ib_device *ibdev = cm_id->device;
	struct ntrdma_dev *dev = ntrdma_ib_dev(ibdev);
	struct ntrdma_iw_cm_id_node *listener;
	struct sockaddr_in *sin = (struct sockaddr_in *)&cm_id->local_addr;

	ntrdma_dbg(dev, "NTRDMA CM DEBUG destroying: %pISpc (%d)\n",
		&cm_id->local_addr, ntohs(sin->sin_port));

	listener = cm_id->provider_data;
	cm_id->provider_data = NULL;

	discard_iw_cm_id(dev, listener);

	return 0;
}

struct iw_cm_verbs*
ntrdma_cm_init(const char *name)
{
	struct ntrdma_iw_cm *ntrdma_iwcm;
	struct iw_cm_verbs *iwcm;

	ntrdma_iwcm = kzalloc(sizeof(*ntrdma_iwcm), GFP_KERNEL);
	if (!ntrdma_iwcm)
		return NULL;

	ntrdma_iwcm->cmid_node_slab = KMEM_CACHE(ntrdma_iw_cm_id_node, 0);
	if (!ntrdma_iwcm->cmid_node_slab)
		goto err_slab;

	iwcm = &ntrdma_iwcm->iwcm;
	iwcm->add_ref = ntrdma_cm_add_ref;
	iwcm->rem_ref = ntrdma_cm_rem_ref;
	iwcm->get_qp = ntrdma_get_qp;
	iwcm->connect = ntrdma_connect;
	iwcm->accept = ntrdma_accept;
	iwcm->reject = ntrdma_reject;
	iwcm->create_listen = ntrdma_create_listen;
	iwcm->destroy_listen = ntrdma_destroy_listen;
	memcpy(iwcm->ifname, name,
			sizeof(iwcm->ifname));

	INIT_LIST_HEAD(&ntrdma_iwcm->ntrdma_iw_cm_list);
	rwlock_init(&ntrdma_iwcm->slock);

	return iwcm;
err_slab:
	kfree(ntrdma_iwcm);
	return NULL;
}

void ntrdma_cm_deinit(struct iw_cm_verbs *iwcm)
{
	struct ntrdma_iw_cm *ntrdma_iwcm = iwcm_2_ntrdma_iwcm(iwcm);
	kmem_cache_destroy(ntrdma_iwcm->cmid_node_slab);
	kfree(ntrdma_iwcm);
}
