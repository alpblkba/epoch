#include <string.h>

#include "epoch/controller.h"
#include "epoch/crc32.h"
#include "epoch/journal.h"
#include "epoch/wire.h"
#include "fault_hooks.h"
#include "test.h"

#define W EPOCH_JOURNAL_WORDS
#define MAGIC 0x45504301u
#define ERA0 0x5794D19Bu
#define SESSION 0xF27741366E83E7ABull

static const struct epoch_ctrl_config cfg = { { MAGIC, epoch_crc32_words }, 2000, 1, 1, 0 };

/* USART1 as the firmware drives it: put waits for TDR to empty as uart_poll_out does, TDR moves to the shift register as soon as it is idle, each byte takes BYTE_US on the line, and a reset keeps only the bytes whose stop bit was sent. */
#define BYTE_US 87u /* 10 bits at 115200 baud, rounded up */
struct uart_model {
    uint32_t now, shift_end, last_put;
    bool tdr_full, shifting, never_done;
    uint8_t tdr, shift;
    uint8_t line[2 * EPOCH_WIRE_MAX_FRAME];
    size_t n;
};

static void settle(struct uart_model *u)
{
    if (u->shifting && u->now >= u->shift_end) {
        u->line[u->n++] = u->shift;
        u->shifting = false;
    }
    if (!u->shifting && u->tdr_full) {
        u->shift = u->tdr;
        u->tdr_full = false;
        u->shifting = true;
        u->shift_end = u->now + BYTE_US;
    }
}

static void tick(struct uart_model *u, uint32_t us)
{
    while (us--) {
        u->now++;
        settle(u);
    }
}

static void model_put(void *ctx, uint8_t byte)
{
    struct uart_model *u = ctx;
    while (u->tdr_full)
        tick(u, 1);
    u->tdr = byte;
    u->tdr_full = true;
    settle(u);
    u->last_put = u->now;
}

static bool model_sent(void *ctx)
{
    struct uart_model *u = ctx;
    return !u->never_done && !u->tdr_full && !u->shifting; /* USART_ISR.TC */
}

static void model_wait(void *ctx, uint32_t us)
{
    tick(ctx, us);
}

/* Feeds what reached the line at the reset through the strict parser; returns the number of frames decoded, *bad the number rejected, and the last good message in *m. */
static unsigned parse_line(const struct uart_model *u, const uint8_t *after, size_t after_n, struct epoch_msg *m, unsigned *bad)
{
    struct epoch_wire_parser p;
    epoch_wire_parser_init(&p);
    unsigned ok = 0;
    *bad = 0;
    for (size_t i = 0; i < u->n + after_n; i++) {
        struct epoch_msg x;
        enum epoch_wire_status st = epoch_wire_feed(&p, i < u->n ? u->line[i] : after[i - u->n], &x);
        if (st == EPOCH_WIRE_OK) {
            ok++;
            *m = x;
        } else if (st != EPOCH_WIRE_NONE) {
            (*bad)++;
        }
    }
    return ok;
}

/* The board as controller_app.c sees it: retained journal, TAMP_BKP30R, the two R1 records and the arm taken at boot. */
struct board {
    struct epoch_retained ret;
    bool bkp30;
    struct fault_record marker, arm;
    struct fault_cold cold;
    unsigned resets;
    uint32_t next_era;
    uint64_t rng;
    struct uart_model uart;
};

static void nop(void *ctx)
{
    (void)ctx;
}

static uint64_t rand64(void *ctx)
{
    return ++((struct board *)ctx)->rng;
}

static uint32_t new_era(void *ctx)
{
    return ++((struct board *)ctx)->next_era;
}

static void drop(void *ctx, const struct epoch_msg *m)
{
    (void)ctx;
    (void)m;
}

/* cold_store_hook in controller_app.c */
static bool hook(void *ctx, const volatile uint32_t *slot, unsigned index)
{
    struct board *b = ctx;
    (void)slot;
    if (!fault_cold_hit(&b->cold, index))
        return false;
    if (b->cold.arm.txid == FAULT_NEXT_WARM)
        b->bkp30 = true;
    fault_record_write(&b->marker, FAULT_MARKER_MAGIC, b->cold.arm.run, index, b->cold.arm.txid, nop, NULL);
    return true;
}

/* main in controller_app.c after a reset that kept the backup domain; counts the reset that ends a boot stopped by the hook. */
static enum epoch_ctrl_result boot(struct board *b, struct epoch_ctrl *c)
{
    const struct epoch_ctrl_env env = { b, rand64, new_era, drop, nop, NULL, hook };
    bool cold = !b->bkp30;
    fault_cold_take(&b->arm, &b->cold, nop, NULL);
    memset(&b->uart, 0, sizeof b->uart); /* the USART starts idle after every reset */
    if (b->cold.armed) {
        const struct fault_tx tx = { &b->uart, model_put, model_sent, model_wait };
        struct epoch_msg r = { 0 };
        r.type = EPOCH_MSG_TEST_DIAG_REPLY;
        r.run = b->cold.arm.run;
        r.checkpoint = b->cold.arm.checkpoint;
        r.txid = b->cold.arm.txid;
        r.reset_cause = 0x10000000u;
        CHECK(fault_send_complete(&tx, &r, 1000));
    }
    enum epoch_ctrl_result r = epoch_ctrl_boot(c, &cfg, &env, &b->ret, cold, 0x10000000u, 0);
    if (r == EPOCH_CTRL_RESET) {
        b->resets++;
        return r; /* b->uart.line holds what the host received before this reset */
    }
    b->bkp30 = true;
    return r;
}

/* fault_test for TEST_ARM with a cold-initialization code, ending in the preparatory reset. */
static void arm_cold(struct board *b, uint32_t run, uint32_t cp, uint32_t mode)
{
    CHECK(fault_decode(cp, mode) == FAULT_COLD);
    b->marker.magic = 0;
    fault_record_write(&b->arm, FAULT_ARM_MAGIC, run, cp, mode, nop, NULL);
    b->bkp30 = false;
    b->resets++;
}

/* SESSION_OPEN, then t=1 up to last, all of one session: last 2 leaves t=2 newest in slot 0 and t=1 in slot 1, last 3 leaves t=3 newest in slot 1 and t=2 in slot 0. */
static void commits(struct board *b, uint32_t last)
{
    memset(b, 0, sizeof *b);
    b->ret.era = ERA0;
    b->bkp30 = true;
    b->next_era = 0x100;
    const struct epoch_journal_io io = { NULL, nop, NULL };
    for (uint32_t t = 0; t <= last; t++)
        CHECK(epoch_journal_publish(&cfg.journal, &io, b->ret.slots, ERA0, SESSION, t, 3134u + t) == EPOCH_PUBLISH_OK);
    CHECK(epoch_journal_newest(&cfg.journal, b->ret.slots, ERA0) == (int)(last % 2u == 0u ? 0 : 1));
}

static void two_commits(struct board *b)
{
    commits(b, 2);
}

static void test_decode(void)
{
    for (uint32_t cp = 0; cp <= EPOCH_CP_F3; cp++)
        CHECK(fault_decode(cp, 77) == FAULT_RUNTIME);
    for (uint32_t mode = 0; mode <= 1; mode++) {
        for (uint32_t cp = EPOCH_STORE_COLD_UNSET_ERA; cp <= EPOCH_STORE_COLD_ERA; cp++)
            CHECK(fault_decode(cp, mode) == FAULT_COLD);
        CHECK(fault_decode(FAULT_CP_FORGE, mode) == FAULT_FORGE);
        for (uint32_t cp = EPOCH_CP_F4; cp < EPOCH_STORE_COLD_UNSET_ERA; cp++)
            CHECK(fault_decode(cp, mode) == FAULT_IGNORE);
        CHECK(fault_decode(14, mode) == FAULT_IGNORE);
        CHECK(fault_decode(UINT32_MAX, mode) == FAULT_IGNORE);
    }
    CHECK(fault_decode(EPOCH_STORE_COLD_UNSET_ERA, 2) == FAULT_IGNORE);
    CHECK(fault_decode(FAULT_CP_FORGE, 2) == FAULT_IGNORE);
}

struct order {
    struct fault_record *r;
    unsigned n;
    bool ok;
};

static void check_order(void *ctx)
{
    struct order *o = ctx;
    o->n++;
    /* before the third barrier the magic must still read 0; at the third the record must be complete */
    if (o->n < 3 && o->r->magic != 0u)
        o->ok = false;
    if (o->n == 3 && !fault_record_valid(o->r, FAULT_MARKER_MAGIC))
        o->ok = false;
}

static void test_record(void)
{
    struct fault_record r;
    memset(&r, 0xA5, sizeof r);
    CHECK(!fault_record_valid(&r, FAULT_MARKER_MAGIC));
    struct order o = { &r, 0, true };
    fault_record_write(&r, FAULT_MARKER_MAGIC, 907010, 10, 1, check_order, &o);
    CHECK(o.ok && o.n == 3);
    CHECK(fault_record_valid(&r, FAULT_MARKER_MAGIC) && !fault_record_valid(&r, FAULT_ARM_MAGIC));
    uint32_t *f[] = { &r.run, &r.checkpoint, &r.txid, &r.crc };
    for (unsigned i = 0; i < 4; i++) {
        *f[i] ^= 1u;
        CHECK(!fault_record_valid(&r, FAULT_MARKER_MAGIC));
        *f[i] ^= 1u;
    }
    r.magic = 0; /* torn: the magic is written last */
    CHECK(!fault_record_valid(&r, FAULT_MARKER_MAGIC));
}

static void test_take_is_one_shot(void)
{
    struct fault_record stored;
    struct fault_cold c;
    fault_record_write(&stored, FAULT_ARM_MAGIC, 5, EPOCH_STORE_COLD_W0_SLOT0, FAULT_NEXT_WARM, nop, NULL);
    fault_cold_take(&stored, &c, nop, NULL);
    CHECK(c.armed && c.arm.run == 5 && c.arm.checkpoint == EPOCH_STORE_COLD_W0_SLOT0 && c.arm.txid == FAULT_NEXT_WARM);
    CHECK(!fault_record_valid(&stored, FAULT_ARM_MAGIC));
    CHECK(!fault_cold_hit(&c, EPOCH_STORE_COLD_UNSET_ERA));
    CHECK(fault_cold_hit(&c, EPOCH_STORE_COLD_W0_SLOT0));
    CHECK(!fault_cold_hit(&c, EPOCH_STORE_COLD_W0_SLOT0));
    fault_cold_take(&stored, &c, nop, NULL);
    CHECK(!c.armed);

    /* a valid record whose code is not a cold store, and power-on contents with the magic but a wrong CRC, never arm and are cleared */
    fault_record_write(&stored, FAULT_ARM_MAGIC, 5, EPOCH_CP_F3, 0, nop, NULL);
    fault_cold_take(&stored, &c, nop, NULL);
    CHECK(!c.armed && stored.magic == 0u);
    memset(&stored, 0x5A, sizeof stored);
    stored.magic = FAULT_ARM_MAGIC;
    fault_cold_take(&stored, &c, nop, NULL);
    CHECK(!c.armed && stored.magic == 0u);
    for (unsigned i = 0; i <= 8; i++)
        CHECK(!fault_cold_hit(&c, i)); /* publish stores never match */
}

/* An interrupted cold initialization on the board: preparatory reset, reset at the armed cold-initialization store, then a boot judged cold or warm. */
static void test_cold_interruption(void)
{
    for (uint32_t cut = EPOCH_STORE_COLD_UNSET_ERA; cut <= EPOCH_STORE_COLD_ERA; cut++) {
        for (uint32_t mode = FAULT_NEXT_COLD; mode <= FAULT_NEXT_WARM; mode++) {
          for (uint32_t last = 2; last <= 3; last++) { /* newest record in slot 0, as on the board, then in slot 1 */
            struct board b;
            struct epoch_ctrl c;
            commits(&b, last);
            unsigned n = last == 2u ? 0u : 1u;
            arm_cold(&b, 900 + cut, cut, mode);
            CHECK(boot(&b, &c) == EPOCH_CTRL_RESET);
            CHECK(b.resets == 2u); /* preparatory and injected, counted apart */
            struct epoch_msg rep;
            unsigned bad;
            CHECK(parse_line(&b.uart, NULL, 0, &rep, &bad) == 1u && bad == 0u); /* the preparatory report left the line whole before the injected reset */
            CHECK(rep.type == EPOCH_MSG_TEST_DIAG_REPLY && !rep.marker_valid && rep.run == 900 + cut && rep.checkpoint == cut && rep.txid == mode);
            CHECK(fault_record_valid(&b.marker, FAULT_MARKER_MAGIC) && b.marker.run == 900 + cut && b.marker.checkpoint == cut && b.marker.txid == mode);
            CHECK(!fault_record_valid(&b.arm, FAULT_ARM_MAGIC));
            CHECK(b.bkp30 == (mode == FAULT_NEXT_WARM)); /* the completed-boot marker was not written */
            CHECK(b.ret.era == (cut == EPOCH_STORE_COLD_ERA ? 0x101u : 0u));
            if (cut == EPOCH_STORE_COLD_W0_SLOT0) {
                /* the store-10 state: slot 0 lost only w0, and restoring it shows the newest record one step above its predecessor, both of the session, under the old era */
                uint32_t s[2 * W];
                for (unsigned i = 0; i < 2 * W; i++)
                    s[i] = b.ret.slots[i];
                CHECK(s[0] == 0u);
                s[0] = MAGIC;
                CHECK(epoch_slot_valid(&cfg.journal, s, ERA0) && epoch_slot_valid(&cfg.journal, s + W, ERA0));
                CHECK(s[n * W + 4] == last && s[(1u - n) * W + 4] == last - 1u && s[n * W + 6] - s[(1u - n) * W + 6] == 1u);
                CHECK(s[2] == (uint32_t)SESSION && s[W + 3] == (uint32_t)(SESSION >> 32));
            }
            CHECK(boot(&b, &c) == EPOCH_CTRL_OK);
            CHECK(c.retention_valid == (cut == EPOCH_STORE_COLD_ERA && mode == FAULT_NEXT_WARM));
            CHECK(c.snapshot.era == (cut == EPOCH_STORE_COLD_ERA ? 0x101u : 0u));
            for (uint32_t t = 1; t <= last; t++) {
                uint32_t counter = 0;
                CHECK(epoch_classify(&cfg.journal, c.retention_valid, &c.snapshot, SESSION, t, &counter) == EPOCH_OUTCOME_UNKNOWN);
            }
            CHECK(b.ret.era != 0u && b.ret.era != ERA0);
            CHECK(epoch_journal_newest(&cfg.journal, b.ret.slots, b.ret.era) == EPOCH_SLOT_NONE);
            for (unsigned i = 0; i < 20; i++)
                CHECK(boot(&b, &c) == EPOCH_CTRL_OK && c.retention_valid); /* no further injected reset */
            CHECK(b.resets == 2u);
          }
        }
    }
}

/* An inconsistent pair on the board: both forge modes give two slots that are each valid but break the one-step rule; the next warm boot keeps the raw pair in its snapshot and starts a new era. */
#ifdef CONFIG_EPOCH_FAULT_CORRUPT
#define LAST_PAIR_MODE FAULT_FORGE_SEQ_PLUS_2 /* mode 1 corrupts in this build, see test_corrupt */
#else
#define LAST_PAIR_MODE FAULT_FORGE_SEQ_EQUAL
#endif
static void test_forge(void)
{
    for (uint32_t mode = FAULT_FORGE_SEQ_PLUS_2; mode <= LAST_PAIR_MODE; mode++) {
        struct board b;
        struct epoch_ctrl c;
        two_commits(&b);
        uint32_t newest[W];
        for (unsigned i = 0; i < W; i++)
            newest[i] = b.ret.slots[i];
        CHECK(fault_decode(FAULT_CP_FORGE, mode) == FAULT_FORGE);
        CHECK(fault_forge_pair(&cfg.journal, b.ret.slots, ERA0, mode, nop, NULL));
        CHECK(epoch_slot_valid(&cfg.journal, b.ret.slots, ERA0) && epoch_slot_valid(&cfg.journal, b.ret.slots + W, ERA0));
        CHECK(epoch_journal_newest(&cfg.journal, b.ret.slots, ERA0) == EPOCH_SLOT_INCONSISTENT);
        CHECK(b.ret.slots[W + 6] - b.ret.slots[6] == (mode == FAULT_FORGE_SEQ_EQUAL ? 0u : 2u));
        for (unsigned i = 0; i < 6; i++)
            CHECK(b.ret.slots[i] == newest[i] && b.ret.slots[W + i] == newest[i]);
        uint32_t raw[2 * W];
        for (unsigned i = 0; i < 2 * W; i++)
            raw[i] = b.ret.slots[i];
        CHECK(boot(&b, &c) == EPOCH_CTRL_OK && b.resets == 0u);
        CHECK(!c.retention_valid && c.snapshot.era == ERA0 && memcmp(c.snapshot.slots, raw, sizeof raw) == 0);
        uint32_t counter = 0;
        CHECK(epoch_classify(&cfg.journal, c.retention_valid, &c.snapshot, SESSION, 2, &counter) == EPOCH_OUTCOME_UNKNOWN);
        CHECK(b.ret.era == 0x101u && epoch_journal_newest(&cfg.journal, b.ret.slots, b.ret.era) == EPOCH_SLOT_NONE);
        const struct epoch_journal_io io = { NULL, nop, NULL };
        CHECK(epoch_journal_publish(&cfg.journal, &io, b.ret.slots, b.ret.era, 7, 0, 0) == EPOCH_PUBLISH_OK);
        CHECK(boot(&b, &c) == EPOCH_CTRL_OK && c.retention_valid && c.counter == 0u);
    }

    /* newest in slot 1 forges slot 0; one valid slot forges the other; no valid slot writes nothing */
    struct board b;
    two_commits(&b);
    const struct epoch_journal_io io = { NULL, nop, NULL };
    CHECK(epoch_journal_publish(&cfg.journal, &io, b.ret.slots, ERA0, SESSION, 3, 3137) == EPOCH_PUBLISH_OK);
    CHECK(fault_forge_pair(&cfg.journal, b.ret.slots, ERA0, FAULT_FORGE_SEQ_PLUS_2, nop, NULL));
    CHECK(b.ret.slots[4] == 3u && b.ret.slots[6] == 5u);
    b.ret.slots[W] = 0;
    CHECK(fault_forge_pair(&cfg.journal, b.ret.slots, ERA0, LAST_PAIR_MODE, nop, NULL));
    CHECK(epoch_journal_newest(&cfg.journal, b.ret.slots, ERA0) == EPOCH_SLOT_INCONSISTENT);
    b.ret.slots[0] = b.ret.slots[W] = 0;
    uint32_t before[2 * W];
    for (unsigned i = 0; i < 2 * W; i++)
        before[i] = b.ret.slots[i];
    CHECK(!fault_forge_pair(&cfg.journal, b.ret.slots, ERA0, FAULT_FORGE_SEQ_PLUS_2, nop, NULL));
    for (unsigned i = 0; i < 2 * W; i++)
        CHECK(b.ret.slots[i] == before[i]);
}

#ifdef CONFIG_EPOCH_FAULT_CORRUPT
/* The corrupted-slot image: mode 1 changes one bit of the newest committed record and nothing else, so its CRC fails with w0 intact; the next warm boot keeps that raw pair, reports retention invalid, starts a new era from counter 0, and no request of the session classifies definitively. Stage B runs it with the newest record in each slot. */
static void test_corrupt(void)
{
    for (uint32_t last = 1; last <= 2; last++) {
        struct board b;
        struct epoch_ctrl c;
        commits(&b, last);
        int n = epoch_journal_newest(&cfg.journal, b.ret.slots, ERA0);
        uint32_t before[2 * W];
        for (unsigned i = 0; i < 2 * W; i++)
            before[i] = b.ret.slots[i];
        CHECK(fault_decode(FAULT_CP_FORGE, FAULT_FORGE_SEQ_EQUAL) == FAULT_FORGE);
        CHECK(fault_forge_pair(&cfg.journal, b.ret.slots, ERA0, FAULT_FORGE_SEQ_EQUAL, nop, NULL));
        for (unsigned i = 0; i < 2 * W; i++)
            CHECK(b.ret.slots[i] == (i == (unsigned)n * W + 5u ? before[i] ^ 1u : before[i]));
        CHECK(b.ret.slots[(unsigned)n * W] == MAGIC && !epoch_slot_valid(&cfg.journal, b.ret.slots + (unsigned)n * W, ERA0));
        CHECK(epoch_slot_valid(&cfg.journal, b.ret.slots + (unsigned)(1 - n) * W, ERA0));
        CHECK(epoch_journal_newest(&cfg.journal, b.ret.slots, ERA0) == EPOCH_SLOT_INCONSISTENT);
        uint32_t raw[2 * W];
        for (unsigned i = 0; i < 2 * W; i++)
            raw[i] = b.ret.slots[i];
        CHECK(boot(&b, &c) == EPOCH_CTRL_OK && b.resets == 0u);
        CHECK(!c.retention_valid && c.snapshot.era == ERA0 && memcmp(c.snapshot.slots, raw, sizeof raw) == 0);
        for (uint32_t t = 0; t <= last + 1u; t++) {
            uint32_t counter = 0;
            CHECK(epoch_classify(&cfg.journal, c.retention_valid, &c.snapshot, SESSION, t, &counter) == EPOCH_OUTCOME_UNKNOWN);
        }
        CHECK(b.ret.era == 0x101u && c.counter == 0u && epoch_journal_newest(&cfg.journal, b.ret.slots, b.ret.era) == EPOCH_SLOT_NONE);
    }
}
#endif

/* The defect seen on the board in run 910002: queued with uart_poll_out only, the frame loses its last data byte and delimiter to the reset, and the next frame's leading delimiter closes it with a CRC error. Waiting for TC delivers it whole, within the two bytes the USART can still hold. */
static void test_report_survives_reset(void)
{
    struct epoch_msg r = { 0 }, ann = { 0 }, got;
    r.type = EPOCH_MSG_TEST_DIAG_REPLY;
    r.run = 910002;
    r.checkpoint = EPOCH_STORE_COLD_UNSET_ERA;
    r.reset_cause = 0x14004400u;
    ann.type = EPOCH_MSG_ANNOUNCE;
    uint8_t frame[EPOCH_WIRE_MAX_FRAME], next[EPOCH_WIRE_MAX_FRAME];
    size_t len = epoch_wire_encode(&r, frame, sizeof frame), next_n = epoch_wire_encode(&ann, next, sizeof next);
    unsigned bad;

    struct uart_model u;
    memset(&u, 0, sizeof u);
    for (size_t i = 0; i < len; i++)
        model_put(&u, frame[i]);
    CHECK(u.n == len - 2u);
    CHECK(parse_line(&u, next, next_n, &got, &bad) == 1u && got.type == EPOCH_MSG_ANNOUNCE && bad == 1u);

    memset(&u, 0, sizeof u);
    const struct fault_tx tx = { &u, model_put, model_sent, model_wait };
    CHECK(fault_send_complete(&tx, &r, 1000));
    CHECK(u.n == len && memcmp(u.line, frame, len) == 0 && u.now - u.last_put <= 2u * BYTE_US + FAULT_TX_POLL_US);
    CHECK(parse_line(&u, next, next_n, &got, &bad) == 2u && bad == 0u);
    CHECK(parse_line(&u, NULL, 0, &got, &bad) == 1u && got.type == r.type && got.run == r.run && got.checkpoint == r.checkpoint && got.reset_cause == r.reset_cause);

    /* a transmitter that never reports completion: bounded wait, then false */
    memset(&u, 0, sizeof u);
    u.never_done = true;
    CHECK(!fault_send_complete(&tx, &r, 1000));
    CHECK(u.now - u.last_put >= 1000u && u.now - u.last_put <= 1000u + FAULT_TX_POLL_US);
}

int main(void)
{
    test_decode();
    test_record();
    test_take_is_one_shot();
    test_cold_interruption();
    test_forge();
#ifdef CONFIG_EPOCH_FAULT_CORRUPT
    test_corrupt();
#endif
    test_report_survives_reset();
    return TEST_DONE("fault_hooks");
}
