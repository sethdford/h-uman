#include "human/context/cognitive_load.h"
#include "human/platform.h"

static float base_capacity_from_hour(const hu_cognitive_load_config_t *config, int hour) {
    const int ps = config->peak_hour_start;
    const int pe = config->peak_hour_end;
    const int ls = config->low_hour_start;
    const int le = config->low_hour_end;

    const int in_peak = (hour >= ps && hour <= pe);
    const int in_low = (ls <= le)
        ? (hour >= ls && hour <= le)
        : (hour >= ls || hour <= le);

    if (in_peak)
        return 1.0f;
    if (in_low)
        return 0.3f;

    /* Transition: linear interpolation between peak and low */
    if (hour > pe && hour < ls) {
        /* Between peak end and low start (e.g. 18–21 when peak 9–17, low 22–6) */
        const int range = ls - pe;
        const int dist = hour - pe;
        return 1.0f - 0.7f * (float)dist / (float)range;
    }
    if (hour > le && hour < ps) {
        /* Between low end and peak start (e.g. 7–8 when low 22–6, peak 9–17) */
        const int range = ps - le;
        const int dist = hour - le;
        return 0.3f + 0.7f * (float)dist / (float)range;
    }
    /* Fallback: assume transition */
    return 0.65f;
}

hu_cognitive_load_state_t hu_cognitive_load_calculate(
    const hu_cognitive_load_config_t *config,
    int conversation_depth,
    time_t now) {

    hu_cognitive_load_state_t state = {0};
    state.capacity = 0.65f;
    state.conversation_depth = conversation_depth;
    state.hour_of_day = 0;
    state.day_of_week = 0;
    state.physical_state = NULL;

    if (!config)
        return state;

    struct tm tm_buf;
    struct tm *tm = hu_platform_localtime_r(&now, &tm_buf);
    if (!tm)
        return state;

    const int hour = tm->tm_hour;
    const int dow = tm->tm_wday; /* 0=Sun, 1=Mon, ..., 6=Sat */

    state.hour_of_day = hour;
    state.day_of_week = dow;

    float base = base_capacity_from_hour(config, hour);

    /* Conversation fatigue: reduce by 0.05 per exchange beyond fatigue_threshold */
    int excess = conversation_depth - config->fatigue_threshold;
    if (excess > 0) {
        float reduction = 0.05f * (float)excess;
        base -= reduction;
    }

    /* Monday: subtract monday_penalty */
    if (dow == 1)
        base -= config->monday_penalty;

    /* Friday: add friday_bonus */
    if (dow == 5)
        base += config->friday_bonus;

    /* Clamp to [0.1, 1.0] */
    if (base < 0.1f)
        base = 0.1f;
    if (base > 1.0f)
        base = 1.0f;

    state.capacity = base;
    return state;
}

const char *hu_cognitive_load_prompt_hint(const hu_cognitive_load_state_t *state) {
    if (!state)
        return NULL;
    float c = state->capacity;
    if (c > 0.8f)
        return NULL;
    if (c >= 0.5f)
        return "[COGNITIVE STATE: Slightly tired. Keep responses natural but don't overthink.]";
    if (c >= 0.3f)
        return "[COGNITIVE STATE: Tired. Shorter sentences. Simpler words. Don't try to be clever.]";
    return "[COGNITIVE STATE: Exhausted. Minimal engagement. One-line responses OK. Typos acceptable.]";
}

int hu_cognitive_load_max_response_length(const hu_cognitive_load_state_t *state) {
    if (!state)
        return 120;
    float c = state->capacity;
    return (int)(40.0f + (200.0f - 40.0f) * c);
}
