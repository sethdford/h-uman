/*
 * Proactive action system — milestones, morning briefing, check-in.
 */
#include "seaclaw/agent/proactive.h"
#include "seaclaw/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int compare_priority_desc(const void *a, const void *b) {
    const sc_proactive_action_t *pa = (const sc_proactive_action_t *)a;
    const sc_proactive_action_t *pb = (const sc_proactive_action_t *)b;
    if (pa->priority > pb->priority)
        return -1;
    if (pa->priority < pb->priority)
        return 1;
    return 0;
}

sc_error_t sc_proactive_check(sc_allocator_t *alloc, uint32_t session_count, uint8_t hour,
                               sc_proactive_result_t *out) {
    return sc_proactive_check_extended(alloc, session_count, hour, NULL, 0, NULL, NULL, 0, out);
}

sc_error_t sc_proactive_check_extended(sc_allocator_t *alloc, uint32_t session_count, uint8_t hour,
                                        const sc_commitment_t *commitments, size_t commitment_count,
                                        const char *const *pattern_subjects,
                                        const uint32_t *pattern_counts, size_t pattern_count,
                                        sc_proactive_result_t *out) {
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    /* Session count milestones: 10, 25, 50, 100 */
    static const uint32_t MILESTONES[] = {10, 25, 50, 100};
    for (size_t i = 0; i < sizeof(MILESTONES) / sizeof(MILESTONES[0]); i++) {
        if (session_count == MILESTONES[i] && out->count < SC_PROACTIVE_MAX_ACTIONS) {
            char msg[128];
            int n = snprintf(msg, sizeof(msg),
                            "This is session %u together — a meaningful milestone.",
                            (unsigned)session_count);
            if (n > 0 && (size_t)n < sizeof(msg)) {
                sc_proactive_action_t *act = &out->actions[out->count];
                act->type = SC_PROACTIVE_MILESTONE;
                act->message = sc_strndup(alloc, msg, (size_t)n);
                if (!act->message)
                    return SC_ERR_OUT_OF_MEMORY;
                act->message_len = (size_t)n;
                act->priority = 0.9;
                out->count++;
            }
            break; /* only one milestone per check */
        }
    }

    /* Morning hour (8-10) → MORNING_BRIEFING */
    if (hour >= 8 && hour <= 10 && out->count < SC_PROACTIVE_MAX_ACTIONS) {
        static const char BRIEF[] =
            "Good morning. Consider reviewing any active commitments and plans for today.";
        sc_proactive_action_t *act = &out->actions[out->count];
        act->type = SC_PROACTIVE_MORNING_BRIEFING;
        act->message = sc_strndup(alloc, BRIEF, sizeof(BRIEF) - 1);
        if (!act->message)
            return SC_ERR_OUT_OF_MEMORY;
        act->message_len = sizeof(BRIEF) - 1;
        act->priority = 0.7;
        out->count++;
    }

    /* COMMITMENT_FOLLOW_UP: up to 2 active commitments with created_at (assume overdue) */
    if (commitments && commitment_count > 0 && out->count < SC_PROACTIVE_MAX_ACTIONS) {
        static const size_t MAX_COMMITMENT_FOLLOW_UPS = 2;
        size_t added = 0;
        for (size_t i = 0; i < commitment_count && added < MAX_COMMITMENT_FOLLOW_UPS; i++) {
            const sc_commitment_t *c = &commitments[i];
            if (c->status != SC_COMMITMENT_ACTIVE)
                continue;
            if (!c->created_at || c->created_at[0] == '\0')
                continue;
            if (!c->summary || c->summary_len == 0)
                continue;
            if (out->count >= SC_PROACTIVE_MAX_ACTIONS)
                break;
            char msg[256];
            size_t summary_len = c->summary_len > 200 ? 200 : c->summary_len;
            int n = snprintf(msg, sizeof(msg),
                            "You mentioned: '%.*s'. Would you like to follow up on this?",
                            (int)summary_len, c->summary);
            if (n > 0 && (size_t)n < sizeof(msg)) {
                sc_proactive_action_t *act = &out->actions[out->count];
                act->type = SC_PROACTIVE_COMMITMENT_FOLLOW_UP;
                act->message = sc_strndup(alloc, msg, (size_t)n);
                if (!act->message)
                    return SC_ERR_OUT_OF_MEMORY;
                act->message_len = (size_t)n;
                act->priority = 0.8;
                out->count++;
                added++;
            }
        }
    }

    /* PATTERN_INSIGHT: up to 2 patterns with occurrence_count >= 5 */
    if (pattern_subjects && pattern_counts && pattern_count > 0 &&
        out->count < SC_PROACTIVE_MAX_ACTIONS) {
        static const size_t MAX_PATTERN_INSIGHTS = 2;
        static const uint32_t PATTERN_THRESHOLD = 5;
        size_t added = 0;
        for (size_t i = 0; i < pattern_count && added < MAX_PATTERN_INSIGHTS; i++) {
            if (pattern_counts[i] < PATTERN_THRESHOLD)
                continue;
            const char *subject = pattern_subjects[i];
            if (!subject)
                continue;
            if (out->count >= SC_PROACTIVE_MAX_ACTIONS)
                break;
            char msg[256];
            int n = snprintf(msg, sizeof(msg),
                            "'%s' has come up %u times in your conversations. This seems "
                            "important to you.",
                            subject, (unsigned)pattern_counts[i]);
            if (n > 0 && (size_t)n < sizeof(msg)) {
                sc_proactive_action_t *act = &out->actions[out->count];
                act->type = SC_PROACTIVE_PATTERN_INSIGHT;
                act->message = sc_strndup(alloc, msg, (size_t)n);
                if (!act->message)
                    return SC_ERR_OUT_OF_MEMORY;
                act->message_len = (size_t)n;
                act->priority = 0.6;
                out->count++;
                added++;
            }
        }
    }

    /* Always: CHECK_IN (low priority) */
    if (out->count < SC_PROACTIVE_MAX_ACTIONS) {
        static const char CHECK[] =
            "Check in on how the user is feeling. Ask about progress on any ongoing goals.";
        sc_proactive_action_t *act = &out->actions[out->count];
        act->type = SC_PROACTIVE_CHECK_IN;
        act->message = sc_strndup(alloc, CHECK, sizeof(CHECK) - 1);
        if (!act->message)
            return SC_ERR_OUT_OF_MEMORY;
        act->message_len = sizeof(CHECK) - 1;
        act->priority = 0.2;
        out->count++;
    }

    return SC_OK;
}

sc_error_t sc_proactive_build_context(const sc_proactive_result_t *result,
                                       sc_allocator_t *alloc, size_t max_actions,
                                       char **out, size_t *out_len) {
    if (!result || !alloc || !out || !out_len)
        return SC_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    if (result->count == 0)
        return SC_OK;

    /* Sort by priority descending (copy to avoid mutating) */
    sc_proactive_action_t sorted[SC_PROACTIVE_MAX_ACTIONS];
    size_t n = result->count < SC_PROACTIVE_MAX_ACTIONS ? result->count : SC_PROACTIVE_MAX_ACTIONS;
    memcpy(sorted, result->actions, n * sizeof(sc_proactive_action_t));
    qsort(sorted, n, sizeof(sc_proactive_action_t), compare_priority_desc);

    size_t take = n < max_actions ? n : max_actions;
    size_t cap = 64;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf)
        return SC_ERR_OUT_OF_MEMORY;
    size_t len = 0;
    buf[0] = '\0';

    static const char HEADER[] = "### Proactive Awareness\n\n";
    size_t hlen = sizeof(HEADER) - 1;
    while (len + hlen + 1 > cap) {
        size_t new_cap = cap * 2;
        char *nb = (char *)alloc->realloc(alloc->ctx, buf, cap, new_cap);
        if (!nb) {
            alloc->free(alloc->ctx, buf, cap);
            return SC_ERR_OUT_OF_MEMORY;
        }
        buf = nb;
        cap = new_cap;
    }
    memcpy(buf, HEADER, hlen);
    len = hlen;

    for (size_t i = 0; i < take; i++) {
        const sc_proactive_action_t *act = &sorted[i];
        if (!act->message || act->message_len == 0)
            continue;
        size_t need = len + 2 + act->message_len + 2; /* "- " + msg + "\n" */
        while (need > cap) {
            size_t new_cap = cap * 2;
            char *nb = (char *)alloc->realloc(alloc->ctx, buf, cap, new_cap);
            if (!nb) {
                alloc->free(alloc->ctx, buf, cap);
                return SC_ERR_OUT_OF_MEMORY;
            }
            buf = nb;
            cap = new_cap;
        }
        buf[len++] = '-';
        buf[len++] = ' ';
        memcpy(buf + len, act->message, act->message_len);
        len += act->message_len;
        buf[len++] = '\n';
    }
    buf[len] = '\0';

    *out = buf;
    *out_len = len;
    return SC_OK;
}

void sc_proactive_result_deinit(sc_proactive_result_t *result, sc_allocator_t *alloc) {
    if (!result || !alloc)
        return;
    for (size_t i = 0; i < result->count; i++) {
        if (result->actions[i].message) {
            alloc->free(alloc->ctx, result->actions[i].message,
                        result->actions[i].message_len + 1);
            result->actions[i].message = NULL;
        }
    }
    result->count = 0;
}
