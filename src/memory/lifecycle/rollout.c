#include "seaclaw/memory/lifecycle/rollout.h"
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

sc_rollout_mode_t sc_rollout_mode_from_string(const char *s, size_t len) {
    if (!s || len == 0) return SC_ROLLOUT_OFF;
    if (len == 3 && memcmp(s, "off", 3) == 0) return SC_ROLLOUT_OFF;
    if (len == 6 && memcmp(s, "shadow", 6) == 0) return SC_ROLLOUT_SHADOW;
    if (len == 6 && memcmp(s, "canary", 6) == 0) return SC_ROLLOUT_CANARY;
    if (len == 2 && memcmp(s, "on", 2) == 0) return SC_ROLLOUT_ON;
    return SC_ROLLOUT_OFF;
}

sc_rollout_policy_t sc_rollout_policy_init(const char *rollout_mode, size_t mode_len,
    unsigned canary_hybrid_percent, unsigned shadow_hybrid_percent) {
    sc_rollout_policy_t p = {
        .mode = sc_rollout_mode_from_string(rollout_mode, mode_len),
        .canary_percent = canary_hybrid_percent > 100 ? 100u : canary_hybrid_percent,
        .shadow_percent = shadow_hybrid_percent > 100 ? 100u : shadow_hybrid_percent,
    };
    return p;
}

sc_rollout_decision_t sc_rollout_decide(const sc_rollout_policy_t *policy,
    const char *session_id, size_t session_id_len) {
    if (!policy) return SC_ROLLOUT_KEYWORD_ONLY;

    switch (policy->mode) {
        case SC_ROLLOUT_OFF:
            return SC_ROLLOUT_KEYWORD_ONLY;
        case SC_ROLLOUT_SHADOW:
            return SC_ROLLOUT_SHADOW_HYBRID;
        case SC_ROLLOUT_ON:
            return SC_ROLLOUT_HYBRID;
        case SC_ROLLOUT_CANARY:
            if (!session_id || session_id_len == 0)
                return SC_ROLLOUT_KEYWORD_ONLY;
            if (fnv1a32(session_id, session_id_len) % 100 < policy->canary_percent)
                return SC_ROLLOUT_HYBRID;
            return SC_ROLLOUT_KEYWORD_ONLY;
    }
    return SC_ROLLOUT_KEYWORD_ONLY;
}
