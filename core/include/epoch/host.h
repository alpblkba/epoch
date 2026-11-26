#ifndef EPOCH_HOST_H
#define EPOCH_HOST_H

#include <stdbool.h>
#include <stdint.h>

#include "epoch/journal.h"
#include "epoch/types.h"

/* Host session state machine with one outstanding non-idempotent INCREMENT. Policy B2 reports a lost reply as UNKNOWN unless a matching COMMITTED reply arrives later; B3 additionally classifies it from the journal snapshot the controller reports in ANNOUNCE, checked at START_ACK. */

enum epoch_host_state { EPOCH_HOST_DOWN, EPOCH_HOST_PROBING, EPOCH_HOST_BINDING, EPOCH_HOST_STARTING, EPOCH_HOST_RUNNING, EPOCH_HOST_VERIFYING };
enum epoch_host_policy { EPOCH_POLICY_B2, EPOCH_POLICY_B3 };

struct epoch_host_config {
    struct epoch_journal_params journal;
    enum epoch_host_policy policy;
    uint32_t version;
    uint32_t handshake_timeout_ms; /* timing values are chosen by the caller; the protocol does not fix them yet */
    uint32_t request_timeout_ms;
    uint32_t ping_timeout_ms;
    uint32_t busy_jitter_ms;       /* jitter is drawn from [0, busy_jitter_ms] */
};

struct epoch_host_env {
    void *ctx;
    uint64_t (*rand64)(void *ctx);
    void (*send)(void *ctx, const struct epoch_msg *m);
    /* Called once per final outcome, and once more if a reported UNKNOWN is later refined by a matched reply or a B3 snapshot. */
    void (*outcome)(void *ctx, uint64_t session, uint32_t txid, enum epoch_outcome o, uint32_t counter);
};

struct epoch_host {
    const struct epoch_host_config *cfg;
    const struct epoch_host_env *env;
    enum epoch_host_state state;
    uint64_t nonce, challenge, session, boot_id;
    bool retention_valid;
    struct epoch_snapshot snapshot;
    bool timer;
    uint32_t deadline;
    unsigned retries;
    uint32_t next_txid;
    uint32_t ping_seq;
    /* The latest INCREMENT: in flight while req_outcome is NONE, refinable once while it is UNKNOWN. */
    bool req;
    uint64_t req_session;
    uint32_t req_txid;
    enum epoch_outcome req_outcome;
};

void epoch_host_init(struct epoch_host *h, const struct epoch_host_config *cfg, const struct epoch_host_env *env);
void epoch_host_start(struct epoch_host *h, uint32_t now);
void epoch_host_receive(struct epoch_host *h, const struct epoch_msg *m, uint32_t now);
void epoch_host_tick(struct epoch_host *h, uint32_t now);
/* Sends INCREMENT and returns its txid, or returns 0 when the host is not RUNNING or a request is in flight. */
uint32_t epoch_host_increment(struct epoch_host *h, uint32_t now);

#endif
