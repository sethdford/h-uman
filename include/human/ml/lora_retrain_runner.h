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
    /* US-11.8 — dual fast/slow LoRA: KL drift exceeded tau on the
     * probe set. Quarantine fast; preserve slow. */
    HU_LORA_RETRAIN_OUTCOME_SKIPPED_KL_DRIFT,
    /* US-11.8 — dual fast/slow LoRA: OLD-pairs NLL regressed beyond
     * forget_tau_nll. Quarantine fast; preserve slow. */
    HU_LORA_RETRAIN_OUTCOME_SKIPPED_FORGETTING,
    /* US-11.8 — dual fast/slow LoRA: EMA compat-check failed
     * (rank/target-modules/base-model mismatch). Preserve slow. */
    HU_LORA_RETRAIN_OUTCOME_EMA_SKIPPED,
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

    /* ── US-11.8: dual fast/slow LoRA ─────────────────────────────── */

    /* Feature flag — when 0, the runner behaves exactly as Sprint 7
     * US-7.5 (single-adapter promotion via `gate_script`). When 1, the
     * runner invokes `cascade_script` (US-11.7), performs the EMA
     * promotion, runs the KL+forgetting sanity checks, and writes the
     * versioned slow artifact. Defaults to 0 so existing tests and
     * production paths are unchanged until the operator opts in. */
    int dual_lora_enabled;

    /* Path layout (all required when dual_lora_enabled is set):
     *   slow_dir          → ~/.human/adapters/  (holds slow.safetensors.v{N})
     *   quarantine_dir    → ~/.human/adapters/quarantine/
     *   fast_path         → ~/.human/adapters/fast.safetensors
     *   kl_probe_set      → tests/fixtures/kl_probe_200.jsonl (or prod equiv)
     *   old_pairs_holdout → tests/fixtures/old_pairs_holdout.jsonl
     *   base_model_path   → "" for the stubbed Sprint 11 path (no real KL) */
    const char *slow_dir;
    const char *quarantine_dir;
    const char *fast_path;
    const char *kl_probe_set;
    const char *old_pairs_holdout;
    const char *base_model_path;

    /* US-11.7 cascade orchestrator — default "scripts/stage_cascade.py". */
    const char *cascade_script;
    /* EMA helper — default "scripts/lora_ema.py". */
    const char *ema_script;
    /* KL drift helper — default "scripts/compute_kl_drift.py". */
    const char *kl_drift_script;
    /* YNTP / OLD-pairs evaluator — default "scripts/yntp_eval.py". */
    const char *yntp_eval_script;

    /* Tunables. 0 selects HU_LORA_EMA_DEFAULT_ALPHA / *_DEFAULT_NATS /
     * *_DEFAULT_NLL. */
    double ema_alpha;
    double kl_tau_nats;
    double forget_tau_nll;

    /* Fixed "today" stamp for quarantine filenames; format YYYY-MM-DD.
     * When empty/NULL the runner derives this from time(NULL). Tests
     * pin this for deterministic filenames. */
    const char *today_yyyymmdd;

    /* Out — populated by the runner on exit. */
    hu_lora_retrain_outcome_t last_outcome;
    int64_t last_run_ts;
    unsigned long long last_pairs_consumed;
    int last_exit_code; /* exit code of the failing step on FAILED outcomes */

    /* US-11.8 — extended status fields for AC-11.8.5. */
    int last_slow_version;           /* N for slow.safetensors.v{N}; -1 if none */
    int last_fast_version;           /* monotonic counter; ++ on each run */
    double last_ema_alpha;           /* alpha used on the last promotion; 0 otherwise */
    char last_gate_verdict[16];      /* "PROMOTE" | "DEFER" | "REJECT" | "" */
    double last_kl_drift_nats;       /* KL on the last KL probe; -1.0 = not run */
    double last_old_pairs_delta_nll; /* OLD-pairs ΔNLL; 0.0 = not run */
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
