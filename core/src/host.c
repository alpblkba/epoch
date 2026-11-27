#include <string.h>

#include "epoch/host.h"

static struct epoch_msg msg(enum epoch_msg_type type)
{
    struct epoch_msg m;
    memset(&m, 0, sizeof m);
    m.type = type;
    return m;
}

static void send(struct epoch_host *h, const struct epoch_msg *m)
{
    h->env->send(h->env->ctx, m);
}

static void arm(struct epoch_host *h, uint32_t at)
{
    h->timer = true;
    h->deadline = at;
}

static void report(struct epoch_host *h, enum epoch_outcome o, uint32_t counter)
{
    h->env->outcome(h->env->ctx, h->req_session, h->req_txid, o, counter);
}

static void settle(struct epoch_host *h, enum epoch_outcome o, uint32_t counter)
{
    report(h, o, counter);
    h->req = false;
}

static void mark_unknown(struct epoch_host *h)
{
    if (h->req && h->req_outcome == EPOCH_OUTCOME_NONE) {
        h->req_outcome = EPOCH_OUTCOME_UNKNOWN;
        report(h, EPOCH_OUTCOME_UNKNOWN, 0);
    }
}

static void send_hello(struct epoch_host *h)
{
    struct epoch_msg m = msg(EPOCH_MSG_HELLO);
    m.challenge = h->challenge;
    m.nonce = h->nonce;
    m.version = h->cfg->version;
    send(h, &m);
}

static void send_session(struct epoch_host *h, enum epoch_msg_type type)
{
    struct epoch_msg m = msg(type);
    m.session = h->session;
    m.counter = type == EPOCH_MSG_PING ? ++h->ping_seq : 0u;
    send(h, &m);
}

static void probe(struct epoch_host *h, uint32_t now, bool fresh)
{
    if (fresh)
        h->nonce = h->env->rand64(h->env->ctx);
    struct epoch_msg m = msg(EPOCH_MSG_PROBE);
    m.nonce = h->nonce;
    send(h, &m);
    h->state = EPOCH_HOST_PROBING;
    h->retries = 0;
    arm(h, now + h->cfg->handshake_timeout_ms);
}

/* RUNNING row for ANNOUNCE with another B, NO_SESSION or a request timeout. */
static void verify(struct epoch_host *h, uint32_t now)
{
    mark_unknown(h);
    send_session(h, EPOCH_MSG_PING);
    h->state = EPOCH_HOST_VERIFYING;
    arm(h, now + h->cfg->ping_timeout_ms);
}

static void resolve_from_snapshot(struct epoch_host *h)
{
    if (!h->req)
        return;
    uint32_t counter = 0;
    enum epoch_outcome o = EPOCH_OUTCOME_UNKNOWN;
    if (h->cfg->policy == EPOCH_POLICY_B3)
        o = epoch_classify(&h->cfg->journal, h->retention_valid, &h->snapshot, h->req_session, h->req_txid, &counter);
    if (o == EPOCH_OUTCOME_UNKNOWN)
        mark_unknown(h);
    else
        settle(h, o, counter);
}

static void ep1_reply(struct epoch_host *h, const struct epoch_msg *m)
{
    if (!h->req || m->session != h->req_session || m->txid != h->req_txid)
        return;
    // a REJECTED_STALE reply covers one delivery of this txid. An earlier copy may already have committed.
    if (m->ep1_status != EPOCH_EP1_COMMITTED)
        return;
    bool in_flight = h->req_outcome == EPOCH_OUTCOME_NONE;
    settle(h, EPOCH_OUTCOME_COMPLETED, m->counter);
    if (in_flight && h->state == EPOCH_HOST_RUNNING)
        h->timer = false;
}

void epoch_host_init(struct epoch_host *h, const struct epoch_host_config *cfg, const struct epoch_host_env *env)
{
    memset(h, 0, sizeof *h);
    h->cfg = cfg;
    h->env = env;
    h->state = EPOCH_HOST_DOWN;
}

void epoch_host_start(struct epoch_host *h, uint32_t now)
{
    if (h->state == EPOCH_HOST_DOWN)
        probe(h, now, true);
}

void epoch_host_receive(struct epoch_host *h, const struct epoch_msg *m, uint32_t now)
{
    switch (m->type) {
    case EPOCH_MSG_ANNOUNCE:
        if (h->state == EPOCH_HOST_DOWN || h->state == EPOCH_HOST_PROBING) {
            if (h->state == EPOCH_HOST_DOWN)
                h->nonce = h->env->rand64(h->env->ctx);
            h->boot_id = m->boot_id;
            h->retention_valid = m->retention_valid;
            h->snapshot = m->snapshot;
            h->challenge = m->challenge;
            send_hello(h);
            h->state = EPOCH_HOST_BINDING;
            h->retries = 0;
            arm(h, now + h->cfg->handshake_timeout_ms);
        } else if (h->state == EPOCH_HOST_RUNNING && m->boot_id != h->boot_id) {
            verify(h, now);
        }
        break;
    case EPOCH_MSG_BUSY:
        if (h->state == EPOCH_HOST_PROBING || h->state == EPOCH_HOST_BINDING) {
            uint32_t jitter = (uint32_t)(h->env->rand64(h->env->ctx) % ((uint64_t)h->cfg->busy_jitter_ms + 1u));
            h->state = EPOCH_HOST_PROBING; /* PROBE(H) goes out when the timer fires */
            h->retries = 0;
            arm(h, now + m->busy_remaining_ms + jitter);
        }
        break;
    case EPOCH_MSG_HELLO_ACK:
        if (h->state == EPOCH_HOST_BINDING && m->challenge == h->challenge && m->nonce == h->nonce) {
            h->session = m->session;
            send_session(h, EPOCH_MSG_START);
            h->state = EPOCH_HOST_STARTING;
            h->retries = 0;
            arm(h, now + h->cfg->handshake_timeout_ms);
        }
        break;
    case EPOCH_MSG_START_ACK:
        if (h->state == EPOCH_HOST_STARTING && m->session == h->session) {
            resolve_from_snapshot(h);
            h->next_txid = 1;
            h->state = EPOCH_HOST_RUNNING;
            h->timer = false;
        }
        break;
    case EPOCH_MSG_NO_SESSION:
        if (h->state == EPOCH_HOST_BINDING || h->state == EPOCH_HOST_STARTING)
            probe(h, now, false);
        else if (h->state == EPOCH_HOST_RUNNING)
            verify(h, now);
        else if (h->state == EPOCH_HOST_VERIFYING)
            probe(h, now, true);
        break;
    case EPOCH_MSG_PONG:
        if (h->state == EPOCH_HOST_VERIFYING && m->session == h->session) {
            h->state = EPOCH_HOST_RUNNING; /* the session is live; an UNKNOWN stays */
            h->timer = false;
        }
        break;
    case EPOCH_MSG_EP1_REPLY:
        ep1_reply(h, m);
        break;
    default:
        break; /* events a state does not handle are ignored */
    }
}

void epoch_host_tick(struct epoch_host *h, uint32_t now)
{
    if (!h->timer || (int32_t)(now - h->deadline) < 0)
        return;
    h->timer = false;
    switch (h->state) {
    case EPOCH_HOST_PROBING:
        probe(h, now, false); /* PROBING has no timeout transition of its own; PROBE is idempotent, so it is resent */
        break;
    case EPOCH_HOST_BINDING:
    case EPOCH_HOST_STARTING:
        if (h->retries < EPOCH_HOST_RETRANSMIT_LIMIT) {
            h->retries++;
            if (h->state == EPOCH_HOST_BINDING)
                send_hello(h);
            else
                send_session(h, EPOCH_MSG_START);
            arm(h, now + h->cfg->handshake_timeout_ms);
        } else {
            probe(h, now, false);
        }
        break;
    case EPOCH_HOST_RUNNING:
        verify(h, now);
        break;
    case EPOCH_HOST_VERIFYING:
        probe(h, now, true);
        break;
    default:
        break;
    }
}

uint32_t epoch_host_increment(struct epoch_host *h, uint32_t now)
{
    if (h->state != EPOCH_HOST_RUNNING || (h->req && h->req_outcome == EPOCH_OUTCOME_NONE))
        return 0;
    if (h->next_txid == 0) {
        probe(h, now, true); /* txid space exhausted: renegotiate */
        return 0;
    }
    h->req = true; /* an older UNKNOWN is superseded and stays UNKNOWN */
    h->req_session = h->session;
    h->req_txid = h->next_txid++;
    h->req_outcome = EPOCH_OUTCOME_NONE;
    struct epoch_msg m = msg(EPOCH_MSG_INCREMENT);
    m.session = h->session;
    m.txid = h->req_txid;
    send(h, &m);
    arm(h, now + h->cfg->request_timeout_ms);
    return h->req_txid;
}
