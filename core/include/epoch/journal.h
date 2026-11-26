#ifndef EPOCH_JOURNAL_H
#define EPOCH_JOURNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "epoch/types.h"

/* Two-slot retained journal. When both slots are valid, the newer one has a seq exactly one greater, modulo 2^32; any other difference makes the pair inconsistent. A slot is empty while its w0 is 0, which a publish and cold initialization guarantee until the slot is complete. MAGIC|VERSION and the record CRC are injected by the caller, so the core fixes neither value. Slot arrays are passed flat, slot i starting at word 8 * i. */

enum { EPOCH_SLOT_NONE = -1, EPOCH_SLOT_INCONSISTENT = -2 };

struct epoch_journal_params {
    uint32_t magic_version;                           /* MAGIC(24) | VERSION(8) */
    uint32_t (*crc)(const uint32_t *words, size_t n); /* over final w0 and w1..w6 */
};

struct epoch_record {
    uint32_t era;
    uint64_t session;
    uint32_t txid;
    uint32_t counter;
    uint32_t seq;
};

/* Called after each store of a publish, and after the barrier where the write order puts one. Index 0 is w0 := 0, 1 to 7 are w1 to w7, 8 is the final w0. Returning true models a reset at that point: the publish stops and returns EPOCH_PUBLISH_RESET. */
typedef bool (*epoch_store_hook)(void *ctx, const volatile uint32_t *slot, unsigned index);
typedef void (*epoch_barrier_fn)(void *ctx);

struct epoch_journal_io {
    void *ctx;
    epoch_barrier_fn barrier;   /* DSB on the target; required */
    epoch_store_hook after_store; /* may be NULL */
};

enum epoch_publish_result { EPOCH_PUBLISH_OK, EPOCH_PUBLISH_RESET, EPOCH_PUBLISH_INCONSISTENT };

enum { EPOCH_STORE_F2A = 0, EPOCH_STORE_F2B = 4, EPOCH_STORE_F2C = 7, EPOCH_STORE_COMMIT = 8 };
/* Store indices of epoch_journal_cold_init passed to the store hook; slot points at the stored word. */
enum { EPOCH_STORE_COLD_UNSET_ERA = 9, EPOCH_STORE_COLD_W0_SLOT0 = 10, EPOCH_STORE_COLD_W0_SLOT1 = 11, EPOCH_STORE_COLD_ERA = 12 };

bool epoch_slot_valid(const struct epoch_journal_params *p, const volatile uint32_t *slot, uint32_t era);
void epoch_record_decode(const volatile uint32_t *slot, struct epoch_record *out);
/* Returns 0 or 1, EPOCH_SLOT_NONE when no slot is valid, or EPOCH_SLOT_INCONSISTENT for corrupted retention: two valid slots that break the one-step sequence rule, or an invalid slot whose w0 is not 0 (a wrong MAGIC or VERSION, another era or a CRC mismatch). A corruption that sets w0 of a committed slot to exactly 0 is indistinguishable from an interrupted publish and is not detected. */
int epoch_journal_newest(const struct epoch_journal_params *p, const volatile uint32_t *slots, uint32_t era);
/* Writes (session, txid, counter) into the slot not holding the newest valid record, with seq newest+1, or seq 0 into slot 0 when no slot is valid. */
enum epoch_publish_result epoch_journal_publish(const struct epoch_journal_params *p, const struct epoch_journal_io *io, volatile uint32_t *slots, uint32_t era, uint64_t session, uint32_t txid, uint32_t counter);
/* Cold start: set the era to 0 (unset) and barrier, zero w0 of both slots and barrier, then install new_era and barrier. A reset anywhere before the last store leaves the era unset whatever its previous value, so the next boot sees retention invalid and runs cold initialization again, even when it is judged warm. new_era must not be 0. Returns EPOCH_PUBLISH_RESET when the store hook models a reset. */
enum epoch_publish_result epoch_journal_cold_init(const struct epoch_journal_io *io, volatile uint32_t *slots, volatile uint32_t *era, uint32_t new_era);
/* Classifies a lost-reply INCREMENT t in session s from the journal snapshot reported after a reset. On COMPLETED, *counter receives the committed value. */
enum epoch_outcome epoch_classify(const struct epoch_journal_params *p, bool retention_valid, const struct epoch_snapshot *snap, uint64_t session, uint32_t txid, uint32_t *counter);

#endif
