#include <linux/kthread.h>

#include "agent.h"
#include "nova.h"

static struct task_struct *nova_ckpt_task;

void nova_ckpt_send_request(struct kfifo *ring, void *payload, size_t size)
{
	if (!kfifo_is_full(ring)) {
		kfifo_in(ring, payload, size);
	}
}

void nova_write_ckpt_entry(struct nova_ckpt *ckpt,
			   struct nova_ckpt_entry *entry)
{
	nova_dbg_ckpt("%s: entry trans_id: %llu, ino: %llu\n", __func__,
		      entry->latest_trans_id, entry->ino);
	void **pentry = radix_tree_lookup_slot(&ckpt->tree, entry->ino);
	struct nova_ckpt_entry *entry_in_nvm;
	size_t size = sizeof(struct nova_ckpt_entry);
	int extended = 0;

	if (pentry) {
		// find
		entry_in_nvm = radix_tree_deref_slot(pentry);
		nova_dbg_ckpt("%s: ino: %llu, entry_in_nvm: %#llx\n", __func__,
			      entry->ino, (u64)entry_in_nvm);
		entry_in_nvm->latest_trans_id = entry->latest_trans_id;
		nova_flush_buffer(&entry_in_nvm->latest_trans_id, sizeof(u64),
				  0);
	} else {
		// alloc
		nova_dbg_ckpt("%s: not found previous, alloc\n", __func__);
		nova_dbg_ckpt("%s: sih log tail: %#llx\n", __func__,
			      ckpt->sih->log_tail);
		u64 curr = nova_get_append_head(ckpt->sb, ckpt->pi, NULL,
						ckpt->sih, ckpt->sih->log_tail,
						size, MAIN_LOG, 0, &extended);
		nova_dbg_ckpt("%s: alloc entry: %#llx\n", __func__, curr);
		entry_in_nvm = nova_get_virt_addr_from_offset(ckpt->sb, curr);
		entry_in_nvm->ino = entry->ino;
		entry_in_nvm->latest_trans_id = entry->latest_trans_id;
		nova_flush_buffer(entry, size, 0);
		curr += size;
		ckpt->pi->log_tail = curr;
		nova_flush_buffer(&ckpt->pi->log_tail, CACHELINE_SIZE, 1);

		// insert
		radix_tree_insert(&ckpt->tree, entry->ino, entry_in_nvm);
	}
}

u64 nova_get_ckpt_id(struct nova_ckpt *ckpt, u64 ino)
{
	void **pentry = radix_tree_lookup_slot(&ckpt->tree, ino);
	struct nova_ckpt_entry *entry_in_nvm;

	if (pentry) {
		// find
		entry_in_nvm = radix_tree_deref_slot(pentry);
		return entry_in_nvm->latest_trans_id;
	} else {
		return -1;
	}
}

int nova_ckpt_recv_request(struct kfifo *ring, void *payload, size_t size)
{
	int ret = 0;
	if (kfifo_is_empty(ring)) {
		ret = -EAGAIN;
		goto out;
	}
	kfifo_out(ring, payload, size);
out:
	return ret;
}

int nova_ckpt_thread(void *arg)
{
	struct nova_ckpt_entry request;
	struct nova_ckpt *ckpt = (struct nova_ckpt *)arg;
	int err;
	while (1) {
try_again:
		err = nova_ckpt_recv_request(&ckpt->ring, &request,
					     sizeof(struct nova_ckpt_entry));

		if (err == -EAGAIN) {
			if (kthread_should_stop())
				break;

			if (need_resched())
				cond_resched();

			goto try_again;
		}

		if (err)
			break;

		nova_write_ckpt_entry(ckpt, &request);

		if (kthread_should_stop())
			break;

		if (need_resched())
			cond_resched();
	}
	while (!kthread_should_stop()) {
		set_current_state(TASK_INTERRUPTIBLE);
		schedule();
	}
	return 0;
}

void nova_ckpt_init(struct super_block *sb)
{
	struct nova_sb_info *sbi = NOVA_SB(sb);
	struct nova_inode_info_header *sih;
	u64 ino = NOVA_CKPT_INO;

	sbi->ckpt = kmalloc(sizeof(struct nova_ckpt), GFP_KERNEL);
	sbi->ckpt->sih =
		kmalloc(sizeof(struct nova_inode_info_header), GFP_KERNEL);
	sih = sbi->ckpt->sih;
	nova_init_header(sb, sih, 0);
	sih->pi_addr = nova_get_reserved_inode_addr(sb, ino);
	sih->alter_pi_addr = nova_get_alter_reserved_inode_addr(sb, ino);
	sih->ino = ino;
	sih->i_blk_type = NOVA_BLOCK_TYPE_4K;

	INIT_RADIX_TREE(&sbi->ckpt->tree, GFP_ATOMIC);
	sbi->ckpt->pi = nova_get_inode_by_ino(sb, ino);
	sbi->ckpt->sb = sb;

	nova_dbg("%s: init ckpt inode\n", __func__);
}

void nova_ckpt_restore(struct super_block *sb)
{
	struct nova_sb_info *sbi = NOVA_SB(sb);
	struct nova_inode_info_header *sih;
	u64 ino = NOVA_CKPT_INO;

	sbi->ckpt = kmalloc(sizeof(struct nova_ckpt), GFP_KERNEL);
	sbi->ckpt->pi = nova_get_inode_by_ino(sb, ino);
	sbi->ckpt->sb = sb;
	sbi->ckpt->sih =
		kmalloc(sizeof(struct nova_inode_info_header), GFP_KERNEL);
	sih = sbi->ckpt->sih;
	nova_init_header(sb, sih, __le16_to_cpu(sbi->ckpt->pi->i_mode));
	sih->pi_addr = nova_get_reserved_inode_addr(sb, ino);
	sih->alter_pi_addr = nova_get_alter_reserved_inode_addr(sb, ino);
	sih->ino = ino;
	sih->i_blk_type = NOVA_BLOCK_TYPE_4K;
	sih->log_head = sbi->ckpt->pi->log_head;
	sih->log_tail = sbi->ckpt->pi->log_tail;

	INIT_RADIX_TREE(&sbi->ckpt->tree, GFP_ATOMIC);

	// scan log to restore radix tree
	u64 curr;
	size_t size = sizeof(struct nova_ckpt_entry);
	struct nova_ckpt_entry *entry;
	curr = sih->log_head;
	while (curr != sih->log_tail) {
		if (is_last_entry(curr, size))
			curr = next_log_page(sb, curr);

		if (curr == 0) {
			nova_warn("%s: curr is NULL!\n", __func__);
			NOVA_ASSERT(0);
			break;
		}

		entry = (struct nova_ckpt_entry *)
			nova_get_virt_addr_from_offset(sb, curr);

		radix_tree_insert(&sbi->ckpt->tree, entry->ino, entry);
		curr += size;
	}
}

int nova_init_ckpt_thread(struct super_block *sb)
{
	struct nova_sb_info *sbi = NOVA_SB(sb);
	char name[255];
	int ret;
	int **socket_cpu = cpu_topology();

	ret = kfifo_alloc(&sbi->ckpt->ring, PAGE_SIZE, GFP_KERNEL);
	if (ret) {
		goto error;
	}

	sprintf(name, "nova_checkpoint");

	nova_ckpt_task =
		kthread_create_on_node(nova_ckpt_thread, sbi->ckpt, 0, name);

	int target_cpu = socket_cpu[0][nova_dele_thrds];
	kthread_bind(nova_ckpt_task, target_cpu);

	if (IS_ERR(nova_ckpt_task)) {
		ret = PTR_ERR(nova_ckpt_task);
		goto error;
	}

	wake_up_process(nova_ckpt_task);

	nova_info("Run checkpoint thread\n");

	cpu_topology_free(socket_cpu);

	return 0;

error:
	cpu_topology_free(socket_cpu);
	nova_ckpt_thread_fini();

	return -ENOMEM;
}

void nova_ckpt_thread_fini(void)
{
	if (nova_ckpt_task) {
		int ret;
		if ((ret = kthread_stop(nova_ckpt_task)))
			nova_info("kthread_stop task returned error %d\n", ret);
	}
}
