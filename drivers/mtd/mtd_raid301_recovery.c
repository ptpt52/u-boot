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

/* Reconstruct Corrupted Sector from the Remaining N - 1 Members (Section 12.1) */
int raid301_reconstruct_sector(struct mtd_info *master, u32 stripe_id,
			       u16 bad_phys_member, u8 *out_payload)
{
	u8 *scratch_buf;
	u64 phys_off;
	int ret = 0;

	scratch_buf = malloc(master->erasesize);
	if (!scratch_buf)
		return -ENOMEM;

	memset(out_payload, 0, RAID301_PAYLOAD_SIZE);

	/* Read & XOR all remaining N - 1 members */
	for (u16 m = 0; m < CONFIG_MTD_RAID301_MEMBER_COUNT; m++) {
		if (m == bad_phys_member)
			continue;

		ret = raid301_calc_physical_offset(m, stripe_id, &phys_off);
		if (ret)
			goto out_free;

		ret = raid301_raw_read(master, phys_off, master->erasesize, scratch_buf);
		if (ret) {
			printf("RAID301 Reconstruction Error: Uncorrectable double fault on member %u\n", m);
			ret = -EIO;
			goto out_free;
		}

		u16 parity_mem = raid301_stripe_to_parity_member(stripe_id);
		u8 expected_role = (m == parity_mem) ? RAID301_ROLE_PARITY : RAID301_ROLE_DATA;
		u16 expected_d_idx = (m == parity_mem) ? RAID301_PARITY_DATA_INDEX :
				     ((m < parity_mem) ? m : m - 1);

		const struct raid301_sector_footer *ftr =
			(const struct raid301_sector_footer *)(scratch_buf + RAID301_PAYLOAD_SIZE);

		if (!raid301_verify_footer(ftr, expected_role, stripe_id, m, expected_d_idx) ||
		    crc32(0, scratch_buf, RAID301_PAYLOAD_SIZE) != le32_to_cpu(ftr->payload_crc32)) {
			printf("RAID301 Reconstruction Error: Second corrupt sector on member %u stripe %u\n",
			       m, stripe_id);
			ret = -EIO;
			goto out_free;
		}

		/* Accumulate XOR */
		for (size_t k = 0; k < RAID301_PAYLOAD_SIZE; k++) {
			out_payload[k] ^= scratch_buf[k];
		}
	}

out_free:
	free(scratch_buf);
	return ret;
}

/* Controlled Self-Healing Rewriting (Section 12.1) */
int raid301_self_heal_sector(struct mtd_info *master, u32 stripe_id,
			     u16 bad_phys_member, const u8 *reconstructed_payload)
{
#if CONFIG_IS_ENABLED(MTD_RAID301_SELF_HEAL)
	u16 parity_mem = raid301_stripe_to_parity_member(stripe_id);
	u8 role = (bad_phys_member == parity_mem) ? RAID301_ROLE_PARITY : RAID301_ROLE_DATA;
	u8 flags = (role == RAID301_ROLE_PARITY) ? RAID301_FLAG_REPAIR_PARITY : RAID301_FLAG_REPAIR_DATA;
	u16 d_idx = (role == RAID301_ROLE_PARITY) ? RAID301_PARITY_DATA_INDEX :
		    ((bad_phys_member < parity_mem) ? bad_phys_member : bad_phys_member - 1);

	u64 phys_off;
	int ret = raid301_calc_physical_offset(bad_phys_member, stripe_id, &phys_off);
	if (ret)
		return ret;

	u32 pcrc = crc32(0, reconstructed_payload, RAID301_PAYLOAD_SIZE);
	struct raid301_sector_footer ftr;
	raid301_populate_footer(&ftr, role, flags, stripe_id, bad_phys_member, d_idx, 999, pcrc);

	printf("RAID301 Self-Heal: Rewriting repaired sector to member %u stripe %u...\n",
	       bad_phys_member, stripe_id);
	return raid301_write_sector_ordered(master, phys_off, reconstructed_payload, &ftr);
#else
	return 0;
#endif
}

/* Global Physical Stripe Integrity Scrubbing (Section 12.2) */
int raid301_scrub_stripes(struct mtd_info *master, bool repair)
{
	u8 *reconstructed;
	int uncorrectable = 0;
	int repaired_cnt = 0;
	int ret;

	reconstructed = malloc(RAID301_PAYLOAD_SIZE);
	if (!reconstructed)
		return -ENOMEM;

	printf("RAID301 Scrub: Starting global stripe integrity scan (%s)...\n",
	       repair ? "repair mode" : "verify-only mode");

	for (u32 stripe = 0; stripe < RAID301_STRIPE_COUNT; stripe++) {
		int bad_count = 0;
		u16 bad_member = 0;

		for (u16 m = 0; m < CONFIG_MTD_RAID301_MEMBER_COUNT; m++) {
			u64 phys_off;
			if (raid301_calc_physical_offset(m, stripe, &phys_off))
				continue;

			u8 *sector_buf = malloc(master->erasesize);
			if (!sector_buf)
				continue;

			if (raid301_raw_read(master, phys_off, master->erasesize, sector_buf) != 0) {
				bad_count++;
				bad_member = m;
			} else {
				u16 parity_mem = raid301_stripe_to_parity_member(stripe);
				u8 expected_role = (m == parity_mem) ? RAID301_ROLE_PARITY : RAID301_ROLE_DATA;
				u16 expected_d_idx = (m == parity_mem) ? RAID301_PARITY_DATA_INDEX :
						     ((m < parity_mem) ? m : m - 1);

				const struct raid301_sector_footer *ftr =
					(const struct raid301_sector_footer *)(sector_buf + RAID301_PAYLOAD_SIZE);

				if (!raid301_verify_footer(ftr, expected_role, stripe, m, expected_d_idx) ||
				    crc32(0, sector_buf, RAID301_PAYLOAD_SIZE) != le32_to_cpu(ftr->payload_crc32)) {
					bad_count++;
					bad_member = m;
				}
			}
			free(sector_buf);
		}

		if (bad_count == 1) {
			printf("RAID301 Scrub: Detected single corruption at stripe %u member %u\n",
			       stripe, bad_member);
			ret = raid301_reconstruct_sector(master, stripe, bad_member, reconstructed);
			if (ret == 0 && repair) {
				ret = raid301_self_heal_sector(master, stripe, bad_member, reconstructed);
				if (ret == 0)
					repaired_cnt++;
			}
		} else if (bad_count > 1) {
			printf("RAID301 Scrub Error: Uncorrectable double fault at stripe %u\n", stripe);
			uncorrectable++;
		}
	}

	free(reconstructed);
	printf("RAID301 Scrub Finished: Repaired: %d, Uncorrectable Stripes: %d\n",
	       repaired_cnt, uncorrectable);

	return (uncorrectable > 0) ? -EIO : 0;
}
