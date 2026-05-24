/* include/human/ml/lora_nightly.h
 *
 * M3 nightly export → train → swap automation — Sprint B residuals #3.
 *
 * The export piece (B-loop C) and the swap admin (Bridge B) are
 * shipped. This module orchestrates them as a once-per-24h job the
 * daemon runs in the background:
 *
 *   1. Predicate: should we run now?
 *      - last_run >= 24h ago AND new pairs since last_run >= MIN_NEW_PAIRS
 *   2. Export current dpo_pairs window to ~/.human/lora-pairs.jsonl
 *   3. Train: subprocess `mlx_lm.lora` to produce a new adapter at
 *      ~/.human/adapters/v<N>/ (skipped in --dry-run mode and on
 *      builds without an MLX runtime)
 *   4. Atomic rotation: relink ~/.human/adapter-current to v<N>
 *   5. Live swap: POST /v1/adapters/swap on the MLX server
 *
 * Each step is independently testable. The orchestrator skeleton is
 * shippable today; the subprocess piece (step 3) requires the
 * mlx-lm binary on PATH and is gated behind `--enable-train`.
 *
 * Why this matters: until automated, the M3 fine-tune flow requires
 * the user to run `human export-dpo`, then `mlx_lm.lora ...`, then
 * `curl /v1/adapters/swap` manually. Most users won't. Automating
 * the loop is the difference between "the bridge works" and "users
 * actually benefit from private learning."
 */
#ifndef HU_ML_LORA_NIGHTLY_H
#define HU_ML_LORA_NIGHTLY_H

#include "human/core/allocator.h"
#include "human/core/error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HU_LORA_NIGHTLY_MIN_INTERVAL_SEC ((int64_t)(24LL * 60 * 60))
#define HU_LORA_NIGHTLY_MIN_NEW_PAIRS    20 /* don't train on tiny deltas */
#define HU_LORA_NIGHTLY_PATH_MAX         512

typedef struct hu_lora_nightly_config {
    char db_path[HU_LORA_NIGHTLY_PATH_MAX];          /* default ~/.human/memory.db */
    char pairs_jsonl_path[HU_LORA_NIGHTLY_PATH_MAX]; /* default ~/.human/lora-pairs.jsonl */
    char adapters_dir[HU_LORA_NIGHTLY_PATH_MAX];     /* default ~/.human/adapters */
    char current_symlink[HU_LORA_NIGHTLY_PATH_MAX];  /* default ~/.human/adapter-current */
    char mlx_base_url[HU_LORA_NIGHTLY_PATH_MAX];     /* default http://127.0.0.1:8741/v1 */
    /* When true, skip the actual mlx_lm subprocess call. Useful for
     * smoke-testing the export+rotation+swap path without an MLX
     * runtime on PATH. */
    bool dry_run;
} hu_lora_nightly_config_t;

/* Initialize the config struct with sensible defaults (resolves $HOME).
 * Returns false when $HOME is unset. */
bool hu_lora_nightly_config_init_defaults(hu_lora_nightly_config_t *cfg);

/* Pure predicate: should the nightly job run NOW?
 *
 *   now_unix          — current time
 *   last_run_unix     — timestamp of the last successful run (0 if never)
 *   new_pairs_since   — count of dpo_pairs added since last_run
 *
 * Returns true when both:
 *   - now_unix - last_run_unix >= HU_LORA_NIGHTLY_MIN_INTERVAL_SEC
 *   - new_pairs_since >= HU_LORA_NIGHTLY_MIN_NEW_PAIRS
 *
 * The first-run case (last_run_unix == 0) treats the interval as
 * always satisfied; the pair-count gate still applies. */
bool hu_lora_nightly_should_run(int64_t now_unix, int64_t last_run_unix, int32_t new_pairs_since);

/* Atomically rotate the `current_symlink` to point at `target_dir`.
 * Implementation: symlink → tmp path → rename. Returns HU_OK on
 * success, HU_ERR_IO on filesystem failure. */
hu_error_t hu_lora_nightly_rotate_symlink(const char *current_symlink, const char *target_dir);

/* End-to-end nightly orchestrator. Steps 2 + 4 + 5 always execute.
 * Step 3 (train) is skipped in dry_run mode and when no mlx_lm
 * subprocess support is compiled in.
 *
 * Returns the count of pairs exported via *out_pair_count.
 * Returns HU_ERR_NOT_FOUND when no new pairs (no-op skip).
 * Returns HU_OK on success of all executed steps. */
hu_error_t hu_lora_nightly_run(hu_allocator_t *alloc, const hu_lora_nightly_config_t *cfg,
                               int64_t now_unix, size_t *out_pair_count);

#ifdef __cplusplus
}
#endif
#endif /* HU_ML_LORA_NIGHTLY_H */
