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

/* ── Persistence (2026-07-18: arm posteriors survive daemon restarts) ── */

#include "human/core/json.h"
#include "human/core/state_file.h"
#include <inttypes.h>
#include <stdio.h>

/* Arms start at Beta(1,1) and only ever increment, so anything below 1.0
 * (or NaN — which fails the >= comparison) is corrupt, and the sampler
 * misbehaves at alpha/beta <= 0. The upper bound keeps a hand-edited file
 * from injecting values that overflow the gamma sampler. */
#define HU_BANDIT_PARAM_MAX 1e9

static double bandit_clamp_param(double x) {
    if (!(x >= 1.0))
        return 1.0;
    if (x > HU_BANDIT_PARAM_MAX)
        return HU_BANDIT_PARAM_MAX;
    return x;
}

hu_error_t hu_humanization_bandit_save_file(const hu_contextual_bandit_t *bandit,
                                            const char *path) {
    if (!bandit || !path || !path[0])
        return HU_ERR_INVALID_ARGUMENT;

    char tmp[512];
    FILE *f = hu_state_file_write_begin(path, tmp, sizeof(tmp));
    if (!f)
        return HU_ERR_IO;

    int w = fprintf(f, "{\"version\": 1, \"arms\": [");
    bool first = true;
    for (size_t i = 0; w > 0 && i < bandit->capacity; i++) {
        const hu_contextual_bandit_arm_t *arm = &bandit->arms[i];
        if (arm->contact_handle == 0)
            continue; /* empty slot */
        /* Handle as decimal STRING: it is a full 64-bit hash and a JSON
         * double (53-bit mantissa) would silently corrupt it. */
        w = fprintf(f,
                    "%s{\"h\": \"%" PRIu64 "\", \"alpha\": %.6f, \"beta\": %.6f, "
                    "\"updates\": %" PRIu64 "}",
                    first ? "" : ", ", arm->contact_handle, arm->alpha, arm->beta, arm->updates);
        first = false;
    }
    if (w > 0)
        w = fprintf(f, "]}\n");
    return hu_state_file_write_commit(f, w > 0, tmp, path);
}

hu_error_t hu_humanization_bandit_load_file(hu_contextual_bandit_t *bandit, const char *path) {
    if (!bandit || !path || !path[0])
        return HU_ERR_INVALID_ARGUMENT;

    FILE *f = fopen(path, "rb");
    if (!f)
        return HU_ERR_NOT_FOUND;

    /* Fixed read cap: 256 KiB fits ~3000 arms, far above the bandit's
     * capacity; a file that fills the cap is rejected as suspect. */
    enum { HU_BANDIT_FILE_MAX = 256 * 1024 };
    hu_allocator_t alloc = hu_system_allocator();
    char *buf = (char *)alloc.alloc(alloc.ctx, HU_BANDIT_FILE_MAX);
    if (!buf) {
        fclose(f);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t got = fread(buf, 1, HU_BANDIT_FILE_MAX - 1, f);
    fclose(f);
    if (got == 0 || got >= HU_BANDIT_FILE_MAX - 1) {
        alloc.free(alloc.ctx, buf, HU_BANDIT_FILE_MAX);
        return HU_ERR_IO;
    }
    buf[got] = '\0';

    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(&alloc, buf, got, &root);
    alloc.free(alloc.ctx, buf, HU_BANDIT_FILE_MAX);
    if (err != HU_OK || !root)
        return HU_ERR_PARSE;

    hu_json_value_t *arms = hu_json_object_get(root, "arms");
    if (root->type != HU_JSON_OBJECT || !arms || arms->type != HU_JSON_ARRAY) {
        hu_json_free(&alloc, root);
        return HU_ERR_PARSE; /* bandit untouched — nothing was applied yet */
    }

    for (size_t i = 0; i < arms->data.array.len; i++) {
        const hu_json_value_t *item = arms->data.array.items[i];
        if (!item || item->type != HU_JSON_OBJECT)
            continue;
        const char *h = hu_json_get_string(item, "h");
        if (!h || !h[0])
            continue;
        char *end = NULL;
        uint64_t handle = strtoull(h, &end, 10);
        if (handle == 0 || (end && *end != '\0'))
            continue; /* zero marks empty slots; never insert it */

        double a = bandit_clamp_param(hu_json_get_number(item, "alpha", 1.0));
        double b = bandit_clamp_param(hu_json_get_number(item, "beta", 1.0));
        double u = hu_json_get_number(item, "updates", 0.0);
        if (!(u >= 0.0))
            u = 0.0;

        if (hu_contextual_bandit_set_arm(bandit, handle, a, b, (uint64_t)u) != HU_OK)
            break; /* capacity exhausted — keep what fit */
    }

    hu_json_free(&alloc, root);
    return HU_OK;
}

const char *hu_humanization_bandit_default_path(char *buf, size_t cap) {
    return hu_state_file_default_path("bandit_humanization.json", buf, cap);
}
