#include "config_internal.h"
#include "config_parse_internal.h"
#include "human/config.h"
#include <string.h>

hu_error_t parse_behavior(hu_allocator_t *a, hu_config_t *cfg, const hu_json_value_t *obj) {
    (void)a; /* unused */
    if (!obj || obj->type != HU_JSON_OBJECT)
        return HU_OK;

    /* Group chat: consecutive message limit */
    double cl = hu_json_get_number(obj, "consecutive_limit", cfg->behavior.consecutive_limit);
    if (cl >= 1 && cl <= 100)
        cfg->behavior.consecutive_limit = (uint32_t)cl;

    /* Group chat: participation percentage threshold */
    double pp = hu_json_get_number(obj, "participation_pct", cfg->behavior.participation_pct);
    if (pp >= 1 && pp <= 100)
        cfg->behavior.participation_pct = (uint32_t)pp;

    /* Response length: maximum characters */
    double mrc = hu_json_get_number(obj, "max_response_chars", cfg->behavior.max_response_chars);
    if (mrc >= 1 && mrc <= 100000)
        cfg->behavior.max_response_chars = (uint32_t)mrc;

    /* Response length: minimum characters */
    double minrc = hu_json_get_number(obj, "min_response_chars", cfg->behavior.min_response_chars);
    if (minrc >= 1 && minrc <= 1000)
        cfg->behavior.min_response_chars = (uint32_t)minrc;

    /* Memory consolidation: decay window in days */
    double dd = hu_json_get_number(obj, "decay_days", cfg->behavior.decay_days);
    if (dd >= 1 && dd <= 365)
        cfg->behavior.decay_days = (uint32_t)dd;

    /* Memory consolidation: deduplication threshold percentage */
    double dt = hu_json_get_number(obj, "dedup_threshold", cfg->behavior.dedup_threshold);
    if (dt >= 1 && dt <= 100)
        cfg->behavior.dedup_threshold = (uint32_t)dt;

    /* Missed message acknowledgment threshold in seconds */
    double mmts = hu_json_get_number(obj, "missed_msg_threshold_sec", cfg->behavior.missed_msg_threshold_sec);
    if (mmts >= 60 && mmts <= 86400)
        cfg->behavior.missed_msg_threshold_sec = (uint32_t)mmts;

    /* Callback delay window in seconds */
    double cw = hu_json_get_number(obj, "callback_window", cfg->behavior.callback_window);
    if (cw >= 0 && cw <= 86400)
        cfg->behavior.callback_window = (uint32_t)cw;

    /* Conversation pattern match threshold percentage */
    double pt = hu_json_get_number(obj, "pattern_threshold", cfg->behavior.pattern_threshold);
    if (pt >= 0 && pt <= 100)
        cfg->behavior.pattern_threshold = (uint32_t)pt;

    /* Probability to skip tapback/reaction percentage */
    double tsp = hu_json_get_number(obj, "tapback_skip_pct", cfg->behavior.tapback_skip_pct);
    if (tsp >= 0 && tsp <= 100)
        cfg->behavior.tapback_skip_pct = (uint32_t)tsp;

    return HU_OK;
}
