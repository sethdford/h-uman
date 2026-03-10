/*
 * Circadian persona overlay — time-of-day adaptive guidance.
 */
#include "seaclaw/persona/circadian.h"
#include "seaclaw/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *PHASE_NAMES[] = {
    "early morning", "morning", "afternoon", "evening", "night", "late night",
};

/* Default guidance per phase. Persona-specific overrides via time_overlays; future config can
 * externalize. */
static const char *PHASE_GUIDANCE[] = {
    "Be gentle and warm. The user is starting their day. Keep responses calm and encouraging.",
    "Be energetic and productive. The user is at peak mental clarity. Be direct and efficient.",
    "Be steady and focused. Energy may be dipping. Keep things clear and structured.",
    "Be relaxed and reflective. The day is winding down. Allow for deeper conversation.",
    "Be calm and intimate. The user is in a quieter headspace. Slow your pace, be thoughtful.",
    ("Be present and unhurried. Late night conversations often carry more weight. Be a quiet "
     "companion."),
};

sc_time_phase_t sc_circadian_phase(uint8_t hour) {
    if (hour < 5)
        return SC_PHASE_LATE_NIGHT;
    if (hour < 8)
        return SC_PHASE_EARLY_MORNING;
    if (hour < 12)
        return SC_PHASE_MORNING;
    if (hour < 17)
        return SC_PHASE_AFTERNOON;
    if (hour < 21)
        return SC_PHASE_EVENING;
    /* 21:00-23:59 */
    return SC_PHASE_NIGHT;
}

sc_error_t sc_circadian_build_prompt(sc_allocator_t *alloc, uint8_t hour, char **out,
                                     size_t *out_len) {
    if (!alloc || !out || !out_len)
        return SC_ERR_INVALID_ARGUMENT;

    sc_time_phase_t phase = sc_circadian_phase(hour);
    const char *name = PHASE_NAMES[(size_t)phase];
    const char *guidance = PHASE_GUIDANCE[(size_t)phase];

#define SC_CIRCADIAN_BUF_CAP 256
    char *buf = (char *)alloc->alloc(alloc->ctx, SC_CIRCADIAN_BUF_CAP);
    if (!buf)
        return SC_ERR_OUT_OF_MEMORY;

    int n = snprintf(buf, SC_CIRCADIAN_BUF_CAP, "\n### Time Awareness\nCurrent phase: %s. %s\n",
                     name, guidance);
    if (n <= 0 || (size_t)n >= SC_CIRCADIAN_BUF_CAP) {
        alloc->free(alloc->ctx, buf, SC_CIRCADIAN_BUF_CAP);
        return SC_ERR_INVALID_ARGUMENT;
    }

    size_t need = (size_t)n + 1;
    char *shrunk = (char *)alloc->realloc(alloc->ctx, buf, SC_CIRCADIAN_BUF_CAP, need);
    if (!shrunk) {
        alloc->free(alloc->ctx, buf, SC_CIRCADIAN_BUF_CAP);
        return SC_ERR_OUT_OF_MEMORY;
    }
    *out = shrunk;
    *out_len = (size_t)n;
#undef SC_CIRCADIAN_BUF_CAP
    return SC_OK;
}
