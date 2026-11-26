#include <string.h>

#include "epoch/controller.h"

#define W EPOCH_JOURNAL_WORDS

static struct epoch_msg msg(enum epoch_msg_type type)
{
    struct epoch_msg m;
    memset(&m, 0, sizeof m);
    m.type = type;
    return m;
}

static void send(struct epoch_ctrl *c, const struct epoch_msg *m)
{
    c->env->send(c->env->ctx, m);
}

static bool checkpoint(struct epoch_ctrl *c, enum epoch_checkpoint cp, uint32_t txid)
{
    return c->env->checkpoint && c->env->checkpoint(c->env->ctx, cp, txid);
}

static void mint_u(struct epoch_ctrl *c, bool owned, uint64_t owner)
{
    c->u = c->env->rand64(c->env->ctx);
    c->u_present = true;
    c->u_owned = owned;
    c->u_owner = owner;
}

/* HELLO consumes U and START does not mint a new one, so a message that must carry U mints an unowned one. */
static uint64_t current_u(struct epoch_ctrl *c)
{
    if (!c->u_present)
        mint_u(c, false, 0);
    return c->u;
}

static void announce(struct epoch_ctrl *c, uint64_t challenge, bool echo, uint64_t nonce)
{
    struct epoch_msg m = msg(EPOCH_MSG_ANNOUNCE);
    m.boot_id = c->boot_id;
    m.challenge = challenge;
    m.nonce_echo = echo;
    m.nonce = nonce;
    m.reset_cause = c->reset_cause;
    m.retention_valid = c->retention_valid;
    m.snapshot = c->snapshot;
    m.version_min = c->cfg->version_min;
    m.version_max = c->cfg->version_max;
    m.capabilities = c->cfg->capabilities;
    send(c, &m);
}

static void hello_ack(struct epoch_ctrl *c)
{
    struct epoch_msg m = msg(EPOCH_MSG_HELLO_ACK);
    m.challenge = c->p_challenge;
    m.nonce = c->p_nonce;
    m.session = c->p_session;
    send(c, &m);
}

static void with_session(struct epoch_ctrl *c, enum epoch_msg_type type, uint64_t session)
{
    struct epoch_msg m = msg(type);
    m.session = session;
    send(c, &m);
}

static void busy(struct epoch_ctrl *c, uint32_t now)
{
    struct epoch_msg m = msg(EPOCH_MSG_BUSY);
    int32_t left = (int32_t)(c->p_deadline - now);
    m.busy_remaining_ms = left > 0 ? (uint32_t)left : 0u;
    send(c, &m);
}

static void no_session(struct epoch_ctrl *c, uint32_t now, bool throttled)
{
    if (throttled && c->no_session_sent && now - c->no_session_at < EPOCH_NO_SESSION_INTERVAL_MS)
        return;
    c->no_session_sent = true;
    c->no_session_at = now;
    struct epoch_msg m = msg(EPOCH_MSG_NO_SESSION);
    m.boot_id = c->boot_id;
    m.challenge = current_u(c);
    send(c, &m);
}

static void ep1_reply(struct epoch_ctrl *c, uint32_t txid, enum epoch_ep1_status status)
{
    struct epoch_msg m = msg(EPOCH_MSG_EP1_REPLY);
    m.session = c->session;
    m.txid = txid;
    m.ep1_status = status;
    m.counter = c->counter;
    send(c, &m);
}

static void barrier(void *ctx)
{
    struct epoch_ctrl *c = ctx;
    c->env->barrier(c->env->ctx);
}

static bool after_store(void *ctx, const volatile uint32_t *slot, unsigned index)
{
    struct epoch_ctrl *c = ctx;
    if (c->env->after_store && c->env->after_store(c->env->ctx, slot, index))
        return true;
    if (!c->writing_increment)
        return false;
    switch (index) {
    case EPOCH_STORE_F2A: return checkpoint(c, EPOCH_CP_F2A, c->write_txid);
    case EPOCH_STORE_F2B: return checkpoint(c, EPOCH_CP_F2B, c->write_txid);
    case EPOCH_STORE_F2C: return checkpoint(c, EPOCH_CP_F2C, c->write_txid);
    default: return false;
    }
}

static enum epoch_publish_result publish(struct epoch_ctrl *c, uint64_t session, uint32_t txid, uint32_t counter, bool increment)
{
    struct epoch_journal_io io = { c, barrier, after_store };
    c->writing_increment = increment;
    c->write_txid = txid;
    enum epoch_publish_result r = epoch_journal_publish(&c->cfg->journal, &io, c->ret->slots, c->ret->era, session, txid, counter);
    c->writing_increment = false;
    return r;
}

enum epoch_ctrl_result epoch_ctrl_boot(struct epoch_ctrl *c, const struct epoch_ctrl_config *cfg, const struct epoch_ctrl_env *env, struct epoch_retained *ret, bool cold, uint32_t reset_cause, uint32_t now)
{
    (void)now;
    memset(c, 0, sizeof *c);
    c->cfg = cfg;
    c->env = env;
    c->ret = ret;
    c->reset_cause = reset_cause;
    for (unsigned i = 0; i < 2 * W; i++)
        c->snapshot.slots[i] = ret->slots[i];
    c->snapshot.era = ret->era;
    /* Corrupted retention (an inconsistent pair, or a slot changed after it committed): report retention invalid and start cold, so every affected outcome stays UNKNOWN, the counter does not fall back to an older record, and sessions can open again. */
    int n = !cold && ret->era != 0 ? epoch_journal_newest(&cfg->journal, ret->slots, ret->era) : EPOCH_SLOT_INCONSISTENT;
    c->retention_valid = n != EPOCH_SLOT_INCONSISTENT;
    if (n >= 0) {
        c->counter = ret->slots[(unsigned)n * W + 5];
    } else if (!c->retention_valid) {
        struct epoch_journal_io io = { c, barrier, after_store };
        if (epoch_journal_cold_init(&io, ret->slots, &ret->era, env->new_era(env->ctx)) == EPOCH_PUBLISH_RESET)
            return EPOCH_CTRL_RESET;
    }
    c->boot_id = env->rand64(env->ctx);
    mint_u(c, false, 0);
    announce(c, c->u, false, 0);
    return EPOCH_CTRL_OK;
}

void epoch_ctrl_tick(struct epoch_ctrl *c, uint32_t now)
{
    if (c->pending && (int32_t)(now - c->p_deadline) >= 0) {
        c->pending = false;
        mint_u(c, false, 0);
    }
}

static enum epoch_ctrl_result increment(struct epoch_ctrl *c, const struct epoch_msg *m, uint32_t now)
{
    if (!c->active || m->session != c->session) {
        no_session(c, now, true);
        return EPOCH_CTRL_OK;
    }
    if (m->txid <= c->fence) {
        ep1_reply(c, m->txid, EPOCH_EP1_REJECTED_STALE);
        return EPOCH_CTRL_OK;
    }
    if (checkpoint(c, EPOCH_CP_F1, m->txid))
        return EPOCH_CTRL_RESET;
    uint32_t next = c->counter + 1u;
    enum epoch_publish_result r = publish(c, c->session, m->txid, next, true);
    if (r == EPOCH_PUBLISH_RESET)
        return EPOCH_CTRL_RESET;
    if (r == EPOCH_PUBLISH_INCONSISTENT)
        return EPOCH_CTRL_OK; /* corrupted journal: nothing executes, so no reply */
    c->counter = next;
    c->fence = m->txid;
    if (checkpoint(c, EPOCH_CP_F3, m->txid) || checkpoint(c, EPOCH_CP_F4, m->txid))
        return EPOCH_CTRL_RESET;
    ep1_reply(c, m->txid, EPOCH_EP1_COMMITTED);
    return EPOCH_CTRL_OK;
}

enum epoch_ctrl_result epoch_ctrl_receive(struct epoch_ctrl *c, const struct epoch_msg *m, uint32_t now)
{
    bool supported;
    epoch_ctrl_tick(c, now);
    switch (m->type) {
    case EPOCH_MSG_PROBE:
        if (c->pending) {
            if (m->nonce == c->p_nonce)
                announce(c, c->p_challenge, true, m->nonce);
            else
                busy(c, now);
        } else {
            if (c->u_present && (!c->u_owned || c->u_owner == m->nonce)) {
                c->u_owned = true;
                c->u_owner = m->nonce;
            } else {
                mint_u(c, true, m->nonce);
            }
            announce(c, c->u, true, m->nonce);
        }
        break;
    case EPOCH_MSG_HELLO:
        /* A version outside the advertised range is handled like a HELLO for the wrong challenge: no pending session is created, the active session is untouched, and outside a pending handshake the ANNOUNCE repeats the supported range. */
        supported = m->version >= c->cfg->version_min && m->version <= c->cfg->version_max;
        if (c->pending) {
            if (supported && m->challenge == c->p_challenge && m->nonce == c->p_nonce)
                hello_ack(c);
            else
                busy(c, now);
        } else if (supported && c->u_present && m->challenge == c->u && (!c->u_owned || c->u_owner == m->nonce)) {
            c->pending = true;
            c->p_challenge = m->challenge;
            c->p_nonce = m->nonce;
            c->p_session = c->env->rand64(c->env->ctx);
            c->p_deadline = now + c->cfg->t_pending_ms;
            c->u_present = false;
            hello_ack(c);
        } else {
            announce(c, current_u(c), false, 0); /* includes C == U owned by another nonce, a case with no dedicated transition */
        }
        break;
    case EPOCH_MSG_START:
        if (c->pending && m->session == c->p_session) {
            /* EP1 runs to completion inside receive here, so no command is in progress when SESSION_OPEN is written. */
            enum epoch_publish_result r = publish(c, m->session, 0, c->counter, false);
            if (r == EPOCH_PUBLISH_RESET)
                return EPOCH_CTRL_RESET;
            if (r == EPOCH_PUBLISH_INCONSISTENT)
                break;
            c->active = true;
            c->session = m->session;
            c->pending = false;
            c->fence = 0;
            with_session(c, EPOCH_MSG_START_ACK, c->session);
        } else if (c->active && m->session == c->session) {
            with_session(c, EPOCH_MSG_START_ACK, c->session);
        } else {
            no_session(c, now, false);
        }
        break;
    case EPOCH_MSG_PING:
        if (c->active && m->session == c->session) {
            struct epoch_msg p = msg(EPOCH_MSG_PONG);
            p.session = c->session;
            p.counter = m->counter;
            send(c, &p);
        } else {
            no_session(c, now, true);
        }
        break;
    case EPOCH_MSG_INCREMENT:
        return increment(c, m, now);
    default:
        break;
    }
    return EPOCH_CTRL_OK;
}
