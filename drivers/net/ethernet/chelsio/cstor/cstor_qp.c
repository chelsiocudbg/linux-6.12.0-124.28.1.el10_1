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

#include "cstor.h"

static void free_rc_queues(struct cstor_qp *qp, int has_rq)
{
	struct cstor_device *cdev = qp->uctx->cdev;
	struct t4_wq *wq = &qp->wq;

	/*
	 * uP clears EQ contexts when the connection exits rdma mode,
	 * so no need to post a RESET WR for these EQs.
	 */
	if (has_rq) {
		dma_free_coherent(&cdev->lldi.pdev->dev, wq->rq.memsize, wq->rq.queue,
				  wq->rq.dma_addr);
		cxgb4_uld_free_rqtpool(cdev->rdma_res, wq->rq.rqt_hwaddr, wq->rq.rqt_size);
		cxgb4_uld_put_qpid(&qp->uctx->d_uctx, wq->rq.qid);
	}

	dma_free_coherent(&cdev->lldi.pdev->dev, wq->sq.memsize, wq->sq.queue, wq->sq.dma_addr);
	cxgb4_uld_put_qpid(&qp->uctx->d_uctx, wq->sq.qid);
}

int
cstor_get_db_gts_phys_addr(struct cstor_device *cdev, u32 qid, enum t4_bar2_qtype qtype,
			   u32 *pbar2_qid, u64 *db_gts_pa)
{
	u64 bar2_qoffset;
	int ret;

	if (cdev->lldi.plat_dev) {
		*db_gts_pa = cdev->lldi.db_gts_pa;
	} else {
		/*
		 * Determine the BAR2 offset and qid. db_gts_pa is not NULL for user mapping
		 * so compute the page-aligned physical address for mapping.
		 */
		ret = cxgb4_bar2_sge_qregs(cdev->lldi.ports[0], qid, (enum cxgb4_bar2_qtype)qtype,
					   1, &bar2_qoffset, pbar2_qid);
		if (ret) {
			cstor_err(cdev, "cxgb4_bar2_sge_qregs() failed, ret %d\n", ret);
			return ret;
		}

		*db_gts_pa = (cdev->bar2_pa + bar2_qoffset) & PAGE_MASK;
	}

	return 0;
}

static int
alloc_rc_queues(struct cstor_qp *qp, struct cstor_cq *rcq, struct cstor_cq *scq, int need_rq)
{
	struct cstor_device *cdev = qp->uctx->cdev;
	struct t4_wq *wq = &qp->wq;
	struct sk_buff *skb = cdev->skb;
	struct fw_ri_res_wr *res_wr;
	struct fw_ri_res *res;
	u32 wr_len;
	int eqsize, ret = -ENOMEM;

	wq->sq.qid = cxgb4_uld_get_qpid(cdev->rdma_res, &qp->uctx->d_uctx);
	if (!wq->sq.qid) {
		cstor_err(cdev, "failed allocate sq qid\n");
		return ret;
	}

	if (need_rq) {
		wq->rq.qid = cxgb4_uld_get_qpid(cdev->rdma_res, &qp->uctx->d_uctx);
		if (!wq->rq.qid) {
			cstor_err(cdev, "failed to allocate rq qid\n");
			goto err1;
		}
		/*
		 * RQT must be a power of 2 and at least 16 deep.
		 */
		wq->rq.rqt_size = roundup_pow_of_two(max_t(u16, wq->rq.max_wr, 16));
		wq->rq.rqt_hwaddr = cxgb4_uld_alloc_rqtpool(cdev->rdma_res, wq->rq.rqt_size);
		if (!wq->rq.rqt_hwaddr) {
			cstor_err(cdev, "failed to allocate rq rqt_hwaddr\n");
			goto err2;
		}
	}

	cstor_debug(cdev, "sq qid %u, rq qid %u\n", wq->sq.qid, wq->rq.qid);

	wq->sq.queue = dma_alloc_coherent(&cdev->lldi.pdev->dev, wq->sq.memsize, &wq->sq.dma_addr,
					  GFP_KERNEL);
	if (!wq->sq.queue) {
		cstor_err(cdev, "failed to allocate sq->queue\n");
		goto err3;
	}

	memset(wq->sq.queue, 0, wq->sq.memsize);

	if (need_rq) {
		wq->rq.queue = dma_alloc_coherent(&cdev->lldi.pdev->dev, wq->rq.memsize,
						  &wq->rq.dma_addr, GFP_KERNEL);
		if (!wq->rq.queue) {
			cstor_err(cdev, "failed to allocate wq->rq.queue\n");
			goto err4;
		}
	}

	cstor_debug(cdev, "sq base va %p pa %#llx rq base va %p pa %#llx\n",
		    wq->sq.queue, (u64)virt_to_phys(wq->sq.queue),
		    wq->rq.queue, need_rq ? (u64)virt_to_phys(wq->rq.queue) : 0);

	ret = cstor_get_db_gts_phys_addr(cdev, wq->sq.qid, T4_BAR2_QTYPE_EGRESS, &wq->sq.bar2_qid,
					 &wq->sq.db_gts_pa);
	if (ret) {
		cstor_err(cdev, "failed to get bar2 addr for sqid %u, ret %d\n", wq->sq.qid, ret);
		goto err5;
	}

	if (need_rq) {
		ret = cstor_get_db_gts_phys_addr(cdev, wq->rq.qid, T4_BAR2_QTYPE_EGRESS,
						 &wq->rq.bar2_qid, &wq->rq.db_gts_pa);
		if (ret) {
			cstor_err(cdev, "failed to get bar2 addr for rqid %u, ret %d\n",
				  wq->rq.qid, ret);
			goto err5;
		}
	}

	/* build fw_ri_res_wr */
	wr_len = sizeof(*res_wr) + sizeof(*res);
	if (need_rq)
		wr_len += sizeof(*res);

	ret = xa_insert(&cdev->qps, qp->wq.sq.qid, qp, GFP_KERNEL);
	if (ret) {
		cstor_err(cdev, "xa_insert() failed, sqid %u ret %d\n", qp->wq.sq.qid, ret);
		goto err5;
	}

	skb_trim(skb, 0);
	skb_get(skb);
	set_wr_txq(skb, CPL_PRIORITY_CONTROL, cdev->lldi.ctrlq_start);

	res_wr = (struct fw_ri_res_wr *)__skb_put_zero(skb, wr_len);
	res_wr->op_nres = cpu_to_be32(FW_WR_OP_V(FW_RI_RES_WR) |
				      FW_RI_RES_WR_NRES_V(need_rq ? 2 : 1) | FW_WR_COMPL_F);
	res_wr->len16_pkd = cpu_to_be32(DIV_ROUND_UP(wr_len, 16));
	res_wr->cookie = (uintptr_t)&cdev->wr_wait;
	res = res_wr->res;
	res->u.sqrq.restype = FW_RI_RES_TYPE_SQ;
	res->u.sqrq.op = FW_RI_RES_OP_WRITE;

	/*
	 * eqsize is the number of 64B entries plus the status page size.
	 */
	eqsize = wq->sq.size + cdev->hw_queue.t4_eq_status_entries;

	res->u.sqrq.fetchszm_to_iqid = cpu_to_be32(FW_RI_RES_WR_HOSTFCMODE_V(0) | /* no host cidx updates */
					FW_RI_RES_WR_CPRIO_V(0) | /* don't keep in chip cache */
					FW_RI_RES_WR_PCIECHN_V(0) | /* set by uP at ri_init time */
					FW_RI_RES_WR_FETCHRO_V(cdev->lldi.relaxed_ordering) |
					//FW_RI_RES_WR_IQID_V(scq->cqid));
					FW_RI_RES_WR_IQID_V(scq ? scq->q.cqid : 0));
	res->u.sqrq.dcaen_to_eqsize = cpu_to_be32(FW_RI_RES_WR_DCAEN_V(0) |
						  FW_RI_RES_WR_DCACPU_V(0) |
						  FW_RI_RES_WR_FBMIN_V(2) |
						  FW_RI_RES_WR_FBMAX_V(3) |
						  FW_RI_RES_WR_CIDXFTHRESHO_V(0) |
						  FW_RI_RES_WR_CIDXFTHRESH_V(0) |
						  FW_RI_RES_WR_EQSIZE_V(eqsize));
	res->u.sqrq.eqid = cpu_to_be32(wq->sq.qid);
	res->u.sqrq.eqaddr = cpu_to_be64(wq->sq.dma_addr);
	if (need_rq) {
		res++;
		res->u.sqrq.restype = FW_RI_RES_TYPE_RQ;
		res->u.sqrq.op = FW_RI_RES_OP_WRITE;

		/*
		 * eqsize is the number of 64B entries plus the status page size.
		 */
		eqsize = wq->rq.size + cdev->hw_queue.t4_eq_status_entries;
		res->u.sqrq.fetchszm_to_iqid = cpu_to_be32(FW_RI_RES_WR_HOSTFCMODE_V(0) | /* no host cidx updates */
					FW_RI_RES_WR_CPRIO_V(0) | /* don't keep in chip cache */
					FW_RI_RES_WR_PCIECHN_V(0) | /* set by uP at ri_init time */
					FW_RI_RES_WR_FETCHRO_V(cdev->lldi.relaxed_ordering) |
					//FW_RI_RES_WR_IQID_V(rcq->cqid));
					FW_RI_RES_WR_IQID_V(rcq ? rcq->q.cqid : 0));
		res->u.sqrq.dcaen_to_eqsize = cpu_to_be32(FW_RI_RES_WR_DCAEN_V(0) |
							  FW_RI_RES_WR_DCACPU_V(0) |
							  FW_RI_RES_WR_FBMIN_V(2) |
							  FW_RI_RES_WR_FBMAX_V(3) |
							  FW_RI_RES_WR_CIDXFTHRESHO_V(0) |
							  FW_RI_RES_WR_CIDXFTHRESH_V(0) |
							  FW_RI_RES_WR_EQSIZE_V(eqsize));
		res->u.sqrq.eqid = cpu_to_be32(wq->rq.qid);
		res->u.sqrq.eqaddr = cpu_to_be64(wq->rq.dma_addr);
	}

	cstor_reinit_wr_wait(&cdev->wr_wait);

	ret = cstor_send_wait(cdev, skb, &cdev->wr_wait, 0, wq->sq.qid, __func__);
	if (ret) {
		cstor_err(cdev, "failed cstor_send_wait(), sqid %u ret %d\n", wq->sq.qid, ret);
		if (ret == -ETIMEDOUT)
			return ret;
		goto err6;
	}

	cstor_debug(cdev, "sqid %#x rqid %#x\n", wq->sq.qid, wq->rq.qid);

	return 0;
err6:
	xa_erase(&cdev->qps, qp->wq.sq.qid);
err5:
	if (need_rq)
		dma_free_coherent(&cdev->lldi.pdev->dev, wq->rq.memsize, wq->rq.queue,
				  wq->rq.dma_addr);
err4:
	dma_free_coherent(&cdev->lldi.pdev->dev, wq->sq.memsize, wq->sq.queue, wq->sq.dma_addr);
err3:
	if (need_rq)
		cxgb4_uld_free_rqtpool(cdev->rdma_res, wq->rq.rqt_hwaddr, wq->rq.rqt_size);
err2:
	if (need_rq)
		cxgb4_uld_put_qpid(&qp->uctx->d_uctx, wq->rq.qid);
err1:
	cxgb4_uld_put_qpid(&qp->uctx->d_uctx, wq->sq.qid);
	return ret;
}

static int cstor_enable_iscsi_qp(struct cstor_qp *qp)
{
	struct cstor_device *cdev = qp->uctx->cdev;
	struct cstor_sock *csk = qp->csk;
	struct sk_buff *skb;
	struct fw_ri_wr *wqe;
	u32 len16 = DIV_ROUND_UP(sizeof(*wqe), 16);
	int ret;
	u32 psz_idx;

	skb = alloc_skb(sizeof(*wqe), GFP_KERNEL | __GFP_NOFAIL);
	if (!skb) {
		cstor_err(cdev, "tid %u, failed to allocate socket buffer\n", csk->tid);
		return -ENOMEM;
	}

	cstor_find_iscsi_page_size_idx(cdev, qp->attr.ddp_page_size, &psz_idx);
	csk->wr_cred -= len16;

	set_wr_txq(skb, CPL_PRIORITY_DATA, csk->txq_idx);

	wqe = (struct fw_ri_wr *)__skb_put_zero(skb, sizeof(*wqe));
	wqe->op_compl = cpu_to_be32(FW_WR_OP_V(FW_RI_INIT_WR) | FW_WR_COMPL_F |
				    FW_RI_WR_TRANSPORT_TYPE_V(FW_QP_TRANSPORT_TYPE_ISCSI));
	wqe->flowid_len16 = cpu_to_be32(FW_WR_FLOWID_V(csk->tid) | FW_WR_LEN16_V(len16));
	wqe->cookie = (uintptr_t)&csk->wr_wait;

	wqe->u.iscsi_init.type = FW_RI_TYPE_INIT;
	wqe->u.iscsi_init.dcrc_dis_to_hcrc = FW_RI_WR_PSZ_IDX_V(psz_idx);
	wqe->u.iscsi_init.qp_caps = FW_RI_QP_FAST_REGISTER_ENABLE;
	wqe->u.iscsi_init.pdid = cpu_to_be32(qp->attr.pdid);
	wqe->u.iscsi_init.sq_eqid = cpu_to_be32(qp->wq.sq.qid);
	wqe->u.iscsi_init.scqid = cpu_to_be32(qp->rxq->iq.cntxt_id);
	wqe->u.iscsi_init.rcqid = cpu_to_be32(qp->rxq->iq.cntxt_id);

	cstor_reinit_wr_wait(&csk->wr_wait);
	ret = cstor_send_wait(cdev, skb, &csk->wr_wait, csk->tid, qp->wq.sq.qid, __func__);
	if (ret)
		cstor_err(cdev, "tid %u sqid %u, cstor_send_wait() failed, ret %d\n",
			  csk->tid, qp->wq.sq.qid, ret);

	cstor_debug(cdev, "tid %u, ret %d\n", csk->tid, ret);
	return ret;
}

static int cstor_enable_nvme_tcp_qp(struct cstor_qp *qp)
{
	struct cstor_device *cdev = qp->uctx->cdev;
	struct sk_buff *skb;
	struct fw_ri_wr *wqe;
	u32 len16 = DIV_ROUND_UP(sizeof(*wqe), 16);
	int ret;

	skb = alloc_skb(sizeof(*wqe), GFP_KERNEL | __GFP_NOFAIL);
	if (!skb) {
		cstor_err(cdev, "failed to allocate socket buffer\n");
		return -ENOMEM;
	}

	qp->csk->wr_cred -= len16;

	set_wr_txq(skb, CPL_PRIORITY_DATA, qp->csk->txq_idx);

	wqe = (struct fw_ri_wr *)__skb_put_zero(skb, sizeof(*wqe));
	wqe->op_compl = cpu_to_be32(FW_WR_OP_V(FW_RI_INIT_WR) | FW_WR_COMPL_F |
				    FW_RI_WR_TRANSPORT_TYPE_V(FW_QP_TRANSPORT_TYPE_NVMET));
	wqe->flowid_len16 = cpu_to_be32(FW_WR_FLOWID_V(qp->csk->tid) | FW_WR_LEN16_V(len16));
	wqe->cookie = (uintptr_t)&qp->csk->wr_wait;

	wqe->u.nvmet_init.type = FW_RI_TYPE_INIT;
	wqe->u.nvmet_init.qp_caps = FW_RI_QP_FAST_REGISTER_ENABLE;
	wqe->u.nvmet_init.pdid = cpu_to_be32(qp->attr.pdid);
	wqe->u.nvmet_init.qpid = cpu_to_be32(qp->wq.sq.qid);
	wqe->u.nvmet_init.sq_eqid = cpu_to_be32(qp->wq.sq.qid);
	wqe->u.nvmet_init.ulpsubmode = FW_NVMET_ULPSUBMODE_PER_PDU_CMP |
				       FW_NVMET_ULPSUBMODE_USER_MODE;
	if (qp->attr.initiator)
		wqe->u.nvmet_init.ulpsubmode |= FW_NVMET_ULPSUBMODE_ING_DIR;

	if (qp->srq) {
		wqe->u.nvmet_init.rq_eqid = cpu_to_be32(FW_RI_INIT_RQEQID_SRQ | qp->srq->idx);
		wqe->u.nvmet_init.ulpsubmode |= FW_NVMET_ULPSUBMODE_SRQ_ENABLE;
	} else {
		wqe->u.nvmet_init.rq_eqid = cpu_to_be32(qp->wq.rq.qid);
		wqe->u.nvmet_init.hwrqsize = cpu_to_be32(qp->wq.rq.rqt_size);
		wqe->u.nvmet_init.hwrqaddr =
			cpu_to_be32(qp->wq.rq.rqt_hwaddr - cdev->lldi.vr->rq.start);
	}

	if (qp->attr.cmd_pdu_hdr_recv_zcopy)
		wqe->u.nvmet_init.nvmt_pda_cmp_imm_sz = FW_RI_WR_NVMT_PDA_V(qp->attr.rx_pda) |
							FW_RI_WR_CMP_IMM_SZ_V(2);
	else
		wqe->u.nvmet_init.nvmt_pda_cmp_imm_sz = FW_RI_WR_NVMT_PDA_V(qp->attr.rx_pda) |
							FW_RI_WR_CMP_IMM_SZ_V(3);
	wqe->u.nvmet_init.scqid = cpu_to_be32(qp->scq->q.cqid);
	wqe->u.nvmet_init.rcqid = cpu_to_be32(qp->rcq->q.cqid);

	wqe->u.nvmet_init.tpt_offset_t10_config =
			cpu_to_be32(FW_RI_WR_TPT_OFFSET_V(qp->attr.stag_idx));

	if (qp->attr.hdgst)
		wqe->u.nvmet_init.ulpsubmode |= FW_NVMET_ULPSUBMODE_HCRC;
	if (qp->attr.ddgst)
		wqe->u.nvmet_init.ulpsubmode |= FW_NVMET_ULPSUBMODE_DCRC;

	cstor_debug(cdev, "sq_equid %u rq_eqid %u, scqid %u rcqid %u, rqt_size %u, hdgst %u, "
		    "ddgst %u, pdid %u\n", qp->wq.sq.qid, qp->wq.rq.qid, qp->scq->q.cqid,
		    qp->rcq->q.cqid, qp->wq.rq.rqt_size, qp->attr.hdgst, qp->attr.ddgst,
		    qp->attr.pdid);

	ret = cstor_send_wait(cdev, skb, &qp->csk->wr_wait, qp->csk->tid, qp->wq.sq.qid, __func__);
	if (ret)
		cstor_err(cdev, "tid %u sqid %u, cstor_send_wait() failed, ret %d\n",
			  qp->csk->tid, qp->wq.sq.qid, ret);

	cstor_debug(cdev, "tid %u ret %d\n", qp->csk->tid, ret);
	return ret;
}

int cstor_modify_qp(struct cstor_qp *qp, enum cstor_qp_state state)
{
	struct cstor_device *cdev = qp->uctx->cdev;
	struct cstor_sock *csk = qp->csk;
	int ret = 0;

	cstor_debug(cdev, "tid %u sqid %#x rqid %#x state %d -> %d\n",
		    csk->tid, qp->wq.sq.qid, qp->wq.rq.qid, qp->attr.state, state);

	switch (state) {
	case CSTOR_QP_STATE_ACTIVE:
		qp->attr.state = state;

		if (csk->protocol == _CSTOR_ISCSI_PROTOCOL)
			ret = cstor_enable_iscsi_qp(qp);
		else
			ret = cstor_enable_nvme_tcp_qp(qp);

		break;
	case CSTOR_QP_STATE_ERROR:
		t4_set_wq_in_error(&qp->wq);
		qp->attr.state = state;
		break;
	default:
		cstor_err(cdev, "tid %u, qp in a bad state %d\n", csk->tid, qp->attr.state);
		ret = -EINVAL;
		break;
	}

	cstor_debug(cdev, "exit state %d\n", qp->attr.state);
	return ret;
}

int __cstor_destroy_qp(struct cstor_qp *qp)
{
	struct cstor_device *cdev = qp->uctx->cdev;
	struct cstor_sock *csk = qp->csk;

	if (csk) {
		if (csk->state != CSTOR_SOCK_STATE_DEAD) {
			cstor_err(cdev, "tid %u, qp destroy invalid state %d, qpid %#x\n",
				  csk->tid, csk->state, qp->wq.sq.qid);
			return -EINVAL;
		}

		csk->qp = NULL;
	}

	xa_erase(&cdev->qps, qp->wq.sq.qid);

	cstor_debug(cdev, "qpid %#x\n", qp->wq.sq.qid);

	if (qp->attr.protocol == _CSTOR_NVME_TCP_PROTOCOL) {
		cstor_reset_tpte(qp);
		cxgb4_uld_put_resource(&cdev->resource.qp_tag_table, qp->attr.tag_idx);

		if (qp->attr.pbl_size)
			cstor_pblpool_free(cdev, qp->attr.pbl_addr, qp->attr.pbl_size);
	}

	free_rc_queues(qp, !qp->srq && !qp->rxq);

	if (qp->scq) {
		refcount_dec(&qp->scq->refcnt);
		if (qp->scq != qp->rcq)
			refcount_dec(&qp->rcq->refcnt);
	}

	kfree(qp);
	return 0;
}

int cstor_destroy_qp(struct cstor_ucontext *uctx, void __user *ubuf)
{
	struct cstor_qp *qp;
	struct cstor_destroy_qp_cmd cmd;

	if (copy_from_user(&cmd, ubuf, sizeof(cmd))) {
		cstor_err(uctx->cdev, "copy_from_user() failed, cmd size %zu\n", sizeof(cmd));
		return -EFAULT;
	}

	qp = get_qp(uctx->cdev, cmd.qid);
	if (!qp) {
		cstor_err(uctx->cdev, "invalid qid %u\n", cmd.qid);
		return -EINVAL;
	}

	return __cstor_destroy_qp(qp);
}

int cstor_create_qp(struct cstor_ucontext *uctx, void __user *ubuf)
{
	struct cstor_device *cdev = uctx->cdev;
	struct cstor_pd *pd;
	struct cstor_qp *qp;
	struct cstor_cq *scq = NULL;
	struct cstor_cq *rcq = NULL;
	struct cstor_srq *srq = NULL;
	struct cstor_rxq *rxq = NULL;
	struct cstor_mm_entry *sq_key_mm, *rq_key_mm = NULL, *sq_db_key_mm;
	struct cstor_mm_entry *rq_db_key_mm = NULL;
	struct cstor_create_qp_cmd cmd;
	struct cstor_create_qp_resp uresp = {};
	void __user *_uresp;
	bool need_rq;
	u32 sqsize, rqsize = 0;
	int ret;

	if (copy_from_user(&cmd, ubuf, sizeof(cmd))) {
		cstor_err(cdev, "copy_from_user() failed, cmd size %zu\n", sizeof(cmd));
		return -EFAULT;
	}

	if (cmd.max_send_wr > cdev->hw_queue.t4_max_sq_size) {
		cstor_err(cdev, "invalid max_send_wr %u cdev->hw_queue.t4_max_sq_size %u\n",
			  cmd.max_send_wr,  cdev->hw_queue.t4_max_sq_size);
		return -EINVAL;
	}

	if (cmd.max_ddp_tag > max_ddp_tag) {
		cstor_err(cdev, "invalid max_ddp_tag value %u max_ddp_tag %u\n",
			  cmd.max_ddp_tag, max_ddp_tag);
		return -EINVAL;
	}

	if (cmd.max_ddp_sge > max_ddp_sge) {
		cstor_err(cdev, "invalid max_ddp_sge %u max_ddp_sge %u\n",
			  cmd.max_ddp_sge, max_ddp_sge);
		return -EINVAL;
	}

	pd = xa_load(&uctx->pds, cmd.pdid);
	if (!pd) {
		cstor_err(cdev, "failed to find pd with pdid %u\n", cmd.pdid);
		return -EINVAL;
	}

	switch (cmd.protocol) {
	case _CSTOR_NVME_TCP_PROTOCOL:
		scq = get_cq(cdev, cmd.scqid);
		rcq = get_cq(cdev, cmd.rcqid);
		if (!scq || !rcq) {
			cstor_err(cdev, "failed to get scq and rcq\n");
			return -EINVAL;
		}
		break;
	case _CSTOR_ISCSI_PROTOCOL:
		break;
	default:
		cstor_err(cdev, "invalid protocol %u\n", cmd.protocol);
		return -EINVAL;
	}

	if (cmd.srqid != CSTOR_INVALID_SRQID) {
		srq = xa_load(&cdev->srqs, cmd.srqid);
		if (!srq) {
			cstor_err(cdev, "unable to load srq, srqid %u\n", cmd.srqid);
			return -EINVAL;
		}
	}

	if (cmd.rxqid != CSTOR_INVALID_RXQID) {
		rxq = xa_load(&cdev->rxqs, cmd.rxqid);
		if (!rxq) {
			cstor_err(cdev, "unable to load rxq, rxqid %u\n", cmd.rxqid);
			return -EINVAL;
		}
	}

	need_rq = !srq && !rxq;
	if (need_rq) {
		if (cmd.max_recv_wr > cdev->hw_queue.t4_max_rq_size) {
			cstor_err(cdev, "invalid max_recv_wr %u "
				  "cdev->hw_queue.t4_max_rq_size %u\n",
				  cmd.max_recv_wr, cdev->hw_queue.t4_max_rq_size);
			return -EINVAL;
		}

		rqsize = max_t(u32, cmd.max_recv_wr + 1, 8);
	}

	sqsize = max_t(u32, cmd.max_send_wr + 1, 8);

	qp = kzalloc(sizeof(*qp), GFP_KERNEL);
	if (!qp) {
		cstor_err(cdev, "failed to allocate qp\n");
		return -ENOMEM;
	}

	qp->uctx = uctx;

	/* num of descriptors */
	qp->wq.sq.size = sqsize * T4_SQ_NUM_SLOTS;
	qp->wq.sq.memsize = (qp->wq.sq.size + cdev->hw_queue.t4_eq_status_entries) *
			    (T4_EQ_ENTRY_SIZE) + (16 * sizeof(__be64));
	if (need_rq) {
		qp->wq.rq.max_wr = rqsize;
		qp->wq.rq.size = rqsize * T4_RQ_NUM_SLOTS;
		qp->wq.rq.memsize =
			(qp->wq.rq.size + cdev->hw_queue.t4_eq_status_entries) * T4_EQ_ENTRY_SIZE;
	}

	qp->wq.sq.memsize = roundup(qp->wq.sq.memsize, PAGE_SIZE);
	if (need_rq)
		qp->wq.rq.memsize = roundup(qp->wq.rq.memsize, PAGE_SIZE);

	ret = alloc_rc_queues(qp, scq, rcq, need_rq);
	if (ret) {
		cstor_err(cdev, "alloc_rc_queues() failed, ret %d\n", ret);
		if (ret == -ETIMEDOUT)
			return ret;
		goto err1;
	}

	uresp.max_recv_wr = rqsize - 1;
	uresp.max_send_wr = sqsize - 1;

	qp->attr.pdid = pd->pdid;
	qp->attr.protocol = cmd.protocol;
	qp->attr.sq_num_entries = uresp.max_send_wr;
	qp->attr.sq_max_ddp_sge = cmd.max_ddp_sge;
	if (need_rq)
		qp->attr.rq_num_entries = uresp.max_recv_wr;

	qp->attr.state = CSTOR_QP_STATE_RESET;

	ret = -ENOMEM;
	if (cmd.protocol == _CSTOR_NVME_TCP_PROTOCOL) {
		qp->attr.tag_idx = cxgb4_uld_get_resource(&cdev->resource.qp_tag_table);
		if (!qp->attr.tag_idx) {
			cstor_err(cdev, "failed to allocate qp tag table\n");
			goto err2;
		}

		if (cmd.max_ddp_sge > 1) {
			u32 npages = ALIGN(cmd.max_ddp_sge, 4);
			u32 pbl_size = (npages * cmd.max_ddp_tag) << 3;

			qp->attr.pbl_addr = cstor_pblpool_alloc(cdev, pbl_size);
			if (!qp->attr.pbl_addr) {
				cstor_err(cdev, "failed to allocate qp->attr.pbl_addr\n");
				goto err3;
			}

			qp->attr.pbl_size = pbl_size;

			uresp.pbl_offset = PBL_OFF(cdev, qp->attr.pbl_addr);
			uresp.pbl_max_ddp_sge = npages;
		}

		qp->attr.stag_idx = (cdev->lldi.vr->stag.size >> 5) + max_mr +
				    (qp->attr.tag_idx * max_ddp_tag);
		uresp.stag_idx = qp->attr.stag_idx;
	}

	sq_key_mm = kmalloc(sizeof(*sq_key_mm), GFP_KERNEL);
	if (!sq_key_mm) {
		cstor_err(cdev, "failed to allocate sq_key_mm\n");
		goto err4;
	}

	if (need_rq) {
		rq_key_mm = kmalloc(sizeof(*rq_key_mm), GFP_KERNEL);
		if (!rq_key_mm) {
			cstor_err(cdev, "failed to allocate rq_key_mm\n");
			goto err5;
		}
	}

	sq_db_key_mm = kmalloc(sizeof(*sq_db_key_mm), GFP_KERNEL);
	if (!sq_db_key_mm) {
		cstor_err(cdev, "failed to allocate sq_db_key_mm\n");
		goto err6;
	}

	if (need_rq) {
		rq_db_key_mm = kmalloc(sizeof(*rq_db_key_mm), GFP_KERNEL);
		if (!rq_db_key_mm) {
			cstor_err(cdev, "failed to allocate rq_db_key_mm\n");
			goto err7;
		}
	}

	uresp.qid_mask = cdev->rdma_res->qpmask;
	uresp.sqid = qp->wq.sq.qid;
	uresp.sq_size = qp->wq.sq.size;
	uresp.sq_memsize = qp->wq.sq.memsize;
	if (need_rq) {
		uresp.rqid = qp->wq.rq.qid;
		uresp.rq_max_wr = qp->wq.rq.max_wr;
		uresp.rq_size = qp->wq.rq.size;
		uresp.rq_memsize = qp->wq.rq.memsize;
	}

	spin_lock(&uctx->mmap_lock);
	uresp.sq_key = uctx->key;
	uctx->key += PAGE_SIZE;
	if (need_rq) {
		uresp.rq_key = uctx->key;
		uctx->key += PAGE_SIZE;
	}

	uresp.sq_db_gts_key = uctx->key;
	uctx->key += PAGE_SIZE;
	if (need_rq) {
		uresp.rq_db_gts_key = uctx->key;
		uctx->key += PAGE_SIZE;
	}
	spin_unlock(&uctx->mmap_lock);

	_uresp = &((struct cstor_create_qp_cmd *)ubuf)->resp;
	ret = copy_to_user(_uresp, &uresp, sizeof(uresp));
	if (ret) {
		cstor_err(cdev, "copy_to_user() failed, uresp size %zu\n", sizeof(uresp));
		goto err8;
	}

	sq_key_mm->key = uresp.sq_key;
	sq_key_mm->vaddr = qp->wq.sq.queue;
	sq_key_mm->dma_addr = qp->wq.sq.dma_addr;
	sq_key_mm->len = PAGE_ALIGN(qp->wq.sq.memsize);
	insert_mmap(uctx, sq_key_mm);
	if (need_rq) {
		rq_key_mm->key = uresp.rq_key;
		rq_key_mm->vaddr = qp->wq.rq.queue;
		rq_key_mm->dma_addr = qp->wq.rq.dma_addr;
		rq_key_mm->len = PAGE_ALIGN(qp->wq.rq.memsize);
		insert_mmap(uctx, rq_key_mm);
	}

	sq_db_key_mm->key = uresp.sq_db_gts_key;
	sq_db_key_mm->addr = qp->wq.sq.db_gts_pa;
	sq_db_key_mm->vaddr = NULL;
	sq_db_key_mm->len = PAGE_SIZE;
	insert_mmap(uctx, sq_db_key_mm);
	if (need_rq) {
		rq_db_key_mm->key = uresp.rq_db_gts_key;
		rq_db_key_mm->addr = qp->wq.rq.db_gts_pa;
		rq_db_key_mm->vaddr = NULL;
		rq_db_key_mm->len = PAGE_SIZE;
		insert_mmap(uctx, rq_db_key_mm);
	}

	if (need_rq) {
		qp->wq.qp_errp = &((struct t4_status_page *)(qp->wq.rq.queue +
				 (qp->wq.rq.size * T4_EQ_ENTRY_SIZE)))->qp_err;
	} else {
		qp->wq.qp_errp = &((struct t4_status_page *)(qp->wq.sq.queue +
				 (qp->wq.sq.size * T4_EQ_ENTRY_SIZE)))->qp_err;
	}

	if (srq)
		qp->srq = srq;

	if (rxq)
		qp->rxq = rxq;

	if (scq && rcq) {
		refcount_inc(&scq->refcnt);
		if (scq != rcq)
			refcount_inc(&rcq->refcnt);
	}

	qp->scq = scq;
	qp->rcq = rcq;
	cstor_debug(cdev, "sq id %u size %u memsize %lu num_entries %u "
		    "rq id %u size %u memsize %lu num_entries %u\n",
		    qp->wq.sq.qid, qp->wq.sq.size, (unsigned long)qp->wq.sq.memsize,
		    uresp.max_send_wr, qp->wq.rq.qid, qp->wq.rq.max_wr,
		    (unsigned long)qp->wq.rq.memsize, uresp.max_recv_wr);

	return 0;

err8:
	kfree(rq_db_key_mm);
err7:
	kfree(sq_db_key_mm);
err6:
	kfree(rq_key_mm);
err5:
	kfree(sq_key_mm);
err4:
	if (qp->attr.pbl_addr)
		cstor_pblpool_free(cdev, qp->attr.pbl_addr, qp->attr.pbl_size);
err3:
	if (qp->attr.tag_idx)
		cxgb4_uld_put_resource(&cdev->resource.qp_tag_table, qp->attr.tag_idx);
err2:
	free_rc_queues(qp, need_rq);
err1:
	kfree(qp);
	return ret;
}
