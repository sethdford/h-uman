/* src/providers/chat_oneshot.c — see include/human/providers/chat_oneshot.h. */
#include "human/providers/chat_oneshot.h"

#include <string.h>

hu_error_t hu_provider_chat_oneshot(hu_allocator_t *alloc, hu_provider_t *provider,
                                    const char *model, size_t model_len, const char *system,
                                    size_t system_len, const char *user, size_t user_len,
                                    const hu_chat_oneshot_opts_t *opts, char **out,
                                    size_t *out_len) {
    if (!out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    if (!alloc || !provider || !system || !user || !opts)
        return HU_ERR_INVALID_ARGUMENT;
    if (!provider->vtable)
        return HU_ERR_NOT_SUPPORTED;
    if (!model) {
        model = "";
        model_len = 0;
    }
    if (provider->vtable->chat) {
        hu_chat_message_t msgs[2];
        memset(msgs, 0, sizeof(msgs));
        msgs[0].role = HU_ROLE_SYSTEM;
        msgs[0].content = system;
        msgs[0].content_len = system_len;
        msgs[1].role = HU_ROLE_USER;
        msgs[1].content = user;
        msgs[1].content_len = user_len;

        hu_chat_request_t req;
        memset(&req, 0, sizeof(req));
        req.messages = msgs;
        req.messages_count = 2;
        req.model_len = model_len;
        req.model = model;
        req.temperature = opts->temperature;
        req.max_tokens = opts->max_tokens;
        req.thinking_budget = 0; /* deterministic classifier — no thinking */
        if (opts->json_object) {
            req.response_format = "json_object";
            req.response_format_len = sizeof("json_object") - 1;
        }

        hu_chat_response_t resp;
        memset(&resp, 0, sizeof(resp));
        hu_error_t err = provider->vtable->chat(provider->ctx, alloc, &req, model, model_len,
                                                opts->temperature, &resp);
        if (err == HU_OK && resp.content && resp.content_len > 0) {
            char *buf = (char *)alloc->alloc(alloc->ctx, resp.content_len + 1);
            if (buf) {
                memcpy(buf, resp.content, resp.content_len);
                buf[resp.content_len] = '\0';
                *out = buf;
                *out_len = resp.content_len;
            } else {
                err = HU_ERR_OUT_OF_MEMORY;
            }
        }
        hu_chat_response_free(alloc, &resp);
        return err;
    }
    if (!provider->vtable->chat_with_system)
        return HU_ERR_NOT_SUPPORTED;
    return provider->vtable->chat_with_system(provider->ctx, alloc, system, system_len, user,
                                              user_len, model, model_len, opts->temperature, out,
                                              out_len);
}
