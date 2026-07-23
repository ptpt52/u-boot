/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * MTD RAID301 Low-Level Physical I/O & Footer Operations
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

/* Low-level Raw MTD Read with strict size & alignment checks */
int raid301_raw_read(struct mtd_info *master, u64 offset, u32 len, u8 *buf)
{
	size_t retlen = 0;
	int ret;

	if (!master || !buf)
		return -EINVAL;

	if (offset + len > master->size)
		return -EOVERFLOW;

	ret = mtd_read(master, (loff_t)offset, len, &retlen, buf);
	if (ret < 0 || retlen != len) {
		printf("RAID301 IO Error: Raw read failed at 0x%llx, len %u (ret: %d, retlen: %zu)\n",
		       offset, len, ret, retlen);
		return -EIO;
	}
	return 0;
}

/* Low-level Raw MTD Write */
int raid301_raw_write(struct mtd_info *master, u64 offset, u32 len, const u8 *buf)
{
	size_t retlen = 0;
	int ret;

	if (!master || !buf)
		return -EINVAL;

	if (offset + len > master->size)
		return -EOVERFLOW;

	ret = mtd_write(master, (loff_t)offset, len, &retlen, buf);
	if (ret < 0 || retlen != len) {
		printf("RAID301 IO Error: Raw write failed at 0x%llx, len %u (ret: %d, retlen: %zu)\n",
		       offset, len, ret, retlen);
		return -EIO;
	}
	return 0;
}

/* Low-level Erase Unit Operation */
int raid301_raw_erase_unit(struct mtd_info *master, u64 offset)
{
	struct erase_info instr;
	int ret;

	if (!master)
		return -EINVAL;

	if (offset + master->erasesize > master->size)
		return -EOVERFLOW;

	memset(&instr, 0, sizeof(instr));
	instr.mtd = master;
	instr.addr = (loff_t)offset;
	instr.len = master->erasesize;

	ret = mtd_erase(master, &instr);
	if (ret != 0 || instr.state == MTD_ERASE_FAILED) {
		printf("RAID301 IO Error: Erase unit failed at 0x%llx\n", offset);
		return -EIO;
	}
	return 0;
}

/* Sector Footer Serialization & CRC Verification */
void raid301_populate_footer(struct raid301_sector_footer *footer,
			     u8 role, u8 flags, u32 stripe_id, u16 member_id,
			     u16 data_index, u64 generation, u32 payload_crc32)
{
	memset(footer, 0, sizeof(*footer));
	footer->magic = RAID301_SECTOR_FOOTER_MAGIC;
	footer->format_version = RAID301_FORMAT_VERSION;
	footer->role = role;
	footer->flags = flags;
	footer->stripe_id = cpu_to_le32(stripe_id);
	footer->member_id = cpu_to_le16(member_id);
	footer->data_index = cpu_to_le16(data_index);
	footer->generation = cpu_to_le64(generation);
	footer->payload_crc32 = cpu_to_le32(payload_crc32);

	/* Footer CRC covers the first 28 bytes of the footer */
	u32 fcrc = crc32(0, (const u8 *)footer, 28);
	footer->footer_crc32 = cpu_to_le32(fcrc);
}

/* Validate Sector Footer Integrity */
bool raid301_verify_footer(const struct raid301_sector_footer *footer,
			    u8 expected_role, u32 expected_stripe,
			    u16 expected_member, u16 expected_data_idx)
{
	if (!footer)
		return false;

	if (footer->magic != RAID301_SECTOR_FOOTER_MAGIC ||
	    footer->format_version != RAID301_FORMAT_VERSION)
		return false;

	if (footer->role != expected_role)
		return false;

	if (le32_to_cpu(footer->stripe_id) != expected_stripe ||
	    le16_to_cpu(footer->member_id) != expected_member ||
	    le16_to_cpu(footer->data_index) != expected_data_idx)
		return false;

	u32 calc_fcrc = crc32(0, (const u8 *)footer, 28);
	if (calc_fcrc != le32_to_cpu(footer->footer_crc32))
		return false;

	return true;
}

/*
 * Ordered Sector Programming:
 * 1. Erase erase_size bytes.
 * 2. Program payload (without footer).
 * 3. Verify payload program result.
 * 4. Program footer separately.
 * 5. Read back payload and footer, verify CRC32 and metadata.
 */
int raid301_write_sector_ordered(struct mtd_info *master, u64 sector_offset,
				 const u8 *payload, const struct raid301_sector_footer *footer)
{
	u32 payload_size = RAID301_PAYLOAD_SIZE;
	u64 footer_offset = sector_offset + payload_size;
	u8 *verify_buf;
	int ret;

	verify_buf = malloc(master->erasesize);
	if (!verify_buf)
		return -ENOMEM;

	/* 1. Erase erase_unit */
	ret = raid301_raw_erase_unit(master, sector_offset);
	if (ret)
		goto out_free;

	/* 2. Program payload */
	ret = raid301_raw_write(master, sector_offset, payload_size, payload);
	if (ret)
		goto out_free;

	/* 4. Program footer separately */
	ret = raid301_raw_write(master, footer_offset, sizeof(*footer), (const u8 *)footer);
	if (ret)
		goto out_free;

	/* 5. Read back and verify */
	ret = raid301_raw_read(master, sector_offset, master->erasesize, verify_buf);
	if (ret)
		goto out_free;

	u32 calc_pcrc = crc32(0, verify_buf, payload_size);
	if (calc_pcrc != le32_to_cpu(footer->payload_crc32)) {
		printf("RAID301 IO Error: Readback payload CRC mismatch at 0x%llx\n", sector_offset);
		ret = -EIO;
		goto out_free;
	}

	const struct raid301_sector_footer *rb_footer =
		(const struct raid301_sector_footer *)(verify_buf + payload_size);
	if (!raid301_verify_footer(rb_footer, footer->role,
				   le32_to_cpu(footer->stripe_id),
				   le16_to_cpu(footer->member_id),
				   le16_to_cpu(footer->data_index))) {
		printf("RAID301 IO Error: Readback footer verification failed at 0x%llx\n", sector_offset);
		ret = -EIO;
		goto out_free;
	}

out_free:
	free(verify_buf);
	return ret;
}
