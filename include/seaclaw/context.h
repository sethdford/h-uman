#ifndef SC_CONTEXT_H
#define SC_CONTEXT_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/provider.h"
#include "seaclaw/agent.h"
#include <stddef.h>
#include <stdint.h>

char *sc_context_build_system_prompt(sc_allocator_t *alloc,
    const char *base, size_t base_len,
    const char *workspace_dir, size_t workspace_dir_len);

sc_error_t sc_context_format_messages(sc_allocator_t *alloc,
    const sc_owned_message_t *history, size_t history_count,
    size_t max_messages,
    sc_chat_message_t **out_messages, size_t *out_count);

uint32_t sc_context_estimate_tokens(const sc_chat_message_t *messages,
    size_t messages_count);

#endif /* SC_CONTEXT_H */
