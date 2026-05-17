#include "human/context.h"
#include "human/core/json.h"
#include "human/core/log.h"
#include "human/core/string.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Persona-first doctrine (2026-05-17): the silent fallback used when no base
 * prompt is supplied must not announce the model as an AI assistant. That
 * framing leaks through to user-visible turns whenever a caller forgets to
 * thread persona context, and it directly contradicts the digital-twin thesis.
 * Stay persona-neutral; the configured persona (when present) overrides this. */
#define HU_CONTEXT_DEFAULT_SYSTEM                                           \
    "Respond naturally and personally. Match the energy and length of the " \
    "message you're replying to. Do not announce yourself as an assistant or AI."

/* Build system prompt. Caller owns returned string. */
char *hu_context_build_system_prompt(hu_allocator_t *alloc, const char *base, size_t base_len,
                                     const char *workspace_dir, size_t workspace_dir_len) {
    (void)workspace_dir;
    (void)workspace_dir_len;
    if (!base || base_len == 0) {
        return hu_strndup(alloc, HU_CONTEXT_DEFAULT_SYSTEM, strlen(HU_CONTEXT_DEFAULT_SYSTEM));
    }
    return hu_strndup(alloc, base, base_len);
}

/* Format history messages for provider request. Allocates messages array.
 * Message content/tool_call pointers are BORROWED from history — do NOT free them.
 * Caller must free only the returned array itself (not individual message contents). */
hu_error_t hu_context_format_messages(hu_allocator_t *alloc, const hu_owned_message_t *history,
                                      size_t history_count, size_t max_messages,
                                      const bool *include_mask, hu_chat_message_t **out_messages,
                                      size_t *out_count) {
    if (!history || history_count == 0) {
        *out_messages = NULL;
        *out_count = 0;
        return HU_OK;
    }

    size_t *pick = (size_t *)alloc->alloc(alloc->ctx, history_count * sizeof(size_t));
    if (!pick)
        return HU_ERR_OUT_OF_MEMORY;
    size_t picked = 0;
    for (size_t i = history_count; i > 0; i--) {
        size_t idx = i - 1;
        if (include_mask && !include_mask[idx])
            continue;
        pick[picked++] = idx;
        if (max_messages > 0 && picked >= max_messages)
            break;
    }

    /* restore chronological order */
    if (picked > 1) {
        for (size_t a = 0, b = picked - 1; a < b; a++, b--) {
            size_t t = pick[a];
            pick[a] = pick[b];
            pick[b] = t;
        }
    }

    if (picked == 0) {
        alloc->free(alloc->ctx, pick, history_count * sizeof(size_t));
        *out_messages = NULL;
        *out_count = 0;
        return HU_OK;
    }

    hu_chat_message_t *msgs =
        (hu_chat_message_t *)alloc->alloc(alloc->ctx, picked * sizeof(hu_chat_message_t));
    if (!msgs) {
        alloc->free(alloc->ctx, pick, history_count * sizeof(size_t));
        return HU_ERR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < picked; i++) {
        const hu_owned_message_t *src = &history[pick[i]];
        msgs[i].role = src->role;
        msgs[i].content = src->content;
        msgs[i].content_len = src->content_len;
        msgs[i].name = src->name;
        msgs[i].name_len = src->name_len;
        msgs[i].tool_call_id = src->tool_call_id;
        msgs[i].tool_call_id_len = src->tool_call_id_len;
        msgs[i].content_parts = src->content_parts;
        msgs[i].content_parts_count = src->content_parts_count;
        msgs[i].tool_calls = src->tool_calls;
        msgs[i].tool_calls_count = src->tool_calls_count;
    }
    alloc->free(alloc->ctx, pick, history_count * sizeof(size_t));
    *out_messages = msgs;
    *out_count = picked;
    return HU_OK;
}

/* Estimate context window size in tokens (rough). */
uint32_t hu_context_estimate_tokens(const hu_chat_message_t *messages, size_t messages_count) {
    uint32_t total = 0;
    for (size_t i = 0; i < messages_count; i++) {
        total += (uint32_t)((messages[i].content_len + 3) / 4);
        total += 4; /* overhead per message */
    }
    return total;
}

/* Estimate tokens for a single text string (rough: ~4 chars per token for English). */
size_t hu_estimate_tokens_text(const char *text, size_t len) {
    if (!text)
        return 0;
    return (len + 3) / 4;
}

bool hu_context_check_pressure(hu_context_pressure_t *p, float pressure_warn,
                               float pressure_compact) {
    if (!p || p->max_tokens == 0)
        return false;
    p->pressure = (float)((double)p->current_tokens / (double)p->max_tokens);
    if (p->pressure > 1.0f)
        p->pressure = 1.0f;

    if (p->pressure >= pressure_warn && !p->warning_85_emitted) {
        p->warning_85_emitted = true;
#ifndef HU_IS_TEST
        hu_log_info("agent", NULL, "Context pressure at %.0f%% — consider compacting",
                    p->pressure * 100.0f);
#endif
    }
    if (p->pressure >= pressure_compact && !p->warning_95_emitted) {
        p->warning_95_emitted = true;
#ifndef HU_IS_TEST
        hu_log_info("agent", NULL, "Context pressure at %.0f%% — auto-compacting oldest messages",
                    p->pressure * 100.0f);
#endif
        return true;
    }
    return false;
}
