/*
 * Proactive action system — milestones, morning briefing, check-in.
 */
#include "human/agent/proactive.h"
#include "human/core/string.h"
#include "human/memory.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef HU_ENABLE_SQLITE
#include "human/memory/superhuman.h"
#endif

#define HU_PROACTIVE_EVENT_FOLLOW_UP_CAP 3
#define MS_PER_HOUR                      (3600ULL * 1000ULL)
#define MS_PER_DAY                       (24ULL * MS_PER_HOUR)
#define HOURS_3_DAYS                     72u
#define HOURS_7_DAYS                     168u
#define HOURS_14_DAYS                    336u

hu_error_t hu_proactive_check_silence(hu_allocator_t *alloc, uint64_t last_contact_ms,
                                      uint64_t now_ms, const hu_silence_config_t *config,
                                      hu_proactive_result_t *out) {
    if (!alloc || !out || !config)
        return HU_ERR_INVALID_ARGUMENT;
    if (!config->enabled || last_contact_ms == 0)
        return HU_OK;
    if (now_ms <= last_contact_ms)
        return HU_OK; /* clock skew or equal timestamps — avoid uint64_t underflow */

    uint64_t elapsed_hours = (now_ms - last_contact_ms) / MS_PER_HOUR;
    if (elapsed_hours < config->threshold_hours)
        return HU_OK;
    if (out->count >= HU_PROACTIVE_MAX_ACTIONS)
        return HU_OK;

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
        return HU_OK;
    size_t msg_len = (size_t)n;

    hu_proactive_action_t *act = &out->actions[out->count];
    act->type = HU_PROACTIVE_CHECK_IN;
    act->message = hu_strndup(alloc, msg, msg_len);
    if (!act->message)
        return HU_ERR_OUT_OF_MEMORY;
    act->message_len = msg_len;
    act->priority = 0.85;
    out->count++;
    return HU_OK;
}

hu_error_t hu_proactive_check_reminder(hu_allocator_t *alloc, const char *contact_id,
                                       size_t contact_id_len, const char *interests,
                                       size_t interests_len, uint64_t now_ms,
                                       uint64_t last_reminder_ms, hu_proactive_result_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    if (!interests || interests_len == 0)
        return HU_OK;
    if (last_reminder_ms > 0) {
        if (now_ms <= last_reminder_ms)
            return HU_OK;
        uint64_t elapsed_ms = now_ms - last_reminder_ms;
        if (elapsed_ms < MS_PER_DAY)
            return HU_OK;
    }
    if (out->count >= HU_PROACTIVE_MAX_ACTIONS)
        return HU_OK;

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
        return HU_OK;

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
        return HU_OK;

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
        return HU_OK;

    hu_proactive_action_t *act = &out->actions[out->count];
    act->type = HU_PROACTIVE_REMINDER;
    act->message = hu_strndup(alloc, msg, (size_t)n);
    if (!act->message)
        return HU_ERR_OUT_OF_MEMORY;
    act->message_len = (size_t)n;
    act->priority = 0.75;
    out->count++;
    return HU_OK;
}

uint32_t hu_proactive_backoff_hours(uint32_t consecutive_unanswered) {
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
    const hu_proactive_action_t *pa = (const hu_proactive_action_t *)a;
    const hu_proactive_action_t *pb = (const hu_proactive_action_t *)b;
    if (pa->priority > pb->priority)
        return -1;
    if (pa->priority < pb->priority)
        return 1;
    return 0;
}

hu_error_t hu_proactive_check(hu_allocator_t *alloc, uint32_t session_count, uint8_t hour,
                              hu_proactive_result_t *out) {
    return hu_proactive_check_extended(alloc, session_count, hour, NULL, 0, NULL, NULL, 0, out);
}

hu_error_t hu_proactive_check_extended(hu_allocator_t *alloc, uint32_t session_count, uint8_t hour,
                                       const hu_commitment_t *commitments, size_t commitment_count,
                                       const char *const *pattern_subjects,
                                       const uint32_t *pattern_counts, size_t pattern_count,
                                       hu_proactive_result_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    /* Session count milestones: 10, 25, 50, 100 — data-driven context */
    static const uint32_t MILESTONES[] = {10, 25, 50, 100};
    for (size_t i = 0; i < sizeof(MILESTONES) / sizeof(MILESTONES[0]); i++) {
        if (session_count == MILESTONES[i] && out->count < HU_PROACTIVE_MAX_ACTIONS) {
            char msg[320];
            int n = snprintf(msg, sizeof(msg),
                             "MILESTONE: This is conversation #%u together. Don't announce the "
                             "number — let it inform your warmth and familiarity naturally. You "
                             "know them well by now. Show it through specificity, not statements "
                             "about knowing them.",
                             (unsigned)session_count);
            if (n > 0 && (size_t)n < sizeof(msg)) {
                hu_proactive_action_t *act = &out->actions[out->count];
                act->type = HU_PROACTIVE_MILESTONE;
                act->message = hu_strndup(alloc, msg, (size_t)n);
                if (!act->message)
                    return HU_ERR_OUT_OF_MEMORY;
                act->message_len = (size_t)n;
                act->priority = 0.9;
                out->count++;
            }
            break; /* only one milestone per check */
        }
    }

    /* Morning hour (8-10) → MORNING_BRIEFING — data-driven from commitments */
    if (hour >= 8 && hour <= 10 && out->count < HU_PROACTIVE_MAX_ACTIONS) {
        char msg[384];
        size_t pos = 0;
        int w = snprintf(msg, sizeof(msg), "MORNING CONTEXT: ");
        if (w > 0 && (size_t)w < sizeof(msg))
            pos = (size_t)w;
        bool has_commitments = false;
        if (commitments && commitment_count > 0 && pos < sizeof(msg)) {
            for (size_t i = 0; i < commitment_count && pos < sizeof(msg) - 100; i++) {
                if (commitments[i].status != HU_COMMITMENT_ACTIVE || !commitments[i].summary)
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
            hu_proactive_action_t *act = &out->actions[out->count];
            act->type = HU_PROACTIVE_MORNING_BRIEFING;
            act->message = hu_strndup(alloc, msg, pos);
            if (act->message) {
                act->message_len = pos;
                act->priority = 0.7;
                out->count++;
            }
        }
    }

    /* COMMITMENT_FOLLOW_UP: up to 2 active commitments — data-driven context */
    if (commitments && commitment_count > 0 && out->count < HU_PROACTIVE_MAX_ACTIONS) {
        static const size_t MAX_COMMITMENT_FOLLOW_UPS = 2;
        size_t added = 0;
        for (size_t i = 0; i < commitment_count && added < MAX_COMMITMENT_FOLLOW_UPS; i++) {
            const hu_commitment_t *c = &commitments[i];
            if (c->status != HU_COMMITMENT_ACTIVE)
                continue;
            if (!c->summary || c->summary_len == 0)
                continue;
            if (out->count >= HU_PROACTIVE_MAX_ACTIONS)
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
                hu_proactive_action_t *act = &out->actions[out->count];
                act->type = HU_PROACTIVE_COMMITMENT_FOLLOW_UP;
                act->message = hu_strndup(alloc, msg, (size_t)n);
                if (!act->message)
                    return HU_ERR_OUT_OF_MEMORY;
                act->message_len = (size_t)n;
                act->priority = 0.8;
                out->count++;
                added++;
            }
        }
    }

    /* PATTERN_INSIGHT: up to 2 patterns with occurrence_count >= 5 — data-driven */
    if (pattern_subjects && pattern_counts && pattern_count > 0 &&
        out->count < HU_PROACTIVE_MAX_ACTIONS) {
        static const size_t MAX_PATTERN_INSIGHTS = 2;
        static const uint32_t PATTERN_THRESHOLD = 5;
        size_t added = 0;
        for (size_t i = 0; i < pattern_count && added < MAX_PATTERN_INSIGHTS; i++) {
            if (pattern_counts[i] < PATTERN_THRESHOLD)
                continue;
            const char *subject = pattern_subjects[i];
            if (!subject)
                continue;
            if (out->count >= HU_PROACTIVE_MAX_ACTIONS)
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
                hu_proactive_action_t *act = &out->actions[out->count];
                act->type = HU_PROACTIVE_PATTERN_INSIGHT;
                act->message = hu_strndup(alloc, msg, (size_t)n);
                if (!act->message)
                    return HU_ERR_OUT_OF_MEMORY;
                act->message_len = (size_t)n;
                act->priority = 0.6;
                out->count++;
                added++;
            }
        }
    }

    /* Always: CHECK_IN (low priority) — contextual guidance */
    if (out->count < HU_PROACTIVE_MAX_ACTIONS) {
        static const char CHECK[] =
            "CHECK-IN: Consider how the user is feeling. Ask about progress on any ongoing goals. "
            "Use context from memory and relationship history.";
        hu_proactive_action_t *act = &out->actions[out->count];
        act->type = HU_PROACTIVE_CHECK_IN;
        act->message = hu_strndup(alloc, CHECK, sizeof(CHECK) - 1);
        if (!act->message)
            return HU_ERR_OUT_OF_MEMORY;
        act->message_len = sizeof(CHECK) - 1;
        act->priority = 0.2;
        out->count++;
    }

    return HU_OK;
}

hu_error_t hu_proactive_check_events(hu_allocator_t *alloc, const hu_extracted_event_t *events,
                                     size_t event_count, hu_proactive_result_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    if (!events && event_count > 0)
        return HU_ERR_INVALID_ARGUMENT;

    size_t added = 0;
    for (size_t i = 0; i < event_count && added < HU_PROACTIVE_EVENT_FOLLOW_UP_CAP; i++) {
        const hu_extracted_event_t *ev = &events[i];
        if (ev->confidence < 0.5)
            continue;
        if (out->count >= HU_PROACTIVE_MAX_ACTIONS)
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

        hu_proactive_action_t *act = &out->actions[out->count];
        act->type = HU_PROACTIVE_CHECK_IN;
        act->message = hu_strndup(alloc, msg, (size_t)n);
        if (!act->message)
            return HU_ERR_OUT_OF_MEMORY;
        act->message_len = (size_t)n;
        act->priority = ev->confidence;
        out->count++;
        added++;
    }
    return HU_OK;
}

hu_error_t hu_proactive_build_context(const hu_proactive_result_t *result, hu_allocator_t *alloc,
                                      size_t max_actions, char **out, size_t *out_len) {
    if (!result || !alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    if (result->count == 0)
        return HU_OK;

    /* Sort by priority descending (copy to avoid mutating) */
    hu_proactive_action_t sorted[HU_PROACTIVE_MAX_ACTIONS];
    size_t n = result->count < HU_PROACTIVE_MAX_ACTIONS ? result->count : HU_PROACTIVE_MAX_ACTIONS;
    memcpy(sorted, result->actions, n * sizeof(hu_proactive_action_t));
    qsort(sorted, n, sizeof(hu_proactive_action_t), compare_priority_desc);

    size_t take = n < max_actions ? n : max_actions;
    size_t cap = 64;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    size_t len = 0;
    buf[0] = '\0';

    static const char HEADER[] = "### Proactive Awareness\n\n";
    size_t hlen = sizeof(HEADER) - 1;
    while (len + hlen + 1 > cap) {
        size_t new_cap = cap * 2;
        char *nb = (char *)alloc->realloc(alloc->ctx, buf, cap, new_cap);
        if (!nb) {
            alloc->free(alloc->ctx, buf, cap);
            return HU_ERR_OUT_OF_MEMORY;
        }
        buf = nb;
        cap = new_cap;
    }
    memcpy(buf, HEADER, hlen);
    len = hlen;

    for (size_t i = 0; i < take; i++) {
        const hu_proactive_action_t *act = &sorted[i];
        if (!act->message || act->message_len == 0)
            continue;
        size_t need = len + 2 + act->message_len + 2; /* "- " + msg + "\n" */
        while (need > cap) {
            size_t new_cap = cap * 2;
            char *nb = (char *)alloc->realloc(alloc->ctx, buf, cap, new_cap);
            if (!nb) {
                alloc->free(alloc->ctx, buf, cap);
                return HU_ERR_OUT_OF_MEMORY;
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
    return HU_OK;
}

hu_error_t hu_proactive_build_starter(hu_allocator_t *alloc, hu_memory_t *memory,
                                      const char *contact_id, size_t contact_id_len, char **out,
                                      size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    if (!memory || !memory->vtable || !memory->vtable->recall)
        return HU_ERR_INVALID_ARGUMENT;

    static const char QUERY[] = "recent topics activities interests";
    hu_memory_entry_t *entries = NULL;
    size_t count = 0;
    hu_error_t err = memory->vtable->recall(memory->ctx, alloc, QUERY, sizeof(QUERY) - 1, 5,
                                            contact_id, contact_id_len, &entries, &count);

    if (err != HU_OK || !entries || count == 0) {
        if (entries) {
            for (size_t i = 0; i < count; i++)
                hu_memory_entry_free_fields(alloc, &entries[i]);
            alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));
        }
        return HU_OK; /* caller falls back to generic check-in */
    }

    size_t cap = 256;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf) {
        for (size_t i = 0; i < count; i++)
            hu_memory_entry_free_fields(alloc, &entries[i]);
        alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));
        return HU_ERR_OUT_OF_MEMORY;
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
                hu_memory_entry_free_fields(alloc, &entries[i]);
            alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));
            return HU_ERR_OUT_OF_MEMORY;
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
                    hu_memory_entry_free_fields(alloc, &entries[j]);
                alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));
                return HU_ERR_OUT_OF_MEMORY;
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
                hu_memory_entry_free_fields(alloc, &entries[i]);
            alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));
            return HU_ERR_OUT_OF_MEMORY;
        }
        buf = nb;
        cap = new_cap;
    }
    memcpy(buf + len, FOOTER, flen);
    len += flen;
    buf[len] = '\0';

    for (size_t i = 0; i < count; i++)
        hu_memory_entry_free_fields(alloc, &entries[i]);
    alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));

    *out = buf;
    *out_len = len;
    return HU_OK;
}

void hu_proactive_result_deinit(hu_proactive_result_t *result, hu_allocator_t *alloc) {
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

bool hu_proactive_check_important_dates(const hu_persona_t *persona, const char *contact_id,
                                        size_t contact_id_len, int month, int day,
                                        char *message_out, size_t msg_cap, char *type_out,
                                        size_t type_cap) {
    (void)contact_id;
    (void)contact_id_len;
    if (!persona || !message_out || msg_cap == 0)
        return false;
    if (!persona->important_dates || persona->important_dates_count == 0)
        return false;

    char expect[8];
    int n = snprintf(expect, sizeof(expect), "%02d-%02d", month, day);
    if (n <= 0 || (size_t)n >= sizeof(expect))
        return false;

    for (size_t i = 0; i < persona->important_dates_count; i++) {
        const hu_important_date_t *d = &persona->important_dates[i];
        if (strcmp(d->date, expect) != 0)
            continue;
        size_t msg_len = strnlen(d->message, sizeof(d->message) - 1);
        if (msg_len == 0)
            continue;
        if (msg_len >= msg_cap)
            msg_len = msg_cap - 1;
        memcpy(message_out, d->message, msg_len);
        message_out[msg_len] = '\0';
        if (type_out && type_cap > 0) {
            size_t type_len = strnlen(d->type, sizeof(d->type) - 1);
            if (type_len >= type_cap)
                type_len = type_cap - 1;
            memcpy(type_out, d->type, type_len);
            type_out[type_len] = '\0';
        }
        return true;
    }
    return false;
}

#ifdef HU_ENABLE_SQLITE
/* F30: Spontaneous curiosity — random genuine questions from micro-moments (10–15% per cycle) */
bool hu_proactive_check_curiosity(hu_allocator_t *alloc, hu_memory_t *memory,
                                  const char *contact_id, size_t contact_id_len, uint32_t seed,
                                  char *message_out, size_t msg_cap) {
    (void)alloc;
    if (!memory || !contact_id || contact_id_len == 0 || !message_out || msg_cap == 0)
        return false;

    /* 10–15% probability based on seed (use 12%). In tests, always proceed when data exists. */
    uint32_t s = seed;
    s = s * 1103515245u + 12345u;
#ifndef HU_IS_TEST
    if ((s >> 16u) % 100u >= 12u)
        return false;
#endif

    char *mm_json = NULL;
    size_t mm_len = 0;
    if (hu_superhuman_micro_moment_list(memory, alloc, contact_id, contact_id_len, 20, &mm_json,
                                         &mm_len) != HU_OK ||
        !mm_json || mm_len == 0)
        return false;

    /* Parse "Micro-moments:\n- fact | significance\n" lines to extract facts */
    const char *p = mm_json;
    const char *line_start = NULL;
    size_t fact_count = 0;
    const char *facts[32];
    size_t fact_lens[32];

    while (*p) {
        if (p[0] == '-' && p[1] == ' ') {
            p += 2;
            line_start = p;
            while (*p && *p != '\n' && *p != '|')
                p++;
            if (p > line_start && fact_count < 32) {
                size_t len = (size_t)(p - line_start);
                while (len > 0 && (line_start[len - 1] == ' ' || line_start[len - 1] == '\t'))
                    len--;
                if (len > 0) {
                    facts[fact_count] = line_start;
                    fact_lens[fact_count] = len;
                    fact_count++;
                }
            }
            while (*p && *p != '\n')
                p++;
            if (*p == '\n')
                p++;
            continue;
        }
        if (*p == '\n')
            p++;
        else
            p++;
    }

    if (fact_count == 0) {
        alloc->free(alloc->ctx, mm_json, mm_len);
        return false;
    }

    /* Pick one randomly */
    s = s * 1103515245u + 12345u;
    size_t idx = ((size_t)(s >> 16u) & 0x7FFFu) % fact_count;
    const char *fact = facts[idx];
    size_t fact_len = fact_lens[idx];

    /* Format: "random question — do you still [topic]?" or "hey whatever happened with [topic]?" */
    s = s * 1103515245u + 12345u;
    int n;
    if ((s >> 16u) % 2u == 0) {
        size_t show = fact_len > 80 ? 80 : fact_len;
        n = snprintf(message_out, msg_cap, "random question — do you still %.*s?",
                     (int)show, fact);
    } else {
        size_t show = fact_len > 80 ? 80 : fact_len;
        n = snprintf(message_out, msg_cap, "hey whatever happened with %.*s?",
                     (int)show, fact);
    }

    /* Free mm_json AFTER we've used the fact pointers */
    alloc->free(alloc->ctx, mm_json, mm_len);

    if (n <= 0 || (size_t)n >= msg_cap)
        return false;
    return true;
}

/* F31: Callback opportunities — reference previous conversations (30% per conversation start) */
bool hu_proactive_check_callbacks(hu_allocator_t *alloc, hu_memory_t *memory,
                                  const char *contact_id, size_t contact_id_len, uint32_t seed,
                                  char *message_out, size_t msg_cap) {
    if (!alloc || !memory || !contact_id || contact_id_len == 0 || !message_out || msg_cap == 0)
        return false;

    /* 30% probability based on seed. In tests, always proceed when data exists. */
    uint32_t s = seed;
    s = s * 1103515245u + 12345u;
#ifndef HU_IS_TEST
    if ((s >> 16u) % 100u >= 30u)
        return false;
#endif

    int64_t now_ts = (int64_t)time(NULL);

    /* Try delayed follow-ups first */
    hu_delayed_followup_t *followups = NULL;
    size_t followup_count = 0;
    if (hu_superhuman_delayed_followup_list_due(memory, alloc, now_ts, &followups,
                                                &followup_count) == HU_OK &&
        followups && followup_count > 0) {
        /* Filter by contact_id and pick one */
        for (size_t i = 0; i < followup_count; i++) {
            size_t slen = strnlen(followups[i].contact_id, sizeof(followups[i].contact_id) - 1);
            if (slen != contact_id_len ||
                memcmp(followups[i].contact_id, contact_id, contact_id_len) != 0)
                continue;
            size_t topic_len = strnlen(followups[i].topic, sizeof(followups[i].topic) - 1);
            if (topic_len == 0)
                continue;
            if (topic_len > 200)
                topic_len = 200;
            int n = snprintf(message_out, msg_cap,
                             "CALLBACK: Consider asking about: %.*s. Only if natural.",
                             (int)topic_len, followups[i].topic);
            hu_superhuman_delayed_followup_free(alloc, followups, followup_count);
            if (n > 0 && (size_t)n < msg_cap)
                return true;
            return false;
        }
        hu_superhuman_delayed_followup_free(alloc, followups, followup_count);
    }

    /* Fall back to commitments due */
    hu_superhuman_commitment_t *commitments = NULL;
    size_t commitment_count = 0;
    if (hu_superhuman_commitment_list_due(memory, alloc, now_ts, 10, &commitments,
                                          &commitment_count) == HU_OK &&
        commitments && commitment_count > 0) {
        for (size_t i = 0; i < commitment_count; i++) {
            size_t slen =
                strnlen(commitments[i].contact_id, sizeof(commitments[i].contact_id) - 1);
            if (slen != contact_id_len ||
                memcmp(commitments[i].contact_id, contact_id, contact_id_len) != 0)
                continue;
            size_t desc_len = strnlen(commitments[i].description,
                                      sizeof(commitments[i].description) - 1);
            if (desc_len == 0)
                continue;
            if (desc_len > 200)
                desc_len = 200;
            int n = snprintf(message_out, msg_cap,
                             "CALLBACK: Consider asking about: %.*s. Only if natural.",
                             (int)desc_len, commitments[i].description);
            hu_superhuman_commitment_free(alloc, commitments, commitment_count);
            if (n > 0 && (size_t)n < msg_cap)
                return true;
            return false;
        }
        hu_superhuman_commitment_free(alloc, commitments, commitment_count);
    }

    return false;
}
#else
bool hu_proactive_check_curiosity(hu_allocator_t *alloc, hu_memory_t *memory,
                                  const char *contact_id, size_t contact_id_len, uint32_t seed,
                                  char *message_out, size_t msg_cap) {
    (void)alloc;
    (void)memory;
    (void)contact_id;
    (void)contact_id_len;
    (void)seed;
    (void)message_out;
    (void)msg_cap;
    return false;
}

bool hu_proactive_check_callbacks(hu_allocator_t *alloc, hu_memory_t *memory,
                                  const char *contact_id, size_t contact_id_len, uint32_t seed,
                                  char *message_out, size_t msg_cap) {
    (void)alloc;
    (void)memory;
    (void)contact_id;
    (void)contact_id_len;
    (void)seed;
    (void)message_out;
    (void)msg_cap;
    return false;
}
#endif
