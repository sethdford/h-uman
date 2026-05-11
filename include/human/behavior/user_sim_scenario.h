#ifndef HU_BEHAVIOR_USER_SIM_SCENARIO_H
#define HU_BEHAVIOR_USER_SIM_SCENARIO_H

#include "human/behavior/policy.h"
#include "human/behavior/user_sim.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

/* B9 — Bounded user-sim scenario runner.
 *
 * Drives an `hu_user_sim_t` through up to N turns and runs each user message
 * through the central behavior policy (`hu_behavior_decide`). Records the
 * resulting `hu_relational_act_t` per turn and (optionally) compares it
 * against an expected sequence.
 *
 * The runner does NOT call the LLM — it exercises the deterministic decision
 * pipeline only. This keeps multi-turn regressions visible without flaky LLM
 * calls.
 *
 * Future extension: plug in a real `hu_agent_turn` for end-to-end runs gated
 * by HU_IS_TEST.
 */

typedef struct hu_user_sim_run_result {
    uint32_t turns_executed;
    uint32_t expected_matches;     /* turns where the act matched the expected one */
    uint32_t expected_total;       /* turns with an expected_act provided */
    uint32_t expected_acts[64];    /* circular buffer of decided acts (cap = 64) */
    uint32_t decided_count;        /* count of acts in `expected_acts` (≤ 64) */
} hu_user_sim_run_result_t;

/* Run the simulator. `expected_acts` (length `expected_count`) are
 * `hu_relational_act_t` values to compare against per-turn decisions. Pass
 * NULL/0 to skip comparison. `max_turns` caps total iterations.
 * `channel_class` matches `hu_behavior_input_t.channel_class`. */
hu_error_t hu_user_sim_scenario_run(hu_user_sim_t *sim, uint32_t max_turns, int channel_class,
                                    const uint32_t *expected_acts, size_t expected_count,
                                    hu_user_sim_run_result_t *out);

#endif /* HU_BEHAVIOR_USER_SIM_SCENARIO_H */
