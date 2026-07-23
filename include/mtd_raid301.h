/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * MTD RAID301 Public API and Kconfig Parameters Validation Header
 *
 * Copyright (C) 2026
 */

#ifndef __MTD_RAID301_H__
#define __MTD_RAID301_H__

#include <config.h>
#include <linux/types.h>
#include <linux/mtd/mtd.h>
#include <u-boot/crc.h>

/* Kconfig Configured or Default Parameters */
#ifndef CONFIG_MTD_RAID301_TOTAL_SIZE_BYTES
#define CONFIG_MTD_RAID301_TOTAL_SIZE_BYTES 33554432
#endif

#ifndef CONFIG_MTD_RAID301_RAW_SIZE_BYTES
#define CONFIG_MTD_RAID301_RAW_SIZE_BYTES 1048576
#endif

#ifndef CONFIG_MTD_RAID301_MEMBER_SIZE_BYTES
#define CONFIG_MTD_RAID301_MEMBER_SIZE_BYTES 1048576
#endif

#ifndef CONFIG_MTD_RAID301_MEMBER_COUNT
#define CONFIG_MTD_RAID301_MEMBER_COUNT 31
#endif

#ifndef CONFIG_MTD_RAID301_ERASE_SIZE_BYTES
#define CONFIG_MTD_RAID301_ERASE_SIZE_BYTES 65536
#endif

#ifndef CONFIG_MTD_RAID301_JOURNAL_UNITS_PER_MEMBER
#define CONFIG_MTD_RAID301_JOURNAL_UNITS_PER_MEMBER 1
#endif

#ifndef CONFIG_MTD_RAID301_JOURNAL_COPIES
#define CONFIG_MTD_RAID301_JOURNAL_COPIES 2
#endif

#ifndef CONFIG_MTD_RAID301_PARITY_OFFSET
#define CONFIG_MTD_RAID301_PARITY_OFFSET 0
#endif

#ifndef CONFIG_MTD_RAID301_PARITY_STRIDE
#define CONFIG_MTD_RAID301_PARITY_STRIDE 2
#endif

#ifndef CONFIG_MTD_RAID301_DEFAULT_MASTER
#define CONFIG_MTD_RAID301_DEFAULT_MASTER "raid301-backing"
#endif

/* Derived Constants */
#define RAID301_SECTOR_FOOTER_SIZE     32
#define RAID301_JOURNAL_HEADER_SIZE    256
#define RAID301_JOURNAL_RECORD_SIZE    64

#define RAID301_PAYLOAD_SIZE           (CONFIG_MTD_RAID301_ERASE_SIZE_BYTES - RAID301_SECTOR_FOOTER_SIZE)
#define RAID301_SLOTS_PER_MEMBER       (CONFIG_MTD_RAID301_MEMBER_SIZE_BYTES / CONFIG_MTD_RAID301_ERASE_SIZE_BYTES)
#define RAID301_STRIPE_COUNT           (RAID301_SLOTS_PER_MEMBER - CONFIG_MTD_RAID301_JOURNAL_UNITS_PER_MEMBER)
#define RAID301_DATA_MEMBER_COUNT      (CONFIG_MTD_RAID301_MEMBER_COUNT - 1)

#define RAID301_LOGICAL_SIZE           ((u64)RAID301_DATA_MEMBER_COUNT * RAID301_STRIPE_COUNT * RAID301_PAYLOAD_SIZE)

/* Statistics Tracker */
struct mtd_raid301_stats {
	u32 logical_reads;
	u32 logical_writes;
	u32 logical_erases;
	u32 physical_reads;
	u32 physical_writes;
	u32 physical_erases;
	u32 data_crc_errors;
	u32 parity_crc_errors;
	u32 footer_errors;
	u32 reconstructed_reads;
	u32 successful_repairs;
	u32 failed_repairs;
	u32 uncorrectable_stripes;
	u32 journal_begin_records;
	u32 journal_commit_records;
	u32 journal_abort_records;
	u32 journal_rollbacks;
	u32 journal_rollforwards;
	u32 degraded_member_mask;
};

/* Virtual Device Context */
struct mtd_raid301_dev {
	struct mtd_info *master;       /* Backing MTD Partition */
	struct mtd_info mtd;          /* Exposed Virtual MTD Device */
	struct mtd_raid301_stats stats;
	u8   volume_uuid[16];
	u32  layout_hash;
	bool initialized;
	bool is_read_only;
};

/* API Declarations */
struct mtd_info *mtd_raid301_attach(const char *backing_mtd_name);
int mtd_raid301_detach(void);
struct mtd_info *mtd_raid301_get_dev(void);
u32 mtd_raid301_calc_layout_hash(void);

#endif /* __MTD_RAID301_H__ */
