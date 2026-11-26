#include "fault_hooks.h"

#include "epoch/crc32.h"
#include "epoch/wire.h"

#define W EPOCH_JOURNAL_WORDS

enum fault_request fault_decode(uint32_t checkpoint, uint32_t txid)
{
    if (checkpoint <= EPOCH_CP_F3)
        return FAULT_RUNTIME;
    if (txid > 1u)
        return FAULT_IGNORE;
    if (checkpoint >= EPOCH_STORE_COLD_UNSET_ERA && checkpoint <= EPOCH_STORE_COLD_ERA)
        return FAULT_COLD;
    return checkpoint == FAULT_CP_FORGE ? FAULT_FORGE : FAULT_IGNORE;
}

void fault_record_write(volatile struct fault_record *r, uint32_t magic, uint32_t run, uint32_t checkpoint, uint32_t txid, epoch_barrier_fn barrier, void *ctx)
{
    const uint32_t w[3] = { run, checkpoint, txid };
    r->magic = 0;
    barrier(ctx);
    r->run = run;
    r->checkpoint = checkpoint;
    r->txid = txid;
    r->crc = epoch_crc32_words(w, 3);
    barrier(ctx);
    r->magic = magic;
    barrier(ctx);
}

bool fault_record_valid(const volatile struct fault_record *r, uint32_t magic)
{
    const uint32_t w[3] = { r->run, r->checkpoint, r->txid };
    return r->magic == magic && r->crc == epoch_crc32_words(w, 3);
}

void fault_cold_take(volatile struct fault_record *stored, struct fault_cold *c, epoch_barrier_fn barrier, void *ctx)
{
    bool valid = fault_record_valid(stored, FAULT_ARM_MAGIC);
    c->arm.magic = stored->magic;
    c->arm.run = stored->run;
    c->arm.checkpoint = stored->checkpoint;
    c->arm.txid = stored->txid;
    c->arm.crc = stored->crc;
    stored->magic = 0; /* unconditionally, so power-on contents can never arm a later boot either */
    barrier(ctx);
    c->armed = valid && fault_decode(c->arm.checkpoint, c->arm.txid) == FAULT_COLD;
}

bool fault_cold_hit(struct fault_cold *c, unsigned index)
{
    if (!c->armed || index != c->arm.checkpoint)
        return false;
    c->armed = false;
    return true;
}

bool fault_send_complete(const struct fault_tx *tx, const struct epoch_msg *m, uint32_t limit_us)
{
    uint8_t frame[EPOCH_WIRE_MAX_FRAME];
    size_t n = epoch_wire_encode(m, frame, sizeof frame);
    for (size_t i = 0; i < n; i++)
        tx->put(tx->ctx, frame[i]);
    for (uint32_t waited = 0; !tx->sent(tx->ctx); waited += FAULT_TX_POLL_US) {
        if (waited >= limit_us)
            return false;
        tx->wait_us(tx->ctx, FAULT_TX_POLL_US);
    }
    return true;
}

bool fault_forge_pair(const struct epoch_journal_params *p, volatile uint32_t *slots, uint32_t era, uint32_t mode, epoch_barrier_fn barrier, void *ctx)
{
    int n = epoch_journal_newest(p, slots, era);
    if (n < 0)
        return false;
#ifdef CONFIG_EPOCH_FAULT_CORRUPT
    if (mode == FAULT_FORGE_SEQ_EQUAL) { /* the only write: w0 stays MAGIC|VERSION and the CRC no longer matches */
        slots[(unsigned)n * W + 5u] ^= 1u;
        barrier(ctx);
        return true;
    }
#endif
    uint32_t w[W];
    for (unsigned i = 0; i < W; i++)
        w[i] = slots[(unsigned)n * W + i];
    w[6] += mode == FAULT_FORGE_SEQ_EQUAL ? 0u : 2u;
    w[W - 1] = p->crc(w, W - 1);
    volatile uint32_t *s = slots + (n == 0 ? W : 0u);
    s[0] = 0;
    barrier(ctx);
    for (unsigned i = 1; i < W; i++)
        s[i] = w[i];
    barrier(ctx);
    s[0] = w[0];
    barrier(ctx);
    return true;
}
