/*
 * src/memory/prospective_policy.c — the pure decisions of prospective memory
 * v2. Contract in include/human/memory/prospective_policy.h; spec
 * docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md §4.2-4.4.
 */
#include "human/memory/prospective_policy.h"

#include "human/core/string.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define PM_COUNT(a) (sizeof(a) / sizeof((a)[0]))

static const char *const k_pm_cue_kind[] = {"keyword", "time", "after_event"};
static const char *const k_pm_status[] = {"pending", "surfaced", "done", "canceled", "expired"};
static const char *const k_pm_source[] = {"extractor", "promise_keeper", "followup"};
static const char *const k_pm_verdict[] = {"fire", "already_resolved", "cancel", "not_now",
                                           "parse_fail"};

static const char *pm_name(const char *const *names, size_t n, int v) {
    return (v >= 0 && (size_t)v < n) ? names[v] : NULL;
}

static bool pm_lookup(const char *const *names, size_t n, const char *s, int *out) {
    if (!s)
        return false;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(names[i], s) == 0) {
            *out = (int)i;
            return true;
        }
    }
    return false;
}

const char *hu_prospective_cue_kind_str(hu_prospective_cue_kind_t k) {
    return pm_name(k_pm_cue_kind, PM_COUNT(k_pm_cue_kind), (int)k);
}

bool hu_prospective_cue_kind_parse(const char *s, hu_prospective_cue_kind_t *out) {
    int v = 0;
    if (!out || !pm_lookup(k_pm_cue_kind, PM_COUNT(k_pm_cue_kind), s, &v))
        return false;
    *out = (hu_prospective_cue_kind_t)v;
    return true;
}

const char *hu_prospective_status_str(hu_prospective_status_t s) {
    return pm_name(k_pm_status, PM_COUNT(k_pm_status), (int)s);
}

bool hu_prospective_status_parse(const char *s, hu_prospective_status_t *out) {
    int v = 0;
    if (!out || !pm_lookup(k_pm_status, PM_COUNT(k_pm_status), s, &v))
        return false;
    *out = (hu_prospective_status_t)v;
    return true;
}

const char *hu_prospective_outcome_str(hu_prospective_outcome_t o) {
    switch (o) {
    case HU_PM_OUTCOME_USED:
        return "used";
    case HU_PM_OUTCOME_IGNORED:
        return "ignored";
    case HU_PM_OUTCOME_SUPPRESSED:
        return "suppressed";
    default:
        return NULL;
    }
}

const char *hu_prospective_source_str(hu_prospective_source_t s) {
    return pm_name(k_pm_source, PM_COUNT(k_pm_source), (int)s);
}

const char *hu_prospective_verdict_str(hu_prospective_verdict_t v) {
    return pm_name(k_pm_verdict, PM_COUNT(k_pm_verdict), (int)v);
}

int hu_prospective_status_to_fired(hu_prospective_status_t s) {
    switch (s) {
    case HU_PM_DONE:
        return 1;
    case HU_PM_CANCELED:
        return 2;
    case HU_PM_EXPIRED:
        return 3;
    default:
        return 0;
    }
}

hu_gate_mode_t hu_prospective_gate_mode(void) {
    return hu_gate_mode_from_env("HU_PROSPECTIVE", HU_GATE_OFF);
}

hu_gate_mode_t hu_prospective_time_gate_mode(void) {
    return hu_gate_mode_from_env("HU_PROSPECTIVE_TIME", HU_GATE_OFF);
}

const char *hu_prospective_gate_banner(hu_gate_mode_t mode, bool time_gate) {
    if (time_gate) {
        switch (mode) {
        case HU_GATE_LIVE:
            return "prospective time LIVE (HU_PROSPECTIVE_TIME=live): due follow-ups come from "
                   "the typed store, at most 1 per contact per day";
        case HU_GATE_SHADOW:
            return "prospective time SHADOW (HU_PROSPECTIVE_TIME=shadow): legacy follow-ups "
                   "unchanged; would-send counts logged";
        default:
            return "prospective time v2 disabled (HU_PROSPECTIVE_TIME unset or off); set "
                   "HU_PROSPECTIVE_TIME=shadow|live to run the per-contact due set";
        }
    }
    switch (mode) {
    case HU_GATE_LIVE:
        return "prospective LIVE (HU_PROSPECTIVE=live): the fire-time check decides every cued "
               "intention";
    case HU_GATE_SHADOW:
        return "prospective SHADOW (HU_PROSPECTIVE=shadow): legacy directive unchanged; "
               "Filter+Decide counts logged";
    default:
        return "prospective v2 disabled (HU_PROSPECTIVE unset or off); set "
               "HU_PROSPECTIVE=shadow|live to run the fire-time check";
    }
}

hu_prospective_filter_t hu_prospective_filter(const hu_prospective_filter_facts_t *f) {
    if (!f || f->status != HU_PM_PENDING || f->is_group || f->is_self)
        return HU_PM_FILTER_SKIP;
    if (f->expires_at > 0 && f->expires_at <= f->now)
        return HU_PM_FILTER_EXPIRE;
    switch (f->cue_kind) {
    case HU_PM_CUE_KEYWORD:
        return f->keyword_in_text ? HU_PM_FILTER_ELIGIBLE : HU_PM_FILTER_SKIP;
    case HU_PM_CUE_TIME:
        if (f->due_at <= 0 || f->now < f->due_at)
            return HU_PM_FILTER_SKIP;
        if (f->now > f->due_at + f->grace_s)
            return HU_PM_FILTER_EXPIRE;
        return f->surfaced_today >= 1 ? HU_PM_FILTER_CAPPED : HU_PM_FILTER_ELIGIBLE;
    default:
        return HU_PM_FILTER_SKIP;
    }
}

/* One [a-z_] word, lowercased into w[cap]; returns the bytes consumed. */
static size_t pm_word(const char *p, const char *end, char *w, size_t cap) {
    size_t n = 0;
    while (p + n < end && (isalpha((unsigned char)p[n]) || p[n] == '_')) {
        if (n + 1 < cap)
            w[n] = (char)tolower((unsigned char)p[n]);
        n++;
    }
    w[n < cap ? n : cap - 1] = '\0';
    return n;
}

hu_prospective_verdict_t hu_prospective_parse_verdict(const char *raw, size_t len) {
    if (!raw || len == 0)
        return HU_PM_VERDICT_PARSE_FAIL;
    const char *p = raw;
    const char *end = raw + len;
    for (const char *q = raw; q + 8 <= end; q++) /* a leaked thinking block is not the answer */
        if (memcmp(q, "</think>", 8) == 0)
            p = q + 8;
    while (p < end && !isalpha((unsigned char)*p))
        p++;
    char w[48];
    size_t n = pm_word(p, end, w, sizeof(w));
    if (strcmp(w, "not") == 0 || strcmp(w, "already") == 0) {
        const char *q = p + n;
        while (q < end && (*q == ' ' || *q == '-'))
            q++;
        char w2[24];
        (void)pm_word(q, end, w2, sizeof(w2));
        size_t wl = strlen(w);
        size_t l2 = strlen(w2);
        if (wl + 1 + l2 < sizeof(w)) {
            w[wl] = '_';
            memcpy(w + wl + 1, w2, l2 + 1);
        }
    }
    if (strcmp(w, "fire") == 0)
        return HU_PM_VERDICT_FIRE;
    if (strcmp(w, "already_resolved") == 0 || strcmp(w, "resolved") == 0)
        return HU_PM_VERDICT_RESOLVED;
    if (strcmp(w, "cancel") == 0 || strcmp(w, "canceled") == 0 || strcmp(w, "cancelled") == 0)
        return HU_PM_VERDICT_CANCEL;
    if (strcmp(w, "not_now") == 0)
        return HU_PM_VERDICT_NOT_NOW;
    return HU_PM_VERDICT_PARSE_FAIL;
}

hu_prospective_action_t hu_prospective_decide(bool judge_ok, hu_prospective_verdict_t v) {
    if (!judge_ok)
        return HU_PM_ACT_KEEP_PENDING;
    switch (v) {
    case HU_PM_VERDICT_FIRE:
        return HU_PM_ACT_SURFACE;
    case HU_PM_VERDICT_RESOLVED:
        return HU_PM_ACT_MARK_DONE;
    case HU_PM_VERDICT_CANCEL:
        return HU_PM_ACT_MARK_CANCELED;
    default:
        return HU_PM_ACT_KEEP_PENDING;
    }
}

/* Words that never identify WHAT an intention is about. Only >= 4-char words
 * matter (shorter ones are skipped before this list is consulted). */
static const char *const k_pm_stop[] = {
    "about",  "again", "also",  "back",  "bring", "check",    "could",  "does",  "done",   "follow",
    "forget", "from",  "going", "gonna", "have",  "into",     "just",   "know",  "later",  "maybe",
    "more",   "much",  "need",  "next",  "over",  "remember", "remind", "send",  "should", "some",
    "still",  "sure",  "tell",  "that",  "their", "them",     "then",   "there", "they",   "thing",
    "this",   "time",  "want",  "what",  "when",  "will",     "with",   "would", "your",
};

static bool pm_is_stop(const char *w) {
    for (size_t i = 0; i < PM_COUNT(k_pm_stop); i++)
        if (strcmp(k_pm_stop[i], w) == 0)
            return true;
    return false;
}

size_t hu_prospective_key_terms(const char *action, char out[][HU_PROSPECTIVE_KEY_TERM_LEN],
                                size_t max) {
    size_t n = 0;
    if (!action || !out)
        return 0;
    const char *p = action;
    while (*p && n < max) {
        while (*p && !isalnum((unsigned char)*p))
            p++;
        char w[HU_PROSPECTIVE_KEY_TERM_LEN];
        size_t wl = 0;
        while (*p && (isalnum((unsigned char)*p) || *p == '\'')) {
            if (wl + 1 < sizeof(w))
                w[wl++] = (char)tolower((unsigned char)*p);
            p++;
        }
        w[wl] = '\0';
        if (wl >= 2 && w[wl - 2] == '\'' && w[wl - 1] == 's') {
            wl -= 2;
            w[wl] = '\0';
        }
        if (wl < 4 || strchr(w, '\'') || pm_is_stop(w))
            continue;
        bool dup = false;
        for (size_t i = 0; i < n && !dup; i++)
            dup = strcmp(out[i], w) == 0;
        if (!dup)
            memcpy(out[n++], w, wl + 1);
    }
    return n;
}

static bool pm_reply_has_term(const char *reply, size_t len, const char *term) {
    if (hu_str_contains_word_ci_n(reply, len, term))
        return true;
    char alt[HU_PROSPECTIVE_KEY_TERM_LEN + 2];
    size_t tl = strlen(term);
    if (tl > 4 && term[tl - 1] == 's') {
        memcpy(alt, term, tl - 1);
        alt[tl - 1] = '\0';
    } else {
        memcpy(alt, term, tl);
        alt[tl] = 's';
        alt[tl + 1] = '\0';
    }
    return hu_str_contains_word_ci_n(reply, len, alt);
}

bool hu_prospective_reply_uses_action(const char *action, const char *reply, size_t reply_len) {
    if (!reply || reply_len == 0)
        return false;
    char terms[HU_PROSPECTIVE_KEY_TERMS_MAX][HU_PROSPECTIVE_KEY_TERM_LEN];
    size_t n = hu_prospective_key_terms(action, terms, HU_PROSPECTIVE_KEY_TERMS_MAX);
    if (n == 0)
        return false;
    size_t hit = 0;
    for (size_t i = 0; i < n; i++)
        if (pm_reply_has_term(reply, reply_len, terms[i]))
            hit++;
    return hit > 0 && hit * 2 >= n;
}

hu_prospective_status_t hu_prospective_after_delivery_status(bool used, int attempts_before,
                                                             int max_attempts) {
    if (used)
        return HU_PM_DONE;
    return attempts_before + 1 >= max_attempts ? HU_PM_EXPIRED : HU_PM_PENDING;
}

static const char k_pm_legacy_prefix[] = "[PROSPECTIVE MEMORY: Remember to: ";
static const char k_pm_soft_prefix[] =
    "[PROSPECTIVE MEMORY: If it fits naturally, you could bring up: ";

static size_t pm_render_due_list(const char *const *actions, size_t n, char *buf, size_t cap,
                                 size_t *out_len) {
    size_t pos = 0;
    size_t rendered = 0;
    for (size_t i = 0; i < n && i < HU_PROSPECTIVE_RENDER_CAP; i++) {
        int w = snprintf(buf + pos, cap - pos, "- %s\n", actions[i]);
        if (w <= 0 || pos + (size_t)w >= cap)
            break;
        pos += (size_t)w;
        rendered++;
    }
    buf[pos] = '\0';
    *out_len = pos;
    return rendered;
}

size_t hu_prospective_render(hu_prospective_render_style_t style, const char *const *actions,
                             const char *const *cues, size_t n, char *buf, size_t cap,
                             size_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (!buf || cap < 2 || !out_len)
        return 0;
    buf[0] = '\0';
    if (!actions || n == 0 || (style == HU_PM_RENDER_LEGACY && !cues))
        return 0;
    if (style == HU_PM_RENDER_DUE_LIST)
        return pm_render_due_list(actions, n, buf, cap, out_len);
    /* LEGACY and SOFT: the pre-v2 loop, byte for byte (cap stands in for the
     * old sizeof(buf); `pos + 64 < cap` is its `pos < sizeof(buf) - 64`). */
    size_t pos = 0;
    int h = snprintf(buf, cap, "%s",
                     style == HU_PM_RENDER_LEGACY ? k_pm_legacy_prefix : k_pm_soft_prefix);
    if (h > 0 && (size_t)h < cap)
        pos = (size_t)h;
    size_t rendered = 0;
    for (size_t i = 0; i < n && i < HU_PROSPECTIVE_RENDER_CAP && pos + 64 < cap; i++) {
        if (i > 0) {
            memcpy(buf + pos, " | ", 3);
            pos += 3;
        }
        int w = style == HU_PM_RENDER_LEGACY
                    ? snprintf(buf + pos, cap - pos, "%s (triggered by: %s)", actions[i], cues[i])
                    : snprintf(buf + pos, cap - pos, "%s", actions[i]);
        if (w <= 0 || pos + (size_t)w >= cap)
            break;
        pos += (size_t)w;
        rendered++;
    }
    if (rendered == 0 || pos + 2 >= cap) {
        buf[0] = '\0';
        return 0;
    }
    buf[pos++] = ']';
    buf[pos] = '\0';
    *out_len = pos;
    return rendered;
}

/* F4 (controller ruling, spec §4.1 source=promise_keeper): a commitment the
 * CONTACT made must read as a question about them, never first person as
 * if Seth owed the follow-through. Only the frame changes; no tense/pronoun
 * rewrite of `summary` is attempted (see header contract). */
size_t hu_prospective_commitment_action(const char *summary, bool contact_committed, char *buf,
                                        size_t cap) {
    if (!buf || cap == 0)
        return 0;
    buf[0] = '\0';
    if (!summary || !summary[0])
        return 0;
    size_t n = contact_committed
                   ? hu_buf_appendf(buf, cap, 0, "ask if they still need to %s", summary)
                   : hu_buf_appendf(buf, cap, 0, "%s", summary);
    if (n >= cap - 1) { /* hu_buf_appendf clamps to cap-1 on truncation */
        buf[0] = '\0';
        return 0;
    }
    return n;
}

static const char k_pm_judge_system[] =
    "You check one reminder before it is shown to Seth while he texts a friend. You see the "
    "recent conversation (oldest first) and one thing Seth meant to bring up. Answer with "
    "exactly one word:\n"
    "fire - it is still open and bringing it up now would be natural\n"
    "already_resolved - the conversation shows it already happened, was answered, or no "
    "longer applies\n"
    "cancel - Seth or the other person called it off\n"
    "not_now - still open, but this is not a good moment\n"
    "If you are unsure, answer not_now.";

const char *hu_prospective_judge_system(size_t *len) {
    if (len)
        *len = sizeof(k_pm_judge_system) - 1;
    return k_pm_judge_system;
}

size_t hu_prospective_judge_user(char *buf, size_t cap, const char *history, size_t history_len,
                                 const char *action, const char *cue,
                                 hu_prospective_cue_kind_t kind, int64_t overdue_s) {
    if (!buf || cap == 0)
        return 0;
    buf[0] = '\0';
    if (!action || !action[0])
        return 0;
    const char *h = history ? history : "";
    size_t hl = history ? history_len : 0;
    if (hl > 4000) { /* keep the most recent lines, starting at a line */
        const char *start = h + hl - 4000;
        const char *nl = memchr(start, '\n', (size_t)(h + hl - start));
        size_t skip = nl ? (size_t)(nl + 1 - h) : hl - 4000;
        h += skip;
        hl -= skip;
    }
    size_t pos =
        hl ? hu_buf_appendf(buf, cap, 0, "conversation (oldest first):\n%.*s\n", (int)hl, h)
           : hu_buf_appendf(buf, cap, 0, "conversation (oldest first):\n(none)\n");
    if (kind == HU_PM_CUE_TIME)
        pos = hu_buf_appendf(buf, cap, pos,
                             "\nintention: %s\ncue: it came due %lld day(s) ago; nobody has "
                             "brought it up yet\n",
                             action, (long long)(overdue_s > 0 ? overdue_s / 86400 : 0));
    else
        pos = hu_buf_appendf(buf, cap, pos, "\nintention: %s\ncue: they just mentioned \"%s\"\n",
                             action, cue ? cue : "");
    pos = hu_buf_appendf(buf, cap, pos, "answer:");
    if (pos >= cap - 1) { /* hu_buf_appendf clamps to cap-1 on truncation */
        buf[0] = '\0';
        return 0;
    }
    return pos;
}

int64_t hu_prospective_local_day_start(int64_t now) {
    time_t t = (time_t)now;
    struct tm tmv;
    if (!localtime_r(&t, &tmv))
        return now - (now % 86400);
    tmv.tm_hour = 0;
    tmv.tm_min = 0;
    tmv.tm_sec = 0;
    tmv.tm_isdst = -1;
    time_t m = mktime(&tmv);
    return m == (time_t)-1 ? now - (now % 86400) : (int64_t)m;
}
