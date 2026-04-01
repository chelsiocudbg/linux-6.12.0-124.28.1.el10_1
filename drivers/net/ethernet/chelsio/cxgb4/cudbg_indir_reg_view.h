#ifndef __CUDBG_INDIR_REG_VIEW_H__
#define __CUDBG_INDIR_REG_VIEW_H__

#define CUDBG_VIEW_INDIR_REG_FIELDS_MAX 32

struct cudbg_view_indir_reg_fields {
	u32 addr;
	u8 len;
	const char *name;
};

struct cudbg_view_indir_reg {
	u32 addr;
	u8 num_fields;
	struct cudbg_view_indir_reg_fields fields[CUDBG_VIEW_INDIR_REG_FIELDS_MAX];
	const char *name;
};

struct cudbg_view_indir_type_entry {
	struct cudbg_view_indir_reg *reg_arr;
	u32 nentries;
};

#include <cudbg_indir_reg_view_t5.h>
#include <cudbg_indir_reg_view_t6.h>
#include <cudbg_indir_reg_view_t7.h>

static inline struct cudbg_view_indir_type_entry *
cudbg_get_view_indir_reg_info(u32 chip_ver, enum cudbg_indir_type type)
{
	switch (chip_ver) {
	case CHELSIO_T5:
		return &t5_view_indir_type_arr[type];
	case CHELSIO_T6:
		return &t6_view_indir_type_arr[type];
	case CHELSIO_T7:
		return &t7_view_indir_type_arr[type];
	}

	return NULL;
}

#endif /* __CUDBG_INDIR_REG_VIEW_H__ */
