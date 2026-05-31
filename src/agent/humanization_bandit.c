#include "human/agent/humanization_bandit.h"
#include "human/core/log.h"
#include <stdlib.h>
#include <string.h>

hu_humanization_config_t hu_humanization_decide_contact_params(hu_contextual_bandit_t *bandit,
                                                               uint64_t contact_handle) {
    hu_humanization_config_t config;
    memset(&config, 0, sizeof(config));

    if (!bandit || contact_handle == 0) {
        /* No bandit or invalid contact → return conservative defaults */
        config.disfluency_frequency = 0.05;
        config.backchannel_probability = 0.10;
        return config;
    }

    /* Get the arm for this contact (initialize if new) */
    hu_contextual_bandit_arm_t arm;
    hu_error_t err = hu_contextual_bandit_get_arm(bandit, contact_handle, &arm);
    if (err != HU_OK) {
        /* Arm doesn't exist or error; default to conservative */
        config.disfluency_frequency = 0.05;
        config.backchannel_probability = 0.10;
        return config;
    }

    /* NEW CONTACT: if arm was just initialized (alpha=1, beta=1, updates=0),
     * return conservative defaults (safe). Else sample and decide. */
    if (arm.alpha == 1.0 && arm.beta == 1.0 && arm.updates == 0) {
        /* New contact; default to conservative (safe) */
        config.disfluency_frequency = 0.05;
        config.backchannel_probability = 0.10;
        return config;
    }

    /* Thompson sample θ from Beta(α, β) */
    uint32_t seed = bandit->rng_seed;
    double theta = hu_contextual_bandit_sample_beta(arm.alpha, arm.beta, &seed);

    /* Log once per process */
    static int logged = 0;
    if (!logged) {
        logged = 1;
        hu_log_info("humanization_bandit", NULL,
                    "Humanization profile selected for contact %llu: theta=%.2f",
                    (unsigned long long)contact_handle, theta);
    }

    /* Classify based on theta threshold */
    if (theta > 0.65) {
        /* Aggressive humanization */
        config.disfluency_frequency = 0.25;
        config.backchannel_probability = 0.45;
    } else if (theta > 0.35) {
        /* Moderate humanization */
        config.disfluency_frequency = 0.15;
        config.backchannel_probability = 0.30;
    } else {
        /* Conservative (default) */
        config.disfluency_frequency = 0.05;
        config.backchannel_probability = 0.10;
    }

    return config;
}

bool hu_humanization_apply_bandit_override(hu_contextual_bandit_t *bandit, uint64_t contact_handle,
                                           hu_humanization_config_t *inout_params) {
    if (!inout_params)
        return false;

    /* Check gate: HU_BANDIT_HUMANIZATION env var (default OFF) */
    const char *gate_env = getenv("HU_BANDIT_HUMANIZATION");
    bool gate_enabled = gate_env && (gate_env[0] == '1' || strcmp(gate_env, "true") == 0 ||
                                     strcmp(gate_env, "yes") == 0 || strcmp(gate_env, "on") == 0);

    if (!gate_enabled || !bandit)
        return false;

    /* Gate is enabled and bandit is non-NULL: apply the decision */
    hu_humanization_config_t decision =
        hu_humanization_decide_contact_params(bandit, contact_handle);
    inout_params->disfluency_frequency = decision.disfluency_frequency;
    inout_params->backchannel_probability = decision.backchannel_probability;

    return true;
}
