/*
 * Copyright (c) 2009-2010 Chelsio, Inc. All rights reserved.
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
/* Crude resource management */
#include <linux/spinlock.h>
#include <linux/genalloc.h>
#include <linux/ratelimit.h>
#include "iw_cxgb4.h"

/* nr_* must be power of 2 */
int c4iw_init_resource(struct c4iw_rdev *rdev, u32 nr_tpt)
{
	int err = 0;

	err = cxgb4_uld_alloc_id_table(&rdev->resource.tpt_table, 0, nr_tpt, 1,
				       CXGB4_ID_TABLE_F_RANDOM);
	if (err)
		goto tpt_err;

	rdev->rdma_res = cxgb4_uld_init_rdma_resource(&rdev->lldi);
	if (!rdev->rdma_res)
		goto resource_err;

	return 0;
 resource_err:
	cxgb4_uld_free_id_table(&rdev->resource.tpt_table);
 tpt_err:
	return -ENOMEM;
}

void c4iw_destroy_resource(struct c4iw_rdev *rdev)
{
	cxgb4_uld_free_id_table(&rdev->resource.tpt_table);
	cxgb4_uld_destroy_rdma_resource(rdev->rdma_res);
}

/*
 * PBL Memory Manager.  Uses Linux generic allocator.
 */

#define MIN_PBL_SHIFT 8			/* 256B == min PBL size (32 entries) */

u32 c4iw_pblpool_alloc(struct c4iw_rdev *rdev, int size)
{
	unsigned long addr = gen_pool_alloc(rdev->pbl_pool, size);
	pr_debug("addr 0x%x size %d\n", (u32)addr, size);
	mutex_lock(&rdev->stats.lock);
	if (addr) {
		rdev->stats.pbl.cur += roundup(size, 1 << MIN_PBL_SHIFT);
		if (rdev->stats.pbl.cur > rdev->stats.pbl.max)
			rdev->stats.pbl.max = rdev->stats.pbl.cur;
		kref_get(&rdev->pbl_kref);
	} else
		rdev->stats.pbl.fail++;
	mutex_unlock(&rdev->stats.lock);
	return (u32)addr;
}

static void destroy_pblpool(struct kref *kref)
{
	struct c4iw_rdev *rdev;

	rdev = container_of(kref, struct c4iw_rdev, pbl_kref);
	gen_pool_destroy(rdev->pbl_pool);
	complete(&rdev->pbl_compl);
}

void c4iw_pblpool_free(struct c4iw_rdev *rdev, u32 addr, int size)
{
	pr_debug("addr 0x%x size %d\n", addr, size);
	mutex_lock(&rdev->stats.lock);
	rdev->stats.pbl.cur -= roundup(size, 1 << MIN_PBL_SHIFT);
	mutex_unlock(&rdev->stats.lock);
	gen_pool_free(rdev->pbl_pool, (unsigned long)addr, size);
	kref_put(&rdev->pbl_kref, destroy_pblpool);
}

int c4iw_pblpool_create(struct c4iw_rdev *rdev)
{
	unsigned pbl_start, pbl_chunk, pbl_top;

	rdev->pbl_pool = gen_pool_create(MIN_PBL_SHIFT, -1);
	if (!rdev->pbl_pool)
		return -ENOMEM;

	pbl_start = rdev->lldi.vr->pbl.start;
	pbl_chunk = rdev->lldi.vr->pbl.size;
	pbl_top = pbl_start + pbl_chunk;

	while (pbl_start < pbl_top) {
		pbl_chunk = min(pbl_top - pbl_start + 1, pbl_chunk);
		if (gen_pool_add(rdev->pbl_pool, pbl_start, pbl_chunk, -1)) {
			pr_debug("failed to add PBL chunk (%x/%x)\n",
				 pbl_start, pbl_chunk);
			if (pbl_chunk <= 1024 << MIN_PBL_SHIFT) {
				pr_warn("Failed to add all PBL chunks (%x/%x)\n",
					pbl_start, pbl_top - pbl_start);
				return 0;
			}
			pbl_chunk >>= 1;
		} else {
			pr_debug("added PBL chunk (%x/%x)\n",
				 pbl_start, pbl_chunk);
			pbl_start += pbl_chunk;
		}
	}

	return 0;
}

void c4iw_pblpool_destroy(struct c4iw_rdev *rdev)
{
	kref_put(&rdev->pbl_kref, destroy_pblpool);
}

u32 c4iw_rqtpool_alloc(struct c4iw_rdev *rdev, int size)
{
	u32 addr;

	addr = cxgb4_uld_alloc_rqtpool(rdev->rdma_res, size);
	if (addr)
		kref_get(&rdev->rqt_kref);

	return (u32)addr;
}

static void destroy_rqtpool(struct kref *kref)
{
	struct c4iw_rdev *rdev;

	rdev = container_of(kref, struct c4iw_rdev, rqt_kref);
	complete(&rdev->rqt_compl);
}

void c4iw_rqtpool_destroy(struct c4iw_rdev *rdev)
{
	kref_put(&rdev->rqt_kref, destroy_rqtpool);
}

void c4iw_rqtpool_free(struct c4iw_rdev *rdev, u32 addr, int size)
{
	cxgb4_uld_free_rqtpool(rdev->rdma_res, addr, size);
	c4iw_rqtpool_destroy(rdev);
}

/*
 * On-Chip QP Memory.
 */
#define MIN_OCQP_SHIFT 12      /* 4KB == min ocqp size */

int c4iw_ocqp_pool_create(struct c4iw_rdev *rdev)
{
	unsigned start, chunk, top;

	rdev->ocqp_pool = gen_pool_create(MIN_OCQP_SHIFT, -1);
	if (!rdev->ocqp_pool)
		return -ENOMEM;

	start = rdev->lldi.vr->ocq.start;
	chunk = rdev->lldi.vr->ocq.size;
	top = start + chunk;

	while (start < top) {
		chunk = min(top - start + 1, chunk);
		if (gen_pool_add(rdev->ocqp_pool, start, chunk, -1)) {
			pr_debug("failed to add OCQP chunk (%x/%x)\n",
				 start, chunk);
			if (chunk <= 1024 << MIN_OCQP_SHIFT) {
				pr_warn("Failed to add all OCQP chunks (%x/%x)\n",
					start, top - start);
				return 0;
			}
			chunk >>= 1;
		} else {
			pr_debug("added OCQP chunk (%x/%x)\n",
				 start, chunk);
			start += chunk;
		}
	}
	return 0;
}

void c4iw_ocqp_pool_destroy(struct c4iw_rdev *rdev)
{
	gen_pool_destroy(rdev->ocqp_pool);
}
