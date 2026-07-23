/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * MTD RAID301 Internal Media Format & Data Structure Definitions
 *
 * Copyright (C) 2026
 */

#ifndef __MTD_RAID301_INTERNAL_H__
#define __MTD_RAID301_INTERNAL_H__

#include <config.h>
#include <linux/types.h>
#include <linux/bug.h>
#include <asm/byteorder.h>

/* Magic Identifier Constants (Little-Endian) */
#define RAID301_SECTOR_FOOTER_MAGIC    cpu_to_le32(0x46533352) /* "R3SF" */
#define RAID301_JOURNAL_HEADER_MAGIC   cpu_to_le32(0x484A3352) /* "R3JH" */
#define RAID301_JOURNAL_RECORD_MAGIC   cpu_to_le32(0x524A3352) /* "R3JR" */

#define RAID301_FORMAT_VERSION         cpu_to_le16(1)

/* Sector Roles */
#define RAID301_ROLE_DATA              1
#define RAID301_ROLE_PARITY            2

/* Sector Flags */
#define RAID301_FLAG_REPAIR_DATA       (1 << 0)
#define RAID301_FLAG_REPAIR_PARITY     (1 << 1)

/* Journal Record Types */
#define RAID301_JOURNAL_TYPE_BEGIN      1
#define RAID301_JOURNAL_TYPE_COMMIT     2
#define RAID301_JOURNAL_TYPE_ABORT      3
#define RAID301_JOURNAL_TYPE_CHECKPOINT 4

#define RAID301_PARITY_DATA_INDEX      0xFFFF

/* -------------------------------------------------------------------------
 * Media Layout Structures (Packed, Little-Endian)
 * ------------------------------------------------------------------------- */

/* Sector Footer (32 Bytes) */
struct raid301_sector_footer {
	__le32 magic;
	__le16 format_version;
	u8     role;               /* DATA (1) or PARITY (2) */
	u8     flags;
	__le32 stripe_id;
	__le16 member_id;
	__le16 data_index;         /* 0xFFFF for parity */
	__le64 generation;         /* Transaction ID */
	__le32 payload_crc32;
	__le32 footer_crc32;
} __packed;

/* Journal Segment Header (Superblock Copy, 256 Bytes) */
struct raid301_journal_header {
	__le32 magic;
	__le16 format_version;
	__le16 header_len;
	u8     volume_uuid[16];
	__le16 member_id;
	__le16 journal_slot;
	__le32 erase_size;
	__le64 segment_sequence;
	__le64 erase_count;
	__le64 txid_high_watermark;
	__le64 total_size;
	__le64 raw_size;
	__le32 member_size;
	__le16 member_count;
	u8     journal_units_per_member;
	u8     journal_copies;
	__le16 parity_offset;
	__le16 parity_stride;
	__le16 sector_footer_size;
	__le16 journal_record_size;
	__le32 layout_hash;
	__le32 header_crc32;
	u8     reserved[160];
} __packed;

/* Journal Record (64 Bytes) */
struct raid301_journal_record {
	__le32 magic;
	__le16 format_version;
	u8     type;               /* BEGIN, COMMIT, ABORT, CHECKPOINT */
	u8     flags;
	__le64 transaction_id;
	__le16 stripe_id;
	__le16 data_index;
	__le32 old_data_crc32;
	__le32 new_data_crc32;
	__le32 old_parity_crc32;
	__le32 new_parity_crc32;
	__le64 old_data_generation;
	__le64 old_parity_generation;
	__le64 new_generation;
	__le32 record_crc32;
} __packed;

/* Compile-time structure size checks */
static inline void raid301_check_layout_struct_sizes(void)
{
	BUILD_BUG_ON(sizeof(struct raid301_sector_footer) != 32);
	BUILD_BUG_ON(sizeof(struct raid301_journal_header) != 256);
	BUILD_BUG_ON(sizeof(struct raid301_journal_record) != 64);
}

/* Mapping API Prototypes */
bool raid301_validate_stride(u32 stride, u32 member_count);
u16  raid301_stripe_to_parity_member(u32 stripe_id);
u16  raid301_data_to_phys_member(u32 stripe_id, u16 data_index);
int  raid301_calc_physical_offset(u16 member_id, u32 stripe_id, u64 *out_offset);
int  raid301_validate_backing_mtd(struct mtd_info *master);

/* Low-Level IO & Footer API Prototypes */
int  raid301_raw_read(struct mtd_info *master, u64 offset, u32 len, u8 *buf);
int  raid301_raw_write(struct mtd_info *master, u64 offset, u32 len, const u8 *buf);
int  raid301_raw_erase_unit(struct mtd_info *master, u64 offset);
void raid301_populate_footer(struct raid301_sector_footer *footer,
			     u8 role, u8 flags, u32 stripe_id, u16 member_id,
			     u16 data_index, u64 generation, u32 payload_crc32);
bool raid301_verify_footer(const struct raid301_sector_footer *footer,
			    u8 expected_role, u32 expected_stripe,
			    u16 expected_member, u16 expected_data_idx);
int  raid301_write_sector_ordered(struct mtd_info *master, u64 sector_offset,
				 const u8 *payload, const struct raid301_sector_footer *footer);

/* Step 3: Format & Mount API Prototypes */
int  raid301_format_device(struct mtd_info *master);
int  raid301_scan_superblocks(struct mtd_info *master, u8 *out_uuid, u32 *out_hash);
int  raid301_verify_all_stripes(struct mtd_info *master);

#endif /* __MTD_RAID301_INTERNAL_H__ */
