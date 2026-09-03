#ifndef HU_CORE_LOG_H
#define HU_CORE_LOG_H

#include "human/observer.h"
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/**
 * Structured logging that routes through hu_observer_t when available,
 * falling back to fprintf(stderr) when no observer is set.
 *
 * Usage:
 *   hu_log_error("daemon", obs, "failed: %s", detail);
 *   hu_log_warn("agent", obs, "degraded path: %s", detail);
 *   hu_log_info("human", obs, "check-in sent to %s", name);
 *
 * The observer pointer may be NULL (triggers fallback).
 */

/* Severity of a log line. The daemon's stderr is a launchd-appended file with
 * no other structure; until 2026-09-02 every line was "[component] msg" with
 * no time and no level, so a 530k-line log could not answer "when" or "how
 * bad" (the 09-01 replay post-mortem had to be reconstructed from chat.db). */
typedef enum hu_log_level {
    HU_LOG_LEVEL_ERROR = 0,
    HU_LOG_LEVEL_WARN = 1,
    HU_LOG_LEVEL_INFO = 2,
} hu_log_level_t;

/* Parse $HU_LOG_LEVEL ("error" | "warn" | "info"; anything else → info). */
static inline hu_log_level_t hu_log_level_parse(const char *s) {
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
static inline atomic_int *hu_log_level_slot_(void) {
    static atomic_int slot = -1;
    return &slot;
}
static inline hu_log_level_t hu_log_level_threshold(void) {
    int v = atomic_load(hu_log_level_slot_());
    if (v < 0) {
        v = (int)hu_log_level_parse(getenv("HU_LOG_LEVEL"));
        atomic_store(hu_log_level_slot_(), v);
    }
    return (hu_log_level_t)v;
}
static inline void hu_log_level_set_for_test(int level_or_minus1) {
    atomic_store(hu_log_level_slot_(), level_or_minus1);
}
static inline bool hu_log_level_enabled(hu_log_level_t level) {
    return (int)level <= (int)hu_log_level_threshold();
}

/* Pure line formatter for the stderr fallback:
 *   "2026-09-02T02:57:16 WARN  [imessage] msg\n"
 * `now` is local time; a zero `now` writes "0000-00-00T00:00:00" (tests).
 * Returns the number of bytes written (excluding NUL), truncated to cap. */
static inline int hu_log_format_line(char *out, size_t cap, hu_log_level_t level,
                                     const char *component, const char *msg, time_t now) {
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

static inline void hu_log_vimpl_(hu_log_level_t level, const char *component, hu_observer_t *obs,
                                 const char *fmt, va_list ap) {
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

static inline void hu_log_impl_(const char *component, hu_observer_t *obs, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    hu_log_vimpl_(HU_LOG_LEVEL_INFO, component, obs, fmt, ap);
    va_end(ap);
}
static inline void hu_log_lvl_impl_(hu_log_level_t level, const char *component, hu_observer_t *obs,
                                    const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    hu_log_vimpl_(level, component, obs, fmt, ap);
    va_end(ap);
}
#define hu_log_error(component, obs_ptr, ...) \
    hu_log_lvl_impl_(HU_LOG_LEVEL_ERROR, (component), (obs_ptr), __VA_ARGS__)
#define hu_log_warn(component, obs_ptr, ...) \
    hu_log_lvl_impl_(HU_LOG_LEVEL_WARN, (component), (obs_ptr), __VA_ARGS__)
#define hu_log_info(component, obs_ptr, ...) \
    hu_log_lvl_impl_(HU_LOG_LEVEL_INFO, (component), (obs_ptr), __VA_ARGS__)

/**
 * Emit a log line at most once per process lifetime.
 *
 * Each call site MUST own its own static atomic_bool guard, initialized
 * to false. The first invocation flips the guard and emits the line;
 * subsequent invocations of the same guard are no-ops.
 *
 *   static atomic_bool warned_reaction_disabled = false;
 *   hu_log_info_once(&warned_reaction_disabled, "daemon", obs,
 *                    "reaction_collection disabled "
 *                    "(cfg->reaction_collection.enabled=false) — set "
 *                    "reaction_collection.enabled=true in config.json to activate");
 *
 * Intended for config-gated background subsystems whose normal disabled
 * path silently returns HU_OK, AND for stub backends in factories that
 * silently return NOT_SUPPORTED. See
 * ~/.claude/rules/silent-config-gated-subsystems.md — the message MUST
 * name the config key/value the operator needs to fix.
 *
 * The guard is process-scoped; in tests, reset it to false between
 * invocations via atomic_store if you need to re-arm the warning.
 */
static inline bool hu_log_once_check_(atomic_bool *guard) {
    bool expected = false;
    return atomic_compare_exchange_strong(guard, &expected, true);
}

#define hu_log_info_once(guard_ptr, component, obs_ptr, ...)   \
    do {                                                       \
        if (hu_log_once_check_(guard_ptr)) {                   \
            hu_log_impl_((component), (obs_ptr), __VA_ARGS__); \
        }                                                      \
    } while (0)

#define hu_log_warn_once(guard_ptr, component, obs_ptr, ...) \
    hu_log_info_once(guard_ptr, component, obs_ptr, __VA_ARGS__)

#endif /* HU_CORE_LOG_H */
