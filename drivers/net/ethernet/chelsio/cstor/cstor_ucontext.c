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

static int
cstor_destroy_ucontext_resources(struct cstor_device *cdev, struct cstor_ucontext *uctx)
{
	struct cstor_qp *qp;
	struct cstor_srq *srq;
	struct cstor_cq *cq;
	struct cstor_mr *mr;
	struct cstor_rxq *rxq;
	unsigned long index;
	int ret;

	xa_for_each(&cdev->qps, index, qp) {
		if (qp->uctx == uctx) {
			ret = __cstor_destroy_qp(qp);
			if (ret) {
				cstor_err(cdev, "failed to destroy qp, ret %d\n", ret);
				goto out;
			}
		}
	}

	xa_for_each(&cdev->srqs, index, srq) {
		if (srq->uctx == uctx) {
			ret = __cstor_destroy_srq(srq);
			if (ret) {
				cstor_err(cdev, "failed to destroy srq, ret %d\n", ret);
				goto out;
			}
		}
	}

	xa_for_each(&cdev->rxqs, index, rxq) {
		if (rxq->uctx == uctx) {
			ret = __cstor_destroy_rxq(rxq);
			if (ret) {
				cstor_err(cdev, "failed to destroy rxq, ret %d\n", ret);
				goto out;
			}
		}
	}

	xa_for_each(&cdev->cqs, index, cq) {
		if (cq->uctx == uctx) {
			ret = __cstor_destroy_cq(cq, false);
			if (ret) {
				cstor_err(cdev, "failed to destroy cq, ret %d\n", ret);
				goto out;
			}
		}
	}

	xa_for_each(&cdev->mrs, index, mr) {
		if (mr->uctx == uctx) {
			ret = __cstor_dereg_mr(mr);
			if (ret) {
				cstor_err(cdev, "failed to destroy mr, ret %d\n", ret);
				goto out;
			}
		}
	}
out:
	return ret;
}

int cstor_dealloc_ucontext(struct cstor_ucontext *uctx)
{
	struct cstor_device *cdev = uctx->cdev;
	struct cstor_pd *pd;
	struct cstor_mm_entry *mm, *tmp;
	struct cstor_event_channel *event_channel;
	struct cstor_rxq *rxq;
	struct cstor_cq *cq;
	unsigned long index;
	int ret;

	xa_lock(&cdev->rxqs);
	xa_for_each(&cdev->rxqs, index, rxq) {
		if (rxq->uctx == uctx) {
			xa_unlock(&cdev->rxqs);
			ret = cstor_disable_rxq(rxq);
			if (ret) {
				cstor_err(cdev, "failed to disable rxq, ret %d\n", ret);
				return ret;
			}
			xa_lock(&cdev->rxqs);
		}
	}
	xa_unlock(&cdev->rxqs);

	xa_lock_bh(&cdev->cqs);
	xa_for_each(&cdev->cqs, index, cq) {
		if (cq->uctx == uctx) {
			xa_unlock_bh(&cdev->cqs);
			ret = cstor_disable_cq(cq);
			if (ret) {
				cstor_err(cdev, "failed to disable cq, ret %d\n", ret);
				return ret;
			}
			xa_lock_bh(&cdev->cqs);
		}
	}
	xa_unlock_bh(&cdev->cqs);

	mutex_lock(&cdev->mlock);
	while (atomic_read(&uctx->num_active_csk)) {
		cstor_debug(cdev, "waiting for pending responses %d\n",
			    atomic_read(&uctx->num_active_csk));
		msleep(1000);
	}

	cstor_destroy_atids(uctx);

	ret = cstor_destroy_all_listen(uctx);
	if (ret) {
		cstor_err(cdev, "cstor_destroy_all_listen() failed, ret %d\n", ret);
		goto out;
	}

	cstor_disconnect_all_sock(uctx);

	while (atomic_read(&uctx->num_csk)) {
		cstor_info(cdev, "sock list not empty %d\n", atomic_read(&uctx->num_csk));
		msleep(1000);
	}

	while (atomic_read(&uctx->num_lcsk)) {
		cstor_info(cdev, "listen sock list not empty %d\n", atomic_read(&uctx->num_lcsk));
		msleep(1000);
	}

	ret = cstor_destroy_ucontext_resources(cdev, uctx);
	if (ret) {
		cstor_err(cdev, "cstor_destroy_ucontext_resources() failed, ret %d\n", ret);
		goto out;
	}

	xa_for_each(&uctx->pds, index, pd)
		__cstor_dealloc_pd(pd);

	list_for_each_entry_safe(mm, tmp, &uctx->mmaps, entry) {
		list_del(&mm->entry);
		kfree(mm);
	}

	cxgb4_uld_release_dev_ucontext(cdev->rdma_res, &uctx->d_uctx);
	mutex_destroy(&uctx->d_uctx.lock);

	uctx->iscsi_region_inuse = false;

	xa_for_each(&uctx->event_channels, index, event_channel)
		kref_put(&event_channel->kref, _cstor_free_event_channel);

	WARN_ON(!xa_empty(&uctx->event_channels));
	xa_destroy(&uctx->event_channels);

out:
	mutex_unlock(&cdev->mlock);
	return ret;
}

struct cstor_ucontext *cstor_alloc_ucontext(struct cstor_device *cdev)
{
	struct cstor_ucontext *uctx;

	uctx = kzalloc(sizeof(*uctx), GFP_KERNEL);
	if (!uctx) {
		cstor_err(cdev, "failed to allocate uctx\n");
		return NULL;
	}

	INIT_LIST_HEAD(&uctx->mmaps);
	spin_lock_init(&uctx->mmap_lock);
	xa_init(&uctx->pds);
	xa_init(&uctx->event_channels);
	cxgb4_uld_init_dev_ucontext(&uctx->d_uctx);
	atomic_set(&uctx->num_csk, 0);
	atomic_set(&uctx->num_lcsk, 0);
	atomic_set(&uctx->num_active_csk, 0);
	uctx->cdev = cdev;

	snprintf(uctx->name, sizeof(uctx->name), "%s_%d", current->comm, current->pid);

	mutex_lock(&cdev->ucontext_list_lock);
	list_add_tail(&uctx->entry, &cdev->ucontext_list);
	mutex_unlock(&cdev->ucontext_list_lock);

	return uctx;
}
