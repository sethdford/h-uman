#include "human/agent/response_guard_retry.h"
#include "human/agent/tool_call_parser.h"
#include "human/config.h"
#include "human/core/log.h"
#include "human/core/string.h"
#include "human/providers/factory.h"

#include <string.h>

#define HU_GUARD_RETRY_USER_MAX    4096
#define HU_GUARD_RETRY_MODEL_CLOUD "gemini-3.1-flash-lite-preview"

/* Match agent_turn last-mile: guard does not strip `<tool_call>` XML leaks. */
static hu_error_t response_guard_retry_strip_text_tool_calls(hu_allocator_t *alloc, char **out,
                                                             size_t *out_len) {
    if (!alloc || !out || !*out || !out_len || *out_len == 0)
        return HU_OK;
    char *cur = *out;
    size_t cur_len = *out_len;
    char *stripped = NULL;
    size_t stripped_len = 0;
    hu_error_t e = hu_text_tool_calls_strip(alloc, cur, cur_len, &stripped, &stripped_len);
    if (e != HU_OK) {
        alloc->free(alloc->ctx, cur, cur_len + 1);
        *out = NULL;
        *out_len = 0;
        return e;
    }
    if (!stripped) {
        alloc->free(alloc->ctx, cur, cur_len + 1);
        *out = NULL;
        *out_len = 0;
        return HU_OK;
    }
    alloc->free(alloc->ctx, cur, cur_len + 1);
    *out = stripped;
    *out_len = stripped_len;
    return HU_OK;
}

static hu_error_t dispatch_slim_chat(hu_allocator_t *alloc, hu_observer_t *obs, hu_provider_t *prov,
                                     const char *model, size_t model_len, const char *user_msg,
                                     size_t user_len, char **out, size_t *out_len,
                                     hu_guard_report_t *guard_report) {
    static const char repair_instruction[] =
        "Your previous draft was invalid (internal model tokens or runaway repetition). "
        "Reply ONLY with the final human-visible text for the user's latest message below. "
        "Short, natural, channel-appropriate. No analysis, XML, markdown fences, or filler. "
        "Do not think out loud or narrate your reasoning — no 'Let me think' or 'Step 1:' prose. "
        "Do not refer to yourself in third-person by name (e.g. 'As Aria, I...'). "
        "Do not end with AI-helper closers (e.g. 'Is there anything else I can help with?').";

    if (!alloc || !prov || !prov->vtable || !prov->vtable->chat || !out || !out_len || !user_msg)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    size_t ulen = user_len;
    const char *u = user_msg;
    if (ulen > HU_GUARD_RETRY_USER_MAX)
        ulen = HU_GUARD_RETRY_USER_MAX;

    hu_chat_message_t msgs[2];
    memset(msgs, 0, sizeof(msgs));
    msgs[0].role = HU_ROLE_SYSTEM;
    msgs[0].content = repair_instruction;
    msgs[0].content_len = sizeof(repair_instruction) - 1;
    msgs[1].role = HU_ROLE_USER;
    msgs[1].content = u;
    msgs[1].content_len = ulen;

    hu_chat_request_t req;
    memset(&req, 0, sizeof(req));
    req.messages = msgs;
    req.messages_count = 2;
    req.temperature = 0.2;
    req.max_tokens = 128;
    req.model = model;
    req.model_len = model_len;
    req.reasoning_effort = NULL;
    req.reasoning_effort_len = 0;
    req.thinking_budget = 0;
    req.include_completion_logprobs = false;

    hu_chat_response_t resp;
    memset(&resp, 0, sizeof(resp));
    hu_error_t err = prov->vtable->chat(prov->ctx, alloc, &req, model, model_len, 0.2, &resp);
    if (err != HU_OK) {
        if (obs)
            hu_log_warn("response_guard_retry", obs, "slim retry chat failed on provider (err=%s)",
                        hu_error_string(err));
        hu_chat_response_free(alloc, &resp);
        return err;
    }

    char *guard_out = NULL;
    size_t guard_out_len = 0;
    hu_guard_outcome_t guard_outcome = HU_GUARD_OK;
    hu_guard_report_t local_report;
    memset(&local_report, 0, sizeof(local_report));
    err = hu_response_guard_check(alloc, resp.content ? resp.content : "", resp.content_len,
                                  &guard_out, &guard_out_len, &guard_outcome,
                                  guard_report ? guard_report : &local_report);
    if (err != HU_OK) {
        hu_chat_response_free(alloc, &resp);
        return err;
    }
    if (guard_outcome == HU_GUARD_REJECT) {
        hu_chat_response_free(alloc, &resp);
        return HU_ERR_PROVIDER_RESPONSE;
    }
    if (guard_outcome == HU_GUARD_REWROTE) {
        *out = guard_out;
        *out_len = guard_out_len;
        hu_chat_response_free(alloc, &resp);
    } else {
        *out = hu_strndup(alloc, resp.content ? resp.content : "", resp.content_len);
        *out_len = *out ? resp.content_len : 0;
        hu_chat_response_free(alloc, &resp);
        if (!*out)
            return HU_ERR_OUT_OF_MEMORY;
    }
    return response_guard_retry_strip_text_tool_calls(alloc, out, out_len);
}

hu_error_t hu_response_guard_retry_slim(hu_allocator_t *alloc, hu_observer_t *obs,
                                        const hu_config_t *cfg, hu_provider_t *primary,
                                        const char *model, size_t model_len, const char *user_msg,
                                        size_t user_msg_len, char **out, size_t *out_len,
                                        hu_guard_report_t *guard_report) {
    if (!alloc || !primary || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    hu_error_t err = dispatch_slim_chat(alloc, obs, primary, model, model_len, user_msg,
                                        user_msg_len, out, out_len, guard_report);
    if (err == HU_OK && *out && *out_len > 0)
        return HU_OK;

#ifndef HU_ENABLE_CURL
    (void)cfg;
#else
    if (!cfg)
        return err;

    static const struct {
        const char *name;
        size_t len;
    } k_fallback[] = {{"gemini", 6}, {"openai", 6}};

    const char *cloud_model = HU_GUARD_RETRY_MODEL_CLOUD;
    size_t cloud_model_len = strlen(cloud_model);

    for (size_t i = 0; i < sizeof(k_fallback) / sizeof(k_fallback[0]); i++) {
        hu_provider_t fb = {0};
        hu_error_t oe =
            hu_provider_create_from_config(alloc, cfg, k_fallback[i].name, k_fallback[i].len, &fb);
        if (oe != HU_OK || !fb.vtable || !fb.vtable->chat) {
            if (fb.vtable && fb.vtable->deinit)
                fb.vtable->deinit(fb.ctx, alloc);
            memset(&fb, 0, sizeof(fb));
            continue;
        }
        if (obs)
            hu_log_warn("response_guard_retry", obs,
                        "slim retry: falling back to cloud provider '%.*s' after primary err=%s",
                        (int)k_fallback[i].len, k_fallback[i].name, hu_error_string(err));
        hu_error_t e2 = dispatch_slim_chat(alloc, obs, &fb, cloud_model, cloud_model_len, user_msg,
                                           user_msg_len, out, out_len, guard_report);
        if (fb.vtable && fb.vtable->deinit)
            fb.vtable->deinit(fb.ctx, alloc);
        if (e2 == HU_OK && *out && *out_len > 0)
            return HU_OK;
        err = e2;
    }
#endif
    return err;
}
