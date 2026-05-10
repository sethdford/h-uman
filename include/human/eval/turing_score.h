#ifndef HU_EVAL_TURING_SCORE_H
#define HU_EVAL_TURING_SCORE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include <stddef.h>
#include <stdint.h>

#define HU_TURING_DIM_COUNT 18

typedef enum {
    /* Original 12 text-based dimensions */
    HU_TURING_NATURAL_LANGUAGE = 0,
    HU_TURING_EMOTIONAL_INTELLIGENCE,
    HU_TURING_APPROPRIATE_LENGTH,
    HU_TURING_PERSONALITY_CONSISTENCY,
    HU_TURING_VULNERABILITY_WILLINGNESS,
    HU_TURING_HUMOR_NATURALNESS,
    HU_TURING_IMPERFECTION,
    HU_TURING_OPINION_HAVING,
    HU_TURING_ENERGY_MATCHING,
    HU_TURING_CONTEXT_AWARENESS,
    HU_TURING_NON_ROBOTIC,
    HU_TURING_GENUINE_WARMTH,
    /* S2S voice dimensions (arXiv:2602.24080 taxonomy) */
    HU_TURING_PROSODY_NATURALNESS,   /* intonation, rhythm, stress patterns */
    HU_TURING_TURN_TIMING,           /* response latency, turn-taking fluency */
    HU_TURING_FILLER_USAGE,          /* natural hesitations (um, uh, like) */
    HU_TURING_EMOTIONAL_PROSODY,     /* vocal emotion expression (not just word choice) */
    HU_TURING_CONVERSATIONAL_REPAIR, /* self-correction, rephrasing mid-utterance */
    HU_TURING_PARALINGUISTIC_CUES,   /* laughter, sighs, breath, vocal quality */
} hu_turing_dimension_t;

typedef enum {
    HU_TURING_HUMAN = 0,
    HU_TURING_BORDERLINE,
    HU_TURING_AI_DETECTED,
} hu_turing_verdict_t;

typedef struct hu_turing_score {
    int dimensions[HU_TURING_DIM_COUNT]; /* 1-10 per dimension */
    int overall;                         /* average across dimensions */
    hu_turing_verdict_t verdict;
    char ai_tells[256];
    char human_signals[256];
} hu_turing_score_t;

/* Evaluate a response for human-likeness using heuristic scoring.
 * conversation_context: recent messages for context (may be NULL).
 * response: the message being scored.
 * max_channel_chars: outbound cap from channel constraints (0 = use built-in
 * casual-text thresholds for structural/length tells).
 * Returns HU_OK on success, fills out. */
hu_error_t hu_turing_score_heuristic(const char *response, size_t response_len,
                                     const char *conversation_context, size_t context_len,
                                     uint32_t max_channel_chars, hu_turing_score_t *out);

/* Evaluate using an LLM judge for semantic scoring.
 * provider: the LLM to use as judge (should be cheap/fast model).
 * Returns HU_OK on success. Falls back to heuristic on LLM failure. */
hu_error_t hu_turing_score_llm(hu_allocator_t *alloc, hu_provider_t *provider, const char *model,
                               size_t model_len, const char *response, size_t response_len,
                               const char *conversation_context, size_t context_len,
                               hu_turing_score_t *out);

/* Apply channel-specific weight adjustments to a scored result.
 * Casual channels (discord, telegram, imessage, whatsapp) emphasize casual markers;
 * formal channels (email, slack) emphasize structure and length. */
void hu_turing_apply_channel_weights(hu_turing_score_t *score, const char *channel,
                                     size_t channel_len);

/* Adjust personality_consistency based on persona trait/vocab alignment.
 * Call after hu_turing_score_heuristic when persona data is available. */
void hu_turing_apply_persona_alignment(hu_turing_score_t *score,
                                       const char *response, size_t response_len,
                                       const char *const *traits, size_t traits_count,
                                       const char *const *preferred_vocab, size_t preferred_count,
                                       const char *const *avoided_vocab, size_t avoided_count);

/* Dimension name for display/logging. */
const char *hu_turing_dimension_name(hu_turing_dimension_t dim);

/* Verdict name for display/logging. */
const char *hu_turing_verdict_name(hu_turing_verdict_t verdict);

/* Format a score as a summary string. Caller owns result. */
char *hu_turing_score_summary(hu_allocator_t *alloc, const hu_turing_score_t *score,
                              size_t *out_len);

/* Trajectory scoring across a conversation (multi-turn). No SQLite required. */
typedef struct hu_turing_trajectory {
    float directional_alignment; /* 0-1: emotional trajectory toward user needs */
    float cumulative_impact;     /* 0-1: positive emotional impact over time */
    float stability;             /* 0-1: empathic behavior stability (no regression) */
    float overall;               /* weighted average */
} hu_turing_trajectory_t;

hu_error_t hu_turing_score_trajectory(const hu_turing_score_t *scores, size_t score_count,
                                      hu_turing_trajectory_t *out);

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

hu_error_t hu_turing_init_tables(sqlite3 *db);

hu_error_t hu_turing_store_score(sqlite3 *db, const char *contact_id, size_t contact_id_len,
                                 int64_t timestamp, const hu_turing_score_t *score);

#define HU_TURING_CONTACT_ID_MAX 128

hu_error_t hu_turing_get_trend(hu_allocator_t *alloc, sqlite3 *db, const char *contact_id,
                               size_t contact_id_len, size_t max_entries, hu_turing_score_t *scores,
                               int64_t *timestamps,
                               char (*out_contact_ids)[HU_TURING_CONTACT_ID_MAX],
                               size_t *out_count);

hu_error_t hu_turing_get_weakest_dimensions(sqlite3 *db, int *dimension_averages);

/* Per-contact dimension averages (last 30 days). */
hu_error_t hu_turing_get_contact_dimensions(sqlite3 *db, const char *contact_id,
                                            size_t contact_id_len, int *dimension_averages);

/* Per-channel dimension averages (last 30 days, derived from contact channel prefix). */
hu_error_t hu_turing_get_channel_dimensions(sqlite3 *db, const char *channel_name,
                                            size_t channel_name_len, int *dimension_averages);

/* A/B test for humanization parameters.
 * Tracks two parameter variants and their resulting Turing scores. */
#define HU_AB_TEST_NAME_MAX 64

typedef struct hu_ab_test {
    char name[HU_AB_TEST_NAME_MAX]; /* test name (e.g., "disfluency_freq") */
    float variant_a;                /* control value */
    float variant_b;                /* treatment value */
    int score_sum_a;                /* cumulative Turing score for variant A */
    int score_count_a;              /* number of observations for A */
    int score_sum_b;                /* cumulative Turing score for variant B */
    int score_count_b;              /* number of observations for B */
    bool active;                    /* test is still running */
} hu_ab_test_t;

hu_error_t hu_ab_test_init_table(sqlite3 *db);

hu_error_t hu_ab_test_create(sqlite3 *db, const char *name, float variant_a, float variant_b);

hu_error_t hu_ab_test_record(sqlite3 *db, const char *name, bool is_variant_b, int turing_score);

hu_error_t hu_ab_test_get_results(sqlite3 *db, const char *name, hu_ab_test_t *out);

/* Auto-resolve: if one variant is significantly better after enough observations, promote it. */
hu_error_t hu_ab_test_resolve(sqlite3 *db, const char *name, float *winning_value);
#endif

/* SQLite-independent Turing utilities (pure computation, no db needed). */

/* Build a contact-specific hint string from weak dimensions.
 * Caller owns returned string (free with alloc). Returns NULL if no weak dims. */
char *hu_turing_build_contact_hint(hu_allocator_t *alloc, const int *dimension_averages,
                                   size_t *out_len);

/* Pick A/B test variant (deterministic based on contact_id hash). */
bool hu_ab_test_pick_variant(const char *contact_id, size_t contact_id_len, const char *test_name);

#endif
