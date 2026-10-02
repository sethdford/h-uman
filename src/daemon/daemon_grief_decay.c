/* daemon_grief_decay.c — time-decayed emotional-tone gate for proactive
 * check-ins (DEF-10). Contract in include/human/daemon/grief_decay.h. */
/* strptime needs the XOPEN feature set. */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include "human/daemon/grief_decay.h"

#include "human/core/log.h"
#include "human/humanness.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void hu_grief_decay_note_inbound(hu_grief_decay_inbound_t *li, const char *text, size_t len,
                                 const char *ts) {
    if (!li)
        return;
    size_t copy = text ? len : 0;
    if (copy >= sizeof(li->text))
        copy = sizeof(li->text) - 1;
    if (copy)
        memcpy(li->text, text, copy);
    li->text[copy] = '\0';
    li->len = copy;
    size_t tl = ts ? strnlen(ts, sizeof(li->ts) - 1) : 0;
    if (tl)
        memcpy(li->ts, ts, tl);
    li->ts[tl] = '\0';
}

hu_gate_mode_t hu_grief_decay_mode(void) {
    return hu_gate_mode_from_env("HU_GRIEF_DECAY", HU_GATE_OFF);
}

double hu_grief_decay_quiet_hours(void) {
    const char *v = getenv("HU_GRIEF_DECAY_QUIET_HOURS");
    if (!v || !v[0])
        return HU_GRIEF_DECAY_DEFAULT_QUIET_HOURS;
    char *end = NULL;
    errno = 0;
    double h = strtod(v, &end);
    if (errno != 0 || end == v || (end && *end != '\0') || h != h)
        return HU_GRIEF_DECAY_DEFAULT_QUIET_HOURS;
    if (h < HU_GRIEF_DECAY_MIN_QUIET_HOURS)
        return HU_GRIEF_DECAY_MIN_QUIET_HOURS;
    if (h > HU_GRIEF_DECAY_MAX_QUIET_HOURS)
        return HU_GRIEF_DECAY_MAX_QUIET_HOURS;
    return h;
}

int64_t hu_grief_decay_parse_ts(const char *ts) {
    if (!ts || !ts[0])
        return -1;
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    const char *end = strptime(ts, "%Y-%m-%d %H:%M", &tmv);
    if (!end)
        return -1;
    tmv.tm_isdst = -1; /* local time, DST resolved by mktime */
    time_t t = mktime(&tmv);
    return t == (time_t)-1 ? -1 : (int64_t)t;
}

bool hu_grief_decay_in_quiet_window(int64_t heavy_ts, int64_t now, double quiet_hours) {
    if (heavy_ts < 0)
        return true; /* unknown time: keep the old suppression */
    return (double)(now - heavy_ts) < quiet_hours * 3600.0;
}

hu_grief_decay_verdict_t hu_grief_decay_decide(const hu_grief_decay_inbound_t *li, int64_t now,
                                               hu_proposer_context_t *pc) {
    if (!li || !hu_proactive_should_suppress_for_emotion(li->text, li->len))
        return HU_GRIEF_DECAY_NONE;
    hu_gate_mode_t mode = hu_grief_decay_mode();
    if (mode == HU_GATE_OFF)
        return HU_GRIEF_DECAY_SUPPRESS; /* byte-identical to before */
    double quiet_h = hu_grief_decay_quiet_hours();
    int64_t heavy_ts = hu_grief_decay_parse_ts(li->ts);
    bool quiet = hu_grief_decay_in_quiet_window(heavy_ts, now, quiet_h);
    long long age_h = heavy_ts < 0 ? -1 : (long long)((now - heavy_ts) / 3600);
    /* A gentle check-in needs the proposer to see the thread. */
    bool context = pc && pc->mode == HU_GATE_LIVE && pc->local_ok;
    const char *grief_decay = quiet ? "quiet" : (context ? "eligible" : "blocked_no_context");
    hu_log_info("daemon", NULL,
                "[grief_decay %s] heavy=1 age_h=%lld quiet_h=%.0f would_allow=%d context=%d "
                "grief_decay=%s",
                mode == HU_GATE_LIVE ? "live" : "shadow", age_h, quiet_h, quiet ? 0 : 1,
                context ? 1 : 0, grief_decay);
    if (mode != HU_GATE_LIVE || quiet || !context)
        return HU_GRIEF_DECAY_SUPPRESS;
    pc->heavy_inbound_hours_ago = age_h;
    return HU_GRIEF_DECAY_GENTLE;
}

bool hu_grief_decay_skip_extras(hu_grief_decay_verdict_t v) {
    return v == HU_GRIEF_DECAY_GENTLE;
}
