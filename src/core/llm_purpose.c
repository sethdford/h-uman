/* src/core/llm_purpose.c — see include/human/core/llm_purpose.h. */

#include "human/core/llm_purpose.h"

#include <stdio.h>

static _Thread_local hu_llm_purpose_t s_purpose = HU_LLM_PURPOSE_UNTAGGED;
static _Thread_local unsigned s_background_depth = 0;

static const char *const k_names[HU_LLM_PURPOSE__COUNT] = {
    [HU_LLM_PURPOSE_UNTAGGED] = "untagged",
    [HU_LLM_PURPOSE_REPLY] = "reply",
    [HU_LLM_PURPOSE_GUARD_RETRY] = "guard_retry",
    [HU_LLM_PURPOSE_PLANNER] = "planner",
    [HU_LLM_PURPOSE_EXTRACT] = "extract",
    [HU_LLM_PURPOSE_EMBED] = "embed",
    [HU_LLM_PURPOSE_PROACTIVE] = "proactive",
    [HU_LLM_PURPOSE_JUDGE] = "judge",
    [HU_LLM_PURPOSE_BACKGROUND] = "background",
    [HU_LLM_PURPOSE_COMMITMENT_CHECK] = "commitment_check",
    [HU_LLM_PURPOSE_MODERATION_CHECK] = "moderation_check",
};

const char *hu_llm_purpose_name(hu_llm_purpose_t purpose) {
    if ((unsigned)purpose >= (unsigned)HU_LLM_PURPOSE__COUNT)
        return k_names[HU_LLM_PURPOSE_UNTAGGED];
    return k_names[purpose];
}

bool hu_llm_purpose_is_background(hu_llm_purpose_t purpose) {
    switch (purpose) {
    case HU_LLM_PURPOSE_EMBED:
    case HU_LLM_PURPOSE_PROACTIVE:
    case HU_LLM_PURPOSE_BACKGROUND:
        return true;
    default:
        return false;
    }
}

hu_llm_purpose_t hu_llm_purpose_current(void) {
    return s_purpose;
}

hu_llm_purpose_t hu_llm_purpose_set(hu_llm_purpose_t purpose) {
    hu_llm_purpose_t prev = s_purpose;
    if ((unsigned)purpose < (unsigned)HU_LLM_PURPOSE__COUNT)
        s_purpose = purpose;
    return prev;
}

hu_llm_purpose_t hu_llm_purpose_set_if_untagged(hu_llm_purpose_t purpose) {
    hu_llm_purpose_t prev = s_purpose;
    if (prev == HU_LLM_PURPOSE_UNTAGGED)
        (void)hu_llm_purpose_set(purpose);
    return prev;
}

void hu_llm_background_enter(void) {
    s_background_depth++;
}

void hu_llm_background_exit(void) {
    if (s_background_depth > 0)
        s_background_depth--;
}

bool hu_llm_background_active(void) {
    return s_background_depth > 0;
}

size_t hu_llm_purpose_headers(hu_llm_purpose_t purpose, char *buf, size_t cap) {
    if (!buf || cap == 0)
        return 0;
    bool lane = hu_llm_background_active();
    bool batch = lane || hu_llm_purpose_is_background(purpose);
    if (lane && (purpose == HU_LLM_PURPOSE_REPLY || purpose == HU_LLM_PURPOSE_UNTAGGED))
        purpose = HU_LLM_PURPOSE_BACKGROUND;
    int n = snprintf(buf, cap, "X-HU-Purpose: %s\r\n%s", hu_llm_purpose_name(purpose),
                     batch ? "X-HU-Priority: batch\r\n" : "");
    if (n > 0 && (size_t)n < cap)
        return (size_t)n;
    buf[0] = '\0'; /* never a truncated header block */
    return 0;
}
