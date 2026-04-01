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
/* Crude resource management */
#include <linux/spinlock.h>
#include <linux/genalloc.h>
#include "cstor.h"

static int tcp_port_start = 40000;
module_param(tcp_port_start, int, 0644);
MODULE_PARM_DESC(tcp_port_start, "Starting value for TCP Port (default=40000");

static int tcp_port_max = 65536;
module_param(tcp_port_max, int, 0644);
MODULE_PARM_DESC(tcp_port_max, "Max value For TCP Port (default=65536)");

/* nr_* must be power of 2 */
int cstor_init_resource(struct cstor_device *cdev, u32 nr_tpt)
{
	u32 nr_qp_tag;
	int ret = 0;

	ret = cxgb4_uld_alloc_id_table(&cdev->resource.tpt_table, 0, max_mr, 1,
				       CXGB4_ID_TABLE_F_RANDOM);
	if (ret) {
		cstor_err(cdev, "failed to allocate tpt_table\n");
		goto err1;
	}

	nr_qp_tag = (nr_tpt - max_mr) / max_ddp_tag;

	cstor_info(cdev, "nr_tpt %u, max_mr %u, max_ddp_tag %u nr_qp_tag %u\n",
		   nr_tpt, max_mr, max_ddp_tag, nr_qp_tag);

	ret = cxgb4_uld_alloc_id_table(&cdev->resource.qp_tag_table, 0, nr_qp_tag, 1,
				       CXGB4_ID_TABLE_F_RANDOM);
	if (ret) {
		cstor_err(cdev, "failed to allocate qp_tag_table\n");
		goto err2;
	}

	ret = cxgb4_uld_alloc_id_table(&cdev->resource.tcp_port_table, tcp_port_start,
				       tcp_port_max, 0, 0);
	if (ret) {
		cstor_err(cdev, "failed to allocate tcp_port_table\n");
		goto err3;
	}

	cdev->rdma_res = cxgb4_uld_init_rdma_resource(&cdev->lldi);
	if (!cdev->rdma_res)
		goto err4;

	return 0;
 err4:
	cxgb4_uld_free_id_table(&cdev->resource.tcp_port_table);
 err3:
	cxgb4_uld_free_id_table(&cdev->resource.qp_tag_table);
 err2:
	cxgb4_uld_free_id_table(&cdev->resource.tpt_table);
 err1:
	return -ENOMEM;
}

void cstor_destroy_resource(struct cstor_device *cdev)
{
	cxgb4_uld_free_id_table(&cdev->resource.tcp_port_table);
	cxgb4_uld_free_id_table(&cdev->resource.qp_tag_table);
	cxgb4_uld_free_id_table(&cdev->resource.tpt_table);
	cxgb4_uld_destroy_rdma_resource(cdev->rdma_res);
}

/*
 * PBL Memory Manager.  Uses Linux generic allocator.
 */

#define MIN_PBL_SHIFT 5			/* 32B == min PBL size (4 entries) */

u32 cstor_pblpool_alloc(struct cstor_device *cdev, int size)
{
	unsigned long addr = gen_pool_alloc(cdev->pbl_pool, size);

	cstor_debug(cdev, "addr %#x size %d\n", (u32)addr, size);
	mutex_lock(&cdev->stats.lock);
	if (addr) {
		cdev->stats.pbl.cur += roundup(size, 1 << MIN_PBL_SHIFT);
		if (cdev->stats.pbl.cur > cdev->stats.pbl.max)
			cdev->stats.pbl.max = cdev->stats.pbl.cur;
	} else {
		cdev->stats.pbl.fail++;
	}
	mutex_unlock(&cdev->stats.lock);

	return (u32)addr;
}

void cstor_pblpool_free(struct cstor_device *cdev, u32 addr, int size)
{
	cstor_debug(cdev, "addr %#x size %d\n", addr, size);

	mutex_lock(&cdev->stats.lock);
	cdev->stats.pbl.cur -= roundup(size, 1 << MIN_PBL_SHIFT);
	mutex_unlock(&cdev->stats.lock);

	gen_pool_free(cdev->pbl_pool, (unsigned long)addr, size);
}

int cstor_pblpool_create(struct cstor_device *cdev)
{
	u32 pbl_start, pbl_chunk, pbl_top;

	cdev->pbl_pool = gen_pool_create(MIN_PBL_SHIFT, -1);
	if (!cdev->pbl_pool)
		return -ENOMEM;

	pbl_start = cdev->lldi.vr->stor_pbl.start;
	pbl_chunk = cdev->lldi.vr->stor_pbl.size;
	pbl_top = pbl_start + pbl_chunk;

	while (pbl_start < pbl_top) {
		pbl_chunk = min(pbl_top - pbl_start + 1, pbl_chunk);
		if (gen_pool_add(cdev->pbl_pool, pbl_start, pbl_chunk, -1)) {
			cstor_debug(cdev, "failed to add PBL chunk (%x/%x)\n",
				    pbl_start, pbl_chunk);
			if (pbl_chunk <= 1024 << MIN_PBL_SHIFT) {
				cstor_warn(cdev, "Failed to add all PBL chunks (%x/%x)\n",
					   pbl_start, pbl_top - pbl_start);
				return 0;
			}

			pbl_chunk >>= 1;
		} else {
			cstor_debug(cdev, "added PBL chunk (%x/%x)\n", pbl_start, pbl_chunk);
			pbl_start += pbl_chunk;
		}
	}

	return 0;
}

void cstor_pblpool_destroy(struct cstor_device *cdev)
{
	gen_pool_destroy(cdev->pbl_pool);
}
