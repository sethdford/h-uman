#ifndef HU_CONTEXT_VOICE_INTENT_H
#define HU_CONTEXT_VOICE_INTENT_H

/* Voice-first memos (spec docs/superpowers/specs/2026-09-28-voice-first-memos-design.md):
 * decide voice from what arrived, BEFORE the reply is written, so the turn can
 * write a memo instead of a text that is later read aloud. Pure: facts in,
 * decision out. */

#include "human/context/voice_decision.h"
#include "human/persona.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *inbound; /* the inbound batch text */
    size_t inbound_len;
    const hu_voice_messages_config_t *cfg;
    bool has_voice_id;
    int64_t secs_since_last_memo; /* to this contact; -1 = never */
    uint32_t min_gap_sec;         /* 0 = no spacing */
} hu_voice_intent_facts_t;

/* Reason is one of "no_voice_id", "disabled", "they_sent_audio", "logistics",
 * "spacing", "heartfelt", "question_worth_talking", "no_trigger" (static,
 * never NULL when out_reason is non-NULL). */
/* A memo the turn actually produced: 12-110 words (Seth's memos run 47-58;
 * 110 words is ~42 s). A slim retry or canned fallback is not one. */
bool hu_voice_intent_memo_shaped(const char *text, size_t len);

/* Whitespace-separated words in s[0..len). */
size_t hu_voice_intent_word_count(const char *s, size_t len);

/* A short (<= 12 words) question about what time / where / when / address:
 * answers faster as text. */
bool hu_voice_intent_is_logistics(const char *s, size_t len);

/* HU_VOICE_MIN_GAP_SEC: a non-negative integer, else the 3 h default. */
uint32_t hu_voice_intent_parse_gap(const char *env);

hu_voice_decision_t hu_voice_intent_decide(const hu_voice_intent_facts_t *f,
                                           const char **out_reason);

#endif
