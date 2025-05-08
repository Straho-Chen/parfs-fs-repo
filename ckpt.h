#ifndef __CKPT_H_
#define __CKPT_H_

#include "nova_def.h"
#include "nova_config.h"

struct nova_ckpt_entry {
	u64 ino;
	u64 latest_trans_id;
};

void nova_ckpt_init(struct super_block *sb);
void nova_ckpt_restore(struct super_block *sb);
void nova_ckpt_send_request(struct kfifo *ring, void *payload, size_t size);
int nova_init_ckpt_thread(struct super_block *sb);
void nova_ckpt_thread_fini(struct super_block *sb);
u64 nova_get_ckpt_id(struct nova_ckpt *ckpt, u64 ino);

#endif