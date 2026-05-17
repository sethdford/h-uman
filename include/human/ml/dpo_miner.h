/* dpo_miner — mine DPO preference pairs from chat.db user-correction triples.
 *
 * Sprint 7 US-7.2 (Option (b) per D2). The miner reuses the existing
 * `user(N) -> assistant(N+1) -> user(N+2)` correction signal that
 * `hu_training_data_extract_dpo` already detects in the `messages` table,
 * but adds three things the existing extractor lacks:
 *
 *   1. `hu_pii_redact` pass on every prompt/chosen/rejected before insert.
 *   2. Content-hash dedup via a `dpo_pair_hashes` tracking table — same
 *      triple is never recorded twice even across multiple miner runs.
 *   3. A `source = "outbound_edit"` tag so downstream DPO training can
 *      distinguish miner-sourced pairs from feedback/retry-sourced pairs.
 *
 * Persistence boundary: the caller owns the `sqlite3*` handle. The miner is
 * read-only against `messages` and writes only to `dpo_pairs` and the new
 * `dpo_pair_hashes` table. Tests pass an in-memory DB (`:memory:`).
 *
 * Known coverage gap (per US-7.2 design §6 R2): `hu_pii_redact` covers
 * structured PII (email/phone/SSN/CC/IP/secrets) but NOT bare contact
 * names. Named-entity redaction is out of scope for this sprint.
 *
 * Concurrency note (per US-7.2 design §6 R3): `dpo_pairs` has no DB-level
 * UNIQUE constraint, so a miner run racing the agent's feedback/retry
 * recorder can land near-simultaneous inserts. The miner's content-hash
 * table guards against the miner inserting twice, NOT against cross-source
 * races. Those rows carry distinct `source` tags and are intentionally
 * preserved as additional signal. */

#ifndef HU_ML_DPO_MINER_H
#define HU_ML_DPO_MINER_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Options for a single miner run.
 *
 * `correction_window_sec`: max seconds between assistant turn N+1 and user
 * followup N+2 for the triple to be treated as a correction. Defaults to
 * `HU_DPO_CORRECTION_WINDOW_SEC` (300) when set to 0 or negative. */
typedef struct hu_dpo_mine_opts {
    int correction_window_sec;
    size_t max_rows; /* 0 = mine everything; otherwise process at most N triples */
} hu_dpo_mine_opts_t;

/* Per-run stats; opaque to the caller except for the count fields. */
typedef struct hu_dpo_mine_stats {
    size_t triples_examined;   /* SELECT returned N rows */
    size_t pairs_recorded;     /* new rows inserted into dpo_pairs */
    size_t pairs_skipped_dup;  /* skipped because (prompt,chosen,rejected) already mined */
    size_t pairs_skipped_size; /* skipped because redacted field overflows pair caps */
    size_t pii_redactions;     /* total redactions performed (informational) */
} hu_dpo_mine_stats_t;

/* Ensure the parent directory of `file_path` exists, creating it (and any
 * intermediate components) with mode 0700 if missing. Returns HU_OK on
 * success, HU_ERR_INVALID_ARGUMENT for NULL/empty input, HU_ERR_IO for
 * filesystem errors that aren't EEXIST.
 *
 * Exposed (rather than left static in dpo_miner.c) so the test suite can
 * exercise the directory-creation logic without going through the CLI —
 * the CLI itself short-circuits under HU_IS_TEST so this helper is the
 * unit-test seam. Used by `human ml mine-corrections` before calling
 * `hu_dpo_export_jsonl` so `~/.human/dpo/pairs.jsonl` lands cleanly even
 * on a fresh install where `~/.human/dpo/` does not yet exist. */
hu_error_t hu_dpo_miner_ensure_parent_dir(const char *file_path);

/* Resolved export decision after parsing flags + applying defaults.
 * `should_export` is true when the miner should write JSONL; otherwise the
 * export step is skipped entirely. `export_path` (when non-NULL) is the
 * absolute path that will be used, with `~` expanded via the supplied
 * `home_dir`. */
typedef struct hu_dpo_miner_export_decision {
    int should_export;
    char export_path[512];
} hu_dpo_miner_export_decision_t;

/* Resolve the export-path decision from the three CLI flag states:
 *   `export_flag_path`         — value of --export-jsonl, NULL if unset.
 *   `no_export_flag_set`       — non-zero if --no-export was passed.
 *   `home_dir`                 — value of $HOME for default-path resolution.
 *                                Pass NULL to simulate "HOME not set" — in
 *                                that case the default path is unavailable
 *                                and `should_export` will be 0 unless
 *                                `export_flag_path` is set.
 *
 * Behavior matrix (test seam for HIGH-1 fix):
 *   --no-export                       → should_export = 0
 *   --export-jsonl X                  → should_export = 1, path = X
 *   --export-jsonl X --no-export      → last-wins via flag order; the CLI
 *                                       loop handles this. This helper
 *                                       receives only the final state.
 *   (neither flag)                    → should_export = 1, path = default
 *
 * Returns HU_OK on success, HU_ERR_INTERNAL if the default path overflows
 * the 512-byte buffer. */
hu_error_t hu_dpo_miner_resolve_export(const char *export_flag_path, int no_export_flag_set,
                                       const char *home_dir, hu_dpo_miner_export_decision_t *out);

#ifdef HU_ENABLE_SQLITE
/* Mine correction pairs from the open SQLite handle.
 *
 * Reads `messages` (id, session_id, role, content, created_at) and writes
 * `dpo_pairs` + `dpo_pair_hashes`. Both write tables are CREATE IF NOT
 * EXISTS — safe to call against a fresh DB or a populated one.
 *
 * On success, `*stats` (when non-NULL) carries per-run counters. */
hu_error_t hu_dpo_mine_corrections(hu_allocator_t *alloc, sqlite3 *db,
                                   const hu_dpo_mine_opts_t *opts, hu_dpo_mine_stats_t *stats);
#endif

#ifdef __cplusplus
}
#endif

#endif /* HU_ML_DPO_MINER_H */
