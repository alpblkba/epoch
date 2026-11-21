#include <stddef.h>

#include "epoch/ring.h"

bool epoch_ring_init(struct epoch_ring *r, uint8_t *buf, uint32_t capacity)
{
    if (buf == NULL || capacity < 2u || capacity > 0x80000000u || (capacity & (capacity - 1u)) != 0u)
        return false;
    atomic_init(&r->head, 0u);
    atomic_init(&r->tail, 0u);
    atomic_init(&r->dropped, 0u);
    r->mask = capacity - 1u;
    r->buf = buf;
    return true;
}

bool epoch_ring_put(struct epoch_ring *r, uint8_t byte)
{
    uint32_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    /* Acquire pairs with the consumer's release of tail: its read of a slot is complete before the slot is reused here. */
    uint32_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    if (head - tail > r->mask) {
        atomic_store_explicit(&r->dropped, atomic_load_explicit(&r->dropped, memory_order_relaxed) + 1u, memory_order_relaxed);
        return false;
    }
    r->buf[head & r->mask] = byte;
    /* Release publishes the byte stored above before the consumer can see the new head. */
    atomic_store_explicit(&r->head, head + 1u, memory_order_release);
    return true;
}

bool epoch_ring_get(struct epoch_ring *r, uint8_t *byte)
{
    uint32_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    /* Acquire pairs with the producer's release of head, so the byte read below is the one it published. */
    uint32_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    if (head == tail)
        return false;
    *byte = r->buf[tail & r->mask];
    /* Release hands the slot back only after the read above. */
    atomic_store_explicit(&r->tail, tail + 1u, memory_order_release);
    return true;
}

uint32_t epoch_ring_dropped(struct epoch_ring *r)
{
    return atomic_load_explicit(&r->dropped, memory_order_relaxed);
}
