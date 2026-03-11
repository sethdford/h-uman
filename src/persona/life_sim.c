/*
 * F59 — Parallel Life Simulation.
 * Simulated daily routine from persona. Time blocks: activity, availability, mood_modifier.
 */
#include "human/core/allocator.h"
#include "human/platform.h"
#include "human/persona/life_sim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *DEFAULT_ACTIVITY = "idle";
static const char *DEFAULT_AVAILABILITY = "available";
static const char *DEFAULT_MOOD = "neutral";
static const float DEFAULT_AVAILABILITY_FACTOR = 0.5f;

static float availability_to_factor(const char *av) {
    if (!av)
        return DEFAULT_AVAILABILITY_FACTOR;
    if (strcmp(av, "available") == 0)
        return 0.5f;
    if (strcmp(av, "brief") == 0)
        return 1.0f;
    if (strcmp(av, "slow") == 0)
        return 2.0f;
    if (strcmp(av, "unavailable") == 0)
        return 5.0f;
    return DEFAULT_AVAILABILITY_FACTOR;
}

/* Parse "HH:MM" to minutes since midnight. Returns -1 on parse failure. */
static int parse_time_to_minutes(const char *time_str) {
    if (!time_str)
        return -1;
    int h = 0, m = 0;
    if (sscanf(time_str, "%d:%d", &h, &m) != 2)
        return -1;
    if (h < 0 || h > 23 || m < 0 || m > 59)
        return -1;
    return h * 60 + m;
}

/* Get block at index, or next block's start for end-of-day. */
static int block_start_minutes(const hu_routine_block_t *blocks, size_t count, size_t idx) {
    if (idx >= count)
        return 24 * 60; /* end of day */
    return parse_time_to_minutes(blocks[idx].time);
}

hu_life_sim_state_t hu_life_sim_get_current(const hu_daily_routine_t *routine, int64_t now_ts,
                                            int day_of_week, uint32_t seed) {
    hu_life_sim_state_t out = {
        .activity = DEFAULT_ACTIVITY,
        .availability = DEFAULT_AVAILABILITY,
        .mood_modifier = DEFAULT_MOOD,
        .availability_factor = DEFAULT_AVAILABILITY_FACTOR,
    };

    if (!routine)
        return out;

    const hu_routine_block_t *blocks = NULL;
    size_t count = 0;
    /* day_of_week: 0=Sun, 1=Mon, ..., 6=Sat. Weekday = 1-5, weekend = 0,6 */
    if (day_of_week >= 1 && day_of_week <= 5) {
        blocks = routine->weekday;
        count = routine->weekday_count;
    } else {
        blocks = routine->weekend;
        count = routine->weekend_count;
    }

    if (!blocks || count == 0)
        return out;

    time_t t = (time_t)now_ts;
    struct tm tm_buf;
    struct tm *tm = hu_platform_localtime_r(&t, &tm_buf);
    if (!tm)
        return out;

    int hour = tm->tm_hour;
    int minute = tm->tm_min;
    int minutes_since_midnight = hour * 60 + minute;

    /* Apply ±15% variance with seed. 15% of 60 min = 9 min. */
    float variance = routine->routine_variance;
    if (variance <= 0.f || variance > 1.f)
        variance = 0.15f;
    int jitter_max = (int)(60.f * variance + 0.5f);
    int jitter = (int)(seed % (2u * (unsigned)jitter_max + 1u)) - jitter_max;
    int effective_minutes = minutes_since_midnight + jitter;
    if (effective_minutes < 0)
        effective_minutes += 24 * 60;
    if (effective_minutes >= 24 * 60)
        effective_minutes -= 24 * 60;

    for (size_t i = 0; i < count; i++) {
        int start = block_start_minutes(blocks, count, i);
        int end = block_start_minutes(blocks, count, i + 1);
        if (effective_minutes >= start && effective_minutes < end) {
            const hu_routine_block_t *b = &blocks[i];
            out.activity = (b->activity[0] != '\0') ? b->activity : DEFAULT_ACTIVITY;
            out.availability = (b->availability[0] != '\0') ? b->availability : DEFAULT_AVAILABILITY;
            out.mood_modifier = (b->mood_modifier[0] != '\0') ? b->mood_modifier : DEFAULT_MOOD;
            out.availability_factor = availability_to_factor(b->availability);
            return out;
        }
    }
    return out;
}

char *hu_life_sim_build_context(hu_allocator_t *alloc, const hu_life_sim_state_t *state,
                                 size_t *out_len) {
    if (!alloc || !state || !out_len)
        return NULL;

    const char *act = state->activity ? state->activity : "idle";
    const char *av = state->availability ? state->availability : "available";
    const char *mood = state->mood_modifier ? state->mood_modifier : "neutral";

    size_t cap = 256;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf)
        return NULL;

    int n = snprintf(buf, cap,
                     "[LIFE CONTEXT: You just finished %s. Availability: %s. Mood: %s.]", act, av,
                     mood);
    if (n <= 0 || (size_t)n >= cap) {
        alloc->free(alloc->ctx, buf, cap);
        return NULL;
    }

    size_t need = (size_t)n + 1;
    char *shrunk = (char *)alloc->realloc(alloc->ctx, buf, cap, need);
    if (!shrunk) {
        alloc->free(alloc->ctx, buf, cap);
        return NULL;
    }
    *out_len = (size_t)n;
    return shrunk;
}
