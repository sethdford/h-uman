/* src/agent/turn/turn_entry.c — S0 entry, carved verbatim out of hu_agent_turn
 * (src/agent/agent_turn.c) by the phase-1 carve
 * (docs/superpowers/plans/2026-09-30-agent-turn-carve-phase1.md). The only
 * edits: the five early `return X;` became `return hu_turn_step_return(X);`
 * and the fall-through returns hu_turn_step_continue(). The two argument
 * checks that used to open the block stay in hu_agent_turn, which needs them
 * before the per-turn context exists. */
#include "../agent_internal.h"
#include "human/agent/contact_stage_turn.h"
#include "human/agent/humanness.h"
#include "human/agent/input_guard.h"
#include "human/agent/speculative.h"
#include "human/agent/turn.h"
#include "human/core/log.h"
#include "human/core/string.h"
#include "human/memory/lifecycle/semantic_cache.h"
#include "human/persona/delta_observer.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

hu_turn_step_t hu_turn_entry(hu_turn_ctx_t *turn_ctx) {
    if (!turn_ctx || !turn_ctx->in.agent || !turn_ctx->in.response_out)
        return hu_turn_step_return(HU_ERR_INVALID_ARGUMENT);
    hu_agent_t *agent = turn_ctx->in.agent;
    const char *msg = turn_ctx->in.msg;
    size_t msg_len = turn_ctx->in.msg_len;
    char **response_out = turn_ctx->in.response_out;
    size_t *response_len_out = turn_ctx->in.response_len_out;
    *response_out = NULL;
    if (response_len_out)
        *response_len_out = 0;

    if (getenv("HU_DEBUG"))
        hu_log_info("agent_turn", NULL, "ENTER agent_turn msg_len=%zu", msg_len);

    hu_agent_set_current_for_tools(agent);
    /* DEF-16: the relationship stage of THIS turn's contact, not an
     * agent-wide counter (src/agent/turn/contact_stage_turn.c). */
    (void)hu_contact_stage_refresh(agent, agent->memory_session_id, agent->memory_session_id_len);

    /* Reset per-turn state tracking so this turn's behavior-log stash sees a
     * clean slate (tool_count, tool_sequence_hash, emotional_register,
     * persona_delta_kind). Populated as the turn progresses; consumed by
     * hu_agent_internal_emit_behavior_record at stash time. */
    hu_agent_turn_state_reset(agent);

    /* Clear the last rejected draft so DPO pairing only captures rejections from THIS turn.
     * Per-turn pairing prevents stale cross-turn alternatives from contaminating the dataset. */
    if (agent->sota.last_rejected_draft) {
        agent->alloc->free(agent->alloc->ctx, agent->sota.last_rejected_draft,
                           agent->sota.last_rejected_draft_len + 1);
        agent->sota.last_rejected_draft = NULL;
        agent->sota.last_rejected_draft_len = 0;
    }

    /* Free any previously-built humanness context, then build fresh for this turn */
    hu_agent_free_turn_context(agent);
    hu_agent_build_turn_context(agent);

    /* Speculative cache: check for pre-computed response */
    if (agent->infra.speculative_cache) {
        hu_speculative_config_t spec_cfg = hu_speculative_config_default();
        hu_prediction_t *hit = NULL;
        int64_t now = (int64_t)time(NULL);
        if (hu_speculative_cache_lookup(agent->infra.speculative_cache, msg, msg_len, now,
                                        &spec_cfg, &hit) == HU_OK &&
            hit) {
            *response_out = hu_strndup(agent->alloc, hit->response, hit->response_len);
            if (*response_out) {
                if (response_len_out)
                    *response_len_out = hit->response_len;
                hu_agent_clear_current_for_tools();
                return hu_turn_step_return(HU_OK);
            }
        }
    }

    /* Semantic response cache: check for semantically similar past query */
    if (agent->infra.response_cache) {
        hu_semantic_cache_hit_t cache_hit;
        memset(&cache_hit, 0, sizeof(cache_hit));
        if (hu_semantic_cache_get(agent->infra.response_cache, agent->alloc, msg, msg_len, msg,
                                  msg_len, &cache_hit) == HU_OK &&
            cache_hit.response) {
            if (cache_hit.similarity >= 0.92f) {
                *response_out = cache_hit.response;
                if (response_len_out)
                    *response_len_out = strlen(cache_hit.response);
                hu_agent_clear_current_for_tools();
                return hu_turn_step_return(HU_OK);
            }
            hu_semantic_cache_hit_free(agent->alloc, &cache_hit);
        }
    }

    hu_agent_internal_process_mailbox_messages(agent);

    char *slash_resp = hu_agent_handle_slash_command(agent, msg, msg_len);
    if (slash_resp) {
        hu_agent_clear_current_for_tools();
        *response_out = slash_resp;
        if (response_len_out)
            *response_len_out = strlen(slash_resp);
        return hu_turn_step_return(HU_OK);
    }

    /* Log workflow step start */
    if (agent->infra.workflow_log) {
        hu_workflow_event_t ev = {0};
        ev.type = HU_WF_EVENT_STEP_STARTED;
        ev.timestamp = hu_workflow_event_current_timestamp_ms();
        hu_workflow_event_log_append(agent->infra.workflow_log, agent->alloc, &ev);
    }

    /* Prompt injection defense-in-depth */
    {
        hu_injection_risk_t risk = HU_INJECTION_SAFE;
        hu_error_t guard_err = hu_input_guard_check(msg, msg_len, &risk);
        if (guard_err != HU_OK) {
            hu_agent_clear_current_for_tools();
            return hu_turn_step_return(guard_err);
        }
        if (risk == HU_INJECTION_HIGH_RISK) {
            if (agent->observer) {
                hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_ERR};
                ev.data.err.component = "input_guard";
                ev.data.err.message = "high-risk injection pattern detected";
                hu_observer_record_event(*agent->observer, &ev);
            }
            *response_out = hu_strndup(agent->alloc,
                                       "I can't process that request due to safety concerns.", 52);
            if (response_len_out)
                *response_len_out = 52;
            hu_agent_clear_current_for_tools();
            return hu_turn_step_return(HU_OK);
        }
    }

    /* W5 producer (FIX 9) + W13 learner bridge (post-FIX-19):
     *
     * Scan the safe user message for explicit persona-correction phrases
     * ("be more X", "stop saying Y", etc.) and record them as delta
     * proposals. The daemon's daily evolver (FIX 3) reads this table at
     * 3 AM and applies stable proposals. The `_with_learner` variant
     * additionally drains every just-proposed delta through
     * `hu_learner_bridge_emit_persona_deltas` so the W13 learner can use
     * them at the next sleep-time training tick. `agent->learner` is
     * tolerated NULL by the bridge — installations without ML enabled
     * get the propose-only path identical to FIX 9. */
    if (agent->verifier_graph && agent->memory_session_id && agent->memory_session_id_len > 0) {
        size_t observed = 0;
        hu_persona_observe_user_correction_with_learner(
            agent->verifier_graph, agent->learner, agent->memory_session_id,
            agent->memory_session_id_len, agent->active_channel, agent->active_channel_len, msg,
            msg_len, 0, &observed);
        if (observed > 0)
            agent->persona_deltas_proposed += observed;
    }

    return hu_turn_step_continue();
}
