#ifndef EPOCH_WIRE_H
#define EPOCH_WIRE_H

#include <stddef.h>
#include <stdint.h>

#include "epoch/types.h"

/* UART wire format. Every value in this header and the field layouts in wire.c are provisional and may change before the format is declared stable. A frame is
 *   0x00, COBS(body), 0x00
 * where body = version(1) type(1) flags(1) reserved(1) fields... crc32(4). All integers are little-endian and the CRC is CRC-32/ISO-HDLC over every body byte before it. The leading delimiter ends any partial frame a reset left on the wire. */

#define EPOCH_WIRE_VERSION 1u
#define EPOCH_WIRE_HEADER 4u
#define EPOCH_WIRE_MAX_BODY 128u                                    /* ANNOUNCE, the largest message, is 116 bytes */
#define EPOCH_WIRE_MAX_FRAME (EPOCH_WIRE_MAX_BODY + 1u + 2u)        /* COBS adds 1 byte per 254, plus two delimiters */

#define EPOCH_WIRE_FLAG_NONCE_ECHO 0x01u  /* ANNOUNCE: nonce holds an echoed H */
#define EPOCH_WIRE_FLAG_RETENTION 0x02u   /* ANNOUNCE: retention_valid */

enum epoch_wire_status {
    EPOCH_WIRE_OK,
    EPOCH_WIRE_NONE,         /* parser: byte consumed, no frame finished */
    EPOCH_WIRE_ERR_COBS,     /* a code byte points past the frame end */
    EPOCH_WIRE_ERR_LENGTH,   /* body shorter than the header and CRC, or not the length the type requires */
    EPOCH_WIRE_ERR_CRC,
    EPOCH_WIRE_ERR_VERSION,
    EPOCH_WIRE_ERR_TYPE,
    EPOCH_WIRE_ERR_FIELD,    /* reserved byte not zero, unknown flag, flag on the wrong type, or status out of range */
    EPOCH_WIRE_ERR_OVERFLOW  /* longer than any valid encoded body without a delimiter; the rest up to the next 0x00 was discarded */
};

/* Encodes msg into out as a complete frame with both delimiters. Returns the frame length, or 0 if msg->type is invalid or cap is too small. */
size_t epoch_wire_encode(const struct epoch_msg *msg, uint8_t *out, size_t cap);

/* Decodes one COBS-decoded body (no delimiters). Fills *msg only on EPOCH_WIRE_OK; fields a type does not carry are zero. */
enum epoch_wire_status epoch_wire_decode_body(const uint8_t *body, size_t len, struct epoch_msg *msg);

/* Streaming parser: any byte chunking, resynchronizes on every 0x00, never stores more than EPOCH_WIRE_MAX_FRAME bytes. */
struct epoch_wire_parser {
    uint8_t buf[EPOCH_WIRE_MAX_FRAME];
    size_t len;
    int discarding; /* set after an overflow until the next delimiter */
};

void epoch_wire_parser_init(struct epoch_wire_parser *p);

/* Feeds one byte. Returns EPOCH_WIRE_NONE while a frame is incomplete or for empty frames, EPOCH_WIRE_OK with *msg filled when a valid frame ends, or an error for a frame that ended invalid; after any result other than NONE the parser is empty again. */
enum epoch_wire_status epoch_wire_feed(struct epoch_wire_parser *p, uint8_t byte, struct epoch_msg *msg);

const char *epoch_wire_status_name(enum epoch_wire_status s);

#endif
