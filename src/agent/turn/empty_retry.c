/* src/agent/turn/empty_retry.c — retry a reply the serving layer discarded.
 * See include/human/agent/empty_retry.h for the contract and the gate. */
#include "human/agent/empty_retry.h"
#include <string.h>

hu_gate_mode_t hu_empty_retry_mode(void) {
    return hu_gate_mode_from_env("HU_EMPTY_REPLY_RETRY", HU_GATE_OFF);
}

bool hu_empty_retry_applies(hu_gate_mode_t mode, const hu_chat_response_t *resp,
                            bool already_retried) {
    if (mode == HU_GATE_OFF || already_retried || !resp)
        return false;
    if (resp->content && resp->content_len > 0)
        return false;
    if (resp->tool_calls_count > 0)
        return false; /* a tool call with no text is a normal turn step */
    /* Nothing generated (a dead or starved backend) is not a discarded draft;
     * re-asking the same backend would not help. */
    return resp->usage.completion_tokens > 0;
}

hu_error_t hu_empty_retry_chat(hu_provider_t *provider, hu_allocator_t *alloc,
                               const hu_chat_request_t *req, const char *model, size_t model_len,
                               double temperature, hu_chat_response_t *out) {
    if (!provider || !provider->vtable || !provider->vtable->chat || !alloc || !req || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    size_t n = req->messages_count;
    hu_chat_message_t *msgs =
        (hu_chat_message_t *)alloc->alloc(alloc->ctx, (n + 1) * sizeof(hu_chat_message_t));
    if (!msgs)
        return HU_ERR_OUT_OF_MEMORY;
    if (n > 0)
        memcpy(msgs, req->messages, n * sizeof(hu_chat_message_t));
    memset(&msgs[n], 0, sizeof(msgs[n]));
    msgs[n].role = HU_ROLE_SYSTEM;
    msgs[n].content = HU_EMPTY_RETRY_NUDGE;
    msgs[n].content_len = sizeof(HU_EMPTY_RETRY_NUDGE) - 1;

    hu_chat_request_t retry = *req;
    retry.messages = msgs;
    retry.messages_count = n + 1;
    hu_error_t err =
        provider->vtable->chat(provider->ctx, alloc, &retry, model, model_len, temperature, out);
    alloc->free(alloc->ctx, msgs, (n + 1) * sizeof(hu_chat_message_t));
    return err;
}
