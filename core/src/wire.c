#include "epoch/wire.h"

#include <string.h>

#include "epoch/crc32.h"

#define MAX_ENCODED (EPOCH_WIRE_MAX_BODY + 1u) /* COBS of a maximal body: one code byte per 254 data bytes */

/* Field order per type is defined by the encode and decode switches below and must match host/client/epoch_wire.py. A cursor walks the body; every access is bounds-checked by the caller's length check. */
struct cursor {
    uint8_t *p;
    const uint8_t *q;
};

static void put32(struct cursor *c, uint32_t v)
{
    for (unsigned i = 0; i < 4u; i++) {
        *c->p++ = (uint8_t)(v >> (8u * i));
    }
}

static void put64(struct cursor *c, uint64_t v)
{
    put32(c, (uint32_t)v);
    put32(c, (uint32_t)(v >> 32));
}

static uint32_t get32(struct cursor *c)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < 4u; i++) {
        v |= (uint32_t)*c->q++ << (8u * i);
    }
    return v;
}

static uint64_t get64(struct cursor *c)
{
    uint64_t lo = get32(c);
    return lo | (uint64_t)get32(c) << 32;
}

/* Field bytes after the header and before the CRC; 0 for an invalid type. */
static size_t fields_len(enum epoch_msg_type t)
{
    switch (t) {
    case EPOCH_MSG_PROBE:
        return 8;
    case EPOCH_MSG_ANNOUNCE:
        return 8 + 8 + 8 + 4 * 5 + 4 * 2 * EPOCH_JOURNAL_WORDS; /* B U H reset_cause vmin vmax caps era, slots */
    case EPOCH_MSG_HELLO:
        return 8 + 8 + 4;
    case EPOCH_MSG_HELLO_ACK:
        return 8 + 8 + 8;
    case EPOCH_MSG_START:
    case EPOCH_MSG_START_ACK:
        return 8;
    case EPOCH_MSG_PING:
    case EPOCH_MSG_PONG:
        return 8 + 4;
    case EPOCH_MSG_NO_SESSION:
        return 8 + 8;
    case EPOCH_MSG_BUSY:
        return 4;
    case EPOCH_MSG_INCREMENT:
        return 8 + 4;
    case EPOCH_MSG_EP1_REPLY:
        return 8 + 4 + 1 + 4;
    case EPOCH_MSG_TEST_ARM:
    case EPOCH_MSG_TEST_ARM_ACK:
        return 4 + 4 + 4;
    case EPOCH_MSG_TEST_DIAG:
        return 4;
    case EPOCH_MSG_TEST_DIAG_REPLY:
        return 1 + 4 + 4 + 4 + 4;
    default:
        return 0;
    }
}

static void put_fields(struct cursor *c, const struct epoch_msg *m)
{
    switch (m->type) {
    case EPOCH_MSG_PROBE:
        put64(c, m->nonce);
        break;
    case EPOCH_MSG_ANNOUNCE:
        put64(c, m->boot_id);
        put64(c, m->challenge);
        put64(c, m->nonce_echo ? m->nonce : 0);
        put32(c, m->reset_cause);
        put32(c, m->version_min);
        put32(c, m->version_max);
        put32(c, m->capabilities);
        put32(c, m->snapshot.era);
        for (unsigned i = 0; i < 2u * EPOCH_JOURNAL_WORDS; i++) {
            put32(c, m->snapshot.slots[i]);
        }
        break;
    case EPOCH_MSG_HELLO:
        put64(c, m->challenge);
        put64(c, m->nonce);
        put32(c, m->version);
        break;
    case EPOCH_MSG_HELLO_ACK:
        put64(c, m->challenge);
        put64(c, m->nonce);
        put64(c, m->session);
        break;
    case EPOCH_MSG_START:
    case EPOCH_MSG_START_ACK:
        put64(c, m->session);
        break;
    case EPOCH_MSG_PING:
    case EPOCH_MSG_PONG:
        put64(c, m->session);
        put32(c, m->counter);
        break;
    case EPOCH_MSG_NO_SESSION:
        put64(c, m->boot_id);
        put64(c, m->challenge);
        break;
    case EPOCH_MSG_BUSY:
        put32(c, m->busy_remaining_ms);
        break;
    case EPOCH_MSG_INCREMENT:
        put64(c, m->session);
        put32(c, m->txid);
        break;
    case EPOCH_MSG_EP1_REPLY:
        put64(c, m->session);
        put32(c, m->txid);
        *c->p++ = (uint8_t)m->ep1_status;
        put32(c, m->counter);
        break;
    case EPOCH_MSG_TEST_ARM:
    case EPOCH_MSG_TEST_ARM_ACK:
        put32(c, m->run);
        put32(c, m->checkpoint);
        put32(c, m->txid);
        break;
    case EPOCH_MSG_TEST_DIAG:
        put32(c, m->run);
        break;
    case EPOCH_MSG_TEST_DIAG_REPLY:
        *c->p++ = m->marker_valid ? 1u : 0u;
        put32(c, m->run);
        put32(c, m->checkpoint);
        put32(c, m->txid);
        put32(c, m->reset_cause);
        break;
    default:
        break;
    }
}

static enum epoch_wire_status get_fields(struct cursor *c, struct epoch_msg *m)
{
    switch (m->type) {
    case EPOCH_MSG_PROBE:
        m->nonce = get64(c);
        break;
    case EPOCH_MSG_ANNOUNCE:
        m->boot_id = get64(c);
        m->challenge = get64(c);
        m->nonce = get64(c);
        m->reset_cause = get32(c);
        m->version_min = get32(c);
        m->version_max = get32(c);
        m->capabilities = get32(c);
        m->snapshot.era = get32(c);
        for (unsigned i = 0; i < 2u * EPOCH_JOURNAL_WORDS; i++) {
            m->snapshot.slots[i] = get32(c);
        }
        break;
    case EPOCH_MSG_HELLO:
        m->challenge = get64(c);
        m->nonce = get64(c);
        m->version = get32(c);
        break;
    case EPOCH_MSG_HELLO_ACK:
        m->challenge = get64(c);
        m->nonce = get64(c);
        m->session = get64(c);
        break;
    case EPOCH_MSG_START:
    case EPOCH_MSG_START_ACK:
        m->session = get64(c);
        break;
    case EPOCH_MSG_PING:
    case EPOCH_MSG_PONG:
        m->session = get64(c);
        m->counter = get32(c);
        break;
    case EPOCH_MSG_NO_SESSION:
        m->boot_id = get64(c);
        m->challenge = get64(c);
        break;
    case EPOCH_MSG_BUSY:
        m->busy_remaining_ms = get32(c);
        break;
    case EPOCH_MSG_INCREMENT:
        m->session = get64(c);
        m->txid = get32(c);
        break;
    case EPOCH_MSG_EP1_REPLY: {
        m->session = get64(c);
        m->txid = get32(c);
        uint8_t s = *c->q++;
        if (s > EPOCH_EP1_REJECTED_STALE) {
            return EPOCH_WIRE_ERR_FIELD;
        }
        m->ep1_status = (enum epoch_ep1_status)s;
        m->counter = get32(c);
        break;
    }
    case EPOCH_MSG_TEST_ARM:
    case EPOCH_MSG_TEST_ARM_ACK:
        m->run = get32(c);
        m->checkpoint = get32(c);
        m->txid = get32(c);
        break;
    case EPOCH_MSG_TEST_DIAG:
        m->run = get32(c);
        break;
    case EPOCH_MSG_TEST_DIAG_REPLY: {
        uint8_t v = *c->q++;
        if (v > 1u) {
            return EPOCH_WIRE_ERR_FIELD;
        }
        m->marker_valid = v != 0;
        m->run = get32(c);
        m->checkpoint = get32(c);
        m->txid = get32(c);
        m->reset_cause = get32(c);
        break;
    }
    default:
        return EPOCH_WIRE_ERR_TYPE;
    }
    return EPOCH_WIRE_OK;
}

size_t epoch_wire_encode(const struct epoch_msg *msg, uint8_t *out, size_t cap)
{
    uint8_t body[EPOCH_WIRE_MAX_BODY];
    size_t flen = msg->type >= EPOCH_MSG_PROBE && msg->type < EPOCH_MSG_TYPE_COUNT ? fields_len(msg->type) : 0;
    if (flen == 0) {
        return 0;
    }
    size_t blen = EPOCH_WIRE_HEADER + flen + 4u;
    struct cursor c = {body, NULL};
    *c.p++ = EPOCH_WIRE_VERSION;
    *c.p++ = (uint8_t)msg->type;
    *c.p++ = (uint8_t)(msg->type == EPOCH_MSG_ANNOUNCE
                           ? (msg->nonce_echo ? EPOCH_WIRE_FLAG_NONCE_ECHO : 0u) | (msg->retention_valid ? EPOCH_WIRE_FLAG_RETENTION : 0u)
                           : 0u);
    *c.p++ = 0;
    put_fields(&c, msg);
    put32(&c, epoch_crc32(body, blen - 4u));

    /* COBS: each code byte gives the distance to the next zero, 0xFF meaning 254 data bytes and no zero. */
    if (cap < blen + blen / 254u + 3u) {
        return 0;
    }
    size_t o = 0;
    out[o++] = 0;
    size_t code_at = o++;
    uint8_t code = 1;
    for (size_t i = 0; i < blen; i++) {
        if (body[i] == 0) {
            out[code_at] = code;
            code_at = o++;
            code = 1;
        } else {
            out[o++] = body[i];
            if (++code == 0xFF) {
                out[code_at] = code;
                code_at = o++;
                code = 1;
            }
        }
    }
    out[code_at] = code;
    out[o++] = 0;
    return o;
}

enum epoch_wire_status epoch_wire_decode_body(const uint8_t *body, size_t len, struct epoch_msg *msg)
{
    if (len < EPOCH_WIRE_HEADER + 4u) {
        return EPOCH_WIRE_ERR_LENGTH;
    }
    struct cursor c = {NULL, body + len - 4u};
    if (get32(&c) != epoch_crc32(body, len - 4u)) {
        return EPOCH_WIRE_ERR_CRC;
    }
    if (body[0] != EPOCH_WIRE_VERSION) {
        return EPOCH_WIRE_ERR_VERSION;
    }
    if (body[1] < EPOCH_MSG_PROBE || body[1] >= EPOCH_MSG_TYPE_COUNT) {
        return EPOCH_WIRE_ERR_TYPE;
    }
    struct epoch_msg m;
    memset(&m, 0, sizeof(m));
    m.type = (enum epoch_msg_type)body[1];
    uint8_t flags = body[2];
    unsigned allowed = m.type == EPOCH_MSG_ANNOUNCE ? EPOCH_WIRE_FLAG_NONCE_ECHO | EPOCH_WIRE_FLAG_RETENTION : 0u;
    if (body[3] != 0 || (flags & ~allowed) != 0) {
        return EPOCH_WIRE_ERR_FIELD;
    }
    if (len != EPOCH_WIRE_HEADER + fields_len(m.type) + 4u) {
        return EPOCH_WIRE_ERR_LENGTH;
    }
    m.nonce_echo = (flags & EPOCH_WIRE_FLAG_NONCE_ECHO) != 0;
    m.retention_valid = (flags & EPOCH_WIRE_FLAG_RETENTION) != 0;
    c.q = body + EPOCH_WIRE_HEADER;
    enum epoch_wire_status s = get_fields(&c, &m);
    if (s != EPOCH_WIRE_OK) {
        return s;
    }
    if (m.type == EPOCH_MSG_ANNOUNCE && !m.nonce_echo && m.nonce != 0) {
        return EPOCH_WIRE_ERR_FIELD; /* an absent echo is encoded as zero */
    }
    *msg = m;
    return EPOCH_WIRE_OK;
}

void epoch_wire_parser_init(struct epoch_wire_parser *p)
{
    p->len = 0;
    p->discarding = 0;
}

static enum epoch_wire_status finish(struct epoch_wire_parser *p, struct epoch_msg *msg)
{
    uint8_t body[MAX_ENCODED];
    size_t n = 0;
    size_t i = 0;
    while (i < p->len) {
        uint8_t code = p->buf[i++];
        if (code == 0 || i + code - 1u > p->len) {
            return EPOCH_WIRE_ERR_COBS;
        }
        for (uint8_t k = 1; k < code; k++) {
            body[n++] = p->buf[i++];
        }
        if (code != 0xFF && i < p->len) {
            body[n++] = 0;
        }
    }
    return epoch_wire_decode_body(body, n, msg);
}

enum epoch_wire_status epoch_wire_feed(struct epoch_wire_parser *p, uint8_t byte, struct epoch_msg *msg)
{
    if (byte != 0) {
        if (!p->discarding) {
            if (p->len < MAX_ENCODED) {
                p->buf[p->len++] = byte;
            } else {
                p->discarding = 1;
            }
        }
        return EPOCH_WIRE_NONE;
    }
    enum epoch_wire_status s = p->discarding ? EPOCH_WIRE_ERR_OVERFLOW : p->len == 0 ? EPOCH_WIRE_NONE : finish(p, msg);
    epoch_wire_parser_init(p);
    return s;
}

const char *epoch_wire_status_name(enum epoch_wire_status s)
{
    static const char *const names[] = {"OK", "NONE", "ERR_COBS", "ERR_LENGTH", "ERR_CRC", "ERR_VERSION", "ERR_TYPE", "ERR_FIELD", "ERR_OVERFLOW"};
    return (unsigned)s < sizeof(names) / sizeof(names[0]) ? names[s] : "?";
}
