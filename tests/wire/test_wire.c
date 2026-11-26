#include <string.h>

#include "epoch/crc32.h"
#include "epoch/wire.h"
#include "test.h"

static struct epoch_msg msg(enum epoch_msg_type t)
{
    struct epoch_msg m;
    memset(&m, 0, sizeof(m));
    m.type = t;
    return m;
}

/* Feeds a byte string and returns the last non-NONE status, or NONE; *count gets the number of OK frames. */
static enum epoch_wire_status feed(struct epoch_wire_parser *p, const uint8_t *b, size_t n, struct epoch_msg *out, int *count)
{
    enum epoch_wire_status last = EPOCH_WIRE_NONE;
    for (size_t i = 0; i < n; i++) {
        enum epoch_wire_status s = epoch_wire_feed(p, b[i], out);
        if (s != EPOCH_WIRE_NONE) {
            last = s;
            *count += s == EPOCH_WIRE_OK;
        }
    }
    return last;
}

static int same(const struct epoch_msg *a, const struct epoch_msg *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

static void roundtrip_all_types(void)
{
    for (int t = EPOCH_MSG_PROBE; t < EPOCH_MSG_TYPE_COUNT; t++) {
        struct epoch_msg m = msg((enum epoch_msg_type)t), d;
        /* every field a type carries gets a value with zero bytes and 0xFF bytes in it */
        m.boot_id = 0x00FF00FF12345678u;
        m.challenge = 0x8000000000000001u;
        m.nonce = 0x0000000000000100u;
        m.session = 0xFFFFFFFF00000000u;
        m.version = m.version_min = m.version_max = 1;
        m.capabilities = 0;
        m.reset_cause = 0x14004400u;
        m.counter = 0x00010000u;
        m.txid = 7;
        m.busy_remaining_ms = 250;
        m.ep1_status = EPOCH_EP1_REJECTED_STALE;
        m.run = 0x00FF0001u;
        m.checkpoint = EPOCH_CP_F3;
        m.marker_valid = t == EPOCH_MSG_TEST_DIAG_REPLY;
        for (unsigned i = 0; i < 2 * EPOCH_JOURNAL_WORDS; i++) {
            m.snapshot.slots[i] = i * 0x01010101u;
        }
        m.snapshot.era = 0xFFFFFFFFu;
        m.nonce_echo = m.retention_valid = t == EPOCH_MSG_ANNOUNCE;
        uint8_t f[EPOCH_WIRE_MAX_FRAME];
        size_t n = epoch_wire_encode(&m, f, sizeof(f));
        CHECK(n > 2 && n <= EPOCH_WIRE_MAX_FRAME && f[0] == 0 && f[n - 1] == 0);
        for (size_t i = 1; i + 1 < n; i++) {
            CHECK(f[i] != 0); /* COBS leaves no zero inside the frame */
        }
        struct epoch_wire_parser p;
        epoch_wire_parser_init(&p);
        int ok = 0;
        CHECK(feed(&p, f, n, &d, &ok) == EPOCH_WIRE_OK && ok == 1);
        /* the decoder zeroes fields the type does not carry, so compare against the re-decoded form */
        struct epoch_msg d2;
        epoch_wire_parser_init(&p);
        ok = 0;
        uint8_t f2[EPOCH_WIRE_MAX_FRAME];
        size_t n2 = epoch_wire_encode(&d, f2, sizeof(f2));
        CHECK(n2 == n && memcmp(f, f2, n) == 0);
        CHECK(feed(&p, f2, n2, &d2, &ok) == EPOCH_WIRE_OK && same(&d, &d2));
        CHECK(d.type == m.type);
    }
    struct epoch_msg a = msg(EPOCH_MSG_ANNOUNCE);
    uint8_t f[EPOCH_WIRE_MAX_FRAME];
    CHECK(epoch_wire_encode(&a, f, sizeof(f)) == 4 + 108 + 4 + 3); /* largest body 116, COBS +1, delimiters +2 */
    CHECK(epoch_wire_encode(&a, f, 50) == 0);
    struct epoch_msg bad = msg(EPOCH_MSG_TYPE_COUNT);
    CHECK(epoch_wire_encode(&bad, f, sizeof(f)) == 0);
}

/* Golden frames, also produced by the independent Python reference in host/client/epoch_wire.py. */
static void golden(void)
{
    struct epoch_msg m = msg(EPOCH_MSG_PING);
    m.session = 0x0102030405060708u;
    m.counter = 0;
    static const uint8_t ping[] = {0x00, 0x03, 0x01, 0x07, 0x01, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03,
                                   0x02, 0x01, 0x01, 0x01, 0x01, 0x05, 0x77, 0x1F, 0x66, 0x74, 0x00};
    uint8_t f[EPOCH_WIRE_MAX_FRAME];
    size_t n = epoch_wire_encode(&m, f, sizeof(f));
    CHECK(n == sizeof(ping) && memcmp(f, ping, n) == 0);
    if (n != sizeof(ping) || memcmp(f, ping, n) != 0) {
        for (size_t i = 0; i < n; i++) {
            fprintf(stderr, "%02X ", f[i]);
        }
        fprintf(stderr, "\n");
    }
    struct epoch_msg r = msg(EPOCH_MSG_EP1_REPLY);
    r.session = 0xFFFFFFFF00000000u;
    r.txid = 7;
    r.ep1_status = EPOCH_EP1_COMMITTED;
    r.counter = 0x10000u;
    static const uint8_t reply[] = {0x00, 0x03, 0x01, 0x0C, 0x01, 0x01, 0x01, 0x01, 0x01, 0x06, 0xFF, 0xFF, 0xFF, 0xFF,
                                    0x07, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02, 0x01, 0x05, 0x0B, 0x9E, 0x19, 0x80, 0x00};
    n = epoch_wire_encode(&r, f, sizeof(f));
    CHECK(n == sizeof(reply) && memcmp(f, reply, n) == 0);
    struct epoch_msg t = msg(EPOCH_MSG_TEST_ARM), d;
    t.run = 0x01020304u;
    t.checkpoint = EPOCH_CP_F3;
    t.txid = 7;
    static const uint8_t arm[] = {0x00, 0x03, 0x01, 0x0D, 0x01, 0x06, 0x04, 0x03, 0x02, 0x01, 0x04, 0x01,
                                  0x01, 0x02, 0x07, 0x01, 0x01, 0x05, 0x8B, 0x07, 0x2A, 0x2E, 0x00};
    n = epoch_wire_encode(&t, f, sizeof(f));
    CHECK(n == sizeof(arm) && memcmp(f, arm, n) == 0);
    CHECK(epoch_wire_decode_body((const uint8_t[]){1, EPOCH_MSG_TEST_ARM, 0, 0, 4, 3, 2, 1, 4, 0, 0, 0, 7, 0, 0, 0, 0x8B, 0x07, 0x2A, 0x2E}, 20, &d) ==
          EPOCH_WIRE_OK);
    CHECK(d.run == t.run && d.checkpoint == t.checkpoint && d.txid == 7);
}

static void errors(void)
{
    struct epoch_wire_parser p;
    struct epoch_msg m = msg(EPOCH_MSG_PONG), d;
    m.session = 42;
    m.counter = 9;
    uint8_t f[EPOCH_WIRE_MAX_FRAME], g[EPOCH_WIRE_MAX_FRAME];
    size_t n = epoch_wire_encode(&m, f, sizeof(f));
    int ok;

    /* corruption of every inner byte is rejected, and the next valid frame still decodes */
    for (size_t i = 1; i + 1 < n; i++) {
        memcpy(g, f, n);
        g[i] = (uint8_t)(g[i] ^ 0x5A) ? (uint8_t)(g[i] ^ 0x5A) : 0x01;
        epoch_wire_parser_init(&p);
        ok = 0;
        CHECK(feed(&p, g, n, &d, &ok) != EPOCH_WIRE_OK);
        CHECK(feed(&p, f, n, &d, &ok) == EPOCH_WIRE_OK && ok == 1 && d.counter == 9);
    }
    /* truncation at every length: the leading delimiter of the next frame ends it, the following frame decodes */
    for (size_t cut = 2; cut + 1 < n; cut++) {
        epoch_wire_parser_init(&p);
        ok = 0;
        CHECK(feed(&p, f, cut, &d, &ok) == EPOCH_WIRE_NONE);
        CHECK(feed(&p, f, n, &d, &ok) == EPOCH_WIRE_OK && ok == 1);
    }
    /* byte at a time, three concatenated frames, empty frames ignored */
    epoch_wire_parser_init(&p);
    ok = 0;
    for (int r = 0; r < 3; r++) {
        feed(&p, f, n, &d, &ok);
    }
    CHECK(ok == 3);
    /* oversized garbage without a delimiter is discarded with one overflow report, then a frame decodes */
    uint8_t junk[400];
    memset(junk, 0x41, sizeof(junk));
    epoch_wire_parser_init(&p);
    ok = 0;
    CHECK(feed(&p, junk, sizeof(junk), &d, &ok) == EPOCH_WIRE_NONE);
    CHECK(feed(&p, f, 1, &d, &ok) == EPOCH_WIRE_ERR_OVERFLOW);
    CHECK(feed(&p, f, n, &d, &ok) == EPOCH_WIRE_OK && ok == 1);
    /* invalid COBS: a code byte pointing past the end */
    const uint8_t cobs[] = {0, 0x09, 1, 2, 0};
    epoch_wire_parser_init(&p);
    ok = 0;
    CHECK(feed(&p, cobs, sizeof(cobs), &d, &ok) == EPOCH_WIRE_ERR_COBS);

    /* body-level checks with a valid CRC */
    uint8_t body[EPOCH_WIRE_MAX_BODY];
    const uint8_t hdr[] = {1, EPOCH_MSG_PONG, 0, 0};
    size_t blen = 4 + 12 + 4;
    memset(body, 0, sizeof(body));
    memcpy(body, hdr, 4);
#define SEAL(len)                                                     \
    do {                                                              \
        uint32_t c = epoch_crc32(body, (len) - 4);                    \
        for (size_t k = 0; k < 4; k++) body[(len) - 4 + k] = (uint8_t)(c >> (8 * k)); \
    } while (0)
    SEAL(blen);
    CHECK(epoch_wire_decode_body(body, blen, &d) == EPOCH_WIRE_OK);
    body[0] = 2;
    SEAL(blen);
    CHECK(epoch_wire_decode_body(body, blen, &d) == EPOCH_WIRE_ERR_VERSION);
    body[0] = 1;
    body[1] = 0;
    SEAL(blen);
    CHECK(epoch_wire_decode_body(body, blen, &d) == EPOCH_WIRE_ERR_TYPE);
    body[1] = EPOCH_MSG_TYPE_COUNT;
    SEAL(blen);
    CHECK(epoch_wire_decode_body(body, blen, &d) == EPOCH_WIRE_ERR_TYPE);
    body[1] = EPOCH_MSG_PONG;
    body[3] = 1;
    SEAL(blen);
    CHECK(epoch_wire_decode_body(body, blen, &d) == EPOCH_WIRE_ERR_FIELD);
    body[3] = 0;
    body[2] = EPOCH_WIRE_FLAG_RETENTION; /* flag on a type that has none */
    SEAL(blen);
    CHECK(epoch_wire_decode_body(body, blen, &d) == EPOCH_WIRE_ERR_FIELD);
    body[2] = 0;
    SEAL(blen + 1);
    CHECK(epoch_wire_decode_body(body, blen + 1, &d) == EPOCH_WIRE_ERR_LENGTH);
    CHECK(epoch_wire_decode_body(body, 7, &d) == EPOCH_WIRE_ERR_LENGTH);
    SEAL(blen);
    body[5] ^= 1;
    CHECK(epoch_wire_decode_body(body, blen, &d) == EPOCH_WIRE_ERR_CRC);
    /* EP1 status out of range */
    size_t rlen = 4 + 17 + 4;
    memset(body, 0, sizeof(body));
    body[0] = 1;
    body[1] = EPOCH_MSG_EP1_REPLY;
    body[4 + 12] = 2;
    SEAL(rlen);
    CHECK(epoch_wire_decode_body(body, rlen, &d) == EPOCH_WIRE_ERR_FIELD);
    /* TEST_DIAG_REPLY marker flag out of range */
    size_t dlen = 4 + 17 + 4;
    memset(body, 0, sizeof(body));
    body[0] = 1;
    body[1] = EPOCH_MSG_TEST_DIAG_REPLY;
    body[4] = 2;
    SEAL(dlen);
    CHECK(epoch_wire_decode_body(body, dlen, &d) == EPOCH_WIRE_ERR_FIELD);
    /* ANNOUNCE with a nonce but no echo flag */
    struct epoch_msg a = msg(EPOCH_MSG_ANNOUNCE);
    a.nonce = 5;
    a.nonce_echo = 1;
    n = epoch_wire_encode(&a, f, sizeof(f));
    epoch_wire_parser_init(&p);
    ok = 0;
    CHECK(feed(&p, f, n, &d, &ok) == EPOCH_WIRE_OK && d.nonce == 5 && d.nonce_echo);
}

/* Deterministic random input: the parser never reads or writes out of bounds (checked under ASan/UBSan) and never accepts a frame whose CRC is wrong. */
static void fuzz(void)
{
    uint32_t x = 0x12345678u;
    struct epoch_wire_parser p;
    struct epoch_msg d;
    epoch_wire_parser_init(&p);
    int ok = 0;
    for (int i = 0; i < 2000000; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        uint8_t b = (x & 0x1F) == 0 ? 0 : (uint8_t)(x >> 8);
        enum epoch_wire_status s = epoch_wire_feed(&p, b, &d);
        ok += s == EPOCH_WIRE_OK;
    }
    CHECK(ok == 0); /* a random frame passing CRC, length and field checks would be a 2^-32 event */
}

int main(void)
{
    CHECK(strcmp(epoch_wire_status_name(EPOCH_WIRE_ERR_CRC), "ERR_CRC") == 0);
    roundtrip_all_types();
    golden();
    errors();
    fuzz();
    return TEST_DONE("wire");
}
