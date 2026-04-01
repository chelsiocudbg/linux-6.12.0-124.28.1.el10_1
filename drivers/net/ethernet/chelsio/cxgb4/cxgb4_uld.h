/*
 * This file is part of the Chelsio T4 Ethernet driver for Linux.
 *
 * Copyright (c) 2003-2016 Chelsio Communications, Inc. All rights reserved.
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

#ifndef __CXGB4_ULD_H
#define __CXGB4_ULD_H

#include <linux/cache.h>
#include <linux/spinlock.h>
#include <linux/skbuff.h>
#include <linux/inetdevice.h>
#include <linux/atomic.h>
#include <net/tls.h>
#include "cxgb4.h"

#define MAX_ULD_QSETS 16
#define MAX_ULD_NPORTS 4

/* ulp_mem_io + ulptx_idata + payload + padding */
#define MAX_IMM_ULPTX_WR_LEN (32 + 8 + 256 + 8)


/* CPL message priority levels */
enum {
	CPL_PRIORITY_DATA     = 0,  /* data messages */
	CPL_PRIORITY_SETUP    = 1,  /* connection setup messages */
	CPL_PRIORITY_TEARDOWN = 0,  /* connection teardown messages */
	CPL_PRIORITY_LISTEN   = 1,  /* listen start/stop messages */
	CPL_PRIORITY_ACK      = 1,  /* RX ACK messages */
	CPL_PRIORITY_CONTROL  = 1   /* control messages */
};


/*
 * fw_ri_rw[fw_ri_type_init] + cpl_tx_tnl_lso + cpl_tx_pkt_xt + fw_ri_imm + headers
 * headers = 18B eth & vlan + 40B Outer_IP + 16B ESP + 40B Inner_IP + 8B UDP + 12B BTH
 */
#define MAX_IMM_ROCE_WR_LEN (round_up(80 + 32 + 16 + 8 + 134, 16))

/* fw_nvmet_v2_fr_nsmr_wr + fw_ri_tpte + payload */
#define MAX_IMM_NVMET_V2_FR_NSMR_WR_LEN \
       (sizeof(struct fw_nvmet_v2_fr_nsmr_wr) + 256)

#define INIT_TP_WR(w, tid) do { \
	(w)->wr.wr_hi = htonl(FW_WR_OP_V(FW_TP_WR) | \
			      FW_WR_IMMDLEN_V(sizeof(*w) - sizeof(w->wr))); \
	(w)->wr.wr_mid = htonl(FW_WR_LEN16_V(DIV_ROUND_UP(sizeof(*w), 16)) | \
			       FW_WR_FLOWID_V(tid)); \
	(w)->wr.wr_lo = cpu_to_be64(0); \
} while (0)

#define INIT_TP_WR_CPL(w, cpl, tid) do { \
	INIT_TP_WR(w, tid); \
	OPCODE_TID(w) = htonl(MK_OPCODE_TID(cpl, tid)); \
} while (0)

#define INIT_TP_WR_MIT_CPL(w, cpl, tid) do { \
        INIT_TP_WR(w, tid); \
        OPCODE_TID(w) = htonl(MK_OPCODE_TID(cpl, tid)); \
} while (0)

#define INIT_ULPTX_WR(w, wrlen, atomic, tid) do { \
	(w)->wr.wr_hi = htonl(FW_WR_OP_V(FW_ULPTX_WR) | \
			      FW_WR_ATOMIC_V(atomic)); \
	(w)->wr.wr_mid = htonl(FW_WR_LEN16_V(DIV_ROUND_UP(wrlen, 16)) | \
			       FW_WR_FLOWID_V(tid)); \
	(w)->wr.wr_lo = cpu_to_be64(0); \
} while (0)

/* Special asynchronous notification message */
#define CXGB4_MSG_AN ((void *)1)
#define TX_ULD(uld)(((uld) != CXGB4_ULD_CRYPTO) ? CXGB4_TX_OFLD :\
		      CXGB4_TX_CRYPTO)


struct in6_addr;

#define CXGB4_ULD_SKB_PIDX_OFFSET_S 16
#define CXGB4_ULD_SKB_PIDX_OFFSET_M 0xffff
#define CXGB4_ULD_SKB_PIDX_OFFSET_V(x) ((x) << CXGB4_ULD_SKB_PIDX_OFFSET_S)
#define CXGB4_ULD_SKB_PIDX_OFFSET_G(x) \
       (((x) >> CXGB4_ULD_SKB_PIDX_OFFSET_S) & CXGB4_ULD_SKB_PIDX_OFFSET_M)

#define CXGB4_ULD_SKB_RSVD_S 8
#define CXGB4_ULD_SKB_RSVD_M 0xff
#define CXGB4_ULD_SKB_RSVD_V(x) ((x) << CXGB4_ULD_SKB_RSVD_S)
#define CXGB4_ULD_SKB_RSVD_G(x) \
       (((x) >> CXGB4_ULD_SKB_RSVD_S) & CXGB4_ULD_SKB_RSVD_M)

#define CXGB4_ULD_SKB_PRIO_S 7
#define CXGB4_ULD_SKB_PRIO_M 1
#define CXGB4_ULD_SKB_PRIO_V(x) ((x) << CXGB4_ULD_SKB_PRIO_S)
#define CXGB4_ULD_SKB_PRIO_G(x) \
       (((x) >> CXGB4_ULD_SKB_PRIO_S) & CXGB4_ULD_SKB_PRIO_M)

#define CXGB4_ULD_SKB_CREDITS_S 0
#define CXGB4_ULD_SKB_CREDITS_M 0x7f
#define CXGB4_ULD_SKB_CREDITS_V(x) ((x) << CXGB4_ULD_SKB_CREDITS_S)
#define CXGB4_ULD_SKB_CREDITS_G(x) \
       (((x) >> CXGB4_ULD_SKB_CREDITS_S) & CXGB4_ULD_SKB_CREDITS_M)

static inline void cxgb4_uld_skb_set_pidx_offset(struct sk_buff *skb,
                                                u16 offset)
{
       skb->priority |= CXGB4_ULD_SKB_PIDX_OFFSET_V(offset);
}

static inline u16 cxgb4_uld_skb_get_pidx_offset(const struct sk_buff *skb)
{
       return CXGB4_ULD_SKB_PIDX_OFFSET_G(skb->priority);
}

struct chcr_ktls {
	refcount_t ktls_refcount;
};

struct ch_filter_specification;

static inline void cxgb4_uld_skb_set_prio(struct sk_buff *skb, bool prio)
{
       skb->priority |= CXGB4_ULD_SKB_PRIO_V(prio);
}

static inline bool cxgb4_uld_skb_get_prio(const struct sk_buff *skb)
{
       return CXGB4_ULD_SKB_PRIO_G(skb->priority);
}

static inline void cxgb4_uld_skb_set_credits(struct sk_buff *skb,
                                            u16 credits)
{
       skb->priority |= CXGB4_ULD_SKB_CREDITS_V(credits);
}

static inline u16 cxgb4_uld_skb_get_credits(const struct sk_buff *skb)
{
       return CXGB4_ULD_SKB_CREDITS_G(skb->priority);
}

static inline void cxgb4_uld_skb_set_queue(struct sk_buff *skb, u16 queue)
{
       skb_set_queue_mapping(skb, queue);
}

static inline u16 cxgb4_uld_skb_get_queue(const struct sk_buff *skb)
 {
       return skb_get_queue_mapping(skb);
}

static inline void set_wr_txq(struct sk_buff *skb, bool prio, u16 queue)
{
       cxgb4_uld_skb_set_prio(skb, prio);
       cxgb4_uld_skb_set_queue(skb, queue);
}

enum cxgb4_tx_uld {
	CXGB4_TX_OFLD,
	CXGB4_TX_CRYPTO,
	CXGB4_TX_MAX
};

enum cxgb4_txq_type {
	CXGB4_TXQ_ETH,
	CXGB4_TXQ_ULD,
	CXGB4_TXQ_CTRL,
	CXGB4_TXQ_MAX
};

enum cxgb4_state {
	CXGB4_STATE_UP,
	CXGB4_STATE_START_RECOVERY,
	CXGB4_STATE_DOWN,
	CXGB4_STATE_DETACH,
	CXGB4_STATE_FATAL_ERROR
};

enum cxgb4_control {
	CXGB4_CONTROL_DB_FULL,
	CXGB4_CONTROL_DB_EMPTY,
	CXGB4_CONTROL_DB_DROP,
};

struct adapter;
struct pci_dev;
struct l2t_data;
struct net_device;
struct pkt_gl;
struct tp_tcp_stats;
struct t4_lro_mgr;

//BEGIN--------------- changes from outbox new file cxgb4_uld.h -------------------BEGIN
extern struct mutex uld_mutex;

enum cxgb4_uld_type {
	CXGB4_ULD_RDMA,
	CXGB4_ULD_ISCSI,
	CXGB4_ULD_ISCSIT,
	CXGB4_ULD_TYPE_NVME_TCP_HOST,
	CXGB4_ULD_TYPE_NVME_TCP_TARGET,
	CXGB4_ULD_TYPE_CSTOR,
	CXGB4_ULD_CRYPTO,
	CXGB4_ULD_TYPE_TOE,
	CXGB4_ULD_TYPE_CHTCP,
	CXGB4_ULD_IPSEC,
	CXGB4_ULD_TYPE_TLS,
	CXGB4_ULD_KTLS,
	CXGB4_ULD_TYPE_MAX
};

struct cxgb4_range {
	unsigned int start;
	unsigned int size;
};

struct cxgb4_virt_res {                      /* virtualized HW resources */
	struct cxgb4_range ddp;
	struct cxgb4_range iscsi;
	struct cxgb4_range stag;
	struct cxgb4_range stor_stag;
	struct cxgb4_range rq;
	struct cxgb4_range srq;
	struct cxgb4_range rrq;
	struct cxgb4_range pbl;
	struct cxgb4_range stor_pbl;
	struct cxgb4_range qp;
	struct cxgb4_range cq;
	struct cxgb4_range ocq;
	struct cxgb4_range key;
	struct cxgb4_range ppod_edram;
	struct cxgb4_range sendpath_qp;
	unsigned int ncrypto_fc;
#if IS_ENABLED(CONFIG_CHELSIO_IPSEC_INLINE)
	unsigned int ipsec_max_nic_tunnel;
	unsigned int ipsec_max_nic_transport;
	unsigned int ipsec_max_ofld_conn;
#endif /* CONFIG_CHELSIO_IPSEC_INLINE */
#ifdef CONFIG_PO_FCOE
	unsigned int toe_nppods;
	unsigned int fcoe_nppods;
#endif /* CONFIG_PO_FCOE */
};

struct chcr_stats_debug {
	atomic_t cipher_rqst;
	atomic_t digest_rqst;
	atomic_t aead_rqst;
	atomic_t complete;
	atomic_t error;
	atomic_t fallback;
	atomic_t tls_pdu_tx;
	atomic_t tls_pdu_rx;
	atomic_t tls_key;
};


#if IS_ENABLED(CONFIG_CHELSIO_IPSEC_INLINE)
struct ch_ipsec_stats_debug {
	atomic_t ipsec_cnt;
};
#endif /* CONFIG_CHELSIO_IPSEC_INLINE */


struct tls_stats {
	atomic_t tls_pdu_tx;
	atomic_t tls_pdu_rx;
	atomic_t dtls_pdu_tx;
	atomic_t dtls_pdu_rx;
	atomic_t tls_key;
};

struct cxgb4_uld_stats {
#if IS_ENABLED(CONFIG_CHELSIO_IPSEC_INLINE)
	struct ch_ipsec_stats_debug ipsec;
#endif /* CONFIG_CHELSIO_IPSEC_INLINE */
	struct chcr_stats_debug chcr;
	struct tls_stats tls;
};

#define OCQ_WIN_OFFSET(pdev, vres) \
	(pci_resource_len((pdev), 2) - roundup_pow_of_two((vres)->ocq.size))

struct cxgb4_uld_tid_info {
	struct cxgb4_range tids;
	struct cxgb4_range atids;
	struct cxgb4_range hpftids;
	struct cxgb4_range ftids;
	struct cxgb4_range stids;
};

enum cxgb4_uld_txq_desc {
	CXGB4_ULD_TXQ_DESC_NUM = 1024,
	CXGB4_ULD_TXQ_SENDPATH_DESC_NUM = 256,
};

enum cxgb4_uld_txq_type {
	CXGB4_ULD_TXQ_TYPE_SHARED = 0,
	CXGB4_ULD_TXQ_TYPE_SENDPATH,
	CXGB4_ULD_TXQ_TYPE_MAX,
};

enum cxgb4_uld_txq_info_flags {
	CXGB4_ULD_TXQ_INFO_FLAG_SENDPATH = BIT(0),
};

struct cxgb4_uld_txq_info {
	/* Filled by ULD */
	u32 uld_index; /* ULD index to the queue */
	u32 flags; /* ULD TXQ_INFO_FLAGS */
	u32 iqid; /* ULD Ingress Queue for completions */
	u64 cookie; /* ULD cookie for completions */

	/* Filled by LLD */
	u32 lld_index; /* LLD index to the queue */
	u16 size; /* Maximum queue size */
};

struct cxgb4_uld_txq {
	struct sge_uld_txq *ofldtxq;
	struct cxgb4_uld_txq_info info;
	enum cxgb4_uld_txq_type qtype;
	enum cxgb4_uld_type uld;
	u32 users;
	struct net_device *dev;
	struct work_struct task_txq_free;

	u8 tid_qid_group_id;
	struct list_head tid_qid_group;
};

struct cxgb4_uld_queue_tid_qid_group {
	struct cxgb4_uld_txq *cur_entry;
	struct list_head list_head;
	spinlock_t lock; /* Lock to update cur_entry */
};

struct cxgb4_uld_queue_tid_qid_map {
	u8 ngroups;
	struct cxgb4_uld_queue_tid_qid_group *qid_arr;
};

struct cxgb4_uld_queue_map {
	u32 max_queues;
	u32 num_queues;
	struct xarray queues;
	struct cxgb4_uld_queue_tid_qid_map *tid_qid_map;
};

struct cxgb4_uld_queues_toe {
	struct cxgb4_uld_queue_map shared_txqs;
	struct cxgb4_uld_queue_map txqs;
};

struct cxgb4_uld_queues_rdma {
	struct cxgb4_uld_queue_map txqs;
};

struct cxgb4_uld_queues_iscsi {
	struct cxgb4_uld_queue_map txqs;
};

struct cxgb4_uld_queues_iscsit {
	struct cxgb4_uld_queue_map txqs;
};

struct cxgb4_uld_queues_nvmeh {
	struct cxgb4_uld_queue_map txqs;
};

struct cxgb4_uld_queues_nvmet {
	struct cxgb4_uld_queue_map txqs;
};

struct cxgb4_uld_queues_cstor {
	struct cxgb4_uld_queue_map txqs;
};

struct cxgb4_uld_queues_crypto {
	struct cxgb4_uld_queue_map shared_txqs;
	struct cxgb4_uld_queue_map txqs;
};

struct cxgb4_uld_queues_chtcp {
	struct cxgb4_uld_queue_map txqs;
};

struct cxgb4_uld_queue_info {
	struct cxgb4_uld_queues_toe toeqs;
	struct cxgb4_uld_queues_rdma rdmaqs;
	struct cxgb4_uld_queues_iscsi iscsiqs;
	struct cxgb4_uld_queues_iscsit iscsitqs;
	struct cxgb4_uld_queues_nvmeh nvmehqs;
	struct cxgb4_uld_queues_nvmet nvmetqs;
	struct cxgb4_uld_queues_cstor cstorqs;
	struct cxgb4_uld_queues_crypto cryptoqs;
	struct cxgb4_uld_queues_chtcp chtcpqs;
};

struct cxgb4_uld_sendpath_res {
	struct ida qp_ida;
};

struct cxgb4_uld_resources {
	struct cxgb4_uld_sendpath_res sendpath_res;
};

struct cxgb4_uld {
	struct mutex uld_mutex; /* Used to sync access to ULD data */
	struct cxgb4_uld_queue_info qinfo[NCHAN];
	struct cxgb4_virt_res vres;
	struct cxgb4_uld_resources res;
	void *iscsi_ppm;
	void *cnvme_ddp;
	void *rdma_resource;
	struct gen_pool *ocqp_pool;
	unsigned long oc_mw_pa;
	void __iomem *oc_mw_kva;
	struct srq_data *srq;
#ifdef CONFIG_PO_FCOE
	u8 *ppod_map;
	u16 *tid2xid;
	spinlock_t ppod_map_lock;       /* page pod map lock */
#endif /* CONFIG_PO_FCOE */
	struct cxgb4_uld_stats stats;
};
//END--------------- changes from outbox new file cxgb4_uld.h -------------------END

#if IS_ENABLED(CONFIG_CHELSIO_TLS_DEVICE)
struct ch_ktls_port_stats_debug {
	atomic64_t ktls_tx_connection_open;
	atomic64_t ktls_tx_connection_fail;
	atomic64_t ktls_tx_connection_close;
	atomic64_t ktls_tx_encrypted_packets;
	atomic64_t ktls_tx_encrypted_bytes;
	atomic64_t ktls_tx_ctx;
	atomic64_t ktls_tx_ooo;
	atomic64_t ktls_tx_skip_no_sync_data;
	atomic64_t ktls_tx_drop_no_sync_data;
	atomic64_t ktls_tx_drop_bypass_req;
};

struct ch_ktls_stats_debug {
	struct ch_ktls_port_stats_debug ktls_port[MAX_ULD_NPORTS];
	atomic64_t ktls_tx_send_records;
	atomic64_t ktls_tx_end_pkts;
	atomic64_t ktls_tx_start_pkts;
	atomic64_t ktls_tx_middle_pkts;
	atomic64_t ktls_tx_retransmit_pkts;
	atomic64_t ktls_tx_complete_pkts;
	atomic64_t ktls_tx_trimmed_pkts;
	atomic64_t ktls_tx_fallback;
};
#endif

/*
 * Block of information the LLD provides to ULDs attaching to a device.
 */
struct cxgb4_lld_info {
	struct device *dev;                  /* associated device */
	struct pci_dev *pdev;                /* associated PCI device */
	const char *name;                    /* associated PCI/Platform device name */
	struct l2t_data *l2t;                /* L2 table */
	struct cxgb4_uld_tid_info uld_tids;  /* ULD TID info */
	struct net_device **ports;           /* device ports */
	const struct cxgb4_virt_res *vr;     /* assorted HW resources */
	const unsigned short *mtus;          /* MTU table */
	const unsigned short *rxq_ids;       /* the ULD's Rx queue ids */
	const unsigned short *txq_ids;       /* the ULD's Tx queue ids */
	const unsigned short *ciq_ids;       /* the ULD's concentrator IQ ids */
	unsigned int ctrlq_start;            /* the ULD's control qid start */
	unsigned short nrxq;                 /* # of Rx queues */
	unsigned short ntxq;                 /* # of Tx queues */
	unsigned short nciq;		     /* # of concentrator IQ */
	unsigned char nchan:4;               /* # of channels */
	unsigned char nports:4;              /* # of ports */
	unsigned char wr_cred;               /* WR 16-byte credits */
	unsigned char adapter_type;          /* type of adapter */
	unsigned char fw_api_ver;            /* FW API version */
	unsigned int fw_vers;                /* FW version */
	unsigned int max_pdu_size;           /* max iSCSI and NVMe/TCP pdu size */
	unsigned int iscsi_iolen;            /* iSCSI max I/O length */
	unsigned int cclk_ps;                /* Core clock period in psec */
	unsigned short udb_density;          /* # of user DB/page */
	unsigned short ucq_density;          /* # of user CQs/page */
	unsigned int sge_host_page_size;     /* SGE host page size */
	unsigned short filt_mode;            /* filter optional components */
	unsigned short tx_modq[NCHAN];       /* maps each tx channel to a */
					     /* scheduler queue */
	void __iomem *db_reg;                /* address of kernel doorbell */
	void __iomem *gts_reg;               /* address of GTS register */
	u64 db_gts_pa;                       /* physical address of doorbell and GTS register */
	int dbfifo_int_thresh;		     /* doorbell fifo int threshold */
	unsigned int sge_ingpadboundary;     /* SGE ingress padding boundary */
	unsigned int sge_egrstatuspagesize;  /* SGE egress status page size */
	unsigned int sge_pktshift;           /* Padding between CPL and */
					     /*	packet data */
	unsigned int pf;		     /* Physical Function we're using */
	bool enable_fw_ofld_conn;            /* Enable connection through fw */
					     /* WR */
	unsigned int max_ordird_qp;          /* Max ORD/IRD depth per RDMA QP */
	unsigned int max_ird_adapter;        /* Max IRD memory per adapter */
	bool ulptx_memwrite_dsgl;            /* use of T5 DSGL allowed */
	bool dev_512sgl_mr;                  /* support 512 pbl entries per FR MR*/
	unsigned int iscsi_tagmask;	     /* iscsi ddp tag mask */
	unsigned int iscsi_pgsz_order;	     /* iscsi ddp page size orders */
	unsigned int iscsi_llimit;	     /* chip's iscsi region llimit */
	unsigned int ulp_crypto;	     /* crypto lookaside support */
	void **iscsi_ppm;		     /* iscsi page pod manager */
	void **cnvme_ddp;                    /* NVMe/TCP DDP resource manager */
	void **rdma_resource;                /* rdma resource manager */
	int nodeid;			     /* device numa node id */
	bool fr_nsmr_tpte_wr_support;	     /* FW supports FR_NSMR_TPTE_WR */
	bool write_w_imm_support;            /* FW supports WRITE_WITH_IMMEDIATE */
	bool relaxed_ordering;               /* OK to use PCIe Relaxed Ordering */
	bool write_cmpl_support;             /* FW supports WRITE_CMPL WR */
	unsigned int neq;                    /* Max # of Tx queues supported by FW */
	bool sendpath_enabled;               /* FW supports Tx SENDPATH */
	bool cpl_nvmt_data_iqe;              /* HW delivers CPL_NVMT_DATA in IQE */
	bool cpl_iscsi_data_iqe;             /* HW delivers CPL_ISCSI_DATA in IQE */
	bool iscsi_all_cmp_mode; 	     /* HW delivers CPL_ISCSI_CMP for all Data pdus */
	bool iscsi_non_ddp_bit;  	     /* HW supports non-ddp bit in iSCSI ddp tag */
	bool plat_dev;                	     /* platform device */
	u8 num_up_cores;                     /* # of enabled uP cores */
	unsigned int tid_qid_sel_mask;	     /* TID based QID selection mask */
	unsigned char tid_qid_sel_shift;     /* TID based QID selection shift */
};

struct cxgb4_uld_info {
	char name[IFNAMSIZ];
	void *handle;
	unsigned int nrxq;
	unsigned int rxq_size;
	unsigned int ntxq;
	bool ciq;
	bool lro;
	void *(*add)(const struct cxgb4_lld_info *p);
	int (*rx_handler)(void *handle, const __be64 *rsp,
			  const struct pkt_gl *gl);
	int (*state_change)(void *handle, enum cxgb4_state new_state);
	int (*control)(void *handle, enum cxgb4_control control, ...);
	int (*lro_rx_handler)(void *handle, const __be64 *rsp,
			      const struct pkt_gl *gl,
			      struct t4_lro_mgr *lro_mgr,
			      struct napi_struct *napi);
	void (*lro_flush)(struct t4_lro_mgr *);
	int (*tx_handler)(struct sk_buff *skb, struct net_device *dev);
#if IS_ENABLED(CONFIG_CHELSIO_TLS_DEVICE)
	const struct tlsdev_ops *tlsdev_ops;
#endif
#if IS_ENABLED(CONFIG_XFRM_OFFLOAD)
	const struct xfrmdev_ops *xfrmdev_ops;
	u16 (*xfrm_ipsecidx_get)(struct xfrm_state *xfrm);
	void (*ch_ipsec_show)(struct adapter *adap, struct seq_file *seq);
#endif
};

extern struct cxgb4_uld_info cxgb4_ulds[CXGB4_ULD_TYPE_MAX];
unsigned int cxgb4_modparam_enable_ulds(void);
int uld_attach(struct adapter *adap, unsigned int uld);
void cxgb4_register_uld(enum cxgb4_uld_type type, const struct cxgb4_uld_info *p);
int cxgb4_unregister_uld(enum cxgb4_uld_type type);
void cxgb4_uld_alloc_resources(struct adapter *adap,
			       enum cxgb4_uld_type type, const struct cxgb4_uld_info *p);
bool cxgb4_uld_is_registered(struct adapter *adap, enum cxgb4_uld_type type);
bool cxgb4_modparam_enable_ulds_supported(enum cxgb4_uld_type type);

unsigned int cxgb4_dbfifo_count(const struct net_device *dev, int lpfifo);
unsigned int cxgb4_port_chan(const struct net_device *dev);
u8 cxgb4_port_tx_chan(const struct net_device *dev);

unsigned int cxgb4_port_e2cchan(const struct net_device *dev);
unsigned int cxgb4_port_viid(const struct net_device *dev);
unsigned int cxgb4_port_idx(const struct net_device *dev);
unsigned int cxgb4_best_mtu(const unsigned short *mtus, unsigned short mtu,
			    unsigned int *idx);
unsigned int cxgb4_best_aligned_mtu(const unsigned short *mtus,
				    unsigned short header_size,
				    unsigned short data_size_max,
				    unsigned short data_size_align,
				    unsigned int *mtu_idxp);
void cxgb4_get_tcp_stats(struct pci_dev *pdev, struct tp_tcp_stats *v4,
			 struct tp_tcp_stats *v6);
int cxgb4_wr_mbox(struct net_device *dev, const void *cmd, int size, void *rpl);
int cxgb4_set_params(struct net_device *dev, unsigned int nparams,
		     const u32 *params, const u32 *val);
void cxgb4_iscsi_init(struct net_device *dev, unsigned int tag_mask,
		      const unsigned int *pgsz_order);
struct sk_buff *cxgb4_pktgl_to_skb(const struct pkt_gl *gl,
				   unsigned int skb_len, unsigned int pull_len);
void t4_pktgl_free(const struct pkt_gl *gl);
int cxgb4_flush_eq_cache(struct net_device *dev);
int cxgb4_read_tpte(struct net_device *dev, u32 stag, __be32 *tpte);
int cxgb4_read_pbl_entries(struct net_device *dev, u32 pbl_addr, u64 len, __be32 *pble);
u64 cxgb4_read_sge_timestamp(struct net_device *dev);

enum cxgb4_bar2_qtype { CXGB4_BAR2_QTYPE_EGRESS, CXGB4_BAR2_QTYPE_INGRESS };
int cxgb4_bar2_sge_qregs(struct net_device *dev,
			 unsigned int qid,
			 enum cxgb4_bar2_qtype qtype,
			 int user,
			 u64 *pbar2_qoffset,
			 unsigned int *pbar2_qid);
u16 cxgb4_uld_xfrm_ipsecidx_get(struct xfrm_state *xfrm);
void cxgb4_fatal_err(struct net_device *dev);

struct resource *cxgb4_bar_resource(struct net_device *dev, u8 index);


//BEGIN--------------- changes from outbox new file cxgb4_uld.h -------------------BEGIN
unsigned int cxgb4_uld_atid_in_use(struct net_device *dev);
void *cxgb4_uld_atid_lookup(struct net_device *dev, u32 atid);
int cxgb4_uld_atid_alloc(struct net_device *dev, void *data);
void cxgb4_uld_atid_free(struct net_device *dev, u32 atid);

u32 cxgb4_uld_tid_in_use(struct net_device *dev);
bool cxgb4_uld_tid_out_of_range(struct net_device *dev, u32 tid);
void *cxgb4_uld_tid_lookup(struct net_device *dev, u32 tid);
int cxgb4_uld_tid_insert(struct net_device *dev, u16 family, u32 tid,
                        void *data);
void cxgb4_uld_tid_remove(struct net_device *dev, u16 ctrlq_idx, u16 family, u32 tid);

void *cxgb4_uld_stid_lookup(struct net_device *dev, u32 stid);
int cxgb4_uld_stid_alloc(struct net_device *dev, u16 family, void *data);
int cxgb4_uld_sftid_alloc(struct net_device *dev, u16 family, void *data);
void cxgb4_uld_stid_free(struct net_device *dev, u16 family, u32 stid);

void *cxgb4_uld_uotid_lookup(struct net_device *dev, u32 uotid);
int cxgb4_uld_uotid_alloc(struct net_device *dev, void *data);
void cxgb4_uld_uotid_free(struct net_device *dev, u32 uotid);

int cxgb4_uld_server_create(const struct net_device *dev, unsigned int stid,
                           __be32 sip, __be16 sport, __be16 vlan,
                           unsigned int queue, const u8 *tx_chan);
int cxgb4_uld_server6_create(const struct net_device *dev, unsigned int stid,
                            const struct in6_addr *sip, __be16 sport,
                            __be16 vlan, unsigned int queue,
                            const u8 *tx_chan);
int __cxgb4_uld_server_remove(const struct net_device *dev, unsigned int stid,
                             unsigned int queue, bool ipv6,
                             struct sk_buff *skb);
int cxgb4_uld_server_remove(const struct net_device *dev, unsigned int stid,
                           unsigned int queue, bool ipv6);

int cxgb4_create_server(const struct net_device *dev, unsigned int stid,
                        __be32 sip, __be16 sport, __be16 vlan,
                        unsigned int queue);
int cxgb4_create_server6(const struct net_device *dev, unsigned int stid,
                         const struct in6_addr *sip, __be16 sport,
                         unsigned int queue);
int cxgb4_remove_server(const struct net_device *dev, unsigned int stid,
                         unsigned int queue, bool ipv6);

void cxgb4_uld_tid_qid_sel_update(struct net_device *dev,
				  enum cxgb4_uld_type uld,
				  u32 tid, u16 *qid);

void cxgb4_uld_tid_ctrlq_id_sel_update(struct net_device *dev,
					u32 tid, u16 *ctrlq_index);

bool cxgb4_uld_sendpath_enabled(struct adapter *adap);
void cxgb4_uld_sendpath_qp_free(struct net_device *dev, unsigned int index);
int cxgb4_uld_sendpath_qp_alloc(struct net_device *dev);
struct cxgb4_uld_queue_map *cxgb4_uld_queues_txq_map_get(struct net_device *dev,
                                                        enum cxgb4_uld_txq_type qtype,
                                                        enum cxgb4_uld_type uld);
void cxgb4_uld_txq_purge(struct net_device *dev, enum cxgb4_uld_type uld,
                        struct cxgb4_uld_txq_info *info);
void cxgb4_uld_txq_free(struct net_device *dev, enum cxgb4_uld_type uld,
                       struct cxgb4_uld_txq_info *info);
int cxgb4_uld_txq_alloc(struct net_device *dev, enum cxgb4_uld_type uld,
                       struct cxgb4_uld_txq_info *info);
int cxgb4_ofld_send(struct net_device *dev, struct sk_buff *skb);
int cxgb4_crypto_send(struct net_device *dev, struct sk_buff *skb);
int cxgb4_uld_xmit(struct net_device *dev, struct sk_buff *skb);
int cxgb4_uld_xmit_direct(struct net_device *dev, bool control,
                         unsigned int index, const void *data,
                         unsigned int len);
void cxgb4_uld_txq_cidx_update(struct net_device *dev, u32 index, u16 cidx);
bool cxgb4_uld_txq_full(struct net_device *dev, unsigned int index);
void cxgb4_uld_txq_all_stop(struct adapter *adap);
void cxgb4_uld_txq_all_start(struct adapter *adap);
void cxgb4_uld_txq_all_disable_dbs(struct adapter *adap);
void cxgb4_uld_txq_all_enable_dbs(struct adapter *adap);
void cxgb4_uld_txq_all_recover(struct adapter *adap);
int cxgb4_uld_txq_sync_pidx(struct net_device *dev, u16 qid, u16 pidx,
                           u16 size);
struct cxgb4_uld_txq *cxgb4_uld_txq_get_by_qid(struct net_device *dev,
                                              enum cxgb4_uld_type uld,
                                              u32 qid);
int cxgb4_uld_txq_get_desc(struct adapter *adap, enum cxgb4_uld_type uld,
                          u32 qid, void *data, u32 off, u32 len);
void cxgb4_uld_txq_free_shared(struct adapter *adap, enum cxgb4_uld_type uld);
void cxgb4_uld_txq_alloc_shared(struct adapter *adap, enum cxgb4_uld_type uld);

void cxgb4_uld_queues_cleanup(struct adapter *adap);
void cxgb4_uld_queues_init(struct adapter *adap);

bool cxgb4_uld_crypto_supported_ulp_tls(const struct net_device *dev);

bool cxgb4_uld_supported_any(struct adapter *adap);
bool cxgb4_uld_supported(struct adapter *adap, enum cxgb4_uld_type uld);
const char *cxgb4_uld_type_to_name(enum cxgb4_uld_type uld);
void cxgb4_uld_cleanup(struct adapter *adap);
int cxgb4_uld_init(struct adapter *adap,
                  const struct fw_caps_config_cmd *caps_cmd);
//END--------------- changes from outbox new file cxgb4_uld.h -------------------END
#endif  /* !__CXGB4_ULD_H */
