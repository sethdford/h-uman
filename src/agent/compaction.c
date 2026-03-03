#include "seaclaw/agent/compaction.h"
#include "seaclaw/core/string.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *role_str(sc_role_t role) {
    switch (role) {
    case SC_ROLE_SYSTEM:
        return "SYSTEM";
    case SC_ROLE_USER:
        return "USER";
    case SC_ROLE_ASSISTANT:
        return "ASSISTANT";
    case SC_ROLE_TOOL:
        return "TOOL";
    default:
        return "UNKNOWN";
    }
}

void sc_compaction_config_default(sc_compaction_config_t *cfg) {
    if (!cfg)
        return;
    cfg->keep_recent = SC_COMPACTION_DEFAULT_KEEP_RECENT;
    cfg->max_summary_chars = SC_COMPACTION_DEFAULT_MAX_SUMMARY_CHARS;
    cfg->max_source_chars = SC_COMPACTION_DEFAULT_MAX_SOURCE_CHARS;
    cfg->token_limit = SC_COMPACTION_DEFAULT_TOKEN_LIMIT;
    cfg->max_history_messages = SC_COMPACTION_DEFAULT_MAX_HISTORY;
}

uint64_t sc_estimate_tokens(const sc_owned_message_t *history, size_t history_count) {
    uint64_t total_chars = 0;
    for (size_t i = 0; i < history_count; i++) {
        total_chars += (uint64_t)history[i].content_len;
    }
    return (total_chars + 3) / 4;
}

bool sc_should_compact(const sc_owned_message_t *history, size_t history_count,
                       const sc_compaction_config_t *config) {
    if (!history || !config)
        return false;

    bool has_system = history_count > 0 && history[0].role == SC_ROLE_SYSTEM;
    size_t start = has_system ? 1 : 0;
    size_t non_system_count = history_count - start;

    if (non_system_count <= config->keep_recent)
        return false;

    /* Message count trigger */
    if (non_system_count > config->max_history_messages)
        return true;

    /* Token trigger: 75% of limit */
    if (config->token_limit > 0) {
        uint64_t tokens = sc_estimate_tokens(history, history_count);
        uint64_t threshold = (config->token_limit * 3) / 4;
        if (tokens > threshold)
            return true;
    }

    return false;
}

/* Build summary from messages [start, end) as concatenation of "ROLE: content\n".
 * Truncates each message to 500 chars and total to max_source_chars. */
static char *build_summary(sc_allocator_t *alloc, const sc_owned_message_t *history, size_t start,
                           size_t end, uint32_t max_source_chars, size_t *alloc_size_out) {
    size_t total = 0;
    for (size_t i = start; i < end && total < max_source_chars; i++) {
        total += strlen(role_str(history[i].role)) + 2;
        size_t content_len = history[i].content_len;
        if (content_len > 500)
            content_len = 500;
        total += content_len + 1;
    }
    if (total > max_source_chars)
        total = max_source_chars;

    char *buf = (char *)alloc->alloc(alloc->ctx, total + 1);
    if (!buf)
        return NULL;
    buf[0] = '\0';
    size_t written = 0;

    for (size_t i = start; i < end && written < max_source_chars; i++) {
        const char *role = role_str(history[i].role);
        size_t rlen = strlen(role);
        if (written + rlen + 2 >= total)
            break;
        memcpy(buf + written, role, rlen);
        written += rlen;
        buf[written++] = ':';
        buf[written++] = ' ';

        size_t content_len = history[i].content_len;
        if (content_len > 500)
            content_len = 500;
        size_t remaining = total - written;
        if (remaining < content_len + 1)
            content_len = remaining > 0 ? remaining - 1 : 0;

        if (content_len > 0 && history[i].content) {
            memcpy(buf + written, history[i].content, content_len);
            written += content_len;
        }
        if (written < total)
            buf[written++] = '\n';
    }
    buf[written] = '\0';
    *alloc_size_out = total + 1;
    return buf;
}

/* Free messages in range [start, end) */
static void free_messages(sc_allocator_t *alloc, sc_owned_message_t *history, size_t start,
                          size_t end) {
    for (size_t i = start; i < end; i++) {
        if (history[i].content) {
            alloc->free(alloc->ctx, history[i].content, history[i].content_len + 1);
            history[i].content = NULL;
        }
        if (history[i].name) {
            alloc->free(alloc->ctx, history[i].name, history[i].name_len + 1);
            history[i].name = NULL;
        }
        if (history[i].tool_call_id) {
            alloc->free(alloc->ctx, history[i].tool_call_id, history[i].tool_call_id_len + 1);
            history[i].tool_call_id = NULL;
        }
    }
}

sc_error_t sc_compact_history(sc_allocator_t *alloc, sc_owned_message_t *history,
                              size_t *history_count, size_t *history_cap,
                              const sc_compaction_config_t *config) {
    if (!alloc || !history || !history_count || !history_cap || !config)
        return SC_ERR_INVALID_ARGUMENT;
    size_t count = *history_count;

    (void)history_cap;
    if (!history || count == 0)
        return SC_OK;
    if (!sc_should_compact(history, count, config))
        return SC_OK;

    bool has_system = history[0].role == SC_ROLE_SYSTEM;
    size_t start = has_system ? 1 : 0;
    size_t non_system = count - start;

    uint32_t keep = config->keep_recent;
    if (keep > (uint32_t)non_system)
        keep = (uint32_t)non_system;

    size_t compact_count = non_system - keep;
    if (compact_count == 0)
        return SC_OK;

    size_t compact_end = start + compact_count;

    /* Build summary from messages [start, compact_end) */
    uint32_t max_src = config->max_source_chars;
    if (max_src == 0)
        max_src = SC_COMPACTION_DEFAULT_MAX_SOURCE_CHARS;

    size_t summary_raw_alloc = 0;
    char *summary_raw =
        build_summary(alloc, history, start, compact_end, max_src, &summary_raw_alloc);
    if (!summary_raw)
        return SC_ERR_OUT_OF_MEMORY;

    size_t sum_len = strlen(summary_raw);
    uint32_t max_sum = config->max_summary_chars;
    if (max_sum == 0)
        max_sum = SC_COMPACTION_DEFAULT_MAX_SUMMARY_CHARS;
    if (sum_len > max_sum) {
        summary_raw[max_sum] = '\0';
        sum_len = max_sum;
    }

    size_t prefix_len = strlen("[Compaction summary]\n");
    char *summary_content = (char *)alloc->alloc(alloc->ctx, prefix_len + sum_len + 1);
    if (!summary_content) {
        alloc->free(alloc->ctx, summary_raw, summary_raw_alloc);
        return SC_ERR_OUT_OF_MEMORY;
    }
    memcpy(summary_content, "[Compaction summary]\n", prefix_len);
    memcpy(summary_content + prefix_len, summary_raw, sum_len + 1);
    alloc->free(alloc->ctx, summary_raw, summary_raw_alloc);

    /* Free compacted messages */
    free_messages(alloc, history, start, compact_end);

    /* Replace first compacted slot with summary (assistant role for consistency) */
    history[start].role = SC_ROLE_ASSISTANT;
    history[start].content = summary_content;
    history[start].content_len = prefix_len + sum_len;
    history[start].name = NULL;
    history[start].name_len = 0;
    history[start].tool_call_id = NULL;
    history[start].tool_call_id_len = 0;

    /* Shift remaining messages down */
    if (compact_end > start + 1) {
        size_t shift = compact_end - start - 1;
        size_t remaining = count - compact_end;
        if (remaining > 0) {
            memmove(&history[start + 1], &history[compact_end],
                    remaining * sizeof(sc_owned_message_t));
        }
        count -= shift;
    }

    *history_count = count;
    return SC_OK;
}
