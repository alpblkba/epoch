#ifndef EPOCH_SIM_H
#define EPOCH_SIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "epoch/controller.h"
#include "epoch/host.h"

/* Deterministic simulator: one controller and one host exchanging logical messages over two lossy links on a 1 ms virtual clock. Messages are passed as struct epoch_msg values and never encoded; the wire codec is tested separately in tests/wire. */

/* TEST ONLY journal MAGIC|VERSION and record checksum. They are not the firmware's values and are not protocol constants. */
#define EPOCH_TEST_MAGIC_VERSION 0x5A5A5A01u
uint32_t epoch_test_checksum(const uint32_t *words, size_t n);
extern const struct epoch_ctrl_config epoch_test_ctrl_config;
struct epoch_host_config epoch_test_host_config(enum epoch_host_policy policy);

#define EPOCH_SIM_TRACKED 16u

struct epoch_sim_config {
    uint64_t seed;
    enum epoch_host_policy policy;
    uint32_t duration_ms;
    uint32_t request_interval_ms;
    uint32_t delay_min_ms, delay_max_ms;
    bool reorder;                  /* false keeps each link in send order */
    uint32_t drop_permille, dup_permille;
    uint32_t reset_permille;       /* per journal store and per named checkpoint */
    uint32_t cold_permille;        /* share of random resets that are cold */
    uint32_t cold_keeps_permille;  /* share of cold resets that leave every retained word intact, as SRAM1 (R1) may across a brown-out; the others clear the era and scramble the slots */
    uint32_t corrupt_permille;     /* share of warm resets that flip one bit of a slot whose w0 is not 0, modelling silent retained-memory corruption outside the proposal's failure model */
    uint32_t faults_until_ms;      /* drops, duplicates and random resets stop here */
    bool script;                   /* one scripted reset at a named checkpoint */
    enum epoch_checkpoint script_cp;
    uint32_t script_txid;
    bool script_cold;
    uint32_t host_restart_at_ms;   /* 0 means never */
    FILE *trace;                   /* NULL for no trace */
};

struct epoch_sim_result {
    uint64_t hash;
    uint32_t issued, completed, rejected, not_executed, unknown, refined, abandoned;
    uint32_t false_completed, false_not_executed, duplicate_commits;
    uint32_t rejected_but_committed; /* REJECTED_STALE answering a duplicate of a committed request */
    uint32_t resets, cold_resets, queue_overflow, table_overflow;
    uint32_t boot_resets;            /* resets that landed inside cold initialization during boot */
    uint32_t boot_resets_newest[2];  /* of those, the ones that began over a valid journal, by the slot holding its newest record */
    uint32_t corruptions, corruptions_newest; /* flipped bits, and those that hit the newest valid record */
    uint32_t completed_after_faults;
    bool running_at_end;
    enum epoch_outcome first_outcome[EPOCH_SIM_TRACKED]; /* by txid, in the first session opened */
    uint32_t first_counter[EPOCH_SIM_TRACKED];
};

void epoch_sim_default(struct epoch_sim_config *cfg, uint64_t seed);
void epoch_sim_run(const struct epoch_sim_config *cfg, struct epoch_sim_result *res);
/* True when the run broke a safety property (a false outcome, a duplicate commit, a committed request reported REJECTED) or overflowed a table or link queue, so it did not model the configured scenario. */
bool epoch_sim_violation(const struct epoch_sim_result *r);

#endif
