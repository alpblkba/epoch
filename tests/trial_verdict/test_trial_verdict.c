#include <stdbool.h>
#include <string.h>

#include "test.h"
#include "trial_verdict.h"

/* The acceptance chain epoch_client.c used before trial_judge, kept here to show which cases it accepted wrongly. */
static bool old_accepts(const struct trial_evidence *e)
{
    uint32_t delta = e->c2 - e->c0 - 1u;
    uint32_t expect = (e->checkpoint == EPOCH_CP_F3 ? 1u : 0u) + (e->policy == 0 ? 1u : 0u);
    bool executed = delta - (e->policy == 0 ? 1u : 0u) >= 1u;
    bool false_completed = e->policy > 0 && e->outcome == EPOCH_OUTCOME_COMPLETED && !executed;
    bool false_not_executed = e->outcome == EPOCH_OUTCOME_NOT_EXECUTED && executed;
    if (delta != expect || e->foreign_replies || false_completed || false_not_executed)
        return false;
    if (e->policy == 3 && e->outcome != (executed ? EPOCH_OUTCOME_COMPLETED : EPOCH_OUTCOME_NOT_EXECUTED))
        return false;
    if (e->policy == 0 && e->outcome != EPOCH_OUTCOME_COMPLETED)
        return false;
    return true;
}

/* A trial whose reset happened exactly as armed and whose counter moved as the checkpoint requires. */
static struct trial_evidence good(int policy, enum epoch_checkpoint cp, enum epoch_outcome o)
{
    struct trial_evidence e;
    memset(&e, 0, sizeof e);
    e.policy = policy;
    e.checkpoint = cp;
    e.run = 7;
    e.txid = 3;
    e.marker_valid = true;
    e.marker_run = 7;
    e.marker_checkpoint = (uint32_t)cp;
    e.marker_txid = 3;
    e.reset_cause = TRIAL_SFTRSTF;
    e.boot_before = 1;
    e.boot_after = 2;
    e.outcome = o;
    e.c0 = 10;
    e.c2 = e.c0 + 1u + (cp == EPOCH_CP_F3 ? 1u : 0u) + (policy == 0 ? 1u : 0u);
    return e;
}

static enum trial_verdict judge(const struct trial_evidence *e)
{
    const char *why = NULL;
    enum trial_verdict v = trial_judge(e, &why);
    CHECK(why != NULL && (v == TRIAL_COUNTED_PASS) == (why[0] == '\0'));
    return v;
}

static void test_valid_trials_pass(void)
{
    for (int cp = EPOCH_CP_F1; cp <= EPOCH_CP_F3; cp++) {
        bool executed = cp == EPOCH_CP_F3;
        struct trial_evidence e[4] = {good(0, (enum epoch_checkpoint)cp, EPOCH_OUTCOME_COMPLETED), good(1, (enum epoch_checkpoint)cp, EPOCH_OUTCOME_UNKNOWN),
                                      good(2, (enum epoch_checkpoint)cp, EPOCH_OUTCOME_UNKNOWN),
                                      good(3, (enum epoch_checkpoint)cp, executed ? EPOCH_OUTCOME_COMPLETED : EPOCH_OUTCOME_NOT_EXECUTED)};
        for (unsigned i = 0; i < 4; i++) {
            CHECK(judge(&e[i]) == TRIAL_COUNTED_PASS);
            CHECK(old_accepts(&e[i]));
        }
    }
}

/* The cases the old chain accepted for B1 and B2: each must now fail for its outcome, not for any other reason. */
static void test_wrong_b1_b2_outcomes_fail(void)
{
    static const struct {
        enum epoch_checkpoint cp;
        enum epoch_outcome o;
    } cases[] = {
        {EPOCH_CP_F1, EPOCH_OUTCOME_NONE}, {EPOCH_CP_F1, EPOCH_OUTCOME_REJECTED}, {EPOCH_CP_F3, EPOCH_OUTCOME_NONE}, {EPOCH_CP_F3, EPOCH_OUTCOME_REJECTED},
        {EPOCH_CP_F3, EPOCH_OUTCOME_COMPLETED}, {EPOCH_CP_F2B, EPOCH_OUTCOME_NONE},
    };
    for (int policy = 1; policy <= 2; policy++) {
        for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            struct trial_evidence e = good(policy, cases[i].cp, cases[i].o);
            const char *why = NULL;
            CHECK(old_accepts(&e));
            CHECK(trial_judge(&e, &why) == TRIAL_COUNTED_FAIL);
            CHECK(why && strcmp(why, "outcome differs from the one expected for the policy") == 0);
        }
    }
}

/* Wrong outcomes the old chain already rejected stay rejected, with the specific reason. */
static void test_false_outcomes_fail(void)
{
    for (int policy = 1; policy <= 3; policy++) {
        struct trial_evidence e = good(policy, EPOCH_CP_F1, EPOCH_OUTCOME_COMPLETED);
        const char *why = NULL;
        CHECK(trial_judge(&e, &why) == TRIAL_COUNTED_FAIL && strcmp(why, "false COMPLETED") == 0);
        e = good(policy, EPOCH_CP_F3, EPOCH_OUTCOME_NOT_EXECUTED);
        CHECK(trial_judge(&e, &why) == TRIAL_COUNTED_FAIL && strcmp(why, "false NOT_EXECUTED") == 0);
    }
    struct trial_evidence e = good(0, EPOCH_CP_F3, EPOCH_OUTCOME_UNKNOWN);
    CHECK(judge(&e) == TRIAL_COUNTED_FAIL);
    e = good(3, EPOCH_CP_F3, EPOCH_OUTCOME_UNKNOWN);
    CHECK(judge(&e) == TRIAL_COUNTED_FAIL);
}

static void test_counter_and_session_checks(void)
{
    struct trial_evidence e = good(2, EPOCH_CP_F3, EPOCH_OUTCOME_UNKNOWN);
    e.c2++; /* a duplicate execution */
    CHECK(judge(&e) == TRIAL_COUNTED_FAIL);
    e = good(2, EPOCH_CP_F1, EPOCH_OUTCOME_UNKNOWN);
    e.c2 = e.c0; /* the oracle INCREMENT itself is missing */
    CHECK(judge(&e) == TRIAL_COUNTED_FAIL);
    e = good(1, EPOCH_CP_F1, EPOCH_OUTCOME_UNKNOWN);
    e.c0 = UINT32_MAX; /* the counter wraps between c0 and c2 */
    e.c2 = 0;
    CHECK(judge(&e) == TRIAL_COUNTED_PASS);
    e = good(2, EPOCH_CP_F1, EPOCH_OUTCOME_UNKNOWN);
    e.foreign_replies = 1;
    CHECK(judge(&e) == TRIAL_COUNTED_FAIL);
}

/* A rejected frame anywhere in a trial keeps it from passing; a failure the oracle shows stays a failure. */
static void test_rejected_frame(void)
{
    for (int policy = 0; policy < 4; policy++) {
        struct trial_evidence e = good(policy, EPOCH_CP_F3, EPOCH_OUTCOME_COMPLETED);
        if (policy == 1 || policy == 2)
            e.outcome = EPOCH_OUTCOME_UNKNOWN;
        CHECK(judge(&e) == TRIAL_COUNTED_PASS);
        e.rx_errors = 1;
        CHECK(judge(&e) == TRIAL_INCONCLUSIVE);
        e.c2++;
        CHECK(judge(&e) == TRIAL_COUNTED_FAIL);
    }
}

static void test_unproven_reset_is_inconclusive(void)
{
    struct trial_evidence e = good(2, EPOCH_CP_F1, EPOCH_OUTCOME_UNKNOWN);
    e.marker_valid = false;
    CHECK(judge(&e) == TRIAL_INCONCLUSIVE);
    e = good(2, EPOCH_CP_F1, EPOCH_OUTCOME_UNKNOWN);
    e.marker_checkpoint = EPOCH_CP_F3;
    CHECK(judge(&e) == TRIAL_INCONCLUSIVE);
    e = good(2, EPOCH_CP_F1, EPOCH_OUTCOME_UNKNOWN);
    e.marker_txid = 4;
    CHECK(judge(&e) == TRIAL_INCONCLUSIVE);
    e = good(2, EPOCH_CP_F1, EPOCH_OUTCOME_UNKNOWN);
    e.reset_cause = 1u << 26; /* pin reset, not the armed software reset */
    CHECK(judge(&e) == TRIAL_INCONCLUSIVE);
    static const uint32_t other[] = { 1u << 31, 1u << 30, 1u << 29, 1u << 27, 1u << 25 }; /* low-power, window and independent watchdog, brown-out, option-byte load */
    for (unsigned i = 0; i < sizeof other / sizeof other[0]; i++) {
        e.reset_cause = TRIAL_SFTRSTF | (1u << 26) | other[i];
        CHECK(judge(&e) == TRIAL_INCONCLUSIVE);
    }
    e.reset_cause = TRIAL_SFTRSTF | (1u << 26) | 0x4400u; /* the cause recorded on the board */
    CHECK(judge(&e) == TRIAL_COUNTED_PASS);
    e = good(2, EPOCH_CP_F1, EPOCH_OUTCOME_UNKNOWN);
    e.boot_after = e.boot_before;
    CHECK(judge(&e) == TRIAL_INCONCLUSIVE);
    e = good(4, EPOCH_CP_F1, EPOCH_OUTCOME_UNKNOWN);
    CHECK(judge(&e) == TRIAL_NOT_RUN);
}

int main(void)
{
    test_valid_trials_pass();
    test_wrong_b1_b2_outcomes_fail();
    test_false_outcomes_fail();
    test_counter_and_session_checks();
    test_unproven_reset_is_inconclusive();
    test_rejected_frame();
    return TEST_DONE("trial_verdict");
}
