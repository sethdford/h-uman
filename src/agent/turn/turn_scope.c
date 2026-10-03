/* src/agent/turn/turn_scope.c — the thread-local scope hu_agent_turn holds
 * around agent_turn_run. Lives outside agent_turn.c so the turn file stays
 * under its line pin (tests/test_turn_sources.c). Contract: agent/turn.h. */
#include "human/agent/turn.h"
#include "human/core/llm_purpose.h"
#include "human/core/local_only_guard.h"

hu_turn_scope_t hu_turn_scope_enter(void) {
    hu_turn_scope_t scope;
    scope.local_only_prev = hu_local_only_enter("agent_turn"); /* audit/refusal caller tag */
    /* X-HU-Purpose: an untagged turn's LLM calls are the reply; a caller's tag
     * (proactive, background lane) wins. llm_purpose.h maps it to priority. */
    scope.llm_purpose_prev = (int)hu_llm_purpose_set_if_untagged(HU_LLM_PURPOSE_REPLY);
    return scope;
}

void hu_turn_scope_exit(hu_turn_scope_t scope) {
    (void)hu_llm_purpose_set((hu_llm_purpose_t)scope.llm_purpose_prev);
    (void)hu_local_only_set_caller(scope.local_only_prev);
}
