/* include/human/ml/lora_subprocess.h
 *
 * mlx_lm.lora subprocess driver — Sprint B residuals N1 (2026-05-25).
 *
 * Wraps the existing hu_process_run_with_timeout primitive in a
 * LoRA-training-specific shape. Composes:
 *   - hu_mlx_lm_module_available() — pre-flight gate
 *   - hu_process_run_with_timeout() — bounded subprocess
 *   - JSONL training data path (from hu_lora_export_dpo_pairs)
 *   - adapter output path (where hu_lora_nightly_run will rotate)
 *
 * The nightly orchestrator (hu_lora_nightly_run, shipped in v2026.5.2)
 * currently treats this step as a no-op in dry_run mode. This module
 * implements the real call so the dry_run flag can flip false.
 *
 * Design choices the IMPLEMENTOR decided:
 *   - Reuses hu_process_run_with_timeout (already shipped) rather than
 *     re-implementing posix_spawn — same fork+exec+wait+SIGKILL plumbing.
 *   - Captures stdout + stderr (subject to a max-bytes cap) into the
 *     hu_run_result_t so callers can log them. The mlx-lm progress
 *     output IS the progress signal — we don't add a streaming
 *     callback because subprocess stdout-line-streaming is its own
 *     ~150 LoC of complexity (pipes + nonblocking reads + line buf).
 *
 * Design choices the USER decides (see lora_subprocess.c TODOs):
 *   - Hard timeout
 *   - Retry policy on failure
 *   - Iteration / layer / batch-size defaults
 */
#ifndef HU_ML_LORA_SUBPROCESS_H
#define HU_ML_LORA_SUBPROCESS_H

#include "human/core/allocator.h"
#include "human/core/error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HU_LORA_SUBPROCESS_PATH_MAX         512
#define HU_LORA_SUBPROCESS_MODEL_ID_MAX     256
#define HU_LORA_SUBPROCESS_MAX_OUTPUT_BYTES (1 * 1024 * 1024) /* 1MB stdout+stderr cap */

typedef struct hu_lora_subprocess_config {
    /* Base model identifier — e.g. "mlx-community/gemma-2-2b-it-4bit".
     * Required; pre-flight fails when empty. */
    char base_model[HU_LORA_SUBPROCESS_MODEL_ID_MAX];

    /* Training data path (JSONL produced by hu_lora_export_dpo_pairs). */
    char data_jsonl_path[HU_LORA_SUBPROCESS_PATH_MAX];

    /* Output adapter directory. hu_lora_nightly_run picks the next
     * v<N>/ slot before calling here. */
    char adapter_output_dir[HU_LORA_SUBPROCESS_PATH_MAX];

    /* Hyperparameters. 0 = use built-in defaults. See the impl for
     * what those defaults are and why. */
    int batch_size;  /* 0 → default */
    int iters;       /* 0 → default */
    int lora_layers; /* 0 → default */

    /* Hard upper bound — child is SIGKILLed if exceeded. 0 = use the
     * built-in default. */
    unsigned int timeout_sec;

    /* Number of retries on subprocess failure. 0 = no retries
     * (single attempt). Each retry waits 30s before re-launching. */
    int max_retries;
} hu_lora_subprocess_config_t;

/* Pre-flight: returns true when the system is ready to train. Checks:
 *   - hu_mlx_lm_module_available() (python3 -c "import mlx_lm" exits 0)
 *   - data_jsonl_path exists and is non-empty
 *   - adapter_output_dir exists or can be created
 *
 * Pure inspection — no subprocess launched. Use this from the daemon
 * tick to skip training when prerequisites aren't met (instead of
 * launching, failing, and burning a retry budget). */
bool hu_lora_subprocess_preflight_ok(const hu_lora_subprocess_config_t *cfg);

/* Run mlx_lm.lora with the given config. Blocks until the subprocess
 * exits or the timeout fires. Retries up to cfg->max_retries times on
 * subprocess failure (non-zero exit or timeout).
 *
 * On success (the subprocess exits 0 AND writes
 * `<adapter_output_dir>/adapters.safetensors`):
 *   - Returns HU_OK.
 *
 * On failure paths:
 *   - HU_ERR_NOT_SUPPORTED — mlx_lm not installed; preflight failed
 *   - HU_ERR_INVALID_ARGUMENT — cfg NULL or required field empty
 *   - HU_ERR_IO — every retry exhausted (last subprocess output is
 *     logged via hu_log_error before returning) */
hu_error_t hu_lora_subprocess_train(hu_allocator_t *alloc, const hu_lora_subprocess_config_t *cfg);

/* Interpreter used for `-m mlx_lm.lora`.
 *
 * Resolution order: $HU_MLX_PYTHON (if executable) → the pinned
 * ~/Documents/gemma-realtime-1/.venv312/bin/python3.12 → "python3".
 *
 * Not the bare string "python3": PATH resolved that to python@3.14 on the dev
 * machine 2026-07-26, the interpreter scripts/human-serve.sh deliberately
 * avoids ("3.14 has loky semaphore crash bug"). Serving and both training
 * paths must share one interpreter. Mirrors training_loop.py::mlx_python().
 *
 * Returns a pointer to static storage; valid until the next call. */
const char *hu_lora_subprocess_python(void);

/* Build the argv that hu_lora_subprocess_train would pass to
 * hu_process_run_with_timeout. Pure — no subprocess launched. Used by
 * tests to pin the wire format. Caller passes a fixed-capacity
 * `argv_out` array (≥16 slots recommended); function writes
 * NULL-terminated pointers, plus character storage into `arg_buf`
 * (single contiguous buffer; pointers in argv_out point into it).
 *
 * Returns the count of argv entries written (excluding the trailing
 * NULL), or 0 on error. */
size_t hu_lora_subprocess_build_argv(const hu_lora_subprocess_config_t *cfg, const char **argv_out,
                                     size_t argv_cap, char *arg_buf, size_t arg_buf_cap);

#ifdef __cplusplus
}
#endif
#endif /* HU_ML_LORA_SUBPROCESS_H */
