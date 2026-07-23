/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * MTD RAID301 Distributed Transaction Journal Engine
 *
 * Copyright (C) 2026
 */

#include <config.h>
#include <log.h>
#include <malloc.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/mtd/mtd.h>
#include <mtd_raid301.h>
#include "mtd_raid301_internal.h"

/* Populate Journal Record Structure */
void raid301_populate_record(struct raid301_journal_record *rec,
			     u8 type, u8 flags, u64 txid, u16 stripe_id,
			     u16 data_index, u32 old_data_crc, u32 new_data_crc,
			     u32 old_parity_crc, u32 new_parity_crc,
			     u64 old_data_gen, u64 old_parity_gen, u64 new_gen)
{
	memset(rec, 0, sizeof(*rec));
	rec->magic = RAID301_JOURNAL_RECORD_MAGIC;
	rec->format_version = RAID301_FORMAT_VERSION;
	rec->type = type;
	rec->flags = flags;
	rec->transaction_id = cpu_to_le64(txid);
	rec->stripe_id = cpu_to_le16(stripe_id);
	rec->data_index = cpu_to_le16(data_index);
	rec->old_data_crc32 = cpu_to_le32(old_data_crc);
	rec->new_data_crc32 = cpu_to_le32(new_data_crc);
	rec->old_parity_crc32 = cpu_to_le32(old_parity_crc);
	rec->new_parity_crc32 = cpu_to_le32(new_parity_crc);
	rec->old_data_generation = cpu_to_le64(old_data_gen);
	rec->old_parity_generation = cpu_to_le64(old_parity_gen);
	rec->new_generation = cpu_to_le64(new_gen);

	u32 rcrc = crc32(0, (const u8 *)rec, 60);
	rec->record_crc32 = cpu_to_le32(rcrc);
}

/* Verify Record Slot Integrity */
enum raid301_slot_status raid301_verify_record(const struct raid301_journal_record *rec)
{
	const u8 *raw = (const u8 *)rec;
	bool is_ff = true;

	for (size_t i = 0; i < sizeof(*rec); i++) {
		if (raw[i] != 0xFF) {
			is_ff = false;
			break;
		}
	}
	if (is_ff)
		return RAID301_SLOT_FREE;

	if (rec->magic != RAID301_JOURNAL_RECORD_MAGIC ||
	    rec->format_version != RAID301_FORMAT_VERSION)
		return RAID301_SLOT_CONSUMED_INVALID;

	u32 calc_rcrc = crc32(0, raw, 60);
	if (calc_rcrc != le32_to_cpu(rec->record_crc32))
		return RAID301_SLOT_CONSUMED_INVALID;

	return RAID301_SLOT_VALID;
}

/* Calculate Number of Records per Journal Segment */
u32 raid301_records_per_segment(void)
{
	u32 erase_size = CONFIG_MTD_RAID301_ERASE_SIZE_BYTES;
	u32 header_size = RAID301_JOURNAL_HEADER_SIZE;
	u32 record_size = RAID301_JOURNAL_RECORD_SIZE;

	if (erase_size <= header_size)
		return 0;

	return (erase_size - header_size) / record_size;
}

/* Dual-Copy Isolation Member Placement Algorithm */
int raid301_select_journal_members(u16 data_member, u16 parity_member,
				   u16 *out_journal_members, u8 count)
{
	u8 selected = 0;

	if (count > CONFIG_MTD_RAID301_JOURNAL_COPIES)
		return -EINVAL;

	for (u16 m = 0; m < CONFIG_MTD_RAID301_MEMBER_COUNT; m++) {
		if (m == data_member || m == parity_member)
			continue;

		out_journal_members[selected++] = m;
		if (selected == count)
			return 0;
	}

	if (selected < count) {
		printf("RAID301 Journal Error: Insufficient available members for placement\n");
		return -ENOSPC;
	}

	return 0;
}

/* Append Record to Journal Segment on Master MTD */
int raid301_append_journal_record(struct mtd_info *master, u16 member_id,
				  const struct raid301_journal_record *rec)
{
	u64 segment_phys_off;
	u32 max_records = raid301_records_per_segment();
	struct raid301_journal_record slot_rec;
	int ret;

	ret = raid301_calc_physical_offset(member_id, RAID301_STRIPE_COUNT, &segment_phys_off);
	if (ret)
		return ret;

	u64 records_base_off = segment_phys_off + RAID301_JOURNAL_HEADER_SIZE;

	/* Scan segment for the first FREE slot */
	u32 target_slot = max_records;
	for (u32 s = 0; s < max_records; s++) {
		u64 slot_off = records_base_off + (u64)s * RAID301_JOURNAL_RECORD_SIZE;
		ret = raid301_raw_read(master, slot_off, sizeof(slot_rec), (u8 *)&slot_rec);
		if (ret)
			return ret;

		enum raid301_slot_status st = raid301_verify_record(&slot_rec);
		if (st == RAID301_SLOT_FREE) {
			target_slot = s;
			break;
		}
	}

	if (target_slot >= max_records) {
		printf("RAID301 Journal Error: Member %u journal segment full\n", member_id);
		return -ENOSPC;
	}

	u64 target_phys_off = records_base_off + (u64)target_slot * RAID301_JOURNAL_RECORD_SIZE;
	ret = raid301_raw_write(master, target_phys_off, sizeof(*rec), (const u8 *)rec);
	if (ret)
		return ret;

	/* Read back and verify */
	ret = raid301_raw_read(master, target_phys_off, sizeof(slot_rec), (u8 *)&slot_rec);
	if (ret)
		return ret;

	if (raid301_verify_record(&slot_rec) != RAID301_SLOT_VALID) {
		printf("RAID301 Journal Error: Readback validation failed after append\n");
		return -EIO;
	}

	return 0;
}

/* Write Checkpoint Record to Maintain High Watermark */
int raid301_write_checkpoint(struct mtd_info *master, u64 high_watermark_txid)
{
	u16 journal_mems[CONFIG_MTD_RAID301_JOURNAL_COPIES];
	int ret;

	ret = raid301_select_journal_members(0, 1, journal_mems, CONFIG_MTD_RAID301_JOURNAL_COPIES);
	if (ret)
		return ret;

	struct raid301_journal_record rec;
	raid301_populate_record(&rec, RAID301_JOURNAL_TYPE_CHECKPOINT, 0,
				high_watermark_txid, 0, 0, 0, 0, 0, 0, 0, 0, high_watermark_txid);

	for (u8 c = 0; c < CONFIG_MTD_RAID301_JOURNAL_COPIES; c++) {
		ret = raid301_append_journal_record(master, journal_mems[c], &rec);
		if (ret) {
			printf("RAID301 Journal Error: Persisting CHECKPOINT failed on member %u\n", journal_mems[c]);
			return ret;
		}
	}
	return 0;
}

/* Journal Garbage Collection & Segment Recycle (Section 11.1) */
int raid301_journal_gc_segment(struct mtd_info *master, u16 member_id,
			      u64 current_seq, u64 current_erase_cnt, u64 txid_watermark)
{
	u64 segment_phys_off;
	int ret;

	ret = raid301_calc_physical_offset(member_id, RAID301_STRIPE_COUNT, &segment_phys_off);
	if (ret)
		return ret;

	/* 1. Erase full journal unit */
	ret = raid301_raw_erase_unit(master, segment_phys_off);
	if (ret)
		return ret;

	/* 2. Write new segment header with incremented sequence and erase count */
	struct raid301_journal_header jhdr;
	u8 volume_uuid[16] = {0};

	u32 hash = mtd_raid301_calc_layout_hash();
	for (int i = 0; i < 16; i++) {
		volume_uuid[i] = (u8)((hash >> ((i % 4) * 8)) ^ (i * 0x31));
	}

	raid301_populate_journal_header(&jhdr, volume_uuid, member_id, 0,
					current_seq + 1, current_erase_cnt + 1, txid_watermark);

	ret = raid301_raw_write(master, segment_phys_off, sizeof(jhdr), (const u8 *)&jhdr);
	if (ret)
		return ret;

	/* 3. Read back and verify header */
	struct raid301_journal_header read_hdr;
	ret = raid301_raw_read(master, segment_phys_off, sizeof(read_hdr), (u8 *)&read_hdr);
	if (ret)
		return ret;

	u32 calc_hcrc = crc32(0, (const u8 *)&read_hdr, 92);
	if (calc_hcrc != le32_to_cpu(read_hdr.header_crc32)) {
		printf("RAID301 GC Error: Readback header CRC check failed after recycle\n");
		return -EIO;
	}

	printf("RAID301 GC: Successfully recycled journal segment on member %u (Erase Count: %llu)\n",
	       member_id, current_erase_cnt + 1);

	return 0;
}
