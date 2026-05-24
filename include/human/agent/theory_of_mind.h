#ifndef HU_THEORY_OF_MIND_H
#define HU_THEORY_OF_MIND_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HU_TOM_MAX_BELIEFS      64
#define HU_TOM_MAX_EXPECTATIONS 32

typedef enum hu_belief_type {
    HU_BELIEF_KNOWS,    /* contact knows this fact */
    HU_BELIEF_ASSUMES,  /* contact likely assumes this */
    HU_BELIEF_UNAWARE,  /* contact doesn't know this yet */
    HU_BELIEF_MISTAKEN, /* contact has wrong info about this */
} hu_belief_type_t;

/* Direction of belief modeling */
typedef enum hu_tom_belief_direction {
    HU_TOM_AI_ABOUT_USER, /* what the AI believes about the user (existing) */
    HU_TOM_USER_ABOUT_AI, /* what the user expects the AI to know */
} hu_tom_belief_direction_t;

/* What the user expects the AI to know */
typedef enum hu_tom_expected_knowledge {
    HU_TOM_EXPECT_REMEMBERS,   /* user expects AI remembers a fact */
    HU_TOM_EXPECT_UNDERSTANDS, /* user expects AI understands a concept */
    HU_TOM_EXPECT_TRACKS,      /* user expects AI tracks an ongoing topic */
} hu_tom_expected_knowledge_t;

/* A user expectation about what the AI should know */
typedef struct hu_tom_expectation {
    char *topic;
    size_t topic_len;
    hu_tom_expected_knowledge_t knowledge_type;
    int64_t recorded_at;
} hu_tom_expectation_t;

/* A gap where user expects knowledge the AI doesn't have */
typedef struct hu_tom_gap {
    char *topic;
    size_t topic_len;
    hu_tom_expected_knowledge_t knowledge_type;
} hu_tom_gap_t;

typedef struct hu_tom_belief {
    char *topic;
    size_t topic_len;
    hu_belief_type_t type;
    float confidence; /* 0.0-1.0 */
    int64_t last_updated;
} hu_tom_belief_t;

typedef struct hu_tom_belief_state {
    char *contact_id;
    size_t contact_id_len;
    hu_tom_belief_t beliefs[HU_TOM_MAX_BELIEFS];
    size_t belief_count;
    hu_tom_expectation_t expectations[HU_TOM_MAX_EXPECTATIONS];
    size_t expectation_count;
} hu_tom_belief_state_t;

/* Initialize a belief state for a contact */
hu_error_t hu_tom_init(hu_tom_belief_state_t *state, hu_allocator_t *alloc, const char *contact_id,
                       size_t contact_id_len);

/* Record a belief from conversation evidence */
hu_error_t hu_tom_record_belief(hu_tom_belief_state_t *state, hu_allocator_t *alloc,
                                const char *topic, size_t topic_len, hu_belief_type_t type,
                                float confidence);

/* Build context string summarizing what the contact knows/doesn't know */
hu_error_t hu_tom_build_context(const hu_tom_belief_state_t *state, hu_allocator_t *alloc,
                                char **out, size_t *out_len);

/* Record what the user expects the AI to know about a topic */
hu_error_t hu_tom_record_user_expectation(hu_tom_belief_state_t *state, hu_allocator_t *alloc,
                                          const char *topic, size_t topic_len,
                                          hu_tom_expected_knowledge_t knowledge_type);

/* Detect gaps: topics where user expects knowledge AI doesn't have.
 * Returns array of gaps (caller owns). gap_count set to number found. */
hu_error_t hu_tom_detect_gaps(const hu_tom_belief_state_t *state, hu_allocator_t *alloc,
                              hu_tom_gap_t **gaps_out, size_t *gap_count);

/* Build directive string for detected gaps, for prompt injection.
 * Returns NULL if no gaps. Caller owns returned string. */
char *hu_tom_build_gap_directive(hu_allocator_t *alloc, const hu_tom_gap_t *gaps, size_t gap_count,
                                 size_t *out_len);

/* Free a gap array returned by hu_tom_detect_gaps */
void hu_tom_gaps_free(hu_allocator_t *alloc, hu_tom_gap_t *gaps, size_t count);

/* Detect if message text indicates user expects AI to know something.
 * Returns true if a pattern like "remember when", "you know my" is found.
 * If true, topic_out and topic_len_out are set to the extracted topic. */
bool hu_tom_detect_user_expectation(const char *text, size_t text_len, const char **topic_out,
                                    size_t *topic_len_out,
                                    hu_tom_expected_knowledge_t *knowledge_type_out);

/* Free all beliefs in a state */
void hu_tom_deinit(hu_tom_belief_state_t *state, hu_allocator_t *alloc);

/* =====================================================================
 * Phase A of Spec 4 (TOM activation): SQLite-backed user-expectation
 * persistence. The functions below operate on the daemon's shared
 * sqlite3 memory handle (see hu_sqlite_memory_get_db). They are the
 * persisted counterparts to the in-memory hu_tom_record_user_expectation
 * above — that function operates on a single-contact belief state and
 * is fixed-size (HU_TOM_MAX_EXPECTATIONS slots); these write to the
 * `tom_user_expectations` table and survive daemon restarts.
 *
 * Schema initialisation: hu_tom_user_expectations_init_table(db).
 *
 * Idempotency: hu_tom_persist_user_expectation is idempotent on
 * (contact_id, topic, session_key) via the UNIQUE constraint /
 * INSERT OR IGNORE semantics (design D-TOM-2 / AC-TOM-2).
 * ===================================================================== */

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

/* Create the tom_user_expectations table and supporting index if they
 * do not already exist. Safe to call on every daemon startup. */
hu_error_t hu_tom_user_expectations_init_table(sqlite3 *db);

/* Persist a user-expectation row keyed by (contact_id, topic, session_key).
 *
 * - contact_id: stable identifier for the contact (callers use the
 *   batch_key from daemon.c:4550 as the session_key, and the same
 *   value as contact_id when no separate identifier exists).
 * - topic / topic_len: extracted from hu_tom_detect_user_expectation.
 * - knowledge_type: HU_TOM_EXPECT_REMEMBERS / UNDERSTANDS / TRACKS.
 * - session_key / session_key_len: per-batch key (channel × contact ×
 *   time-window). May be NULL/0 for global expectations.
 * - turn_number: 0 if not conversation-scoped, otherwise the turn
 *   number within the current batch.
 * - now_ts_ms: caller-injected wallclock in milliseconds (UTC). Tests
 *   inject deterministic values; production passes
 *   (int64_t)time(NULL) * 1000.
 *
 * Resolution (Q-TOM-B) is deferred to Phase B; new rows are inserted
 * with resolved_ts_ms = NULL and the resolution UPDATE path is not
 * implemented in Phase A. */
hu_error_t hu_tom_persist_user_expectation(sqlite3 *db, const char *contact_id, const char *topic,
                                           size_t topic_len,
                                           hu_tom_expected_knowledge_t knowledge_type,
                                           const char *session_key, size_t session_key_len,
                                           int64_t turn_number, int64_t now_ts_ms);

/* Test/operator helper: count rows currently in tom_user_expectations
 * for a given contact_id, ignoring resolved_ts_ms. */
hu_error_t hu_tom_user_expectations_count_for_contact(sqlite3 *db, const char *contact_id,
                                                      int64_t *out_count);

/* Load currently-unresolved expectations for `contact_id` that have no
 * matching belief in `tom_user_beliefs` (i.e. resolved_ts_ms IS NULL).
 * Output array is allocator-owned; caller frees via
 * hu_tom_persisted_expectations_free.
 *
 * Each entry copies `topic` and `expected_knowledge_type` for use in
 * hu_tom_build_context. */
typedef struct hu_tom_persisted_expectation {
    char *topic;
    size_t topic_len;
    hu_tom_expected_knowledge_t knowledge_type;
} hu_tom_persisted_expectation_t;

hu_error_t hu_tom_user_expectations_load_unresolved(sqlite3 *db, hu_allocator_t *alloc,
                                                    const char *contact_id, size_t max_rows,
                                                    hu_tom_persisted_expectation_t **out,
                                                    size_t *out_count);

void hu_tom_persisted_expectations_free(hu_allocator_t *alloc, hu_tom_persisted_expectation_t *rows,
                                        size_t count);

/* Task 5: extension of hu_tom_build_context that appends an
 * "### Unmet User Expectations" section listing the persisted unresolved
 * expectations passed in. When `exp_count == 0` the section is omitted.
 * The base "### Contact Mental Model" block is built unchanged via
 * hu_tom_build_context above; this function concatenates that block
 * with the unmet-expectations block. */
hu_error_t hu_tom_build_context_with_expectations(const hu_tom_belief_state_t *state,
                                                  const hu_tom_persisted_expectation_t *exps,
                                                  size_t exp_count, hu_allocator_t *alloc,
                                                  char **out, size_t *out_len);

/* Task 11: periodic GC of tom_user_expectations rows that were resolved
 * more than `ttl_ms` milliseconds ago. Caller is expected to pass the
 * configured TTL (default 30 days = 2592000000 ms) and an injected
 * `now_ts_ms`. Returns the number of rows deleted in `*out_deleted`.
 *
 * Idempotency: safe to call on every tick; cheap when nothing matches. */
hu_error_t hu_tom_user_expectations_gc(sqlite3 *db, int64_t now_ts_ms, int64_t ttl_ms,
                                       int64_t *out_deleted);

/* Daemon-tick wrapper: only runs the GC every `interval_seconds` (default
 * 86400 = 24h). `last_run_ts_ms_inout` is the persistent watermark
 * threaded through the daemon's main loop. Emits one log line on first
 * enabled tick (per silent-config-gated-subsystems rule). */
hu_error_t hu_daemon_tick_tom_expectation_gc(sqlite3 *db, int64_t now_ts_ms,
                                             int64_t *last_run_ts_ms_inout,
                                             int64_t interval_seconds, int64_t ttl_ms);

#if HU_IS_TEST
void hu_daemon_tick_tom_expectation_gc_reset_warn_guards_for_test(void);
#endif

/* =====================================================================
 * Phase B of Spec 4 (TOM activation): conversation-local belief
 * temporality. Beliefs persisted via the SQLite-backed `tom_user_beliefs`
 * table carry optional `session_key` and `turn_number` columns so the
 * daemon can distinguish "told in this conversation" from "told in a
 * prior conversation" (D-TOM-4 / AC-TOM-4). Existing in-memory
 * hu_tom_belief_state_t remains the hot-path cache; this table is the
 * durable temporal layer that survives daemon restarts.
 *
 * Backward compatibility: rows MAY have NULL session_key / turn_number,
 * representing "global / not conversation-scoped" beliefs. New writes
 * from the daemon's post-turn capture point populate both; the existing
 * in-memory hu_tom_record_belief path is unchanged.
 * ===================================================================== */

/* Create the tom_user_beliefs table and supporting index if they do
 * not already exist. Safe to call on every daemon startup. */
hu_error_t hu_tom_user_beliefs_init_table(sqlite3 *db);

/* Persist a belief row. Idempotent on
 * (contact_id, topic, session_key) via UNIQUE INDEX — repeat writes
 * with the same (contact, topic, session) tuple UPDATE last_updated_ts
 * + confidence + belief_type rather than inserting duplicates.
 *
 * - contact_id: stable identifier for the contact.
 * - topic / topic_len: extracted topic (e.g. via hu_fast_capture).
 * - belief_type: HU_BELIEF_KNOWS / ASSUMES / UNAWARE / MISTAKEN.
 * - confidence: clamped to [0.0, 1.0].
 * - session_key / session_key_len: per-batch key. May be NULL/0 for
 *   global / pre-temporality beliefs (AC-TOM-4 backward-compat).
 * - turn_number: 0 if not conversation-scoped.
 * - now_ts_ms: caller-injected wallclock in milliseconds. */
hu_error_t hu_tom_persist_belief(sqlite3 *db, const char *contact_id, const char *topic,
                                 size_t topic_len, hu_belief_type_t belief_type, float confidence,
                                 const char *session_key, size_t session_key_len,
                                 int64_t turn_number, int64_t now_ts_ms);

/* Test/operator helper: count rows in tom_user_beliefs for a
 * (contact_id, session_key) pair. Pass NULL session_key to count
 * globally-scoped rows (rows with session_key IS NULL). */
hu_error_t hu_tom_user_beliefs_count_for_contact_session(sqlite3 *db, const char *contact_id,
                                                         const char *session_key,
                                                         int64_t *out_count);

/* =====================================================================
 * Phase C of Spec 4: self-change event recording. When the agent's own
 * configuration changes (persona delta applied, LoRA adapter swapped,
 * emotional register shifted), one row lands in tom_self_change_events
 * so later turns can detect that prior beliefs may be stale (D-TOM-5 /
 * AC-TOM-5).
 * ===================================================================== */

typedef enum hu_tom_self_change_kind {
    HU_TOM_SELF_CHANGE_PERSONA_DELTA = 1,  /* persona delta applied */
    HU_TOM_SELF_CHANGE_ADAPTER_SWAP = 2,   /* LoRA adapter swap success */
    HU_TOM_SELF_CHANGE_REGISTER_SHIFT = 3, /* emotional-register transition */
} hu_tom_self_change_kind_t;

/* Create the tom_self_change_events table and supporting index if they
 * do not already exist. Safe to call on every daemon startup. */
hu_error_t hu_tom_self_change_events_init_table(sqlite3 *db);

/* Record one self-change event against a contact. Always inserts a new
 * row (events are append-only history; no idempotency dedup). */
hu_error_t hu_tom_record_self_change_event(sqlite3 *db, const char *contact_id,
                                           hu_tom_self_change_kind_t event_kind,
                                           const char *session_key, size_t session_key_len,
                                           int64_t turn_number, double magnitude,
                                           int64_t now_ts_ms);

/* Test/operator helper: count self-change events for a (contact_id,
 * event_kind) pair. Pass event_kind == 0 to count all kinds. */
hu_error_t hu_tom_self_change_events_count(sqlite3 *db, const char *contact_id,
                                           hu_tom_self_change_kind_t event_kind,
                                           int64_t *out_count);

/* =====================================================================
 * Phase D of Spec 4: staleness gap detection.
 *
 * A belief is "stale" relative to a self-change event of relevant kind
 * for the same contact when
 *
 *   belief.last_updated_ts < event.timestamp_utc_ms
 *   AND event.timestamp_utc_ms > now - staleness_window_sec * 1000
 *
 * The relevant-kind mapping (D-TOM-6) is:
 *   PERSONA_DELTA   -> invalidates beliefs of REMEMBERS
 *   ADAPTER_SWAP    -> invalidates beliefs of UNDERSTANDS
 *   REGISTER_SHIFT  -> invalidates beliefs of TRACKS
 * ===================================================================== */

#ifndef HU_TOM_DEFAULT_STALENESS_WINDOW_SEC
#define HU_TOM_DEFAULT_STALENESS_WINDOW_SEC ((int64_t)7 * 24 * 60 * 60) /* 7 days */
#endif

typedef struct hu_tom_staleness_gap {
    char *topic;
    size_t topic_len;
    hu_tom_expected_knowledge_t expected_kind;
    hu_tom_self_change_kind_t event_kind;
    int64_t belief_ts_ms;
    int64_t event_ts_ms;
} hu_tom_staleness_gap_t;

/* Detect staleness gaps for a contact. Scans persisted beliefs against
 * persisted self-change events, applies D-TOM-6 kind mapping, and emits
 * one gap per belief that pre-dates a relevant event within the
 * staleness window.
 *
 * - now_ts_ms: injected wallclock in milliseconds.
 * - staleness_window_sec: typically HU_TOM_DEFAULT_STALENESS_WINDOW_SEC.
 * - max_gaps: caller-side cap; pass 32 for typical use.
 *
 * Caller owns *out and frees via hu_tom_staleness_gaps_free. */
hu_error_t hu_tom_detect_staleness_gaps(sqlite3 *db, hu_allocator_t *alloc, const char *contact_id,
                                        int64_t now_ts_ms, int64_t staleness_window_sec,
                                        size_t max_gaps, hu_tom_staleness_gap_t **out,
                                        size_t *out_count);

void hu_tom_staleness_gaps_free(hu_allocator_t *alloc, hu_tom_staleness_gap_t *gaps, size_t count);

/* D-TOM-6 mapping: which belief-expectation kind is invalidated by a
 * given self-change event kind. Exposed for tests and the prompt
 * directive layer. */
hu_tom_expected_knowledge_t
hu_tom_self_change_invalidates_kind(hu_tom_self_change_kind_t event_kind);

#endif /* HU_ENABLE_SQLITE */

#endif /* HU_THEORY_OF_MIND_H */
