#include "epoch/journal.h"

#define W EPOCH_JOURNAL_WORDS

bool epoch_slot_valid(const struct epoch_journal_params *p, const volatile uint32_t *slot, uint32_t era)
{
    uint32_t w[W - 1];
    for (unsigned i = 0; i < W - 1; i++)
        w[i] = slot[i];
    return w[0] == p->magic_version && w[1] == era && slot[W - 1] == p->crc(w, W - 1);
}

void epoch_record_decode(const volatile uint32_t *slot, struct epoch_record *out)
{
    out->era = slot[1];
    out->session = (uint64_t)slot[3] << 32 | slot[2];
    out->txid = slot[4];
    out->counter = slot[5];
    out->seq = slot[6];
}

int epoch_journal_newest(const struct epoch_journal_params *p, const volatile uint32_t *slots, uint32_t era)
{
    bool v0 = epoch_slot_valid(p, slots, era);
    bool v1 = epoch_slot_valid(p, slots + W, era);
    /* A publish and cold initialization leave w0 at 0 until a slot is complete, so an invalid slot with any other w0 was changed after it committed. Its sequence number is unreadable, so the other slot may be the older record, and neither can be trusted as the newest. */
    if ((!v0 && slots[0] != 0u) || (!v1 && slots[W] != 0u))
        return EPOCH_SLOT_INCONSISTENT;
    if (v0 && v1) {
        uint32_t d = slots[6] - slots[W + 6];
        if (d == 1u)
            return 0;
        if (d == UINT32_MAX)
            return 1;
        return EPOCH_SLOT_INCONSISTENT; /* seq values not one step apart */
    }
    return v0 ? 0 : v1 ? 1 : EPOCH_SLOT_NONE;
}

static bool reset_at(const struct epoch_journal_io *io, const volatile uint32_t *slot, unsigned index)
{
    return io->after_store && io->after_store(io->ctx, slot, index);
}

enum epoch_publish_result epoch_journal_publish(const struct epoch_journal_params *p, const struct epoch_journal_io *io, volatile uint32_t *slots, uint32_t era, uint64_t session, uint32_t txid, uint32_t counter)
{
    int n = epoch_journal_newest(p, slots, era);
    if (n == EPOCH_SLOT_INCONSISTENT)
        return EPOCH_PUBLISH_INCONSISTENT;
    uint32_t w[W] = { p->magic_version, era, (uint32_t)session, (uint32_t)(session >> 32), txid, counter,
                      n == EPOCH_SLOT_NONE ? 0u : slots[(unsigned)n * W + 6] + 1u, 0 };
    w[W - 1] = p->crc(w, W - 1);
    volatile uint32_t *s = slots + (n == 0 ? W : 0u);

    s[0] = 0;
    io->barrier(io->ctx);
    if (reset_at(io, s, 0))
        return EPOCH_PUBLISH_RESET;
    for (unsigned i = 1; i < W; i++) {
        s[i] = w[i];
        if (i == W - 1)
            io->barrier(io->ctx);
        if (reset_at(io, s, i))
            return EPOCH_PUBLISH_RESET;
    }
    s[0] = w[0];
    io->barrier(io->ctx);
    if (reset_at(io, s, EPOCH_STORE_COMMIT))
        return EPOCH_PUBLISH_RESET;
    return EPOCH_PUBLISH_OK;
}

enum epoch_publish_result epoch_journal_cold_init(const struct epoch_journal_io *io, volatile uint32_t *slots, volatile uint32_t *era, uint32_t new_era)
{
    /* The era goes to 0 before any slot store, so a surviving older slot can never be read under the old era after an interrupted start. */
    *era = 0;
    io->barrier(io->ctx);
    if (reset_at(io, era, EPOCH_STORE_COLD_UNSET_ERA))
        return EPOCH_PUBLISH_RESET;
    slots[0] = 0;
    if (reset_at(io, slots, EPOCH_STORE_COLD_W0_SLOT0))
        return EPOCH_PUBLISH_RESET;
    slots[W] = 0;
    io->barrier(io->ctx);
    if (reset_at(io, slots + W, EPOCH_STORE_COLD_W0_SLOT1))
        return EPOCH_PUBLISH_RESET;
    *era = new_era;
    io->barrier(io->ctx);
    if (reset_at(io, era, EPOCH_STORE_COLD_ERA))
        return EPOCH_PUBLISH_RESET;
    return EPOCH_PUBLISH_OK;
}

enum epoch_outcome epoch_classify(const struct epoch_journal_params *p, bool retention_valid, const struct epoch_snapshot *snap, uint64_t session, uint32_t txid, uint32_t *counter)
{
    if (!retention_valid)
        return EPOCH_OUTCOME_UNKNOWN;
    int n = epoch_journal_newest(p, snap->slots, snap->era);
    if (n < 0)
        return EPOCH_OUTCOME_UNKNOWN;
    struct epoch_record r;
    epoch_record_decode(snap->slots + (unsigned)n * W, &r);
    /* An older slot holding s behind a newer session, or a newest txid above t, is not classified; both stay UNKNOWN. */
    if (r.session != session)
        return EPOCH_OUTCOME_UNKNOWN;
    if (r.txid == txid) {
        *counter = r.counter;
        return EPOCH_OUTCOME_COMPLETED;
    }
    return r.txid < txid ? EPOCH_OUTCOME_NOT_EXECUTED : EPOCH_OUTCOME_UNKNOWN;
}
