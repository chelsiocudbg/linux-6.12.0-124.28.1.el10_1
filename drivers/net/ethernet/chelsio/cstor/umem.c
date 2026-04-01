/*
 * Copyright (c) 2005 Topspin Communications.  All rights reserved.
 * Copyright (c) 2005 Cisco Systems.  All rights reserved.
 * Copyright (c) 2005 Mellanox Technologies. All rights reserved.
 * Copyright (c) 2020 Intel Corporation. All rights reserved.
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

#include <linux/mm.h>
#include <linux/dma-mapping.h>
#include <linux/sched/signal.h>
#include <linux/sched/mm.h>
#include <linux/export.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#include <linux/count_zeros.h>

#include "umem.h"
#include "cstor_ioctl.h"

static void __cstor_umem_release(struct cstor_device *cdev, struct cstor_umem *umem, int dirty)
{
	struct scatterlist *sg;
	bool make_dirty = umem->writable && dirty;
	unsigned int i;

	if (dirty)
		dma_unmap_sgtable(&cdev->lldi.pdev->dev, &umem->sgt_append.sgt,
				  DMA_BIDIRECTIONAL, 0);

	for_each_sgtable_sg(&umem->sgt_append.sgt, sg, i)
		unpin_user_page_range_dirty_lock(sg_page(sg),
			DIV_ROUND_UP(sg->length, PAGE_SIZE), make_dirty);

	sg_free_append_table(&umem->sgt_append);
}

/**
 * cstor_umem_find_best_pgsz - Find best HW page size to use for this MR
 *
 * @umem: umem struct
 * @pgsz_bitmap: bitmap of HW supported page sizes
 * @virt: IOVA
 *
 * This helper is intended for HW that support multiple page
 * sizes but can do only a single page size in an MR.
 *
 * Returns 0 if the umem requires page sizes not supported by
 * the driver to be mapped. Drivers always supporting PAGE_SIZE
 * or smaller will never see a 0 result.
 */
unsigned long
cstor_umem_find_best_pgsz(struct cstor_umem *umem, unsigned long pgsz_bitmap, unsigned long virt)
{
	struct scatterlist *sg;
	dma_addr_t mask;
	unsigned long va, pgoff;
	int i;

	umem->iova = va = virt;
	/* The best result is the smallest page size that results in the minimum
	 * number of required pages. Compute the largest page size that could
	 * work based on VA address bits that don't change.
	 */
	mask = pgsz_bitmap &
	       GENMASK(BITS_PER_LONG - 1, bits_per((umem->length - 1 + virt) ^ virt));
	/* offset into first SGL */
	pgoff = umem->address & ~PAGE_MASK;

	for_each_sgtable_dma_sg(&umem->sgt_append.sgt, sg, i) {
		/* Walk SGL and reduce max page size if VA/PA bits differ
		 * for any address.
		 */
		mask |= (sg_dma_address(sg) + pgoff) ^ va;
		va += sg_dma_len(sg) - pgoff;
		/* Except for the last entry, the ending iova alignment sets
		 * the maximum possible page size as the low bits of the iova
		 * must be zero when starting the next chunk.
		 */
		if (i != (umem->sgt_append.sgt.nents - 1))
			mask |= va;

		pgoff = 0;
	}

	/* The mask accumulates 1's in each position where the VA and physical
	 * address differ, thus the length of trailing 0 is the largest page
	 * size that can pass the VA through to the physical.
	 */
	if (mask)
		pgsz_bitmap &= GENMASK(count_trailing_zeros(mask), 0);

	return pgsz_bitmap ? rounddown_pow_of_two(pgsz_bitmap) : 0;
}

/**
 * cstor_umem_get - Pin and DMA map userspace memory.
 *
 * @device: IB device to connect UMEM
 * @addr: userspace virtual address to start at
 * @size: length of region to pin
 * @access: IB_ACCESS_xxx flags for memory being pinned
 */
struct cstor_umem *
cstor_umem_get(struct cstor_device *cdev, unsigned long addr, size_t size, int access)
{
	struct cstor_umem *umem;
	struct page **page_list;
	struct mm_struct *mm;
	unsigned long lock_limit;
	unsigned long new_pinned;
	unsigned long cur_base;
	unsigned long dma_attr = 0;
	unsigned long npages;
	unsigned int gup_flags = FOLL_LONGTERM;
	int pinned, ret;

	/*
	 * If the combination of the addr and size requested for this memory
	 * region causes an integer overflow, return error.
	 */
	if (((addr + size) < addr) ||
	    PAGE_ALIGN(addr + size) < (addr + size))
		return ERR_PTR(-EINVAL);

	if (!can_do_mlock())
		return ERR_PTR(-EPERM);

	umem = kzalloc(sizeof(*umem), GFP_KERNEL);
	if (!umem)
		return ERR_PTR(-ENOMEM);

	umem->dev = cdev;
	umem->length     = size;
	umem->address    = addr;
	/*
	 * Drivers should call cstor_umem_find_best_pgsz() to set the iova
	 * correctly.
	 */
	umem->iova = addr;

	if (access & (_CSTOR_ACCESS_LOCAL_WRITE | _CSTOR_ACCESS_REMOTE_WRITE))
		umem->writable = 1;

	umem->owning_mm = mm = current->mm;
	mmgrab(mm);

	page_list = (struct page **)__get_free_page(GFP_KERNEL);
	if (!page_list) {
		ret = -ENOMEM;
		goto umem_kfree;
	}

	npages = cstor_umem_num_pages(umem);
	if ((npages == 0) || (npages > UINT_MAX)) {
		ret = -EINVAL;
		goto out;
	}

	lock_limit = rlimit(RLIMIT_MEMLOCK) >> PAGE_SHIFT;

	new_pinned = atomic64_add_return(npages, &mm->pinned_vm);
	if ((new_pinned > lock_limit) && !capable(CAP_IPC_LOCK)) {
		atomic64_sub(npages, &mm->pinned_vm);
		ret = -ENOMEM;
		goto out;
	}

	cur_base = addr & PAGE_MASK;

	if (umem->writable)
		gup_flags |= FOLL_WRITE;

	while (npages) {
		cond_resched();
		pinned = pin_user_pages_fast(cur_base,
					     min_t(unsigned long, npages,
						   PAGE_SIZE / sizeof(struct page *)),
					     gup_flags, page_list);
		if (pinned < 0) {
			ret = pinned;
			goto umem_release;
		}

		cur_base += pinned * PAGE_SIZE;
		npages -= pinned;
		ret = sg_alloc_append_table_from_pages(&umem->sgt_append, page_list, pinned, 0,
						       pinned << PAGE_SHIFT,
						       dma_get_max_seg_size(&cdev->lldi.pdev->dev),
						       npages, GFP_KERNEL);
		if (ret) {
			unpin_user_pages_dirty_lock(page_list, pinned, 0);
			goto umem_release;
		}
	}

	ret = dma_map_sgtable(&cdev->lldi.pdev->dev, &umem->sgt_append.sgt, DMA_BIDIRECTIONAL,
			      dma_attr);
	if (!ret)
		goto out;

umem_release:
	__cstor_umem_release(cdev, umem, 0);
	atomic64_sub(cstor_umem_num_pages(umem), &mm->pinned_vm);
out:
	free_page((unsigned long)page_list);
umem_kfree:
	if (ret) {
		mmdrop(umem->owning_mm);
		kfree(umem);
	}

	return ret ? ERR_PTR(ret) : umem;
}

/**
 * cstor_umem_release - release memory pinned with cstor_umem_get
 * @umem: umem struct to release
 */
void cstor_umem_release(struct cstor_umem *umem)
{
	if (!umem)
		return;

	__cstor_umem_release(umem->dev, umem, 1);

	atomic64_sub(cstor_umem_num_pages(umem), &umem->owning_mm->pinned_vm);
	mmdrop(umem->owning_mm);
	kfree(umem);
}
