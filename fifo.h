#ifndef __NOVA_FIFO_H__
#define __NOVA_FIFO_H__

#include "nova_def.h"
#include "delegation.h"
#include <linux/kfifo.h>
#include <linux/spinlock.h>

struct nova_ring_buffer {
	spinlock_t spinlock;
	struct kfifo fifo;
	int writing;
};

typedef struct nova_ring_buffer nova_ring_buffer_t;

nova_ring_buffer_t *nova_fifo_create(size_t size);
void nova_fifo_destroy(nova_ring_buffer_t *ring);
int nova_fifo_send_request(nova_ring_buffer_t *ring, void *payload,
			   size_t size);
int nova_fifo_receive_request(nova_ring_buffer_t *ring, void *payload,
			      size_t size);
int nova_fifo_empty(nova_ring_buffer_t *ring);
size_t nova_fifo_len(nova_ring_buffer_t *ring);

#endif