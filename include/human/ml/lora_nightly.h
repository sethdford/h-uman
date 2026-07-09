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
    /* Base model identifier passed to mlx_lm.lora --model. Empty →
     * subprocess training is skipped (rotation + swap still execute
     * against an empty adapter dir, useful for smoke-testing). Set
     * via hu_lora_nightly_config_init_defaults to a built-in default. */
    char base_model[256];
    /* When true, skip the actual mlx_lm subprocess call. Useful for
     * smoke-testing the export+rotation+swap path without an MLX
     * runtime on PATH. */
    bool dry_run;
    /* Path to the blind-A/B gate JSON file. Default ~/.human/blind_ab_gate.json.
     * See hu_lora_gate_verdict_from_file docs. */
    char gate_verdict_path[HU_LORA_NIGHTLY_PATH_MAX];
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

/* ── Measurement-gated promotion ────────────────────────────────────────
 *
 * A freshly-trained adapter must NOT be swapped onto the LIVE server
 * unconditionally — a degenerate train (e.g. loss collapse / over-fit, the
 * lora-scale-default-or-die failure) would otherwise poison production. The
 * promotion decision is a pure predicate so its truth table is testable
 * without filesystem or network (security-predicate-extraction pattern). */

/* Verdict of the persona measurement gate (the blind-A/B). */
typedef enum hu_lora_gate_verdict {
    HU_LORA_GATE_ABSENT = 0, /* no measurement available (e.g. human A/B not yet run) */
    HU_LORA_GATE_PASS,       /* new adapter measured >= incumbent */
    HU_LORA_GATE_FAIL,       /* new adapter measured WORSE — never promote */
} hu_lora_gate_verdict_t;

/* What to do with a freshly-trained adapter. */
typedef enum hu_lora_promotion_decision {
    HU_LORA_PROMOTE_REJECT = 0, /* discard — invalid adapter or failed measurement */
    HU_LORA_PROMOTE_HOLD,       /* stage on disk, do NOT swap live (awaiting measurement) */
    HU_LORA_PROMOTE_LIVE,       /* safe to swap onto the live server */
} hu_lora_promotion_decision_t;

/* Pure decision: may the new adapter be promoted to the LIVE server?
 *
 *   adapter_valid    — the trained adapter file exists and is non-empty
 *   measurement      — the blind-A/B verdict for the new adapter
 *   allow_unmeasured — operator opt-in to promote when NO measurement exists
 *                      (default false: an unmeasured adapter is HELD, not
 *                      auto-promoted — the feature-gate-requires-measurement
 *                      contract).
 *
 * Truth table:
 *   !adapter_valid                      -> REJECT
 *   measurement == FAIL                 -> REJECT
 *   measurement == PASS                 -> LIVE
 *   measurement == ABSENT && allow      -> LIVE
 *   measurement == ABSENT && !allow     -> HOLD
 *
 * Note FAIL subsumes the loss-collapse case: a degenerate adapter regresses
 * the measurement, so gating on the verdict catches it regardless of cause. */
hu_lora_promotion_decision_t hu_lora_nightly_promotion_allowed(bool adapter_valid,
                                                               hu_lora_gate_verdict_t measurement,
                                                               bool allow_unmeasured);

/* Atomically rotate the `current_symlink` to point at `target_dir`.
 * Implementation: symlink → tmp path → rename. Returns HU_OK on
 * success, HU_ERR_IO on filesystem failure. */
hu_error_t hu_lora_nightly_rotate_symlink(const char *current_symlink, const char *target_dir);

/* ── Blind-A/B gate verdict parsing ──────────────────────────────────────
 *
 * Parse the human half of the blind_ab_gate.json schema:
 *   {"human":{"verdict":"PASS"|"FAIL"|...}}
 *
 * Returns the verdict (PASS, FAIL, or ABSENT for malformed/missing).
 * A fail-safe: any parsing error or missing key → ABSENT (safe default).
 * Verdicts: detection <= 0.65 → PASS, >= 0.75 → FAIL, between → ABSENT.
 *
 * Default runtime path: $HOME/.human/blind_ab_gate.json (document in
 * hu_lora_nightly_config_init_defaults). */

/* Pure parser: parse JSON string {"human":{"verdict":"..."}} into a verdict.
 * Returns HU_LORA_GATE_ABSENT on malformed JSON, missing keys, or unrecognized
 * verdict values. Caller owns the JSON string lifetime. Bounded 16KB cap
 * on the JSON input (safety against huge files). */
hu_lora_gate_verdict_t hu_lora_gate_verdict_parse(const char *json, size_t len);

/* File loader: read and parse a blind_ab_gate.json file.
 * Returns HU_LORA_GATE_ABSENT if:
 *   - file does not exist
 *   - file is unreadable
 *   - JSON parsing fails
 *   - "human" or "verdict" keys are missing
 * Otherwise returns the parsed verdict. */
hu_lora_gate_verdict_t hu_lora_gate_verdict_from_file(const char *path);

/* Pure freshness guard (adversarial-review finding 2026-06-10): a verdict
 * file written BEFORE the adapter it would judge measured a PREVIOUS adapter
 * and must be demoted to ABSENT by the caller. True when
 * verdict_mtime >= adapter_mtime. */
bool hu_lora_gate_verdict_fresh(int64_t verdict_mtime, int64_t adapter_mtime);

/* KTO auto-train handoff: when the nightly finds 0 DPO pairs but exports
 * single-sided KTO signals, it writes "<kto_jsonl_path>.pending" (JSON:
 * data path, signal count, exported_unix). The maintenance-window job
 * (scripts/kto-train-window.sh, launchd 04:40) consumes the marker,
 * stops the MLX server, trains, restarts — closing the reaction→train
 * loop without co-running a 31B train against the live server (OOMs,
 * verified 2026-06-06). Overwrites any stale marker atomically. */
hu_error_t hu_lora_nightly_write_kto_pending(const char *kto_jsonl_path, size_t signal_count,
                                             int64_t now_unix);

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
