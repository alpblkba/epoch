#ifndef EPOCH_CONTROLLER_H
#define EPOCH_CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>

#include "epoch/journal.h"
#include "epoch/types.h"

/* Controller session state machine, the product of three components: an active session (A), a pending session awaiting START (P) and the current challenge (U), plus the EP1 INCREMENT service with its fence. Times are milliseconds on a free-running uint32_t clock. */

struct epoch_retained {
    volatile uint32_t slots[2 * EPOCH_JOURNAL_WORDS];
    volatile uint32_t era; /* 0 means unset */
};

struct epoch_ctrl_config {
    struct epoch_journal_params journal;
    uint32_t t_pending_ms; /* lifetime of a pending session; provisional, chosen by the caller */
    uint32_t version_min, version_max, capabilities;
};

struct epoch_ctrl_env {
    void *ctx;
    uint64_t (*rand64)(void *ctx);   /* hardware RNG on the board */
    uint32_t (*new_era)(void *ctx);  /* era source; must not return 0 */
    void (*send)(void *ctx, const struct epoch_msg *m);
    void (*barrier)(void *ctx);      /* DSB on the board */
    bool (*checkpoint)(void *ctx, enum epoch_checkpoint cp, uint32_t txid); /* may be NULL; true models a reset */
    epoch_store_hook after_store;    /* may be NULL; sees every journal store */
};

enum epoch_ctrl_result { EPOCH_CTRL_OK, EPOCH_CTRL_RESET };

struct epoch_ctrl {
    const struct epoch_ctrl_config *cfg;
    const struct epoch_ctrl_env *env;
    struct epoch_retained *ret;
    struct epoch_snapshot snapshot;
    bool retention_valid;
    uint32_t reset_cause;
    uint64_t boot_id;
    bool active;            /* A */
    uint64_t session;
    bool pending;           /* P */
    uint64_t p_challenge, p_nonce, p_session;
    uint32_t p_deadline;
    bool u_present, u_owned; /* U */
    uint64_t u, u_owner;
    uint32_t fence;
    uint32_t counter;
    bool writing_increment;
    uint32_t write_txid;
    bool no_session_sent;
    uint32_t no_session_at;
};

/* cold is the platform's cold-start verdict (power-on or brown-out flags, or a cleared backup register). An era of 0 also forces cold initialization. */
/* Returns EPOCH_CTRL_RESET when a store hook models a reset inside cold initialization; nothing is sent then. */
enum epoch_ctrl_result epoch_ctrl_boot(struct epoch_ctrl *c, const struct epoch_ctrl_config *cfg, const struct epoch_ctrl_env *env, struct epoch_retained *ret, bool cold, uint32_t reset_cause, uint32_t now);
enum epoch_ctrl_result epoch_ctrl_receive(struct epoch_ctrl *c, const struct epoch_msg *m, uint32_t now);
void epoch_ctrl_tick(struct epoch_ctrl *c, uint32_t now);

#endif
