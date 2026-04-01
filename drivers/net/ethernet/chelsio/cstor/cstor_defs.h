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
#ifndef __CSTOR_DEFS_H__
#define __CSTOR_DEFS_H__

#include "t4_hw.h"
#include "t4_regs.h"
#include "t4_values.h"
#include "t4_msg.h"
#include "t4_tcb.h"
#include "t4fw_ri_api.h"

#define T4_MAX_MR_SIZE (~0ULL)
#define T4_PAGE_SIZE_CAP 0x1fffff000  /* 4KB-4GB */

#define T4_FL_MAX_PAGE_SIZE 65536
#define T4_RQT_ENTRY_SHIFT 6
#define T4_EQ_ENTRY_SIZE 64

#define T4_SQ_NUM_SLOTS 5
#define T4_SQ_NUM_BYTES (T4_EQ_ENTRY_SIZE * T4_SQ_NUM_SLOTS)

#define T4_RQ_NUM_SLOTS 2

static inline pgprot_t t4_pgprot_wc(pgprot_t prot)
{
#if defined(__i386__) || defined(__x86_64__) || defined(CONFIG_PPC64)
	return pgprot_writecombine(prot);
#else
	return pgprot_noncached(prot);
#endif
}

/* CQE/AE status codes */
#define T4_ERR_SUCCESS                     0x0
#define T4_ERR_STAG                        0x1	/* STAG invalid: either the */
						/* STAG is offlimt, being 0, */
						/* or STAG_key mismatch */
#define T4_ERR_PDID                        0x2	/* PDID mismatch */
#define T4_ERR_QPID                        0x3	/* QPID mismatch */
#define T4_ERR_ACCESS                      0x4	/* Invalid access right */
#define T4_ERR_WRAP                        0x5	/* Wrap error */
#define T4_ERR_BOUND                       0x6	/* base and bounds voilation */
#define T4_ERR_INVALIDATE_SHARED_MR        0x7	/* attempt to invalidate a  */
						/* shared memory region */
#define T4_ERR_INVALIDATE_MR_WITH_MW_BOUND 0x8	/* attempt to invalidate a  */
						/* shared memory region */
#define T4_ERR_ECC                         0x9	/* ECC error detected */
#define T4_ERR_ECC_PSTAG                   0xA	/* ECC error detected when  */
						/* reading PSTAG for a MW  */
						/* Invalidate */
#define T4_ERR_PBL_ADDR_BOUND              0xB	/* pbl addr out of bounds:  */
						/* software error */
#define T4_ERR_SWFLUSH                     0xC	/* SW FLUSHED */
#define T4_ERR_CRC                         0x10 /* CRC error */
#define T4_ERR_MARKER                      0x11 /* Marker error */
#define T4_ERR_PDU_LEN_ERR                 0x12 /* invalid PDU length */
#define T4_ERR_OUT_OF_RQE                  0x13 /* out of RQE */
#define T4_ERR_DDP_VERSION                 0x14 /* wrong DDP version */
#define T4_ERR_RDMA_VERSION                0x15 /* wrong RDMA version */
#define T4_ERR_OPCODE                      0x16 /* invalid rdma opcode */
#define T4_ERR_DDP_QUEUE_NUM               0x17 /* invalid ddp queue number */
#define T4_ERR_MSN                         0x18 /* MSN error */
#define T4_ERR_TBIT                        0x19 /* tag bit not set correctly */
#define T4_ERR_MO                          0x1A /* MO not 0 for TERMINATE  */
						/* or READ_REQ */
#define T4_ERR_MSN_GAP                     0x1B
#define T4_ERR_MSN_RANGE                   0x1C
#define T4_ERR_IRD_OVERFLOW                0x1D
#define T4_ERR_RQE_ADDR_BOUND              0x1E /* RQE addr out of bounds:  */
						/* software error */
#define T4_ERR_INTERNAL_ERR                0x1F /* internal error (opcode  */
						/* mismatch) */
struct t4_sq {
	void *queue;
	dma_addr_t dma_addr;
	u64 db_gts_pa;
	size_t memsize;
	u32 bar2_qid;
	u32 qid;
	u32 size;
};

struct t4_rq {
	void *queue;
	dma_addr_t dma_addr;
	u64 db_gts_pa;
	size_t memsize;
	u32 bar2_qid;
	u32 qid;
	u32 rqt_hwaddr;
	u32 rqt_size;
	u32 max_wr;
	u32 size;
};

struct t4_wq {
	u8 *qp_errp;
	struct t4_sq sq;
	struct t4_rq rq;
};

struct t4_srq {
	void *queue;
	dma_addr_t dma_addr;
	u64 db_gts_pa;
	size_t memsize;
	u32 bar2_qid;
	u32 qid;
	u32 rqt_hwaddr;
	u32 rqt_abs_idx;
	u32 rqt_size;
	u32 max_wr;
	u32 size;
};

static inline void t4_set_wq_in_error(struct t4_wq *wq)
{
	*wq->qp_errp = 1;
}

struct t4_cq {
	void *queue;
	dma_addr_t dma_addr;
	void __iomem *gts;
	struct t4_status_page *status;
	u64 db_gts_pa;
	size_t memsize;
	unsigned long flags;
	u32 cqid;
	u32 bar2_qid;
	u32 qid_mask;
	u32 size;
	u16 ciq_idx;
};

#define T4_IQE_LEN 64

struct t4_iq {
	void *desc;
	dma_addr_t dma_addr;
	size_t memsize;
	u32 cntxt_id;
	u32 abs_id;
	u32 size;
};

struct t4_fl {
	void *desc;
	dma_addr_t dma_addr;
	size_t memsize;
	u32 cntxt_id;
	u32 size;
	u32 fl_align;
	u8 packed;
};

/*
 * sge_opaque_hdr -
 * Opaque version of structure the SGE stores at skb->head of TX_DATA packets
 * and for which we must reserve space.
 */
struct sge_opaque_hdr {
        void *dev;
        dma_addr_t addr[MAX_SKB_FRAGS + 1];
};

#endif
