#include "human/agent/humanization_bandit.h"
#include "human/core/log.h"
#include <stdlib.h>
#include <string.h>

/* Thompson-sampled humanization tiers. theta = sampled P(this contact rewards
 * more expressive humanization), drawn from the contact's Beta(alpha,beta) arm;
 * higher theta -> more disfluency + backchannels. Thresholds and tier values
 * are tunable in this one place rather than scattered as magic numbers. */
#define HU_BANDIT_THETA_AGGRESSIVE 0.65 /* theta above this -> aggressive tier */
#define HU_BANDIT_THETA_MODERATE   0.35 /* theta above this -> moderate tier   */

typedef struct {
    float disfluency;
    float backchannel;
} hum_tier_t;

/* CONSERVATIVE is the safe default for new/unknown contacts and the low-theta
 * bucket — least likely to read as "off". */
static const hum_tier_t hum_conservative = {0.05f, 0.10f};
static const hum_tier_t hum_moderate = {0.15f, 0.30f};
static const hum_tier_t hum_aggressive = {0.25f, 0.45f};

static hu_humanization_config_t tier_config(hum_tier_t t) {
    hu_humanization_config_t config;
    memset(&config, 0, sizeof(config));
    config.disfluency_frequency = t.disfluency;
    config.backchannel_probability = t.backchannel;
    return config;
}

hu_humanization_config_t hu_humanization_decide_contact_params(hu_contextual_bandit_t *bandit,
                                                               uint64_t contact_handle) {
    if (!bandit || contact_handle == 0)
        return tier_config(hum_conservative); /* no bandit / invalid contact */

    hu_contextual_bandit_arm_t arm;
    if (hu_contextual_bandit_get_arm(bandit, contact_handle, &arm) != HU_OK)
        return tier_config(hum_conservative); /* arm missing or error */

    /* New contact (arm freshly initialized): conservative until we have signal. */
    if (arm.alpha == 1.0 && arm.beta == 1.0 && arm.updates == 0)
        return tier_config(hum_conservative);

    /* Thompson sample theta from Beta(alpha, beta). */
    uint32_t seed = bandit->rng_seed;
    double theta = hu_contextual_bandit_sample_beta(arm.alpha, arm.beta, &seed);

    static int logged = 0;
    if (!logged) {
        logged = 1;
        hu_log_info("humanization_bandit", NULL,
                    "Humanization profile selected for contact %llu: theta=%.2f",
                    (unsigned long long)contact_handle, theta);
    }

    if (theta > HU_BANDIT_THETA_AGGRESSIVE)
        return tier_config(hum_aggressive);
    if (theta > HU_BANDIT_THETA_MODERATE)
        return tier_config(hum_moderate);
    return tier_config(hum_conservative);
}

bool hu_humanization_apply_bandit_override(hu_contextual_bandit_t *bandit, uint64_t contact_handle,
                                           hu_humanization_config_t *inout_params) {
    if (!inout_params)
        return false;

    /* Gate: HU_BANDIT_HUMANIZATION. Activated 2026-05-31 after blind A/B (g2g) —
     * ENABLED by default. Disable via HU_BANDIT_HUMANIZATION=off|0|false|no. */
    const char *gate_env = getenv("HU_BANDIT_HUMANIZATION");
    bool gate_enabled = true;
    if (gate_env && *gate_env)
        gate_enabled = !(strcmp(gate_env, "0") == 0 || strcmp(gate_env, "off") == 0 ||
                         strcmp(gate_env, "false") == 0 || strcmp(gate_env, "no") == 0);

    if (!gate_enabled || !bandit)
        return false;

    /* Gate is enabled and bandit is non-NULL: apply the decision */
    hu_humanization_config_t decision =
        hu_humanization_decide_contact_params(bandit, contact_handle);
    inout_params->disfluency_frequency = decision.disfluency_frequency;
    inout_params->backchannel_probability = decision.backchannel_probability;

    return true;
}
