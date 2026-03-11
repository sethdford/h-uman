#ifndef HU_CONVERSATION_PLAN_H
#define HU_CONVERSATION_PLAN_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum hu_plan_intent {
    HU_PLAN_RESPOND,  /* direct response */
    HU_PLAN_REDIRECT, /* steer conversation */
    HU_PLAN_DEEPEN,   /* go deeper on current topic */
    HU_PLAN_LIGHTEN,  /* lighten the mood */
    HU_PLAN_VALIDATE, /* validate their feelings */
    HU_PLAN_INFORM,   /* share relevant information */
} hu_plan_intent_t;

typedef struct hu_conversation_plan {
    hu_plan_intent_t primary_intent;
    char *reasoning;
    size_t reasoning_len;
    char *tone_guidance;
    size_t tone_guidance_len;
    size_t target_length; /* suggested response length in chars */
    bool should_ask_question;
    bool should_share_personal;
} hu_conversation_plan_t;

hu_error_t hu_plan_conversation(hu_allocator_t *alloc, const char *user_message,
                                size_t user_msg_len, const char *conversation_history,
                                size_t history_len, const char *emotional_context,
                                size_t emotional_len, hu_conversation_plan_t *plan);

hu_error_t hu_plan_build_prompt(const hu_conversation_plan_t *plan, hu_allocator_t *alloc,
                                char **out, size_t *out_len);

void hu_conversation_plan_deinit(hu_conversation_plan_t *plan, hu_allocator_t *alloc);

#endif
