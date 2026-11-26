/* EPOCH host client for the board: the portable host state machine (core/src/host.c) and the same wire codec as the firmware (core/src/wire.c) over a serial port.
 * Usage: epoch_client <port> <pings> <increments>. Binds a session, sends <pings> PINGs, then <increments> INCREMENTs one at a time.
 * Every frame sent or received is printed as one line "<ms> tx|rx <TYPE> key=value...", followed by one RESULT line. Exit 0 when every step succeeded, 1 otherwise, 2 on a usage or open error.
 * Usage: epoch_client <port> campaign <b0|b1|b2|b3> <f1|f2a|f2b|f2c|f3> <trials> <run_base> <trials.jsonl>. Runs a reset campaign against the fault-test firmware (see campaign below); one JSON line per trial is appended and fsynced. Exit 0 when every trial was COUNTED_PASS. */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "epoch/crc32.h"
#include "epoch/host.h"
#include "epoch/wire.h"
#include "trial_verdict.h"

#define JOURNAL_MAGIC_VERSION 0x45504301u /* must match firmware/stm32u585/src/controller_app.c */
#define STEP_LIMIT_MS 5000u               /* per handshake, PING or INCREMENT */

struct client {
    int fd;
    struct timespec t0;
    uint32_t pong_counter;
    unsigned pongs;
    enum epoch_outcome last_outcome;
    uint32_t last_counter;
    int io_error;
    struct epoch_msg reply;  /* latest EP1_REPLY */
    struct epoch_msg test;   /* latest TEST_ARM_ACK or TEST_DIAG_REPLY */
    unsigned replies, tests; /* arrival counts for the two above */
    unsigned foreign;        /* EP1_REPLY for a session other than the current one: a session isolation (P1) violation */
    unsigned rx_errors;      /* frames rejected by the wire parser */
    uint64_t session;        /* current session as the runner sees it */
};

static uint32_t now_ms(const struct client *c)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((t.tv_sec - c->t0.tv_sec) * 1000 + (t.tv_nsec - c->t0.tv_nsec) / 1000000);
}

static void log_msg(struct client *c, const char *dir, const struct epoch_msg *m)
{
    printf("%" PRIu32 " %s %s", now_ms(c), dir, epoch_msg_name(m->type));
    switch (m->type) {
    case EPOCH_MSG_ANNOUNCE:
        printf(" boot_id=%016" PRIx64 " challenge=%016" PRIx64 " echo=%d nonce=%016" PRIx64 " reset_cause=%08" PRIx32 " retention_valid=%d version=%" PRIu32 "..%" PRIu32 " era=%08" PRIx32 " slots=",
               m->boot_id, m->challenge, m->nonce_echo, m->nonce, m->reset_cause, m->retention_valid, m->version_min, m->version_max, m->snapshot.era);
        for (unsigned i = 0; i < 2u * EPOCH_JOURNAL_WORDS; i++) {
            printf("%s%08" PRIx32, i ? "," : "", m->snapshot.slots[i]);
        }
        break;
    case EPOCH_MSG_NO_SESSION:
        printf(" boot_id=%016" PRIx64 " challenge=%016" PRIx64, m->boot_id, m->challenge);
        break;
    case EPOCH_MSG_BUSY:
        printf(" remaining_ms=%" PRIu32, m->busy_remaining_ms);
        break;
    case EPOCH_MSG_EP1_REPLY:
        printf(" session=%016" PRIx64 " txid=%" PRIu32 " status=%s counter=%" PRIu32, m->session, m->txid,
               m->ep1_status == EPOCH_EP1_COMMITTED ? "COMMITTED" : "REJECTED_STALE", m->counter);
        break;
    default:
        printf(" challenge=%016" PRIx64 " nonce=%016" PRIx64 " session=%016" PRIx64 " counter=%" PRIu32 " txid=%" PRIu32 " version=%" PRIu32,
               m->challenge, m->nonce, m->session, m->counter, m->txid, m->version);
        break;
    }
    printf("\n");
    fflush(stdout);
}

static uint64_t rand64(void *ctx)
{
    (void)ctx;
    uint64_t v;
    arc4random_buf(&v, sizeof(v));
    return v;
}

static void send_msg(void *ctx, const struct epoch_msg *m)
{
    struct client *c = ctx;
    uint8_t frame[EPOCH_WIRE_MAX_FRAME];
    size_t n = epoch_wire_encode(m, frame, sizeof(frame));
    log_msg(c, "tx", m);
    for (size_t o = 0; o < n;) {
        ssize_t w = write(c->fd, frame + o, n - o);
        if (w < 0 && errno != EINTR && errno != EAGAIN) {
            c->io_error = errno;
            return;
        }
        o += w > 0 ? (size_t)w : 0u;
    }
}

static void outcome(void *ctx, uint64_t session, uint32_t txid, enum epoch_outcome o, uint32_t counter)
{
    struct client *c = ctx;
    printf("%" PRIu32 " outcome session=%016" PRIx64 " txid=%" PRIu32 " %s counter=%" PRIu32 "\n", now_ms(c), session, txid, epoch_outcome_name(o), counter);
    c->last_outcome = o;
    c->last_counter = counter;
}

/* Reads and dispatches what arrives within one read (at most 100 ms), then ticks the host. */
static void pump(struct client *c, struct epoch_host *h, struct epoch_wire_parser *p)
{
    uint8_t buf[256];
    ssize_t n = read(c->fd, buf, sizeof(buf)); /* VTIME = 1: returns after 100 ms at most, or as soon as bytes arrive */
    if (n < 0 && errno != EINTR && errno != EAGAIN) {
        c->io_error = errno;
    }
    for (ssize_t i = 0; i < n; i++) {
        struct epoch_msg m;
        enum epoch_wire_status s = epoch_wire_feed(p, buf[i], &m);
        if (s == EPOCH_WIRE_OK) {
            log_msg(c, "rx", &m);
            if (m.type == EPOCH_MSG_PONG && h && m.session == h->session && m.counter == c->pong_counter) {
                c->pongs++;
            } else if (m.type == EPOCH_MSG_EP1_REPLY) {
                c->reply = m;
                c->replies++;
                c->foreign += m.session != c->session;
            } else if (m.type == EPOCH_MSG_TEST_ARM_ACK || m.type == EPOCH_MSG_TEST_DIAG_REPLY) {
                c->test = m;
                c->tests++;
            }
            if (h) {
                epoch_host_receive(h, &m, now_ms(c));
            }
        } else if (s != EPOCH_WIRE_NONE) {
            printf("%" PRIu32 " rx_error %s\n", now_ms(c), epoch_wire_status_name(s));
            c->rx_errors++;
        }
    }
    if (h) {
        epoch_host_tick(h, now_ms(c));
    }
}

static int open_port(const char *path)
{
    int fd = open(path, O_RDWR | O_NOCTTY);
    struct termios t;
    if (fd < 0 || tcgetattr(fd, &t) != 0) {
        return -1;
    }
    cfmakeraw(&t);
    cfsetspeed(&t, B115200);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 1;
    if (tcsetattr(fd, TCSANOW, &t) != 0 || tcflush(fd, TCIOFLUSH) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Reset campaign. One trial:
 *   1. a plain INCREMENT gives the counter c0 (oracle baseline);
 *   2. TEST_ARM(run, checkpoint, t) for the next txid t, acknowledged by the board;
 *   3. INCREMENT t under the policy; the board resets at the checkpoint and the policy reports its outcome for t;
 *   4. after the host is bound again, TEST_DIAG reports the marker and the boot reset cause, and a plain INCREMENT gives c2.
 * The oracle is the device counter: c2 - c0 - 1 executions of the trial request, independent of the classifier and of the journal snapshot.
 * A trial counts only when the marker matches (run, checkpoint, t), the reset cause has SFTRSTF and the boot ID changed; trial_judge in trial_verdict.c then requires the oracle count and the outcome expected for the policy.
 * B2 and B3 are core/src/host.c. B1 and B0 bind with host.c and then handle the request themselves: B1 reports UNKNOWN on timeout and binds
 * again; B0 binds again and resends the request until it gets a reply. */
#define B0_RETRIES 3

struct cell {
    struct client *c;
    struct epoch_host *h;
    struct epoch_wire_parser *p;
    const struct epoch_host_config *cfg;
    const struct epoch_host_env *env;
    int policy;      /* 0..3 for B0..B3 */
    uint32_t txid;   /* B0 and B1: next txid in the current session */
};

static int bind(struct cell *k)
{
    epoch_host_init(k->h, k->cfg, k->env);
    epoch_host_start(k->h, now_ms(k->c));
    uint32_t start = now_ms(k->c);
    while (k->h->state != EPOCH_HOST_RUNNING && now_ms(k->c) - start < STEP_LIMIT_MS && !k->c->io_error) {
        pump(k->c, k->h, k->p);
    }
    k->c->session = k->h->session;
    k->txid = 1;
    return k->h->state == EPOCH_HOST_RUNNING;
}

/* Sends INCREMENT(t) outside host.c and waits for its reply; returns 1 with *counter on COMMITTED, 0 on timeout or another status. */
static int raw_increment(struct cell *k, uint32_t t, uint32_t *counter)
{
    struct epoch_msg m = {0};
    m.type = EPOCH_MSG_INCREMENT;
    m.session = k->c->session;
    m.txid = t;
    unsigned before = k->c->replies;
    send_msg(k->c, &m);
    uint32_t start = now_ms(k->c);
    while (now_ms(k->c) - start < k->cfg->request_timeout_ms && !k->c->io_error) {
        pump(k->c, NULL, k->p);
        if (k->c->replies != before && k->c->reply.session == m.session && k->c->reply.txid == t) {
            *counter = k->c->reply.counter;
            return k->c->reply.ep1_status == EPOCH_EP1_COMMITTED;
        }
    }
    return 0;
}

/* Plain INCREMENT through the policy's own path, for the oracle; returns 1 with *counter when it completed. */
static int plain_increment(struct cell *k, uint32_t *counter)
{
    if (k->policy < 2) {
        return raw_increment(k, k->txid++, counter);
    }
    k->c->last_outcome = EPOCH_OUTCOME_NONE;
    uint32_t t = epoch_host_increment(k->h, now_ms(k->c));
    uint32_t start = now_ms(k->c);
    while (t && k->c->last_outcome == EPOCH_OUTCOME_NONE && now_ms(k->c) - start < STEP_LIMIT_MS && !k->c->io_error) {
        pump(k->c, k->h, k->p);
    }
    *counter = k->c->last_counter;
    return t && k->c->last_outcome == EPOCH_OUTCOME_COMPLETED;
}

static int test_exchange(struct cell *k, const struct epoch_msg *m, enum epoch_msg_type want)
{
    unsigned before = k->c->tests;
    send_msg(k->c, m);
    uint32_t start = now_ms(k->c);
    while (now_ms(k->c) - start < 1000u && !k->c->io_error) {
        pump(k->c, k->policy >= 2 ? k->h : NULL, k->p);
        if (k->c->tests != before && k->c->test.type == want) {
            return 1;
        }
    }
    return 0;
}

static const char *const POLICY[] = {"b0", "b1", "b2", "b3"};
static const char *const CP[] = {"f1", "f2a", "f2b", "f2c", "f3"};

static int lookup(const char *const *names, int n, const char *s)
{
    for (int i = 0; i < n; i++) {
        if (strcmp(names[i], s) == 0) {
            return i;
        }
    }
    return -1;
}

/* Runs one trial and writes its JSON line; returns 0 COUNTED_PASS, 1 COUNTED_FAIL, 2 INCONCLUSIVE, 3 NOT_RUN. */
static int trial(struct cell *k, int cp, uint32_t run, FILE *out)
{
    static const char *const VERDICT[] = {"COUNTED_PASS", "COUNTED_FAIL", "INCONCLUSIVE", "NOT_RUN"};
    struct client *c = k->c;
    uint32_t c0 = 0, c2 = 0, retry_counter = 0;
    uint64_t s0 = c->session, b0 = k->h->boot_id;
    unsigned foreign0 = c->foreign, rx0 = c->rx_errors;
    enum epoch_outcome o = EPOCH_OUTCOME_NONE;
    const char *why = "";
    int v = 3, retries = 0;
    struct epoch_msg arm = {0}, diag = {0};

    if ((k->h->state != EPOCH_HOST_RUNNING && !bind(k)) || !plain_increment(k, &c0)) {
        why = "baseline INCREMENT failed";
        goto done;
    }
    s0 = c->session;
    b0 = k->h->boot_id;
    uint32_t t = k->policy < 2 ? k->txid : k->h->next_txid;
    arm.type = EPOCH_MSG_TEST_ARM;
    arm.run = run;
    arm.checkpoint = (uint32_t)cp;
    arm.txid = t;
    if (!test_exchange(k, &arm, EPOCH_MSG_TEST_ARM_ACK) || c->test.run != run || c->test.checkpoint != (uint32_t)cp || c->test.txid != t) {
        why = "TEST_ARM not acknowledged";
        goto done;
    }
    printf("%" PRIu32 " commanded run=%" PRIu32 " checkpoint=%s txid=%" PRIu32 "\n", now_ms(c), run, CP[cp], t);
    fflush(stdout);

    /* 3. the request under the policy */
    v = 2;
    if (k->policy >= 2) {
        c->last_outcome = EPOCH_OUTCOME_NONE;
        if (epoch_host_increment(k->h, now_ms(c)) != t) {
            why = "host refused the INCREMENT";
            goto done;
        }
        /* final once the host is RUNNING again in a new session: B3 has refined at START_ACK by then */
        uint32_t start = now_ms(c);
        while (!(k->h->state == EPOCH_HOST_RUNNING && k->h->session != s0) && c->last_outcome != EPOCH_OUTCOME_COMPLETED &&
               now_ms(c) - start < 3u * STEP_LIMIT_MS && !c->io_error) {
            pump(c, k->h, k->p);
        }
        o = c->last_outcome;
        c->session = k->h->session;
    } else {
        k->txid++;
        if (raw_increment(k, t, &retry_counter)) {
            o = EPOCH_OUTCOME_COMPLETED;
        } else if (k->policy == 1) {
            o = EPOCH_OUTCOME_UNKNOWN;
            bind(k);
        } else {
            while (o == EPOCH_OUTCOME_NONE && retries++ < B0_RETRIES && bind(k)) {
                if (raw_increment(k, k->txid++, &retry_counter)) {
                    o = EPOCH_OUTCOME_COMPLETED;
                }
            }
        }
    }
    if (k->h->state != EPOCH_HOST_RUNNING && !bind(k)) {
        why = "could not bind after the reset";
        goto done;
    }

    /* 4. oracle */
    diag.type = EPOCH_MSG_TEST_DIAG;
    diag.run = run;
    if (!test_exchange(k, &diag, EPOCH_MSG_TEST_DIAG_REPLY)) {
        why = "TEST_DIAG not answered";
        goto done;
    }
    diag = c->test;
    if (!plain_increment(k, &c2)) {
        why = "oracle INCREMENT failed";
        goto done;
    }
    const struct trial_evidence e = {k->policy, (enum epoch_checkpoint)cp, run, t, diag.marker_valid, diag.run, diag.checkpoint, diag.txid,
                                     diag.reset_cause, b0, k->h->boot_id, o, c0, c2, c->foreign - foreign0, c->rx_errors - rx0};
    v = (int)trial_judge(&e, &why);

done:
    fprintf(out,
            "{\"utc_ms\":%lld,\"policy\":\"%s\",\"checkpoint\":\"%s\",\"run\":%" PRIu32 ",\"verdict\":\"%s\",\"why\":\"%s\",\"outcome\":\"%s\",\"c0\":%" PRIu32
            ",\"c2\":%" PRIu32 ",\"retry_counter\":%" PRIu32 ",\"b0_retries\":%d,\"marker\":{\"valid\":%d,\"run\":%" PRIu32 ",\"checkpoint\":%" PRIu32
            ",\"txid\":%" PRIu32 "},\"reset_cause\":\"%08" PRIx32 "\",\"armed_txid\":%" PRIu32 ",\"boot_before\":\"%016" PRIx64 "\",\"boot_after\":\"%016" PRIx64
            "\",\"session_before\":\"%016" PRIx64 "\",\"session_after\":\"%016" PRIx64 "\",\"retention_valid\":%d,\"foreign_replies\":%u,\"rx_errors\":%u}\n",
            (long long)time(NULL) * 1000, POLICY[k->policy], CP[cp], run, VERDICT[v], why, epoch_outcome_name(o), c0, c2, retry_counter, retries,
            diag.marker_valid, diag.run, diag.checkpoint, diag.txid, diag.reset_cause, arm.txid, b0, k->h->boot_id, s0, c->session,
            k->h->retention_valid, c->foreign - foreign0, c->rx_errors - rx0);
    fflush(out);
    fsync(fileno(out));
    printf("%" PRIu32 " TRIAL run=%" PRIu32 " %s %s outcome=%s c0=%" PRIu32 " c2=%" PRIu32 " %s\n", now_ms(c), run, CP[cp], VERDICT[v],
           epoch_outcome_name(o), c0, c2, why);
    fflush(stdout);
    return v;
}

static int campaign(struct client *c, const struct epoch_host_env *env, char **argv)
{
    char *end;
    int policy = lookup(POLICY, 4, argv[3]), cp = lookup(CP, 5, argv[4]);
    long trials = strtol(argv[5], &end, 10);
    unsigned long run_base = *end ? 0 : strtoul(argv[6], &end, 10);
    if (policy < 0 || cp < 0 || trials < 1 || trials > 200 || *end || run_base == 0 || run_base > 0xFFFF0000ul) {
        fprintf(stderr, "usage: epoch_client <port> campaign <b0|b1|b2|b3> <f1|f2a|f2b|f2c|f3> <trials 1..200> <run_base> <trials.jsonl>\n");
        return 2;
    }
    FILE *out = fopen(argv[7], "a");
    if (!out) {
        fprintf(stderr, "FAIL open %s: %s\n", argv[7], strerror(errno));
        return 2;
    }
    const struct epoch_host_config cfg = {{JOURNAL_MAGIC_VERSION, epoch_crc32_words}, policy == 3 ? EPOCH_POLICY_B3 : EPOCH_POLICY_B2,
                                          EPOCH_WIRE_VERSION, 500u, 500u, 500u, 100u};
    struct epoch_host h;
    struct epoch_wire_parser p;
    epoch_wire_parser_init(&p);
    struct cell k = {c, &h, &p, &cfg, env, policy, 1};
    epoch_host_init(&h, &cfg, env);
    unsigned counts[4] = {0}, streak = 0;
    for (long i = 0; i < trials && streak < 3 && !c->io_error; i++) {
        int v = trial(&k, cp, (uint32_t)(run_base + (unsigned long)i), out);
        counts[v]++;
        streak = v >= 2 ? streak + 1 : 0;
    }
    fclose(out);
    printf("RESULT %s %s COUNTED_PASS=%u COUNTED_FAIL=%u INCONCLUSIVE=%u NOT_RUN=%u%s io_error=%d\n", POLICY[policy], CP[cp], counts[0], counts[1],
           counts[2], counts[3], streak >= 3 ? " STOPPED_AFTER_3_UNCOUNTED" : "", c->io_error);
    return counts[1] || counts[2] || counts[3] || c->io_error ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc == 8 && strcmp(argv[2], "campaign") == 0) {
        struct client c = {0};
        clock_gettime(CLOCK_MONOTONIC, &c.t0);
        c.fd = open_port(argv[1]);
        if (c.fd < 0) {
            fprintf(stderr, "FAIL open %s: %s\n", argv[1], strerror(errno));
            return 2;
        }
        const struct epoch_host_env env = {&c, rand64, send_msg, outcome};
        int rc = campaign(&c, &env, argv);
        close(c.fd);
        return rc;
    }
    char *end1, *end2;
    long pings = argc == 4 ? strtol(argv[2], &end1, 10) : -1;
    long incs = argc == 4 ? strtol(argv[3], &end2, 10) : -1;
    if (argc != 4 || *end1 || *end2 || pings < 0 || pings > 10000 || incs < 0 || incs > 10000) {
        fprintf(stderr, "usage: epoch_client <port> <pings 0..10000> <increments 0..10000>\n");
        return 2;
    }
    struct client c = {0};
    clock_gettime(CLOCK_MONOTONIC, &c.t0);
    c.fd = open_port(argv[1]);
    if (c.fd < 0) {
        fprintf(stderr, "FAIL open %s: %s\n", argv[1], strerror(errno));
        return 2;
    }
    const struct epoch_host_config cfg = {{JOURNAL_MAGIC_VERSION, epoch_crc32_words}, EPOCH_POLICY_B2, EPOCH_WIRE_VERSION, 500u, 500u, 500u, 100u};
    const struct epoch_host_env env = {&c, rand64, send_msg, outcome};
    struct epoch_host h;
    struct epoch_wire_parser p;
    epoch_wire_parser_init(&p);
    epoch_host_init(&h, &cfg, &env);
    epoch_host_start(&h, now_ms(&c));

    uint32_t start = now_ms(&c);
    while (h.state != EPOCH_HOST_RUNNING && now_ms(&c) - start < STEP_LIMIT_MS && !c.io_error) {
        pump(&c, &h, &p);
    }
    int ok = h.state == EPOCH_HOST_RUNNING;
    uint64_t session = h.session;

    for (long i = 0; ok && i < pings; i++) {
        struct epoch_msg m = {0};
        m.type = EPOCH_MSG_PING;
        m.session = h.session;
        m.counter = c.pong_counter = (uint32_t)i + 1u;
        unsigned before = c.pongs;
        send_msg(&c, &m);
        start = now_ms(&c);
        while (c.pongs == before && now_ms(&c) - start < STEP_LIMIT_MS && !c.io_error) {
            pump(&c, &h, &p);
        }
        ok = c.pongs == before + 1u;
    }

    unsigned completed = 0;
    uint32_t first_counter = 0, prev_counter = 0;
    for (long i = 0; ok && i < incs; i++) {
        c.last_outcome = EPOCH_OUTCOME_NONE;
        uint32_t txid = epoch_host_increment(&h, now_ms(&c));
        start = now_ms(&c);
        while (txid && c.last_outcome == EPOCH_OUTCOME_NONE && now_ms(&c) - start < STEP_LIMIT_MS && !c.io_error) {
            pump(&c, &h, &p);
        }
        /* each completed INCREMENT must return the previous counter plus one */
        ok = txid != 0 && c.last_outcome == EPOCH_OUTCOME_COMPLETED && (i == 0 || c.last_counter == prev_counter + 1u);
        first_counter = i == 0 ? c.last_counter : first_counter;
        prev_counter = c.last_counter;
        completed += ok ? 1u : 0u;
    }
    ok = ok && !c.io_error && h.session == session;
    printf("RESULT %s state=%d session=%016" PRIx64 " boot_id=%016" PRIx64 " pongs=%u/%ld increments=%u/%ld counter=%" PRIu32 "..%" PRIu32 " io_error=%d\n",
           ok ? "PASS" : "FAIL", (int)h.state, h.session, h.boot_id, c.pongs, pings, completed, incs, first_counter, prev_counter, c.io_error);
    close(c.fd);
    return ok ? 0 : 1;
}
