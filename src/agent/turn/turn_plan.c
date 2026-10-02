/* src/agent/turn/turn_plan.c — resuming an [ACTIVE_PLAN] from history, moved
 * out of agent_turn_run's automatic-planning block (src/agent/agent_turn.c).
 *
 * The scan used to sit inside that block's `#ifndef HU_IS_TEST` guard with the
 * planner and plan executor. It makes no provider call, so it now runs in test
 * builds too: that is what lets the suite drive a turn with a live plan_ctx
 * (the replan and exit-path frees in the turn body were otherwise unreachable
 * under HU_IS_TEST). Production behaviour is unchanged. */
#include "human/agent/turn.h"
#include "human/core/string.h"
#include <string.h>

char *hu_turn_active_plan(hu_agent_t *agent, size_t *plan_len_out) {
    if (plan_len_out)
        *plan_len_out = 0;
    if (!agent || !agent->alloc || !plan_len_out)
        return NULL;
    size_t scan_n = agent->history_count < 10 ? agent->history_count : 10;
    for (size_t k = 0; k < scan_n; k++) {
        size_t hi = agent->history_count - 1 - k;
        if (agent->history[hi].role != HU_ROLE_SYSTEM || !agent->history[hi].content)
            continue;
        const char *hc = agent->history[hi].content;
        if (strncmp(hc, "[ACTIVE_PLAN]", 13) != 0)
            continue;
        size_t clen = strlen(hc);
        char *plan = hu_strndup(agent->alloc, hc, clen);
        if (plan)
            *plan_len_out = clen;
        return plan;
    }
    return NULL;
}
