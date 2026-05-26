#ifndef HUMAN_CHANNELS_IMESSAGE_ACTION_FACTS_H
#define HUMAN_CHANNELS_IMESSAGE_ACTION_FACTS_H

#include "human/channels/imessage_action.h" /* hu_reply_style_facts_t */
#include "human/persona.h"                  /* hu_persona_t */
#include <stdbool.h>
#include <stdint.h>

/* A snapshot of recent conversation activity, supplied by the caller. F1
 * keeps the chat.db read out of the predicate-facts builder so the builder
 * stays trivially testable. The dispatcher (F2) will populate this from
 * actual chat.db queries; tests construct it inline. */
typedef struct {
    int64_t parent_seconds_ago;        /* age of the inbound being replied to */
    int parent_position_from_bottom;   /* 0 = most recent, N = older */
    bool parent_is_question;           /* did the parent end with ? */
    int parent_emotional_intensity;    /* HU_EMOTION_THRESHOLD_* */
    int pending_questions_in_window;   /* unresolved Qs in last 10 inbound */
    int other_threaded_replies_recent; /* their threading in last 20 of theirs */
    int our_threaded_replies_recent;   /* our consistency in last 20 outbound */
    float conv_density_msgs_per_min;   /* over last 5-min window */
} hu_conversation_snapshot_t;

/* Populate `facts` from `snapshot` + `persona`. Pure: no I/O. */
void hu_imessage_build_reply_facts(const hu_conversation_snapshot_t *snapshot,
                                   const hu_persona_t *persona, hu_reply_style_facts_t *facts_out);

#endif
