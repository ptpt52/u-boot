/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * MTD RAID301 Storage Abstraction Layer - Format & Mount Engine
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
#include <mtd.h>
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
	layout_params[0] = cpu_to_le32(1);
	layout_params[1] = cpu_to_le32(0);
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

/* Populate Journal Segment Header (Superblock) */
static void raid301_populate_journal_header(struct raid301_journal_header *hdr,
					     const u8 *volume_uuid, u16 member_id,
					     u16 journal_slot, u64 seq, u64 erase_cnt,
					     u64 txid_watermark)
{
	memset(hdr, 0, sizeof(*hdr));
	hdr->magic = RAID301_JOURNAL_HEADER_MAGIC;
	hdr->format_version = RAID301_FORMAT_VERSION;
	hdr->header_len = cpu_to_le16(sizeof(*hdr));
	memcpy(hdr->volume_uuid, volume_uuid, 16);
	hdr->member_id = cpu_to_le16(member_id);
	hdr->journal_slot = cpu_to_le16(journal_slot);
	hdr->erase_size = cpu_to_le32(CONFIG_MTD_RAID301_ERASE_SIZE_BYTES);
	hdr->segment_sequence = cpu_to_le64(seq);
	hdr->erase_count = cpu_to_le64(erase_cnt);
	hdr->txid_high_watermark = cpu_to_le64(txid_watermark);
	hdr->total_size = cpu_to_le64(CONFIG_MTD_RAID301_TOTAL_SIZE_BYTES);
	hdr->raw_size = cpu_to_le64(CONFIG_MTD_RAID301_RAW_SIZE_BYTES);
	hdr->member_size = cpu_to_le32(CONFIG_MTD_RAID301_MEMBER_SIZE_BYTES);
	hdr->member_count = cpu_to_le16(CONFIG_MTD_RAID301_MEMBER_COUNT);
	hdr->journal_units_per_member = CONFIG_MTD_RAID301_JOURNAL_UNITS_PER_MEMBER;
	hdr->journal_copies = CONFIG_MTD_RAID301_JOURNAL_COPIES;
	hdr->parity_offset = cpu_to_le16(CONFIG_MTD_RAID301_PARITY_OFFSET);
	hdr->parity_stride = cpu_to_le16(CONFIG_MTD_RAID301_PARITY_STRIDE);
	hdr->sector_footer_size = cpu_to_le16(RAID301_SECTOR_FOOTER_SIZE);
	hdr->journal_record_size = cpu_to_le16(RAID301_JOURNAL_RECORD_SIZE);
	hdr->layout_hash = cpu_to_le32(mtd_raid301_calc_layout_hash());

	u32 hcrc = crc32(0, (const u8 *)hdr, 92);
	hdr->header_crc32 = cpu_to_le32(hcrc);
}

/* Format Device */
int raid301_format_device(struct mtd_info *master)
{
	u8 *ff_payload, *parity_payload;
	u8 volume_uuid[16];
	u32 ff_crc, parity_crc;
	struct raid301_sector_footer footer;
	struct raid301_journal_header jhdr;
	int ret;

	ret = raid301_validate_backing_mtd(master);
	if (ret)
		return ret;

	printf("RAID301: Formatting backing device '%s'...\n", master->name);

	/* Generate a deterministic pseudo-UUID based on layout hash */
	u32 hash = mtd_raid301_calc_layout_hash();
	for (int i = 0; i < 16; i++) {
		volume_uuid[i] = (u8)((hash >> ((i % 4) * 8)) ^ (i * 0x31));
	}

	ff_payload = malloc(RAID301_PAYLOAD_SIZE);
	parity_payload = malloc(RAID301_PAYLOAD_SIZE);
	if (!ff_payload || !parity_payload) {
		free(ff_payload);
		free(parity_payload);
		return -ENOMEM;
	}

	memset(ff_payload, 0xFF, RAID301_PAYLOAD_SIZE);
	ff_crc = crc32(0, ff_payload, RAID301_PAYLOAD_SIZE);

	/*
	 * Initial Parity Math Rule:
	 * If D (DATA_MEMBER_COUNT) is even, XOR of D 0xFF payloads is 0x00.
	 * If D is odd, XOR is 0xFF.
	 */
	u8 parity_byte = (RAID301_DATA_MEMBER_COUNT % 2 == 0) ? 0x00 : 0xFF;
	memset(parity_payload, parity_byte, RAID301_PAYLOAD_SIZE);
	parity_crc = crc32(0, parity_payload, RAID301_PAYLOAD_SIZE);

	/* Format Data and Parity Slots for each Stripe */
	for (u32 stripe = 0; stripe < RAID301_STRIPE_COUNT; stripe++) {
		u16 parity_mem = raid301_stripe_to_parity_member(stripe);

		for (u16 m = 0; m < CONFIG_MTD_RAID301_MEMBER_COUNT; m++) {
			u64 phys_off;
			ret = raid301_calc_physical_offset(m, stripe, &phys_off);
			if (ret)
				goto out_free;

			if (m == parity_mem) {
				raid301_populate_footer(&footer, RAID301_ROLE_PARITY, 0,
							stripe, m, RAID301_PARITY_DATA_INDEX,
							1, parity_crc);
				ret = raid301_write_sector_ordered(master, phys_off, parity_payload, &footer);
			} else {
				u16 d_idx = (m < parity_mem) ? m : m - 1;
				raid301_populate_footer(&footer, RAID301_ROLE_DATA, 0,
							stripe, m, d_idx, 1, ff_crc);
				ret = raid301_write_sector_ordered(master, phys_off, ff_payload, &footer);
			}

			if (ret) {
				printf("RAID301 Format Error: Failed writing member %u stripe %u\n", m, stripe);
				goto out_free;
			}
		}
	}

	/* Format Journal Segments with Superblock Headers */
	for (u16 m = 0; m < CONFIG_MTD_RAID301_MEMBER_COUNT; m++) {
		for (u8 j = 0; j < CONFIG_MTD_RAID301_JOURNAL_UNITS_PER_MEMBER; j++) {
			u32 j_stripe = RAID301_STRIPE_COUNT + j;
			u64 j_phys_off;
			ret = raid301_calc_physical_offset(m, j_stripe, &j_phys_off);
			if (ret)
				goto out_free;

			ret = raid301_raw_erase_unit(master, j_phys_off);
			if (ret)
				goto out_free;

			raid301_populate_journal_header(&jhdr, volume_uuid, m, j, 1, 1, 1);
			ret = raid301_raw_write(master, j_phys_off, sizeof(jhdr), (const u8 *)&jhdr);
			if (ret)
				goto out_free;
		}
	}

	printf("RAID301: Format completed successfully.\n");

out_free:
	free(ff_payload);
	free(parity_payload);
	return ret;
}

/* Scan Superblocks during attach */
int raid301_scan_superblocks(struct mtd_info *master, u8 *out_uuid, u32 *out_hash)
{
	struct raid301_journal_header jhdr;
	u32 expected_hash = mtd_raid301_calc_layout_hash();
	int valid_superblocks = 0;
	int ret;

	for (u16 m = 0; m < CONFIG_MTD_RAID301_MEMBER_COUNT; m++) {
		u64 j_phys_off;
		ret = raid301_calc_physical_offset(m, RAID301_STRIPE_COUNT, &j_phys_off);
		if (ret)
			continue;

		ret = raid301_raw_read(master, j_phys_off, sizeof(jhdr), (u8 *)&jhdr);
		if (ret)
			continue;

		if (jhdr.magic != RAID301_JOURNAL_HEADER_MAGIC ||
		    jhdr.format_version != RAID301_FORMAT_VERSION)
			continue;

		u32 calc_hcrc = crc32(0, (const u8 *)&jhdr, 92);
		if (calc_hcrc != le32_to_cpu(jhdr.header_crc32))
			continue;

		if (le32_to_cpu(jhdr.layout_hash) != expected_hash) {
			printf("RAID301 Scan Error: Member %u layout hash mismatch\n", m);
			return -EINVAL;
		}

		if (valid_superblocks == 0) {
			memcpy(out_uuid, jhdr.volume_uuid, 16);
			*out_hash = expected_hash;
		} else if (memcmp(out_uuid, jhdr.volume_uuid, 16) != 0) {
			printf("RAID301 Scan Error: Member %u UUID mismatch\n", m);
			return -EINVAL;
		}

		valid_superblocks++;
	}

	if (valid_superblocks < CONFIG_MTD_RAID301_MEMBER_COUNT - 1) {
		printf("RAID301 Scan Error: Insufficient valid superblocks (%d / %d)\n",
		       valid_superblocks, CONFIG_MTD_RAID301_MEMBER_COUNT);
		return -EINVAL;
	}

	return 0;
}

/* Verify All Stripes Data & Parity Footer Integrity */
int raid301_verify_all_stripes(struct mtd_info *master)
{
	struct raid301_sector_footer footer;
	int ret;

	for (u32 stripe = 0; stripe < RAID301_STRIPE_COUNT; stripe++) {
		u16 parity_mem = raid301_stripe_to_parity_member(stripe);

		for (u16 m = 0; m < CONFIG_MTD_RAID301_MEMBER_COUNT; m++) {
			u64 phys_off;
			ret = raid301_calc_physical_offset(m, stripe, &phys_off);
			if (ret)
				return ret;

			u64 footer_off = phys_off + RAID301_PAYLOAD_SIZE;
			ret = raid301_raw_read(master, footer_off, sizeof(footer), (u8 *)&footer);
			if (ret)
				return ret;

			u8 expected_role = (m == parity_mem) ? RAID301_ROLE_PARITY : RAID301_ROLE_DATA;
			u16 expected_d_idx = (m == parity_mem) ? RAID301_PARITY_DATA_INDEX :
					     ((m < parity_mem) ? m : m - 1);

			if (!raid301_verify_footer(&footer, expected_role, stripe, m, expected_d_idx)) {
				printf("RAID301 Stripe Verification Error at member %u stripe %u\n", m, stripe);
				return -EIO;
			}
		}
	}

	return 0;
}

/* Attach MTD RAID301 Virtual Device */
struct mtd_info *mtd_raid301_attach(const char *backing_mtd_name)
{
	struct mtd_info *master;
	int ret;

	raid301_compile_time_checks();

	if (g_raid301_dev.initialized) {
		printf("RAID301: Device already attached.\n");
		return &g_raid301_dev.mtd;
	}

	if (!backing_mtd_name)
		backing_mtd_name = CONFIG_MTD_RAID301_DEFAULT_MASTER;

	mtd_probe_devices();
	master = get_mtd_device_nm(backing_mtd_name);
	if (IS_ERR_OR_NULL(master)) {
		printf("RAID301 Attach Error: Could not find backing MTD device '%s'\n", backing_mtd_name);
		return NULL;
	}

	ret = raid301_validate_backing_mtd(master);
	if (ret)
		return NULL;

	ret = raid301_scan_superblocks(master, g_raid301_dev.volume_uuid, &g_raid301_dev.layout_hash);
	if (ret) {
		printf("RAID301 Attach Error: Superblock scan failed. Device may need formatting.\n");
		return NULL;
	}

	ret = raid301_verify_all_stripes(master);
	if (ret) {
		printf("RAID301 Attach Error: Stripe verification failed.\n");
		return NULL;
	}

	memset(&g_raid301_dev, 0, sizeof(g_raid301_dev));
	g_raid301_dev.master = master;

	struct mtd_info *mtd = &g_raid301_dev.mtd;
	memset(mtd, 0, sizeof(struct mtd_info));

	mtd->name = "raid301";
	mtd->type = MTD_NORFLASH;
	mtd->flags = MTD_CAP_NORFLASH;
	mtd->size = RAID301_LOGICAL_SIZE;
	mtd->erasesize = RAID301_PAYLOAD_SIZE;
	mtd->writesize = 1;
	mtd->writebufsize = master->writebufsize;

	if (add_mtd_device(mtd)) {
		printf("RAID301 Attach Error: Failed registering virtual MTD device\n");
		return NULL;
	}

	g_raid301_dev.initialized = true;
	printf("RAID301: Successfully attached to backing MTD '%s' (Logical Size: %llu Bytes)\n",
	       master->name, mtd->size);

	return mtd;
}

int mtd_raid301_detach(void)
{
	if (!g_raid301_dev.initialized)
		return 0;

	del_mtd_device(&g_raid301_dev.mtd);
	if (g_raid301_dev.master) {
		put_mtd_device(g_raid301_dev.master);
	}

	memset(&g_raid301_dev, 0, sizeof(g_raid301_dev));
	printf("RAID301: Detached device successfully.\n");
	return 0;
}

struct mtd_info *mtd_raid301_get_dev(void)
{
	raid301_compile_time_checks();
	if (g_raid301_dev.initialized)
		return &g_raid301_dev.mtd;
	return NULL;
}
