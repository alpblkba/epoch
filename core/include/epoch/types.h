#ifndef EPOCH_TYPES_H
#define EPOCH_TYPES_H

#include <stdbool.h>
#include <stdint.h>

/* Logical messages exchanged by host and controller. The values of enum epoch_msg_type, enum epoch_ep1_status and enum epoch_checkpoint are also the wire codes written by epoch/wire.h, so changing them changes the provisional wire format. The struct layout is in-memory only; epoch/wire.h defines the byte layout. */

#define EPOCH_JOURNAL_WORDS 8u
#define EPOCH_NO_SESSION_INTERVAL_MS 50u /* the controller sends NO_SESSION at most once per 50 ms */
#define EPOCH_HOST_RETRANSMIT_LIMIT 3u   /* the host retransmits a handshake message at most 3 times */

enum epoch_endpoint { EPOCH_EP0, EPOCH_EP1, EPOCH_EP2 };

enum epoch_msg_type {
    EPOCH_MSG_PROBE = 1,
    EPOCH_MSG_ANNOUNCE,
    EPOCH_MSG_HELLO,
    EPOCH_MSG_HELLO_ACK,
    EPOCH_MSG_START,
    EPOCH_MSG_START_ACK,
    EPOCH_MSG_PING,
    EPOCH_MSG_PONG,
    EPOCH_MSG_NO_SESSION,
    EPOCH_MSG_BUSY,
    EPOCH_MSG_INCREMENT, /* EP1 request (S, txid) */
    EPOCH_MSG_EP1_REPLY, /* EP1 response (S, txid): committed counter or REJECTED_STALE */
    EPOCH_MSG_TEST_ARM,  /* fault-test builds only: arm a reset at (checkpoint, txid) for run; controllers ignore it */
    EPOCH_MSG_TEST_ARM_ACK,
    EPOCH_MSG_TEST_DIAG, /* fault-test builds only: report the checkpoint marker and the boot reset cause */
    EPOCH_MSG_TEST_DIAG_REPLY,
    EPOCH_MSG_TYPE_COUNT
};

enum epoch_ep1_status { EPOCH_EP1_COMMITTED, EPOCH_EP1_REJECTED_STALE };

enum epoch_outcome {
    EPOCH_OUTCOME_NONE,
    EPOCH_OUTCOME_COMPLETED,
    EPOCH_OUTCOME_REJECTED,
    EPOCH_OUTCOME_NOT_EXECUTED,
    EPOCH_OUTCOME_UNKNOWN
};

/* Named reset points for fault injection: F1 before the journal write, F2a, F2b and F2c inside the record write (EPOCH_STORE_F2A, F2B, F2C in epoch/journal.h), F3 and F4 after the commit and before the reply (the core checks both at the same point). Resets during a partially transferred frame act below the logical message level and are not represented here. */
enum epoch_checkpoint { EPOCH_CP_F1, EPOCH_CP_F2A, EPOCH_CP_F2B, EPOCH_CP_F2C, EPOCH_CP_F3, EPOCH_CP_F4 };

/* Raw journal slots and era captured at boot and carried in ANNOUNCE, so the host can validate them with the same rules as the controller. */
struct epoch_snapshot {
    uint32_t slots[2 * EPOCH_JOURNAL_WORDS]; /* slot i starts at word 8 * i */
    uint32_t era;
};

struct epoch_msg {
    enum epoch_msg_type type;
    uint64_t boot_id;          /* B: ANNOUNCE, NO_SESSION */
    uint64_t challenge;        /* U in ANNOUNCE and NO_SESSION, C in HELLO and HELLO_ACK */
    uint64_t nonce;            /* H: PROBE, HELLO, HELLO_ACK, ANNOUNCE echo */
    bool nonce_echo;           /* ANNOUNCE: nonce holds an echoed H */
    uint64_t session;          /* S: HELLO_ACK, START, START_ACK, PING, PONG, EP1 */
    uint32_t version;          /* HELLO: chosen version */
    uint32_t version_min;      /* ANNOUNCE */
    uint32_t version_max;      /* ANNOUNCE */
    uint32_t capabilities;     /* ANNOUNCE, no capability bits defined */
    uint32_t reset_cause;      /* ANNOUNCE, opaque platform flags */
    bool retention_valid;      /* ANNOUNCE */
    struct epoch_snapshot snapshot; /* ANNOUNCE */
    uint32_t counter;          /* PING and PONG counter; EP1 reply committed counter */
    uint32_t txid;             /* EP1 */
    uint32_t busy_remaining_ms; /* BUSY */
    enum epoch_ep1_status ep1_status; /* EP1 reply */
    uint32_t run;              /* TEST_*: harness trial identifier */
    uint32_t checkpoint;       /* TEST_ARM, TEST_ARM_ACK, TEST_DIAG_REPLY: enum epoch_checkpoint */
    bool marker_valid;         /* TEST_DIAG_REPLY; txid and reset_cause carry the marker txid and the boot reset cause */
};

enum epoch_endpoint epoch_msg_endpoint(enum epoch_msg_type type);
const char *epoch_msg_name(enum epoch_msg_type type);
const char *epoch_outcome_name(enum epoch_outcome outcome);

#endif
