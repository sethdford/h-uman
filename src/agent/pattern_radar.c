/*
 * Pattern radar — tracks recurring topics, emotional trends, and behavioral patterns.
 */
#include "seaclaw/agent/pattern_radar.h"
#include "seaclaw/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SC_PATTERN_THRESHOLD 3

sc_error_t sc_pattern_radar_init(sc_pattern_radar_t *radar, sc_allocator_t alloc) {
    if (!radar)
        return SC_ERR_INVALID_ARGUMENT;
    memset(radar, 0, sizeof(*radar));
    radar->alloc = alloc;
    return SC_OK;
}

static void observation_deinit(sc_pattern_observation_t *obs, sc_allocator_t *alloc) {
    if (!obs || !alloc)
        return;
    if (obs->subject) {
        alloc->free(alloc->ctx, obs->subject, obs->subject_len + 1);
        obs->subject = NULL;
    }
    if (obs->detail) {
        alloc->free(alloc->ctx, obs->detail, obs->detail_len + 1);
        obs->detail = NULL;
    }
    if (obs->first_seen) {
        alloc->free(alloc->ctx, obs->first_seen, strlen(obs->first_seen) + 1);
        obs->first_seen = NULL;
    }
    if (obs->last_seen) {
        alloc->free(alloc->ctx, obs->last_seen, strlen(obs->last_seen) + 1);
        obs->last_seen = NULL;
    }
}

void sc_pattern_radar_deinit(sc_pattern_radar_t *radar) {
    if (!radar)
        return;
    sc_allocator_t *a = &radar->alloc;
    for (size_t i = 0; i < radar->observation_count; i++)
        observation_deinit(&radar->observations[i], a);
    radar->observation_count = 0;
}

static bool observation_matches(const sc_pattern_observation_t *obs, const char *subject,
                                size_t subject_len, sc_pattern_type_t type) {
    if (obs->type != type)
        return false;
    if (obs->subject_len != subject_len)
        return false;
    return memcmp(obs->subject, subject, subject_len) == 0;
}

sc_error_t sc_pattern_radar_observe(sc_pattern_radar_t *radar,
                                     const char *subject, size_t subject_len,
                                     sc_pattern_type_t type,
                                     const char *detail, size_t detail_len,
                                     const char *timestamp, size_t timestamp_len) {
    if (!radar || !subject || subject_len == 0)
        return SC_ERR_INVALID_ARGUMENT;

    sc_allocator_t *a = &radar->alloc;

    /* Search for existing matching observation */
    for (size_t i = 0; i < radar->observation_count; i++) {
        sc_pattern_observation_t *obs = &radar->observations[i];
        if (observation_matches(obs, subject, subject_len, type)) {
            obs->occurrence_count++;
            if (timestamp && timestamp_len > 0 && obs->last_seen) {
                a->free(a->ctx, obs->last_seen, strlen(obs->last_seen) + 1);
                obs->last_seen = NULL;
            }
            if (timestamp && timestamp_len > 0) {
                obs->last_seen = sc_strndup(a, timestamp, timestamp_len);
                if (!obs->last_seen)
                    return SC_ERR_OUT_OF_MEMORY;
            }
            return SC_OK;
        }
    }

    /* At capacity: skip new entry */
    if (radar->observation_count >= SC_PATTERN_MAX_OBSERVATIONS)
        return SC_OK;

    /* Add new observation */
    sc_pattern_observation_t *obs = &radar->observations[radar->observation_count];
    obs->type = type;
    obs->subject = sc_strndup(a, subject, subject_len);
    if (!obs->subject)
        return SC_ERR_OUT_OF_MEMORY;
    obs->subject_len = subject_len;

    obs->detail = (detail && detail_len > 0) ? sc_strndup(a, detail, detail_len) : NULL;
    obs->detail_len = obs->detail ? detail_len : 0;

    obs->occurrence_count = 1;
    obs->confidence = 1.0;

    if (timestamp && timestamp_len > 0) {
        obs->first_seen = sc_strndup(a, timestamp, timestamp_len);
        obs->last_seen = sc_strndup(a, timestamp, timestamp_len);
        if (!obs->first_seen || !obs->last_seen) {
            observation_deinit(obs, a);
            return SC_ERR_OUT_OF_MEMORY;
        }
    } else {
        obs->first_seen = NULL;
        obs->last_seen = NULL;
    }

    radar->observation_count++;
    return SC_OK;
}

sc_error_t sc_pattern_radar_build_context(sc_pattern_radar_t *radar, sc_allocator_t *alloc,
                                           char **out, size_t *out_len) {
    if (!radar || !alloc || !out || !out_len)
        return SC_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    size_t cap = 256;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf)
        return SC_ERR_OUT_OF_MEMORY;
    size_t len = 0;
    buf[0] = '\0';

    bool first = true;
    for (size_t i = 0; i < radar->observation_count; i++) {
        const sc_pattern_observation_t *obs = &radar->observations[i];
        if (obs->occurrence_count < SC_PATTERN_THRESHOLD)
            continue;

        if (first) {
            const char *header = "### Pattern Insights\n\n";
            size_t hlen = strlen(header);
            while (len + hlen + 1 > cap) {
                size_t new_cap = cap * 2;
                char *nb = (char *)alloc->realloc(alloc->ctx, buf, cap, new_cap);
                if (!nb) {
                    alloc->free(alloc->ctx, buf, cap);
                    return SC_ERR_OUT_OF_MEMORY;
                }
                buf = nb;
                cap = new_cap;
            }
            memcpy(buf + len, header, hlen + 1);
            len += hlen;
            first = false;
        }

        char line[512];
        const char *first_str = obs->first_seen ? obs->first_seen : "unknown";
        const char *last_str = obs->last_seen ? obs->last_seen : "unknown";
        int n = snprintf(line, sizeof(line),
                        "- Topic \"%.*s\" has come up %u times (first: %s, last: %s)\n",
                        (int)obs->subject_len, obs->subject, obs->occurrence_count, first_str,
                        last_str);
        if (n > 0 && (size_t)n < sizeof(line)) {
            size_t llen = (size_t)n;
            while (len + llen + 1 > cap) {
                size_t new_cap = cap * 2;
                char *nb = (char *)alloc->realloc(alloc->ctx, buf, cap, new_cap);
                if (!nb) {
                    alloc->free(alloc->ctx, buf, cap);
                    return SC_ERR_OUT_OF_MEMORY;
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
        return SC_OK;
    }

    *out = buf;
    *out_len = len;
    return SC_OK;
}
