/* W14 Training data extraction runner.
 *
 * Periodically extracts new conversations into training data JSONL and
 * auto-generates DPO pairs from detected user corrections. When enough
 * new examples have accumulated, enqueues a follow-up LoRA training job
 * on the same scheduler.
 *
 * `user_data` carries a `hu_training_data_runner_ctx_t *` that bundles
 * paths, the scheduler handle, and a running extraction count.
 *
 * Determinism: the runner reads OS clock only for timestamping JSONL
 * filenames and extraction records. The extraction logic itself is
 * deterministic given the same DB state. */

#include "human/agent/training_data_runner.h"
#include "human/agent/scheduler.h"
#include "human/agent/world_model_bridge.h"
#include "human/core/error.h"
#include "human/core/log.h"
#include "human/ml/training_data_extractor.h"

#include <string.h>
#include <time.h>

hu_error_t hu_training_data_runner(hu_memory_facade_t *m,
                                   const struct hu_job_spec *spec,
                                   int64_t budget_ms, void *user_data) {
    (void)m;
    (void)budget_ms;
    if (!spec || !user_data)
        return HU_ERR_INVALID_ARGUMENT;

    hu_training_data_runner_ctx_t *ctx =
        (hu_training_data_runner_ctx_t *)user_data;

    if (!ctx->memory_db_path || !ctx->output_dir)
        return HU_ERR_INVALID_ARGUMENT;

    hu_allocator_t sys;
    hu_allocator_t *alloc = ctx->alloc;
    if (!alloc) {
        sys = hu_system_allocator();
        alloc = &sys;
    }

    size_t threshold = ctx->retrain_threshold > 0
                           ? ctx->retrain_threshold
                           : HU_TRAINING_DATA_RETRAIN_THRESHOLD;

    /* Phase 1: Extract conversations into JSONL training data. */
    size_t extracted = 0;
    hu_error_t err = hu_training_data_extract(
        alloc, ctx->memory_db_path, ctx->persona_path,
        ctx->output_dir, &extracted);

    if (err != HU_OK) {
        hu_log_warn("training-data-runner", NULL,
                    "extraction failed: %s", hu_error_string(err));
        return err;
    }

    if (extracted > 0)
        hu_log_info("training-data-runner", NULL,
                    "extracted %zu new training examples", extracted);

    /* Phase 2: Auto-DPO pair generation from user corrections. */
    size_t dpo_pairs = 0;
    hu_error_t dpo_err = hu_training_data_extract_dpo(
        alloc, ctx->memory_db_path,
        ctx->correction_window_sec, &dpo_pairs);

    if (dpo_err != HU_OK && dpo_err != HU_ERR_NOT_SUPPORTED)
        hu_log_warn("training-data-runner", NULL,
                    "DPO extraction failed: %s", hu_error_string(dpo_err));

    if (dpo_pairs > 0)
        hu_log_info("training-data-runner", NULL,
                    "generated %zu auto-DPO pairs from corrections", dpo_pairs);

    /* Phase 3: Accumulate and check threshold for LoRA retraining. */
    ctx->cumulative_extracted += extracted;

    if (ctx->cumulative_extracted >= threshold && ctx->scheduler) {
        int64_t now_ms = (int64_t)time(NULL) * 1000LL;
        hu_error_t enq = hu_w14_scheduler_enqueue_lora(
            ctx->scheduler, now_ms, 300000);
        if (enq == HU_OK) {
            hu_log_info("training-data-runner", NULL,
                        "enqueued LoRA retraining (%zu cumulative examples)",
                        ctx->cumulative_extracted);
            ctx->cumulative_extracted = 0;
        }
    }

    return HU_OK;
}
