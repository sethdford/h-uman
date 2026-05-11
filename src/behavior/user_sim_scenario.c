#include "human/behavior/user_sim_scenario.h"

#include "human/behavior/policy.h"
#include <stdlib.h>
#include <string.h>

hu_error_t hu_user_sim_scenario_run(hu_user_sim_t *sim, uint32_t max_turns, int channel_class,
                                    const uint32_t *expected_acts, size_t expected_count,
                                    hu_user_sim_run_result_t *out) {
    if (!sim || !out) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    if (max_turns == 0) {
        return HU_OK;
    }

    for (uint32_t t = 0; t < max_turns; t++) {
        hu_user_sim_turn_ctx_t tctx;
        memset(&tctx, 0, sizeof(tctx));
        tctx.turn_index = t;
        char *line = NULL;
        size_t line_len = 0;
        hu_error_t err = hu_user_sim_next(sim, &tctx, &line, &line_len);
        if (err != HU_OK) {
            return err;
        }
        if (!line) {
            /* Sim has no more lines. */
            break;
        }
        hu_behavior_input_t bin;
        hu_behavior_input_from_user_message(&bin, line, line_len, channel_class);
        hu_behavior_decision_t bdec;
        memset(&bdec, 0, sizeof(bdec));
        if (hu_behavior_decide(&bin, &bdec) != HU_OK) {
            free(line);
            continue;
        }
        if (out->decided_count <
            sizeof(out->expected_acts) / sizeof(out->expected_acts[0])) {
            out->expected_acts[out->decided_count++] = (uint32_t)bdec.act;
        }
        out->turns_executed++;
        if (expected_acts && t < expected_count) {
            out->expected_total++;
            if ((uint32_t)bdec.act == expected_acts[t]) {
                out->expected_matches++;
            }
        }
        free(line);
    }
    return HU_OK;
}
