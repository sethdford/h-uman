#ifndef HU_M3_FRONTIER_ADAPTER_H
#define HU_M3_FRONTIER_ADAPTER_H

/* M3 — Frontier persona adapter (stub / fixture path).
 *
 * Track D vertical slice: prove we can **load** a versioned placeholder
 * descriptor from disk and run a **no-op inference** hook without network.
 * Real GGUF / llama.cpp / MLX wiring replaces the file format later; this
 * header is the stable seam tests and the agent can depend on. */

#include "human/core/allocator.h"
#include "human/core/error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Track D D1.3 — single source of truth for the rollback decision.
 * Returns true when the M3 frontier-bridge attach should be skipped:
 *   1. `cfg_disabled` matches the parsed value of
 *      `personalization.m3_adapter_disabled`. Pass `false` when no
 *      config is available.
 *   2. The `HUMAN_M3_ADAPTER_DISABLE` env var, when set to anything
 *      other than empty / "0", forces-disable regardless of config —
 *      the operational kill switch.
 *
 * Pure: no side effects beyond reading the env var. Safe to call
 * before bootstrap, in tests, and from anywhere a caller wants to
 * decide whether to attach the bridge. */
bool hu_m3_adapter_should_disable(bool cfg_disabled);

typedef struct hu_m3_frontier_adapter hu_m3_frontier_adapter_t;

/* On-disk magic for fixture adapters (8 bytes, no NUL). */
#define HU_M3_ADAPTER_MAGIC "HU_M3AD\x01"

/* Try to open a stub adapter from `path` (NUL-terminated or `path_len` bytes).
 * Returns HU_ERR_IO when the file is missing or the header does not match.
 * On success, `*out` is owned; free with `hu_m3_frontier_adapter_close`. */
hu_error_t hu_m3_frontier_adapter_try_open(hu_allocator_t *alloc, const char *path, size_t path_len,
                                           hu_m3_frontier_adapter_t **out);

/* Deterministic probe "inference" — always HU_OK; increments an internal
 * call counter on the adapter (observable via
 * `hu_m3_frontier_adapter_probe_count`). Replaces the older
 * `hu_m3_frontier_adapter_noop_infer`, which silently returned HU_OK with
 * no side effect — meaning a regression that dropped one of the 11
 * provider-success call sites would be undetectable at the test layer.
 *
 * The counter is a *signal*, not a model: no tensors, no learning, no
 * gradient. It exists so:
 *   1. A test can pin "the chat path actually reaches the M3 hook"
 *      (see tests/test_m3_frontier_probe.c) instead of trusting that
 *      a (void)return; means the wiring works.
 *   2. The eventual real-tensor implementation has a known-good seam
 *      to slot under — replace the body of `probe_infer` with the
 *      tensor call; the counter side effect can stay or go.
 *
 * Backwards-compat: `hu_m3_frontier_adapter_noop_infer` is preserved as
 * a thin wrapper that calls `probe_infer` and discards the count delta,
 * so the agent-side `hu_agent_m3_on_provider_success` callers do not
 * need to be re-edited for this slice. See
 * docs/plans/2026-05-17-m3-mlx-bridge-execution-plan.md Phase B-pre. */
hu_error_t hu_m3_frontier_adapter_probe_infer(hu_m3_frontier_adapter_t *adapter);
hu_error_t hu_m3_frontier_adapter_noop_infer(hu_m3_frontier_adapter_t *adapter);

/* Read-only: number of times probe_infer (or noop_infer) was called on
 * this adapter since open. Zero for NULL. Test-observable seam. */
uint64_t hu_m3_frontier_adapter_probe_count(const hu_m3_frontier_adapter_t *adapter);

void hu_m3_frontier_adapter_close(hu_allocator_t *alloc, hu_m3_frontier_adapter_t *adapter);

/* Read-only: schema version from the opened file (0 if NULL). */
uint32_t hu_m3_frontier_adapter_schema_version(const hu_m3_frontier_adapter_t *adapter);

/* Track D D2.1 — user-facing honest-gap caveat strings.
 *
 * The `human ml lora-persona` command emits these strings at training
 * start (and from `--help`). They make explicit that the LoRA path
 * trains a reference HUML GPT, NOT the frontier model the user
 * actually chats with. Centralizing them here (rather than embedding
 * literal printfs in cli.c) means:
 *
 *   - Tests can pin the substrings that matter (no silent drift to
 *     overclaiming language during refactors).
 *   - The caveat doc path is a single constant, so when the path
 *     changes (rare) only one place updates.
 *
 * Thread-safe and allocator-free — both functions return pointers
 * to static storage. */

/* Path to the honest-gap planning doc, relative to repo root. */
const char *hu_ml_lora_persona_caveat_doc_path(void);

/* Multi-line caveat block printed before training starts. Lines are
 * `\n`-terminated and each is prefixed with `[lora-persona]` so the
 * block aligns visually with the rest of the CLI output. */
const char *hu_ml_lora_persona_caveat_block(void);

/* ─────────────────────────────────────────────────────────────────────
 * Phase B1 (redefined 2026-05-17): inference outcome capture
 *
 * The probe counter from Phase B-pre proves the chat-path hooks REACH
 * the adapter. This phase captures WHAT they reach with: a structured
 * outcome record per inference, accumulated in a ring buffer the
 * future training loop can pull from.
 *
 * Why a ring buffer:
 *   - O(1) append under lock contention from concurrent turn paths.
 *   - Fixed memory ceiling (~384 KB at 4096 capacity).
 *   - Training reads via batched snapshot; no per-record locks.
 *
 * Why hashes, not raw content:
 *   - Privacy: structural — the adapter's RAM never holds raw
 *     prompt/response text. The conversation DB (under proper memory
 *     governance) remains the canonical source of content.
 *   - Dedup: 64-bit hashes let the training loop drop duplicate-prompt
 *     samples without re-reading the DB.
 *   - Correlation: contact_id_hash links an outcome to a per-contact
 *     training partition without leaking contact identity in RAM.
 * ───────────────────────────────────────────────────────────────── */

#define HU_M3_OUTCOMES_RING_CAPACITY 4096u
#define HU_M3_OUTCOME_RECORD_BYTES   96u

typedef enum hu_m3_guard_decision {
    HU_M3_GUARD_UNKNOWN = 0,
    HU_M3_GUARD_PASS = 1,
    HU_M3_GUARD_REWRITE = 2,
    HU_M3_GUARD_REJECT = 3,
} hu_m3_guard_decision_t;

typedef struct hu_m3_inference_outcome {
    uint64_t timestamp_unix_ms; /* wall clock at record time */
    uint64_t latency_ms;        /* inference duration */
    uint64_t prompt_hash;       /* FNV-1a of the system+user prompt */
    uint64_t response_hash;     /* FNV-1a of the model's output */
    uint64_t contact_id_hash;   /* FNV-1a of contact_id; 0 = no contact */
    uint32_t prompt_tokens;     /* prompt token count if known; 0 = unknown */
    uint32_t completion_tokens; /* completion token count if known */
    uint16_t model_id;          /* small id of active model (config-mapped) */
    uint16_t adapter_id;        /* small id of active LoRA adapter; 0 = none */
    uint8_t guard_decision;     /* hu_m3_guard_decision_t cast to byte */
    uint8_t turn_kind;          /* 0 = unknown, 1 = stream, 2 = batch, 3 = proactive */
    uint8_t reserved[38];       /* keep record exactly 96 bytes (8-aligned) */
} hu_m3_inference_outcome_t;

/* Record an outcome into the ring. NULL adapter is a no-op (returns
 * HU_OK) so callers don't need to gate. The record is COPIED. */
hu_error_t hu_m3_frontier_adapter_record_outcome(hu_m3_frontier_adapter_t *adapter,
                                                 const hu_m3_inference_outcome_t *outcome);

/* Total outcomes ever recorded (monotonic, NOT capped to ring size).
 * Useful for "the hook fired N times since boot" assertions. */
uint64_t hu_m3_frontier_adapter_outcomes_recorded(const hu_m3_frontier_adapter_t *adapter);

/* Snapshot up to `max_count` most-recent outcomes into `out_buf`, oldest
 * first within the snapshot. Writes count into *out_count. NULL adapter
 * writes 0 and returns HU_OK. */
hu_error_t hu_m3_frontier_adapter_snapshot_outcomes(const hu_m3_frontier_adapter_t *adapter,
                                                    hu_m3_inference_outcome_t *out_buf,
                                                    size_t max_count, size_t *out_count);

/* Pure helper: deterministic FNV-1a 64-bit hash. Used by the agent
 * paths to compute prompt_hash / response_hash without depending on
 * the daemon's hash module. */
uint64_t hu_m3_outcome_hash_bytes(const void *data, size_t len);

/* ─────────────────────────────────────────────────────────────────────
 * Phase B3 v0 (2026-05-17 round 2): outcome export for the training loop
 *
 * The training loop runs in a separate process (Python typically). It
 * needs to PULL outcomes from the running daemon. We expose them as
 * JSONL (one outcome per line) — the natural shape for streaming
 * ingest by ML pipelines.
 *
 * The HTTP endpoint at GET /v1/m3/outcomes wraps these helpers; tests
 * exercise the serializer directly without needing the gateway.
 * ───────────────────────────────────────────────────────────────── */

/* Filter passed to `hu_m3_outcomes_to_jsonl`. Each field has a "no
 * filter" sentinel so callers can leave the field unset. */
typedef struct hu_m3_outcomes_filter {
    /* Maximum outcomes to include. 0 = no limit (return everything in
     * the live ring). Capped at HU_M3_OUTCOMES_RING_CAPACITY. */
    size_t max_count;
    /* Only include outcomes with this turn_kind value. 0 = no filter
     * (turn_kind 0 outcomes don't exist in production — turn_kind is
     * set to 1/2/3 at record time). */
    uint8_t turn_kind;
    /* Only include outcomes with timestamp_unix_ms >= since_ms. 0 =
     * no filter. */
    uint64_t since_ms;
} hu_m3_outcomes_filter_t;

/* Serialize matching outcomes from the adapter's ring into a JSONL
 * buffer (one JSON object per line, '\n' separator, no trailing
 * newline). `*out_buf` is allocator-owned; free with
 * `alloc->free(alloc->ctx, *out_buf, *out_cap)` using the returned
 * capacity (NOT *out_len + 1 — the allocator contract requires the
 * exact allocation size).
 *
 * Returns:
 *   HU_OK with *out_len > 0 — one or more outcomes serialized.
 *   HU_OK with *out_len == 0 — no outcomes matched the filter.
 *   HU_ERR_INVALID_ARGUMENT on NULL alloc / out_buf / out_len /
 *     out_cap.
 *   HU_ERR_OUT_OF_MEMORY if the allocation fails.
 *
 * The adapter parameter may be NULL — in that case *out_len = 0,
 * returns HU_OK. Lets the gateway expose the endpoint cleanly even
 * when no adapter is attached. */
hu_error_t hu_m3_outcomes_to_jsonl(hu_allocator_t *alloc, const hu_m3_frontier_adapter_t *adapter,
                                   const hu_m3_outcomes_filter_t *filter, char **out_buf,
                                   size_t *out_len, size_t *out_cap);

/* Process-global accessor used by the gateway endpoint.
 *
 * The daemon registers its agent's M3 adapter once at boot (after
 * `hu_agent_m3_adapter_attach` succeeds) so the gateway endpoint can
 * snapshot outcomes without holding an agent pointer. Single adapter
 * per process — there's only ever one M3 personalization loop active.
 *
 * Pass NULL to clear (e.g. on agent teardown). Reads are
 * atomic-enough for the gateway's single-writer/many-reader pattern;
 * a stale pointer at teardown returns no outcomes rather than crashes
 * because the serializer treats NULL adapter as "no outcomes". */
void hu_m3_outcomes_register_global_adapter(hu_m3_frontier_adapter_t *adapter);
hu_m3_frontier_adapter_t *hu_m3_outcomes_global_adapter(void);

/* ─────────────────────────────────────────────────────────────────────
 * Spec 1 Task 7 (AC-M3-4): outcome-ring population from provider results.
 *
 * This is the seam the spec asks for in design.md — `hu_agent_m3_*`
 * pre-existing helpers already populate the ring at 11 call sites in
 * agent_turn.c + agent_stream.c via `hu_agent_m3_record_chat_outcome`.
 * That path needs full prompt+response bytes (hashes are computed from
 * them).
 *
 * This NEW helper records an outcome from a provider-result-only view
 * (token_count, latency_ms, contact_hash) — the data the streaming
 * path has at first-chunk receipt time before the response is fully
 * accumulated. It writes directly into the ring buffer so production
 * paths can call it at points where the full text isn't yet known.
 *
 * NULL adapter is a no-op (returns HU_OK), matching the rest of the
 * outcome ring API. */
hu_error_t hu_m3_record_outcome_from_provider_result(
    hu_m3_frontier_adapter_t *adapter, uint64_t timestamp_unix_ms, uint32_t prompt_tokens,
    uint32_t completion_tokens, uint64_t latency_ms, uint64_t contact_hash, uint8_t turn_kind);

/* ─────────────────────────────────────────────────────────────────────
 * Spec 1 Task 8 (AC-M3-4 expanded): outcome-ring drainer.
 *
 * The outcome ring is a circular write-buffer: at 4096 capacity it wraps
 * silently and the dropped records are lost. Without a production
 * drainer, populating the ring is a half-fix — the training loop can
 * only see at most the last 4096 outcomes, and if the daemon serves
 * more than that between training runs the data falls off.
 *
 * The drainer reads outcomes via snapshot, persists them to SQLite, and
 * advances a per-process "drain marker" so the next drain only persists
 * NEWLY-RECORDED outcomes. The marker is `total_recorded` at last drain.
 *
 * Daemon-tick semantics (silent-config-gated-subsystems.md):
 *   - The tick is interval-gated; the operator-visible log line on first
 *     enabled tick names the interval; a separate once-guarded log fires
 *     when called with a NULL db (so a misconfigured daemon doesn't
 *     silently drop outcomes).
 * ───────────────────────────────────────────────────────────────── */

/* Read-only: total outcomes drained so far (monotonic across process
 * lifetime, NOT capped to ring size). NULL adapter returns 0. */
uint64_t hu_m3_frontier_adapter_drain_marker(const hu_m3_frontier_adapter_t *adapter);

/* Advance the drain marker to `recorded_through` (typically the value of
 * `hu_m3_frontier_adapter_outcomes_recorded` at the time the caller
 * snapshotted). The marker is monotonic — values <= current marker are
 * silently ignored to keep concurrent drainers from rolling it back. */
void hu_m3_frontier_adapter_advance_drain_marker(hu_m3_frontier_adapter_t *adapter,
                                                 uint64_t recorded_through);

#ifdef HU_ENABLE_SQLITE
struct sqlite3;
typedef struct sqlite3 sqlite3;

/* Create the m3_outcomes table + index if missing. Schema:
 *   CREATE TABLE m3_outcomes(
 *     id INTEGER PRIMARY KEY AUTOINCREMENT,
 *     timestamp_unix_ms INTEGER NOT NULL,
 *     latency_ms        INTEGER NOT NULL,
 *     prompt_hash       INTEGER NOT NULL,
 *     response_hash     INTEGER NOT NULL,
 *     contact_id_hash   INTEGER NOT NULL,
 *     prompt_tokens     INTEGER NOT NULL,
 *     completion_tokens INTEGER NOT NULL,
 *     model_id          INTEGER NOT NULL,
 *     adapter_id        INTEGER NOT NULL,
 *     guard_decision    INTEGER NOT NULL,
 *     turn_kind         INTEGER NOT NULL,
 *     drained_ts_ms     INTEGER NOT NULL
 *   );
 *   CREATE INDEX idx_m3_outcomes_timestamp ON m3_outcomes(timestamp_unix_ms);
 *
 * Idempotent — safe to call on every daemon boot. */
hu_error_t hu_m3_outcomes_init_table(sqlite3 *db);

/* Drain newly-recorded outcomes from the adapter's ring into the
 * m3_outcomes table. Reads at most `max_outcomes` records (0 == use
 * default of HU_M3_OUTCOMES_RING_CAPACITY); persists records whose
 * "position" in the monotonic total_recorded sequence is strictly
 * greater than the current drain marker; advances the marker.
 *
 * `now_ts_ms` is the wall-clock for the drained_ts_ms column.
 * `*out_drained` receives the count of rows actually persisted.
 *
 * Returns:
 *   HU_OK on success (including the no-new-outcomes case where
 *     *out_drained == 0).
 *   HU_ERR_INVALID_ARGUMENT on NULL db.
 *   HU_ERR_OUT_OF_MEMORY if the snapshot buffer can't be allocated.
 *   HU_ERR_IO on a SQLite insert failure.
 *
 * Safe to call with a NULL adapter (becomes a no-op with
 * *out_drained == 0). */
hu_error_t hu_m3_drain_outcomes_to_sqlite(hu_m3_frontier_adapter_t *adapter, sqlite3 *db,
                                          int64_t now_ts_ms, hu_allocator_t *alloc,
                                          size_t max_outcomes, int64_t *out_drained);

/* Daemon-tick wrapper. Interval-gated: the drain runs at most once per
 * `interval_seconds`; calls in between return HU_OK without touching
 * the db. `last_run_ts_ms_inout` is updated only when the drain
 * actually runs.
 *
 * The first-enabled tick emits an hu_log_info_once landmark naming the
 * interval. NULL db short-circuits to HU_OK with a separate once-guard
 * warning so a misconfigured daemon is observable rather than silent.
 *
 * Returns the same error class as `hu_m3_drain_outcomes_to_sqlite`. */
hu_error_t hu_daemon_tick_m3_outcome_drain(hu_m3_frontier_adapter_t *adapter, sqlite3 *db,
                                           int64_t now_ts_ms, int64_t *last_run_ts_ms_inout,
                                           int64_t interval_seconds, hu_allocator_t *alloc,
                                           int64_t *out_drained);

#if HU_IS_TEST
/* Reset the daemon-tick once-guards so a test can re-arm the
 * first-enabled / null-db warnings. */
void hu_daemon_tick_m3_outcome_drain_reset_warn_guards_for_test(void);
#endif

#endif /* HU_ENABLE_SQLITE */

#ifdef __cplusplus
}
#endif

#endif /* HU_M3_FRONTIER_ADAPTER_H */
