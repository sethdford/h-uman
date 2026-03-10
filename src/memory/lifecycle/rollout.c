#include "human/memory/lifecycle/rollout.h"
#include <stdint.h>
#include <string.h>

/* FNV-1a 32-bit hash for canary session bucketing */
static uint32_t fnv1a32(const char *s, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)s[i];
        h *= 16777619u;
    }
    return h;
}

hu_rollout_mode_t hu_rollout_mode_from_string(const char *s, size_t len) {
    if (!s || len == 0)
        return HU_ROLLOUT_OFF;
    if (len == 3 && memcmp(s, "off", 3) == 0)
        return HU_ROLLOUT_OFF;
    if (len == 6 && memcmp(s, "shadow", 6) == 0)
        return HU_ROLLOUT_SHADOW;
    if (len == 6 && memcmp(s, "canary", 6) == 0)
        return HU_ROLLOUT_CANARY;
    if (len == 2 && memcmp(s, "on", 2) == 0)
        return HU_ROLLOUT_ON;
    return HU_ROLLOUT_OFF;
}

hu_rollout_policy_t hu_rollout_policy_init(const char *rollout_mode, size_t mode_len,
                                           unsigned canary_hybrid_percent,
                                           unsigned shadow_hybrid_percent) {
    hu_rollout_policy_t p = {
        .mode = hu_rollout_mode_from_string(rollout_mode, mode_len),
        .canary_percent = canary_hybrid_percent > 100 ? 100u : canary_hybrid_percent,
        .shadow_percent = shadow_hybrid_percent > 100 ? 100u : shadow_hybrid_percent,
    };
    return p;
}

hu_rollout_decision_t hu_rollout_decide(const hu_rollout_policy_t *policy, const char *session_id,
                                        size_t session_id_len) {
    if (!policy)
        return HU_ROLLOUT_KEYWORD_ONLY;

    switch (policy->mode) {
    case HU_ROLLOUT_OFF:
        return HU_ROLLOUT_KEYWORD_ONLY;
    case HU_ROLLOUT_SHADOW:
        return HU_ROLLOUT_SHADOW_HYBRID;
    case HU_ROLLOUT_ON:
        return HU_ROLLOUT_HYBRID;
    case HU_ROLLOUT_CANARY:
        if (!session_id || session_id_len == 0)
            return HU_ROLLOUT_KEYWORD_ONLY;
        if (fnv1a32(session_id, session_id_len) % 100 < policy->canary_percent)
            return HU_ROLLOUT_HYBRID;
        return HU_ROLLOUT_KEYWORD_ONLY;
    }
    return HU_ROLLOUT_KEYWORD_ONLY;
}
