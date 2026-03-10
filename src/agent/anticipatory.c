#include "human/agent/anticipatory.h"
#include "human/core/string.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define HU_SECONDS_PER_DAY   86400
#define HU_ANTICIPATORY_DAYS 7

static void add_action(hu_anticipatory_result_t *result, hu_allocator_t *alloc,
                       hu_action_type_t type, const char *desc, size_t desc_len,
                       const char *trigger, size_t trigger_len, float relevance,
                       int64_t suggested_time) {
    if (result->action_count >= HU_ANTICIPATORY_MAX_ACTIONS)
        return;
    hu_anticipatory_action_t *a = &result->actions[result->action_count];
    a->type = type;
    a->description = desc_len > 0 ? hu_strndup(alloc, desc, desc_len) : NULL;
    a->description_len = a->description ? desc_len : 0;
    a->trigger_entity = trigger_len > 0 ? hu_strndup(alloc, trigger, trigger_len) : NULL;
    a->trigger_entity_len = a->trigger_entity ? trigger_len : 0;
    a->relevance = relevance;
    a->suggested_time = suggested_time;
    result->action_count++;
}

static void parse_temporal_lines(const char *text, size_t text_len,
                                 hu_anticipatory_result_t *result, hu_allocator_t *alloc,
                                 int64_t now_ts) {
    const char *p = text;
    const char *end = text + text_len;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t'))
            p++;
        if (p >= end)
            break;
        if (p + 2 <= end && p[0] == '-' && p[1] == ' ') {
            p += 2;
            const char *line_start = p;
            while (p < end && *p != '\n')
                p++;
            size_t line_len = (size_t)(p - line_start);
            if (line_len == 0) {
                if (p < end)
                    p++;
                continue;
            }
            /* Format: "[ts] name: description" or "name: description" */
            const char *bracket = memchr(line_start, '[', line_len);
            const char *colon = memchr(line_start, ':', line_len);
            const char *desc_start = line_start;
            size_t desc_len = line_len;
            const char *trigger_start = line_start;
            size_t trigger_len = 0;
            int64_t ts = now_ts;
            if (bracket && colon && bracket < colon) {
                long long parsed = 0;
                if (sscanf(bracket, "[%lld]", &parsed) == 1)
                    ts = (int64_t)parsed;
                const char *after_bracket =
                    memchr(bracket, ']', line_len - (size_t)(bracket - line_start));
                if (after_bracket && after_bracket < colon) {
                    trigger_start = after_bracket + 1;
                    while (trigger_start < colon &&
                           (*trigger_start == ' ' || *trigger_start == '\t'))
                        trigger_start++;
                    trigger_len = (size_t)(colon - trigger_start);
                    desc_start = colon + 1;
                    while (desc_start < line_start + line_len &&
                           (*desc_start == ' ' || *desc_start == '\t'))
                        desc_start++;
                    desc_len = (size_t)((line_start + line_len) - desc_start);
                }
            } else if (colon) {
                trigger_start = line_start;
                trigger_len = (size_t)(colon - line_start);
                desc_start = colon + 1;
                while (desc_start < line_start + line_len &&
                       (*desc_start == ' ' || *desc_start == '\t'))
                    desc_start++;
                desc_len = (size_t)((line_start + line_len) - desc_start);
            }
            if (desc_len > 0) {
                float rel = 0.8f;
                if (ts > now_ts) {
                    int64_t delta = ts - now_ts;
                    if (delta <= HU_SECONDS_PER_DAY)
                        rel = 0.95f;
                    else if (delta <= 3 * HU_SECONDS_PER_DAY)
                        rel = 0.85f;
                }
                add_action(result, alloc, HU_ACTION_CELEBRATE, desc_start, desc_len, trigger_start,
                           trigger_len, rel, ts);
            }
        }
        if (p < end && *p == '\n')
            p++;
    }
}

static void parse_causal_lines(const char *text, size_t text_len, hu_anticipatory_result_t *result,
                               hu_allocator_t *alloc) {
    const char *p = text;
    const char *end = text + text_len;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t'))
            p++;
        if (p >= end)
            break;
        if (p + 2 <= end && p[0] == '-' && p[1] == ' ') {
            p += 2;
            const char *line_start = p;
            while (p < end && *p != '\n')
                p++;
            size_t line_len = (size_t)(p - line_start);
            if (line_len == 0) {
                if (p < end)
                    p++;
                continue;
            }
            /* Format: "action -> outcome (confidence%, context)" */
            const char *arrow = NULL;
            if (line_len >= 4) {
                for (size_t i = 0; i + 3 < line_len; i++) {
                    if (line_start[i] == ' ' && line_start[i + 1] == '-' &&
                        line_start[i + 2] == '>' && line_start[i + 3] == ' ') {
                        arrow = line_start + i;
                        break;
                    }
                }
            }
            if (arrow) {
                size_t action_len = (size_t)(arrow - line_start);
                const char *outcome_start = arrow + 4;
                size_t outcome_len = (size_t)((line_start + line_len) - outcome_start);
                const char *paren = memchr(outcome_start, '(', outcome_len);
                if (paren)
                    outcome_len = (size_t)(paren - outcome_start);
                while (outcome_len > 0 && (outcome_start[outcome_len - 1] == ' ' ||
                                           outcome_start[outcome_len - 1] == '\t'))
                    outcome_len--;
                float confidence = 0.7f;
                if (paren && paren < line_start + line_len) {
                    float pct = 0.0f;
                    if (sscanf(paren, "(%f%%", &pct) == 1)
                        confidence = pct / 100.0f;
                }
                if (action_len > 0 && outcome_len > 0) {
                    char desc_buf[256];
                    int n = snprintf(desc_buf, sizeof(desc_buf), "Remind: %.*s -> %.*s",
                                     (int)action_len, line_start, (int)outcome_len, outcome_start);
                    if (n > 0 && (size_t)n < sizeof(desc_buf))
                        add_action(result, alloc, HU_ACTION_REMIND, desc_buf, (size_t)n, line_start,
                                   action_len, confidence, 0);
                }
            }
        }
        if (p < end && *p == '\n')
            p++;
    }
}

hu_error_t hu_anticipatory_analyze(hu_graph_t *graph, hu_allocator_t *alloc, const char *contact_id,
                                   size_t contact_id_len, int64_t now_ts,
                                   hu_anticipatory_result_t *result) {
    if (!graph || !alloc || !result)
        return HU_ERR_INVALID_ARGUMENT;
    memset(result, 0, sizeof(*result));

    int64_t to_ts = now_ts + (int64_t)(HU_ANTICIPATORY_DAYS * HU_SECONDS_PER_DAY);
    char *temporal_out = NULL;
    size_t temporal_len = 0;
    hu_error_t err =
        hu_graph_query_temporal(graph, alloc, now_ts, to_ts, 20, &temporal_out, &temporal_len);
    if (err == HU_OK && temporal_out && temporal_len > 0) {
        parse_temporal_lines(temporal_out, temporal_len, result, alloc, now_ts);
        alloc->free(alloc->ctx, temporal_out, temporal_len + 1);
        temporal_out = NULL;
    } else if (temporal_out) {
        alloc->free(alloc->ctx, temporal_out, temporal_len + 1);
        temporal_out = NULL;
    }

    if (contact_id && contact_id_len > 0 && result->action_count < HU_ANTICIPATORY_MAX_ACTIONS) {
        hu_graph_entity_t ent;
        memset(&ent, 0, sizeof(ent));
        err = hu_graph_find_entity(graph, contact_id, contact_id_len, &ent);
        if (err == HU_OK && ent.id > 0) {
            char *causal_out = NULL;
            size_t causal_len = 0;
            hu_error_t c_err =
                hu_graph_query_causal(graph, alloc, ent.id, 10, &causal_out, &causal_len);
            if (c_err == HU_OK && causal_out && causal_len > 0) {
                parse_causal_lines(causal_out, causal_len, result, alloc);
                alloc->free(alloc->ctx, causal_out, causal_len + 1);
            } else if (causal_out) {
                alloc->free(alloc->ctx, causal_out, causal_len + 1);
            }
            if (ent.name)
                alloc->free(alloc->ctx, ent.name, ent.name_len + 1);
            if (ent.metadata_json)
                alloc->free(alloc->ctx, ent.metadata_json, strlen(ent.metadata_json) + 1);
        }
    }

    return HU_OK;
}

hu_error_t hu_anticipatory_build_context(const hu_anticipatory_result_t *result,
                                         hu_allocator_t *alloc, char **out, size_t *out_len) {
    if (!result || !alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    if (result->action_count == 0)
        return HU_OK;

    static const char *const type_str[] = {"FOLLOW_UP", "REMIND", "CELEBRATE", "EMPATHIZE"};
    size_t cap = 512;
    for (size_t i = 0; i < result->action_count; i++) {
        const hu_anticipatory_action_t *a = &result->actions[i];
        cap += 64;
        if (a->description)
            cap += a->description_len;
    }
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    size_t pos = 0;
    int w = snprintf(buf, cap, "### Anticipatory Awareness\n");
    if (w > 0)
        pos = (size_t)w;
    for (size_t i = 0; i < result->action_count && pos < cap - 128; i++) {
        const hu_anticipatory_action_t *a = &result->actions[i];
        const char *t = (a->type <= HU_ACTION_EMPATHIZE) ? type_str[a->type] : "FOLLOW_UP";
        int pct = (int)(a->relevance * 100.0f + 0.5f);
        if (pct > 100)
            pct = 100;
        if (pct < 0)
            pct = 0;
        const char *desc = a->description ? a->description : "";
        size_t desc_len = a->description_len;
        w = snprintf(buf + pos, cap - pos, "- [%s] %.*s (%d%%)\n", t, (int)desc_len, desc, pct);
        if (w > 0)
            pos += (size_t)w;
    }
    buf[pos] = '\0';
    *out = buf;
    *out_len = pos;
    return HU_OK;
}

void hu_anticipatory_result_deinit(hu_anticipatory_result_t *result, hu_allocator_t *alloc) {
    if (!result || !alloc)
        return;
    for (size_t i = 0; i < result->action_count; i++) {
        hu_anticipatory_action_t *a = &result->actions[i];
        if (a->description) {
            hu_str_free(alloc, a->description);
            a->description = NULL;
            a->description_len = 0;
        }
        if (a->trigger_entity) {
            hu_str_free(alloc, a->trigger_entity);
            a->trigger_entity = NULL;
            a->trigger_entity_len = 0;
        }
    }
    result->action_count = 0;
}
