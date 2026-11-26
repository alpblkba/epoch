#include <string.h>

#include "epoch/journal.h"
#include "sim.h"
#include "test.h"

/* Model check of the journal against its history, not against the slot rule: every state is reached by real publishes (or one change after them), the ground truth is which records committed, and each classification must agree with that truth. The newest-slot rule of the production code is never consulted. */

#define W EPOCH_JOURNAL_WORDS
#define S 0x1122334455667788ull
#define OTHER 0x99ull
#define ERA 3u
#define NONE 99u

static const struct epoch_journal_params P = { EPOCH_TEST_MAGIC_VERSION, epoch_test_checksum };

static void nop(void *ctx)
{
    (void)ctx;
}

static bool stop_at(void *ctx, const volatile uint32_t *slot, unsigned index)
{
    (void)slot;
    return index == *(unsigned *)ctx;
}

enum change { INTACT, FLIP, ZERO_W0_NEWEST, ZERO_W0_OLDER, FORGED, OTHER_ERA };

struct counts {
    unsigned states, decisive, violations, control_violations;
};

/* What classify may say about txid t of session s when last is the newest committed txid of S (0 for none) and inflight the txid whose publish was cut before its commit (0 for none). */
static bool sound(enum epoch_outcome o, uint64_t s, uint32_t t, uint32_t last)
{
    if (o == EPOCH_OUTCOME_COMPLETED)
        return s == S && t == last;
    if (o == EPOCH_OUTCOME_NOT_EXECUTED)
        return s == S && t > last;
    return true;
}

static void judge(struct counts *n, const uint32_t *slots, uint32_t last, uint32_t inflight, enum change ch)
{
    struct epoch_snapshot snap;
    memcpy(snap.slots, slots, sizeof snap.slots);
    snap.era = ch == OTHER_ERA ? ERA + 1u : ERA;
    bool ok = true;
    n->states++;
    for (uint32_t t = 1; t <= last + 2u; t++) {
        uint32_t counter = 0;
        enum epoch_outcome o = epoch_classify(&P, true, &snap, S, t, &counter);
        ok = ok && sound(o, S, t, last) && epoch_classify(&P, true, &snap, OTHER, t, &counter) == EPOCH_OUTCOME_UNKNOWN;
        if (ch == INTACT) { /* liveness: with nothing changed, the newest commit and the request cut before its commit are decided */
            if (t == last)
                ok = ok && o == EPOCH_OUTCOME_COMPLETED && counter == last;
            if (t == inflight)
                ok = ok && o == EPOCH_OUTCOME_NOT_EXECUTED;
        }
        if (ch == FORGED || ch == OTHER_ERA) /* a pair that breaks the one-step rule, and records of another era, decide nothing */
            ok = ok && o == EPOCH_OUTCOME_UNKNOWN;
        n->decisive += o != EPOCH_OUTCOME_UNKNOWN;
        ok = ok && epoch_classify(&P, false, &snap, S, t, &counter) == EPOCH_OUTCOME_UNKNOWN; /* retention invalid decides nothing */
    }
    if (!ok && ch == ZERO_W0_NEWEST)
        n->control_violations++;
    else if (!ok)
        n->violations++;
}

int main(void)
{
    static const uint32_t bases[] = { 5u, UINT32_MAX - 1u }; /* the second wraps the sequence within the history */
    struct counts n = { 0, 0, 0, 0 };
    for (unsigned b = 0; b < 2; b++) {
        for (unsigned place = 0; place <= 1; place++) {     /* the slot of the first record */
            for (uint32_t later = 0; later <= 3; later++) {  /* publishes after the first record */
                for (unsigned cut = 0; cut <= NONE; cut = cut == EPOCH_STORE_COMMIT ? NONE : cut + 1u) { /* reset at a store of the last publish, or none */
                    uint32_t slots[2 * W] = { 0 };
                    uint32_t first[W] = { EPOCH_TEST_MAGIC_VERSION, ERA, (uint32_t)S, (uint32_t)(S >> 32), 1, 1, bases[b], 0 };
                    first[W - 1] = epoch_test_checksum(first, W - 1);
                    memcpy(slots + place * W, first, sizeof first);
                    uint32_t last = 1, inflight = 0;
                    for (uint32_t t = 2; t <= later + 1u; t++) {
                        unsigned at = t == later + 1u ? cut : NONE;
                        struct epoch_journal_io io = { &at, nop, stop_at };
                        enum epoch_publish_result r = epoch_journal_publish(&P, &io, slots, ERA, S, t, t);
                        CHECK(r == (at == NONE ? EPOCH_PUBLISH_OK : EPOCH_PUBLISH_RESET));
                        if (at == NONE || at == EPOCH_STORE_COMMIT) /* the record committed once its final w0 store landed */
                            last = t;
                        else
                            inflight = t;
                    }
                    if (later == 0 && cut != NONE)
                        continue; /* no publish to cut */
                    judge(&n, slots, last, inflight, INTACT);
                    judge(&n, slots, last, inflight, OTHER_ERA);
                    int newest = -1;
                    for (int i = 0; i < 2; i++) /* from the history: the slot holding the last committed record */
                        if (slots[(unsigned)i * W] == EPOCH_TEST_MAGIC_VERSION && slots[(unsigned)i * W + 4] == last)
                            newest = i;
                    CHECK(newest >= 0);
                    for (unsigned k = 0; k < 2 * W * 2; k++) { /* bit 0, then bit 8, of every word of both slots */
                        uint32_t c[2 * W];
                        memcpy(c, slots, sizeof c);
                        c[k % (2 * W)] ^= k < 2 * W ? 1u : 0x100u;
                        judge(&n, c, last, inflight, FLIP);
                    }
                    uint32_t c[2 * W];
                    memcpy(c, slots, sizeof c);
                    c[(unsigned)newest * W] = 0;
                    judge(&n, c, last, inflight, ZERO_W0_NEWEST);
                    memcpy(c, slots, sizeof c);
                    c[(unsigned)(1 - newest) * W] = 0;
                    judge(&n, c, last, inflight, ZERO_W0_OLDER);
                    for (uint32_t gap = 0; gap <= 2 && last > 1u; gap += 2) { /* the other slot rewritten as a stale record of last - 1, valid on its own, with the same seq or seq + 2, as the forge hook writes pairs */
                        uint32_t *o = c + (unsigned)(1 - newest) * W;
                        memcpy(c, slots, sizeof c);
                        uint32_t r[W] = { EPOCH_TEST_MAGIC_VERSION, ERA, (uint32_t)S, (uint32_t)(S >> 32), last - 1u, last - 1u, slots[(unsigned)newest * W + 6] + gap, 0 };
                        r[W - 1] = epoch_test_checksum(r, W - 1);
                        memcpy(o, r, sizeof r);
                        judge(&n, c, last, inflight, FORGED);
                    }
                }
            }
        }
    }
    printf("journal_model: %u states, %u decisive classifications, %u violations, %u in the w0-to-0 control\n", n.states, n.decisive, n.violations, n.control_violations);
    CHECK(n.violations == 0u);
    CHECK(n.control_violations > 0u); /* the known residual case: w0 of the newest record changed to exactly 0 reads as an empty slot, so the oracle must see it */
    CHECK(n.decisive > 0u);
    return TEST_DONE("journal_model");
}
