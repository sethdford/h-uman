/* include/human/ml/cli_grpo.h — Phase 4 Task 9 (RL SOTA)
 *
 * CLI handler for `human ml grpo-train`. Trains a GRPO trainer (Group
 * Relative Policy Optimization, Shao et al. 2024 — DeepSeekMath §4.1.2)
 * on prompt rows extracted from a JSONL preference-pair file. Rollouts
 * are sampled from the live policy by the trainer; chosen/rejected
 * columns in the JSONL are deliberately ignored.
 *
 * Signature mirrors Phase 2 hu_ml_cli_dpo_real and Phase 3
 * hu_ml_cli_kto_train / hu_ml_cli_rm_train — the cmd_ml dispatcher in
 * src/main.c forwards (alloc, argc - 2, argv + 2) verbatim.
 *
 * R9 reward-hacking pin (umbrella §10): there is no implicit default
 * for the reward source. The handler errors with HU_ERR_INVALID_ARGUMENT
 * if neither --reward-fn nor --reward-model is supplied. Picking the
 * wrong source silently is the named risk; explicit beats convenient.
 */
#ifndef HU_ML_CLI_GRPO_H
#define HU_ML_CLI_GRPO_H

#include "human/core/allocator.h"
#include "human/core/error.h"

#ifdef __cplusplus
extern "C" {
#endif

hu_error_t hu_ml_cli_grpo_train(hu_allocator_t *alloc, int argc, const char **argv);

#ifdef __cplusplus
}
#endif
#endif /* HU_ML_CLI_GRPO_H */
