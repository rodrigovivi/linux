/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2021 Intel Corporation
 */

#ifndef __INTEL_PM_TYPES_H__
#define __INTEL_PM_TYPES_H__

#include <linux/types.h>

enum intel_ddb_partitioning {
	INTEL_DDB_PART_1_2,
	INTEL_DDB_PART_5_6, /* IVB+ */
};

struct skl_ddb_entry {
	u16 start, end;	/* in number of blocks, 'end' is exclusive */
};

static inline u16 skl_ddb_entry_size(const struct skl_ddb_entry *entry)
{
	return entry->end - entry->start;
}

static inline bool skl_ddb_entry_equal(const struct skl_ddb_entry *e1,
				       const struct skl_ddb_entry *e2)
{
	if (e1->start == e2->start && e1->end == e2->end)
		return true;

	return false;
}

#endif /* __INTEL_PM_TYPES_H__ */
