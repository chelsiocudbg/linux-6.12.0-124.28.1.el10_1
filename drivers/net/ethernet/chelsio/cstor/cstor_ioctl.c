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
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/device.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/ethtool.h>
#include <linux/rtnetlink.h>
#include <linux/inetdevice.h>
#include <linux/io.h>
#include <linux/debugfs.h>

#include <asm/irq.h>
#include <asm/byteorder.h>

#include "cstor.h"

static const char * const ioctl_str[] = {
	[CSTOR_IOCTL_CMD_QUERY_DEVICE] = "QUERY DEVICE",
	[CSTOR_IOCTL_CMD_ALLOC_PD] = "ALLOC PD",
	[CSTOR_IOCTL_CMD_DEALLOC_PD] = "DEALLOC PD",
	[CSTOR_IOCTL_CMD_REG_MR] = "REG MR",
	[CSTOR_IOCTL_CMD_DEREG_MR] = "DEREG MR",
	[CSTOR_IOCTL_CMD_CREATE_CQ] = "CREATE CQ",
	[CSTOR_IOCTL_CMD_DESTROY_CQ] = "DESTROY CQ",
	[CSTOR_IOCTL_CMD_CREATE_SRQ] = "CREATE SRQ",
	[CSTOR_IOCTL_CMD_DESTROY_SRQ] = "DESTROY SRQ",
	[CSTOR_IOCTL_CMD_CREATE_RXQ] = "CREATE RXQ",
	[CSTOR_IOCTL_CMD_DESTROY_RXQ] = "DESTROY RXQ",
	[CSTOR_IOCTL_CMD_CREATE_QP] = "CREATE QP",
	[CSTOR_IOCTL_CMD_DESTROY_QP] = "DESTROY QP",
	[CSTOR_IOCTL_CMD_CREATE_LISTEN] = "CREATE LISTEN",
	[CSTOR_IOCTL_CMD_DESTROY_LISTEN] = "DESTROY LISTEN",
	[CSTOR_IOCTL_CMD_SOCK_ACCEPT] = "SOCK ACCEPT",
	[CSTOR_IOCTL_CMD_SOCK_REJECT] = "SOCK REJECT",
	[CSTOR_IOCTL_CMD_FREE_ATID] = "FREE_ATID",
	[CSTOR_IOCTL_CMD_SOCK_ATTACH_QP] = "SOCK ATTACH QP",
	[CSTOR_IOCTL_CMD_RESOLVE_ROUTE] = "RESOLVE ROUTE",
	[CSTOR_IOCTL_CMD_CONNECT] = "CONNECT",
	[CSTOR_IOCTL_CMD_SOCK_DISCONNECT] = "SOCK_DISCONNECT",
	[CSTOR_IOCTL_CMD_SOCK_RELEASE] = "SOCK RELEASE",
	[CSTOR_IOCTL_CMD_GET_UEVENT] = "GET UEVENT",
	[CSTOR_IOCTL_CMD_CREATE_EVENT_CHANNEL] = "CREATE EVENT CHANNEL",
	[CSTOR_IOCTL_CMD_DESTROY_EVENT_CHANNEL] = "DESTROY EVENT CHANNEL",
	[CSTOR_IOCTL_CMD_FIND_DEVICE] = "FIND_DEVICE",
	[CSTOR_IOCTL_CMD_SET_ISCSI_REGION_STATUS] = "SET ISCSI REGION STATUS",
	[CSTOR_IOCTL_CMD_ENABLE_ISCSI_DIGEST] = "ENABLE ISCSI DIGEST",
	[CSTOR_IOCTL_CMD_INVALIDATE_ISCSI_TAG] = "INVALIDATE_ISCSI_TAG",
	[CSTOR_IOCTL_CMD_SEND_ISCSI_PDU] = "SEND ISCSI PDU",
};

long cstor_ioctl(struct file *file, u32 cmd, unsigned long arg)
{
	struct cstor_ucontext *uctx = file->private_data;
	struct cstor_device *cdev = uctx->cdev;
	void __user *ubuf;
	u32 ioctl_num = _IOC_NR(cmd);
	int ret = 0;

	if (test_bit(CDEV_FLAG_FATAL_ERROR, &cdev->flags) &&
	    (ioctl_num != CSTOR_IOCTL_CMD_GET_UEVENT)) {
		cstor_err(cdev, "device in error state, cdev->flags %lx\n", cdev->flags);
		return -EIO;
	}

	if (_IOC_TYPE(cmd) != CSTOR_IOCTL_MAGIC) {
		cstor_err(cdev, "invalid cmd %u\n", cmd);
		return -ENOTTY;
	}

	cstor_debug(cdev, "ioctl cmd %u, %s\n", ioctl_num, ioctl_str[ioctl_num]);

	ubuf = (void __user *)arg;
	mutex_lock(&cdev->mlock);
	switch (cmd) {
	case CSTOR_IOCTL_QUERY_DEVICE:
		ret = cstor_query_device(uctx, ubuf);
		break;
	case CSTOR_IOCTL_ALLOC_PD:
		ret = cstor_alloc_pd(uctx, ubuf);
		break;
	case CSTOR_IOCTL_DEALLOC_PD:
		ret = cstor_dealloc_pd(uctx, ubuf);
		break;
	case CSTOR_IOCTL_REG_MR:
		ret = cstor_reg_mr(uctx, ubuf);
		break;
	case CSTOR_IOCTL_DEREG_MR:
		ret = cstor_dereg_mr(uctx, ubuf);
		break;
	case CSTOR_IOCTL_CREATE_CQ:
		ret = cstor_create_cq(uctx, ubuf);
		break;
	case CSTOR_IOCTL_DESTROY_CQ:
		ret = cstor_destroy_cq(uctx, ubuf);
		break;
	case CSTOR_IOCTL_CREATE_SRQ:
		ret = cstor_create_srq(uctx, ubuf);
		break;
	case CSTOR_IOCTL_DESTROY_SRQ:
		ret = cstor_destroy_srq(uctx, ubuf);
		break;
	case CSTOR_IOCTL_CREATE_RXQ:
		ret = cstor_create_rxq(uctx, ubuf);
		break;
	case CSTOR_IOCTL_DESTROY_RXQ:
		ret = cstor_destroy_rxq(uctx, ubuf);
		break;
	case CSTOR_IOCTL_CREATE_QP:
		ret = cstor_create_qp(uctx, ubuf);
		break;
	case CSTOR_IOCTL_DESTROY_QP:
		ret = cstor_destroy_qp(uctx, ubuf);
		break;
	case CSTOR_IOCTL_CREATE_LISTEN:
		ret = cstor_create_listen(uctx, ubuf);
		break;
	case CSTOR_IOCTL_DESTROY_LISTEN:
		ret = cstor_destroy_listen(uctx, ubuf);
		break;
	case CSTOR_IOCTL_SOCK_ACCEPT:
		ret = cstor_sock_accept(uctx, ubuf);
		break;
	case CSTOR_IOCTL_SOCK_REJECT:
		ret = cstor_sock_reject(uctx, ubuf);
		break;
	case CSTOR_IOCTL_FREE_ATID:
		ret = cstor_free_atid(uctx, ubuf);
		break;
	case CSTOR_IOCTL_SOCK_ATTACH_QP:
		ret = cstor_sock_attach_qp(uctx, ubuf);
		break;
	case CSTOR_IOCTL_RESOLVE_ROUTE:
		ret = cstor_resolve_route(uctx, ubuf);
		break;
	case CSTOR_IOCTL_CONNECT:
		ret = cstor_connect(uctx, ubuf);
		break;
	case CSTOR_IOCTL_SOCK_DISCONNECT:
		ret = _cstor_sock_disconnect(uctx, ubuf);
		break;
	case CSTOR_IOCTL_SOCK_RELEASE:
		ret = cstor_sock_release(uctx, ubuf);
		break;
	case CSTOR_IOCTL_GET_UEVENT:
		ret = cstor_get_event(uctx, ubuf);
		break;
	case CSTOR_IOCTL_CREATE_EVENT_CHANNEL:
		ret = cstor_create_event_channel(uctx, ubuf);
		break;
	case CSTOR_IOCTL_DESTROY_EVENT_CHANNEL:
		ret = cstor_destroy_event_channel(uctx, ubuf);
		break;
	case CSTOR_IOCTL_FIND_DEVICE:
		ret = cstor_find_device(uctx, ubuf);
		break;
	case CSTOR_IOCTL_SET_ISCSI_REGION_STATUS:
		ret = cstor_set_iscsi_region_status(uctx, ubuf);
		break;
	case CSTOR_IOCTL_ENABLE_ISCSI_DIGEST:
		ret = cstor_enable_iscsi_digest(uctx, ubuf);
		break;
	case CSTOR_IOCTL_INVALIDATE_ISCSI_TAG:
		ret = cstor_invalidate_iscsi_tag(uctx, ubuf);
		break;
	case CSTOR_IOCTL_SEND_ISCSI_PDU:
		ret = cstor_send_iscsi_pdu(uctx, ubuf);
		break;
	default:
		cstor_err(cdev, "invalid ioctl %u", ioctl_num);
		ret = -ENOTTY;
	}
	mutex_unlock(&cdev->mlock);

	return ret;
}

int cstor_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct cstor_ucontext *uctx = file->private_data;
	struct cstor_device *cdev = uctx->cdev;
	struct cstor_mm_entry *mm;
	u32 key = vma->vm_pgoff << PAGE_SHIFT;
	u32 len = vma->vm_end - vma->vm_start;
	int ret = 0;

	if (test_bit(CDEV_FLAG_FATAL_ERROR, &cdev->flags)) {
		cstor_err(cdev, "device in error state, cdev->flags %lx\n", cdev->flags);
		return -EIO;
	}

	cstor_debug(cdev, "pgoff %#lx key %u len %u\n", vma->vm_pgoff, key, len);

	if (vma->vm_start & (PAGE_SIZE - 1)) {
		cstor_err(cdev, "invalid vm_start %lu\n", vma->vm_start);
		return -EINVAL;
	}

	mm = remove_mmap(uctx, key, len);
	if (!mm) {
		cstor_err(cdev, "failed remove_mmap(), key %u len %u\n", key, len);
		return -EINVAL;
	}

	if (mm->vaddr) {
		unsigned long vm_pgoff = vma->vm_pgoff;

		/*
		 * Map WQ or CQ dma memory...
		 */
		vma->vm_pgoff = 0;
		ret = dma_mmap_coherent(&cdev->lldi.pdev->dev, vma, mm->vaddr, mm->dma_addr,
					mm->len);
		vma->vm_pgoff = vm_pgoff;
	} else {
		/*
		 * Map user DB memory...
		 */
		if (enable_wc && !cdev->lldi.plat_dev)
			vma->vm_page_prot = t4_pgprot_wc(vma->vm_page_prot);
		else
			vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

		ret = io_remap_pfn_range(vma, vma->vm_start, mm->addr >> PAGE_SHIFT,
					 len, vma->vm_page_prot);
	}

	kfree(mm);
	return ret;
}

int cstor_open(struct inode *inode, struct file *file)
{
	struct cstor_ucontext *uctx;
	struct cstor_device *cdev;
	struct cdev *c_dev = file->f_inode->i_cdev;

	cdev = container_of(c_dev, struct cstor_device, c_dev);
	cstor_info(cdev, "device file opened\n");

	if (test_bit(CDEV_FLAG_FATAL_ERROR, &cdev->flags) ||
	    !test_bit(CDEV_FLAG_STATE_UP, &cdev->flags)) {
		cstor_err(cdev, "device in error state, flag %lu\n", cdev->flags);
		return -EIO;
	}

	if (!try_module_get(THIS_MODULE)) {
		cstor_err(cdev, "failed to increment module reference count\n");
		return -EAGAIN;
	}

	uctx = cstor_alloc_ucontext(cdev);
	if (!uctx) {
		cstor_err(cdev, "failed to allocate uctx\n");
		module_put(THIS_MODULE);
		return -ENOMEM;
	}

	file->private_data  = uctx;
	return 0;
}

int cstor_release(struct inode *inode, struct file *file)
{
	struct cstor_ucontext *uctx = file->private_data;
	struct cstor_device *cdev = uctx->cdev;
	int ret;

	cstor_info(cdev, "device file closed\n");

	if (test_bit(CDEV_FLAG_FATAL_ERROR, &cdev->flags)) {
		cstor_err(cdev, "device in error state, flag %lu\n", cdev->flags);
		return -EIO;
	}

	ret = cstor_dealloc_ucontext(uctx);
	if (ret) {
		cstor_err(cdev, "cstor_dealloc_ucontext() failed, ret %d\n", ret);
		return 0;
	}

	mutex_lock(&cdev->ucontext_list_lock);
	list_del(&uctx->entry);
	mutex_unlock(&cdev->ucontext_list_lock);

	kfree(uctx);

	module_put(THIS_MODULE);

	return 0;
}
