/* Tolerant scheduler.status JSON (written by daemon W14 tick): shared by doctor + ML CLI. */

#include "human/agent/scheduler_status_json.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static const char *scheduler_status_skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        ++p;
    return p;
}

static bool scheduler_status_tail_ok_bool(const char *after_word) {
    char c = *after_word;
    return c == '\0' || c == ',' || c == '}' || c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static bool scheduler_status_u64_after_key(const char *buf, const char *quoted_key,
                                           unsigned long long *out) {
    const char *p = strstr(buf, quoted_key);
    if (!p)
        return false;
    const char *colon = strchr(p, ':');
    if (!colon)
        return false;
    const char *v = scheduler_status_skip_ws(colon + 1);
    char *end = NULL;
    unsigned long long x = strtoull(v, &end, 10);
    if (end == v)
        return false;
    *out = x;
    return true;
}

static bool scheduler_status_i64_after_key(const char *buf, const char *quoted_key, long long *out) {
    const char *p = strstr(buf, quoted_key);
    if (!p)
        return false;
    const char *colon = strchr(p, ':');
    if (!colon)
        return false;
    const char *v = scheduler_status_skip_ws(colon + 1);
    char *end = NULL;
    long long x = strtoll(v, &end, 10);
    if (end == v)
        return false;
    *out = x;
    return true;
}

static bool scheduler_status_bool_text_after_key(const char *buf, const char *quoted_key,
                                                 char *text, size_t cap) {
    const char *p = strstr(buf, quoted_key);
    if (!p)
        return false;
    const char *colon = strchr(p, ':');
    if (!colon)
        return false;
    const char *v = scheduler_status_skip_ws(colon + 1);
    if (strncmp(v, "true", 4) == 0 && scheduler_status_tail_ok_bool(v + 4)) {
        if (cap < 5)
            return false;
        memcpy(text, "true", 5);
        return true;
    }
    if (strncmp(v, "false", 5) == 0 && scheduler_status_tail_ok_bool(v + 5)) {
        if (cap < 6)
            return false;
        memcpy(text, "false", 6);
        return true;
    }
    return false;
}

hu_error_t hu_scheduler_status_parse_json(const char *json, unsigned long long *jobs_pending,
                                          unsigned long long *jobs_completed_today,
                                          long long *battery_pct, char *on_ac_power_text,
                                          size_t on_ac_power_cap, long long *updated_epoch) {
    if (!json || !jobs_pending || !jobs_completed_today || !battery_pct || !on_ac_power_text ||
        on_ac_power_cap < 6U || !updated_epoch)
        return HU_ERR_INVALID_ARGUMENT;
    if (!scheduler_status_u64_after_key(json, "\"jobs_pending\"", jobs_pending))
        return HU_ERR_INVALID_ARGUMENT;
    if (!scheduler_status_u64_after_key(json, "\"jobs_completed_today\"", jobs_completed_today))
        return HU_ERR_INVALID_ARGUMENT;
    if (!scheduler_status_i64_after_key(json, "\"battery_pct\"", battery_pct))
        return HU_ERR_INVALID_ARGUMENT;
    if (!scheduler_status_bool_text_after_key(json, "\"on_ac_power\"", on_ac_power_text,
                                              on_ac_power_cap))
        return HU_ERR_INVALID_ARGUMENT;
    if (!scheduler_status_i64_after_key(json, "\"updated_epoch\"", updated_epoch))
        return HU_ERR_INVALID_ARGUMENT;
    return HU_OK;
}
