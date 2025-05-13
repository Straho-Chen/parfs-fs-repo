/*
 * BRIEF DESCRIPTION
 *
 * File operations for files.
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

#include "linux/types.h"
#include <linux/slab.h>
#include <linux/uio.h>
#include <linux/uaccess.h>
#include <linux/falloc.h>
#include <asm/mman.h>
#include "nova.h"

static inline int nova_can_set_blocksize_hint(struct inode *inode,
					      struct nova_inode *pi,
					      loff_t new_size)
{
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;

	/* Currently, we don't deallocate data blocks till the file is deleted.
	 * So no changing blocksize hints once allocation is done.
	 */
	if (sih->i_size > 0)
		return 0;
	return 1;
}

int nova_set_blocksize_hint(struct super_block *sb, struct inode *inode,
			    struct nova_inode *pi, loff_t new_size)
{
	unsigned short block_type;
	unsigned long irq_flags = 0;

	if (!nova_can_set_blocksize_hint(inode, pi, new_size))
		return 0;

	if (new_size >= 0x40000000) { /* 1G */
		block_type = NOVA_BLOCK_TYPE_1G;
		goto hint_set;
	}

	if (new_size >= 0x200000) { /* 2M */
		block_type = NOVA_BLOCK_TYPE_2M;
		goto hint_set;
	}

	if (new_size >= 0x8000) { /* 32K */
		block_type = NOVA_BLOCK_TYPE_32K;
		goto hint_set;
	}

	/* defaulting to 4K */
	block_type = NOVA_BLOCK_TYPE_4K;

hint_set:
	nova_dbg_verbose("Hint: new_size 0x%llx, i_size 0x%llx\n", new_size,
			 pi->i_size);
	nova_dbg_verbose("Setting the hint to 0x%x\n", block_type);
	nova_memunlock_inode(sb, pi, &irq_flags);
	pi->i_blk_type = block_type;
	nova_memlock_inode(sb, pi, &irq_flags);
	return 0;
}

static loff_t nova_llseek(struct file *file, loff_t offset, int origin)
{
	struct inode *inode = file->f_path.dentry->d_inode;
	int retval;

	if (origin != SEEK_DATA && origin != SEEK_HOLE)
		return generic_file_llseek(file, offset, origin);

	inode_lock(inode);
	switch (origin) {
	case SEEK_DATA:
		retval = nova_find_region(inode, &offset, 0);
		if (retval) {
			inode_unlock(inode);
			return retval;
		}
		break;
	case SEEK_HOLE:
		retval = nova_find_region(inode, &offset, 1);
		if (retval) {
			inode_unlock(inode);
			return retval;
		}
		break;
	}

	if ((offset < 0 && !(file->f_mode & FMODE_UNSIGNED_OFFSET)) ||
	    offset > inode->i_sb->s_maxbytes) {
		inode_unlock(inode);
		return -ENXIO;
	}

	if (offset != file->f_pos) {
		file->f_pos = offset;
		file->f_version = 0;
	}

	inode_unlock(inode);
	return offset;
}

/* This function is called by both msync() and fsync().
 * TODO: Check if we can avoid calling nova_flush_buffer() for fsync. We use
 * movnti to write data to files, so we may want to avoid doing unnecessary
 * nova_flush_buffer() on fsync()
 */
static int nova_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
	struct address_space *mapping = file->f_mapping;
	struct inode *inode = file->f_path.dentry->d_inode;
	struct super_block *sb = inode->i_sb;
	unsigned long start_pgoff, end_pgoff;
	int ret = 0;
	INIT_TIMING(fsync_time);

	NOVA_START_TIMING(fsync_t, fsync_time);

	if (datasync)
		NOVA_STATS_ADD(fdatasync, 1);

	/* No need to flush if the file is not mmaped */
	if (!mapping_mapped(mapping))
		goto persist;

	start_pgoff = start >> PAGE_SHIFT;
	end_pgoff = (end + 1) >> PAGE_SHIFT;
	nova_dbg_verbose("%s: msync pgoff range %#lx to %#lx\n", __func__,
			 start_pgoff, end_pgoff);

	/*
	 * Set csum and parity.
	 * We do not protect data integrity during mmap, but we have to
	 * update csum here since msync clears dirty bit.
	 */
	nova_reset_mapping_csum_parity(sb, inode, mapping, start_pgoff,
				       end_pgoff);

	ret = generic_file_fsync(file, start, end, datasync);

persist:
	PERSISTENT_BARRIER();
	NOVA_END_TIMING(fsync_t, fsync_time);

	return ret;
}

/* This callback is called when a file is closed */
static int nova_flush(struct file *file, fl_owner_t id)
{
	PERSISTENT_BARRIER();
	return 0;
}

static int nova_open(struct inode *inode, struct file *filp)
{
	return generic_file_open(inode, filp);
}

static long nova_fallocate(struct file *file, int mode, loff_t offset,
			   loff_t len)
{
	struct inode *inode = file->f_path.dentry->d_inode;
	struct super_block *sb = inode->i_sb;
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
	struct nova_inode *pi;
	struct nova_inode pic;
	struct nova_file_write_entry *entry;
	struct nova_file_write_entry *entryc, entry_copy;
	struct nova_file_write_entry entry_data;
	struct nova_inode_update update;
	unsigned long start_blk, num_blocks, ent_blks = 0;
	unsigned long total_blocks = 0;
	unsigned long blocknr = 0;
	unsigned long blockoff;
	unsigned int data_bits;
	loff_t new_size;
	long ret = 0;
	int inplace = 0;
	int blocksize_mask;
	int allocated = 0;
	bool update_log = false;
	INIT_TIMING(fallocate_time);
	u64 begin_tail = 0;
	u64 epoch_id;
	u32 time;
	unsigned long irq_flags = 0;

	/*
	 * Fallocate does not make much sence for CoW,
	 * but we still support it for DAX-mmap purpose.
	 */

	/* We only support the FALLOC_FL_KEEP_SIZE mode */
	if (mode & ~FALLOC_FL_KEEP_SIZE)
		return -EOPNOTSUPP;

	if (S_ISDIR(inode->i_mode))
		return -ENODEV;

	new_size = len + offset;
	if (!(mode & FALLOC_FL_KEEP_SIZE) && new_size > inode->i_size) {
		ret = inode_newsize_ok(inode, new_size);
		if (ret)
			return ret;
	} else {
		new_size = inode->i_size;
	}

	nova_dbg_verbose("%s: inode %lu, offset %lld, count %lld, mode 0x%x\n",
			 __func__, inode->i_ino, offset, len, mode);

	NOVA_START_TIMING(fallocate_t, fallocate_time);
	inode_lock(inode);

	pi = nova_get_inode(sb, inode);
	if (!pi) {
		ret = -EACCES;
		goto out;
	}
	memcpy(&pic, pi, sizeof(struct nova_inode));

	inode->i_mtime = inode_set_ctime_current(inode);
	time = inode->i_mtime.tv_sec;

	data_bits = nova_inode_blk_shift(sih);
	blocksize_mask = nova_inode_blk_shift(sih) - 1;
	start_blk = offset >> data_bits;
	blockoff = offset & blocksize_mask;
	num_blocks = ((blockoff + len - 1) >> data_bits) + 1;

	epoch_id = nova_get_epoch_id(sb);
	update.tail = sih->log_tail;
	update.alter_tail = sih->alter_log_tail;
	while (num_blocks > 0) {
		ent_blks = nova_check_existing_entry(sb, inode, num_blocks,
						     start_blk, &entry,
						     &entry_copy, 1, epoch_id,
						     &inplace, 1);

		entryc = (metadata_csum == 0) ? entry : &entry_copy;

		if (entry && inplace) {
			if (entryc->size < new_size) {
				/* Update existing entry */
				nova_memunlock_range(sb, entry, CACHELINE_SIZE,
						     &irq_flags);
				entry->size = new_size;
				nova_update_entry_csum(entry);
				nova_update_alter_entry(sb, entry);
				nova_memlock_range(sb, entry, CACHELINE_SIZE,
						   &irq_flags);
			}
			allocated = ent_blks;
			goto next;
		}

		/* Allocate zeroed blocks to fill hole */
		allocated = nova_new_data_blocks(sb, sih, &blocknr, start_blk,
						 ent_blks, ALLOC_INIT_ZERO,
						 ANY_CPU, ALLOC_FROM_HEAD);
		nova_dbg_verbose("%s: alloc %d blocks @ %#lx\n", __func__,
				 allocated, blocknr);

		if (allocated <= 0) {
			nova_dbg("%s alloc %#lx blocks failed!, %d\n", __func__,
				 ent_blks, allocated);
			ret = allocated;
			goto out;
		}

		/* Handle hole fill write */
		nova_init_file_write_entry(sb, sih, &entry_data, epoch_id,
					   start_blk, allocated, blocknr, time,
					   new_size);

		ret = nova_append_file_write_entry(sb, pi, &pic, inode,
						   &entry_data, &update);
		if (ret) {
			nova_dbg("%s: append inode entry failed\n", __func__);
			ret = -ENOSPC;
			goto out;
		}

		entry = nova_get_virt_addr_from_offset(sb, update.curr_entry,
						       1);
		nova_reset_csum_parity_range(sb, sih, entry, start_blk,
					     start_blk + allocated, 1, 0);

		update_log = true;
		if (begin_tail == 0)
			begin_tail = update.curr_entry;

		total_blocks += allocated;
next:
		num_blocks -= allocated;
		start_blk += allocated;
	}

	sih->i_blocks += (total_blocks << (data_bits - sb->s_blocksize_bits));

	inode->i_blocks = sih->i_blocks;

	if (update_log) {
		sih->log_tail = update.tail;
		sih->alter_log_tail = update.alter_tail;

		nova_memunlock_inode(sb, pi, &irq_flags);
		nova_update_tail(&pic, update.tail, 0);
		if (metadata_csum)
			nova_update_alter_tail(&pic, update.alter_tail, 0);
		nova_memlock_inode(sb, pi, &irq_flags);
		// /* Update file tree */
		// ret = nova_reassign_file_tree(sb, sih, begin_tail);
		// if (ret)
		// 	goto out;
	}

	nova_dbg_verbose("blocks: %#lx, %#lx\n", inode->i_blocks,
			 sih->i_blocks);

	if (ret || (mode & FALLOC_FL_KEEP_SIZE)) {
		nova_memunlock_inode(sb, pi, &irq_flags);
		pi->i_flags |= cpu_to_le32(NOVA_EOFBLOCKS_FL);
		nova_memlock_inode(sb, pi, &irq_flags);
		sih->i_flags |= cpu_to_le32(NOVA_EOFBLOCKS_FL);
	}

	if (!(mode & FALLOC_FL_KEEP_SIZE) && new_size > inode->i_size) {
		inode->i_size = new_size;
		sih->i_size = new_size;
	}

	nova_memunlock_inode(sb, pi, &irq_flags);
	nova_update_inode_checksum(&pic, 0);
	nova_update_alter_inode(sb, inode, &pic);
	memcpy_to_pmem_nocache(pi, &pic, sizeof(struct nova_inode));
	nova_memlock_inode(sb, pi, &irq_flags);

	sih->trans_id++;
out:
	if (ret < 0)
		nova_cleanup_incomplete_write(sb, sih, blocknr, allocated,
					      begin_tail, update.tail);

	inode_unlock(inode);
	NOVA_END_TIMING(fallocate_t, fallocate_time);
	return ret;
}

static int nova_iomap_begin_nolock(struct inode *inode, loff_t offset,
				   loff_t length, unsigned int flags,
				   struct iomap *iomap, struct iomap *srcmap)
{
	return nova_iomap_begin(inode, offset, length, flags, iomap, srcmap,
				false);
}

static struct iomap_ops nova_iomap_ops_nolock = {
	.iomap_begin = nova_iomap_begin_nolock,
	.iomap_end = nova_iomap_end,
};

static ssize_t nova_dax_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct inode *inode = iocb->ki_filp->f_mapping->host;
	ssize_t ret;
	INIT_TIMING(read_iter_time);

	if (!iov_iter_count(to))
		return 0;

	NOVA_START_TIMING(read_iter_t, read_iter_time);
	inode_lock_shared(inode);
	ret = dax_iomap_rw(iocb, to, &nova_iomap_ops_nolock);
	inode_unlock_shared(inode);

	file_accessed(iocb->ki_filp);
	NOVA_END_TIMING(read_iter_t, read_iter_time);
	return ret;
}

static int nova_update_iter_csum_parity(struct super_block *sb,
					struct inode *inode, loff_t offset,
					size_t count)
{
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
	unsigned long start_pgoff, end_pgoff;
	int data_bits = nova_inode_blk_shift(sih);
	loff_t end;

	if (data_csum == 0 && data_parity == 0)
		return 0;

	end = offset + count;

	start_pgoff = offset >> data_bits;
	end_pgoff = end >> data_bits;
	if (end & (nova_inode_blk_size(sih) - 1))
		end_pgoff++;

	nova_reset_csum_parity_range(sb, sih, NULL, start_pgoff, end_pgoff, 0,
				     0);

	return 0;
}

static ssize_t nova_dax_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file->f_mapping->host;
	struct super_block *sb = inode->i_sb;
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
	loff_t offset;
	size_t count;
	ssize_t ret;
	INIT_TIMING(write_iter_time);

	NOVA_START_TIMING(write_iter_t, write_iter_time);
	inode_lock(inode);
	ret = generic_write_checks(iocb, from);
	if (ret <= 0)
		goto out_unlock;

	ret = file_remove_privs(file);
	if (ret)
		goto out_unlock;

	ret = file_update_time(file);
	if (ret)
		goto out_unlock;

	count = iov_iter_count(from);
	offset = iocb->ki_pos;

	ret = dax_iomap_rw(iocb, from, &nova_iomap_ops_nolock);
	if (ret > 0 && iocb->ki_pos > i_size_read(inode)) {
		i_size_write(inode, iocb->ki_pos);
		sih->i_size = iocb->ki_pos;
		mark_inode_dirty(inode);
	}

	nova_update_iter_csum_parity(sb, inode, offset, count);

out_unlock:
	inode_unlock(inode);
	if (ret > 0)
		ret = generic_write_sync(iocb, ret);
	NOVA_END_TIMING(write_iter_t, write_iter_time);
	return ret;
}

static ssize_t do_dax_mapping_read(struct file *filp, char __user *buf,
				   size_t len, loff_t *ppos)
{
	struct inode *inode = filp->f_mapping->host;
	struct super_block *sb = inode->i_sb;
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_sb_info *sbi = NOVA_SB(sb);
	struct nova_inode_info_header *sih = &si->header;
	struct nova_file_write_entry *entry;
	struct nova_file_write_entry *entryc, entry_copy;
	pgoff_t index, end_index;
	unsigned long offset;
	loff_t isize, pos;
	size_t copied = 0, error = 0;
	int blocksize_mask = nova_inode_blk_size(sih) - 1;
	int data_bits = nova_inode_blk_shift(sih);
	size_t data_block_size = nova_inode_blk_size(sih);
	int socket;

	INIT_TIMING(fini_delegation_time);

	int cond_cnt = 0;
	long issued_cnt[NOVA_MAX_SOCKET];
	struct nova_notifyer completed_cnt[NOVA_MAX_SOCKET];

	bool is_dele = false;

	memset(issued_cnt, 0, sizeof(long) * NOVA_MAX_SOCKET);
	memset(completed_cnt, 0,
	       sizeof(struct nova_notifyer) * NOVA_MAX_SOCKET);

	pos = *ppos;
	index = pos >> data_bits;
	offset = pos & blocksize_mask;

	nova_dbg_verbose("%s: pos: %lld, index: %ld, offset: %ld\n", __func__,
			 pos, index, offset);

	if (!access_ok(buf, len)) {
		error = -EFAULT;
		goto out;
	}

	isize = i_size_read(inode);
	if (!isize)
		goto out;

	nova_dbg_verbose("%s: inode %lu, offset %lld, count %lu, size %lld\n",
			 __func__, inode->i_ino, pos, len, isize);

	if (len > isize - pos)
		len = isize - pos;

	nova_dbg_verbose("%s: set len to %lu\n", __func__, len);

	if (len <= 0)
		goto out;

	entryc = (metadata_csum == 0) ? entry : &entry_copy;

	end_index = (isize - 1) >> data_bits;
	do {
		unsigned long nr, left;
		unsigned long nvmm;
		void *dax_mem = NULL;
		int zero = 0;

		/* nr is the maximum number of bytes to copy from this page */
		if (index >= end_index) {
			if (index > end_index)
				goto out;
			nr = ((isize - 1) & blocksize_mask) + 1;
			if (nr <= offset)
				goto out;
		}

		entry = nova_get_write_entry(sb, sih, index);
		if (unlikely(entry == NULL)) {
			nova_dbg_verbose(
				"Required extent not found: pgoff %#lx, inode size %lld\n",
				index, isize);
			nr = data_block_size - offset;
			if (nr > len - copied)
				nr = len - copied;
			left = __clear_user(buf + copied, nr);
			if (left) {
				nova_dbg(
					"%s ERROR!: fill zero bytes %#lx, left %#lx\n",
					__func__, nr, left);
				error = -EFAULT;
				goto out;
			}
			copied += (nr - left);
			offset += (nr - left);
			index += offset >> data_bits;
			offset &= ~blocksize_mask;
			continue;
		}

		/*
		* Do all verify om recovery path
		*/
		if (metadata_csum == 0)
			entryc = entry;
#if NOVA_VERIFY_ENTRY_CSUM
		else if (!nova_verify_entry_csum(sb, entry, entryc))
			return -EIO;
#else
#if NOVA_ENTRY_IN_MEM
		else if (!nova_get_entry_copy(sb, entry, entryc)) {
			nova_err(sb, "%s: copy entry failed!\n", __func__);
			return -EIO;
		}
#else
		entryc = entry;
#endif
#endif

		if (index < entryc->pgoff ||
		    index - entryc->pgoff >= entryc->num_pages) {
			nova_err(
				sb,
				"%s ERROR: %#lx, entry pgoff %llu, num %u, blocknr %llu\n",
				__func__, index, entry->pgoff, entry->num_pages,
				entry->blocknr);
			return -EINVAL;
		}
		// do read in block granularity
		// nr is keep in the same entry range
		if (entryc->reassigned == 0) {
			nr = (entryc->num_pages - (index - entryc->pgoff)) *
			     data_block_size;
		} else {
			nr = data_block_size;
		}

		nr = nr - offset;
		if (nr > len - copied)
			nr = len - copied;

		while (nr + offset >= data_block_size) {
			nova_dbg_verbose("%s: nr: %lu, copy a data block\n",
					 __func__, nr);
			nvmm = get_nvmm(sb, sih, entryc, index);
			socket = nova_block_to_socket(sbi, nvmm,
						      sih->i_blk_type, 0);
			nova_dbg_verbose(
				"%s: tail block nvmm: %#lx, socket: %d, index: %lu\n",
				__func__, nvmm, socket, index);
			dax_mem = nova_get_virt_addr_from_offset(
				sb,
				nova_get_block_off(sb, nvmm, sih->i_blk_type,
						   0),
				0);

			nova_dbg_verbose(
				"%s: entryc_num_pages: %d, entryc_pgoff: %#llx, index: %#lx, nr: %#lx, offset: %#lx\n",
				__func__, entryc->num_pages, entryc->pgoff,
				index, nr, offset);

			left = do_nova_nvmm_read(
				sb, buf + copied, dax_mem + offset,
				data_block_size - offset, 0, socket, zero,
				issued_cnt, completed_cnt,
				len >= NOVA_READ_WAIT_THRESHOLD, &is_dele);

			if (left) {
				nova_dbg("%s ERROR!: bytes %#lx, left %#lx\n",
					 __func__, nr, left);
				error = -EFAULT;
				goto out;
			}
			copied += (data_block_size - offset);
			nr -= (data_block_size - offset);
			offset += (data_block_size - offset);
			index += offset >> data_bits;
			offset &= blocksize_mask;
		}
		if (nr != 0) {
			// tail block
			nova_dbg_verbose("%s: nr: %lu, tail block\n", __func__,
					 nr);
			nvmm = get_nvmm(sb, sih, entryc, index);
			socket = nova_block_to_socket(sbi, nvmm,
						      sih->i_blk_type, 0);
			nova_dbg_verbose(
				"%s: tail block nvmm: %#lx, socket: %d, index: %lu\n",
				__func__, nvmm, socket, index);
			dax_mem = nova_get_virt_addr_from_offset(
				sb,
				nova_get_block_off(sb, nvmm, sih->i_blk_type,
						   0),
				0);

			left = do_nova_nvmm_read(
				sb, buf + copied, dax_mem + offset, nr, 0,
				socket, zero, issued_cnt, completed_cnt,
				len >= NOVA_READ_WAIT_THRESHOLD, &is_dele);

			if (left) {
				nova_dbg("%s ERROR!: bytes %#lx, left %#lx\n",
					 __func__, nr, left);
				error = -EFAULT;
				goto out;
			}
			copied += (nr - left);
			nr -= (nr - left);
			offset += (nr - left);
			index += offset >> data_bits;
			offset &= blocksize_mask;
		}

		cond_cnt++;
		if (cond_cnt >= NOVA_APP_RING_BUFFER_CHECK_COUNT) {
			cond_cnt = 0;
			if (need_resched())
				cond_resched();
		}
	} while (copied < len);

out:
	if (is_dele) {
		NOVA_START_TIMING(fini_delegation_r_t, fini_delegation_time);
		nova_complete_delegation(issued_cnt, completed_cnt);
		NOVA_END_TIMING(fini_delegation_r_t, fini_delegation_time);
	}
	*ppos = pos + copied;
	if (filp)
		file_accessed(filp);

	NOVA_STATS_ADD(read_bytes, copied);

	nova_dbg_verbose("%s returned %zu\n", __func__, copied);
	return copied ? copied : error;
}

/*
 * Wrappers. We need to use the rcu read lock to avoid
 * concurrent truncate operation. No problem for write because we held
 * lock.
 */
static ssize_t nova_dax_file_read(struct file *filp, char __user *buf,
				  size_t len, loff_t *ppos)
{
	struct inode *inode = filp->f_mapping->host;
	ssize_t res;
	INIT_TIMING(dax_read_time);

	NOVA_START_TIMING(dax_read_t, dax_read_time);
	inode_lock_shared(inode);
	res = do_dax_mapping_read(filp, buf, len, ppos);
	inode_unlock_shared(inode);
	NOVA_END_TIMING(dax_read_t, dax_read_time);
	return res;
}

/*
 * Perform a COW write.   Must hold the inode lock before calling.
 */
static ssize_t do_nova_cow_file_write(struct file *filp, const char __user *buf,
				      size_t len, loff_t *ppos)
{
	struct address_space *mapping = filp->f_mapping;
	struct inode *inode = mapping->host;
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
	struct super_block *sb = inode->i_sb;
	struct nova_sb_info *sbi = NOVA_SB(sb);
	struct nova_inode *pi, inode_copy;
	struct nova_file_write_entry entry_data;
	struct nova_inode_update update;
	char *ubuf_copy, *ubuf_copy_src = NULL;
	ssize_t written = 0;
	loff_t pos;
	size_t count, offset, copied;
	unsigned long start_blk, num_blocks;
	unsigned long total_blocks;
	unsigned long blocknr = 0;
	unsigned int data_bits;
	int allocated = 0;
	void *kmem;
	u64 file_size;
	size_t bytes;
	long status = 0;
	INIT_TIMING(cow_write_time);
	INIT_TIMING(fini_delegation_time);
	INIT_TIMING(bd_write_time);
	INIT_TIMING(bd_data_csum_time);
	INIT_TIMING(bd_meta_write_time);
	unsigned long step = 0;
	ssize_t ret;
	u64 begin_tail = 0;
	int try_inplace = 0;
	u64 epoch_id;
	u32 time;
	unsigned long irq_flags = 0;
	int blocksize_mask;
	int i, socket;
	size_t head, tail;
	size_t aligned_num_blocks;
	unsigned long blocknr_loop;
	bool is_dele = false;
	bool try_do_dele;

	int cond_cnt = 0;
	long issued_cnt[NOVA_MAX_SOCKET];
	struct nova_notifyer completed_cnt[NOVA_MAX_SOCKET];

	if (len == 0)
		return 0;

	NOVA_START_TIMING(do_cow_write_t, cow_write_time);
	NOVA_START_META_TIMING(bd_cow_write_t, bd_write_time);

	if (!access_ok(buf, len)) {
		ret = -EFAULT;
		goto out;
	}

	atomic_inc(&sbi->write_requests);

	if (atomic_read(&sbi->write_requests) > NOVA_DELE_START_THREADS) {
		try_do_dele = true;
	} else {
		try_do_dele = false;
	}

	/*
	 * let user buffer to be kernel thread shared and 64-byte aligned
	 */

	if (try_do_dele) {
#if NOVA_KERNEL_COPY_USER_BUFFER
		ubuf_copy = kmalloc(len + 64, GFP_KERNEL);
		ubuf_copy_src = ubuf_copy;
		if (ubuf_copy == NULL) {
			nova_err(sb,
				 "%s: user kernel buffer allocation error\n",
				 __func__);
			return -ENOMEM;
		}
		ubuf_copy = (char *)(((unsigned long)ubuf_copy + 63) & ~0x3f);
		ret = copy_from_user(ubuf_copy, buf, len);
		if (ret) {
			nova_dbg("%s: copy_from_user failed %ld\n", __func__,
				 ret);
			ret = -EFAULT;
			goto out;
		}
#endif

		memset(issued_cnt, 0, sizeof(long) * NOVA_MAX_SOCKET);
		memset(completed_cnt, 0,
		       sizeof(struct nova_notifyer) * NOVA_MAX_SOCKET);
	} else {
		ubuf_copy = (char *)buf;
	}

	pos = *ppos;

	if (filp->f_flags & O_APPEND)
		pos = i_size_read(inode);

	count = len;

	pi = nova_get_virt_addr_from_offset(sb, sih->pi_addr, 1);

/* nova_inode tail pointer will be updated and we make sure all other
	 * inode fields are good before checksumming the whole structure
	 */
// if (nova_check_inode_integrity(sb, sih->ino, sih->pi_addr,
// 			       sih->alter_pi_addr, &inode_copy,
// 			       0) < 0) {
// 	ret = -EIO;
// 	goto out;
// }
/* Do integrity checking on recovery. */
#if NOVA_INODE_IN_MEM
	if (nova_copy_inode(sb, sih->ino, sih->pi_addr, sih->alter_pi_addr,
			    &inode_copy) < 0) {
		ret = -EIO;
		goto out;
	}
#endif

	data_bits = nova_inode_blk_shift(sih);
	blocksize_mask = nova_inode_blk_size(sih) - 1;
	offset = pos & blocksize_mask;
	num_blocks = ((count + offset - 1) >> data_bits) + 1;
	total_blocks = num_blocks;
	start_blk = pos >> data_bits;

	if (nova_check_overlap_vmas(sb, sih, start_blk, num_blocks)) {
		nova_dbg_verbose(
			"COW write overlaps with vma: inode %lu, pgoff %#lx, %#lx blocks\n",
			inode->i_ino, start_blk, num_blocks);
		NOVA_STATS_ADD(cow_overlap_mmap, 1);
		try_inplace = 1;
		ret = -EACCES;
		goto out;
	}

	/* offset in the actual block size block */

	ret = file_remove_privs(filp);
	if (ret)
		goto out;

	inode->i_mtime = inode_set_ctime_current(inode);
	time = inode->i_mtime.tv_sec;

	epoch_id = nova_get_epoch_id(sb);

	nova_dbg_verbose(
		"%s: epoch_id %llu, inode %lu, offset %lld, count %lu, numblocks: %#lx\n",
		__func__, epoch_id, inode->i_ino, pos, count, num_blocks);
	update.tail = sih->log_tail;
	update.alter_tail = sih->alter_log_tail;
	is_dele = false;
	while (num_blocks > 0) {
		offset = pos & blocksize_mask;
		start_blk = pos >> data_bits;

		/* don't zero-out the allocated blocks */
		allocated = nova_new_data_blocks(sb, sih, &blocknr, start_blk,
						 num_blocks, ALLOC_NO_INIT,
						 ANY_CPU, ALLOC_FROM_HEAD);

		nova_dbg_verbose("%s: alloc %d blocks from %#lx, to %#lx\n",
				 __func__, allocated, blocknr,
				 blocknr + allocated - 1);

		if (allocated <= 0) {
			nova_dbg("%s alloc blocks failed %d\n", __func__,
				 allocated);
			ret = allocated;
			goto out;
		}

		step++;
		bytes = nova_inode_blk_size(sih) * allocated - offset;
		if (bytes > count)
			bytes = count;

		head = tail = 0;
		if (offset || ((offset + bytes) & (PAGE_SIZE - 1)) != 0) {
			/*
			 * Do COW. Copy the data from the old block to the new block.
			 * If the old block is not persent, fill zero to the new block.
			 * Copy user data to the new block at the same time.
			 */

#if NOVA_KERNEL_COPY_USER_BUFFER
			ret = nova_handle_head_tail_blocks(
				sb, inode, pos, bytes, blocknr, ubuf_copy,
				&head, &tail, 0, issued_cnt, completed_cnt,
				try_do_dele, &is_dele);
#else
			ret = nova_handle_head_tail_blocks(
				sb, inode, pos, bytes, blocknr, (char *)buf,
				&head, &tail, 0, issued_cnt, completed_cnt,
				try_do_dele, &is_dele);
#endif

			if (ret)
				goto out;
		}

		nova_dbg_verbose("%s: copy head: %lu, tail: %lu\n", __func__,
				 head, tail);

		aligned_num_blocks = (bytes - head - tail) >> PAGE_SHIFT;
		nova_dbg_verbose("%s: last page aligned blocks: %lu\n",
				 __func__, aligned_num_blocks);

		// move blocknr to the start of contiguous blocks
		if (head) {
			blocknr += 1;
		}
		/* Now copy from user buf (in inode data block granularity) */
		copied = 0;
		if (sbi->data_nvm_num == 1) {
			// all data allocate to one socket, write once
			kmem = nova_get_virt_addr_from_offset(
				inode->i_sb,
				nova_get_block_off(sb, blocknr, sih->i_blk_type,
						   0),
				0);
			socket = nova_block_to_socket(sbi, blocknr,
						      sih->i_blk_type, 0);

#if NOVA_KERNEL_COPY_USER_BUFFER
			copied += do_nova_nvmm_write(
				sb, kmem, (void *)(ubuf_copy + head),
				aligned_num_blocks << PAGE_SHIFT, 0, socket, 0,
				1, 0, issued_cnt, completed_cnt,
				len >= NOVA_WRITE_WAIT_THRESHOLD, try_do_dele,
				&is_dele);
#else
			copied += do_nova_nvmm_write(
				sb, kmem, (void *)(buf + head),
				aligned_num_blocks << PAGE_SHIFT, 0, socket, 0,
				1, 0, issued_cnt, completed_cnt,
				len >= NOVA_WRITE_WAIT_THRESHOLD, try_do_dele,
				&is_dele);
#endif
		} else {
			// data striped, write in block granularity
			for (i = 0; i < aligned_num_blocks; i++) {
				/* Now copy from user buf */
				blocknr_loop = blocknr + i;
				nova_dbg_verbose("%s: copy to blocknr: %#lx\n",
						 __func__, blocknr_loop);
				kmem = nova_get_virt_addr_from_offset(
					inode->i_sb,
					nova_get_block_off(sb, blocknr_loop,
							   sih->i_blk_type, 0),
					0);
				socket = nova_block_to_socket(
					sbi, blocknr_loop, sih->i_blk_type, 0);

#if NOVA_KERNEL_COPY_USER_BUFFER
				copied += do_nova_nvmm_write(
					sb, kmem,
					(void *)(ubuf_copy + head +
						 PAGE_SIZE * i),
					PAGE_SIZE, 0, socket, 0, 1, 0,
					issued_cnt, completed_cnt,
					len >= NOVA_WRITE_WAIT_THRESHOLD,
					try_do_dele, &is_dele);
#else
				copied += do_nova_nvmm_write(
					sb, kmem,
					(void *)(buf + head + PAGE_SIZE * i),
					PAGE_SIZE, 0, socket, 0, 1, 0,
					issued_cnt, completed_cnt,
					len >= NOVA_WRITE_WAIT_THRESHOLD,
					try_do_dele, &is_dele);
#endif
			}
		}
		if (copied) {
			nova_err(sb, "%s: delegation failed to copy all\n",
				 __func__);
			ret = -EFAULT;
			goto out;
		}
		if (data_csum == 0 && data_parity == 0 && is_dele) {
			NOVA_START_TIMING(fini_delegation_w_t,
					  fini_delegation_time);
			nova_complete_delegation(issued_cnt, completed_cnt);
			NOVA_END_TIMING(fini_delegation_w_t,
					fini_delegation_time);
		}
		// restore blocknr
		if (head) {
			blocknr -= 1;
		}
		copied = bytes;

		// we do data csum on cow write, no need is_dele check
		if (is_dele && (data_csum > 0 || data_parity > 0)) {
			/* calculate data checksum and write csum to pmem */
			NOVA_START_META_TIMING(bd_data_csum_t,
					       bd_data_csum_time);
#if NOVA_KERNEL_COPY_USER_BUFFER
			ret = nova_protect_file_data(sb, inode, pos, bytes,
						     ubuf_copy, blocknr);
#else
			ret = nova_protect_file_data(sb, inode, pos, bytes,
						     (char *)buf, blocknr);
#endif
			NOVA_END_META_TIMING(bd_data_csum_t, bd_data_csum_time);
			if (ret)
				goto out;
		}

		if (pos + copied > inode->i_size)
			file_size = cpu_to_le64(pos + copied);
		else
			file_size = cpu_to_le64(inode->i_size);

		NOVA_START_META_TIMING(bd_meta_write_t, bd_meta_write_time);
		/* init log entry */
		nova_init_file_write_entry(sb, sih, &entry_data, epoch_id,
					   start_blk, allocated, blocknr, time,
					   file_size);

/* write entry to pm; Jm and M */
/* may do gc here */
#if NOVA_INODE_IN_MEM
		ret = nova_append_file_write_entry(sb, pi, &inode_copy, inode,
						   &entry_data, &update);
#else
		ret = nova_append_file_write_entry(sb, pi, NULL, inode,
						   &entry_data, &update);
#endif
		NOVA_END_META_TIMING(bd_meta_write_t, bd_meta_write_time);

		if (ret) {
			nova_dbg("%s: append inode entry failed\n", __func__);
			ret = -ENOSPC;
			goto out;
		}

		nova_dbg_verbose("Write: %p, %#lx\n", kmem, copied);
		if (copied > 0) {
			status = copied;
			written += copied;
			pos += copied;
#if NOVA_KERNEL_COPY_USER_BUFFER
			ubuf_copy += copied;
#else
			buf += copied;
#endif
			count -= copied;
			num_blocks -= allocated;
		}
		if (unlikely(copied != bytes)) {
			nova_dbg("%s ERROR!: %p, bytes %#lx, copied %#lx\n",
				 __func__, kmem, bytes, copied);
			if (status >= 0)
				status = -EFAULT;
		}
		if (status < 0)
			break;

		if (begin_tail == 0)
			begin_tail = update.curr_entry;

		cond_cnt++;
		if (cond_cnt >= NOVA_APP_RING_BUFFER_CHECK_COUNT) {
			cond_cnt = 0;
			if (need_resched())
				cond_resched();
		}
	}

	sih->i_blocks += (total_blocks << (data_bits - sb->s_blocksize_bits));

	nova_memunlock_inode(sb, pi, &irq_flags);
	NOVA_START_META_TIMING(bd_meta_write_t, bd_meta_write_time);
// update inode (pi->log_tail); like Jc
#if NOVA_INODE_IN_MEM
	nova_update_inode(sb, inode, pi, &inode_copy, &update, 1);
#else
	nova_update_inode(sb, inode, pi, NULL, &update, 1);
#endif
	NOVA_END_META_TIMING(bd_meta_write_t, bd_meta_write_time);
	nova_memlock_inode(sb, pi, &irq_flags);

	/* Free the overlap blocks after the write is committed */
	// ret = nova_reassign_file_tree(sb, sih, begin_tail);
	// if (ret)
	// 	goto out;

	inode->i_blocks = sih->i_blocks;

	ret = written;
	NOVA_STATS_ADD(cow_write_breaks, step);
	nova_dbg_verbose("blocks: %llu, %#lx\n", inode->i_blocks,
			 sih->i_blocks);

	*ppos = pos;
	if (pos > inode->i_size) {
		i_size_write(inode, pos);
		sih->i_size = pos;
	}

out:
	if (is_dele) {
		NOVA_START_TIMING(fini_delegation_w_t, fini_delegation_time);
		nova_complete_delegation(issued_cnt, completed_cnt);
		NOVA_END_TIMING(fini_delegation_w_t, fini_delegation_time);
	}

#if NOVA_CKPT
	struct nova_ckpt_entry ckpt_entry;
	ckpt_entry.ino = sih->ino;
	ckpt_entry.latest_trans_id = sih->trans_id;
	nova_ckpt_send_request(&sbi->ckpt->ring, &ckpt_entry,
			       sizeof(struct nova_ckpt_entry));
#endif

	sih->trans_id++;

	if (ret < 0)
		nova_cleanup_incomplete_write(sb, sih, blocknr, allocated,
					      begin_tail, update.tail);

	atomic_dec(&sbi->write_requests);
	NOVA_END_META_TIMING(bd_cow_write_t, bd_write_time);
	NOVA_END_TIMING(do_cow_write_t, cow_write_time);
	NOVA_STATS_ADD(cow_write_bytes, written);
	if (ubuf_copy_src)
		kfree(ubuf_copy_src);

	if (try_inplace)
		return do_nova_inplace_file_write(filp, buf, len, ppos);

	return ret;
}

/*
 * Acquire locks and perform COW write.
 */
ssize_t nova_cow_file_write(struct file *filp, const char __user *buf,
			    size_t len, loff_t *ppos)
{
	struct address_space *mapping = filp->f_mapping;
	struct inode *inode = mapping->host;
	int ret;
	INIT_TIMING(time);

	if (len == 0)
		return 0;

	NOVA_START_TIMING(cow_write_t, time);

	sb_start_write(inode->i_sb);
	inode_lock(inode);

#if NOVA_OPTIMIZE_APPEND
	/*
	 * If we find that the pos is pointing to the end of file,
	 * we need to do an optimized append write (inplace append).
	 */
	if (*ppos == i_size_read(inode)) {
		nova_dbg_verbose("%s: pos = isize, do inplace append\n",
				 __func__);
		ret = do_nova_inplace_file_write(filp, buf, len, ppos);
	} else {
		nova_dbg_verbose("%s: pos: %llu, size: %llu\n", __func__, *ppos,
				 i_size_read(inode));
		ret = do_nova_cow_file_write(filp, buf, len, ppos);
	}
#else
	ret = do_nova_cow_file_write(filp, buf, len, ppos);
#endif

	inode_unlock(inode);
	sb_end_write(inode->i_sb);

	NOVA_END_TIMING(cow_write_t, time);
	return ret;
}

static ssize_t nova_dax_file_write(struct file *filp, const char __user *buf,
				   size_t len, loff_t *ppos)
{
	struct address_space *mapping = filp->f_mapping;
	struct inode *inode = mapping->host;

	if (test_opt(inode->i_sb, DATA_COW))
		return nova_cow_file_write(filp, buf, len, ppos);
	else
		return nova_inplace_file_write(filp, buf, len, ppos);
}

static ssize_t do_nova_dax_file_write(struct file *filp, const char __user *buf,
				      size_t len, loff_t *ppos)
{
	struct address_space *mapping = filp->f_mapping;
	struct inode *inode = mapping->host;

	if (test_opt(inode->i_sb, DATA_COW)) {
#if NOVA_OPTIMIZE_APPEND
		/*
	 	 * If we find that the pos is pointing to the end of file,
	 	 * we need to do an optimized append write (inplace append).
	 	 */
		if (*ppos == i_size_read(inode)) {
			nova_dbg_verbose("%s: pos = isize, do inplace append\n",
					 __func__);
			return do_nova_inplace_file_write(filp, buf, len, ppos);
		} else {
			nova_dbg_verbose("%s: pos: %llu, size: %llu\n",
					 __func__, *ppos, i_size_read(inode));
			return do_nova_cow_file_write(filp, buf, len, ppos);
		}
#else
		return do_nova_cow_file_write(filp, buf, len, ppos);
#endif

	} else
		return do_nova_inplace_file_write(filp, buf, len, ppos);
}

static int nova_dax_file_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct inode *inode = file->f_mapping->host;

	file_accessed(file);

	vm_flags_set(vma, VM_MIXEDMAP | VM_HUGEPAGE);

	vma->vm_ops = &nova_dax_vm_ops;

	nova_insert_write_vma(vma);

	nova_dbg_mmap4k(
		"[%s:%d] inode %lu, MMAP 4KPAGE vm_start(0x%lx), vm_end(0x%lx), vm pgoff %#lx, %#lx blocks, vm_flags(0x%lx), vm_page_prot(0x%lx)\n",
		__func__, __LINE__, inode->i_ino, vma->vm_start, vma->vm_end,
		vma->vm_pgoff, (vma->vm_end - vma->vm_start) >> PAGE_SHIFT,
		vma->vm_flags, pgprot_val(vma->vm_page_prot));

	return 0;
}

/*
 * The difference between nova_wrap_file_ops is read/write_iter.
 * We hope to do less mmap io. So we don't use this dax_ops.
 */
const struct file_operations nova_dax_file_operations = {
	.llseek = nova_llseek,
	.read = nova_dax_file_read,
	.write = nova_dax_file_write,
	.read_iter = nova_dax_read_iter,
	.write_iter = nova_dax_write_iter,
	.mmap = nova_dax_file_mmap,
	.mmap_supported_flags = MAP_SYNC,
	.open = nova_open,
	.fsync = nova_fsync,
	.flush = nova_flush,
	.unlocked_ioctl = nova_ioctl,
	.fallocate = nova_fallocate,
#ifdef CONFIG_COMPAT
	.compat_ioctl = nova_compat_ioctl,
#endif
};

static ssize_t nova_wrap_rw_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	struct file *filp = iocb->ki_filp;
	struct inode *inode = filp->f_mapping->host;
	ssize_t ret = -EIO;
	ssize_t written = 0;
	unsigned long seg;
	unsigned long nr_segs = iter->nr_segs;
	const struct iovec *iv;
	INIT_TIMING(wrap_iter_time);

	NOVA_START_TIMING(wrap_iter_t, wrap_iter_time);

	nova_dbg_verbose("%s %s: %#lx segs\n", __func__,
			 iov_iter_rw(iter) == READ ? "read" : "write", nr_segs);

	if (iov_iter_rw(iter) == WRITE) {
		sb_start_write(inode->i_sb);
		inode_lock(inode);
	} else {
		inode_lock_shared(inode);
	}

	iv = iter_iov(iter);
	for (seg = 0; seg < nr_segs; seg++) {
		if (iov_iter_rw(iter) == READ) {
			ret = do_dax_mapping_read(filp, iv->iov_base,
						  iv->iov_len, &iocb->ki_pos);
		} else if (iov_iter_rw(iter) == WRITE) {
			ret = do_nova_dax_file_write(
				filp, iv->iov_base, iv->iov_len, &iocb->ki_pos);
		} else {
			BUG();
		}
		if (ret < 0)
			goto err;

		if (iter->count > iv->iov_len)
			iter->count -= iv->iov_len;
		else
			iter->count = 0;

		written += ret;
		iter->nr_segs--;
		iv++;
	}
	ret = written;
err:
	if (iov_iter_rw(iter) == WRITE) {
		inode_unlock(inode);
		sb_end_write(inode->i_sb);
	} else {
		inode_unlock_shared(inode);
	}

	NOVA_END_TIMING(wrap_iter_t, wrap_iter_time);
	return ret;
}

/* Wrap read/write_iter for DP, CoW and WP */
const struct file_operations nova_wrap_file_operations = {
	.llseek = nova_llseek,
	.read = nova_dax_file_read,
	.write = nova_dax_file_write,
	.read_iter = nova_wrap_rw_iter,
	.write_iter = nova_wrap_rw_iter,
	.mmap = nova_dax_file_mmap,
	.get_unmapped_area = thp_get_unmapped_area,
	.open = nova_open,
	.fsync = nova_fsync,
	.flush = nova_flush,
	.unlocked_ioctl = nova_ioctl,
	.fallocate = nova_fallocate,
#ifdef CONFIG_COMPAT
	.compat_ioctl = nova_compat_ioctl,
#endif
};

const struct inode_operations nova_file_inode_operations = {
	.setattr = nova_notify_change,
	.getattr = nova_getattr,
	.get_acl = NULL,
};
