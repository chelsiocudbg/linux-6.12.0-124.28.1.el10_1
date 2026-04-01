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

static int __destroy_srq_queue(struct cstor_srq *srq)
{
	struct cstor_device *cdev = srq->uctx->cdev;
	struct sk_buff *skb = cdev->skb;
	struct t4_srq *wq = &srq->wq;
	struct fw_ri_res_wr *res_wr;
	struct fw_ri_res *res;
	u32 wr_len = sizeof(*res_wr) + sizeof(*res);
	int ret;

	skb_trim(skb, 0);
	skb_get(skb);
	set_wr_txq(skb, CPL_PRIORITY_CONTROL, cdev->lldi.ctrlq_start);

	res_wr = (struct fw_ri_res_wr *)__skb_put_zero(skb, wr_len);
	res_wr->op_nres = cpu_to_be32(FW_WR_OP_V(FW_RI_RES_WR) | FW_RI_RES_WR_NRES_V(1) |
				      FW_WR_COMPL_F);
	res_wr->len16_pkd = cpu_to_be32(DIV_ROUND_UP(wr_len, 16));
	res_wr->cookie = (uintptr_t)&cdev->wr_wait;

	res = res_wr->res;
	res->u.srq.restype = FW_RI_RES_TYPE_SRQ;
	res->u.srq.op = FW_RI_RES_OP_RESET;
	res->u.srq.srqid = cpu_to_be32(srq->idx);
	res->u.srq.eqid = cpu_to_be32(wq->qid);

	cstor_reinit_wr_wait(&cdev->wr_wait);
	ret = cstor_send_wait(cdev, skb, &cdev->wr_wait, 0, 0, __func__);
	if (ret) {
		cstor_err(cdev, "cstor_send_wait() failed, ret %d\n", ret);
		return ret;
	}

	dma_free_coherent(&cdev->lldi.pdev->dev, wq->memsize, wq->queue, wq->dma_addr);
	cxgb4_uld_free_rqtpool(cdev->rdma_res, wq->rqt_hwaddr, wq->rqt_size);
	xa_erase(&cdev->srqs, srq->wq.qid);
	cxgb4_uld_put_qpid(&srq->uctx->d_uctx, wq->qid);
	return 0;
}

static int alloc_srq_queue(struct cstor_srq *srq)
{
	struct cstor_device *cdev = srq->uctx->cdev;
	struct t4_srq *wq = &srq->wq;
	struct sk_buff *skb = cdev->skb;
	struct fw_ri_res_wr *res_wr;
	struct fw_ri_res *res;
	u32 wr_len = sizeof(*res_wr) + sizeof(*res);
	int eqsize;
	int ret = -ENOMEM;

	wq->qid = cxgb4_uld_get_qpid(cdev->rdma_res, &srq->uctx->d_uctx);
	if (!wq->qid) {
		cstor_err(cdev, "unable to allocate qid\n");
		return ret;
	}

	wq->rqt_size = wq->max_wr;
	wq->rqt_hwaddr = cxgb4_uld_alloc_rqtpool(cdev->rdma_res, wq->rqt_size);
	if (!wq->rqt_hwaddr) {
		cstor_err(cdev, "failed to allocate memory from rqt pool, wq->rqt_size %d\n",
			  wq->rqt_size);
		goto err1;
	}

	wq->rqt_abs_idx = (wq->rqt_hwaddr - cdev->lldi.vr->rq.start) >> T4_RQT_ENTRY_SHIFT;

	wq->queue = dma_alloc_coherent(&cdev->lldi.pdev->dev, wq->memsize, &wq->dma_addr,
				       GFP_KERNEL);
	if (!wq->queue) {
		cstor_err(cdev, "failed to allocate wq->queue\n");
		goto err1;
	}

	ret = cstor_get_db_gts_phys_addr(cdev, wq->qid, T4_BAR2_QTYPE_EGRESS, &wq->bar2_qid,
					 &wq->db_gts_pa);
	if (ret) {
		cstor_err(cdev, "failed to get bar2 addr for srqid %u, ret %d\n", wq->qid, ret);
		goto err1;
	}

	ret = xa_insert(&cdev->srqs, srq->wq.qid, srq, GFP_KERNEL);
	if (ret) {
		cstor_err(cdev, "xa_insert() failed, srqid %u ret %d\n", srq->wq.qid, ret);
		goto err1;
	}

	/* build fw_ri_res_wr */
	skb_trim(skb, 0);
	skb_get(skb);
	set_wr_txq(skb, CPL_PRIORITY_CONTROL, cdev->lldi.ctrlq_start);

	res_wr = (struct fw_ri_res_wr *)__skb_put_zero(skb, wr_len);
	res_wr->op_nres = cpu_to_be32(FW_WR_OP_V(FW_RI_RES_WR) |
				      FW_RI_RES_WR_NRES_V(1) | FW_WR_COMPL_F);
	res_wr->len16_pkd = cpu_to_be32(DIV_ROUND_UP(wr_len, 16));
	res_wr->cookie = (uintptr_t)&cdev->wr_wait;

	/*
	 * eqsize is the number of 64B entries plus the status page size.
	 */
	eqsize = wq->size + cdev->hw_queue.t4_eq_status_entries;

	res = res_wr->res;
	res->u.srq.restype = FW_RI_RES_TYPE_SRQ;
	res->u.srq.op = FW_RI_RES_OP_WRITE;
	res->u.srq.eqid = cpu_to_be32(wq->qid);
	res->u.srq.fetchszm_to_iqid = cpu_to_be32(FW_RI_RES_WR_HOSTFCMODE_V(0) | /* no host cidx updates */
					FW_RI_RES_WR_CPRIO_V(0) | /* don't keep in chip cache */
					FW_RI_RES_WR_PCIECHN_V(0) | /* set by uP at ri_init time */
					FW_RI_RES_WR_FETCHRO_V(cdev->lldi.relaxed_ordering));
	res->u.srq.dcaen_to_eqsize = cpu_to_be32(FW_RI_RES_WR_DCAEN_V(0) |
						 FW_RI_RES_WR_DCACPU_V(0) |
						 FW_RI_RES_WR_FBMIN_V(2) |
						 FW_RI_RES_WR_FBMAX_V(3) |
						 FW_RI_RES_WR_CIDXFTHRESHO_V(0) |
						 FW_RI_RES_WR_CIDXFTHRESH_V(0) |
						 FW_RI_RES_WR_EQSIZE_V(eqsize));
	res->u.srq.eqaddr = cpu_to_be64(wq->dma_addr);
	res->u.srq.srqid = cpu_to_be32(srq->idx);
	res->u.srq.pdid = cpu_to_be32(srq->pdid);
	res->u.srq.hwsrqsize = cpu_to_be32(wq->rqt_size);
	res->u.srq.hwsrqaddr = cpu_to_be32(wq->rqt_hwaddr - cdev->lldi.vr->rq.start);

	cstor_reinit_wr_wait(&cdev->wr_wait);

	ret = cstor_send_wait(cdev, skb, &cdev->wr_wait, 0, wq->qid, __func__);
	if (ret) {
		cstor_err(cdev, "cstor_send_wait() failed, qid %u ret %d\n", wq->qid, ret);
		if (ret == -ETIMEDOUT)
			return ret;
		goto err2;
	}

	cstor_debug(cdev, "srq %u eqid %u pdid %u queue va %p pa %#llx rqt addr %#x size %d\n",
		    srq->idx, wq->qid, srq->pdid, wq->queue, (u64)virt_to_phys(wq->queue),
		    wq->rqt_hwaddr, wq->rqt_size);

	return 0;
err2:
	xa_erase(&cdev->srqs, srq->wq.qid);
err1:
	if (wq->queue)
		dma_free_coherent(&cdev->lldi.pdev->dev, wq->memsize, wq->queue, wq->dma_addr);

	if (wq->rqt_hwaddr)
		cxgb4_uld_free_rqtpool(cdev->rdma_res, wq->rqt_hwaddr, wq->rqt_size);

	cxgb4_uld_put_qpid(&srq->uctx->d_uctx, wq->qid);
	return ret;
}

int __cstor_destroy_srq(struct cstor_srq *srq)
{
	struct cstor_device *cdev = srq->uctx->cdev;
	int ret;

	if (refcount_read(&srq->refcnt) > 1) {
		cstor_err(cdev, "srq with id %u is in use\n", srq->wq.qid);
		return -EINVAL;
	}

	cstor_debug(cdev, "id %u\n", srq->wq.qid);

	ret = __destroy_srq_queue(srq);
	if (ret) {
		cstor_err(cdev, "error destroying srq with id %u\n", srq->wq.qid);
		return ret;
	}

	cxgb4_uld_free_srq_idx(cdev->rdma_res, srq->idx);
	kfree(srq);
	return 0;
}

int cstor_destroy_srq(struct cstor_ucontext *uctx, void __user *ubuf)
{
	struct cstor_device *cdev = uctx->cdev;
	struct cstor_srq *srq;
	struct cstor_destroy_srq_cmd cmd;

	if (copy_from_user(&cmd, ubuf, sizeof(cmd))) {
		cstor_err(uctx->cdev, "copy_from_user() failed, cmd size %zu\n", sizeof(cmd));
		return -EFAULT;
	}

	srq = xa_load(&cdev->srqs, cmd.srqid);
	if (!srq) {
		cstor_err(cdev, "unable to load srq, srqid %u\n", cmd.srqid);
		return -EINVAL;
	}

	return __cstor_destroy_srq(srq);
}

int cstor_create_srq(struct cstor_ucontext *uctx, void __user *ubuf)
{
	struct cstor_device *cdev = uctx->cdev;
	struct cstor_pd *pd;
	struct cstor_srq *srq;
	struct cstor_mm_entry *srq_key_mm = NULL, *srq_db_key_mm = NULL;
	struct cstor_create_srq_cmd cmd;
	struct cstor_create_srq_resp uresp = {};
	void __user *_uresp;
	int rqsize;
	int ret = -ENOMEM;

	if (copy_from_user(&cmd, ubuf, sizeof(cmd))) {
		cstor_err(uctx->cdev, "copy_from_user() failed, cmd size %zu\n", sizeof(cmd));
		return -EFAULT;
	}

	if (!cdev->lldi.vr->srq.size) {
		cstor_err(cdev, "invalid srq size is 0\n");
		return -EINVAL;
	}

	if (cmd.max_wr > cdev->hw_queue.t4_max_rq_size) {
		cstor_err(cdev, "invalid cmd.max_wr value %u cdev->hw_queue.t4_max_rq_size %u\n",
			  cmd.max_wr, cdev->hw_queue.t4_max_rq_size);
		return -E2BIG;
	}

	pd = xa_load(&uctx->pds, cmd.pdid);
	if (!pd) {
		cstor_err(cdev, "failed to find pd with pdid %u\n", cmd.pdid);
		return -EINVAL;
	}

	cstor_debug(cdev, "pdid %u\n", pd->pdid);

	/*
	 * SRQ RQT and RQ must be a power of 2 and at least 16 deep.
	 */
	rqsize = roundup_pow_of_two(max_t(u16, cmd.max_wr + 1, 16));

	srq = kzalloc(sizeof(*srq), GFP_KERNEL);
	if (!srq) {
		cstor_err(cdev, "srq allocation failed\n");
		return ret;
	}

	srq->idx = -1;

	srq_key_mm = kmalloc(sizeof(*srq_key_mm), GFP_KERNEL);
	if (!srq_key_mm) {
		cstor_err(cdev, "failed to allocate srq_key_mm\n");
		goto err;
	}

	srq_db_key_mm = kmalloc(sizeof(*srq_db_key_mm), GFP_KERNEL);
	if (!srq_db_key_mm) {
		cstor_err(cdev, "failed to allocate srq_db_key_mm\n");
		goto err;
	}

	srq->idx = cxgb4_uld_alloc_srq_idx(cdev->rdma_res);
	if (srq->idx < 0) {
		cstor_err(cdev, "failed to allocate srq idx\n");
		goto err;
	}

	srq->uctx = uctx;
	srq->pdid = pd->pdid;
	srq->wq.max_wr = rqsize;
	srq->wq.size = rqsize * T4_RQ_NUM_SLOTS;
	srq->wq.memsize = (srq->wq.size + cdev->hw_queue.t4_eq_status_entries) * T4_EQ_ENTRY_SIZE;
	srq->wq.memsize = roundup(srq->wq.memsize, PAGE_SIZE);

	ret = alloc_srq_queue(srq);
	if (ret) {
		cstor_err(cdev, "alloc_srq_queue() failed, ret %d\n", ret);
		if (ret == -ETIMEDOUT) {
			kfree(srq_db_key_mm);
			kfree(srq_key_mm);
			return ret;
		}

		goto err;
	}

	uresp.max_wr = rqsize - 1;

	uresp.flags = srq->flags;
	uresp.qid_mask = cdev->rdma_res->qpmask;
	uresp.srqid = srq->wq.qid;
	uresp.srq_size = srq->wq.size;
	uresp.srq_max_wr = srq->wq.max_wr;
	uresp.srq_memsize = srq->wq.memsize;
	uresp.rqt_abs_idx = srq->wq.rqt_abs_idx;

	spin_lock(&uctx->mmap_lock);
	uresp.srq_key = uctx->key;
	uctx->key += PAGE_SIZE;
	uresp.srq_db_gts_key = uctx->key;
	uctx->key += PAGE_SIZE;
	spin_unlock(&uctx->mmap_lock);

	_uresp = &((struct cstor_create_srq_cmd *)ubuf)->resp;
	ret = copy_to_user(_uresp, &uresp, sizeof(uresp));
	if (ret) {
		cstor_err(cdev, "copy_to_user() failed, uresp size %zu\n", sizeof(uresp));
		ret = __destroy_srq_queue(srq);
		if (ret) {
			cstor_err(cdev, "__destroy_srq_queue() failed, ret %d\n", ret);
			kfree(srq_db_key_mm);
			kfree(srq_key_mm);
			return -EFAULT;
		}

		ret = -EFAULT;
		goto err;
	}

	srq_key_mm->key = uresp.srq_key;
	srq_key_mm->vaddr = srq->wq.queue;
	srq_key_mm->dma_addr = srq->wq.dma_addr;
	srq_key_mm->len = PAGE_ALIGN(srq->wq.memsize);
	insert_mmap(uctx, srq_key_mm);

	srq_db_key_mm->key = uresp.srq_db_gts_key;
	srq_db_key_mm->addr = srq->wq.db_gts_pa;
	srq_db_key_mm->vaddr = NULL;
	srq_db_key_mm->len = PAGE_SIZE;
	insert_mmap(uctx, srq_db_key_mm);

	cstor_debug(cdev, "srq qid %u idx %u size %u memsize %lu num_entries %u\n", srq->wq.qid,
		    srq->idx, srq->wq.max_wr, (unsigned long)srq->wq.memsize, uresp.max_wr);

	refcount_set(&srq->refcnt, 1);
	return 0;

err:
	kfree(srq_db_key_mm);
	kfree(srq_key_mm);

	if (srq->idx >= 0)
		cxgb4_uld_free_srq_idx(cdev->rdma_res, srq->idx);

	kfree(srq);
	return ret;
}
