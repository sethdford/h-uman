#include "human/agent/compaction.h"
#include "human/agent/compaction_structured.h"
#include "human/core/string.h"
#include "human/core/tokens.h"
#include "human/provider.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* chat_with_system model label: provider ctx supplies real model on main path; compaction has no
 * per-call model here, so use the same neutral default as the multi-provider router. */
static const char hu_compaction_chat_model[] = "default";
#define HU_COMPACTION_CHAT_MODEL_LEN (sizeof(hu_compaction_chat_model) - 1)

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
    cfg->use_structured_summary = false;
    cfg->preserve_recent_count = 0;
    cfg->inject_continuation_preamble = false;
    cfg->pinned_artifacts = NULL;
    cfg->pinned_artifacts_count = 0;
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
    /* Divides the SUM rather than per-message, so it deliberately does NOT use
     * hu_tokens_estimate_text — that would change rounding and therefore move
     * compaction thresholds. Only the divisor is shared. */
    return (total_chars + 3 * (uint64_t)history_count) / HU_TOKENS_BYTES_PER_TOKEN +
           multimodal_tokens;
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

hu_error_t hu_compact_history(hu_allocator_t *alloc, hu_owned_message_t **history,
                              size_t *history_count, size_t *history_cap,
                              const hu_compaction_config_t *config) {
    if (!alloc || !history || !*history || !history_count || !history_cap || !config)
        return HU_ERR_INVALID_ARGUMENT;
    size_t count = *history_count;

    (void)history_cap;
    if (!*history || count == 0)
        return HU_OK;
    if (!hu_should_compact(*history, count, config))
        return HU_OK;

    /* Structured compaction path: generate XML summary with metadata */
    if (config->use_structured_summary) {
        uint32_t keep =
            config->preserve_recent_count > 0 ? config->preserve_recent_count : config->keep_recent;

        hu_compaction_summary_t meta;
        hu_error_t err = hu_compact_extract_metadata(alloc, *history, count, keep, &meta);
        if (err != HU_OK)
            return err;

        /* Determine range to summarize */
        bool sys = count > 0 && (*history)[0].role == HU_ROLE_SYSTEM;
        size_t s = sys ? 1 : 0;
        size_t non_sys = count - s;
        if (keep > (uint32_t)non_sys)
            keep = (uint32_t)non_sys;
        size_t compact_end = s + (non_sys - keep);

        /* Build structured summary from messages being compacted */
        char *xml = NULL;
        size_t xml_len = 0;
        err = hu_compact_build_structured_summary(alloc, *history + s, compact_end - s, &meta, &xml,
                                                  &xml_len);
        if (err != HU_OK) {
            hu_compaction_summary_free(alloc, &meta);
            return err;
        }

        /* Free compacted messages */
        free_messages(alloc, *history, s, compact_end);

        /* Replace first slot with structured summary */
        (*history)[s].role = HU_ROLE_ASSISTANT;
        (*history)[s].content = xml;
        (*history)[s].content_len = xml_len;
        (*history)[s].name = NULL;
        (*history)[s].name_len = 0;
        (*history)[s].tool_call_id = NULL;
        (*history)[s].tool_call_id_len = 0;
        (*history)[s].tool_calls = NULL;
        (*history)[s].tool_calls_count = 0;
        (*history)[s].content_parts = NULL;
        (*history)[s].content_parts_count = 0;

        /* Shift remaining messages down */
        if (compact_end > s + 1) {
            size_t shift = compact_end - s - 1;
            size_t remaining = count - compact_end;
            if (remaining > 0) {
                memmove(&(*history)[s + 1], &(*history)[compact_end],
                        remaining * sizeof(hu_owned_message_t));
            }
            count -= shift;
        }
        *history_count = count;

        /* Inject continuation preamble if configured */
        if (config->inject_continuation_preamble) {
            err = hu_compact_inject_continuation_preamble(alloc, &meta, history, history_count,
                                                          history_cap);
        }

        hu_compaction_summary_free(alloc, &meta);
        return err;
    }

    bool has_system = (*history)[0].role == HU_ROLE_SYSTEM;
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
        build_summary(alloc, *history, start, compact_end, max_src, &summary_raw_alloc);
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
    free_messages(alloc, *history, start, compact_end);

    /* Replace first compacted slot with summary (assistant role for consistency) */
    (*history)[start].role = HU_ROLE_ASSISTANT;
    (*history)[start].content = summary_content;
    (*history)[start].content_len = prefix_len + sum_len;
    (*history)[start].name = NULL;
    (*history)[start].name_len = 0;
    (*history)[start].tool_call_id = NULL;
    (*history)[start].tool_call_id_len = 0;
    (*history)[start].tool_calls = NULL;
    (*history)[start].tool_calls_count = 0;

    /* Shift remaining messages down */
    if (compact_end > start + 1) {
        size_t shift = compact_end - start - 1;
        size_t remaining = count - compact_end;
        if (remaining > 0) {
            memmove(&(*history)[start + 1], &(*history)[compact_end],
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
    /* OpenAI-compatible APIs require every tool message to follow an assistant message that
     * includes matching tool_calls. If we drop an assistant-with-tools but keep its tool
     * results, the next request returns HTTP 400 (tool_call_id not in previous tool_calls). */
    if (compact_end > start && compact_end <= count) {
        hu_owned_message_t *last_removed = &history[compact_end - 1];
        if (last_removed->role == HU_ROLE_ASSISTANT && last_removed->tool_calls &&
            last_removed->tool_calls_count > 0) {
            while (compact_end < count && history[compact_end].role == HU_ROLE_TOOL)
                compact_end++;
        }
    }
    size_t n_freed = compact_end - start;
    char buf[96];
    int n = snprintf(buf, sizeof(buf), "[Previous context compacted: %zu messages summarized]",
                     n_freed);
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

hu_error_t hu_compact_history_llm(hu_allocator_t *alloc, hu_owned_message_t **history,
                                  size_t *history_count, size_t *history_cap,
                                  const hu_compaction_config_t *config, hu_provider_t *provider) {
    if (!alloc || !history || !*history || !history_count || !history_cap || !config)
        return HU_ERR_INVALID_ARGUMENT;

    if (!provider || !provider->vtable || !provider->vtable->chat_with_system)
        return hu_compact_history(alloc, history, history_count, history_cap, config);

    size_t count = *history_count;
    if (!hu_should_compact(*history, count, config))
        return HU_OK;

    bool has_system = (*history)[0].role == HU_ROLE_SYSTEM;
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
    char *raw = build_summary(alloc, *history, start, compact_end, max_src, &raw_alloc);
    if (!raw)
        return HU_ERR_OUT_OF_MEMORY;

    static const char sys_prompt[] =
        "Summarize the following conversation excerpt concisely, preserving key decisions, "
        "facts, and action items. Output only the summary, no preamble.";

    char *llm_summary = NULL;
    size_t llm_summary_len = 0;
    hu_error_t err = provider->vtable->chat_with_system(
        provider->ctx, alloc, sys_prompt, sizeof(sys_prompt) - 1, raw, strlen(raw),
        hu_compaction_chat_model, HU_COMPACTION_CHAT_MODEL_LEN, 0.2, &llm_summary,
        &llm_summary_len);

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

    free_messages(alloc, *history, start, compact_end);

    (*history)[start].role = HU_ROLE_ASSISTANT;
    (*history)[start].content = summary_content;
    (*history)[start].content_len = prefix_len + llm_summary_len;
    (*history)[start].name = NULL;
    (*history)[start].name_len = 0;
    (*history)[start].tool_call_id = NULL;
    (*history)[start].tool_call_id_len = 0;
    (*history)[start].tool_calls = NULL;
    (*history)[start].tool_calls_count = 0;

    if (compact_end > start + 1) {
        size_t shift = compact_end - start - 1;
        size_t remaining = count - compact_end;
        if (remaining > 0) {
            memmove(&(*history)[start + 1], &(*history)[compact_end],
                    remaining * sizeof(hu_owned_message_t));
        }
        count -= shift;
    }

    *history_count = count;
    return HU_OK;
}

#if !(defined(HU_IS_TEST) && HU_IS_TEST)

/* Rough char caps (~5 chars/word) for hierarchical summaries */
#define HU_HIER_SESSION_MAX_CHARS 1400u
#define HU_HIER_CHAPTER_MAX_CHARS 700u
#define HU_HIER_OVERALL_MAX_CHARS 350u

static void hierarchical_truncate(char *s, size_t *len, size_t max_chars) {
    if (!s || !len || max_chars == 0)
        return;
    if (*len > max_chars) {
        s[max_chars] = '\0';
        *len = max_chars;
    }
}

static hu_error_t hierarchical_chat(hu_provider_t *provider, hu_allocator_t *alloc,
                                    const char *model, size_t model_len, const char *system_prompt,
                                    size_t system_prompt_len, const char *user, size_t user_len,
                                    char **out, size_t *out_len) {
    if (!provider || !provider->vtable || !provider->vtable->chat_with_system)
        return HU_ERR_NOT_SUPPORTED;
    return provider->vtable->chat_with_system(provider->ctx, alloc, system_prompt,
                                              system_prompt_len, user, user_len, model, model_len,
                                              0.2, out, out_len);
}

static char *hier_build_user2(hu_allocator_t *alloc, const char *session, size_t session_len,
                              size_t *out_len) {
    static const char pre[] = "### Session summary\n";
    static const char mid[] = "\n### Recent chapter context\n";
    static const char none[] = "(none)\n";
    static const char post[] =
        "\nCombine the session summary with the chapter context. Produce a chapter-level summary "
        "in about 100 words (ongoing themes, key decisions). Output only the summary, no preamble.";
    size_t n = (sizeof(pre) - 1) + session_len + (sizeof(mid) - 1) + (sizeof(none) - 1) +
               (sizeof(post) - 1);
    if (n < session_len)
        return NULL;
    char *buf = (char *)alloc->alloc(alloc->ctx, n + 1);
    if (!buf)
        return NULL;
    size_t o = 0;
    memcpy(buf + o, pre, sizeof(pre) - 1);
    o += sizeof(pre) - 1;
    if (session_len > 0 && session) {
        memcpy(buf + o, session, session_len);
        o += session_len;
    }
    memcpy(buf + o, mid, sizeof(mid) - 1);
    o += sizeof(mid) - 1;
    memcpy(buf + o, none, sizeof(none) - 1);
    o += sizeof(none) - 1;
    memcpy(buf + o, post, sizeof(post) - 1);
    o += sizeof(post) - 1;
    buf[o] = '\0';
    *out_len = o;
    return buf;
}

static char *hier_build_user3(hu_allocator_t *alloc, const char *chapter, size_t chapter_len,
                              size_t *out_len) {
    static const char pre[] = "### Chapter summary\n";
    static const char mid[] = "\n### Previous overall summary\n";
    static const char none[] = "(none)\n";
    static const char post[] =
        "\nMerge the chapter summary with the previous overall summary. Produce an updated overall "
        "summary in about 50 words. Output only the summary, no preamble.";
    size_t n = (sizeof(pre) - 1) + chapter_len + (sizeof(mid) - 1) + (sizeof(none) - 1) +
               (sizeof(post) - 1);
    if (n < chapter_len)
        return NULL;
    char *buf = (char *)alloc->alloc(alloc->ctx, n + 1);
    if (!buf)
        return NULL;
    size_t o = 0;
    memcpy(buf + o, pre, sizeof(pre) - 1);
    o += sizeof(pre) - 1;
    if (chapter_len > 0 && chapter) {
        memcpy(buf + o, chapter, chapter_len);
        o += chapter_len;
    }
    memcpy(buf + o, mid, sizeof(mid) - 1);
    o += sizeof(mid) - 1;
    memcpy(buf + o, none, sizeof(none) - 1);
    o += sizeof(none) - 1;
    memcpy(buf + o, post, sizeof(post) - 1);
    o += sizeof(post) - 1;
    buf[o] = '\0';
    *out_len = o;
    return buf;
}

#endif /* !(HU_IS_TEST) */

hu_error_t hu_compact_hierarchical(hu_allocator_t *alloc, hu_provider_t *provider,
                                   const char *model, size_t model_len, const char *conversation,
                                   size_t conversation_len, char **session_summary,
                                   size_t *session_len, char **chapter_summary, size_t *chapter_len,
                                   char **overall_summary, size_t *overall_len) {
    if (!alloc || !session_summary || !session_len || !chapter_summary || !chapter_len ||
        !overall_summary || !overall_len)
        return HU_ERR_INVALID_ARGUMENT;
    if (!conversation && conversation_len > 0)
        return HU_ERR_INVALID_ARGUMENT;

    *session_summary = NULL;
    *session_len = 0;
    *chapter_summary = NULL;
    *chapter_len = 0;
    *overall_summary = NULL;
    *overall_len = 0;

#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)provider;
    (void)model;
    (void)model_len;
    (void)conversation;
    (void)conversation_len;

    static const char mock_s[] = "Session summary: discussed project plans and timeline";
    static const char mock_c[] = "Chapter summary: ongoing project work, key decisions made";
    static const char mock_o[] = "Overall: productive collaboration on software project";

    size_t sl = sizeof(mock_s) - 1;
    size_t cl = sizeof(mock_c) - 1;
    size_t ol = sizeof(mock_o) - 1;
    char *s = hu_strndup(alloc, mock_s, sl);
    char *c = hu_strndup(alloc, mock_c, cl);
    char *o = hu_strndup(alloc, mock_o, ol);
    if (!s || !c || !o) {
        if (s)
            alloc->free(alloc->ctx, s, sl + 1);
        if (c)
            alloc->free(alloc->ctx, c, cl + 1);
        if (o)
            alloc->free(alloc->ctx, o, ol + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    *session_summary = s;
    *session_len = sl;
    *chapter_summary = c;
    *chapter_len = cl;
    *overall_summary = o;
    *overall_len = ol;
    return HU_OK;
#else
    if (!provider || !model || model_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    const char *conv = conversation ? conversation : "";

    static const char sys1[] =
        "Summarize the conversation excerpt concisely in about 200 words. "
        "PRIORITIZE preserving: (1) emotional context — how people felt, tone shifts, "
        "moments of vulnerability or connection; (2) relational dynamics — trust, "
        "rapport, inside jokes, shared references; (3) key decisions, commitments, "
        "and open questions; (4) factual details. Output only the summary text, "
        "no preamble.";
    static const char tail1[] =
        "\n\nSummarize the above conversation for long-term session memory.";

    size_t t1 = sizeof(tail1) - 1;
    size_t u1_len = conversation_len + t1;
    if (u1_len < conversation_len)
        return HU_ERR_INVALID_ARGUMENT;
    char *user1 = (char *)alloc->alloc(alloc->ctx, u1_len + 1);
    if (!user1)
        return HU_ERR_OUT_OF_MEMORY;
    if (conversation_len > 0)
        memcpy(user1, conv, conversation_len);
    memcpy(user1 + conversation_len, tail1, t1 + 1);

    char *sess = NULL;
    size_t sess_len = 0;
    hu_error_t err = hierarchical_chat(provider, alloc, model, model_len, sys1, sizeof(sys1) - 1,
                                       user1, u1_len, &sess, &sess_len);
    alloc->free(alloc->ctx, user1, u1_len + 1);
    if (err != HU_OK || !sess || sess_len == 0) {
        if (sess)
            alloc->free(alloc->ctx, sess, sess_len + 1);
        return err != HU_OK ? err : HU_ERR_PROVIDER_RESPONSE;
    }
    hierarchical_truncate(sess, &sess_len, HU_HIER_SESSION_MAX_CHARS);

    static const char sys2[] =
        "You consolidate rolling memory across sessions within a chapter. Follow the user "
        "instructions exactly.";

    size_t u2_len = 0;
    char *user2 = hier_build_user2(alloc, sess, sess_len, &u2_len);
    if (!user2) {
        alloc->free(alloc->ctx, sess, sess_len + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }

    char *chap = NULL;
    size_t chap_len = 0;
    err = hierarchical_chat(provider, alloc, model, model_len, sys2, sizeof(sys2) - 1, user2,
                            u2_len, &chap, &chap_len);
    alloc->free(alloc->ctx, user2, u2_len + 1);
    if (err != HU_OK || !chap || chap_len == 0) {
        alloc->free(alloc->ctx, sess, sess_len + 1);
        if (chap)
            alloc->free(alloc->ctx, chap, chap_len + 1);
        return err != HU_OK ? err : HU_ERR_PROVIDER_RESPONSE;
    }
    hierarchical_truncate(chap, &chap_len, HU_HIER_CHAPTER_MAX_CHARS);

    static const char sys3[] =
        "You maintain a compact long-running narrative summary. Follow the user instructions "
        "exactly.";

    size_t u3_len = 0;
    char *user3 = hier_build_user3(alloc, chap, chap_len, &u3_len);
    if (!user3) {
        alloc->free(alloc->ctx, sess, sess_len + 1);
        alloc->free(alloc->ctx, chap, chap_len + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }

    char *overall = NULL;
    size_t overall_l = 0;
    err = hierarchical_chat(provider, alloc, model, model_len, sys3, sizeof(sys3) - 1, user3,
                            u3_len, &overall, &overall_l);
    alloc->free(alloc->ctx, user3, u3_len + 1);
    if (err != HU_OK || !overall || overall_l == 0) {
        alloc->free(alloc->ctx, sess, sess_len + 1);
        alloc->free(alloc->ctx, chap, chap_len + 1);
        if (overall)
            alloc->free(alloc->ctx, overall, overall_l + 1);
        return err != HU_OK ? err : HU_ERR_PROVIDER_RESPONSE;
    }
    hierarchical_truncate(overall, &overall_l, HU_HIER_OVERALL_MAX_CHARS);

    *session_summary = sess;
    *session_len = sess_len;
    *chapter_summary = chap;
    *chapter_len = chap_len;
    *overall_summary = overall;
    *overall_len = overall_l;
    return HU_OK;
#endif /* HU_IS_TEST */
}

/* ── Hierarchical Compaction ──────────────────────────────────────────── */

static char *summarize_chunk_text(hu_allocator_t *alloc, hu_provider_t *provider, const char *chunk,
                                  size_t chunk_len, uint32_t max_chars, size_t *out_len) {
    *out_len = 0;
    if (provider && provider->vtable && provider->vtable->chat_with_system) {
        static const char sys[] =
            "Summarize the following conversation excerpt into a brief paragraph. "
            "PRIORITIZE: emotional context (feelings, tone, vulnerability), "
            "relational dynamics (trust, rapport, shared references), then "
            "decisions, commitments, and facts. Output only the summary.";
        char *summary = NULL;
        size_t summary_len = 0;
        hu_error_t serr = provider->vtable->chat_with_system(
            provider->ctx, alloc, sys, sizeof(sys) - 1, chunk, chunk_len, hu_compaction_chat_model,
            HU_COMPACTION_CHAT_MODEL_LEN, 0.2, &summary, &summary_len);
        if (serr == HU_OK && summary && summary_len > 0) {
            if (summary_len > max_chars) {
                summary[max_chars] = '\0';
                summary_len = max_chars;
            }
            *out_len = summary_len;
            return summary;
        }
        if (summary)
            alloc->free(alloc->ctx, summary, summary_len + 1);
    }
    size_t tlen = chunk_len < max_chars ? chunk_len : max_chars;
    char *trunc = hu_strndup(alloc, chunk, tlen);
    if (trunc)
        *out_len = tlen;
    return trunc;
}

hu_error_t hu_compact_history_hierarchical(hu_allocator_t *alloc, hu_owned_message_t **history,
                                           size_t *history_count, size_t *history_cap,
                                           const hu_compaction_config_t *config,
                                           hu_provider_t *provider, uint32_t chunk_size,
                                           uint32_t max_depth) {
    if (!alloc || !history || !*history || !history_count || !history_cap || !config)
        return HU_ERR_INVALID_ARGUMENT;

    size_t cnt = *history_count;
    if (!hu_should_compact(*history, cnt, config))
        return HU_OK;

    if (chunk_size == 0)
        chunk_size = 10;
    if (max_depth == 0)
        max_depth = 3;

    bool has_sys = cnt > 0 && (*history)[0].role == HU_ROLE_SYSTEM;
    size_t hstart = has_sys ? 1 : 0;
    size_t non_sys = cnt - hstart;

    uint32_t keep = config->keep_recent;
    if (keep > (uint32_t)non_sys)
        keep = (uint32_t)non_sys;

    size_t compact_n = non_sys - keep;
    if (compact_n == 0)
        return HU_OK;

    size_t cend = hstart + compact_n;
    size_t orig_chunks = (compact_n + chunk_size - 1) / chunk_size;
    size_t nchunks = orig_chunks;

    char **ctexts = (char **)alloc->alloc(alloc->ctx, orig_chunks * sizeof(char *));
    size_t *clens = (size_t *)alloc->alloc(alloc->ctx, orig_chunks * sizeof(size_t));
    if (!ctexts || !clens) {
        if (ctexts)
            alloc->free(alloc->ctx, ctexts, orig_chunks * sizeof(char *));
        if (clens)
            alloc->free(alloc->ctx, clens, orig_chunks * sizeof(size_t));
        return hu_compact_history_llm(alloc, history, history_count, history_cap, config, provider);
    }
    memset(ctexts, 0, orig_chunks * sizeof(char *));
    memset(clens, 0, orig_chunks * sizeof(size_t));

    uint32_t msrc = config->max_source_chars > 0 ? config->max_source_chars
                                                 : HU_COMPACTION_DEFAULT_MAX_SOURCE_CHARS;
    uint32_t msum = config->max_summary_chars > 0 ? config->max_summary_chars
                                                  : HU_COMPACTION_DEFAULT_MAX_SUMMARY_CHARS;

    for (size_t ci = 0; ci < nchunks; ci++) {
        size_t cs = hstart + ci * chunk_size;
        size_t ce = cs + chunk_size;
        if (ce > cend)
            ce = cend;

        size_t raw_alloc_sz = 0;
        char *raw = build_summary(alloc, *history, cs, ce, msrc, &raw_alloc_sz);
        if (!raw)
            continue;

        size_t slen = 0;
        uint32_t pcmax = msum / (uint32_t)nchunks + 200;
        char *s = summarize_chunk_text(alloc, provider, raw, strlen(raw), pcmax, &slen);
        alloc->free(alloc->ctx, raw, raw_alloc_sz);
        ctexts[ci] = s;
        clens[ci] = slen;
    }

    /* Hierarchical reduction: merge pairs until result fits */
    uint32_t depth = 0;
    while (nchunks > 1 && depth < max_depth) {
        size_t tl = 0;
        for (size_t i = 0; i < nchunks; i++)
            tl += clens[i];
        if (tl <= msum)
            break;

        size_t nc = (nchunks + 1) / 2;
        for (size_t i = 0; i < nc; i++) {
            size_t a = i * 2;
            size_t b = a + 1;
            if (b >= nchunks) {
                if (a != i) {
                    ctexts[i] = ctexts[a];
                    clens[i] = clens[a];
                    ctexts[a] = NULL;
                }
                continue;
            }
            size_t comb_len = clens[a] + 1 + clens[b];
            char *comb = (char *)alloc->alloc(alloc->ctx, comb_len + 1);
            if (comb) {
                if (ctexts[a])
                    memcpy(comb, ctexts[a], clens[a]);
                comb[clens[a]] = '\n';
                if (ctexts[b])
                    memcpy(comb + clens[a] + 1, ctexts[b], clens[b]);
                comb[comb_len] = '\0';

                size_t ml = 0;
                uint32_t pm = msum / (uint32_t)nc + 200;
                char *m = summarize_chunk_text(alloc, provider, comb, comb_len, pm, &ml);
                alloc->free(alloc->ctx, comb, comb_len + 1);
                if (ctexts[a])
                    alloc->free(alloc->ctx, ctexts[a], clens[a] + 1);
                if (ctexts[b])
                    alloc->free(alloc->ctx, ctexts[b], clens[b] + 1);
                ctexts[a] = NULL;
                ctexts[b] = NULL;
                ctexts[i] = m;
                clens[i] = ml;
            } else {
                if (a != i) {
                    ctexts[i] = ctexts[a];
                    clens[i] = clens[a];
                    ctexts[a] = NULL;
                }
                if (ctexts[b])
                    alloc->free(alloc->ctx, ctexts[b], clens[b] + 1);
                ctexts[b] = NULL;
            }
        }
        nchunks = nc;
        depth++;
    }

    size_t ftotal = 0;
    for (size_t i = 0; i < nchunks; i++)
        ftotal += clens[i] + 1;

    char pfx[64];
    int pn = snprintf(pfx, sizeof(pfx), "[Hierarchical compaction L%u]\n", depth + 1);
    size_t pfx_len = (pn > 0 && (size_t)pn < sizeof(pfx)) ? (size_t)pn : 0;

    char *fc = (char *)alloc->alloc(alloc->ctx, pfx_len + ftotal + 1);
    if (!fc) {
        for (size_t i = 0; i < orig_chunks; i++)
            if (ctexts[i])
                alloc->free(alloc->ctx, ctexts[i], clens[i] + 1);
        alloc->free(alloc->ctx, ctexts, orig_chunks * sizeof(char *));
        alloc->free(alloc->ctx, clens, orig_chunks * sizeof(size_t));
        return HU_ERR_OUT_OF_MEMORY;
    }

    memcpy(fc, pfx, pfx_len);
    size_t fp = pfx_len;
    for (size_t i = 0; i < nchunks; i++) {
        if (ctexts[i] && clens[i] > 0) {
            memcpy(fc + fp, ctexts[i], clens[i]);
            fp += clens[i];
            fc[fp++] = '\n';
        }
    }
    fc[fp] = '\0';

    for (size_t i = 0; i < orig_chunks; i++)
        if (ctexts[i])
            alloc->free(alloc->ctx, ctexts[i], clens[i] + 1);
    alloc->free(alloc->ctx, ctexts, orig_chunks * sizeof(char *));
    alloc->free(alloc->ctx, clens, orig_chunks * sizeof(size_t));

    free_messages(alloc, *history, hstart, cend);

    (*history)[hstart].role = HU_ROLE_ASSISTANT;
    (*history)[hstart].content = fc;
    (*history)[hstart].content_len = fp;
    (*history)[hstart].name = NULL;
    (*history)[hstart].name_len = 0;
    (*history)[hstart].tool_call_id = NULL;
    (*history)[hstart].tool_call_id_len = 0;
    (*history)[hstart].tool_calls = NULL;
    (*history)[hstart].tool_calls_count = 0;

    if (cend > hstart + 1) {
        size_t hshift = cend - hstart - 1;
        size_t hrem = cnt - cend;
        if (hrem > 0)
            memmove(&(*history)[hstart + 1], &(*history)[cend], hrem * sizeof(hu_owned_message_t));
        cnt -= hshift;
    }

    *history_count = cnt;
    return HU_OK;
}
