/*
 * Proactive action system — milestones, morning briefing, check-in.
 */
#include "seaclaw/agent/proactive.h"
#include "seaclaw/core/string.h"
#include "seaclaw/memory.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SC_PROACTIVE_EVENT_FOLLOW_UP_CAP 3
#define MS_PER_HOUR                      (3600ULL * 1000ULL)
#define MS_PER_DAY                       (24ULL * MS_PER_HOUR)
#define HOURS_3_DAYS                     72u
#define HOURS_7_DAYS                     168u
#define HOURS_14_DAYS                    336u

sc_error_t sc_proactive_check_silence(sc_allocator_t *alloc, uint64_t last_contact_ms,
                                      uint64_t now_ms, const sc_silence_config_t *config,
                                      sc_proactive_result_t *out) {
    if (!alloc || !out || !config)
        return SC_ERR_INVALID_ARGUMENT;
    if (!config->enabled || last_contact_ms == 0)
        return SC_OK;
    if (now_ms <= last_contact_ms)
        return SC_OK; /* clock skew or equal timestamps — avoid uint64_t underflow */

    uint64_t elapsed_hours = (now_ms - last_contact_ms) / MS_PER_HOUR;
    if (elapsed_hours < config->threshold_hours)
        return SC_OK;
    if (out->count >= SC_PROACTIVE_MAX_ACTIONS)
        return SC_OK;

    /* Data-driven: exact elapsed time, contextual guidance for LLM */
    uint32_t elapsed_days = (uint32_t)(elapsed_hours / 24u);
    char msg[384];
    int n;
    if (elapsed_days == 0) {
        n = snprintf(msg, sizeof(msg),
                     "PROACTIVE CHECK-IN: Last conversation: %u hours ago. Generate a natural, "
                     "warm check-in. Don't say \"I was thinking about you\" or \"just checking "
                     "in\" — find a specific, genuine reason to reach out based on what you know.",
                     (unsigned)elapsed_hours);
    } else if (elapsed_days == 1) {
        n = snprintf(msg, sizeof(msg),
                     "PROACTIVE CHECK-IN: Last conversation: 1 day ago. Generate a natural, warm "
                     "check-in. Don't say \"I was thinking about you\" or \"just checking in\" — "
                     "find a specific, genuine reason to reach out based on what you know.");
    } else {
        n = snprintf(msg, sizeof(msg),
                     "PROACTIVE CHECK-IN: Last conversation: %u days ago. Generate a natural, "
                     "warm check-in. Don't say \"I was thinking about you\" or \"just checking "
                     "in\" — find a specific, genuine reason to reach out based on what you know.",
                     (unsigned)elapsed_days);
    }
    if (n <= 0 || (size_t)n >= sizeof(msg))
        return SC_OK;
    size_t msg_len = (size_t)n;

    sc_proactive_action_t *act = &out->actions[out->count];
    act->type = SC_PROACTIVE_CHECK_IN;
    act->message = sc_strndup(alloc, msg, msg_len);
    if (!act->message)
        return SC_ERR_OUT_OF_MEMORY;
    act->message_len = msg_len;
    act->priority = 0.85;
    out->count++;
    return SC_OK;
}

sc_error_t sc_proactive_check_reminder(sc_allocator_t *alloc, const char *contact_id,
                                       size_t contact_id_len, const char *interests,
                                       size_t interests_len, uint64_t now_ms,
                                       uint64_t last_reminder_ms, sc_proactive_result_t *out) {
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;
    if (!interests || interests_len == 0)
        return SC_OK;
    if (last_reminder_ms > 0) {
        if (now_ms <= last_reminder_ms)
            return SC_OK;
        uint64_t elapsed_ms = now_ms - last_reminder_ms;
        if (elapsed_ms < MS_PER_DAY)
            return SC_OK;
    }
    if (out->count >= SC_PROACTIVE_MAX_ACTIONS)
        return SC_OK;

    /* Count comma-separated interests */
    size_t token_count = 0;
    size_t i = 0;
    while (i < interests_len) {
        while (i < interests_len &&
               (interests[i] == ',' || interests[i] == ' ' || interests[i] == '\t'))
            i++;
        if (i >= interests_len)
            break;
        token_count++;
        while (i < interests_len && interests[i] != ',')
            i++;
    }
    if (token_count == 0)
        return SC_OK;

    /* Pick one randomly using now_ms as seed (deterministic LCG) */
    uint32_t seed = (uint32_t)(now_ms & 0xFFFFFFFFu);
    if (seed == 0)
        seed = 1;
    seed = seed * 1103515245u + 12345u;
    size_t idx = ((size_t)(seed >> 16) & 0x7FFFu) % token_count;

    /* Extract the idx-th token */
    const char *token_start = NULL;
    size_t token_len = 0;
    size_t t = 0;
    i = 0;
    while (i < interests_len && t <= idx) {
        while (i < interests_len &&
               (interests[i] == ',' || interests[i] == ' ' || interests[i] == '\t'))
            i++;
        if (i >= interests_len)
            break;
        if (t == idx) {
            token_start = interests + i;
            while (i < interests_len && interests[i] != ',')
                i++;
            token_len = (size_t)((interests + i) - token_start);
            while (token_len > 0 &&
                   (token_start[token_len - 1] == ' ' || token_start[token_len - 1] == '\t'))
                token_len--;
            break;
        }
        t++;
        while (i < interests_len && interests[i] != ',')
            i++;
    }
    if (!token_start || token_len == 0)
        return SC_OK;

    const char *contact_display = contact_id && contact_id_len > 0 ? contact_id : "this contact";
    size_t contact_display_len = contact_id && contact_id_len > 0 ? contact_id_len : 14;

    char msg[512];
    int n = snprintf(
        msg, sizeof(msg),
        "PROACTIVE REMINDER: %.*s is interested in %.*s. Imagine you just saw "
        "something related to %.*s. Write a SHORT, natural 'this reminded me of you' "
        "message. Examples: 'oh btw did you see [thing about interest]?' or 'lol this "
        "is so %.*s' or 'random but %.*s-related thought'. One sentence max. Reply SKIP "
        "if nothing natural.",
        (int)contact_display_len, contact_display, (int)token_len, token_start, (int)token_len,
        token_start, (int)token_len, token_start, (int)token_len, token_start);
    if (n <= 0 || (size_t)n >= sizeof(msg))
        return SC_OK;

    sc_proactive_action_t *act = &out->actions[out->count];
    act->type = SC_PROACTIVE_REMINDER;
    act->message = sc_strndup(alloc, msg, (size_t)n);
    if (!act->message)
        return SC_ERR_OUT_OF_MEMORY;
    act->message_len = (size_t)n;
    act->priority = 0.75;
    out->count++;
    return SC_OK;
}

uint32_t sc_proactive_backoff_hours(uint32_t consecutive_unanswered) {
    switch (consecutive_unanswered) {
    case 0:
        return 72u;
    case 1:
        return 144u;
    case 2:
        return 288u;
    default:
        return UINT32_MAX;
    }
}

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

    /* Session count milestones: 10, 25, 50, 100 — data-driven context */
    static const uint32_t MILESTONES[] = {10, 25, 50, 100};
    for (size_t i = 0; i < sizeof(MILESTONES) / sizeof(MILESTONES[0]); i++) {
        if (session_count == MILESTONES[i] && out->count < SC_PROACTIVE_MAX_ACTIONS) {
            char msg[320];
            int n = snprintf(msg, sizeof(msg),
                             "MILESTONE: This is conversation #%u together. Don't announce the "
                             "number — let it inform your warmth and familiarity naturally. You "
                             "know them well by now. Show it through specificity, not statements "
                             "about knowing them.",
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

    /* Morning hour (8-10) → MORNING_BRIEFING — data-driven from commitments */
    if (hour >= 8 && hour <= 10 && out->count < SC_PROACTIVE_MAX_ACTIONS) {
        char msg[384];
        size_t pos = 0;
        int w = snprintf(msg, sizeof(msg), "MORNING CONTEXT: ");
        if (w > 0 && (size_t)w < sizeof(msg))
            pos = (size_t)w;
        bool has_commitments = false;
        if (commitments && commitment_count > 0 && pos < sizeof(msg)) {
            for (size_t i = 0; i < commitment_count && pos < sizeof(msg) - 100; i++) {
                if (commitments[i].status != SC_COMMITMENT_ACTIVE || !commitments[i].summary)
                    continue;
                if (!has_commitments) {
                    w = snprintf(msg + pos, sizeof(msg) - pos, "Active commitments: ");
                    if (w > 0 && pos + (size_t)w < sizeof(msg)) {
                        pos += (size_t)w;
                        has_commitments = true;
                    }
                }
                size_t show = commitments[i].summary_len > 60 ? 60 : commitments[i].summary_len;
                w = snprintf(msg + pos, sizeof(msg) - pos, "%.*s%s", (int)show,
                             commitments[i].summary, (i + 1 < commitment_count) ? "; " : ". ");
                if (w > 0 && pos + (size_t)w < sizeof(msg))
                    pos += (size_t)w;
            }
        }
        w = snprintf(msg + pos, sizeof(msg) - pos,
                     "Generate a morning message that naturally references what's relevant "
                     "today. Don't use the word 'briefing' or list items — weave it into "
                     "natural conversation.");
        if (w > 0 && pos + (size_t)w < sizeof(msg))
            pos += (size_t)w;
        if (pos > 0 && pos < sizeof(msg)) {
            sc_proactive_action_t *act = &out->actions[out->count];
            act->type = SC_PROACTIVE_MORNING_BRIEFING;
            act->message = sc_strndup(alloc, msg, pos);
            if (act->message) {
                act->message_len = pos;
                act->priority = 0.7;
                out->count++;
            }
        }
    }

    /* COMMITMENT_FOLLOW_UP: up to 2 active commitments — data-driven context */
    if (commitments && commitment_count > 0 && out->count < SC_PROACTIVE_MAX_ACTIONS) {
        static const size_t MAX_COMMITMENT_FOLLOW_UPS = 2;
        size_t added = 0;
        for (size_t i = 0; i < commitment_count && added < MAX_COMMITMENT_FOLLOW_UPS; i++) {
            const sc_commitment_t *c = &commitments[i];
            if (c->status != SC_COMMITMENT_ACTIVE)
                continue;
            if (!c->summary || c->summary_len == 0)
                continue;
            if (out->count >= SC_PROACTIVE_MAX_ACTIONS)
                break;
            char msg[384];
            size_t summary_len = c->summary_len > 120 ? 120 : c->summary_len;
            int n;
            if (c->created_at && c->created_at[0] != '\0') {
                n = snprintf(msg, sizeof(msg),
                             "COMMITMENT FOLLOW-UP: They mentioned '%.*s' (created %s). "
                             "Generate a natural follow-up that shows genuine interest without "
                             "sounding like a reminder app.",
                             (int)summary_len, c->summary, c->created_at);
            } else {
                n = snprintf(msg, sizeof(msg),
                             "COMMITMENT FOLLOW-UP: They mentioned '%.*s'. Generate a natural "
                             "follow-up that shows genuine interest without sounding like a "
                             "reminder app.",
                             (int)summary_len, c->summary);
            }
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

    /* PATTERN_INSIGHT: up to 2 patterns with occurrence_count >= 5 — data-driven */
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
            char msg[320];
            size_t sublen = strlen(subject);
            if (sublen > 120)
                sublen = 120;
            int n = snprintf(msg, sizeof(msg),
                             "PATTERN INSIGHT: '%.*s' has come up %u times in your conversations. "
                             "Generate a message that naturally reflects this importance — don't "
                             "announce it as a statistic.",
                             (int)sublen, subject, (unsigned)pattern_counts[i]);
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

    /* Always: CHECK_IN (low priority) — contextual guidance */
    if (out->count < SC_PROACTIVE_MAX_ACTIONS) {
        static const char CHECK[] =
            "CHECK-IN: Consider how the user is feeling. Ask about progress on any ongoing goals. "
            "Use context from memory and relationship history.";
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

sc_error_t sc_proactive_check_events(sc_allocator_t *alloc, const sc_extracted_event_t *events,
                                     size_t event_count, sc_proactive_result_t *out) {
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;
    if (!events && event_count > 0)
        return SC_ERR_INVALID_ARGUMENT;

    size_t added = 0;
    for (size_t i = 0; i < event_count && added < SC_PROACTIVE_EVENT_FOLLOW_UP_CAP; i++) {
        const sc_extracted_event_t *ev = &events[i];
        if (ev->confidence < 0.5)
            continue;
        if (out->count >= SC_PROACTIVE_MAX_ACTIONS)
            break;

        const char *desc = ev->description ? ev->description : "something";
        size_t desc_len = ev->description ? ev->description_len : 8;
        if (desc_len > 80)
            desc_len = 80;
        const char *temporal = ev->temporal_ref ? ev->temporal_ref : "";
        size_t temporal_len = ev->temporal_ref ? ev->temporal_ref_len : 0;

        char msg[384];
        int n;
        if (temporal_len > 0) {
            n = snprintf(msg, sizeof(msg),
                         "EVENT FOLLOW-UP: Event: %.*s on %.*s. They mentioned this recently. "
                         "Generate a natural follow-up that shows genuine interest without "
                         "sounding like a reminder app.",
                         (int)desc_len, desc, (int)temporal_len, temporal);
        } else {
            n = snprintf(msg, sizeof(msg),
                         "EVENT FOLLOW-UP: Event: %.*s. They mentioned this recently. Generate a "
                         "natural follow-up that shows genuine interest without sounding like a "
                         "reminder app.",
                         (int)desc_len, desc);
        }
        if (n <= 0 || (size_t)n >= sizeof(msg))
            continue;

        sc_proactive_action_t *act = &out->actions[out->count];
        act->type = SC_PROACTIVE_CHECK_IN;
        act->message = sc_strndup(alloc, msg, (size_t)n);
        if (!act->message)
            return SC_ERR_OUT_OF_MEMORY;
        act->message_len = (size_t)n;
        act->priority = ev->confidence;
        out->count++;
        added++;
    }
    return SC_OK;
}

sc_error_t sc_proactive_build_context(const sc_proactive_result_t *result, sc_allocator_t *alloc,
                                      size_t max_actions, char **out, size_t *out_len) {
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

sc_error_t sc_proactive_build_starter(sc_allocator_t *alloc, sc_memory_t *memory,
                                      const char *contact_id, size_t contact_id_len, char **out,
                                      size_t *out_len) {
    if (!alloc || !out || !out_len)
        return SC_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    if (!memory || !memory->vtable || !memory->vtable->recall)
        return SC_ERR_INVALID_ARGUMENT;

    static const char QUERY[] = "recent topics activities interests";
    sc_memory_entry_t *entries = NULL;
    size_t count = 0;
    sc_error_t err = memory->vtable->recall(memory->ctx, alloc, QUERY, sizeof(QUERY) - 1, 5,
                                            contact_id, contact_id_len, &entries, &count);

    if (err != SC_OK || !entries || count == 0) {
        if (entries) {
            for (size_t i = 0; i < count; i++)
                sc_memory_entry_free_fields(alloc, &entries[i]);
            alloc->free(alloc->ctx, entries, count * sizeof(sc_memory_entry_t));
        }
        return SC_OK; /* caller falls back to generic check-in */
    }

    size_t cap = 256;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf) {
        for (size_t i = 0; i < count; i++)
            sc_memory_entry_free_fields(alloc, &entries[i]);
        alloc->free(alloc->ctx, entries, count * sizeof(sc_memory_entry_t));
        return SC_ERR_OUT_OF_MEMORY;
    }
    size_t len = 0;
    buf[0] = '\0';

    static const char HEADER[] =
        "Here are some natural conversation starting points based on what you know about them:\n";
    size_t hlen = sizeof(HEADER) - 1;
    while (len + hlen + 1 > cap) {
        size_t new_cap = cap * 2;
        char *nb = (char *)alloc->realloc(alloc->ctx, buf, cap, new_cap);
        if (!nb) {
            alloc->free(alloc->ctx, buf, cap);
            for (size_t i = 0; i < count; i++)
                sc_memory_entry_free_fields(alloc, &entries[i]);
            alloc->free(alloc->ctx, entries, count * sizeof(sc_memory_entry_t));
            return SC_ERR_OUT_OF_MEMORY;
        }
        buf = nb;
        cap = new_cap;
    }
    memcpy(buf, HEADER, hlen);
    len = hlen;

    for (size_t i = 0; i < count; i++) {
        if (!entries[i].content || entries[i].content_len == 0)
            continue;
        size_t show = entries[i].content_len > 100 ? 100 : entries[i].content_len;
        size_t need = len + 2 + show + 2; /* "- " + content + "\n" */
        while (need > cap) {
            size_t new_cap = cap * 2;
            char *nb = (char *)alloc->realloc(alloc->ctx, buf, cap, new_cap);
            if (!nb) {
                alloc->free(alloc->ctx, buf, cap);
                for (size_t j = 0; j < count; j++)
                    sc_memory_entry_free_fields(alloc, &entries[j]);
                alloc->free(alloc->ctx, entries, count * sizeof(sc_memory_entry_t));
                return SC_ERR_OUT_OF_MEMORY;
            }
            buf = nb;
            cap = new_cap;
        }
        buf[len++] = '-';
        buf[len++] = ' ';
        memcpy(buf + len, entries[i].content, show);
        len += show;
        buf[len++] = '\n';
    }

    static const char FOOTER[] =
        "Start with whatever feels most natural right now. Don't force it.\n";
    size_t flen = sizeof(FOOTER) - 1;
    while (len + flen + 1 > cap) {
        size_t new_cap = cap * 2;
        char *nb = (char *)alloc->realloc(alloc->ctx, buf, cap, new_cap);
        if (!nb) {
            alloc->free(alloc->ctx, buf, cap);
            for (size_t i = 0; i < count; i++)
                sc_memory_entry_free_fields(alloc, &entries[i]);
            alloc->free(alloc->ctx, entries, count * sizeof(sc_memory_entry_t));
            return SC_ERR_OUT_OF_MEMORY;
        }
        buf = nb;
        cap = new_cap;
    }
    memcpy(buf + len, FOOTER, flen);
    len += flen;
    buf[len] = '\0';

    for (size_t i = 0; i < count; i++)
        sc_memory_entry_free_fields(alloc, &entries[i]);
    alloc->free(alloc->ctx, entries, count * sizeof(sc_memory_entry_t));

    *out = buf;
    *out_len = len;
    return SC_OK;
}

void sc_proactive_result_deinit(sc_proactive_result_t *result, sc_allocator_t *alloc) {
    if (!result || !alloc)
        return;
    for (size_t i = 0; i < result->count; i++) {
        if (result->actions[i].message) {
            alloc->free(alloc->ctx, result->actions[i].message, result->actions[i].message_len + 1);
            result->actions[i].message = NULL;
        }
    }
    result->count = 0;
}
