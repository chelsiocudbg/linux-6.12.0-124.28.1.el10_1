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
#include <rdma/ib_umem_odp.h>

#include "uverbs.h"

#include <linux/memremap.h>
struct pgmap_umem_entry {
	struct dev_pagemap *pgmap;
	unsigned long first_pfn;
	unsigned long npages;
	struct list_head node;
};

#define ib_umem_pgmap(umem) (!list_empty(&umem->pgmap_list))


static void put_umem_pgmap_list(struct ib_umem *umem)
{
	struct list_head *node, *tmp;
	list_for_each_safe(node, tmp, &umem->pgmap_list) {
                struct pgmap_umem_entry *ent = list_entry(node, struct pgmap_umem_entry, node);
		 put_dev_pagemap(ent->pgmap);
		 list_del(node);
		 kfree(ent);
	}
}

static void __ib_umem_release(struct ib_device *dev, struct ib_umem *umem, int dirty)
{
	bool make_dirty = umem->writable && dirty;
	struct scatterlist *sg;
	unsigned int i;

	if (dirty)
		ib_dma_unmap_sgtable_attrs(dev, &umem->sgt_append.sgt,
					   DMA_BIDIRECTIONAL, 0);

	if (ib_umem_pgmap(umem)) {
		put_umem_pgmap_list(umem);
		goto free_table;
	}

	for_each_sgtable_sg(&umem->sgt_append.sgt, sg, i)
		unpin_user_page_range_dirty_lock(sg_page(sg),
			DIV_ROUND_UP(sg->length, PAGE_SIZE), make_dirty);

free_table:
	sg_free_append_table(&umem->sgt_append);
}

/**
 * ib_umem_find_best_pgsz - Find best HW page size to use for this MR
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
unsigned long ib_umem_find_best_pgsz(struct ib_umem *umem,
				     unsigned long pgsz_bitmap,
				     unsigned long virt)
{
	unsigned long curr_len = 0;
	dma_addr_t curr_base = ~0;
	unsigned long va, pgoff;
	struct scatterlist *sg;
	dma_addr_t mask;
	dma_addr_t end;
	int i;

	umem->iova = va = virt;

	if (umem->is_odp) {
		unsigned int page_size = BIT(to_ib_umem_odp(umem)->page_shift);

		/* ODP must always be self consistent. */
		if (!(pgsz_bitmap & page_size))
			return 0;
		return page_size;
	}

	/* The best result is the smallest page size that results in the minimum
	 * number of required pages. Compute the largest page size that could
	 * work based on VA address bits that don't change.
	 */
	mask = pgsz_bitmap &
	       GENMASK(BITS_PER_LONG - 1,
		       bits_per((umem->length - 1 + virt) ^ virt));
	/* offset into first SGL */
	pgoff = umem->address & ~PAGE_MASK;

	for_each_sgtable_dma_sg(&umem->sgt_append.sgt, sg, i) {
		/* If the current entry is physically contiguous with the previous
		 * one, no need to take its start addresses into consideration.
		 */
		if (check_add_overflow(curr_base, curr_len, &end) ||
		    end != sg_dma_address(sg)) {

			curr_base = sg_dma_address(sg);
			curr_len = 0;

			/* Reduce max page size if VA/PA bits differ */
			mask |= (curr_base + pgoff) ^ va;

			/* The alignment of any VA matching a discontinuity point
			* in the physical memory sets the maximum possible page
			* size as this must be a starting point of a new page that
			* needs to be aligned.
			*/
			if (i != 0)
				mask |= va;
		}

		curr_len += sg_dma_len(sg);
		va += sg_dma_len(sg) - pgoff;

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
EXPORT_SYMBOL(ib_umem_find_best_pgsz);


static unsigned long get_region_single_pgmap(struct ib_umem *umem, unsigned long pg_off)
{
	unsigned long addr = ALIGN_DOWN(umem->address, PAGE_SIZE) + (pg_off << PAGE_SHIFT);
	struct vm_area_struct *vma = find_vma(current->mm, addr);
	struct dev_pagemap *pgmap, *res;
	unsigned long first_pfn;
	unsigned long last_pfn;
	unsigned long region_first_pfn;
	unsigned long region_last_pfn;
	struct pgmap_umem_entry *entry;
	unsigned long npages = ib_umem_num_pages(umem) - pg_off;
	unsigned long vma_pages;

	if (!vma)
		return 0;
	if (!(vma->vm_flags & VM_SHARED))
		return 0;
	if (!(vma->vm_flags & VM_MAYREAD))
		return 0;
	if (umem->writable && !(vma->vm_flags & VM_MAYWRITE))
		return 0;
	if (!vma->vm_ops->get_dev_pgmap)
		return 0;
	vma_pages = (vma->vm_end - addr) >> PAGE_SHIFT;
	if (vma_pages < npages)
		npages = vma_pages;

	pgmap = vma->vm_ops->get_dev_pgmap(vma);
	if (!pgmap)
		return 0;

	if (pgmap_altmap(pgmap))
		first_pfn = pgmap->altmap.base_pfn + pgmap->altmap.reserve + pgmap->altmap.alloc + pgmap->altmap.align;
	else
		first_pfn = (PAGE_ALIGN(pgmap->range.start)) >> PAGE_SHIFT;
	last_pfn =  (pgmap->range.end + 1) >> PAGE_SHIFT;


	region_first_pfn = linear_page_index(vma, addr)  + first_pfn;
	region_last_pfn = linear_page_index(vma, addr + (npages << PAGE_SHIFT))  + first_pfn;
	if (region_first_pfn < first_pfn || region_first_pfn > last_pfn)
		return 0;
	if (region_last_pfn <= first_pfn || region_last_pfn > last_pfn)
		return 0;

	/* bound checking OK */
	res = get_dev_pagemap(first_pfn, NULL);
	if (res == pgmap) {
		entry = kmalloc(sizeof(*entry), GFP_KERNEL);
		if (!entry)
			return 0;
		entry->pgmap = pgmap;
		entry->npages = npages;
		entry->first_pfn = region_first_pfn;
		list_add_tail(&entry->node, &umem->pgmap_list);
	} else {
		put_dev_pagemap(res);
		return 0;
	}
	return npages;
}

static int fast_populate_sglist_with_pgmap(struct ib_umem *umem,struct ib_device *device)
{
	size_t max_segment_pages;
	unsigned long chunks = 0;
	unsigned long npages, done_pages = 0, all_pages;
	struct page *pg;
	struct scatterlist *sg, *next_sg;
	int ret;
	struct list_head *node;

	INIT_LIST_HEAD(&umem->pgmap_list);
	all_pages = ib_umem_num_pages(umem);
	if (all_pages == 0 || all_pages > UINT_MAX)
		return -EINVAL;

	mmap_read_lock(umem->owning_mm);
	max_segment_pages = (dma_get_max_seg_size(device->dma_device)) >> PAGE_SHIFT;
	while (done_pages < all_pages) {
		npages = get_region_single_pgmap(umem, done_pages);
		if (!npages) {
			put_umem_pgmap_list(umem);
			mmap_read_unlock(umem->owning_mm);
			return 0;
		}
		chunks += ((npages + max_segment_pages - 1) / max_segment_pages);
		done_pages += npages;
	}
	ret = sg_alloc_table(&umem->sgt_append.sgt, chunks, GFP_KERNEL);
	if (ret) {
		put_umem_pgmap_list(umem);
                mmap_read_unlock(umem->owning_mm);
		return ret;
	}

	/* OK, we have a shared RW mapping backed by pgmap, no copy on-write or
	 * modified pages. The mapping as it stands now points only to pgmap pages,
	 * regradless of whether page tables are established or not.
	 * The pages cannot be freed as long as pgmap has reference, and we
	 * have grabbed one. We do not care about swap or invalid page tables,
	 * since pages are present and cannot be freed. Similarly, we do not care about
	 * establising page tables or increasing PIN count. The application has already
	 * faulted or will fault later, and this shall take care of the PIN count.
	 * Also, we do not update the page counts, since we do not need additional
	 * page count protection. All we care about is setting up the SG list. The application
	 * may unmap or remap these addresses in the future, but the pages will be
	 * referenced by PGMAP for the entire MR life cycle. The same may happen with normal
	 * slow path of ib_umem_get() - the application may unmap after registration and pages
	 * will be held by MR. The only thing we warrant is that this mapping does not change
	 * during SG list population.
	 */
	sg = next_sg = umem->sgt_append.sgt.sgl;
	list_for_each(node, &umem->pgmap_list) {
		unsigned long i;
		unsigned long ent_chunks;
		struct pgmap_umem_entry *ent =  list_entry(node, struct pgmap_umem_entry, node);
		sg = next_sg;
		pg = pfn_to_page(ent->first_pfn);
		ent_chunks = (ent->npages + max_segment_pages - 1) / max_segment_pages;
		for (i = 0; i < ent_chunks-1; i++) {
			sg_set_page(sg, pg, max_segment_pages << PAGE_SHIFT, 0);
			sg = sg_next(sg);
			pg += max_segment_pages;
		}

		i = ent->npages - (ent_chunks - 1) * max_segment_pages;
		sg_set_page(sg, pg, i << PAGE_SHIFT, 0);
		next_sg = sg_next(sg);
	}
	sg_mark_end(sg);
	umem->sgt_append.total_nents = chunks;
	mmap_read_unlock(umem->owning_mm);
	return 0;
}

/**
 * ib_umem_get - Pin and DMA map userspace memory.
 *
 * @device: IB device to connect UMEM
 * @addr: userspace virtual address to start at
 * @size: length of region to pin
 * @access: IB_ACCESS_xxx flags for memory being pinned
 */
struct ib_umem *ib_umem_get(struct ib_device *device, unsigned long addr,
			    size_t size, int access)
{
	struct ib_umem *umem;
	struct page **page_list = NULL;
	unsigned long lock_limit;
	unsigned long new_pinned;
	unsigned long cur_base;
	unsigned long dma_attr = 0;
	struct mm_struct *mm;
	unsigned long npages;
	int pinned, ret;
	unsigned int gup_flags = FOLL_LONGTERM;

	/*
	 * If the combination of the addr and size requested for this memory
	 * region causes an integer overflow, return error.
	 */
	if (((addr + size) < addr) ||
	    PAGE_ALIGN(addr + size) < (addr + size))
		return ERR_PTR(-EINVAL);

	if (!can_do_mlock())
		return ERR_PTR(-EPERM);

	if (access & IB_ACCESS_ON_DEMAND)
		return ERR_PTR(-EOPNOTSUPP);

	umem = kzalloc(sizeof(*umem), GFP_KERNEL);
	if (!umem)
		return ERR_PTR(-ENOMEM);
	umem->ibdev      = device;
	umem->length     = size;
	umem->address    = addr;
	/*
	 * Drivers should call ib_umem_find_best_pgsz() to set the iova
	 * correctly.
	 */
	umem->iova = addr;
	umem->writable   = ib_access_writable(access);
	umem->owning_mm = mm = current->mm;
	mmgrab(mm);

	ret = fast_populate_sglist_with_pgmap(umem,device);
	if (ret)
		goto umem_kfree;
	if (ib_umem_pgmap(umem))
		goto dma_map;

	page_list = (struct page **) __get_free_page(GFP_KERNEL);
	if (!page_list) {
		ret = -ENOMEM;
		goto umem_kfree;
	}

	npages = ib_umem_num_pages(umem);
	if (npages == 0 || npages > UINT_MAX) {
		ret = -EINVAL;
		goto out;
	}

	lock_limit = rlimit(RLIMIT_MEMLOCK) >> PAGE_SHIFT;

	new_pinned = atomic64_add_return(npages, &mm->pinned_vm);
	if (new_pinned > lock_limit && !capable(CAP_IPC_LOCK)) {
		atomic64_sub(npages, &mm->pinned_vm);
		ret = -ENOMEM;
		goto out;
	}

	cur_base = addr & PAGE_MASK;

	if (umem->writable)
		gup_flags |= FOLL_WRITE;

	while (npages) {
		pinned = pin_user_pages_fast(cur_base,
					  min_t(unsigned long, npages,
						PAGE_SIZE /
						sizeof(struct page *)),
					  gup_flags, page_list);
		if (pinned < 0) {
			ret = pinned;
			goto umem_release;
		}

		cur_base += pinned * PAGE_SIZE;
		npages -= pinned;
		ret = sg_alloc_append_table_from_pages(
			&umem->sgt_append, page_list, pinned, 0,
			pinned << PAGE_SHIFT, ib_dma_max_seg_size(device),
			npages, GFP_KERNEL);
		if (ret) {
			unpin_user_pages_dirty_lock(page_list, pinned, 0);
			goto umem_release;
		}
	}

dma_map:
	if (access & IB_ACCESS_RELAXED_ORDERING)
		dma_attr |= DMA_ATTR_WEAK_ORDERING;

	ret = ib_dma_map_sgtable_attrs(device, &umem->sgt_append.sgt,
				       DMA_BIDIRECTIONAL, dma_attr);
	if (ret)
		goto umem_release;
	goto out;

umem_release:
	__ib_umem_release(device, umem, 0);
        if (!ib_umem_pgmap(umem))
		atomic64_sub(ib_umem_num_pages(umem), &mm->pinned_vm);
out:
	if (page_list)
		free_page((unsigned long) page_list);
umem_kfree:
	if (ret) {
		mmdrop(umem->owning_mm);
		kfree(umem);
	}
	return ret ? ERR_PTR(ret) : umem;
}
EXPORT_SYMBOL(ib_umem_get);

/**
 * ib_umem_release - release memory pinned with ib_umem_get
 * @umem: umem struct to release
 */
void ib_umem_release(struct ib_umem *umem)
{
	if (!umem)
		return;
	if (umem->is_dmabuf)
		return ib_umem_dmabuf_release(to_ib_umem_dmabuf(umem));
	if (umem->is_odp)
		return ib_umem_odp_release(to_ib_umem_odp(umem));

	__ib_umem_release(umem->ibdev, umem, 1);

	if (!ib_umem_pgmap(umem))
		atomic64_sub(ib_umem_num_pages(umem), &umem->owning_mm->pinned_vm);
	mmdrop(umem->owning_mm);
	kfree(umem);
}
EXPORT_SYMBOL(ib_umem_release);

/*
 * Copy from the given ib_umem's pages to the given buffer.
 *
 * umem - the umem to copy from
 * offset - offset to start copying from
 * dst - destination buffer
 * length - buffer length
 *
 * Returns 0 on success, or an error code.
 */
int ib_umem_copy_from(void *dst, struct ib_umem *umem, size_t offset,
		      size_t length)
{
	size_t end = offset + length;
	int ret;

	if (offset > umem->length || length > umem->length - offset) {
		pr_err("%s not in range. offset: %zd umem length: %zd end: %zd\n",
		       __func__, offset, umem->length, end);
		return -EINVAL;
	}

	ret = sg_pcopy_to_buffer(umem->sgt_append.sgt.sgl,
				 umem->sgt_append.sgt.orig_nents, dst, length,
				 offset + ib_umem_offset(umem));

	if (ret < 0)
		return ret;
	else if (ret != length)
		return -EINVAL;
	else
		return 0;
}
EXPORT_SYMBOL(ib_umem_copy_from);
