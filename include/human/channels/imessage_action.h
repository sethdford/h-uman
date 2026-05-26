#ifndef HUMAN_CHANNELS_IMESSAGE_ACTION_H
#define HUMAN_CHANNELS_IMESSAGE_ACTION_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    HU_REPLY_STYLE_FLAT = 0,
    HU_REPLY_STYLE_THREADED = 1,
    HU_REPLY_STYLE_TAPBACK = 2,
    HU_REPLY_STYLE_TAPBACK_PLUS_FLAT = 3,
} hu_reply_style_t;

#define HU_EMOTION_THRESHOLD_LOW    1
#define HU_EMOTION_THRESHOLD_MEDIUM 2
#define HU_EMOTION_THRESHOLD_HIGH   3

typedef struct {
    /* Time / position. */
    int64_t seconds_since_parent;
    int parent_position_from_bottom; /* 0 = most recent inbound */

    /* Conversational context. */
    int pending_questions_in_window;   /* unresolved Qs in last 10 inbound msgs */
    int other_threaded_replies_recent; /* their style — last 20 of their msgs */
    int our_threaded_replies_recent;   /* our consistency — last 20 outbound */
    float conv_density_msgs_per_min;   /* over last 5-min window */
    bool parent_was_a_question;        /* ends in ? or imperative shape */

    /* Persona. */
    float persona_formality;       /* [0..1] from existing persona.formality */
    float persona_thread_affinity; /* [0..1] new persona dial, default 0.3 */

    /* Emotional protection (AC-3). */
    int parent_emotional_intensity; /* enum HU_EMOTION_THRESHOLD_* */
} hu_reply_style_facts_t;

/* Test helper — exposes the underlying score so the truth table can assert
 * the right *probability* not just the sampled style. */
typedef struct {
    float p_thread;
    float p_tapback;
    float p_flat;
    float p_tapback_plus_flat;
} hu_reply_style_scores_t;

/* Pure. No I/O. Deterministic given facts + rng_seed. */
hu_reply_style_scores_t hu_imessage_score_reply_style(const hu_reply_style_facts_t *facts);

hu_reply_style_t hu_imessage_choose_reply_style(const hu_reply_style_facts_t *facts,
                                                uint64_t rng_seed);

#include <stddef.h>

#include "human/core/error.h"

/* One JSONL entry per reply-style decision + send attempt. */
typedef struct {
    int64_t ts_unix;                 /* unix epoch seconds */
    const char *target_chat_id_hash; /* opaque hex string, agent-supplied */
    hu_reply_style_facts_t facts;    /* full input facts */
    hu_reply_style_t style_chosen;   /* what the predicate picked */
    int send_result;                 /* hu_error_t int value, 0 = HU_OK */
    const char *tier_used;           /* "cmdR"|"ax_menu"|"flat_fallback"|"tapback" */
    int elapsed_ms;                  /* wall-clock for the send */
} hu_imessage_action_log_t;

/* Append one JSONL line to the action-log file. Resolves log dir from
 * env var HU_IMESSAGE_ACTION_LOG_DIR if set (for tests), else
 * ~/.human/logs/. Creates the dir if missing. Returns HU_OK on success.
 * Failure is non-fatal — the caller already did the real work. */
hu_error_t hu_imessage_action_log_jsonl(const hu_imessage_action_log_t *log);

#endif
