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
#include <linux/sched.h>

#include "umem.h"

struct cstor_debugfs_data {
	struct cstor_device *cdev;
	char *buf;
	int bufsize;
	int pos;
	void (*dump_fn)(struct cstor_debugfs_data *d, struct cstor_ucontext *uctx, int count);
};

static ssize_t
debugfs_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct cstor_debugfs_data *d = file->private_data;
	loff_t pos = *ppos;
	loff_t avail;

	if (!d)
		return 0;

	if (pos < 0) {
		cstor_err(d->cdev, "Invalid offset, pos %llu\n", pos);
		return -EINVAL;
	}

	avail = d->pos;
	if (pos >= avail)
		return 0;

	if (count > (avail - pos))
		count = avail - pos;

	while (count) {
		size_t len = min_t(size_t, count, d->pos - pos);

		if (len == 0) {
			cstor_err(d->cdev, "Invalid len %lu\n", len);
			return -EINVAL;
		}

		if (copy_to_user(buf, d->buf + pos, len)) {
			cstor_err(d->cdev, "copy_to_user() failed, len %lu\n", len);
			return -EFAULT;
		}

		buf += len;
		pos += len;
		count -= len;
	}

	count = pos - *ppos;
	*ppos = pos;
	return count;
}

static void cstor_debugfs_dump(struct cstor_debugfs_data *d, int count);

static void
dump_qp(struct cstor_debugfs_data *qpd, struct cstor_ucontext *uctx, int count)
{
	struct cstor_qp *qp;
	unsigned long index;
	int space = qpd->bufsize - qpd->pos - 1;
	int cc;

	xa_for_each(&qpd->cdev->qps, index, qp) {
		if (qp->uctx != uctx)
			continue;

		if (index != qp->wq.sq.qid)
			continue;

		if (qp->csk) {
			struct cstor_sock *csk = qp->csk;

			if (csk->laddr.ss_family == AF_INET) {
				struct sockaddr_in *lsin = (struct sockaddr_in *)&csk->laddr;
				struct sockaddr_in *rsin = (struct sockaddr_in *)&csk->raddr;

				cc = snprintf(qpd->buf + qpd->pos, space,
					      "rc qp sq id %5u %3s id %5u state %u "
					      "tid %6u state %s (%d) %pI4:%u->%pI4:%u\n",
					      qp->wq.sq.qid, qp->srq ? "srq" : "rq",
					      qp->srq ? qp->srq->idx : qp->wq.rq.qid,
					      qp->attr.state, csk->tid, states[csk->state],
					      csk->state, &lsin->sin_addr,
					      be16_to_cpu(lsin->sin_port), &rsin->sin_addr,
					      be16_to_cpu(rsin->sin_port));
			} else {
				struct sockaddr_in6 *lsin6 = (struct sockaddr_in6 *)&csk->laddr;
				struct sockaddr_in6 *rsin6 = (struct sockaddr_in6 *)&csk->laddr;

				cc = snprintf(qpd->buf + qpd->pos, space,
					      "rc qp sq id %5u rq id %5u state %u "
					      "tid %6u state %s (%d) %pI6:%u->%pI6:%u\n",
					      qp->wq.sq.qid, qp->wq.rq.qid, qp->attr.state,
					      csk->tid, states[csk->state], csk->state,
					      &lsin6->sin6_addr, be16_to_cpu(lsin6->sin6_port),
					      &rsin6->sin6_addr, be16_to_cpu(rsin6->sin6_port));
			}
		} else {
			cc = snprintf(qpd->buf + qpd->pos, space,
				      "rc qp sq id %5u rq id %5u state %u\n",
				      qp->wq.sq.qid, qp->wq.rq.qid, qp->attr.state);
		}

		if (cc >= space)
			break;

		qpd->pos += cc;
		space -= cc;

		if (!(--count))
			break;
	}
}

static int qp_release(struct inode *inode, struct file *file)
{
	struct cstor_debugfs_data *qpd = file->private_data;

	if (!qpd)
		return 0;

	vfree(qpd->buf);
	kfree(qpd);
	return 0;
}

static int qp_open(struct inode *inode, struct file *file)
{
	struct cstor_device *cdev = inode->i_private;
	struct cstor_ucontext *uctx;
	struct cstor_qp *qp;
	struct cstor_debugfs_data *qpd;
	unsigned long index;
	int ret = 0, count = 0;

	mutex_lock(&cdev->mlock);

	list_for_each_entry(uctx, &cdev->ucontext_list, entry)
		count++;

	xa_for_each(&cdev->qps, index, qp)
		count++;

	if (!count)
		goto out;

	qpd = kmalloc(sizeof(*qpd), GFP_KERNEL);
	if (!qpd) {
		cstor_err(cdev, "unable to allocate qpd\n");
		ret = -ENOMEM;
		goto out;
	}

	qpd->cdev = cdev;
	qpd->pos = 0;
	qpd->bufsize = count * 180;

	qpd->buf = vmalloc(qpd->bufsize);
	if (!qpd->buf) {
		cstor_err(cdev, "unable to allocate buffer, qpd->bufsize %d\n", qpd->bufsize);
		ret = -ENOMEM;
		kfree(qpd);
		goto out;
	}

	qpd->dump_fn = dump_qp;
	cstor_debugfs_dump(qpd, count);

	file->private_data = qpd;
out:
	mutex_unlock(&cdev->mlock);
	return ret;
}

static const struct file_operations qp_debugfs_fops = {
	.owner   = THIS_MODULE,
	.open    = qp_open,
	.release = qp_release,
	.read    = debugfs_read,
};

static void
dump_cq(struct cstor_debugfs_data *cqd, struct cstor_ucontext *uctx, int count)
{
	struct cstor_cq *cq;
	unsigned long index;
	int space = cqd->bufsize - cqd->pos - 1;
	int cc;

	xa_lock_bh(&cqd->cdev->cqs);
	xa_for_each(&cqd->cdev->cqs, index, cq) {
		if (cq->uctx != uctx)
			continue;

		if (index != cq->q.cqid)
			continue;

		cc = snprintf(cqd->buf + cqd->pos, space,
			      "cq id %5u flags %#lx size %5u\n",
			      cq->q.cqid, cq->q.flags, cq->q.size);

		if (cc >= space)
			break;

		cqd->pos += cc;
		space -= cc;

		if (!(--count))
			break;
	}
	xa_unlock_bh(&cqd->cdev->cqs);
}

static int cq_release(struct inode *inode, struct file *file)
{
	struct cstor_debugfs_data *cqd = file->private_data;

	if (!cqd)
		return 0;

	vfree(cqd->buf);
	kfree(cqd);
	return 0;
}

static int cq_open(struct inode *inode, struct file *file)
{
	struct cstor_device *cdev = inode->i_private;
	struct cstor_ucontext *uctx;
	struct cstor_cq *cq;
	struct cstor_debugfs_data *cqd;
	unsigned long index;
	int ret = 0, count = 0;

	mutex_lock(&cdev->mlock);

	list_for_each_entry(uctx, &cdev->ucontext_list, entry)
		count++;

	xa_lock_bh(&cdev->cqs);
	xa_for_each(&cdev->cqs, index, cq)
		count++;
	xa_unlock_bh(&cdev->cqs);

	if (!count)
		goto out;

	cqd = kmalloc(sizeof(*cqd), GFP_KERNEL);
	if (!cqd) {
		cstor_err(cdev, "unable to allocate cqd\n");
		ret = -ENOMEM;
		goto out;
	}

	cqd->cdev = cdev;
	cqd->pos = 0;
	cqd->bufsize = count * 180;

	cqd->buf = vmalloc(cqd->bufsize);
	if (!cqd->buf) {
		cstor_err(cdev, "unable to allocate buffer, cqd->bufsize %d\n",
			  cqd->bufsize);
		ret = -ENOMEM;
		kfree(cqd);
		goto out;
	}

	cqd->dump_fn = dump_cq;
	cstor_debugfs_dump(cqd, count);

	file->private_data = cqd;
out:
	mutex_unlock(&cdev->mlock);
	return ret;
}

static const struct file_operations cq_debugfs_fops = {
	.owner   = THIS_MODULE,
	.open    = cq_open,
	.release = cq_release,
	.read    = debugfs_read,
};

static void
dump_srq(struct cstor_debugfs_data *srqd, struct cstor_ucontext *uctx, int count)
{
	struct cstor_srq *srq;
	unsigned long index;
	int space = srqd->bufsize - srqd->pos - 1;
	int cc;

	xa_for_each(&srqd->cdev->srqs, index, srq) {
		if (srq->uctx != uctx)
			continue;

		if (index != srq->wq.qid)
			continue;

		cc = snprintf(srqd->buf + srqd->pos, space,
			      "srq qid %5u idx %4d flags %#x\n",
			      srq->wq.qid, srq->idx, srq->flags);

		if (cc >= space)
			break;

		srqd->pos += cc;
		space -= cc;

		if (!(--count))
			break;
	}
}

static int srq_release(struct inode *inode, struct file *file)
{
	struct cstor_debugfs_data *srqd = file->private_data;

	if (!srqd)
		return 0;

	vfree(srqd->buf);
	kfree(srqd);
	return 0;
}

static int srq_open(struct inode *inode, struct file *file)
{
	struct cstor_device *cdev = inode->i_private;
	struct cstor_ucontext *uctx;
	struct cstor_srq *srq;
	struct cstor_debugfs_data *srqd;
	unsigned long index;
	int ret = 0, count = 0;

	mutex_lock(&cdev->mlock);

	list_for_each_entry(uctx, &cdev->ucontext_list, entry)
		count++;

	xa_for_each(&cdev->srqs, index, srq)
		count++;

	if (!count)
		goto out;

	srqd = kmalloc(sizeof(*srqd), GFP_KERNEL);
	if (!srqd) {
		cstor_err(cdev, "unable to allocate srqd\n");
		ret = -ENOMEM;
		goto out;
	}

	srqd->cdev = cdev;
	srqd->pos = 0;
	srqd->bufsize = count * 180;

	srqd->buf = vmalloc(srqd->bufsize);
	if (!srqd->buf) {
		cstor_err(cdev, "unable to allocate buffer, srqd->bufsize %d\n", srqd->bufsize);
		ret = -ENOMEM;
		kfree(srqd);
		goto out;
	}

	srqd->dump_fn = dump_srq;
	cstor_debugfs_dump(srqd, count);

	file->private_data = srqd;
out:
	mutex_unlock(&cdev->mlock);
	return ret;
}

static const struct file_operations srq_debugfs_fops = {
	.owner   = THIS_MODULE,
	.open    = srq_open,
	.release = srq_release,
	.read    = debugfs_read,
};

static void
dump_rxq(struct cstor_debugfs_data *rxqd, struct cstor_ucontext *uctx, int count)
{
	struct cstor_rxq *rxq;
	unsigned long index;
	int space = rxqd->bufsize - rxqd->pos - 1;
	int cc;

	xa_for_each(&rxqd->cdev->rxqs, index, rxq) {
		if (rxq->uctx != uctx)
			continue;

		cc = snprintf(rxqd->buf + rxqd->pos, space,
			      "rxq iq cntxt_id %4u size %4u fl cntxt_id %4u size %4u\n",
			      rxq->iq.cntxt_id, rxq->iq.size, rxq->fl.cntxt_id, rxq->fl.size);

		if (cc >= space)
			break;

		rxqd->pos += cc;
		space -= cc;

		if (!(--count))
			break;
	}
}

static int rxq_release(struct inode *inode, struct file *file)
{
	struct cstor_debugfs_data *rxqd = file->private_data;

	if (!rxqd)
		return 0;

	vfree(rxqd->buf);
	kfree(rxqd);
	return 0;
}

static int rxq_open(struct inode *inode, struct file *file)
{
	struct cstor_device *cdev = inode->i_private;
	struct cstor_ucontext *uctx;
	struct cstor_rxq *rxq;
	struct cstor_debugfs_data *rxqd;
	unsigned long index;
	int ret = 0, count = 0;

	mutex_lock(&cdev->mlock);

	list_for_each_entry(uctx, &cdev->ucontext_list, entry)
		count++;

	xa_for_each(&cdev->rxqs, index, rxq)
		count++;

	if (!count)
		goto out;

	rxqd = kmalloc(sizeof(*rxqd), GFP_KERNEL);
	if (!rxqd) {
		cstor_err(cdev, "unable to allocate rxqd\n");
		ret = -ENOMEM;
		goto out;
	}

	rxqd->cdev = cdev;
	rxqd->pos = 0;
	rxqd->bufsize = count * 180;

	rxqd->buf = vmalloc(rxqd->bufsize);
	if (!rxqd->buf) {
		cstor_err(cdev, "unable to allocate buffer, rxqd->bufsize %d\n", rxqd->bufsize);
		ret = -ENOMEM;
		kfree(rxqd);
		goto out;
	}

	rxqd->dump_fn = dump_rxq;
	cstor_debugfs_dump(rxqd, count);

	file->private_data = rxqd;
out:
	mutex_unlock(&cdev->mlock);
	return ret;
}

static const struct file_operations rxq_debugfs_fops = {
	.owner   = THIS_MODULE,
	.open    = rxq_open,
	.release = rxq_release,
	.read    = debugfs_read,
};

static void
dump_mrs(struct cstor_debugfs_data *mrsd, struct cstor_ucontext *uctx, int count)
{
	struct cstor_mr *mr;
	unsigned long index;
	int space = mrsd->bufsize - mrsd->pos - 1;
	int cc;

	xa_for_each(&mrsd->cdev->mrs, index, mr) {
		if (mr->uctx != uctx)
			continue;

		cc = snprintf(mrsd->buf + mrsd->pos, space,
			      "mrs stag %6u tid %6u pdid %2u length %10ld permissions : %5s\n",
			      mr->attr.stag, mr->attr.tid, mr->attr.pdid, mr->umem->length,
			      mr->umem->writable ? "Write" : "0");

		if (cc >= space)
			break;

		mrsd->pos += cc;
		space -= cc;

		if (!(--count))
			break;
	}
}

static int mrs_release(struct inode *inode, struct file *file)
{
	struct cstor_debugfs_data *mrsd = file->private_data;

	if (!mrsd)
		return 0;

	vfree(mrsd->buf);
	kfree(mrsd);
	return 0;
}

static int mrs_open(struct inode *inode, struct file *file)
{
	struct cstor_device *cdev = inode->i_private;
	struct cstor_ucontext *uctx;
	struct cstor_mr *mr;
	struct cstor_debugfs_data *mrsd;
	unsigned long index;
	int ret = 0, count = 0;

	mutex_lock(&cdev->mlock);

	list_for_each_entry(uctx, &cdev->ucontext_list, entry)
		count++;

	xa_for_each(&cdev->mrs, index, mr)
		count++;

	if (!count)
		goto out;

	mrsd = kmalloc(sizeof(*mrsd), GFP_KERNEL);
	if (!mrsd) {
		cstor_err(cdev, "unable to allocate mrsd\n");
		ret = -ENOMEM;
		goto out;
	}

	mrsd->cdev = cdev;
	mrsd->pos = 0;
	mrsd->bufsize = count * 180;

	mrsd->buf = vmalloc(mrsd->bufsize);
	if (!mrsd->buf) {
		cstor_err(cdev, "unable to allocate buffer, mrsd->bufsize %d\n", mrsd->bufsize);
		ret = -ENOMEM;
		kfree(mrsd);
		goto out;
	}

	mrsd->dump_fn = dump_mrs;
	cstor_debugfs_dump(mrsd, count);

	file->private_data = mrsd;
out:
	mutex_unlock(&cdev->mlock);
	return ret;
}

static const struct file_operations mrs_debugfs_fops = {
	.owner   = THIS_MODULE,
	.open    = mrs_open,
	.release = mrs_release,
	.read    = debugfs_read,
};

static int dump_stag(unsigned long id, struct cstor_debugfs_data *stagd)
{
	struct fw_ri_tpte tpte;
	int space = stagd->bufsize - stagd->pos - 1;
	int cc, ret;

	if (space == 0)
		return 1;

	ret = cxgb4_read_tpte(stagd->cdev->lldi.ports[0], (u32)id << 8, (__be32 *)&tpte);
	if (ret) {
		cstor_err(stagd->cdev, "cxgb4_read_tpte() failed, id %lu ret %d\n", id, ret);
		return ret;
	}

	cc = snprintf(stagd->buf + stagd->pos, space,
		      "stag: idx %#10x valid %d key %#4x state %d pdid %d "
		      "perm %#x ps %2d len %#10llx va %#llx\n", (u32)id << 8,
		      FW_RI_TPTE_VALID_G(be32_to_cpu(tpte.valid_to_pdid)),
		      FW_RI_TPTE_STAGKEY_G(be32_to_cpu(tpte.valid_to_pdid)),
		      FW_RI_TPTE_STAGSTATE_G(be32_to_cpu(tpte.valid_to_pdid)),
		      FW_RI_TPTE_PDID_G(be32_to_cpu(tpte.valid_to_pdid)),
		      FW_RI_TPTE_PERM_G(be32_to_cpu(tpte.locread_to_qpid)),
		      FW_RI_TPTE_PS_G(be32_to_cpu(tpte.locread_to_qpid)),
		      ((u64)be32_to_cpu(tpte.len_hi) << 32) | be32_to_cpu(tpte.len_lo),
		      ((u64)be32_to_cpu(tpte.va_hi) << 32) | be32_to_cpu(tpte.va_lo_fbo));

	if (cc < space)
		stagd->pos += cc;

	return 0;
}

static int stag_release(struct inode *inode, struct file *file)
{
	struct cstor_debugfs_data *stagd = file->private_data;

	if (!stagd)
		return 0;

	vfree(stagd->buf);
	kfree(stagd);
	return 0;
}

static int stag_open(struct inode *inode, struct file *file)
{
	struct cstor_device *cdev = inode->i_private;
	struct cstor_debugfs_data *stagd;
	void *p;
	unsigned long index;
	int ret = 0, count = 0;

	mutex_lock(&cdev->mlock);

	xa_for_each(&cdev->mrs, index, p)
		count++;

	if (!count)
		goto out;

	stagd = kmalloc(sizeof(*stagd), GFP_KERNEL);
	if (!stagd) {
		cstor_err(cdev, "unable to allocate stagd\n");
		ret = -ENOMEM;
		goto out;
	}

	stagd->cdev = cdev;
	stagd->pos = 0;
	stagd->bufsize = count * 256;

	stagd->buf = vmalloc(stagd->bufsize);
	if (!stagd->buf) {
		cstor_err(cdev, "unable to allocate buffer, stagd->bufsize %d\n", stagd->bufsize);
		ret = -ENOMEM;
		kfree(stagd);
		goto out;
	}

	xa_for_each(&cdev->mrs, index, p)
		dump_stag(index, stagd);

	file->private_data = stagd;
out:
	mutex_unlock(&cdev->mlock);
	return ret;
}

static const struct file_operations stag_debugfs_fops = {
	.owner   = THIS_MODULE,
	.open    = stag_open,
	.release = stag_release,
	.read    = debugfs_read,
};

static int stats_show(struct seq_file *seq, void *v)
{
	struct cstor_device *cdev = seq->private;

	seq_printf(seq, "%15s: %10s %10s %10s %10s\n", "Object", "Total",
		   "Current", "Max", "Fail");
	seq_printf(seq, "%15s: %10llu %10llu %10llu %10llu\n", "PDID",
		   cdev->rdma_res->stats.pd.total, cdev->rdma_res->stats.pd.cur,
		   cdev->rdma_res->stats.pd.max, cdev->rdma_res->stats.pd.fail);
	seq_printf(seq, "%15s: %10llu %10llu %10llu %10llu\n", "QID",
		   cdev->rdma_res->stats.qid.total, cdev->rdma_res->stats.qid.cur,
		   cdev->rdma_res->stats.qid.max, cdev->rdma_res->stats.qid.fail);
	seq_printf(seq, "%15s: %10llu %10llu %10llu %10llu\n", "SRQS",
		   cdev->rdma_res->stats.srqt.total, cdev->rdma_res->stats.srqt.cur,
		   cdev->rdma_res->stats.srqt.max, cdev->rdma_res->stats.srqt.fail);
	seq_printf(seq, "%15s: %10llu %10llu %10llu %10llu\n", "TPTMEM",
		   cdev->stats.stag.total, cdev->stats.stag.cur,
		   cdev->stats.stag.max, cdev->stats.stag.fail);
	seq_printf(seq, "%15s: %10llu %10llu %10llu %10llu\n", "PBLMEM",
		   cdev->stats.pbl.total, cdev->stats.pbl.cur,
		   cdev->stats.pbl.max, cdev->stats.pbl.fail);
	seq_printf(seq, "%15s: %10llu %10llu %10llu %10llu\n", "RQTMEM",
		   cdev->rdma_res->stats.rqt.total, cdev->rdma_res->stats.rqt.cur,
		   cdev->rdma_res->stats.rqt.max, cdev->rdma_res->stats.rqt.fail);
	seq_printf(seq, "%15s: %10llu\n", "NEG_ADV_RCVD", cdev->stats.neg_adv);
	return 0;
}

static int stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, stats_show, inode->i_private);
}

static ssize_t
stats_clear(struct file *file, const char __user *buf, size_t count, loff_t *pos)
{
	struct cstor_device *cdev = ((struct seq_file *)file->private_data)->private;

	mutex_lock(&cdev->stats.lock);
	cdev->stats.stag.max = 0;
	cdev->stats.stag.fail = 0;
	cdev->stats.pbl.max = 0;
	cdev->stats.pbl.fail = 0;
	mutex_unlock(&cdev->stats.lock);

	mutex_lock(&cdev->rdma_res->stats.lock);
	cdev->rdma_res->stats.pd.max = 0;
	cdev->rdma_res->stats.pd.fail = 0;
	cdev->rdma_res->stats.qid.max = 0;
	cdev->rdma_res->stats.qid.fail = 0;
	cdev->rdma_res->stats.rqt.max = 0;
	cdev->rdma_res->stats.rqt.fail = 0;
	cdev->rdma_res->stats.srqt.max = 0;
	cdev->rdma_res->stats.srqt.fail = 0;
	mutex_unlock(&cdev->rdma_res->stats.lock);

	return count;
}

static const struct file_operations stats_debugfs_fops = {
	.owner   = THIS_MODULE,
	.open    = stats_open,
	.release = single_release,
	.read	 = seq_read,
	.llseek  = seq_lseek,
	.write   = stats_clear,
};

static int dump_sock(struct cstor_sock *csk, struct cstor_debugfs_data *cskd)
{
	int space = cskd->bufsize - cskd->pos - 1;
	int cc;

	if (space == 0)
		return 1;

	if (csk->laddr.ss_family == AF_INET) {
		struct sockaddr_in *lsin = (struct sockaddr_in *)&csk->laddr;
		struct sockaddr_in *rsin = (struct sockaddr_in *)&csk->raddr;

		cc = snprintf(cskd->buf + cskd->pos, space,
			      "tid %6u port_id %d state %s (%d) flags %#4lx "
			      "history %#5lx %pI4:%d <-> %pI4:%d\n",
			      csk->tid, csk->port_id, states[csk->state], csk->state, csk->flags,
			      csk->history, &lsin->sin_addr, be16_to_cpu(lsin->sin_port),
			      &rsin->sin_addr, be16_to_cpu(rsin->sin_port));
	} else {
		struct sockaddr_in6 *lsin6 = (struct sockaddr_in6 *)&csk->laddr;
		struct sockaddr_in6 *rsin6 = (struct sockaddr_in6 *)&csk->raddr;

		cc = snprintf(cskd->buf + cskd->pos, space,
			      "tid %6u port_id %d state %s (%d) flags %#4lx "
			      "history %#5lx %pI6:%d <-> %pI6:%d\n", csk->tid, csk->port_id,
			      states[csk->state], csk->state, csk->flags, csk->history,
			      &lsin6->sin6_addr, be16_to_cpu(lsin6->sin6_port),
			      &rsin6->sin6_addr, be16_to_cpu(rsin6->sin6_port));
	}

	if (cc < space)
		cskd->pos += cc;

	return 0;
}

static void
dump_active_sock(struct cstor_debugfs_data *cskd, struct cstor_ucontext *uctx, int count)
{
	struct cstor_sock *csk;
	unsigned long index;

	xa_lock(&cskd->cdev->tids);
	xa_for_each_marked(&cskd->cdev->tids, index, csk, CSTOR_ACTIVE_SOCK) {
		if (csk->uctx == uctx) {
			dump_sock(csk, cskd);
			if (!(--count))
				break;
		}
	}
	xa_unlock(&cskd->cdev->tids);
}

static int active_sock_release(struct inode *inode, struct file *file)
{
	struct cstor_debugfs_data *cskd = file->private_data;

	if (!cskd)
		return 0;

	vfree(cskd->buf);
	kfree(cskd);
	return 0;
}

static int active_sock_open(struct inode *inode, struct file *file)
{
	struct cstor_device *cdev = inode->i_private;
	struct cstor_ucontext *uctx;
	struct cstor_sock *csk;
	struct cstor_debugfs_data *cskd;
	unsigned long index;
	int ret = 0, count = 0;

	mutex_lock(&cdev->mlock);

	list_for_each_entry(uctx, &cdev->ucontext_list, entry)
		count++;

	xa_lock(&cdev->tids);
	xa_for_each_marked(&cdev->tids, index, csk, CSTOR_ACTIVE_SOCK)
		count++;
	xa_unlock(&cdev->tids);

	if (!count)
		goto out;

	cskd = kmalloc(sizeof(*cskd), GFP_KERNEL);
	if (!cskd) {
		cstor_err(cdev, "unable to allocate cskd\n");
		ret = -ENOMEM;
		goto out;
	}

	cskd->cdev = cdev;
	cskd->pos = 0;
	cskd->bufsize = count * 240;

	cskd->buf = vmalloc(cskd->bufsize);
	if (!cskd->buf) {
		cstor_err(cdev, "unable to allocate buffer, cskd->bufsize %d\n", cskd->bufsize);
		ret = -ENOMEM;
		kfree(cskd);
		goto out;
	}

	cskd->dump_fn = dump_active_sock;
	cstor_debugfs_dump(cskd, count);

	file->private_data = cskd;
out:
	mutex_unlock(&cdev->mlock);
	return ret;
}

static const struct file_operations csk_debugfs_fops = {
	.owner   = THIS_MODULE,
	.open    = active_sock_open,
	.release = active_sock_release,
	.read    = debugfs_read,
};

static void
dump_passive_sock(struct cstor_debugfs_data *pcskd, struct cstor_ucontext *uctx, int count)
{
	struct cstor_sock *csk;
	unsigned long index;

	xa_lock(&pcskd->cdev->tids);
	xa_for_each_marked(&pcskd->cdev->tids, index, csk, CSTOR_PASSIVE_SOCK) {
		if (csk->uctx == uctx) {
			dump_sock(csk, pcskd);
			if (!(--count))
				break;
		}
	}
	xa_unlock(&pcskd->cdev->tids);
}

static int passive_sock_release(struct inode *inode, struct file *file)
{
	struct cstor_debugfs_data *pcskd = file->private_data;

	if (!pcskd)
		return 0;

	vfree(pcskd->buf);
	kfree(pcskd);
	return 0;
}

static int passive_sock_open(struct inode *inode, struct file *file)
{
	struct cstor_device *cdev = inode->i_private;
	struct cstor_ucontext *uctx;
	struct cstor_sock *csk;
	struct cstor_debugfs_data *pcskd;
	unsigned long index;
	int ret = 0, count = 0;

	mutex_lock(&cdev->mlock);

	list_for_each_entry(uctx, &cdev->ucontext_list, entry)
		count++;

	xa_lock(&cdev->tids);
	xa_for_each_marked(&cdev->tids, index, csk, CSTOR_PASSIVE_SOCK)
		count++;
	xa_unlock(&cdev->tids);

	if (!count)
		goto out;

	pcskd = kmalloc(sizeof(*pcskd), GFP_KERNEL);
	if (!pcskd) {
		cstor_err(cdev, "unable to allocate pcskd\n");
		ret = -ENOMEM;
		goto out;
	}

	pcskd->cdev = cdev;
	pcskd->pos = 0;
	pcskd->bufsize = count * 240;

	pcskd->buf = vmalloc(pcskd->bufsize);
	if (!pcskd->buf) {
		cstor_err(cdev, "unable to allocate buffer, pcskd->bufsize %d\n", pcskd->bufsize);
		ret = -ENOMEM;
		kfree(pcskd);
		goto out;
	}

	pcskd->dump_fn = dump_passive_sock;
	cstor_debugfs_dump(pcskd, count);

	file->private_data = pcskd;
out:
	mutex_unlock(&cdev->mlock);
	return ret;
}

static const struct file_operations pcsk_debugfs_fops = {
	.owner   = THIS_MODULE,
	.open    = passive_sock_open,
	.release = passive_sock_release,
	.read    = debugfs_read,
};

static void
dump_listen_sock(struct cstor_debugfs_data *lcskd, struct cstor_ucontext *uctx, int count)
{
	struct cstor_listen_sock *lcsk;
	unsigned long index;
	int space = lcskd->bufsize - lcskd->pos - 1;
	int cc;

	xa_lock(&lcskd->cdev->stids);
	xa_for_each(&lcskd->cdev->stids, index, lcsk) {
		if (lcsk->uctx != uctx)
			continue;

		if (lcsk->laddr.ss_family == AF_INET) {
			struct sockaddr_in *lsin = (struct sockaddr_in *)&lcsk->laddr;

			cc = snprintf(lcskd->buf + lcskd->pos, space,
				      "stid %6u state %u %pI4:%d\n",
				      lcsk->stid, (u32)lcsk->listen, &lsin->sin_addr,
				      be16_to_cpu(lsin->sin_port));
		} else {
			struct sockaddr_in6 *lsin6 = (struct sockaddr_in6 *)&lcsk->laddr;

			cc = snprintf(lcskd->buf + lcskd->pos, space,
				      "stid %6u state %u %pI6:%d\n",
				      lcsk->stid, (u32)lcsk->listen, &lsin6->sin6_addr,
				      be16_to_cpu(lsin6->sin6_port));
		}

		if (cc >= space)
			break;

		lcskd->pos += cc;
		space -= cc;

		if (!(--count))
			break;
	}
	xa_unlock(&lcskd->cdev->stids);
}

static int lcsk_release(struct inode *inode, struct file *file)
{
	struct cstor_debugfs_data *lcskd = file->private_data;

	if (!lcskd)
		return 0;

	vfree(lcskd->buf);
	kfree(lcskd);
	return 0;
}

static int lcsk_open(struct inode *inode, struct file *file)
{
	struct cstor_device *cdev = inode->i_private;
	struct cstor_ucontext *uctx;
	struct cstor_listen_sock *lcsk;
	struct cstor_debugfs_data *lcskd;
	unsigned long index;
	int ret = 0, count = 0;

	mutex_lock(&cdev->mlock);

	list_for_each_entry(uctx, &cdev->ucontext_list, entry)
		count++;

	xa_lock(&cdev->stids);
	xa_for_each(&cdev->stids, index, lcsk)
		count++;
	xa_unlock(&cdev->stids);

	if (!count)
		goto out;

	lcskd = kmalloc(sizeof(*lcskd), GFP_KERNEL);
	if (!lcskd) {
		cstor_err(cdev, "unable to allocate lcskd\n");
		ret = -ENOMEM;
		goto out;
	}

	lcskd->cdev = cdev;
	lcskd->pos = 0;
	lcskd->bufsize = count * 240;

	lcskd->buf = vmalloc(lcskd->bufsize);
	if (!lcskd->buf) {
		cstor_err(cdev, "unable to allocate buffer, lcskd->bufsize %d\n", lcskd->bufsize);
		ret = -ENOMEM;
		kfree(lcskd);
		goto out;
	}

	lcskd->dump_fn = dump_listen_sock;
	cstor_debugfs_dump(lcskd, count);

	file->private_data = lcskd;
out:
	mutex_unlock(&cdev->mlock);
	return ret;
}

static void cstor_debugfs_dump(struct cstor_debugfs_data *d, int count)
{
	struct cstor_ucontext *uctx;
	int space, pos, cc;
	char *buf = "################################";

	list_for_each_entry(uctx, &d->cdev->ucontext_list, entry) {
		space = d->bufsize - d->pos - 1;
		if (space == 0)
			break;

		cc = snprintf(d->buf + d->pos, space, "%s\n%.4s  %-20s  %.4s\n%s\n",
			      buf, buf, uctx->name, buf, buf);
		if (cc >= space)
			break;

		d->pos += cc;
		pos = d->pos;

		d->dump_fn(d, uctx, count);

		if (d->pos == pos) {
			space -= cc;
			cc = snprintf(d->buf + d->pos, space, "No entries found.\n");
			if (cc >= space)
				break;

			d->pos += cc;
		}

		space = d->bufsize - d->pos - 1;
		cc = snprintf(d->buf + d->pos, space, "\n");
		if (cc >= space)
			break;

		d->pos += cc;
	}
}

static const struct file_operations lcsk_debugfs_fops = {
	.owner   = THIS_MODULE,
	.open    = lcsk_open,
	.release = lcsk_release,
	.read    = debugfs_read,
};

void setup_debugfs(struct cstor_device *cdev)
{
	struct dentry *de;

	de = debugfs_create_file("stags", S_IWUSR, cdev->debugfs_root,
				 (void *)cdev, &stag_debugfs_fops);
	if (!IS_ERR(de) && de->d_inode)
		de->d_inode->i_size = SZ_4K;

	de = debugfs_create_file("stats", S_IWUSR, cdev->debugfs_root,
				 (void *)cdev, &stats_debugfs_fops);
	if (!IS_ERR(de) && de->d_inode)
		de->d_inode->i_size = SZ_4K;

	de = debugfs_create_file("active_sock", S_IWUSR, cdev->debugfs_root,
				 (void *)cdev, &csk_debugfs_fops);
	if (!IS_ERR(de) && de->d_inode)
		de->d_inode->i_size = SZ_4K;

	de = debugfs_create_file("passive_sock", S_IWUSR, cdev->debugfs_root,
				 (void *)cdev, &pcsk_debugfs_fops);
	if (!IS_ERR(de) && de->d_inode)
		de->d_inode->i_size = SZ_4K;

	de = debugfs_create_file("listen_sock", S_IWUSR, cdev->debugfs_root,
				 (void *)cdev, &lcsk_debugfs_fops);
	if (!IS_ERR(de) && de->d_inode)
		de->d_inode->i_size = SZ_4K;

	de = debugfs_create_file("qps", S_IWUSR, cdev->debugfs_root,
				 (void *)cdev, &qp_debugfs_fops);
	if (!IS_ERR(de) && de->d_inode)
		de->d_inode->i_size = SZ_4K;

	de = debugfs_create_file("cqs", S_IWUSR, cdev->debugfs_root,
				 (void *)cdev, &cq_debugfs_fops);
	if (!IS_ERR(de) && de->d_inode)
		de->d_inode->i_size = SZ_4K;

	de = debugfs_create_file("srqs", S_IWUSR, cdev->debugfs_root,
				 (void *)cdev, &srq_debugfs_fops);
	if (!IS_ERR(de) && de->d_inode)
		de->d_inode->i_size = SZ_4K;

	de = debugfs_create_file("rxqs", S_IWUSR, cdev->debugfs_root,
				 (void *)cdev, &rxq_debugfs_fops);
	if (!IS_ERR(de) && de->d_inode)
		de->d_inode->i_size = SZ_4K;

	de = debugfs_create_file("mrs", S_IWUSR, cdev->debugfs_root,
				 (void *)cdev, &mrs_debugfs_fops);
	if (!IS_ERR(de) && de->d_inode)
		de->d_inode->i_size = SZ_4K;
}
