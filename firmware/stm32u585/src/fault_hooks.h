#ifndef EPOCH_FAULT_HOOKS_H
#define EPOCH_FAULT_HOOKS_H

/* Test-only reset injection for the fault build (fault.conf), kept free of Zephyr headers so the native tests in tests/fault_hooks compile the same code. Never part of a production image. */

#include <stdbool.h>
#include <stdint.h>

#include "epoch/journal.h"
#include "epoch/types.h"

#define FAULT_MARKER_MAGIC 0x4D4B5231u /* "MKR1": checkpoint reached */
#define FAULT_ARM_MAGIC 0x41524D31u    /* "ARM1": cold-initialization reset armed for the next boot */

/* TEST_ARM checkpoint codes: 0 to 4 are enum epoch_checkpoint, 9 to 12 the cold-initialization stores EPOCH_STORE_COLD_*, 13 the inconsistent-pair forge. For 9 to 13 the txid field carries the mode. */
enum { FAULT_CP_FORGE = 13 };
enum { FAULT_NEXT_COLD = 0, FAULT_NEXT_WARM = 1 };          /* codes 9 to 12: how the firmware judges the boot after the injected reset */
enum { FAULT_FORGE_SEQ_PLUS_2 = 0, FAULT_FORGE_SEQ_EQUAL = 1 }; /* code 13: seq of the forged copy */
enum fault_request { FAULT_IGNORE, FAULT_RUNTIME, FAULT_COLD, FAULT_FORGE };

/* Retained record in SRAM1 .noinit: run, checkpoint and txid, their CRC, and the magic written last. */
struct fault_record {
    uint32_t magic, run, checkpoint, txid, crc;
};

/* An armed cold-initialization reset taken at boot; fires at most once. */
struct fault_cold {
    struct fault_record arm;
    bool armed;
};

enum fault_request fault_decode(uint32_t checkpoint, uint32_t txid);
/* Writes run, checkpoint, txid and the CRC, a barrier, then the magic and a barrier, so a torn record reads invalid and a complete one is committed on return. */
void fault_record_write(volatile struct fault_record *r, uint32_t magic, uint32_t run, uint32_t checkpoint, uint32_t txid, epoch_barrier_fn barrier, void *ctx);
bool fault_record_valid(const volatile struct fault_record *r, uint32_t magic);
/* Clears the stored arm, with a barrier, before returning what it held, so no later reset can fire it again; c->armed is true only for a valid cold-initialization arm. */
void fault_cold_take(volatile struct fault_record *stored, struct fault_cold *c, epoch_barrier_fn barrier, void *ctx);
/* True once, at the armed store index. */
bool fault_cold_hit(struct fault_cold *c, unsigned index);
/* A byte transmitter as the firmware sees it: put returns once the byte is queued, sent reports that every queued byte, stop bit included, has left the line. */
struct fault_tx {
    void *ctx;
    void (*put)(void *ctx, uint8_t byte);
    bool (*sent)(void *ctx);
    void (*wait_us)(void *ctx, uint32_t us);
};
enum { FAULT_TX_POLL_US = 10 };
/* Encodes m, queues every byte and waits until the transmitter reports them sent, at most limit_us; returns false on timeout. Used before a test reset, which otherwise cuts the bytes still queued. */
bool fault_send_complete(const struct fault_tx *tx, const struct epoch_msg *m, uint32_t limit_us);
/* Copies the newest valid record into the other slot with seq + 2 or the same seq and a correct CRC, w0 last, so both slots are valid and break the one-step rule. With CONFIG_EPOCH_FAULT_CORRUPT, mode FAULT_FORGE_SEQ_EQUAL instead changes bit 0 of word 5 of the newest valid slot and nothing else. Returns false and writes nothing when no slot is valid. */
bool fault_forge_pair(const struct epoch_journal_params *p, volatile uint32_t *slots, uint32_t era, uint32_t mode, epoch_barrier_fn barrier, void *ctx);

#endif
