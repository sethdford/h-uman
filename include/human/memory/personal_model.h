#ifndef HU_MEMORY_PERSONAL_MODEL_H
#define HU_MEMORY_PERSONAL_MODEL_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/fact_extract.h"
#include "human/memory/tiers.h"
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
    int64_t deadline; /* 0 = no deadline */
    float progress;   /* 0.0-1.0 estimated progress */
} hu_personal_goal_t;

typedef struct hu_communication_style {
    float formality;             /* 0.0 (casual) to 1.0 (formal) */
    float verbosity;             /* 0.0 (terse) to 1.0 (verbose) */
    float emoji_frequency;       /* 0.0 (never) to 1.0 (heavy) */
    float humor_receptivity;     /* 0.0 (serious) to 1.0 (playful) */
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
} hu_personal_model_t;

/* Initialize a personal model with defaults. */
void hu_personal_model_init(hu_personal_model_t *model);

/* Build a prompt context block from the personal model.
 * Writes a human-readable summary into buf. Returns bytes written. */
size_t hu_personal_model_build_prompt(const hu_personal_model_t *model, char *buf, size_t cap);

/* True when the model carries any concrete signal worth injecting into a
 * system prompt (facts, topics, goals, named identity, or observed style).
 * Lets callers skip prompt-block injection on a fresh / unused model so we
 * don't waste tokens on "(No detailed personal data yet.)" noise. */
bool hu_personal_model_has_content(const hu_personal_model_t *model);

/* Ingest a new message into the personal model.
 * Updates facts, style metrics, topics, and temporal patterns. */
hu_error_t hu_personal_model_ingest(hu_personal_model_t *model, const char *message,
                                    size_t message_len, bool from_user, int64_t timestamp);

/* Merge facts from a fact extraction result into the model. */
hu_error_t hu_personal_model_merge_facts(hu_personal_model_t *model,
                                         const hu_fact_extract_result_t *facts);

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
hu_error_t hu_personal_model_contradicts_user(const hu_personal_model_t *model,
                                              const char *message, size_t message_len,
                                              bool *out_contradicts);

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
 *   2. `$HOME/.human/personal_model.bin`.
 *
 * Writes the resolved path into `buf` and returns it (NUL-terminated). Returns
 * NULL when neither override nor `HOME` is available, or when the resolved
 * path would overflow `cap`. The caller owns `buf`. Pure path resolution —
 * no I/O, no allocation. */
const char *hu_personal_model_resolve_default_path(char *buf, size_t cap);

#endif /* HU_MEMORY_PERSONAL_MODEL_H */
