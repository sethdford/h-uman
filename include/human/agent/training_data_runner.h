#ifndef HU_AGENT_TRAINING_DATA_RUNNER_H
#define HU_AGENT_TRAINING_DATA_RUNNER_H

/* W14 — Training data extraction scheduler runner.
 *
 * Bridges the continuous learning loop into the W14 sleep-time
 * scheduler. During idle periods, this runner:
 *
 * 1. Extracts new conversations from memory.db into JSONL training data.
 * 2. Generates auto-DPO pairs from detected user corrections.
 * 3. If enough new training examples have accumulated, enqueues a
 *    LoRA training job via the existing HU_JOB_LORA_TRAINING path.
 *
 * `user_data` is `hu_training_data_runner_ctx_t *`. The caller owns
 * every pointer inside it; the runner does not free anything.
 *
 * Layer 4 of the v2 stack. */

#include "human/core/allocator.h"
#include "human/core/error.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hu_memory_facade;
struct hu_job_spec;
struct hu_w14_scheduler;

typedef struct hu_training_data_runner_ctx {
    hu_allocator_t *alloc;
    const char *memory_db_path;      /* path to memory.db */
    const char *persona_path;        /* persona JSON path, may be NULL */
    const char *output_dir;          /* training data output directory */
    struct hu_w14_scheduler *scheduler; /* for enqueuing follow-up LoRA jobs */
    size_t retrain_threshold;        /* min examples before LoRA enqueue; 0 = default (50) */
    int correction_window_sec;       /* DPO correction detection window; 0 = default (120) */
    size_t cumulative_extracted;     /* running count of extracted examples since last train */
} hu_training_data_runner_ctx_t;

hu_error_t hu_training_data_runner(struct hu_memory_facade *m,
                                   const struct hu_job_spec *spec,
                                   int64_t budget_ms, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_TRAINING_DATA_RUNNER_H */
