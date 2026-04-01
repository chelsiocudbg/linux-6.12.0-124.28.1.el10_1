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
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/atomic.h>

#include "umem.h"

#define CSTOR_INLINE_THRESHOLD 256
#define CSTOR_PBL_ALIGNMENT 4

static void
cstor_fill_ulp_mem_io_req(struct ulp_mem_io *req, struct cstor_wr_wait *wr_waitp,
			  u32 addr, __be32 cmd, u32 data_len, u32 wr_len)
{
	req->wr.wr_hi = cpu_to_be32(FW_WR_OP_V(FW_ULPTX_WR) | FW_WR_COMPL_F);
	req->wr.wr_mid = cpu_to_be32(FW_WR_LEN16_V(DIV_ROUND_UP(wr_len, 16)));
	req->wr.wr_lo = (__force __be64)(uintptr_t)wr_waitp;

	req->cmd = cmd;
	req->dlen = cpu_to_be32(T7_ULP_MEMIO_DATA_LEN_V(data_len >> 5));
	req->len16 = cpu_to_be32(DIV_ROUND_UP(wr_len - sizeof(req->wr), 16));
	req->lock_addr = cpu_to_be32(ULP_MEMIO_ADDR_V(addr));
}

static int cstor_write_pbl_imm(struct cstor_mr *mr, u64 page_size, u32 len, void *usr_pbl)
{
	struct cstor_device *cdev = mr->uctx->cdev;
	struct sk_buff *skb = cdev->skb;
	struct ulp_mem_io *req;
	struct ulptx_idata *sc;
	struct ib_block_iter biter;
	__be64 *pbl;
	__u64 *raw_pbl;
	__be32 cmd = cpu_to_be32(ULPTX_CMD_V(ULP_TX_MEM_WRITE) | T5_ULP_MEMIO_IMM_V(1));
	u32 wr_len = sizeof(*req) + (2 * sizeof(*sc)) + len;
	u32 addr = (mr->attr.pbl_addr >> 5) & 0x7FFFFFF;
	int i = 0, ret;

	raw_pbl = kzalloc(len, GFP_KERNEL);
	if (!raw_pbl) {
		cstor_err(cdev, "failed to allocate raw_pbl\n");
		return -ENOMEM;
	}

	skb_trim(skb, 0);
	set_wr_txq(skb, CPL_PRIORITY_CONTROL, cdev->lldi.ctrlq_start);
	cstor_reinit_wr_wait(&cdev->wr_wait);

	req = (struct ulp_mem_io *)__skb_put(skb, wr_len);
	cstor_fill_ulp_mem_io_req(req, &cdev->wr_wait, addr, cmd, len, wr_len);

	sc = (struct ulptx_idata *)(req + 1);
	sc->cmd_more = cpu_to_be32(ULPTX_CMD_V(ULP_TX_SC_IMM));
	sc->len = cpu_to_be32(len);
	pbl = (__be64 *)(sc + 1);

	rdma_umem_for_each_dma_block(mr->umem, &biter, page_size) {
		dma_addr_t dma_addr = rdma_block_iter_dma_address(&biter);

		raw_pbl[i] = dma_addr;
		pbl[i++] = cpu_to_be64(dma_addr);
	}

	ret = copy_to_user(usr_pbl, raw_pbl, i << 3);
	if (ret) {
		cstor_err(cdev, "copy_to_user() failed, i << 3 %d\n", i << 3);
		goto err_free_raw_pbl;
	}

	while (i % CSTOR_PBL_ALIGNMENT)
		pbl[i++] = cpu_to_be64(0);

	sc = (void *)(pbl + i);
	sc->cmd_more = cpu_to_be32(ULPTX_CMD_V(ULP_TX_SC_NOOP));
	sc->len = cpu_to_be32(0);

	skb_get(skb);
	ret = cstor_send_wait(cdev, skb, &cdev->wr_wait, 0, 0, __func__);

err_free_raw_pbl:
	kfree(raw_pbl);
	return ret;
}

static int cstor_write_mem_dsgl(struct cstor_device *cdev, u32 addr, dma_addr_t daddr, u32 len)
{
	struct sk_buff *skb = cdev->skb;
	struct ulp_mem_io *req;
	struct ulptx_sgl *sgl;
	__be32 cmd = cpu_to_be32(ULPTX_CMD_V(ULP_TX_MEM_WRITE) | T5_ULP_MEMIO_ORDER_V(1) |
				 T5_ULP_MEMIO_FID_V(cdev->lldi.rxq_ids[0]));
	u32 wr_len = sizeof(*req) + sizeof(*sgl);

	skb_trim(skb, 0);
	skb_get(skb);
	set_wr_txq(skb, CPL_PRIORITY_CONTROL, cdev->lldi.ctrlq_start);
	cstor_reinit_wr_wait(&cdev->wr_wait);

	req = (struct ulp_mem_io *)__skb_put(skb, wr_len);
	cstor_fill_ulp_mem_io_req(req, &cdev->wr_wait, addr, cmd, len, wr_len);

	sgl = (struct ulptx_sgl *)(req + 1);
	sgl->cmd_nsge = cpu_to_be32(ULPTX_CMD_V(ULP_TX_SC_DSGL) | ULPTX_NSGE_V(1));
	sgl->len0 = cpu_to_be32(len);
	sgl->addr0 = cpu_to_be64(daddr);

	return cstor_send_wait(cdev, skb, &cdev->wr_wait, 0, 0, __func__);
}

static int
cstor_write_pbl_dsgl(struct cstor_mr *mr, u64 page_size, u32 len, __u64 __user *usr_pbl)
{
	struct cstor_device *cdev = mr->uctx->cdev;
	struct ib_block_iter biter;
	__u64 *raw_pbl;
	__be64 *pages = NULL;
	dma_addr_t daddr;
	u32 max_dma_len;
	int order = get_order(min_t(u32, len, cdev->max_dma_len));
	int i = 0, n = 0, ret = -ENOMEM;
	gfp_t gfp = GFP_KERNEL | __GFP_COMP;

	while (order && !pages) {
		pages = (__be64 *)__get_free_pages(gfp, order);
		if (!pages)
			order--;
	}

	if (!order) {
		pages = (__be64 *)__get_free_page(GFP_KERNEL);
		if (!pages) {
			cstor_err(cdev, "failed to allocate pages\n");
			return -ENOMEM;
		}

		gfp = GFP_KERNEL;
	}

	raw_pbl = (__u64 *)__get_free_pages(gfp, order);
	if (!raw_pbl) {
		cstor_err(cdev, "failed to allocate raw_pbl\n");
		goto err1;
	}

	max_dma_len = min3(len, (u32)(PAGE_SIZE << order), cdev->max_dma_len);

	daddr = dma_map_single(&cdev->lldi.pdev->dev, pages, max_dma_len, DMA_TO_DEVICE);
	if (dma_mapping_error(&cdev->lldi.pdev->dev, daddr)) {
		cstor_err(cdev, "dma_map_single() failed\n");
		goto err2;
	}

	dma_sync_single_for_cpu(&cdev->lldi.pdev->dev, daddr, max_dma_len, DMA_TO_DEVICE);

	rdma_umem_for_each_dma_block(mr->umem, &biter, page_size) {
		dma_addr_t dma_addr = rdma_block_iter_dma_address(&biter);

		raw_pbl[i] = dma_addr;
		pages[i++] = cpu_to_be64(dma_addr);

		if (i == (max_dma_len / sizeof(*pages))) {
			dma_sync_single_for_device(&cdev->lldi.pdev->dev, daddr,
						   max_dma_len, DMA_TO_DEVICE);

			ret = cstor_write_mem_dsgl(cdev, (mr->attr.pbl_addr + (n << 3)) >> 5,
						   daddr, max_dma_len);
			if (ret) {
				cstor_err(cdev, "failed cstor_write_mem_dsgl(), "
					  "n %d max_dma_len %u mr->attr.pbl_addr %u "
					  "daddr %llu ret %d\n", n, max_dma_len,
					  mr->attr.pbl_addr, daddr, ret);
				goto err3;
			}

			dma_sync_single_for_cpu(&cdev->lldi.pdev->dev, daddr,
						max_dma_len, DMA_TO_DEVICE);

			ret = copy_to_user(usr_pbl + n, raw_pbl, max_dma_len);
			if (ret) {
				cstor_err(cdev, "copy_to_user() failed, max_dma_len %u\n",
					  max_dma_len);
				goto err3;
			}

			n += i;
			i = 0;
		}
	}

	if (i) {
		while (i % CSTOR_PBL_ALIGNMENT)
			pages[i++] = cpu_to_be64(0);

		dma_sync_single_for_device(&cdev->lldi.pdev->dev, daddr, i << 3, DMA_TO_DEVICE);

		ret = cstor_write_mem_dsgl(cdev, (mr->attr.pbl_addr + (n << 3)) >> 5,
					   daddr, i << 3);
		if (ret) {
			cstor_err(cdev, "cstor_write_mem_dsgl() failed, n %d i %d "
				  "mr->attr.pbl_addr %u daddr %llu ret %d\n",
				  n, i, mr->attr.pbl_addr, daddr, ret);
		} else {
			ret = copy_to_user(usr_pbl + n, raw_pbl, i * sizeof(*usr_pbl));
			if (ret)
				cstor_err(cdev, "copy_to_user() failed, i %d "
					  "usr_pbl size %zu\n", i, sizeof(*usr_pbl));
		}
	}

err3:
	dma_unmap_single(&cdev->lldi.pdev->dev, daddr, max_dma_len, DMA_TO_DEVICE);
err2:
	free_pages((unsigned long)raw_pbl, order);
err1:
	free_pages((unsigned long)pages, order);
	return ret;
}

static int cstor_reset_mem_with_imm(struct cstor_device *cdev, u32 addr, u32 len)
{
	struct sk_buff *skb = cdev->skb;
	struct ulp_mem_io *req;
	struct ulptx_idata *sc;
	__be32 cmd = cpu_to_be32(ULPTX_CMD_V(ULP_TX_MEM_WRITE) |
				 T5_ULP_MEMIO_IMM_V(1) | (1U << 21));
	u32 wr_len = sizeof(*req) + (2 * sizeof(*sc));

	skb_trim(skb, 0);
	skb_get(skb);
	set_wr_txq(skb, CPL_PRIORITY_CONTROL, cdev->lldi.ctrlq_start);
	cstor_reinit_wr_wait(&cdev->wr_wait);

	req = (struct ulp_mem_io *)__skb_put(skb, wr_len);
	cstor_fill_ulp_mem_io_req(req, &cdev->wr_wait, addr, cmd, len, wr_len);

	sc = (struct ulptx_idata *)(req + 1);
	sc->cmd_more = cpu_to_be32(ULPTX_CMD_V(ULP_TX_SC_IMM));
	sc->len = cpu_to_be32(len);

	sc = sc + 1;
	sc->cmd_more = cpu_to_be32(ULPTX_CMD_V(ULP_TX_SC_NOOP));
	sc->len = cpu_to_be32(0);

	return cstor_send_wait(cdev, skb, &cdev->wr_wait, 0, 0, __func__);
}

int cstor_reset_tpte(struct cstor_qp *qp)
{
	struct cstor_device *cdev = qp->uctx->cdev;
	u32 len = sizeof(struct fw_ri_tpte) * max_ddp_tag;
	u32 addr = (cdev->lldi.vr->stag.start >> 5) + qp->attr.stag_idx;

	return cstor_reset_mem_with_imm(cdev, addr, len);
}

int cstor_invalidate_iscsi_tag(struct cstor_ucontext *uctx, void __user *ubuf)
{
	struct cstor_device *cdev = uctx->cdev;
	struct cstor_invalidate_iscsi_tag_cmd cmd;
	struct cstor_iscsi_tag_info *tinfo;
	int ret, i;

	if (copy_from_user(&cmd, ubuf, sizeof(cmd))) {
		cstor_err(cdev, "copy_from_user() failed, cmd size %zu", sizeof(cmd));
		return -EFAULT;
	}

	tinfo = cmd.tinfo;
	for (i = 0; i < cmd.count; i++) {
		ret = cstor_reset_mem_with_imm(cdev, tinfo->pm_addr >> 5, tinfo->dlen);
		if (ret) {
			cstor_err(cdev, "cstor_reset_mem_with_imm() failed tinfo->dlen %u ret %d\n",
				  tinfo->dlen, ret);
			return ret;
		}

		tinfo++;
	}

	return 0;
}

static int alloc_stag(struct cstor_device *cdev, u32 *stag)
{
	u32 stag_idx;
	static atomic_t key;

	stag_idx = cxgb4_uld_get_resource(&cdev->resource.tpt_table);
	if (!stag_idx) {
		cstor_err(cdev, "unable to get stag_idx\n");
		mutex_lock(&cdev->stats.lock);
		cdev->stats.stag.fail++;
		mutex_unlock(&cdev->stats.lock);
		return -ENOMEM;
	}

	mutex_lock(&cdev->stats.lock);
	cdev->stats.stag.cur += 32;
	if (cdev->stats.stag.cur > cdev->stats.stag.max)
		cdev->stats.stag.max = cdev->stats.stag.cur;
	mutex_unlock(&cdev->stats.lock);

	stag_idx += (cdev->lldi.vr->stag.size >> 5);
	*stag = (stag_idx << 8) | (atomic_inc_return(&key) & 0xff);

	return 0;
}

static void free_stag(struct cstor_device *cdev, u32 stag)
{
	u32 stag_idx = (stag >> 8) - (cdev->lldi.vr->stag.size >> 5);

	cxgb4_uld_put_resource(&cdev->resource.tpt_table, stag_idx);

	mutex_lock(&cdev->stats.lock);
	cdev->stats.stag.cur -= 32;
	mutex_unlock(&cdev->stats.lock);
}

static void
cstor_fill_tpte(struct cstor_device *cdev, struct fw_ri_tpte *tpt, struct tpt_attributes *attr)
{
	tpt->valid_to_pdid = cpu_to_be32(FW_RI_TPTE_VALID_F |
			FW_RI_TPTE_STAGKEY_V((attr->stag & FW_RI_TPTE_STAGKEY_M)) |
			FW_RI_TPTE_STAGSTATE_V(1) |
			FW_RI_TPTE_STAGTYPE_V(attr->srq ? FW_RI_STAG_SMR : FW_RI_STAG_NSMR) |
			FW_RI_TPTE_PDID_V(attr->pdid));
	tpt->locread_to_qpid = cpu_to_be32(FW_RI_TPTE_PERM_V(attr->len ? attr->perms : 0) |
					(attr->mw_bind_enable ? FW_RI_TPTE_MWBINDEN_F : 0) |
					FW_RI_TPTE_ADDRTYPE_V((attr->zbva ? FW_RI_ZERO_BASED_TO :
									    FW_RI_VA_BASED_TO)) |
					FW_RI_TPTE_PS_V(attr->page_size) |
					FW_RI_TPTE_QPID_V(attr->tid));
	tpt->nosnoop_pbladdr = !attr->pbl_size ? 0 :
		cpu_to_be32(FW_RI_TPTE_PBLADDR_V(PBL_OFF(cdev, attr->pbl_addr) >> 3));
	tpt->len_lo = cpu_to_be32((u32)((attr->len ? : -1) & 0xffffffffUL));
	tpt->va_hi = cpu_to_be32((u32)(attr->va_fbo >> 32));
	tpt->va_lo_fbo = cpu_to_be32((u32)(attr->va_fbo & 0xffffffffUL));
	tpt->dca_mwbcnt_pstag = cpu_to_be32(0);
	tpt->len_hi = cpu_to_be32((u32)((attr->len ? : -1) >> 32));
}

static int
cstor_write_mem_tpte(struct cstor_device *cdev, struct tpt_attributes *tpt_attr, u32 addr)
{
	struct sk_buff *skb = cdev->skb;
	struct ulp_mem_io *req;
	struct ulptx_idata *sc;
	__be32 cmd = cpu_to_be32(ULPTX_CMD_V(ULP_TX_MEM_WRITE) | T5_ULP_MEMIO_IMM_V(1));
	u32 len = sizeof(struct fw_ri_tpte);
	u32 wr_len = sizeof(*req) + (2 * sizeof(*sc)) + len;

	skb_trim(skb, 0);
	skb_get(skb);
	set_wr_txq(skb, CPL_PRIORITY_CONTROL, cdev->lldi.ctrlq_start);
	cstor_reinit_wr_wait(&cdev->wr_wait);

	req = (struct ulp_mem_io *)__skb_put(skb, wr_len);
	cstor_fill_ulp_mem_io_req(req, &cdev->wr_wait, addr, cmd, len, wr_len);

	sc = (struct ulptx_idata *)(req + 1);
	sc->cmd_more = cpu_to_be32(ULPTX_CMD_V(ULP_TX_SC_IMM));
	sc->len = cpu_to_be32(len);

	cstor_fill_tpte(cdev, (void *)(sc + 1), tpt_attr);

	sc = (void *)(sc + 1) + len;
	sc->cmd_more = cpu_to_be32(ULPTX_CMD_V(ULP_TX_SC_NOOP));
	sc->len = cpu_to_be32(0);

	return cstor_send_wait(cdev, skb, &cdev->wr_wait, 0, 0, __func__);
}

static int alloc_pbl(struct cstor_mr *mr, int npages)
{
	mr->attr.pbl_addr = cstor_pblpool_alloc(mr->uctx->cdev, npages << 3);
	if (!mr->attr.pbl_addr) {
		cstor_err(mr->uctx->cdev, "failed to allocate mr->attr.pbl_addr, npages %d\n",
			  npages);
		return -ENOMEM;
	}

	mr->attr.pbl_size = npages;
	return 0;
}

int cstor_reg_mr(struct cstor_ucontext *uctx, void __user *ubuf)
{
	struct cstor_device *cdev = uctx->cdev;
	struct cstor_pd *pd;
	struct cstor_mr *mr;
	struct cstor_reg_mr_cmd cmd;
	struct cstor_reg_mr_resp uresp;
	void __user *_uresp;
	__u64 __user *usr_pbl_ptr;
	u64 start, length;
	u64 page_size;
	size_t npbls;
	u32 stag;
	int acc, ret;

	if (copy_from_user(&cmd, ubuf, sizeof(cmd))) {
		cstor_err(cdev, "copy_from_user() failed, cmd size %zu\n", sizeof(cmd));
		return -EFAULT;
	}

	start = cmd.start;
	length = cmd.length;
	acc = cmd.acc;

	if ((length == ~0ULL) || ((length + start) < start)) {
		cstor_err(cdev, "invalid start %llu length %llu\n", start, length);
		return -EINVAL;
	}

	pd = xa_load(&uctx->pds, cmd.pdid);
	if (!pd) {
		cstor_err(cdev, "failed to find pd with pdid %u\n", cmd.pdid);
		return -EINVAL;
	}

	mr = kzalloc(sizeof(*mr), GFP_KERNEL);
	if (!mr) {
		cstor_err(cdev, "failed to allocate mr\n");
		return -ENOMEM;
	}

	mr->uctx = uctx;

	ret = alloc_stag(cdev, &stag);
	if (ret) {
		cstor_err(cdev, "failed to allocate stag, ret %d\n", ret);
		goto err1;
	}

	mr->attr.stag = stag;

	ret = xa_insert(&cdev->mrs, stag >> 8, mr, GFP_KERNEL);
	if (ret) {
		cstor_err(cdev, "xa_insert() failed, stag %u, ret %d\n", stag, ret);
		goto err2;
	}

	mr->umem = cstor_umem_get(cdev, start, length, acc);
	if (IS_ERR(mr->umem)) {
		cstor_err(cdev, "cstor_umem_get() failed, start %llu length %llu acc %d\n",
			  start, length, acc);
		ret = -ENOMEM;
		goto err3;
	}

	page_size = cstor_umem_find_best_pgsz(mr->umem, T4_PAGE_SIZE_CAP, start);
	if (!page_size) {
		cstor_err(cdev, "cstor_umem_find_best_pgsz() failed, start %llu\n", start);
		ret = -ENOTSUPP;
		goto err4;
	}

	if ((__ffs64(page_size) - 12) > T6_MAX_PAGE_SIZE) {
		cstor_err(cdev, "Invalid page_size %llu\n", page_size);
		ret = -EINVAL;
		goto err4;
	}

	npbls = ALIGN(cstor_umem_num_dma_blocks(mr->umem, page_size), CSTOR_PBL_ALIGNMENT);

	ret = alloc_pbl(mr, npbls);
	if (ret) {
		cstor_err(cdev, "alloc_pbl() failed, npbls %lu ret %d\n", npbls, ret);
		goto err4;
	}

	usr_pbl_ptr = (__u64 __user *)(unsigned long)cmd.pbl_ptr;

	if (npbls <= (CSTOR_INLINE_THRESHOLD >> 3))
		ret = cstor_write_pbl_imm(mr, page_size, npbls << 3, usr_pbl_ptr);
	else
		ret = cstor_write_pbl_dsgl(mr, page_size, npbls << 3, usr_pbl_ptr);

	if (ret) {
		cstor_err(cdev, "cstor_write_pbl() failed, page_size %llu npbls %lu, ret %d\n",
			  page_size, npbls, ret);
		goto err5;
	}

	mr->attr.pdid = pd->pdid;
	mr->attr.zbva = 0;
	mr->attr.perms = cstor_ib_to_tpt_access(acc);
	mr->attr.va_fbo = start;
	mr->attr.page_size = __ffs64(page_size) - 12;
	mr->attr.len = length;
	mr->attr.tid = cmd.tid;
	mr->attr.srq = cmd.srq;
	ret = cstor_write_mem_tpte(cdev, &mr->attr, (stag >> 8) +
				   (cdev->lldi.vr->stag.start >> 5));
	if (ret) {
		cstor_err(cdev, "cstor_write_mem_tpte() failed, stag %u ret %d\n", stag, ret);
		goto err5;
	}

	mr->attr.state = 1;

	uresp.pbl_addr = mr->attr.pbl_addr;
	uresp.page_size = page_size;
	uresp.lkey = mr->attr.stag;
	uresp.pbl_start = cdev->lldi.vr->pbl.start;

	_uresp = &((struct cstor_reg_mr_cmd *)ubuf)->resp;
	ret = copy_to_user(_uresp, &uresp, sizeof(uresp));
	if (ret) {
		cstor_err(cdev, "copy_to_user() failed, uresp size %zu\n", sizeof(uresp));
		goto err5;
	}

	return 0;

err5:
	cstor_pblpool_free(cdev, mr->attr.pbl_addr, mr->attr.pbl_size << 3);
err4:
	cstor_umem_release(mr->umem);
err3:
	xa_erase(&cdev->mrs, stag >> 8);
err2:
	free_stag(cdev, stag);
err1:
	kfree(mr);
	return ret;
}

int __cstor_dereg_mr(struct cstor_mr *mr)
{
	struct cstor_device *cdev = mr->uctx->cdev;
	u32 addr = (mr->attr.stag >> 8) + (cdev->lldi.vr->stag.start >> 5);
	int ret;

	ret = cstor_reset_mem_with_imm(cdev, addr, sizeof(struct fw_ri_tpte));
	if (ret) {
		cstor_err(cdev, "cstor_reset_mem_with_imm() failed, addr %u ret %d\n", addr, ret);
		return ret;
	}

	xa_erase(&cdev->mrs, mr->attr.stag >> 8);
	free_stag(cdev, mr->attr.stag);

	if (mr->attr.pbl_size)
		cstor_pblpool_free(cdev, mr->attr.pbl_addr, mr->attr.pbl_size << 3);

	if (mr->umem)
		cstor_umem_release(mr->umem);

	kfree(mr);
	return 0;
}

int cstor_dereg_mr(struct cstor_ucontext *uctx, void __user *ubuf)
{
	struct cstor_device *cdev = uctx->cdev;
	struct cstor_mr *mr;
	struct cstor_dereg_mr_cmd cmd;

	if (copy_from_user(&cmd, ubuf, sizeof(cmd))) {
		cstor_err(uctx->cdev, "copy_from_user() failed, cmd size %zu\n", sizeof(cmd));
		return -EFAULT;
	}

	mr = xa_load(&cdev->mrs, cmd.lkey >> 8);

	return __cstor_dereg_mr(mr);
}
