#include "ring.h"

nova_ring_buffer_t *nova_ring_buffer[NOVA_MAX_SOCKET][NOVA_MAX_AGENT_PER_SOCKET];

static int nova_number_of_sockets;
int nova_num_of_rings_per_socket;

#define NOVA_RING_ALIGN (64)

int nova_init_ring_buffers(int sockets)
{
	int i, j;

	nova_number_of_sockets = sockets;
	nova_num_of_rings_per_socket = nova_dele_thrds;

	for (i = 0; i < sockets; i++)
		for (j = 0; j < nova_dele_thrds; j++) {
			nova_ring_buffer_t *ret;
			/* nonblocking ring buffer for each socket */

#if NOVA_SOLROS_RING_BUFFER
			ret = solros_ring_create(NOVA_RING_SIZE,
						 NOVA_RING_ALIGN, i, 0);
#else
			ret = ring_buffer_alloc(NOVA_RING_SIZE, 0);
#endif

			if (ret == NULL)
				goto err;

			nova_ring_buffer[i][j] = ret;
		}

	return 0;

err:
	nova_fini_ring_buffers();

	return -ENOMEM;
}

void nova_fini_ring_buffers(void)
{
	int i, j;

	for (i = 0; i < nova_number_of_sockets; i++)
		for (j = 0; j < nova_num_of_rings_per_socket; j++) {
			if (!nova_ring_buffer[i][j])
				continue;

#if NOVA_SOLROS_RING_BUFFER
			solros_ring_destroy(nova_ring_buffer[i][j]);
#else
			ring_buffer_free(nova_ring_buffer[i][j]);
#endif
		}
}

int nova_send_request(nova_ring_buffer_t *ring,
		      struct nova_delegation_request *request)
{
#if NOVA_SOLROS_RING_BUFFER
	return solros_ring_enqueue(ring, request,
				   sizeof(struct nova_delegation_request), 1);
#else
	return ring_buffer_write(ring, sizeof(struct nova_delegation_request),
				 request);
#endif
}

int nova_recv_request(nova_ring_buffer_t *ring,
		      struct nova_delegation_request *request)
{
#if NOVA_SOLROS_RING_BUFFER
	return solros_ring_dequeue(ring, request, NULL, 1);
#else
// TODO: wait for implementation
#endif
}