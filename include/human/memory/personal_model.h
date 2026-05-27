#ifndef HU_MEMORY_PERSONAL_MODEL_H
#define HU_MEMORY_PERSONAL_MODEL_H

#include "human/channels/reaction_event.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/fact_extract.h"
#include "human/memory/tiers.h"
#include "human/memory/trust.h"
#include "human/persona/circadian.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Unified personal model — aggregates identity, preferences, behavioral
 * patterns, and relationship context into a single queryable structure.
 *
 * Replaces the scattered approach of core_memory + fact_extract + tier store
 * with a single source of truth for "what we know about this person."
 *
 * Designed for:
 * - Prompt enrichment (build a "personal context" block)
 * - Preference-aware tool selection
 * - Adaptive communication style
 * - Memory consolidation feedback
 */

#define HU_PM_MAX_FACTS  64
#define HU_PM_MAX_GOALS  8
#define HU_PM_MAX_TOPICS 16
#define HU_PM_MAX_FIELD  256
/* SOTA-2026 init-09 §2.6: pending-facts quarantine queue.
 * Facts extracted from THIRD_PARTY-or-below content sit here until
 * the user re-states them in a USER_DIRECT message within
 * HU_PM_PENDING_FACT_TTL_SEC, or 3 independent low-trust sources
 * corroborate. Otherwise they expire silently on the next decay tick. */
#define HU_PM_MAX_PENDING_FACTS                  16
#define HU_PM_PENDING_FACT_TTL_SEC               ((int64_t)(24LL * 60 * 60))
#define HU_PM_PENDING_FACT_PROMOTE_CORROBORATION 3

typedef struct hu_personal_topic {
    char name[HU_PM_MAX_FIELD];
    float interest_score;   /* 0.0-1.0: how interested the user is */
    uint32_t mention_count; /* how often this topic appears */
    int64_t last_mentioned; /* unix timestamp */
} hu_personal_topic_t;

typedef struct hu_personal_goal {
    char description[512];
    bool active;
    int64_t created_at;
    /* Unix timestamp of the most recent reference to this goal in
     * conversation. Drives `hu_personal_goal_effective_priority`:
     * a goal not touched for >= one half-life is presumed stale and
     * earns less prompt space (see HU_PM_GOAL_RELEVANCE_HALF_LIFE_SEC).
     * Defaults to created_at when the goal is inserted; callers that
     * detect a re-mention should bump this to the new wall-clock time. */
    int64_t last_referenced;
    int64_t deadline; /* 0 = no deadline */
    float progress;   /* 0.0-1.0 estimated progress */
} hu_personal_goal_t;

typedef struct hu_communication_style {
    float formality;         /* 0.0 (casual) to 1.0 (formal) */
    float verbosity;         /* 0.0 (terse) to 1.0 (verbose) */
    float emoji_frequency;   /* 0.0 (never) to 1.0 (heavy) */
    float humor_receptivity; /* 0.0 (serious) to 1.0 (playful) */
    /* Punctuation / case axes — complement the four quantitative axes
     * above. EWMA-tracked over `sample_count`, ranging 0.0 to 1.0:
     *   - lowercase_ratio:    proportion of msgs typed all-lowercase
     *   - abbreviation_ratio: proportion of msgs with chat abbreviations
     *                         ("u", "rn", "btw", "ty", "lmk", "yw")
     * Surfaced in the personal-model directive so the frontier model
     * matches casing and shorthand instead of defaulting to its
     * training-distribution register. */
    float lowercase_ratio;
    float abbreviation_ratio;
    uint32_t avg_message_length; /* in characters */
    uint32_t sample_count;       /* messages analyzed */
    /* Unix timestamp of the most recent style observation. Drives
     * `hu_personal_communication_style_freshness` so a year-old style
     * sample doesn't shape the directive on a fresh conversation;
     * after one half-life the freshness multiplier is 0.5 and the
     * "Mirror their style" directive is dropped from the prompt
     * (HU_PM_STYLE_OBSERVATION_HALF_LIFE_SEC). 0 means "never
     * observed" — callers should treat freshness as 0 in that case. */
    int64_t last_observed_at;
    /* Phase 5 Task 1 (RL SOTA) — decision-style EWMA axes consumed
     * ONLY by the opt-in v2 fidelity scorer (`_score_v2`). Appended
     * at the END of the struct so:
     *   - zero-init / `memset(&s, 0, sizeof(s))` callers still get
     *     0.0 defaults across the new fields (legacy compatible);
     *   - designated initializers (`{.lowercase_ratio=…}`) on the
     *     v1 fields keep working byte-identically;
     *   - the v1 scorer (`hu_communication_style_fidelity_score`)
     *     never reads these fields — its 3-axis body is unchanged
     *     per round-1 BLOCKER-1 (v2 is opt-in only, never replaces
     *     v1, never modifies v1 callers in place).
     *
     * Semantics (all in [0, 1]):
     *   - hedging_ratio:    EWMA fraction of user messages whose
     *                       wording leans hedge-y ("maybe", "could",
     *                       "perhaps", "might", "possibly", ...).
     *   - question_ratio:   EWMA fraction of user messages framed
     *                       as questions (terminator is `?`).
     *   - imperative_ratio: EWMA fraction of user sentences that
     *                       start with an imperative verb ("do",
     *                       "check", "fix", "ship", "send", ...).
     * Together these three sub-signals form the 4th composite
     * "decision-style" axis in the v2 scorer; the user's documented
     * decision style is reflected via this fingerprint rather than
     * a free-form persona string so the scorer stays deterministic
     * and side-effect-free. When all three target axes are 0 the
     * v2 composite is the no-signal neutral (0.5). */
    float hedging_ratio;
    float question_ratio;
    float imperative_ratio;
} hu_communication_style_t;

typedef struct hu_personal_model {
    hu_core_memory_t core; /* identity: name, bio, preferences, goals */

    /* Structured facts extracted from conversations */
    hu_heuristic_fact_t facts[HU_PM_MAX_FACTS];
    size_t fact_count;

    /* Learned communication style */
    hu_communication_style_t style;

    /* Topics the user cares about, sorted by interest */
    hu_personal_topic_t topics[HU_PM_MAX_TOPICS];
    size_t topic_count;

    /* Active goals and commitments */
    hu_personal_goal_t goals[HU_PM_MAX_GOALS];
    size_t goal_count;

    /* Temporal patterns */
    uint8_t active_hours[24]; /* message frequency by hour (0-255 normalized) */
    uint8_t active_days[7];   /* message frequency by day of week */

    /* Model metadata */
    int64_t created_at;
    int64_t updated_at;
    uint32_t interaction_count; /* total conversations analyzed */
    uint32_t version;           /* schema version for migration */

    /* SOTA-2026 init-09 §2.6: pending-facts quarantine queue.
     *
     * Facts extracted from `provenance.tier <= THIRD_PARTY` content
     * are buffered here rather than committed to `facts[]`. They are
     * promoted to `facts[]` when EITHER:
     *   (a) the user re-states the fact in a `tier >= USER_DIRECT`
     *       message within HU_PM_PENDING_FACT_TTL_SEC, OR
     *   (b) HU_PM_PENDING_FACT_PROMOTE_CORROBORATION independent
     *       low-trust sources corroborate the same fact.
     * Otherwise they expire silently on the next decay tick.
     *
     * `pending_corroboration_count[i]` tracks how many independent
     * low-trust sources have asserted `pending_facts[i]` (the contact
     * handles are compared in personal_model.c). */
    hu_heuristic_fact_t pending_facts[HU_PM_MAX_PENDING_FACTS];
    int64_t pending_since[HU_PM_MAX_PENDING_FACTS];
    uint8_t pending_corroboration_count[HU_PM_MAX_PENDING_FACTS];
    size_t pending_fact_count;
} hu_personal_model_t;

/* Initialize a personal model with defaults. */
void hu_personal_model_init(hu_personal_model_t *model);

/* Build a prompt context block from the personal model.
 * Writes a human-readable summary into buf. Returns bytes written. */
size_t hu_personal_model_build_prompt(const hu_personal_model_t *model, char *buf, size_t cap);

/* Same as `_build_prompt`, but tunes the channel-affected directive
 * lines (notably the recently-completed acknowledgment directive) to
 * the active channel's persona overlay.
 *
 * Honored overlay fields, when present:
 *   - `formality`     "casual" → permit warmer/punchier wording;
 *                     "formal" → demand brevity, no emoji.
 *   - `avg_length`    "short" or numeric ≤30 → directive emphasizes
 *                     "a single sentence" instead of "warmly".
 *   - `emoji_usage`   "moderate" / "high" → directive may suggest
 *                     "an emoji is fine if it fits"; "minimal" or
 *                     unspecified → no emoji guidance.
 *
 * `overlay = NULL` is equivalent to `hu_personal_model_build_prompt`
 * (the legacy wrapper) — emits the channel-neutral default
 * directive. The overlay is consulted for the directive line only;
 * everything else (facts, topics, goals, recently-completed list,
 * style summary) is identical to the legacy call. */
struct hu_persona_overlay; /* fwd decl — keeps personal_model.h
                            * out of the persona dependency tree;
                            * the typedef in persona.h is
                            * compatible with this anonymous tag. */
size_t hu_personal_model_build_prompt_with_overlay(const hu_personal_model_t *model,
                                                   const struct hu_persona_overlay *overlay,
                                                   char *buf, size_t cap);

/* T7 of docs/plans/2026-05-26-reflection-loop. Same output as
 * `_build_prompt_with_overlay`, plus a "Recent observations" slice
 * appended at the tail, scoped to the active channel via
 * hu_reflection_query_for_system_prompt(db, channel, ...).
 *
 * The slice contains up to `max_patterns` non-retired, non-already-
 * surfaced patterns ≥ 0.5 confidence, plus the latest run's prose
 * summary (when available). Each surfaced pattern is marked via
 * hu_reflection_mark_surfaced so the same observation doesn't reach
 * the model on every turn — patterns surface once, then become
 * candidates for init_proposer's separate "should I mention this?"
 * path until retired or expired.
 *
 * `db == NULL` or `channel == NULL`: behaves exactly like
 * `_build_prompt_with_overlay` (no reflection slice appended). This
 * is the safe default for callers that don't have SQLite available.
 *
 * Forward-declared struct sqlite3 keeps callers from dragging in
 * sqlite3.h. */
struct sqlite3;
size_t hu_personal_model_build_prompt_with_reflection(const hu_personal_model_t *model,
                                                      const struct hu_persona_overlay *overlay,
                                                      struct sqlite3 *db, const char *channel,
                                                      int max_patterns, char *buf, size_t cap);

/* Sprint B.8 wire — set the identity graph used by the prompt builder
 * to surface cross-handle merge candidates ("IDENTITY: \"alice@new\"
 * may be same person as Alice"). Caller retains ownership; passing
 * NULL clears.
 *
 * Like hu_reaction_handler_set_identity_graph, this is a process-
 * lifetime borrow stored in a file-scope static. Single-threaded
 * daemon → no lock needed. The graph type is treated as opaque here
 * via void* to keep identity_resolver.h out of personal_model.h's
 * dep tree; callers MUST pass a (hu_identity_graph_t *). */
void hu_personal_model_set_identity_graph(const void *graph);

/* Track D D2.2 — acknowledgment-directive variant telemetry.
 *
 * Every time the prompt builder fires the recently-completed
 * acknowledgment directive (see `acknowledgment_directive_for_overlay`
 * in personal_model.c), it picks one of a small fixed set of
 * variants based on the active channel's persona overlay. The
 * counters below let dashboards and tests measure which variants
 * actually fire in production — answering "do casual+emoji
 * channels really see the casual+emoji wording, or is the gate
 * cliffing somewhere?" without sprinkling log lines on every
 * agent turn.
 *
 * Counters are pure in-memory atomics — no I/O, no log line per
 * fire (volume on a chatty channel would be overkill). Read with
 * `hu_personal_model_directive_telemetry_snapshot`; reset with
 * `_reset` (intended for tests; production code should leave the
 * counters monotonically increasing).
 *
 * IMPORTANT: the enum order matches the static counter array in
 * personal_model.c — never reorder, only append. */
typedef enum {
    HU_DIRECTIVE_VARIANT_NULL_OVERLAY = 0, /* legacy `_build_prompt`: NULL overlay */
    HU_DIRECTIVE_VARIANT_DEFAULT,          /* overlay present but no useful signal */
    HU_DIRECTIVE_VARIANT_FORMAL_TERSE,     /* formal/professional channels */
    HU_DIRECTIVE_VARIANT_CASUAL_EMOJI,     /* casual + moderate/high emoji */
    HU_DIRECTIVE_VARIANT_CASUAL_OR_SHORT,  /* casual w/o emoji license, or short channels */
    HU_DIRECTIVE_VARIANT_ADAPTIVE_EMOJI,   /* unspecified formality + emoji license */
    HU_DIRECTIVE_VARIANT__COUNT
} hu_directive_variant_t;

typedef struct {
    /* Per-variant fire counts. Indexed by `hu_directive_variant_t`. */
    uint64_t counts[HU_DIRECTIVE_VARIANT__COUNT];
    /* Total directive fires across all variants. Equals the sum
     * of `counts[]` — exposed separately so callers can compute
     * variant share without a loop. */
    uint64_t total;
} hu_directive_telemetry_t;

/* Snapshot the current counters. Thread-safe; reads atomic
 * locations without taking a lock. The returned struct is a
 * value-type copy — callers may persist or display it freely. */
void hu_personal_model_directive_telemetry_snapshot(hu_directive_telemetry_t *out);

/* Zero out all counters. Pure for-tests helper — production
 * surfaces should never call this (it loses observability data).
 * Tests use it to isolate their own variant counts from any
 * residual fires from earlier suites in the same process. */
void hu_personal_model_directive_telemetry_reset(void);

/* Returns a human-readable label for the variant ("null_overlay",
 * "casual_emoji", etc.). Stable across builds — safe for log lines,
 * dashboard JSON, and metric labels. Returns "unknown" for
 * out-of-range values. */
const char *hu_personal_model_directive_variant_label(hu_directive_variant_t v);

/* True when the model carries any concrete signal worth injecting into a
 * system prompt (facts, topics, goals, named identity, or observed style).
 * Lets callers skip prompt-block injection on a fresh / unused model so we
 * don't waste tokens on "(No detailed personal data yet.)" noise. */
bool hu_personal_model_has_content(const hu_personal_model_t *model);

/* Ingest a new message into the personal model.
 * Updates facts, style metrics, topics, and temporal patterns.
 *
 * SOTA-2026 init-09: `prov` carries the trust tier + provenance stamp
 * for `message`. Passing NULL defaults to USER_DIRECT and is permitted
 * only from inside `#ifdef _HU_PM_SELF_TEST` self-tests; production
 * call sites MUST pass a non-NULL `hu_provenance_t *` derived from the
 * active channel (see `hu_channel_trust_stamp`). */
hu_error_t hu_personal_model_ingest(hu_personal_model_t *model, const char *message,
                                    size_t message_len, bool from_user, int64_t timestamp,
                                    const hu_provenance_t *prov);

/* Merge facts from a fact extraction result into the model. */
hu_error_t hu_personal_model_merge_facts(hu_personal_model_t *model,
                                         const hu_fact_extract_result_t *facts);

/* SOTA-2026 init-09 §2.5: trust-gated merge.
 *
 * Same as `hu_personal_model_merge_facts`, except a fact whose
 * `provenance.tier` is strictly less than an existing duplicate's tier
 * is *not* allowed to overwrite the stored fact. The lower-trust
 * contradiction is recorded via `hu_minja_quarantine_log` and dropped.
 *
 * Facts originating from `THIRD_PARTY` or below are routed into the
 * pending-facts quarantine queue rather than `facts[]` unless they
 * collide with an existing key (same subject+predicate at same-or-higher
 * trust). See `pending_facts[]` in `hu_personal_model_t`. */
hu_error_t hu_personal_model_merge_facts_checked(hu_personal_model_t *model,
                                                 const hu_fact_extract_result_t *facts,
                                                 const hu_provenance_t *prov);

/* SOTA-2026 init-09: promote any pending facts whose key matches a fact
 * just asserted in a USER_DIRECT message. Idempotent. Returns the number
 * of promotions. */
size_t hu_personal_model_promote_pending_facts(hu_personal_model_t *model,
                                               const hu_fact_extract_result_t *user_direct_facts,
                                               int64_t now);

/* Extract topic keywords from a reaction's target-message text and bump
 * the corresponding entries in `model->topics[]`. Reactions are a strong
 * salience signal: a topic the user POSITIVELY reacts to (love / like /
 * laugh / emphasize / custom-emoji) is by definition something they care
 * about — even if they never typed about it themselves. A negatively-
 * reacted topic (dislike / question) is demoted so it doesn't dominate.
 *
 * Tokenization is intentionally simple (no LLM): walk `target_text`
 * word-by-word, lowercase each token, strip leading/trailing punctuation,
 * drop tokens < 4 chars and the standard topic-stopword set. The
 * surviving tokens each get a `bump_topic` (positive polarity) or
 * `decay_topic` (negative polarity) hit. The polarity is derived from
 * `event->polarity` (HU_REACTION_POSITIVE / NEGATIVE / NEUTRAL) — neutral
 * reactions (e.g. QUESTION) are no-ops on topic salience.
 *
 * Returns the number of distinct topic slots touched (added, bumped, or
 * demoted). NULL-safe on every argument. Removal events (is_removal=1)
 * are no-ops because retracting a reaction shouldn't retroactively
 * change topic salience — the original bump already happened and would
 * have been observed by everyone who saw the chat.
 *
 * Wired into `hu_reaction_ingest_personal_model` so every reaction
 * routed through the reaction handler bubbles up topics in addition
 * to the (subject, predicate, object) fact triple. */
size_t hu_personal_model_bump_topics_from_reaction(hu_personal_model_t *model,
                                                   const hu_reaction_event_t *event,
                                                   const char *target_text, int64_t now_unix);

/* SOTA-2026 init-09: expire pending facts older than TTL or whose decay
 * has dropped below floor. Returns the count expired. Called from the
 * existing decay-pruning path. */
size_t hu_personal_model_expire_pending_facts(hu_personal_model_t *model, int64_t now);

/* Query: does the user have a known preference about this topic?
 * Returns the matching fact if found, NULL otherwise. */
const hu_heuristic_fact_t *hu_personal_model_query_preference(const hu_personal_model_t *model,
                                                              const char *topic, size_t topic_len);

/* B11 — Cross-turn contradiction detection from the personal-model layer.
 *
 * Extracts heuristic facts from the user's *current* message, compares
 * each against `model->facts[]`, and flips `*out_contradicts` to true
 * when a contradiction is detected. Used by agent_turn.c to feed the
 * trust calibrator's `memory_contradicts_user` signal so reassertions
 * against memory escalate push-back rather than agreeing.
 *
 * Two contradiction shapes are detected:
 *
 *   (1) Same subject + same predicate + DIFFERENT object. Catches
 *       "I work at Acme" → "I work at Initech" and similar identity /
 *       affiliation flips. Object compare is case-insensitive.
 *
 *   (2) Same subject + ANTONYM predicate pair + SAME object. Catches
 *       "I like coffee" → "I hate coffee" and similar valence flips.
 *       The antonym pairs are a small fixed table inside the
 *       implementation (i like ↔ i don't like / i hate / i dislike,
 *       i love ↔ i hate / i don't like, i'm interested in ↔ i'm not
 *       interested in, i always ↔ i never, …).
 *
 * Stored facts must satisfy `confidence >= 0.6` to count — low-confidence
 * extractions cannot trigger push-back, since the alternative is the
 * model gaslighting the user with a half-remembered guess.
 *
 * Returns HU_OK on success (with `*out_contradicts` set), or
 * HU_ERR_INVALID_ARGUMENT when any pointer is NULL. Pure CPU; no I/O.
 * Safe to call on every turn — work is bounded by HU_FACT_EXTRACT_MAX
 * (32) × HU_PM_MAX_FACTS (64) = 2048 string compares max. */
hu_error_t hu_personal_model_contradicts_user(const hu_personal_model_t *model, const char *message,
                                              size_t message_len, bool *out_contradicts);

/* M2 P1 — Persistence. The personal model is the only piece of agent
 * state that genuinely accumulates value over time (facts, observed
 * style, topics, temporal patterns). Until now it lived only in RAM, so
 * every daemon restart erased the user-specific signal. These two calls
 * round-trip the struct as a binary blob with a 16-byte header
 * (magic + version + reserved). Path is allocator-owned UTF-8.
 *
 * `save`  — returns HU_OK on a clean fsync, HU_ERR_IO on write failure.
 *           Creates intermediate directories.
 * `load`  — reads and populates `*out`. On version mismatch or magic
 *           failure, returns HU_ERR_PARSE and leaves `*out`
 *           initialized to defaults (caller can keep walking).
 *           Returns HU_ERR_NOT_FOUND when the file doesn't exist.
 *
 * Binary format chosen over JSON for size (~6KB vs ~25KB) and zero
 * dependencies. The struct is POD with fixed-size fields; no embedded
 * pointers, so memcpy is safe. Schema migrations are handled by the
 * `version` field — incrementing it forces fresh state on existing
 * users (correct: old facts may be incompatible with new code). */
hu_error_t hu_personal_model_save(const hu_personal_model_t *model, const char *path);
hu_error_t hu_personal_model_load(hu_personal_model_t *out, const char *path);

/* Load per-contact personal model facts from the global database.
 *
 * Filters the database to retrieve only facts tagged with the given
 * contact_handle (fields with matching contact_handle). Returns HU_OK
 * on success (even if no facts are found; the model is initialized but
 * may be empty). */
hu_error_t hu_personal_model_load_for_contact(hu_personal_model_t *out, const char *contact_handle,
                                              const char *path);

/* Ingest a message into the per-contact personal model and save atomically.
 *
 * Extracts facts from the message text, tags each fact with contact_handle
 * (per-contact scoping), and atomically saves to the database. The per-contact
 * model state is derived from the global model on load — this function
 * mutates the caller's model struct and persists it.
 *
 * Returns HU_OK on success, HU_ERR_IO on save failure, HU_ERR_INVALID_ARGUMENT
 * on missing required arguments. */
hu_error_t hu_personal_model_ingest_for_contact(hu_personal_model_t *model,
                                                const char *contact_handle, const char *message,
                                                size_t message_len, bool from_user, int64_t ts,
                                                const char *db_path);

/* Infer the user's chronotype from observed `active_hours` distribution.
 *
 * The personal model accumulates a coarse hour-of-day histogram on every
 * ingest (`active_hours[24]`, capped at 255 per bucket). Once enough
 * samples have been observed, this function classifies the histogram into
 * one of four chronotype buckets — closing the loop with the B16
 * chronotype-aware quiet-hours scheduler so proactive messaging respects
 * a learned schedule, not just a manually-configured persona field.
 *
 * Decision rule (kept deliberately simple to avoid overfitting on
 * sparse data):
 *   - early    = sum(active_hours[5..9])    — the morning-lark window
 *   - late     = sum(active_hours[21..23])  + active_hours[0]
 *                                           — the evening-owl window
 *   - middle   = sum(active_hours[10..20])  — the intermediate window
 *   - total    = sum across all 24 hours
 *
 *   if total < HU_PM_CHRONOTYPE_MIN_SAMPLES (30):  HU_CHRONO_UNKNOWN
 *   else if early >= 1.5 * late and early >= 0.4 * total:
 *                                                  HU_CHRONO_MORNING_LARK
 *   else if late  >= 1.5 * early and late  >= 0.4 * total:
 *                                                  HU_CHRONO_EVENING_OWL
 *   else:                                          HU_CHRONO_INTERMEDIATE
 *
 * The 1.5× ratio + 40% concentration thresholds keep the classifier
 * conservative — a flat or near-flat distribution returns
 * HU_CHRONO_INTERMEDIATE rather than picking a side on noise.
 *
 * Pure observation; no I/O. Safe to call on every turn — work is O(24). */
hu_chronotype_t hu_personal_model_infer_chronotype(const hu_personal_model_t *model);

/* Resolve the default on-disk location for the personal model.
 *
 * Resolution order:
 *   1. `HUMAN_PERSONAL_MODEL_PATH` environment variable (full path, overrides
 *      everything; intended for tests and explicit operator overrides).
 *   2. `$HOME/.human/models/personal_model.db` (SQLite, per stakeholder decision).
 *
 * Writes the resolved path into `buf` and returns it (NUL-terminated). Returns
 * NULL when neither override nor `HOME` is available, or when the resolved
 * path would overflow `cap`. The caller owns `buf`. Pure path resolution —
 * no I/O, no allocation. */
const char *hu_personal_model_resolve_default_path(char *buf, size_t cap);

/* ── Symmetric signal aging — topics, goals, style ─────────────────────
 *
 * Facts have `last_seen_at` + exponential-decay confidence via
 * `hu_heuristic_fact_effective_confidence` (see fact_extract.h). The
 * three blocks below extend the same pattern to topics, goals, and
 * communication style so the personal model ages out stale signal
 * uniformly — no asymmetry between "facts that decay" and "topics
 * that dominate forever." Same lookup-table approximation, same
 * "raw value × 0.5^(age / half_life)" decay shape, no math.h.
 *
 * Half-lives are tuned per signal:
 *   - Topics: 60 days. Conversation interests rotate fastest —
 *     a topic mentioned heavily 6 months ago is rarely current.
 *   - Goals: 120 days. Active commitments age slower than
 *     interests; a project pursued for a season has lasting
 *     relevance even after a few weeks of silence.
 *   - Style: 180 days. Communication style is the slowest-moving
 *     signal — formality and verbosity drift, but a year-old
 *     observation still carries useful prior. */

/* Topic interest decays from `last_mentioned`. After 60 days the
 * effective interest score is half the raw value; after 120 days
 * a quarter. Returns `topic->interest_score` unchanged when
 * `last_mentioned` is 0 (no decay data) or `now <= last_mentioned`.
 * Values are floored at 0 beyond ~10 half-lives. NULL-safe. */
#define HU_PM_TOPIC_INTEREST_HALF_LIFE_SEC ((int64_t)(60LL * 24 * 60 * 60))
float hu_personal_topic_effective_score(const hu_personal_topic_t *topic, int64_t now);

/* Active goals carry an implicit priority of 1.0 right after a
 * reference, decaying with `last_referenced`. Inactive goals
 * always return 0. Returns 0 when last_referenced is 0 AND
 * created_at is also 0 (genuinely-empty slot) — a freshly-created
 * goal whose `last_referenced` is still at `created_at` is treated
 * as just-touched and returns 1.0. Floored at 0 beyond ~10
 * half-lives. NULL-safe. */
#define HU_PM_GOAL_RELEVANCE_HALF_LIFE_SEC ((int64_t)(120LL * 24 * 60 * 60))
float hu_personal_goal_effective_priority(const hu_personal_goal_t *goal, int64_t now);

/* Recently-completed goals — once a goal is resolved (active=false +
 * last_referenced bumped to the resolution time) we keep the slot
 * alive for `HU_PM_COMPLETED_GOAL_RETAIN_SEC` so the prompt builder
 * can surface a "Recently completed: …" line. After the retention
 * window the goal is pruned by apply_decay like any other stale
 * entry. 7 days is long enough to catch follow-up conversations
 * about the just-finished goal but short enough that the slot
 * doesn't dominate the array forever. */
#define HU_PM_COMPLETED_GOAL_RETAIN_SEC ((int64_t)(7LL * 24 * 60 * 60))

/* True when the goal is inactive but `last_referenced` is within
 * `HU_PM_COMPLETED_GOAL_RETAIN_SEC` of `now`. Active goals always
 * return false. NULL-safe. */
bool hu_personal_goal_is_recently_completed(const hu_personal_goal_t *goal, int64_t now);

/* Bulk getter — fill `out_buf` with pointers to every goal in the
 * model that satisfies `hu_personal_goal_is_recently_completed(now)`,
 * up to `out_cap` entries. Returns the number of pointers written.
 *
 * The pointers alias into `model->goals` and remain valid only
 * until the next mutation of the model (ingest, decay, save/load).
 * Callers that need to retain values across mutations should copy
 * the relevant fields before triggering further work. The order
 * of returned pointers matches the array order in `model->goals`,
 * which the loader/writer preserves byte-for-byte.
 *
 * NULL-safe on every argument: NULL `model` or NULL `out_buf` or
 * `out_cap == 0` returns 0 with no writes. The function never
 * exceeds `out_cap` even if more goals qualify — callers can
 * detect truncation by comparing the return value to `out_cap`
 * and re-running with a larger buffer (or by walking the model
 * directly).
 *
 * Used by downstream surfaces that need the same "Recently
 * completed: …" signal the prompt builder uses, but in a form
 * they can iterate (planner follow-up suggestions, channel-
 * specific congratulation messages, observability hooks). The
 * prompt builder still walks `model->goals` directly — there's
 * no inversion of control problem because it doesn't need the
 * pointer array, just a single pass. */
size_t hu_personal_model_get_recently_completed_goals(const hu_personal_model_t *model, int64_t now,
                                                      const hu_personal_goal_t **out_buf,
                                                      size_t out_cap);

/* Render a short comma-separated list of recently-completed goal
 * descriptions into `buf`, sized to fit within `cap` bytes
 * (NUL-terminator included). Returns the number of bytes written
 * (excluding the NUL), or 0 when no goals match or `buf`/`cap` is
 * invalid. The result is always NUL-terminated when `cap > 0`.
 *
 * The output is suitable for one-line observability events
 * ("goals completed: ship feature, finish report"), status output,
 * and channel-specific follow-up message prompts. When the list
 * doesn't fit, the function truncates at the last full description
 * + ellipsis ("ship feature, …") rather than mid-word — keeps
 * downstream log parsers from seeing partial UTF-8.
 *
 * NULL-safe on `model` and `buf`. */
size_t hu_personal_model_describe_recently_completed(const hu_personal_model_t *model, int64_t now,
                                                     char *buf, size_t cap);

/* Style freshness — how recent the observed-style aggregate is.
 * Returns 1.0 right after the latest observation, decaying to 0.5
 * after one half-life (180 days), 0.25 after two. Returns 0 when
 * `last_observed_at` is 0 (style was never observed). The prompt
 * builder gates the "Mirror their style" directive on freshness
 * crossing 0.3 so a year-old style fingerprint doesn't shape a
 * fresh conversation when the user has been quiet for months and
 * may have shifted tone. NULL-safe. */
#define HU_PM_STYLE_OBSERVATION_HALF_LIFE_SEC ((int64_t)(180LL * 24 * 60 * 60))
float hu_personal_communication_style_freshness(const hu_communication_style_t *style, int64_t now);

/* Drift toward neutral as freshness fades — returns a copy of `style`
 * with each percentage axis (formality, verbosity, emoji_frequency,
 * humor_receptivity, lowercase_ratio, abbreviation_ratio) blended
 * with the neutral midpoint (0.5) proportionally to (1 - freshness):
 *
 *     blended_axis = raw_axis * freshness + 0.5 * (1 - freshness)
 *
 * At freshness=1.0 (just-observed) we keep the raw EWMA value; at
 * freshness=0.0 (one half-life × log(0)) we'd be fully neutral.
 * Used by the prompt builder so the "Mirror their style" directive
 * smoothly fades toward neutral instead of hitting a hard cliff at
 * the existing 0.3 freshness gate. The `avg_message_length` and
 * `sample_count` fields pass through unchanged because they aren't
 * 0..1 axes — message-length neutrality is meaningless and the
 * sample count is a tally of observations, not an estimate of a
 * value. NULL-safe; returns a zero-initialized style on NULL input. */
hu_communication_style_t
hu_personal_communication_style_blend_with_freshness(const hu_communication_style_t *style,
                                                     int64_t now);

/* Track D D2.2 — offline persona-fidelity scorer.
 *
 * Given a `target` communication style (typically the personal
 * model's EWMA-tracked style fingerprint) and a `response` text,
 * compute a deterministic [0,1] fidelity score where 1.0 means the
 * response perfectly matches the target's style axes and 0.0 means
 * maximally different. The score is the mean of three axis matches:
 *
 *   1. lowercase_ratio  — observed letter case in `response` vs target
 *   2. abbreviation use — "u", "rn", "btw", "ty", "lmk", "yw" frequency
 *   3. length match     — bytes(response) vs target->avg_message_length
 *
 * Used as the floor for an offline LoRA A/B comparison: score the
 * frontier model's pre-LoRA responses, then re-score with LoRA, and
 * the delta tells you whether the adapter is actually personalizing
 * (D2.2 of the master follow-through plan). The scorer is fully
 * deterministic and never calls a provider — it's a feature-match
 * heuristic, not a quality judgment.
 *
 * Returns:
 *   - [0.0, 1.0] on success.
 *   - -1.0f on NULL `target` or NULL `response` or `response_len == 0`,
 *     or when `target->sample_count == 0` (no fingerprint to score
 *     against). Callers should treat -1.0 as "no comparison possible". */
float hu_communication_style_fidelity_score(const hu_communication_style_t *target,
                                            const char *response, size_t response_len);

/* Per-set summary of fidelity scores produced by
 * `hu_communication_style_compare_response_sets`. */
typedef struct hu_communication_style_set_summary {
    size_t scored;   /* number of responses that yielded a valid (>=0) score */
    size_t skipped;  /* NULL / empty / score=-1 responses skipped */
    float mean;      /* mean fidelity across `scored` responses; 0.f when scored == 0 */
    float min_score; /* min fidelity across `scored` responses; 1.f when scored == 0 */
    float max_score; /* max fidelity across `scored` responses; 0.f when scored == 0 */
} hu_communication_style_set_summary_t;

/* Track D D2.2 — A/B comparator for two response sets.
 *
 * Score every response in `set_a[]` and `set_b[]` against `target`,
 * accumulate into per-set summaries, and return the mean delta
 * `b.mean - a.mean` via `*out_delta`. The intended use is offline
 * LoRA evaluation: pass the frontier model's pre-LoRA outputs as
 * `set_a` and the post-LoRA outputs as `set_b`. A positive delta
 * indicates the adapter is pulling the model toward persona
 * fidelity; near-zero or negative means the LoRA isn't doing much
 * (or is hurting).
 *
 * Both sets are simple `const char *[]` arrays with explicit length
 * arrays so the caller controls binary safety (the JSON loader
 * doesn't need to NUL-terminate every response). NULL `set_*` is
 * allowed when the corresponding `n_*` is 0.
 *
 * Returns HU_OK and writes both summaries + delta on success, even
 * when one or both sets contain only un-scoreable responses (the
 * summary's `scored=0` signals the caller to ignore the mean).
 * Returns HU_ERR_INVALID_ARGUMENT on NULL `target`, `out_a`,
 * `out_b`, or `out_delta`, or when `target->sample_count == 0`. */
hu_error_t hu_communication_style_compare_response_sets(
    const hu_communication_style_t *target, const char *const *set_a, const size_t *lens_a,
    size_t n_a, const char *const *set_b, const size_t *lens_b, size_t n_b,
    hu_communication_style_set_summary_t *out_a, hu_communication_style_set_summary_t *out_b,
    float *out_delta);

/* Phase 5 Task 1 (RL SOTA) — opt-in 4-axis fidelity scorer (v2).
 *
 * Identical contract to `hu_communication_style_fidelity_score`
 * except the mean is over FOUR axes instead of three:
 *
 *   1. lowercase_ratio       (v1 axis — unchanged)
 *   2. abbreviation_ratio    (v1 axis — unchanged)
 *   3. length match          (v1 axis — unchanged)
 *   4. decision-style match  (NEW)   — composite of hedging /
 *      question / imperative sub-axes derived from the response
 *      text and the target's EWMA-tracked `*_ratio` fields. When
 *      all three target sub-axes are zero (no decision-style
 *      fingerprint observed yet), the composite collapses to the
 *      neutral 0.5 so an un-fingerprinted target neither rewards
 *      nor penalises the response on this axis.
 *
 * Round-1 BLOCKER-1 contract: this is an ADDITIVE, OPT-IN symbol.
 * The v1 entry point above is NEVER renamed, deprecated, or
 * forwarded through a shim — its body remains byte-identical, so
 * existing call sites (personal_model.c per-set scorer, ml/cli.c,
 * ml/fidelity.c) continue to receive the 3-axis result they always
 * did. Callers that want the 4th axis (eval gate, competitive
 * harness in Phase 5 Tasks 5 + 9) opt in by calling `_score_v2`.
 *
 * Returns / failure modes are identical to v1:
 *   - [0.0, 1.0] on success.
 *   - -1.0f on NULL `target`, NULL/empty `response`, or
 *     `target->sample_count == 0`. */
float hu_communication_style_fidelity_score_v2(const hu_communication_style_t *target,
                                               const char *response, size_t response_len);

/* Phase 5 Task 1 (RL SOTA) — opt-in 4-axis batch comparator.
 *
 * Same shape and failure modes as
 * `hu_communication_style_compare_response_sets`, but every
 * response is scored through the v2 (4-axis) scorer rather than v1.
 * Used by the Phase 5 Task 9 competitive harness to compare two
 * response sets (baseline vs RL policy) under the decision-style-
 * aware metric. A positive `*out_delta` indicates set B is closer
 * to the target style than set A.
 *
 * The v1 comparator above stays byte-identical (per round-1
 * BLOCKER-1: never modify v1 callers in place); this is a separate
 * symbol with its own internal per-set scorer. */
hu_error_t hu_communication_style_compare_response_sets_v2(
    const hu_communication_style_t *target, const char *const *set_a, const size_t *lens_a,
    size_t n_a, const char *const *set_b, const size_t *lens_b, size_t n_b,
    hu_communication_style_set_summary_t *out_a, hu_communication_style_set_summary_t *out_b,
    float *out_delta);

/* Goal completion detection — walk every active goal and deactivate
 * the ones the user's message indicates they've finished. The signal
 * is intentionally conservative: a goal is deactivated only when the
 * message contains BOTH a completion verb (e.g. "shipped",
 * "finished", "done", "wrapped up", "completed") AND a 5+ char
 * content word from the goal description, with no negation
 * ("not", "n't", "without") in the 12 chars before the completion
 * verb. False positives mark a goal inactive and silently drop its
 * prompt influence, so we'd rather miss legitimate completions than
 * fabricate them.
 *
 * On match: sets `active = false`, `progress = 1.0f`. The goal is
 * NOT removed from the array — `apply_decay` will eventually prune
 * inactive goals on a future tick. Keeping the slot occupied for one
 * decay cycle lets the prompt builder optionally surface "Recently
 * completed" entries (a future enhancement).
 *
 * Returns the number of goals deactivated. NULL-safe; messages of
 * length 0 are no-ops. */
size_t hu_personal_model_resolve_goals_in_message(hu_personal_model_t *model, const char *msg,
                                                  size_t msg_len, int64_t now);

/* Goal mention detection — walk every active goal and bump
 * `last_referenced` to `now` when the user's message contains a
 * content word from the goal description. Content words are 5+
 * characters and case-folded; stopwords are NOT excluded
 * (the length filter is heuristic enough for most goals like
 * "ship the new feature" or "learn esperanto"). The first
 * matching word per goal triggers the bump and we move on —
 * no ranking, no scoring, just a freshness signal so the
 * effective-priority decay restarts.
 *
 * Returns the number of goals whose `last_referenced` was bumped.
 * NULL-safe on every argument; messages of length 0 are no-ops. */
size_t hu_personal_model_touch_goals_in_message(hu_personal_model_t *model, const char *msg,
                                                size_t msg_len, int64_t now);

/* Bundle of per-turn personal-model maintenance counters returned
 * by `hu_personal_model_per_turn_tick`. Reports what each phase did
 * so callers can log or test the wiring without re-running the
 * helpers. All counts default to 0 on early-return paths. */
typedef struct hu_personal_model_turn_tick_result {
    hu_error_t ingest_error; /* result of hu_personal_model_ingest */
    size_t goals_touched;    /* count from touch_goals_in_message */
    size_t goals_resolved;   /* count from resolve_goals_in_message */
    size_t entries_pruned;   /* count from apply_decay */
} hu_personal_model_turn_tick_result_t;

/* Run the canonical per-turn maintenance sequence on a personal
 * model: ingest → touch_goals → resolve_goals → apply_decay.
 *
 * The `agent_turn.c` production path runs this exact sequence;
 * exposing it as a single helper has three benefits:
 *
 *   1. The order is testable. The unit-test path can call this
 *      helper directly (no HU_IS_TEST guard, no daemon plumbing)
 *      and assert that touch happens before resolve happens before
 *      decay — the order matters because a freshly-touched goal
 *      shouldn't be pruned in the same turn it was just touched,
 *      and a freshly-resolved goal becomes inactive before decay
 *      sees it.
 *
 *   2. New maintenance phases (e.g. style cleanup, contradiction
 *      detection) get one place to land instead of scattering
 *      through agent_turn.c.
 *
 *   3. Other call sites (agent shutdown, daemon idle tick) can
 *      reuse the same sequence without copy-paste.
 *
 * Pure CPU. NULL-safe on every argument; messages of length 0
 * still ingest (some callers want the temporal-bump side effect).
 *
 * Returns the per-phase counter struct. The aggregate is non-fatal:
 * if `ingest_error != HU_OK` the helper still runs the goal/decay
 * phases on whatever state ingest produced (empty model is fine). */
hu_personal_model_turn_tick_result_t hu_personal_model_per_turn_tick(hu_personal_model_t *model,
                                                                     const char *msg,
                                                                     size_t msg_len, bool from_user,
                                                                     int64_t now);

/* Periodic decay tick — sweep every fact, topic, and goal, and
 * remove entries whose effective_* score has fallen below the
 * forgetting floor (0.05). Compacts arrays in place; the model's
 * fact_count / topic_count / goal_count are updated to reflect
 * the survivors.
 *
 * Style aggregates are NOT pruned (there's only one slot, and
 * the prompt builder already gates the directive on freshness).
 *
 * Idempotent: calling twice in a row at the same `now` is a no-op,
 * because the entries that survived the first pass also survive
 * the second. Running it once per day keeps the in-memory model
 * from accumulating dead slots and freeing room for fresh signal
 * — without it, the eviction policy can only run when the array
 * is full, which lets stale entries crowd out new ones for months.
 *
 * Returns the total number of entries pruned across all categories
 * (facts + topics + goals). Pure CPU; no I/O. */
size_t hu_personal_model_apply_decay(hu_personal_model_t *model, int64_t now);

/* Rate-limit helper for periodic maintenance ticks (decay, autosave,
 * etc.). Returns true when `now - last >= interval`, OR when `last == 0`
 * (first-call semantics: always run once at startup). When the function
 * returns true, the caller is expected to perform the maintenance work
 * AND update `*last_inout` to `now` so the next call sees the new
 * baseline. When the function returns false, `*last_inout` is left
 * untouched.
 *
 * Pulled out as a single-purpose helper so the daemon's hourly
 * personal-model decay path (`src/daemon.c`) can be unit-tested
 * without booting the whole daemon. The function has zero
 * dependencies on the personal model itself — it's a pure
 * is-it-time-yet predicate over (last, now, interval) — but lives
 * here because every current caller is a personal-model maintenance
 * site and the alternative (a `core/scheduling.h`) would be one
 * symbol with no other tenants.
 *
 * Returns false when:
 *   - `last_inout` is NULL
 *   - `now` is non-positive (defensive: a clock returning 0 or a
 *     negative `time_t` shouldn't license maintenance work)
 *   - `interval` is non-positive (a zero or negative interval is
 *     a programmer error; refusing to run avoids tight loops on it)
 *   - `now - *last_inout < interval` (and `*last_inout > 0`)
 */
bool hu_personal_model_idle_due(int64_t *last_inout, int64_t now, int64_t interval);

/* Forgetting floor — the effective-score threshold below which a
 * fact / topic / goal is removed by `hu_personal_model_apply_decay`.
 * Exposed for tests so the behavior under specific decay fixtures
 * is verifiable without copying the constant. */
#define HU_PM_FORGET_FLOOR 0.05f

#endif /* HU_MEMORY_PERSONAL_MODEL_H */
