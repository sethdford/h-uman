/* Log emitter — one compiled copy of the level filter and line formatter.
 *
 * eb4b7b8e7 added levels + timestamps as `static inline` functions in
 * include/human/core/log.h. Every one of the ~150 logging translation units
 * then carried its own copy of the parser, the threshold, the formatter and
 * the emitter; LTO does not fold them, and the MinSizeRel binary grew 49 KB
 * past its 2600 KB budget (measured 2026-09-02: 2,626,720 → 2,676,304 B).
 * The header keeps the macros and the tiny once-guard; everything with a
 * body lives here. */
#include "human/core/log.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

hu_log_level_t hu_log_level_parse(const char *s) {
    if (!s || !*s)
        return HU_LOG_LEVEL_INFO;
    if (strcmp(s, "error") == 0 || strcmp(s, "ERROR") == 0)
        return HU_LOG_LEVEL_ERROR;
    if (strcmp(s, "warn") == 0 || strcmp(s, "WARN") == 0 || strcmp(s, "warning") == 0)
        return HU_LOG_LEVEL_WARN;
    return HU_LOG_LEVEL_INFO;
}

/* Process-wide threshold, read from $HU_LOG_LEVEL once (first log call).
 * -1 = not yet read. Tests may reset it via hu_log_level_set_for_test(). */
static atomic_int s_log_level_slot = -1;

hu_log_level_t hu_log_level_threshold(void) {
    int v = atomic_load(&s_log_level_slot);
    if (v < 0) {
        v = (int)hu_log_level_parse(getenv("HU_LOG_LEVEL"));
        atomic_store(&s_log_level_slot, v);
    }
    return (hu_log_level_t)v;
}

void hu_log_level_set_for_test(int level_or_minus1) {
    atomic_store(&s_log_level_slot, level_or_minus1);
}

bool hu_log_level_enabled(hu_log_level_t level) {
    return (int)level <= (int)hu_log_level_threshold();
}

int hu_log_format_line(char *out, size_t cap, hu_log_level_t level, const char *component,
                       const char *msg, time_t now) {
    char ts[24] = "0000-00-00T00:00:00";
    if (now > 0) {
        struct tm tmv;
        if (localtime_r(&now, &tmv))
            strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tmv);
    }
    const char *lvl = level == HU_LOG_LEVEL_ERROR  ? "ERROR"
                      : level == HU_LOG_LEVEL_WARN ? "WARN "
                                                   : "INFO ";
    return snprintf(out, cap, "%s %s [%s] %s\n", ts, lvl, component ? component : "?",
                    msg ? msg : "");
}

void hu_log_vimpl_(hu_log_level_t level, const char *component, hu_observer_t *obs, const char *fmt,
                   va_list ap) {
    if (!hu_log_level_enabled(level))
        return;
    char buf[512];
    (void)vsnprintf(buf, sizeof(buf), fmt, ap);
    if (obs && obs->vtable && obs->vtable->record_event) {
        hu_observer_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.tag = HU_OBSERVER_EVENT_ERR;
        ev.data.err.component = component;
        ev.data.err.message = buf;
        obs->vtable->record_event(obs->ctx, &ev);
    } else {
        char line[640];
        hu_log_format_line(line, sizeof(line), level, component, buf, time(NULL));
        fputs(line, stderr);
    }
}

void hu_log_impl_(const char *component, hu_observer_t *obs, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    hu_log_vimpl_(HU_LOG_LEVEL_INFO, component, obs, fmt, ap);
    va_end(ap);
}

void hu_log_lvl_impl_(hu_log_level_t level, const char *component, hu_observer_t *obs,
                      const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    hu_log_vimpl_(level, component, obs, fmt, ap);
    va_end(ap);
}
