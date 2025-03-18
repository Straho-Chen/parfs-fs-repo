/*
 * BRIEF DESCRIPTION
 *
 * Definitions for the NOVA filesystem.
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
#ifndef __NOVA_H
#define __NOVA_H

#include <linux/fs.h>
#include <linux/dax.h>
#include <linux/init.h>
#include <linux/time.h>
#include <linux/rtc.h>
#include <linux/mm.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/pagemap.h>
#include <linux/backing-dev.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/rcupdate.h>
#include <linux/types.h>
#include <linux/rbtree.h>
#include <linux/radix-tree.h>
#include <linux/version.h>
#include <linux/kthread.h>
#include <linux/buffer_head.h>
#include <linux/uio.h>
#include <linux/iomap.h>
#include <linux/crc32c.h>
#include <linux/xxhash.h>
#include <asm/tlbflush.h>
#include <linux/version.h>
#include <linux/pfn_t.h>
#include <linux/pagevec.h>

#include "nova_def.h"
#include "pmem_ar_block.h"
#include "stats.h"
#include "snapshot.h"

static inline int memcpy_mcsafe(void *dst, const void *src, size_t size)
{
	if (memcpy(dst, src, size) == NULL)
		return -1;
	return 0;
}

/* Mask out flags that are inappropriate for the given type of inode. */
static inline __le32 nova_mask_flags(umode_t mode, __le32 flags)
{
	flags &= cpu_to_le32(NOVA_FL_INHERITED);
	if (S_ISDIR(mode))
		return flags;
	else if (S_ISREG(mode))
		return flags & cpu_to_le32(NOVA_REG_FLMASK);
	else
		return flags & cpu_to_le32(NOVA_OTHER_FLMASK);
}

/* Update the crc32c value by appending a 64b data word. */
#define nova_crc32c_qword(qword, crc)                 \
	do {                                          \
		asm volatile("crc32q %1, %0"          \
			     : "=r"(crc)              \
			     : "r"(qword), "0"(crc)); \
	} while (0)

static inline u32 nova_crc32c(u32 crc, const u8 *data, size_t len)
{
	u8 *ptr = (u8 *)data;
	u64 acc = crc; /* accumulator, crc32c value in lower 32b */
	u32 csum;

	/* x86 instruction crc32 is part of SSE-4.2 */
	if (static_cpu_has(X86_FEATURE_XMM4_2)) {
		/* This inline assembly implementation should be equivalent
		 * to the kernel's crc32c_intel_le_hw() function used by
		 * crc32c(), but this performs better on test machines.
		 */
		while (len > 8) {
			asm volatile(/* 64b quad words */
				     "crc32q (%1), %0"
				     : "=r"(acc)
				     : "r"(ptr), "0"(acc));
			ptr += 8;
			len -= 8;
		}

		while (len > 0) {
			asm volatile(/* trailing bytes */
				     "crc32b (%1), %0"
				     : "=r"(acc)
				     : "r"(ptr), "0"(acc));
			ptr++;
			len--;
		}

		csum = (u32)acc;
	} else {
		/* The kernel's crc32c() function should also detect and use the
		 * crc32 instruction of SSE-4.2. But calling in to this function
		 * is about 3x to 5x slower than the inline assembly version on
		 * some test machines.
		 */
		csum = crc32c(crc, data, len);
	}

	return csum;
}

static inline u32 nova_calc_csum32(u32 seed, u8 *data, size_t len)
{
#if NOVA_XXHASH_CSUM
	return xxh32(data, len, seed);
#else
	return nova_crc32c(seed, data, len);
#endif
}

static inline void nova_calc_csum_qword(u64 *qword, u64 *crc)
{
#if NOVA_XXHASH_CSUM
	*crc = xxh64(qword, sizeof(u64), *crc);
#else
	nova_crc32c_qword(*qword, *crc);
#endif
}

/* uses CPU instructions to atomically write up to 8 bytes */
static inline void nova_memcpy_atomic(void *dst, const void *src, u8 size)
{
	switch (size) {
	case 1: {
		volatile u8 *daddr = dst;
		const u8 *saddr = src;
		*daddr = *saddr;
		break;
	}
	case 2: {
		volatile __le16 *daddr = dst;
		const u16 *saddr = src;
		*daddr = cpu_to_le16(*saddr);
		break;
	}
	case 4: {
		volatile __le32 *daddr = dst;
		const u32 *saddr = src;
		*daddr = cpu_to_le32(*saddr);
		break;
	}
	case 8: {
		volatile __le64 *daddr = dst;
		const u64 *saddr = src;
		*daddr = cpu_to_le64(*saddr);
		break;
	}
	default:
		nova_dbg("error: memcpy_atomic called with %d bytes\n", size);
		//BUG();
	}
}

static inline int memcpy_to_pmem_nocache(void *dst, const void *src,
					 unsigned int size)
{
	int ret;

	// __copy_user_nocache return uncopied bytes or 0 if successful
	ret = __copy_from_user_inatomic_nocache(dst, src, size);

	return ret;
}

/*
 * ubuf_copy should be in kernel space and 64-byte aligned.
 */
static inline int memcpy_to_pmem_avx_nocache(void *dst_addr,
					     void *src_ubuf_copy,
					     unsigned int size)
{
	int ret;

	if ((unsigned long)src_ubuf_copy & 0x3f) {
		nova_warn("src_ubuf_copy %p unaligned to 64 bytes\n",
			  src_ubuf_copy);
		return -1;
	}

	// check 64-byte alignment
	if (((unsigned long)dst_addr & 0x3f) == 0) {
		size_t i;

		kernel_fpu_begin();
		// 每次处理 256 字节
		for (i = 0; i + 256 <= size; i += 256) {
			asm volatile(
				"vmovdqa64 (%[src]), %%zmm0 \n" // 从 src_addr 加载 64 字节到 zmm0
				"vmovdqa64 64(%[src]), %%zmm1 \n" // 从 src_addr + 64 加载 64 字节到 zmm1
				"vmovdqa64 128(%[src]), %%zmm2 \n" // 从 src_addr + 128 加载 64 字节到 zmm2
				"vmovdqa64 192(%[src]), %%zmm3 \n" // 从 src_addr + 192 加载 64 字节到 zmm3

				"vmovntdq %%zmm0, (%[dst]) \n" // 将 zmm0 中的数据非临时存储到 start_addr
				"vmovntdq %%zmm1, 64(%[dst]) \n" // 将 zmm1 中的数据非临时存储到 start_addr + 64
				"vmovntdq %%zmm2, 128(%[dst]) \n" // 将 zmm2 中的数据非临时存储到 start_addr + 128
				"vmovntdq %%zmm3, 192(%[dst]) \n" // 将 zmm3 中的数据非临时存储到 start_addr + 192
				:
				: [src] "r"(src_ubuf_copy + i), [dst] "r"(
									dst_addr +
									i)
				: "zmm0", "zmm1", "zmm2", "zmm3", "memory");
		}
		kernel_fpu_end();

		// 处理剩余的数据
		for (; i < size; ++i) {
			((char *)dst_addr)[i] = ((char *)src_ubuf_copy)[i];
		}
		PERSISTENT_BARRIER();

		ret = 0;

	} else {
		ret = __copy_from_user_inatomic_nocache(dst_addr, src_ubuf_copy,
							size);
	}

	return ret;
}

/* assumes the length to be 4-byte aligned */
static inline void memset_nt(void *dest, uint32_t dword, size_t length)
{
	uint64_t dummy1, dummy2;
	uint64_t qword = ((uint64_t)dword << 32) | dword;

	asm volatile("movl %%edx,%%ecx\n"
		     "andl $63,%%edx\n"
		     "shrl $6,%%ecx\n"
		     "jz 9f\n"
		     "1:	 movnti %%rax,(%%rdi)\n"
		     "2:	 movnti %%rax,1*8(%%rdi)\n"
		     "3:	 movnti %%rax,2*8(%%rdi)\n"
		     "4:	 movnti %%rax,3*8(%%rdi)\n"
		     "5:	 movnti %%rax,4*8(%%rdi)\n"
		     "8:	 movnti %%rax,5*8(%%rdi)\n"
		     "7:	 movnti %%rax,6*8(%%rdi)\n"
		     "8:	 movnti %%rax,7*8(%%rdi)\n"
		     "leaq 64(%%rdi),%%rdi\n"
		     "decl %%ecx\n"
		     "jnz 1b\n"
		     "9:	movl %%edx,%%ecx\n"
		     "andl $7,%%edx\n"
		     "shrl $3,%%ecx\n"
		     "jz 11f\n"
		     "10:	 movnti %%rax,(%%rdi)\n"
		     "leaq 8(%%rdi),%%rdi\n"
		     "decl %%ecx\n"
		     "jnz 10b\n"
		     "11:	 movl %%edx,%%ecx\n"
		     "shrl $2,%%ecx\n"
		     "jz 12f\n"
		     "movnti %%eax,(%%rdi)\n"
		     "12:\n"
		     : "=D"(dummy1), "=d"(dummy2)
		     : "D"(dest), "a"(qword), "d"(length)
		     : "memory", "rcx");
}

#include "super.h" // Remove when we factor out these and other functions.

static inline unsigned long nova_get_numblocks(unsigned short btype)
{
	unsigned long num_blocks;

	if (btype == NOVA_BLOCK_TYPE_4K) {
		num_blocks = 1;
	} else if (btype == NOVA_BLOCK_TYPE_32K) {
		num_blocks = 8;
	} else if (btype == NOVA_BLOCK_TYPE_2M) {
		num_blocks = 512;
	} else {
		//btype == NOVA_BLOCK_TYPE_1G
		num_blocks = 0x40000;
	}
	return num_blocks;
}

static inline int nova_blocknr_to_index(int blocknr, int btype)
{
	return blocknr / nova_get_numblocks(btype);
}

/* Which socket this block belongs to */
static inline int nova_block_to_socket(struct nova_sb_info *sbi, int blocknr,
				       int btype)
{
	return nova_blocknr_to_index(blocknr, btype) % sbi->sockets;
}

static inline int nova_block_to_cpu(struct nova_sb_info *sbi, int blocknr)
{
	return blocknr / sbi->per_list_blocks;
}

static inline u64 nova_block_to_nvmoff(struct nova_sb_info *sbi, int blocknr)
{
	return blocknr / sbi->sockets;
}

/* Translate an offset the beginning of the Nova instance to a PMEM address.
 *
 * If this is part of a read-modify-write of the block,
 * nova_memunlock_block() before calling!
 */
static inline void *nova_get_virt_addr_from_offset(struct super_block *sb,
						   u64 off)
{
	struct nova_super_block *ps = nova_get_super(sb);

	return off ? ((void *)ps + off) : NULL;
}

static inline int nova_get_block_from_addr(struct nova_sb_info *sbi, void *addr)
{
	return ((addr - sbi->virt_addr) >> PAGE_SHIFT);
}

static inline int nova_get_reference(struct super_block *sb, u64 off,
				     void *dram, void **nvmm, size_t size)
{
	int rc;

	*nvmm = nova_get_virt_addr_from_offset(sb, off);
	rc = memcpy_mcsafe(dram, *nvmm, size);
	return rc;
}

static inline u64 nova_get_addr_off(struct nova_sb_info *sbi, void *addr)
{
	/*
	 * It seems that we can make sure the address is valid.
	 * So we make the check to be enable when debugging.
	 */
	if (nova_dbgmask) {
		if (pmem_ar_addr_invaild(addr)) {
			nova_err(sbi->sb, "Invalid address %p\n", addr);
		}
	}
	return (u64)(addr - sbi->virt_addr);
}

static inline u64 nova_get_block_off(struct super_block *sb,
				     unsigned long blocknr,
				     unsigned short btype)
{
	struct nova_sb_info *sbi = NOVA_SB(sb);
	// nova_dbg_verbose("%s: input blocknr: %#lx, btype: %d\n", __func__,
	// 		 blocknr, btype);
	int socket = nova_block_to_socket(sbi, blocknr, btype);
	int socket_off = nova_block_to_nvmoff(sbi, blocknr);
	u64 ret_off = (sbi->block_info[socket].start_block + socket_off)
		      << PAGE_SHIFT;
	// nova_dbg_verbose(
	// 	"%s: output block off: %#llx, socket: %d, socket_off: %d, start_block_cur_socket: %#lx\n",
	// 	__func__, ret_off, socket, socket_off,
	// 	sbi->block_info[socket].start_block);
	return ret_off;
}

static inline int nova_get_cpuid(struct super_block *sb)
{
	struct nova_sb_info *sbi = NOVA_SB(sb);

	return smp_processor_id() % sbi->cpus;
}

static inline u64 nova_get_epoch_id(struct super_block *sb)
{
	struct nova_sb_info *sbi = NOVA_SB(sb);

	return sbi->s_epoch_id;
}

static inline void nova_print_curr_epoch_id(struct super_block *sb)
{
	struct nova_sb_info *sbi = NOVA_SB(sb);
	u64 ret;

	ret = sbi->s_epoch_id;
	nova_info("Current epoch id: %llu\n", ret);
}

#include "inode.h"
static inline int nova_get_head_tail(struct super_block *sb,
				     struct nova_inode *pi,
				     struct nova_inode_info_header *sih)
{
	struct nova_inode fake_pi;
	int rc;

	rc = memcpy_mcsafe(&fake_pi, pi, sizeof(struct nova_inode));
	if (rc)
		return rc;

	sih->i_blk_type = fake_pi.i_blk_type;
	sih->log_head = fake_pi.log_head;
	sih->log_tail = fake_pi.log_tail;
	sih->alter_log_head = fake_pi.alter_log_head;
	sih->alter_log_tail = fake_pi.alter_log_tail;

	return rc;
}

struct nova_range_node_lowhigh {
	__le64 range_low;
	__le64 range_high;
};

#define RANGENODE_PER_PAGE 254

/* A node in the RB tree representing a range of pages */
struct nova_range_node {
	struct rb_node node;
	struct vm_area_struct *vma;
	unsigned long mmap_entry;
	union {
		/* Block, inode */
		struct {
			unsigned long range_low;
			unsigned long range_high;
		};
		/* Dir node */
		struct {
			unsigned long hash;
			void *direntry;
		};
	};
	u32 csum; /* Protect vma, range low/high */
};

struct vma_item {
	/* Reuse header of nova_range_node struct */
	struct rb_node node;
	struct vm_area_struct *vma;
	unsigned long mmap_entry;
};

static inline u32 nova_calculate_range_node_csum(struct nova_range_node *node)
{
	u32 csum;

	csum = nova_calc_csum32(~0, (__u8 *)&node->vma,
				(unsigned long)&node->csum -
					(unsigned long)&node->vma);

	return csum;
}

static inline int nova_update_range_node_checksum(struct nova_range_node *node)
{
	if (dram_struct_csum)
		node->csum = nova_calculate_range_node_csum(node);

	return 0;
}

static inline bool nova_range_node_checksum_ok(struct nova_range_node *node)
{
	bool ret;

	if (dram_struct_csum == 0)
		return true;

	ret = node->csum == nova_calculate_range_node_csum(node);
	if (!ret) {
		nova_dbg(
			"%s: checksum failure, vma %p, range low %lu, range high %lu, csum 0x%x\n",
			__func__, node->vma, node->range_low, node->range_high,
			node->csum);
	}

	return ret;
}

enum bm_type {
	BM_4K = 0,
	BM_2M,
	BM_1G,
};

struct single_scan_bm {
	unsigned long bitmap_size;
	unsigned long *bitmap;
};

struct scan_bitmap {
	struct single_scan_bm scan_bm_4K;
	struct single_scan_bm scan_bm_2M;
	struct single_scan_bm scan_bm_1G;
};

struct inode_map {
	struct mutex inode_table_mutex;
	struct rb_root inode_inuse_tree;
	unsigned long num_range_node_inode;
	struct nova_range_node *first_inode_range;
	int allocated;
	int freed;
};

/* Old entry is freeable if it is appended after the latest snapshot */
static inline int old_entry_freeable(struct super_block *sb, u64 epoch_id)
{
	struct nova_sb_info *sbi = NOVA_SB(sb);

	if (epoch_id == sbi->s_epoch_id)
		return 1;

	return 0;
}

static inline int pass_mount_snapshot(struct super_block *sb, u64 epoch_id)
{
	struct nova_sb_info *sbi = NOVA_SB(sb);

	if (epoch_id > sbi->mount_snapshot_epoch_id)
		return 1;

	return 0;
}

// BKDR String Hash Function
static inline unsigned long BKDRHash(const char *str, int length)
{
	unsigned int seed = 131; // 31 131 1313 13131 131313 etc..
	unsigned long hash = 0;
	int i;

	for (i = 0; i < length; i++)
		hash = hash * seed + (*str++);

	return hash;
}

#include "mprotect.h"

#include "log.h"

static inline struct nova_file_write_entry *
nova_get_write_entry(struct super_block *sb, struct nova_inode_info_header *sih,
		     unsigned long blocknr)
{
	struct nova_file_write_entry *entry;

	entry = radix_tree_lookup(&sih->tree, blocknr);

	return entry;
}

/*
 * Find data at a file offset (pgoff) in the data pointed to by a write log
 * entry.
 */
static inline unsigned long get_nvmm(struct super_block *sb,
				     struct nova_inode_info_header *sih,
				     struct nova_file_write_entry *entry,
				     unsigned long pgoff)
{
	/* entry is already verified before this call and resides in dram
	 * or we can do memcpy_mcsafe here but have to avoid double copy and
	 * verification of the entry.
	 */
	if (entry->pgoff > pgoff ||
	    (unsigned long)entry->pgoff + (unsigned long)entry->num_pages <=
		    pgoff) {
		struct nova_sb_info *sbi = NOVA_SB(sb);
		u64 curr;

		curr = nova_get_addr_off(sbi, entry);
		nova_dbg(
			"Entry ERROR: inode %lu, curr 0x%llx, pgoff %lu, entry pgoff %llu, num %u\n",
			sih->ino, curr, pgoff, entry->pgoff, entry->num_pages);
		nova_print_nova_log_pages(sb, sih);
		nova_print_nova_log(sb, sih);
		NOVA_ASSERT(0);
	}

	return (unsigned long)entry->blocknr + pgoff - entry->pgoff;
}

bool nova_verify_entry_csum(struct super_block *sb, void *entry, void *entryc);
bool nova_get_entry_copy(struct super_block *sb, void *entry, void *entryc);

static inline u64 nova_find_nvmm_block(struct super_block *sb,
				       struct nova_inode_info_header *sih,
				       struct nova_file_write_entry *entry,
				       unsigned long blocknr)
{
	unsigned long nvmm;
	struct nova_file_write_entry *entryc, entry_copy;

	if (!entry) {
		entry = nova_get_write_entry(sb, sih, blocknr);
		if (!entry)
			return 0;
	}

	/* Don't check entry here as someone else may be modifying it
	 * when called from reset_vma_csum_parity
	 */
	entryc = &entry_copy;
	if (memcpy_mcsafe(entryc, entry, sizeof(struct nova_file_write_entry)) <
	    0)
		return 0;

	nvmm = get_nvmm(sb, sih, entryc, blocknr);
	return nvmm << PAGE_SHIFT;
}

static inline unsigned long nova_get_blocknr(struct super_block *sb, u64 block,
					     unsigned short btype)
{
	return block >> PAGE_SHIFT;
}

static inline unsigned long nova_get_pfn(struct super_block *sb, u64 block)
{
	return (NOVA_SB(sb)->phys_addr + block) >> PAGE_SHIFT;
}

static inline u64 next_log_page(struct super_block *sb, u64 curr)
{
	struct nova_inode_log_page *curr_page;
	u64 next = 0;
	int rc;

	curr = BLOCK_OFF(curr);
	curr_page =
		(struct nova_inode_log_page *)nova_get_virt_addr_from_offset(
			sb, curr);
	rc = memcpy_mcsafe(&next, &curr_page->page_tail.next_page, sizeof(u64));
	if (rc)
		return rc;

	return next;
}

static inline u64 alter_log_page(struct super_block *sb, u64 curr)
{
	struct nova_inode_log_page *curr_page;
	u64 next = 0;
	int rc;

	if (metadata_csum == 0)
		return 0;

	curr = BLOCK_OFF(curr);
	curr_page =
		(struct nova_inode_log_page *)nova_get_virt_addr_from_offset(
			sb, curr);
	rc = memcpy_mcsafe(&next, &curr_page->page_tail.alter_page,
			   sizeof(u64));
	if (rc)
		return rc;

	return next;
}

static inline u64 alter_log_entry(struct super_block *sb, u64 curr_p)
{
	u64 alter_page;
	void *curr_addr = nova_get_virt_addr_from_offset(sb, curr_p);
	unsigned long page_tail =
		BLOCK_OFF((unsigned long)curr_addr) + LOG_BLOCK_TAIL;
	if (metadata_csum == 0)
		return 0;

	alter_page = ((struct nova_inode_page_tail *)page_tail)->alter_page;
	return alter_page + ENTRY_LOC(curr_p);
}

static inline void nova_set_next_page_flag(struct super_block *sb, u64 curr_off)
{
	void *p;

	if (ENTRY_LOC(curr_off) >= LOG_BLOCK_TAIL)
		return;

	p = nova_get_virt_addr_from_offset(sb, curr_off);
	nova_set_entry_type(p, NEXT_PAGE);
	nova_flush_buffer(p, CACHELINE_SIZE, 0);
}

static inline void
nova_set_next_page_address(struct super_block *sb,
			   struct nova_inode_log_page *curr_page, u64 next_page,
			   int fence)
{
	curr_page->page_tail.next_page = next_page;
	nova_flush_buffer(&curr_page->page_tail,
			  sizeof(struct nova_inode_page_tail), fence);
}

static inline void
nova_set_page_num_entries(struct super_block *sb,
			  struct nova_inode_log_page *curr_page, int num,
			  int flush)
{
	curr_page->page_tail.num_entries = num;
	if (flush)
		nova_flush_buffer(&curr_page->page_tail,
				  sizeof(struct nova_inode_page_tail), 0);
}

static inline void
nova_set_page_invalid_entries(struct super_block *sb,
			      struct nova_inode_log_page *curr_page, int num,
			      int flush)
{
	curr_page->page_tail.invalid_entries = num;
	if (flush)
		nova_flush_buffer(&curr_page->page_tail,
				  sizeof(struct nova_inode_page_tail), 0);
}

static inline void nova_inc_page_num_entries(struct super_block *sb, u64 curr)
{
	struct nova_inode_log_page *curr_page;

	curr = BLOCK_OFF(curr);
	curr_page =
		(struct nova_inode_log_page *)nova_get_virt_addr_from_offset(
			sb, curr);

	curr_page->page_tail.num_entries++;
	nova_flush_buffer(&curr_page->page_tail,
			  sizeof(struct nova_inode_page_tail), 0);
}

u64 nova_print_log_entry(struct super_block *sb, u64 curr);

static inline void nova_inc_page_invalid_entries(struct super_block *sb,
						 u64 curr)
{
	struct nova_inode_log_page *curr_page;
	u64 old_curr = curr;

	curr = BLOCK_OFF(curr);
	curr_page =
		(struct nova_inode_log_page *)nova_get_virt_addr_from_offset(
			sb, curr);

	curr_page->page_tail.invalid_entries++;
	if (curr_page->page_tail.invalid_entries >
	    curr_page->page_tail.num_entries) {
		nova_dbg("Page 0x%llx has %u entries, %u invalid\n", curr,
			 curr_page->page_tail.num_entries,
			 curr_page->page_tail.invalid_entries);
		nova_print_log_entry(sb, old_curr);
	}

	nova_flush_buffer(&curr_page->page_tail,
			  sizeof(struct nova_inode_page_tail), 0);
}

static inline void nova_set_alter_page_address(struct super_block *sb,
					       u64 curr_off, u64 alter_curr_off)
{
	struct nova_inode_log_page *curr_page;
	struct nova_inode_log_page *alter_page;

	if (metadata_csum == 0)
		return;

	curr_page = nova_get_virt_addr_from_offset(sb, BLOCK_OFF(curr_off));
	alter_page =
		nova_get_virt_addr_from_offset(sb, BLOCK_OFF(alter_curr_off));

	curr_page->page_tail.alter_page = alter_curr_off;
	nova_flush_buffer(&curr_page->page_tail,
			  sizeof(struct nova_inode_page_tail), 0);

	alter_page->page_tail.alter_page = curr_off;
	nova_flush_buffer(&alter_page->page_tail,
			  sizeof(struct nova_inode_page_tail), 0);
}

#define CACHE_ALIGN(p) ((p) & ~(CACHELINE_SIZE - 1))

static inline bool is_last_entry(u64 curr_p, size_t size)
{
	unsigned int entry_end;

	entry_end = ENTRY_LOC(curr_p) + size;

	return entry_end > LOG_BLOCK_TAIL;
}

static inline bool goto_next_page(struct super_block *sb, u64 curr_off)
{
	void *addr;
	u8 type;
	int rc;

	/* Each kind of entry takes at least 32 bytes */
	if (ENTRY_LOC(curr_off) + 32 > LOG_BLOCK_TAIL)
		return true;

	addr = nova_get_virt_addr_from_offset(sb, curr_off);
	rc = memcpy_mcsafe(&type, addr, sizeof(u8));

	if (rc < 0)
		return true;

	if (type == NEXT_PAGE)
		return true;

	return false;
}

static inline int is_dir_init_entry(struct super_block *sb,
				    struct nova_dentry *entry)
{
	if (entry->name_len == 1 && strncmp(entry->name, ".", 1) == 0)
		return 1;
	if (entry->name_len == 2 && strncmp(entry->name, "..", 2) == 0)
		return 1;

	return 0;
}

#include "balloc.h" // remove once we move the following functions away

/* Checksum methods */
static inline void *nova_get_data_csum_addr(struct super_block *sb, u64 blocknr,
					    int replica)
{
	struct nova_sb_info *sbi = NOVA_SB(sb);
	struct free_list *free_list;
	void *data_csum_addr;
	u64 blockoff;
	int cpu;
	u64 strp_nr;
	u64 strp_block_off;
	int BLOCK_SHIFT = PAGE_SHIFT - NOVA_STRIPE_SHIFT;

	if (!data_csum) {
		nova_dbg("%s: Data checksum is disabled!\n", __func__);
		return NULL;
	}

	cpu = nova_block_to_cpu(sbi, blocknr);

	// nova_dbg_verbose("%s: blocknr: %#llx, locate on cpu: %d\n", __func__,
	// 		 blocknr, cpu);

	if (cpu >= sbi->cpus) {
		nova_dbg("%s: Invalid blocknr %#llx\n", __func__, blocknr);
		return NULL;
	}

	// stripe number = block index on per cpu list << BLOCK_SHIFT
	strp_nr = (blocknr - cpu * sbi->per_list_blocks) << BLOCK_SHIFT;
	strp_block_off = (strp_nr * NOVA_DATA_CSUM_LEN) >> PAGE_SHIFT;

	free_list = nova_get_free_list(sb, cpu);
	if (replica == 0)
		blockoff = nova_get_block_off(
			sb, free_list->csum_start + strp_block_off,
			NOVA_BLOCK_TYPE_4K);
	else
		blockoff = nova_get_block_off(
			sb, free_list->replica_csum_start + strp_block_off,
			NOVA_BLOCK_TYPE_4K);

	data_csum_addr = (u8 *)nova_get_virt_addr_from_offset(sb, blockoff);

	return data_csum_addr;
}

static inline void *nova_get_parity_addr(struct super_block *sb,
					 unsigned long blocknr)
{
	struct nova_sb_info *sbi = NOVA_SB(sb);
	struct free_list *free_list;
	void *data_csum_addr;
	u64 blockoff;
	u64 strp_block_off;
	int cpu;

	if (data_parity == 0) {
		nova_dbg("%s: Data parity is disabled!\n", __func__);
		return NULL;
	}

	cpu = nova_block_to_cpu(sbi, blocknr);

	if (cpu >= sbi->cpus) {
		nova_dbg("%s: Invalid blocknr %lu\n", __func__, blocknr);
		return NULL;
	}

	free_list = nova_get_free_list(sb, cpu);
	strp_block_off =
		((blocknr - free_list->block_start) << NOVA_STRIPE_SHIFT) >>
		PAGE_SHIFT;
	blockoff = nova_get_block_off(sb,
				      free_list->parity_start + strp_block_off,
				      NOVA_BLOCK_TYPE_4K);

	data_csum_addr = (u8 *)nova_get_virt_addr_from_offset(sb, blockoff);

	return data_csum_addr;
}

static inline int nova_get_init_nsocket(struct nova_sb_info *sbi)
{
	return xor_random() % sbi->sockets;
}

static inline int nova_get_nsocket(struct nova_sb_info *sbi,
				   struct nova_inode_info_header *sih)
{
	return ((sih->nsocket + 1) % sbi->sockets);
}

#include "delegation.h"

/*
 * socket: kmem_dest fall in which socket
 */
static inline size_t do_nova_nvmm_write(struct super_block *sb, void *kmem_dest,
					void *kubuf_src, size_t bytes,
					int socket, int zero, int flush_cache,
					int sfence, long *issued_cnt,
					struct nova_notifyer *completed_cnt,
					int wait_hint)
{
	size_t left;
	unsigned long irq_flags = 0;
	INIT_TIMING(memcpy_time);
	INIT_TIMING(delegation_time);

	nova_memunlock_range(sb, kmem_dest, bytes, &irq_flags);
	if (bytes < NOVA_WRITE_DELEGATION_LIMIT) {
		nova_dbg_verbose("less than delegation limit\n");
		NOVA_START_TIMING(memcpy_w_nvmm_t, memcpy_time);
		if (zero) {
			nova_dbg_verbose("do memset_nt to fill zero\n");
			memset_nt(kmem_dest, 0, bytes);
			left = 0;
		} else {
			nova_dbg_verbose(
				"do memcpy_to_pmem_nocache from %#lx to %#lx\n",
				(unsigned long)kubuf_src,
				(unsigned long)kmem_dest);
			left = memcpy_to_pmem_nocache(kmem_dest, kubuf_src,
						      bytes);
			//TODO: remove
			nova_dbg_verbose("%s: kubuf content: %s, size: %ld\n",
					 __func__, (char *)kubuf_src, bytes);
		}
		NOVA_END_TIMING(memcpy_w_nvmm_t, memcpy_time);
	} else {
		nova_dbg_verbose("do delegation\n");
		NOVA_START_TIMING(do_delegation_w_t, delegation_time);
		left = nova_do_write_delegation(NOVA_SB(sb), current->mm,
						(unsigned long)kubuf_src,
						(unsigned long)kmem_dest, bytes,
						socket, zero, flush_cache,
						sfence, issued_cnt,
						completed_cnt, wait_hint);
		NOVA_END_TIMING(do_delegation_w_t, delegation_time);
	}
	nova_memlock_range(sb, kmem_dest, bytes, &irq_flags);

	return left;
}

/*
 * socket: kmem_src fall in which socket
 */
static inline size_t do_nova_nvmm_read(struct super_block *sb, void *ubuf_dest,
				       void *kmem_src, size_t bytes, int socket,
				       int zero, long *issued_cnt,
				       struct nova_notifyer *completed_cnt,
				       int wait_hint)
{
	size_t left;
	INIT_TIMING(memcpy_time);
	INIT_TIMING(delegation_time);

	if (bytes < NOVA_READ_DELEGATION_LIMIT) {
		nova_dbg_verbose("less than delegation limit\n");
		NOVA_START_TIMING(memcpy_r_nvmm_t, memcpy_time);

		if (!zero) {
			nova_dbg_verbose(
				"do __copy_to_user from %#lx to %#lx\n",
				(unsigned long)kmem_src,
				(unsigned long)ubuf_dest);
			left = __copy_to_user(ubuf_dest, kmem_src, bytes);
		} else {
			nova_dbg_verbose("do __clear_user to fill zero\n");
			left = __clear_user(ubuf_dest, bytes);
		}

		NOVA_END_TIMING(memcpy_r_nvmm_t, memcpy_time);
	} else {
		nova_dbg_verbose("do delegation\n");
		NOVA_START_TIMING(do_delegation_r_t, delegation_time);
		left = nova_do_read_delegation(NOVA_SB(sb), current->mm,
					       (unsigned long)ubuf_dest,
					       (unsigned long)kmem_src, bytes,
					       socket, zero, issued_cnt,
					       completed_cnt, wait_hint);
		NOVA_END_TIMING(do_delegation_r_t, delegation_time);
	}

	return left;
}

/* Function Prototypes */

/* bbuild.c */
inline void set_bm(unsigned long bit, struct scan_bitmap *bm,
		   enum bm_type type);
void nova_save_blocknode_mappings_to_log(struct super_block *sb);
void nova_save_inode_list_to_log(struct super_block *sb);
void nova_init_header(struct super_block *sb,
		      struct nova_inode_info_header *sih, u16 i_mode);
int nova_recovery(struct super_block *sb);

/* checksum.c */
void nova_update_entry_csum(void *entry);
int nova_update_block_csum(struct super_block *sb,
			   struct nova_inode_info_header *sih, u8 *block,
			   unsigned long blocknr, size_t offset, size_t bytes,
			   int zero);
int nova_update_alter_entry(struct super_block *sb, void *entry);
int nova_copy_inode(struct super_block *sb, u64 ino, u64 pi_addr,
		    u64 alter_pi_addr, struct nova_inode *pic);
int nova_check_inode_integrity(struct super_block *sb, u64 ino, u64 pi_addr,
			       u64 alter_pi_addr, struct nova_inode *pic,
			       int check_replica);
int nova_update_pgoff_csum(struct super_block *sb,
			   struct nova_inode_info_header *sih,
			   struct nova_file_write_entry *entry,
			   unsigned long pgoff, int zero);
bool nova_verify_data_csum(struct super_block *sb,
			   struct nova_inode_info_header *sih,
			   unsigned long blocknr, size_t offset, size_t bytes);
int nova_update_truncated_block_csum(struct super_block *sb,
				     struct inode *inode, loff_t newsize);

/*
 * Inodes and files operations
 */

/* dax.c */
int nova_cleanup_incomplete_write(struct super_block *sb,
				  struct nova_inode_info_header *sih,
				  unsigned long blocknr, int allocated,
				  u64 begin_tail, u64 end_tail);
void nova_init_file_write_entry(struct super_block *sb,
				struct nova_inode_info_header *sih,
				struct nova_file_write_entry *entry,
				u64 epoch_id, u64 pgoff, int num_pages,
				u64 blocknr, u32 time, u64 size);
int nova_reassign_file_tree(struct super_block *sb,
			    struct nova_inode_info_header *sih, u64 begin_tail);
unsigned long nova_check_existing_entry(
	struct super_block *sb, struct inode *inode, unsigned long num_blocks,
	unsigned long start_blk, struct nova_file_write_entry **ret_entry,
	struct nova_file_write_entry *ret_entryc, int check_next, u64 epoch_id,
	int *inplace, int locked);
int nova_iomap_begin(struct inode *inode, loff_t offset, loff_t length,
		     unsigned int flags, struct iomap *iomap,
		     struct iomap *srcmap, bool taking_lock);
int nova_iomap_end(struct inode *inode, loff_t offset, loff_t length,
		   ssize_t written, unsigned int flags, struct iomap *iomap);
int nova_insert_write_vma(struct vm_area_struct *vma);

int nova_check_overlap_vmas(struct super_block *sb,
			    struct nova_inode_info_header *sih,
			    unsigned long pgoff, unsigned long num_pages);
int nova_handle_head_tail_blocks(struct super_block *sb, struct inode *inode,
				 loff_t pos, size_t count,
				 unsigned long blocknr, int socket,
				 void *ubuf_copy, int *head, int *tail,
				 long *issued_cnt,
				 struct nova_notifyer *completed_cnt);
int nova_protect_file_data(struct super_block *sb, struct inode *inode,
			   loff_t pos, size_t count, char *ubuf_copy,
			   unsigned long blocknr, bool inplace);
ssize_t nova_inplace_file_write(struct file *filp, const char __user *buf,
				size_t len, loff_t *ppos);
ssize_t do_nova_inplace_file_write(struct file *filp, const char __user *buf,
				   size_t len, loff_t *ppos);

extern const struct vm_operations_struct nova_dax_vm_ops;

/* dir.c */
extern const struct file_operations nova_dir_operations;
int nova_insert_dir_tree(struct super_block *sb,
			 struct nova_inode_info_header *sih, const char *name,
			 int namelen, struct nova_dentry *direntry);
int nova_remove_dir_tree(struct super_block *sb,
			 struct nova_inode_info_header *sih, const char *name,
			 int namelen, int replay,
			 struct nova_dentry **create_dentry);
int nova_append_dentry(struct super_block *sb, struct nova_inode *pi,
		       struct inode *dir, struct dentry *dentry, u64 ino,
		       unsigned short de_len, struct nova_inode_update *update,
		       int link_change, u64 epoch_id);
int nova_append_dir_init_entries(struct super_block *sb, struct nova_inode *pi,
				 struct nova_inode *pic, u64 self_ino,
				 u64 parent_ino, u64 epoch_id);
int nova_add_dentry(struct dentry *dentry, u64 ino, int inc_link,
		    struct nova_inode_update *update, u64 epoch_id);
int nova_remove_dentry(struct dentry *dentry, int dec_link,
		       struct nova_inode_update *update, u64 epoch_id,
		       bool rename);
int nova_invalidate_dentries(struct super_block *sb,
			     struct nova_inode_update *update);
void nova_print_dir_tree(struct super_block *sb,
			 struct nova_inode_info_header *sih, unsigned long ino);
void nova_delete_dir_tree(struct super_block *sb,
			  struct nova_inode_info_header *sih);
struct nova_dentry *nova_find_dentry(struct super_block *sb,
				     struct nova_inode *pi, struct inode *inode,
				     const char *name, unsigned long name_len);

/* file.c */
extern const struct inode_operations nova_file_inode_operations;
extern const struct file_operations nova_dax_file_operations;
extern const struct file_operations nova_wrap_file_operations;

/* gc.c */
int nova_inode_log_fast_gc(struct super_block *sb, struct nova_inode *pi,
			   struct nova_inode *pic,
			   struct nova_inode_info_header *sih, u64 curr_tail,
			   u64 new_block, u64 alter_new_block, int num_pages,
			   int force_thorough);

/* ioctl.c */
extern long nova_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);
#ifdef CONFIG_COMPAT
extern long nova_compat_ioctl(struct file *file, unsigned int cmd,
			      unsigned long arg);
#endif

/* mprotect.c */
extern int nova_dax_mem_protect(struct super_block *sb, void *vaddr,
				unsigned long size, int rw);
int nova_get_vma_overlap_range(struct super_block *sb,
			       struct nova_inode_info_header *sih,
			       struct vm_area_struct *vma,
			       unsigned long entry_pgoff,
			       unsigned long entry_pages,
			       unsigned long *start_pgoff,
			       unsigned long *num_pages);
int nova_mmap_to_new_blocks(struct vm_area_struct *vma, unsigned long address);
bool nova_find_pgoff_in_vma(struct inode *inode, unsigned long pgoff);
int nova_set_vmas_readonly(struct super_block *sb);

/* namei.c */
extern const struct inode_operations nova_dir_inode_operations;
extern const struct inode_operations nova_special_inode_operations;
extern struct dentry *nova_get_parent(struct dentry *child);

/* parity.c */
int nova_update_pgoff_parity(struct super_block *sb,
			     struct nova_inode_info_header *sih,
			     struct nova_file_write_entry *entry,
			     unsigned long pgoff, int zero);
int nova_update_block_csum_parity(struct super_block *sb,
				  struct nova_inode_info_header *sih, u8 *block,
				  unsigned long blocknr, size_t offset,
				  size_t bytes);
int nova_restore_data(struct super_block *sb, unsigned long blocknr,
		      unsigned int badstrip_id, void *badstrip, int nvmmerr,
		      u32 csum0, u32 csum1, u32 *csum_good);
int nova_update_truncated_block_parity(struct super_block *sb,
				       struct inode *inode, loff_t newsize);

/* rebuild.c */
int nova_reset_csum_parity_range(struct super_block *sb,
				 struct nova_inode_info_header *sih,
				 struct nova_file_write_entry *entry,
				 unsigned long start_pgoff,
				 unsigned long end_pgoff, int zero,
				 int check_entry);
int nova_reset_mapping_csum_parity(struct super_block *sb, struct inode *inode,
				   struct address_space *mapping,
				   unsigned long start_pgoff,
				   unsigned long end_pgoff);
int nova_reset_vma_csum_parity(struct super_block *sb, struct vma_item *item);
int nova_rebuild_dir_inode_tree(struct super_block *sb, struct nova_inode *pi,
				u64 pi_addr,
				struct nova_inode_info_header *sih);
int nova_rebuild_inode(struct super_block *sb, struct nova_inode_info *si,
		       u64 ino, u64 pi_addr, int rebuild_dir);
int nova_restore_snapshot_table(struct super_block *sb, int just_init);

/* snapshot.c */
int nova_encounter_mount_snapshot(struct super_block *sb, void *addr, u8 type);
int nova_save_snapshots(struct super_block *sb);
int nova_destroy_snapshot_infos(struct super_block *sb);
int nova_restore_snapshot_entry(struct super_block *sb,
				struct nova_snapshot_info_entry *entry,
				u64 curr_p, int just_init);
int nova_mount_snapshot(struct super_block *sb);
int nova_append_data_to_snapshot(struct super_block *sb,
				 struct nova_file_write_entry *entry, u64 nvmm,
				 u64 num_pages, u64 delete_epoch_id);
int nova_append_inode_to_snapshot(struct super_block *sb,
				  struct nova_inode *pi);
int nova_print_snapshots(struct super_block *sb, struct seq_file *seq);
int nova_print_snapshot_lists(struct super_block *sb, struct seq_file *seq);
int nova_delete_dead_inode(struct super_block *sb, u64 ino);
int nova_create_snapshot(struct super_block *sb);
int nova_delete_snapshot(struct super_block *sb, u64 epoch_id);
int nova_snapshot_init(struct super_block *sb);

/* symlink.c */
int nova_block_symlink(struct super_block *sb, struct nova_inode *pi,
		       struct inode *inode, const char *symname, int len,
		       u64 epoch_id);
extern const struct inode_operations nova_symlink_inode_operations;

/* sysfs.c */
extern const char *proc_dirname;
extern struct proc_dir_entry *nova_proc_root;
void nova_sysfs_init(struct super_block *sb);
void nova_sysfs_exit(struct super_block *sb);

/* nova_stats.c */
void nova_get_timing_stats(void);
void nova_get_IO_stats(void);
void nova_print_timing_stats(struct super_block *sb);
void nova_clear_stats(struct super_block *sb);
void nova_print_inode(struct nova_inode *pi);
void nova_print_inode_log(struct super_block *sb, struct inode *inode);
void nova_print_inode_log_pages(struct super_block *sb, struct inode *inode);
int nova_check_inode_logs(struct super_block *sb, struct nova_inode *pi);
void nova_print_free_lists(struct super_block *sb);

/* perf.c */
int nova_test_perf(struct super_block *sb, unsigned int func_id,
		   unsigned int poolmb, size_t size, unsigned int disks);

#endif /* __NOVA_H */
