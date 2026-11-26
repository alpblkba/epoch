#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "sim.h"

/* Usage: epoch_sim [first_seed [count]]. A single seed prints its full trace to stdout. */
int main(int argc, char **argv)
{
    uint64_t first = argc > 1 ? strtoull(argv[1], NULL, 0) : 1u;
    uint64_t count = argc > 2 ? strtoull(argv[2], NULL, 0) : 1u;
    int bad = 0;
    for (uint64_t seed = first; seed < first + count; seed++) {
        struct epoch_sim_config cfg;
        struct epoch_sim_result r;
        epoch_sim_default(&cfg, seed);
        cfg.trace = count == 1u ? stdout : NULL;
        epoch_sim_run(&cfg, &r);
        bool violation = epoch_sim_violation(&r);
        bad |= violation || !r.running_at_end || r.completed_after_faults == 0u;
        printf("seed=%" PRIu64 " hash=%016" PRIx64 " issued=%" PRIu32 " completed=%" PRIu32 " rejected=%" PRIu32 " not_executed=%" PRIu32 " unknown=%" PRIu32 " refined=%" PRIu32 " abandoned=%" PRIu32 " resets=%" PRIu32 " cold=%" PRIu32 " boot_resets=%" PRIu32 " false_completed=%" PRIu32 " false_not_executed=%" PRIu32 " duplicate_commits=%" PRIu32 " rejected_but_committed=%" PRIu32 " after_faults=%" PRIu32 " running=%d\n",
               seed, r.hash, r.issued, r.completed, r.rejected, r.not_executed, r.unknown, r.refined, r.abandoned, r.resets, r.cold_resets, r.boot_resets, r.false_completed, r.false_not_executed, r.duplicate_commits, r.rejected_but_committed, r.completed_after_faults, r.running_at_end);
    }
    return bad ? EXIT_FAILURE : EXIT_SUCCESS;
}
