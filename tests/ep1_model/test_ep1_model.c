#include <string.h>

#include "epoch/controller.h"
#include "epoch/journal.h"
#include "sim.h"
#include "test.h"

/* EP1 exactly once, checked against an effect model kept apart from the controller: a logical increment takes effect when the controller confirms it (COMMITTED) or, after a reset at a checkpoint, when the boot snapshot classifies it COMPLETED; the model counts those confirmations and nothing else. Duplicates, older txids, stale sessions, warm reboots and resets at every checkpoint must leave the controller counter equal to that count at every step, and the outcome of a cut request must follow from where it was cut: before the commit store NOT_EXECUTED, after it COMPLETED with the counter of its effect. */

#define W EPOCH_JOURNAL_WORDS
#define SEEDS 300u
#define STEPS 60u

struct rig {
    struct epoch_ctrl_env env;
    struct epoch_retained ret;
    struct epoch_ctrl c;
    uint64_t rng;
    struct epoch_msg out;
    unsigned sent;
    bool armed;
    enum epoch_checkpoint arm_cp;
    uint32_t arm_txid;
};

static uint64_t rand64(void *ctx)
{
    struct rig *r = ctx;
    r->rng ^= r->rng << 13;
    r->rng ^= r->rng >> 7;
    r->rng ^= r->rng << 17;
    return r->rng;
}

static uint32_t new_era(void *ctx)
{
    return (uint32_t)rand64(ctx) | 1u;
}

static void send(void *ctx, const struct epoch_msg *m)
{
    struct rig *r = ctx;
    r->out = *m;
    r->sent++;
}

static void nop(void *ctx)
{
    (void)ctx;
}

static bool cut(void *ctx, enum epoch_checkpoint cp, uint32_t txid)
{
    struct rig *r = ctx;
    if (!r->armed || cp != r->arm_cp || txid != r->arm_txid)
        return false;
    r->armed = false;
    return true;
}

static struct epoch_msg mk(enum epoch_msg_type type)
{
    struct epoch_msg m;
    memset(&m, 0, sizeof m);
    m.type = type;
    m.version = 1u;
    return m;
}

/* Sends m and returns the reply, or a message of type 0 when there was none; *reset reports a modeled reset. */
static struct epoch_msg exchange(struct rig *r, struct epoch_msg m, uint32_t now, bool *reset)
{
    unsigned before = r->sent;
    *reset = epoch_ctrl_receive(&r->c, &m, now) == EPOCH_CTRL_RESET;
    CHECK(!*reset || r->sent == before); /* every checkpoint lies before the reply, so a cut request is never answered */
    return r->sent > before && !*reset ? r->out : mk((enum epoch_msg_type)0);
}

static uint64_t bind(struct rig *r, uint32_t now)
{
    bool reset;
    struct epoch_msg m = mk(EPOCH_MSG_PROBE);
    m.nonce = rand64(r);
    struct epoch_msg a = exchange(r, m, now, &reset);
    m = mk(EPOCH_MSG_HELLO);
    m.challenge = a.challenge;
    m.nonce = a.nonce;
    struct epoch_msg ack = exchange(r, m, now, &reset);
    m = mk(EPOCH_MSG_START);
    m.session = ack.session;
    CHECK(exchange(r, m, now, &reset).type == EPOCH_MSG_START_ACK);
    return ack.session;
}

static void reboot(struct rig *r, uint32_t now)
{
    CHECK(epoch_ctrl_boot(&r->c, &epoch_test_ctrl_config, &r->env, &r->ret, false, 2u, now) == EPOCH_CTRL_OK);
    CHECK(r->c.retention_valid);
}

static enum epoch_outcome outcome(const struct rig *r, uint64_t s, uint32_t txid, uint32_t *counter)
{
    return epoch_classify(&epoch_test_ctrl_config.journal, r->c.retention_valid, &r->c.snapshot, s, txid, counter);
}

int main(void)
{
    unsigned committed = 0, cuts_before = 0, cuts_after = 0, rejected = 0, stale_sessions = 0, reboots = 0;
    for (uint64_t seed = 1; seed <= SEEDS; seed++) {
        struct rig r;
        memset(&r, 0, sizeof r);
        r.rng = seed * 0x9E3779B97F4A7C15u;
        r.env = (struct epoch_ctrl_env){ &r, rand64, new_era, send, nop, cut, NULL };
        CHECK(epoch_ctrl_boot(&r.c, &epoch_test_ctrl_config, &r.env, &r.ret, true, 1u, 0u) == EPOCH_CTRL_OK);
        uint32_t now = 0, done = 0, fence = 0; /* the model: confirmed effects, and the highest txid of the session that took effect */
        uint64_t s = bind(&r, now), old = 0xBADu;
        for (unsigned step = 0; step < STEPS; step++) {
            now += 100u; /* past the NO_SESSION interval, so every refusal is answered */
            uint64_t pick = rand64(&r) % 10u;
            bool reset;
            struct epoch_msg m = mk(EPOCH_MSG_INCREMENT);
            m.session = s;
            if (pick < 6u) { /* a new logical increment, cut at a checkpoint one time in three */
                m.txid = fence + 1u + (uint32_t)(rand64(&r) % 2u);
                r.armed = rand64(&r) % 3u == 0u;
                r.arm_cp = (enum epoch_checkpoint)(rand64(&r) % 6u);
                r.arm_txid = m.txid;
                bool armed = r.armed;
                struct epoch_msg reply = exchange(&r, m, now, &reset);
                CHECK(reset == armed);
                if (!reset) {
                    CHECK(reply.type == EPOCH_MSG_EP1_REPLY && reply.ep1_status == EPOCH_EP1_COMMITTED && reply.session == s && reply.txid == m.txid && reply.counter == done + 1u);
                    done++;
                    fence = m.txid;
                    committed++;
                } else {
                    bool after_commit = r.arm_cp >= EPOCH_CP_F3;
                    reboot(&r, now);
                    reboots++;
                    uint32_t counter = 0;
                    enum epoch_outcome o = outcome(&r, s, m.txid, &counter);
                    CHECK(o == (after_commit ? EPOCH_OUTCOME_COMPLETED : EPOCH_OUTCOME_NOT_EXECUTED));
                    if (after_commit) {
                        CHECK(counter == done + 1u);
                        done++;
                        cuts_after++;
                    } else {
                        cuts_before++;
                    }
                    CHECK(r.c.counter == done);
                    old = s;
                    s = bind(&r, now);
                    fence = 0;
                }
            } else if (pick == 6u && fence > 0u) { /* the last txid again, or an older one: refused with the current counter */
                m.txid = fence - (uint32_t)(rand64(&r) % 2u);
                struct epoch_msg reply = exchange(&r, m, now, &reset);
                CHECK(!reset && reply.type == EPOCH_MSG_EP1_REPLY && reply.ep1_status == EPOCH_EP1_REJECTED_STALE && reply.txid == m.txid && reply.counter == done);
                rejected++;
            } else if (pick == 7u) { /* a request of a session that is no longer active */
                m.session = old;
                m.txid = fence + 1u;
                uint32_t before[2 * W];
                memcpy(before, (const void *)r.ret.slots, sizeof before);
                CHECK(exchange(&r, m, now, &reset).type == EPOCH_MSG_NO_SESSION && !reset);
                CHECK(memcmp(before, (const void *)r.ret.slots, sizeof before) == 0);
                stale_sessions++;
            } else if (pick == 8u) { /* a warm reboot without a cut: the last effect, or the session opening, is what the snapshot shows */
                reboot(&r, now);
                reboots++;
                uint32_t counter = 0;
                CHECK(outcome(&r, s, fence + 1u, &counter) == EPOCH_OUTCOME_NOT_EXECUTED);
                if (fence > 0u)
                    CHECK(outcome(&r, s, fence, &counter) == EPOCH_OUTCOME_COMPLETED && counter == done);
                old = s;
                s = bind(&r, now);
                fence = 0;
            } else if (pick == 9u) { /* a rebind on the running controller: the old session stops at once */
                old = s;
                s = bind(&r, now);
                fence = 0;
            }
            CHECK(r.c.counter == done);
        }
    }
    printf("ep1_model: %u committed, %u cut before and %u after the commit, %u refused retransmits, %u stale-session requests, %u reboots\n", committed, cuts_before, cuts_after, rejected,
           stale_sessions, reboots);
    CHECK(cuts_before > 0u && cuts_after > 0u && rejected > 0u && stale_sessions > 0u);
    return TEST_DONE("ep1_model");
}
