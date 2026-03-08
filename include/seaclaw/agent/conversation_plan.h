#ifndef SC_CONVERSATION_PLAN_H
#define SC_CONVERSATION_PLAN_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum sc_plan_intent {
    SC_PLAN_RESPOND,  /* direct response */
    SC_PLAN_REDIRECT, /* steer conversation */
    SC_PLAN_DEEPEN,   /* go deeper on current topic */
    SC_PLAN_LIGHTEN,  /* lighten the mood */
    SC_PLAN_VALIDATE, /* validate their feelings */
    SC_PLAN_INFORM,   /* share relevant information */
} sc_plan_intent_t;

typedef struct sc_conversation_plan {
    sc_plan_intent_t primary_intent;
    char *reasoning;
    size_t reasoning_len;
    char *tone_guidance;
    size_t tone_guidance_len;
    size_t target_length; /* suggested response length in chars */
    bool should_ask_question;
    bool should_share_personal;
} sc_conversation_plan_t;

sc_error_t sc_plan_conversation(sc_allocator_t *alloc, const char *user_message,
                                size_t user_msg_len, const char *conversation_history,
                                size_t history_len, const char *emotional_context,
                                size_t emotional_len, sc_conversation_plan_t *plan);

sc_error_t sc_plan_build_prompt(const sc_conversation_plan_t *plan, sc_allocator_t *alloc,
                                char **out, size_t *out_len);

void sc_plan_deinit(sc_conversation_plan_t *plan, sc_allocator_t *alloc);

#endif
