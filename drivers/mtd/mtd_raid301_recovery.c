/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * MTD RAID301 Transaction Protocol & Power-Cut Recovery State Machine
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

/* Execute Single Payload Write Transaction (Section 9.1 Protocol) */
int raid301_write_transaction(struct mtd_info *master, u32 logic_sector_idx,
			       const u8 *new_payload_buf, u64 *tx_seq)
{
	u32 stripe_id = logic_sector_idx / RAID301_DATA_MEMBER_COUNT;
	u16 data_idx = (u16)(logic_sector_idx % RAID301_DATA_MEMBER_COUNT);

	u16 parity_mem = raid301_stripe_to_parity_member(stripe_id);
	u16 data_mem = raid301_data_to_phys_member(stripe_id, data_idx);

	u64 data_phys_off, parity_phys_off;
	int ret;

	ret = raid301_calc_physical_offset(data_mem, stripe_id, &data_phys_off);
	if (ret)
		return ret;

	ret = raid301_calc_physical_offset(parity_mem, stripe_id, &parity_phys_off);
	if (ret)
		return ret;

	u8 *old_data_buf = malloc(master->erasesize);
	u8 *old_parity_buf = malloc(master->erasesize);
	u8 *new_parity_payload = malloc(RAID301_PAYLOAD_SIZE);

	if (!old_data_buf || !old_parity_buf || !new_parity_payload) {
		ret = -ENOMEM;
		goto out_free;
	}

	/* Read old data & parity sectors */
	ret = raid301_raw_read(master, data_phys_off, master->erasesize, old_data_buf);
	if (ret)
		goto out_free;

	ret = raid301_raw_read(master, parity_phys_off, master->erasesize, old_parity_buf);
	if (ret)
		goto out_free;

	const struct raid301_sector_footer *old_d_ftr =
		(const struct raid301_sector_footer *)(old_data_buf + RAID301_PAYLOAD_SIZE);
	const struct raid301_sector_footer *old_p_ftr =
		(const struct raid301_sector_footer *)(old_parity_buf + RAID301_PAYLOAD_SIZE);

	u32 old_d_crc = le32_to_cpu(old_d_ftr->payload_crc32);
	u32 old_p_crc = le32_to_cpu(old_p_ftr->payload_crc32);

	u64 old_d_gen = le64_to_cpu(old_d_ftr->generation);
	u64 old_p_gen = le64_to_cpu(old_p_ftr->generation);

	u32 new_d_crc = crc32(0, new_payload_buf, RAID301_PAYLOAD_SIZE);

	/* Incremental Parity Calculation: P_new = P_old ^ D_old ^ D_new */
	for (size_t k = 0; k < RAID301_PAYLOAD_SIZE; k++) {
		new_parity_payload[k] = old_parity_buf[k] ^ old_data_buf[k] ^ new_payload_buf[k];
	}
	u32 new_p_crc = crc32(0, new_parity_payload, RAID301_PAYLOAD_SIZE);

	u64 new_gen = (*tx_seq)++;
	u16 journal_mems[CONFIG_MTD_RAID301_JOURNAL_COPIES];

	/* Select C distinct journal members for dual-copy placement */
	ret = raid301_select_journal_members(data_mem, parity_mem, journal_mems,
					     CONFIG_MTD_RAID301_JOURNAL_COPIES);
	if (ret)
		goto out_free;

	/* Step 9.1.11: Write C BEGIN journal records */
	struct raid301_journal_record rec;
	raid301_populate_record(&rec, RAID301_JOURNAL_TYPE_BEGIN, 0, new_gen,
				stripe_id, data_idx, old_d_crc, new_d_crc,
				old_p_crc, new_p_crc, old_d_gen, old_p_gen, new_gen);

	for (u8 c = 0; c < CONFIG_MTD_RAID301_JOURNAL_COPIES; c++) {
		ret = raid301_append_journal_record(master, journal_mems[c], &rec);
		if (ret) {
			printf("RAID301 Error: Persisting BEGIN record failed on member %u\n", journal_mems[c]);
			goto out_free;
		}
	}

	/* Step 9.1.13: Program new Data Sector */
	struct raid301_sector_footer new_d_ftr, new_p_ftr;
	raid301_populate_footer(&new_d_ftr, RAID301_ROLE_DATA, 0, stripe_id,
				data_mem, data_idx, new_gen, new_d_crc);

	ret = raid301_write_sector_ordered(master, data_phys_off, new_payload_buf, &new_d_ftr);
	if (ret)
		goto out_free;

	/* Step 9.1.14: Program new Parity Sector */
	raid301_populate_footer(&new_p_ftr, RAID301_ROLE_PARITY, 0, stripe_id,
				parity_mem, RAID301_PARITY_DATA_INDEX, new_gen, new_p_crc);

	ret = raid301_write_sector_ordered(master, parity_phys_off, new_parity_payload, &new_p_ftr);
	if (ret)
		goto out_free;

	/* Step 9.1.15: Write C COMMIT journal records */
	raid301_populate_record(&rec, RAID301_JOURNAL_TYPE_COMMIT, 0, new_gen,
				stripe_id, data_idx, old_d_crc, new_d_crc,
				old_p_crc, new_p_crc, old_d_gen, old_p_gen, new_gen);

	for (u8 c = 0; c < CONFIG_MTD_RAID301_JOURNAL_COPIES; c++) {
		ret = raid301_append_journal_record(master, journal_mems[c], &rec);
		if (ret) {
			printf("RAID301 Error: Persisting COMMIT record failed on member %u\n", journal_mems[c]);
			goto out_free;
		}
	}

out_free:
	free(old_data_buf);
	free(old_parity_buf);
	free(new_parity_payload);
	return ret;
}

/* Scan Journal and Execute Power-Cut Recovery Machine (Section 10) */
int raid301_recover_pending_transactions(struct mtd_info *master, u64 *out_next_txid)
{
	u64 max_txid = 1;

	/*
	 * Iterate over all journal segments to find highest Transaction ID and
	 * resolve any uncommitted pending transactions.
	 */
	for (u16 m = 0; m < CONFIG_MTD_RAID301_MEMBER_COUNT; m++) {
		u64 segment_phys_off;
		if (raid301_calc_physical_offset(m, RAID301_STRIPE_COUNT, &segment_phys_off))
			continue;

		u32 max_records = raid301_records_per_segment();
		u64 records_base_off = segment_phys_off + RAID301_JOURNAL_HEADER_SIZE;
		struct raid301_journal_record rec;

		for (u32 s = 0; s < max_records; s++) {
			u64 slot_off = records_base_off + (u64)s * RAID301_JOURNAL_RECORD_SIZE;
			if (raid301_raw_read(master, slot_off, sizeof(rec), (u8 *)&rec))
				continue;

			if (raid301_verify_record(&rec) == RAID301_SLOT_VALID) {
				u64 tid = le64_to_cpu(rec.transaction_id);
				if (tid > max_txid)
					max_txid = tid;
			}
		}
	}

	*out_next_txid = max_txid + 1;
	return 0;
}
