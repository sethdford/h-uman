/* src/daemon/daemon_commitment_guard.c — HU_COMMITMENT_GUARD.
 *
 * Never let the twin commit Seth to a plan, a time, money, a favour or a
 * sensitive decision he can't or wouldn't make. Contract and decision table:
 * include/human/daemon/commitment_guard.h. Promotion measurement and rollback:
 * docs/guides/commitment-guard.md. */
#include "human/daemon/commitment_guard.h"

#include "human/agent.h"
#include "human/core/json.h"
#include "human/core/log.h"
#include "human/core/string.h"
#include "human/daemon/owner_notify.h"
#include "human/daemon/proposer_context.h"
#include "human/persona.h"
#include "human/providers/compatible.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HU_COMMIT_SCAN_MAX    2048
#define HU_COMMIT_REWRITE_MAX 1000
#define HU_COMMIT_PURPOSE     "commitment_check"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

hu_gate_mode_t hu_commitment_guard_mode(void) {
    return hu_gate_mode_from_env("HU_COMMITMENT_GUARD", HU_GATE_OFF);
}

const char *hu_commit_kind_name(hu_commit_kind_t k) {
    static const char *const names[] = {"none", "plan", "money", "favour", "sensitive"};
    return (unsigned)k < ARRAY_LEN(names) ? names[k] : "none";
}

const char *hu_commit_decision_name(hu_commit_decision_t d) {
    static const char *const names[] = {"skip", "allow", "rewrite_conflict", "hold",
                                        "detect_failed"};
    return (unsigned)d < ARRAY_LEN(names) ? names[d] : "skip";
}

/* ── prefilter ────────────────────────────────────────────────────────────
 * RECALL-oriented: it decides whether a reply is worth one local model call,
 * never what happens to it. A false positive costs ~one detector call; a false
 * negative lets a commitment through unchecked, so the lists lean wide. The
 * shadow audit (every HU_COMMIT_AUDIT_EVERY-th miss) measures what it misses. */

static const char *const k_time_cues[] = {
    "monday",    "tuesday",   "wednesday", "thursday",  "friday",      "saturday",  "sunday",
    "tues",      "weds",      "thurs",     "fri",       "sat",         "today",     "tonight",
    "tonite",    "tomorrow",  "tmrw",      "tmr",       "tmw",         "tomoro",    "weekend",
    "next week", "this week", "morning",   "afternoon", "evening",     "noon",      "midnight",
    "lunch",     "dinner",    "brunch",    "breakfast", "drinks",      "january",   "february",
    "april",     "june",      "july",      "august",    "september",   "october",   "november",
    "december",  "be there",  "see you",   "see u",     "pick you up", "pick u up", "meet",
};

static const char *const k_commit_verbs[] = {
    "i'll",          "ill",          "i will",   "i can",       "i could",  "i'm down",
    "im down",       "i am down",    "down to",  "count me in", "i'm in",   "im in",
    "works for me",  "that works",   "works",    "sounds good", "deal",     "promise",
    "for sure",      "i got you",    "i gotchu", "on it",       "go ahead", "you can tell",
    "i'm fine with", "im fine with", "i agree",  "let's",       "lets",     "help you",
    "help u",        "bring",        "cover",    "send you",    "send u",
};

static const char *const k_money_cues[] = {
    "venmo", "zelle", "paypal", "cashapp", "cash app", "lend",    "loan", "borrow",
    "pay",   "owe",   "money",  "cash",    "bucks",    "dollars", "rent",
};

static const char *const k_request_cues[] = {
    "can you",  "could you", "would you",  "will you", "can u",       "could u",
    "would u",  "will u",    "wanna",      "want to",  "do you mind", "are you free",
    "you free", "u free",    "you around", "u around", "you down",    "u down",
    "come",     "join",      "help me",    "favor",    "favour",      "ride",
    "should i", "is it ok",  "is it okay", "can i",    "do you want", "you coming",
    "u coming", "hang",      "plans",
};

static const char *const k_affirmatives[] = {
    "yes",       "yeah", "yea",        "ya", "yep",        "yup",        "sure",
    "ok",        "okay", "k",          "kk", "definitely", "absolutely", "bet",
    "of course", "ofc",  "no problem", "np", "totally",    "always",     "100",
};

/* Lowercase; typographic apostrophe (U+2019) -> '. */
static size_t commit_normalize(const char *in, size_t len, char *out, size_t cap) {
    size_t o = 0;
    if (len > HU_COMMIT_SCAN_MAX)
        len = HU_COMMIT_SCAN_MAX;
    for (size_t i = 0; i < len && o + 1 < cap; i++) {
        if (i + 2 < len && memcmp(in + i, "\xE2\x80\x99", 3) == 0) {
            out[o++] = '\'';
            i += 2;
        } else {
            out[o++] = (char)tolower((unsigned char)in[i]);
        }
    }
    out[o] = '\0';
    return o;
}

static bool any_word(const char *s, size_t n, const char *const *list, size_t count) {
    for (size_t i = 0; i < count; i++)
        if (hu_str_contains_word_ci_n(s, n, list[i]))
            return true;
    return false;
}

/* Clock times and numeric dates: "7pm", "7 pm", "at 7", "@7", "by 8", "7:30",
 * "10/3", and "$40". */
static bool has_clock_or_amount(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '$' && i + 1 < n && isdigit((unsigned char)s[i + 1]))
            return true;
        if (!isdigit((unsigned char)s[i]) || (i > 0 && isdigit((unsigned char)s[i - 1])))
            continue;
        size_t j = i;
        while (j < n && isdigit((unsigned char)s[j]))
            j++;
        if (j - i > 2)
            continue;
        if (j + 2 < n && (s[j] == ':' || s[j] == '/') && isdigit((unsigned char)s[j + 1]))
            return true;
        size_t k = (j < n && s[j] == ' ') ? j + 1 : j;
        if (k + 1 < n && (s[k] == 'a' || s[k] == 'p') && s[k + 1] == 'm')
            return true;
        if (i >= 1 && s[i - 1] == '@')
            return true;
        static const char *const leads[] = {"at ", "by ", "around ", "til ", "till ", "until "};
        for (size_t l = 0; l < ARRAY_LEN(leads); l++) {
            size_t ll = strlen(leads[l]);
            if (i >= ll && memcmp(s + i - ll, leads[l], ll) == 0 &&
                (i == ll || !isalpha((unsigned char)s[i - ll - 1])))
                return true;
        }
    }
    return false;
}

bool hu_commitment_prefilter(const char *draft, size_t draft_len, const char *inbound,
                             size_t inbound_len) {
    if (!draft || draft_len == 0)
        return false;
    char d[HU_COMMIT_SCAN_MAX + 1];
    size_t dn = commit_normalize(draft, draft_len, d, sizeof(d));
    if (any_word(d, dn, k_time_cues, ARRAY_LEN(k_time_cues)) ||
        any_word(d, dn, k_commit_verbs, ARRAY_LEN(k_commit_verbs)) ||
        any_word(d, dn, k_money_cues, ARRAY_LEN(k_money_cues)) || has_clock_or_amount(d, dn))
        return true;
    if (!inbound || inbound_len == 0 || !any_word(d, dn, k_affirmatives, ARRAY_LEN(k_affirmatives)))
        return false;
    char in[HU_COMMIT_SCAN_MAX + 1];
    size_t inn = commit_normalize(inbound, inbound_len, in, sizeof(in));
    return any_word(in, inn, k_request_cues, ARRAY_LEN(k_request_cues)) ||
           any_word(in, inn, k_time_cues, ARRAY_LEN(k_time_cues)) ||
           any_word(in, inn, k_money_cues, ARRAY_LEN(k_money_cues)) || has_clock_or_amount(in, inn);
}

/* Offers to move money. Deterministic floor for when the detector is down. */
static const char *const k_money_floor[] = {
    "venmo you",   "venmo u",      "zelle you", "zelle u", "paypal you", "paypal u",
    "cashapp you", "cash app you", "lend you",  "lend u",  "loan you",   "loan u",
    "pay you",     "pay u",        "spot you",  "spot u",  "cover you",  "cover u",
};

bool hu_commitment_money_floor(const char *draft, size_t draft_len) {
    if (!draft || draft_len == 0)
        return false;
    char d[HU_COMMIT_SCAN_MAX + 1];
    size_t dn = commit_normalize(draft, draft_len, d, sizeof(d));
    return any_word(d, dn, k_money_floor, ARRAY_LEN(k_money_floor));
}

/* ── parse ─────────────────────────────────────────────────────────────── */

static int64_t local_epoch(int y, int mo, int d, int h, int mi) {
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_isdst = -1;
    time_t e = mktime(&t);
    return e == (time_t)-1 ? -1 : (int64_t)e;
}

/* "YYYY-MM-DDTHH:MM" (or a space) -> [t, t + 2h); "YYYY-MM-DD" -> [09:00, 22:00). */
static bool parse_when(const char *w, int64_t *start, int64_t *end) {
    int y, mo, d, h, mi;
    char sep;
    if (sscanf(w, "%4d-%2d-%2d%c%2d:%2d", &y, &mo, &d, &sep, &h, &mi) == 6 &&
        (sep == 'T' || sep == ' ') && h >= 0 && h < 24 && mi >= 0 && mi < 60) {
        *start = local_epoch(y, mo, d, h, mi);
        *end = *start + HU_COMMIT_TIMED_WINDOW_S;
    } else if (sscanf(w, "%4d-%2d-%2d", &y, &mo, &d) == 3) {
        *start = local_epoch(y, mo, d, 9, 0);
        *end = local_epoch(y, mo, d, 22, 0);
    } else {
        return false;
    }
    return y >= 2000 && mo >= 1 && mo <= 12 && d >= 1 && d <= 31 && *start > 0 && *end > *start;
}

bool hu_commitment_parse(const char *raw, size_t raw_len, hu_commit_detection_t *out) {
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    if (!raw || raw_len == 0)
        return false;
    const char *open = memchr(raw, '{', raw_len);
    const char *close = NULL;
    for (size_t i = raw_len; i > 0; i--) {
        if (raw[i - 1] == '}') {
            close = raw + i - 1;
            break;
        }
    }
    if (!open || !close || close < open)
        return false;
    hu_allocator_t a = hu_system_allocator();
    hu_json_value_t *root = NULL;
    if (hu_json_parse(&a, open, (size_t)(close - open) + 1, &root) != HU_OK || !root)
        return false;
    bool ok = false;
    const char *kind = root->type == HU_JSON_OBJECT ? hu_json_get_string(root, "kind") : NULL;
    for (int k = HU_COMMIT_NONE; kind && k <= HU_COMMIT_SENSITIVE; k++) {
        if (strcmp(kind, hu_commit_kind_name((hu_commit_kind_t)k)) == 0 ||
            (k == HU_COMMIT_FAVOUR && strcmp(kind, "favor") == 0)) {
            out->kind = (hu_commit_kind_t)k;
            ok = true;
        }
    }
    if (ok) {
        double c = hu_json_get_number(root, "confidence", 0.0);
        out->confidence = c < 0.0 ? 0.0 : c > 1.0 ? 1.0 : c;
        const char *stakes = hu_json_get_string(root, "stakes");
        /* Missing or odd stakes fails toward caution; money and sensitive
         * matters are never low-stakes. */
        out->high_stakes = !(stakes && strcmp(stakes, "low") == 0) ||
                           out->kind == HU_COMMIT_MONEY || out->kind == HU_COMMIT_SENSITIVE;
        const char *when = hu_json_get_string(root, "when");
        if (when && when[0])
            out->has_when = parse_when(when, &out->when_start, &out->when_end);
    }
    hu_json_free(&a, root);
    return ok;
}

/* ── decide ────────────────────────────────────────────────────────────── */

hu_commit_decision_t hu_commitment_decide(const hu_commit_detection_t *d, hu_calendar_state_t cal) {
    if (!d || d->kind == HU_COMMIT_NONE || d->confidence < HU_COMMIT_MIN_CONFIDENCE)
        return HU_COMMIT_ALLOW;
    switch (d->kind) {
    case HU_COMMIT_PLAN:
        if (cal == HU_CAL_BUSY)
            return HU_COMMIT_REWRITE_CONFLICT;
        return cal == HU_CAL_FREE ? HU_COMMIT_ALLOW : HU_COMMIT_HOLD;
    case HU_COMMIT_FAVOUR:
        if (d->high_stakes)
            return HU_COMMIT_HOLD;
        return cal == HU_CAL_BUSY ? HU_COMMIT_REWRITE_CONFLICT : HU_COMMIT_ALLOW;
    default: /* money, sensitive */
        return HU_COMMIT_HOLD;
    }
}

/* ── local model calls ─────────────────────────────────────────────────── */

static const char k_detect_system[] =
    "You check a text message Seth is about to send, to see whether it commits Seth to "
    "something. Reply with ONLY one JSON object and nothing else:\n"
    "{\"kind\":\"none|plan|money|favour|sensitive\",\"stakes\":\"low|high\","
    "\"when\":\"YYYY-MM-DDTHH:MM\" or \"YYYY-MM-DD\" or null,\"confidence\":0.0-1.0}\n"
    "kind:\n"
    "- plan: Seth accepts or proposes a specific plan, meeting or time (\"yeah saturday "
    "works\", \"i'll be there at 7\").\n"
    "- money: Seth agrees to give, lend, pay or send money or things.\n"
    "- favour: Seth promises to do something for them. stakes high if it costs real time, "
    "money or effort (helping someone move, an airport ride); low if trivial (sending a "
    "link).\n"
    "- sensitive: Seth makes a decision or a disclosure he would want to make himself "
    "(health, relationships, work, legal, secrets, private things about other people).\n"
    "- none: chatting, asking, declining, maybe, or a vague \"sometime\".\n"
    "when: the date and time the commitment is for, resolved against Now; null if none.\n"
    "Judge Seth's draft in the context of their message.";

static const char k_rewrite_system[] =
    "Rewrite the text message Seth is about to send so that it does NOT commit him to "
    "anything: no yes to the plan or time, no money, no favour, no decision. Keep his "
    "voice, length, casing and punctuation, and keep it warm and natural; it is fine to "
    "say he needs to check first. Reply with ONLY the rewritten message.";

static void clip_into(char *buf, size_t cap, size_t *pos, const char *label, const char *s,
                      size_t n) {
    if (n > HU_COMMIT_TEXT_MAX)
        n = HU_COMMIT_TEXT_MAX;
    *pos = hu_buf_appendf(buf, cap, *pos, "%s%.*s", label, (int)n, s ? s : "");
}

/* One local call under X-HU-Purpose: commitment_check. *out is allocated with
 * io->alloc (size *out_len + 1) on success. */
static hu_error_t local_call(const hu_commitment_guard_io_t *io, const char *sys, size_t sys_len,
                             const char *msg, size_t msg_len, char **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    if (!io->local.vtable || !io->local.vtable->chat_with_system)
        return HU_ERR_NOT_SUPPORTED;
    const char *prev = hu_compatible_purpose_set(HU_COMMIT_PURPOSE);
    hu_error_t err =
        io->local.vtable->chat_with_system(io->local.ctx, io->alloc, sys, sys_len, msg, msg_len,
                                           io->model, io->model_len, 0.0, out, out_len);
    (void)hu_compatible_purpose_set(prev);
    if (err != HU_OK && *out) {
        io->alloc->free(io->alloc->ctx, *out, *out_len + 1);
        *out = NULL;
    }
    return err;
}

static bool detect(const hu_commitment_guard_io_t *io, const char *inbound, size_t inbound_len,
                   const char *draft, size_t draft_len, hu_commit_detection_t *out) {
    char msg[2 * HU_COMMIT_TEXT_MAX + 256];
    size_t pos = 0;
    char now[48] = "";
    time_t t = (time_t)io->now;
    struct tm lt;
    if (localtime_r(&t, &lt))
        strftime(now, sizeof(now), "%A %Y-%m-%d %H:%M", &lt);
    pos = hu_buf_appendf(msg, sizeof(msg), pos, "Now: %s (local time)\n\n", now);
    clip_into(msg, sizeof(msg), &pos, "Their message:\n", inbound, inbound_len);
    clip_into(msg, sizeof(msg), &pos, "\n\nSeth's draft reply:\n", draft, draft_len);
    char *raw = NULL;
    size_t raw_len = 0;
    if (local_call(io, k_detect_system, sizeof(k_detect_system) - 1, msg, pos, &raw, &raw_len) !=
        HU_OK)
        return false;
    bool ok = hu_commitment_parse(raw, raw_len, out);
    io->alloc->free(io->alloc->ctx, raw, raw_len + 1);
    return ok;
}

/* Trimmed, unquoted rewrite, or NULL. */
static char *rewrite(const hu_commitment_guard_io_t *io, hu_commit_decision_t why,
                     const char *inbound, size_t inbound_len, const char *draft, size_t draft_len,
                     size_t *out_len) {
    char msg[2 * HU_COMMIT_TEXT_MAX + 256];
    size_t pos = 0;
    clip_into(msg, sizeof(msg), &pos, "Their message:\n", inbound, inbound_len);
    clip_into(msg, sizeof(msg), &pos, "\n\nSeth's draft:\n", draft, draft_len);
    pos = hu_buf_appendf(msg, sizeof(msg), pos, "\n\nWhy: %s",
                         why == HU_COMMIT_REWRITE_CONFLICT
                             ? "his calendar shows he is busy then."
                             : "this is his call to make himself, later.");
    char *raw = NULL;
    size_t raw_len = 0;
    if (local_call(io, k_rewrite_system, sizeof(k_rewrite_system) - 1, msg, pos, &raw, &raw_len) !=
            HU_OK ||
        !raw)
        return NULL;
    size_t b = 0, e = raw_len;
    while (b < e && isspace((unsigned char)raw[b]))
        b++;
    while (e > b && isspace((unsigned char)raw[e - 1]))
        e--;
    if (e - b >= 2 && raw[b] == '"' && raw[e - 1] == '"') {
        b++;
        e--;
    }
    char *txt =
        (e > b && e - b <= HU_COMMIT_REWRITE_MAX) ? hu_strndup(io->alloc, raw + b, e - b) : NULL;
    io->alloc->free(io->alloc->ctx, raw, raw_len + 1);
    *out_len = txt ? strlen(txt) : 0;
    return txt;
}

static void notify_owner(const hu_commitment_guard_io_t *io,
                         const hu_commitment_guard_result_t *r) {
    const char *who = (io->contact_name && io->contact_name[0]) ? io->contact_name : "a contact";
    const char *kind = hu_commit_kind_name(r->detection.kind);
    char body[256];
    if (r->suppressed)
        snprintf(body, sizeof(body),
                 "h-uman did not send its reply to %s: it would have committed you (%s). "
                 "Answer them yourself.",
                 who, kind);
    else
        snprintf(body, sizeof(body),
                 "h-uman held back a %s commitment in its reply to %s%s. Check the thread and "
                 "answer yourself.",
                 kind, who,
                 r->decision == HU_COMMIT_REWRITE_CONFLICT ? " (your calendar is busy then)"
                 : r->calendar == HU_CAL_UNKNOWN           ? " (couldn't check your calendar)"
                                                           : "");
    (void)hu_owner_notify_local(body);
}

/* ── run ───────────────────────────────────────────────────────────────── */

static atomic_uint g_miss_count;

static int64_t mono_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void suppress(const hu_commitment_guard_io_t *io, char **response, size_t *response_len,
                     hu_commitment_guard_result_t *r) {
    io->alloc->free(io->alloc->ctx, *response, *response_len + 1);
    *response = NULL;
    *response_len = 0;
    r->suppressed = true;
}

hu_error_t hu_commitment_guard_run(hu_gate_mode_t mode, const hu_commitment_guard_io_t *io,
                                   const char *inbound, size_t inbound_len, char **response,
                                   size_t *response_len, hu_commitment_guard_result_t *out) {
    hu_commitment_guard_result_t r;
    memset(&r, 0, sizeof(r));
    if (out)
        *out = r;
    if (mode == HU_GATE_OFF)
        return HU_OK;
    if (!io || !io->alloc || !response || !*response || !response_len || *response_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    const char *draft = *response;
    size_t draft_len = *response_len;

    int64_t t0 = mono_us();
    r.prefilter = hu_commitment_prefilter(draft, draft_len, inbound, inbound_len);
    r.prefilter_us = mono_us() - t0;
    if (!r.prefilter) {
        unsigned n = atomic_fetch_add(&g_miss_count, 1) + 1;
        r.audited = mode == HU_GATE_SHADOW && io->local.vtable && n % HU_COMMIT_AUDIT_EVERY == 0;
        if (!r.audited) {
            if (out)
                *out = r;
            return HU_OK;
        }
    }

    int64_t t1 = mono_us();
    r.detector_ok = detect(io, inbound, inbound_len, draft, draft_len, &r.detection);
    r.detect_ms = (mono_us() - t1) / 1000;
    if (!r.detector_ok && hu_commitment_money_floor(draft, draft_len)) {
        r.detection.kind = HU_COMMIT_MONEY; /* safety floor: the detector is down */
        r.detection.high_stakes = true;
        r.detection.confidence = 1.0;
    }
    if (!r.detector_ok && r.detection.kind == HU_COMMIT_NONE) {
        r.decision = HU_COMMIT_DETECT_FAILED;
    } else {
        bool cal_needed = (r.detection.kind == HU_COMMIT_PLAN ||
                           (r.detection.kind == HU_COMMIT_FAVOUR && !r.detection.high_stakes)) &&
                          r.detection.confidence >= HU_COMMIT_MIN_CONFIDENCE;
        if (cal_needed && r.detection.has_when && io->calendar) {
            int64_t t2 = mono_us();
            r.calendar =
                io->calendar(io->calendar_ctx, r.detection.when_start, r.detection.when_end);
            r.calendar_ms = (mono_us() - t2) / 1000;
        } else if (cal_needed) {
            r.calendar = r.detection.has_when ? HU_CAL_UNKNOWN : HU_CAL_NOT_CHECKED;
        }
        r.decision = hu_commitment_decide(&r.detection, r.calendar);
    }
    if (r.audited && r.decision == HU_COMMIT_DETECT_FAILED)
        r.decision = HU_COMMIT_SKIP; /* an audit that failed is just a miss */

    bool act = mode == HU_GATE_LIVE &&
               (r.decision == HU_COMMIT_REWRITE_CONFLICT || r.decision == HU_COMMIT_HOLD);
    if (act) {
        int64_t t3 = mono_us();
        size_t new_len = 0;
        char *txt = rewrite(io, r.decision, inbound, inbound_len, draft, draft_len, &new_len);
        hu_commit_detection_t again;
        bool still_commits =
            !txt || !detect(io, inbound, inbound_len, txt, new_len, &again) ||
            (again.kind != HU_COMMIT_NONE && again.confidence >= HU_COMMIT_MIN_CONFIDENCE);
        r.rewrite_ms = (mono_us() - t3) / 1000;
        if (still_commits) {
            if (txt)
                io->alloc->free(io->alloc->ctx, txt, new_len + 1);
            suppress(io, response, response_len, &r);
        } else {
            io->alloc->free(io->alloc->ctx, *response, *response_len + 1);
            *response = txt;
            *response_len = new_len;
            r.rewritten = true;
        }
        notify_owner(io, &r);
        r.notified = true;
    }
    if (out)
        *out = r;
    return HU_OK;
}

/* ── daemon glue ───────────────────────────────────────────────────────── */

#ifdef HU_IS_TEST
static hu_provider_t g_test_provider;
static bool g_test_provider_set;
static hu_calendar_query_fn g_test_cal_fn;
static void *g_test_cal_ctx;

void hu_commitment_guard_set_test_provider(const hu_provider_t *p) {
    g_test_provider_set = p != NULL;
    if (p)
        g_test_provider = *p;
    else
        memset(&g_test_provider, 0, sizeof(g_test_provider));
}
void hu_commitment_guard_set_test_calendar(hu_calendar_query_fn fn, void *ctx) {
    g_test_cal_fn = fn;
    g_test_cal_ctx = ctx;
}
void hu_commitment_guard_test_reset(void) {
    atomic_store(&g_miss_count, 0);
}
#endif

/* ONE aggregate line per guarded reply: enums, booleans, latencies, byte
 * counts. Never text, names or handles. */
static void log_line(hu_gate_mode_t mode, const hu_commitment_guard_result_t *r, bool local_ok,
                     size_t draft_b, size_t out_b) {
    const char *detector = (!r->prefilter && !r->audited) ? "skipped"
                           : !local_ok                    ? "unavailable"
                           : r->detector_ok               ? "ok"
                                                          : "failed";
    const char *action = r->suppressed ? "suppressed" : r->rewritten ? "rewritten" : "none";
    hu_log_info("commitment_guard", NULL,
                "[HU_COMMITMENT_GUARD %s] prefilter=%d audit=%d detector=%s kind=%s stakes=%s "
                "when=%d conf=%.2f calendar=%s decision=%s action=%s notified=%d prefilter_us=%lld "
                "detect_ms=%lld calendar_ms=%lld rewrite_ms=%lld draft_b=%zu out_b=%zu",
                mode == HU_GATE_LIVE ? "live" : "shadow", (int)r->prefilter, (int)r->audited,
                detector, hu_commit_kind_name(r->detection.kind),
                r->detection.kind == HU_COMMIT_NONE ? "-"
                : r->detection.high_stakes          ? "high"
                                                    : "low",
                (int)r->detection.has_when, r->detection.confidence,
                hu_calendar_state_name(r->calendar), hu_commit_decision_name(r->decision), action,
                (int)r->notified, (long long)r->prefilter_us, (long long)r->detect_ms,
                (long long)r->calendar_ms, (long long)r->rewrite_ms, draft_b, out_b);
}

bool hu_daemon_commitment_guard_apply(struct hu_agent *agent, const char *batch_key, size_t key_len,
                                      const char *inbound, size_t inbound_len, char **response,
                                      size_t *response_len) {
    hu_gate_mode_t mode = hu_commitment_guard_mode();
    if (mode == HU_GATE_OFF || !agent || !agent->alloc || !response || !*response ||
        !response_len || *response_len == 0)
        return false;
    hu_commitment_guard_io_t io;
    memset(&io, 0, sizeof(io));
    io.alloc = agent->alloc;
#ifdef HU_IS_TEST
    if (g_test_provider_set)
        io.local = g_test_provider;
    io.calendar = g_test_cal_fn;
    io.calendar_ctx = g_test_cal_ctx;
#else
    /* Local only: the loopback primary itself, never the reliable wrapper's
     * cloud fallbacks. No loopback model -> no call at all. */
    hu_provider_t local;
    if (hu_proposer_context_local_provider(&agent->provider, &local))
        io.local = local;
    io.calendar = hu_calendar_free_busy_query;
#endif
    io.model = agent->model_name;
    io.model_len = agent->model_name_len;
    const hu_contact_profile_t *cp =
        (agent->persona && batch_key && key_len > 0)
            ? hu_persona_find_contact(agent->persona, batch_key, key_len)
            : NULL;
    io.contact_name = cp ? cp->name : NULL;
    io.now = (int64_t)time(NULL);
    size_t draft_b = *response_len;
    hu_commitment_guard_result_t r;
    if (hu_commitment_guard_run(mode, &io, inbound, inbound_len, response, response_len, &r) !=
        HU_OK)
        return false;
    log_line(mode, &r, io.local.vtable != NULL, draft_b, *response_len);
    return r.rewritten || r.suppressed;
}
