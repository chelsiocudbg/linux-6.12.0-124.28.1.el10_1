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

#include "cstor.h"

static int destroy_cq(struct cstor_cq *cq)
{
	struct cstor_device *cdev = cq->uctx->cdev;
	struct sk_buff *skb = cdev->skb;
	struct fw_ri_res_wr *res_wr;
	struct fw_ri_res *res;
	u32 wr_len = sizeof(*res_wr) + sizeof(*res);
	int ret;

	skb_trim(skb, 0);
	skb_get(skb);
	set_wr_txq(skb, CPL_PRIORITY_CONTROL, cdev->lldi.ctrlq_start);

	res_wr = (struct fw_ri_res_wr *)__skb_put_zero(skb, wr_len);
	res_wr->op_nres = cpu_to_be32(FW_WR_OP_V(FW_RI_RES_WR) |
				      FW_RI_RES_WR_NRES_V(1) | FW_WR_COMPL_F);
	res_wr->len16_pkd = cpu_to_be32(DIV_ROUND_UP(wr_len, 16));
	res_wr->cookie = (uintptr_t)&cdev->wr_wait;

	res = res_wr->res;
	res->u.cq.restype = FW_RI_RES_TYPE_CQ;
	res->u.cq.op = FW_RI_RES_OP_RESET;
	res->u.cq.iqid = cpu_to_be32(cq->q.cqid);

	cstor_reinit_wr_wait(&cdev->wr_wait);
	ret = cstor_send_wait(cdev, skb, &cdev->wr_wait, 0, 0, __func__);
	if (ret)
		cstor_err(cdev, "cstor_send_wait() failed, ret %d\n", ret);

	return ret;
}

static int create_cq(struct cstor_cq *cq, u16 cqe_size)
{
	struct cstor_device *cdev = cq->uctx->cdev;
	struct sk_buff *skb = cdev->skb;
	struct fw_ri_res_wr *res_wr;
	struct fw_ri_res *res;
	u32 wr_len = sizeof(*res_wr) + sizeof(*res);
	int ret;

	cq->q.cqid = cxgb4_uld_get_cqid(cdev->rdma_res, &cq->uctx->d_uctx);
	if (!cq->q.cqid) {
		cstor_err(cdev, "unable to get cqid\n");
		ret = -ENOMEM;
		goto err1;
	}

	cq->q.queue = dma_alloc_coherent(&cdev->lldi.pdev->dev, cq->q.memsize, &cq->q.dma_addr,
					 GFP_KERNEL);
	if (!cq->q.queue) {
		cstor_err(cdev, "failed to allocate cq queue\n");
		ret = -ENOMEM;
		goto err2;
	}

	ret = xa_insert_bh(&cdev->cqs, cq->q.cqid, cq, GFP_KERNEL);
	if (ret) {
		cstor_err(cdev, "xa_insert_bh() failed, cqid %u ret %d\n", cq->q.cqid, ret);
		goto err3;
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

	res = res_wr->res;
	res->u.cq.restype = FW_RI_RES_TYPE_CQ;
	res->u.cq.op = FW_RI_RES_OP_WRITE;
	res->u.cq.iqid = cpu_to_be32(cq->q.cqid);
	res->u.cq.iqandst_to_iqandstindex = cpu_to_be32(FW_RI_RES_WR_IQANUS_V(0) |
				FW_RI_RES_WR_IQANUD_V(UPDATEDELIVERY_INTERRUPT_X) |
				FW_RI_RES_WR_IQANDST_F |
				FW_RI_RES_WR_IQANDSTINDEX_V(cdev->lldi.ciq_ids[cq->q.ciq_idx]));
	res->u.cq.iqdroprss_to_iqesize = cpu_to_be16(FW_RI_RES_WR_IQPCIECH_V(2) |
					FW_RI_RES_WR_IQINTCNTTHRESH_V(0) |
					FW_RI_RES_WR_IQESIZE_V(ilog2(cqe_size) - 4));
	res->u.cq.iqsize = cpu_to_be16(cq->q.size);
	res->u.cq.iqaddr = cpu_to_be64(cq->q.dma_addr);

	cstor_reinit_wr_wait(&cdev->wr_wait);
	ret = cstor_send_wait(cdev, skb, &cdev->wr_wait, 0, 0, __func__);
	if (ret) {
		cstor_err(cdev, "cstor_send_wait() failed, ret %d\n", ret);
		if (ret == -ETIMEDOUT)
			return ret;
		goto err4;
	}

	cq->q.gts = cdev->lldi.gts_reg;

	ret = cstor_get_db_gts_phys_addr(cdev, cq->q.cqid, T4_BAR2_QTYPE_INGRESS, &cq->q.bar2_qid,
					 &cq->q.db_gts_pa);
	if (ret) {
		cstor_err(cdev, "failed to get bar2 addr for cqid %u, ret %d\n", cq->q.cqid, ret);
		goto err3;
	}

	return 0;
err4:
	xa_erase_bh(&cdev->cqs, cq->q.cqid);
err3:
	dma_free_coherent(&cdev->lldi.pdev->dev, cq->q.memsize, cq->q.queue, cq->q.dma_addr);
err2:
	cxgb4_uld_put_cqid(&cq->uctx->d_uctx, cq->q.cqid);
err1:
	return ret;
}

int __cstor_destroy_cq(struct cstor_cq *cq, bool reset_cq_ctx)
{
	struct cstor_device *cdev = cq->uctx->cdev;

	if (refcount_read(&cq->refcnt) != 1) {
		cstor_err(cq->uctx->cdev, "cqid %u still in use\n", cq->q.cqid);
		return -EINVAL;
	}

	if (reset_cq_ctx) {
		int ret;

		ret = destroy_cq(cq);
		if (ret) {
			cstor_err(cdev, "error destroying cqid %u, ret %d\n", cq->q.cqid, ret);
			return ret;
		}
	}

	xa_erase_bh(&cdev->cqs, cq->q.cqid);
	dma_free_coherent(&cdev->lldi.pdev->dev, cq->q.memsize, cq->q.queue, cq->q.dma_addr);
	cxgb4_uld_put_cqid(&cq->uctx->d_uctx, cq->q.cqid);

	if (cq->eventfd_ctx)
		eventfd_ctx_put(cq->eventfd_ctx);

	kfree(cq);
	return 0;
}

int cstor_disable_cq(struct cstor_cq *cq)
{
	struct cstor_device *cdev = cq->uctx->cdev;
	struct fw_iq_cmd c = {};
	int ret;

	cstor_debug(cdev, "cqid %u\n", cq->q.cqid);

	c.op_to_vfn = cpu_to_be32(FW_CMD_OP_V(FW_IQ_CMD) | FW_CMD_REQUEST_F | FW_CMD_EXEC_F |
				  FW_IQ_CMD_PFN_V(cdev->lldi.pf) | FW_IQ_CMD_VFN_V(0));
	c.alloc_to_len16 = cpu_to_be32(FW_IQ_CMD_FREE_F | FW_LEN16(c));
	c.type_to_iqandstindex = cpu_to_be32(FW_IQ_CMD_TYPE_V(FW_IQ_TYPE_CQ));
	c.iqid = cpu_to_be16(cq->q.cqid);

	rtnl_lock();
	ret = cxgb4_wr_mbox(cdev->lldi.ports[0], &c, sizeof(c), NULL);
	rtnl_unlock();
	if (ret)
		cstor_err(cdev, "cxgb4_wr_mbox() failed, ret %d\n", ret);

	return ret;
}

int cstor_destroy_cq(struct cstor_ucontext *uctx, void __user *ubuf)
{
	struct cstor_cq *cq;
	struct cstor_destroy_cq_cmd cmd;

	if (copy_from_user(&cmd, ubuf, sizeof(cmd))) {
		cstor_err(uctx->cdev, "copy_from_user() failed, cmd size %zu\n", sizeof(cmd));
		return -EFAULT;
	}

	cq = get_cq(uctx->cdev, cmd.cqid);
	if (!cq) {
		cstor_err(uctx->cdev, "unable to get cq, cmd.cqid %u\n", cmd.cqid);
		return -EINVAL;
	}

	return __cstor_destroy_cq(cq, true);
}

int cstor_create_cq(struct cstor_ucontext *uctx, void __user *ubuf)
{
	struct cstor_device *cdev = uctx->cdev;
	struct cstor_cq *cq;
	struct cstor_create_cq_cmd cmd;
	struct cstor_create_cq_resp uresp = {};
	struct cstor_mm_entry *mm = NULL, *mm2 = NULL;
	void __user *_uresp;
	size_t memsize, hwentries;
	int entries;
	int ret = -ENOMEM;

	if (copy_from_user(&cmd, ubuf, sizeof(cmd))) {
		cstor_err(cdev, "copy_from_user() failed, cmd size %zu\n", sizeof(cmd));
		return -EFAULT;
	}

	entries = cmd.num_cqe;
	cstor_debug(cdev, "entries %d\n", entries);

	cq = kzalloc(sizeof(*cq), GFP_KERNEL);
	if (!cq) {
		cstor_err(cdev, "failed to allocate cq\n");
		return ret;
	}

	mm = kmalloc(sizeof(*mm), GFP_KERNEL);
	if (!mm) {
		cstor_err(cdev, "failed to allocate mm\n");
		goto err1;
	}

	mm2 = kmalloc(sizeof(*mm2), GFP_KERNEL);
	if (!mm2) {
		cstor_err(cdev, "failed to allocate mm2\n");
		goto err1;
	}

	if (cmd.event_fd != INVALID_EVENT_FD) {
		cstor_debug(cdev, "alloc event fd context, cmd.event_fd %u\n", cmd.event_fd);
		cq->eventfd_ctx = eventfd_ctx_fdget(cmd.event_fd);
		if (IS_ERR(cq->eventfd_ctx)) {
			ret = PTR_ERR(cq->eventfd_ctx);
			goto err1;
		}
	}

	/* account for the status page. */
	entries++;

	/* IQ needs one extra entry to differentiate full vs empty. */
	entries++;

	/*
	 * entries must be multiple of 16 for HW.
	 */
	entries = roundup(entries, 16);

	hwentries = min(entries, cdev->hw_queue.t4_max_iq_size);

	/*
	 * Make HW queue at least 64 entries so GTS updates aren't too
	 * frequent.
	 */
	if (hwentries < 64)
		hwentries = 64;

	memsize = hwentries * cmd.cqe_size;

	/*
	 * memsize must be a multiple of the page size if its a user cq.
	 */
	memsize = roundup(memsize, PAGE_SIZE);

	cq->uctx = uctx;
	refcount_set(&cq->refcnt, 1);
	cq->q.size = hwentries;
	cq->q.memsize = memsize;
	cq->q.ciq_idx = cdev->ciq_idx;

	ret = create_cq(cq, cmd.cqe_size);
	if (ret) {
		cstor_err(cdev, "create_cq failed, ret %d\n", ret);
		if (ret == -ETIMEDOUT) {
			if (cq->eventfd_ctx) {
				eventfd_ctx_put(cq->eventfd_ctx);
				cq->eventfd_ctx = NULL;
			}

			kfree(mm2);
			kfree(mm);
			return ret;
		}

		goto err2;
	}

	cq->q.size--; /* status page */
	cq->q.status = (struct t4_status_page *)(cq->q.queue + (cq->q.size * cmd.cqe_size));

	uresp.qid_mask = cdev->rdma_res->cqmask;
	uresp.cqid = cq->q.cqid;
	uresp.size = cq->q.size;
	uresp.memsize = cq->q.memsize;

	spin_lock(&uctx->mmap_lock);
	uresp.key = uctx->key;
	uctx->key += PAGE_SIZE;
	uresp.gts_key = uctx->key;
	uctx->key += PAGE_SIZE;
	spin_unlock(&uctx->mmap_lock);

	_uresp = &((struct cstor_create_cq_cmd *)ubuf)->resp;
	ret = copy_to_user(_uresp, &uresp, sizeof(uresp));
	if (ret) {
		cstor_err(cdev, "copy_to_user() failed, uresp size %zu, ret %d\n",
			  sizeof(uresp), ret);
		if (__cstor_destroy_cq(cq, true)) {
			cstor_err(cdev, "__cstor_destroy_cq() failed\n");
			if (cq->eventfd_ctx) {
				eventfd_ctx_put(cq->eventfd_ctx);
				cq->eventfd_ctx = NULL;
			}
		}

		kfree(mm2);
		kfree(mm);
		return ret;
	}

	mm->key = uresp.key;
	mm->vaddr = cq->q.queue;
	mm->dma_addr = cq->q.dma_addr;
	mm->len = cq->q.memsize;
	insert_mmap(uctx, mm);

	mm2->key = uresp.gts_key;
	mm2->addr = cq->q.db_gts_pa;
	mm2->vaddr = NULL;
	mm2->len = PAGE_SIZE;
	insert_mmap(uctx, mm2);

	cdev->ciq_idx = (cdev->ciq_idx + 1) % cdev->lldi.nciq;
	cstor_debug(cdev, "cqid %#x size %u memsize %zu, dma_addr %pad\n",
		    cq->q.cqid, cq->q.size, cq->q.memsize, &cq->q.dma_addr);
	return 0;
err2:
	if (cq->eventfd_ctx)
		eventfd_ctx_put(cq->eventfd_ctx);
err1:
	kfree(mm2);
	kfree(mm);
	kfree(cq);
	return ret;
}
