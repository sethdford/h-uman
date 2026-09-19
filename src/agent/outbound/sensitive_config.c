/* outbound/sensitive_config.c — config → protected-set adapter.
 *
 * Same role crosstalk_sqlite.c plays for the crosstalk stage: the stage itself
 * has no way to reach configuration from inside the pipeline, so a thin
 * adapter registers a provider callback that serves the parsed `privacy`
 * block. Keeping this in its own translation unit means sensitive.c stays a
 * pure predicate + stage with no config dependency, and tests can inject a
 * literal set without touching config at all.
 *
 * The tier for each value is DERIVED from its category rather than declared
 * alongside it (hu_sensitive_category_default_tier), so a street address can
 * never be filed as trust-gated or an employer as never-send.
 */

#include "human/agent/outbound_sensitive.h"

#include "human/config.h"
#include "human/core/log.h"

#include <stddef.h>
#include <string.h>

/* Flattened view of the config block. Storage is static because the provider
 * hands out a BORROWED set that must outlive the call, and the daemon holds
 * exactly one config. `value` pointers alias the arena-allocated config
 * strings, which live as long as the config does. */
#define SENS_CFG_MAX_VALUES 64u

static hu_sensitive_value_t s_values[SENS_CFG_MAX_VALUES];
static hu_sensitive_set_t s_set = {NULL, 0};
static bool s_built = false;

static void sens_cfg_append(char **list, size_t count, hu_sensitive_category_t cat, size_t *n_inout,
                            size_t *dropped) {
    if (!list)
        return;
    for (size_t i = 0; i < count; i++) {
        if (!list[i] || list[i][0] == '\0')
            continue;
        if (*n_inout >= SENS_CFG_MAX_VALUES) {
            (*dropped)++;
            continue;
        }
        hu_sensitive_value_t *v = &s_values[*n_inout];
        v->value = list[i];
        v->value_len = strlen(list[i]);
        v->category = cat;
        v->tier = hu_sensitive_category_default_tier(cat);
        (*n_inout)++;
    }
}

static int sens_cfg_provider(void *userdata, const hu_sensitive_set_t **out_set) {
    (void)userdata;
    if (!out_set)
        return -1;
    if (!s_built)
        return -1;
    *out_set = &s_set;
    return 0;
}

void hu_outbound_sensitive_register_config(const hu_config_t *cfg) {
    if (!cfg) {
        hu_outbound_sensitive_set_provider(NULL, NULL);
        s_built = false;
        return;
    }

    size_t n = 0;
    size_t dropped = 0;
    /* Cast away const: the values are borrowed read-only, but the config's
     * arrays are declared char** and the set stores const char*. */
    const hu_privacy_config_t *p = &cfg->privacy;
    sens_cfg_append(p->street_address, p->street_address_count, HU_SENSITIVE_CAT_STREET_ADDRESS, &n,
                    &dropped);
    sens_cfg_append(p->employer, p->employer_count, HU_SENSITIVE_CAT_EMPLOYER, &n, &dropped);
    sens_cfg_append(p->city, p->city_count, HU_SENSITIVE_CAT_CITY, &n, &dropped);
    sens_cfg_append(p->family_names, p->family_names_count, HU_SENSITIVE_CAT_FAMILY_NAME, &n,
                    &dropped);
    sens_cfg_append(p->financial, p->financial_count, HU_SENSITIVE_CAT_FINANCIAL, &n, &dropped);

    s_set.values = s_values;
    s_set.count = n;
    s_built = true;
    hu_outbound_sensitive_set_provider(sens_cfg_provider, NULL);

    /* Never log the values themselves — counts only. */
    if (dropped > 0) {
        /* Silent truncation of a security control's input is exactly the
         * no-number-without-a-measurement failure: the set would look
         * populated while some values were never checked. Say so loudly. */
        hu_log_warn("outbound_sensitive", NULL,
                    "protected-value set TRUNCATED: %zu declared values dropped (cap %u). "
                    "Those values are NOT being checked. Reduce the privacy block or raise "
                    "SENS_CFG_MAX_VALUES.",
                    dropped, (unsigned)SENS_CFG_MAX_VALUES);
    }
    hu_log_info("outbound_sensitive", NULL,
                "protected-value set registered: %zu values (address=%zu employer=%zu city=%zu "
                "family=%zu financial=%zu)",
                n, p->street_address_count, p->employer_count, p->city_count, p->family_names_count,
                p->financial_count);
}
