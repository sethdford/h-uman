#include "human/context/rel_dynamics.h"
#include "human/core/string.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define HU_REL_MS_PER_DAY 86400000ULL

#define CLAMP(x, lo, hi) (((x) < (lo)) ? (lo) : (((x) > (hi)) ? (hi) : (x)))

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
    float initiation_balance = (float)((int)init_recv - (int)init_sent) / (float)total_init;
    initiation_balance = (float)CLAMP(initiation_balance, -1.0, 1.0);

    double response_speed_factor =
        1.0 - (double)vel->avg_response_time_ms / (double)HU_REL_MS_PER_DAY;
    response_speed_factor = CLAMP(response_speed_factor, -1.0, 1.0);

    float iq = (float)CLAMP((double)vel->interaction_quality, -1.0, 1.0);

    float v = 0.3f * msg_balance + 0.3f * initiation_balance + 0.2f * (float)response_speed_factor +
              0.2f * iq;
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

hu_error_t hu_rel_dynamics_build_prompt(hu_allocator_t *alloc, const hu_rel_velocity_t *vel,
                                        const hu_drift_signal_t *signal,
                                        const hu_repair_state_t *repair, char **out,
                                        size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    const char *directive = "Relationship is stable — maintain natural engagement.";
    if (repair && repair->active) {
        directive = "Relationship is in repair mode. Reduce humor. Increase warmth. "
                    "Acknowledge tension without over-apologizing. Give space.";
    } else if (signal && signal->is_drifting) {
        directive = "Relationship is cooling — be warm but don't chase. Give space when "
                    "they pull back.";
    } else if (vel && vel->trend == HU_REL_TREND_DEEPENING) {
        directive = "This relationship is deepening — matched energy is good.";
    } else if (vel && vel->trend == HU_REL_TREND_COOLING) {
        directive = "Relationship is cooling — be warm but don't chase.";
    }

    const char *header = "[RELATIONSHIP DYNAMICS]:\n";
    char *result = hu_sprintf(alloc, "%s%s", header, directive);
    if (!result)
        return HU_ERR_OUT_OF_MEMORY;

    *out = result;
    *out_len = strlen(result);
    return HU_OK;
}
