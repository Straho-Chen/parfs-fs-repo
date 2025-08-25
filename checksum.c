/*
 * BRIEF DESCRIPTION
 *
 * Checksum related methods.
 *
 * Copyright 2015-2016 Regents of the University of California,
 * UCSD Non-Volatile Systems Lab, Andiry Xu <jix024@cs.ucsd.edu>
 * Copyright 2012-2013 Intel Corporation
 * Copyright 2009-2011 Marco Stornelli <marco.stornelli@gmail.com>
 * Copyright 2003 Sony Corporation
 * Copyright 2003 Matsushita Electric Industrial Co., Ltd.
 * 2003-2004 (c) MontaVista Software, Inc. , Steve Longerbeam
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include "nova.h"
#include "nova_def.h"

static int get_entry_copy(struct super_block *sb, void *entry, u32 *entry_csum,
			  size_t *entry_size, void *entry_copy)
{
	u8 type;
	struct nova_dentry *dentry;
	int ret = 0;

	ret = memcpy_mcsafe(&type, entry, sizeof(u8));
	if (ret < 0)
		return ret;

	switch (type) {
	case DIR_LOG:
		dentry = DENTRY(entry_copy);
		ret = memcpy_mcsafe(dentry, entry, NOVA_DENTRY_HEADER_LEN);
		if (ret < 0 || dentry->de_len > NOVA_MAX_ENTRY_LEN)
			break;
		*entry_size = dentry->de_len;
		ret = memcpy_mcsafe((u8 *)dentry + NOVA_DENTRY_HEADER_LEN,
				    (u8 *)entry + NOVA_DENTRY_HEADER_LEN,
				    *entry_size - NOVA_DENTRY_HEADER_LEN);
		if (ret < 0)
			break;
		*entry_csum = dentry->csum;
		break;
	case FILE_WRITE:
		*entry_size = sizeof(struct nova_file_write_entry);
		ret = memcpy_mcsafe(entry_copy, entry, *entry_size);
		if (ret < 0)
			break;
		*entry_csum = WENTRY(entry_copy)->csum;
		break;
	case SET_ATTR:
		*entry_size = sizeof(struct nova_setattr_logentry);
		ret = memcpy_mcsafe(entry_copy, entry, *entry_size);
		if (ret < 0)
			break;
		*entry_csum = SENTRY(entry_copy)->csum;
		break;
	case LINK_CHANGE:
		*entry_size = sizeof(struct nova_link_change_entry);
		ret = memcpy_mcsafe(entry_copy, entry, *entry_size);
		if (ret < 0)
			break;
		*entry_csum = LCENTRY(entry_copy)->csum;
		break;
	case MMAP_WRITE:
		*entry_size = sizeof(struct nova_mmap_entry);
		ret = memcpy_mcsafe(entry_copy, entry, *entry_size);
		if (ret < 0)
			break;
		*entry_csum = MMENTRY(entry_copy)->csum;
		break;
	case SNAPSHOT_INFO:
		*entry_size = sizeof(struct nova_snapshot_info_entry);
		ret = memcpy_mcsafe(entry_copy, entry, *entry_size);
		if (ret < 0)
			break;
		*entry_csum = SNENTRY(entry_copy)->csum;
		break;
	default:
		*entry_csum = 0;
		*entry_size = 0;
		nova_dbg(
			"%s: unknown or unsupported entry type (%d) for checksum, %#llx\n",
			__func__, type, (u64)entry);
		ret = -EINVAL;
		dump_stack();
		break;
	}

	return ret;
}

/* Calculate the entry checksum. */
static u32 nova_calc_entry_csum(void *entry)
{
	u8 type;
	u32 csum = 0;
	size_t entry_len, check_len;
	void *csum_addr, *remain;
	INIT_TIMING(calc_time);

	NOVA_START_TIMING(calc_entry_csum_t, calc_time);

	/* Entry is checksummed excluding its csum field. */
	type = nova_get_entry_type(entry);
	switch (type) {
	/* nova_dentry has variable length due to its name. */
	case DIR_LOG:
		entry_len = DENTRY(entry)->de_len;
		csum_addr = &DENTRY(entry)->csum;
		break;
	case FILE_WRITE:
		entry_len = sizeof(struct nova_file_write_entry);
		csum_addr = &WENTRY(entry)->csum;
		break;
	case SET_ATTR:
		entry_len = sizeof(struct nova_setattr_logentry);
		csum_addr = &SENTRY(entry)->csum;
		break;
	case LINK_CHANGE:
		entry_len = sizeof(struct nova_link_change_entry);
		csum_addr = &LCENTRY(entry)->csum;
		break;
	case MMAP_WRITE:
		entry_len = sizeof(struct nova_mmap_entry);
		csum_addr = &MMENTRY(entry)->csum;
		break;
	case SNAPSHOT_INFO:
		entry_len = sizeof(struct nova_snapshot_info_entry);
		csum_addr = &SNENTRY(entry)->csum;
		break;
	default:
		entry_len = 0;
		csum_addr = NULL;
		nova_dbg(
			"%s: unknown or unsupported entry type (%d) for checksum, 0x%llx\n",
			__func__, type, (u64)entry);
		break;
	}

	if (entry_len > 0) {
		check_len = ((u8 *)csum_addr) - ((u8 *)entry);
		csum = nova_crc32c(NOVA_INIT_CSUM, entry, check_len);
		check_len = entry_len - (check_len + NOVA_META_CSUM_LEN);
		if (check_len > 0) {
			remain = ((u8 *)csum_addr) + NOVA_META_CSUM_LEN;
			csum = nova_crc32c(csum, remain, check_len);
		}

		if (check_len < 0) {
			nova_dbg("%s: checksum run-length error %ld < 0",
				 __func__, check_len);
		}
	}

	NOVA_END_TIMING(calc_entry_csum_t, calc_time);
	return csum;
}

/* Update the log entry checksum. */
void nova_update_entry_csum(void *entry)
{
	u8 type;
	u32 csum;
	size_t entry_len = CACHELINE_SIZE;

	if (metadata_csum == 0)
		goto out;

	type = nova_get_entry_type(entry);
	csum = nova_calc_entry_csum(entry);

	switch (type) {
	case DIR_LOG:
		DENTRY(entry)->csum = cpu_to_le32(csum);
		entry_len = DENTRY(entry)->de_len;
		break;
	case FILE_WRITE:
		WENTRY(entry)->csum = cpu_to_le32(csum);
		entry_len = sizeof(struct nova_file_write_entry);
		break;
	case SET_ATTR:
		SENTRY(entry)->csum = cpu_to_le32(csum);
		entry_len = sizeof(struct nova_setattr_logentry);
		break;
	case LINK_CHANGE:
		LCENTRY(entry)->csum = cpu_to_le32(csum);
		entry_len = sizeof(struct nova_link_change_entry);
		break;
	case MMAP_WRITE:
		MMENTRY(entry)->csum = cpu_to_le32(csum);
		entry_len = sizeof(struct nova_mmap_entry);
		break;
	case SNAPSHOT_INFO:
		SNENTRY(entry)->csum = cpu_to_le32(csum);
		entry_len = sizeof(struct nova_snapshot_info_entry);
		break;
	default:
		entry_len = 0;
		nova_dbg("%s: unknown or unsupported entry type (%d), 0x%llx\n",
			 __func__, type, (u64)entry);
		break;
	}

	if (entry_len > 0)
		nova_flush_buffer(entry, entry_len, 0);

out:
	return;
}

int nova_update_alter_entry(struct super_block *sb, void *entry)
{
	struct nova_sb_info *sbi = NOVA_SB(sb);
	void *alter_entry;
	u64 curr, alter_curr;
	u32 entry_csum;
	size_t size;
	char entry_copy[NOVA_MAX_ENTRY_LEN];
	int ret;

	if (metadata_csum == 0)
		return 0;

	curr = nova_get_addr_off(sbi, entry);
	alter_curr = alter_log_entry(sb, curr);

	if (alter_curr == 0) {
		nova_err(sb, "%s: log page tail error detected\n", __func__);
		return -EIO;
	}
	alter_entry = (void *)nova_get_virt_addr_from_offset(sb, alter_curr);

	ret = get_entry_copy(sb, entry, &entry_csum, &size, entry_copy);
	if (ret)
		return ret;

	ret = memcpy_to_pmem_nocache(alter_entry, entry_copy, size);
	return ret;
}

/* media error: repair the poison radius that the entry belongs to */
static int nova_repair_entry_pr(struct super_block *sb, void *entry)
{
	struct nova_sb_info *sbi = NOVA_SB(sb);
	int ret;
	u64 entry_off, alter_off;
	void *entry_pr, *alter_pr;
	unsigned long irq_flags = 0;

	entry_off = nova_get_addr_off(sbi, entry);
	alter_off = alter_log_entry(sb, entry_off);
	if (alter_off == 0) {
		nova_err(sb, "%s: log page tail error detected\n", __func__);
		goto fail;
	}

	entry_pr = (void *)nova_get_virt_addr_from_offset(
		sb, entry_off & POISON_MASK);
	alter_pr = (void *)nova_get_virt_addr_from_offset(
		sb, alter_off & POISON_MASK);

	if (entry_pr == NULL || alter_pr == NULL)
		BUG();

	nova_memunlock_range(sb, entry_pr, POISON_RADIUS, &irq_flags);
	ret = memcpy_mcsafe(entry_pr, alter_pr, POISON_RADIUS);
	nova_memlock_range(sb, entry_pr, POISON_RADIUS, &irq_flags);
	nova_flush_buffer(entry_pr, POISON_RADIUS, 0);

	/* alter_entry shows media error during memcpy */
	if (ret < 0)
		goto fail;

	nova_dbg("%s: entry media error repaired\n", __func__);
	return 0;

fail:
	nova_err(sb, "%s: unrecoverable media error detected\n", __func__);
	return -1;
}

static int nova_repair_entry(struct super_block *sb, void *bad, void *good,
			     size_t entry_size)
{
	int ret;
	unsigned long irq_flags = 0;

	nova_memunlock_range(sb, bad, entry_size, &irq_flags);
	ret = memcpy_to_pmem_nocache(bad, good, entry_size);
	nova_memlock_range(sb, bad, entry_size, &irq_flags);

	if (ret == 0)
		nova_dbg("%s: entry error repaired\n", __func__);

	return ret;
}

bool nova_get_entry_copy(struct super_block *sb, void *entry, void *entryc)
{
	int ret = 0;
	size_t entry_size;
	u32 entry_csum;
	char entry_copy[NOVA_MAX_ENTRY_LEN];
	ret = get_entry_copy(sb, entry, &entry_csum, &entry_size, entry_copy);
	if (ret < 0) { /* media error */
		nova_dbg_verbose("%s: get_entry_copy failed: %d\n", __func__,
				 ret);
		ret = nova_repair_entry_pr(sb, entry);
		nova_dbg_verbose("%s: nova_repair_entry_pr failed: %d\n",
				 __func__, ret);
		if (ret < 0)
			goto fail;
		/* try again */
		ret = get_entry_copy(sb, entry, &entry_csum, &entry_size,
				     entry_copy);
		nova_dbg_verbose("%s: get_entry_copy failed again: %d\n",
				 __func__, ret);
		if (ret < 0)
			goto fail;
	}
	memcpy(entryc, entry_copy, entry_size);
	return true;
fail:
	nova_err(sb, "%s: unable to repair entry errors\n", __func__);
	return false;
}

/* Verify the log entry checksum and get a copy in DRAM. */
bool nova_verify_entry_csum(struct super_block *sb, void *entry, void *entryc)
{
	struct nova_sb_info *sbi = NOVA_SB(sb);
	int ret = 0;
	u64 entry_off, alter_off;
	void *alter;
	size_t entry_size, alter_size;
	u32 entry_csum, alter_csum;
	u32 entry_csum_calc, alter_csum_calc;
	char entry_copy[NOVA_MAX_ENTRY_LEN];
	char alter_copy[NOVA_MAX_ENTRY_LEN];
	INIT_TIMING(verify_time);

	if (metadata_csum == 0)
		return true;

	NOVA_START_TIMING(verify_entry_csum_t, verify_time);

	ret = get_entry_copy(sb, entry, &entry_csum, &entry_size, entry_copy);
	if (ret < 0) { /* media error */
		nova_dbg_verbose("%s: get_entry_copy main failed: %d\n",
				 __func__, ret);
		ret = nova_repair_entry_pr(sb, entry);
		nova_dbg_verbose("%s: nova_repair_entry_pr main failed: %d\n",
				 __func__, ret);
		if (ret < 0)
			goto fail;
		/* try again */
		ret = get_entry_copy(sb, entry, &entry_csum, &entry_size,
				     entry_copy);
		nova_dbg_verbose("%s: get_entry_copy main failed again: %d\n",
				 __func__, ret);
		if (ret < 0)
			goto fail;
	}

	entry_off = nova_get_addr_off(sbi, entry);
	alter_off = alter_log_entry(sb, entry_off);
	if (alter_off == 0) {
		nova_err(sb, "%s: log page tail error detected\n", __func__);
		goto fail;
	}

	alter = (void *)nova_get_virt_addr_from_offset(sb, alter_off);
	ret = get_entry_copy(sb, alter, &alter_csum, &alter_size, alter_copy);
	if (ret < 0) { /* media error */
		nova_dbg_verbose("%s: get_entry_copy failed: %d\n", __func__,
				 ret);
		ret = nova_repair_entry_pr(sb, alter);
		nova_dbg_verbose("%s: nova_repair_entry_pr alter failed: %d\n",
				 __func__, ret);
		if (ret < 0)
			goto fail;
		/* try again */
		ret = get_entry_copy(sb, alter, &alter_csum, &alter_size,
				     alter_copy);
		nova_dbg_verbose("%s: get_entry_copy alter failed again: %d\n",
				 __func__, ret);
		if (ret < 0)
			goto fail;
	}

	/* no media errors, now verify the checksums */
	entry_csum = le32_to_cpu(entry_csum);
	alter_csum = le32_to_cpu(alter_csum);
	entry_csum_calc = nova_calc_entry_csum(entry_copy);
	alter_csum_calc = nova_calc_entry_csum(alter_copy);

	if (entry_csum != entry_csum_calc && alter_csum != alter_csum_calc) {
		nova_err(
			sb,
			"%s: both entry and its replica fail checksum verification\n",
			__func__);
		goto fail;
	} else if (entry_csum != entry_csum_calc) {
		nova_dbg(
			"%s: entry %p checksum error, trying to repair using the replica\n",
			__func__, entry);
		ret = nova_repair_entry(sb, entry, alter_copy, alter_size);
		if (ret != 0)
			goto fail;

		memcpy(entryc, alter_copy, alter_size);
	} else if (alter_csum != alter_csum_calc) {
		nova_dbg(
			"%s: entry replica %p checksum error, trying to repair using the primary\n",
			__func__, alter);
		ret = nova_repair_entry(sb, alter, entry_copy, entry_size);
		if (ret != 0)
			goto fail;

		memcpy(entryc, entry_copy, entry_size);
	} else {
		/* now both entries pass checksum verification and the primary
		 * is trusted if their buffers don't match
		 */
		if (memcmp(entry_copy, alter_copy, entry_size)) {
			nova_dbg(
				"%s: entry replica %p error, trying to repair using the primary\n",
				__func__, alter);
			ret = nova_repair_entry(sb, alter, entry_copy,
						entry_size);
			if (ret != 0)
				goto fail;
		}

		memcpy(entryc, entry_copy, entry_size);
	}

	NOVA_END_TIMING(verify_entry_csum_t, verify_time);
	return true;

fail:
	nova_err(sb, "%s: unable to repair entry errors\n", __func__);

	NOVA_END_TIMING(verify_entry_csum_t, verify_time);
	return false;
}

/* media error: repair the poison radius that the inode belongs to */
static int nova_repair_inode_pr(struct super_block *sb,
				struct nova_inode *bad_pi,
				struct nova_inode *good_pi)
{
	int ret;
	void *bad_pr, *good_pr;
	unsigned long irq_flags = 0;

	bad_pr = (void *)((u64)bad_pi & POISON_MASK);
	good_pr = (void *)((u64)good_pi & POISON_MASK);

	if (bad_pr == NULL || good_pr == NULL)
		BUG();

	nova_memunlock_range(sb, bad_pr, POISON_RADIUS, &irq_flags);
	ret = memcpy_mcsafe(bad_pr, good_pr, POISON_RADIUS);
	nova_memlock_range(sb, bad_pr, POISON_RADIUS, &irq_flags);
	nova_flush_buffer(bad_pr, POISON_RADIUS, 0);

	/* good_pi shows media error during memcpy */
	if (ret < 0)
		goto fail;

	nova_dbg("%s: inode media error repaired\n", __func__);
	return 0;

fail:
	nova_err(sb, "%s: unrecoverable media error detected\n", __func__);
	return -1;
}

static int nova_repair_inode(struct super_block *sb, struct nova_inode *bad_pi,
			     struct nova_inode *good_copy)
{
	int ret;
	unsigned long irq_flags = 0;

	nova_memunlock_inode(sb, bad_pi, &irq_flags);
	ret = memcpy_to_pmem_nocache(bad_pi, good_copy,
				     sizeof(struct nova_inode));
	nova_memlock_inode(sb, bad_pi, &irq_flags);

	if (ret == 0)
		nova_dbg("%s: inode %llu error repaired\n", __func__,
			 good_copy->nova_ino);

	return ret;
}

int nova_copy_inode(struct super_block *sb, u64 ino, u64 pi_addr,
		    u64 alter_pi_addr, struct nova_inode *pic)
{
	struct nova_inode *pi, *alter_pi;
	int ret;

	pi = (struct nova_inode *)nova_get_virt_addr_from_offset(sb, pi_addr);

	ret = memcpy_mcsafe(pic, pi, sizeof(struct nova_inode));

	if (metadata_csum == 0)
		return ret;

	alter_pi = (struct nova_inode *)nova_get_virt_addr_from_offset(
		sb, alter_pi_addr);

	if (ret < 0) { /* media error */
		ret = nova_repair_inode_pr(sb, pi, alter_pi);
		if (ret < 0)
			goto fail;
		/* try again */
		ret = memcpy_mcsafe(pic, pi, sizeof(struct nova_inode));
		if (ret < 0)
			goto fail;
	}

	return 0;

fail:
	nova_err(sb, "%s: unable to repair inode errors\n", __func__);

	return -EIO;
}

/*
 * Check nova_inode and get a copy in DRAM.
 * If we are going to update (write) the inode, we don't need to check the
 * alter inode if the major inode checks ok. If we are going to read or rebuild
 * the inode, also check the alter even if the major inode checks ok.
 */
int nova_check_inode_integrity(struct super_block *sb, u64 ino, u64 pi_addr,
			       u64 alter_pi_addr, struct nova_inode *pic,
			       int check_replica)
{
	struct nova_inode *pi, *alter_pi, alter_copy, *alter_pic;
	int inode_bad, alter_bad;
	int ret;

	pi = (struct nova_inode *)nova_get_virt_addr_from_offset(sb, pi_addr);

	ret = memcpy_mcsafe(pic, pi, sizeof(struct nova_inode));

	if (metadata_csum == 0)
		return ret;

	alter_pi = (struct nova_inode *)nova_get_virt_addr_from_offset(
		sb, alter_pi_addr);

	if (ret < 0) { /* media error */
		ret = nova_repair_inode_pr(sb, pi, alter_pi);
		if (ret < 0)
			goto fail;
		/* try again */
		ret = memcpy_mcsafe(pic, pi, sizeof(struct nova_inode));
		if (ret < 0)
			goto fail;
	}

	inode_bad = nova_check_inode_checksum(pic);

	if (!inode_bad && !check_replica)
		return 0;

	alter_pic = &alter_copy;
	ret = memcpy_mcsafe(alter_pic, alter_pi, sizeof(struct nova_inode));
	if (ret < 0) { /* media error */
		if (inode_bad)
			goto fail;
		ret = nova_repair_inode_pr(sb, alter_pi, pi);
		if (ret < 0)
			goto fail;
		/* try again */
		ret = memcpy_mcsafe(alter_pic, alter_pi,
				    sizeof(struct nova_inode));
		if (ret < 0)
			goto fail;
	}

	alter_bad = nova_check_inode_checksum(alter_pic);

	if (inode_bad && alter_bad) {
		nova_err(
			sb,
			"%s: both inode and its replica fail checksum verification\n",
			__func__);
		goto fail;
	} else if (inode_bad) {
		nova_dbg(
			"%s: inode %llu checksum error, trying to repair using the replica\n",
			__func__, ino);
		nova_print_inode(pi);
		nova_print_inode(alter_pi);
		ret = nova_repair_inode(sb, pi, alter_pic);
		if (ret != 0)
			goto fail;

		memcpy(pic, alter_pic, sizeof(struct nova_inode));
	} else if (alter_bad) {
		nova_dbg(
			"%s: inode replica %llu checksum error, trying to repair using the primary\n",
			__func__, ino);
		ret = nova_repair_inode(sb, alter_pi, pic);
		if (ret != 0)
			goto fail;
	} else if (memcmp(pic, alter_pic, sizeof(struct nova_inode))) {
		nova_dbg(
			"%s: inode replica %llu is stale, trying to repair using the primary\n",
			__func__, ino);
		ret = nova_repair_inode(sb, alter_pi, pic);
		if (ret != 0)
			goto fail;
	}

	return 0;

fail:
	nova_err(sb, "%s: unable to repair inode errors\n", __func__);

	return -EIO;
}

/* Calculate the stripe csum by crc.
 * In this case, we assume that csum is 32 bytes and block is 4K aligned.
 */
static int nova_stripe_csum_crc(struct super_block *sb, unsigned long strps,
				unsigned long blocknr, u8 *strp_ptr, int zero)
{
	struct nova_sb_info *sbi = NOVA_SB(sb);
	size_t strp_size = NOVA_STRIPE_SIZE;
	unsigned long strp;
	u32 csum;
	u32 crc[8];
	void *csum_addr;
	void *src_addr;
	unsigned long irq_flags = 0;

	nova_dbg_verbose("%s: blocknr: %#lx, strps: %#lx\n", __func__, blocknr,
			 strps);

	/*
	 * 4K aligned data page has 8 stripes
	 * calculate checksum for 8 stripes at a time
	 */
	while (strps >= 8) {
		if (zero) {
			src_addr = sbi->zero_csum;
			goto copy;
		}

		crc[0] = cpu_to_le32(
			nova_crc32c(NOVA_INIT_CSUM, strp_ptr, strp_size));
		crc[1] = cpu_to_le32(nova_crc32c(
			NOVA_INIT_CSUM, strp_ptr + strp_size, strp_size));
		crc[2] = cpu_to_le32(nova_crc32c(
			NOVA_INIT_CSUM, strp_ptr + strp_size * 2, strp_size));
		crc[3] = cpu_to_le32(nova_crc32c(
			NOVA_INIT_CSUM, strp_ptr + strp_size * 3, strp_size));
		crc[4] = cpu_to_le32(nova_crc32c(
			NOVA_INIT_CSUM, strp_ptr + strp_size * 4, strp_size));
		crc[5] = cpu_to_le32(nova_crc32c(
			NOVA_INIT_CSUM, strp_ptr + strp_size * 5, strp_size));
		crc[6] = cpu_to_le32(nova_crc32c(
			NOVA_INIT_CSUM, strp_ptr + strp_size * 6, strp_size));
		crc[7] = cpu_to_le32(nova_crc32c(
			NOVA_INIT_CSUM, strp_ptr + strp_size * 7, strp_size));

		src_addr = crc;
copy:
		csum_addr = nova_get_data_csum_addr(sb, blocknr);

		nova_memunlock_range(sb, csum_addr, NOVA_DATA_CSUM_LEN * 8,
				     &irq_flags);
		if (support_clwb) {
			for (int i = 0; i < 8; i++) {
				nova_dbg_verbose(
					"%s: blocknr: %#lx, strip %d, data csum: 0x%08x\n",
					__func__, blocknr, i, crc[i]);
			}
			memcpy(csum_addr, src_addr, NOVA_DATA_CSUM_LEN * 8);
		} else {
			memcpy_to_pmem_nocache(csum_addr, src_addr,
					       NOVA_DATA_CSUM_LEN * 8);
		}
		nova_memlock_range(sb, csum_addr, NOVA_DATA_CSUM_LEN * 8,
				   &irq_flags);
		if (support_clwb) {
			nova_flush_buffer(csum_addr, NOVA_DATA_CSUM_LEN * 8, 0);
		}

		// next page
		blocknr++;
		strps -= 8;
		if (!zero)
			strp_ptr += strp_size * 8;
	}

	/*
	 * checksum for the remaining stripes
	 * Cause remaining stripes should be less than 8, so thay are located on the same blocknr.
	 */
	nova_dbg_verbose("%s: last blocknr: %lx, last strps: %lu\n", __func__,
			 blocknr, strps);
	if (strps) {
		csum_addr = nova_get_data_csum_addr(sb, blocknr);
		for (strp = 0; strp < strps; strp++) {
			if (zero)
				csum = sbi->zero_csum[0];
			else
				csum = nova_crc32c(NOVA_INIT_CSUM, strp_ptr,
						   strp_size);

			csum = cpu_to_le32(csum);

			nova_memunlock_range(sb, csum_addr, NOVA_DATA_CSUM_LEN,
					     &irq_flags);
			memcpy_to_pmem_nocache(csum_addr, &csum,
					       NOVA_DATA_CSUM_LEN);
			nova_memlock_range(sb, csum_addr, NOVA_DATA_CSUM_LEN,
					   &irq_flags);

			csum_addr += NOVA_DATA_CSUM_LEN;

			if (!zero)
				strp_ptr += strp_size;
		}
	}

	return 0;
}

/* Calculate the stripe csum by crc.
 * In this case, we assume that csum is 64 bytes and block is 4K aligned.
 */
static int nova_stripe_csum_xxhash(struct super_block *sb, unsigned long strps,
				   unsigned long blocknr, u8 *strp_ptr,
				   int zero)
{
	struct nova_sb_info *sbi = NOVA_SB(sb);
	size_t strp_size = NOVA_STRIPE_SIZE;
	unsigned long strp;
	u64 csum;
	void *csum_addr;
	void *src_addr;
	unsigned long irq_flags = 0;

	/*
	 * 4K aligned data page has one stripe
	 * calculate checksum for one stripe at a time
	 */
	for (strp = 0; strp < strps; strp++) {
		if (zero) {
			src_addr = sbi->zero_csum;
			goto copy;
		}

		csum = xxh64(strp_ptr, strp_size, NOVA_INIT_CSUM);

		src_addr = &csum;
copy:
		csum_addr = nova_get_data_csum_addr(sb, blocknr + strp);

		nova_memunlock_range(sb, csum_addr, NOVA_DATA_CSUM_LEN,
				     &irq_flags);
		memcpy_to_pmem_nocache(csum_addr, src_addr, NOVA_DATA_CSUM_LEN);
		nova_memlock_range(sb, csum_addr, NOVA_DATA_CSUM_LEN,
				   &irq_flags);

		if (!zero)
			strp_ptr += strp_size;
	}

	return 0;
}

/*
 * We calculate the checksum in a sripe. The stripe size is define in nova_def.h
 * as NOVA_STRIPE_SIZE(SHIFT).
 */
int nova_update_block_csum(struct super_block *sb, unsigned long bytes,
			   unsigned long start_blknr, u8 *strp_ptr, int zero)
{
	if (!IS_ALIGNED(bytes, NOVA_STRIPE_SIZE)) {
		nova_err(sb, "%s: bytes is not aligned with stripe\n",
			 __func__);
		return -EINVAL;
	}
	unsigned long strps = bytes >> NOVA_STRIPE_SHIFT;
#if NOVA_XXHASH_CSUM
	return nova_stripe_csum_xxhash(sb, strps, start_blknr, strp_ptr, zero);
#else
	return nova_stripe_csum_crc(sb, strps, start_blknr, strp_ptr, zero);
#endif
	return 0;
}

int nova_update_pgoff_csum(struct super_block *sb,
			   struct nova_inode_info_header *sih,
			   struct nova_file_write_entry *entry,
			   unsigned long pgoff, int zero)
{
	u64 blocknr;
	void *dax_mem = NULL;

	blocknr = get_nvmm(sb, sih, entry, pgoff);

	dax_mem = nova_get_virt_addr_from_offset(
		sb, nova_get_block_off(sb, blocknr, sih->i_blk_type));

	nova_dbg_verbose("%s: find block: %#llx\n", __func__, blocknr);

	nova_update_block_csum(sb, nova_inode_blk_size(sih), blocknr, dax_mem,
			       zero);

	return 0;
}

/* Verify checksums of requested data bytes starting from offset of blocknr.
 *
 * Only a whole block can be checksum verified.
 * Because we only support cow, so every modification of data will
 * generate a new block.
 *
 * blocknr: container blocknr for the first stripe to be verified
 * bytes:   number of contiguous bytes to be verified starting from offset
 *
 * return: true or false
 */
bool nova_verify_data_csum(struct super_block *sb,
			   struct nova_inode_info_header *sih,
			   unsigned long blocknr, size_t bytes)
{
	void *blockptr;
	size_t blockoff;
	size_t strp_size = NOVA_STRIPE_SIZE;
	unsigned int strp_shift = NOVA_STRIPE_SHIFT;
	unsigned long strp, strps_per_block, blocks, block;
	void *strip = NULL;
	u32 csum_calc, csum_nvmm;
	u32 *csum_addr;
	int error;
	bool match;
	unsigned long irq_flags = 0;
	INIT_TIMING(verify_time);

	NOVA_START_TIMING(verify_data_csum_t, verify_time);

	/* Only a whole stripe can be checksum verified.
	 * strps: # of stripes to be checked since offset.
	 */
	blocks = ((bytes - 1) >> PAGE_SHIFT) + 1;
	strps_per_block = PAGE_SIZE >> strp_shift;

	for (block = 0; block < blocks; block++) {
		blockoff = nova_get_block_off(sb, blocknr + block,
					      sih->i_blk_type);
		blockptr = nova_get_virt_addr_from_offset(sb, blockoff);

		strip = kmalloc(strp_size, GFP_KERNEL);
		if (strip == NULL) {
			nova_err(sb, "%s: kmalloc failed\n", __func__);
			match = false;
			goto out;
		}

		match = true;
		for (strp = 0; strp < strps_per_block; strp++) {
			csum_addr = nova_get_data_csum_addr(sb, blocknr) +
				    strp * NOVA_DATA_CSUM_LEN;
			csum_nvmm = le32_to_cpu(*csum_addr);

			error = memcpy_mcsafe(
				strip, blockptr + strp * strp_size, strp_size);
			if (error < 0) {
				nova_dbg(
					"%s: media error in data strip detected!\n",
					__func__);
				match = false;
				goto out;
			} else {
#if NOVA_XXHASH_CSUM
				csum_calc =
					xxh64(strip, strp_size, NOVA_INIT_CSUM);
#else
				csum_calc = nova_crc32c(NOVA_INIT_CSUM, strip,
							strp_size);
#endif
				match = (csum_calc == csum_nvmm);
			}

			if (!match) {
				/* Getting here, data is considered corrupted.
				 *
				 * if: csum_nvmm0 == csum_nvmm1
				 *     both csums good, run data recovery
				 * if: csum_nvmm0 != csum_nvmm1
				 *     at least one csum is corrupted, also need to run
				 *     data recovery to see if one csum is still good
				 */
				nova_dbg(
					"%s: nova data corruption detected! inode %lu, strp %#lx block %#lx of blocks %#lx, block nr %#lx, csum calc 0x%08x, csum nvmm 0x%08x\n",
					__func__, sih->ino, strp, block, blocks,
					blocknr, csum_calc, csum_nvmm);

				// data corruption, roll back to old block on caller
				goto out;
			} else {
				nova_dbg_verbose(
					"%s: blocknr: %#lx, strip %ld, data csum: 0x%08x\n",
					__func__, blocknr, strp, csum_nvmm);
			}
		}
	}

out:
	if (strip != NULL)
		kfree(strip);

	NOVA_END_TIMING(verify_data_csum_t, verify_time);

	return match;
}

int nova_update_truncated_block_csum(struct super_block *sb,
				     struct inode *inode, loff_t newsize)
{
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
	unsigned long pgoff, length, blocknr;
	u64 nvmm;
	char *nvmm_addr, *block;
	int ret = 0;

	pgoff = newsize >> nova_inode_blk_shift(sih);

	nvmm = nova_find_nvmm_block(sb, sih, NULL, pgoff);
	nvmm_addr = (char *)nova_get_virt_addr_from_offset(sb, nvmm);
	blocknr = nova_get_blocknr(sb, nvmm, sih->i_blk_type);

	nova_dbg_verbose("%s: nvmm: %#llx, nvmm_addr: %#llx, blocknr: %#lx\n",
			 __func__, nvmm, (u64)nvmm_addr, blocknr);

	length = nova_inode_blk_size(sih);

	/* Copy to DRAM to catch MCE. */
	block = kmalloc(length, GFP_KERNEL);
	if (block == NULL) {
		ret = -ENOMEM;
		goto out;
	}

	if (memcpy_mcsafe(block, nvmm_addr, length) < 0) {
		ret = -EIO;
		goto out;
	}

	nova_update_block_csum(sb, length, blocknr, block, 0);

out:
	if (block != NULL)
		kfree(block);

	return ret;
}
