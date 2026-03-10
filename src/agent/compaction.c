#include "human/agent/compaction.h"
#include "human/core/string.h"
#include "human/provider.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *role_str(hu_role_t role) {
    switch (role) {
    case HU_ROLE_SYSTEM:
        return "SYSTEM";
    case HU_ROLE_USER:
        return "USER";
    case HU_ROLE_ASSISTANT:
        return "ASSISTANT";
    case HU_ROLE_TOOL:
        return "TOOL";
    default:
        return "UNKNOWN";
    }
}

void hu_compaction_config_default(hu_compaction_config_t *cfg) {
    if (!cfg)
        return;
    cfg->keep_recent = HU_COMPACTION_DEFAULT_KEEP_RECENT;
    cfg->max_summary_chars = HU_COMPACTION_DEFAULT_MAX_SUMMARY_CHARS;
    cfg->max_source_chars = HU_COMPACTION_DEFAULT_MAX_SOURCE_CHARS;
    cfg->token_limit = HU_COMPACTION_DEFAULT_TOKEN_LIMIT;
    cfg->max_history_messages = HU_COMPACTION_DEFAULT_MAX_HISTORY;
}

/*
 * Approximate token costs for non-text content parts.
 * OpenAI: images ~85 tokens (low) to ~765 tokens (high detail, per tile).
 * Audio/video: ~25 tokens per second (~1500 bytes of base64 per second).
 * We use conservative middle-ground estimates.
 */
#define HU_IMAGE_TOKEN_ESTIMATE 170
#define HU_AUDIO_TOKENS_PER_KB  17
#define HU_VIDEO_TOKEN_ESTIMATE 256

static uint64_t estimate_content_parts_tokens(const hu_content_part_t *parts, size_t count) {
    uint64_t tokens = 0;
    for (size_t p = 0; p < count; p++) {
        switch (parts[p].tag) {
        case HU_CONTENT_PART_TEXT:
            tokens += (uint64_t)parts[p].data.text.len / 4;
            break;
        case HU_CONTENT_PART_IMAGE_URL:
        case HU_CONTENT_PART_IMAGE_BASE64:
            tokens += HU_IMAGE_TOKEN_ESTIMATE;
            break;
        case HU_CONTENT_PART_AUDIO_BASE64:
            tokens +=
                ((uint64_t)parts[p].data.audio_base64.data_len / 1024) * HU_AUDIO_TOKENS_PER_KB;
            break;
        case HU_CONTENT_PART_VIDEO_URL:
            tokens += HU_VIDEO_TOKEN_ESTIMATE;
            break;
        default:
            break;
        }
    }
    return tokens;
}

uint64_t hu_estimate_tokens(const hu_owned_message_t *history, size_t history_count) {
    uint64_t total_chars = 0;
    uint64_t multimodal_tokens = 0;
    for (size_t i = 0; i < history_count; i++) {
        const hu_owned_message_t *m = &history[i];
        if (m->content_parts_count > 0 && m->content_parts) {
            multimodal_tokens +=
                estimate_content_parts_tokens(m->content_parts, m->content_parts_count);
        } else {
            total_chars += (uint64_t)m->content_len;
        }
    }
    return (total_chars + 3 * (uint64_t)history_count) / 4 + multimodal_tokens;
}

bool hu_should_compact(const hu_owned_message_t *history, size_t history_count,
                       const hu_compaction_config_t *config) {
    if (!history || !config)
        return false;

    bool has_system = history_count > 0 && history[0].role == HU_ROLE_SYSTEM;
    size_t start = has_system ? 1 : 0;
    size_t non_system_count = history_count - start;

    if (non_system_count <= config->keep_recent)
        return false;

    /* Message count trigger */
    if (non_system_count > config->max_history_messages)
        return true;

    /* Token trigger: 75% of limit */
    if (config->token_limit > 0) {
        uint64_t tokens = hu_estimate_tokens(history, history_count);
        uint64_t threshold = (config->token_limit * 3) / 4;
        if (tokens > threshold)
            return true;
    }

    return false;
}

/* Build summary from messages [start, end) as concatenation of "ROLE: content\n".
 * Truncates each message to 500 chars and total to max_source_chars. */
static char *build_summary(hu_allocator_t *alloc, const hu_owned_message_t *history, size_t start,
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

static void free_tool_calls(hu_allocator_t *alloc, hu_tool_call_t *tcs, size_t count) {
    if (!alloc || !tcs || count == 0)
        return;
    for (size_t i = 0; i < count; i++) {
        if (tcs[i].id && tcs[i].id_len > 0)
            alloc->free(alloc->ctx, (void *)tcs[i].id, tcs[i].id_len + 1);
        if (tcs[i].name && tcs[i].name_len > 0)
            alloc->free(alloc->ctx, (void *)tcs[i].name, tcs[i].name_len + 1);
        if (tcs[i].arguments && tcs[i].arguments_len > 0)
            alloc->free(alloc->ctx, (void *)tcs[i].arguments, tcs[i].arguments_len + 1);
    }
    alloc->free(alloc->ctx, tcs, count * sizeof(hu_tool_call_t));
}

/* Free messages in range [start, end) */
static void free_messages(hu_allocator_t *alloc, hu_owned_message_t *history, size_t start,
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
        if (history[i].tool_calls) {
            free_tool_calls(alloc, history[i].tool_calls, history[i].tool_calls_count);
            history[i].tool_calls = NULL;
            history[i].tool_calls_count = 0;
        }
        if (history[i].content_parts) {
            for (size_t j = 0; j < history[i].content_parts_count; j++) {
                hu_content_part_t *cp = &history[i].content_parts[j];
                if (cp->tag == HU_CONTENT_PART_TEXT && cp->data.text.ptr) {
                    alloc->free(alloc->ctx, (void *)cp->data.text.ptr, cp->data.text.len + 1);
                } else if (cp->tag == HU_CONTENT_PART_IMAGE_URL && cp->data.image_url.url) {
                    alloc->free(alloc->ctx, (void *)cp->data.image_url.url,
                                cp->data.image_url.url_len + 1);
                } else if (cp->tag == HU_CONTENT_PART_IMAGE_BASE64) {
                    if (cp->data.image_base64.data)
                        alloc->free(alloc->ctx, (void *)cp->data.image_base64.data,
                                    cp->data.image_base64.data_len + 1);
                    if (cp->data.image_base64.media_type)
                        alloc->free(alloc->ctx, (void *)cp->data.image_base64.media_type,
                                    cp->data.image_base64.media_type_len + 1);
                } else if (cp->tag == HU_CONTENT_PART_AUDIO_BASE64) {
                    if (cp->data.audio_base64.data)
                        alloc->free(alloc->ctx, (void *)cp->data.audio_base64.data,
                                    cp->data.audio_base64.data_len + 1);
                    if (cp->data.audio_base64.media_type)
                        alloc->free(alloc->ctx, (void *)cp->data.audio_base64.media_type,
                                    cp->data.audio_base64.media_type_len + 1);
                } else if (cp->tag == HU_CONTENT_PART_VIDEO_URL) {
                    if (cp->data.video_url.url)
                        alloc->free(alloc->ctx, (void *)cp->data.video_url.url,
                                    cp->data.video_url.url_len + 1);
                    if (cp->data.video_url.media_type)
                        alloc->free(alloc->ctx, (void *)cp->data.video_url.media_type,
                                    cp->data.video_url.media_type_len + 1);
                }
            }
            alloc->free(alloc->ctx, history[i].content_parts,
                        history[i].content_parts_count * sizeof(hu_content_part_t));
            history[i].content_parts = NULL;
            history[i].content_parts_count = 0;
        }
    }
}

hu_error_t hu_compact_history(hu_allocator_t *alloc, hu_owned_message_t *history,
                              size_t *history_count, size_t *history_cap,
                              const hu_compaction_config_t *config) {
    if (!alloc || !history || !history_count || !history_cap || !config)
        return HU_ERR_INVALID_ARGUMENT;
    size_t count = *history_count;

    (void)history_cap;
    if (!history || count == 0)
        return HU_OK;
    if (!hu_should_compact(history, count, config))
        return HU_OK;

    bool has_system = history[0].role == HU_ROLE_SYSTEM;
    size_t start = has_system ? 1 : 0;
    size_t non_system = count - start;

    uint32_t keep = config->keep_recent;
    if (keep > (uint32_t)non_system)
        keep = (uint32_t)non_system;

    size_t compact_count = non_system - keep;
    if (compact_count == 0)
        return HU_OK;

    size_t compact_end = start + compact_count;

    /* Build summary from messages [start, compact_end) */
    uint32_t max_src = config->max_source_chars;
    if (max_src == 0)
        max_src = HU_COMPACTION_DEFAULT_MAX_SOURCE_CHARS;

    size_t summary_raw_alloc = 0;
    char *summary_raw =
        build_summary(alloc, history, start, compact_end, max_src, &summary_raw_alloc);
    if (!summary_raw)
        return HU_ERR_OUT_OF_MEMORY;

    size_t sum_len = strlen(summary_raw);
    uint32_t max_sum = config->max_summary_chars;
    if (max_sum == 0)
        max_sum = HU_COMPACTION_DEFAULT_MAX_SUMMARY_CHARS;
    if (sum_len > max_sum) {
        summary_raw[max_sum] = '\0';
        sum_len = max_sum;
    }

    size_t prefix_len = strlen("[Compaction summary]\n");
    char *summary_content = (char *)alloc->alloc(alloc->ctx, prefix_len + sum_len + 1);
    if (!summary_content) {
        alloc->free(alloc->ctx, summary_raw, summary_raw_alloc);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(summary_content, "[Compaction summary]\n", prefix_len);
    memcpy(summary_content + prefix_len, summary_raw, sum_len + 1);
    alloc->free(alloc->ctx, summary_raw, summary_raw_alloc);

    /* Free compacted messages */
    free_messages(alloc, history, start, compact_end);

    /* Replace first compacted slot with summary (assistant role for consistency) */
    history[start].role = HU_ROLE_ASSISTANT;
    history[start].content = summary_content;
    history[start].content_len = prefix_len + sum_len;
    history[start].name = NULL;
    history[start].name_len = 0;
    history[start].tool_call_id = NULL;
    history[start].tool_call_id_len = 0;
    history[start].tool_calls = NULL;
    history[start].tool_calls_count = 0;

    /* Shift remaining messages down */
    if (compact_end > start + 1) {
        size_t shift = compact_end - start - 1;
        size_t remaining = count - compact_end;
        if (remaining > 0) {
            memmove(&history[start + 1], &history[compact_end],
                    remaining * sizeof(hu_owned_message_t));
        }
        count -= shift;
    }

    *history_count = count;
    return HU_OK;
}

hu_error_t hu_context_compact_for_pressure(hu_allocator_t *alloc, hu_owned_message_t *history,
                                           size_t *history_count, size_t *history_cap,
                                           size_t max_tokens, float target_pressure) {
    (void)history_cap;
    if (!alloc || !history || !history_count || max_tokens == 0)
        return HU_ERR_INVALID_ARGUMENT;
    size_t count = *history_count;
    if (count == 0)
        return HU_OK;

    uint64_t current = hu_estimate_tokens(history, count);
    float pressure = (float)((double)current / (double)max_tokens);
    if (pressure < target_pressure)
        return HU_OK;

    bool has_system = history[0].role == HU_ROLE_SYSTEM;
    size_t start = has_system ? 1 : 0;
    size_t non_system = count - start;
    if (non_system <= 1)
        return HU_OK;

    uint64_t target_tokens = (uint64_t)((double)max_tokens * (double)target_pressure);
    size_t compact_count = 0;
    for (size_t k = 1; k < non_system; k++) {
        uint64_t new_tokens = 0;
        for (size_t i = 0; i < start; i++)
            new_tokens += (uint64_t)((history[i].content_len + 3) / 4);
        for (size_t i = start + k; i < count; i++)
            new_tokens += (uint64_t)((history[i].content_len + 3) / 4);
        /* Marker + overhead */
        new_tokens += 20; /* "[Previous context compacted: N messages summarized]" ~20 tokens */
        if (new_tokens < target_tokens) {
            compact_count = k;
            break;
        }
        compact_count = k;
    }
    if (compact_count == 0)
        return HU_OK;

    size_t compact_end = start + compact_count;
    char buf[96];
    int n = snprintf(buf, sizeof(buf), "[Previous context compacted: %zu messages summarized]",
                     compact_count);
    if (n < 0 || (size_t)n >= sizeof(buf))
        return HU_ERR_INVALID_ARGUMENT;
    size_t marker_len = (size_t)n;

    char *summary_content = (char *)alloc->alloc(alloc->ctx, marker_len + 1);
    if (!summary_content)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(summary_content, buf, marker_len + 1);

    /* Free compacted messages */
    free_messages(alloc, history, start, compact_end);

    history[start].role = HU_ROLE_ASSISTANT;
    history[start].content = summary_content;
    history[start].content_len = marker_len;
    history[start].name = NULL;
    history[start].name_len = 0;
    history[start].tool_call_id = NULL;
    history[start].tool_call_id_len = 0;
    history[start].tool_calls = NULL;
    history[start].tool_calls_count = 0;

    if (compact_end > start + 1) {
        size_t remaining = count - compact_end;
        if (remaining > 0) {
            memmove(&history[start + 1], &history[compact_end],
                    remaining * sizeof(hu_owned_message_t));
        }
        count -= (compact_end - start - 1);
    }

    *history_count = count;
    return HU_OK;
}

hu_error_t hu_compact_history_llm(hu_allocator_t *alloc, hu_owned_message_t *history,
                                  size_t *history_count, size_t *history_cap,
                                  const hu_compaction_config_t *config, hu_provider_t *provider) {
    if (!alloc || !history || !history_count || !history_cap || !config)
        return HU_ERR_INVALID_ARGUMENT;

    if (!provider || !provider->vtable || !provider->vtable->chat_with_system)
        return hu_compact_history(alloc, history, history_count, history_cap, config);

    size_t count = *history_count;
    if (!hu_should_compact(history, count, config))
        return HU_OK;

    bool has_system = history[0].role == HU_ROLE_SYSTEM;
    size_t start = has_system ? 1 : 0;
    size_t non_system = count - start;

    uint32_t keep = config->keep_recent;
    if (keep > (uint32_t)non_system)
        keep = (uint32_t)non_system;

    size_t compact_count = non_system - keep;
    if (compact_count == 0)
        return HU_OK;

    size_t compact_end = start + compact_count;

    uint32_t max_src = config->max_source_chars;
    if (max_src == 0)
        max_src = HU_COMPACTION_DEFAULT_MAX_SOURCE_CHARS;

    size_t raw_alloc = 0;
    char *raw = build_summary(alloc, history, start, compact_end, max_src, &raw_alloc);
    if (!raw)
        return HU_ERR_OUT_OF_MEMORY;

    static const char sys_prompt[] =
        "Summarize the following conversation excerpt concisely, preserving key decisions, "
        "facts, and action items. Output only the summary, no preamble.";

    char *llm_summary = NULL;
    size_t llm_summary_len = 0;
    hu_error_t err = provider->vtable->chat_with_system(
        provider->ctx, alloc, sys_prompt, sizeof(sys_prompt) - 1, raw, strlen(raw), "gpt-4o-mini",
        11, 0.2, &llm_summary, &llm_summary_len);

    alloc->free(alloc->ctx, raw, raw_alloc);

    if (err != HU_OK || !llm_summary || llm_summary_len == 0) {
        if (llm_summary)
            alloc->free(alloc->ctx, llm_summary, llm_summary_len + 1);
        return hu_compact_history(alloc, history, history_count, history_cap, config);
    }

    uint32_t max_sum = config->max_summary_chars;
    if (max_sum == 0)
        max_sum = HU_COMPACTION_DEFAULT_MAX_SUMMARY_CHARS;
    if (llm_summary_len > max_sum) {
        llm_summary[max_sum] = '\0';
        llm_summary_len = max_sum;
    }

    static const char llm_prefix[] = "[LLM compaction summary]\n";
    size_t prefix_len = sizeof(llm_prefix) - 1;
    char *summary_content = (char *)alloc->alloc(alloc->ctx, prefix_len + llm_summary_len + 1);
    if (!summary_content) {
        alloc->free(alloc->ctx, llm_summary, llm_summary_len + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(summary_content, llm_prefix, prefix_len);
    memcpy(summary_content + prefix_len, llm_summary, llm_summary_len + 1);
    alloc->free(alloc->ctx, llm_summary, llm_summary_len + 1);

    free_messages(alloc, history, start, compact_end);

    history[start].role = HU_ROLE_ASSISTANT;
    history[start].content = summary_content;
    history[start].content_len = prefix_len + llm_summary_len;
    history[start].name = NULL;
    history[start].name_len = 0;
    history[start].tool_call_id = NULL;
    history[start].tool_call_id_len = 0;
    history[start].tool_calls = NULL;
    history[start].tool_calls_count = 0;

    if (compact_end > start + 1) {
        size_t shift = compact_end - start - 1;
        size_t remaining = count - compact_end;
        if (remaining > 0) {
            memmove(&history[start + 1], &history[compact_end],
                    remaining * sizeof(hu_owned_message_t));
        }
        count -= shift;
    }

    *history_count = count;
    return HU_OK;
}
