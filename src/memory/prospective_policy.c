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
    for (const char *q = raw; end - q >= 8; q++) /* a leaked thinking block is not the answer */
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
    "about",   "after", "again",   "also",  "back",  "been",   "before",   "bring",    "call",
    "check",   "could", "does",    "done",  "email", "follow", "forget",   "from",     "going",
    "gonna",   "have",  "here",    "into",  "just",  "know",   "later",    "maybe",    "more",
    "morning", "much",  "need",    "next",  "night", "over",   "remember", "remind",   "send",
    "should",  "some",  "still",   "sure",  "tell",  "text",   "that",     "their",    "them",
    "then",    "there", "they",    "thing", "this",  "time",   "today",    "tomorrow", "tonight",
    "want",    "week",  "weekend", "were",  "what",  "when",   "where",    "which",    "will",
    "with",    "would", "your",
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

static bool pm_prefix_ci(const char *s, size_t len, const char *prefix);

/* Length of a relative-time phrase at s[0..n) ("today", "tonight",
 * "tomorrow", "yesterday", "this weekend", "next week", "in N days",
 * "N days ago") ending at a word boundary, else 0. */
static size_t pm_rel_time_len(const char *s, size_t n) {
    size_t l = 0;
    size_t d = pm_prefix_ci(s, n, "in ") ? 3 : 0;
    size_t i = d;
    while (i < n && isdigit((unsigned char)s[i]))
        i++;
    if (i > d && d == 3 && pm_prefix_ci(s + i, n - i, " days"))
        l = i + 5;
    else if (i > 0 && d == 0 && pm_prefix_ci(s + i, n - i, " days ago"))
        l = i + 9;
    for (const char *w = "this weekend\0next week\0yesterday\0tomorrow\0tonight\0today\0";
         l == 0 && *w; w += strlen(w) + 1)
        if (pm_prefix_ci(s, n, w))
            l = strlen(w);
    return l > 0 && (l == n || !(isalnum((unsigned char)s[l]) || s[l] == '\'')) ? l : 0;
}

/* Task 8 M4: a follow-up queued with its relative day baked in ("they
 * mentioned X (tomorrow)", "call mom tonight") is stale once due. Returns `a`
 * with ONE parenthesized, leading or trailing relative phrase cut (into tmp),
 * or `a` itself when there is none or nothing else would be left. */
static const char *pm_due_action(const char *a, char *tmp, size_t cap) {
    size_t n = strlen(a);
    for (size_t i = 0; i < n; i++) {
        char before = i > 0 ? a[i - 1] : ' ';
        size_t l = before == ' ' || before == '(' ? pm_rel_time_len(a + i, n - i) : 0;
        size_t s = i;
        size_t e = i + l;
        if (l > 0 && before == '(' && e < n && a[e] == ')') {
            s = i - 1; /* "(tomorrow)" */
            e++;
        } else if (l > 0 && i == 0 && e < n && (a[e] == ' ' || a[e] == ',')) {
            while (e < n && (a[e] == ' ' || a[e] == ','))
                e++; /* "Tomorrow, call …" */
        } else if (l == 0 || i == 0 || before != ' ' || e != n) {
            continue; /* not a trailing "… tonight" either */
        }
        while (s > 0 && (a[s - 1] == ' ' || a[s - 1] == ','))
            s--;
        if ((s == 0 && e == n) || n - (e - s) >= cap)
            return a;
        memcpy(tmp, a, s);
        memcpy(tmp + s, a + e, n - e + 1);
        return tmp;
    }
    return a;
}

/* Parsed from the end, since a topic may itself hold parentheses:
 * "<d>.<d>" <- "); confidence " <- <when> <- " (" <- <topic> <- prefix. */
size_t hu_prospective_frame_topic(const char *frame, size_t len, char *out, size_t cap) {
    static const char pre[] = "they mentioned ";
    static const char mid[] = "); confidence ";
    const size_t pl = sizeof(pre) - 1;
    const size_t ml = sizeof(mid) - 1;
    if (out && cap > 0)
        out[0] = '\0';
    if (!frame || !out || cap == 0 || len <= pl || memcmp(frame, pre, pl) != 0)
        return 0;
    size_t e = len;
    while (e > 0 && isdigit((unsigned char)frame[e - 1]))
        e--;
    if (e == len || e == 0 || frame[--e] != '.')
        return 0; /* no fractional digits, or no '.' before them */
    size_t dot = e;
    while (e > 0 && isdigit((unsigned char)frame[e - 1]))
        e--;
    if (e == dot || e < pl + ml || memcmp(frame + e - ml, mid, ml) != 0)
        return 0;
    size_t close = e - ml; /* the ')' ending <when> */
    size_t open = close;   /* one past the '(' starting it */
    while (open > pl && frame[open - 1] != '(')
        open--;
    size_t wl = close - open;
    if (open <= pl + 2 || frame[open - 1] != '(' || frame[open - 2] != ' ' || wl == 0 ||
        pm_rel_time_len(frame + open, wl) != wl)
        return 0;
    size_t tl = open - 2 - pl;
    if (tl >= cap)
        return 0;
    memcpy(out, frame + pl, tl);
    out[tl] = '\0';
    return tl;
}

static size_t pm_render_due_list(const char *const *actions, size_t n, char *buf, size_t cap,
                                 size_t *out_len) {
    size_t pos = 0;
    size_t rendered = 0;
    char tmp[512];
    for (size_t i = 0; i < n && i < HU_PROSPECTIVE_RENDER_CAP; i++) {
        int w =
            snprintf(buf + pos, cap - pos, "- %s\n", pm_due_action(actions[i], tmp, sizeof(tmp)));
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
        if (w <= 0 || pos + (size_t)w >= cap) {
            /* SOFT drops the separator it just wrote for an item that did not
             * fit. LEGACY keeps the pre-v2 bytes (a dangling " | "), because
             * HU_PROSPECTIVE=off must stay byte-identical. */
            if (i > 0 && style != HU_PM_RENDER_LEGACY)
                pos -= 3;
            break;
        }
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

/* F4 continued (fix round 1, I1): summaries are text AFTER "I'll "/
 * "I promise "/"remind me "/"my goal is " etc. (src/agent/commitment.c:
 * 25-30,87) — commitment.c strips only the ONE pattern it matched, so a
 * summary can still start with a second pattern's leftover ("I'll be
 * there" when "I promise " was the match) or carry mid-clause first-person
 * pronouns ("text you when I land"). Both must be neutralized before the
 * "ask if they..." frame is safe to use. */

static bool pm_prefix_ci(const char *s, size_t len, const char *prefix) {
    size_t pl = strlen(prefix);
    if (len < pl)
        return false;
    for (size_t i = 0; i < pl; i++)
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)prefix[i]))
            return false;
    return true;
}

static bool pm_boundary_before(const char *s, size_t i) {
    return i == 0 || !isalnum((unsigned char)s[i - 1]);
}

static bool pm_boundary_after(const char *s, size_t len, size_t i) {
    return i >= len || !isalnum((unsigned char)s[i]);
}

/* Case-insensitive whole-word match of `tok` at s[i..); false if it runs
 * past `len` or the next byte continues an alnum run. */
static bool pm_word_at_ci(const char *s, size_t len, size_t i, const char *tok) {
    size_t tl = strlen(tok);
    if (i + tl > len)
        return false;
    for (size_t k = 0; k < tl; k++)
        if (tolower((unsigned char)s[i + k]) != tolower((unsigned char)tok[k]))
            return false;
    return pm_boundary_after(s, len, i + tl);
}

/* 1 byte for a straight apostrophe, 3 for the curly U+2019 UTF-8 sequence,
 * 0 if s[i] is neither. */
static size_t pm_apostrophe_len(const char *s, size_t len, size_t i) {
    if (i < len && s[i] == '\'')
        return 1;
    if (i + 3 <= len && (unsigned char)s[i] == 0xE2 && (unsigned char)s[i + 1] == 0x80 &&
        (unsigned char)s[i + 2] == 0x99)
        return 3;
    return 0;
}

/* Appends `word` (length `wl`) to out[oi..), advances *i by `consume`; false
 * (out untouched past *oi) if it wouldn't fit. Collapses the repeated
 * bounds-check-then-memcpy shape shared by every pronoun swap below. */
static bool pm_emit(char *out, size_t out_cap, size_t *oi, const char *word, size_t wl, size_t *i,
                    size_t consume) {
    if (*oi + wl >= out_cap)
        return false;
    memcpy(out + *oi, word, wl);
    *oi += wl;
    *i += consume;
    return true;
}

#define PM_EMIT(word, consume) pm_emit(out, out_cap, &oi, word, sizeof(word) - 1, &i, (consume))

/* {word, replacement} table for the plain-possessive/objective swaps —
 * "I"/"I'm"/"I'll"/"I've"/"I'd" are handled separately in pm_third_person
 * (they hinge on the apostrophe, not a plain word match). Looping this
 * table, rather than one if-block per word, is what keeps the four swaps
 * from reading as four copies of the same six lines (clone-ratchet.md). */
static const struct {
    const char *word;
    const char *repl;
} k_pm_swaps[] = {
    {"myself", "themselves"},
    {"mine", "theirs"},
    {"my", "their"},
    {"me", "them"},
};

/* Tries every entry of k_pm_swaps at s[i..). Returns the number of input
 * bytes consumed on a match (>0), 0 for no match, -1 if `out` is full. */
static int pm_apply_pronoun_swap(const char *s, size_t len, size_t i, char *out, size_t out_cap,
                                 size_t *oi) {
    for (size_t k = 0; k < PM_COUNT(k_pm_swaps); k++) {
        if (!pm_word_at_ci(s, len, i, k_pm_swaps[k].word))
            continue;
        size_t wl = strlen(k_pm_swaps[k].word);
        size_t rl = strlen(k_pm_swaps[k].repl);
        size_t dummy_i = i;
        if (!pm_emit(out, out_cap, oi, k_pm_swaps[k].repl, rl, &dummy_i, wl))
            return -1;
        return (int)wl;
    }
    return 0;
}

/* Rewrites first-person pronouns to third-person, whole-word and
 * case-insensitive: I->they, me->them, my->their, mine->theirs,
 * myself->themselves, I'm->they're, I've->they've, I'll->they'll,
 * I'd->they'd (bare "I" already catches the 'll/'ve/'d forms via the
 * apostrophe boundary, so only "I'm" needs special-casing — "am" has no
 * third-person-plural contraction of its own). Returns false (leaving
 * *out_len untouched) when nothing survives, or a whole-word first-person
 * token is still present afterward — a safety net that fails toward
 * silence rather than risk a leaked pronoun. */
static bool pm_third_person(const char *s, size_t len, char *out, size_t out_cap, size_t *out_len) {
    size_t oi = 0;
    for (size_t i = 0; i < len;) {
        if (!pm_boundary_before(s, i)) {
            if (!pm_emit(out, out_cap, &oi, s + i, 1, &i, 1))
                return false;
            continue;
        }
        if (s[i] == 'I' || s[i] == 'i') {
            size_t ap = pm_apostrophe_len(s, len, i + 1);
            if (ap && i + 1 + ap < len && (s[i + 1 + ap] == 'm' || s[i + 1 + ap] == 'M') &&
                pm_boundary_after(s, len, i + 2 + ap)) {
                if (!PM_EMIT("they're", 2 + ap))
                    return false;
                continue;
            }
            if (pm_boundary_after(s, len, i + 1)) { /* bare "I" (also catches I'll/I've/I'd) */
                if (!PM_EMIT("they", 1))
                    return false;
                continue;
            }
        }
        int consumed = pm_apply_pronoun_swap(s, len, i, out, out_cap, &oi);
        if (consumed < 0)
            return false;
        if (consumed > 0) {
            i += (size_t)consumed;
            continue;
        }
        if (!pm_emit(out, out_cap, &oi, s + i, 1, &i, 1))
            return false;
    }
    if (oi == 0)
        return false;
    out[oi] = '\0';
    if (hu_str_contains_word_ci_n(out, oi, "i") || hu_str_contains_word_ci_n(out, oi, "me") ||
        hu_str_contains_word_ci_n(out, oi, "my") || hu_str_contains_word_ci_n(out, oi, "mine") ||
        hu_str_contains_word_ci_n(out, oi, "myself"))
        return false;
    *out_len = oi;
    return true;
}

#undef PM_EMIT

/* hu_buf_appendf clamps to cap-1 on truncation: treat that as "did not
 * fit" and refuse the partial write, rather than ever returning a
 * half-composed action. Shared so the check exists once, not once per
 * hu_prospective_commitment_action return path (clone-ratchet.md). */
static size_t pm_finish_or_empty(char *buf, size_t cap, size_t n) {
    if (n >= cap - 1) {
        buf[0] = '\0';
        return 0;
    }
    return n;
}

size_t hu_prospective_commitment_action(const char *summary, bool contact_committed, char *buf,
                                        size_t cap) {
    if (!buf || cap == 0)
        return 0;
    buf[0] = '\0';
    if (!summary || !summary[0])
        return 0;
    if (!contact_committed)
        return pm_finish_or_empty(buf, cap, hu_buf_appendf(buf, cap, 0, "%s", summary));
    size_t slen = strlen(summary);
    /* No named array here (dead-strip-ratchet.md): a function-local static
     * gets its own `_hu_commitment_action.k_leading`-shaped symbol, and
     * three inline checks over three short literals are simpler than a
     * lookup table anyway. (Production caller: hu_prospective_mirror_action
     * below, shared by src/memory/superhuman.c and the backfill.) */
    if (pm_prefix_ci(summary, slen, "I'll ")) {
        summary += 5;
        slen -= 5;
    } else if (pm_prefix_ci(summary, slen, "I will ")) {
        summary += 7;
        slen -= 7;
    } else if (pm_prefix_ci(summary, slen, "to ")) {
        summary += 3;
        slen -= 3;
    }
    if (slen == 0)
        return 0;
    char rewritten[512];
    size_t rlen = 0;
    if (!pm_third_person(summary, slen, rewritten, sizeof(rewritten), &rlen))
        return 0;
    return pm_finish_or_empty(
        buf, cap,
        hu_buf_appendf(buf, cap, 0, "ask if they still need to %.*s", (int)rlen, rewritten));
}

/* Ownership signal (ruling F4): no `who`, "" or "me" is the owner; anything
 * else is the contact. */
static bool pm_who_is_contact(const char *who, size_t who_len) {
    if (!who || who_len == 0)
        return false;
    return !(who_len == 2 && strncmp(who, "me", 2) == 0);
}

/* A dated-moment frame's topic is at most this long (the frame itself is
 * built in a 320-byte buffer); the live mirror has always used it. */
#define PM_MIRROR_TOPIC_CAP 512

hu_prospective_mirror_t hu_prospective_mirror_action(bool is_followup, const char *text,
                                                     size_t text_len, const char *who,
                                                     size_t who_len, char *buf, size_t cap,
                                                     const char **action, size_t *action_len) {
    *action = NULL;
    *action_len = 0;
    if (!pm_who_is_contact(who, who_len)) {
        /* A dated-moment situation frame mirrors as its topic: the frame's
         * relative day is stale once due and its wrapper words defeat the
         * done-after-evidence match. The ledger row keeps the frame. */
        size_t tl =
            is_followup
                ? hu_prospective_frame_topic(text, text_len, buf,
                                             cap < PM_MIRROR_TOPIC_CAP ? cap : PM_MIRROR_TOPIC_CAP)
                : 0;
        *action = tl > 0 ? buf : text;
        *action_len = tl > 0 ? tl : text_len;
        return tl > 0 ? HU_PM_MIRROR_TOPIC : HU_PM_MIRROR_VERBATIM;
    }
    /* Contact-owned: the rephraser wants a NUL-terminated string and
     * text/text_len is not guaranteed to be one, so copy into a bounded
     * local first. Too-long input is not safe to rephrase either. */
    char text_z[512];
    if (text_len >= sizeof(text_z))
        return HU_PM_MIRROR_SKIP_TOO_LONG;
    memcpy(text_z, text, text_len);
    text_z[text_len] = '\0';
    size_t al = hu_prospective_commitment_action(text_z, true, buf, cap);
    if (al == 0)
        return HU_PM_MIRROR_SKIP_UNSAFE;
    *action = buf;
    *action_len = al;
    return HU_PM_MIRROR_REPHRASED;
}

/* Calibrated 2026-10-01 against scripts/pm_bench_local.py --judge model (GLM
 * on :8741): the earlier wording ("not_now - still open, but this is not a
 * good moment") never told the model that a candidate reaches it BECAUSE its
 * moment arrived, so it missed 28 of 40 due/cued intentions (24 as not_now;
 * set_f1 0.444). The verdicts are listed settled-first so the model checks
 * the conversation for evidence before defaulting to fire, "time passing
 * alone settles nothing" stops it inventing an off-screen resolution, and
 * an unclear conversation still means not_now (spec §4.3: fail toward
 * silence). */
static const char k_pm_judge_system[] =
    "You check one reminder before it is shown to Seth while he texts a friend. You see the "
    "recent conversation (oldest first), one thing Seth meant to bring up, and why it came up "
    "now. Answer with exactly one word:\n"
    "already_resolved - the conversation above shows it is covered: they already said how it "
    "went, Seth already asked or answered it, or Seth already did it\n"
    "cancel - Seth or the other person called it off, or it no longer applies\n"
    "not_now - the conversation moved it to a later time, or it clearly has not happened yet\n"
    "fire - none of the above: it is still open\n"
    "Judge only from the conversation shown; time passing alone settles nothing. It came up "
    "because its moment arrived: they just mentioned its topic, or the time Seth meant to follow "
    "up has come. So a reminder that is still open should fire. If you cannot tell whether the "
    "conversation settles it, answer not_now.";

const char *hu_prospective_judge_system(size_t *len) {
    if (len)
        *len = sizeof(k_pm_judge_system) - 1;
    return k_pm_judge_system;
}

size_t hu_prospective_judge_user(char *buf, size_t cap, const char *history, size_t history_len,
                                 const char *action, const char *cue,
                                 hu_prospective_cue_kind_t kind, int64_t overdue_s,
                                 int64_t noted_age_s) {
    if (!buf || cap == 0)
        return 0;
    buf[0] = '\0';
    if (!action || !action[0])
        return 0;
    const char *h = history ? history : "";
    size_t hl = history ? history_len : 0;
    if (hl > 4000) { /* keep the most recent lines, starting at a line */
        const char *win_end = h + hl;
        const char *start = win_end - 4000;
        const char *nl = memchr(start, '\n', (size_t)(win_end - start));
        /* M1 (fix round 1): a newline with nothing after it (the only
         * newline in the window is the trailing byte) is not a usable line
         * boundary — trimming there throws the whole tail away. Keep the
         * raw tail instead of reporting "(none)". */
        size_t skip = (nl && nl + 1 < win_end) ? (size_t)(nl + 1 - h) : hl - 4000;
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
    else {
        /* A keyword cue has no clock of its own: without the note's age the
         * judge cannot tell "vet visit tomorrow" said yesterday from said
         * today, and answered not_now ("not happened yet") on cross-day cues. */
        pos = hu_buf_appendf(buf, cap, pos, "\nintention: %s\n", action);
        if (noted_age_s >= 0) {
            long long d = (long long)(noted_age_s / 86400);
            if (d == 0)
                pos = hu_buf_appendf(buf, cap, pos, "noted: today\n");
            else
                pos =
                    hu_buf_appendf(buf, cap, pos, "noted: %lld day%s ago\n", d, d == 1 ? "" : "s");
        }
        pos = hu_buf_appendf(buf, cap, pos, "cue: they just mentioned \"%s\"\n", cue ? cue : "");
    }
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
