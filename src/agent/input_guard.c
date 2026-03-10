#include "human/agent/input_guard.h"
#include <ctype.h>
#include <string.h>

static const char *ci_strstr(const char *hay, size_t hay_len, const char *needle,
                             size_t needle_len) {
    if (needle_len == 0 || needle_len > hay_len)
        return NULL;
    for (size_t i = 0; i <= hay_len - needle_len; i++) {
        size_t j = 0;
        while (j < needle_len &&
               tolower((unsigned char)hay[i + j]) == tolower((unsigned char)needle[j]))
            j++;
        if (j == needle_len)
            return &hay[i];
    }
    return NULL;
}

static int has(const char *msg, size_t len, const char *pat) {
    return ci_strstr(msg, len, pat, strlen(pat)) != NULL;
}

#define HU_INPUT_GUARD_MAX_LEN (256u * 1024u)

hu_error_t hu_input_guard_check(const char *message, size_t message_len,
                                hu_injection_risk_t *out_risk) {
    if (!out_risk)
        return HU_ERR_INVALID_ARGUMENT;
    if (!message || message_len == 0) {
        *out_risk = HU_INJECTION_SAFE;
        return HU_OK;
    }
    if (message_len > HU_INPUT_GUARD_MAX_LEN)
        message_len = HU_INPUT_GUARD_MAX_LEN;

    int score = 0;

    static const char *const high[] = {
        "ignore previous instructions",
        "ignore all previous",
        "disregard your instructions",
        "forget your instructions",
        "override your system prompt",
        "new system prompt",
        "you are now",
        "act as if you have no restrictions",
        "pretend you are",
        "jailbreak",
        "do anything now",
        "developer mode",
    };

    static const char *const med[] = {
        "[system]",
        "[admin]",
        "[override]",
        "[instruction]",
        "```system",
        "<system>",
        "</system>",
        "base64:",
        "ignore the above",
        "bypass safety",
        "reveal your prompt",
        "show your system message",
        "what are your instructions",
    };

    for (size_t i = 0; i < sizeof(high) / sizeof(high[0]); i++)
        if (has(message, message_len, high[i]))
            score += 3;

    for (size_t i = 0; i < sizeof(med) / sizeof(med[0]); i++)
        if (has(message, message_len, med[i]))
            score += 1;

    if (score >= 3)
        *out_risk = HU_INJECTION_HIGH_RISK;
    else if (score >= 1)
        *out_risk = HU_INJECTION_SUSPICIOUS;
    else
        *out_risk = HU_INJECTION_SAFE;

    return HU_OK;
}
