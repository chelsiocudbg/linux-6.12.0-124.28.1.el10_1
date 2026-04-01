/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/*
 * Copyright (c) 2007 Cisco Systems.  All rights reserved.
 * Copyright (c) 2020 Intel Corporation.  All rights reserved.
 */

#ifndef IB_UMEM_H
#define IB_UMEM_H

#include <linux/list.h>
#include <linux/scatterlist.h>
#include <linux/workqueue.h>
#include <rdma/ib_verbs.h>
#include "cstor.h"

struct dma_buf_attach_ops;

struct cstor_umem {
	struct cstor_device        *dev;
	struct mm_struct       *owning_mm;
	u64 iova;
	size_t			length;
	unsigned long		address;
	u32 writable : 1;
	u32 is_odp : 1;
	u32 is_dmabuf : 1;
	struct work_struct	work;
	struct sg_append_table sgt_append;
};

/* Returns the offset of the umem start relative to the first page. */
static inline int cstor_umem_offset(struct cstor_umem *umem)
{
	return umem->address & ~PAGE_MASK;
}

static inline unsigned long cstor_umem_dma_offset(struct cstor_umem *umem,
						  unsigned long pgsz)
{
	return (sg_dma_address(umem->sgt_append.sgt.sgl) + cstor_umem_offset(umem)) &
	       (pgsz - 1);
}

static inline size_t cstor_umem_num_dma_blocks(struct cstor_umem *umem,
					       unsigned long pgsz)
{
	return (size_t)((ALIGN(umem->iova + umem->length, pgsz) -
			 ALIGN_DOWN(umem->iova, pgsz))) / pgsz;
}

static inline size_t cstor_umem_num_pages(struct cstor_umem *umem)
{
	return cstor_umem_num_dma_blocks(umem, PAGE_SIZE);
}

static inline void __rdma_umem_block_iter_start(struct ib_block_iter *biter,
						struct cstor_umem *umem,
						unsigned long pgsz)
{
	__rdma_block_iter_start(biter, umem->sgt_append.sgt.sgl,
				umem->sgt_append.sgt.nents, pgsz);
	biter->__sg_advance = cstor_umem_offset(umem) & ~(pgsz - 1);
	biter->__sg_numblocks = cstor_umem_num_dma_blocks(umem, pgsz);
}

static inline bool __rdma_umem_block_iter_next(struct ib_block_iter *biter)
{
	return __rdma_block_iter_next(biter) && biter->__sg_numblocks--;
}

/**
 * rdma_umem_for_each_dma_block - iterate over contiguous DMA blocks of the umem
 * @umem: umem to iterate over
 * @pgsz: Page size to split the list into
 *
 * pgsz must be <= PAGE_SIZE or computed by cstor_umem_find_best_pgsz(). The
 * returned DMA blocks will be aligned to pgsz and span the range:
 * ALIGN_DOWN(umem->address, pgsz) to ALIGN(umem->address + umem->length, pgsz)
 *
 * Performs exactly cstor_umem_num_dma_blocks() iterations.
 */
#define rdma_umem_for_each_dma_block(umem, biter, pgsz)                        \
	for (__rdma_umem_block_iter_start(biter, umem, pgsz);                  \
	     __rdma_umem_block_iter_next(biter);)

struct cstor_umem *cstor_umem_get(struct cstor_device *cdev, unsigned long addr,
				  size_t size, int access);
void cstor_umem_release(struct cstor_umem *umem);
unsigned long cstor_umem_find_best_pgsz(struct cstor_umem *umem,
					unsigned long pgsz_bitmap,
					unsigned long virt);
#endif /* CSTOR_UMEM_H */
