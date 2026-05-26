/* include/human/ml/init_dpo_bridge.h
 *
 * Initiative-layer → DPO pairs bridge.
 *
 * Closes the M2 learning loop: every time the proposer FIRES a draft AND
 * the user either replies (within HU_INIT_OUTCOME_REPLY_WINDOW_SECS) or
 * ignores it past that window, this bridge records the outcome as a
 * single-sided DPO row in the dpo_pairs SQLite table.
 *
 * Single-sided shape mirrors the reaction_handler precedent at
 * src/ml/dpo.c:88-99 — REPLIED → chosen=draft (with rejected=""),
 * IGNORED → rejected=draft (with chosen=""). The reader filter at
 * hu_dpo_iterate_pairs currently skips single-sided rows for ORPO
 * training; a future pairing pass (proposal_ts → context join) can
 * convert these signals into true preference pairs without a schema
 * change.
 *
 * source="init_proposer_v1" lets the read side filter on this signal
 * independently of tapback rows or other reaction sources.
 *
 * GATING: this module is compiled only when HU_ENABLE_ML is defined.
 * The init_outcome resolver call site is `#ifdef HU_ENABLE_ML`-wrapped
 * with an `#else` one-shot log so operators see "bridge inactive — ML
 * disabled at build time".
 *
 * See docs/plans/2026-05-25-doctor-prompt-budget-initiative/ Part B.
 */
#ifndef HU_ML_INIT_DPO_BRIDGE_H
#define HU_ML_INIT_DPO_BRIDGE_H

#ifdef HU_ENABLE_ML

#include "human/agent/init_outcome.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdint.h>

/* Stable source-tag for every single-sided row this bridge writes. The
 * read side (or a doctor audit) can `SELECT * FROM dpo_pairs WHERE
 * source = ?` to isolate the init-proposer signal. */
#define HU_INIT_DPO_BRIDGE_SOURCE "init_proposer_v1"

/* Stable source-tag for paired rows produced by hu_init_dpo_bridge_pair_singles.
 * Paired rows carry BOTH chosen and rejected populated → they pass the
 * 4-byte-minimum read filter at hu_dpo_iterate_pairs and become eligible
 * for ORPO/DPO training. Distinct tag lets readers filter / audit
 * separately from single-sided input. */
#define HU_INIT_DPO_BRIDGE_PAIRED_SOURCE "init_proposer_paired_v1"

/* Sentinel margin value applied to the two single-sided ROWS that fed
 * a successful pair. The pairing pass uses this to mark already-paired
 * rows so re-running the pass is idempotent (rows with margin == -1.0
 * are skipped). Distinct from any real DPO margin (which lives in
 * [0.0, 1.0]). */
#define HU_INIT_DPO_BRIDGE_PAIRED_MARGIN_SENTINEL (-1.0)

/* Register the daemon's hu_dpo_collector_t with the bridge. Should be
 * called once at daemon init (after `hu_dpo_collector_create` succeeds).
 * Passing NULL clears the registration — useful for tests that want
 * back-to-back fixtures with different collectors.
 *
 * The bridge does NOT own the collector — it borrows the pointer. The
 * daemon is responsible for `hu_dpo_collector_deinit` at shutdown. */
struct hu_dpo_collector;
void hu_init_dpo_bridge_set_collector(struct hu_dpo_collector *collector);

/* Test-only accessor used by the bridge tests to confirm the singleton
 * landed at the expected pointer. Safe to call in production but not
 * intended for normal use. */
struct hu_dpo_collector *hu_init_dpo_bridge_get_collector(void);

/* Record one resolved init_proposer outcome as a single-sided DPO row.
 *
 * Inputs:
 *   alloc        — allocator for transient JSON / SQL bind buffers
 *   outcome      — must be REPLIED or IGNORED (PENDING is a no-op,
 *                  returns HU_ERR_INVALID_ARGUMENT)
 *   draft        — the FIRED draft text (NUL-terminated, ≤ 1024 chars
 *                  per pending_proposal_t's bound)
 *   target       — phone or handle the proposal was aimed at; included
 *                  in the prompt template
 *   resolution_ts — the wall-clock instant the resolution was decided
 *
 * Returns:
 *   HU_OK on successful dpo_pairs insert
 *   HU_ERR_INVALID_ARGUMENT on PENDING outcome or NULL draft
 *   HU_ERR_NOT_SUPPORTED if no dpo_collector is available at runtime
 *                        (SQLite disabled, collector not created — the
 *                        resolver logs this once via warn_once and
 *                        keeps going; the outcome JSONL line is the
 *                        authoritative record, the dpo row is derived
 *                        signal)
 *   HU_ERR_IO on SQLite failure (caller handles; resolver does NOT
 *             retry — losing a single derived row is acceptable; the
 *             JSONL line is the source of truth)
 *
 * Cost: one INSERT into dpo_pairs (~1-10 ms typically). Bounded by the
 * resolver's cadence (every proposer tick, default 30 min). */
hu_error_t hu_init_dpo_bridge_record(hu_allocator_t *alloc, hu_init_resolution_t outcome,
                                     const char *draft, const char *target, int64_t resolution_ts);

/* ──────────────────────────────────────────────────────────────────────────
 * Pairing pass — convert accumulated single-sided init_proposer rows into
 * true two-sided preference pairs that the read filter at
 * hu_dpo_iterate_pairs will surface for ORPO/DPO training.
 *
 * Algorithm (idempotent on re-run):
 *   1. SELECT id, prompt, chosen, rejected, timestamp FROM dpo_pairs
 *      WHERE source = HU_INIT_DPO_BRIDGE_SOURCE
 *        AND margin != HU_INIT_DPO_BRIDGE_PAIRED_MARGIN_SENTINEL
 *      ORDER BY timestamp;
 *   2. Extract target_handle from each prompt (the bridge prompt format
 *      is "proactive-proposal: target=<handle> ts=<unix>").
 *   3. For each (target_handle) bucket, take the most-recent REPLIED row
 *      (chosen non-empty, rejected empty) and pair it with the
 *      most-recent IGNORED row (chosen empty, rejected non-empty) where
 *      the IGNORED ts < REPLIED ts (we want the "they DIDN'T like that
 *      draft, then they DID like this one" gradient).
 *   4. INSERT a new row: source=PAIRED_SOURCE, prompt=concat of source
 *      prompts, chosen=REPLIED draft, rejected=IGNORED draft, margin=1.0,
 *      timestamp=now.
 *   5. UPDATE both source rows: SET margin =
 *      HU_INIT_DPO_BRIDGE_PAIRED_MARGIN_SENTINEL (idempotency mark).
 *
 * Returns HU_OK on success (paired_count populated). Returns
 * HU_ERR_NOT_SUPPORTED if no collector is registered. Returns HU_ERR_IO
 * on SQLite failure (paired_count reflects rows successfully paired
 * before the error).
 *
 * Safe to call repeatedly. Each call only consumes rows that haven't
 * been paired yet.
 *
 * Cost: O(N) walk over single-sided rows + one INSERT and two UPDATEs
 * per pair. For ~hundreds of rows the call is sub-second. */
hu_error_t hu_init_dpo_bridge_pair_singles(hu_allocator_t *alloc, size_t *paired_count);

#endif /* HU_ENABLE_ML */

#endif /* HU_ML_INIT_DPO_BRIDGE_H */
