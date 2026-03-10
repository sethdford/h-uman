/*
 * Superhuman predictive coaching service — wraps pattern radar.
 */
#include "human/agent/pattern_radar.h"
#include "human/agent/superhuman.h"
#include "human/agent/superhuman_predictive.h"
#include "human/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HU_PREDICTIVE_THRESHOLD 3

static hu_error_t predictive_build_context(void *ctx, hu_allocator_t *alloc, char **out,
                                             size_t *out_len) {
    hu_pattern_radar_t *radar = (hu_pattern_radar_t *)ctx;
    if (!radar || !alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    size_t cap = 256;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    size_t len = 0;
    buf[0] = '\0';

    for (size_t i = 0; i < radar->observation_count; i++) {
        const hu_pattern_observation_t *obs = &radar->observations[i];
        if (obs->occurrence_count < HU_PREDICTIVE_THRESHOLD)
            continue;

        char line[512];
        int n = snprintf(line, sizeof(line),
                         "Recurring patterns detected: %.*s has been mentioned %u times. "
                         "Consider gently surfacing this pattern to the user.\n",
                         (int)obs->subject_len, obs->subject ? obs->subject : "",
                         obs->occurrence_count);
        if (n > 0 && (size_t)n < sizeof(line)) {
            size_t llen = (size_t)n;
            while (len + llen + 1 > cap) {
                size_t new_cap = cap * 2;
                char *nb = (char *)alloc->realloc(alloc->ctx, buf, cap, new_cap);
                if (!nb) {
                    alloc->free(alloc->ctx, buf, cap);
                    return HU_ERR_OUT_OF_MEMORY;
                }
                buf = nb;
                cap = new_cap;
            }
            memcpy(buf + len, line, llen + 1);
            len += llen;
        }
    }

    if (len == 0) {
        alloc->free(alloc->ctx, buf, cap);
        *out = NULL;
        *out_len = 0;
        return HU_OK;
    }

    *out = buf;
    *out_len = len;
    return HU_OK;
}

static hu_error_t predictive_observe(void *ctx, hu_allocator_t *alloc, const char *text,
                                      size_t text_len, const char *role, size_t role_len) {
    (void)ctx;
    (void)alloc;
    (void)text;
    (void)text_len;
    (void)role;
    (void)role_len;
    return HU_OK;
}

hu_error_t hu_superhuman_predictive_service(hu_pattern_radar_t *radar,
                                             hu_superhuman_service_t *out) {
    if (!radar || !out)
        return HU_ERR_INVALID_ARGUMENT;
    out->name = "Predictive Coaching";
    out->build_context = predictive_build_context;
    out->observe = predictive_observe;
    out->ctx = radar;
    return HU_OK;
}
