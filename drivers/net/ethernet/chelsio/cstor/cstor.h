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
#ifndef __CSTOR_H__
#define __CSTOR_H__
#include <linux/kconfig.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/completion.h>
#include <linux/netdevice.h>
#include <linux/sched.h>
#include <linux/pci.h>
#include <linux/dma-mapping.h>
#include <linux/inet.h>
#include <linux/wait.h>
#include <linux/kref.h>
#include <linux/timer.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/time.h>
#include <linux/workqueue.h>
#include <linux/eventfd.h>
#include <linux/nvme-tcp.h>
#include <linux/atomic.h>

#include <asm/byteorder.h>

#include <net/net_namespace.h>

#include "l2t.h"
#include "cxgb4.h"
#include "cxgb4_rdma_resource.h"
#include "cstor_ioctl.h"
#include "cstor_defs.h"

#ifdef pr_fmt
#undef pr_fmt
#endif

#define pr_fmt(fmt) KBUILD_MODNAME ": %s: %s: %d: %s: " fmt

#define cstor_err(cdev, fmt, ...)								\
	pr_err(fmt, pci_name((cdev)->lldi.pdev), kbasename(__FILE__), __LINE__, __func__,	\
	       ##__VA_ARGS__)

#define cstor_warn(cdev, fmt, ...)								\
	pr_warn(fmt, pci_name((cdev)->lldi.pdev), kbasename(__FILE__), __LINE__, __func__,	\
		##__VA_ARGS__)

#define cstor_info(cdev, fmt, ...)								\
	pr_info(fmt, pci_name((cdev)->lldi.pdev), kbasename(__FILE__), __LINE__, __func__,	\
		##__VA_ARGS__)

extern bool enable_debug;
#define cstor_debug(cdev, fmt, ...)								\
	do {											\
		if (enable_debug) {								\
			pr_info(fmt, pci_name((cdev)->lldi.pdev), kbasename(__FILE__),		\
				__LINE__, __func__, ##__VA_ARGS__);				\
		} else {									\
			pr_debug(fmt, pci_name((cdev)->lldi.pdev), kbasename(__FILE__),		\
				 __LINE__, __func__, ##__VA_ARGS__);				\
		}										\
	} while (0)										\

#define cstor_warn_ratelimited(cdev, fmt, ...)							\
	pr_warn_ratelimited(fmt, pci_name((cdev)->lldi.pdev), kbasename(__FILE__), __LINE__,	\
			    __func__, ##__VA_ARGS__)

#define cstor_printk(log_level, fmt, ...)							\
	do {											\
		if (enable_debug || (log_level[1] != '7'))					\
			printk(KERN_INFO KBUILD_MODNAME ": %s: %d: %s: " fmt,			\
			       kbasename(__FILE__), __LINE__, __func__, ##__VA_ARGS__);		\
	} while (0)

#define PBL_OFF(cdev_p, a) ((a) - (cdev_p)->lldi.vr->pbl.start)
#define RQT_OFF(cdev_p, a) ((a) - (cdev_p)->lldi.vr->rq.start)

#define T6_MAX_PAGE_SIZE 0x8000000

#define CSTOR_LOOPBACK_PORT_SHIFT 4

#define ISCSI_PGSZ_IDX_MAX	4
#define ISCSI_PGSZ_BASE_SHIFT	12

struct cstor_resource {
	struct cxgb4_id_table tpt_table;
	struct cxgb4_id_table qp_tag_table;
	struct cxgb4_id_table tcp_port_table;
};

enum cstor_device_flags {
	CDEV_FLAG_STOP_QUEUE_WORK	= 0,
	CDEV_FLAG_FATAL_ERROR		= 1,
	CDEV_FLAG_STATE_UP		= 2,
};

struct cstor_stats {
	struct mutex lock;
	struct cxgb4_rdma_stat stag;
	struct cxgb4_rdma_stat pbl;
	u64 neg_adv;
};

struct cstor_hw_queue {
	int t4_eq_status_entries;
	int t4_max_iq_size;
	int t4_max_eq_size;
	int t4_max_rq_size;
	int t4_max_sq_size;
	int t4_max_qp_depth;
	int t4_max_cq_depth;
	int t4_stat_len;
};

struct cstor_uevent_node {
	struct sk_buff *skb;
	struct list_head entry;
	struct cstor_uevent uevt;
};

struct cstor_wr_wait {
	struct completion completion;
	int ret;
};

struct cstor_device {
	struct device *pdev;
	struct cdev c_dev;
	dev_t devno;
	spinlock_t slock;
	struct mutex mlock;
	struct mutex ucontext_list_lock;
	struct list_head entry;
	struct list_head ucontext_list;
	struct cxgb4_lld_info lldi;
	struct cxgb4_rdma_resource *rdma_res;
	struct cstor_resource resource;
	struct cstor_stats stats;
	struct cstor_wr_wait wr_wait;
	struct cstor_wr_wait *wr_waitp;
	struct cstor_hw_queue hw_queue;
	struct xarray cqs;
	struct xarray qps;
	struct xarray srqs;
	struct xarray rxqs;
	struct xarray mrs;
	struct xarray stids;
	struct xarray atids;
	struct xarray tids;
	struct workqueue_struct *workq;
	struct work_struct rx_work;
	struct sk_buff_head rxq;
	struct sk_buff *skb;
	struct dentry *debugfs_root;
	struct gen_pool *pbl_pool;
	unsigned long bar2_pa;
	unsigned long flags;
	u32 max_dma_len;
	u16 ciq_idx;
};

struct cstor_ucontext {
	struct cstor_device *cdev;
	struct list_head entry;
	struct list_head mmaps;
	struct xarray pds;
	struct xarray event_channels;
	struct cxgb4_dev_ucontext d_uctx;
	spinlock_t mmap_lock;
	atomic_t num_lcsk;
	atomic_t num_csk;
	atomic_t num_active_csk;
	u32 key;
	u32 lcsk_close_pending;
	char name[20];
	bool iscsi_region_inuse;
};

struct cstor_dcb_info {
	u16 protocol;
	u8 priority;
	u8 port_id;
};

union cstor_skb_rx_cb {
	struct cstor_sock *csk;
	struct cstor_dcb_info dcb;
};

struct cstor_skb_tx_cb {
	u32 extra_len;
	u8 submode;
};

union cstor_skb_cb {
	struct {
		/* This member must be first. */
		struct l2t_skb_cb l2t;
		struct sk_buff *wr_next;
	};

	struct {
		union {
			struct cstor_skb_tx_cb tx;
			union cstor_skb_rx_cb rx;
		};
	};
};

#define CSTOR_SKB_CB(skb)		((union cstor_skb_cb *)&((skb)->cb[0]))

#define cstor_skcb_wr_next(skb)		(CSTOR_SKB_CB(skb)->wr_next)

#define cstor_skcb_submode(skb)		(CSTOR_SKB_CB(skb)->tx.submode)
#define cstor_skcb_tx_extralen(skb)	(CSTOR_SKB_CB(skb)->tx.extra_len)

#define cstor_skcb_rx_csk(skb)		(CSTOR_SKB_CB(skb)->rx.csk)
#define cstor_skcb_dcb_priority(skb)	(CSTOR_SKB_CB(skb)->rx.dcb.priority)
#define cstor_skcb_dcb_port_id(skb)	(CSTOR_SKB_CB(skb)->rx.dcb.port_id)
#define cstor_skcb_dcb_protocol(skb)	(CSTOR_SKB_CB(skb)->rx.dcb.protocol)

static inline int cstor_num_stags(struct cstor_device *cdev)
{
	return (int)(cdev->lldi.vr->stor_stag.size >> 5);
}

#define CSTOR_WR_TO (60 * HZ)

static inline void cstor_reinit_wr_wait(struct cstor_wr_wait *wr_waitp)
{
	wr_waitp->ret = 0;
	reinit_completion(&wr_waitp->completion);
}

static inline void cstor_wake_up(struct cstor_wr_wait *wr_waitp, int ret)
{
	wr_waitp->ret = ret;
	complete(&wr_waitp->completion);
}

static inline void *cplhdr(struct sk_buff *skb)
{
	return skb->data;
}

void cstor_disable_device(struct cstor_device *cdev);

static inline int
cstor_wait_for_reply(struct cstor_device *cdev, struct cstor_wr_wait *wr_waitp,
		     u32 tid, u32 qpid, const char *func)
{
	int ret = 0;

	if (test_bit(CDEV_FLAG_FATAL_ERROR, &cdev->flags)) {
		cstor_err(cdev, "device in error state\n");
		wr_waitp->ret = -EIO;
		goto out;
	}

	cdev->wr_waitp = wr_waitp;

	ret = wait_for_completion_timeout(&wr_waitp->completion, CSTOR_WR_TO);

	cdev->wr_waitp = NULL;
	cstor_debug(cdev, "delete wr_waitp %p\n", wr_waitp);

	if (!ret) {
		cstor_err(cdev, "tid %u qpid %u func %s - Device not responding "
			  "(disabling device)\n", tid, qpid, func);
		wr_waitp->ret = -ETIMEDOUT;
		cstor_disable_device(cdev);
		goto out;
	}

	if (wr_waitp->ret)
		cstor_debug(cdev, "tid %u qpid %u FW reply %d\n", tid, qpid, wr_waitp->ret);
out:
	return wr_waitp->ret;
}

int cstor_ofld_send(struct cstor_device *cdev, struct sk_buff *skb);

static inline int
cstor_send_wait(struct cstor_device *cdev, struct sk_buff *skb,
		struct cstor_wr_wait *wr_waitp,	u32 tid, u32 qpid, const char *func)
{
	int ret;

	cstor_debug(cdev, "tid %u qpid %u func %s wr_wait %p\n", tid, qpid, func, wr_waitp);
	ret = cstor_ofld_send(cdev, skb);
	if (ret)
		return ret;

	return cstor_wait_for_reply(cdev, wr_waitp, tid, qpid, func);
}

static inline struct cstor_cq *get_cq(struct cstor_device *cdev, u32 cqid)
{
	return xa_load(&cdev->cqs, cqid);
}

static inline struct cstor_qp *get_qp(struct cstor_device *cdev, u32 qpid)
{
	return xa_load(&cdev->qps, qpid);
}

struct cstor_pd {
	struct cstor_ucontext *uctx;
	u32 pdid;
};

struct tpt_attributes {
	u64 len;
	u64 va_fbo;
	enum fw_ri_mem_perms perms;
	u32 stag;
	u32 pdid;
	u32 qpid;
	u32 tid;
	u32 srq;
	u32 pbl_addr;
	u32 pbl_size;
	u32 state:1;
	u32 type:2;
	u32 rsvd:1;
	u32 remote_invaliate_disable:1;
	u32 zbva:1;
	u32 mw_bind_enable:1;
	u32 page_size:5;
};

struct cstor_mr {
	struct cstor_ucontext *uctx;
	struct cstor_umem *umem;
	struct tpt_attributes attr;
};

struct cstor_cq {
	struct cstor_ucontext *uctx;
	struct eventfd_ctx *eventfd_ctx;
	struct t4_cq q;
	refcount_t refcnt;
};

struct cstor_qp_attributes {
	u32 sq_num_entries;
	u32 rq_num_entries;
	u32 sq_max_ddp_sge;
	u32 state;
	u32 pdid;
	u32 tag_idx;
	u32 stag_idx;
	u32 pbl_addr;
	u32 pbl_size;
	union {
		struct {
			u8 rx_pda;
			u8 cmd_pdu_hdr_recv_zcopy;
		};
		struct {
			u32 ddp_page_size;
		};
	};
	u8 hdgst;
	u8 ddgst;
	u8 protocol;
	bool initiator;
};

struct cstor_qp {
	struct cstor_ucontext *uctx;
	struct cstor_sock *csk;
	struct cstor_srq *srq;
	struct cstor_rxq *rxq;
	struct cstor_cq *scq;
	struct cstor_cq *rcq;
	struct cstor_qp_attributes attr;
	struct t4_wq wq;
};

struct cstor_srq {
	struct cstor_ucontext *uctx;
	struct t4_srq wq;
	refcount_t refcnt;
	int idx;
	__u32 flags;
	u32 pdid;
};

struct cstor_rxq {
	struct cstor_ucontext *uctx;
	struct net_device *netdev;
	struct t4_iq iq;
	struct t4_fl fl;
};

struct cstor_mm_entry {
	struct list_head entry;
	void *vaddr;
	union {
		u64 addr;
		dma_addr_t dma_addr;
	};
	u32 key;
	u32 len;
};

static inline struct cstor_mm_entry *remove_mmap(struct cstor_ucontext *uctx, u32 key, u32 len)
{
	struct list_head *pos, *nxt;
	struct cstor_mm_entry *mm;

	spin_lock(&uctx->mmap_lock);
	list_for_each_safe(pos, nxt, &uctx->mmaps) {
		mm = list_entry(pos, struct cstor_mm_entry, entry);
		if ((mm->key == key) && (mm->len == len)) {
			list_del_init(&mm->entry);
			spin_unlock(&uctx->mmap_lock);
			cstor_debug(uctx->cdev, "key %#x addr %#llx len %d\n",
				    key, (unsigned long long)mm->addr, mm->len);
			return mm;
		}
	}
	spin_unlock(&uctx->mmap_lock);
	return NULL;
}

static inline void insert_mmap(struct cstor_ucontext *uctx, struct cstor_mm_entry *mm)
{
	cstor_debug(uctx->cdev, "key %#x addr %#llx len %d\n",
		    mm->key, (unsigned long long)mm->addr, mm->len);
	spin_lock(&uctx->mmap_lock);
	list_add_tail(&mm->entry, &uctx->mmaps);
	spin_unlock(&uctx->mmap_lock);
}

enum cstor_qp_state {
	CSTOR_QP_STATE_RESET,
	CSTOR_QP_STATE_ACTIVE,
	CSTOR_QP_STATE_ERROR,
};

static inline u32 cstor_ib_to_tpt_access(int a)
{
	return ((a & _CSTOR_ACCESS_REMOTE_WRITE) ? FW_RI_MEM_ACCESS_REM_WRITE : 0) |
	       ((a & _CSTOR_ACCESS_REMOTE_READ) ? FW_RI_MEM_ACCESS_REM_READ : 0) |
	       ((a & _CSTOR_ACCESS_LOCAL_WRITE) ? FW_RI_MEM_ACCESS_LOCAL_WRITE : 0) |
	       FW_RI_MEM_ACCESS_LOCAL_READ;
}

static inline u32 cstor_ib_to_tpt_bind_access(int acc)
{
	return ((acc & _CSTOR_ACCESS_REMOTE_WRITE) ? FW_RI_MEM_ACCESS_REM_WRITE : 0) |
	       ((acc & _CSTOR_ACCESS_REMOTE_READ) ? FW_RI_MEM_ACCESS_REM_READ : 0);
}

enum cstor_sock_state {
	CSTOR_SOCK_STATE_IDLE			= 0,
	CSTOR_SOCK_STATE_CONNECTING		= 1,
	CSTOR_SOCK_STATE_CONNECTED		= 2,
	CSTOR_SOCK_STATE_LOGIN_REQ_WAIT		= 3,
	CSTOR_SOCK_STATE_LOGIN_REQ_RCVD		= 4,
	CSTOR_SOCK_STATE_QP_MODE		= 5,
	CSTOR_SOCK_STATE_ABORTING		= 6,
	CSTOR_SOCK_STATE_CLOSING		= 7,
	CSTOR_SOCK_STATE_MORIBUND		= 8,
	CSTOR_SOCK_STATE_DEAD			= 9,
};

enum cstor_sock_flags {
	CSTOR_SOCK_F_CLOSE_SENT			= 0,
	CSTOR_SOCK_F_ACCEPT			= 1,
	CSTOR_SOCK_F_APP_REF			= 2,
	CSTOR_SOCK_F_DISCONNECT_UPCALL		= 3,
	CSTOR_SOCK_F_LPORT_VALID		= 4,
	CSTOR_SOCK_F_IPSEC_TRANSPORT_MODE	= 5,
	CSTOR_SOCK_F_IPSEC_TUNNEL_MODE		= 6,
	CSTOR_SOCK_F_ACT_OPEN			= 7,
	CSTOR_SOCK_F_ACTIVE			= 8,
	CSTOR_SOCK_F_ATTACH_QP			= 9,
	CSTOR_SOCK_F_NEG_ADV_DISCONNECT		= 10,
	CSTOR_SOCK_F_VALIDATE_CREDITS		= 11,
	CSTOR_SOCK_F_FLOWC_SENT			= 12,
	CSTOR_SOCK_F_CLIP_RELEASE		= 13,
	CSTOR_SOCK_F_TXQ_VALID			= 14,
};

enum cstor_sock_history {
	CSTOR_SOCK_H_ACT_OPEN_REQ		= 1,
	CSTOR_SOCK_H_ACT_ESTAB			= 2,
	CSTOR_SOCK_H_ACT_OPEN_RPL		= 3,
	CSTOR_SOCK_H_PASS_ACCEPT_REQ		= 4,
	CSTOR_SOCK_H_PASS_ESTAB			= 5,
	CSTOR_SOCK_H_ULP_ACCEPT			= 6,
	CSTOR_SOCK_H_ULP_REJECT			= 7,
	CSTOR_SOCK_H_TIMEDOUT			= 8,
	CSTOR_SOCK_H_PEER_ABORT			= 9,
	CSTOR_SOCK_H_PEER_CLOSE			= 10,
	CSTOR_SOCK_H_CONNREQ_UPCALL		= 11,
	CSTOR_SOCK_H_DISCONNECTED_UPCALL	= 12,
	CSTOR_SOCK_H_DISC_ABORT			= 13,
	CSTOR_SOCK_H_DISC_FAIL			= 14,
	CSTOR_SOCK_H_CONN_RPL_UPCALL		= 15,
	CSTOR_SOCK_H_CLOSE_CON_RPL		= 16,
	CSTOR_SOCK_H_ULP_DISCONNECT		= 17,
	CSTOR_SOCK_H_ULP_RELEASE		= 18,
	CSTOR_SOCK_H_ULP_FREE_ATID		= 19,
	CSTOR_SOCK_H_ULP_ATTACH_QP		= 20,
};

enum conn_pre_alloc_buffers {
	CN_ABORT_REQ_BUF,
	CN_FLOWC_BUF,
	CN_TIMEOUT_BUF,
	CN_MAX_CON_BUF
};

enum {
	FLOWC_LEN = offsetof(struct fw_flowc_wr, mnemval[FW_FLOWC_MNEM_MAX]),
};

enum {
	FAKE_CPL_PUT_SOCK_SAFE	= NUM_CPL_CMDS + 0,
	FAKE_CPL_TIMEOUT	= FAKE_CPL_PUT_SOCK_SAFE + 1,
	DCB_CPL_CONN_RESET	= FAKE_CPL_TIMEOUT + 1,
	MAX_CPL_CMDS		= DCB_CPL_CONN_RESET + 1,
};

union cpl_wr_size {
	struct cpl_abort_req abrt_req;
	struct fw_ri_wr ri_req;
	char flowc_buf[FLOWC_LEN];
};

struct cstor_event_channel {
	struct cstor_ucontext *uctx;
	struct eventfd_ctx *efd_ctx;
	struct cstor_uevent_node *fatal_uevt_node;
	struct list_head uevt_list;
	struct mutex uevt_list_lock;
	struct kref kref;
	int efd;
	int flag;

};

#define CSTOR_INVALID_STID 0xFFFFFFFF

struct cstor_listen_sock {
	struct cstor_ucontext *uctx;
	struct cstor_event_channel *event_channel;
	struct mutex mutex;
	struct sockaddr_storage laddr;
	struct cstor_wr_wait wr_wait;
	struct sk_buff *destroy_skb;
	struct kref kref;
	u32 stid;
	u32 first_pdu_recv_timeout;
	u8 protocol;
	u8 clip_release:1;
	u8 listen:1;
	u8 app_close:1;
};

#define CSTOR_PASSIVE_SOCK XA_MARK_0
#define CSTOR_ACTIVE_SOCK XA_MARK_1

struct cstor_sock {
	struct cstor_ucontext *uctx;
	struct cstor_event_channel *event_channel;
	struct cstor_listen_sock *lcsk;
	struct cstor_qp *qp;
	struct l2t_entry *l2t;
	struct dst_entry *dst;
	struct sk_buff *wr_pending_head;
	struct sk_buff *wr_pending_tail;
	struct sk_buff *dskb;
	struct sk_buff_head skb_list;
	struct sockaddr_storage laddr;
	struct sockaddr_storage raddr;
	struct cstor_wr_wait wr_wait;
	struct timer_list timer;
	struct list_head uevt_list;
	struct mutex mutex;
	struct kref kref;
	enum cstor_sock_state state;
	unsigned long flags;
	unsigned long history;
	int snd_win;
	int rcv_win;
	u32 atid;
	u32 tid;
	u32 snd_wscale;
	u32 snd_nxt;
	u32 rcv_nxt;
	u32 wr_cred;
	u32 wr_max_cred;
	u32 first_pdu_recv_timeout;
	u32 smac_idx;
	u32 tx_chan;
	u32 rx_chan;
	u32 tpt_idx;
	u32 mtu;
	u32 abort_neg_adv;
	u16 emss;
	u16 rss_qid;
	u16 txq_idx;
	u16 ctrlq_idx;
	u8 tos;
	u8 port_id;
	u8 protocol;
#ifdef CONFIG_CHELSIO_T4_DCB
	u8 dcb_priority;
#endif
};

static inline int compute_wscale(int win)
{
	int wscale = 0;

	while ((wscale < 14) && ((65535 << wscale) < win))
		wscale++;

	return wscale;
}

extern unsigned int max_mr;
extern unsigned int max_ddp_tag;
extern unsigned int max_ddp_sge;
extern unsigned int max_rt;
extern bool enable_wc;
extern char *states[];

typedef void (*cstor_handler_func)(struct cstor_device *dev, struct sk_buff *skb);
extern cstor_handler_func cstor_handlers[NUM_CPL_CMDS];
void process_work(struct work_struct *work);
void sched(struct cstor_device *dev, struct sk_buff *skb);

int cstor_init_resource(struct cstor_device *cdev, u32 nr_tpt);
void cstor_destroy_resource(struct cstor_device *cdev);
int cstor_pblpool_create(struct cstor_device *cdev);
u32 cstor_pblpool_alloc(struct cstor_device *cdev, int size);
void cstor_pblpool_free(struct cstor_device *cdev, u32 addr, int size);
void cstor_pblpool_destroy(struct cstor_device *cdev);

int cstor_open(struct inode *inode, struct file *file);
int cstor_release(struct inode *inode, struct file *file);
int cstor_mmap(struct file *file, struct vm_area_struct *vma);
long cstor_ioctl(struct file *file, u32 cmd, unsigned long arg);

int cstor_query_device(struct cstor_ucontext *uctx, void __user *ubuf);
int cstor_alloc_pd(struct cstor_ucontext *uctx, void __user *ubuf);
int cstor_dealloc_pd(struct cstor_ucontext *uctx, void __user *ubuf);
int cstor_reg_mr(struct cstor_ucontext *uctx, void __user *ubuf);
int cstor_dereg_mr(struct cstor_ucontext *uctx, void __user *ubuf);
int cstor_create_cq(struct cstor_ucontext *uctx, void __user *ubuf);
int cstor_destroy_cq(struct cstor_ucontext *uctx, void __user *ubuf);
int cstor_create_srq(struct cstor_ucontext *uctx, void __user *ubuf);
int cstor_destroy_srq(struct cstor_ucontext *uctx, void __user *ubuf);
int cstor_create_rxq(struct cstor_ucontext *uctx, void __user *ubuf);
int cstor_destroy_rxq(struct cstor_ucontext *uctx, void __user *ubuf);
int cstor_create_qp(struct cstor_ucontext *uctx, void __user *ubuf);
int cstor_destroy_qp(struct cstor_ucontext *uctx, void __user *ubuf);
int cstor_create_listen(struct cstor_ucontext *uctx, void __user *ubuf);
int cstor_destroy_listen(struct cstor_ucontext *uctx, void __user *ubuf);
int cstor_sock_accept(struct cstor_ucontext *uctx, void __user *ubuf);
int cstor_sock_reject(struct cstor_ucontext *uctx, void __user *ubuf);
int cstor_free_atid(struct cstor_ucontext *uctx, void __user *ubuf);
int cstor_sock_attach_qp(struct cstor_ucontext *uctx, void __user *ubuf);
int cstor_resolve_route(struct cstor_ucontext *uctx, void __user *ubuf);
int cstor_connect(struct cstor_ucontext *uctx, void __user *ubuf);
int _cstor_sock_disconnect(struct cstor_ucontext *uctx, void __user *ubuf);
int cstor_sock_release(struct cstor_ucontext *uctx, void __user *ubuf);
int cstor_get_event(struct cstor_ucontext *uctx, void __user *ubuf);
int cstor_create_event_channel(struct cstor_ucontext *uctx, void __user *ubuf);
int cstor_destroy_event_channel(struct cstor_ucontext *uctx, void __user *ubuf);
int cstor_find_device(struct cstor_ucontext *uctx, void __user *ubuf);
int cstor_set_iscsi_region_status(struct cstor_ucontext *ucontext, void __user *ubuf);
int cstor_enable_iscsi_digest(struct cstor_ucontext *uctx, void __user *ubuf);
int cstor_invalidate_iscsi_tag(struct cstor_ucontext *uctx, void __user *ubuf);
int cstor_send_iscsi_pdu(struct cstor_ucontext *uctx, void __user *ubuf);

struct cstor_ucontext *cstor_alloc_ucontext(struct cstor_device *cdev);
int cstor_dealloc_ucontext(struct cstor_ucontext *uctx);
void setup_debugfs(struct cstor_device *cdev);
void setup_uctx_debugfs(struct cstor_ucontext *uctx);

void __cstor_dealloc_pd(struct cstor_pd *pd);
int __cstor_dereg_mr(struct cstor_mr *mr);
int __cstor_destroy_cq(struct cstor_cq *cq, bool reset_cq_ctx);
int cstor_disable_cq(struct cstor_cq *cq);
int __cstor_destroy_srq(struct cstor_srq *srq);
int __cstor_destroy_rxq(struct cstor_rxq *rxq);
int __cstor_destroy_qp(struct cstor_qp *qp);
void _cstor_free_listen_sock(struct kref *kref);
void _cstor_free_sock(struct kref *kref);

int cstor_modify_qp(struct cstor_qp *qp, enum cstor_qp_state state);
int cstor_reset_tpte(struct cstor_qp *qp);
int cstor_disable_rxq(struct cstor_rxq *rxq);
void cstor_destroy_atids(struct cstor_ucontext *uctx);
int cstor_destroy_all_listen(struct cstor_ucontext *uctx);
int cstor_sock_disconnect(struct cstor_sock *csk);
void cstor_disconnect_all_sock(struct cstor_ucontext *uctx);
int
cstor_find_iscsi_page_size_idx(struct cstor_device *cdev, u32 ddp_page_size, u32 *psz_idx);
int
cstor_get_db_gts_phys_addr(struct cstor_device *cdev, u32 qid, enum t4_bar2_qtype qtype,
			   u32 *pbar2_qid, u64 *db_gts_pa);

void
add_to_uevt_list(struct cstor_event_channel *event_channel, struct cstor_uevent_node *uevt_node);
int cstor_ev_handler(struct cstor_device *cdev, u32 qid, u32 pidx);
void cstor_ev_dispatch(struct cstor_device *cdev, void *err_cqe);
void cstor_free_uevent_node(struct cstor_uevent_node *uevt_node);
void _cstor_free_event_channel(struct kref *kref);
#endif
