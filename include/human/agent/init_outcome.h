#ifndef HU_AGENT_INIT_OUTCOME_H
#define HU_AGENT_INIT_OUTCOME_H

#include "human/agent/init_proposer.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdint.h>

/* Initiative Layer — outcome capture (T8 slice 1, "what did the model
 * propose?"). See docs/plans/2026-05-25-initiative-layer/.
 *
 * Every non-gated init_proposer tick produces a decision (FIRED,
 * LOW_CONFIDENCE, NEGATIVE, LLM_ERROR, PARSE_ERROR). Slice 1 persists
 * those decisions as JSONL so operators (and a future tuner) can see
 * the proposal record + context that led to it.
 *
 * Slice 2 (deferred) will layer reply-detection on top of the same
 * file: once dry_run=false, for each FIRED proposal, check chat.db for
 * an inbound reply from target_handle within 24h and append an
 * outcome line. The append-only format means slice 2 doesn't have to
 * rewrite slice 1's records.
 *
 * File format: one JSON object per line, no array wrapping. Schema:
 *   {
 *     "schema":      "init_outcome_v1",
 *     "ts_unix":     <int>,            // when the tick happened
 *     "tick_id":     <int>,            // monotonic per process; resets on restart
 *     "verdict":     "FIRED" | "LOW_CONFIDENCE" | "NEGATIVE" |
 *                    "LLM_ERROR" | "PARSE_ERROR",
 *     "confidence":  <float 0..1>,     // 0 when verdict is *_ERROR
 *     "draft":       "<text>",         // empty unless FIRED/LOW_CONFIDENCE
 *     "reason":      "<text>",         // empty unless NEGATIVE
 *     "target":      "<phone or empty>",
 *     "dry_run":     <bool>            // was the daemon in dry-run when this fired?
 *   }
 *
 * Empty/zero fields are still serialized so the record's shape is
 * stable across slices and operators can grep/jq without
 * special-casing. */

/* Append one decision record to the JSONL file. Path is resolved as:
 *   $HOME/.human/initiative_proposals.jsonl
 * Creates parent dirs if missing. Uses fopen("a") so concurrent appends
 * from the same daemon are serialized via stdio's internal lock — good
 * enough for a single-process daemon; multi-process callers would need
 * flock.
 *
 * Returns HU_OK on success. Returns HU_ERR_IO on filesystem failure
 * (logged once via hu_log_warn_once so a chronically full disk doesn't
 * spam the log). Returns HU_ERR_INVALID_ARGUMENT on NULL args.
 *
 * Cost: one fopen+fprintf+fclose per call; <1ms typically. Called at
 * most once per init_proposer tick (every 30 min default). */
hu_error_t hu_init_outcome_append(hu_allocator_t *alloc, int64_t ts_unix, uint64_t tick_id,
                                  hu_init_proposer_result_t verdict,
                                  const hu_init_decision_t *decision, const char *target_handle,
                                  bool dry_run);

/* Resolve the path used by hu_init_outcome_append. The returned string is
 * written into out_buf (NUL-terminated). Returns the number of bytes
 * written (excluding NUL), or 0 on failure. Pure predicate — used by
 * tests + the doctor check to know where the file lives. */
size_t hu_init_outcome_resolve_path(char *out_buf, size_t out_cap);

/* Test-only: override the default $HOME-relative path. Pass NULL to
 * revert to the default. No-op outside HU_IS_TEST. */
void hu_init_outcome_set_path_for_test(const char *path);

/* ──────────────────────────────────────────────────────────────────────────
 * CLI: human initiative <log|status>
 *
 * Read-only operations over the JSONL. Both subcommands are pure (no
 * mutation of the file), so safe to invoke against a live daemon's
 * working file. */

/* `human initiative log [--last N]` — pretty-print last N entries
 * (default 10). `human initiative status` — aggregate counts by
 * verdict + mean confidence + last fire time. Returns HU_OK / non-zero
 * on usage errors. */
hu_error_t cmd_initiative(hu_allocator_t *alloc, int argc, char **argv);

/* Aggregate counters for the status subcommand. Pure predicate over a
 * single JSON line; testable without spinning a real CLI. The caller
 * walks the JSONL file line-by-line and calls this once per line. */
typedef struct hu_init_status {
    size_t total;
    size_t count_fired;
    size_t count_low_confidence;
    size_t count_negative;
    size_t count_llm_error;
    size_t count_parse_error;
    size_t count_unknown_verdict; /* schema drift safety */
    double sum_confidence;        /* divided by total → mean */
    int64_t last_fired_ts_unix;   /* 0 if no FIRED seen */
} hu_init_status_t;

/* Update `status` in place from one JSONL line. Robust to malformed
 * lines (returns without mutating on parse failure). Pure — no I/O,
 * uses the system allocator transiently for the JSON parse. */
void hu_init_outcome_aggregate_line(hu_init_status_t *status, const char *line, size_t line_len);

#endif /* HU_AGENT_INIT_OUTCOME_H */
