#include <string.h>

#include "epoch/controller.h"
#include "epoch/journal.h"
#include "sim.h"
#include "test.h"

#define W EPOCH_JOURNAL_WORDS

static const struct epoch_journal_params P = { EPOCH_TEST_MAGIC_VERSION, epoch_test_checksum };

struct probe {
    unsigned barriers;
    unsigned stores;
    unsigned reset_at; /* store index that models a reset, or 99 for none */
    bool order_ok;
    uint32_t expect[W];
};

static void count_barrier(void *ctx)
{
    ((struct probe *)ctx)->barriers++;
}

static bool watch(void *ctx, const volatile uint32_t *slot, unsigned index)
{
    struct probe *p = ctx;
    /* w0 must read 0 for every store before the commit, and each store must land the expected word. */
    if (index < EPOCH_STORE_COMMIT && slot[0] != 0u)
        p->order_ok = false;
    if (index >= 1u && index < W && slot[index] != p->expect[index])
        p->order_ok = false;
    if (index == EPOCH_STORE_COMMIT && slot[0] != EPOCH_TEST_MAGIC_VERSION)
        p->order_ok = false;
    p->stores++;
    return index == p->reset_at;
}

static void make_slot(uint32_t *s, uint32_t era, uint64_t session, uint32_t txid, uint32_t counter, uint32_t seq)
{
    uint32_t w[W] = { EPOCH_TEST_MAGIC_VERSION, era, (uint32_t)session, (uint32_t)(session >> 32), txid, counter, seq, 0 };
    w[W - 1] = epoch_test_checksum(w, W - 1);
    memcpy(s, w, sizeof w);
}

static void test_publish_alternates(void)
{
    uint32_t slots[2 * W] = { 0 };
    struct probe pr = { 0, 0, 99, true, { 0 } };
    struct epoch_journal_io io = { &pr, count_barrier, watch };
    struct epoch_record r;

    CHECK(epoch_journal_newest(&P, slots, 7) == EPOCH_SLOT_NONE);
    CHECK(epoch_journal_publish(&P, &io, slots, 7, 0x0123456789ABCDEFu, 0, 10) == EPOCH_PUBLISH_OK);
    CHECK(pr.barriers == 3u);
    CHECK(pr.stores == 9u);
    CHECK(epoch_journal_newest(&P, slots, 7) == 0);
    epoch_record_decode(slots, &r);
    CHECK(r.era == 7u && r.session == 0x0123456789ABCDEFu && r.txid == 0u && r.counter == 10u && r.seq == 0u);

    io.after_store = NULL;
    CHECK(epoch_journal_publish(&P, &io, slots, 7, 0x0123456789ABCDEFu, 1, 11) == EPOCH_PUBLISH_OK);
    CHECK(epoch_journal_newest(&P, slots, 7) == 1);
    epoch_record_decode(slots + W, &r);
    CHECK(r.txid == 1u && r.counter == 11u && r.seq == 1u);
    CHECK(epoch_journal_publish(&P, &io, slots, 7, 0x0123456789ABCDEFu, 2, 12) == EPOCH_PUBLISH_OK);
    CHECK(epoch_journal_newest(&P, slots, 7) == 0);
    epoch_record_decode(slots, &r);
    CHECK(r.txid == 2u && r.seq == 2u);
    CHECK(epoch_slot_valid(&P, slots + W, 7)); /* the previous record survives in the other slot */
}

static void test_write_order(void)
{
    uint32_t slots[2 * W] = { 0 };
    make_slot(slots, 3, 5, 1, 1, 40);
    struct probe pr = { 0, 0, 99, true, { 0, 3, 5, 0, 2, 2, 41, 0 } };
    struct epoch_journal_io io = { &pr, count_barrier, watch };
    uint32_t w[W] = { EPOCH_TEST_MAGIC_VERSION, 3, 5, 0, 2, 2, 41, 0 };
    pr.expect[7] = epoch_test_checksum(w, W - 1);
    CHECK(epoch_journal_publish(&P, &io, slots, 3, 5, 2, 2) == EPOCH_PUBLISH_OK);
    CHECK(pr.order_ok);
    CHECK(epoch_journal_newest(&P, slots, 3) == 1);
}

static void test_tear_at_every_store(void)
{
    for (unsigned k = 0; k <= EPOCH_STORE_COMMIT; k++) {
        uint32_t slots[2 * W] = { 0 };
        make_slot(slots, 3, 5, 1, 1, 0);
        struct probe pr = { 0, 0, k, true, { 0 } };
        struct epoch_journal_io io = { &pr, count_barrier, watch };
        CHECK(epoch_journal_publish(&P, &io, slots, 3, 5, 2, 2) == EPOCH_PUBLISH_RESET);
        CHECK(pr.stores == k + 1u);
        struct epoch_record r;
        int n = epoch_journal_newest(&P, slots, 3);
        if (k < EPOCH_STORE_COMMIT) {
            CHECK(!epoch_slot_valid(&P, slots + W, 3));
            CHECK(n == 0);
        } else {
            CHECK(n == 1);
        }
        epoch_record_decode(slots + (unsigned)(n < 0 ? 0 : n) * W, &r);
        CHECK(r.txid == (k < EPOCH_STORE_COMMIT ? 1u : 2u));
    }
}

static void test_validity(void)
{
    uint32_t slots[2 * W] = { 0 };
    make_slot(slots, 3, 5, 1, 1, 0);
    CHECK(epoch_slot_valid(&P, slots, 3));
    CHECK(!epoch_slot_valid(&P, slots, 4)); /* era mismatch */
    slots[5] ^= 1u;
    CHECK(!epoch_slot_valid(&P, slots, 3)); /* CRC catches corruption */
    slots[5] ^= 1u;
    slots[0] = 0;
    CHECK(!epoch_slot_valid(&P, slots, 3)); /* uncommitted */
}

static void test_newest_rule(void)
{
    uint32_t slots[2 * W];
    make_slot(slots, 3, 5, 1, 1, UINT32_MAX);
    make_slot(slots + W, 3, 5, 2, 2, 0);
    CHECK(epoch_journal_newest(&P, slots, 3) == 1); /* wrap */
    make_slot(slots, 3, 5, 1, 1, 9);
    make_slot(slots + W, 3, 5, 2, 2, 8);
    CHECK(epoch_journal_newest(&P, slots, 3) == 0);
    make_slot(slots + W, 3, 5, 2, 2, 11);
    CHECK(epoch_journal_newest(&P, slots, 3) == EPOCH_SLOT_INCONSISTENT); /* difference 2 */
    make_slot(slots + W, 3, 5, 2, 2, 9);
    CHECK(epoch_journal_newest(&P, slots, 3) == EPOCH_SLOT_INCONSISTENT); /* equal */

    uint32_t before[2 * W];
    memcpy(before, slots, sizeof slots);
    struct probe pr = { 0, 0, 99, true, { 0 } };
    struct epoch_journal_io io = { &pr, count_barrier, NULL };
    CHECK(epoch_journal_publish(&P, &io, slots, 3, 5, 3, 3) == EPOCH_PUBLISH_INCONSISTENT);
    CHECK(memcmp(before, slots, sizeof slots) == 0);
}

static void test_cold_init(void)
{
    uint32_t slots[2 * W];
    uint32_t era = 3;
    make_slot(slots, 3, 5, 1, 1, 0);
    make_slot(slots + W, 3, 5, 2, 2, 1);
    struct probe pr = { 0, 0, 99, true, { 0 } };
    struct epoch_journal_io io = { &pr, count_barrier, NULL };
    CHECK(epoch_journal_cold_init(&io, slots, &era, 9) == EPOCH_PUBLISH_OK);
    CHECK(era == 9u);
    CHECK(slots[0] == 0u && slots[W] == 0u);
    CHECK(pr.barriers == 3u);
    CHECK(epoch_journal_newest(&P, slots, 3) == EPOCH_SLOT_NONE);
    CHECK(epoch_journal_newest(&P, slots, 9) == EPOCH_SLOT_NONE);
}

static void test_classify(void)
{
    struct epoch_snapshot snap;
    uint32_t c = 0;
    memset(&snap, 0, sizeof snap);
    snap.era = 3;
    CHECK(epoch_classify(&P, true, &snap, 5, 4, &c) == EPOCH_OUTCOME_UNKNOWN); /* no valid slot */

    make_slot(snap.slots, 3, 5, 3, 30, 6);
    make_slot(snap.slots + W, 3, 5, 4, 31, 7);
    CHECK(epoch_classify(&P, false, &snap, 5, 4, &c) == EPOCH_OUTCOME_UNKNOWN); /* retention invalid */
    CHECK(epoch_classify(&P, true, &snap, 5, 4, &c) == EPOCH_OUTCOME_COMPLETED && c == 31u);
    CHECK(epoch_classify(&P, true, &snap, 5, 5, &c) == EPOCH_OUTCOME_NOT_EXECUTED);
    CHECK(epoch_classify(&P, true, &snap, 5, 3, &c) == EPOCH_OUTCOME_UNKNOWN); /* newest txid above t */
    CHECK(epoch_classify(&P, true, &snap, 6, 1, &c) == EPOCH_OUTCOME_UNKNOWN); /* no record of the session */

    make_slot(snap.slots + W, 3, 8, 0, 30, 7); /* newer SESSION_OPEN of session 8, session 5 only in the older slot */
    CHECK(epoch_classify(&P, true, &snap, 5, 4, &c) == EPOCH_OUTCOME_UNKNOWN);

    make_slot(snap.slots + W, 3, 5, 4, 31, 9); /* inconsistent pair */
    CHECK(epoch_classify(&P, true, &snap, 5, 4, &c) == EPOCH_OUTCOME_UNKNOWN);

    make_slot(snap.slots + W, 3, 5, 4, 31, 7);
    snap.slots[W + 5] ^= 1u; /* a corrupted newest slot does not let the older one decide */
    CHECK(epoch_classify(&P, true, &snap, 5, 4, &c) == EPOCH_OUTCOME_UNKNOWN);
    CHECK(epoch_classify(&P, true, &snap, 5, 5, &c) == EPOCH_OUTCOME_UNKNOWN);
}

static uint64_t fixed64(void *ctx)
{
    (void)ctx;
    return 0x1234u;
}

static uint32_t era9(void *ctx)
{
    (void)ctx;
    return 9u;
}

static void drop(void *ctx, const struct epoch_msg *m)
{
    (void)ctx;
    (void)m;
}

static void nop(void *ctx)
{
    (void)ctx;
}

static bool stop_at(void *ctx, const volatile uint32_t *slot, unsigned index)
{
    (void)slot;
    return index == *(const unsigned *)ctx;
}

/* Era 3 holding the committed t=4 (S=5, counter 4, seq 11) and its predecessor t=3 (seq 10): t=4 in slot 0 unless newest is 1. */
static void surviving_journal(struct epoch_retained *r, unsigned newest)
{
    uint32_t s[2 * W];
    make_slot(s + newest * W, 3, 5, 4, 4, 11);
    make_slot(s + (1u - newest) * W, 3, 5, 3, 3, 10);
    for (unsigned i = 0; i < 2 * W; i++)
        r->slots[i] = s[i];
    r->era = 3;
}

/* A reset after any cold-initialization store, followed by a warm or cold boot, must never yield a false outcome for the committed t=4 or accept a slot under the old era. Before the era was unset first, a reset after the slot 0 store and a warm boot gave NOT_EXECUTED. */
static void test_cold_init_crash_points(void)
{
    const struct epoch_ctrl_env env = { NULL, fixed64, era9, drop, nop, NULL, NULL };
    for (unsigned cut = EPOCH_STORE_COLD_UNSET_ERA; cut <= EPOCH_STORE_COLD_ERA; cut++) {
        for (int next_cold = 0; next_cold <= 1; next_cold++) {
          for (unsigned newest = 0; newest <= 1; newest++) {
            struct epoch_retained r;
            surviving_journal(&r, newest);
            struct epoch_journal_io io = { &cut, nop, stop_at };
            CHECK(epoch_journal_cold_init(&io, r.slots, &r.era, 7) == EPOCH_PUBLISH_RESET);
            CHECK(cut == EPOCH_STORE_COLD_ERA ? r.era == 7u : r.era == 0u);
            struct epoch_ctrl c;
            CHECK(epoch_ctrl_boot(&c, &epoch_test_ctrl_config, &env, &r, next_cold != 0, 2u, 0u) == EPOCH_CTRL_OK);
            uint32_t counter = 0;
            enum epoch_outcome o = epoch_classify(&epoch_test_ctrl_config.journal, c.retention_valid, &c.snapshot, 5, 4, &counter);
            CHECK(o == EPOCH_OUTCOME_UNKNOWN);
            CHECK(c.retention_valid == (cut == EPOCH_STORE_COLD_ERA && !next_cold));
            CHECK(r.era != 0u && r.era != 3u);
            CHECK(epoch_journal_newest(&epoch_test_ctrl_config.journal, r.slots, 3) == EPOCH_SLOT_NONE);
            counter = 0;
            CHECK(epoch_classify(&epoch_test_ctrl_config.journal, c.retention_valid, &c.snapshot, 5, 3, &counter) == EPOCH_OUTCOME_UNKNOWN);
          }
        }
    }
}

/* The cold-initialization order before the era was unset first: the slot 0 w0, the slot 1 w0, then the new era. A model kept only to show which journal layout can expose that order. */
static void old_order_cold_init(struct epoch_retained *r, uint32_t new_era, unsigned cut)
{
    r->slots[0] = 0;
    if (cut == EPOCH_STORE_COLD_W0_SLOT0)
        return;
    r->slots[W] = 0;
    if (cut == EPOCH_STORE_COLD_W0_SLOT1)
        return;
    r->era = new_era;
}

/* Under the old order a warm boot after the slot 0 store reads the committed t=4 as NOT_EXECUTED only when slot 0 held it, the layout of the board trials. When slot 1 holds it, the first store clears the predecessor: t=4 reads COMPLETED, which is true, so no false outcome is reachable, but the old era is still accepted. The current order gives UNKNOWN for both layouts (test_cold_init_crash_points). */
static void test_old_order_by_layout(void)
{
    const struct epoch_ctrl_env env = { NULL, fixed64, era9, drop, nop, NULL, NULL };
    for (unsigned newest = 0; newest <= 1; newest++) {
        for (unsigned cut = EPOCH_STORE_COLD_W0_SLOT0; cut <= EPOCH_STORE_COLD_ERA; cut++) {
            struct epoch_retained r;
            surviving_journal(&r, newest);
            old_order_cold_init(&r, 7, cut);
            struct epoch_ctrl c;
            CHECK(epoch_ctrl_boot(&c, &epoch_test_ctrl_config, &env, &r, false, 2u, 0u) == EPOCH_CTRL_OK);
            uint32_t counter = 0;
            enum epoch_outcome t4 = epoch_classify(&epoch_test_ctrl_config.journal, c.retention_valid, &c.snapshot, 5, 4, &counter);
            enum epoch_outcome t3 = epoch_classify(&epoch_test_ctrl_config.journal, c.retention_valid, &c.snapshot, 5, 3, &counter);
            bool first_store = cut == EPOCH_STORE_COLD_W0_SLOT0;
            CHECK(t4 == (first_store ? (newest ? EPOCH_OUTCOME_COMPLETED : EPOCH_OUTCOME_NOT_EXECUTED) : EPOCH_OUTCOME_UNKNOWN));
            CHECK(t3 == (first_store && !newest ? EPOCH_OUTCOME_COMPLETED : EPOCH_OUTCOME_UNKNOWN)); /* the predecessor alone survives in slot 1 */
            CHECK(c.retention_valid && c.snapshot.era == (cut == EPOCH_STORE_COLD_ERA ? 7u : 3u)); /* the old era survives every store before the last */
        }
    }
}

/* Two valid slots that break the one-step rule are corrupted retention: the boot reports retention invalid, keeps the raw pair in the snapshot, starts a new era and can publish again. */
static void test_inconsistent_pair_at_boot(void)
{
    const struct epoch_ctrl_env env = { NULL, fixed64, era9, drop, nop, NULL, NULL };
    static const uint32_t older_seq[] = { 11u, 13u, 9u }; /* equal, gap of 2 forward, gap of 2 backward */
    for (unsigned i = 0; i < sizeof older_seq / sizeof older_seq[0]; i++) {
        struct epoch_retained r;
        surviving_journal(&r, 0);
        uint32_t s1[W];
        make_slot(s1, 3, 5, 3, 3, older_seq[i]);
        for (unsigned k = 0; k < W; k++)
            r.slots[W + k] = s1[k];
        struct epoch_ctrl c;
        CHECK(epoch_ctrl_boot(&c, &epoch_test_ctrl_config, &env, &r, false, 2u, 0u) == EPOCH_CTRL_OK);
        uint32_t counter = 0;
        CHECK(!c.retention_valid && c.counter == 0u);
        CHECK(c.snapshot.era == 3u && c.snapshot.slots[W + 6] == older_seq[i]);
        CHECK(epoch_classify(&epoch_test_ctrl_config.journal, true, &c.snapshot, 5, 4, &counter) == EPOCH_OUTCOME_UNKNOWN);
        CHECK(r.era == 9u && r.slots[0] == 0u && r.slots[W] == 0u);
        struct epoch_journal_io io = { NULL, nop, NULL };
        CHECK(epoch_journal_publish(&epoch_test_ctrl_config.journal, &io, r.slots, r.era, 6, 0, 0) == EPOCH_PUBLISH_OK);
    }
}

/* Pairs the one-step rule must order or reject. Cold initialization clears w0 of both slots before it installs an era, so a committed slot from another era is corrupted retention, not an older record. */
static void test_slot_pairs(void)
{
    uint32_t s[2 * W];
    make_slot(s, 3, 5, 4, 4, 11);
    make_slot(s + W, 4, 5, 3, 3, 10);
    CHECK(epoch_journal_newest(&P, s, 3) == EPOCH_SLOT_INCONSISTENT);
    CHECK(epoch_journal_newest(&P, s, 4) == EPOCH_SLOT_INCONSISTENT);
    CHECK(epoch_journal_newest(&P, s, 5) == EPOCH_SLOT_INCONSISTENT);
    s[W] = 0;
    CHECK(epoch_journal_newest(&P, s, 3) == 0); /* the same slot with w0 cleared is empty */
    make_slot(s + W, 3, 5, 3, 3, 10);
    s[0] = 0; /* torn publication into slot 0 with the older slot surviving */
    CHECK(epoch_journal_newest(&P, s, 3) == 1);
    make_slot(s, 3, 5, 4, 4, 0);
    make_slot(s + W, 3, 5, 3, 3, UINT32_MAX);
    CHECK(epoch_journal_newest(&P, s, 3) == 0); /* wraparound */
}

/* A slot changed after it committed, while the other slot is valid: either slot may hold the newest record, so the journal is corrupted retention for every word changed, in both layouts and across the sequence wrap. The controller reports retention invalid, starts a new era without rolling the counter back to the surviving record, refuses no later session, and meets the same state again with the same result. */
static void test_corrupted_slot(void)
{
    const struct epoch_ctrl_env env = { NULL, fixed64, era9, drop, nop, NULL, NULL };
    static const uint32_t seqs[] = { 11u, 0u }; /* the newest seq; 0 puts the older record at UINT32_MAX */
    for (unsigned q = 0; q < 2; q++) {
      for (unsigned newest = 0; newest <= 1; newest++) {
        for (unsigned bad = 0; bad <= 1; bad++) { /* the slot changed: the newest record, or its predecessor */
          for (unsigned k = 0; k < 2 * W; k++) { /* every word with bit 0, then bit 8 changed: in w0 these give another VERSION and another MAGIC */
            unsigned word = k % W;
            struct epoch_retained r;
            surviving_journal(&r, newest);
            make_slot((uint32_t *)r.slots + newest * W, 3, 5, 4, 4, seqs[q]);
            make_slot((uint32_t *)r.slots + (1u - newest) * W, 3, 5, 3, 3, seqs[q] - 1u);
            unsigned slot = bad ? 1u - newest : newest;
            r.slots[slot * W + word] ^= k < W ? 1u : 0x100u;
            CHECK(epoch_journal_newest(&P, r.slots, 3) == EPOCH_SLOT_INCONSISTENT);
            uint32_t raw[2 * W];
            for (unsigned i = 0; i < 2 * W; i++)
                raw[i] = r.slots[i];
            for (unsigned boot = 0; boot < 2; boot++) {
                struct epoch_ctrl c;
                CHECK(epoch_ctrl_boot(&c, &epoch_test_ctrl_config, &env, &r, false, 2u, 0u) == EPOCH_CTRL_OK);
                CHECK(!c.retention_valid && c.counter == 0u);
                CHECK(c.snapshot.era == 3u && memcmp(c.snapshot.slots, raw, sizeof raw) == 0);
                uint32_t counter = 0;
                for (uint32_t t = 3; t <= 5; t++) /* committed t=3 and t=4, and the never-sent t=5 */
                    CHECK(epoch_classify(&P, true, &c.snapshot, 5, t, &counter) == EPOCH_OUTCOME_UNKNOWN);
                CHECK(r.era == 9u && r.slots[0] == 0u && r.slots[W] == 0u);
                struct epoch_journal_io io = { NULL, nop, NULL };
                CHECK(epoch_journal_publish(&P, &io, r.slots, r.era, 6, 0, 0) == EPOCH_PUBLISH_OK);
                for (unsigned i = 0; i < 2 * W; i++) /* the same corrupted journal is met again at the next boot */
                    r.slots[i] = raw[i];
                r.era = 3;
            }
          }
        }
      }
    }
}

/* A publish that finds a slot changed after it committed writes nothing, as for an inconsistent pair, so the corruption is neither overwritten nor joined by a record that would order against it. */
static void test_publish_refuses_corrupted_slot(void)
{
    uint32_t slots[2 * W];
    make_slot(slots, 3, 5, 3, 3, 10);
    make_slot(slots + W, 3, 5, 4, 4, 11);
    slots[W + 7] ^= 1u;
    uint32_t before[2 * W];
    memcpy(before, slots, sizeof slots);
    struct epoch_journal_io io = { NULL, nop, NULL };
    CHECK(epoch_journal_publish(&P, &io, slots, 3, 5, 5, 5) == EPOCH_PUBLISH_INCONSISTENT);
    CHECK(memcmp(before, slots, sizeof slots) == 0);
}

/* The one corruption the rule cannot see: w0 of the newest record changed to exactly 0 looks like a publish interrupted after its first store, so the older record decides and the committed t=4 reads NOT_EXECUTED. Silent corruption is outside the failure model; this test records the residual case. */
static void test_corruption_to_empty_is_not_detected(void)
{
    struct epoch_snapshot snap;
    memset(&snap, 0, sizeof snap);
    snap.era = 3;
    make_slot(snap.slots, 3, 5, 3, 3, 10);
    make_slot(snap.slots + W, 3, 5, 4, 4, 11);
    snap.slots[W] = 0;
    uint32_t c = 0;
    CHECK(epoch_journal_newest(&P, snap.slots, 3) == 0);
    CHECK(epoch_classify(&P, true, &snap, 5, 4, &c) == EPOCH_OUTCOME_NOT_EXECUTED);
}

int main(void)
{
    test_publish_alternates();
    test_write_order();
    test_tear_at_every_store();
    test_validity();
    test_newest_rule();
    test_cold_init();
    test_classify();
    test_cold_init_crash_points();
    test_old_order_by_layout();
    test_inconsistent_pair_at_boot();
    test_slot_pairs();
    test_corrupted_slot();
    test_publish_refuses_corrupted_slot();
    test_corruption_to_empty_is_not_detected();
    return TEST_DONE("journal");
}
