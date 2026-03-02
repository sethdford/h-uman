#include "seaclaw/memory/retrieval.h"
#include "seaclaw/memory.h"
#include "seaclaw/core/allocator.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Parse ISO 8601 timestamp, return hours since epoch, or -1 on failure */
static double parse_iso8601_hours(const char *ts, size_t ts_len) {
    if (!ts || ts_len < 10) return -1.0;
    int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0;
    int n = 0;
    if (sscanf(ts, "%4d-%2d-%2d%n", &year, &month, &day, &n) < 3) return -1.0;
    if ((size_t)n < ts_len && ts[n] == 'T') {
        n++;
        if ((size_t)n + 8 <= ts_len &&
            sscanf(ts + n, "%2d:%2d:%2d", &hour, &min, &sec) >= 2) {
            /* OK */
        }
    }
    struct tm tm = {0};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_sec = sec;
    tm.tm_isdst = -1;
    time_t t = timegm(&tm);
    if (t == (time_t)-1) return -1.0;
    return (double)t / 3600.0;
}

double sc_temporal_decay_score(double base_score, double decay_factor,
    const char *timestamp, size_t timestamp_len) {
    if (decay_factor <= 0.0) return base_score;
    if (!timestamp || timestamp_len == 0) return base_score;

    double ts_hours = parse_iso8601_hours(timestamp, timestamp_len);
    if (ts_hours < 0.0) return base_score;

    time_t now = time(NULL);
    double now_hours = (double)now / 3600.0;
    double age_hours = now_hours - ts_hours;
    if (age_hours < 0.0) age_hours = 0.0;

    /* decay_score = base_score * exp(-decay_factor * age_hours / 24.0) */
    double decayed = base_score * exp(-decay_factor * age_hours / 24.0);
    return decayed;
}
