/* src/core/gate_mode.c — canonical off/shadow/live gate parsing.
 * See include/human/core/gate_mode.h for the contract and history. */
#include "human/core/gate_mode.h"

#include <stdlib.h>
#include <strings.h>

hu_gate_mode_t hu_gate_mode_parse(const char *value, hu_gate_mode_t unset_default) {
    if (!value || !*value)
        return unset_default;
    if (strcasecmp(value, "on") == 0 || strcasecmp(value, "live") == 0 ||
        strcasecmp(value, "1") == 0)
        return HU_GATE_LIVE;
    if (strcasecmp(value, "shadow") == 0)
        return HU_GATE_SHADOW;
    /* "off" and anything unrecognized fail closed — a typo must never
     * activate behavior, not even the caller's own default. */
    return HU_GATE_OFF;
}

hu_gate_mode_t hu_gate_mode_from_env(const char *env_name, hu_gate_mode_t unset_default) {
    if (!env_name)
        return unset_default;
    return hu_gate_mode_parse(getenv(env_name), unset_default);
}
