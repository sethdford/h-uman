#ifndef HU_AGENT_SELF_MODEL_H
#define HU_AGENT_SELF_MODEL_H

/* Spec 2026-05-19 self-model-scaffold — Phase A.
 *
 * Observe-only behavioral self-model. Records per-turn behavioral metrics
 * (length, tool-sequence hash, emotional register, latency) into a fixed-
 * capacity ring buffer for later aggregation by the world model.
 *
 * Distinct from:
 *   - persona (prescriptive — "who I should be")
 *   - personal model (the user — "who they are")
 *   - calibration (reactive — "how to adjust given feedback")
 *
 * This is "how I have actually been behaving" — substrate for drift
 * detection. Acting on observations (auto-correction, prompt biasing) is
 * explicitly deferred to follow-up specs (Phases C-E of the same spec).
 *
 * Privacy invariant (AC-SM-7): the log records ONLY hashes, sizes, enums,
 * and timestamps. NEVER user message text, NEVER agent response bodies,
 * NEVER tool argument payloads. Pinned by the grep-based privacy test in
 * tests/test_self_model_no_content_capture.c (Phase B).
 *
 * Feature flag: HU_ENABLE_SELF_MODEL. When OFF, the .c file's `#else`
 * branch provides bare-return stubs so symbols resolve at link time and
 * callers do not need conditional compilation (per AC-SM-6 + the
 * `.claude/rules/test-source-gate-symmetry.md` discipline).
 */

#include "human/core/allocator.h"
#include "human/core/error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Default ring-buffer capacity (turns). Configurable later via runtime
 * config `self_model.behavior_log_capacity` — Phase B / Task 7. */
#define HU_AGENT_BEHAVIOR_LOG_DEFAULT_CAPACITY 256

/* Maximum capacity accepted by `hu_agent_behavior_log_init`. Bounds the
 * memory footprint and prevents pathological allocations from a hostile
 * or buggy config. 64 KiB of records (~128 bytes each) at this cap. */
#define HU_AGENT_BEHAVIOR_LOG_MAX_CAPACITY 4096

/* Persona-delta kind recorded on the turn — enum-only, NO body text. */
typedef enum hu_agent_persona_delta_kind {
    HU_AGENT_PERSONA_DELTA_NONE = 0,
    HU_AGENT_PERSONA_DELTA_FORMALITY,
    HU_AGENT_PERSONA_DELTA_LENGTH,
    HU_AGENT_PERSONA_DELTA_WARMTH,
    HU_AGENT_PERSONA_DELTA_HUMOR,
    HU_AGENT_PERSONA_DELTA_OTHER
} hu_agent_persona_delta_kind_t;

/* Emotional register recorded on the turn — enum-only, mirrors the
 * world-model dominant-emotion bucket. Concrete values are private to
 * the world-model module; this enum is a compact shadow for the log. */
typedef enum hu_agent_emotional_register {
    HU_AGENT_EMOTION_NEUTRAL = 0,
    HU_AGENT_EMOTION_POSITIVE,
    HU_AGENT_EMOTION_NEGATIVE,
    HU_AGENT_EMOTION_CAUTIOUS,
    HU_AGENT_EMOTION_OTHER
} hu_agent_emotional_register_t;

/* One per-turn record. All fields are hashes, sizes, enums, or
 * timestamps — NO content-carrying strings. */
typedef struct hu_agent_behavior_record {
    /* Output sizing — chars and an estimated token count. The estimate
     * is the caller's responsibility; this struct only stores the
     * scalar. NOT the body text. */
    uint32_t response_length_chars;
    uint32_t response_length_tokens_est;

    /* FNV-1a 32-bit hash of the ordered tool-name sequence. Distinct
     * orderings hash distinctly. Never the tool arguments. */
    uint32_t tool_sequence_hash;
    uint16_t tool_count;

    /* Compact enum shadows of larger objects. */
    uint8_t emotional_register; /* hu_agent_emotional_register_t */
    uint8_t persona_delta_kind; /* hu_agent_persona_delta_kind_t */

    /* Provider→response latency in milliseconds. */
    uint32_t response_latency_ms;

    /* Stable hashes — never raw contact handles or channel display
     * names. The caller chooses the hashing function; this struct
     * just stores it. */
    uint64_t contact_hash;
    uint32_t channel_id;

    /* Wall-clock UTC milliseconds at record time. The caller passes
     * the timestamp explicitly so the hot path doesn't reach for
     * gettimeofday() and tests are deterministic. */
    int64_t timestamp_utc_ms;
} hu_agent_behavior_record_t;

/* Aggregate over a window of records. Computed off the hot path by the
 * Phase C aggregation tick. Defined here so the planner / world-model
 * surface can reference the type without including a Phase-C header
 * later. */
typedef struct hu_agent_self_observation {
    int64_t window_start_ts_ms;
    int64_t window_end_ts_ms;
    uint32_t n_turns;

    double response_length_mean;
    double response_length_stddev;
    double tool_selection_entropy;

    uint32_t emotion_dist[5]; /* one bucket per hu_agent_emotional_register_t */

    uint32_t latency_p50_ms;
    uint32_t latency_p95_ms;
} hu_agent_self_observation_t;

/* Drift signal — a flagged dimension where an observation exceeded the
 * calibrated baseline by more than the configured σ threshold. Computed
 * in Phase C; defined here so the world-model integration in Phase D
 * can carry the type across module boundaries. */
typedef enum hu_agent_self_concern_dimension {
    HU_AGENT_CONCERN_DIM_RESPONSE_LENGTH = 0,
    HU_AGENT_CONCERN_DIM_TOOL_ENTROPY,
    HU_AGENT_CONCERN_DIM_EMOTION_CONSISTENCY,
    HU_AGENT_CONCERN_DIM_LATENCY
} hu_agent_self_concern_dimension_t;

typedef struct hu_agent_self_concern {
    uint64_t observation_id;
    uint8_t dimension; /* hu_agent_self_concern_dimension_t */
    double magnitude_sigma;
    uint32_t window_n_turns;
    int64_t created_ts_ms;
} hu_agent_self_concern_t;

/* The ring buffer. Owns its records slab; zero alloc per record.
 *
 * Threading: single-writer / multi-reader. The current `record` call
 * site is `hu_agent_m3_on_provider_success`, which is per-turn and
 * serialized in the daemon. If a future caller invokes record from
 * multiple threads, the head update must become atomic — out of scope
 * for Phase A. */
typedef struct hu_agent_behavior_log {
    hu_agent_behavior_record_t *records; /* slab; owns memory */
    size_t capacity;                     /* power of 2 not required */
    size_t head;                         /* MONOTONIC count of records ever recorded;
                                          * physical slot is head % capacity. */
    hu_allocator_t allocator;            /* copied; used for slab free */
} hu_agent_behavior_log_t;

/* Initialize a log with a fixed capacity. capacity=0 selects the
 * default. capacity > HU_AGENT_BEHAVIOR_LOG_MAX_CAPACITY is rejected
 * with HU_ERR_INVALID_ARGUMENT.
 *
 * On success: log owns a zero-initialized slab; head=0.
 * On any failure: log->records is NULL, capacity=0, head=0.
 *
 * Returns HU_OK on success.
 *         HU_ERR_INVALID_ARGUMENT if log or allocator is NULL or capacity is too large.
 *         HU_ERR_OUT_OF_MEMORY    if the slab allocation fails.
 *
 * When HU_ENABLE_SELF_MODEL is OFF: returns HU_OK without allocating;
 * log is zeroed. record() and snapshot() will both no-op safely. */
hu_error_t hu_agent_behavior_log_init(hu_agent_behavior_log_t *log, const hu_allocator_t *allocator,
                                      size_t capacity);

/* Free the slab and zero the struct. Safe on a zero-initialized log. */
void hu_agent_behavior_log_destroy(hu_agent_behavior_log_t *log);

/* Hot path: record one turn. Zero heap allocations. Idempotent on a
 * NULL or uninitialized log (silently returns HU_OK).
 *
 * Per AC-SM-1 the record is a value copy of `rec` into slot
 * `log->head % log->capacity`, then head++.
 *
 * When HU_ENABLE_SELF_MODEL is OFF: silent no-op, returns HU_OK. The
 * caller does not need conditional compilation. */
hu_error_t hu_agent_behavior_log_record(hu_agent_behavior_log_t *log,
                                        const hu_agent_behavior_record_t *rec);

/* Read up to `max_out` of the most-recent records into `out` in
 * chronological order (oldest first). Returns the number of records
 * actually copied via `out_count`.
 *
 * If head < capacity: copies min(head, max_out) starting from slot 0.
 * If head >= capacity: copies the last min(capacity, max_out) records,
 *                      walking backwards from head and reversing into
 *                      chronological order in `out`.
 *
 * When HU_ENABLE_SELF_MODEL is OFF or log is uninitialized:
 * out_count = 0, returns HU_OK. */
hu_error_t hu_agent_behavior_log_snapshot(const hu_agent_behavior_log_t *log,
                                          hu_agent_behavior_record_t *out, size_t max_out,
                                          size_t *out_count);

/* Monotonic count of records ever recorded. Useful for "did head
 * advance by N over a fixture run" assertions (AC-SM-1 pin). */
size_t hu_agent_behavior_log_total_records(const hu_agent_behavior_log_t *log);

/* Self-model readback: summarize the recent behavior-log window into a short
 * first-person "self-awareness" directive (avg reply length + dominant tone) so
 * the agent can SEE how it has been showing up — the metacognition loop the log
 * was built for. Privacy-safe: reads only sizes + the emotion enum, never
 * content. *out is a freshly allocated NUL-terminated string the caller frees via
 * alloc->free(ctx, *out, *out_len + 1); *out is NULL (HU_OK) when there are too
 * few turns (<3) to summarize or when HU_ENABLE_SELF_MODEL is OFF (stub). */
hu_error_t hu_agent_self_model_build_directive(const hu_agent_behavior_log_t *log,
                                               const hu_allocator_t *alloc, char **out,
                                               size_t *out_len);

/* ===================================================================
 * Phase C — Periodic aggregation + drift signal
 * ===================================================================
 *
 * Every N turns OR T minutes, fold the recent ring window into one
 * aggregate row of `agent_self_observations`. For each dimension D,
 * compute the σ-deviation from the calibrated baseline and, if
 * |σ| ≥ drift_threshold_sigma (default 2.0) AND baseline N ≥
 * drift_minimum_baseline_n (default 50), insert a row into
 * `agent_self_concerns`.
 *
 * All writes go through SQLite. The schema is created lazily on first
 * tick via `hu_agent_self_model_init_tables`. Gated on HU_ENABLE_SQLITE
 * at compile time; gated on `self_model.aggregate_every_n_turns > 0`
 * and `self_model.aggregate_every_sec > 0` at run time.
 *
 * Per `~/.claude/rules/silent-config-gated-subsystems.md`, the tick
 * emits ONE `hu_log_info_once` line per process lifetime in both the
 * disabled and first-enabled states.
 *
 * NOT WIRED INTO daemon.c BY THIS SPEC. The function is callable from
 * tests today; production wiring lands in a follow-up alongside the
 * config-parser surface. */

#ifndef HU_AGENT_SELF_OBSERVATION_AGG_DEFAULT_TURNS
#define HU_AGENT_SELF_OBSERVATION_AGG_DEFAULT_TURNS 100
#endif

#ifndef HU_AGENT_SELF_OBSERVATION_AGG_DEFAULT_SEC
#define HU_AGENT_SELF_OBSERVATION_AGG_DEFAULT_SEC 3600
#endif

#ifndef HU_AGENT_SELF_MODEL_DRIFT_THRESHOLD_SIGMA_DEFAULT
#define HU_AGENT_SELF_MODEL_DRIFT_THRESHOLD_SIGMA_DEFAULT 2.0
#endif

#ifndef HU_AGENT_SELF_MODEL_DRIFT_MIN_BASELINE_N_DEFAULT
#define HU_AGENT_SELF_MODEL_DRIFT_MIN_BASELINE_N_DEFAULT 50
#endif

#ifdef HU_ENABLE_SQLITE
struct sqlite3;
typedef struct sqlite3 sqlite3;

/* Schema migration. Idempotent. Safe to call on every tick. Creates:
 *   - agent_self_observations(...)
 *   - agent_self_concerns(...)
 * Returns HU_OK on success or when the tables already exist. */
hu_error_t hu_agent_self_model_init_tables(sqlite3 *db);

/* Compute and INSERT one observation row from the recent `log` window
 * (up to `window_n` records, capped at the log's snapshot capacity).
 * `now_ts_ms` is the wall clock that ends the window.
 *
 * Returns HU_OK and writes `*out_observation_id = sqlite3_last_insert_rowid`
 * on success. Returns HU_ERR_INVALID_ARGUMENT when args are NULL. */
hu_error_t hu_agent_self_model_compute_and_insert_observation(
    sqlite3 *db, const hu_agent_behavior_log_t *log, size_t window_n, int64_t now_ts_ms,
    int64_t *out_observation_id, hu_agent_self_observation_t *out_observation);

/* Run drift detection over `observation` against `baseline`. Inserts
 * one row into `agent_self_concerns` for each dimension whose |σ| ≥
 * `drift_threshold_sigma`, when `baseline_n` ≥ `min_baseline_n`.
 *
 * `baseline_response_length_mean` and `baseline_response_length_stddev`
 * supply the response-length baseline; pass 0 / 0 to skip that
 * dimension. The tool-entropy / emotion / latency dimensions are
 * compared against the observation's own field — placeholder until
 * the calibration baseline exposes them. The σ math uses the
 * observation's mean against the baseline mean / stddev.
 *
 * Returns the number of concerns inserted via `*out_concerns_inserted`. */
hu_error_t
hu_agent_self_model_detect_drift(sqlite3 *db, const hu_agent_self_observation_t *observation,
                                 int64_t observation_id, double baseline_response_length_mean,
                                 double baseline_response_length_stddev, uint32_t baseline_n,
                                 double drift_threshold_sigma, uint32_t min_baseline_n,
                                 int64_t now_ts_ms, uint32_t *out_concerns_inserted);

/* Daemon tick wrapper. Fires aggregation when EITHER:
 *   - `total_records - *last_run_total_records_inout` >= `aggregate_every_n_turns`
 *   - `now_ts_ms - *last_run_ts_ms_inout`            >= `aggregate_every_sec * 1000`
 * whichever first. After a fire, both watermarks are updated.
 *
 * `aggregate_every_n_turns == 0` AND `aggregate_every_sec == 0` →
 * subsystem disabled; the function emits one operator-visible log
 * line on first call (per silent-config-gated-subsystems.md) and
 * returns HU_OK without doing work.
 *
 * Drift detection runs against the supplied baseline; pass 0/0/0 to
 * skip drift work entirely (observation still recorded). */
hu_error_t hu_daemon_tick_self_observation_aggregate(
    sqlite3 *db, const hu_agent_behavior_log_t *log, int64_t now_ts_ms,
    int64_t *last_run_ts_ms_inout, size_t *last_run_total_records_inout,
    int64_t aggregate_every_n_turns, int64_t aggregate_every_sec,
    double baseline_response_length_mean, double baseline_response_length_stddev,
    uint32_t baseline_n, double drift_threshold_sigma, uint32_t min_baseline_n);

#if HU_IS_TEST
void hu_daemon_tick_self_observation_aggregate_reset_warn_guards_for_test(void);
#endif

#endif /* HU_ENABLE_SQLITE */

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_SELF_MODEL_H */
