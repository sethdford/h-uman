/*
 * Constitutional AI — self-critique against configurable principles.
 * Evaluates responses against principles and rewrites if violated.
 */

#include "human/agent/constitutional.h"
#include "human/core/log.h"
#include "human/core/string.h"
#include "human/provider.h"
#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONST_DEFAULT_HELPFUL      "helpful"
#define CONST_DEFAULT_HELPFUL_DESC "Responses should directly address what the user needs."
#define CONST_DEFAULT_HARMLESS     "harmless"
#define CONST_DEFAULT_HARMLESS_DESC \
    "Responses must not cause harm, promote violence, or assist in dangerous activities."
#define CONST_DEFAULT_HONEST      "honest"
#define CONST_DEFAULT_HONEST_DESC "Responses must be truthful and not fabricate information."

#if defined(__GNUC__) || defined(__clang__)
#define CONST_UNUSED __attribute__((unused))
#else
#define CONST_UNUSED
#endif

hu_constitutional_config_t hu_constitutional_config_default(void) {
    hu_constitutional_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.principles[0].name = CONST_DEFAULT_HELPFUL;
    cfg.principles[0].name_len = sizeof(CONST_DEFAULT_HELPFUL) - 1;
    cfg.principles[0].description = CONST_DEFAULT_HELPFUL_DESC;
    cfg.principles[0].description_len = sizeof(CONST_DEFAULT_HELPFUL_DESC) - 1;
    cfg.principles[1].name = CONST_DEFAULT_HARMLESS;
    cfg.principles[1].name_len = sizeof(CONST_DEFAULT_HARMLESS) - 1;
    cfg.principles[1].description = CONST_DEFAULT_HARMLESS_DESC;
    cfg.principles[1].description_len = sizeof(CONST_DEFAULT_HARMLESS_DESC) - 1;
    cfg.principles[2].name = CONST_DEFAULT_HONEST;
    cfg.principles[2].name_len = sizeof(CONST_DEFAULT_HONEST) - 1;
    cfg.principles[2].description = CONST_DEFAULT_HONEST_DESC;
    cfg.principles[2].description_len = sizeof(CONST_DEFAULT_HONEST_DESC) - 1;
    cfg.principle_count = 3;
    cfg.enabled = true;
    cfg.rewrite_enabled = true;
    return cfg;
}

void hu_critique_result_free(hu_allocator_t *alloc, hu_critique_result_t *result) {
    if (!alloc || !result)
        return;
    if (result->revised_response) {
        hu_str_free(alloc, result->revised_response);
        result->revised_response = NULL;
    }
    result->revised_response_len = 0;
    if (result->reasoning) {
        hu_str_free(alloc, result->reasoning);
        result->reasoning = NULL;
    }
    result->reasoning_len = 0;
    result->verdict = HU_CRITIQUE_PASS;
    result->principle_index = -1;
}

/* Parse verdict from response: PASS, MINOR, or REWRITE at start. */
static CONST_UNUSED hu_critique_verdict_t parse_verdict(const char *resp, size_t resp_len,
                                                        int *principle_idx) {
    if (principle_idx)
        *principle_idx = -1;
    if (!resp || resp_len == 0)
        return HU_CRITIQUE_PASS;

    const char *p = resp;
    while (p < resp + resp_len && (isspace((unsigned char)*p) || *p == '\n'))
        p++;
    size_t len = (size_t)((resp + resp_len) - p);

    if (len >= 4 && (p[0] == 'P' || p[0] == 'p') && (p[1] == 'A' || p[1] == 'a') &&
        (p[2] == 'S' || p[2] == 's') && (p[3] == 'S' || p[3] == 's') &&
        (len == 4 || !isalnum((unsigned char)p[4])))
        return HU_CRITIQUE_PASS;
    if (len >= 5 && (p[0] == 'M' || p[0] == 'm') && (p[1] == 'I' || p[1] == 'i') &&
        (p[2] == 'N' || p[2] == 'n') && (p[3] == 'O' || p[3] == 'o') &&
        (p[4] == 'R' || p[4] == 'r') && (len == 5 || !isalnum((unsigned char)p[5])))
        return HU_CRITIQUE_MINOR;
    if (len >= 7 && (p[0] == 'R' || p[0] == 'r') && (p[1] == 'E' || p[1] == 'e') &&
        (p[2] == 'W' || p[2] == 'w') && (p[3] == 'R' || p[3] == 'r') &&
        (p[4] == 'I' || p[4] == 'i') && (p[5] == 'T' || p[5] == 't') &&
        (p[6] == 'E' || p[6] == 'e') && (len == 7 || !isalnum((unsigned char)p[7])))
        return HU_CRITIQUE_REWRITE;

    return HU_CRITIQUE_PASS;
}

/* Scan digits, capped to prevent overflow. Returns -1 if no digits found. */
static CONST_UNUSED int scan_capped_int(const char **pp, const char *end) {
    const char *p = *pp;
    while (p < end && (isspace((unsigned char)*p) || *p == '#' || *p == ':'))
        p++;
    if (p >= end || *p < '0' || *p > '9')
        return -1;
    int idx = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        idx = idx * 10 + (int)(*p - '0');
        if (idx > 999) {
            idx = 999;
            break;
        }
        p++;
    }
    *pp = p;
    return idx;
}

/* Extract principle index from response. Tries "principle N" first, then
 * falls back to the first integer after the verdict keyword. */
static CONST_UNUSED int parse_principle_index(const char *resp, size_t resp_len) {
    if (!resp || resp_len == 0)
        return -1;
    const char *p = resp;
    const char *end = resp + resp_len;
    /* Try "principle" keyword first */
    while (p + 9 <= end) {
        if ((p[0] == 'p' || p[0] == 'P') && (p[1] == 'r' || p[1] == 'R') &&
            (p[2] == 'i' || p[2] == 'I') && (p[3] == 'n' || p[3] == 'N') &&
            (p[4] == 'c' || p[4] == 'C') && (p[5] == 'i' || p[5] == 'I') &&
            (p[6] == 'p' || p[6] == 'P') && (p[7] == 'l' || p[7] == 'L') &&
            (p[8] == 'e' || p[8] == 'E')) {
            const char *after = p + 9;
            int idx = scan_capped_int(&after, end);
            if (idx >= 0)
                return idx;
        }
        p++;
    }
    /* Fallback: first integer after the verdict keyword */
    p = resp;
    while (p < end && (isspace((unsigned char)*p) || *p == '\n'))
        p++;
    /* Skip past verdict word */
    while (p < end && isalpha((unsigned char)*p))
        p++;
    {
        const char *fp = p;
        int idx = scan_capped_int(&fp, end);
        if (idx >= 0)
            return idx;
    }
    return -1;
}

hu_error_t hu_constitutional_critique(hu_allocator_t *alloc, hu_provider_t *provider,
                                      const char *model, size_t model_len, const char *user_msg,
                                      size_t user_msg_len, const char *response,
                                      size_t response_len, const hu_constitutional_config_t *config,
                                      hu_critique_result_t *result) {
    if (!alloc || !result)
        return HU_ERR_INVALID_ARGUMENT;
    memset(result, 0, sizeof(*result));
    result->principle_index = -1;

    if (!config || !config->enabled || config->principle_count == 0) {
        if (config && !config->enabled) {
            static atomic_bool warned_constitutional_disabled = false;
            hu_log_info_once(&warned_constitutional_disabled, "constitutional", NULL,
                             "constitutional AI critique disabled — "
                             "set constitutional.enabled=true in config.json "
                             "and define principles to enable self-critique");
        }
        result->verdict = HU_CRITIQUE_PASS;
        return HU_OK;
    }

#ifdef HU_IS_TEST
    /* Deterministic stub: no LLM calls, always return PASS */
    (void)provider;
    (void)model;
    (void)model_len;
    (void)user_msg;
    (void)user_msg_len;
    (void)response;
    (void)response_len;
    (void)config;
    result->verdict = HU_CRITIQUE_PASS;
    return HU_OK;
#else
    if (!provider || !provider->vtable || !provider->vtable->chat)
        return HU_ERR_NOT_SUPPORTED;
    if (!user_msg || !response)
        return HU_ERR_INVALID_ARGUMENT;
    if (user_msg_len == 0)
        user_msg_len = strlen(user_msg);
    if (response_len == 0)
        response_len = strlen(response);

    /* Build critique prompt: principles + user msg + response */
    size_t prompt_cap = 512;
    for (size_t i = 0; i < config->principle_count; i++) {
        prompt_cap += config->principles[i].name_len + config->principles[i].description_len + 8;
    }
    prompt_cap += user_msg_len + response_len + 256;

    char *critique_prompt = (char *)alloc->alloc(alloc->ctx, prompt_cap);
    if (!critique_prompt)
        return HU_ERR_OUT_OF_MEMORY;

    size_t pos = 0;
    pos = hu_buf_appendf(critique_prompt, prompt_cap, pos, "Given these principles:\n");
    for (size_t i = 0; i < config->principle_count && pos < prompt_cap - 1; i++) {
        const hu_principle_t *pr = &config->principles[i];
        int n = snprintf(critique_prompt + pos, prompt_cap - pos, "%zu. %.*s: %.*s\n", i + 1,
                         (int)pr->name_len, pr->name, (int)pr->description_len, pr->description);
        if (n > 0 && (size_t)n < prompt_cap - pos)
            pos += (size_t)n;
    }
    {
        int tail = snprintf(critique_prompt + pos, prompt_cap - pos,
                            "\nUser asked: %.*s\n\nAssistant responded: %.*s\n\n"
                            "Does the response violate any principle? "
                            "Reply with PASS, MINOR, or REWRITE followed by the principle number "
                            "and reasoning.",
                            (int)user_msg_len, user_msg, (int)response_len, response);
        if (tail > 0 && (size_t)tail < prompt_cap - pos)
            pos += (size_t)tail;
        else if (tail > 0)
            pos = prompt_cap - 1;
    }

    hu_chat_message_t msgs[2];
    memset(msgs, 0, sizeof(msgs));
    msgs[0].role = HU_ROLE_SYSTEM;
    msgs[0].content =
        "You are a safety critic. Evaluate the assistant response against the principles. "
        "Reply with PASS (no violation), MINOR (style issue, no rewrite needed), or REWRITE "
        "(needs revision). If not PASS, include the principle number and brief reasoning.";
    msgs[0].content_len = strlen(msgs[0].content);
    msgs[1].role = HU_ROLE_USER;
    msgs[1].content = critique_prompt;
    msgs[1].content_len = pos;

    hu_chat_request_t req;
    memset(&req, 0, sizeof(req));
    req.messages = msgs;
    req.messages_count = 2;
    req.model = model;
    req.model_len = model_len;
    req.temperature = 0.0;

    hu_chat_response_t resp;
    memset(&resp, 0, sizeof(resp));
    hu_error_t err =
        provider->vtable->chat(provider->ctx, alloc, &req, model, model_len, 0.0, &resp);

    if (err != HU_OK) {
        alloc->free(alloc->ctx, critique_prompt, prompt_cap);
        return err;
    }

    hu_critique_verdict_t verdict = HU_CRITIQUE_PASS;
    int principle_idx = -1;
    char *reasoning = NULL;
    size_t reasoning_len = 0;

    if (resp.content && resp.content_len > 0) {
        verdict = parse_verdict(resp.content, resp.content_len, &principle_idx);
        int explicit_idx = parse_principle_index(resp.content, resp.content_len);
        if (explicit_idx >= 0)
            principle_idx = explicit_idx;
        reasoning = hu_strndup(alloc, resp.content, resp.content_len);
        if (reasoning)
            reasoning_len = strlen(reasoning);
    }
    hu_chat_response_free(alloc, &resp);
    alloc->free(alloc->ctx, critique_prompt, prompt_cap);

    result->verdict = verdict;
    result->reasoning = reasoning;
    result->reasoning_len = reasoning_len;
    if (principle_idx >= 1 && (size_t)principle_idx <= config->principle_count)
        result->principle_index = principle_idx;
    else
        result->principle_index = -1;

    /* If REWRITE and rewrite_enabled, call provider again for revision */
    if (verdict == HU_CRITIQUE_REWRITE && config->rewrite_enabled && provider->vtable->chat) {
        const char *principle_desc = "the stated principles";
        if (principle_idx >= 1 && (size_t)principle_idx <= config->principle_count)
            principle_desc = config->principles[principle_idx - 1].description;

        size_t rewrite_cap = response_len + 256;
        char *rewrite_prompt = (char *)alloc->alloc(alloc->ctx, rewrite_cap);
        if (!rewrite_prompt) {
            /* OOM: keep REWRITE verdict so caller knows the response wasn't approved */
        } else {
            int n = snprintf(rewrite_prompt, rewrite_cap,
                             "Revise this response to satisfy the principle: %s\n\n"
                             "Original: %.*s\n\nRewrite:",
                             principle_desc, (int)response_len, response);
            size_t rewrite_len =
                (n > 0 && (size_t)n < rewrite_cap) ? (size_t)n : strlen(rewrite_prompt);

            hu_chat_message_t rw_msgs[2];
            memset(rw_msgs, 0, sizeof(rw_msgs));
            rw_msgs[0].role = HU_ROLE_SYSTEM;
            rw_msgs[0].content =
                "Revise the response to comply with the principle. Output only the revised text.";
            rw_msgs[0].content_len = strlen(rw_msgs[0].content);
            rw_msgs[1].role = HU_ROLE_USER;
            rw_msgs[1].content = rewrite_prompt;
            rw_msgs[1].content_len = rewrite_len;

            hu_chat_request_t rw_req;
            memset(&rw_req, 0, sizeof(rw_req));
            rw_req.messages = rw_msgs;
            rw_req.messages_count = 2;
            rw_req.model = model;
            rw_req.model_len = model_len;
            rw_req.temperature = 0.3;

            hu_chat_response_t rw_resp;
            memset(&rw_resp, 0, sizeof(rw_resp));
            err = provider->vtable->chat(provider->ctx, alloc, &rw_req, model, model_len, 0.3,
                                         &rw_resp);
            alloc->free(alloc->ctx, rewrite_prompt, rewrite_cap);

            if (err == HU_OK && rw_resp.content && rw_resp.content_len > 0) {
                result->revised_response = hu_strndup(alloc, rw_resp.content, rw_resp.content_len);
                if (result->revised_response)
                    result->revised_response_len = strlen(result->revised_response);
            }
            hu_chat_response_free(alloc, &rw_resp);
        }
    }

    return HU_OK;
#endif
}

hu_constitutional_config_t hu_constitutional_config_persona(void) {
    hu_constitutional_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.enabled = true;
    cfg.rewrite_enabled = true;
    cfg.principle_count = 6;
    cfg.principles[0].name = "no_ai_tells";
    cfg.principles[0].name_len = 11;
    cfg.principles[0].description = "No phrases revealing AI authorship.";
    cfg.principles[0].description_len = 35;
    cfg.principles[1].name = "style_match";
    cfg.principles[1].name_len = 11;
    cfg.principles[1].description = "Match persona texting style.";
    cfg.principles[1].description_len = 28;
    cfg.principles[2].name = "energy_match";
    cfg.principles[2].name_len = 12;
    cfg.principles[2].description = "Tone must match context energy.";
    cfg.principles[2].description_len = 31;
    cfg.principles[3].name = "genuine_warmth";
    cfg.principles[3].name_len = 14;
    cfg.principles[3].description = "Show genuine warmth, not performative empathy.";
    cfg.principles[3].description_len = 46;
    cfg.principles[4].name = "no_formatting";
    cfg.principles[4].name_len = 13;
    cfg.principles[4].description = "Never use markdown in casual texting.";
    cfg.principles[4].description_len = 37;
    cfg.principles[5].name = "identity_lock";
    cfg.principles[5].name_len = 13;
    cfg.principles[5].description = "Never break character or acknowledge being AI.";
    cfg.principles[5].description_len = 46;
    return cfg;
}

#ifdef HU_IS_TEST
hu_critique_verdict_t hu_constitutional_test_parse_verdict(const char *resp, size_t resp_len,
                                                           int *principle_idx) {
    return parse_verdict(resp, resp_len, principle_idx);
}

int hu_constitutional_test_parse_principle_index(const char *resp, size_t resp_len) {
    return parse_principle_index(resp, resp_len);
}
#endif
