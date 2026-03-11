#include "human/context/rel_dynamics.h"
#include "human/core/string.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define HU_REL_ESCAPE_BUF 1024
#define HU_REL_MS_PER_DAY 86400000ULL

#define CLAMP(x, lo, hi) (((x) < (lo)) ? (lo) : (((x) > (hi)) ? (hi) : (x)))

static void escape_sql_string(const char *s, size_t len, char *buf, size_t cap,
                              size_t *out_len) {
    size_t pos = 0;
    for (size_t i = 0; i < len && pos + 2 < cap; i++) {
        if (s[i] == '\'') {
            buf[pos++] = '\'';
            buf[pos++] = '\'';
        } else {
            buf[pos++] = s[i];
        }
    }
    buf[pos] = '\0';
    *out_len = pos;
}

hu_error_t hu_rel_dynamics_create_table_sql(char *buf, size_t cap, size_t *out_len) {
    if (!buf || !out_len || cap < 512)
        return HU_ERR_INVALID_ARGUMENT;
    static const char sql[] =
        "CREATE TABLE IF NOT EXISTS relationship_dynamics (\n"
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
        "    contact_id TEXT NOT NULL,\n"
        "    messages_sent INTEGER NOT NULL,\n"
        "    messages_received INTEGER NOT NULL,\n"
        "    initiations_sent INTEGER NOT NULL,\n"
        "    initiations_received INTEGER NOT NULL,\n"
        "    avg_response_time_ms INTEGER NOT NULL,\n"
        "    interaction_quality REAL NOT NULL,\n"
        "    velocity REAL NOT NULL,\n"
        "    trend TEXT NOT NULL,\n"
        "    measured_at INTEGER NOT NULL\n"
        ")";
    size_t len = sizeof(sql) - 1;
    if (len >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(buf, sql, len + 1);
    *out_len = len;
    return HU_OK;
}

float hu_rel_velocity_compute(hu_rel_velocity_t *vel) {
    if (!vel)
        return 0.0f;

    float prev = vel->velocity;
    uint32_t sent = vel->messages_sent_30d;
    uint32_t recv = vel->messages_received_30d;
    uint32_t total_msg = sent + recv;
    if (total_msg == 0)
        total_msg = 1;
    float msg_balance = (float)((int)recv - (int)sent) / (float)total_msg;
    msg_balance = (float)CLAMP(msg_balance, -1.0, 1.0);

    uint32_t init_sent = vel->initiations_sent_30d;
    uint32_t init_recv = vel->initiations_received_30d;
    uint32_t total_init = init_sent + init_recv;
    if (total_init == 0)
        total_init = 1;
    float initiation_balance =
        (float)((int)init_recv - (int)init_sent) / (float)total_init;
    initiation_balance = (float)CLAMP(initiation_balance, -1.0, 1.0);

    double response_speed_factor =
        1.0 - (double)vel->avg_response_time_ms / (double)HU_REL_MS_PER_DAY;
    response_speed_factor = CLAMP(response_speed_factor, -1.0, 1.0);

    float iq = (float)CLAMP((double)vel->interaction_quality, -1.0, 1.0);

    float v = 0.3f * msg_balance + 0.3f * initiation_balance
              + 0.2f * (float)response_speed_factor + 0.2f * iq;
    vel->velocity = (float)CLAMP(v, -1.0, 1.0);
    vel->trend = hu_rel_trend_classify(vel->velocity, prev);
    return vel->velocity;
}

hu_rel_trend_t hu_rel_trend_classify(float velocity, float prev_velocity) {
    (void)prev_velocity;
    if (velocity > 0.15f)
        return HU_REL_TREND_DEEPENING;
    if (velocity < -0.15f)
        return HU_REL_TREND_COOLING;
    return HU_REL_TREND_STABLE;
}

hu_error_t hu_rel_dynamics_insert_sql(const hu_rel_velocity_t *vel, uint64_t timestamp_ms,
                                      char *buf, size_t cap, size_t *out_len) {
    if (!vel || !buf || !out_len || cap < 512)
        return HU_ERR_INVALID_ARGUMENT;

    const char *cid = vel->contact_id ? vel->contact_id : "";
    size_t cid_len = vel->contact_id_len;
    if (cid_len == 0 && cid[0] != '\0')
        cid_len = strlen(cid);

    char contact_esc[HU_REL_ESCAPE_BUF];
    size_t ce_len;
    escape_sql_string(cid, cid_len, contact_esc, sizeof(contact_esc), &ce_len);

    const char *trend_str = hu_rel_trend_str(vel->trend);

    int n = snprintf(
        buf, cap,
        "INSERT INTO relationship_dynamics (contact_id, messages_sent, messages_received, "
        "initiations_sent, initiations_received, avg_response_time_ms, interaction_quality, "
        "velocity, trend, measured_at) VALUES ('%s', %u, %u, %u, %u, %llu, %f, %f, '%s', %llu)",
        contact_esc, vel->messages_sent_30d, vel->messages_received_30d,
        vel->initiations_sent_30d, vel->initiations_received_30d,
        (unsigned long long)vel->avg_response_time_ms, (double)vel->interaction_quality,
        (double)vel->velocity, trend_str, (unsigned long long)timestamp_ms);

    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_rel_dynamics_query_sql(const char *contact_id, size_t contact_id_len,
                                     size_t limit, char *buf, size_t cap, size_t *out_len) {
    if (!contact_id || contact_id_len == 0 || !buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;

    char contact_esc[HU_REL_ESCAPE_BUF];
    size_t ce_len;
    escape_sql_string(contact_id, contact_id_len, contact_esc, sizeof(contact_esc),
                     &ce_len);

    int n = snprintf(buf, cap,
                    "SELECT contact_id, messages_sent, messages_received, "
                    "initiations_sent, initiations_received, avg_response_time_ms, "
                    "interaction_quality, velocity, trend, measured_at "
                    "FROM relationship_dynamics WHERE contact_id = '%s' "
                    "ORDER BY measured_at DESC LIMIT %zu",
                    contact_esc, limit > 0 ? limit : 100);

    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

bool hu_drift_detect(const hu_rel_velocity_t *measurements, size_t count,
                    hu_drift_signal_t *signal) {
    if (!measurements || !signal || count < 2)
        return false;

    signal->consecutive_negative_periods = 0;
    signal->last_velocity = 0.0f;
    signal->current_velocity = measurements[count - 1].velocity;
    signal->is_drifting = false;

    if (count > 0) {
        signal->contact_id = measurements[0].contact_id;
        signal->contact_id_len = measurements[0].contact_id_len;
    }

    for (size_t i = count; i > 0; i--) {
        size_t idx = i - 1;
        if (measurements[idx].velocity < 0.0f) {
            signal->consecutive_negative_periods++;
            if (idx > 0)
                signal->last_velocity = measurements[idx - 1].velocity;
        } else {
            break;
        }
    }

    signal->is_drifting = signal->consecutive_negative_periods >= 2;
    return signal->is_drifting;
}

bool hu_repair_should_activate(const hu_drift_signal_t *signal,
                               float interaction_quality) {
    if (!signal || !signal->is_drifting)
        return false;
    return interaction_quality < -0.3f;
}

float hu_rel_dynamics_budget_multiplier(hu_rel_trend_t trend) {
    switch (trend) {
    case HU_REL_TREND_DEEPENING:
        return 1.2f;
    case HU_REL_TREND_STABLE:
        return 1.0f;
    case HU_REL_TREND_COOLING:
        return 0.7f;
    case HU_REL_TREND_STRAINED:
        return 0.4f;
    case HU_REL_TREND_REPAIR:
        return 0.5f;
    default:
        return 1.0f;
    }
}

const char *hu_rel_trend_str(hu_rel_trend_t trend) {
    switch (trend) {
    case HU_REL_TREND_DEEPENING:
        return "deepening";
    case HU_REL_TREND_STABLE:
        return "stable";
    case HU_REL_TREND_COOLING:
        return "cooling";
    case HU_REL_TREND_STRAINED:
        return "strained";
    case HU_REL_TREND_REPAIR:
        return "repair";
    default:
        return "stable";
    }
}

hu_error_t hu_rel_dynamics_build_prompt(hu_allocator_t *alloc,
                                         const hu_rel_velocity_t *vel,
                                         const hu_drift_signal_t *signal,
                                         const hu_repair_state_t *repair, char **out,
                                         size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    const char *directive = "Relationship is stable — maintain natural engagement.";
    if (repair && repair->active) {
        directive =
            "Relationship is in repair mode. Reduce humor. Increase warmth. "
            "Acknowledge tension without over-apologizing. Give space.";
    } else if (signal && signal->is_drifting) {
        directive =
            "Relationship is cooling — be warm but don't chase. Give space when "
            "they pull back.";
    } else if (vel && vel->trend == HU_REL_TREND_DEEPENING) {
        directive = "This relationship is deepening — matched energy is good.";
    } else if (vel && vel->trend == HU_REL_TREND_COOLING) {
        directive =
            "Relationship is cooling — be warm but don't chase.";
    }

    const char *header = "[RELATIONSHIP DYNAMICS]:\n";
    char *result = hu_sprintf(alloc, "%s%s", header, directive);
    if (!result)
        return HU_ERR_OUT_OF_MEMORY;

    *out = result;
    *out_len = strlen(result);
    return HU_OK;
}

void hu_rel_velocity_deinit(hu_allocator_t *alloc, hu_rel_velocity_t *vel) {
    (void)alloc;
    if (!vel)
        return;
    vel->contact_id = NULL;
    vel->contact_id_len = 0;
}

void hu_repair_state_deinit(hu_allocator_t *alloc, hu_repair_state_t *state) {
    if (!alloc || !state)
        return;
    if (state->contact_id) {
        hu_str_free(alloc, state->contact_id);
        state->contact_id = NULL;
        state->contact_id_len = 0;
    }
    if (state->reason) {
        hu_str_free(alloc, state->reason);
        state->reason = NULL;
        state->reason_len = 0;
    }
}
