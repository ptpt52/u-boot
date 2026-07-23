/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * 30+1 XOR RAID-5 Driver Abstraction Layer for 32MB SPI NOR Flash
 *
 * Copyright (C) 2026
 */

#ifndef __MTD_RAID301_H__
#define __MTD_RAID301_H__

#include <config.h>
#include <linux/types.h>
#include <linux/mtd/mtd.h>
#include <u-boot/crc.h>

#define MTD_RAID301_FLASH_SIZE        (32 * 1024 * 1024)  /* 32MB */
#define MTD_RAID301_BLOCK_SIZE        (1 * 1024 * 1024)   /* 1MB Block */
#define MTD_RAID301_SECTOR_SIZE       (4096)              /* 4KB Sector */
#define MTD_RAID301_SECTORS_PER_BLOCK (MTD_RAID301_BLOCK_SIZE / MTD_RAID301_SECTOR_SIZE) /* 256 */

/* Partition Layout */
#define MTD_RAID301_RAW_BLOCKS        (1)                 /* Block 0 (1MB) as Raw MTD */
#define MTD_RAID301_RAW_SIZE          (MTD_RAID301_RAW_BLOCKS * MTD_RAID301_BLOCK_SIZE)

#define MTD_RAID301_RAID_BLOCKS       (31)                /* Block 1~31 (31MB) as RAID5 */
#define MTD_RAID301_DATA_BLOCKS       (30)                /* 30 Data Blocks */
#define MTD_RAID301_PARITY_BLOCKS     (1)                 /* 1 Parity Block */

/* Sector Layout (4KB = 4092 Data + 4 CRC32) */
#define MTD_RAID301_SECTOR_DATA_SIZE  (4092)
#define MTD_RAID301_SECTOR_CRC_SIZE   (4)

/* Total Logical Size: 30 Blocks * 256 Sectors * 4092 Data Bytes */
#define MTD_RAID301_LOGICAL_SIZE      (MTD_RAID301_DATA_BLOCKS * MTD_RAID301_SECTORS_PER_BLOCK * MTD_RAID301_SECTOR_DATA_SIZE)

struct sector_unit {
	u8  data[MTD_RAID301_SECTOR_DATA_SIZE];
	u32 crc32;
} __packed;

struct mtd_raid301_stats {
	u32 read_ops;
	u32 write_ops;
	u32 erase_ops;
	u32 crc_errors;
	u32 recovered_sectors;
	u32 uncorrectable_errors;
};

struct mtd_raid301_dev {
	struct mtd_info *master;       /* Master 32M SPI NOR MTD device */
	struct mtd_info mtd;          /* Virtual MTD device exposed to U-Boot */
	struct mtd_raid301_stats stats;
	bool initialized;
};

/* API Declarations */
struct mtd_info *mtd_raid301_init(struct mtd_info *master);
void mtd_raid301_cleanup(void);
struct mtd_info *mtd_raid301_get_dev(void);

int mtd_raid301_inject_fault(u32 phys_block, u32 sector_idx);
int mtd_raid301_scrub(void);
void mtd_raid301_dump_info(void);

#endif /* __MTD_RAID301_H__ */
