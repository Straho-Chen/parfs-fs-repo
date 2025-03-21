#include "fifo.h"

nova_ring_buffer_t *nova_fifo_create(size_t size)
{
	nova_ring_buffer_t *ret;
	ret = kmalloc(sizeof(nova_ring_buffer_t), GFP_KERNEL);
	if (ret == NULL) {
		nova_warn("%s: alloc fifo failed\n", __func__);
		return NULL;
	}
	if (kfifo_alloc(&ret->fifo, size, GFP_KERNEL)) {
		kfree(ret);
		nova_warn("%s: alloc kfifo failed\n", __func__);
		return NULL;
	}
	spin_lock_init(&ret->spinlock);
	return ret;
}

void nova_fifo_destroy(nova_ring_buffer_t *ring)
{
	kfifo_free(&ring->fifo);
	kfree(ring);
}

int nova_fifo_send_request(nova_ring_buffer_t *ring, void *payload, size_t size)
{
	int ret = 0;
	if (kfifo_is_full(&ring->fifo)) {
		ret = -EAGAIN;
		goto out;
	}
	kfifo_in_spinlocked_noirqsave(&ring->fifo, payload, size,
				      &ring->spinlock);
out:
	return ret;
}

int nova_fifo_receive_request(nova_ring_buffer_t *ring, void *payload,
			      size_t size)
{
	int ret = 0;
	if (kfifo_is_empty(&ring->fifo)) {
		ret = -EAGAIN;
		goto out;
	}
	kfifo_out(&ring->fifo, payload, size);
out:
	return ret;
}

size_t nova_fifo_len(nova_ring_buffer_t *ring)
{
	return kfifo_len(&ring->fifo);
}