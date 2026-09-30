/* src/daemon/daemon_reminders.c — contract in include/human/daemon/reminders.h
 *
 * The parser accepts a time phrase only as a whole run of time words at the
 * start of the request ("remind me tomorrow at 9 to …") or at its end ("…
 * call mom at 5"). A time word inside the task ("water the plants in the
 * sun") is part of the task. That rule is what keeps "remind me to take the
 * 5 train at 6" at 6, not at 5. */
#include "human/agent.h"
#include "human/channel.h"
#include "human/core/gate_mode.h"
#include "human/core/log.h"
#include "human/daemon.h"
#include "human/daemon/reminders.h"
#include "human/daemon/share_queue.h"
#include "human/daemon_outbound_bus.h"
#include "human/memory.h"
#ifdef HU_ENABLE_SQLITE
#include "human/memory/reminder_repo.h"
#endif

#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define MAX_TOKENS  64
#define TOKEN_CHARS 32

typedef struct token {
    const char *src; /* first byte in the original text */
    size_t len;      /* bytes in the original, trailing punctuation dropped */
    char low[TOKEN_CHARS];
} token_t;

static size_t tokenize(const char *text, size_t len, token_t *out, size_t cap) {
    size_t n = 0;
    for (size_t i = 0; i < len && n < cap;) {
        while (i < len && isspace((unsigned char)text[i]))
            i++;
        size_t start = i;
        while (i < len && !isspace((unsigned char)text[i]))
            i++;
        size_t wl = i - start;
        while (wl > 0 && strchr(",.!?;:", text[start + wl - 1]))
            wl--;
        if (wl == 0)
            continue;
        token_t *t = &out[n++];
        t->src = text + start;
        t->len = wl;
        if (wl < TOKEN_CHARS) {
            for (size_t k = 0; k < wl; k++)
                t->low[k] = (char)tolower((unsigned char)t->src[k]);
            t->low[wl] = '\0';
        } else {
            t->low[0] = '\0'; /* too long to be a keyword */
        }
    }
    return n;
}

static bool tok_is(const token_t *t, const char *word) {
    return strcmp(t->low, word) == 0;
}

static bool tok_in(const token_t *t, const char *const *words, size_t count) {
    for (size_t i = 0; i < count; i++)
        if (tok_is(t, words[i]))
            return true;
    return false;
}

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* ── Durations: "20 minutes", "20m", "an hour", "half an hour" ─────────── */

static int64_t unit_seconds(const char *u) {
    static const char *const mins[] = {"m", "min", "mins", "minute", "minutes"};
    static const char *const hours[] = {"h", "hr", "hrs", "hour", "hours"};
    static const char *const days[] = {"d", "day", "days"};
    for (size_t i = 0; i < COUNT(mins); i++)
        if (strcmp(u, mins[i]) == 0)
            return 60;
    for (size_t i = 0; i < COUNT(hours); i++)
        if (strcmp(u, hours[i]) == 0)
            return 3600;
    for (size_t i = 0; i < COUNT(days); i++)
        if (strcmp(u, days[i]) == 0)
            return 86400;
    return 0;
}

/* Leading decimal count of up to 3 digits; *rest points after it. */
static int leading_count(const char *s, const char **rest) {
    int v = 0, digits = 0;
    while (isdigit((unsigned char)s[digits]) && digits < 3)
        v = v * 10 + (s[digits++] - '0');
    if (digits == 0 || isdigit((unsigned char)s[digits]))
        return -1;
    *rest = s + digits;
    return v;
}

static size_t parse_duration(const token_t *t, size_t n, size_t i, int64_t *secs) {
    if (i >= n)
        return 0;
    if (i + 2 < n && tok_is(&t[i], "half") && (tok_is(&t[i + 1], "an") || tok_is(&t[i + 1], "a")) &&
        tok_is(&t[i + 2], "hour")) {
        *secs = 1800;
        return 3;
    }
    if (i + 1 < n && (tok_is(&t[i], "an") || tok_is(&t[i], "a"))) {
        int64_t u = unit_seconds(t[i + 1].low);
        if (u > 0 && (tok_is(&t[i + 1], "hour") || tok_is(&t[i + 1], "minute") ||
                      tok_is(&t[i + 1], "day"))) {
            *secs = u;
            return 2;
        }
        return 0;
    }
    const char *rest = NULL;
    int count = leading_count(t[i].low, &rest);
    if (count <= 0)
        return 0;
    if (*rest) { /* "20m", "2h", "20min" */
        int64_t u = unit_seconds(rest);
        if (u == 0)
            return 0;
        *secs = (int64_t)count * u;
        return 1;
    }
    if (i + 1 < n) {
        int64_t u = unit_seconds(t[i + 1].low);
        if (u > 0) {
            *secs = (int64_t)count * u;
            return 2;
        }
    }
    return 0;
}

/* ── Time phrases ───────────────────────────────────────────────────────── */

enum { PERIOD_NONE = 0, PERIOD_MORNING, PERIOD_AFTERNOON, PERIOD_EVENING, PERIOD_NIGHT };

typedef struct when_acc {
    int64_t relative_s;
    int day_offset;
    int weekday;
    bool have_day;
    int hour, minute;
    int half; /* 0 unknown, 1 am, 2 pm, 3 24-hour clock or noon/midnight */
    bool have_clock;
    int period;
} when_acc_t;

static int weekday_of(const char *w, bool allow_abbrev) {
    static const char *const full[] = {"sunday",   "monday", "tuesday", "wednesday",
                                       "thursday", "friday", "saturday"};
    static const char *const abbr[][2] = {{"sun", NULL}, {"mon", NULL},    {"tue", "tues"},
                                          {"wed", NULL}, {"thu", "thurs"}, {"fri", NULL},
                                          {"sat", NULL}};
    for (int d = 0; d < 7; d++) {
        if (strcmp(w, full[d]) == 0)
            return d;
        if (allow_abbrev)
            for (int k = 0; k < 2; k++)
                if (abbr[d][k] && strcmp(w, abbr[d][k]) == 0)
                    return d;
    }
    return -1;
}

static int half_of(const char *w) {
    if (strcmp(w, "am") == 0 || strcmp(w, "a.m") == 0)
        return 1;
    if (strcmp(w, "pm") == 0 || strcmp(w, "p.m") == 0)
        return 2;
    return 0;
}

/* "5", "5pm", "5:30", "5:30pm", "17:00", "noon", "midnight", optionally
 * followed by a separate "am"/"pm" token. Returns tokens consumed. */
static size_t parse_clock(const token_t *t, size_t n, size_t i, when_acc_t *a) {
    if (i >= n)
        return 0;
    if (tok_is(&t[i], "noon") || tok_is(&t[i], "midday")) {
        a->hour = 12, a->minute = 0, a->half = 3;
        return 1;
    }
    if (tok_is(&t[i], "midnight")) {
        a->hour = 0, a->minute = 0, a->half = 3;
        return 1;
    }
    const char *p = t[i].low;
    int h = 0, m = 0, hd = 0;
    while (isdigit((unsigned char)*p) && hd < 2)
        h = h * 10 + (*p++ - '0'), hd++;
    if (hd == 0)
        return 0;
    bool colon = false;
    if (*p == ':') {
        colon = true;
        p++;
        if (!isdigit((unsigned char)p[0]) || !isdigit((unsigned char)p[1]))
            return 0;
        m = (p[0] - '0') * 10 + (p[1] - '0');
        p += 2;
    }
    int half = half_of(p);
    if (*p && half == 0)
        return 0;
    size_t used = 1;
    if (half == 0 && i + 1 < n && (half = half_of(t[i + 1].low)) != 0)
        used = 2;
    if (m > 59 || h > 23)
        return 0;
    if (half != 0) {
        if (h < 1 || h > 12)
            return 0;
        h = (h % 12) + (half == 2 ? 12 : 0);
    } else if (h == 0 || h >= 12 || (colon && hd == 2 && t[i].low[0] == '0')) {
        half = 3; /* "17:00", "0:30", "09:15" — a 24-hour clock; "at 12" is noon */
    }
    a->hour = h, a->minute = m, a->half = half;
    return used;
}

/* One time phrase at t[i]. Returns tokens consumed, 0 if none, and refuses
 * a phrase that contradicts one already accumulated. */
static size_t parse_time_phrase(const token_t *t, size_t n, size_t i, when_acc_t *a) {
    const token_t *w = &t[i];
    int64_t secs = 0;
    size_t used;

    if (tok_is(w, "in")) {
        if (i + 2 < n && tok_is(&t[i + 1], "the")) {
            int period = tok_is(&t[i + 2], "morning")     ? PERIOD_MORNING
                         : tok_is(&t[i + 2], "afternoon") ? PERIOD_AFTERNOON
                         : tok_is(&t[i + 2], "evening")   ? PERIOD_EVENING
                                                          : PERIOD_NONE;
            if (period == PERIOD_NONE || a->period)
                return 0;
            a->period = period;
            return 3;
        }
        used = parse_duration(t, n, i + 1, &secs);
        if (used == 0 || a->relative_s || a->have_day || a->have_clock || a->period)
            return 0;
        a->relative_s = secs;
        return used + 1;
    }
    if (a->relative_s)
        return 0; /* "in 20 minutes" stands alone */

    if (tok_is(w, "at") || tok_is(w, "by")) {
        if (a->have_clock)
            return 0;
        used = parse_clock(t, n, i + 1, a);
        if (used == 0)
            return 0;
        a->have_clock = true;
        return used + 1;
    }
    if (!a->have_clock) {
        /* A bare clock only with am/pm or as noon/midnight: "5pm", "noon". */
        when_acc_t probe = *a;
        used = parse_clock(t, n, i, &probe);
        if (used > 0 && (probe.half == 1 || probe.half == 2 || tok_is(w, "noon") ||
                         tok_is(w, "midnight") || tok_is(w, "midday"))) {
            *a = probe;
            a->have_clock = true;
            return used;
        }
    }

    if (tok_is(w, "today") || tok_is(w, "tonight") || tok_is(w, "tomorrow") || tok_is(w, "tmrw") ||
        tok_is(w, "tmr")) {
        if (a->have_day)
            return 0;
        a->have_day = true;
        a->day_offset = tok_is(w, "today") || tok_is(w, "tonight") ? 0 : 1;
        if (tok_is(w, "tonight")) {
            if (a->period && a->period != PERIOD_NIGHT)
                return 0;
            a->period = PERIOD_NIGHT;
        }
        return 1;
    }

    if (tok_is(w, "this") && i + 1 < n) {
        int period = tok_is(&t[i + 1], "morning")     ? PERIOD_MORNING
                     : tok_is(&t[i + 1], "afternoon") ? PERIOD_AFTERNOON
                     : tok_is(&t[i + 1], "evening")   ? PERIOD_EVENING
                                                      : PERIOD_NONE;
        if (period != PERIOD_NONE) {
            if (a->period || (a->have_day && a->day_offset != 0))
                return 0;
            a->period = period;
            a->have_day = true;
            a->day_offset = 0;
            return 2;
        }
    }
    {
        int period = tok_is(w, "morning")     ? PERIOD_MORNING
                     : tok_is(w, "afternoon") ? PERIOD_AFTERNOON
                     : tok_is(w, "evening")   ? PERIOD_EVENING
                     : tok_is(w, "night")     ? PERIOD_NIGHT
                                              : PERIOD_NONE;
        if (period != PERIOD_NONE) {
            /* Bare "morning" only after a day word: "tomorrow morning". */
            if (a->period || !a->have_day)
                return 0;
            a->period = period;
            return 1;
        }
    }

    bool lead = tok_is(w, "on") || tok_is(w, "next") || tok_is(w, "this");
    size_t wi = lead ? i + 1 : i;
    if (wi < n) {
        int d = weekday_of(t[wi].low, lead);
        if (d >= 0) {
            if (a->have_day)
                return 0;
            a->have_day = true;
            a->weekday = d;
            return wi - i + 1;
        }
    }
    return 0;
}

/* t[from..to) is one or more time phrases and nothing else. */
static bool parse_time_chain(const token_t *t, size_t from, size_t to, when_acc_t *a) {
    memset(a, 0, sizeof(*a));
    a->weekday = -1;
    a->hour = -1;
    if (from >= to)
        return false;
    for (size_t i = from; i < to;) {
        size_t used = parse_time_phrase(t, to, i, a);
        if (used == 0)
            return false;
        i += used;
    }
    return true;
}

static void acc_to_when(const when_acc_t *a, hu_reminder_when_t *w) {
    memset(w, 0, sizeof(*w));
    w->weekday = -1;
    w->hour = -1;
    if (a->relative_s > 0) {
        w->relative_s = a->relative_s;
        return;
    }
    w->day_offset = a->weekday >= 0 ? 0 : a->day_offset;
    w->weekday = a->weekday;
    if (a->have_clock) {
        int h = a->hour;
        if (a->half == 0) {
            if (a->period == PERIOD_MORNING)
                h = h % 12;
            else if (a->period != PERIOD_NONE)
                h = h % 12 + 12;
            else
                w->half_unknown = true;
        }
        w->hour = h;
        w->minute = a->minute;
        return;
    }
    static const int period_default_hour[] = {9, 9, 15, 18, 20};
    if (a->period != PERIOD_NONE || a->have_day) {
        w->hour = period_default_hour[a->period];
        w->minute = 0;
        w->time_is_default = true;
    }
}

/* ── Command parse ──────────────────────────────────────────────────────── */

static bool phrase_equals(const token_t *t, size_t n, const char *phrase) {
    size_t i = 0;
    const char *p = phrase;
    while (*p) {
        const char *sp = strchr(p, ' ');
        size_t wl = sp ? (size_t)(sp - p) : strlen(p);
        if (i >= n || strlen(t[i].low) != wl || strncmp(t[i].low, p, wl) != 0)
            return false;
        i++;
        p += wl;
        while (*p == ' ')
            p++;
    }
    return i == n;
}

static bool matches_any(const token_t *t, size_t n, const char *const *phrases, size_t count) {
    for (size_t k = 0; k < count; k++)
        if (phrase_equals(t, n, phrases[k]))
            return true;
    return false;
}

static void copy_what(const token_t *t, size_t from, size_t to, char *out, size_t cap) {
    out[0] = '\0';
    if (from >= to || cap == 0)
        return;
    const char *start = t[from].src;
    const char *end = t[to - 1].src + t[to - 1].len;
    size_t len = (size_t)(end - start);
    if (len >= cap) {
        len = cap - 1;
        while (len > 0 && ((unsigned char)start[len] & 0xC0) == 0x80)
            len--; /* never cut a UTF-8 sequence in half */
    }
    memcpy(out, start, len);
    out[len] = '\0';
}

bool hu_reminder_parse(const char *text, size_t len, hu_reminder_cmd_t *out) {
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    out->when.weekday = -1;
    out->when.hour = -1;
    if (!text || len == 0 || len > 1024)
        return false;

    token_t t[MAX_TOKENS];
    size_t n = tokenize(text, len, t, MAX_TOKENS);
    static const char *const polite[] = {"please", "pls", "plz", "thanks", "thx", "ty"};
    while (n > 0 && tok_in(&t[n - 1], polite, COUNT(polite)))
        n--;
    if (n == 0)
        return false;

    static const char *const list_phrases[] = {
        "reminders",      "my reminders",      "list reminders",        "list my reminders",
        "show reminders", "show my reminders", "what are my reminders", "what reminders do i have",
        "any reminders",  "what's on my list", "whats on my list",
    };
    if (matches_any(t, n, list_phrases, COUNT(list_phrases))) {
        out->kind = HU_REMINDER_CMD_LIST;
        return true;
    }
    static const char *const done_phrases[] = {"done", "did it", "finished", "all done",
                                               "completed"};
    if (matches_any(t, n, done_phrases, COUNT(done_phrases))) {
        out->kind = HU_REMINDER_CMD_DONE;
        return true;
    }
    if (tok_is(&t[0], "snooze")) {
        int64_t secs = 900;
        size_t i = 1;
        if (i < n && tok_is(&t[i], "for"))
            i++;
        if (i < n) {
            size_t used = parse_duration(t, n, i, &secs);
            if (used == 0) {
                const char *rest = NULL;
                int mins = leading_count(t[i].low, &rest);
                if (mins <= 0 || *rest)
                    return false;
                secs = (int64_t)mins * 60; /* "snooze 10" means minutes */
                used = 1;
            }
            if (i + used != n)
                return false;
        }
        out->kind = HU_REMINDER_CMD_SNOOZE;
        out->snooze_s = secs;
        return true;
    }

    /* "remind me …", optionally after "hey", "can you", "please". */
    static const char *const lead_in[] = {"hey", "please", "pls", "can", "could", "would", "you"};
    size_t i = 0;
    while (i < n && i < 3 && tok_in(&t[i], lead_in, COUNT(lead_in)))
        i++;
    if (!(i + 1 < n && tok_is(&t[i], "remind") && tok_is(&t[i + 1], "me"))) {
        when_acc_t a;
        if (parse_time_chain(t, 0, n, &a)) { /* a bare time answers "when?" */
            out->kind = HU_REMINDER_CMD_WHEN;
            acc_to_when(&a, &out->when);
            return true;
        }
        return false;
    }
    i += 2;
    if (i >= n)
        return false;

    when_acc_t a;
    bool timed = false;
    size_t what_from = i, what_to = n;

    /* Time first: "remind me tomorrow at 9 to call mom". */
    for (size_t k = i + 1; k < n; k++) {
        if ((tok_is(&t[k], "to") || tok_is(&t[k], "about")) && parse_time_chain(t, i, k, &a)) {
            timed = true;
            what_from = k + 1;
            break;
        }
    }
    if (!timed) {
        if (tok_is(&t[i], "to") || tok_is(&t[i], "about"))
            what_from = i + 1;
        /* Time last: the shortest trailing run of time phrases. */
        for (size_t j = what_from + 1; j < n; j++) {
            if (parse_time_chain(t, j, n, &a)) {
                timed = true;
                what_to = j;
                break;
            }
        }
        if (!timed && what_from == i && parse_time_chain(t, i, n, &a)) {
            timed = true; /* "remind me in 20 minutes" — no task given */
            what_to = what_from;
        }
    }

    copy_what(t, what_from, what_to, out->what, sizeof(out->what));
    bool again = strcasecmp(out->what, "again") == 0 || strcasecmp(out->what, "later") == 0;
    if (again || out->what[0] == '\0') {
        /* No task: "remind me again in 20 min", "remind me later", "remind
         * me tomorrow" re-arm the reminder that was just delivered. */
        if (!again && !timed)
            return false;
        out->kind = HU_REMINDER_CMD_SNOOZE;
        out->snooze_s = 3600;
        if (timed)
            acc_to_when(&a, &out->when);
        out->what[0] = '\0';
        return true;
    }
    if (!timed) {
        out->kind = HU_REMINDER_CMD_ADD_NO_TIME;
        return true;
    }
    out->kind = HU_REMINDER_CMD_ADD;
    acc_to_when(&a, &out->when);
    return true;
}

/* ── Resolving and naming instants ──────────────────────────────────────── */

static int64_t local_instant(const struct tm *base, int add_days, int hour, int minute) {
    struct tm c = *base;
    c.tm_mday += add_days;
    c.tm_hour = hour;
    c.tm_min = minute;
    c.tm_sec = 0;
    c.tm_isdst = -1; /* let mktime apply the zone's DST rule for that day */
    time_t r = mktime(&c);
    return r == (time_t)-1 ? 0 : (int64_t)r;
}

int64_t hu_reminder_resolve(const hu_reminder_when_t *when, int64_t now) {
    if (!when || now <= 0)
        return 0;
    if (when->relative_s > 0)
        return now + when->relative_s;
    if (when->hour < 0 || when->hour > 23 || when->minute < 0 || when->minute > 59)
        return 0;
    time_t tn = (time_t)now;
    struct tm base;
    if (!localtime_r(&tn, &base))
        return 0;

    int add = when->day_offset;
    if (when->weekday >= 0 && when->weekday <= 6)
        add = (when->weekday - base.tm_wday + 7) % 7;
    int hour = when->hour;

    if (when->half_unknown && hour >= 1 && hour <= 11) {
        if (add == 0) {
            /* Today: whichever of h:00 and (h+12):00 comes next. */
            int64_t r = local_instant(&base, 0, hour, when->minute);
            if (r > now)
                return r;
            r = local_instant(&base, 0, hour + 12, when->minute);
            if (r > now)
                return r;
            return local_instant(&base, when->weekday >= 0 ? 7 : 1, hour, when->minute);
        }
        /* Another day: 1–6 means afternoon, 7–11 morning. */
        if (hour <= 6)
            hour += 12;
    }

    int64_t r = local_instant(&base, add, hour, when->minute);
    if (r > now)
        return r;
    if (when->weekday >= 0)
        return local_instant(&base, add + 7, hour, when->minute);
    if (add == 0 && when->time_is_default)
        return now + 3600; /* "remind me tonight" said at 9pm: in an hour */
    if (add == 0)
        return local_instant(&base, 1, hour, when->minute);
    return r;
}

/* Calendar days from now's local date to due's local date. */
static int local_day_diff(const struct tm *due, const struct tm *now) {
    struct tm a = *due, b = *now;
    a.tm_hour = b.tm_hour = 12;
    a.tm_min = b.tm_min = a.tm_sec = b.tm_sec = 0;
    a.tm_isdst = b.tm_isdst = -1;
    time_t ta = mktime(&a), tb = mktime(&b);
    double days = difftime(ta, tb) / 86400.0;
    return (int)(days < 0 ? days - 0.5 : days + 0.5);
}

size_t hu_reminder_format_due(int64_t due, int64_t now, char *buf, size_t cap) {
    if (!buf || cap == 0)
        return 0;
    buf[0] = '\0';
    if (due <= 0 || now <= 0)
        return 0;
    int w;
    int64_t ahead = due - now;
    if (ahead > 0 && ahead < 3600) {
        w = snprintf(buf, cap, "in %lld min", (long long)((ahead + 59) / 60));
    } else {
        time_t td = (time_t)due, tn = (time_t)now;
        struct tm d, t;
        if (!localtime_r(&td, &d) || !localtime_r(&tn, &t))
            return 0;
        char clock[16];
        int h12 = d.tm_hour % 12 == 0 ? 12 : d.tm_hour % 12;
        const char *ap = d.tm_hour < 12 ? "am" : "pm";
        if (d.tm_min == 0)
            snprintf(clock, sizeof(clock), "%d%s", h12, ap);
        else
            snprintf(clock, sizeof(clock), "%d:%02d%s", h12, d.tm_min, ap);
        static const char *const wd[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                         "Thursday", "Friday", "Saturday"};
        static const char *const mon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        int diff = local_day_diff(&d, &t);
        if (diff == 0)
            w = snprintf(buf, cap, "today at %s", clock);
        else if (diff == 1)
            w = snprintf(buf, cap, "tomorrow at %s", clock);
        else if (diff > 1 && diff < 7)
            w = snprintf(buf, cap, "%s at %s", wd[d.tm_wday], clock);
        else
            w = snprintf(buf, cap, "%s %d at %s", mon[d.tm_mon], d.tm_mday, clock);
    }
    if (w < 0 || (size_t)w >= cap) {
        buf[0] = '\0';
        return 0;
    }
    return (size_t)w;
}

/* ── Daemon side ────────────────────────────────────────────────────────── */

static hu_gate_mode_t reminders_gate(void) {
    return hu_gate_mode_from_env("HU_REMINDERS", HU_GATE_OFF);
}

static const char *kind_name(hu_reminder_cmd_kind_t k) {
    static const char *const names[] = {"none", "add",    "add-no-time", "list",
                                        "done", "snooze", "when"};
    return (size_t)k < COUNT(names) ? names[k] : "?";
}

#ifdef HU_ENABLE_SQLITE

#define PENDING_TTL_S 600                    /* how long "sure, when?" waits for an answer */
#define DONE_WINDOW_S 7200                   /* "done"/"snooze" refer to a delivery this recent */
#define LATE_AFTER_S  7200                   /* later than this is missed, not delivered */
#define MAX_AHEAD_S   ((int64_t)400 * 86400) /* refuse a due time more than ~13 months out */

/* The one task waiting for a time. Owner-only and single-threaded (the
 * service loop), so one slot is enough. */
static struct {
    char owner[HU_REMINDER_OWNER_MAX];
    char channel[HU_REMINDER_CHANNEL_MAX];
    char what[HU_REMINDER_TEXT_MAX];
    int64_t asked_at;
} g_pending;

/* Schema creation is idempotent and cheap, so it runs on every use rather
 * than being cached by db pointer (a freed handle's address can come back). */
static struct sqlite3 *reminders_db(struct hu_agent *agent) {
    struct sqlite3 *db = agent && agent->memory ? hu_sqlite_memory_get_db(agent->memory) : NULL;
    if (db && hu_reminder_repo_ensure_schema(db) != HU_OK)
        return NULL;
    return db;
}

static void set_str(char *dst, size_t cap, const char *src, size_t len) {
    if (len >= cap)
        len = cap - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static bool file_reminder(struct sqlite3 *db, const char *owner, const char *channel,
                          const char *what, int64_t due, int64_t now, char *reply,
                          size_t reply_cap) {
    if (due <= now || due - now > MAX_AHEAD_S) {
        snprintf(reply, reply_cap, "hmm, I couldn't place that time. when should I remind you?");
        return false;
    }
    int64_t id = 0;
    if (hu_reminder_repo_add(db, owner, channel, what, due, now, "owner_text", &id) != HU_OK) {
        snprintf(reply, reply_cap, "couldn't save that one, try again in a sec?");
        return false;
    }
    char when_s[48];
    hu_reminder_format_due(due, now, when_s, sizeof(when_s));
    snprintf(reply, reply_cap, "ok, %s: %s", when_s, what);
    return true;
}

bool hu_reminders_handle_owner_message(struct hu_agent *agent, const char *owner, size_t owner_len,
                                       const char *channel_name, const char *text, size_t len,
                                       int64_t now, char *reply, size_t reply_cap) {
    if (!agent || !owner || owner_len == 0 || !text || !reply || reply_cap == 0)
        return false;
    reply[0] = '\0';
    hu_gate_mode_t gate = reminders_gate();
    if (gate == HU_GATE_OFF || !agent->persona ||
        !hu_share_is_owner(agent->persona, owner, owner_len))
        return false;
    hu_reminder_cmd_t cmd;
    if (!hu_reminder_parse(text, len, &cmd))
        return false;
    if (gate == HU_GATE_SHADOW) {
        int64_t due = cmd.kind == HU_REMINDER_CMD_ADD ? hu_reminder_resolve(&cmd.when, now) : 0;
        hu_log_info("reminders", agent->observer,
                    "shadow: would handle %s (what %zu chars, due in %llds)", kind_name(cmd.kind),
                    strlen(cmd.what), (long long)(due > 0 ? due - now : 0));
        return false;
    }

    struct sqlite3 *db = reminders_db(agent);
    if (!db) {
        static atomic_bool warned = false;
        hu_log_warn_once(&warned, "reminders", agent->observer,
                         "reminders live but memory has no SQLite database; not handling");
        return false;
    }
    char owner_z[HU_REMINDER_OWNER_MAX], channel_z[HU_REMINDER_CHANNEL_MAX];
    set_str(owner_z, sizeof(owner_z), owner, owner_len);
    const char *cn = channel_name ? channel_name : "";
    set_str(channel_z, sizeof(channel_z), cn, strlen(cn));

    switch (cmd.kind) {
    case HU_REMINDER_CMD_ADD:
        if (!file_reminder(db, owner_z, channel_z, cmd.what, hu_reminder_resolve(&cmd.when, now),
                           now, reply, reply_cap)) {
            /* Keep the task so the next message can just be the time. */
            snprintf(g_pending.owner, sizeof(g_pending.owner), "%s", owner_z);
            snprintf(g_pending.channel, sizeof(g_pending.channel), "%s", channel_z);
            snprintf(g_pending.what, sizeof(g_pending.what), "%s", cmd.what);
            g_pending.asked_at = now;
        }
        return true;
    case HU_REMINDER_CMD_ADD_NO_TIME:
        snprintf(g_pending.owner, sizeof(g_pending.owner), "%s", owner_z);
        snprintf(g_pending.channel, sizeof(g_pending.channel), "%s", channel_z);
        snprintf(g_pending.what, sizeof(g_pending.what), "%s", cmd.what);
        g_pending.asked_at = now;
        snprintf(reply, reply_cap, "sure, when?");
        return true;
    case HU_REMINDER_CMD_WHEN:
        if (g_pending.asked_at == 0 || now - g_pending.asked_at > PENDING_TTL_S ||
            strcmp(g_pending.owner, owner_z) != 0)
            return false; /* just a message that happens to be a time */
        if (file_reminder(db, g_pending.owner, g_pending.channel, g_pending.what,
                          hu_reminder_resolve(&cmd.when, now), now, reply, reply_cap))
            memset(&g_pending, 0, sizeof(g_pending));
        return true;
    case HU_REMINDER_CMD_LIST: {
        hu_reminder_t rows[10];
        size_t n = 0;
        if (hu_reminder_repo_upcoming(db, owner_z, rows, 10, &n) != HU_OK)
            return false;
        if (n == 0) {
            snprintf(reply, reply_cap, "nothing on your list");
            return true;
        }
        size_t off = (size_t)snprintf(reply, reply_cap, "%zu coming up:", n);
        for (size_t k = 0; k < n && off < reply_cap; k++) {
            char when_s[48];
            hu_reminder_format_due(rows[k].due_at, now, when_s, sizeof(when_s));
            int w = snprintf(reply + off, reply_cap - off, "\n- %s, %s", rows[k].what, when_s);
            if (w < 0 || (size_t)w >= reply_cap - off)
                break; /* the list is cut, never a line */
            off += (size_t)w;
        }
        reply[off < reply_cap ? off : reply_cap - 1] = '\0';
        return true;
    }
    case HU_REMINDER_CMD_DONE:
    case HU_REMINDER_CMD_SNOOZE: {
        hu_reminder_t last;
        if (hu_reminder_repo_last_sent(db, owner_z, now - DONE_WINDOW_S, &last) != HU_OK)
            return false; /* "done" with nothing just delivered is ordinary chat */
        if (cmd.kind == HU_REMINDER_CMD_DONE) {
            if (hu_reminder_repo_mark(db, last.id, "done", now) != HU_OK)
                return false;
            snprintf(reply, reply_cap, "nice, crossed off");
            return true;
        }
        bool timed = cmd.when.relative_s > 0 || cmd.when.hour >= 0;
        int64_t due = timed ? hu_reminder_resolve(&cmd.when, now) : now + cmd.snooze_s;
        if (due <= now)
            return false;
        if (hu_reminder_repo_snooze(db, last.id, due, now) != HU_OK)
            return false;
        char when_s[48];
        hu_reminder_format_due(due, now, when_s, sizeof(when_s));
        snprintf(reply, reply_cap, "ok, again %s", when_s);
        return true;
    }
    case HU_REMINDER_CMD_NONE:
    default:
        return false;
    }
}

static bool is_owner(struct hu_agent *agent, const char *handle) {
    return agent->persona && hu_share_is_owner(agent->persona, handle, strlen(handle));
}

/* Missed reminders are mentioned once, together, and never sent as if on
 * time. One message per pass covers every row for the first row's thread. */
static void tell_missed(struct hu_agent *agent, struct sqlite3 *db, hu_service_channel_t *channels,
                        size_t channel_count, int64_t now) {
    hu_reminder_t rows[8];
    size_t n = 0;
    if (hu_reminder_repo_missed(db, rows, 8, &n) != HU_OK || n == 0)
        return;
    char msg[1024];
    size_t off = 0;
    const hu_reminder_t *first = NULL;
    int64_t told[8];
    size_t told_n = 0;
    for (size_t k = 0; k < n; k++) {
        if (!is_owner(agent, rows[k].owner)) {
            (void)hu_reminder_repo_mark(db, rows[k].id, "done", now);
            continue;
        }
        if (!first) {
            first = &rows[k];
            off = (size_t)snprintf(msg, sizeof(msg), "missed these while I was offline:");
        } else if (strcmp(rows[k].owner, first->owner) != 0 ||
                   strcmp(rows[k].channel, first->channel) != 0) {
            continue; /* another thread: next pass */
        }
        char when_s[48];
        hu_reminder_format_due(rows[k].due_at, now, when_s, sizeof(when_s));
        int w = snprintf(msg + off, sizeof(msg) - off, "\n- %s (%s)", rows[k].what, when_s);
        if (w < 0 || (size_t)w >= sizeof(msg) - off)
            break; /* the rest are told next pass */
        off += (size_t)w;
        told[told_n++] = rows[k].id;
    }
    if (!first || told_n == 0)
        return;
    hu_service_channel_t *sc =
        hu_daemon_outbound_find_sender(channels, channel_count, first->channel);
    hu_error_t err = sc ? sc->channel->vtable->send(sc->channel->ctx, first->owner,
                                                    strlen(first->owner), msg, off, NULL, 0)
                        : HU_ERR_NOT_FOUND;
    if (err != HU_OK) {
        hu_log_warn("reminders", agent->observer, "missed-reminder note not delivered on %s: %s",
                    first->channel, sc ? hu_error_string(err) : "no channel");
        return; /* stays missed: told on a later pass */
    }
    for (size_t k = 0; k < told_n; k++)
        (void)hu_reminder_repo_mark(db, told[k], "missed_told", now);
}

void hu_reminders_tick(struct hu_agent *agent, struct hu_service_channel *channels,
                       size_t channel_count, int64_t now) {
    static int64_t last_pass = 0;
    if (!agent)
        return;
    if (reminders_gate() != HU_GATE_LIVE) {
        static atomic_bool noted = false;
        hu_log_info_once(&noted, "reminders", agent->observer,
                         "owner reminders not delivering (HU_REMINDERS is not live); "
                         "set HU_REMINDERS=live to activate");
        return;
    }
    if (now >= last_pass && now - last_pass < 20)
        return; /* a clock that went backwards starts a fresh pass */
    last_pass = now;
    struct sqlite3 *db = reminders_db(agent);
    if (!db)
        return;

    hu_reminder_t due[8];
    size_t n = 0, missed = 0;
    if (hu_reminder_repo_claim_due(db, now, LATE_AFTER_S, due, 8, &n, &missed) != HU_OK)
        return;
    if (missed > 0)
        hu_log_warn("reminders", agent->observer,
                    "%zu reminder(s) marked missed: more than %ds late (daemon was down?)", missed,
                    LATE_AFTER_S);
    for (size_t k = 0; k < n; k++) {
        hu_reminder_t *r = &due[k];
        /* A reminder only ever goes back to the owner. If the owner changed
         * since it was filed, drop it rather than text anyone else. */
        if (!is_owner(agent, r->owner)) {
            (void)hu_reminder_repo_mark(db, r->id, "done", now);
            hu_log_warn("reminders", agent->observer,
                        "reminder %lld dropped: its recipient is no longer the owner",
                        (long long)r->id);
            continue;
        }
        hu_service_channel_t *sc =
            hu_daemon_outbound_find_sender(channels, channel_count, r->channel);
        char msg[HU_REMINDER_TEXT_MAX + 16];
        int w = snprintf(msg, sizeof(msg), "reminder: %s", r->what);
        hu_error_t err = HU_ERR_NOT_FOUND;
        if (sc && w > 0)
            err = sc->channel->vtable->send(sc->channel->ctx, r->owner, strlen(r->owner), msg,
                                            (size_t)w < sizeof(msg) ? (size_t)w : sizeof(msg) - 1,
                                            NULL, 0);
        if (err == HU_OK) {
            (void)hu_reminder_repo_mark(db, r->id, "sent", now);
            hu_log_info("reminders", agent->observer, "reminder %lld delivered on %s",
                        (long long)r->id, r->channel);
        } else {
            /* Back to pending: retried next pass until it is too late. */
            (void)hu_reminder_repo_mark(db, r->id, "pending", now);
            hu_log_warn("reminders", agent->observer, "reminder %lld not delivered on %s: %s",
                        (long long)r->id, r->channel, sc ? hu_error_string(err) : "no channel");
        }
    }
    tell_missed(agent, db, channels, channel_count, now);
}

#else /* !HU_ENABLE_SQLITE — nothing to store reminders in */

bool hu_reminders_handle_owner_message(struct hu_agent *agent, const char *owner, size_t owner_len,
                                       const char *channel_name, const char *text, size_t len,
                                       int64_t now, char *reply, size_t reply_cap) {
    (void)agent, (void)owner, (void)owner_len, (void)channel_name, (void)now;
    if (reply && reply_cap)
        reply[0] = '\0';
    if (reminders_gate() != HU_GATE_OFF) {
        hu_reminder_cmd_t cmd;
        if (hu_reminder_parse(text, len, &cmd)) {
            static atomic_bool warned = false;
            hu_log_warn_once(&warned, "reminders", NULL,
                             "reminders need SQLite; this build has none (saw %s)",
                             kind_name(cmd.kind));
        }
    }
    return false;
}

void hu_reminders_tick(struct hu_agent *agent, struct hu_service_channel *channels,
                       size_t channel_count, int64_t now) {
    (void)agent, (void)channels, (void)channel_count, (void)now;
}

#endif /* HU_ENABLE_SQLITE */
