#include "seaclaw/agent.h"
#include <stdint.h>
#include "seaclaw/core/string.h"
#include "seaclaw/core/json.h"
#include <string.h>
#include <stdlib.h>

#define SC_CONTEXT_DEFAULT_SYSTEM "You are a helpful AI assistant."

/* Build system prompt. Caller owns returned string. */
char *sc_context_build_system_prompt(sc_allocator_t *alloc,
    const char *base, size_t base_len,
    const char *workspace_dir, size_t workspace_dir_len)
{
    (void)workspace_dir;
    (void)workspace_dir_len;
    if (!base || base_len == 0) {
        return sc_strndup(alloc, SC_CONTEXT_DEFAULT_SYSTEM,
            strlen(SC_CONTEXT_DEFAULT_SYSTEM));
    }
    return sc_strndup(alloc, base, base_len);
}

/* Format history messages for provider request. Allocates messages array.
 * Caller must free each message's content and the array. */
sc_error_t sc_context_format_messages(sc_allocator_t *alloc,
    const sc_owned_message_t *history, size_t history_count,
    size_t max_messages,
    sc_chat_message_t **out_messages, size_t *out_count)
{
    if (!history || history_count == 0) {
        *out_messages = NULL;
        *out_count = 0;
        return SC_OK;
    }
    size_t n = history_count;
    if (max_messages > 0 && n > max_messages) {
        n = max_messages;
    }
    sc_chat_message_t *msgs = (sc_chat_message_t *)alloc->alloc(alloc->ctx,
        n * sizeof(sc_chat_message_t));
    if (!msgs) return SC_ERR_OUT_OF_MEMORY;
    size_t start = history_count - n;
    for (size_t i = 0; i < n; i++) {
        const sc_owned_message_t *src = &history[start + i];
        msgs[i].role = src->role;
        msgs[i].content = src->content;
        msgs[i].content_len = src->content_len;
        msgs[i].name = src->name;
        msgs[i].name_len = src->name_len;
        msgs[i].tool_call_id = src->tool_call_id;
        msgs[i].tool_call_id_len = src->tool_call_id_len;
        msgs[i].content_parts = NULL;
        msgs[i].content_parts_count = 0;
        msgs[i].tool_calls = src->tool_calls;
        msgs[i].tool_calls_count = src->tool_calls_count;
    }
    *out_messages = msgs;
    *out_count = n;
    return SC_OK;
}

/* Estimate context window size in tokens (rough). */
uint32_t sc_context_estimate_tokens(const sc_chat_message_t *messages,
    size_t messages_count)
{
    uint32_t total = 0;
    for (size_t i = 0; i < messages_count; i++) {
        total += (uint32_t)((messages[i].content_len + 3) / 4);
        total += 4; /* overhead per message */
    }
    return total;
}
