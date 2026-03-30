/* SPDX-License-Identifier: GPL-2.0-only */
/*
 *  Copyright (C) 2017 Chelsio Communications.  All rights reserved.
 */

#ifndef __CXGB4_CUDBG_H__
#define __CXGB4_CUDBG_H__

#include <asm/cpufeature.h>
#include "cudbg_if.h"
#include "cudbg_lib_common.h"
#include "cudbg_entity.h"
#include "cudbg_lib.h"

#define CUDBG_DUMP_BUFF_SIZE (32 * 1024 * 1024) /* 32 MB */
#define CUDBG_COMPRESS_BUFF_SIZE (4 * 1024 * 1024) /* 4 MB */

typedef int (*cudbg_collect_callback_t)(struct cudbg_init *pdbg_init,
					struct cudbg_buffer *dbg_buff,
					struct cudbg_error *cudbg_err);

struct cxgb4_collect_entity {
	enum cudbg_dbg_entity_type entity;
	cudbg_collect_callback_t collect_cb;
};

enum CXGB4_ETHTOOL_DUMP_FLAGS {
	CXGB4_ETH_DUMP_NONE = ETH_FW_DUMP_DISABLE,
	CXGB4_ETH_DUMP_MEM = (1 << 0), /* On-Chip Memory Dumps */
	CXGB4_ETH_DUMP_HW = (1 << 1), /* various FW and HW dumps */
	CXGB4_ETH_DUMP_FLASH = (1 << 2), /* Dump flash memory */
};

typedef struct {
       u64 a, b, c, d;
} __u256;
typedef __u256 u256, __le256;

static inline int cxgb4_has_avx(void)
{
       return boot_cpu_has(X86_FEATURE_AVX);
}

static inline u256 readqq(const volatile void __iomem *addr)
{
       u256 ret;

       if (!cxgb4_has_avx()) {
               const volatile u64 __iomem *p = addr;

               ret.a = readq(p);
               ret.b = readq(p + 1);
               ret.c = readq(p + 2);
               ret.d = readq(p + 3);

               return ret;
       }

       asm volatile("vmovdqu %0, %%ymm0" :
                    : "m" (*(volatile u256 __force *)addr));
       asm volatile("vmovdqu %%ymm0, %0" : "=m" (ret) : : "memory");
       return ret;
}

static inline u256 le256_to_cpu(__le256 val)
{
       u256 ret;

       ret.a = le64_to_cpu(val.a);
       ret.b = le64_to_cpu(val.b);
       ret.c = le64_to_cpu(val.c);
       ret.d = le64_to_cpu(val.d);

       return ret;
}

#define CXGB4_ETH_DUMP_ALL (CXGB4_ETH_DUMP_MEM | CXGB4_ETH_DUMP_HW)

u32 cxgb4_get_dump_length(struct adapter *adap, u32 flag);
int cxgb4_cudbg_collect(struct adapter *adap, void *buf, u32 *buf_size,
			u32 flag);
void cxgb4_init_ethtool_dump(struct adapter *adapter);
int cxgb4_cudbg_vmcore_add_dump(struct adapter *adap);
#endif /* __CXGB4_CUDBG_H__ */
