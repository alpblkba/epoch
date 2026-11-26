#include "trial_verdict.h"

/* Every armed checkpoint resets the board before the reply for txid is sent, so no policy can have seen that reply. B1 and B2 have no other evidence and must report UNKNOWN; B0 must complete through its retry; B3 must match the oracle from the journal snapshot. */
static enum epoch_outcome expected(int policy, bool executed)
{
    switch (policy) {
    case 0: return EPOCH_OUTCOME_COMPLETED;
    case 1:
    case 2: return EPOCH_OUTCOME_UNKNOWN;
    case 3: return executed ? EPOCH_OUTCOME_COMPLETED : EPOCH_OUTCOME_NOT_EXECUTED;
    default: return EPOCH_OUTCOME_NONE;
    }
}

enum trial_verdict trial_judge(const struct trial_evidence *e, const char **why)
{
    if (e->policy < 0 || e->policy > 3 || e->checkpoint > EPOCH_CP_F3) {
        *why = "policy or checkpoint outside the campaign";
        return TRIAL_NOT_RUN;
    }
    if (!e->marker_valid || e->marker_run != e->run || e->marker_checkpoint != (uint32_t)e->checkpoint || e->marker_txid != e->txid) {
        *why = "marker does not match the commanded checkpoint";
        return TRIAL_INCONCLUSIVE;
    }
    if (!(e->reset_cause & TRIAL_SFTRSTF) || (e->reset_cause & TRIAL_OTHER_RESET_FLAGS) || e->boot_after == e->boot_before) {
        *why = "reset cause or boot ID does not show a software reset";
        return TRIAL_INCONCLUSIVE;
    }
    /* c2 - c0 counts the trial request, B0's retries and the oracle INCREMENT itself */
    uint32_t delta = e->c2 - e->c0 - 1u;
    uint32_t retry = e->policy == 0 ? 1u : 0u;
    bool executed = e->checkpoint == EPOCH_CP_F3;
    if (delta != (executed ? 1u : 0u) + retry) {
        *why = "oracle count disagrees with the checkpoint";
        return TRIAL_COUNTED_FAIL;
    }
    if (e->foreign_replies) {
        *why = "EP1_REPLY for another session (P1)";
        return TRIAL_COUNTED_FAIL;
    }
    if (e->policy > 0 && e->outcome == EPOCH_OUTCOME_COMPLETED && !executed) {
        *why = "false COMPLETED";
        return TRIAL_COUNTED_FAIL;
    }
    if (e->outcome == EPOCH_OUTCOME_NOT_EXECUTED && executed) {
        *why = "false NOT_EXECUTED";
        return TRIAL_COUNTED_FAIL;
    }
    if (e->outcome != expected(e->policy, executed)) {
        *why = "outcome differs from the one expected for the policy";
        return TRIAL_COUNTED_FAIL;
    }
    if (e->rx_errors) {
        *why = "rejected frame during the trial";
        return TRIAL_INCONCLUSIVE;
    }
    *why = "";
    return TRIAL_COUNTED_PASS;
}
