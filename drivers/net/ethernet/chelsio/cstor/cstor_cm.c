/*
 * Copyright (c) 2021-2025 Chelsio Communications. All rights reserved.
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
 *	  copyright notice, this list of conditions and the following
 *	  disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *	  copyright notice, this list of conditions and the following
 *	  disclaimer in the documentation and/or other materials
 *	  provided with the distribution.
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
#include <linux/kconfig.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/workqueue.h>
#include <linux/skbuff.h>
#include <linux/timer.h>
#include <linux/notifier.h>
#include <linux/inetdevice.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <net/tcp.h>
#include <linux/inetdevice.h>
#include <linux/if_vlan.h>
#include <linux/xarray.h>

#include <net/ip6_route.h>
#include <net/addrconf.h>
#include <net/neighbour.h>
#include <net/netevent.h>
#include <net/route.h>
#if IS_ENABLED(CONFIG_CHELSIO_T4_IPSEC_INLINE)
#include <net/xfrm.h>
#endif

#include "cstor.h"
#include <clip_tbl.h>

char *states[] = {
	[CSTOR_SOCK_STATE_IDLE]			= "idle",
	[CSTOR_SOCK_STATE_CONNECTING]		= "connecting",
	[CSTOR_SOCK_STATE_CONNECTED]		= "connected",
	[CSTOR_SOCK_STATE_LOGIN_REQ_WAIT]	= "login_req_wait",
	[CSTOR_SOCK_STATE_LOGIN_REQ_RCVD]	= "login_req_rcvd",
	[CSTOR_SOCK_STATE_QP_MODE]		= "qp_mode",
	[CSTOR_SOCK_STATE_ABORTING]		= "aborting",
	[CSTOR_SOCK_STATE_CLOSING]		= "closing",
	[CSTOR_SOCK_STATE_MORIBUND]		= "moribund",
	[CSTOR_SOCK_STATE_DEAD]			= "dead",
};

static bool nocong;
module_param(nocong, bool, 0644);
MODULE_PARM_DESC(nocong, "Turn of congestion control (default=0)");

static bool enable_ecn = true;
module_param(enable_ecn, bool, 0644);
MODULE_PARM_DESC(enable_ecn, "Enable ECN (default=true/enabled)");

static bool enable_tcp_timestamps = true;
module_param(enable_tcp_timestamps, bool, 0644);
MODULE_PARM_DESC(enable_tcp_timestamps, "Enable tcp timestamps (default=true)");

static bool enable_tcp_sack = true;
module_param(enable_tcp_sack, bool, 0644);
MODULE_PARM_DESC(enable_tcp_sack, "Enable tcp SACK (default=true)");

static bool enable_tcp_window_scaling = true;
module_param(enable_tcp_window_scaling, bool, 0644);
MODULE_PARM_DESC(enable_tcp_window_scaling, "Enable tcp window scaling (default=true)");

bool enable_debug;
module_param(enable_debug, bool, 0644);
MODULE_PARM_DESC(enable_debug, "Enable debug logging (default=false)");

static int sock_timeout_secs = 60;
module_param(sock_timeout_secs, int, 0644);
MODULE_PARM_DESC(sock_timeout_secs, "CM Endpoint operation timeout in seconds (default=60)");

static int rcv_win = 1023 * SZ_1K;
module_param(rcv_win, int, 0644);
MODULE_PARM_DESC(rcv_win, "TCP receive window in bytes (default=1023KB)");

static int snd_win = 512 * SZ_1K;
module_param(snd_win, int, 0644);
MODULE_PARM_DESC(snd_win, "TCP send window in bytes (default=512KB)");

static bool adjust_win = true;
module_param(adjust_win, bool, 0644);
MODULE_PARM_DESC(adjust_win, "Adjust TCP window based on link speed (default=true)");

static void sock_timeout(struct timer_list *t);

void cstor_free_uevent_node(struct cstor_uevent_node *uevt_node)
{
	list_del(&uevt_node->entry);
	kfree_skb(uevt_node->skb);
	kfree(uevt_node);
}

void _cstor_free_event_channel(struct kref *kref)
{
	struct cstor_event_channel *event_channel =
			container_of(kref, struct cstor_event_channel, kref);
	struct cstor_uevent_node *uevt_node, *tmp;

	xa_erase(&event_channel->uctx->event_channels, event_channel->efd);

	mutex_lock(&event_channel->uevt_list_lock);
	list_for_each_entry_safe(uevt_node, tmp, &event_channel->uevt_list, entry)
		cstor_free_uevent_node(uevt_node);
	mutex_unlock(&event_channel->uevt_list_lock);

	mutex_destroy(&event_channel->uevt_list_lock);

	eventfd_ctx_put(event_channel->efd_ctx);

	kfree(event_channel->fatal_uevt_node);

	kfree(event_channel);
}

static void cstor_put_listen_sock(struct cstor_listen_sock *lcsk)
{
	cstor_debug(lcsk->uctx->cdev, "stid %u refcnt %d\n", lcsk->stid, kref_read(&lcsk->kref));
	WARN_ON(kref_read(&lcsk->kref) < 1);
	kref_put(&lcsk->kref, _cstor_free_listen_sock);
}

static void cstor_get_listen_sock(struct cstor_listen_sock *lcsk)
{
	cstor_debug(lcsk->uctx->cdev, "stid %u refcnt %d\n", lcsk->stid, kref_read(&lcsk->kref));
	kref_get(&lcsk->kref);
}

static void cstor_put_sock(struct cstor_sock *csk)
{
	WARN_ON(kref_read(&csk->kref) < 1);
	kref_put(&csk->kref, _cstor_free_sock);
}

static void cstor_get_sock(struct cstor_sock *csk)
{
	cstor_debug(csk->uctx->cdev, "tid %u refcnt %d\n", csk->tid, kref_read(&csk->kref));
	kref_get(&csk->kref);
}

static void cstor_free_lport(struct cstor_sock *csk)
{
	struct cstor_device *cdev = csk->uctx->cdev;
	u16 lport;

	if (!test_and_clear_bit(CSTOR_SOCK_F_LPORT_VALID, &csk->flags))
		return;

	if (csk->laddr.ss_family == AF_INET) {
		struct sockaddr_in *sin = (struct sockaddr_in *)&csk->laddr;

		lport = be16_to_cpu(sin->sin_port);
	} else {
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&csk->laddr;

		lport = be16_to_cpu(sin6->sin6_port);
	}

	cxgb4_uld_put_resource(&cdev->resource.tcp_port_table, lport);
}

static int cstor_get_lport(struct cstor_sock *csk, u16 *_lport)
{
	struct cstor_device *cdev = csk->uctx->cdev;
	u32 lport;

	lport = cxgb4_uld_get_resource(&cdev->resource.tcp_port_table);
	if (!lport)
		return -1;

	if (csk->laddr.ss_family == AF_INET) {
		struct sockaddr_in *sin = (struct sockaddr_in *)&csk->laddr;

		sin->sin_port = cpu_to_be16(lport);
	} else {
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&csk->laddr;

		sin6->sin6_port = cpu_to_be16(lport);
	}

	*_lport = lport;
	set_bit(CSTOR_SOCK_F_LPORT_VALID, &csk->flags);
	return 0;
}

static void start_sock_timer(struct cstor_sock *csk)
{
	cstor_debug(csk->uctx->cdev, "tid %u starting\n", csk->tid);
	cstor_get_sock(csk);
	csk->timer.expires = jiffies + (csk->first_pdu_recv_timeout * HZ);
	add_timer(&csk->timer);
}

static void stop_sock_timer(struct cstor_sock *csk)
{
	cstor_debug(csk->uctx->cdev, "tid %u stopping\n", csk->tid);
	if (del_timer_sync(&csk->timer))
		cstor_put_sock(csk);
}

static int
cstor_l2t_send(struct cstor_device *cdev, struct sk_buff *skb, struct cstor_sock *csk)
{
	int ret = 0;

	if (test_bit(CDEV_FLAG_FATAL_ERROR, &cdev->flags)) {
		cstor_err(cdev, "device in error state - dropping\n");
		kfree_skb(skb);
		return -EIO;
	}

	ret = cxgb4_l2t_send(cdev->lldi.ports[0], skb, csk->l2t);
	if (net_xmit_eval(ret) != NET_XMIT_SUCCESS) {
		cstor_err(cdev, "cxgb4_l2t_send() failed, err %d\n", ret);
		return -EINVAL;
	}

	return 0;
}

int cstor_ofld_send(struct cstor_device *cdev, struct sk_buff *skb)
{
	int ret = 0;

	if (test_bit(CDEV_FLAG_FATAL_ERROR, &cdev->flags)) {
		cstor_err(cdev, "device in error state - dropping\n");
		kfree_skb(skb);
		return -EIO;
	}

	ret = cxgb4_uld_xmit(cdev->lldi.ports[0], skb);
	if (net_xmit_eval(ret) != NET_XMIT_SUCCESS) {
		cstor_err(cdev, "cxgb4_uld_xmit() failed, err %d\n", ret);
		return -EINVAL;
	}

	return 0;
}

static void release_tid(struct cstor_device *cdev, u32 tid, u16 port_id, struct sk_buff *skb)
{
	struct cpl_tid_release *req;
	u16 ctrlq_idx = cdev->lldi.ctrlq_start + (port_id * cdev->lldi.num_up_cores);

	skb_get(skb);
	skb_trim(skb, 0);
	cxgb4_uld_tid_ctrlq_id_sel_update(cdev->lldi.ports[0], tid, &ctrlq_idx);
	set_wr_txq(skb, CPL_PRIORITY_SETUP, ctrlq_idx);
	t4_set_arp_err_handler(skb, NULL, NULL);

	req = (struct cpl_tid_release *)skb_put(skb, sizeof(*req));
	INIT_TP_WR(req, tid);
	OPCODE_TID(req) = cpu_to_be32(MK_OPCODE_TID(CPL_TID_RELEASE, tid));
	req->rsvd = cpu_to_be32(0);
	cstor_ofld_send(cdev, skb);
}

static int insert_csk_tid(struct cstor_sock *csk)
{
	struct cstor_device *cdev = csk->uctx->cdev;
	int ret;

	xa_lock(&cdev->tids);
	ret = __xa_insert(&cdev->tids, csk->tid, csk, GFP_KERNEL);
	if (!ret) {
		__xa_set_mark(&cdev->tids, csk->tid, CSTOR_ACTIVE_SOCK);
		atomic_inc(&csk->uctx->num_csk);
	}

	xa_unlock(&cdev->tids);

	return ret;
}

static u16 cstor_get_hdr_size(struct cstor_sock *csk, u8 tstamp)
{
	bool ipv4 = (csk->laddr.ss_family == AF_INET);
	u16 hdr_size = (ipv4 ? sizeof(struct iphdr) : sizeof(struct ipv6hdr));

#define CSTOR_IV_LENGTH 8
	if (test_bit(CSTOR_SOCK_F_IPSEC_TRANSPORT_MODE, &csk->flags)) {
		hdr_size += (sizeof(struct ip_esp_hdr) + CSTOR_IV_LENGTH);
	} else if (test_bit(CSTOR_SOCK_F_IPSEC_TUNNEL_MODE, &csk->flags)) {
		hdr_size += (sizeof(struct ip_esp_hdr) + CSTOR_IV_LENGTH);
		hdr_size += (ipv4 ? sizeof(struct iphdr) : sizeof(struct ipv6hdr));
	}

	hdr_size += sizeof(struct tcphdr);
	hdr_size += (tstamp ? round_up(TCPOLEN_TIMESTAMP, 4) : 0);

	return hdr_size;
}

static void set_emss(struct cstor_sock *csk, u16 opt)
{
	struct cstor_device *cdev = csk->uctx->cdev;

	csk->emss = cdev->lldi.mtus[TCPOPT_MSS_G(opt)] -
			cstor_get_hdr_size(csk, TCPOPT_TSTAMP_G(opt));
	if (csk->emss < 128)
		csk->emss = 128;

	if (csk->emss & 7)
		cstor_debug(cdev, "tid %u, Warning: misaligned mtu idx %u emss=%u\n",
			    csk->tid, TCPOPT_MSS_G(opt), csk->emss);
	else
		cstor_debug(cdev, "tid %u mss_idx %u emss=%u\n",
			    csk->tid, TCPOPT_MSS_G(opt), csk->emss);
}

static void free_uevent_node_list(struct cstor_sock *csk)
{
	struct cstor_uevent_node *uevt_node, *tmp;

	list_for_each_entry_safe(uevt_node, tmp, &csk->uevt_list, entry) {
		list_del(&uevt_node->entry);
		kfree(uevt_node);
	}
}

static void *alloc_sock(struct cstor_ucontext *uctx, struct cstor_listen_sock *lcsk)
{
	struct cstor_sock *csk;
	struct cstor_uevent_node *uevt_node;
	struct sk_buff *skb;
	u32 len = roundup(sizeof(union cpl_wr_size), 16);
	int i;

	csk = kzalloc(sizeof(*csk), GFP_KERNEL);
	if (!csk)
		return NULL;

	skb_queue_head_init(&csk->skb_list);
	for (i = 0; i < CN_MAX_CON_BUF; i++) {
		skb = alloc_skb(len, GFP_KERNEL);
		if (!skb)
			goto err1;

		skb_queue_tail(&csk->skb_list, skb);
	}

	INIT_LIST_HEAD(&csk->uevt_list);
	for (i = 0; i < 2; i++) {
		uevt_node = kzalloc(sizeof(*uevt_node), GFP_KERNEL);
		if (!uevt_node)
			goto err2;

		INIT_LIST_HEAD(&uevt_node->entry);
		list_add_tail(&uevt_node->entry, &csk->uevt_list);
	}

	kref_init(&csk->kref);
	mutex_init(&csk->mutex);
	init_completion(&csk->wr_wait.completion);

	csk->uctx = uctx;
	csk->tid = CSTOR_INVALID_TID;
	csk->atid = CSTOR_INVALID_ATID;
	csk->wr_cred = uctx->cdev->lldi.wr_cred - DIV_ROUND_UP(sizeof(struct cpl_abort_req), 16);
	csk->wr_max_cred = csk->wr_cred;
	if (lcsk) {
		csk->first_pdu_recv_timeout = lcsk->first_pdu_recv_timeout;
		csk->lcsk = lcsk;
		cstor_get_listen_sock(lcsk);
	} else {
		set_bit(CSTOR_SOCK_F_ACTIVE, &csk->flags);
	}

	return csk;
err2:
	free_uevent_node_list(csk);
err1:
	skb_queue_purge(&csk->skb_list);
	kfree(csk);
	return NULL;
}

/*
 * Atomically lookup the csk ptr given the tid and grab a reference on the csk.
 */
static struct cstor_sock *get_sock_from_tid(struct cstor_device *cdev, u32 tid)
{
	struct cstor_sock *csk;

	xa_lock(&cdev->tids);
	csk = xa_load(&cdev->tids, tid);
	xa_unlock(&cdev->tids);
	return csk;
}

/*
 * Atomically lookup the csk ptr given the stid and grab a reference on the csk.
 */
static struct cstor_listen_sock *get_listen_sock_from_stid(struct cstor_device *cdev, u32 stid)
{
	struct cstor_listen_sock *lcsk;

	xa_lock(&cdev->stids);
	lcsk = xa_load(&cdev->stids, stid);
	xa_unlock(&cdev->stids);
	return lcsk;
}

void _cstor_free_listen_sock(struct kref *kref)
{
	struct cstor_device *cdev;
	struct cstor_listen_sock *lcsk = container_of(kref, struct cstor_listen_sock, kref);
	struct cstor_event_channel *event_channel = lcsk->event_channel;

	cdev = lcsk->uctx->cdev;
	if (lcsk->stid != CSTOR_INVALID_STID) {
		xa_erase(&cdev->stids, lcsk->stid);
		cxgb4_uld_stid_free(cdev->lldi.ports[0], lcsk->laddr.ss_family, lcsk->stid);
		atomic_dec(&lcsk->uctx->num_lcsk);
		lcsk->stid = CSTOR_INVALID_STID;
	}

	if (lcsk->clip_release) {
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&lcsk->laddr;

		cxgb4_clip_release(cdev->lldi.ports[0], (const u32 *)&sin6->sin6_addr.s6_addr, 1);
		lcsk->clip_release = false;
	}

	if (lcsk->destroy_skb)
		kfree_skb(lcsk->destroy_skb);

	if (event_channel) {
		WARN_ON(kref_read(&event_channel->kref) < 1);
		kref_put(&event_channel->kref, _cstor_free_event_channel);
	}

	mutex_destroy(&lcsk->mutex);
	kfree(lcsk);
}

static void _cstor_free_atid(struct cstor_sock *csk)
{
	if (csk->atid != CSTOR_INVALID_ATID) {
		xa_erase(&csk->uctx->cdev->atids, csk->atid);
		cxgb4_uld_atid_free(csk->uctx->cdev->lldi.ports[0], csk->atid);
		csk->atid = CSTOR_INVALID_ATID;
	}
}

int cstor_free_atid(struct cstor_ucontext *uctx, void __user *ubuf)
{
	struct cstor_device *cdev = uctx->cdev;
	struct cstor_sock *csk;
	struct cstor_free_atid_cmd cmd;

	if (copy_from_user(&cmd, ubuf, sizeof(cmd))) {
		cstor_err(cdev, "copy_from_user() failed, cmd size %zu\n", sizeof(cmd));
		return -EFAULT;
	}

	csk = cxgb4_uld_atid_lookup(cdev->lldi.ports[0], cmd.atid);
	if (!csk) {
		cstor_err(cdev, "atid %u, csk not found\n", cmd.atid);
		return -EINVAL;
	}

	mutex_lock(&csk->mutex);

	set_bit(CSTOR_SOCK_H_ULP_FREE_ATID, &csk->history);

	if (csk->state > CSTOR_SOCK_STATE_QP_MODE) {
		cstor_err(cdev, "tid %u, atid %u, invalid state : %s (%d)\n", csk->tid,
			  csk->atid, states[csk->state], csk->state);
		mutex_unlock(&csk->mutex);
		return -ECONNRESET;
	} else if (csk->state != CSTOR_SOCK_STATE_CONNECTED) {
		cstor_err(cdev, "tid %u, atid %u, invalid state : %s (%d)\n", csk->tid,
			  csk->atid, states[csk->state], csk->state);
		mutex_unlock(&csk->mutex);
		return -EINVAL;
	}

	_cstor_free_atid(csk);

	mutex_unlock(&csk->mutex);
	return 0;
}

static struct sk_buff *cstor_sock_peek_wr(const struct cstor_sock *csk)
{
	return csk->wr_pending_head;
}

static void cstor_sock_enqueue_wr(struct cstor_sock *csk, struct sk_buff *skb)
{
	cstor_skcb_wr_next(skb) = NULL;

	skb_get(skb);

	if (!csk->wr_pending_head)
		csk->wr_pending_head = skb;
	else
		cstor_skcb_wr_next(csk->wr_pending_tail) = skb;

	csk->wr_pending_tail = skb;
}

static struct sk_buff *cstor_sock_dequeue_wr(struct cstor_sock *csk)
{
	struct sk_buff *skb = csk->wr_pending_head;

	if (likely(skb)) {
		csk->wr_pending_head = cstor_skcb_wr_next(skb);
		cstor_skcb_wr_next(skb) = NULL;
	}

	return skb;
}

static void cstor_sock_free_skb(struct cstor_sock *csk)
{
	struct sk_buff *skb;

	skb_queue_purge(&csk->skb_list);
	kfree_skb(csk->dskb);

	while ((skb = cstor_sock_dequeue_wr(csk)))
		kfree_skb(skb);
}

static void cstor_free_txq(struct cstor_sock *csk)
{
	if (test_and_clear_bit(CSTOR_SOCK_F_TXQ_VALID, &csk->flags)) {
		struct cstor_device *cdev = csk->uctx->cdev;
		struct cxgb4_uld_txq_info txq_info = {};

		txq_info.lld_index = csk->txq_idx;
		cxgb4_uld_txq_free(cdev->lldi.ports[csk->port_id],
				   CXGB4_ULD_TYPE_CSTOR, &txq_info);
	}
}

static int cstor_alloc_txq(struct cstor_sock *csk)
{
	struct cstor_device *cdev = csk->uctx->cdev;
	struct cxgb4_uld_txq_info txq_info = {};
	int ret;

	ret = cxgb4_uld_txq_alloc(cdev->lldi.ports[csk->port_id], CXGB4_ULD_TYPE_CSTOR, &txq_info);
	if (ret)
		return ret;

	csk->txq_idx = txq_info.lld_index;
	set_bit(CSTOR_SOCK_F_TXQ_VALID, &csk->flags);
	return 0;
}

void _cstor_free_sock(struct kref *kref)
{
	struct cstor_device *cdev;
	struct cstor_listen_sock *lcsk = NULL;
	struct cstor_sock *csk = container_of(kref, struct cstor_sock, kref);
	struct cstor_event_channel *event_channel = csk->event_channel;

	cdev = csk->uctx->cdev;
	cstor_debug(cdev, "tid %u state %s (%d)\n", csk->tid, states[csk->state], csk->state);

	if (csk->lcsk)
		lcsk = csk->lcsk;

	if (csk->tid != CSTOR_INVALID_TID) {
		xa_erase(&cdev->tids, csk->tid);
		cxgb4_uld_tid_remove(cdev->lldi.ports[0], csk->ctrlq_idx,
				     csk->laddr.ss_family, csk->tid);
		atomic_dec(&csk->uctx->num_csk);
	}

	_cstor_free_atid(csk);
	cstor_free_lport(csk);
	cstor_free_txq(csk);

	if (test_and_clear_bit(CSTOR_SOCK_F_CLIP_RELEASE, &csk->flags)) {
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&csk->laddr;

		cxgb4_clip_release(cdev->lldi.ports[0], (const u32 *)&sin6->sin6_addr.s6_addr, 1);
	}

	if (csk->qp)
		csk->qp->csk = NULL;

	dst_release(csk->dst);

	if (csk->l2t)
		cxgb4_l2t_release(csk->l2t);

	free_uevent_node_list(csk);

	if (event_channel) {
		WARN_ON(kref_read(&event_channel->kref) < 1);
		kref_put(&event_channel->kref, _cstor_free_event_channel);
	}

	if (lcsk)
		cstor_put_listen_sock(lcsk);

	cstor_sock_free_skb(csk);
	mutex_destroy(&csk->mutex);
	kfree(csk);
}

static int status2errno(int status)
{
	switch (status) {
	case CPL_ERR_NONE:
		return 0;
	case CPL_ERR_CONN_RESET:
		return -ECONNRESET;
	case CPL_ERR_ARP_MISS:
		return -EHOSTUNREACH;
	case CPL_ERR_CONN_TIMEDOUT:
		return -ETIMEDOUT;
	case CPL_ERR_TCAM_FULL:
		return -ENOMEM;
	case CPL_ERR_CONN_EXIST:
		return -EADDRINUSE;
	default:
		return -EIO;
	}
}

static struct net_device *cstor_get_real_dev(struct net_device *ndev)
{
	if (ndev->priv_flags & IFF_BONDING) {
		cstor_printk(KERN_ERR, "Bond devices are not supported. "
			     "Interface:%s\n", ndev->name);
		return NULL;
	}

	if (is_vlan_dev(ndev))
		return vlan_dev_real_dev(ndev);

	return ndev;
}

static int our_interface(struct cstor_device *cdev, struct net_device *ndev)
{
	int i;

	ndev = cstor_get_real_dev(ndev);
	if (!ndev)
		return 0;

	for (i = 0; i < cdev->lldi.nports; i++)
		if (cdev->lldi.ports[i] == ndev)
			return 1;
	return 0;
}

static struct dst_entry *
find_route6(struct cstor_device *cdev, struct sockaddr_in6 *saddr, __u8 *local_ip,
	    __u8 *peer_ip, __u32 sin6_scope_id)
{
	struct dst_entry *dst = NULL;
#if IS_ENABLED(CONFIG_IPV6)
	struct flowi6 fl6 = {};

	memcpy(&fl6.daddr, peer_ip, 16);
	if (local_ip)
		memcpy(&fl6.saddr, local_ip, 16);

	if (ipv6_addr_type(&fl6.daddr) & IPV6_ADDR_LINKLOCAL)
		fl6.flowi6_oif = sin6_scope_id;

	dst = ip6_route_output(&init_net, NULL, &fl6);
	if (dst->error) {
		cstor_err(cdev, "ip6_route_output() failed, daddr %pI6 sin6_scope_id %u\n",
			  &fl6.daddr, sin6_scope_id);
		dst_release(dst);
		return NULL;
	}

	if (!our_interface(cdev, ip6_dst_idev(dst)->dev) &&
	    !(ip6_dst_idev(dst)->dev->flags & IFF_LOOPBACK)) {
		cstor_err(cdev, "not our interface nor loopback flag, interface name %s flag %u\n",
			  ip6_dst_idev(dst)->dev->name, ip6_dst_idev(dst)->dev->flags);
		dst_release(dst);
		return NULL;
	}

	if (saddr) {
		struct rt6_info *rt = container_of(dst, struct rt6_info, dst);

		if (ipv6_addr_any(&rt->rt6i_src.addr)) {
			struct inet6_dev *idev = ip6_dst_idev((struct dst_entry *)rt);
			int ret;

			ret = ipv6_dev_get_saddr(&init_net, idev ? idev->dev : NULL,
						 &fl6.daddr, 0, &saddr->sin6_addr);
			if (ret) {
				cstor_err(cdev, "failed to get ipv6 source address to "
					  "reach %pI6\n", &fl6.daddr);
				dst_release(dst);
				return NULL;
			}
		} else {
			saddr->sin6_addr = rt->rt6i_src.addr;
		}

		saddr->sin6_family = PF_INET6;
	}
#endif

	return dst;
}

static struct dst_entry *
find_route(struct cstor_device *cdev, struct sockaddr_in *saddr, __be32 local_ip, __be32 peer_ip,
	   __be16 local_port, __be16 peer_port, u8 tos)
{
	struct rtable *rt;
	struct flowi4 fl4;
	struct neighbour *neigh = NULL;

	rt = ip_route_output_ports(&init_net, &fl4, NULL, peer_ip, local_ip,
				   peer_port, local_port, IPPROTO_TCP, tos, 0);
	if (IS_ERR(rt)) {
		cstor_err(cdev, "ip_route_output_ports() failed, laddr %pI4 raddr %pI4 "
			  "lport %d rport %d tos %u\n", &local_ip, &peer_ip,
			  be16_to_cpu(local_port), be16_to_cpu(peer_port), tos);
		return NULL;
	}

	neigh = dst_neigh_lookup(&rt->dst, &peer_ip);
	if (!neigh) {
		cstor_err(cdev, "dst_neigh_lookup() failed raddr %pI4\n", &peer_ip);
		ip_rt_put(rt);
		return NULL;
	}

	if (!our_interface(cdev, neigh->dev) &&
	    !(neigh->dev->flags & IFF_LOOPBACK)) {
		cstor_err(cdev, "not our interface nor loopback flag, interface name %s flag %u\n",
			  neigh->dev->name, neigh->dev->flags);
		neigh_release(neigh);
		ip_rt_put(rt);
		return NULL;
	}

	if (saddr) {
		saddr->sin_family = AF_INET;
		saddr->sin_addr.s_addr = fl4.saddr;
	}

	neigh_release(neigh);
	return &rt->dst;
}

static void arp_failure_discard(void *handle, struct sk_buff *skb)
{
	struct cstor_sock *csk = handle;

	if (csk->laddr.ss_family == AF_INET) {
		struct sockaddr_in *la = (struct sockaddr_in *)&csk->laddr;
		struct sockaddr_in *ra = (struct sockaddr_in *)&csk->raddr;

		cstor_err(csk->uctx->cdev, "ARP failure - tid %u, laddr %pI4 lport %u "
			  "raddr %pI4 rport %u\n", csk->tid, &la->sin_addr.s_addr,
			  be16_to_cpu(la->sin_port), &ra->sin_addr.s_addr,
			  be16_to_cpu(ra->sin_port));
	} else {
		struct sockaddr_in6 *la6 = (struct sockaddr_in6 *)&csk->laddr;
		struct sockaddr_in6 *ra6 = (struct sockaddr_in6 *)&csk->raddr;

		cstor_err(csk->uctx->cdev, "ARP failure - tid %u, laddr %pI6 lport %u "
			  "raddr %pI6 rport %u\n", csk->tid, la6->sin6_addr.s6_addr,
			  be16_to_cpu(la6->sin6_port), ra6->sin6_addr.s6_addr,
			  be16_to_cpu(ra6->sin6_port));
	}

	kfree_skb(skb);
}

static void _put_sock_safe(struct cstor_device *cdev, struct sk_buff *skb)
{
	cstor_put_sock(cstor_skcb_rx_csk(skb));
}

/*
 * Fake up a special CPL opcode and call sched() so process_work() will call
 * _put_csk_safe() in a safe context to free the csk resources.  This is needed
 * because ARP error handlers are called in an ATOMIC context, and _cstor_free_csk()
 * needs to block.
 */
static void queue_arp_failure_cpl(struct cstor_sock *csk, struct sk_buff *skb, int cpl)
{
	struct cpl_act_establish *rpl = cplhdr(skb);

	/*
	 * Set our special ARP_FAILURE opcode.
	 */
	rpl->ot.opcode = cpl;

	/*
	 * Save csk in the skb->cb area, after where sched() will save the dev ptr.
	 */
	cstor_skcb_rx_csk(skb) = csk;
	sched(csk->uctx->cdev, skb);
}

/*
 * Handle an ARP failure for an accept.
 */
static void pass_accept_rpl_arp_failure(void *handle, struct sk_buff *skb)
{
	struct cstor_sock *csk = handle;

	if (csk->laddr.ss_family == AF_INET) {
		struct sockaddr_in *la = (struct sockaddr_in *)&csk->laddr;
		struct sockaddr_in *ra = (struct sockaddr_in *)&csk->raddr;

		cstor_err(csk->uctx->cdev, "ARP failure during accept - dropping connection, "
			  "tid %u, laddr %pI4 lport %u raddr %pI4 rport %u\n", csk->tid,
			  &la->sin_addr.s_addr, be16_to_cpu(la->sin_port),
			  &ra->sin_addr.s_addr, be16_to_cpu(ra->sin_port));
	} else {
		struct sockaddr_in6 *la6 = (struct sockaddr_in6 *)&csk->laddr;
		struct sockaddr_in6 *ra6 = (struct sockaddr_in6 *)&csk->raddr;

		cstor_err(csk->uctx->cdev, "ARP failure during accept - dropping connection, "
			  "tid %u, laddr %pI6 lport %u raddr %pI6 rport %u\n", csk->tid,
			  la6->sin6_addr.s6_addr, be16_to_cpu(la6->sin6_port),
			  ra6->sin6_addr.s6_addr, be16_to_cpu(ra6->sin6_port));
	}

	csk->state = CSTOR_SOCK_STATE_DEAD;
	queue_arp_failure_cpl(csk, skb, FAKE_CPL_PUT_SOCK_SAFE);
}

/*
 * Handle an ARP failure for a CPL_ABORT_REQ.  Change it into a no RST variant
 * and send it along.
 */
static void abort_arp_failure(void *handle, struct sk_buff *skb)
{
	struct cstor_device *cdev;
	struct cstor_sock *csk = handle;
	struct cpl_abort_req *req = cplhdr(skb);
	int ret;

	cdev = csk->uctx->cdev;
	if (csk->laddr.ss_family == AF_INET) {
		struct sockaddr_in *la = (struct sockaddr_in *)&csk->laddr;
		struct sockaddr_in *ra = (struct sockaddr_in *)&csk->raddr;

		cstor_err(cdev, "ARP failure during abort - tid %u, laddr %pI4 lport %u "
			  "raddr %pI4 rport %u\n", csk->tid, &la->sin_addr.s_addr,
			  be16_to_cpu(la->sin_port), &ra->sin_addr.s_addr,
			  be16_to_cpu(ra->sin_port));
	} else {
		struct sockaddr_in6 *la6 = (struct sockaddr_in6 *)&csk->laddr;
		struct sockaddr_in6 *ra6 = (struct sockaddr_in6 *)&csk->raddr;

		cstor_err(cdev, "ARP failure during abort - tid %u, laddr %pI6 lport %u "
			  "raddr %pI6 rport %u\n", csk->tid, la6->sin6_addr.s6_addr,
			  be16_to_cpu(la6->sin6_port), ra6->sin6_addr.s6_addr,
			  be16_to_cpu(ra6->sin6_port));
	}

	req->cmd = CPL_ABORT_NO_RST;
	skb_get(skb);
	ret = cstor_ofld_send(cdev, skb);
	if (!ret) {
		kfree_skb(skb);
		return;
	}

	csk->state = CSTOR_SOCK_STATE_DEAD;
	queue_arp_failure_cpl(csk, skb, FAKE_CPL_PUT_SOCK_SAFE);
}

#define FLOWC_WR_NPARAMS_MIN 9

static int send_flowc(struct cstor_sock *csk)
{
	struct cstor_device *cdev = csk->uctx->cdev;
	struct sk_buff *skb = skb_dequeue(&csk->skb_list);
	struct fw_flowc_wr *flowc;
	int nparams = FLOWC_WR_NPARAMS_MIN;
	int flowclen, flowclen16, ret;
	u16 vlan = csk->l2t->vlan;
	u8 port_chan, index;

	if (csk->snd_wscale)
		nparams++;

#ifdef CONFIG_CHELSIO_T4_DCB
	nparams++;
#else
	if (vlan != CPL_L2T_VLAN_NONE)
		nparams++;
#endif

	flowclen = offsetof(struct fw_flowc_wr, mnemval[nparams]);
	flowclen16 = DIV_ROUND_UP(flowclen, 16);
	flowclen = flowclen16 * 16;

	csk->wr_cred -= flowclen16;

	flowc = (struct fw_flowc_wr *)__skb_put(skb, flowclen);
	memset(flowc, 0, flowclen);

	flowc->op_to_nparams = cpu_to_be32(FW_WR_OP_V(FW_FLOWC_WR) |
					   FW_WR_COMPL_F | FW_FLOWC_WR_NPARAMS_V(nparams));
	flowc->flowid_len16 = cpu_to_be32(FW_WR_LEN16_V(flowclen16) |
					  FW_WR_FLOWID_V(csk->tid));

	flowc->mnemval[0].mnemonic = FW_FLOWC_MNEM_PFNVFN;
	flowc->mnemval[0].val = cpu_to_be32(FW_PFVF_CMD_PFN_V(cdev->lldi.pf));
	flowc->mnemval[1].mnemonic = FW_FLOWC_MNEM_CH;
	port_chan = cxgb4_port_chan(cdev->lldi.ports[csk->port_id]);
	flowc->mnemval[1].val = cpu_to_be32(port_chan);
	flowc->mnemval[2].mnemonic = FW_FLOWC_MNEM_PORT;
	flowc->mnemval[2].val = cpu_to_be32(port_chan);
	flowc->mnemval[3].mnemonic = FW_FLOWC_MNEM_IQID;
	flowc->mnemval[3].val = cpu_to_be32(csk->rss_qid);
	flowc->mnemval[4].mnemonic = FW_FLOWC_MNEM_SNDNXT;
	flowc->mnemval[4].val = cpu_to_be32(csk->snd_nxt);
	flowc->mnemval[5].mnemonic = FW_FLOWC_MNEM_RCVNXT;
	flowc->mnemval[5].val = cpu_to_be32(csk->rcv_nxt);
	flowc->mnemval[6].mnemonic = FW_FLOWC_MNEM_SNDBUF;
	flowc->mnemval[6].val = cpu_to_be32(csk->snd_win);
	flowc->mnemval[7].mnemonic = FW_FLOWC_MNEM_MSS;
	flowc->mnemval[7].val = cpu_to_be32(csk->emss);
	flowc->mnemval[8].mnemonic = FW_FLOWC_MNEM_TXDATAPLEN_MAX;
	flowc->mnemval[8].val = cpu_to_be32(65535);

	index = 9;

	if (csk->snd_wscale) {
		flowc->mnemval[index].mnemonic = FW_FLOWC_MNEM_RCV_SCALE;
		flowc->mnemval[index++].val = cpu_to_be32(csk->snd_wscale);
	}

#ifdef CONFIG_CHELSIO_T4_DCB
	flowc->mnemval[index].mnemonic = FW_FLOWC_MNEM_DCBPRIO;
	if (vlan == CPL_L2T_VLAN_NONE) {
		cstor_warn(cdev, "csk tid %u without VLAN Tag on DCB Link\n", csk->tid);
		flowc->mnemval[index].val = cpu_to_be32(0);
	} else {
		flowc->mnemval[index].val =
			cpu_to_be32((vlan & VLAN_PRIO_MASK) >> VLAN_PRIO_SHIFT);
	}
#else
	if (vlan != CPL_L2T_VLAN_NONE) {
		u16 pri = (vlan & VLAN_PRIO_MASK) >> VLAN_PRIO_SHIFT;

		flowc->mnemval[index].mnemonic = FW_FLOWC_MNEM_SCHEDCLASS;
		flowc->mnemval[index].val = cpu_to_be32(pri);
	}
#endif

	set_wr_txq(skb, CPL_PRIORITY_DATA, csk->txq_idx);
	ret = cstor_ofld_send(cdev, skb);
	if (ret) {
		cstor_err(cdev, "tid %u, cstor_ofld_send() failed ret %d, disabling device\n",
			  csk->tid, ret);
		cstor_disable_device(cdev);
	} else {
		set_bit(CSTOR_SOCK_F_FLOWC_SENT, &csk->flags);
	}

	return ret;
}

static int send_abort_req(struct cstor_sock *csk)
{
	struct cstor_device *cdev = csk->uctx->cdev;
	struct sk_buff *req_skb = skb_dequeue(&csk->skb_list);
	struct cpl_abort_req *req;
	int wrlen = roundup(sizeof(*req), 16);

	cstor_debug(cdev, "tid %u\n", csk->tid);
	if (WARN_ON(!req_skb)) {
		cstor_err(cdev, "tid %u, socket buffer list is empty\n", csk->tid);
		return -ENOMEM;
	}

	set_wr_txq(req_skb, CPL_PRIORITY_DATA, csk->txq_idx);
	t4_set_arp_err_handler(req_skb, csk, abort_arp_failure);
	req = (struct cpl_abort_req *)skb_put(req_skb, wrlen);
	memset(req, 0, wrlen);
	INIT_TP_WR(req, csk->tid);
	OPCODE_TID(req) = cpu_to_be32(MK_OPCODE_TID(CPL_ABORT_REQ, csk->tid));
	req->cmd = CPL_ABORT_SEND_RST;
	return cstor_l2t_send(cdev, req_skb, csk);
}

static void best_mtu(struct cstor_sock *csk, u32 *idx, u8 tstamp)
{
	const u16 *mtus = csk->uctx->cdev->lldi.mtus;
	u16 hdr_size = cstor_get_hdr_size(csk, tstamp);
	u16 data_size = csk->mtu - hdr_size;

	cxgb4_best_aligned_mtu(mtus, hdr_size, data_size, 8, idx);
}

void
add_to_uevt_list(struct cstor_event_channel *event_channel, struct cstor_uevent_node *uevt_node)
{
	mutex_lock(&event_channel->uevt_list_lock);
	list_add_tail(&uevt_node->entry, &event_channel->uevt_list);
	mutex_unlock(&event_channel->uevt_list_lock);

	eventfd_signal(event_channel->efd_ctx);
}

static void disconnected_upcall(struct cstor_sock *csk)
{
	struct cstor_device *cdev = csk->uctx->cdev;
	struct cstor_event_channel *event_channel = csk->event_channel;
	struct cstor_uevent_node *uevt_node;
	struct cstor_uevent *uevt;

	if (!test_bit(CSTOR_SOCK_F_ACT_OPEN, &csk->flags)) {
		if (!test_bit(CSTOR_SOCK_F_ACCEPT, &csk->flags)) {
			cstor_err(cdev, "tid %u, invalid flag\n", csk->tid);
			return;
		}
	}

	if (test_and_set_bit(CSTOR_SOCK_F_DISCONNECT_UPCALL, &csk->flags)) {
		cstor_err(cdev, "tid %u, disconnected event already added\n", csk->tid);
		return;
	}

	set_bit(CSTOR_SOCK_H_DISCONNECTED_UPCALL, &csk->history);

	uevt_node = list_first_entry_or_null(&csk->uevt_list, struct cstor_uevent_node, entry);
	if (!uevt_node) {
		cstor_err(cdev, "tid %u, uevent list is empty\n", csk->tid);
		return;
	}

	list_del_init(&uevt_node->entry);

	uevt = &uevt_node->uevt;
	uevt->event = CSTOR_UEVENT_DISCONNECTED;
	uevt->u.tid = csk->tid;

	add_to_uevt_list(event_channel, uevt_node);
}

static void connect_upcall(struct cstor_sock *csk, struct sk_buff *skb)
{
	struct cstor_listen_sock *lcsk = csk->lcsk;
	struct cstor_event_channel *event_channel = csk->event_channel;
	struct cstor_uevent_node *uevt_node =
		list_first_entry_or_null(&csk->uevt_list, struct cstor_uevent_node, entry);
	struct cstor_uevent *uevt;
	struct _cstor_connect_req *req;

	list_del_init(&uevt_node->entry);

	uevt = &uevt_node->uevt;
	uevt->event = CSTOR_UEVENT_CONNECT_REQ;
	req = &uevt->u.req;
	req->stid = lcsk->stid;
	req->tid = csk->tid;
	req->vlan_id = csk->l2t->vlan;
	req->port_id = csk->port_id;
	req->protocol = csk->protocol;
	if (csk->laddr.ss_family == AF_INET) {
		struct sockaddr_in *sin = (struct sockaddr_in *)&csk->laddr;

		req->ipv4 = 1;
		req->lport = sin->sin_port;
		req->laddr[0] = sin->sin_addr.s_addr;

		sin = (struct sockaddr_in *)&csk->raddr;
		req->rport = sin->sin_port;
		req->raddr[0] = sin->sin_addr.s_addr;
	} else {
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&csk->laddr;

		req->lport = sin6->sin6_port;
		memcpy(req->laddr, sin6->sin6_addr.s6_addr, 16);

		sin6 = (struct sockaddr_in6 *)&csk->raddr;
		req->rport = sin6->sin6_port;
		memcpy(req->raddr, sin6->sin6_addr.s6_addr, 16);
	}

	uevt_node->skb = skb;
	req->rcv_nxt = csk->rcv_nxt;

	cstor_get_sock(csk);
	set_bit(CSTOR_SOCK_F_APP_REF, &csk->flags);
	set_bit(CSTOR_SOCK_H_CONNREQ_UPCALL, &csk->history);

	add_to_uevt_list(event_channel, uevt_node);
}

static void active_connect_upcall(struct cstor_sock *csk, u8 status)
{
	struct cstor_event_channel *event_channel = csk->event_channel;
	struct cstor_uevent_node *uevt_node =
		list_first_entry_or_null(&csk->uevt_list, struct cstor_uevent_node, entry);
	struct cstor_uevent *uevt;
	struct _cstor_connect_rpl *rpl;

	list_del_init(&uevt_node->entry);

	uevt = &uevt_node->uevt;
	uevt->event = CSTOR_UEVENT_CONNECT_RPL;
	rpl = &uevt->u.rpl;
	rpl->tid = csk->tid;
	rpl->atid = csk->atid;
	rpl->vlan_id = csk->l2t->vlan;
	rpl->port_id = csk->port_id;
	rpl->status = status;
	if (csk->laddr.ss_family == AF_INET) {
		struct sockaddr_in *sin = (struct sockaddr_in *)&csk->laddr;

		rpl->ipv4 = 1;
		rpl->lport = sin->sin_port;
		rpl->laddr[0] = sin->sin_addr.s_addr;

		sin = (struct sockaddr_in *)&csk->raddr;
		rpl->rport = sin->sin_port;
		rpl->raddr[0] = sin->sin_addr.s_addr;
	} else {
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&csk->laddr;

		rpl->lport = sin6->sin6_port;
		memcpy(rpl->laddr, sin6->sin6_addr.s6_addr, 16);

		sin6 = (struct sockaddr_in6 *)&csk->raddr;
		rpl->rport = sin6->sin6_port;
		memcpy(rpl->raddr, sin6->sin6_addr.s6_addr, 16);
	}

	rpl->rcv_nxt = csk->rcv_nxt;

	set_bit(CSTOR_SOCK_F_ACT_OPEN, &csk->flags);
	set_bit(CSTOR_SOCK_H_CONN_RPL_UPCALL, &csk->history);

	add_to_uevt_list(event_channel, uevt_node);
}

static int iscsi_pdu_upcall(struct cstor_sock *csk, struct sk_buff *skb, u32 status)
{
	struct cstor_event_channel *event_channel = csk->event_channel;
	struct cstor_uevent_node *uevt_node;
	struct cstor_uevent *uevt;
	struct _cstor_iscsi_pdu_info *pdu_info;

	if (csk->state != CSTOR_SOCK_STATE_CONNECTED) {
		cstor_err(csk->uctx->cdev, "tid %u, invalid state : %s (%d)\n",
			  csk->tid, states[csk->state], csk->state);
		return -EINVAL;
	}

	uevt_node = kzalloc(sizeof(*uevt_node), GFP_KERNEL);
	if (!uevt_node) {
		cstor_err(csk->uctx->cdev, "tid %u, failed to allocate uevt_node\n", csk->tid);
		return -ENOMEM;
	}

	INIT_LIST_HEAD(&uevt_node->entry);

	uevt = &uevt_node->uevt;
	uevt->event = CSTOR_UEVENT_RECV_ISCSI_PDU;

	pdu_info = &uevt->u.pdu_info;
	pdu_info->tid = csk->tid;
	pdu_info->status = status;
	pdu_info->hlen = skb->len - skb->data_len;
	pdu_info->pdu_len = skb->len;

	uevt_node->skb = skb;
	add_to_uevt_list(event_channel, uevt_node);

	return 0;
}

#if 0
void cstor_hexdump(void *buf, u32 len)
{
	u32 *ptr = buf;
	u32 i, count = len >> 4;

	cstor_printk(KERN_INFO, "len %u\n", len);

	for (i = 0; i < count; i++) {
		cstor_printk(KERN_INFO, "%p: %08x %08x %08x %08x\n",
			     ptr, be32_to_cpu(ptr[0]), be32_to_cpu(ptr[1]),
			     be32_to_cpu(ptr[2]), be32_to_cpu(ptr[3]));
		ptr += 4;
	}

	if ((len % 16) == 8)
		cstor_printk(KERN_INFO, "%p: %08x %08x\n",
			     ptr, be32_to_cpu(ptr[0]), be32_to_cpu(ptr[1]));
}
#endif

static void rx_data(struct cstor_device *cdev, struct sk_buff *skb)
{
	struct cstor_sock *csk;
	struct cpl_rx_data *hdr = cplhdr(skb);
	u32 tid = GET_TID(hdr);
	__u8 status = hdr->status;

	csk = get_sock_from_tid(cdev, tid);
	if (!csk) {
		cstor_err(cdev, "tid %u, get_sock_from_tid() failed\n", tid);
		return;
	}

	mutex_lock(&csk->mutex);

	cstor_err(cdev, "tid %u state %s (%d) status %d - unexpected streaming data\n",
		  csk->tid, states[csk->state], csk->state, status);

	if (csk->qp)
		cstor_modify_qp(csk->qp, CSTOR_QP_STATE_ERROR);
	mutex_unlock(&csk->mutex);

	cstor_sock_disconnect(csk);
}

/*
 * Handle an ARP failure for an active open.
 */
static void act_open_req_arp_failure(void *handle, struct sk_buff *skb)
{
	struct cstor_sock *csk = handle;

	if (csk->laddr.ss_family == AF_INET) {
		struct sockaddr_in *la = (struct sockaddr_in *)&csk->laddr;
		struct sockaddr_in *ra = (struct sockaddr_in *)&csk->raddr;

		cstor_err(csk->uctx->cdev, "ARP failure during connect - atid %u, "
			  "laddr %pI4 lport %u raddr %pI4 rport %u\n", csk->atid,
			  &la->sin_addr.s_addr, be16_to_cpu(la->sin_port),
			  &ra->sin_addr.s_addr, be16_to_cpu(ra->sin_port));
	} else {
		struct sockaddr_in6 *la6 = (struct sockaddr_in6 *)&csk->laddr;
		struct sockaddr_in6 *ra6 = (struct sockaddr_in6 *)&csk->raddr;

		cstor_err(csk->uctx->cdev, "ARP failure during connect - atid %u, "
			  "laddr %pI6 lport %u raddr %pI6 rport %u\n", csk->atid,
			  la6->sin6_addr.s6_addr, be16_to_cpu(la6->sin6_port),
			  ra6->sin6_addr.s6_addr, be16_to_cpu(ra6->sin6_port));
	}

	atomic_dec(&csk->uctx->num_active_csk);

	active_connect_upcall(csk, _CSTOR_CONNECT_FAILURE);

	csk->state = CSTOR_SOCK_STATE_DEAD;
	queue_arp_failure_cpl(csk, skb, FAKE_CPL_PUT_SOCK_SAFE);
}

static void abort_rpl(struct cstor_device *cdev, struct sk_buff *skb)
{
	struct cstor_sock *csk;
	struct cpl_abort_rpl_rss6 *rpl = cplhdr(skb);
	u32 tid = GET_TID(rpl);
	int release = 0;

	csk = get_sock_from_tid(cdev, tid);
	if (!csk) {
		cstor_err(cdev, "tid %u, get_sock_from_tid() failed\n", tid);
		return;
	}

	cstor_debug(cdev, "tid %u\n", csk->tid);
	mutex_lock(&csk->mutex);
	switch (csk->state) {
	case CSTOR_SOCK_STATE_ABORTING:
		cstor_wake_up(&csk->wr_wait, -ECONNRESET);
		csk->state = CSTOR_SOCK_STATE_DEAD;
		if (test_bit(CSTOR_SOCK_F_NEG_ADV_DISCONNECT, &csk->flags))
			active_connect_upcall(csk, _CSTOR_CONNECT_FAILURE);
		else
			disconnected_upcall(csk);

		release = 1;
		break;
	default:
		cstor_err(cdev, "tid %u state %s (%d)\n",
			  csk->tid, states[csk->state], csk->state);
		break;
	}
	mutex_unlock(&csk->mutex);

	if (release)
		cstor_put_sock(csk);
}

/*
 * Returns whether an ABORT_REQ_RSS message is a negative advice.
 */
static int is_neg_adv(u32 status)
{
	return (status == CPL_ERR_RTX_NEG_ADVICE) ||
	       (status == CPL_ERR_PERSIST_NEG_ADVICE) ||
	       (status == CPL_ERR_KEEPALV_NEG_ADVICE);
}

static char *neg_adv_str(u32 status)
{
	switch (status) {
	case CPL_ERR_RTX_NEG_ADVICE:
		return "Retransmit timeout";
	case CPL_ERR_PERSIST_NEG_ADVICE:
		return "Persist timeout";
	case CPL_ERR_KEEPALV_NEG_ADVICE:
		return "Kecskalive timeout";
	default:
		return "Unknown";
	}
}

static void set_tcp_window(struct cstor_sock *csk, struct port_info *pi)
{
	csk->snd_win = snd_win;
	csk->rcv_win = rcv_win;
	if (adjust_win && (pi->link_cfg.speed >= 40000)) {
		csk->snd_win *= 4;
		csk->rcv_win *= 4;
	}

	cstor_debug(csk->uctx->cdev, "snd_win %d rcv_win %d\n", csk->snd_win, csk->rcv_win);
}

#ifdef CONFIG_CHELSIO_T4_DCB
static u8 cstor_get_dcb_state(struct net_device *ndev)
{
	return ndev->dcbnl_ops->getstate(ndev);
}

static int cstor_select_priority(int pri_mask)
{
	if (!pri_mask)
		return 0;

	return (ffs(pri_mask) - 1);
}

static u8 cstor_get_dcb_priority(struct net_device *ndev, u16 dcb_port)
{
	int ret;
	u8 caps;

	struct dcb_app cstor_dcb_app = {
		.protocol = dcb_port
	};

	ret = (int)ndev->dcbnl_ops->getcap(ndev, DCB_CAP_ATTR_DCBX, &caps);
	if (ret)
		return 0;

	if (caps & DCB_CAP_DCBX_VER_IEEE) {
		cstor_dcb_app.selector = IEEE_8021QAZ_APP_SEL_STREAM;
		ret = dcb_ieee_getapp_mask(ndev, &cstor_dcb_app);
		if (!ret) {
			cstor_dcb_app.selector = IEEE_8021QAZ_APP_SEL_ANY;
			ret = dcb_ieee_getapp_mask(ndev, &cstor_dcb_app);
		}
	} else if (caps & DCB_CAP_DCBX_VER_CEE) {
		cstor_dcb_app.selector = DCB_APP_IDTYPE_PORTNUM;
		ret = dcb_getapp(ndev, &cstor_dcb_app);
	}

	cstor_printk(KERN_DEBUG, "priority is set to %u\n", cstor_select_priority(ret));

	return cstor_select_priority(ret);
}
#endif

static struct net_device *cstor_ipv4_netdev(__be32 saddr)
{
	struct net_device *ndev;

	ndev = __ip_dev_find(&init_net, saddr, false);
	if (!ndev)
		return NULL;

	return cstor_get_real_dev(ndev);
}

static struct net_device *cstor_ipv6_netdev(struct in6_addr *addr6)
{
	struct net_device *ndev = NULL;
	bool found = false;

#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	for_each_netdev_rcu(&init_net, ndev) {
		if (ipv6_chk_addr(&init_net, addr6, ndev, 1)) {
			found = true;
			break;
		}
	}
#endif

	if (!found)
		return NULL;

	return cstor_get_real_dev(ndev);
}

static int import_sock(struct cstor_sock *csk, int iptype, __u8 *peer_ip, u16 dcb_port, u8 tos)
{
	struct cstor_device *cdev = csk->uctx->cdev;
	struct net_device *ndev = NULL;
	struct port_info *pi;
	struct neighbour *n;
	int ret, step;
	u8 priority = rt_tos2priority(tos);

	n = dst_neigh_lookup(csk->dst, peer_ip);
	if (!n) {
		cstor_err(cdev, "dst_neigh_lookup() failed, raddr %pI4\n", peer_ip);
		return -ENODEV;
	}

	if (!(n->nud_state & NUD_VALID))
		neigh_event_send(n, NULL);

	rcu_read_lock();
	if (n->dev->flags & IFF_LOOPBACK) {
		if (iptype == 4)
			ndev = cstor_ipv4_netdev(*(__be32 *)peer_ip);
		else
			ndev = cstor_ipv6_netdev((struct in6_addr *)peer_ip);

		if (!ndev) {
			cstor_err(cdev, "net device not found\n");
			ret = -ENODEV;
			goto out;
		}

		csk->mtu = ndev->mtu;
	} else {
		ndev = cstor_get_real_dev(n->dev);
		if (!ndev) {
			cstor_err(cdev, "net device not found\n");
			ret = -ENODEV;
			goto out;
		}

#ifdef CONFIG_CHELSIO_T4_DCB
		if (cstor_get_dcb_state(ndev))
			priority = cstor_get_dcb_priority(ndev, dcb_port);

		csk->dcb_priority = priority;
#endif
		csk->mtu = dst_mtu(csk->dst);
	}

	csk->l2t = cxgb4_l2t_get(cdev->lldi.l2t, n, ndev, priority);
	if (!csk->l2t) {
		cstor_err(cdev, "cxgb4_l2t_get() failed\n");
		ret = -ENOMEM;
		goto out;
	}

	csk->port_id = cxgb4_port_idx(ndev);
	ret = cstor_alloc_txq(csk);
	if (ret < 0) {
		cstor_err(cdev, "cstor_alloc_txq() failed, ret %d\n", ret);
		goto out;
	}

	step = cdev->lldi.nrxq / cdev->lldi.nchan;
	pi = (struct port_info *)netdev_priv(ndev);
	csk->tx_chan = cxgb4_port_tx_chan(ndev);
	csk->rx_chan = cxgb4_port_e2cchan(ndev);
	csk->smac_idx = pi->smt_idx;
	csk->ctrlq_idx = cdev->lldi.ctrlq_start +
			 (csk->port_id * cdev->lldi.num_up_cores);
	csk->rss_qid = cdev->lldi.rxq_ids[csk->port_id * step];
	set_tcp_window(csk, pi);

	BUG_ON(!csk->smac_idx);
out:
	rcu_read_unlock();
	neigh_release(n);
	return ret;
}

static void pass_open_rpl(struct cstor_device *cdev, struct sk_buff *skb)
{
	struct cstor_listen_sock *lcsk;
	struct cpl_pass_open_rpl *rpl = cplhdr(skb);
	u32 stid = GET_TID(rpl);

	lcsk = get_listen_sock_from_stid(cdev, stid);
	if (!lcsk) {
		cstor_err(cdev, "stid %u lookup failure!\n", stid);
		return;
	}

	cstor_debug(cdev, "stid %u status %d error %d\n", stid,
		    rpl->status, status2errno(rpl->status));
	cstor_wake_up(&lcsk->wr_wait, status2errno(rpl->status));
}

static void close_listsrv_rpl(struct cstor_device *cdev, struct sk_buff *skb)
{
	struct cstor_listen_sock *lcsk;
	struct cpl_close_listsvr_rpl *rpl = cplhdr(skb);
	u32 stid = GET_TID(rpl);

	lcsk = get_listen_sock_from_stid(cdev, stid);
	if (!lcsk) {
		cstor_err(cdev, "stid %u lookup failure!\n", stid);
		return;
	}

	cstor_debug(cdev, "stid %u status %d error %d\n",
		    stid, rpl->status, status2errno(rpl->status));

	if (lcsk->app_close) {
		struct cstor_ucontext *uctx = lcsk->uctx;

		cstor_put_listen_sock(lcsk);
		uctx->lcsk_close_pending--;
		if (!uctx->lcsk_close_pending)
			cstor_wake_up(&cdev->wr_wait, status2errno(rpl->status));
	} else {
		cstor_wake_up(&lcsk->wr_wait, status2errno(rpl->status));
	}
}

static int
accept_cr(struct cstor_sock *csk, struct sk_buff *skb, struct cpl_pass_accept_req *req)
{
	struct cstor_device *cdev = csk->uctx->cdev;
	struct cpl_t5_pass_accept_rpl *rpl = NULL;
	u64 opt0;
	u32 opt2;
	u32 win;
	u32 isn = (get_random_u32() & ~7UL) - 1;
	u32 mtu_idx;
	int wscale = compute_wscale(csk->rcv_win);

	best_mtu(csk, &mtu_idx, (enable_tcp_timestamps && req->tcpopt.tstamp));

	/*
	 * Specify the largest window that will fit in opt0. The
	 * remainder will be specified in the rx_data_ack.
	 */
	win = min_t(u32, csk->rcv_win >> 10, RCV_BUFSIZ_M);

	opt0 = (nocong ? NO_CONG_F : 0) |
	       DELACK_F |
	       TCAM_BYPASS_F |
	       KEEP_ALIVE_F |
	       WND_SCALE_V(wscale) |
	       MSS_IDX_V(mtu_idx) |
	       L2T_IDX_V(csk->l2t->idx) |
	       TX_CHAN_V(csk->tx_chan) |
	       SMAC_SEL_V(csk->smac_idx) |
	       DSCP_V(csk->tos >> 2) |
	       RCV_BUFSIZ_V(win);

	if (csk->protocol == _CSTOR_ISCSI_PROTOCOL)
		opt0 |= ULP_MODE_V(ULP_MODE_ISCSI);
	else
		opt0 |= ULP_MODE_V(ULP_MODE_NVMET);

	opt2 = T5_OPT_2_VALID_F | PACE_V(1) | T5_ISS_F |
	       TX_QUEUE_V(cdev->lldi.tx_modq[csk->tx_chan]) |
	       RSS_QUEUE_VALID_F | RSS_QUEUE_V(csk->rss_qid) |
	       RX_FC_DISABLE_F | CONG_CNTRL_V(CONG_ALG_NEWRENO);

	if (enable_tcp_timestamps && req->tcpopt.tstamp)
		opt2 |= TSTAMPS_EN_F;

	if (enable_tcp_sack && req->tcpopt.sack)
		opt2 |= SACK_EN_F;

	if (wscale && enable_tcp_window_scaling)
		opt2 |= WND_SCALE_EN_F;

	if (enable_ecn) {
		const struct tcphdr *tcph;
		u32 hlen = be32_to_cpu(req->hdr_len);

		tcph = (const void *)(req + 1) + T6_ETH_HDR_LEN_G(hlen) +
		       T6_IP_HDR_LEN_G(hlen);

		if (tcph->ece && tcph->cwr)
			opt2 |= CCTRL_ECN_F;
	}

	skb_get(skb);
	rpl = cplhdr(skb);
	skb_trim(skb, roundup(sizeof(*rpl), 16));
	INIT_TP_WR(rpl, csk->tid);
	memset_after(rpl, 0, iss);

	OPCODE_TID(rpl) = cpu_to_be32(MK_OPCODE_TID(CPL_PASS_ACCEPT_RPL, csk->tid));

	rpl->iss = cpu_to_be32(isn);
	rpl->opt0 = cpu_to_be64(opt0);
	rpl->opt2 = cpu_to_be32(opt2);
	set_wr_txq(skb, CPL_PRIORITY_SETUP, csk->ctrlq_idx);
	t4_set_arp_err_handler(skb, csk, pass_accept_rpl_arp_failure);

	cstor_debug(cdev, "tid %u iss %u\n", csk->tid, cpu_to_be32(rpl->iss));
	return cstor_l2t_send(cdev, skb, csk);
}

static void reject_cr(struct cstor_device *cdev, u32 tid, u16 port_id, struct sk_buff *skb)
{
	cstor_debug(cdev, "rejecting connection request for tid %u\n", tid);
	release_tid(cdev, tid, port_id, skb);
}

static void
get_4tuple(struct cstor_sock *csk, struct cpl_pass_accept_req *req, int *iptype,
	   u8 *local_ip, u8 *peer_ip, __be16 *local_port, __be16 *peer_port)
{
	struct iphdr *ip;
	struct tcphdr *tcp;
	int eth_len = T6_ETH_HDR_LEN_G(be32_to_cpu(req->hdr_len));
	int ip_len = T6_IP_HDR_LEN_G(be32_to_cpu(req->hdr_len));

	ip = (struct iphdr *)((u8 *)(req + 1) + eth_len);
	tcp = (struct tcphdr *)((u8 *)ip + ip_len);
	if (ip->version == 4) {
		*iptype = 4;
		memcpy(peer_ip, &ip->saddr, 4);
		memcpy(local_ip, &ip->daddr, 4);
	} else {
		struct ipv6hdr *ip6 = (struct ipv6hdr *)ip;

		*iptype = 6;
		memcpy(peer_ip, ip6->saddr.s6_addr, 16);
		memcpy(local_ip, ip6->daddr.s6_addr, 16);
	}

	*peer_port = tcp->source;
	*local_port = tcp->dest;
}

static void pass_accept_req(struct cstor_device *cdev, struct sk_buff *skb)
{
	struct cstor_ucontext *uctx;
	struct cstor_listen_sock *lcsk;
	struct cstor_sock *csk = NULL;
	struct dst_entry *dst;
	struct cpl_pass_accept_req *req = cplhdr(skb);
	u32 stid = PASS_OPEN_TID_G(be32_to_cpu(req->tos_stid));
	u32 tid = GET_TID(req);
	int iptype, ret;
	__be16 local_port, peer_port;
	unsigned short hdrs;
	u16 peer_mss = be16_to_cpu(req->tcpopt.mss);
	u16 port_id = SYN_INTF_G(be16_to_cpu(req->l2info));
	u8 tos = PASS_OPEN_TOS_G(be32_to_cpu(req->tos_stid));
	__u8 local_ip[16], peer_ip[16];

	lcsk = get_listen_sock_from_stid(cdev, stid);
	if (!lcsk) {
		cstor_err(cdev, "connect request on invalid stid %u\n", stid);
		goto reject;
	}
	uctx = lcsk->uctx;

	mutex_lock(&lcsk->mutex);
	if (!lcsk->listen) {
		mutex_unlock(&lcsk->mutex);
		cstor_err(cdev, "stid %u listening csk not in LISTEN\n", stid);
		goto reject;
	}
	mutex_unlock(&lcsk->mutex);

	csk = alloc_sock(lcsk->uctx, lcsk);
	if (!csk) {
		cstor_err(cdev, "stid %u, failed to allocate csk entry!\n", stid);
		goto reject;
	}

	get_4tuple(csk, req, &iptype, local_ip, peer_ip, &local_port, &peer_port);

	/* Find output route */
	if (iptype == 4) {
		cstor_debug(cdev, "tid %u stid %u laddr %pI4 raddr %pI4 lport %d "
			    "rport %d peer_mss %d\n", tid, stid, local_ip, peer_ip,
			    be16_to_cpu(local_port), be16_to_cpu(peer_port), peer_mss);

		dst = find_route(cdev, NULL, *(__be32 *)local_ip, *(__be32 *)peer_ip,
				 local_port, peer_port, tos);
	} else {
		cstor_debug(cdev, "tid %u stid %u laddr %pI6 raddr %pI6 lport%d "
			    "rport %d peer_mss %d\n", tid, stid, local_ip, peer_ip,
			    be16_to_cpu(local_port), be16_to_cpu(peer_port), peer_mss);

		dst = find_route6(cdev, NULL, local_ip, peer_ip,
				  ((struct sockaddr_in6 *)&lcsk->laddr)->sin6_scope_id);
	}

	if (!dst) {
		if (iptype == 4)
			cstor_err(cdev, "find_route() failed, tid %u stid %u laddr %pI4 "
				  "raddr %pI4 lport %d rport %d\n", tid, stid, local_ip, peer_ip,
				  be16_to_cpu(local_port), be16_to_cpu(peer_port));
		else
			cstor_err(cdev, "find_route6() failed, tid %u stid %u laddr %pI6 "
				  "raddr %pI6 lport %d rport %d\n", tid, stid, local_ip, peer_ip,
				  be16_to_cpu(local_port), be16_to_cpu(peer_port));
		goto reject;
	}

	csk->dst = dst;

	ret = import_sock(csk, iptype, peer_ip, be16_to_cpu(local_port), tos);
	if (ret) {
		if (iptype == 4)
			cstor_err(cdev, "import_sock() failed, tid %u stid %u raddr %pI4 "
				  "lport %d tos %u ret %d\n", tid, stid, peer_ip,
				  be16_to_cpu(local_port), tos, ret);
		else
			cstor_err(cdev, "import_sock() failed, tid %u stid %u raddr %pI6 "
				  "lport %d tos %u ret %d\n", tid, stid, peer_ip,
				  be16_to_cpu(local_port), tos, ret);

		goto reject;
	}

	csk->state = CSTOR_SOCK_STATE_CONNECTING;

	cxgb4_uld_tid_ctrlq_id_sel_update(cdev->lldi.ports[0], tid, &csk->ctrlq_idx);
	cxgb4_uld_tid_qid_sel_update(cdev->lldi.ports[csk->port_id], CXGB4_ULD_TYPE_CSTOR,
				     tid, &csk->txq_idx);

	if (iptype == 4) {
		struct sockaddr_in *sin = (struct sockaddr_in *)&csk->laddr;

		sin->sin_family = PF_INET;
		sin->sin_port = local_port;
		sin->sin_addr.s_addr = *(__be32 *)local_ip;

		sin = (struct sockaddr_in *)&csk->raddr;
		sin->sin_family = PF_INET;
		sin->sin_port = peer_port;
		sin->sin_addr.s_addr = *(__be32 *)peer_ip;
	} else {
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&csk->laddr;

		sin6->sin6_family = PF_INET6;
		sin6->sin6_port = local_port;
		memcpy(sin6->sin6_addr.s6_addr, local_ip, 16);

		ret = cxgb4_clip_get(cdev->lldi.ports[0],
				     (const u32 *)&sin6->sin6_addr.s6_addr, 1);
		if (ret) {
			cstor_err(cdev, "tid %u, cxgb4_clip_get() failed, addr %pI6 ret %d\n",
				  tid, sin6->sin6_addr.s6_addr, ret);
			goto reject;
		}

		set_bit(CSTOR_SOCK_F_CLIP_RELEASE, &csk->flags);

		sin6 = (struct sockaddr_in6 *)&csk->raddr;
		sin6->sin6_family = PF_INET6;
		sin6->sin6_port = peer_port;
		memcpy(sin6->sin6_addr.s6_addr, peer_ip, 16);
	}

	csk->tos = tos;
	csk->tid = tid;
	csk->protocol = lcsk->protocol;
	csk->event_channel = lcsk->event_channel;

	kref_get(&csk->event_channel->kref);

	hdrs = cstor_get_hdr_size(csk, (enable_tcp_timestamps && req->tcpopt.tstamp));

	if (peer_mss && (csk->mtu > (peer_mss + hdrs)))
		csk->mtu = peer_mss + hdrs;

	timer_setup(&csk->timer, sock_timeout, 0);
	xa_lock(&cdev->tids);
	ret = __xa_insert(&cdev->tids, csk->tid, csk, GFP_KERNEL);
	if (ret) {
		xa_unlock(&cdev->tids);
		cstor_err(cdev, "tid %u, __xa_insert() failed, ret %d\n", tid, ret);
		csk->tid = CSTOR_INVALID_TID;
		goto reject;
	}

	__xa_set_mark(&cdev->tids, csk->tid, CSTOR_PASSIVE_SOCK);
	xa_unlock(&cdev->tids);
	atomic_inc(&uctx->num_csk);

	cxgb4_uld_tid_insert(cdev->lldi.ports[0], csk->laddr.ss_family, tid, csk);
	ret = accept_cr(csk, skb, req);
	if (!ret)
		set_bit(CSTOR_SOCK_H_PASS_ACCEPT_REQ, &csk->history);

	return;

reject:
	if (csk)
		cstor_put_sock(csk);

	reject_cr(cdev, tid, port_id % NCHAN, skb);
}

static void pass_establish(struct cstor_device *cdev, struct sk_buff *skb)
{
	struct cstor_sock *csk;
	struct cpl_pass_establish *req = cplhdr(skb);
	u32 tid = GET_TID(req);
	u16 tcp_opt = be16_to_cpu(req->tcp_opt);

	csk = get_sock_from_tid(cdev, tid);
	if (!csk) {
		cstor_err(cdev, "tid %u, get_sock_from_tid() failed\n", tid);
		return;
	}

	csk->snd_nxt = be32_to_cpu(req->snd_isn);
	csk->rcv_nxt = be32_to_cpu(req->rcv_isn);
	csk->snd_wscale = TCPOPT_SND_WSCALE_G(tcp_opt);

	cstor_debug(cdev, "tid %u tcp_opt %#02x\n", tid, be16_to_cpu(tcp_opt));

	set_emss(csk, tcp_opt);

	dst_confirm(csk->dst);
	mutex_lock(&csk->mutex);
	csk->state = CSTOR_SOCK_STATE_LOGIN_REQ_WAIT;
	start_sock_timer(csk);
	set_bit(CSTOR_SOCK_H_PASS_ESTAB, &csk->history);
	send_flowc(csk);
	mutex_unlock(&csk->mutex);
}

static void peer_close(struct cstor_device *cdev, struct sk_buff *skb)
{
	struct cstor_sock *csk;
	struct cpl_peer_close *hdr = cplhdr(skb);
	u32 tid = GET_TID(hdr);
	int disconnect = 1;
	int release = 0;

	csk = get_sock_from_tid(cdev, tid);
	if (!csk) {
		cstor_err(cdev, "tid %u, get_sock_from_tid() failed\n", tid);
		return;
	}

	dst_confirm(csk->dst);

	set_bit(CSTOR_SOCK_H_PEER_CLOSE, &csk->history);

	cstor_wake_up(&csk->wr_wait, -ECONNRESET);

	mutex_lock(&csk->mutex);
	cstor_debug(cdev, "tid %u State: %s (%d)\n", tid, states[csk->state], csk->state);
	switch (csk->state) {
	case CSTOR_SOCK_STATE_LOGIN_REQ_WAIT:
		stop_sock_timer(csk);
		csk->state = CSTOR_SOCK_STATE_CLOSING;
		break;
	case CSTOR_SOCK_STATE_LOGIN_REQ_RCVD:

		/*
		 * We're gonna mark this puppy DEAD, but kecsk
		 * the reference on it until the ULP accepts or
		 * rejects the CR. Also wake up anyone waiting
		 * in rdma connection migration (see cstor_accept_cr()).
		 */
		csk->state = CSTOR_SOCK_STATE_CLOSING;
		break;
	case CSTOR_SOCK_STATE_CONNECTED:
		csk->state = CSTOR_SOCK_STATE_CLOSING;
		break;
	case CSTOR_SOCK_STATE_QP_MODE:
		csk->state = CSTOR_SOCK_STATE_CLOSING;
		cstor_modify_qp(csk->qp, CSTOR_QP_STATE_ERROR);
		break;
	case CSTOR_SOCK_STATE_ABORTING:
		disconnect = 0;
		break;
	case CSTOR_SOCK_STATE_CLOSING:
		csk->state = CSTOR_SOCK_STATE_MORIBUND;
		disconnect = 0;
		break;
	case CSTOR_SOCK_STATE_MORIBUND:
		csk->state = CSTOR_SOCK_STATE_DEAD;
		disconnected_upcall(csk);
		release = 1;
		disconnect = 0;
		break;
	case CSTOR_SOCK_STATE_DEAD:
		disconnect = 0;
		break;
	default:
		WARN_ONCE(1, "Bad endpoint state %u\n", csk->state);
	}
	mutex_unlock(&csk->mutex);

	if (disconnect)
		cstor_sock_disconnect(csk);

	if (release)
		cstor_put_sock(csk);
}

static void peer_abort(struct cstor_device *cdev, struct sk_buff *skb)
{
	struct cstor_sock *csk;
	struct cpl_abort_req_rss6 *req = cplhdr(skb);
	struct cpl_abort_rpl *rpl;
	u32 tid = GET_TID(req);
	int release = 0;
	u8 status;

	csk = get_sock_from_tid(cdev, tid);
	if (!csk) {
		cstor_err(cdev, "tid %u, get_sock_from_tid() failed\n", tid);
		return;
	}

	status = ABORT_RSS_STATUS_G(be32_to_cpu(req->srqidx_status));

	if (is_neg_adv(status)) {
		cstor_debug(cdev, "tid %u, Negative advice on connection - status %d (%s)\n",
			    csk->tid, status, neg_adv_str(status));
		csk->abort_neg_adv++;
		mutex_lock(&cdev->stats.lock);
		cdev->stats.neg_adv++;
		mutex_unlock(&cdev->stats.lock);
		return;
	}

	set_bit(CSTOR_SOCK_H_PEER_ABORT, &csk->history);

	/*
	 * Wake up any threads in rdma_init() or rdma_fini().
	 * However, this is not needed if com state is just
	 * MPA_REQ_SENT
	 */

	cstor_wake_up(&csk->wr_wait, -ECONNRESET);

	mutex_lock(&csk->mutex);
	cstor_debug(cdev, "tid %u State: %s (%d)\n", csk->tid, states[csk->state], csk->state);
	switch (csk->state) {
	case CSTOR_SOCK_STATE_CONNECTING:
	case CSTOR_SOCK_STATE_CONNECTED:
		break;
	case CSTOR_SOCK_STATE_LOGIN_REQ_WAIT:
		stop_sock_timer(csk);
		break;
	case CSTOR_SOCK_STATE_LOGIN_REQ_RCVD:
		break;
	case CSTOR_SOCK_STATE_MORIBUND:
	case CSTOR_SOCK_STATE_CLOSING:
		fallthrough;
	case CSTOR_SOCK_STATE_QP_MODE:
		if (csk->qp)
			cstor_modify_qp(csk->qp, CSTOR_QP_STATE_ERROR);

		break;
	case CSTOR_SOCK_STATE_ABORTING:
		break;
	case CSTOR_SOCK_STATE_DEAD:
		cstor_err(cdev, "tid %u, PEER_ABORT IN DEAD STATE!!!\n", csk->tid);
		mutex_unlock(&csk->mutex);
		return;
	default:
		WARN_ONCE(1, "Bad endpoint state %u\n", csk->state);
		break;
	}

	dst_confirm(csk->dst);
	if (csk->state != CSTOR_SOCK_STATE_ABORTING) {
		csk->state = CSTOR_SOCK_STATE_DEAD;
		disconnected_upcall(csk);
		release = 1;
	}
	mutex_unlock(&csk->mutex);

	skb_get(skb);
	skb_trim(skb, 0);
	set_wr_txq(skb, CPL_PRIORITY_DATA, csk->txq_idx);

	rpl = (struct cpl_abort_rpl *)skb_put(skb, sizeof(*rpl));
	INIT_TP_WR(rpl, csk->tid);
	OPCODE_TID(rpl) = cpu_to_be32(MK_OPCODE_TID(CPL_ABORT_RPL, csk->tid));
	rpl->cmd = CPL_ABORT_NO_RST;
	cstor_ofld_send(cdev, skb);

	if (release)
		cstor_put_sock(csk);
}

static void close_con_rpl(struct cstor_device *cdev, struct sk_buff *skb)
{
	struct cstor_sock *csk;
	struct cpl_close_con_rpl *rpl = cplhdr(skb);
	u32 tid = GET_TID(rpl);
	int release = 0;

	csk = get_sock_from_tid(cdev, tid);
	if (!csk) {
		cstor_err(cdev, "get_sock_from_tid() failed, tid %u\n", tid);
		return;
	}

	cstor_debug(cdev, "tid %u, state: %s (%d)\n", csk->tid, states[csk->state], csk->state);

	mutex_lock(&csk->mutex);
	set_bit(CSTOR_SOCK_H_CLOSE_CON_RPL, &csk->history);
	switch (csk->state) {
	case CSTOR_SOCK_STATE_CLOSING:
		csk->state = CSTOR_SOCK_STATE_MORIBUND;
		break;
	case CSTOR_SOCK_STATE_MORIBUND:
		csk->state = CSTOR_SOCK_STATE_DEAD;
		disconnected_upcall(csk);
		release = 1;
		break;
	case CSTOR_SOCK_STATE_ABORTING:
	case CSTOR_SOCK_STATE_DEAD:
		break;
	default:
		WARN_ONCE(1, "Bad endpoint state %u\n", csk->state);
		break;
	}
	mutex_unlock(&csk->mutex);

	if (release)
		cstor_put_sock(csk);
}

/*
 * Upcall from the adapter indicating data has been transmitted.
 * For us its just the single MPA request or reply.  We can now free
 * the skb holding the mpa message.
 */
static void fw4_ack(struct cstor_device *cdev, struct sk_buff *skb)
{
	struct cstor_sock *csk;
	struct sk_buff *p;
	struct cpl_fw4_ack *hdr = cplhdr(skb);
	u32 csum;
	u32 tid = GET_TID(hdr);
	u8 credits = hdr->credits;

	csk = get_sock_from_tid(cdev, tid);
	if (!csk) {
		cstor_err(cdev, "tid %u, get_sock_from_tid() failed\n", tid);
		return;
	}

	mutex_lock(&csk->mutex);

	cstor_debug(cdev, "tid %u credits %u\n", csk->tid, credits);

	if (credits == 0) {
		cstor_debug(cdev, "tid %u state %s (%u), 0 credit ack\n",
			    csk->tid, states[csk->state], csk->state);
		goto out;
	}

	csk->wr_cred += credits;
	if (!test_bit(CSTOR_SOCK_F_VALIDATE_CREDITS, &csk->flags))
		goto out;

	while (credits) {
		p = cstor_sock_peek_wr(csk);
		if (unlikely(!p)) {
			cstor_err(cdev, "tid %u, cr %u,%u, empty.\n",
				  csk->tid, credits, csk->wr_cred);
			break;
		}

		csum = (__force u32)p->csum;
		if (unlikely(credits < csum)) {
			cstor_warn(cdev, "tid %u, cr %u,%u, < %u.\n",
				   csk->tid, credits, csk->wr_cred, csum);
			p->csum = (__force __wsum)(csum - credits);
			break;
		}

		cstor_sock_dequeue_wr(csk);
		credits -= csum;
		kfree_skb(p);
	}

out:
	if (unlikely(csk->wr_cred > csk->wr_max_cred)) {
		cstor_warn(cdev, "tid %u, cr %u > max_cr %u\n", csk->tid,
			   csk->wr_cred, csk->wr_max_cred);
	}

	mutex_unlock(&csk->mutex);
}

static void set_tcb_rpl(struct cstor_device *cdev, struct sk_buff *skb)
{
	struct cstor_sock *csk;
	struct cpl_set_tcb_rpl *rpl = cplhdr(skb);
	u32 tid = GET_TID(rpl);

	csk = get_sock_from_tid(cdev, tid);
	if (!csk) {
		cstor_err(cdev, "tid %u, get_sock_from_tid() failed\n", tid);
		return;
	}

	if (rpl->status != CPL_ERR_NONE)
		cstor_err(cdev, "Unexpected SET_TCB_RPL status %u for tid %u\n", rpl->status, tid);

	if (csk->protocol == _CSTOR_ISCSI_PROTOCOL)
		cstor_wake_up(&csk->wr_wait, status2errno(rpl->status));
}

int cstor_sock_reject(struct cstor_ucontext *uctx, void __user *ubuf)
{
	struct cstor_device *cdev = uctx->cdev;
	struct cstor_reject_cr_cmd cmd;
	struct cstor_sock *csk;

	if (copy_from_user(&cmd, ubuf, sizeof(cmd))) {
		cstor_err(cdev, "copy_from_user() failed, cmd size %zu\n", sizeof(cmd));
		return -EFAULT;
	}

	csk = get_sock_from_tid(cdev, cmd.tid);
	if (!csk) {
		cstor_err(cdev, "tid %u, get_sock_from_tid() failed\n", cmd.tid);
		return -EINVAL;
	}

	cstor_debug(cdev, "tid %u, state: %s (%d)\n", csk->tid, states[csk->state], csk->state);

	mutex_lock(&csk->mutex);
	if ((csk->state < CSTOR_SOCK_STATE_LOGIN_REQ_RCVD) ||
	    (csk->state == CSTOR_SOCK_STATE_QP_MODE)) {
		cstor_err(cdev, "tid %u, invalid state : %s (%d)\n", csk->tid,
			  states[csk->state], csk->state);
		mutex_unlock(&csk->mutex);
		return -EINVAL;
	}

	set_bit(CSTOR_SOCK_H_ULP_REJECT, &csk->history);
	if (csk->state > CSTOR_SOCK_STATE_QP_MODE) {
		cstor_err(cdev, "tid %u, invalid state : %s (%d)\n", csk->tid,
			  states[csk->state], csk->state);
		clear_bit(CSTOR_SOCK_F_APP_REF, &csk->flags);
		mutex_unlock(&csk->mutex);
		cstor_put_sock(csk);
		return 0;
	}
	mutex_unlock(&csk->mutex);

	cstor_sock_disconnect(csk);
	clear_bit(CSTOR_SOCK_F_APP_REF, &csk->flags);
	cstor_put_sock(csk);
	return 0;
}

int
cstor_find_iscsi_page_size_idx(struct cstor_device *cdev, u32 ddp_page_size, u32 *psz_idx)
{
	u32 iscsi_page_size_order = cdev->lldi.iscsi_pgsz_order;
	u32 page_size_order, idx;

	for (idx = 0; idx < ISCSI_PGSZ_IDX_MAX; idx++) {
		page_size_order = (iscsi_page_size_order >> (idx << 3)) & 0xF;
		if (ddp_page_size == (1U << (ISCSI_PGSZ_BASE_SHIFT + page_size_order))) {
			*psz_idx = idx;
			return 0;
		}
	}

	return -EINVAL;
}

int cstor_sock_accept(struct cstor_ucontext *uctx, void __user *ubuf)
{
	struct cstor_device *cdev = uctx->cdev;
	struct cstor_sock *csk;
	struct cstor_qp *qp;
	struct cstor_accept_cr_cmd cmd;
	int ret;
	u32 psz_idx;

	if (copy_from_user(&cmd, ubuf, sizeof(cmd))) {
		cstor_err(cdev, "copy_from_user() failed, cmd size %zu\n", sizeof(cmd));
		return -EFAULT;
	}

	csk = get_sock_from_tid(cdev, cmd.tid);
	if (!csk) {
		cstor_err(cdev, "tid %u, get_sock_from_tid() failed\n", cmd.tid);
		return -EINVAL;
	}

	if (csk->protocol != cmd.protocol) {
		cstor_err(cdev, "tid %u, invalid protocol csk->protocol %d cmd.protocol %d\n",
			  csk->tid, csk->protocol, cmd.protocol);
		return -EINVAL;
	}

	if (csk->protocol == _CSTOR_ISCSI_PROTOCOL) {
		ret = cstor_find_iscsi_page_size_idx(cdev, cmd.attr.ddp_page_size, &psz_idx);
		if (ret) {
			cstor_err(cdev, "tid %u, cstor_find_iscsi_page_size_idx() failed, "
				  "ddp_page_size %u ret %d\n", csk->tid,
				  cmd.attr.ddp_page_size, ret);
			return ret;
		}
	}

	mutex_lock(&csk->mutex);
	if ((csk->state < CSTOR_SOCK_STATE_LOGIN_REQ_RCVD) ||
	    (csk->state == CSTOR_SOCK_STATE_QP_MODE)) {
		cstor_err(cdev, "tid %u, invalid state : %s (%d)\n", csk->tid,
			  states[csk->state], csk->state);
		ret = -EINVAL;
		goto out;
	}

	if (csk->state > CSTOR_SOCK_STATE_QP_MODE) {
		cstor_err(cdev, "tid %u, invalid state : %s (%d)\n", csk->tid,
			  states[csk->state], csk->state);
		ret = -ECONNRESET;
		goto out;
	}

	qp = get_qp(cdev, cmd.qid);
	if (!qp) {
		cstor_err(cdev, "Invalid qid %u\n", cmd.qid);
		ret = -EINVAL;
		goto out;
	}

	set_bit(CSTOR_SOCK_H_ULP_ACCEPT, &csk->history);

	csk->qp = qp;
	qp->csk = csk;

	if (csk->protocol == _CSTOR_NVME_TCP_PROTOCOL) {
		qp->attr.rx_pda = cmd.attr.rx_pda;
		qp->attr.cmd_pdu_hdr_recv_zcopy = cmd.attr.cmd_pdu_hdr_recv_zcopy;
		qp->attr.hdgst = cmd.attr.hdgst;
		qp->attr.ddgst = cmd.attr.ddgst;
	} else {
		qp->attr.ddp_page_size = cmd.attr.ddp_page_size;
	}

	ret = cstor_modify_qp(qp, CSTOR_QP_STATE_ACTIVE);
	if (ret) {
		cstor_err(cdev, "tid %u qid %u, cstor_modify_qp() failed\n", csk->tid, cmd.qid);
		csk->qp = NULL;
		qp->csk = NULL;
		goto out;
	}

	csk->state = CSTOR_SOCK_STATE_QP_MODE;
	set_bit(CSTOR_SOCK_F_ACCEPT, &csk->flags);
	set_bit(CSTOR_SOCK_F_ATTACH_QP, &csk->flags);
out:
	mutex_unlock(&csk->mutex);
	return ret;
}

int cstor_sock_attach_qp(struct cstor_ucontext *uctx, void __user *ubuf)
{
	struct cstor_device *cdev = uctx->cdev;
	struct cstor_sock *csk;
	struct cstor_qp *qp;
	struct cstor_sock_attach_qp_cmd cmd;
	u32 psz_idx;
	int ret = 0;

	if (copy_from_user(&cmd, ubuf, sizeof(cmd))) {
		cstor_err(cdev, "copy_from_user() failed, cmd size %zu\n", sizeof(cmd));
		return -EFAULT;
	}

	qp = get_qp(cdev, cmd.qid);
	if (!qp) {
		cstor_err(cdev, "Invalid qid %u\n", cmd.qid);
		return -EINVAL;
	}

	csk = get_sock_from_tid(cdev, cmd.tid);
	if (!csk) {
		cstor_err(cdev, "tid %u, get_sock_from_tid() failed\n", cmd.tid);
		return -EINVAL;
	}

	if (cmd.protocol != csk->protocol) {
		cstor_err(cdev, "invalid protocol, cmd.protocol %d csk->protocol %d\n",
			  cmd.protocol, csk->protocol);
		return -EINVAL;
	}

	if (csk->protocol == _CSTOR_ISCSI_PROTOCOL) {
		ret = cstor_find_iscsi_page_size_idx(cdev, cmd.attr.ddp_page_size, &psz_idx);
		if (ret) {
			cstor_err(cdev, "tid %u, cstor_find_iscsi_page_size_idx() failed, "
				  "ddp_page_size %u ret %d\n",
				  csk->tid, cmd.attr.ddp_page_size, ret);
			return ret;
		}
	}

	mutex_lock(&csk->mutex);

	set_bit(CSTOR_SOCK_H_ULP_ATTACH_QP, &csk->history);

	if (csk->state > CSTOR_SOCK_STATE_QP_MODE) {
		cstor_err(cdev, "tid %u, invalid state : %s (%d)\n", csk->tid,
			  states[csk->state], csk->state);
		mutex_unlock(&csk->mutex);
		return -ECONNRESET;
	} else if (csk->state != CSTOR_SOCK_STATE_CONNECTED) {
		cstor_err(cdev, "tid %u, invalid state : %s (%d)\n", csk->tid,
			  states[csk->state], csk->state);
		mutex_unlock(&csk->mutex);
		return -EINVAL;
	}

	csk->qp = qp;
	qp->csk = csk;
	qp->attr.initiator = true;

	if (csk->protocol == _CSTOR_NVME_TCP_PROTOCOL) {
		qp->attr.rx_pda = cmd.attr.rx_pda;
		qp->attr.cmd_pdu_hdr_recv_zcopy = cmd.attr.cmd_pdu_hdr_recv_zcopy;
		qp->attr.hdgst = cmd.attr.hdgst;
		qp->attr.ddgst = cmd.attr.ddgst;
	} else {
		qp->attr.ddp_page_size = cmd.attr.ddp_page_size;
	}

	ret = cstor_modify_qp(qp, CSTOR_QP_STATE_ACTIVE);
	if (ret) {
		cstor_err(cdev, "tid %u qid %u, cstor_modify_qp() failed, ret %d\n",
			  csk->tid, cmd.qid, ret);
		qp->csk = NULL;
		csk->qp = NULL;
		mutex_unlock(&csk->mutex);
		return ret;
	}

	csk->state = CSTOR_SOCK_STATE_QP_MODE;
	set_bit(CSTOR_SOCK_F_ATTACH_QP, &csk->flags);
	mutex_unlock(&csk->mutex);

	return 0;
}

int _cstor_sock_disconnect(struct cstor_ucontext *uctx, void __user *ubuf)
{
	struct cstor_device *cdev = uctx->cdev;
	struct cstor_sock *csk;
	struct cstor_qp *qp;
	struct cstor_sock_disconnect_cmd cmd;
	int ret;

	if (copy_from_user(&cmd, ubuf, sizeof(cmd))) {
		cstor_err(cdev, "copy_from_user() failed, cmd size %zu\n", sizeof(cmd));
		return -EFAULT;
	}

	csk = get_sock_from_tid(cdev, cmd.tid);
	if (!csk) {
		cstor_err(cdev, "tid %u, get_sock_from_tid() failed\n", cmd.tid);
		return -EINVAL;
	}

	cstor_debug(cdev, "tid %u, state: %s (%d)\n", csk->tid, states[csk->state], csk->state);

	mutex_lock(&csk->mutex);

	if (test_bit(CSTOR_SOCK_F_ACT_OPEN, &csk->flags)) {
		if (!test_bit(CSTOR_SOCK_F_ATTACH_QP, &csk->flags))
			goto disconnect;
	}

	if (csk->state < CSTOR_SOCK_STATE_QP_MODE) {
		cstor_err(cdev, "tid %u, invalid state : %s (%d)\n", csk->tid,
			  states[csk->state], csk->state);
		ret = -EINVAL;
		goto err;
	} else if (csk->state > CSTOR_SOCK_STATE_QP_MODE) {
		cstor_debug(cdev, "tid %u, invalid state : %s (%d)\n", csk->tid,
			    states[csk->state], csk->state);
		ret = 0;
		set_bit(CSTOR_SOCK_H_ULP_DISCONNECT, &csk->history);
		goto err;
	}

	qp = csk->qp;
	if (!qp) {
		cstor_err(cdev, "tid %u, qp unavailable\n", csk->tid);
		ret = -EINVAL;
		goto err;
	}

	set_bit(CSTOR_SOCK_H_ULP_DISCONNECT, &csk->history);

	cstor_modify_qp(csk->qp, CSTOR_QP_STATE_ERROR);

disconnect:
	mutex_unlock(&csk->mutex);
	cstor_sock_disconnect(csk);
	return 0;

err:
	mutex_unlock(&csk->mutex);
	return ret;
}

int cstor_sock_release(struct cstor_ucontext *uctx, void __user *ubuf)
{
	struct cstor_device *cdev = uctx->cdev;
	struct cstor_sock *csk;
	struct cstor_sock_release_cmd cmd;

	if (copy_from_user(&cmd, ubuf, sizeof(cmd))) {
		cstor_err(cdev, "copy_from_user() failed, cmd size %zu\n", sizeof(cmd));
		return -EFAULT;
	}

	if (cmd.tid != CSTOR_INVALID_TID) {
		csk = get_sock_from_tid(cdev, cmd.tid);
		if (!csk) {
			cstor_err(cdev, "tid %u, get_sock_from_tid() failed\n", cmd.tid);
			return -EINVAL;
		}
	} else if (cmd.atid != CSTOR_INVALID_ATID) {
		csk = cxgb4_uld_atid_lookup(cdev->lldi.ports[0], cmd.atid);
		if (!csk) {
			cstor_err(cdev, "atid %u, csk not found\n", cmd.atid);
			return -EINVAL;
		}
	} else {
		return -EINVAL;
	}

	cstor_debug(cdev, "tid %u, state: %s (%d)\n", csk->tid, states[csk->state], csk->state);

	mutex_lock(&csk->mutex);
	if (csk->state != CSTOR_SOCK_STATE_DEAD) {
		mutex_unlock(&csk->mutex);
		cstor_err(cdev, "tid %u, invalid state : %s (%d)\n", csk->tid,
			  states[csk->state], csk->state);
		return -EINVAL;
	}

	set_bit(CSTOR_SOCK_H_ULP_RELEASE, &csk->history);
	mutex_unlock(&csk->mutex);

	clear_bit(CSTOR_SOCK_F_APP_REF, &csk->flags);
	cstor_put_sock(csk);
	return 0;
}

static int create_server6(struct cstor_listen_sock *lcsk)
{
	struct cstor_device *cdev = lcsk->uctx->cdev;
	struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&lcsk->laddr;
	int ret;

	if (ipv6_addr_type(&sin6->sin6_addr) != IPV6_ADDR_ANY) {
		ret = cxgb4_clip_get(cdev->lldi.ports[0],
				     (const u32 *)&sin6->sin6_addr.s6_addr, 1);
		if (ret) {
			cstor_err(cdev, "stid %u, cxgb4_clip_get() failed, laddr %pI6 ret %d\n",
				  lcsk->stid, sin6->sin6_addr.s6_addr, ret);
			return ret;
		}

		lcsk->clip_release = true;
	}

	cstor_reinit_wr_wait(&lcsk->wr_wait);
	ret = cxgb4_uld_server6_create(cdev->lldi.ports[0], lcsk->stid, &sin6->sin6_addr,
				       sin6->sin6_port, 0, cdev->lldi.rxq_ids[0], NULL);
	if (!ret)
		ret = cstor_wait_for_reply(cdev, &lcsk->wr_wait, 0, 0, __func__);

	return ret;
}

static int create_server4(struct cstor_listen_sock *lcsk)
{
	struct cstor_device *cdev = lcsk->uctx->cdev;
	struct sockaddr_in *sin = (struct sockaddr_in *)&lcsk->laddr;
	int ret;

	cstor_reinit_wr_wait(&lcsk->wr_wait);
	ret = cxgb4_uld_server_create(cdev->lldi.ports[0], lcsk->stid, sin->sin_addr.s_addr,
				      sin->sin_port, 0, cdev->lldi.rxq_ids[0], NULL);
	if (!ret)
		ret = cstor_wait_for_reply(cdev, &lcsk->wr_wait, 0, 0, __func__);

	return ret;
}

static struct net_device *cstor_find_ndev(struct sockaddr_storage *addr)
{
	struct net_device *ndev = NULL;

	if (addr->ss_family == AF_INET) {
		struct sockaddr_in *sin = (struct sockaddr_in *)addr;

		ndev = cstor_ipv4_netdev(sin->sin_addr.s_addr);
	} else if (addr->ss_family == AF_INET6) {
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)addr;

		ndev = cstor_ipv6_netdev(&sin6->sin6_addr);
	}

	return ndev;
}

static int cstor_find_port_id(struct cstor_device *cdev, struct net_device *ndev, u8 *port_id)
{
	struct cxgb4_lld_info *lldi = &cdev->lldi;
	u32 i;

	for (i = 0; i < lldi->nports; i++) {
		if (lldi->ports[i] == ndev) {
			if (port_id)
				*port_id = i;
			return 0;
		}
	}

	return -ENODEV;
}

static int destroy_listen(struct cstor_listen_sock *lcsk)
{
	struct cstor_device *cdev = lcsk->uctx->cdev;
	struct sk_buff *skb = lcsk->destroy_skb;
	int ret;

	lcsk->destroy_skb = NULL;
	cstor_reinit_wr_wait(&lcsk->wr_wait);
	ret = __cxgb4_uld_server_remove(cdev->lldi.ports[0], lcsk->stid,
					cdev->lldi.rxq_ids[0],
					lcsk->laddr.ss_family == AF_INET6, skb);
	if (ret) {
		cstor_err(cdev, "stid %u, __cxgb4_uld_server_remove() failed, ret %d\n",
			  lcsk->stid, ret);
		return ret;
	}

	if (!lcsk->app_close)
		ret = cstor_wait_for_reply(cdev, &lcsk->wr_wait, 0, 0, __func__);

	return ret;
}

int cstor_create_listen(struct cstor_ucontext *uctx, void __user *ubuf)
{
	struct cstor_device *cdev = uctx->cdev;
	struct cstor_listen_sock *lcsk;
	struct cstor_create_listen_cmd cmd;
	struct cstor_create_listen_resp uresp;
	void __user *_ubuf;
	int ret = 0;
	u16 lport;
	u8 port_id;
	char laddr[40];

	if (copy_from_user(&cmd, ubuf, sizeof(cmd))) {
		cstor_err(cdev, "copy_from_user() failed, cmd size %zu\n", sizeof(cmd));
		return -EFAULT;
	}

	if ((cmd.protocol != _CSTOR_ISCSI_PROTOCOL) &&
	    (cmd.protocol != _CSTOR_NVME_TCP_PROTOCOL)) {
		cstor_err(cdev, "invalid protocol cmd.protocol = %d\n", cmd.protocol);
		return -EINVAL;
	}

	if (!cmd.first_pdu_recv_timeout) {
		cstor_err(cdev, "first_pdu_recv_timeout value not present\n");
		return -EINVAL;
	}

	lcsk = kzalloc(sizeof(*lcsk), GFP_KERNEL);
	if (!lcsk) {
		cstor_err(cdev, "cannot alloc lcsk\n");
		return -ENOMEM;
	}

	lcsk->destroy_skb = alloc_skb(sizeof(struct cpl_close_listsvr_req), GFP_KERNEL);
	if (!lcsk->destroy_skb) {
		cstor_err(cdev, "failed to allocate lcsk->destroy_skb\n");
		kfree(lcsk);
		return -ENOMEM;
	}

	kref_init(&lcsk->kref);
	mutex_init(&lcsk->mutex);
	init_completion(&lcsk->wr_wait.completion);

	lcsk->uctx = uctx;
	lcsk->stid = CSTOR_INVALID_STID;
	lcsk->protocol = cmd.protocol;
	lcsk->first_pdu_recv_timeout = cmd.first_pdu_recv_timeout;

	lcsk->event_channel = xa_load(&uctx->event_channels, cmd.efd);
	if (!lcsk->event_channel) {
		cstor_err(cdev, "invalid event file descriptor cmd.efd %u\n", cmd.efd);
		ret = -EBADF;
		goto err;
	}

	kref_get(&lcsk->event_channel->kref);

	if (cmd.ipv4) {
		struct sockaddr_in *sin = (struct sockaddr_in *)&lcsk->laddr;

		sin->sin_family = AF_INET;
		sin->sin_port = cmd.tcp_port;
		sin->sin_addr.s_addr = cmd.ip_addr[0];
		snprintf(laddr, sizeof(laddr), "%pI4", cmd.ip_addr);
	} else {
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&lcsk->laddr;

		sin6->sin6_family = AF_INET6;
		sin6->sin6_port = cmd.tcp_port;
		memcpy(sin6->sin6_addr.s6_addr, cmd.ip_addr, sizeof(cmd.ip_addr));
		snprintf(laddr, sizeof(laddr), "%pI6", cmd.ip_addr);
	}

	lport = be16_to_cpu(cmd.tcp_port);

	if (cmd.inaddr_any) {
		port_id = _CSTOR_LCSK_INADDR_ANY_PORT_ID;
	} else {
		struct net_device *ndev;

		rcu_read_lock();
		ndev = cstor_find_ndev(&lcsk->laddr);
		if (!ndev) {
			rcu_read_unlock();
			cstor_err(cdev, "net device not found, laddr %s, lport %u\n",
				  laddr, lport);
			ret = -ENODEV;
			goto err;
		}

		ret = cstor_find_port_id(cdev, ndev, &port_id);
		rcu_read_unlock();

		if (ret) {
			cstor_err(cdev, "cstor_find_port_id() failed, laddr %s, "
				  "lport %u, ret %d\n", laddr, lport, ret);
			goto err;
		}
	}

	/*
	 * Allocate a server TID.
	 */
	ret = cxgb4_uld_stid_alloc(cdev->lldi.ports[0], lcsk->laddr.ss_family, lcsk);
	if (ret < 0) {
		cstor_err(cdev, "cannot alloc stid for laddr %s, lport %u, ret %d\n",
			  laddr, lport, ret);
		goto err;
	}

	lcsk->stid = ret;
	xa_lock(&cdev->stids);
	ret = __xa_insert(&cdev->stids, lcsk->stid, lcsk, GFP_KERNEL);
	if (ret) {
		xa_unlock(&cdev->stids);
		cstor_err(cdev, "lcsk->stid %u, __xa_insert() failed for laddr %s, "
			  "lport %u, ret %d\n", lcsk->stid, laddr, lport, ret);
		cxgb4_uld_stid_free(cdev->lldi.ports[0], lcsk->laddr.ss_family, lcsk->stid);
		lcsk->stid = CSTOR_INVALID_STID;
		goto err;
	}
	xa_unlock(&cdev->stids);

	atomic_inc(&uctx->num_lcsk);

	mutex_lock(&lcsk->mutex);

	if (cmd.ipv4)
		ret = create_server4(lcsk);
	else
		ret = create_server6(lcsk);

	if (ret) {
		mutex_unlock(&lcsk->mutex);
		cstor_err(cdev, "stid %u, failed to create server for laddr %s lport %u ret %d\n",
			  lcsk->stid, laddr, lport, ret);
		if (ret == -ETIMEDOUT)
			return ret;

		goto err;
	}

	_ubuf = &((struct cstor_create_listen_cmd *)ubuf)->resp;
	uresp.stid = lcsk->stid;
	uresp.port_id = port_id;

	if (copy_to_user(_ubuf, &uresp, sizeof(uresp))) {
		mutex_unlock(&lcsk->mutex);
		cstor_err(cdev, "copy_to_user() failed, uresp size %zu\n", sizeof(uresp));
		ret = destroy_listen(lcsk);
		if (ret == -ETIMEDOUT)
			return -EFAULT;

		ret = -EFAULT;
		goto err;
	}

	lcsk->listen = true;
	mutex_unlock(&lcsk->mutex);

	return 0;
err:
	cstor_put_listen_sock(lcsk);
	return ret;
}

static void remove_pending_connect_event(struct cstor_listen_sock *lcsk)
{
	struct cstor_event_channel *event_channel = lcsk->event_channel;
	struct cstor_uevent_node *uevt_node, *tmp;
	struct cstor_uevent *uevt;
	struct cstor_sock *csk;

	mutex_lock(&event_channel->uevt_list_lock);
	list_for_each_entry_safe(uevt_node, tmp, &event_channel->uevt_list, entry) {
		uevt = &uevt_node->uevt;
		if (uevt->event == CSTOR_UEVENT_CONNECT_REQ) {
			csk = get_sock_from_tid(lcsk->uctx->cdev, uevt->u.req.tid);
			cstor_free_uevent_node(uevt_node);
			cstor_sock_disconnect(csk);
			clear_bit(CSTOR_SOCK_F_APP_REF, &csk->flags);
			cstor_put_sock(csk);
		}
	}
	mutex_unlock(&event_channel->uevt_list_lock);
}

static int __cstor_destroy_listen(struct cstor_listen_sock *lcsk)
{
	struct cstor_ucontext *uctx = lcsk->uctx;
	int ret;

	mutex_lock(&lcsk->mutex);
	lcsk->listen = false;
	remove_pending_connect_event(lcsk);
	mutex_unlock(&lcsk->mutex);

	ret = destroy_listen(lcsk);
	if (ret) {
		cstor_err(uctx->cdev, "stid %u, destroy_listen() failed, ret %d\n",
			  lcsk->stid, ret);
		return ret;
	}

	if (!lcsk->app_close)
		cstor_put_listen_sock(lcsk);

	return ret;
}

int cstor_destroy_listen(struct cstor_ucontext *uctx, void __user *ubuf)
{
	struct cstor_device *cdev = uctx->cdev;
	struct cstor_destroy_listen_cmd cmd;
	struct cstor_listen_sock *lcsk;

	if (copy_from_user(&cmd, ubuf, sizeof(cmd))) {
		cstor_err(cdev, "copy_from_user() failed, cmd size %zu\n", sizeof(cmd));
		return -EFAULT;
	}

	lcsk = get_listen_sock_from_stid(cdev, cmd.stid);
	if (!lcsk) {
		cstor_err(cdev, "stid %u lookup failure!\n", cmd.stid);
		return -EINVAL;
	}

	if (!lcsk->listen) {
		cstor_err(cdev, "stid %u, listening csk not in LISTEN\n", cmd.stid);
		return -EINVAL;
	}

	return __cstor_destroy_listen(lcsk);
}

int cstor_destroy_all_listen(struct cstor_ucontext *uctx)
{
	struct cstor_device *cdev = uctx->cdev;
	struct cstor_listen_sock *lcsk;
	unsigned long index;
	bool wait = false;
	int ret = 0;

	spin_lock_bh(&cdev->slock);
	set_bit(CDEV_FLAG_STOP_QUEUE_WORK, &cdev->flags);
	spin_unlock_bh(&cdev->slock);

	flush_workqueue(cdev->workq);

	xa_for_each(&cdev->stids, index, lcsk) {
		if (!lcsk->listen || (lcsk->uctx != uctx))
			continue;

		lcsk->app_close = true;
		ret = __cstor_destroy_listen(lcsk);
		if (ret) {
			cstor_err(cdev, "stid %u,  __cstor_destroy_listen() failed, ret %d\n",
				  lcsk->stid, ret);
			return ret;
		}

		uctx->lcsk_close_pending++;
	}

	if (uctx->lcsk_close_pending)
		wait = true;

	clear_bit(CDEV_FLAG_STOP_QUEUE_WORK, &cdev->flags);
	queue_work(cdev->workq, &cdev->rx_work);

	if (wait)
		ret = cstor_wait_for_reply(cdev, &cdev->wr_wait, 0, 0, __func__);

	return ret;
}

int cstor_sock_disconnect(struct cstor_sock *csk)
{
	struct cstor_device *cdev = csk->uctx->cdev;
	int close = 0, ret = 0;

	mutex_lock(&csk->mutex);

	cstor_debug(cdev, "tid %u state %s (%d)\n", csk->tid, states[csk->state], csk->state);

	cstor_get_sock(csk);

	if (!test_bit(CSTOR_SOCK_F_FLOWC_SENT, &csk->flags)) {
		ret = send_flowc(csk);
		if (ret)
			goto out;
	}

	switch (csk->state) {
	case CSTOR_SOCK_STATE_LOGIN_REQ_WAIT:
		stop_sock_timer(csk);
		fallthrough;
	case CSTOR_SOCK_STATE_LOGIN_REQ_RCVD:
	case CSTOR_SOCK_STATE_CONNECTING:
	case CSTOR_SOCK_STATE_CONNECTED:
	case CSTOR_SOCK_STATE_QP_MODE:
		close = 1;
		set_bit(CSTOR_SOCK_F_CLOSE_SENT, &csk->flags);
		csk->state = CSTOR_SOCK_STATE_ABORTING;
		break;
	case CSTOR_SOCK_STATE_CLOSING:
		if (!test_and_set_bit(CSTOR_SOCK_F_CLOSE_SENT, &csk->flags)) {
			close = 1;
			csk->state = CSTOR_SOCK_STATE_ABORTING;
		}
		break;
	case CSTOR_SOCK_STATE_MORIBUND:
	case CSTOR_SOCK_STATE_ABORTING:
	case CSTOR_SOCK_STATE_DEAD:
		cstor_err(cdev, "ignoring disconnect tid %u state %u\n", csk->tid, csk->state);
		break;
	default:
		BUG();
		break;
	}

	if (!close)
		goto out;

	set_bit(CSTOR_SOCK_H_DISC_ABORT, &csk->history);
	ret = send_abort_req(csk);
	if (ret) {
		set_bit(CSTOR_SOCK_H_DISC_FAIL, &csk->history);
		csk->state = CSTOR_SOCK_STATE_DEAD;
		cstor_put_sock(csk);
	}
out:
	mutex_unlock(&csk->mutex);
	cstor_put_sock(csk);
	return ret;
}

static void __cstor_disconnect_sock(struct cstor_sock *csk)
{
	bool disconnect = true;
	int ret;

	if (test_bit(CSTOR_SOCK_F_APP_REF, &csk->flags)) {
		if (kref_read(&csk->kref) == 1)
			disconnect = false;

		clear_bit(CSTOR_SOCK_F_APP_REF, &csk->flags);
		cstor_put_sock(csk);
	}

	if (disconnect)
		ret = cstor_sock_disconnect(csk);
}

void cstor_disconnect_all_sock(struct cstor_ucontext *uctx)
{
	struct cstor_device *cdev = uctx->cdev;
	struct cstor_sock *csk;
	unsigned long index;

	spin_lock_bh(&cdev->slock);
	set_bit(CDEV_FLAG_STOP_QUEUE_WORK, &cdev->flags);
	spin_unlock_bh(&cdev->slock);

	flush_workqueue(cdev->workq);

	xa_for_each(&cdev->tids, index, csk) {
		if (csk->uctx == uctx)
			__cstor_disconnect_sock(csk);
	}

	clear_bit(CDEV_FLAG_STOP_QUEUE_WORK, &cdev->flags);
	queue_work(cdev->workq, &cdev->rx_work);
}

void cstor_destroy_atids(struct cstor_ucontext *uctx)
{
	struct cstor_device *cdev = uctx->cdev;
	struct cstor_sock *csk;
	unsigned long index;

	spin_lock_bh(&cdev->slock);
	set_bit(CDEV_FLAG_STOP_QUEUE_WORK, &cdev->flags);
	spin_unlock_bh(&cdev->slock);

	flush_workqueue(cdev->workq);

	xa_for_each(&cdev->atids, index, csk) {
		if (csk->uctx == uctx)
			__cstor_disconnect_sock(csk);
	}

	clear_bit(CDEV_FLAG_STOP_QUEUE_WORK, &cdev->flags);
	queue_work(cdev->workq, &cdev->rx_work);
}

static void deferred_fw6_msg(struct cstor_device *cdev, struct sk_buff *skb)
{
	struct cpl_fw6_msg *rpl = cplhdr(skb);

	switch (rpl->type) {
	case FW6_TYPE_CQE:

		/*
		 * With the new 64B CQE format, CQEs now have
		 * an RSS header flit.  But only when inserted into
		 * a CQ.  Error CQEs (aka AEs) don't have this flit.
		 * So back up the ptr 1 flit to properly align the cqe
		 * for the dispatch code.
		 */
		cstor_ev_dispatch(cdev, (struct t4_cqe *)(rpl->data - 1));
		break;
	}
}

static void act_establish(struct cstor_device *cdev, struct sk_buff *skb)
{
	struct cstor_sock *csk;
	struct cpl_act_establish *req = cplhdr(skb);
	u32 tid = GET_TID(req);
	u32 atid = TID_TID_G(be32_to_cpu(req->tos_atid));
	unsigned short tcp_opt = be16_to_cpu(req->tcp_opt);

	csk = cxgb4_uld_atid_lookup(cdev->lldi.ports[0], atid);

	cstor_debug(cdev, "tid %u snd_isn %u rcv_isn %u\n", tid,
		    be32_to_cpu(req->snd_isn), be32_to_cpu(req->rcv_isn));

	mutex_lock(&csk->mutex);
	if (csk->state != CSTOR_SOCK_STATE_CONNECTING) {
		mutex_unlock(&csk->mutex);
		return;
	}

	dst_confirm(csk->dst);

	/* setup the tid for this connection */
	csk->tid = tid;
	cxgb4_uld_tid_insert(cdev->lldi.ports[0], csk->laddr.ss_family, tid, csk);
	insert_csk_tid(csk);
	cxgb4_uld_tid_ctrlq_id_sel_update(cdev->lldi.ports[0], tid, &csk->ctrlq_idx);
	cxgb4_uld_tid_qid_sel_update(cdev->lldi.ports[csk->port_id], CXGB4_ULD_TYPE_CSTOR,
				     tid, &csk->txq_idx);

	atomic_dec(&csk->uctx->num_active_csk);

	csk->snd_nxt = be32_to_cpu(req->snd_isn);
	csk->rcv_nxt = be32_to_cpu(req->rcv_isn);
	csk->snd_wscale = TCPOPT_SND_WSCALE_G(tcp_opt);

	set_emss(csk, be16_to_cpu(req->tcp_opt));

	set_bit(CSTOR_SOCK_H_ACT_ESTAB, &csk->history);

	if (send_flowc(csk)) {
		mutex_unlock(&csk->mutex);
		return;
	}

	csk->state = CSTOR_SOCK_STATE_CONNECTED;

	active_connect_upcall(csk, _CSTOR_CONNECT_SUCCESS);

	mutex_unlock(&csk->mutex);
}

static void act_open_rpl(struct cstor_device *cdev, struct sk_buff *skb)
{
	struct cstor_sock *csk;
	struct cpl_act_open_rpl *rpl = cplhdr(skb);
	u32 atid = TID_TID_G(AOPEN_ATID_G(be32_to_cpu(rpl->atid_status)));
	int status = AOPEN_STATUS_G(be32_to_cpu(rpl->atid_status));

	csk = cxgb4_uld_atid_lookup(cdev->lldi.ports[0], atid);

	cstor_debug(cdev, "atid %u status %u errno %d\n", atid, status, status2errno(status));

	mutex_lock(&csk->mutex);
	if (csk->state != CSTOR_SOCK_STATE_CONNECTING) {
		mutex_unlock(&csk->mutex);
		return;
	}
	mutex_unlock(&csk->mutex);

	if (is_neg_adv(status)) {
		u32 tid = GET_TID(rpl);

		cstor_warn(cdev, "Connection problems for atid %u, tid %u status %u (%s)\n",
			   atid, tid, status, neg_adv_str(status));
		mutex_lock(&cdev->stats.lock);
		cdev->stats.neg_adv++;
		mutex_unlock(&cdev->stats.lock);
		csk->tid = tid;
		cxgb4_uld_tid_insert(cdev->lldi.ports[0], csk->laddr.ss_family, tid, csk);
		insert_csk_tid(csk);
		cxgb4_uld_tid_ctrlq_id_sel_update(cdev->lldi.ports[0], tid, &csk->ctrlq_idx);
		cxgb4_uld_tid_qid_sel_update(cdev->lldi.ports[csk->port_id], CXGB4_ULD_TYPE_CSTOR,
					     tid, &csk->txq_idx);
		cstor_sock_disconnect(csk);
		set_bit(CSTOR_SOCK_F_NEG_ADV_DISCONNECT, &csk->flags);
		atomic_dec(&csk->uctx->num_active_csk);
		return;
	}

	mutex_lock(&csk->mutex);

	atomic_dec(&csk->uctx->num_active_csk);

	set_bit(CSTOR_SOCK_H_ACT_OPEN_RPL, &csk->history);

	switch (status) {
	case CPL_ERR_CONN_RESET:
	case CPL_ERR_CONN_TIMEDOUT:
		break;
	default:
		if (csk->laddr.ss_family == AF_INET) {
			struct sockaddr_in *la = (struct sockaddr_in *)&csk->laddr;
			struct sockaddr_in *ra = (struct sockaddr_in *)&csk->raddr;

			cstor_err(cdev, "Active open failure - atid %u status %u "
				  "errno %d %pI4:%u->%pI4:%u\n", atid, status,
				  status2errno(status), &la->sin_addr.s_addr,
				  be16_to_cpu(la->sin_port), &ra->sin_addr.s_addr,
				  be16_to_cpu(ra->sin_port));
		} else {
			struct sockaddr_in6 *la6 = (struct sockaddr_in6 *)&csk->laddr;
			struct sockaddr_in6 *ra6 = (struct sockaddr_in6 *)&csk->raddr;

			cstor_err(cdev, "Active open failure - atid %u status %u "
				  "errno %d %pI6:%u->%pI6:%u\n", atid, status,
				  status2errno(status), la6->sin6_addr.s6_addr,
				  be16_to_cpu(la6->sin6_port), ra6->sin6_addr.s6_addr,
				  be16_to_cpu(ra6->sin6_port));
		}
	}

	active_connect_upcall(csk, _CSTOR_CONNECT_FAILURE);

	csk->state = CSTOR_SOCK_STATE_DEAD;

	if (status && act_open_has_tid(status)) {
		u32 tid = GET_TID(rpl);

		cxgb4_uld_tid_ctrlq_id_sel_update(cdev->lldi.ports[0], tid, &csk->ctrlq_idx);
		cxgb4_uld_tid_remove(cdev->lldi.ports[0], csk->ctrlq_idx,
				     csk->laddr.ss_family, tid);
	}

	mutex_unlock(&csk->mutex);
	cstor_put_sock(csk);
}

static void nvmt_cmp(struct cstor_device *cdev, struct sk_buff *skb)
{
	struct cstor_listen_sock *lcsk;
	struct cstor_sock *csk;
	struct cpl_nvmt_cmp *cpl = cplhdr(skb);
	struct nvme_tcp_hdr *hdr;
	u32 tid = GET_TID(cpl);

	csk = get_sock_from_tid(cdev, tid);
	if (!csk) {
		cstor_err(cdev, "tid %u, get_sock_from_tid() failed\n", tid);
		return;
	}

	cstor_debug(cdev, "tid %u\n", csk->tid);

	if (csk->rcv_nxt != be32_to_cpu(cpl->seq)) {
		cstor_err(cdev, "tcp seq mismatch on tid=%u, expected=%x received=%x\n",
			  tid, csk->rcv_nxt, be32_to_cpu(cpl->seq));
		goto err;
	}

	if (unlikely(cpl->status)) {
		cstor_err(cdev, "tid %u, cpl status %u error\n", tid, cpl->status);
		goto err;
	}

	skb_pull(skb, sizeof(*cpl));

	stop_sock_timer(csk);
	csk->state = CSTOR_SOCK_STATE_LOGIN_REQ_RCVD;

	lcsk = csk->lcsk;
	mutex_lock(&lcsk->mutex);
	if (!lcsk->listen) {
		cstor_err(cdev, "tid %u, lcsk listen is not listening, port id %u\n",
			  tid, csk->port_id);
		mutex_unlock(&lcsk->mutex);
		goto err;
	}

	hdr = (struct nvme_tcp_hdr *)skb->data;
	csk->rcv_nxt += hdr->plen;
	skb_get(skb);
	connect_upcall(csk, skb);
	mutex_unlock(&lcsk->mutex);

	return;
err:
	cstor_sock_disconnect(csk);
}

static void rx_iscsi_cmp(struct cstor_device *cdev, struct sk_buff *skb)
{
	struct cstor_listen_sock *lcsk;
	struct cstor_sock *csk;
	struct cpl_rx_iscsi_cmp *cpl = cplhdr(skb);
	u32 status;
	u32 tid = GET_TID(cpl);
	int ret;

	csk = get_sock_from_tid(cdev, tid);
	if (!csk) {
		cstor_err(cdev, "tid %u, get_sock_from_tid() failed\n", tid);
		return;
	}

	if (csk->rcv_nxt != be32_to_cpu(cpl->seq)) {
		cstor_err(cdev, "tcp seq mismatch on tid=%u, expected=%x received=%x\n",
			  tid, csk->rcv_nxt, be32_to_cpu(cpl->seq));
		goto err;
	}

	csk->rcv_nxt += be16_to_cpu(cpl->pdu_len_ddp);

	skb_pull(skb, sizeof(*cpl));
	if (skb_linearize(skb)) {
		cstor_err(cdev, "failed to linearize frags with len %u\n", skb->data_len);
		goto err;
	}

	mutex_lock(&csk->mutex);
	if (csk->dskb) {
		struct skb_shared_info *ssi = skb_shinfo(skb);
		struct skb_shared_info *dssi = skb_shinfo(csk->dskb);
		skb_frag_t *frag = ssi->frags;
		skb_frag_t *dfrag = dssi->frags;
		u32 i;

		for (i = 0; i < dssi->nr_frags; i++) {
			skb_frag_fill_page_desc(&frag[i], skb_frag_page(&dfrag[i]),
						skb_frag_off(&dfrag[i]), skb_frag_size(&dfrag[i]));
			get_page(skb_frag_page(&dfrag[i]));
		}

		ssi->nr_frags = dssi->nr_frags;
		skb->len += csk->dskb->data_len;
		skb->data_len += csk->dskb->data_len;
		skb->truesize += csk->dskb->data_len;

		kfree_skb(csk->dskb);
		csk->dskb = NULL;
	}

	status = be32_to_cpu(cpl->ddpvld) & CSTOR_ISCSI_ERR_MASK;

	if (test_bit(CSTOR_SOCK_F_ACT_OPEN, &csk->flags)) {
		skb_get(skb);
		ret = iscsi_pdu_upcall(csk, skb, status);
		if (ret) {
			cstor_err(cdev, "tid %u, failed pdu upcall, ret %u\n", csk->tid, ret);
			kfree_skb(skb);
			mutex_unlock(&csk->mutex);
			goto err;
		}

		mutex_unlock(&csk->mutex);
		return;
	}

	if (unlikely(status)) {
		cstor_err(cdev, "tid %u, ddp validation status %u error\n", tid, status);
		mutex_unlock(&csk->mutex);
		goto err;
	}

	stop_sock_timer(csk);
	csk->state = CSTOR_SOCK_STATE_LOGIN_REQ_RCVD;
	mutex_unlock(&csk->mutex);

	lcsk = csk->lcsk;
	mutex_lock(&lcsk->mutex);
	if (!lcsk->listen) {
		cstor_err(cdev, "tid %u, lcsk listen is not listening, port id %u\n",
			  tid, csk->port_id);
		mutex_unlock(&lcsk->mutex);
		goto err;
	}

	skb_get(skb);
	connect_upcall(csk, skb);
	mutex_unlock(&lcsk->mutex);

	return;
err:
	cstor_sock_disconnect(csk);
}

static void iscsi_data(struct cstor_device *cdev, struct sk_buff *skb)
{
	struct cstor_sock *csk;
	struct cpl_iscsi_data *cpl = cplhdr(skb);
	u32 tid = GET_TID(cpl);

	csk = get_sock_from_tid(cdev, tid);
	if (!csk) {
		cstor_err(cdev, "tid %u, get_sock_from_tid() failed\n", tid);
		return;
	}

	skb_get(skb);
	csk->dskb = skb;
}

static void _process_timeout(struct cstor_device *cdev, struct sk_buff *skb)
{
	struct cstor_sock *csk = cstor_skcb_rx_csk(skb);
	bool disconnect = false;

	mutex_lock(&csk->mutex);
	if (csk->state == CSTOR_SOCK_STATE_LOGIN_REQ_WAIT)
		disconnect = true;
	mutex_unlock(&csk->mutex);

	if (disconnect)
		cstor_sock_disconnect(csk);

	cstor_put_sock(csk);
}

#ifdef CONFIG_CHELSIO_T4_DCB
static void update_dcb_priority(struct cstor_device *cdev, struct sk_buff *skb)
{
	struct cstor_sock *csk;
	unsigned long index;
	u16 dcb_port, port_num = cstor_skcb_dcb_protocol(skb);
	u8 dcb_priority = cstor_skcb_dcb_priority(skb);
	u8 port_id = cstor_skcb_dcb_port_id(skb);

	xa_lock(&cdev->tids);
	xa_for_each(&cdev->tids, index, csk) {
		if (csk->state > CSTOR_SOCK_STATE_QP_MODE)
			continue;

		xa_unlock(&cdev->tids);

		mutex_lock(&csk->mutex);
		if ((csk->port_id != port_id) || (csk->dcb_priority == dcb_priority)) {
			mutex_unlock(&csk->mutex);
			xa_lock(&cdev->tids);
			continue;
		}

		if (csk->lcsk) {
			if (csk->laddr.ss_family == AF_INET6) {
				struct sockaddr_in6 *sock_in6 = (struct sockaddr_in6 *)&csk->laddr;

				dcb_port = be16_to_cpu(sock_in6->sin6_port);
			} else {
				struct sockaddr_in *sock_in = (struct sockaddr_in *)&csk->laddr;

				dcb_port = be16_to_cpu(sock_in->sin_port);
			}
		} else {
			if (csk->raddr.ss_family == AF_INET6) {
				struct sockaddr_in6 *sock_in6 = (struct sockaddr_in6 *)&csk->raddr;

				dcb_port = be16_to_cpu(sock_in6->sin6_port);
			} else {
				struct sockaddr_in *sock_in = (struct sockaddr_in *)&csk->raddr;

				dcb_port = be16_to_cpu(sock_in->sin_port);
			}
		}

		if (dcb_port != port_num) {
			mutex_unlock(&csk->mutex);
			xa_lock(&cdev->tids);
			continue;
		}

		set_bit(CSTOR_SOCK_H_ULP_DISCONNECT, &csk->history);
		if (csk->qp)
			cstor_modify_qp(csk->qp, CSTOR_QP_STATE_ERROR);
		mutex_unlock(&csk->mutex);

		cstor_sock_disconnect(csk);
		xa_lock(&cdev->tids);
	}
	xa_unlock(&cdev->tids);
}
#endif

/*
 * These are the real handlers that are called from a
 * work queue.
 */
static cstor_handler_func work_handlers[MAX_CPL_CMDS] = {
	[CPL_ACT_ESTABLISH] = act_establish,
	[CPL_ACT_OPEN_RPL] = act_open_rpl,
	[CPL_RX_DATA] = rx_data,
	[CPL_NVMT_CMP] = nvmt_cmp,
	[CPL_ISCSI_DATA] = iscsi_data,
	[CPL_RX_ISCSI_CMP] = rx_iscsi_cmp,
	[CPL_ABORT_RPL_RSS] = abort_rpl,
	[CPL_ABORT_RPL] = abort_rpl,
	[CPL_PASS_OPEN_RPL] = pass_open_rpl,
	[CPL_CLOSE_LISTSRV_RPL] = close_listsrv_rpl,
	[CPL_PASS_ACCEPT_REQ] = pass_accept_req,
	[CPL_PASS_ESTABLISH] = pass_establish,
	[CPL_PEER_CLOSE] = peer_close,
	[CPL_ABORT_REQ_RSS] = peer_abort,
	[CPL_CLOSE_CON_RPL] = close_con_rpl,
	[CPL_FW4_ACK] = fw4_ack,
	[CPL_SET_TCB_RPL] = set_tcb_rpl,
	[CPL_FW6_MSG] = deferred_fw6_msg,
	[FAKE_CPL_PUT_SOCK_SAFE] = _put_sock_safe,
	[FAKE_CPL_TIMEOUT] = _process_timeout,
#ifdef CONFIG_CHELSIO_T4_DCB
	[DCB_CPL_CONN_RESET] = update_dcb_priority,
#endif
};

void process_work(struct work_struct *work)
{
	struct cstor_device *cdev = container_of(work, struct cstor_device, rx_work);
	struct sk_buff *skb = NULL;
	struct cpl_act_establish *rpl;
	u32 opcode;

	while ((skb = skb_dequeue(&cdev->rxq))) {
		rpl = cplhdr(skb);
		opcode = rpl->ot.opcode;
		cstor_debug(cdev, "%u", opcode);
		work_handlers[opcode](cdev, skb);
		kfree_skb(skb);
	}
}

static void sock_timeout(struct timer_list *t)
{
	struct cstor_sock *csk = from_timer(csk, t, timer);
	struct sk_buff *req_skb = skb_dequeue(&csk->skb_list);
	struct cpl_act_establish *rpl;

	WARN_ON(!req_skb);
	set_bit(CSTOR_SOCK_H_TIMEDOUT, &csk->history);

	rpl = cplhdr(req_skb);
	rpl->ot.opcode = FAKE_CPL_TIMEOUT;
	cstor_skcb_rx_csk(req_skb) = csk;
	sched(csk->uctx->cdev, req_skb);
}

/*
 * All the CM events are handled on a work queue to have a safe context.
 */
void sched(struct cstor_device *cdev, struct sk_buff *skb)
{
	/*
	 * Queue the skb and schedule the worker thread.
	 */
	skb_queue_tail(&cdev->rxq, skb);

	spin_lock_bh(&cdev->slock);
	if (!test_bit(CDEV_FLAG_STOP_QUEUE_WORK, &cdev->flags))
		queue_work(cdev->workq, &cdev->rx_work);
	spin_unlock_bh(&cdev->slock);
}

static void fw6_msg(struct cstor_device *cdev, struct sk_buff *skb)
{
	struct cpl_fw6_msg *rpl = cplhdr(skb);
	struct cstor_wr_wait *wr_waitp;
	int ret;

	cstor_debug(cdev, "type %u\n", rpl->type);

	switch (rpl->type) {
	case FW6_TYPE_WR_RPL:
		ret = (int)((be64_to_cpu(rpl->data[0]) >> 8) & 0xff);
		wr_waitp = (__force struct cstor_wr_wait *)(unsigned long)rpl->data[1];

		if (wr_waitp)
			cstor_wake_up(wr_waitp, ret ? -ret : 0);
		__kfree_skb(skb);
		break;
	case FW6_TYPE_CQE:
		sched(cdev, skb);
		break;
	default:
		cstor_err(cdev, "unexpected fw6 msg type %u\n", rpl->type);
		__kfree_skb(skb);
		break;
	}
}

/*
 * Most upcalls from the T4 Core go to sched() to
 * schedule the processing on a work queue.
 */
cstor_handler_func cstor_handlers[NUM_CPL_CMDS] = {
	[CPL_ACT_ESTABLISH] = sched,
	[CPL_ACT_OPEN_RPL] = sched,
	[CPL_RX_DATA] = sched,
	[CPL_NVMT_CMP] = sched,
	[CPL_ISCSI_DATA] = sched,
	[CPL_RX_ISCSI_CMP] = sched,
	[CPL_ABORT_RPL_RSS] = sched,
	[CPL_ABORT_RPL] = sched,
	[CPL_PASS_OPEN_RPL] = sched,
	[CPL_CLOSE_LISTSRV_RPL] = sched,
	[CPL_PASS_ACCEPT_REQ] = sched,
	[CPL_PASS_ESTABLISH] = sched,
	[CPL_PEER_CLOSE] = sched,
	[CPL_CLOSE_CON_RPL] = sched,
	[CPL_ABORT_REQ_RSS] = sched,
	[CPL_FW4_ACK] = sched,
	[CPL_SET_TCB_RPL] = sched,
	[CPL_FW6_MSG] = fw6_msg,
};

static void queue_device_fatal_event(struct cstor_device *cdev)
{
	struct cstor_ucontext *uctx;
	struct cstor_event_channel *event_channel;
	struct cstor_uevent_node *uevt_node;
	struct cstor_uevent *uevt;
	unsigned long index;

	mutex_lock(&cdev->ucontext_list_lock);
	list_for_each_entry(uctx, &cdev->ucontext_list, entry) {
		xa_lock(&uctx->event_channels);
		xa_for_each(&uctx->event_channels, index, event_channel) {
			if (!(event_channel->flag & _CSTOR_EVENT_CHANNEL_FLAG_ASYNC_EVENT))
				continue;

			uevt_node = event_channel->fatal_uevt_node;
			event_channel->fatal_uevt_node = NULL;
			xa_unlock(&uctx->event_channels);

			uevt = &uevt_node->uevt;
			uevt->event = CSTOR_UEVENT_DEVICE_FATAL;
			add_to_uevt_list(event_channel, uevt_node);
			xa_lock(&uctx->event_channels);
		}
		xa_unlock(&uctx->event_channels);
	}
	mutex_unlock(&cdev->ucontext_list_lock);
}

void cstor_disable_device(struct cstor_device *cdev)
{
	if (test_bit(CDEV_FLAG_FATAL_ERROR, &cdev->flags)) {
		cstor_err(cdev, "Invalid cdev flags\n");
		return;
	}

	cstor_err(cdev, "Disabling device due to fatal error\n");

	set_bit(CDEV_FLAG_FATAL_ERROR, &cdev->flags);

	queue_device_fatal_event(cdev);

	/* Wake up anybody waiting for HW/FW messages */
	if (cdev->wr_waitp) {
		cstor_wake_up(cdev->wr_waitp, -EIO);
		cstor_debug(cdev, "woke up wr_waitp %p\n", cdev->wr_waitp);
	}

	cxgb4_fatal_err(cdev->lldi.ports[0]);
}

int cstor_resolve_route(struct cstor_ucontext *uctx, void __user *ubuf)
{
	struct cstor_device *cdev = uctx->cdev;
	struct net_device *ndev = NULL;
	struct neighbour *n;
	struct dst_entry *dst = NULL;
	struct cstor_resolve_route_cmd cmd;
	struct cstor_resolve_route_resp uresp;
	__u8 *peer_ip;
	void __user *_ubuf;
	int ret;
	u8 port_id;

	if (copy_from_user(&cmd, ubuf, sizeof(cmd))) {
		cstor_err(cdev, "copy_from_user() failed, cmd size %zu\n", sizeof(cmd));
		return -EFAULT;
	}

	if (cmd.raddr.ss_family == AF_INET) {
		struct sockaddr_in *raddr = (struct sockaddr_in *)&cmd.raddr;

		peer_ip = (__u8 *)&raddr->sin_addr;
		dst = find_route(cdev, NULL, 0, raddr->sin_addr.s_addr, 0, raddr->sin_port, 0);
		if (!dst) {
			cstor_err(cdev, "find_route() failed raddr %pI4\n", peer_ip);
			return -EHOSTUNREACH;
		}
	} else {
		struct sockaddr_in6 *raddr6 = (struct sockaddr_in6 *)&cmd.raddr;

		peer_ip = (__u8 *)&raddr6->sin6_addr;
		dst = find_route6(cdev, NULL, NULL, raddr6->sin6_addr.s6_addr,
				  raddr6->sin6_scope_id);
		if (!dst) {
			cstor_err(cdev, "find_route6() failed raddr %pI6\n", peer_ip);
			return -EHOSTUNREACH;
		}
	}

	n = dst_neigh_lookup(dst, peer_ip);
	if (!n) {
		cstor_err(cdev, "dst_neigh_lookup() failed\n");
		dst_release(dst);
		return -ENODEV;
	}

	if (!(n->nud_state & NUD_VALID))
		neigh_event_send(n, NULL);

	rcu_read_lock();
	if (n->dev->flags & IFF_LOOPBACK) {
		if (cmd.raddr.ss_family == AF_INET)
			ndev = cstor_ipv4_netdev(*(__be32 *)peer_ip);
		else
			ndev = cstor_ipv6_netdev((struct in6_addr *)peer_ip);
	} else {
		ndev = cstor_get_real_dev(n->dev);
	}

	if (!ndev) {
		rcu_read_unlock();
		cstor_err(cdev, "device not found\n");
		ret = -ENODEV;
		goto out;
	}

	port_id = cxgb4_port_idx(ndev);
	rcu_read_unlock();

	_ubuf = &((struct cstor_resolve_route_cmd *)ubuf)->resp;
	uresp.port_id = port_id;

	if (copy_to_user(_ubuf, &uresp, sizeof(uresp))) {
		cstor_err(cdev, "copy_to_user() failed, uresp size %zu\n", sizeof(uresp));
		ret = -EFAULT;
		goto out;
	}

	ret = 0;
out:
	neigh_release(n);
	dst_release(dst);
	return ret;
}

static int
send_connect_ipv4(struct cstor_sock *csk, struct sk_buff *skb,
		  u64 params, u64 opt0, u32 opt2, u32 isn, u32 len)
{
	struct cstor_device *cdev = csk->uctx->cdev;
	struct cpl_t7_act_open_req *req = NULL;
	struct sockaddr_in *la = (struct sockaddr_in *)&csk->laddr;
	struct sockaddr_in *ra = (struct sockaddr_in *)&csk->raddr;

	req = (struct cpl_t7_act_open_req *)__skb_put(skb, len);
	INIT_TP_WR(req, 0);
	OPCODE_TID(req) = cpu_to_be32(MK_OPCODE_TID(CPL_ACT_OPEN_REQ,
						    ((csk->rss_qid << 14) | csk->atid)));

	req->local_port = la->sin_port;
	req->peer_port = ra->sin_port;
	req->local_ip = la->sin_addr.s_addr;
	req->peer_ip = ra->sin_addr.s_addr;
	req->opt0 = cpu_to_be64(opt0);
	req->iss = cpu_to_be32(isn);
	req->opt2 = cpu_to_be32(opt2);
	req->params = cpu_to_be64(T7_FILTER_TUPLE_V(params));
	req->rsvd2 = cpu_to_be32(0);
	req->opt3 = cpu_to_be32(0);

	cstor_debug(cdev, "laddr: %pI4\n", &la->sin_addr.s_addr);

	return 0;
}

static int
send_connect_ipv6(struct cstor_sock *csk, struct sk_buff *skb,
		  u64 params, u64 opt0, u32 opt2, u32 isn, u32 len)
{
	struct cstor_device *cdev = csk->uctx->cdev;
	struct cpl_t7_act_open_req6 *req6 = NULL;
	struct sockaddr_in6 *la6 = (struct sockaddr_in6 *)&csk->laddr;
	struct sockaddr_in6 *ra6 = (struct sockaddr_in6 *)&csk->raddr;
	int ret;

	ret = cxgb4_clip_get(cdev->lldi.ports[0], (const u32 *)&la6->sin6_addr.s6_addr, 1);
	if (ret) {
		cstor_err(cdev, "cxgb4_clip_get() failed, ret %d\n", ret);
		return ret;
	}

	set_bit(CSTOR_SOCK_F_CLIP_RELEASE, &csk->flags);

	req6 = (struct cpl_t7_act_open_req6 *)__skb_put(skb, len);
	INIT_TP_WR(req6, 0);
	OPCODE_TID(req6) = cpu_to_be32(MK_OPCODE_TID(CPL_ACT_OPEN_REQ6,
						     ((csk->rss_qid << 14) | csk->atid)));

	req6->local_port = la6->sin6_port;
	req6->peer_port = ra6->sin6_port;
	req6->local_ip_hi = *((__be64 *)(la6->sin6_addr.s6_addr));
	req6->local_ip_lo = *((__be64 *)(la6->sin6_addr.s6_addr + 8));
	req6->peer_ip_hi = *((__be64 *)(ra6->sin6_addr.s6_addr));
	req6->peer_ip_lo = *((__be64 *)(ra6->sin6_addr.s6_addr + 8));
	req6->opt0 = cpu_to_be64(opt0);
	req6->iss = cpu_to_be32(isn);
	req6->opt2 = cpu_to_be32(opt2);
	req6->params = cpu_to_be64(T7_FILTER_TUPLE_V(params));
	req6->rsvd2 = cpu_to_be32(0);
	req6->opt3 = cpu_to_be32(0);

	cstor_debug(cdev, "laddr: %pI6\n", &la6->sin6_addr.s6_addr);
	return 0;
}

static int send_connect(struct cstor_sock *csk)
{
	struct cstor_device *cdev = csk->uctx->cdev;
	struct net_device *netdev = cdev->lldi.ports[0];
	struct sk_buff *skb;
	u64 params, opt0;
	u32 opt2, mtu_idx;
	u32 isn = (get_random_u32() & ~7UL) - 1;
	u32 sizev4 = sizeof(struct cpl_t7_act_open_req);
	u32 sizev6 = sizeof(struct cpl_t7_act_open_req6);
	u32 wrlen = roundup((csk->raddr.ss_family == AF_INET) ? sizev4 : sizev6, 16);
	int wscale, win;
	int ret = 0;

	cstor_debug(cdev, "atid %u snd_isn %u\n", csk->atid, isn);

	skb = alloc_skb(wrlen, GFP_KERNEL);
	if (!skb) {
		cstor_err(cdev, "atid %u, failed to alloc skb\n", csk->atid);
		return -ENOMEM;
	}

	set_wr_txq(skb, CPL_PRIORITY_SETUP, csk->ctrlq_idx);
	best_mtu(csk, &mtu_idx,	enable_tcp_timestamps);
	wscale = compute_wscale(csk->rcv_win);

	/*
	 * Specify the largest window that will fit in opt0. The
	 * remainder will be specified in the rx_data_ack.
	 */
	win = min_t(u32, csk->rcv_win >> 10, RCV_BUFSIZ_M);

	opt0 = TCAM_BYPASS_F |
	       (nocong ? NO_CONG_F : 0) |
	       KEEP_ALIVE_F |
	       DELACK_F |
	       MAX_RT_OVERRIDE_F |
	       MAX_RT_V(max_rt) |
	       WND_SCALE_V(wscale) |
	       MSS_IDX_V(mtu_idx) |
	       L2T_IDX_V(csk->l2t->idx) |
	       TX_CHAN_V(csk->tx_chan) |
	       SMAC_SEL_V(csk->smac_idx) |
	       DSCP_V(csk->tos >> 2) |
	       RCV_BUFSIZ_V(win);

	opt0 |= (csk->protocol == _CSTOR_ISCSI_PROTOCOL) ?
			ULP_MODE_V(ULP_MODE_ISCSI) : ULP_MODE_V(ULP_MODE_NVMET);

	opt2 = T5_OPT_2_VALID_F | PACE_V(1) | T5_ISS_F |
	       TX_QUEUE_V(cdev->lldi.tx_modq[csk->tx_chan]) |
	       RX_CHANNEL_V(0) | CCTRL_ECN_V(enable_ecn) |
	       RSS_QUEUE_VALID_F | RSS_QUEUE_V(csk->rss_qid) |
	       RX_FC_DISABLE_F | CONG_CNTRL_V(CONG_ALG_NEWRENO) |
	       (enable_tcp_sack ? SACK_EN_F : 0) |
	       (enable_tcp_timestamps ? TSTAMPS_EN_F : 0) |
	       ((wscale && enable_tcp_window_scaling) ? WND_SCALE_EN_F : 0);

	params = cxgb4_select_ntuple(netdev, csk->l2t);

	if (csk->raddr.ss_family == AF_INET)
		ret = send_connect_ipv4(csk, skb, params, opt0, opt2, isn, wrlen);
	else
		ret = send_connect_ipv6(csk, skb, params, opt0, opt2, isn, wrlen);

	if (ret) {
		__kfree_skb(skb);
		return ret;
	}

	t4_set_arp_err_handler(skb, csk, act_open_req_arp_failure);
	set_bit(CSTOR_SOCK_H_ACT_OPEN_REQ, &csk->history);
	return cstor_l2t_send(cdev, skb, csk);
}

int cstor_connect(struct cstor_ucontext *uctx, void __user *ubuf)
{
	struct cstor_device *cdev = uctx->cdev;
	struct cstor_sock *csk = NULL;
	struct cstor_connect_cmd cmd;
	struct cstor_connect_resp uresp;
	void __user *_ubuf;
	__u8 *ra;
	int iptype, ret;
	u16 lport, rport;

	if (copy_from_user(&cmd, ubuf, sizeof(cmd))) {
		cstor_err(cdev, "copy_from_user() failed, cmd size %zu\n", sizeof(cmd));
		return -EFAULT;
	}

	if ((cmd.protocol != _CSTOR_ISCSI_PROTOCOL) &&
	    (cmd.protocol != _CSTOR_NVME_TCP_PROTOCOL)) {
		cstor_err(cdev, "Invalid protocol %u\n", cmd.protocol);
		return -EINVAL;
	}

	csk = alloc_sock(uctx, NULL);
	if (!csk) {
		cstor_err(cdev, "cannot alloc csk\n");
		return -ENOMEM;
	}

	csk->event_channel = xa_load(&uctx->event_channels, cmd.efd);
	if (!csk->event_channel) {
		cstor_err(cdev, "Unable to find event channel for connection\n");
		ret = -EBADF;
		goto out;
	}

	kref_get(&csk->event_channel->kref);

	csk->protocol = cmd.protocol;

	/*
	 * Allocate an active TID to initiate a TCP connection.
	 */
	ret = cxgb4_uld_atid_alloc(cdev->lldi.ports[0], csk);
	if (ret < 0) {
		cstor_err(cdev, "cannot alloc atid, ret %d\n", ret);
		goto out;
	}

	csk->atid = ret;
	ret = xa_insert(&cdev->atids, csk->atid, csk, GFP_KERNEL);
	if (ret) {
		cstor_err(cdev, "atid %u, xa_insert() failed, ret %d\n", csk->atid, ret);
		cxgb4_uld_atid_free(cdev->lldi.ports[0], csk->atid);
		csk->atid = CSTOR_INVALID_ATID;
		goto out;
	}

	memcpy(&csk->raddr, &cmd.raddr, sizeof(csk->raddr));

	if (cmd.raddr.ss_family == AF_INET) {
		struct sockaddr_in *raddr = (struct sockaddr_in *)&csk->raddr;

		iptype = 4;
		ra = (__u8 *)&raddr->sin_addr;
		rport = be16_to_cpu(raddr->sin_port);
#if 0
		/* Handle loopback requests to INADDR_ANY. */
		if ((__force int)raddr->sin_addr.s_addr == INADDR_ANY) {
			err = pick_local_ipaddrs(dev, cm_id);
			if (err)
				goto fail4;
		}
#endif
		/* find a route */
		cstor_debug(cdev, "raddr %pI4 rport %#x\n", ra, be16_to_cpu(raddr->sin_port));

		csk->dst = find_route(cdev, (struct sockaddr_in *)&csk->laddr, 0,
				      raddr->sin_addr.s_addr, 0, raddr->sin_port, 0);
	} else {
		struct sockaddr_in6 *raddr6 = (struct sockaddr_in6 *)&csk->raddr;

		iptype = 6;
		ra = (__u8 *)&raddr6->sin6_addr;
		rport = be16_to_cpu(raddr6->sin6_port);
#if 0
		/*
		 * Handle loopback requests to INADDR_ANY.
		 */
		if (ipv6_addr_type(&raddr6->sin6_addr) == IPV6_ADDR_ANY) {
			err = pick_local_ip6addrs(dev, cm_id);
			if (err)
				goto fail4;
		}
#endif
		/* find a route */
		cstor_debug(cdev, "raddr %pI6 rport %#x\n", raddr6->sin6_addr.s6_addr,
			    be16_to_cpu(raddr6->sin6_port));

		csk->dst = find_route6(cdev, (struct sockaddr_in6 *)&csk->laddr, NULL,
				       raddr6->sin6_addr.s6_addr, raddr6->sin6_scope_id);
	}

	if (!csk->dst) {
		cstor_err(cdev, "atid %u, find_route() failed\n", csk->atid);
		ret = -EHOSTUNREACH;
		goto out;
	}

	ret = cstor_get_lport(csk, &lport);
	if (ret) {
		cstor_err(cdev, "cstor_get_lport() failed\n");
		goto out;
	}

	ret = import_sock(csk, iptype, ra, rport, 0);
	if (ret) {
		cstor_err(cdev, "atid %u, import_sock() failed, ret %d\n", csk->atid, ret);
		goto out;
	}

	cstor_debug(cdev, "atid %u txq_idx %u tx_chan %u smac_idx %u rss_qid %u l2t_idx %u\n",
		    csk->atid, csk->txq_idx, csk->tx_chan, csk->smac_idx, csk->rss_qid,
		    csk->l2t->idx);
	csk->state = CSTOR_SOCK_STATE_CONNECTING;
	csk->tos = 0;

	_ubuf = &((struct cstor_connect_cmd *)ubuf)->resp;
	uresp.atid = csk->atid;
	if (copy_to_user(_ubuf, &uresp, sizeof(uresp))) {
		cstor_err(cdev, "copy_to_user() failed, uresp size %zu\n", sizeof(uresp));
		ret = -EFAULT;
		goto out;
	}

	ret = send_connect(csk);
	if (ret) {
		cstor_err(cdev, "atid %u, failed send_connect(), ret %d\n", csk->atid, ret);
		csk->state = CSTOR_SOCK_STATE_DEAD;
		goto out;
	}

	atomic_inc(&csk->uctx->num_active_csk);
	cstor_get_sock(csk);
	set_bit(CSTOR_SOCK_F_APP_REF, &csk->flags);
	return 0;
out:
	cstor_put_sock(csk);
	return ret;
}

int cstor_find_device(struct cstor_ucontext *uctx, void __user *ubuf)
{
	struct cstor_device *cdev = uctx->cdev;
	struct net_device *ndev;
	struct sockaddr_storage laddr;
	struct cstor_find_device_cmd cmd;
	struct cstor_find_device_resp uresp;
	void __user *_ubuf;
	int ret;
	u8 port_id;

	if (copy_from_user(&cmd, ubuf, sizeof(cmd))) {
		cstor_err(cdev, "copy_from_user() failed, cmd size %zu\n", sizeof(cmd));
		return -EFAULT;
	}

	if (cmd.ipv4) {
		struct sockaddr_in *sin = (struct sockaddr_in *)&laddr;

		sin->sin_family = AF_INET;
		sin->sin_port = cmd.tcp_port;
		sin->sin_addr.s_addr = cmd.ip_addr[0];
	} else {
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&laddr;

		sin6->sin6_family = AF_INET6;
		sin6->sin6_port = cmd.tcp_port;
		memcpy(sin6->sin6_addr.s6_addr, cmd.ip_addr, sizeof(cmd.ip_addr));
	}

	rcu_read_lock();
	ndev = cstor_find_ndev(&laddr);
	if (!ndev) {
		rcu_read_unlock();
		if (cmd.ipv4)
			cstor_err(cdev, "net device not found, laddr %pI4\n", cmd.ip_addr);
		else
			cstor_err(cdev, "net device not found, laddr %pI6\n", cmd.ip_addr);

		return -ENODEV;
	}

	ret = cstor_find_port_id(cdev, ndev, &port_id);
	rcu_read_unlock();
	if (ret) {
		cstor_err(cdev, "error cstor_find_port_id(), ret %d\n", ret);
		return ret;
	}

	_ubuf = &((struct cstor_find_device_cmd *)ubuf)->resp;
	uresp.port_id = port_id;

	ret = copy_to_user(_ubuf, &uresp, sizeof(uresp));
	if (ret)
		cstor_err(cdev, "copy_to_user() failed, uresp size %zu, ret %d\n",
			  sizeof(uresp), ret);

	return ret;
}

int cstor_enable_iscsi_digest(struct cstor_ucontext *uctx, void __user *ubuf)
{
	struct cstor_device *cdev = uctx->cdev;
	struct cstor_sock *csk;
	struct sk_buff *skb = cdev->skb;
	struct cpl_set_tcb_field *req;
	struct cstor_enable_iscsi_digest_cmd cmd;
	u32 qpid = 0;
	int ret;

	if (copy_from_user(&cmd, ubuf, sizeof(cmd))) {
		cstor_err(cdev, "copy_from_user() failed, cmd size %zu\n", sizeof(cmd));
		return -EFAULT;
	}

	csk = get_sock_from_tid(cdev, cmd.tid);
	if (!csk) {
		cstor_err(cdev, "Failed to get csk with tid %u\n", cmd.tid);
		return -EINVAL;
	}

	if (csk->qp) {
		qpid = csk->qp->wq.sq.qid;
		csk->qp->attr.hdgst = cmd.dgst.hdgst;
		csk->qp->attr.ddgst = cmd.dgst.ddgst;
	}

	skb_trim(skb, 0);
	skb_get(skb);
	set_wr_txq(skb, CPL_PRIORITY_CONTROL, csk->ctrlq_idx);

	req = (struct cpl_set_tcb_field *)__skb_put_zero(skb, roundup(sizeof(*req), 16));
	INIT_TP_WR(req, csk->tid);
	OPCODE_TID(req) = cpu_to_be32(MK_OPCODE_TID(CPL_SET_TCB_FIELD, csk->tid));

	req->reply_ctrl = cpu_to_be16(NO_REPLY_V(0) | T7_QUEUENO_V(csk->rss_qid) |
				      T7_REPLY_CHAN_V(csk->rx_chan));
	req->word_cookie = cpu_to_be16(0);
	req->mask = cpu_to_be64(0x3 << 4);
	req->val = cpu_to_be64(((cmd.dgst.hdgst ? ULP_CRC_HEADER : 0) |
			       (cmd.dgst.ddgst ? ULP_CRC_DATA : 0)) << 4);

	cstor_reinit_wr_wait(&csk->wr_wait);
	ret = cstor_send_wait(cdev, skb, &csk->wr_wait, csk->tid, qpid, __func__);
	if (ret)
		cstor_err(cdev, "cstor_send_wait() failed, cmd.dgst.hdgst %u ret %d"
			  "cmd.dgst.ddgst %u\n", cmd.dgst.hdgst, cmd.dgst.ddgst, ret);

	return ret;
}

#define OFLD_TX_DATA_WR_LEN (sizeof(struct fw_ofld_tx_data_wr))
#define TX_HDR_LEN (sizeof(struct sge_opaque_hdr) + OFLD_TX_DATA_WR_LEN)

static int cstor_is_ofld_imm(struct cstor_sock *csk, const struct sk_buff *skb)
{
	return ((skb->len + OFLD_TX_DATA_WR_LEN) <= MAX_IMM_OFLD_TX_DATA_WR_LEN);
}

static inline u32 cstor_sgl_len(u32 n)
{
	n--;
	return (3 * n) / 2 + (n & 1) + 2;
}

static u32 cstor_calc_tx_flits_ofld(struct cstor_sock *csk, const struct sk_buff *skb)
{
	u32 flits = skb_transport_offset(skb) / 8;
	u32 cnt = skb_shinfo(skb)->nr_frags;

	if (skb_tail_pointer(skb) != skb_transport_header(skb))
		cnt++;

	return flits + cstor_sgl_len(cnt);
}

static void
cstor_tx_data_wr(struct cstor_sock *csk, struct sk_buff *skb, u32 dlen, u32 len, u32 credits)
{
	struct fw_ofld_tx_data_wr *req;
	u32 submode = cstor_skcb_submode(skb);
	u32 hdr_size = sizeof(*req);
	u32 immlen = 0;

	if (cstor_is_ofld_imm(csk, skb))
		immlen += dlen;

	req = __skb_push(skb, hdr_size);
	req->op_to_immdlen = cpu_to_be32(FW_WR_OP_V(FW_OFLD_TX_DATA_WR) |
					 FW_WR_COMPL_V(1) | FW_WR_IMMDLEN_V(immlen));

	req->flowid_len16 = cpu_to_be32(FW_WR_FLOWID_V(csk->tid) | FW_WR_LEN16_V(credits));

	req->plen = cpu_to_be32(len);

	req->tunnel_to_proxy = cpu_to_be32(TX_ULP_MODE_V(ULP_MODE_ISCSI) |
					   TX_ULP_SUBMODE_V(submode) |
					   T6_TX_FORCE_F | TX_SHOVE_V(1U));
}

static int cstor_send_skb(struct cstor_sock *csk, struct sk_buff *skb)
{
	u32 dlen = skb->len;
	u32 len = skb->len;
	u32 credits = 0;

	if (cstor_is_ofld_imm(csk, skb))
		credits = DIV_ROUND_UP(dlen, 16);
	else
		credits = DIV_ROUND_UP((8 * cstor_calc_tx_flits_ofld(csk, skb)), 16);

	credits += DIV_ROUND_UP(OFLD_TX_DATA_WR_LEN, 16);

	if (csk->wr_cred < credits) {
		cstor_err(csk->uctx->cdev, "tid %u, skb %u/%u, wr %d < %u.\n",
			  csk->tid, skb->len, skb->data_len, credits, csk->wr_cred);
		kfree_skb(skb);
		return -ENOMEM;
	}

	set_bit(CSTOR_SOCK_F_VALIDATE_CREDITS, &csk->flags);

	set_wr_txq(skb, CPL_PRIORITY_DATA, csk->txq_idx);
	skb->csum = (__force __wsum)(credits);
	csk->wr_cred -= credits;

	len += cstor_skcb_tx_extralen(skb);

	cstor_tx_data_wr(csk, skb, dlen, len, credits);
	cstor_sock_enqueue_wr(csk, skb);

	t4_set_arp_err_handler(skb, csk, arp_failure_discard);

	cstor_debug(csk->uctx->cdev, "tid %u, skb %u/%u, len %u, wr %d, left %u.\n",
		    csk->tid, skb->len, skb->data_len, len, credits, csk->wr_cred);

	return cstor_l2t_send(csk->uctx->cdev, skb, csk);
}

int cstor_send_iscsi_pdu(struct cstor_ucontext *uctx, void __user *ubuf)
{
	struct cstor_sock *csk;
	struct sk_buff *skb = NULL;
	struct cstor_send_iscsi_pdu_cmd cmd;
	int ret = 0;

	if (copy_from_user(&cmd, ubuf, sizeof(cmd))) {
		cstor_err(uctx->cdev, "copy_from_user() failed, cmd size %zu\n", sizeof(cmd));
		return -EFAULT;
	}

	csk = get_sock_from_tid(uctx->cdev, cmd.tid);
	if (!csk) {
		cstor_err(uctx->cdev, "tid %u, get_sock_from_tid() failed\n", cmd.tid);
		return -EINVAL;
	}

	mutex_lock(&csk->mutex);
	if (unlikely(csk->state != CSTOR_SOCK_STATE_CONNECTED)) {
		cstor_err(uctx->cdev, "tid %u state %s (%d)\n",
			  csk->tid, states[csk->state], csk->state);
		ret = -EINVAL;
		goto err;
	}

	if ((TX_HDR_LEN + cmd.buf_len) > 2048) {
		cstor_err(uctx->cdev, "tid %u, size(%lu) larger than 2048 bytes\n",
			  cmd.tid, TX_HDR_LEN + cmd.buf_len);
		ret = -EINVAL;
		goto err;
	}

	skb = alloc_skb(TX_HDR_LEN + cmd.buf_len, GFP_KERNEL);
	if (unlikely(!skb)) {
		cstor_err(uctx->cdev, "tid %u, failed to alloc socket buffer\n", cmd.tid);
		ret = -ENOMEM;
		goto err;
	}

	skb_reserve(skb, TX_HDR_LEN);
	skb_reset_transport_header(skb);

	skb_put(skb, cmd.buf_len);
	if (copy_from_user(skb->data, (void __user *)cmd.buf, cmd.buf_len)) {
		cstor_err(uctx->cdev, "copy_from_user() failed, length %u\n", cmd.buf_len);
		ret = -EFAULT;
		kfree_skb(skb);
		goto err;
	}

	if (cmd.hdgst) {
		cstor_skcb_tx_extralen(skb) += 4;
		cstor_skcb_submode(skb) |= ULP_CRC_HEADER;
	}

	if (cmd.ddgst) {
		cstor_skcb_tx_extralen(skb) += 4;
		cstor_skcb_submode(skb) |= ULP_CRC_DATA;
	}

	ret = cstor_send_skb(csk, skb);
	if (ret)
		cstor_err(uctx->cdev, "tid %u, failed to send pdu, len %u, ret %d\n",
			  csk->tid, cmd.buf_len, ret);

err:
	mutex_unlock(&csk->mutex);
	return ret;
}
