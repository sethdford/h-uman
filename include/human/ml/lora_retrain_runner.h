#ifndef HU_ML_LORA_RETRAIN_RUNNER_H
#define HU_ML_LORA_RETRAIN_RUNNER_H

/* W14 — Nightly LoRA re-train cron (US-7.5).
 *
 * Sibling runner to `hu_lora_training_runner` (HUML GPT in-process path).
 * This runner orchestrates the MLX-Gemma frontier path as a subprocess
 * chain: pair-count probe → finetune → judgment gate → atomic promote.
 *
 * Registered for `HU_JOB_LORA_RETRAIN_NIGHTLY` (distinct from
 * HU_JOB_LORA_TRAINING). The daemon enqueues this job once per 24h via
 * `hu_w14_scheduler_enqueue_lora_retrain_nightly`; the scheduler gates on
 * idle + AC-power per the W14 contract.
 *
 * Subprocess seam: all exec is funneled through `test_run_subprocess` when
 * non-NULL — every AC test registers a deterministic hook. Under
 * `HU_IS_TEST` the production fork/exec path is disabled to prevent
 * accidental real Python invocation. Outside `HU_IS_TEST`, the runner
 * uses `posix_spawn` + `waitpid` to invoke `finetune-gemma.py` and the
 * gate script.
 *
 * Outcome events are emitted via the optional `emit_event` callback; when
 * NULL they fall through to `hu_log_info`. The persistent state is the
 * `lora_retrain` block written into ~/.human/scheduler.status by
 * `hu_w14_scheduler_status_save` (extended for this story). */

#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hu_allocator;
struct hu_memory_facade;
struct hu_job_spec;

typedef enum hu_lora_retrain_outcome {
    HU_LORA_RETRAIN_OUTCOME_UNKNOWN = 0,
    HU_LORA_RETRAIN_OUTCOME_SKIPPED_NO_NEW_DATA,
    HU_LORA_RETRAIN_OUTCOME_SKIPPED_ALREADY_RUNNING,
    HU_LORA_RETRAIN_OUTCOME_SKIPPED_GATE_FAIL,
    HU_LORA_RETRAIN_OUTCOME_FAILED,
    HU_LORA_RETRAIN_OUTCOME_PROMOTED,
    HU_LORA_RETRAIN_OUTCOME_PROMOTION_FAILED,
} hu_lora_retrain_outcome_t;

/* Subprocess result returned by the test/production exec hook. */
typedef struct hu_lora_retrain_proc_result {
    int exit_code;         /* process exit status (or signal-encoded) */
    char stdout_buf[2048]; /* captured stdout (NUL-terminated) */
    size_t stdout_len;     /* bytes written; <= sizeof(stdout_buf)-1 */
} hu_lora_retrain_proc_result_t;

/* Hook signature for subprocess invocation. Implementations MUST NUL-terminate
 * `result->stdout_buf` and set `result->stdout_len`. Return HU_OK on a clean
 * spawn+wait (regardless of exit code). Return non-OK only on infrastructure
 * failure (cannot fork, etc.) — that surfaces a runner-level failure. */
typedef hu_error_t (*hu_lora_retrain_subprocess_fn)(const char *const argv[],
                                                    hu_lora_retrain_proc_result_t *result,
                                                    void *user_data);

/* Event emission hook. `payload_json` is a NUL-terminated JSON object (no
 * trailing newline). Implementations must not retain pointers past the call. */
typedef void (*hu_lora_retrain_event_fn)(const char *event_name, const char *payload_json,
                                         void *user_data);

typedef struct hu_lora_retrain_ctx {
    struct hu_allocator *alloc; /* optional; system allocator if NULL */

    /* Subprocess command pieces — all NUL-terminated. NULL falls back to the
     * documented defaults; that's the normal production path. */
    const char *miner_argv0;     /* default: "human" */
    const char *miner_subcmd[3]; /* default: {"ml", "mine-corrections", NULL} */
    const char *finetune_script; /* default: "scripts/finetune-gemma.py" */
    const char *gate_script;     /* default: "scripts/check-lora-ab.sh" */
    const char *candidate_dir;   /* required: where the new adapter lands */
    const char *current_symlink; /* required: e.g. ~/.human/ml/seth-lora-current */

    /* Event sink (optional). */
    hu_lora_retrain_event_fn emit_event;
    void *emit_user_data;

    /* Test seam — when set, replaces real subprocess exec. */
    hu_lora_retrain_subprocess_fn test_run_subprocess;
    void *test_subprocess_ud;

    /* PID file path for single-flight enforcement. Required for the
     * production path; tests may set this to a tmp path. NULL disables the
     * single-flight guard entirely (test convenience). */
    const char *pidfile_path;

    /* Out — populated by the runner on exit. */
    hu_lora_retrain_outcome_t last_outcome;
    int64_t last_run_ts;
    unsigned long long last_pairs_consumed;
    int last_exit_code; /* exit code of the failing step on FAILED outcomes */
} hu_lora_retrain_ctx_t;

/* Runner entry point. `spec->kind` must be HU_JOB_LORA_RETRAIN_NIGHTLY.
 * Returns HU_OK on every code path (the runner converts failures into
 * outcome events + status JSON so the scheduler can't mark the job
 * "failed" and re-enter); only argument-validation errors return non-OK. */
hu_error_t hu_lora_retrain_runner(struct hu_memory_facade *m, const struct hu_job_spec *spec,
                                  int64_t budget_ms, void *user_data);

/* String form for status JSON serialization / parsing. */
const char *hu_lora_retrain_outcome_str(hu_lora_retrain_outcome_t o);
hu_lora_retrain_outcome_t hu_lora_retrain_outcome_from_str(const char *s);

/* Parse the optional `lora_retrain` block from scheduler.status JSON.
 * Returns HU_OK when the block is present AND parses; HU_ERR_NOT_FOUND when
 * the block is absent (back-compat: callers should treat as "no data yet");
 * HU_ERR_INVALID_ARGUMENT when arguments are bad or the block is malformed. */
hu_error_t hu_lora_retrain_status_parse(const char *json, long long *out_last_run_ts,
                                        hu_lora_retrain_outcome_t *out_last_outcome,
                                        unsigned long long *out_pairs_consumed);

#ifdef __cplusplus
}
#endif

#endif /* HU_ML_LORA_RETRAIN_RUNNER_H */
