/*
 * BRIEF DESCRIPTION
 *
 * DAX file operations.
 *
 * Copyright 2015-2016 Regents of the University of California,
 * UCSD Non-Volatile Systems Lab, Andiry Xu <jix024@cs.ucsd.edu>
 * Copyright 2012-2013 Intel Corporation
 * Copyright 2009-2011 Marco Stornelli <marco.stornelli@gmail.com>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/module.h>
#include <linux/buffer_head.h>
#include <linux/cpufeature.h>
#include <asm/pgtable.h>
#include <linux/version.h>
#include "nova.h"

/*
 * Only copy pm block here, do user buffer copy in caller.
 *
 * @entry: nvm block entry, search the old block to copy. If NULL, fill zero.
 * @offset: If head, offset is 0, else offset is the place to start copy tail block
 * @kmem: new cow nvm block
 * @len: copy length
 * @socket: socket id where cow block is located
 */
static inline int nova_handle_partial_block(
	struct super_block *sb, struct nova_inode_info_header *sih,
	struct nova_file_write_entry *entry, unsigned long index, size_t offset,
	void *kmem, size_t len, int socket, long *issued_cnt,
	struct nova_notifyer *completed_cnt, bool try_do_dele, bool *is_dele)
{
	struct nova_file_write_entry *entryc, entry_copy;
	void *ptr;
	unsigned long nvmm;
	unsigned long irq_flags = 0;
	int ret = 0;
	size_t left = 0;

	nova_memunlock_block(sb, kmem, &irq_flags);
	if (entry == NULL) {
		/* Fill zero */
		nova_dbg_verbose("%s: entry is null, fill 0\n", __func__);
		left = do_nova_nvmm_write(sb, kmem + offset, NULL, len, 0,
					  socket, 1, support_clwb, 0,
					  issued_cnt, completed_cnt, 0,
					  try_do_dele, is_dele);
		if (left) {
			nova_dbg_verbose("%s: fill zero block left: %ld\n",
					 __func__, left);
		}
	} else {
		/* Copy from original block */
		if (metadata_csum == 0)
			entryc = entry;
		else {
			entryc = &entry_copy;
#if NOVA_VERIFY_ENTRY_CSUM
			if (!nova_verify_entry_csum(sb, entry, entryc))
				return -EIO;
#else
#if NOVA_ENTRY_IN_MEM
			if (!nova_get_entry_copy(sb, entry, entryc)) {
				nova_err(sb, "%s: copy entry failed!\n",
					 __func__);
				return -EIO;
			}
#else
			entryc = entry;
#endif
#endif
		}

		nvmm = get_nvmm(sb, sih, entry, index);
		ptr = nova_get_virt_addr_from_offset(
			sb, nova_get_block_off(sb, nvmm, sih->i_blk_type, 0),
			0);

		if (ptr != NULL) {
#if NOVA_KERNEL_COPY_USER_BUFFER
			left = do_nova_nvmm_write(sb, kmem + offset,
						  ptr + offset, len, 0, socket,
						  0, support_clwb, 0,
						  issued_cnt, completed_cnt, 0,
						  try_do_dele, is_dele);
#else
			unsigned long irq_flags = 0;
			INIT_TIMING(memcpy_time);
			nova_memunlock_range(sb, kmem + offset, len,
					     &irq_flags);
			NOVA_START_TIMING(memcpy_w_nvmm_t, memcpy_time);
			// NOVA_START_META_TIMING(bd_memcpy_w_t, memcpy_time);
			left = memcpy_to_pmem_nocache(kmem + offset,
						      ptr + offset, len);
			// NOVA_END_META_TIMING(bd_memcpy_w_t, memcpy_time);
			NOVA_END_TIMING(memcpy_w_nvmm_t, memcpy_time);
			nova_memlock_range(sb, kmem + offset, len, &irq_flags);
#endif
			if (left) {
				nova_dbg_verbose("%s: copy block left: %ld\n",
						 __func__, left);
			}
		} else {
			BUG();
		}
	}
	nova_memlock_block(sb, kmem, &irq_flags);
	if (left) {
		ret = -EIO;
	}
	return ret;
}

/*
 * Fill the new start/end block from original blocks.
 * Do nothing if fully covered; copy if original blocks present;
 * Fill zero otherwise.
 */
int nova_handle_head_tail_blocks(struct super_block *sb, struct inode *inode,
				 loff_t pos, size_t count,
				 unsigned long blocknr, void *ubuf_copy,
				 size_t *head, size_t *tail, int append,
				 long *issued_cnt,
				 struct nova_notifyer *completed_cnt,
				 bool try_do_dele, bool *is_dele)
{
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
	size_t offset, eblk_offset;
	unsigned long start_blk, end_blk, num_blocks;
	struct nova_file_write_entry *entry;
	int data_bits = PAGE_SHIFT;
	int data_block_size = PAGE_SIZE;
	INIT_TIMING(partial_time);
	int ret = 0;
	void *kmem = NULL;
	off_t ubuf_off;
	size_t bytes = 0;
	int socket;
	bool inner_is_dele = false;
	int head_eq_tail = 0;

	NOVA_START_TIMING(partial_block_t, partial_time);
	/* offset in the actual block size block */
	offset = pos & (data_block_size - 1);
	num_blocks = ((count + offset - 1) >> data_bits) + 1;
	start_blk = pos >> data_bits;
	end_blk = start_blk + num_blocks - 1;
	if (start_blk == end_blk)
		head_eq_tail = 1;

	nova_dbg_verbose("%s: %#lx blocks, head equal tail: %d\n", __func__,
			 num_blocks, head_eq_tail);
	/* We avoid zeroing the alloc'd range, which is going to be overwritten
   * by this system call anyway
   */
	nova_dbg_verbose("%s: start offset %lu start blk %#lx\n", __func__,
			 offset, start_blk);
	if (offset != 0) {
		nova_dbg_verbose("%s: head blocknr: %#lx\n", __func__, blocknr);
		kmem = nova_get_virt_addr_from_offset(
			inode->i_sb,
			nova_get_block_off(sb, blocknr, sih->i_blk_type, 0), 0);
		socket = nova_block_to_socket(NOVA_SB(sb), blocknr,
					      sih->i_blk_type, 0);
		if (!append) {
			/* copy [start, offset] from old block(or fill 0) to new cow block */
			entry = nova_get_write_entry(sb, sih, start_blk);
			ret = nova_handle_partial_block(
				sb, sih, entry, start_blk, 0, kmem, offset,
				socket, issued_cnt, completed_cnt, try_do_dele,
				&inner_is_dele);
			if (ret < 0)
				return ret;
		}

		// copy user data to the new block
		ubuf_off = 0;
		if (count > data_block_size - offset) {
			// count larger than a block
			bytes = data_block_size - offset;
			nova_dbg_verbose("%s: partial copy size: %#lx\n",
					 __func__, bytes);
		} else {
			bytes = count;
		}
		ret = do_nova_nvmm_write(sb, kmem + offset,
					 (void *)(ubuf_copy + ubuf_off), bytes,
					 0, socket, 0, support_clwb, 0,
					 issued_cnt, completed_cnt, 0,
					 try_do_dele, &inner_is_dele);
		if (ret < 0)
			return ret;

		*head = bytes;
	}

	eblk_offset = (pos + count) & (data_block_size - 1);
	nova_dbg_verbose("%s: end offset %lu, end blk %#lx\n", __func__,
			 eblk_offset, end_blk);
	if (eblk_offset != 0) {
		// copy user buffer to the new cow block
		blocknr = blocknr + (num_blocks - 1) *
					    nova_get_numblocks(sih->i_blk_type);
		nova_dbg_verbose("%s: tail blocknr: %#lx\n", __func__, blocknr);
		kmem = nova_get_virt_addr_from_offset(
			inode->i_sb,
			nova_get_block_off(sb, blocknr, sih->i_blk_type, 0), 0);
		ubuf_off = count - eblk_offset;
		socket = nova_block_to_socket(NOVA_SB(sb), blocknr,
					      sih->i_blk_type, 0);
		if (!head_eq_tail || !(*head)) {
			// we have already copy user buffer when head = tail and offset != 0
			nova_dbg_verbose("%s: copy tail user buffer\n",
					 __func__);
			nova_dbg_verbose("%s: ubuf_off: %lu\n", __func__,
					 ubuf_off);
			ret = do_nova_nvmm_write(sb, kmem, ubuf_copy + ubuf_off,
						 eblk_offset, 0, socket, 0,
						 support_clwb, 0, issued_cnt,
						 completed_cnt, 0, try_do_dele,
						 &inner_is_dele);
			if (ret < 0)
				return ret;

			*tail = eblk_offset;
		}
		if (!append) {
			/* copy [end, blk_end] from old block(or fill 0) to new cow block */
			entry = nova_get_write_entry(sb, sih, end_blk);
			ret = nova_handle_partial_block(
				sb, sih, entry, end_blk, eblk_offset, kmem,
				data_block_size - eblk_offset, socket,
				issued_cnt, completed_cnt, try_do_dele,
				&inner_is_dele);

			if (ret < 0)
				return ret;
		}
	}
	NOVA_END_TIMING(partial_block_t, partial_time);
	if (inner_is_dele && is_dele) {
		*is_dele = true;
	}

	return ret;
}

int nova_reassign_file_tree(struct super_block *sb,
			    struct nova_inode_info_header *sih, u64 begin_tail)
{
	void *addr;
	struct nova_file_write_entry *entry;
	struct nova_file_write_entry *entryc, entry_copy;
	u64 curr_p = begin_tail;
	size_t entry_size = sizeof(struct nova_file_write_entry);

	entryc = (metadata_csum == 0) ? entry : &entry_copy;

	while (curr_p && curr_p != sih->log_tail) {
		if (is_last_entry(curr_p, entry_size))
			curr_p = next_log_page(sb, curr_p);

		if (curr_p == 0) {
			nova_err(sb, "%s: File inode %lu log is NULL!\n",
				 __func__, sih->ino);
			return -EINVAL;
		}

		addr = (void *)nova_get_virt_addr_from_offset(sb, curr_p, 1);
		entry = (struct nova_file_write_entry *)addr;

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

		if (nova_get_entry_type(entryc) != FILE_WRITE) {
			nova_dbg("%s: entry type is not write? %d\n", __func__,
				 nova_get_entry_type(entry));
			curr_p += entry_size;
			continue;
		}

		nova_assign_write_entry(sb, sih, entry, entryc, true);
		curr_p += entry_size;
	}

	return 0;
}

int nova_cleanup_incomplete_write(struct super_block *sb,
				  struct nova_inode_info_header *sih,
				  unsigned long blocknr, int allocated,
				  u64 begin_tail, u64 end_tail)
{
	void *addr;
	struct nova_file_write_entry *entry;
	struct nova_file_write_entry *entryc, entry_copy;
	u64 curr_p = begin_tail;
	size_t entry_size = sizeof(struct nova_file_write_entry);

	if (blocknr > 0 && allocated > 0)
		nova_free_data_blocks(sb, sih, blocknr, allocated);

	if (begin_tail == 0 || end_tail == 0)
		return 0;

	entryc = (metadata_csum == 0) ? entry : &entry_copy;

	while (curr_p != end_tail) {
		if (is_last_entry(curr_p, entry_size))
			curr_p = next_log_page(sb, curr_p);

		if (curr_p == 0) {
			nova_err(sb, "%s: File inode %lu log is NULL!\n",
				 __func__, sih->ino);
			return -EINVAL;
		}

		addr = (void *)nova_get_virt_addr_from_offset(sb, curr_p, 1);
		entry = (struct nova_file_write_entry *)addr;

		if (metadata_csum == 0)
			entryc = entry;
		else {
			/* skip entry check here as the entry checksum may not
       * be updated when this is called
       */
			if (memcpy_mcsafe(entryc, entry,
					  sizeof(struct nova_file_write_entry)))
				return -EIO;
		}

		if (nova_get_entry_type(entryc) != FILE_WRITE) {
			nova_dbg("%s: entry type is not write? %d\n", __func__,
				 nova_get_entry_type(entry));
			curr_p += entry_size;
			continue;
		}

		blocknr = entryc->blocknr;
		nova_free_data_blocks(sb, sih, blocknr, entryc->num_pages);
		curr_p += entry_size;
	}

	return 0;
}

void nova_init_file_write_entry(struct super_block *sb,
				struct nova_inode_info_header *sih,
				struct nova_file_write_entry *entry,
				u64 epoch_id, u64 pgoff, int num_pages,
				u64 blocknr, u32 time, u64 file_size)
{
	memset(entry, 0, sizeof(struct nova_file_write_entry));
	entry->entry_type = FILE_WRITE;
	entry->reassigned = 0;
	entry->updating = 0;
	entry->epoch_id = epoch_id;
	entry->trans_id = sih->trans_id;
	entry->pgoff = cpu_to_le64(pgoff);
	entry->num_pages = cpu_to_le32(num_pages);
	entry->invalid_pages = 0;
	entry->blocknr = blocknr;
	entry->mtime = cpu_to_le32(time);

	entry->size = file_size;
}

/*
 * We do user buffer copy on caller.
 * So we don't need to copy user buffer again here.
 *
 * TODO: Block checksum is calculated in 4K granularity.
 */
int nova_protect_file_data(struct super_block *sb, struct inode *inode,
			   loff_t pos, size_t count, char *ubuf_copy,
			   unsigned long blocknr)
{
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
	size_t offset, eblk_offset, bytes;
	unsigned long start_blk, end_blk, num_blocks, nvmm, nvmmoff;
	unsigned long blocksize = PAGE_SIZE;
	unsigned int blocksize_bits = PAGE_SHIFT;
	u8 *blockbuf, *blockptr;
	struct nova_file_write_entry *entry;
	struct nova_file_write_entry *entryc, entry_copy;
	int ret = 0;
	int aligned = 0;
	INIT_TIMING(protect_file_data_time);
	INIT_TIMING(memcpy_time);

	NOVA_START_TIMING(protect_file_data_t, protect_file_data_time);

	offset = pos & (blocksize - 1);
	eblk_offset = (pos + count) & (blocksize - 1);
	num_blocks = ((offset + count - 1) >> blocksize_bits) + 1;
	start_blk = pos >> blocksize_bits;
	end_blk = start_blk + num_blocks - 1;

	nova_dbg_verbose(
		"%s: offset: %#lx, eblk_offset: %#lx, numblocks: %#lx, start_blk: %#lx, end_blk: %#lx\n",
		__func__, offset, eblk_offset, num_blocks, start_blk, end_blk);

	NOVA_START_TIMING(protect_memcpy_t, memcpy_time);
#if NOVA_ONE_COPY && NOVA_KERNEL_COPY_USER_BUFFER
	if (!offset & !eblk_offset) {
		blockbuf = NULL;
		aligned = 1;
		nova_dbg_verbose("%s: aligned\n", __func__);
		goto aligned_copy;
	}
#endif
	blockbuf = kmalloc(blocksize, GFP_KERNEL);
	if (blockbuf == NULL) {
		nova_err(sb, "%s: block buffer allocation error\n", __func__);
		ret = -ENOMEM;
		goto out;
	}

	// resolve head block
	bytes = blocksize - offset;
	if (bytes > count)
		bytes = count;

/* load first block from buf to blockbuf(in memory) */
#if NOVA_KERNEL_COPY_USER_BUFFER
	ret = memcpy_mcsafe(blockbuf + offset, ubuf_copy, bytes);
#else
	ret = copy_from_user(blockbuf + offset, ubuf_copy, bytes);
#endif
	NOVA_END_TIMING(protect_memcpy_t, memcpy_time);
	if (unlikely(ret != 0)) {
		nova_err(
			sb,
			"%s: not all data is copied from user! expect to copy %zu bytes, failed\n",
			__func__, bytes);
		ret = -EFAULT;
		goto out;
	}

	entryc = (metadata_csum == 0) ? entry : &entry_copy;

	if (offset != 0) {
		NOVA_STATS_ADD(protect_head, 1);
		entry = nova_get_write_entry(sb, sih, start_blk);
		if (entry != NULL) {
			/*
			* Do verify on recovery
			*/
			if (metadata_csum == 0)
				entryc = entry;
#if NOVA_VERIFY_ENTRY_CSUM
			else if (!nova_verify_entry_csum(sb, entry, entryc))
				ret = -EIO;
			goto out;
#else
#if NOVA_ENTRY_IN_MEM
			else if (!nova_get_entry_copy(sb, entry, entryc)) {
				nova_err(sb, "%s: copy entry failed!\n",
					 __func__);
				ret = -EIO;
				goto out;
			}
#else
			entryc = entry;
#endif
#endif

			/* make sure data in the partial block head is good */
			nvmm = get_nvmm(sb, sih, entryc, start_blk);
			nvmmoff = nova_get_block_off(sb, nvmm, sih->i_blk_type,
						     0);
			blockptr = (u8 *)nova_get_virt_addr_from_offset(
				sb, nvmmoff, 0);

			/* load data from nvmm to blockbuf */
			ret = memcpy_mcsafe(blockbuf, blockptr, offset);
			if (ret < 0)
				goto out;
		} else {
			/* block not found in radix tree, which means it is a new block, so [0, offset] is 0 */
			memset(blockbuf, 0, offset);
		}

		/* copying existing checksums from nvmm can be even slower than
		* re-computing checksums of a whole block.
			if (data_csum > 0)
			nova_copy_partial_block_csum(sb, sih, entry, start_blk, offset, blocknr, false);
		*/
	}

	if (num_blocks == 1)
		goto eblk;

	/* calculate and write checksum of blockbuf in a block granularity */
	nova_update_block_csum(sb, blocksize, blocknr, blockbuf, 0);

	// make it aligned
	blocknr++;
	pos += bytes;
	ubuf_copy += bytes;
	count -= bytes;
	offset = pos & (blocksize - 1);
	if (offset) {
		nova_err(sb, "%s: BUG: offset should be 0 here\n", __func__);
		ret = -EINVAL;
		goto out;
	}

aligned_copy:
	while (count > blocksize) {
		/* calculate and write checksum of blockbuf in a block granularity */
#if NOVA_KERNEL_COPY_USER_BUFFER
		nova_update_block_csum(sb, blocksize, blocknr, ubuf_copy, 0);
#else
		ret = copy_from_user(blockbuf, ubuf_copy, blocksize);
		if (unlikely(ret != 0)) {
			nova_err(
				sb,
				"%s: not all data is copied from user! expect to copy %zu bytes, failed\n",
				__func__, blocksize);
			ret = -EFAULT;
			goto out;
		}
		nova_update_block_csum(sb, blocksize, blocknr, blockbuf, 0);
#endif

		blocknr++;
		pos += blocksize;
		ubuf_copy += blocksize;
		count -= blocksize;
	}

	if (aligned)
		goto out;

	// last block copy to blockbuf
	bytes = count;
#if NOVA_KERNEL_COPY_USER_BUFFER
	ret = memcpy_mcsafe(blockbuf, ubuf_copy, bytes);
#else
	ret = copy_from_user(blockbuf, ubuf_copy, bytes);
#endif
	if (ret) {
		nova_err(
			sb,
			"%s: not all data is copied from user!  expect to copy last %zu, failed\n",
			__func__, bytes);
		ret = -EFAULT;
		goto out;
	}

eblk:
	if (eblk_offset != 0) {
		NOVA_STATS_ADD(protect_tail, 1);
		entry = nova_get_write_entry(sb, sih, end_blk);
		if (entry != NULL) {
			if (metadata_csum == 0)
				entryc = entry;
#if NOVA_VERIFY_ENTRY_CSUM
			else if (!nova_verify_entry_csum(sb, entry, entryc))
				ret = -EIO;
			goto out;
#else
#if NOVA_ENTRY_IN_MEM
			else if (!nova_get_entry_copy(sb, entry, entryc)) {
				nova_err(sb, "%s: copy entry failed!\n",
					 __func__);
				ret = -EIO;
				goto out;
			}
#else
			entryc = entry;
#endif
#endif

			/* make sure data in the partial block tail is good */
			nvmm = get_nvmm(sb, sih, entryc, end_blk);
			nvmmoff = nova_get_block_off(sb, nvmm, sih->i_blk_type,
						     0);
			blockptr = (u8 *)nova_get_virt_addr_from_offset(
				sb, nvmmoff, 0);

			ret = memcpy_mcsafe(blockbuf + eblk_offset,
					    blockptr + eblk_offset,
					    blocksize - eblk_offset);
			if (ret < 0)
				goto out;
		} else {
			memset(blockbuf + eblk_offset, 0,
			       blocksize - eblk_offset);
		}

		/* copying existing checksums from nvmm can be even slower than
* re-computing checksums of a whole block.
if (data_csum > 0)
nova_copy_partial_block_csum(sb, sih, entry, end_blk,
            eblk_offset, blocknr, true);
*/
	}

	nova_update_block_csum(sb, blocksize, blocknr, blockbuf, 0);

out:
	if (blockbuf != NULL)
		kfree(blockbuf);

	NOVA_END_TIMING(protect_file_data_t, protect_file_data_time);

	return ret;
}

#if NOVA_VERIFY_ENTRY_CSUM
static bool nova_get_verify_entry(struct super_block *sb,
				  struct nova_file_write_entry *entry,
				  struct nova_file_write_entry *entryc,
				  int locked)
{
	int ret = 0;

	if (metadata_csum == 0)
		return true;

	if (locked == 0) {
		/* Someone else may be updating the entry. Skip check */
		ret = memcpy_mcsafe(entryc, entry,
				    sizeof(struct nova_file_write_entry));
		if (ret < 0)
			return false;

		return true;
	}

	return nova_verify_entry_csum(sb, entry, entryc);
}
#else
static bool nova_get_entry(struct super_block *sb,
			   struct nova_file_write_entry *entry,
			   struct nova_file_write_entry *entryc, int locked)
{
	int ret = 0;

	if (metadata_csum == 0)
		return true;

	if (locked == 0) {
		/* Someone else may be updating the entry. Skip check */
		ret = memcpy_mcsafe(entryc, entry,
				    sizeof(struct nova_file_write_entry));
		if (ret < 0)
			return false;
	}

	return nova_get_entry_copy(sb, entry, entryc);
}
#endif

/*
 * Check if there is an existing entry for target page offset.
 * Used for inplace write, direct IO, DAX-mmap and fallocate.
 */
unsigned long nova_check_existing_entry(
	struct super_block *sb, struct inode *inode, unsigned long num_blocks,
	unsigned long start_blk, struct nova_file_write_entry **ret_entry,
	struct nova_file_write_entry *ret_entryc, int check_next, u64 epoch_id,
	int *inplace, int locked)
{
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
	struct nova_file_write_entry *entry;
	struct nova_file_write_entry *entryc;
	unsigned long next_pgoff;
	unsigned long ent_blks = 0;
	INIT_TIMING(check_time);

	NOVA_START_TIMING(check_entry_t, check_time);

	*ret_entry = NULL;
	*inplace = 0;
	entry = nova_get_write_entry(sb, sih, start_blk);

	entryc = (metadata_csum == 0) ? entry : ret_entryc;

	if (entry) {
		if (metadata_csum == 0)
			entryc = entry;
#if NOVA_VERIFY_ENTRY_CSUM
		else if (!nova_get_verify_entry(sb, entry, entryc, locked))
			goto out;
#else
#if NOVA_ENTRY_IN_MEM
		else if (!nova_get_entry(sb, entry, entryc, locked))
			goto out;
#else
		entryc = entry;
#endif
#endif

		*ret_entry = entry;

		nova_dbg_verbose("%s: entry reassigned: %d\n", __func__,
				 entryc->reassigned);

		/* We can do inplace write. Find contiguous blocks */
		if (entryc->reassigned == 0)
			ent_blks =
				entryc->num_pages - (start_blk - entryc->pgoff);
		else
			ent_blks = 1;

		if (ent_blks > num_blocks)
			ent_blks = num_blocks;

		if (entryc->epoch_id == epoch_id)
			*inplace = 1;

	} else if (check_next) {
		nova_dbg_verbose("%s: check next entry\n", __func__);
		/* Possible Hole */
		entry = nova_find_next_entry(sb, sih, start_blk);
		if (entry) {
			if (metadata_csum == 0)
				entryc = entry;
#if NOVA_VERIFY_ENTRY_CSUM
			else if (!nova_get_verify_entry(sb, entry, entryc,
							locked))
				goto out;
#else
#if NOVA_ENTRY_IN_MEM
			else if (!nova_get_entry(sb, entry, entryc, locked))
				goto out;
#else
			entryc = entry;
#endif
#endif

			next_pgoff = entryc->pgoff;
			if (next_pgoff <= start_blk) {
				nova_err(
					sb,
					"iblock %#lx, entry pgoff %#lx, num pages %#lx\n",
					start_blk, next_pgoff,
					entry->num_pages);
				nova_print_inode_log(sb, inode);
				BUG();
				ent_blks = num_blocks;
				goto out;
			}
			ent_blks = next_pgoff - start_blk;
			if (ent_blks > num_blocks)
				ent_blks = num_blocks;
		} else {
			/* File grow */
			ent_blks = num_blocks;
		}
	}

	if (entry && ent_blks == 0) {
		nova_dbg("%s: %d\n", __func__, check_next);
		dump_stack();
	}

out:
	NOVA_END_TIMING(check_entry_t, check_time);
	return ent_blks;
}

void check_alter_pages(struct super_block *sb, u64 curr_p)
{
	if (curr_p == 0)
		return;
	void *addr = (void *)nova_get_virt_addr_from_offset(sb, curr_p, 1);
	struct nova_file_write_entry *entry =
		(struct nova_file_write_entry *)addr;
	u64 entry_off = nova_get_addr_off(NOVA_SB(sb), entry, 1);
	u64 alter_off = alter_log_entry(sb, entry_off);
	void *alter = (void *)nova_get_virt_addr_from_offset(sb, alter_off, 1);
	nova_dbg("%s: entry_off: %#llx, alter_off: %#llx, alter: %#llx\n",
		 __func__, entry_off, alter_off, (u64)alter);
	if (pmem_ar_addr_invaild(alter)) {
		nova_err(sb, "%s: alter entry address invalid\n", __func__);
		dump_stack();
	}
}

/*
 * Do an inplace write.  This function assumes that the lock on the inode is
 * already held.
 */
ssize_t do_nova_inplace_file_write(struct file *filp, const char __user *buf,
				   size_t len, loff_t *ppos)
{
	struct address_space *mapping = filp->f_mapping;
	struct inode *inode = mapping->host;
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
	struct super_block *sb = inode->i_sb;
	struct nova_sb_info *sbi = NOVA_SB(sb);
	struct nova_inode *pi, inode_copy;
	struct nova_file_write_entry *entry;
	struct nova_file_write_entry *entryc, entry_copy;
	struct nova_file_write_entry entry_data;
	struct nova_inode_update update;
	char *ubuf_copy, *ubuf_copy_src = NULL;
	ssize_t written = 0;
	loff_t pos;
	size_t count, offset, copied;
	unsigned long start_blk, num_blocks, ent_blks = 0;
	unsigned long total_blocks;
	unsigned long new_blocks = 0;
	unsigned long blocknr = 0;
	unsigned int data_bits;
	int allocated = 0;
	int inplace = 0;
	bool hole_fill = false;
	bool update_log = false;
	void *kmem;
	size_t bytes;
	long status = 0;
	INIT_TIMING(inplace_write_time);
	INIT_TIMING(fini_delegation_time);
	INIT_TIMING(bd_write_time);
	INIT_TIMING(bd_data_csum_time);
	INIT_TIMING(bd_meta_write_time);
	unsigned long step = 0;
	u64 begin_tail = 0;
	u64 epoch_id;
	u64 file_size;
	u32 time;
	ssize_t ret;
	unsigned long irq_flags = 0;
	int blocksize_mask;
	int i, socket;
	size_t head, tail;
	size_t aligned_num_blocks;
	unsigned long blocknr_loop;
	bool fair_new = false, append = false, is_dele = false;

	int cond_cnt = 0;
	long issued_cnt[NOVA_MAX_SOCKET];
	struct nova_notifyer completed_cnt[NOVA_MAX_SOCKET];

	if (len == 0)
		return 0;

	NOVA_START_TIMING(inplace_write_t, inplace_write_time);
	NOVA_START_META_TIMING(bd_cow_write_t, bd_write_time);

	if (!access_ok(buf, len)) {
		ret = -EFAULT;
		goto out;
	}

	/*
	 * let user buffer to be kernel thread shared and 64-byte aligned
	 */

#if NOVA_KERNEL_COPY_USER_BUFFER
	ubuf_copy = kmalloc(len + 64, GFP_KERNEL);
	ubuf_copy_src = ubuf_copy;
	if (ubuf_copy == NULL) {
		nova_err(sb, "%s: user kernel buffer allocation error\n",
			 __func__);
		return -ENOMEM;
	}
	ubuf_copy = (char *)(((unsigned long)ubuf_copy + 63) & ~0x3f);
	ret = copy_from_user(ubuf_copy, buf, len);
	if (ret) {
		nova_dbg("%s: copy_from_user failed %ld\n", __func__, ret);
		ret = -EFAULT;
		goto out;
	}
#endif

	memset(issued_cnt, 0, sizeof(long) * NOVA_MAX_SOCKET);
	memset(completed_cnt, 0,
	       sizeof(struct nova_notifyer) * NOVA_MAX_SOCKET);

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

	/* offset in the actual block size block */

	ret = file_remove_privs(filp);
	if (ret)
		goto out;

	inode->i_mtime = inode_set_ctime_current(inode);
	time = inode->i_mtime.tv_sec;

	epoch_id = nova_get_epoch_id(sb);

	nova_dbg_verbose(
		"%s: epoch_id %llu, inode %lu, offset %lld, count %lu, alloc blocks: %ld\n",
		__func__, epoch_id, inode->i_ino, pos, count, num_blocks);
	update.tail = sih->log_tail;
	update.alter_tail = sih->alter_log_tail;

	is_dele = false;
	while (num_blocks > 0) {
		hole_fill = false;
		offset = pos & blocksize_mask;
		start_blk = pos >> data_bits;

		ent_blks = nova_check_existing_entry(sb, inode, num_blocks,
						     start_blk, &entry,
						     &entry_copy, 0, epoch_id,
						     &inplace, 1);

		entryc = (metadata_csum == 0) ? entry : &entry_copy;

		nova_dbg_verbose(
			"%s: offset: %ld start_blk: %ld ent_blks: %ld num_blocks: %ld\n",
			__func__, offset, start_blk, ent_blks, num_blocks);

		if (entry && inplace) {
			nova_dbg_verbose(
				"%s: inplace update; entry: %#llx, inplace: %d\n",
				__func__, (u64)entry, inplace);
			/* We can do inplace write. Find contiguous blocks */
			blocknr = get_nvmm(sb, sih, entryc, start_blk);
			allocated = ent_blks;
			// if (data_csum || data_parity)
			// 	nova_set_write_entry_updating(sb, entry, 1);
		} else {
			if (!entry)
				fair_new = true;
			nova_dbg_verbose(
				"%s: no inplace update; entry: %#llx, inplace: %d, alloc new block\n",
				__func__, (u64)entry, inplace);
			/* Allocate blocks to fill hole */
			allocated = nova_new_data_blocks(sb, sih, &blocknr,
							 start_blk, num_blocks,
							 ALLOC_NO_INIT, ANY_CPU,
							 ALLOC_FROM_HEAD);
			nova_dbg_verbose("%s: alloc %d blocks @ %#lx\n",
					 __func__, allocated, blocknr);

			if (allocated <= 0) {
				nova_dbg("%s alloc blocks failed!, %d\n",
					 __func__, allocated);
				ret = allocated;
				goto out;
			}

			hole_fill = true;
			new_blocks += allocated;
		}

		step++;
		bytes = nova_inode_blk_size(sih) * allocated - offset;
		if (bytes > count)
			bytes = count;

		head = tail = 0;
		if (offset || ((offset + bytes) & (PAGE_SIZE - 1)) != 0) {
			if (hole_fill) {
				// new block allocated, if offset == 0, we don't need to fill 0 on tail use append here
				if (fair_new && offset == 0) {
					append = true;
				} else {
					append = false;
				}
#if NOVA_KERNEL_COPY_USER_BUFFER
				ret = nova_handle_head_tail_blocks(
					sb, inode, pos, bytes, blocknr,
					ubuf_copy, &head, &tail, append,
					issued_cnt, completed_cnt, hole_fill,
					&is_dele);
#else
				ret = nova_handle_head_tail_blocks(
					sb, inode, pos, bytes, blocknr,
					(char *)buf, &head, &tail, append,
					issued_cnt, completed_cnt, hole_fill,
					&is_dele);
#endif
			} else {
// just do copy, no cow
#if NOVA_KERNEL_COPY_USER_BUFFER
				ret = nova_handle_head_tail_blocks(
					sb, inode, pos, bytes, blocknr,
					ubuf_copy, &head, &tail, 1, issued_cnt,
					completed_cnt, hole_fill, &is_dele);
#else
				ret = nova_handle_head_tail_blocks(
					sb, inode, pos, bytes, blocknr,
					(char *)buf, &head, &tail, 1,
					issued_cnt, completed_cnt, hole_fill,
					&is_dele);
#endif
			}
			if (ret)
				goto out;
		}

		aligned_num_blocks = (bytes - head - tail) >> PAGE_SHIFT;
		nova_dbg_verbose(
			"%s: bytes: %lu, head: %lu, tail: %lu, aligned_num_blocks: %lu\n",
			__func__, bytes, head, tail, aligned_num_blocks);
		// move blocknr to the start of contiguous blocks
		if (head) {
			blocknr += 1;
		}
		copied = 0;

		// TODO: 32k blksize should do larger io
		// 		kmem = nova_get_virt_addr_from_offset(
		// 			inode->i_sb,
		// 			nova_get_block_off(sb, blocknr, sih->i_blk_type, 0), 0);
		// 		socket = nova_block_to_socket(sbi, blocknr, sih->i_blk_type, 0);

		// #if NOVA_KERNEL_COPY_USER_BUFFER
		// 		copied += do_nova_nvmm_write(
		// 			sb, kmem, (void *)(ubuf_copy + head),
		// 			aligned_num_blocks << PAGE_SHIFT, 0, socket, 0, 1, 0,
		// 			issued_cnt, completed_cnt,
		// 			len >= NOVA_WRITE_WAIT_THRESHOLD, hole_fill, &is_dele);
		// #else
		// 		copied += do_nova_nvmm_write(sb, kmem, (void *)(buf + head),
		// 					     aligned_num_blocks << PAGE_SHIFT,
		// 					     0, socket, 0, 1, 0, issued_cnt,
		// 					     completed_cnt,
		// 					     len >= NOVA_WRITE_WAIT_THRESHOLD,
		// 					     hole_fill, &is_dele);
		// #endif

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
			socket = nova_block_to_socket(sbi, blocknr_loop,
						      sih->i_blk_type, 0);

#if NOVA_KERNEL_COPY_USER_BUFFER
			copied += do_nova_nvmm_write(
				sb, kmem,
				(void *)(ubuf_copy + head + PAGE_SIZE * i),
				PAGE_SIZE, 0, socket, 0, 1, 0, issued_cnt,
				completed_cnt, len >= NOVA_WRITE_WAIT_THRESHOLD,
				true, &is_dele);
#else
			copied += do_nova_nvmm_write(
				sb, kmem, (void *)(buf + head + PAGE_SIZE * i),
				PAGE_SIZE, 0, socket, 0, 1, 0, issued_cnt,
				completed_cnt, len >= NOVA_WRITE_WAIT_THRESHOLD,
				true, &is_dele);
#endif
		}

		if (copied) {
			nova_err(sb, "%s: delegation failed to copy all\n",
				 __func__);
			ret = -EFAULT;
			goto out;
		}
		// restore blocknr
		if (head) {
			blocknr -= 1;
		}
		copied = bytes;

		if (hole_fill || is_dele) {
			if (data_csum > 0 || data_parity > 0) {
				/* calculate data checksum and write csum to pmem */
				NOVA_START_META_TIMING(bd_data_csum_t,
						       bd_data_csum_time);
#if NOVA_KERNEL_COPY_USER_BUFFER
				ret = nova_protect_file_data(sb, inode, pos,
							     bytes, ubuf_copy,
							     blocknr);
#else
				ret = nova_protect_file_data(sb, inode, pos,
							     bytes, (char *)buf,
							     blocknr);
#endif
				NOVA_END_META_TIMING(bd_data_csum_t,
						     bd_data_csum_time);
				if (ret)
					goto out;
			}
		}

		if (pos + copied > inode->i_size)
			file_size = cpu_to_le64(pos + copied);
		else
			file_size = cpu_to_le64(inode->i_size);

		/* Handle hole fill write */
		if (hole_fill) {
			NOVA_START_META_TIMING(bd_meta_write_t,
					       bd_meta_write_time);
			nova_init_file_write_entry(sb, sih, &entry_data,
						   epoch_id, start_blk,
						   allocated, blocknr, time,
						   file_size);

// ret = nova_append_file_write_entry(
// 	sb, pi, inode, &entry_data, &update);
#if NOVA_INODE_IN_MEM
			ret = nova_append_file_write_entry(sb, pi, &inode_copy,
							   inode, &entry_data,
							   &update);
#else
			ret = nova_append_file_write_entry(
				sb, pi, NULL, inode, &entry_data, &update);
#endif
			NOVA_END_META_TIMING(bd_meta_write_t,
					     bd_meta_write_time);

			if (ret) {
				nova_dbg("%s: append inode entry failed\n",
					 __func__);
				ret = -ENOSPC;
				goto out;
			}

		} else {
			/* Update existing entry */
			NOVA_START_META_TIMING(bd_meta_write_t,
					       bd_meta_write_time);
			int atomic_update = 0;
			struct nova_log_entry_info entry_info;

			if (entry->entry_type != FILE_WRITE)
				atomic_update += 1;
			if (entry->epoch_id != epoch_id)
				atomic_update += 1;
			if (entry->mtime != time)
				atomic_update += 1;
			if (entry->size != file_size)
				atomic_update += 1;

			nova_dbg_verbose(
				"%s: atomic_update: %d, entry size: %#llx, file_size: %#llx\n",
				__func__, atomic_update, entry->size,
				file_size);
			if (atomic_update == 1 && entry->size != file_size) {
				// TODO: in the POSIX mode, cksum should exclude size feild
				// TODO: file_size might be changed to the [pgoff, pgoff+size]
				entry->size = file_size;
				nova_flush_buffer(&entry->size, sizeof(u64), 1);
			} else {
				// otherwise, start a transaction
				entry_info.type = FILE_WRITE;
				entry_info.epoch_id = epoch_id;
				entry_info.trans_id = sih->trans_id;
				entry_info.time = time;
				entry_info.file_size = file_size;
				entry_info.inplace = 1;
				nova_inplace_update_write_entry(
					sb, inode, entry, &entry_info);
			}
			NOVA_END_META_TIMING(bd_meta_write_t,
					     bd_meta_write_time);
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
		if (status < 0) {
			written = status;
			break;
		}

		if (hole_fill) {
			update_log = true;
			if (begin_tail == 0)
				begin_tail = update.curr_entry;
		}

		cond_cnt++;
		if (cond_cnt >= NOVA_APP_RING_BUFFER_CHECK_COUNT) {
			cond_cnt = 0;
			if (need_resched())
				cond_resched();
		}
	}

	sih->i_blocks += (new_blocks << (data_bits - sb->s_blocksize_bits));

	inode->i_blocks = sih->i_blocks;

	if (update_log) {
		nova_memunlock_inode(sb, pi, &irq_flags);
		NOVA_START_META_TIMING(bd_meta_write_t, bd_meta_write_time);
#if NOVA_INODE_IN_MEM
		nova_update_inode(sb, inode, pi, &inode_copy, &update, 1);
#else
		nova_update_inode(sb, inode, pi, NULL, &update, 1);
#endif
		NOVA_END_META_TIMING(bd_meta_write_t, bd_meta_write_time);
		nova_memlock_inode(sb, pi, &irq_flags);
		NOVA_STATS_ADD(inplace_new_blocks, 1);
	}

	ret = written;
	NOVA_STATS_ADD(inplace_write_breaks, step);
	nova_dbg_verbose("blocks: %llu, %#lx\n", inode->i_blocks,
			 sih->i_blocks);

	*ppos = pos;
	if (pos > inode->i_size) {
		i_size_write(inode, pos);
		sih->i_size = pos;
	}

out:
	NOVA_START_TIMING(fini_delegation_w_t, fini_delegation_time);
	if (is_dele)
		nova_complete_delegation(issued_cnt, completed_cnt);
	NOVA_END_TIMING(fini_delegation_w_t, fini_delegation_time);

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

	NOVA_END_META_TIMING(bd_cow_write_t, bd_write_time);
	NOVA_END_TIMING(inplace_write_t, inplace_write_time);
	NOVA_STATS_ADD(inplace_write_bytes, written);

	if (ubuf_copy_src)
		kfree(ubuf_copy_src);
	return ret;
}

/*
 * Acquire locks and perform an inplace update.
 */
ssize_t nova_inplace_file_write(struct file *filp, const char __user *buf,
				size_t len, loff_t *ppos)
{
	struct address_space *mapping = filp->f_mapping;
	struct inode *inode = mapping->host;
	int ret;

	if (len == 0)
		return 0;

	sb_start_write(inode->i_sb);
	inode_lock(inode);

	ret = do_nova_inplace_file_write(filp, buf, len, ppos);

	inode_unlock(inode);
	sb_end_write(inode->i_sb);

	return ret;
}

/* Check if existing entry overlap with vma regions */
int nova_check_overlap_vmas(struct super_block *sb,
			    struct nova_inode_info_header *sih,
			    unsigned long pgoff, unsigned long num_pages)
{
	unsigned long start_pgoff = 0;
	unsigned long num = 0;
	unsigned long i;
	struct vma_item *item;
	struct rb_node *temp;
	int ret = 0;

	if (sih->num_vmas == 0)
		return 0;

	temp = rb_first(&sih->vma_tree);
	while (temp) {
		item = container_of(temp, struct vma_item, node);
		temp = rb_next(temp);
		ret = nova_get_vma_overlap_range(sb, sih, item->vma, pgoff,
						 num_pages, &start_pgoff, &num);
		if (ret) {
			for (i = 0; i < num; i++) {
				if (nova_get_write_entry(sb, sih,
							 start_pgoff + i))
					return 1;
			}
		}
	}

	return 0;
}

/*
 * return > 0, # of blocks mapped or allocated.
 * return = 0, if plain lookup failed.
 * return < 0, error case.
 */
static int nova_dax_get_blocks(struct inode *inode, sector_t iblock,
			       unsigned long max_blocks, u32 *bno, bool *new,
			       bool *boundary, int create, bool taking_lock)
{
	struct super_block *sb = inode->i_sb;
	struct nova_inode *pi;
	struct nova_inode pic;
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
	struct nova_file_write_entry *entry = NULL;
	struct nova_file_write_entry *entryc, entry_copy;
	struct nova_file_write_entry entry_data;
	struct nova_inode_update update;
	u32 time;
	unsigned int data_bits;
	unsigned long nvmm = 0;
	unsigned long blocknr = 0;
	u64 epoch_id;
	int num_blocks = 0;
	int inplace = 0;
	int allocated = 0;
	int locked = 0;
	int check_next = 1;
	int ret = 0;
	unsigned long irq_flags = 0;
	INIT_TIMING(get_block_time);

	if (max_blocks == 0)
		return 0;

	NOVA_START_TIMING(dax_get_block_t, get_block_time);

	nova_dbg_verbose("%s: pgoff %#llx, num %#lx, create %d\n", __func__,
			 iblock, max_blocks, create);

	epoch_id = nova_get_epoch_id(sb);

	if (taking_lock)
		check_next = 0;

again:
	num_blocks = nova_check_existing_entry(sb, inode, max_blocks, iblock,
					       &entry, &entry_copy, check_next,
					       epoch_id, &inplace, locked);

	entryc = (metadata_csum == 0) ? entry : &entry_copy;

	if (entry) {
		if (create == 0 || inplace) {
			nvmm = get_nvmm(sb, sih, entryc, iblock);
			nova_dbg_verbose("%s: found pgoff %#llx, block %#lx\n",
					 __func__, iblock, nvmm);
			goto out;
		}
	}

	if (create == 0) {
		num_blocks = 0;
		goto out1;
	}

	if (taking_lock && locked == 0) {
		inode_lock(inode);
		locked = 1;
		/* Check again incase someone has done it for us */
		check_next = 1;
		goto again;
	}

	pi = nova_get_inode(sb, inode);
	memcpy(&pic, pi, sizeof(struct nova_inode));
	inode->i_mtime = inode_set_ctime_current(inode);
	time = inode->i_mtime.tv_sec;
	update.tail = sih->log_tail;
	update.alter_tail = sih->alter_log_tail;

	/* Return initialized blocks to the user */
	allocated = nova_new_data_blocks(sb, sih, &blocknr, iblock, num_blocks,
					 ALLOC_INIT_ZERO, ANY_CPU,
					 ALLOC_FROM_HEAD);
	if (allocated <= 0) {
		nova_dbg_verbose("%s alloc blocks failed %d\n", __func__,
				 allocated);
		ret = allocated;
		goto out;
	}

	num_blocks = allocated;
	/* Do not extend file size */
	nova_init_file_write_entry(sb, sih, &entry_data, epoch_id, iblock,
				   num_blocks, blocknr, time, inode->i_size);

	ret = nova_append_file_write_entry(sb, pi, &pic, inode, &entry_data,
					   &update);
	if (ret) {
		nova_dbg_verbose("%s: append inode entry failed\n", __func__);
		ret = -ENOSPC;
		goto out;
	}

	nvmm = blocknr;
	data_bits = blk_type_to_shift[sih->i_blk_type];
	sih->i_blocks += (num_blocks << (data_bits - sb->s_blocksize_bits));

	nova_memunlock_inode(sb, pi, &irq_flags);
	nova_update_inode(sb, inode, pi, &pic, &update, 1);
	nova_memlock_inode(sb, pi, &irq_flags);

	// ret = nova_reassign_file_tree(sb, sih, update.curr_entry);
	// if (ret) {
	// 	nova_dbg_verbose("%s: nova_reassign_file_tree failed: %d\n",
	// 			 __func__, ret);
	// 	goto out;
	// }
	inode->i_blocks = sih->i_blocks;
	sih->trans_id++;
	NOVA_STATS_ADD(dax_new_blocks, 1);

//	set_buffer_new(bh);
out:
	if (ret < 0) {
		nova_cleanup_incomplete_write(sb, sih, blocknr, allocated, 0,
					      update.tail);
		num_blocks = ret;
		goto out1;
	}

	*bno = nvmm;
	//	if (num_blocks > 1)
	//		bh->b_size = sb->s_blocksize * num_blocks;

out1:
	if (taking_lock && locked)
		inode_unlock(inode);

	NOVA_END_TIMING(dax_get_block_t, get_block_time);
	return num_blocks;
}

// TODO: buggy
int nova_iomap_begin(struct inode *inode, loff_t offset, loff_t length,
		     unsigned int flags, struct iomap *iomap,
		     struct iomap *srcmap, bool taking_lock)
{
	unsigned int blkbits = inode->i_blkbits;
	unsigned long first_block = offset >> blkbits;
	unsigned long max_blocks = (length + (1 << blkbits) - 1) >> blkbits;
	bool new = false, boundary = false;
	u32 bno;
	int ret;

	ret = nova_dax_get_blocks(inode, first_block, max_blocks, &bno, &new,
				  &boundary, flags & IOMAP_WRITE, taking_lock);
	if (ret < 0) {
		nova_dbg_verbose("%s: nova_dax_get_blocks failed %d", __func__,
				 ret);
		return ret;
	}

	iomap->flags = 0;
	iomap->bdev = inode->i_sb->s_bdev;
	// TODO: need to fix, we don't have a unify dax device
	iomap->dax_dev = pmem_ar_dev.dax_dev[0];
	iomap->offset = (u64)first_block << blkbits;

	if (ret == 0) {
		iomap->type = IOMAP_HOLE;
		iomap->addr = IOMAP_NULL_ADDR;
		iomap->length = 1 << blkbits;
	} else {
		iomap->type = IOMAP_MAPPED;
		iomap->addr = (u64)bno << blkbits;
		iomap->length = (u64)ret << blkbits;
		iomap->flags |= IOMAP_F_MERGED;
	}

	if (new)
		iomap->flags |= IOMAP_F_NEW;
	return 0;
}

int nova_iomap_end(struct inode *inode, loff_t offset, loff_t length,
		   ssize_t written, unsigned int flags, struct iomap *iomap)
{
	if (iomap->type == IOMAP_MAPPED && written < length &&
	    (flags & IOMAP_WRITE))
		truncate_pagecache(inode, inode->i_size);
	return 0;
}

static int nova_iomap_begin_lock(struct inode *inode, loff_t offset,
				 loff_t length, unsigned int flags,
				 struct iomap *iomap, struct iomap *srcmap)
{
	return nova_iomap_begin(inode, offset, length, flags, iomap, srcmap,
				true);
}

static struct iomap_ops nova_iomap_ops_lock = {
	.iomap_begin = nova_iomap_begin_lock,
	.iomap_end = nova_iomap_end,
};

static vm_fault_t nova_dax_huge_fault(struct vm_fault *vmf, unsigned int order)
{
	vm_fault_t ret;
	int error = 0;
	pfn_t pfn;
	// INIT_TIMING(fault_time);
	struct address_space *mapping = vmf->vma->vm_file->f_mapping;
	struct inode *inode = mapping->host;

	// NOVA_START_TIMING(pmd_fault_t, fault_time);

	nova_dbg_verbose("%s: inode %lu, pgoff %#lx\n", __func__, inode->i_ino,
			 vmf->pgoff);

	if (vmf->flags & FAULT_FLAG_WRITE)
		file_update_time(vmf->vma->vm_file);

	ret = dax_iomap_fault(vmf, order, &pfn, &error, &nova_iomap_ops_lock);

	// NOVA_END_TIMING(pmd_fault_t, fault_time);
	return ret;
}

static vm_fault_t nova_dax_fault(struct vm_fault *vmf)
{
	struct address_space *mapping = vmf->vma->vm_file->f_mapping;
	struct inode *inode = mapping->host;

	nova_dbg_verbose("%s: inode %lu, pgoff %#lx, flags 0x%x\n", __func__,
			 inode->i_ino, vmf->pgoff, vmf->flags);

	return nova_dax_huge_fault(vmf, 0);
}

static vm_fault_t nova_dax_pfn_mkwrite(struct vm_fault *vmf)
{
	struct address_space *mapping = vmf->vma->vm_file->f_mapping;
	struct inode *inode = mapping->host;

	nova_dbg_verbose("%s: inode %lu, pgoff %#lx, flags 0x%x\n", __func__,
			 inode->i_ino, vmf->pgoff, vmf->flags);

	return nova_dax_huge_fault(vmf, 0);
}

static inline int nova_rbtree_compare_vma(struct vma_item *curr,
					  struct vm_area_struct *vma)
{
	if (vma < curr->vma)
		return -1;
	if (vma > curr->vma)
		return 1;

	return 0;
}

static int nova_append_write_mmap_to_log(struct super_block *sb,
					 struct inode *inode,
					 struct vma_item *item)
{
	struct vm_area_struct *vma = item->vma;
	struct nova_inode *pi;
	struct nova_mmap_entry data;
	struct nova_inode_update update;
	unsigned long num_pages;
	u64 epoch_id;
	int ret;

	/* Only for csum and parity update */
	if (data_csum == 0 && data_parity == 0)
		return 0;

	pi = nova_get_inode(sb, inode);
	epoch_id = nova_get_epoch_id(sb);
	update.tail = update.alter_tail = 0;

	memset(&data, 0, sizeof(struct nova_mmap_entry));
	data.entry_type = MMAP_WRITE;
	data.epoch_id = epoch_id;
	data.pgoff = cpu_to_le64(vma->vm_pgoff);
	num_pages = (vma->vm_end - vma->vm_start) >> PAGE_SHIFT;
	data.num_pages = cpu_to_le64(num_pages);
	data.invalid = 0;

	nova_dbg_verbose(
		"%s : Appending mmap log entry for inode %lu, pgoff %llu, %llu pages\n",
		__func__, inode->i_ino, data.pgoff, data.num_pages);

	ret = nova_append_mmap_entry(sb, pi, inode, &data, &update, item);
	if (ret) {
		nova_dbg("%s: append write mmap entry failure\n", __func__);
		goto out;
	}

out:
	return ret;
}

int nova_insert_write_vma(struct vm_area_struct *vma)
{
	struct address_space *mapping = vma->vm_file->f_mapping;
	struct inode *inode = mapping->host;
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
	struct super_block *sb = inode->i_sb;
	struct nova_sb_info *sbi = NOVA_SB(sb);
	unsigned long flags = VM_SHARED | VM_WRITE;
	struct vma_item *item, *curr;
	struct rb_node **temp, *parent;
	int compVal;
	int insert = 0;
	int ret;
	// INIT_TIMING(insert_vma_time);

	if ((vma->vm_flags & flags) != flags)
		return 0;

	// NOVA_START_TIMING(insert_vma_t, insert_vma_time);

	item = nova_alloc_vma_item(sb);
	if (!item) {
		// NOVA_END_TIMING(insert_vma_t, insert_vma_time);
		return -ENOMEM;
	}

	item->vma = vma;

	nova_dbg_verbose(
		"Inode %lu insert vma %p, start 0x%lx, end 0x%lx, pgoff %#lx\n",
		inode->i_ino, vma, vma->vm_start, vma->vm_end, vma->vm_pgoff);

	inode_lock(inode);

	/* Append to log */
	ret = nova_append_write_mmap_to_log(sb, inode, item);
	if (ret)
		goto out;

	temp = &(sih->vma_tree.rb_node);
	parent = NULL;

	while (*temp) {
		curr = container_of(*temp, struct vma_item, node);
		compVal = nova_rbtree_compare_vma(curr, vma);
		parent = *temp;

		if (compVal == -1) {
			temp = &((*temp)->rb_left);
		} else if (compVal == 1) {
			temp = &((*temp)->rb_right);
		} else {
			nova_dbg("%s: vma %p already exists\n", __func__, vma);
			kfree(item);
			goto out;
		}
	}

	rb_link_node(&item->node, parent, temp);
	rb_insert_color(&item->node, &sih->vma_tree);

	sih->num_vmas++;
	if (sih->num_vmas == 1)
		insert = 1;

	sih->trans_id++;
out:
	inode_unlock(inode);

	if (insert) {
		mutex_lock(&sbi->vma_mutex);
		list_add_tail(&sih->list, &sbi->mmap_sih_list);
		mutex_unlock(&sbi->vma_mutex);
	}

	// NOVA_END_TIMING(insert_vma_t, insert_vma_time);
	return ret;
}

static int nova_remove_write_vma(struct vm_area_struct *vma)
{
	struct address_space *mapping = vma->vm_file->f_mapping;
	struct inode *inode = mapping->host;
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
	struct super_block *sb = inode->i_sb;
	struct nova_sb_info *sbi = NOVA_SB(sb);
	struct vma_item *curr = NULL;
	struct rb_node *temp;
	int compVal;
	int found = 0;
	int remove = 0;
	// INIT_TIMING(remove_vma_time);

	// NOVA_START_TIMING(remove_vma_t, remove_vma_time);
	inode_lock(inode);

	temp = sih->vma_tree.rb_node;
	while (temp) {
		curr = container_of(temp, struct vma_item, node);
		compVal = nova_rbtree_compare_vma(curr, vma);

		if (compVal == -1) {
			temp = temp->rb_left;
		} else if (compVal == 1) {
			temp = temp->rb_right;
		} else {
			nova_reset_vma_csum_parity(sb, curr);
			rb_erase(&curr->node, &sih->vma_tree);
			found = 1;
			break;
		}
	}

	if (found) {
		sih->num_vmas--;
		if (sih->num_vmas == 0)
			remove = 1;
	}

	inode_unlock(inode);

	if (found) {
		nova_dbg_verbose(
			"Inode %lu remove vma %p, start 0x%lx, end 0x%lx, pgoff %#lx\n",
			inode->i_ino, curr->vma, curr->vma->vm_start,
			curr->vma->vm_end, curr->vma->vm_pgoff);
		nova_free_vma_item(sb, curr);
	}

	if (remove) {
		mutex_lock(&sbi->vma_mutex);
		list_del(&sih->list);
		mutex_unlock(&sbi->vma_mutex);
	}

	// NOVA_END_TIMING(remove_vma_t, remove_vma_time);
	return 0;
}

#if 0
static int nova_restore_page_write(struct vm_area_struct *vma,
	unsigned long address)
{
	struct mm_struct *mm = vma->vm_mm;


	down_write(&mm->mmap_sem);

	nova_dbg_verbose("Restore vma %p write, start 0x%lx, end 0x%lx, address 0x%lx\n",
		  vma, vma->vm_start, vma->vm_end, address);

	/* Restore single page write */
	nova_mmap_to_new_blocks(vma, address);

	up_write(&mm->mmap_sem);

	return 0;
}
#endif

static void nova_vma_open(struct vm_area_struct *vma)
{
	struct address_space *mapping = vma->vm_file->f_mapping;
	struct inode *inode = mapping->host;

	nova_dbg_mmap4k(
		"[%s:%d] inode %lu, MMAP 4KPAGE vm_start(0x%lx), vm_end(0x%lx), vm pgoff "
		"%#lx, %#lx blocks, vm_flags(0x%lx), vm_page_prot(0x%lx)\n",
		__func__, __LINE__, inode->i_ino, vma->vm_start, vma->vm_end,
		vma->vm_pgoff, (vma->vm_end - vma->vm_start) >> PAGE_SHIFT,
		vma->vm_flags, pgprot_val(vma->vm_page_prot));

	nova_insert_write_vma(vma);
}

static void nova_vma_close(struct vm_area_struct *vma)
{
	nova_dbg_verbose("[%s:%d] MMAP 4KPAGE vm_start(0x%lx), vm_end(0x%lx), "
			 "vm_flags(0x%lx), vm_page_prot(0x%lx)\n",
			 __func__, __LINE__, vma->vm_start, vma->vm_end,
			 vma->vm_flags, pgprot_val(vma->vm_page_prot));

	//	vma->original_write = 0;
	nova_remove_write_vma(vma);
}

const struct vm_operations_struct nova_dax_vm_ops = {
	.fault = nova_dax_fault,
	.huge_fault = nova_dax_huge_fault,
	.page_mkwrite = nova_dax_fault,
	.pfn_mkwrite = nova_dax_pfn_mkwrite,
	.open = nova_vma_open,
	.close = nova_vma_close,
	//	.dax_cow = nova_restore_page_write,
};
