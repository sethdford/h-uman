/*
 * Pattern radar — tracks recurring topics, emotional trends, and behavioral patterns.
 */
#include "human/agent/pattern_radar.h"
#include "human/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HU_PATTERN_THRESHOLD 3

hu_error_t hu_pattern_radar_init(hu_pattern_radar_t *radar, hu_allocator_t alloc) {
    if (!radar)
        return HU_ERR_INVALID_ARGUMENT;
    memset(radar, 0, sizeof(*radar));
    radar->alloc = alloc;
    return HU_OK;
}

static void observation_deinit(hu_pattern_observation_t *obs, hu_allocator_t *alloc) {
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

void hu_pattern_radar_deinit(hu_pattern_radar_t *radar) {
    if (!radar)
        return;
    hu_allocator_t *a = &radar->alloc;
    for (size_t i = 0; i < radar->observation_count; i++)
        observation_deinit(&radar->observations[i], a);
    radar->observation_count = 0;
}

static bool observation_matches(const hu_pattern_observation_t *obs, const char *subject,
                                size_t subject_len, hu_pattern_type_t type) {
    if (obs->type != type)
        return false;
    if (obs->subject_len != subject_len)
        return false;
    return memcmp(obs->subject, subject, subject_len) == 0;
}

hu_error_t hu_pattern_radar_observe(hu_pattern_radar_t *radar,
                                     const char *subject, size_t subject_len,
                                     hu_pattern_type_t type,
                                     const char *detail, size_t detail_len,
                                     const char *timestamp, size_t timestamp_len) {
    if (!radar || !subject || subject_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    hu_allocator_t *a = &radar->alloc;

    /* Search for existing matching observation */
    for (size_t i = 0; i < radar->observation_count; i++) {
        hu_pattern_observation_t *obs = &radar->observations[i];
        if (observation_matches(obs, subject, subject_len, type)) {
            obs->occurrence_count++;
            if (timestamp && timestamp_len > 0 && obs->last_seen) {
                a->free(a->ctx, obs->last_seen, strlen(obs->last_seen) + 1);
                obs->last_seen = NULL;
            }
            if (timestamp && timestamp_len > 0) {
                obs->last_seen = hu_strndup(a, timestamp, timestamp_len);
                if (!obs->last_seen)
                    return HU_ERR_OUT_OF_MEMORY;
            }
            return HU_OK;
        }
    }

    /* At capacity: skip new entry */
    if (radar->observation_count >= HU_PATTERN_MAX_OBSERVATIONS)
        return HU_OK;

    /* Add new observation */
    hu_pattern_observation_t *obs = &radar->observations[radar->observation_count];
    obs->type = type;
    obs->subject = hu_strndup(a, subject, subject_len);
    if (!obs->subject)
        return HU_ERR_OUT_OF_MEMORY;
    obs->subject_len = subject_len;

    obs->detail = (detail && detail_len > 0) ? hu_strndup(a, detail, detail_len) : NULL;
    obs->detail_len = obs->detail ? detail_len : 0;

    obs->occurrence_count = 1;
    obs->confidence = 1.0;

    if (timestamp && timestamp_len > 0) {
        obs->first_seen = hu_strndup(a, timestamp, timestamp_len);
        obs->last_seen = hu_strndup(a, timestamp, timestamp_len);
        if (!obs->first_seen || !obs->last_seen) {
            observation_deinit(obs, a);
            return HU_ERR_OUT_OF_MEMORY;
        }
    } else {
        obs->first_seen = NULL;
        obs->last_seen = NULL;
    }

    radar->observation_count++;
    return HU_OK;
}

hu_error_t hu_pattern_radar_build_context(hu_pattern_radar_t *radar, hu_allocator_t *alloc,
                                           char **out, size_t *out_len) {
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

    bool first = true;
    for (size_t i = 0; i < radar->observation_count; i++) {
        const hu_pattern_observation_t *obs = &radar->observations[i];
        if (obs->occurrence_count < HU_PATTERN_THRESHOLD)
            continue;

        if (first) {
            const char *header = "### Pattern Insights\n\n";
            size_t hlen = strlen(header);
            while (len + hlen + 1 > cap) {
                size_t new_cap = cap * 2;
                char *nb = (char *)alloc->realloc(alloc->ctx, buf, cap, new_cap);
                if (!nb) {
                    alloc->free(alloc->ctx, buf, cap);
                    return HU_ERR_OUT_OF_MEMORY;
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
