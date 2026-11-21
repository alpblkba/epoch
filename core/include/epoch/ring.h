#ifndef EPOCH_RING_H
#define EPOCH_RING_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/* Single-producer single-consumer byte ring for the UART receive path. Assumptions: one core, the UART receive ISR is the only producer and owns head, the protocol thread is the only consumer and owns tail, and no DMA writes the buffer. Indices run free and wrap at 2^32, so the capacity is a power of two no larger than 2^31. The Zephyr ring_buf compile-time switch belongs to the firmware build and is not here. */

/* uint32_t is unsigned int on the Mac and unsigned long on arm-zephyr-eabi, so the macro is selected by the actual type. */
_Static_assert(_Generic((uint32_t)0, unsigned int: ATOMIC_INT_LOCK_FREE, unsigned long: ATOMIC_LONG_LOCK_FREE, default: 0) == 2, "atomic uint32_t must be lock-free");

struct epoch_ring {
    _Atomic uint32_t head;    /* written by the producer only */
    _Atomic uint32_t tail;    /* written by the consumer only */
    _Atomic uint32_t dropped; /* written by the producer only; readable through epoch_ring_dropped() */
    uint32_t mask;
    uint8_t *buf;
};

/* Returns false unless capacity is a power of two between 2 and 2^31. */
bool epoch_ring_init(struct epoch_ring *r, uint8_t *buf, uint32_t capacity);
/* Producer side. On a full ring the new byte is dropped and counted. */
bool epoch_ring_put(struct epoch_ring *r, uint8_t byte);
/* Consumer side. */
bool epoch_ring_get(struct epoch_ring *r, uint8_t *byte);
uint32_t epoch_ring_dropped(struct epoch_ring *r);

#endif
