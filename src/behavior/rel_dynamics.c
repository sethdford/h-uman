#include "human/behavior/rel_dynamics.h"
#include "human/core/string.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define HU_REL_DRIFT_THRESHOLD_DEFAULT         (-0.1)
#define HU_REL_CLEAR_DRIFT_THRESHOLD_DEFAULT   (-0.3)
#define HU_REL_REPAIR_EXIT_DAYS_DEFAULT        3u
#define HU_REL_DRIFT_BUDGET_MULTIPLIER_DEFAULT 0.5
#define HU_REL_MS_PER_DAY                      86400000ULL

#define CLAMP(x, lo, hi) (((x) < (lo)) ? (lo) : (((x) > (hi)) ? (hi) : (x)))

static double clamp_signal(double v) {
    return CLAMP(v, -1.0, 1.0);
}

double hu_rel_compute_velocity(const hu_rel_signals_t *signals) {
    if (!signals)
        return 0.0;
    double f = clamp_signal(signals->frequency_delta);
    double i = clamp_signal(signals->initiation_delta);
    double r = clamp_signal(signals->response_time_delta);
    double m = clamp_signal(signals->msg_length_delta);
    double v = clamp_signal(signals->vulnerability_delta);
    double p = CLAMP(signals->plan_completion_rate, 0.0, 1.0);
    double s = clamp_signal(signals->sentiment_delta);
    double t = clamp_signal(signals->topic_diversity_delta);

    double vel =
        0.20 * f + 0.15 * i + 0.15 * r + 0.10 * m + 0.15 * v + 0.10 * p + 0.10 * s + 0.05 * t;
    return CLAMP(vel, -1.0, 1.0);
}

hu_rel_mode_t hu_rel_classify_mode(double velocity, double prev_velocity,
                                   hu_rel_mode_t current_mode) {
    const double drift_threshold = HU_REL_DRIFT_THRESHOLD_DEFAULT;
    const double clear_drift = HU_REL_CLEAR_DRIFT_THRESHOLD_DEFAULT;

    if (current_mode == HU_REL_REPAIR)
        return HU_REL_REPAIR;

    if (velocity > 0.1)
        return HU_REL_DEEPENING;

    if (velocity < clear_drift)
        return HU_REL_DRIFTING;

    if (velocity < drift_threshold && prev_velocity < drift_threshold)
        return HU_REL_DRIFTING;

    if (current_mode == HU_REL_DRIFTING && velocity > 0.0)
        return HU_REL_RECONNECTING;

    return HU_REL_NORMAL;
}

hu_error_t hu_rel_compute_state(const hu_rel_signals_t *signals, double current_closeness,
                                hu_rel_mode_t current_mode, double prev_velocity,
                                hu_rel_state_t *out) {
    if (!signals || !out)
        return HU_ERR_INVALID_ARGUMENT;

    double velocity = hu_rel_compute_velocity(signals);
    hu_rel_mode_t mode = hu_rel_classify_mode(velocity, prev_velocity, current_mode);

    double new_closeness = current_closeness + velocity * 0.1;
    new_closeness = CLAMP(new_closeness, 0.0, 1.0);

    double vuln = 0.5 + 0.5 * clamp_signal(signals->vulnerability_delta);
    vuln = CLAMP(vuln, 0.0, 1.0);

    uint64_t now_ms = (uint64_t)time(NULL) * 1000ULL;

    out->closeness = new_closeness;
    out->velocity = velocity;
    out->vulnerability_depth = vuln;
    out->reciprocity = clamp_signal(signals->initiation_delta);
    out->last_interaction = 0;
    out->last_vulnerability_moment = 0;
    out->mode = mode;
    out->mode_entered_at = now_ms;
    out->measured_at = now_ms;

    return HU_OK;
}

double hu_rel_budget_multiplier(const hu_rel_state_t *state, const hu_rel_config_t *config) {
    if (!state)
        return 1.0;

    switch (state->mode) {
    case HU_REL_NORMAL:
    case HU_REL_DEEPENING:
        return 1.0;
    case HU_REL_DRIFTING:
        return config && config->drift_budget_multiplier > 0.0
                   ? config->drift_budget_multiplier
                   : HU_REL_DRIFT_BUDGET_MULTIPLIER_DEFAULT;
    case HU_REL_REPAIR:
        return 0.3;
    case HU_REL_RECONNECTING:
        return 0.8;
    default:
        return 1.0;
    }
}

bool hu_rel_should_exit_repair(const hu_rel_state_t *state, const hu_rel_config_t *config,
                               uint64_t now_ms) {
    if (!state || state->mode != HU_REL_REPAIR)
        return false;
    if (state->velocity <= 0.0)
        return false;

    uint32_t days = config && config->repair_exit_days > 0 ? config->repair_exit_days
                                                           : HU_REL_REPAIR_EXIT_DAYS_DEFAULT;
    uint64_t min_elapsed = (uint64_t)days * HU_REL_MS_PER_DAY;
    return (now_ms - state->mode_entered_at) > min_elapsed;
}

#define HU_REL_ESCAPE_BUF 512

static size_t escape_sql_string(const char *s, size_t len, char *out, size_t out_cap) {
    size_t n = 0;
    (void)hu_sql_quote_escape_into(s, len, out, out_cap, &n);
    return n;
}

hu_error_t hu_rel_create_table_sql(char *buf, size_t cap, size_t *out_len) {
    if (!buf || !out_len || cap < 512)
        return HU_ERR_INVALID_ARGUMENT;

    static const char sql[] = "CREATE TABLE IF NOT EXISTS relationship_state (\n"
                              "    id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
                              "    contact_id TEXT NOT NULL,\n"
                              "    closeness REAL NOT NULL,\n"
                              "    velocity REAL NOT NULL,\n"
                              "    vulnerability_depth REAL NOT NULL,\n"
                              "    reciprocity REAL NOT NULL,\n"
                              "    last_interaction INTEGER NOT NULL,\n"
                              "    last_vulnerability_moment INTEGER NOT NULL,\n"
                              "    mode TEXT NOT NULL,\n"
                              "    mode_entered_at INTEGER NOT NULL,\n"
                              "    measured_at INTEGER NOT NULL\n"
                              ")";

    size_t len = sizeof(sql) - 1;
    if (len >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(buf, sql, len + 1);
    *out_len = len;
    return HU_OK;
}

hu_error_t hu_rel_insert_sql(const hu_rel_state_t *state, char *buf, size_t cap, size_t *out_len) {
    if (!state || !buf || !out_len || cap < 512)
        return HU_ERR_INVALID_ARGUMENT;

    const char *cid = state->contact_id ? state->contact_id : "";
    size_t cid_len = state->contact_id_len ? state->contact_id_len : strlen(cid);

    char esc_contact[HU_REL_ESCAPE_BUF];
    if (cid_len > 0 && escape_sql_string(cid, cid_len, esc_contact, sizeof(esc_contact)) == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (cid_len == 0)
        esc_contact[0] = '\0';

    const char *mode_str = hu_rel_mode_str(state->mode);

    int n = snprintf(buf, cap,
                     "INSERT INTO relationship_state (contact_id, closeness, velocity, "
                     "vulnerability_depth, reciprocity, last_interaction, "
                     "last_vulnerability_moment, mode, mode_entered_at, measured_at) "
                     "VALUES ('%s', %f, %f, %f, %f, %llu, %llu, '%s', %llu, %llu)",
                     esc_contact, state->closeness, state->velocity, state->vulnerability_depth,
                     state->reciprocity, (unsigned long long)state->last_interaction,
                     (unsigned long long)state->last_vulnerability_moment, mode_str,
                     (unsigned long long)state->mode_entered_at,
                     (unsigned long long)state->measured_at);

    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_rel_query_latest_sql(const char *contact_id, size_t contact_id_len, char *buf,
                                   size_t cap, size_t *out_len) {
    if (!contact_id || contact_id_len == 0 || !buf || !out_len || cap < 128)
        return HU_ERR_INVALID_ARGUMENT;

    char esc[HU_REL_ESCAPE_BUF];
    if (escape_sql_string(contact_id, contact_id_len, esc, sizeof(esc)) == 0)
        return HU_ERR_INVALID_ARGUMENT;

    int n = snprintf(buf, cap,
                     "SELECT contact_id, closeness, velocity, vulnerability_depth, "
                     "reciprocity, last_interaction, last_vulnerability_moment, "
                     "mode, mode_entered_at, measured_at FROM relationship_state "
                     "WHERE contact_id = '%s' ORDER BY measured_at DESC LIMIT 1",
                     esc);

    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

const char *hu_rel_mode_str(hu_rel_mode_t mode) {
    switch (mode) {
    case HU_REL_NORMAL:
        return "normal";
    case HU_REL_DEEPENING:
        return "deepening";
    case HU_REL_DRIFTING:
        return "drifting";
    case HU_REL_REPAIR:
        return "repair";
    case HU_REL_RECONNECTING:
        return "reconnecting";
    default:
        return "normal";
    }
}

bool hu_rel_mode_from_str(const char *str, hu_rel_mode_t *out) {
    if (!str || !out)
        return false;
    if (strcmp(str, "normal") == 0) {
        *out = HU_REL_NORMAL;
        return true;
    }
    if (strcmp(str, "deepening") == 0) {
        *out = HU_REL_DEEPENING;
        return true;
    }
    if (strcmp(str, "drifting") == 0) {
        *out = HU_REL_DRIFTING;
        return true;
    }
    if (strcmp(str, "repair") == 0) {
        *out = HU_REL_REPAIR;
        return true;
    }
    if (strcmp(str, "reconnecting") == 0) {
        *out = HU_REL_RECONNECTING;
        return true;
    }
    return false;
}

static const char *closeness_label(double c) {
    if (c < 0.25)
        return "stranger";
    if (c < 0.5)
        return "acquaintance";
    if (c < 0.75)
        return "friend";
    if (c < 0.9)
        return "close friend";
    return "intimate";
}

static const char *velocity_trend(double v) {
    if (v > 0.05)
        return "deepening";
    if (v < -0.05)
        return "drifting";
    return "stable";
}

hu_error_t hu_rel_build_prompt(hu_allocator_t *alloc, const hu_rel_state_t *state, char **out,
                               size_t *out_len) {
    if (!alloc || !state || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    const char *mode_str = hu_rel_mode_str(state->mode);
    const char *closeness_lbl = closeness_label(state->closeness);
    const char *trend = velocity_trend(state->velocity);

    const char *behavior = "none needed";
    if (state->mode == HU_REL_DRIFTING)
        behavior = "Give space. Don't chase. Be warm when they reach out.";
    else if (state->mode == HU_REL_REPAIR)
        behavior = "Reduce humor. Increase warmth. Acknowledge tension without "
                   "over-apologizing.";

    int n = snprintf(NULL, 0,
                     "[RELATIONSHIP STATE with this contact]:\n"
                     "- Closeness: %.2f (%s range)\n"
                     "- Trend: %s (velocity %+.2f)\n"
                     "- Mode: %s\n\n"
                     "Behavioral adjustments: %s\n",
                     state->closeness, closeness_lbl, trend, state->velocity, mode_str, behavior);

    if (n < 0)
        return HU_ERR_INVALID_ARGUMENT;

    size_t need = (size_t)n + 1;
    char *buf = alloc->alloc(alloc->ctx, need);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    int written =
        snprintf(buf, need,
                 "[RELATIONSHIP STATE with this contact]:\n"
                 "- Closeness: %.2f (%s range)\n"
                 "- Trend: %s (velocity %+.2f)\n"
                 "- Mode: %s\n\n"
                 "Behavioral adjustments: %s\n",
                 state->closeness, closeness_lbl, trend, state->velocity, mode_str, behavior);

    if (written < 0 || (size_t)written >= need) {
        alloc->free(alloc->ctx, buf, need);
        return HU_ERR_INVALID_ARGUMENT;
    }

    *out = buf;
    *out_len = (size_t)written;
    return HU_OK;
}

void hu_rel_state_deinit(hu_allocator_t *alloc, hu_rel_state_t *state) {
    if (!alloc || !state)
        return;
    if (state->contact_id) {
        hu_str_free(alloc, state->contact_id);
        state->contact_id = NULL;
        state->contact_id_len = 0;
    }
}
