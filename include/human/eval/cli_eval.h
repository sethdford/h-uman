#ifndef HU_EVAL_CLI_EVAL_H
#define HU_EVAL_CLI_EVAL_H

#include "human/core/allocator.h"
#include "human/core/error.h"

#ifdef __cplusplus
extern "C" {
#endif

hu_error_t hu_eval_cli_competitive(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t hu_eval_cli_leaderboard(hu_allocator_t *alloc, int argc, char **argv);
hu_error_t hu_eval_cli_gate(hu_allocator_t *alloc, int argc, char **argv);

/* `human eval score` — per-axis humanness scores for a JSONL of replies.
 * Unconditional (no HU_ENABLE_RL_FULL gate): depends only on the always-built
 * shape / fidelity / register scorers. See human/eval/eval_score.h for the
 * pure, unit-testable core (hu_eval_score_jsonl). */
hu_error_t hu_eval_cli_score(hu_allocator_t *alloc, int argc, char **argv);

#ifdef HU_IS_TEST
bool hu_build_has_competitive_eval(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* HU_EVAL_CLI_EVAL_H */
