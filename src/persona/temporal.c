/*
 * Temporal reasoning — seasonal awareness, anniversary detection, life transitions.
 */
#include "human/persona/temporal.h"
#include "human/core/string.h"
#include <stdio.h>
#include <string.h>

hu_season_t hu_temporal_season(int month) {
    if (month < 1 || month > 12)
        return HU_SEASON_SPRING; /* default for invalid */
    if (month >= 3 && month <= 5)
        return HU_SEASON_SPRING;
    if (month >= 6 && month <= 8)
        return HU_SEASON_SUMMER;
    if (month >= 9 && month <= 11)
        return HU_SEASON_AUTUMN;
    return HU_SEASON_WINTER; /* 12, 1, 2 */
}

const char *hu_temporal_season_name(hu_season_t season) {
    switch (season) {
    case HU_SEASON_SPRING:
        return "spring";
    case HU_SEASON_SUMMER:
        return "summer";
    case HU_SEASON_AUTUMN:
        return "autumn";
    case HU_SEASON_WINTER:
        return "winter";
    }
    return "spring";
}

static bool is_leap_year(int year) {
    return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}

/* Days in month, leap-year aware */
static int days_in_month(int year, int m) {
    static const int DAYS[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12)
        return 30;
    if (m == 2 && is_leap_year(year))
        return 29;
    return DAYS[m];
}

/* Day of year (1-based, leap-year aware). Returns 0 for invalid month. */
static int day_of_year(int year, int month, int day) {
    if (month < 1)
        return 0;
    int doy = 0;
    for (int m = 1; m < month; m++)
        doy += days_in_month(year, m);
    return doy + day;
}

/* Days between two month/day pairs, wrapping around year boundary */
static int days_until(int year, int from_month, int from_day, int to_month, int to_day) {
    int from_doy = day_of_year(year, from_month, from_day);
    int to_doy = day_of_year(year, to_month, to_day);
    int diff = to_doy - from_doy;
    if (diff < 0)
        diff += (is_leap_year(year) ? 366 : 365);
    return diff;
}

size_t hu_temporal_check_anniversaries(const hu_date_entry_t *dates, size_t date_count,
                                       int current_year, int current_month, int current_day,
                                       int window_days, hu_anniversary_t *out, size_t out_cap) {
    if (!dates || date_count == 0 || !out || out_cap == 0)
        return 0;
    if (current_month < 1 || current_month > 12 || current_day < 1 || current_day > 31)
        return 0;

    size_t found = 0;
    for (size_t i = 0; i < date_count && found < out_cap; i++) {
        if (dates[i].month < 1 || dates[i].month > 12)
            continue;
        if (dates[i].day < 1 || dates[i].day > 31)
            continue;

        int ann_month = dates[i].month;
        int ann_day = dates[i].day;
        /* Feb 29 anniversary in a non-leap year: treat as Feb 28 */
        if (ann_month == 2 && ann_day == 29 && !is_leap_year(current_year))
            ann_day = 28;

        int dist = days_until(current_year, current_month, current_day, ann_month, ann_day);
        if (dist <= window_days) {
            out[found].label = (char *)dates[i].label; /* borrowed pointer */
            out[found].label_len = dates[i].label_len;
            out[found].month = dates[i].month;
            out[found].day = dates[i].day;
            out[found].days_away = dist;
            found++;
        }
    }
    return found;
}

/* Case-insensitive substring check */
typedef struct {
    const char *pattern;
    size_t pattern_len;
    hu_life_transition_t transition;
} transition_pattern_t;

static const transition_pattern_t TRANSITION_PATTERNS[] = {
    {"new job", 7, HU_TRANSITION_JOB_CHANGE},
    {"got fired", 9, HU_TRANSITION_JOB_CHANGE},
    {"got laid off", 12, HU_TRANSITION_JOB_CHANGE},
    {"starting a new position", 23, HU_TRANSITION_JOB_CHANGE},
    {"quit my job", 11, HU_TRANSITION_JOB_CHANGE},
    {"career change", 13, HU_TRANSITION_JOB_CHANGE},
    {"moving to", 9, HU_TRANSITION_MOVE},
    {"just moved", 10, HU_TRANSITION_MOVE},
    {"new apartment", 13, HU_TRANSITION_MOVE},
    {"new house", 9, HU_TRANSITION_MOVE},
    {"relocating", 10, HU_TRANSITION_MOVE},
    {"broke up", 8, HU_TRANSITION_BREAKUP},
    {"breakup", 7, HU_TRANSITION_BREAKUP},
    {"divorce", 7, HU_TRANSITION_BREAKUP},
    {"split up", 8, HU_TRANSITION_BREAKUP},
    {"expecting a baby", 16, HU_TRANSITION_NEW_BABY},
    {"pregnant", 8, HU_TRANSITION_NEW_BABY},
    {"new baby", 8, HU_TRANSITION_NEW_BABY},
    {"just had a baby", 15, HU_TRANSITION_NEW_BABY},
    {"graduating", 10, HU_TRANSITION_GRADUATION},
    {"graduation", 10, HU_TRANSITION_GRADUATION},
    {"finished school", 15, HU_TRANSITION_GRADUATION},
    {"got my degree", 13, HU_TRANSITION_GRADUATION},
    {"retiring", 8, HU_TRANSITION_RETIREMENT},
    {"retirement", 10, HU_TRANSITION_RETIREMENT},
    {"last day at work", 16, HU_TRANSITION_RETIREMENT},
    {"diagnosed", 9, HU_TRANSITION_HEALTH_EVENT},
    {"surgery", 7, HU_TRANSITION_HEALTH_EVENT},
    {"hospitalized", 12, HU_TRANSITION_HEALTH_EVENT},
    {"passed away", 11, HU_TRANSITION_LOSS},
    {"died", 4, HU_TRANSITION_LOSS},
    {"funeral", 7, HU_TRANSITION_LOSS},
    {"lost my", 7, HU_TRANSITION_LOSS},
};

hu_life_transition_t hu_temporal_detect_life_transition(const hu_temporal_message_t *messages,
                                                        size_t count) {
    if (!messages || count == 0)
        return HU_TRANSITION_NONE;

    size_t pattern_count = sizeof(TRANSITION_PATTERNS) / sizeof(TRANSITION_PATTERNS[0]);
    for (size_t i = 0; i < count; i++) {
        if (!messages[i].text || messages[i].text_len == 0)
            continue;
        for (size_t p = 0; p < pattern_count; p++) {
            if (hu_str_contains_ci(messages[i].text, messages[i].text_len, TRANSITION_PATTERNS[p].pattern,
                            TRANSITION_PATTERNS[p].pattern_len)) {
                return TRANSITION_PATTERNS[p].transition;
            }
        }
    }
    return HU_TRANSITION_NONE;
}

static const char *transition_name(hu_life_transition_t t) {
    switch (t) {
    case HU_TRANSITION_JOB_CHANGE:
        return "job change";
    case HU_TRANSITION_MOVE:
        return "relocation";
    case HU_TRANSITION_BREAKUP:
        return "relationship change";
    case HU_TRANSITION_NEW_BABY:
        return "new baby";
    case HU_TRANSITION_GRADUATION:
        return "graduation";
    case HU_TRANSITION_RETIREMENT:
        return "retirement";
    case HU_TRANSITION_HEALTH_EVENT:
        return "health event";
    case HU_TRANSITION_LOSS:
        return "loss";
    case HU_TRANSITION_NONE:
        return NULL;
    }
    return NULL;
}

hu_error_t hu_temporal_build_context(hu_allocator_t *alloc, int month, int day,
                                     const hu_anniversary_t *anniversaries, size_t ann_count,
                                     hu_life_transition_t transition, char **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    char buf[1024];
    size_t pos = 0;

    /* Season */
    hu_season_t season = hu_temporal_season(month);
    const char *sname = hu_temporal_season_name(season);
    int w = snprintf(buf + pos, sizeof(buf) - pos,
                     "TEMPORAL CONTEXT: It is %s (month %d, day %d). ", sname, month, day);
    if (w > 0 && pos + (size_t)w < sizeof(buf))
        pos += (size_t)w;

    /* Anniversaries */
    for (size_t i = 0; i < ann_count && pos < sizeof(buf) - 80; i++) {
        if (!anniversaries[i].label)
            continue;
        if (anniversaries[i].days_away == 0) {
            w = snprintf(buf + pos, sizeof(buf) - pos, "TODAY is %.*s. ",
                         (int)anniversaries[i].label_len, anniversaries[i].label);
        } else {
            w = snprintf(buf + pos, sizeof(buf) - pos, "%.*s in %d day%s. ",
                         (int)anniversaries[i].label_len, anniversaries[i].label,
                         anniversaries[i].days_away, anniversaries[i].days_away == 1 ? "" : "s");
        }
        if (w > 0 && pos + (size_t)w < sizeof(buf))
            pos += (size_t)w;
    }

    /* Life transition */
    if (transition != HU_TRANSITION_NONE) {
        const char *tname = transition_name(transition);
        if (tname) {
            w = snprintf(buf + pos, sizeof(buf) - pos,
                         "Recent life transition detected: %s. Be sensitive and supportive. ",
                         tname);
            if (w > 0 && pos + (size_t)w < sizeof(buf))
                pos += (size_t)w;
        }
    }

    if (pos == 0)
        return HU_OK;

    *out = hu_strndup(alloc, buf, pos);
    if (!*out)
        return HU_ERR_OUT_OF_MEMORY;
    *out_len = pos;
    return HU_OK;
}
