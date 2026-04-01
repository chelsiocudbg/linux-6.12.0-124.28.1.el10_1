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
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/debugfs.h>
#include <linux/math64.h>
#include <linux/vmalloc.h>

#include "cstor.h"

#ifdef CONFIG_CHELSIO_T4_DCB
#include <net/dcbevent.h>
#include "cxgb4_dcb.h"
#endif

MODULE_AUTHOR("Chelsio Communications");
MODULE_DESCRIPTION("Chelsio T7 User Space NVMe/TCP offload driver");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_VERSION(CSTOR_MODULE_VERSION);

/* 48 bytes for sizeof(struct ulp_mem_io) + (2 * sizeof(struct ulptx_idata))
 * or sizeof(struct ulp_mem_io) + sizeof(struct ulptx_sgl).
 * 256 bytes for imm.
 */
#define CSTOR_SKB_LEN	(48 + 256)

unsigned int max_mr = 512;
module_param(max_mr, uint, S_IRUGO);
MODULE_PARM_DESC(max_mr, "max MR (default 512, 1 reserved)");

unsigned int max_ddp_tag = 128;
module_param(max_ddp_tag, uint, S_IRUGO);
MODULE_PARM_DESC(max_ddp_tag, "max ddp tags per QP (default 128)");

unsigned int max_ddp_sge = 1;
module_param(max_ddp_sge, uint, S_IRUGO);
MODULE_PARM_DESC(max_ddp_sge, "max ddp sge per I/O (default 1)");

static unsigned int num_zones = DEFAULT_DDR_ZONES;
static unsigned int ppod_zone_percentage[DEFAULT_DDR_ZONES] = {40, 20, 20, 20};
module_param_array(ppod_zone_percentage, uint, &num_zones, S_IRUGO);
MODULE_PARM_DESC(ppod_zone_percentage, "ppod zone percentage config "
		 "(ppod per bit should be configured along with this) (default={40, 20, 20, 20})");

static unsigned int num_ppod_per_bit = DEFAULT_DDR_ZONES;
static unsigned int ppod_per_bit[DEFAULT_DDR_ZONES] = {16, 8, 4, 1};
module_param_array(ppod_per_bit, uint, &num_ppod_per_bit, S_IRUGO);
MODULE_PARM_DESC(ppod_per_bit, "ppod per bit config (ppod zone percentage should be configured "
		 "along with this and values should be in descending order) "
		 "(default={16, 8, 4, 1})");

unsigned int max_rt = 7;
module_param(max_rt, uint, 0644);
MODULE_PARM_DESC(max_rt, "Maximum tcp retransmission "
		 "for NVMe/TCP and iSCSI initiator (default = 7)");

bool enable_wc;
module_param(enable_wc, bool, 0644);
MODULE_PARM_DESC(enable_wc, "Enable write combining (default false)");

static LIST_HEAD(cdev_list);
static DEFINE_MUTEX(cdev_mutex);
static dev_t cstor_device;
static struct class *cstor_class;

static struct dentry *cstor_debugfs_root;

const struct file_operations cstor_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= cstor_ioctl,
	.mmap		= cstor_mmap,
	.open		= cstor_open,
	.release	= cstor_release,
};

int cstor_set_iscsi_region_status(struct cstor_ucontext *ucontext, void __user *ubuf)
{
	struct cstor_ucontext *uctx;
	struct cstor_set_iscsi_region_status_cmd cmd;
	int ret = 0;

	if (copy_from_user(&cmd, ubuf, sizeof(cmd))) {
		cstor_err(ucontext->cdev, "copy_from_user() failed, cmd size %zu\n", sizeof(cmd));
		return -EFAULT;
	}

	switch (cmd.status) {
	case CSTOR_ISCSI_REGION_INUSE:
		list_for_each_entry(uctx, &ucontext->cdev->ucontext_list, entry) {
			if (uctx->iscsi_region_inuse) {
				cstor_err(ucontext->cdev, "iscsi region in use\n");
				ret = -EBUSY;
				goto out;
			}
		}

		ucontext->iscsi_region_inuse = true;
		break;
	case CSTOR_ISCSI_REGION_FREE:
		ucontext->iscsi_region_inuse = false;
		break;
	default:
		cstor_err(ucontext->cdev, "invalid iscsi region status %u\n", cmd.status);
		ret = -EINVAL;
	}
out:
	return ret;
}

void __cstor_dealloc_pd(struct cstor_pd *pd)
{
	struct cstor_device *cdev = pd->uctx->cdev;

	cstor_debug(cdev, "pdid %#x\n", pd->pdid);
	xa_erase(&pd->uctx->pds, pd->pdid);
	cxgb4_uld_put_pdid(cdev->rdma_res, pd->pdid);
	kfree(pd);
}

int cstor_dealloc_pd(struct cstor_ucontext *uctx, void __user *ubuf)
{
	struct cstor_pd *pd;
	struct cstor_dealloc_pd_cmd cmd;

	if (copy_from_user(&cmd, ubuf, sizeof(cmd))) {
		cstor_err(uctx->cdev, "copy_from_user() failed, cmd size %zu\n", sizeof(cmd));
		return -EFAULT;
	}

	pd = xa_load(&uctx->pds, cmd.pdid);
	if (!pd) {
		cstor_err(uctx->cdev, "unable to find pd with pdid %u\n", cmd.pdid);
		return -EINVAL;
	}

	__cstor_dealloc_pd(pd);
	return 0;
}

int cstor_alloc_pd(struct cstor_ucontext *uctx, void __user *ubuf)
{
	struct cstor_device *cdev = uctx->cdev;
	struct cstor_pd *pd;
	struct cstor_alloc_pd_resp uresp;
	void __user *_uresp;
	int ret = -ENOMEM;

	pd = kzalloc(sizeof(*pd), GFP_KERNEL);
	if (!pd) {
		cstor_err(cdev, "unable to allocate cstor_pd\n");
		goto err1;
	}

	pd->pdid = cxgb4_uld_get_pdid(cdev->rdma_res);
	if (!pd->pdid) {
		cstor_err(cdev, "unable to get pdid\n");
		goto err2;
	}

	ret = xa_insert(&uctx->pds, pd->pdid, pd, GFP_KERNEL);
	if (ret) {
		cstor_err(cdev, "xa_insert() failed with err %d\n", ret);
		goto err3;
	}

	pd->uctx = uctx;

	uresp.pdid = pd->pdid;
	_uresp = &((struct cstor_alloc_pd_cmd *)ubuf)->resp;
	if (copy_to_user(_uresp, &uresp, sizeof(uresp))) {
		cstor_err(cdev, "copy_to_user() failed, uresp size %zu\n", sizeof(uresp));
		ret = -EFAULT;
		goto err4;
	}

	cstor_debug(cdev, "pdid %#x\n", pd->pdid);
	return 0;
err4:
	xa_erase(&uctx->pds, pd->pdid);
err3:
	cxgb4_uld_put_pdid(cdev->rdma_res, pd->pdid);
err2:
	kfree(pd);
err1:
	return ret;
}

static u32 cstor_get_iscsi_page_size_cap(u32 page_size_order)
{
	u32 page_size_cap = 0;
	u8 i;

	for (i = 0; i < ISCSI_PGSZ_IDX_MAX; i++)
		page_size_cap |= 1U << (ISCSI_PGSZ_BASE_SHIFT +
					((page_size_order >> (i << 3)) & 0xF));

	return page_size_cap;
}

int cstor_query_device(struct cstor_ucontext *uctx, void __user *ubuf)
{
	struct cstor_device *cdev = uctx->cdev;
	struct cxgb4_lld_info *lldi = &cdev->lldi;
	struct _cstor_device_attr *attr;
	struct cstor_query_device_resp uresp = {};
	void __user *_uresp;
	int i;

	attr = &uresp.attr;
	/* common attributes */
	snprintf(attr->name, sizeof(attr->name), "%s", pci_name(lldi->pdev));
	attr->fw_ver = lldi->fw_vers;
	for (i = 0; i < lldi->nports; i++)
		memcpy(&attr->mac_addr[i], lldi->ports[i]->dev_addr, 6);
	attr->plat_dev = lldi->plat_dev;
	attr->vendor_id = (u32)lldi->pdev->vendor;
	attr->vendor_part_id = (u32)lldi->pdev->device;
	attr->hw_ver = CHELSIO_CHIP_RELEASE(lldi->adapter_type);
	attr->chip_ver = CHELSIO_CHIP_VERSION(lldi->adapter_type);
	attr->qp_start = lldi->vr->qp.start;
	attr->max_qp = lldi->vr->qp.size;
	attr->max_qp_wr = cdev->hw_queue.t4_max_qp_depth;
	attr->max_pd = 1;
	attr->max_lso_buf_size = 0;
	attr->max_pdu_size = lldi->max_pdu_size;
	attr->stid_base = lldi->uld_tids.stids.start;
	attr->max_listen_sock = lldi->uld_tids.stids.size;
	attr->tid_base = lldi->uld_tids.tids.start;
	attr->max_sock = lldi->uld_tids.tids.size;
	attr->max_atids = lldi->uld_tids.atids.size;
	attr->num_ports = lldi->nports;
	attr->wc_enabled = lldi->plat_dev ? 0 : enable_wc;
	attr->max_mr = (lldi->vr->stag.size + lldi->vr->stor_stag.size) >> 5;
	attr->max_mr_size = T4_MAX_MR_SIZE;

	/* NVMe/TCP attributes */
	attr->nvme.page_size_cap = T4_PAGE_SIZE_CAP;
	attr->nvme.max_ddp_sge = max_ddp_sge;
	attr->nvme.max_ddp_tag = max_ddp_tag;
	attr->nvme.stag_start_addr32 = lldi->vr->stag.start >> 5;
	attr->nvme.cq_start = lldi->vr->cq.start;
	attr->nvme.max_cq = lldi->vr->cq.size;
	attr->nvme.max_cqe = cdev->hw_queue.t4_max_cq_depth;
	attr->nvme.max_srq = lldi->vr->srq.size;
	attr->nvme.max_srq_wr = cdev->hw_queue.t4_max_qp_depth;

	/* iSCSI attributes */
	attr->iscsi.fl_page_size_cap = T4_FL_MAX_PAGE_SIZE;
	attr->iscsi.ddp_page_size_cap =	cstor_get_iscsi_page_size_cap(lldi->iscsi_pgsz_order);
	attr->iscsi.region_size = lldi->vr->iscsi.size;
	attr->iscsi.iscsi_tagmask = lldi->iscsi_tagmask;
	attr->iscsi.ppod_llimit = lldi->iscsi_llimit;
	attr->iscsi.ppod_start = lldi->vr->iscsi.start;
	attr->iscsi.edram_start = lldi->vr->ppod_edram.start;
	attr->iscsi.edram_size = lldi->vr->ppod_edram.size;
	if (attr->iscsi.edram_size) {
		attr->iscsi.edram_ppod_zone_percentage[0] = 75;
		attr->iscsi.edram_ppod_zone_percentage[1] = 25;
		attr->iscsi.edram_ppod_per_bit[0] = 16;
		attr->iscsi.edram_ppod_per_bit[1] = 4;
		attr->iscsi.num_edram_zones = MAX_EDRAM_ZONES;
	}

	attr->iscsi.num_ddr_zones = num_zones;
	for (i = 0; i < attr->iscsi.num_ddr_zones; i++) {
		attr->iscsi.ddr_ppod_zone_percentage[i] = ppod_zone_percentage[i];
		attr->iscsi.ddr_ppod_per_bit[i] = ppod_per_bit[i];
	}
	_uresp = &((struct cstor_query_device_cmd *)ubuf)->resp;
	if (copy_to_user(_uresp, &uresp, sizeof(uresp))) {
		cstor_err(cdev, "copy_to_user() failed, uresp size %zu\n", sizeof(uresp));
		return -EFAULT;
	}

	return 0;
}

static atomic_t index = ATOMIC_INIT(0);
static int cstor_register_char_device(struct cstor_device *cdev)
{
	char cdevname[20];
	int ret;

	cdev->devno = MKDEV(MAJOR(cstor_device), (MINOR(cstor_device) +  atomic_read(&index)));
	cdev_init(&cdev->c_dev, &cstor_fops);
	ret = cdev_add(&cdev->c_dev, cdev->devno, 1);
	if (ret < 0) {
		cstor_err(cdev, "failed to add char device, ret %d\n", ret);
		return ret;
	}

	scnprintf(cdevname, sizeof(cdevname), CSTOR_DRIVER_NAME "%d", atomic_read(&index));
	cdev->pdev = device_create(cstor_class, NULL, cdev->devno, NULL, cdevname);
	if (IS_ERR(cdev->pdev)) {
		cstor_err(cdev, "failed to create device, devno %u err %ld\n",
			  cdev->devno, PTR_ERR(cdev->pdev));
		cdev_del(&cdev->c_dev);
		return PTR_ERR(cdev->pdev);
	}

	atomic_inc(&index);
	return 0;
}

static void cstor_unregister_char_device(struct cstor_device *cdev)
{
	device_destroy(cstor_class, cdev->devno);
	cdev_del(&cdev->c_dev);
}

static int rdma_supported(const struct cxgb4_lld_info *lldi)
{
	return (lldi->vr->stor_stag.size > 0) && (lldi->vr->stor_pbl.size > 0) &&
	       (lldi->vr->rq.size > 0) && (lldi->vr->qp.size > 0) &&
	       (lldi->vr->cq.size > 0);
}

static void cstor_free_device(struct cstor_device *cdev)
{
	debugfs_remove_recursive(cdev->debugfs_root);

	if (!IS_ERR_OR_NULL(cdev->pdev))
		cstor_unregister_char_device(cdev);

	kfree_skb(cdev->skb);

	if (cdev->pbl_pool)
		cstor_pblpool_destroy(cdev);

	if (cdev->rdma_res)
		cstor_destroy_resource(cdev);

	WARN_ON(!list_empty(&cdev->ucontext_list));
	WARN_ON(!xa_empty(&cdev->cqs));
	WARN_ON(!xa_empty(&cdev->qps));
	WARN_ON(!xa_empty(&cdev->srqs));
	WARN_ON(!xa_empty(&cdev->mrs));
	WARN_ON(!xa_empty(&cdev->rxqs));

	if (cdev->workq)
		destroy_workqueue(cdev->workq);

	xa_destroy(&cdev->cqs);
	xa_destroy(&cdev->qps);
	xa_destroy(&cdev->srqs);
	xa_destroy(&cdev->mrs);
	xa_destroy(&cdev->rxqs);
	xa_destroy(&cdev->tids);
	xa_destroy(&cdev->stids);
	xa_destroy(&cdev->atids);

	mutex_destroy(&cdev->ucontext_list_lock);
	mutex_destroy(&cdev->mlock);
	mutex_destroy(&cdev->stats.lock);
	kfree(cdev);
}

static struct cstor_device *cstor_alloc_device(const struct cxgb4_lld_info *lldi)
{
	struct cstor_device *cdev;
	struct resource *res;
	int ret;
	char workq_name[24];

	res = cxgb4_bar_resource(lldi->ports[0], lldi->plat_dev ? 0 : 2);
	if (!res) {
		cstor_printk(KERN_ERR, "cxgb4_bar_resource() failed\n");
		return ERR_PTR(-EOPNOTSUPP);
	}

	cdev = kzalloc(sizeof(*cdev), GFP_KERNEL);
	if (!cdev) {
		cstor_printk(KERN_ERR, "cannot allocate cstor device\n");
		return ERR_PTR(-ENOMEM);
	}

	cdev->lldi = *lldi;

	/* init various hw-queue params based on lld info */
	cstor_info(cdev, "ing. padding boundary is %d, egrsstatuspagesize = %d\n",
		   lldi->sge_ingpadboundary, lldi->sge_egrstatuspagesize);

	cdev->hw_queue.t4_eq_status_entries = lldi->sge_egrstatuspagesize / 64;
	cdev->hw_queue.t4_max_eq_size = 65520;
	cdev->hw_queue.t4_max_iq_size = 65520;
	cdev->hw_queue.t4_max_rq_size = 8192 - cdev->hw_queue.t4_eq_status_entries - 1;
	cdev->hw_queue.t4_max_sq_size =
		cdev->hw_queue.t4_max_eq_size -	cdev->hw_queue.t4_eq_status_entries - 1;
	cdev->hw_queue.t4_max_qp_depth = cdev->hw_queue.t4_max_rq_size;
	cdev->hw_queue.t4_max_cq_depth = cdev->hw_queue.t4_max_iq_size - 2;
	cdev->hw_queue.t4_stat_len = lldi->sge_egrstatuspagesize;

	xa_init(&cdev->cqs);
	xa_init(&cdev->qps);
	xa_init(&cdev->srqs);
	xa_init(&cdev->mrs);
	xa_init(&cdev->rxqs);
	xa_init(&cdev->tids);
	xa_init(&cdev->stids);
	xa_init(&cdev->atids);
	/* remove this */
	spin_lock_init(&cdev->slock);
	mutex_init(&cdev->stats.lock);
	INIT_LIST_HEAD(&cdev->ucontext_list);
	mutex_init(&cdev->mlock);
	mutex_init(&cdev->ucontext_list_lock);
	init_completion(&cdev->wr_wait.completion);

	skb_queue_head_init(&cdev->rxq);
	INIT_WORK(&cdev->rx_work, process_work);
	snprintf(workq_name, sizeof(workq_name), CSTOR_DRIVER_NAME "-%s", pci_name(lldi->pdev));
	cdev->workq = create_singlethread_workqueue(workq_name);
	if (!cdev->workq) {
		cstor_err(cdev, "unable to allocate cdev->workq\n");
		ret = -ENOMEM;
		goto err;
	}

	if (!lldi->plat_dev)
		cdev->bar2_pa = res->start;

	cdev->stats.stag.total = lldi->vr->stor_stag.size;
	cdev->stats.pbl.total = lldi->vr->stor_pbl.size;

	ret = cstor_init_resource(cdev, lldi->vr->stor_stag.size >> 5);
	if (ret) {
		cstor_err(cdev, "error initializing resources, ret %d\n", ret);
		goto err;
	}

	cstor_debug(cdev, "qpmask %#x cqmask %#x\n",
		    cdev->rdma_res->qpmask, cdev->rdma_res->cqmask);

	ret = cstor_pblpool_create(cdev);
	if (ret) {
		cstor_err(cdev, "cstor_pblpool_create() failed, ret %d\n", ret);
		goto err;
	}

	if (cstor_debugfs_root) {
		cdev->debugfs_root = debugfs_create_dir(pci_name(lldi->pdev), cstor_debugfs_root);
		setup_debugfs(cdev);
	}

	cdev->skb = alloc_skb(CSTOR_SKB_LEN, GFP_KERNEL);
	if (!cdev->skb) {
		cstor_err(cdev, "failed to allocate skb for tpte\n");
		ret = -ENOMEM;
		goto err;
	}

	cdev->max_dma_len = SZ_16K;

	ret = cstor_register_char_device(cdev);
	if (ret)
		goto err;

	return cdev;
err:
	cstor_free_device(cdev);
	return ERR_PTR(ret);
}

static void *cstor_uld_add(const struct cxgb4_lld_info *lldi)
{
	struct cstor_device *cdev;

	if ((CHELSIO_CHIP_VERSION(lldi->adapter_type) < CHELSIO_T7)) {
		cstor_printk(KERN_ERR, "%s: unsupported adapter, chip version %u\n",
			     pci_name(lldi->pdev), CHELSIO_CHIP_VERSION(lldi->adapter_type));
		return ERR_PTR(-EINVAL);
	}

	cstor_printk(KERN_INFO, "dev %s: Chelsio T7 user space iSCSI and NVMe/TCP offload driver "
		     "- version %s\n", pci_name(lldi->pdev), CSTOR_MODULE_VERSION);

	if (!rdma_supported(lldi)) {
		cstor_printk(KERN_ERR, "RDMA not supported on this device %s\n", pci_name(lldi->pdev));
		return ERR_PTR(-ENOSYS);
	}
	/*
	 * This implementation assumes udb_density == ucq_density!  Eventually
	 * we might need to support this but for now fail the open. Also the
	 * cqid and qpid range must match for now.
	 */
	if (lldi->udb_density != lldi->ucq_density) {
		cstor_printk(KERN_ERR, "dev %s: unsupported udb/ucq densities %u/%u\n",
			     pci_name(lldi->pdev), lldi->udb_density, lldi->ucq_density);
		return ERR_PTR(-EINVAL);
	}

	if ((lldi->vr->qp.start != lldi->vr->cq.start) ||
	    (lldi->vr->qp.size != lldi->vr->cq.size)) {
		cstor_printk(KERN_ERR, "dev %s: unsupported qp and cq id ranges "
			     "qp start %u size %u cq start %u size %u\n",
			     pci_name(lldi->pdev), lldi->vr->qp.start, lldi->vr->qp.size,
			     lldi->vr->cq.start, lldi->vr->cq.size);
		return ERR_PTR(-EINVAL);
	}

	/* This implementation requires a sge_host_page_size <= PAGE_SIZE. */
	if (lldi->sge_host_page_size > PAGE_SIZE) {
		cstor_printk(KERN_ERR, "dev %s: unsupported sge host page size %u\n",
			     pci_name(lldi->pdev), lldi->sge_host_page_size);
		return ERR_PTR(-EINVAL);
	}

	cstor_printk(KERN_INFO, "found device %s nchan %u nrxq %u ntxq %u nports %u\n",
		     pci_name(lldi->pdev), lldi->nchan, lldi->nrxq, lldi->ntxq, lldi->nports);

	cstor_printk(KERN_INFO, "dev %s stag start %#x size %#x "
		     "num stags %d pbl start %#x size %#x rq start %#x size %#x "
		     "qp qid start %u size %u cq qid start %u size %u\n",
		     pci_name(lldi->pdev), lldi->vr->stor_stag.start,
		     lldi->vr->stor_stag.size, 128 /*cstor_num_stags(lldi)*/,
		     lldi->vr->stor_pbl.start, lldi->vr->stor_pbl.size,
		     lldi->vr->rq.start, lldi->vr->rq.size, lldi->vr->qp.start,
		     lldi->vr->qp.size, lldi->vr->cq.start, lldi->vr->cq.size);

	cdev = cstor_alloc_device(lldi);
	if (IS_ERR(cdev)) {
		cstor_printk(KERN_ERR, "cstor_alloc_device() failed\n");
		return cdev;
	}

	mutex_lock(&cdev_mutex);
	list_add_tail(&cdev->entry, &cdev_list);
	mutex_unlock(&cdev_mutex);

	return cdev;
}

static struct sk_buff *t4_pktgl_to_skb(const struct pkt_gl *gl, u32 pull_len)
{
	struct sk_buff *skb;

	if (gl->tot_len <= 512) {
		skb = alloc_skb(gl->tot_len, GFP_ATOMIC);
		if (unlikely(!skb)) {
			cstor_printk(KERN_ERR, "unable to allocate socket buffer\n");
			return NULL;
		}

		__skb_put(skb, gl->tot_len);
		skb_copy_to_linear_data(skb, gl->va, gl->tot_len);
	} else {
		struct skb_shared_info *ssi;
		u32 i;

		skb = alloc_skb(pull_len, GFP_ATOMIC);
		if (unlikely(!skb)) {
			cstor_printk(KERN_ERR, "unable to allocate socket buffer\n");
			return NULL;
		}

		__skb_put(skb, pull_len);
		skb_copy_to_linear_data(skb, gl->va, pull_len);

		ssi = skb_shinfo(skb);

		skb_frag_fill_page_desc(&ssi->frags[0], gl->frags[0].page,
					gl->frags[0].offset + pull_len,
					gl->frags[0].size - pull_len);

		for (i = 1; i < gl->nfrags; i++)
			skb_frag_fill_page_desc(&ssi->frags[i], gl->frags[i].page,
						gl->frags[i].offset, gl->frags[i].size);

		ssi->nr_frags = gl->nfrags;

		skb->len = gl->tot_len;
		skb->data_len = skb->len - pull_len;
		skb->truesize += skb->data_len;

		/* Get a reference for the last page, we don't own it */
		get_page(gl->frags[gl->nfrags - 1].page);
	}

	return skb;
}

static struct sk_buff *
build_iscsi_data_skb(struct cstor_device *cdev, const struct pkt_gl *gl, const __be64 *rsp)
{
	struct sk_buff *skb;
	struct skb_shared_info *ssi;
	u32 cpl_len = sizeof(struct cpl_iscsi_data);
	u32 i;

	skb = alloc_skb(cpl_len, GFP_ATOMIC);
	if (unlikely(!skb))
		return NULL;

	__skb_put(skb, cpl_len);

	skb_copy_to_linear_data(skb, &rsp[1], cpl_len);

	ssi = skb_shinfo(skb);

	skb_frag_fill_page_desc(&ssi->frags[0], gl->frags[0].page,
				gl->frags[0].offset,
				gl->frags[0].size);

	for (i = 1; i < gl->nfrags; i++)
		skb_frag_fill_page_desc(&ssi->frags[i], gl->frags[i].page,
					gl->frags[i].offset, gl->frags[i].size);

	ssi->nr_frags = gl->nfrags;
	skb->len += gl->tot_len;
	skb->data_len += gl->tot_len;
	skb->truesize += gl->tot_len;

	/* Get a reference for the last page, we don't own it */
	get_page(gl->frags[gl->nfrags - 1].page);

	return skb;
}

static int cstor_uld_rx_handler(void *handle, const __be64 *rsp, const struct pkt_gl *gl)
{
	struct cstor_device *cdev = handle;
	struct sk_buff *skb;
	u8 opcode = *(u8 *)rsp;

	if (test_bit(CDEV_FLAG_FATAL_ERROR, &cdev->flags)) {
		cstor_err(cdev, "device in error state, flag %lu\n", cdev->flags);
		return 0;
	}

	if (gl) {
		if (gl == CXGB4_MSG_AN) {
			const struct rsp_ctrl *rc = (void *)rsp;
			u32 qid = be32_to_cpu(rc->pldbuflen_qid);
			u32 pidx = RSPD_LEN_G(be32_to_cpu(rc->hdrbuflen_pidx));

			cstor_ev_handler(cdev, qid, pidx);
			return 0;
		}

		if (opcode == CPL_ISCSI_DATA) {
			skb = build_iscsi_data_skb(cdev, gl, rsp);
			if (unlikely(!skb)) {
				cstor_err(cdev, "build_iscsi_data_skb() failed\n");
				return -ENOMEM;
			}

			goto cpl_handlers;
		}

		if (unlikely(opcode != *(u8 *)gl->va)) {
			cstor_err(cdev, "unexpected FL contents at %p, "
				  "RSS %#llx, FL %#llx, len %u\n", gl->va,
				  (unsigned long long)be64_to_cpu(*rsp),
				  (unsigned long long)be64_to_cpu(*(u64 *)gl->va),
				  gl->tot_len);
			return 0;
		}

		skb = t4_pktgl_to_skb(gl, 128);
		if (unlikely(!skb)) {
			cstor_err(cdev, "failed to alloc skb\n");
			return -ENOMEM;
		}
	} else {
		/* omit RSS and rsp_ctrl at end of descriptor */
		u32 len = 64 - sizeof(struct rsp_ctrl) - 8;

		skb = alloc_skb(256, GFP_ATOMIC);
		if (!skb) {
			cstor_err(cdev, "unable to allocate socket buffer\n");
			return -ENOMEM;
		}

		__skb_put(skb, len);
		skb_copy_to_linear_data(skb, &rsp[1], len);
	}

cpl_handlers:
	if (cstor_handlers[opcode]) {
		cstor_handlers[opcode](cdev, skb);
	} else {
		cstor_err(cdev, "no handler opcode %#x...\n", opcode);
		__kfree_skb(skb);
	}

	return 0;
}

static int cstor_uld_state_change(void *handle, enum cxgb4_state new_state)
{
	struct cstor_device *cdev = handle;

	switch (new_state) {
	case CXGB4_STATE_UP:
		cstor_info(cdev, "Up\n");
		set_bit(CDEV_FLAG_STATE_UP, &cdev->flags);
		break;
	case CXGB4_STATE_DOWN:
		cstor_info(cdev, "Down\n");
		break;
	case CXGB4_STATE_START_RECOVERY:
		cstor_info(cdev, "Fatal Error\n");
		//cstor_disable_device(cdev->dev, 1);
		break;
	case CXGB4_STATE_DETACH:
		cstor_info(cdev, "Detach\n");
		break;
	case CXGB4_STATE_FATAL_ERROR:
		cstor_info(cdev, "Fatal Error\n");
		set_bit(CDEV_FLAG_FATAL_ERROR, &cdev->flags);
		break;
	}

	return 0;
}

static struct cxgb4_uld_info cstor_uld_info = {
	.name = CSTOR_DRIVER_NAME,
	.nrxq = MAX_CSTOR_QUEUES,
	.ntxq = MAX_OFLD_QSETS,
	.rxq_size = 511,
	.ciq = true,
	.lro = false,
	.add = cstor_uld_add,
	.rx_handler = cstor_uld_rx_handler,
	.state_change = cstor_uld_state_change,
};

#ifdef CONFIG_CHELSIO_T4_DCB
struct cstor_dcb_work {
	struct dcb_app_type dcb_app;
	struct work_struct work;
};

static void cstor_dcb_workfn(struct work_struct *work)
{
	struct cstor_device *cdev = NULL;
	struct net_device *ndev;
	struct cstor_dcb_work *dcb_work = container_of(work, struct cstor_dcb_work, work);
	struct dcb_app_type *cstor_app = &dcb_work->dcb_app;
	struct sk_buff *dcb_skb;
	struct cpl_act_establish *rpl;
	u8 priority, port_id = 0xff;
	u8 i;

	if (cstor_app->dcbx & DCB_CAP_DCBX_VER_IEEE) {
		if ((cstor_app->app.selector != IEEE_8021QAZ_APP_SEL_STREAM) &&
		    (cstor_app->app.selector != IEEE_8021QAZ_APP_SEL_ANY))
			goto out;

		priority = cstor_app->app.priority;
	} else if ((cstor_app->dcbx & DCB_CAP_DCBX_VER_CEE) &&
		   (cstor_app->app.selector == DCB_APP_IDTYPE_PORTNUM) &&
		   cstor_app->app.priority) {
		priority = ffs(cstor_app->app.priority) - 1;
	} else {
		goto out;
	}

	cstor_printk(KERN_DEBUG, "priority for ifid %d is %u\n", cstor_app->ifindex, priority);

	ndev = dev_get_by_index(&init_net, cstor_app->ifindex);
	if (!ndev) {
		cstor_printk(KERN_ERR, "dev_get_by_index() failed, ifid %d\n", cstor_app->ifindex);
		goto out;
	}

	mutex_lock(&cdev_mutex);
	list_for_each_entry(cdev, &cdev_list, entry) {
		for (i = 0; i < cdev->lldi.nports; i++) {
			if (ndev == cdev->lldi.ports[i]) {
				port_id = i;
				goto dev_found;
			}
		}
	}

dev_found:
	mutex_unlock(&cdev_mutex);
	dev_put(ndev);

	if (port_id == 0xff)
		goto out;

	dcb_skb = alloc_skb(sizeof(*rpl), GFP_KERNEL);
	if (!dcb_skb) {
		cstor_printk(KERN_ERR, "dcb_skb allocation failed\n");
		goto out;
	}

	rpl = cplhdr(dcb_skb);
	rpl->ot.opcode = DCB_CPL_CONN_RESET;
	cstor_skcb_dcb_priority(dcb_skb) = priority;
	cstor_skcb_dcb_port_id(dcb_skb) = port_id;
	cstor_skcb_dcb_protocol(dcb_skb) = cstor_app->app.protocol;

	sched(cdev, dcb_skb);
out:
	kfree(dcb_work);
}

static int cstor_dcbevent_notify(struct notifier_block *nb, unsigned long action, void *data)
{
	struct cstor_dcb_work *dcb_work;
	struct dcb_app_type *dcb_app = data;

	dcb_work = kzalloc(sizeof(*dcb_work), GFP_ATOMIC);
	if (!dcb_work) {
		cstor_printk(KERN_ERR, "dcb_work allocation failed\n");
		return NOTIFY_OK;
	}

	dcb_work->dcb_app = *dcb_app;
	INIT_WORK(&dcb_work->work, cstor_dcb_workfn);
	schedule_work(&dcb_work->work);
	return NOTIFY_OK;
}

static struct notifier_block cstor_dcbevent_nb = {
	.notifier_call = cstor_dcbevent_notify,
};
#endif

static int cstor_validate_module_parameters(void)
{
	if (!is_power_of_2(max_mr)) {
		cstor_printk(KERN_ERR, "max_mr %u is not a power of 2\n", max_mr);
		return -EINVAL;
	}

	if (!is_power_of_2(max_ddp_tag)) {
		cstor_printk(KERN_ERR, "max_ddp_tag %u is not a power of 2\n", max_ddp_tag);
		return -EINVAL;
	}

	if (max_ddp_sge > 12) {
		cstor_printk(KERN_ERR, "max_ddp_sge %u must be <= 12\n", max_ddp_sge);
		return -EINVAL;
	}

	if (max_rt > MAX_RT_M) {
		cstor_printk(KERN_ERR, "max_rt %u must be <= %u\n", max_rt, MAX_RT_M);
		return -EINVAL;
	}

	return 0;
}

static int cstor_validate_ppod_config(void)
{
	u32 i, tot_percentage = 0;

	if (num_zones > MAX_DDR_ZONES) {
		cstor_printk(KERN_ERR, "num_zones %u must be <= %u\n", num_zones, MAX_DDR_ZONES);
		return -EINVAL;
	}

	if (num_ppod_per_bit != num_zones) {
		cstor_printk(KERN_ERR, "num_zones_percentages %u != ppod_per_bit_len %u\n",
			     num_zones, num_ppod_per_bit);
		return -EINVAL;
	}

	for (i = 0; i < num_zones - 1; i++) {
		if (!is_power_of_2(ppod_per_bit[i])) {
			cstor_printk(KERN_ERR, "ppod_per_bit[%u] = %u is not a power of 2\n",
				     i, ppod_per_bit[i]);
			return -EINVAL;
		}

		if (ppod_per_bit[i] <= ppod_per_bit[i + 1]) {
			cstor_printk(KERN_ERR, "ppod per bit[] should be in descending order\n");
			return -EINVAL;
		}

		tot_percentage += ppod_zone_percentage[i];
	}

	if (!is_power_of_2(ppod_per_bit[i])) {
		cstor_printk(KERN_ERR, "ppod_per_bit[%u] = %u is not a power of 2\n",
			     i, ppod_per_bit[i]);
		return -EINVAL;
	}

	tot_percentage += ppod_zone_percentage[i];
	if (tot_percentage != 100) {
		cstor_printk(KERN_ERR, "total of zone_percentage[] %u != 100 (should be 100)\n",
			     tot_percentage);
		return -EINVAL;
	}

	return 0;
}

static int __init cstor_init_module(void)
{
	int ret;

	ret = cstor_validate_module_parameters();
	if (ret) {
		cstor_printk(KERN_ERR, "cstor_validate_module_parameters() failed, ret %d\n", ret);
		return ret;
	}

	ret = cstor_validate_ppod_config();
	if (ret) {
		cstor_printk(KERN_ERR, "cstor_validate_ppod_config() failed, ret %d\n", ret);
		return ret;
	}

#define T4_MAX_ADAPTER_NUM 4
	ret = alloc_chrdev_region(&cstor_device, 0, T4_MAX_ADAPTER_NUM, CSTOR_DRIVER_NAME);
	if (ret < 0) {
		cstor_printk(KERN_ERR, "could not allocate major number\n");
		return ret;
	}

	cstor_class = class_create(CSTOR_DRIVER_NAME);
	if (IS_ERR(cstor_class)) {
		cstor_printk(KERN_ERR, "failed to create class\n");
		ret = PTR_ERR(cstor_class);
		goto err;
	}

	cstor_debugfs_root = debugfs_create_dir(CSTOR_DRIVER_NAME, NULL);
	if (IS_ERR(cstor_debugfs_root)) {
		cstor_printk(KERN_WARNING, "failed to create %s debugfs directory, ret %ld\n"
			     "continuing without debugfs\n", CSTOR_DRIVER_NAME,
			     PTR_ERR(cstor_debugfs_root));
		cstor_debugfs_root = NULL;
	}

	cxgb4_register_uld(CXGB4_ULD_TYPE_CSTOR, &cstor_uld_info);

#ifdef CONFIG_CHELSIO_T4_DCB
	cstor_printk(KERN_INFO, "dcb is enabled.\n");
	ret = register_dcbevent_notifier(&cstor_dcbevent_nb);
	if (ret < 0)
		cstor_printk(KERN_WARNING, "failed to register dcb\n");
#endif

	return 0;

err:
	unregister_chrdev_region(cstor_device, T4_MAX_ADAPTER_NUM);
	return ret;
}

static void __exit cstor_exit_module(void)
{
	struct cstor_device *cdev, *tmp;

#ifdef CONFIG_CHELSIO_T4_DCB
	unregister_dcbevent_notifier(&cstor_dcbevent_nb);
#endif
	mutex_lock(&cdev_mutex);
	list_for_each_entry_safe(cdev, tmp, &cdev_list, entry) {
		list_del(&cdev->entry);
		cstor_free_device(cdev);
	}
	mutex_unlock(&cdev_mutex);

	class_destroy(cstor_class);
	unregister_chrdev_region(cstor_device, T4_MAX_ADAPTER_NUM);
	cxgb4_unregister_uld(CXGB4_ULD_TYPE_CSTOR);
	debugfs_remove_recursive(cstor_debugfs_root);
}

module_init(cstor_init_module);
module_exit(cstor_exit_module);
