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
#include <linux/kconfig.h>
#include <linux/slab.h>
#include <linux/mman.h>
#include <net/sock.h>

#include "cstor.h"

void cstor_ev_dispatch(struct cstor_device *cdev, void *err_cqe)
{
	struct cpl_rdma_cqe_err *rpl = err_cqe;
	struct cstor_sock *csk;
	struct cstor_qp *qp;
	u32 qpid = CPL_RDMA_CQE_ERR_QPID_G(be32_to_cpu(rpl->qpid_to_wr_type));
	u32 status = CPL_RDMA_CQE_ERR_STATUS_G(be32_to_cpu(rpl->qpid_to_wr_type));

	cstor_err(cdev, "CQE_ERR : opcode_tid %u tid_flitcnt %u qid_to_wr_type %u "
		  "length %u tag %u msn %u\n", be32_to_cpu(rpl->ot.opcode_tid),
		  be32_to_cpu(rpl->tid_flitcnt), be32_to_cpu(rpl->qpid_to_wr_type),
		  be32_to_cpu(rpl->length), be32_to_cpu(rpl->tag), be32_to_cpu(rpl->msn));

	qp = get_qp(cdev, qpid);
	if (!qp) {
		cstor_err(cdev, "qpid %u, get_qp() failed\n", qpid);
		return;
	}

	csk = qp->csk;

	switch (status) {
	/* Completion Events */
	case T4_ERR_SUCCESS:
		cstor_err(cdev, "CQE SUCCESS tid %u\n", csk->tid);
		break;

	case T4_ERR_STAG:
	case T4_ERR_PDID:
	case T4_ERR_QPID:
	case T4_ERR_ACCESS:
	case T4_ERR_WRAP:
	case T4_ERR_BOUND:
	case T4_ERR_INVALIDATE_SHARED_MR:
	case T4_ERR_INVALIDATE_MR_WITH_MW_BOUND:
	case T4_ERR_OUT_OF_RQE:
	case T4_ERR_PBL_ADDR_BOUND:
	case T4_ERR_CRC:
	case T4_ERR_MARKER:
	case T4_ERR_PDU_LEN_ERR:
	case T4_ERR_DDP_VERSION:
	case T4_ERR_RDMA_VERSION:
	case T4_ERR_OPCODE:
	case T4_ERR_DDP_QUEUE_NUM:
	case T4_ERR_MSN:
	case T4_ERR_TBIT:
	case T4_ERR_MO:
	case T4_ERR_MSN_GAP:
	case T4_ERR_MSN_RANGE:
	case T4_ERR_RQE_ADDR_BOUND:
	case T4_ERR_IRD_OVERFLOW:
		cstor_err(cdev, "CQE error for tid %u state %d status %d\n",
			  csk->tid, csk->state, status);
		break;

	/* Device Fatal Errors */
	case T4_ERR_ECC:
	case T4_ERR_ECC_PSTAG:
	case T4_ERR_INTERNAL_ERR:
		cstor_err(cdev, "CQE device fatal error for tid %u state %d status %d\n",
			  csk->tid, csk->state, status);
		cstor_disable_device(cdev);
		break;

	default:
		cstor_err(cdev, "Unknown CQE error for tid %u state %d status %d\n",
			  csk->tid, csk->state, status);
		break;
	}
}

int cstor_ev_handler(struct cstor_device *cdev, u32 qid, u32 pidx)
{
	struct cstor_cq *cq;

	cq = get_cq(cdev, qid);
	if (cq) {
		cq->q.status->cq_armed = 0;
		if (cq->eventfd_ctx)
			eventfd_signal(cq->eventfd_ctx);
		else
			cstor_err(cdev, "cqid %u, CQ is armed without creating eventfd\n",
				  cq->q.cqid);
	}

	return 0;
}

static int cstor_copy_skbs_to_user(struct sk_buff *skb, void __user *ubuf, u32 buf_len)
{
	struct skb_shared_info *ssi = skb_shinfo(skb);
	void *ptr = NULL;
	u32 dlen = skb->len - skb->data_len;
	u32 offset = 0, i;

	if (buf_len < skb->len)
		return -ENOBUFS;

	if (dlen) {
		if (copy_to_user((void __user *)(u8 *)ubuf, skb->data, dlen))
			return -EFAULT;

		offset += dlen;
	}

	for (i = 0; i < ssi->nr_frags; i++) {
		ptr = skb_frag_address(&ssi->frags[i]);
		dlen = skb_frag_size(&ssi->frags[i]);

		if (copy_to_user((void __user *)((u8 *)ubuf + offset), ptr, dlen))
			return -EFAULT;

		offset += dlen;
	}

	return 0;
}

int cstor_get_event(struct cstor_ucontext *uctx, void __user *ubuf)
{
	struct cstor_event_channel *event_channel;
	struct cstor_uevent_node *uevt_node;
	struct cstor_get_uevent_cmd cmd;
	struct cstor_get_uevent_resp uresp;
	void __user *_ubuf;
	int ret = 0;

	if (copy_from_user(&cmd, ubuf, sizeof(cmd))) {
		cstor_err(uctx->cdev, "copy_from_user() failed, cmd size %zu\n", sizeof(cmd));
		return -EFAULT;
	}

	event_channel = xa_load(&uctx->event_channels, cmd.efd);
	if (!event_channel) {
		cstor_err(uctx->cdev, "invalid event file descriptor %u\n", cmd.efd);
		return -EBADF;
	}

	mutex_lock(&event_channel->uevt_list_lock);
	uevt_node = list_first_entry_or_null(&event_channel->uevt_list,
					     struct cstor_uevent_node, entry);

	if (!uevt_node) {
		cstor_err(uctx->cdev, "empty uevent list\n");
		ret = -EINVAL;
		goto out;
	}

	if ((uevt_node->uevt.event == CSTOR_UEVENT_CONNECT_REQ) ||
	    (uevt_node->uevt.event == CSTOR_UEVENT_RECV_ISCSI_PDU)) {
		if (!cmd.buf) {
			cstor_err(uctx->cdev, "no buffer given for event\n");
			ret = -ENOBUFS;
			goto out;
		}

		ret = cstor_copy_skbs_to_user(uevt_node->skb, (void __user *)cmd.buf, cmd.buf_len);
		if (ret) {
			cstor_err(uctx->cdev, "cstor_copy_skbs_to_user() failed, len %u\n",
				  cmd.buf_len);
			goto out;
		}
	}

	memcpy(&uresp.uevt, &uevt_node->uevt, sizeof(struct cstor_uevent));
	_ubuf = &((struct cstor_get_uevent_cmd *)ubuf)->resp;
	if (copy_to_user(_ubuf, &uresp, sizeof(uresp))) {
		cstor_err(uctx->cdev, "copy_to_user() failed, uresp size %zu\n", sizeof(uresp));
		ret = -EFAULT;
		goto out;
	}

	cstor_free_uevent_node(uevt_node);
out:
	mutex_unlock(&event_channel->uevt_list_lock);
	return ret;
}

int cstor_destroy_event_channel(struct cstor_ucontext *uctx, void __user *ubuf)
{
	struct cstor_event_channel *event_channel;
	struct cstor_destroy_event_channel_cmd cmd;

	if (copy_from_user(&cmd, ubuf, sizeof(cmd))) {
		cstor_err(uctx->cdev, "copy_from_user() failed, cmd size %zu\n", sizeof(cmd));
		return -EINVAL;
	}

	event_channel = xa_load(&uctx->event_channels, cmd.efd);
	if (!event_channel) {
		cstor_err(uctx->cdev, "invalid event file descriptor, cmd.efd %u\n", cmd.efd);
		return -EBADF;
	}

	if (kref_read(&event_channel->kref) > 1) {
		cstor_err(uctx->cdev, "event channel in use\n");
		return -EBUSY;
	}

	kref_put(&event_channel->kref, _cstor_free_event_channel);

	return 0;
}

int cstor_create_event_channel(struct cstor_ucontext *uctx, void __user *ubuf)
{
	struct cstor_event_channel *event_channel;
	struct cstor_create_event_channel_cmd cmd;
	int ret;

	if (copy_from_user(&cmd, ubuf, sizeof(cmd))) {
		cstor_err(uctx->cdev, "copy_from_user() failed, cmd size %zu\n", sizeof(cmd));
		return -EINVAL;
	}

	event_channel = kzalloc(sizeof(*event_channel), GFP_KERNEL);
	if (!event_channel) {
		cstor_err(uctx->cdev, "event channel allocation failed\n");
		return -ENOMEM;
	}

	event_channel->efd = cmd.efd;
	event_channel->flag = cmd.flag;
	event_channel->efd_ctx = eventfd_ctx_fdget(cmd.efd);

	if (IS_ERR(event_channel->efd_ctx)) {
		ret = PTR_ERR(event_channel->efd_ctx);
		cstor_err(uctx->cdev, "eventfd_ctx_fdget() failed, cmd.efd %u ret %d\n",
			  cmd.efd, ret);
		kfree(event_channel);
		return ret;
	}

	if (event_channel->flag & _CSTOR_EVENT_CHANNEL_FLAG_ASYNC_EVENT) {
		event_channel->fatal_uevt_node = kzalloc(sizeof(*event_channel->fatal_uevt_node),
							 GFP_KERNEL);
		if (!event_channel->fatal_uevt_node) {
			cstor_err(uctx->cdev, "fatal_uevt_node allocation failed\n");
			eventfd_ctx_put(event_channel->efd_ctx);
			kfree(event_channel);
			return -ENOMEM;
		}
	}

	event_channel->uctx = uctx;
	kref_init(&event_channel->kref);
	INIT_LIST_HEAD(&event_channel->uevt_list);
	mutex_init(&event_channel->uevt_list_lock);

	ret = xa_insert(&uctx->event_channels, cmd.efd, event_channel, GFP_KERNEL);
	if (ret) {
		cstor_err(uctx->cdev, "xa_insert() failed, cmd.efd %u ret %d\n", cmd.efd, ret);
		mutex_destroy(&event_channel->uevt_list_lock);
		eventfd_ctx_put(event_channel->efd_ctx);
		kfree(event_channel->fatal_uevt_node);
		kfree(event_channel);
		return ret;
	}

	return 0;
}
