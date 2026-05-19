#ifndef HU_CORE_LOG_H
#define HU_CORE_LOG_H

#include "human/observer.h"
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

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

static inline void hu_log_impl_(const char *component, hu_observer_t *obs, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (obs && obs->vtable && obs->vtable->record_event) {
        hu_observer_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.tag = HU_OBSERVER_EVENT_ERR;
        ev.data.err.component = component;
        ev.data.err.message = buf;
        obs->vtable->record_event(obs->ctx, &ev);
    } else {
        fprintf(stderr, "[%s] %s\n", component, buf);
    }
}

#define hu_log_error(component, obs_ptr, ...) hu_log_impl_((component), (obs_ptr), __VA_ARGS__)
#define hu_log_warn(component, obs_ptr, ...)  hu_log_impl_((component), (obs_ptr), __VA_ARGS__)
#define hu_log_info(component, obs_ptr, ...)  hu_log_impl_((component), (obs_ptr), __VA_ARGS__)

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
