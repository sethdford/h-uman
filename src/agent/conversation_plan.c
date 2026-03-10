#include "human/agent/conversation_plan.h"
#include "human/core/string.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define TARGET_LENGTH_CAP           300
#define DEEP_CONVERSATION_THRESHOLD 1000

static const char *const EMOTIONAL_KEYWORDS[] = {
    "sad",          "happy",    "angry",       "frustrated", "worried", "anxious", "excited",
    "disappointed", "stressed", "overwhelmed", "lonely",     "scared",  "hurt",
};
#define EMOTIONAL_KEYWORD_COUNT (sizeof(EMOTIONAL_KEYWORDS) / sizeof(EMOTIONAL_KEYWORDS[0]))

static bool contains_emotional_keyword(const char *msg, size_t len) {
    for (size_t i = 0; i < EMOTIONAL_KEYWORD_COUNT; i++) {
        const char *kw = EMOTIONAL_KEYWORDS[i];
        size_t kw_len = strlen(kw);
        for (size_t j = 0; j + kw_len <= len; j++) {
            if (strncasecmp(msg + j, kw, kw_len) != 0)
                continue;
            bool start_ok = (j == 0) || !isalnum((unsigned char)msg[j - 1]);
            bool end_ok = (j + kw_len >= len) || !isalnum((unsigned char)msg[j + kw_len]);
            if (start_ok && end_ok)
                return true;
        }
    }
    return false;
}

static bool contains_question(const char *msg, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (msg[i] == '?')
            return true;
    }
    return false;
}

static bool mentions_new_topic(const char *msg, size_t len) {
    /* Simple heuristic: "new", "about", "tell me", "what about", "regarding" */
    const char *patterns[] = {"new ", "about ", "tell me", "what about", "regarding "};
    for (size_t p = 0; p < sizeof(patterns) / sizeof(patterns[0]); p++) {
        size_t plen = strlen(patterns[p]);
        for (size_t i = 0; i + plen <= len; i++) {
            if (strncasecmp(msg + i, patterns[p], plen) == 0)
                return true;
        }
    }
    return false;
}

static const char *intent_name(hu_plan_intent_t intent) {
    switch (intent) {
    case HU_PLAN_RESPOND:
        return "respond";
    case HU_PLAN_REDIRECT:
        return "redirect";
    case HU_PLAN_DEEPEN:
        return "deepen";
    case HU_PLAN_LIGHTEN:
        return "lighten";
    case HU_PLAN_VALIDATE:
        return "validate";
    case HU_PLAN_INFORM:
        return "inform";
    default:
        return "respond";
    }
}

hu_error_t hu_plan_conversation(hu_allocator_t *alloc, const char *user_message,
                                size_t user_msg_len,
                                const char *conversation_history __attribute__((unused)),
                                size_t history_len, const char *emotional_context,
                                size_t emotional_len, hu_conversation_plan_t *plan) {
    if (!alloc || !plan)
        return HU_ERR_INVALID_ARGUMENT;

    memset(plan, 0, sizeof(*plan));

    plan->primary_intent = HU_PLAN_RESPOND;

    if (user_message && user_msg_len > 0) {
        if (user_msg_len < 20 && contains_emotional_keyword(user_message, user_msg_len))
            plan->primary_intent = HU_PLAN_VALIDATE;
        else if (mentions_new_topic(user_message, user_msg_len))
            plan->primary_intent = HU_PLAN_INFORM;
        else if (contains_question(user_message, user_msg_len))
            plan->primary_intent = HU_PLAN_RESPOND;
    }

    /* Target length: roughly match message length, cap at 300 */
    plan->target_length = user_msg_len > 0 ? user_msg_len : 80;
    if (plan->target_length > TARGET_LENGTH_CAP)
        plan->target_length = TARGET_LENGTH_CAP;

    /* Ask follow-up if conversation is deep */
    plan->should_ask_question = (history_len > DEEP_CONVERSATION_THRESHOLD);

    /* Tone guidance from emotional context */
    if (emotional_context && emotional_len > 0) {
        plan->tone_guidance = hu_strndup(alloc, emotional_context, emotional_len);
        if (plan->tone_guidance)
            plan->tone_guidance_len = emotional_len;
    }

    return HU_OK;
}

hu_error_t hu_plan_build_prompt(const hu_conversation_plan_t *plan, hu_allocator_t *alloc,
                                char **out, size_t *out_len) {
    if (!plan || !alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    *out = NULL;
    *out_len = 0;

    char buf[1024];
    size_t pos = 0;

    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos,
                            "### Response Plan\nIntent: %s\nTone: %s\nTarget length: ~%zu chars\n",
                            intent_name(plan->primary_intent),
                            plan->tone_guidance && plan->tone_guidance_len > 0 ? plan->tone_guidance
                                                                               : "match context",
                            plan->target_length);

    if (plan->should_ask_question)
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "Ask a follow-up question.\n");
    if (plan->should_share_personal)
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos,
                                "Share something personal if relevant.\n");

    *out = hu_strndup(alloc, buf, pos);
    if (!*out)
        return HU_ERR_OUT_OF_MEMORY;
    *out_len = pos;
    return HU_OK;
}

void hu_plan_deinit(hu_conversation_plan_t *plan, hu_allocator_t *alloc) {
    if (!plan || !alloc)
        return;
    if (plan->reasoning) {
        hu_str_free(alloc, plan->reasoning);
        plan->reasoning = NULL;
    }
    plan->reasoning_len = 0;
    if (plan->tone_guidance) {
        hu_str_free(alloc, plan->tone_guidance);
        plan->tone_guidance = NULL;
    }
    plan->tone_guidance_len = 0;
}
