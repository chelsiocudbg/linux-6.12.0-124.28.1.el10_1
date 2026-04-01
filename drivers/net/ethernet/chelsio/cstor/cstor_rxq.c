/*
 * Copyright (c) 2025 Chelsio Communications. All rights reserved.
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

static int cstor_create_and_send_iq_cmd(struct cstor_rxq *rxq, bool free_rxq)
{
	struct fw_iq_cmd c = {};
	int ret;

	cstor_debug(rxq->uctx->cdev, "iq cntxt_id %d\n", rxq->iq.cntxt_id);
	c.op_to_vfn = cpu_to_be32(FW_CMD_OP_V(FW_IQ_CMD) | FW_CMD_REQUEST_F | FW_CMD_EXEC_F |
				  FW_IQ_CMD_PFN_V(rxq->uctx->cdev->lldi.pf) | FW_IQ_CMD_VFN_V(0));
	c.alloc_to_len16 = cpu_to_be32((free_rxq ? FW_IQ_CMD_FREE_F : FW_IQ_CMD_IQSTOP_F) |
				       FW_LEN16(c));
	c.type_to_iqandstindex = cpu_to_be32(FW_IQ_CMD_TYPE_V(FW_IQ_TYPE_FL_INT_CAP));
	c.iqid = cpu_to_be16(rxq->iq.cntxt_id);
	c.fl0id = cpu_to_be16(rxq->fl.cntxt_id);
	c.fl1id = cpu_to_be16(0xffff);

	rtnl_lock();
	ret = cxgb4_wr_mbox(rxq->netdev, &c, sizeof(c), NULL);
	rtnl_unlock();

	return ret;
}

int cstor_disable_rxq(struct cstor_rxq *rxq)
{
	int ret;

	ret = cstor_create_and_send_iq_cmd(rxq, false);
	if (ret)
		cstor_err(rxq->uctx->cdev, "failed to send iq command with err %d\n", ret);

	return ret;
}

static void free_rxq(struct cstor_rxq *rxq)
{
	struct cstor_device *cdev = rxq->uctx->cdev;
	int ret;

	ret = cstor_create_and_send_iq_cmd(rxq, true);
	if (ret) {
		cstor_err(rxq->uctx->cdev, "failed to send iq command with err %d\n", ret);
		return;
	}

	dma_free_coherent(&cdev->lldi.pdev->dev, rxq->iq.memsize, rxq->iq.desc, rxq->iq.dma_addr);
	dma_free_coherent(&cdev->lldi.pdev->dev, rxq->fl.memsize, rxq->fl.desc, rxq->fl.dma_addr);
}

static void *alloc_ring(struct cstor_device *cdev, size_t len, dma_addr_t *dma_addr)
{
	void *p;

	p = dma_alloc_coherent(&cdev->lldi.pdev->dev, len, dma_addr, GFP_KERNEL);
	if (!p) {
		cstor_err(cdev, "mem alloc failed with a len of %lu\n", len);
		return NULL;
	}

	memset(p, 0, len);
	return p;
}

static int alloc_rxq(struct cstor_rxq *rxq)
{
	struct cstor_device *cdev = rxq->uctx->cdev;
	struct t4_iq *iq = &rxq->iq;
	struct t4_fl *fl = &rxq->fl;
	struct fw_iq_cmd c = {};
	u32 v, conm;
	int ret, flsz = 0;

	iq->desc = alloc_ring(cdev, iq->memsize, &iq->dma_addr);
	if (!iq->desc) {
		cstor_err(cdev, "failed to allocate iq desc, iq memsize %lu\n", iq->memsize);
		return -ENOMEM;
	}

	fl->size = roundup(fl->size, 8);
	fl->desc = alloc_ring(cdev, fl->memsize, &fl->dma_addr);
	if (!fl->desc) {
		cstor_err(cdev, "failed to allocate fl desc, fl memsize %lu\n", fl->memsize);
		ret = -ENOMEM;
		goto err;
	}

	flsz = fl->size / 8 + cdev->hw_queue.t4_eq_status_entries;

	c.op_to_vfn = cpu_to_be32(FW_CMD_OP_V(FW_IQ_CMD) | FW_CMD_REQUEST_F |
				  FW_CMD_WRITE_F | FW_CMD_EXEC_F |
				  FW_IQ_CMD_PFN_V(cdev->lldi.pf) | FW_IQ_CMD_VFN_V(0));
	c.alloc_to_len16 = cpu_to_be32(FW_IQ_CMD_ALLOC_F | FW_IQ_CMD_IQSTART_F | FW_LEN16(c));
	c.type_to_iqandstindex = cpu_to_be32(FW_IQ_CMD_TYPE_V(FW_IQ_TYPE_FL_INT_CAP) |
					     FW_IQ_CMD_IQASYNCH_V(0) |
					     FW_IQ_CMD_VIID_V(cxgb4_port_viid(rxq->netdev)) |
					     FW_IQ_CMD_IQANUD_V(SGE_UPDATEDEL_NONE));
	c.iqdroprss_to_iqesize = cpu_to_be16(FW_IQ_CMD_IQGTSMODE_F |
					     FW_IQ_CMD_IQPCIECH_V(cxgb4_port_chan(rxq->netdev)) |
					     FW_IQ_CMD_IQESIZE_V(ilog2(T4_IQE_LEN) - 4));
	c.iqsize = cpu_to_be16(iq->size);
	c.iqaddr = cpu_to_be64(iq->dma_addr);
	c.iqns_to_fl0congen = cpu_to_be32(FW_IQ_CMD_IQFLINTCONGEN_F |
					  FW_IQ_CMD_IQTYPE_V(FW_IQ_IQTYPE_OFLD) |
					  FW_IQ_CMD_FL0DATARO_V(cdev->lldi.relaxed_ordering) |
					  FW_IQ_CMD_FL0CONGCIF_F |
					  FW_IQ_CMD_FL0FETCHRO_V(cdev->lldi.relaxed_ordering) |
					  FW_IQ_CMD_FL0HOSTFCMODE_V(SGE_HOSTFCMODE_NONE) |
					  FW_IQ_CMD_FL0PADEN_F |
					  (fl->packed ? FW_IQ_CMD_FL0PACKEN_F : 0) |
					  FW_IQ_CMD_FL0CONGEN_F);
	c.fl0dcaen_to_fl0cidxfthresh = cpu_to_be16(FW_IQ_CMD_FL0FBMIN_V(FETCHBURSTMIN_64B_T6_X) |
					FW_IQ_CMD_FL0FBMAX_V(FETCHBURSTMAX_256B_X) |
					FW_IQ_CMD_FL0CIDXFTHRESH_V(SGE_CIDXFLUSHTHRESH_1));
	c.fl0size = cpu_to_be16(flsz);
	c.fl0addr = cpu_to_be64(fl->dma_addr);

	rtnl_lock();
	ret = cxgb4_wr_mbox(rxq->netdev, &c, sizeof(c), &c);
	rtnl_unlock();
	if (ret) {
		cstor_err(cdev, "mbox error %d\n", ret);
		goto err;
	}

	iq->cntxt_id = be16_to_cpu(c.iqid);
	iq->abs_id = be16_to_cpu(c.physiqid);
	iq->size--;			/* subtract status entry */

	fl->cntxt_id = be16_to_cpu(c.fl0id);

	/*
	 * Set the congestion management context to enable congestion control
	 * signals from SGE back to TP. This allows TP to drop on ingress when
	 * no FL bufs are available.  Otherwise the SGE can get stuck...
	 */

	v = FW_PARAMS_MNEM_V(FW_PARAMS_MNEM_DMAQ) |
	    FW_PARAMS_PARAM_X_V(FW_PARAMS_PARAM_DMAQ_CONM_CTXT) |
	    FW_PARAMS_PARAM_YZ_V(iq->cntxt_id);
	conm = 1 << 19; /* CngTPMode 1 */

	rtnl_lock();
	ret = cxgb4_set_params(rxq->netdev, 1, &v, &conm);
	rtnl_unlock();
	if (ret) {
		cstor_err(cdev, "set conm ctx error %d\n", ret);
		free_rxq(rxq);
		return ret;
	}

	cstor_debug(cdev, "fl cntxt_id %d, size %d memsize %zu, "
		    "iq cntxt_id %d size %d memsize %zu packed %u\n",
		    fl->cntxt_id, fl->size, fl->memsize, iq->cntxt_id,
		    iq->size, iq->memsize, fl->packed);
	return 0;
err:
	if (iq->desc)
		dma_free_coherent(&cdev->lldi.pdev->dev, iq->memsize, iq->desc, iq->dma_addr);

	if (fl->desc)
		dma_free_coherent(&cdev->lldi.pdev->dev, fl->memsize, fl->desc, fl->dma_addr);

	return ret;
}

int cstor_create_rxq(struct cstor_ucontext *uctx, void __user *ubuf)
{
	struct cstor_device *cdev = uctx->cdev;
	struct cstor_rxq *rxq;
	struct cstor_mm_entry *fl_key_mm, *iq_key_mm;
	struct cstor_mm_entry *fl_db_key_mm = NULL, *iq_db_key_mm = NULL;
	struct cstor_create_rxq_cmd cmd;
	struct cstor_create_rxq_resp uresp = {};
	void __user *_uresp;
	u64 db_gts_pa;
	u32 bar2_qid;
	int flsize, iqsize;
	int ret;

	if (copy_from_user(&cmd, ubuf, sizeof(cmd))) {
		cstor_err(uctx->cdev, "copy_from_user() failed, cmd size %zu\n", sizeof(cmd));
		return -EFAULT;
	}

	if (cmd.fl_page_size != T4_FL_MAX_PAGE_SIZE) {
		cstor_err(cdev, "invalid fl_page_size %u\n", cmd.fl_page_size);
		return -EINVAL;
	}

	if (cmd.max_wr > cdev->hw_queue.t4_max_sq_size) {
		cstor_err(cdev, "invalid max_wr %u\n", cmd.max_wr);
		return -E2BIG;
	}

	flsize = cmd.max_wr + 1;

	iqsize = min_t(int, roundup(flsize * 4, 16), cdev->hw_queue.t4_max_iq_size);

	rxq = kzalloc(sizeof(*rxq), GFP_KERNEL);
	if (!rxq) {
		cstor_err(cdev, "failed to allocate rxq\n");
		return -ENOMEM;
	}

	rxq->fl.size = flsize;
	rxq->iq.size = iqsize;
	rxq->netdev = cdev->lldi.ports[cmd.port_id];
	rxq->uctx = uctx;
	rxq->iq.memsize = PAGE_ALIGN(rxq->iq.size * T4_IQE_LEN);
	rxq->fl.memsize = PAGE_ALIGN(rxq->fl.size * sizeof(__be64) + cdev->hw_queue.t4_stat_len);
	rxq->fl.packed = 1;
	rxq->fl.fl_align = cdev->lldi.sge_ingpadboundary;
	ret = alloc_rxq(rxq);
	if (ret) {
		cstor_err(cdev, "alloc_rxq() failed, ret %d\n", ret);
		goto err1;
	}

	cmd.max_wr = rxq->fl.size - 1;

	ret = xa_insert(&cdev->rxqs, rxq->iq.cntxt_id, rxq, GFP_KERNEL);
	if (ret) {
		cstor_err(cdev, "__xa_insert() failed, rxq->iq.cntxt_id %u ret %d\n",
			  rxq->iq.cntxt_id, ret);
		goto err2;
	}

	ret = -ENOMEM;
	fl_key_mm = kmalloc(sizeof(*fl_key_mm), GFP_KERNEL);
	if (!fl_key_mm) {
		cstor_err(cdev, "failed to allocate fl_key_mm\n");
		goto err3;
	}

	iq_key_mm = kmalloc(sizeof(*iq_key_mm), GFP_KERNEL);
	if (!iq_key_mm) {
		cstor_err(cdev, "failed to allocate iq_key_mm\n");
		goto err4;
	}

	fl_db_key_mm = kmalloc(sizeof(*fl_db_key_mm), GFP_KERNEL);
	if (!fl_db_key_mm) {
		cstor_err(cdev, "failed to allocate fl_db_key_mm\n");
		goto err5;
	}

	iq_db_key_mm = kmalloc(sizeof(*iq_db_key_mm), GFP_KERNEL);
	if (!iq_db_key_mm) {
		cstor_err(cdev, "failed to allocate iq_db_key_mm\n");
		goto err6;
	}

	uresp.fl_id = rxq->fl.cntxt_id;
	uresp.iq_id = rxq->iq.cntxt_id;
	uresp.abs_id = rxq->iq.abs_id;
	uresp.fl_size = rxq->fl.size;
	uresp.iq_size = rxq->iq.size;
	uresp.fl_memsize = rxq->fl.memsize;
	uresp.iq_memsize = rxq->iq.memsize;
	uresp.qid_mask = cdev->rdma_res->qpmask;
	uresp.fl_align = rxq->fl.fl_align;
	uresp.iqe_len = T4_IQE_LEN;

	spin_lock(&uctx->mmap_lock);
	uresp.fl_key = uctx->key;
	uctx->key += PAGE_SIZE;
	uresp.iq_key = uctx->key;
	uctx->key += PAGE_SIZE;
	uresp.fl_bar2_key = uctx->key;
	uctx->key += PAGE_SIZE;
	uresp.iq_bar2_key = uctx->key;
	uctx->key += PAGE_SIZE;
	spin_unlock(&uctx->mmap_lock);

	_uresp = &((struct cstor_create_rxq_cmd *)ubuf)->resp;
	ret = copy_to_user(_uresp, &uresp, sizeof(uresp));
	if (ret) {
		cstor_err(cdev, "copy_to_user() failed, uresp size %zu\n", sizeof(uresp));
		ret = -EFAULT;
		goto err7;
	}

	fl_key_mm->key = uresp.fl_key;
	fl_key_mm->vaddr = rxq->fl.desc;
	fl_key_mm->dma_addr = rxq->fl.dma_addr;
	fl_key_mm->len = uresp.fl_memsize;
	insert_mmap(uctx, fl_key_mm);
	iq_key_mm->key = uresp.iq_key;
	iq_key_mm->vaddr = rxq->iq.desc;
	iq_key_mm->dma_addr = rxq->iq.dma_addr;
	iq_key_mm->len = uresp.iq_memsize;
	insert_mmap(uctx, iq_key_mm);

	ret = cstor_get_db_gts_phys_addr(cdev, rxq->fl.cntxt_id, T4_BAR2_QTYPE_EGRESS, &bar2_qid,
					 &db_gts_pa);
	if (ret) {
		cstor_err(cdev, "failed to get bar2 addr for rxq->fl.cntxt_id %u, ret %d\n",
			  rxq->fl.cntxt_id, ret);
		goto err8;
	}

	fl_db_key_mm->key = uresp.fl_bar2_key;
	fl_db_key_mm->addr = db_gts_pa;
	fl_db_key_mm->vaddr = NULL;
	fl_db_key_mm->len = PAGE_SIZE;
	insert_mmap(uctx, fl_db_key_mm);

	ret = cstor_get_db_gts_phys_addr(cdev, rxq->iq.cntxt_id, T4_BAR2_QTYPE_EGRESS, &bar2_qid,
					 &db_gts_pa);
	if (ret) {
		cstor_err(cdev, "failed to get bar2 addr for rxq->iq.cntxt_id %u, ret %d\n",
			  rxq->iq.cntxt_id, ret);
		goto err9;
	}

	iq_db_key_mm->key = uresp.iq_bar2_key;
	iq_db_key_mm->addr = db_gts_pa;
	iq_db_key_mm->vaddr = NULL;
	iq_db_key_mm->len = PAGE_SIZE;
	insert_mmap(uctx, iq_db_key_mm);

	cstor_debug(cdev, "fl id %u size %u memsize %zu num_entries %u "
		    "iq id %u size %u memsize %zu num_entries %u\n",
		    rxq->fl.cntxt_id, rxq->fl.size, rxq->fl.memsize, cmd.max_wr,
		    rxq->iq.cntxt_id, rxq->iq.size, rxq->iq.memsize, rxq->iq.size - 1);
	return 0;
err9:
	remove_mmap(uctx, fl_db_key_mm->key, fl_db_key_mm->len);
err8:
	remove_mmap(uctx, iq_key_mm->key, iq_key_mm->len);
	remove_mmap(uctx, fl_key_mm->key, fl_key_mm->len);
err7:
	kfree(iq_db_key_mm);
err6:
	kfree(fl_db_key_mm);
err5:
	kfree(iq_key_mm);
err4:
	kfree(fl_key_mm);
err3:
	xa_erase(&cdev->rxqs, rxq->iq.cntxt_id);
err2:
	free_rxq(rxq);
err1:
	kfree(rxq);
	return ret;
}

int __cstor_destroy_rxq(struct cstor_rxq *rxq)
{
	struct cstor_device *cdev = rxq->uctx->cdev;

	cstor_debug(cdev, "iqid %d\n", rxq->iq.cntxt_id);

	xa_erase(&cdev->rxqs, rxq->iq.cntxt_id);

	free_rxq(rxq);

	kfree(rxq);

	return 0;
}

int cstor_destroy_rxq(struct cstor_ucontext *uctx, void __user *ubuf)
{
	struct cstor_rxq *rxq;
	struct cstor_destroy_rxq_cmd cmd;

	if (copy_from_user(&cmd, ubuf, sizeof(cmd))) {
		cstor_err(uctx->cdev, "copy_from_user() failed, cmd size %zu\n", sizeof(cmd));
		return -EFAULT;
	}

	rxq = xa_load(&uctx->cdev->rxqs, cmd.rxqid);
	if (!rxq) {
		cstor_err(uctx->cdev, "failed to load rxq, rxqid %u\n", cmd.rxqid);
		return -EINVAL;
	}

	return __cstor_destroy_rxq(rxq);
}
