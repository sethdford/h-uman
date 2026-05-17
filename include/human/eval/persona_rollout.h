#ifndef HU_EVAL_PERSONA_ROLLOUT_H
#define HU_EVAL_PERSONA_ROLLOUT_H

/* Shared persona rollout + v2 fidelity scoring for LoRA eval gate,
 * `human ml lora-runner`, competitive scorecard, and demo evidence. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/personal_model.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hu_provider;

typedef struct hu_persona_rollout_config {
    struct hu_provider *provider;
    const char *adapter_path;
    const hu_communication_style_t *target;
    const char **prompts;
    const char **system_prompts;
    size_t n_prompts;
    int64_t timeout_ms_per_prompt;
    bool capture_responses;
} hu_persona_rollout_config_t;

typedef struct hu_persona_rollout_result {
    double *persona_scores;
    size_t n_prompts;
    size_t n_scored;
    double p95_ms;
    double mean_ms;
    size_t n_errors;
    char **responses;
    size_t *response_lens;
} hu_persona_rollout_result_t;

hu_error_t hu_persona_rollout_run(hu_allocator_t *alloc, const hu_persona_rollout_config_t *cfg,
                                  hu_persona_rollout_result_t *out);

void hu_persona_rollout_result_free(hu_allocator_t *alloc, hu_persona_rollout_result_t *r);

/* Load newline-delimited prompts from a fixture file (skips blanks and # lines). */
hu_error_t hu_persona_rollout_load_prompt_fixture(hu_allocator_t *alloc, const char *path,
                                                  char ***out_prompts, size_t *out_n);

#ifdef __cplusplus
}
#endif

#endif /* HU_EVAL_PERSONA_ROLLOUT_H */
