#include <inttypes.h>
#include <stdio.h>

#include "sim.h"
#include "test.h"

#define CAMPAIGN_SEEDS 300u

static void quiet(struct epoch_sim_config *c, uint64_t seed)
{
    epoch_sim_default(c, seed);
    c->drop_permille = 0;
    c->dup_permille = 0;
    c->reset_permille = 0;
    c->duration_ms = 3000;
    c->faults_until_ms = 0; /* the whole run counts as the fault-free phase */
}

static void check_sound(const struct epoch_sim_result *r)
{
    CHECK(r->false_completed == 0u);
    CHECK(r->false_not_executed == 0u);
    CHECK(r->duplicate_commits == 0u);
    CHECK(r->rejected_but_committed == 0u); /* a committed request must never be reported REJECTED */
    CHECK(r->table_overflow == 0u);
    CHECK(r->queue_overflow == 0u);
}

/* epoch_sim exits nonzero on exactly these conditions; it once ignored rejected_but_committed and queue_overflow. */
static void test_violation_oracle(void)
{
    struct epoch_sim_result r = { 0 };
    CHECK(!epoch_sim_violation(&r));
    uint32_t *fields[] = { &r.false_completed, &r.false_not_executed, &r.duplicate_commits, &r.rejected_but_committed, &r.table_overflow, &r.queue_overflow };
    for (unsigned i = 0; i < sizeof fields / sizeof fields[0]; i++) {
        *fields[i] = 1u;
        CHECK(epoch_sim_violation(&r));
        *fields[i] = 0u;
    }
}

static void test_scenario_a_normal(void)
{
    struct epoch_sim_config c;
    struct epoch_sim_result r;
    quiet(&c, 1);
    epoch_sim_run(&c, &r);
    check_sound(&r);
    CHECK(r.issued > 50u && r.completed + r.abandoned == r.issued && r.abandoned <= 1u);
    CHECK(r.unknown == 0u && r.rejected == 0u && r.resets == 0u && r.running_at_end);
    for (uint32_t t = 1; t < EPOCH_SIM_TRACKED; t++)
        CHECK(r.first_outcome[t] == EPOCH_OUTCOME_COMPLETED && r.first_counter[t] == t);
}

static struct epoch_sim_result scripted(enum epoch_checkpoint cp, bool cold, enum epoch_host_policy policy)
{
    struct epoch_sim_config c;
    struct epoch_sim_result r;
    quiet(&c, 2);
    c.policy = policy;
    c.script = true;
    c.script_cp = cp;
    c.script_txid = 3;
    c.script_cold = cold;
    epoch_sim_run(&c, &r);
    check_sound(&r);
    CHECK(r.resets == 1u && r.running_at_end && r.completed_after_faults > 0u);
    CHECK(r.first_outcome[2] == EPOCH_OUTCOME_COMPLETED && r.first_counter[2] == 2u);
    return r;
}

static void test_scenario_b_not_executed(void)
{
    const enum epoch_checkpoint before_commit[] = { EPOCH_CP_F1, EPOCH_CP_F2A, EPOCH_CP_F2B, EPOCH_CP_F2C };
    for (unsigned i = 0; i < sizeof before_commit / sizeof before_commit[0]; i++) {
        struct epoch_sim_result r = scripted(before_commit[i], false, EPOCH_POLICY_B3);
        CHECK(r.first_outcome[3] == EPOCH_OUTCOME_NOT_EXECUTED);
        r = scripted(before_commit[i], false, EPOCH_POLICY_B2);
        CHECK(r.first_outcome[3] == EPOCH_OUTCOME_UNKNOWN);
    }
}

static void test_scenario_c_lost_reply(void)
{
    struct epoch_sim_result r = scripted(EPOCH_CP_F3, false, EPOCH_POLICY_B2);
    CHECK(r.first_outcome[3] == EPOCH_OUTCOME_UNKNOWN);
    r = scripted(EPOCH_CP_F3, true, EPOCH_POLICY_B3); /* retention lost */
    CHECK(r.first_outcome[3] == EPOCH_OUTCOME_UNKNOWN);
    r = scripted(EPOCH_CP_F3, false, EPOCH_POLICY_B3);
    CHECK(r.first_outcome[3] == EPOCH_OUTCOME_COMPLETED && r.first_counter[3] == 3u);
    r = scripted(EPOCH_CP_F4, false, EPOCH_POLICY_B3);
    CHECK(r.first_outcome[3] == EPOCH_OUTCOME_COMPLETED && r.first_counter[3] == 3u);
}

/* Every named checkpoint, warm and cold, under both policies: exactly one scripted reset lands, the run stays sound and live, and the cut request ends as the policy and the retained journal allow. */
static void test_scripted_matrix(void)
{
    unsigned cells = 0, decided = 0;
    for (unsigned p = 0; p < 2; p++) {
        enum epoch_host_policy policy = p ? EPOCH_POLICY_B3 : EPOCH_POLICY_B2;
        for (unsigned cp = EPOCH_CP_F1; cp <= EPOCH_CP_F4; cp++) {
            for (unsigned cold = 0; cold <= 1; cold++) {
                struct epoch_sim_result r = scripted((enum epoch_checkpoint)cp, cold, policy);
                enum epoch_outcome want = policy == EPOCH_POLICY_B3 && !cold ? (cp >= EPOCH_CP_F3 ? EPOCH_OUTCOME_COMPLETED : EPOCH_OUTCOME_NOT_EXECUTED) : EPOCH_OUTCOME_UNKNOWN;
                CHECK(r.first_outcome[3] == want && r.cold_resets == cold);
                if (want == EPOCH_OUTCOME_COMPLETED)
                    CHECK(r.first_counter[3] == 3u);
                cells++;
                decided += want != EPOCH_OUTCOME_UNKNOWN;
            }
        }
    }
    printf("scripted matrix: %u cells, %u decided, each with one reset\n", cells, decided);
    CHECK(cells == 24u && decided == 6u);
}

static void test_host_restart(void)
{
    struct epoch_sim_config c;
    struct epoch_sim_result r;
    quiet(&c, 3);
    c.host_restart_at_ms = 1013;
    epoch_sim_run(&c, &r);
    check_sound(&r);
    CHECK(r.resets == 0u && r.running_at_end && r.completed_after_faults > 0u && r.abandoned <= 2u);
}

static void test_determinism(void)
{
    struct epoch_sim_config c;
    struct epoch_sim_result a, b;
    epoch_sim_default(&c, 77);
    epoch_sim_run(&c, &a);
    epoch_sim_run(&c, &b);
    CHECK(a.hash == b.hash && a.issued == b.issued && a.resets == b.resets);
    c.seed = 78;
    epoch_sim_run(&c, &b);
    CHECK(a.hash != b.hash);
}

/* Randomized property campaign: drops, duplicates, delay, reorder, warm and cold resets at any journal store and named checkpoint, then a fault-free phase that checks conditional liveness (P6): once faults stop, a session is established and new requests complete. */
/* cold_keeps_permille > 0 adds the R1 cold model, in which a cold reset may leave the old era and slots intact; with a raised reset rate, resets also land inside cold initialization during boot. */
/* corrupt_permille > 0 flips one bit of a committed slot at some warm resets; detected corruption must cost only UNKNOWN outcomes and one cold start, never a false outcome or lost liveness. */
static void campaign(enum epoch_host_policy policy, bool reorder, uint32_t host_restart_at, uint32_t cold_keeps_permille, uint32_t corrupt_permille)
{
    struct epoch_sim_result sum = { 0 };
    unsigned live = 0;
    for (uint64_t seed = 1; seed <= CAMPAIGN_SEEDS; seed++) {
        struct epoch_sim_config c;
        struct epoch_sim_result r;
        epoch_sim_default(&c, seed);
        c.policy = policy;
        c.reorder = reorder;
        c.host_restart_at_ms = host_restart_at;
        if (cold_keeps_permille) {
            c.cold_keeps_permille = cold_keeps_permille;
            c.cold_permille = 500u;
            c.reset_permille = 20u;
        }
        if (corrupt_permille) {
            c.corrupt_permille = corrupt_permille;
            c.reset_permille = 20u;
        }
        epoch_sim_run(&c, &r);
        if (epoch_sim_violation(&r))
            fprintf(stderr, "violation at seed %" PRIu64 ": rerun with epoch_sim %" PRIu64 "\n", seed, seed);
        check_sound(&r);
        live += r.running_at_end && r.completed_after_faults > 0u;
        sum.issued += r.issued;
        sum.completed += r.completed;
        sum.rejected += r.rejected;
        sum.not_executed += r.not_executed;
        sum.unknown += r.unknown;
        sum.refined += r.refined;
        sum.abandoned += r.abandoned;
        sum.resets += r.resets;
        sum.cold_resets += r.cold_resets;
        sum.boot_resets += r.boot_resets;
        sum.boot_resets_newest[0] += r.boot_resets_newest[0];
        sum.boot_resets_newest[1] += r.boot_resets_newest[1];
        sum.rejected_but_committed += r.rejected_but_committed;
        sum.corruptions += r.corruptions;
        sum.corruptions_newest += r.corruptions_newest;
    }
    CHECK(live == CAMPAIGN_SEEDS);
    CHECK(sum.resets > 0u && sum.cold_resets > 0u && (!cold_keeps_permille || sum.boot_resets > 0u));
    /* resets inside cold initialization over an intact journal reach both layouts, newest record in slot 0 and in slot 1, and check_sound found no false outcome in any seed */
    CHECK(!cold_keeps_permille || (sum.boot_resets_newest[0] > 0u && sum.boot_resets_newest[1] > 0u));
    CHECK(!corrupt_permille || (sum.corruptions_newest > 0u && sum.corruptions > sum.corruptions_newest));
    if (corrupt_permille)
        printf("committed slots corrupted at a warm reset: %" PRIu32 ", of them the newest record %" PRIu32 "\n", sum.corruptions, sum.corruptions_newest);
    if (cold_keeps_permille)
        printf("cold initialization interrupted over a valid journal: newest in slot 0 %" PRIu32 ", in slot 1 %" PRIu32 "\n", sum.boot_resets_newest[0], sum.boot_resets_newest[1]);
    printf("campaign policy=B%d reorder=%d host_restart=%" PRIu32 " seeds=%u live=%u issued=%" PRIu32 " completed=%" PRIu32 " rejected=%" PRIu32 " not_executed=%" PRIu32 " unknown=%" PRIu32 " refined=%" PRIu32 " abandoned=%" PRIu32 " resets=%" PRIu32 " cold=%" PRIu32 " boot_resets=%" PRIu32 " cold_keeps_permille=%" PRIu32 " rejected_but_committed=%" PRIu32 "\n",
           policy == EPOCH_POLICY_B3 ? 3 : 2, reorder, host_restart_at, CAMPAIGN_SEEDS, live, sum.issued, sum.completed, sum.rejected, sum.not_executed, sum.unknown, sum.refined, sum.abandoned, sum.resets, sum.cold_resets, sum.boot_resets, cold_keeps_permille, sum.rejected_but_committed);
}

int main(void)
{
    test_violation_oracle();
    test_scenario_a_normal();
    test_scenario_b_not_executed();
    test_scenario_c_lost_reply();
    test_scripted_matrix();
    test_host_restart();
    test_determinism();
    campaign(EPOCH_POLICY_B3, true, 0, 0, 0);
    campaign(EPOCH_POLICY_B2, false, 0, 0, 0);
    campaign(EPOCH_POLICY_B3, true, 7001, 0, 0);
    campaign(EPOCH_POLICY_B3, true, 0, 500u, 0);
    campaign(EPOCH_POLICY_B2, false, 0, 500u, 0);
    campaign(EPOCH_POLICY_B3, true, 0, 0, 300u);
    return TEST_DONE("fault_campaign");
}
