#ifndef EPOCH_TRIAL_VERDICT_H
#define EPOCH_TRIAL_VERDICT_H

#include <stdbool.h>
#include <stdint.h>

#include "epoch/types.h"

/* Acceptance predicate for one reset-campaign trial of host/client/epoch_client.c, kept separate so a native test can check it without a board. */

enum trial_verdict { TRIAL_COUNTED_PASS, TRIAL_COUNTED_FAIL, TRIAL_INCONCLUSIVE, TRIAL_NOT_RUN };

struct trial_evidence {
    int policy;                     /* 0..3 for B0..B3 */
    enum epoch_checkpoint checkpoint;
    uint32_t run, txid;             /* commanded trial identity */
    bool marker_valid;              /* TEST_DIAG_REPLY after the reset */
    uint32_t marker_run, marker_checkpoint, marker_txid;
    uint32_t reset_cause;           /* raw RCC_CSR captured at boot */
    uint64_t boot_before, boot_after;
    enum epoch_outcome outcome;     /* what the policy reported for txid */
    uint32_t c0, c2;                /* counter from a plain INCREMENT before the trial and after the reset */
    unsigned foreign_replies;       /* EP1_REPLY for another session during the trial */
    unsigned rx_errors;             /* frames the host rejected (COBS, CRC, length or field) during the trial */
};

#define TRIAL_SFTRSTF (1u << 28)
#define TRIAL_OTHER_RESET_FLAGS 0xEA000000u /* RCC_CSR LPWRRSTF, WWDGRSTF, IWDGRSTF, BORRSTF, OBLRSTF; PINRSTF accompanies every reset and is not checked */

/* Returns the verdict and sets *why to a reason, empty on COUNTED_PASS. Evidence that the commanded reset did not happen as armed gives INCONCLUSIVE; a wrong count, a session isolation violation or an outcome other than the one expected for the policy gives COUNTED_FAIL; a trial that would pass but had a rejected frame is INCONCLUSIVE. */
enum trial_verdict trial_judge(const struct trial_evidence *e, const char **why);

#endif
