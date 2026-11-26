#include <string.h>

#include "epoch/controller.h"
#include "epoch/host.h"
#include "sim.h"
#include "test.h"

#define W EPOCH_JOURNAL_WORDS
#define CAP 32u

struct cap {
    struct epoch_msg m[CAP];
    unsigned n;
    uint64_t rng;
    unsigned outcomes;
    enum epoch_outcome last_o;
    uint32_t last_counter;
};

static void cap_send(void *ctx, const struct epoch_msg *m)
{
    struct cap *c = ctx;
    if (c->n < CAP)
        c->m[c->n++] = *m;
}

static uint64_t cap_rand(void *ctx)
{
    struct cap *c = ctx;
    return ++c->rng * 0x9E3779B97F4A7C15u;
}

static uint32_t cap_era(void *ctx)
{
    (void)ctx;
    return 7u;
}

static void cap_barrier(void *ctx)
{
    (void)ctx;
}

static void cap_outcome(void *ctx, uint64_t session, uint32_t txid, enum epoch_outcome o, uint32_t counter)
{
    struct cap *c = ctx;
    (void)session;
    (void)txid;
    c->outcomes++;
    c->last_o = o;
    c->last_counter = counter;
}

static const struct epoch_msg *last(const struct cap *c)
{
    static const struct epoch_msg none;
    return c->n ? &c->m[c->n - 1u] : &none;
}

static struct epoch_msg mk(enum epoch_msg_type type)
{
    struct epoch_msg m;
    memset(&m, 0, sizeof m);
    m.type = type;
    if (type == EPOCH_MSG_HELLO)
        m.version = 1u; /* the only version epoch_test_ctrl_config supports */
    return m;
}

struct rig {
    struct cap cap;
    struct epoch_ctrl_env env;
    struct epoch_retained ret;
    struct epoch_ctrl c;
};

static void rig_boot(struct rig *r)
{
    memset(r, 0, sizeof *r);
    r->env = (struct epoch_ctrl_env){ &r->cap, cap_rand, cap_era, cap_send, cap_barrier, NULL, NULL };
    epoch_ctrl_boot(&r->c, &epoch_test_ctrl_config, &r->env, &r->ret, true, 1u, 0u);
}

static struct epoch_msg recv(struct rig *r, struct epoch_msg m, uint32_t now)
{
    unsigned before = r->cap.n;
    CHECK(epoch_ctrl_receive(&r->c, &m, now) == EPOCH_CTRL_OK);
    return r->cap.n > before ? *last(&r->cap) : mk((enum epoch_msg_type)0);
}

/* Drives PROBE, HELLO and START for nonce h and returns the session. */
static uint64_t open_session(struct rig *r, uint64_t h, uint32_t now)
{
    struct epoch_msg m = mk(EPOCH_MSG_PROBE);
    m.nonce = h;
    struct epoch_msg a = recv(r, m, now);
    CHECK(a.type == EPOCH_MSG_ANNOUNCE && a.nonce_echo && a.nonce == h);
    m = mk(EPOCH_MSG_HELLO);
    m.challenge = a.challenge;
    m.nonce = h;
    struct epoch_msg ack = recv(r, m, now);
    CHECK(ack.type == EPOCH_MSG_HELLO_ACK && ack.challenge == a.challenge && ack.nonce == h);
    m = mk(EPOCH_MSG_START);
    m.session = ack.session;
    struct epoch_msg sa = recv(r, m, now);
    CHECK(sa.type == EPOCH_MSG_START_ACK && sa.session == ack.session);
    return ack.session;
}

static struct epoch_msg inc(struct rig *r, uint64_t s, uint32_t txid, uint32_t now)
{
    struct epoch_msg m = mk(EPOCH_MSG_INCREMENT);
    m.session = s;
    m.txid = txid;
    return recv(r, m, now);
}

static void test_boot_announce(void)
{
    struct rig r;
    rig_boot(&r);
    CHECK(r.cap.n == 1u && r.cap.m[0].type == EPOCH_MSG_ANNOUNCE);
    CHECK(!r.cap.m[0].retention_valid && !r.cap.m[0].nonce_echo);
    CHECK(r.ret.era == 7u);
    /* warm reboot keeps retention and the counter */
    uint64_t s = open_session(&r, 1, 0);
    CHECK(inc(&r, s, 1, 0).counter == 1u);
    epoch_ctrl_boot(&r.c, &epoch_test_ctrl_config, &r.env, &r.ret, false, 2u, 5u);
    CHECK(last(&r.cap)->retention_valid && r.c.counter == 1u && !r.c.active);
}

static void test_duplicate_hello(void)
{
    struct rig r;
    rig_boot(&r);
    struct epoch_msg m = mk(EPOCH_MSG_HELLO);
    m.challenge = r.cap.m[0].challenge; /* boot ANNOUNCE, U has no owner */
    m.nonce = 42;
    struct epoch_msg a1 = recv(&r, m, 1);
    struct epoch_msg a2 = recv(&r, m, 2);
    CHECK(a1.type == EPOCH_MSG_HELLO_ACK && a2.type == EPOCH_MSG_HELLO_ACK && a1.session == a2.session);
    struct epoch_msg st = mk(EPOCH_MSG_START);
    st.session = a1.session;
    CHECK(recv(&r, st, 3).type == EPOCH_MSG_START_ACK);
    int n = epoch_journal_newest(&epoch_test_ctrl_config.journal, r.ret.slots, r.ret.era);
    uint32_t seq = r.ret.slots[(unsigned)n * W + 6];
    CHECK(recv(&r, st, 4).type == EPOCH_MSG_START_ACK); /* A = S row */
    CHECK(r.ret.slots[(unsigned)n * W + 6] == seq);     /* no second SESSION_OPEN */
    CHECK(recv(&r, m, 5).type == EPOCH_MSG_ANNOUNCE);   /* U consumed: C differs from U */
}

static void test_no_session_rate_limit(void)
{
    struct rig r;
    rig_boot(&r);
    CHECK(inc(&r, 99, 1, 10).type == EPOCH_MSG_NO_SESSION);
    CHECK(inc(&r, 99, 2, 59).type == 0);
    struct epoch_msg p = mk(EPOCH_MSG_PING);
    p.session = 99;
    CHECK(recv(&r, p, 40).type == 0);
    CHECK(recv(&r, p, 60).type == EPOCH_MSG_NO_SESSION);
    CHECK(last(&r.cap)->boot_id == r.c.boot_id);
    struct epoch_msg st = mk(EPOCH_MSG_START);
    st.session = 99;
    CHECK(recv(&r, st, 61).type == EPOCH_MSG_NO_SESSION); /* the START row is not rate-limited */
}

static void test_duplicate_probe(void)
{
    struct rig r;
    rig_boot(&r);
    struct epoch_msg pr = mk(EPOCH_MSG_PROBE);
    pr.nonce = 5;
    struct epoch_msg a1 = recv(&r, pr, 1);
    struct epoch_msg a2 = recv(&r, pr, 2);
    CHECK(a1.challenge == a2.challenge && a1.challenge == r.cap.m[0].challenge);
    pr.nonce = 6;
    struct epoch_msg a3 = recv(&r, pr, 3);
    CHECK(a3.challenge != a1.challenge && a3.nonce == 6u);
    struct epoch_msg h = mk(EPOCH_MSG_HELLO);
    h.challenge = a1.challenge;
    h.nonce = 5;
    CHECK(recv(&r, h, 4).type == EPOCH_MSG_ANNOUNCE); /* superseded challenge */
    h.challenge = a3.challenge;
    h.nonce = 5;
    CHECK(recv(&r, h, 5).type == EPOCH_MSG_ANNOUNCE); /* owned by nonce 6 */
    h.nonce = 6;
    CHECK(recv(&r, h, 6).type == EPOCH_MSG_HELLO_ACK);
}

static void test_pending_beside_live(void)
{
    struct rig r;
    rig_boot(&r);
    uint64_t s1 = open_session(&r, 1, 0);
    struct epoch_msg pr = mk(EPOCH_MSG_PROBE);
    pr.nonce = 2;
    struct epoch_msg a = recv(&r, pr, 10);
    struct epoch_msg h = mk(EPOCH_MSG_HELLO);
    h.challenge = a.challenge;
    h.nonce = 2;
    struct epoch_msg k1 = recv(&r, h, 11);
    struct epoch_msg k2 = recv(&r, h, 12);
    CHECK(k1.type == EPOCH_MSG_HELLO_ACK && k1.session == k2.session && k1.session != s1);
    struct epoch_msg p = mk(EPOCH_MSG_PING);
    p.session = s1;
    p.counter = 77;
    struct epoch_msg pong = recv(&r, p, 13);
    CHECK(pong.type == EPOCH_MSG_PONG && pong.session == s1 && pong.counter == 77u);
    struct epoch_msg rep = inc(&r, s1, 1, 14);
    CHECK(rep.type == EPOCH_MSG_EP1_REPLY && rep.ep1_status == EPOCH_EP1_COMMITTED && rep.counter == 1u);
    h.nonce = 3;
    CHECK(recv(&r, h, 15).type == EPOCH_MSG_BUSY);
    pr.nonce = 3;
    struct epoch_msg b = recv(&r, pr, 16);
    CHECK(b.type == EPOCH_MSG_BUSY && b.busy_remaining_ms == 11u + 200u - 16u);
    pr.nonce = 2;
    CHECK(recv(&r, pr, 17).challenge == a.challenge); /* P's own PROBE gets P's challenge */
    struct epoch_msg st = mk(EPOCH_MSG_START);
    st.session = k1.session;
    CHECK(recv(&r, st, 18).type == EPOCH_MSG_START_ACK);
    CHECK(recv(&r, p, 19).type == EPOCH_MSG_NO_SESSION);
    /* Host restart while RUNNING, journal part: SESSION_OPEN wrote the other slot, so the old session's newest record survives */
    const struct epoch_journal_params *jp = &epoch_test_ctrl_config.journal;
    int n = epoch_journal_newest(jp, r.ret.slots, r.ret.era);
    struct epoch_record newest, older;
    epoch_record_decode(r.ret.slots + (unsigned)n * W, &newest);
    epoch_record_decode(r.ret.slots + (unsigned)(1 - n) * W, &older);
    CHECK(newest.session == k1.session && newest.txid == 0u && newest.counter == 1u);
    CHECK(epoch_slot_valid(jp, r.ret.slots + (unsigned)(1 - n) * W, r.ret.era) && older.session == s1 && older.txid == 1u);
}

static void test_dead_host_during_boot_negotiation(void)
{
    struct rig r;
    rig_boot(&r);
    struct epoch_msg h = mk(EPOCH_MSG_HELLO);
    h.challenge = r.cap.m[0].challenge;
    h.nonce = 1;
    CHECK(recv(&r, h, 0).type == EPOCH_MSG_HELLO_ACK);
    struct epoch_msg pr = mk(EPOCH_MSG_PROBE);
    pr.nonce = 2;
    struct epoch_msg b = recv(&r, pr, 50);
    CHECK(b.type == EPOCH_MSG_BUSY && b.busy_remaining_ms == 150u);
    epoch_ctrl_tick(&r.c, 199);
    CHECK(r.c.pending);
    epoch_ctrl_tick(&r.c, 200);
    CHECK(!r.c.pending && r.c.u_present && !r.c.u_owned && !r.c.active);
    CHECK(open_session(&r, 2, 210) != 0u);
}

static void test_fence(void)
{
    struct rig r;
    rig_boot(&r);
    uint64_t s = open_session(&r, 1, 0);
    struct epoch_msg m = inc(&r, s, 0, 1);
    CHECK(m.type == EPOCH_MSG_EP1_REPLY && m.ep1_status == EPOCH_EP1_REJECTED_STALE);
    CHECK(inc(&r, s, 1, 2).ep1_status == EPOCH_EP1_COMMITTED);
    CHECK(inc(&r, s, 1, 3).ep1_status == EPOCH_EP1_REJECTED_STALE);
    m = inc(&r, s, 5, 4);
    CHECK(m.ep1_status == EPOCH_EP1_COMMITTED && m.counter == 2u && m.txid == 5u);
    CHECK(inc(&r, s, 4, 5).ep1_status == EPOCH_EP1_REJECTED_STALE);
    CHECK(r.c.counter == 2u);
}

struct hrig {
    struct cap cap;
    struct epoch_host_config cfg;
    struct epoch_host_env env;
    struct epoch_host h;
};

static void hrig_running(struct hrig *r, enum epoch_host_policy policy, uint64_t boot, uint64_t session)
{
    memset(r, 0, sizeof *r);
    r->cfg = epoch_test_host_config(policy);
    r->env = (struct epoch_host_env){ &r->cap, cap_rand, cap_send, cap_outcome };
    epoch_host_init(&r->h, &r->cfg, &r->env);
    epoch_host_start(&r->h, 0);
    CHECK(last(&r->cap)->type == EPOCH_MSG_PROBE);
    struct epoch_msg a = mk(EPOCH_MSG_ANNOUNCE);
    a.boot_id = boot;
    a.challenge = 500;
    a.retention_valid = true;
    epoch_host_receive(&r->h, &a, 1);
    CHECK(last(&r->cap)->type == EPOCH_MSG_HELLO && last(&r->cap)->challenge == 500u);
    struct epoch_msg k = mk(EPOCH_MSG_HELLO_ACK);
    k.challenge = 500;
    k.nonce = r->h.nonce;
    k.session = session;
    epoch_host_receive(&r->h, &k, 2);
    CHECK(last(&r->cap)->type == EPOCH_MSG_START && last(&r->cap)->session == session);
    struct epoch_msg sa = mk(EPOCH_MSG_START_ACK);
    sa.session = session;
    epoch_host_receive(&r->h, &sa, 3);
    CHECK(r->h.state == EPOCH_HOST_RUNNING);
}

static void test_stale_announce(void)
{
    struct hrig r;
    hrig_running(&r, EPOCH_POLICY_B3, 11, 9);
    unsigned n = r.cap.n;
    struct epoch_msg a = mk(EPOCH_MSG_ANNOUNCE);
    a.boot_id = 11;
    epoch_host_receive(&r.h, &a, 10);
    CHECK(r.h.state == EPOCH_HOST_RUNNING && r.cap.n == n);
    a.boot_id = 12;
    epoch_host_receive(&r.h, &a, 11);
    CHECK(r.h.state == EPOCH_HOST_VERIFYING && last(&r.cap)->type == EPOCH_MSG_PING && last(&r.cap)->session == 9u);
    struct epoch_msg p = mk(EPOCH_MSG_PONG);
    p.session = 9;
    epoch_host_receive(&r.h, &p, 12);
    CHECK(r.h.state == EPOCH_HOST_RUNNING);
}

static void test_no_session_host_side(void)
{
    struct hrig r;
    hrig_running(&r, EPOCH_POLICY_B3, 11, 9);
    struct epoch_msg ns = mk(EPOCH_MSG_NO_SESSION);
    epoch_host_receive(&r.h, &ns, 10);
    CHECK(r.h.state == EPOCH_HOST_VERIFYING);
    uint64_t old = r.h.nonce;
    epoch_host_receive(&r.h, &ns, 11);
    CHECK(r.h.state == EPOCH_HOST_PROBING && last(&r.cap)->type == EPOCH_MSG_PROBE && last(&r.cap)->nonce != old);
}

static void test_pong_after_timeout(void)
{
    struct hrig r;
    hrig_running(&r, EPOCH_POLICY_B3, 11, 9);
    CHECK(epoch_host_increment(&r.h, 100) == 1u);
    CHECK(epoch_host_increment(&r.h, 101) == 0u); /* one outstanding request */
    epoch_host_tick(&r.h, 199);
    CHECK(r.cap.outcomes == 0u);
    epoch_host_tick(&r.h, 200);
    CHECK(r.cap.outcomes == 1u && r.cap.last_o == EPOCH_OUTCOME_UNKNOWN && r.h.state == EPOCH_HOST_VERIFYING);
    struct epoch_msg p = mk(EPOCH_MSG_PONG);
    p.session = 9;
    epoch_host_receive(&r.h, &p, 210);
    CHECK(r.h.state == EPOCH_HOST_RUNNING && r.cap.outcomes == 1u); /* PONG proves liveness only */
    struct epoch_msg rej = mk(EPOCH_MSG_EP1_REPLY);
    rej.session = 9;
    rej.txid = 1;
    rej.ep1_status = EPOCH_EP1_REJECTED_STALE;
    epoch_host_receive(&r.h, &rej, 211);
    CHECK(r.cap.outcomes == 1u); /* REJECTED_STALE cannot refine an UNKNOWN */
    struct epoch_msg late = rej;
    late.ep1_status = EPOCH_EP1_COMMITTED;
    late.counter = 4;
    late.session = 8;
    epoch_host_receive(&r.h, &late, 212);
    CHECK(r.cap.outcomes == 1u); /* wrong session does not match */
    late.session = 9;
    epoch_host_receive(&r.h, &late, 213);
    CHECK(r.cap.outcomes == 2u && r.cap.last_o == EPOCH_OUTCOME_COMPLETED && r.cap.last_counter == 4u);
    epoch_host_receive(&r.h, &late, 214);
    CHECK(r.cap.outcomes == 2u); /* refined at most once */
}

static void test_b3_resolution_at_start_ack(void)
{
    for (int policy = EPOCH_POLICY_B2; policy <= EPOCH_POLICY_B3; policy++) {
        struct hrig r;
        hrig_running(&r, (enum epoch_host_policy)policy, 11, 9);
        CHECK(epoch_host_increment(&r.h, 100) == 1u);
        CHECK(epoch_host_increment(&r.h, 100) == 0u);
        struct epoch_msg rep = mk(EPOCH_MSG_EP1_REPLY);
        rep.session = 9;
        rep.txid = 1;
        rep.counter = 1;
        epoch_host_receive(&r.h, &rep, 110);
        CHECK(r.cap.last_o == EPOCH_OUTCOME_COMPLETED);
        CHECK(epoch_host_increment(&r.h, 120) == 2u);
        struct epoch_msg a = mk(EPOCH_MSG_ANNOUNCE); /* controller rebooted after committing nothing for txid 2 */
        a.boot_id = 12;
        epoch_host_receive(&r.h, &a, 130);
        CHECK(r.cap.last_o == EPOCH_OUTCOME_UNKNOWN && r.h.state == EPOCH_HOST_VERIFYING);
        struct epoch_msg ns = mk(EPOCH_MSG_NO_SESSION);
        epoch_host_receive(&r.h, &ns, 131);
        CHECK(r.h.state == EPOCH_HOST_PROBING);
        a.challenge = 600;
        a.retention_valid = true;
        a.snapshot.era = 3;
        uint32_t w[W] = { EPOCH_TEST_MAGIC_VERSION, 3, 9, 0, 1, 1, 4, 0 };
        w[W - 1] = epoch_test_checksum(w, W - 1);
        memcpy(a.snapshot.slots, w, sizeof w);
        epoch_host_receive(&r.h, &a, 132);
        struct epoch_msg k = mk(EPOCH_MSG_HELLO_ACK);
        k.challenge = 600;
        k.nonce = r.h.nonce;
        k.session = 10;
        epoch_host_receive(&r.h, &k, 133);
        unsigned before = r.cap.outcomes;
        struct epoch_msg sa = mk(EPOCH_MSG_START_ACK);
        sa.session = 10;
        epoch_host_receive(&r.h, &sa, 134);
        CHECK(r.h.state == EPOCH_HOST_RUNNING);
        if (policy == EPOCH_POLICY_B3)
            CHECK(r.cap.outcomes == before + 1u && r.cap.last_o == EPOCH_OUTCOME_NOT_EXECUTED);
        else
            CHECK(r.cap.outcomes == before);
        CHECK(epoch_host_increment(&r.h, 140) == 1u);
    }
}

static void test_host_retransmit_then_probe(void)
{
    struct hrig r;
    memset(&r, 0, sizeof r);
    r.cfg = epoch_test_host_config(EPOCH_POLICY_B3);
    r.env = (struct epoch_host_env){ &r.cap, cap_rand, cap_send, cap_outcome };
    epoch_host_init(&r.h, &r.cfg, &r.env);
    struct epoch_msg a = mk(EPOCH_MSG_ANNOUNCE);
    a.challenge = 1;
    epoch_host_receive(&r.h, &a, 0); /* DOWN row */
    CHECK(r.h.state == EPOCH_HOST_BINDING && r.cap.n == 1u);
    for (uint32_t t = 100; t <= 300; t += 100)
        epoch_host_tick(&r.h, t);
    CHECK(r.cap.n == 4u && last(&r.cap)->type == EPOCH_MSG_HELLO);
    uint64_t h = r.h.nonce;
    epoch_host_tick(&r.h, 400);
    CHECK(r.h.state == EPOCH_HOST_PROBING && last(&r.cap)->type == EPOCH_MSG_PROBE && last(&r.cap)->nonce == h);
    epoch_host_tick(&r.h, 500);
    CHECK(r.cap.n == 6u && last(&r.cap)->type == EPOCH_MSG_PROBE); /* PROBING retry */
    struct epoch_msg b = mk(EPOCH_MSG_BUSY);
    b.busy_remaining_ms = 50;
    epoch_host_receive(&r.h, &b, 510);
    CHECK(r.cap.n == 6u);
    epoch_host_tick(&r.h, 559);
    CHECK(r.cap.n == 6u);
    epoch_host_tick(&r.h, 580);
    CHECK(r.cap.n == 7u && last(&r.cap)->type == EPOCH_MSG_PROBE);
}

static void test_txid_exhaustion(void)
{
    struct hrig r;
    hrig_running(&r, EPOCH_POLICY_B3, 11, 9);
    r.h.next_txid = UINT32_MAX;
    CHECK(epoch_host_increment(&r.h, 10) == UINT32_MAX);
    struct epoch_msg rep = mk(EPOCH_MSG_EP1_REPLY);
    rep.session = 9;
    rep.txid = UINT32_MAX;
    epoch_host_receive(&r.h, &rep, 11);
    CHECK(r.cap.last_o == EPOCH_OUTCOME_COMPLETED);
    uint64_t h = r.h.nonce;
    CHECK(epoch_host_increment(&r.h, 12) == 0u);
    CHECK(r.h.state == EPOCH_HOST_PROBING && last(&r.cap)->type == EPOCH_MSG_PROBE && last(&r.cap)->nonce != h);
}

/* Regression: a network duplicate of an executed INCREMENT is answered REJECTED_STALE. That reply must not settle the in-flight request as REJECTED, because the request did execute; seed 2 of epoch_sim reproduced it through reordering. */
static void test_duplicate_reply_does_not_settle_rejected(void)
{
    for (int lose_commit_reply = 0; lose_commit_reply <= 1; lose_commit_reply++) {
        struct rig r;
        rig_boot(&r);
        struct hrig hr;
        hrig_running(&hr, EPOCH_POLICY_B3, r.c.boot_id, open_session(&r, 1, 0));
        CHECK(epoch_host_increment(&hr.h, 10) == 1u);
        struct epoch_msg req = *last(&hr.cap);
        struct epoch_msg first = recv(&r, req, 11);  /* executes */
        struct epoch_msg second = recv(&r, req, 12); /* network duplicate */
        CHECK(first.ep1_status == EPOCH_EP1_COMMITTED && second.ep1_status == EPOCH_EP1_REJECTED_STALE);
        CHECK(r.c.counter == 1u);
        epoch_host_receive(&hr.h, &second, 13); /* overtakes the COMMITTED reply */
        CHECK(hr.cap.outcomes == 0u);
        if (!lose_commit_reply) {
            epoch_host_receive(&hr.h, &first, 14);
            CHECK(hr.cap.outcomes == 1u && hr.cap.last_o == EPOCH_OUTCOME_COMPLETED && hr.cap.last_counter == 1u);
        } else {
            epoch_host_tick(&hr.h, 10u + hr.cfg.request_timeout_ms);
            CHECK(hr.cap.outcomes == 1u && hr.cap.last_o == EPOCH_OUTCOME_UNKNOWN && hr.h.state == EPOCH_HOST_VERIFYING);
        }
    }
}

static struct epoch_msg hello(uint64_t challenge, uint64_t h, uint32_t version)
{
    struct epoch_msg m = mk(EPOCH_MSG_HELLO);
    m.challenge = challenge;
    m.nonce = h;
    m.version = version;
    return m;
}

/* A controller that supports versions 2 and 3 acknowledges both ends of the range and handles 1 and 4 like a HELLO for the wrong challenge. */
static void test_hello_version_range(void)
{
    static const struct { uint32_t version; bool supported; } cases[] = { { 2u, true }, { 3u, true }, { 1u, false }, { 4u, false }, { 0u, false } };
    struct epoch_ctrl_config cfg = epoch_test_ctrl_config;
    cfg.version_min = 2u;
    cfg.version_max = 3u;
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        struct rig r;
        rig_boot(&r);
        r.c.cfg = &cfg;
        struct epoch_msg p = mk(EPOCH_MSG_PROBE);
        p.nonce = 1;
        struct epoch_msg a = recv(&r, p, 0);
        struct epoch_msg got = recv(&r, hello(a.challenge, 1, cases[i].version), 0);
        CHECK(got.type == (cases[i].supported ? EPOCH_MSG_HELLO_ACK : EPOCH_MSG_ANNOUNCE));
        CHECK(r.c.pending == cases[i].supported && !r.c.active);
        if (!cases[i].supported) {
            CHECK(got.version_min == 2u && got.version_max == 3u);
            /* the ANNOUNCE carries a usable challenge, so a supported HELLO binds next */
            got = recv(&r, hello(got.challenge, 1, 2u), 0);
            CHECK(got.type == EPOCH_MSG_HELLO_ACK && r.c.pending);
        }
    }
}

/* An unsupported duplicate of a pending HELLO gets BUSY and leaves the pending session, which START still completes. */
static void test_unsupported_duplicate_hello_keeps_pending(void)
{
    struct rig r;
    rig_boot(&r);
    struct epoch_msg p = mk(EPOCH_MSG_PROBE);
    p.nonce = 1;
    struct epoch_msg a = recv(&r, p, 0);
    struct epoch_msg ack = recv(&r, hello(a.challenge, 1, 1u), 0);
    CHECK(ack.type == EPOCH_MSG_HELLO_ACK);
    CHECK(recv(&r, hello(a.challenge, 1, 2u), 1).type == EPOCH_MSG_BUSY);
    CHECK(recv(&r, hello(a.challenge, 1, 1u), 2).type == EPOCH_MSG_HELLO_ACK);
    CHECK(r.c.pending && r.c.p_session == ack.session);
    struct epoch_msg st = mk(EPOCH_MSG_START);
    st.session = ack.session;
    CHECK(recv(&r, st, 3).type == EPOCH_MSG_START_ACK && r.c.active && r.c.session == ack.session);
}

/* An unsupported HELLO beside a live session neither replaces it nor opens a pending one; a valid handshake afterwards rebinds. */
static void test_unsupported_hello_beside_active_session(void)
{
    struct rig r;
    rig_boot(&r);
    uint64_t s = open_session(&r, 1, 0);
    struct epoch_msg p = mk(EPOCH_MSG_PROBE);
    p.nonce = 2;
    struct epoch_msg a = recv(&r, p, 1);
    CHECK(recv(&r, hello(a.challenge, 2, 9u), 1).type == EPOCH_MSG_ANNOUNCE);
    CHECK(r.c.active && r.c.session == s && !r.c.pending);
    struct epoch_msg ping = mk(EPOCH_MSG_PING);
    ping.session = s;
    CHECK(recv(&r, ping, 2).type == EPOCH_MSG_PONG);
    uint64_t s2 = open_session(&r, 3, 3);
    CHECK(s2 != s && r.c.active && r.c.session == s2);
}

/* The EP0 transition matrix: six controller states against every host input class, each from a fresh boot. Besides the expected reply, every cell checks what must hold whatever the table says: at most one reply, the journal written only by START of the pending session or a fresh INCREMENT, the active session replaced only by START of the pending one, and session-carrying replies naming the live or pending session. Device-only and unknown message types must change nothing in any state. */
enum ep0_state { ST_IDLE, ST_OFFERED, ST_PENDING, ST_ACTIVE, ST_ACTIVE_PENDING, ST_EXPIRED, ST_COUNT };
enum ep0_input { IN_PROBE, IN_PROBE_OTHER, IN_HELLO, IN_HELLO_WRONG_CHALLENGE, IN_HELLO_WRONG_VERSION, IN_HELLO_OTHER_NONCE, IN_START_PENDING, IN_START_ACTIVE, IN_START_UNKNOWN,
                 IN_PING_ACTIVE, IN_PING_UNKNOWN, IN_INC_ACTIVE, IN_INC_UNKNOWN, IN_COUNT };

struct known {
    uint64_t nonce, challenge, p_session, session;
    uint32_t now;
};

static uint64_t last_challenge(const struct rig *r)
{
    for (unsigned i = r->cap.n; i-- > 0;)
        if (r->cap.m[i].type == EPOCH_MSG_ANNOUNCE)
            return r->cap.m[i].challenge;
    return 0;
}

static struct known ep0_setup(struct rig *r, enum ep0_state st)
{
    struct known k = { 0x11u, 0, 0x5EED0001u, 0x5EED0002u, 30u }; /* session values never issued unless the state sets them */
    rig_boot(r);
    k.challenge = last_challenge(r);
    if (st == ST_ACTIVE || st == ST_ACTIVE_PENDING) {
        k.session = open_session(r, 0x22u, 10u);
        k.challenge = last_challenge(r);
        k.nonce = 0x22u;
        if (st == ST_ACTIVE)
            return k;
        k.nonce = 0x11u;
    }
    if (st == ST_IDLE)
        return k;
    struct epoch_msg m = mk(EPOCH_MSG_PROBE);
    m.nonce = k.nonce;
    k.challenge = recv(r, m, 20u).challenge;
    if (st == ST_OFFERED)
        return k;
    k.p_session = recv(r, hello(k.challenge, k.nonce, 1u), 20u).session;
    if (st == ST_EXPIRED)
        k.now = 20u + epoch_test_ctrl_config.t_pending_ms + 1u;
    return k;
}

static struct epoch_msg ep0_input(const struct known *k, enum ep0_input in)
{
    static const enum epoch_msg_type type[IN_COUNT] = { EPOCH_MSG_PROBE, EPOCH_MSG_PROBE, EPOCH_MSG_HELLO, EPOCH_MSG_HELLO, EPOCH_MSG_HELLO, EPOCH_MSG_HELLO, EPOCH_MSG_START,
                                                        EPOCH_MSG_START, EPOCH_MSG_START, EPOCH_MSG_PING, EPOCH_MSG_PING, EPOCH_MSG_INCREMENT, EPOCH_MSG_INCREMENT };
    struct epoch_msg m = mk(type[in]);
    m.nonce = in == IN_PROBE_OTHER || in == IN_HELLO_OTHER_NONCE ? 0x99u : k->nonce;
    m.challenge = in == IN_HELLO_WRONG_CHALLENGE ? k->challenge ^ 1u : k->challenge;
    m.version = in == IN_HELLO_WRONG_VERSION ? 2u : 1u;
    m.session = in == IN_START_PENDING ? k->p_session : in == IN_START_ACTIVE || in == IN_PING_ACTIVE || in == IN_INC_ACTIVE ? k->session : 0xBADu;
    m.txid = 1u;
    m.counter = 5u;
    return m;
}

static void test_ep0_matrix(void)
{
    /* macros rather than an anonymous enum, so comparisons with out->type stay within enum epoch_msg_type (GCC -Wenum-compare) */
#define A EPOCH_MSG_ANNOUNCE
#define H EPOCH_MSG_HELLO_ACK
#define B EPOCH_MSG_BUSY
#define S EPOCH_MSG_START_ACK
#define N EPOCH_MSG_NO_SESSION
#define P EPOCH_MSG_PONG
#define E EPOCH_MSG_EP1_REPLY
    /*                                      PROBE oth  HELLO chal ver  nonce START pend act unk  PING act unk  INC act unk */
    static const unsigned char want[ST_COUNT][IN_COUNT] = {
        [ST_IDLE] = { A, A, H, A, A, H, N, N, N, N, N, N, N },
        [ST_OFFERED] = { A, A, H, A, A, A, N, N, N, N, N, N, N },
        [ST_PENDING] = { A, B, H, B, B, B, S, N, N, N, N, N, N },
        [ST_ACTIVE] = { A, A, A, A, A, A, N, S, N, P, N, E, N },
        [ST_ACTIVE_PENDING] = { A, B, H, B, B, B, S, S, N, P, N, E, N },
        [ST_EXPIRED] = { A, A, A, A, A, A, N, N, N, N, N, N, N },
    };
    static const unsigned device_only[] = { 0u, EPOCH_MSG_ANNOUNCE, EPOCH_MSG_HELLO_ACK, EPOCH_MSG_START_ACK, EPOCH_MSG_PONG, EPOCH_MSG_NO_SESSION, EPOCH_MSG_BUSY, EPOCH_MSG_EP1_REPLY,
                                            EPOCH_MSG_TEST_ARM, EPOCH_MSG_TEST_ARM_ACK, EPOCH_MSG_TEST_DIAG, EPOCH_MSG_TEST_DIAG_REPLY, EPOCH_MSG_TYPE_COUNT, 200u };
    for (unsigned st = 0; st < ST_COUNT; st++) {
        for (unsigned in = 0; in < IN_COUNT; in++) {
            struct rig r;
            struct known k = ep0_setup(&r, (enum ep0_state)st);
            epoch_ctrl_tick(&r.c, k.now);
            struct epoch_ctrl before = r.c;
            uint32_t slots[2 * W];
            memcpy(slots, (const void *)r.ret.slots, sizeof slots);
            unsigned n0 = r.cap.n;
            struct epoch_msg m = ep0_input(&k, (enum ep0_input)in);
            CHECK(epoch_ctrl_receive(&r.c, &m, k.now) == EPOCH_CTRL_OK);
            unsigned replies = r.cap.n - n0;
            const struct epoch_msg *out = last(&r.cap);
            bool ok = replies == 1u && out->type == want[st][in];
            bool wrote = memcmp(slots, (const void *)r.ret.slots, sizeof slots) != 0;
            ok = ok && wrote == ((in == IN_START_PENDING && out->type == S) || (in == IN_INC_ACTIVE && out->type == E));
            ok = ok && (!r.c.active || r.c.session == (in == IN_START_PENDING && out->type == S ? k.p_session : before.session));
            if (out->type == H)
                ok = ok && r.c.pending && out->session == r.c.p_session && (!before.pending || before.p_session == r.c.p_session);
            if (out->type == S || out->type == P || out->type == E)
                ok = ok && out->session == r.c.session;
            if (!ok)
                fprintf(stderr, "ep0 matrix: state %u input %u: %u replies, type %u, expected %u, journal %s\n", st, in, replies, replies ? (unsigned)out->type : 0u, (unsigned)want[st][in], wrote ? "written" : "kept");
            CHECK(ok);
        }
        for (unsigned i = 0; i < sizeof device_only / sizeof device_only[0]; i++) {
            struct rig r;
            struct known k = ep0_setup(&r, (enum ep0_state)st);
            epoch_ctrl_tick(&r.c, k.now);
            struct rig before;
            memcpy(&before, &r, sizeof r);
            struct epoch_msg m = ep0_input(&k, IN_INC_ACTIVE);
            m.type = (enum epoch_msg_type)device_only[i];
            CHECK(epoch_ctrl_receive(&r.c, &m, k.now) == EPOCH_CTRL_OK);
            CHECK(memcmp(&before, &r, sizeof r) == 0);
        }
    }
}
#undef A
#undef H
#undef B
#undef S
#undef N
#undef P
#undef E

int main(void)
{
    test_boot_announce();
    test_duplicate_hello();
    test_stale_announce();
    test_no_session_rate_limit();
    test_no_session_host_side();
    test_duplicate_probe();
    test_pending_beside_live();
    test_pong_after_timeout();
    test_dead_host_during_boot_negotiation();
    test_fence();
    test_b3_resolution_at_start_ack();
    test_host_retransmit_then_probe();
    test_txid_exhaustion();
    test_duplicate_reply_does_not_settle_rejected();
    test_hello_version_range();
    test_unsupported_duplicate_hello_keeps_pending();
    test_unsupported_hello_beside_active_session();
    test_ep0_matrix();
    return TEST_DONE("state_machine");
}
