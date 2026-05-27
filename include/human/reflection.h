/* include/human/reflection.h — Reflection Loop public API.
 *
 * Periodic batch task that distills accumulated conversations into typed,
 * queryable PATTERNS the agent can act on. Closes M2 (Personal Model)
 * per CLAUDE.md.
 *
 * Spec: docs/plans/2026-05-26-reflection-loop/{design.md,tasks.md}.
 *
 * Architecture (Phase 1):
 *   - `hu_reflection_tick()` — cheap gate, called from daemon main loop.
 *   - `hu_reflection_run()` — orchestrates: assemble transcript → LLM
 *     reflection → parse → store. Idle-gated + interval-gated.
 *   - `hu_reflection_parse()` — pure JSON-validator + stable-id
 *     assigner. Testable without sqlite/provider.
 *   - Query helpers — read patterns out of SQLite for system-prompt
 *     and init_proposer consumption.
 *
 * The header is standalone-compilable; downstream callers don't need
 * sqlite3.h or the daemon header unless they call the relevant API. */

#ifndef HU_REFLECTION_H
#define HU_REFLECTION_H

#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations — avoid pulling sqlite3.h or daemon.h into
 * every consumer just for the prototypes. */
struct hu_daemon;
struct sqlite3;

/* The six pattern types Phase 1 reflection emits. Order is stable
 * (used in the stable-id hash); appending new variants is safe but
 * reordering breaks existing pattern IDs. HU_REFLECTION_PATTERN_COUNT
 * is the sentinel for array sizing — callers iterating must stop
 * BEFORE it. */
typedef enum {
    HU_REFLECTION_PATTERN_TOPIC_RECURRENCE = 0,
    HU_REFLECTION_PATTERN_BEHAVIORAL_SHIFT,
    HU_REFLECTION_PATTERN_PREFERENCE,
    HU_REFLECTION_PATTERN_EMOTIONAL_STATE,
    HU_REFLECTION_PATTERN_SCHEDULE_PATTERN,
    HU_REFLECTION_PATTERN_RELATIONSHIP,
    HU_REFLECTION_PATTERN_COUNT
} hu_reflection_pattern_type_t;

/* One pattern emitted by a reflection run. Buffer sizes are fixed so
 * the struct can be passed by value and arrays of these can live on
 * the stack — keeps the schema parser allocation-free for the common
 * case (a handful of patterns per run). */
typedef struct hu_reflection_pattern {
    /* Stable hash of (type_str, subject, observation[:128]) — first
     * 16 hex chars of SHA-256. Lets storage UPSERT cleanly: same
     * observation re-derived in a later run hits the same row and
     * bumps last_observed_at_ms instead of duplicating. */
    char id[64];

    hu_reflection_pattern_type_t type;

    /* Who or what the pattern is about. "alice", "morning routine",
     * "work stress" — short noun phrase. */
    char subject[128];

    /* What the pattern claims. Full sentence. Bounded at 511 bytes;
     * truncation is silent because the prose is for downstream LLM
     * consumption, not a contract-bearing field. */
    char observation[512];

    /* Self-rated by the reflection model in [0, 1]. Patterns below
     * cfg->reflection.min_confidence (default 0.5) are returned by
     * parse but flagged so storage can drop them — see Layer 2 of
     * the failure-handling section in design.md. */
    double confidence;

    /* Up to 8 turn-id references back to the source conversation,
     * for evidence-trail UI / debugging. Layer 3 of the failure
     * model: if a pattern is challenged, we can show the turns it
     * was derived from. */
    char evidence_ids[8][64];
    int evidence_count;

    /* Which channels this pattern was observed across. Cap of 8 is
     * enough for h-uman's 4 Tier-1 channels (imessage, sms,
     * telegram, discord) plus headroom; the schema parser silently
     * truncates if a reflection model emits more and emits one
     * operator-visible warning. */
    char channels[8][32];
    int channel_count;

    /* Wall-clock metadata. All ms-since-epoch (uint64 sized to
     * outlive int32 wraparound). expires_at_ms encodes the half-
     * life — 30 days default. */
    uint64_t created_at_ms;
    uint64_t last_observed_at_ms;
    uint64_t expires_at_ms;

    /* surfaced_to_user: set by hu_reflection_mark_surfaced when the
     * pattern has been used in a system prompt or init_proposer
     * candidate. Lets the unsurfaced-query API avoid re-recommending
     * the same pattern.
     *
     * retired: set by hu_reflection_retire when the user contradicts
     * the pattern. Distinguishes "pattern is wrong" from "pattern is
     * stale" (expires_at_ms < now). Retired patterns are NEVER
     * resurfaced even if re-derived. */
    bool surfaced_to_user;
    bool retired;
    uint64_t retired_at_ms;
} hu_reflection_pattern_t;

/* Tick — called from daemon main loop every iteration. Cheap (sub-
 * millisecond). Gates internally on cfg->reflection.enabled,
 * min_interval_hours, and idle/daily-floor conditions. */
hu_error_t hu_reflection_tick(struct hu_daemon *d);

/* Force a reflection run. Used by tests and manual operator
 * triggers. `force=true` bypasses the min_interval gate (useful for
 * "I want the latest patterns NOW after a long conversation"); the
 * config-enabled check still applies. */
hu_error_t hu_reflection_run(struct hu_daemon *d, bool force);

/* Parse a reflection model's JSON output into a heap-allocated array
 * of patterns + a prose summary. Pure — no I/O, no provider call.
 *
 * Returns HU_OK on a structurally valid response, even if individual
 * patterns fall below the confidence floor (those are returned but
 * flagged via `confidence < cfg->reflection.min_confidence`; the
 * storage layer drops them).
 *
 * On parse failure: *out_patterns = NULL, *out_count = 0, and
 * *out_error is set to a heap-allocated human-readable error string
 * (caller frees with free()). Non-NULL *out_error means there was
 * an error worth logging even if the function returned HU_OK with a
 * partial parse.
 *
 * *out_patterns is heap-allocated with malloc — caller frees with
 * free() when count > 0. *out_prose_summary is also malloc'd.
 *
 * Stable-id assignment: each pattern's `id` field is computed as the
 * first 16 hex chars of SHA-256(type_str + "|" + subject + "|" +
 * observation[:128]). Same input across runs → same id, so the
 * storage layer's UPSERT is deterministic. */
hu_error_t hu_reflection_parse(const char *json, hu_reflection_pattern_t **out_patterns,
                               int *out_count, char **out_prose_summary, char **out_error);

/* Query patterns for inclusion in the system prompt for `channel`.
 * Returns up to `max_patterns` non-retired, non-expired patterns
 * scoped to the channel. Caller frees `*out_patterns` with free(). */
hu_error_t hu_reflection_query_for_system_prompt(struct sqlite3 *db, const char *channel,
                                                 int max_patterns,
                                                 hu_reflection_pattern_t **out_patterns,
                                                 int *out_count);

/* Query patterns NOT yet surfaced to the user with confidence
 * ≥ min_confidence. Used by init_proposer as a candidate source.
 * Caller frees `*out_patterns` with free(). */
hu_error_t hu_reflection_query_unsurfaced(struct sqlite3 *db, double min_confidence,
                                          hu_reflection_pattern_t **out_patterns, int *out_count);

/* Mark a pattern as surfaced. Side-effecting; idempotent. */
void hu_reflection_mark_surfaced(struct sqlite3 *db, const char *pattern_id);

/* Retire a pattern (user contradicted it). Side-effecting; idempotent.
 * Retired patterns are NEVER resurfaced even if re-derived in a later
 * run — the storage UPSERT preserves the retired flag. */
void hu_reflection_retire(struct sqlite3 *db, const char *pattern_id);

/* Quorum predicate for Phase 2 (belief updates). Returns true iff
 * `pattern_id` has been observed in ≥ 3 distinct runs with
 * confidence > 0.7 in each. Phase 1 callers may use this for
 * logging/telemetry only — MUST NOT mutate personal_model on it
 * (mutation is Phase 2's contract). */
bool hu_reflection_pattern_has_quorum(struct sqlite3 *db, const char *pattern_id);

/* Convert the enum to the lower-snake-case string used in the
 * stable-id hash AND in the JSON schema's "type" field. Stable
 * across releases (changing breaks existing pattern IDs). */
const char *hu_reflection_pattern_type_str(hu_reflection_pattern_type_t type);

/* Compute the stable pattern id for a (type, subject, observation)
 * triple into `out_id` (must have capacity ≥ 17 — 16 hex + NUL).
 * First 16 hex chars of SHA-256("type_str|subject|observation[:128]").
 * Same input → same id across processes, machines, releases. Storage
 * uses this id as the UPSERT key; changing the canonicalization rule
 * would break every existing reflection_patterns row. */
void hu_reflection_compute_id(hu_reflection_pattern_type_t type, const char *subject,
                              const char *observation, char *out_id, size_t id_cap);

/* ── Storage (T2) ─────────────────────────────────────────────────
 *
 * Two-table SQLite schema:
 *   reflection_runs(run_id, provider, started_at_ms, completed_at_ms,
 *                   input_turns, output_tokens, status, error_message,
 *                   json_dump_path, prose_summary,
 *                   low_confidence_dropped_count)
 *   reflection_patterns(id, type, subject, observation, confidence,
 *                       evidence_json, channels_json,
 *                       first_seen_run_id, last_seen_run_id,
 *                       observation_count, created_at_ms,
 *                       last_observed_at_ms, expires_at_ms,
 *                       surfaced_to_user, retired, retired_at_ms)
 *
 * All functions take `struct sqlite3 *` (forward-declared above) so
 * callers don't need to drag in <sqlite3.h> unless they call these.
 * UPSERT semantics: same pattern id re-derived in a later run bumps
 * observation_count and takes MAX(old_confidence, new_confidence). */

/* Create tables + indexes if missing. Idempotent. */
hu_error_t hu_reflection_storage_migrate(struct sqlite3 *db);

/* Insert a new in-progress run row. status='in_progress',
 * completed_at_ms=NULL until hu_reflection_storage_complete_run is
 * called. */
hu_error_t hu_reflection_storage_insert_run(struct sqlite3 *db, const char *run_id,
                                            const char *provider, uint64_t started_at_ms,
                                            int input_turns);

/* Mark a run complete. `status` is one of: "ok", "schema_invalid",
 * "provider_error", "abandoned". `prose_summary`, `json_dump_path`,
 * `error_message` may be NULL. */
hu_error_t hu_reflection_storage_complete_run(struct sqlite3 *db, const char *run_id,
                                              const char *status, int output_tokens,
                                              const char *prose_summary, const char *json_dump_path,
                                              const char *error_message,
                                              int low_confidence_dropped_count);

/* UPSERT a pattern. If pattern->confidence < 0.5, returns HU_OK
 * without inserting (caller may bump low_confidence_dropped_count
 * via complete_run). Otherwise inserts new row OR bumps existing
 * row's observation_count and takes MAX confidence. */
hu_error_t hu_reflection_storage_upsert(struct sqlite3 *db, const char *run_id,
                                        const hu_reflection_pattern_t *pattern);

/* Returns MAX(completed_at_ms) across rows WHERE status='ok'. Used
 * by the tick gate to decide whether the min_interval has elapsed.
 * Returns 0 if no completed runs exist yet. */
uint64_t hu_reflection_storage_last_completed_ms(struct sqlite3 *db);

#ifdef __cplusplus
}
#endif

#endif /* HU_REFLECTION_H */
