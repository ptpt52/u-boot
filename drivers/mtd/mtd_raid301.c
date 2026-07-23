/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * MTD RAID301 Storage Abstraction Layer - Core Framework
 *
 * Copyright (C) 2026
 */

#include <config.h>
#include <log.h>
#include <malloc.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/string.h>
#include <linux/mtd/mtd.h>
#include <mtd_raid301.h>
#include "mtd_raid301_internal.h"

static struct mtd_raid301_dev g_raid301_dev;

/* Compile-time Layout Validation Checks */
static void raid301_compile_time_checks(void)
{
	raid301_check_layout_struct_sizes();

	BUILD_BUG_ON(CONFIG_MTD_RAID301_MEMBER_SIZE_BYTES % CONFIG_MTD_RAID301_ERASE_SIZE_BYTES != 0);
	BUILD_BUG_ON(CONFIG_MTD_RAID301_MEMBER_COUNT < 2 + CONFIG_MTD_RAID301_JOURNAL_COPIES);
	BUILD_BUG_ON(CONFIG_MTD_RAID301_JOURNAL_COPIES < 2);
	BUILD_BUG_ON(CONFIG_MTD_RAID301_JOURNAL_UNITS_PER_MEMBER < 1);
	BUILD_BUG_ON(CONFIG_MTD_RAID301_JOURNAL_UNITS_PER_MEMBER >= (CONFIG_MTD_RAID301_MEMBER_SIZE_BYTES / CONFIG_MTD_RAID301_ERASE_SIZE_BYTES));
	BUILD_BUG_ON(CONFIG_MTD_RAID301_PARITY_OFFSET >= CONFIG_MTD_RAID301_MEMBER_COUNT);
	BUILD_BUG_ON(CONFIG_MTD_RAID301_TOTAL_SIZE_BYTES != (CONFIG_MTD_RAID301_RAW_SIZE_BYTES + (u64)CONFIG_MTD_RAID301_MEMBER_COUNT * CONFIG_MTD_RAID301_MEMBER_SIZE_BYTES));
	BUILD_BUG_ON(CONFIG_MTD_RAID301_ERASE_SIZE_BYTES != 32768 && CONFIG_MTD_RAID301_ERASE_SIZE_BYTES != 65536);
}

u32 mtd_raid301_calc_layout_hash(void)
{
	u32 layout_params[14];
	layout_params[0] = cpu_to_le32(1); /* format version */
	layout_params[1] = cpu_to_le32(0); /* crc algorithm id */
	layout_params[2] = cpu_to_le32(CONFIG_MTD_RAID301_TOTAL_SIZE_BYTES);
	layout_params[3] = cpu_to_le32(CONFIG_MTD_RAID301_RAW_SIZE_BYTES);
	layout_params[4] = cpu_to_le32(CONFIG_MTD_RAID301_MEMBER_SIZE_BYTES);
	layout_params[5] = cpu_to_le32(CONFIG_MTD_RAID301_MEMBER_COUNT);
	layout_params[6] = cpu_to_le32(CONFIG_MTD_RAID301_ERASE_SIZE_BYTES);
	layout_params[7] = cpu_to_le32(CONFIG_MTD_RAID301_JOURNAL_UNITS_PER_MEMBER);
	layout_params[8] = cpu_to_le32(CONFIG_MTD_RAID301_JOURNAL_COPIES);
	layout_params[9] = cpu_to_le32(CONFIG_MTD_RAID301_PARITY_OFFSET);
	layout_params[10] = cpu_to_le32(CONFIG_MTD_RAID301_PARITY_STRIDE);
	layout_params[11] = cpu_to_le32(RAID301_SECTOR_FOOTER_SIZE);
	layout_params[12] = cpu_to_le32(RAID301_JOURNAL_HEADER_SIZE);
	layout_params[13] = cpu_to_le32(RAID301_JOURNAL_RECORD_SIZE);

	return crc32(0, (const u8 *)layout_params, sizeof(layout_params));
}

struct mtd_info *mtd_raid301_get_dev(void)
{
	raid301_compile_time_checks();
	if (g_raid301_dev.initialized)
		return &g_raid301_dev.mtd;
	return NULL;
}
