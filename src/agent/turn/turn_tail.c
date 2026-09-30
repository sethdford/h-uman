/* src/agent/turn/turn_tail.c — S17 iteration tail and S18 exhausted-exit
 * events, carved verbatim out of agent_turn_run (src/agent/agent_turn.c); the
 * first phase-2 move of the hu_agent_turn carve
 * (docs/superpowers/specs/2026-09-30-agent-turn-carve-design.md, S17–S18 row;
 * phase-1 conventions in docs/superpowers/plans/2026-09-30-agent-turn-carve-phase1.md).
 *
 * hu_turn_tail is the end of each tool iteration: replan on tool failure,
 * mid-turn and goal-relevant memory retrieval, scratchpad turn metadata,
 * periodic checkpoint. It has no loop control, so it moved whole.
 * hu_turn_exhausted is the two observer events of the tool-iterations-exhausted
 * exit; that exit's clear, frees and HU_ERR_TIMEOUT return are agent_turn_run
 * locals and stay at its call site (plan gap G7). No edits beyond the
 * local-to-field aliases below. */
#include "../agent_internal.h"
#include "human/agent/memory_loader.h"
#include "human/agent/planner.h"
#include "human/agent/turn.h"
#include "human/core/log.h"
#include "human/core/string.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

hu_error_t hu_turn_tail(hu_turn_ctx_t *turn_ctx) {
    if (!turn_ctx || !turn_ctx->in.agent || !turn_ctx->in.msg)
        return HU_ERR_INVALID_ARGUMENT;
    const char *plan_ctx = turn_ctx->context.plan_ctx;
    hu_agent_t *agent = turn_ctx->in.agent;
    uint32_t iter = turn_ctx->loop.iter;
    const char *msg = turn_ctx->in.msg;
    size_t msg_len = turn_ctx->in.msg_len;
    uint64_t turn_tokens = turn_ctx->loop.turn_tokens;
    size_t turn_tool_results_count = turn_ctx->loop.turn_tool_results_count;
    /* Replan on tool failure: if any tool failed and we have a plan, generate
     * a revised plan and inject it as context for the next iteration */
    if (plan_ctx && !agent->cancel_requested) {
        size_t fail_count = 0;
        char fail_detail[512];
        size_t fail_pos = 0;
        for (size_t hi = agent->history_count; hi > 0 && hi > agent->history_count - 8; hi--) {
            if (agent->history[hi - 1].role == HU_ROLE_TOOL && agent->history[hi - 1].content &&
                agent->history[hi - 1].content_len > 0) {
                const char *c = agent->history[hi - 1].content;
                if ((c[0] == 'E' || c[0] == 'e') ||
                    (agent->history[hi - 1].content_len > 6 && memcmp(c, "denied", 6) == 0)) {
                    fail_count++;
                    if (fail_pos < sizeof(fail_detail) - 2) {
                        size_t chunk = agent->history[hi - 1].content_len;
                        if (chunk > 80)
                            chunk = 80;
                        if (fail_pos + chunk + 2 < sizeof(fail_detail)) {
                            memcpy(fail_detail + fail_pos, c, chunk);
                            fail_pos += chunk;
                            fail_detail[fail_pos++] = '\n';
                        }
                    }
                }
            }
        }
        if (fail_count >= 2) {
            fail_detail[fail_pos] = '\0';
            hu_plan_t *revised = NULL;
            hu_error_t rp_err = hu_planner_replan(
                agent->alloc, &agent->provider, agent->model_name, agent->model_name_len, msg,
                msg_len, "partial progress", 16, fail_detail, fail_pos, NULL, 0, &revised);
            if (rp_err == HU_OK && revised && revised->steps_count > 0) {
                char replan_note[1024];
                int rn = snprintf(replan_note, sizeof(replan_note),
                                  "[REPLAN after %zu tool failures]: %zu new steps", fail_count,
                                  revised->steps_count);
                if (rn > 0) {
                    hu_agent_internal_append_history(agent, HU_ROLE_SYSTEM, replan_note, (size_t)rn,
                                                     NULL, 0, NULL, 0);
                }
            }
            if (revised)
                hu_plan_free(agent->alloc, revised);
        }
    }

    /* Mid-turn retrieval: after tool results, augment context with
     * memory relevant to both tool output and the evolving conversation */
    if (agent->memory && agent->memory->vtable && agent->history_count > 1 &&
        agent->history[agent->history_count - 1].role == HU_ROLE_TOOL && !agent->cancel_requested) {
        const char *last_result = NULL;
        size_t last_result_len = 0;
        if (agent->history_count > 0) {
            last_result = agent->history[agent->history_count - 1].content;
            last_result_len = agent->history[agent->history_count - 1].content_len;
        }
        if (last_result && last_result_len > 20) {
            char *mid_ctx = NULL;
            size_t mid_ctx_len = 0;
            hu_memory_loader_t mid_loader;
            hu_memory_loader_init(&mid_loader, agent->alloc, agent->memory, agent->retrieval_engine,
                                  5, 2000);
            hu_memory_loader_set_facade(&mid_loader, agent->w7_facade);
            if (hu_memory_loader_load(&mid_loader, last_result, last_result_len,
                                      agent->memory_session_id ? agent->memory_session_id : "",
                                      agent->memory_session_id ? agent->memory_session_id_len : 0,
                                      &mid_ctx, &mid_ctx_len) == HU_OK &&
                mid_ctx && mid_ctx_len > 0) {
                char *note = hu_sprintf(agent->alloc, "[memory context from tool results]: %.*s",
                                        (int)(mid_ctx_len < 2000 ? mid_ctx_len : 2000), mid_ctx);
                if (note) {
                    hu_error_t hist_err = hu_agent_internal_append_history(
                        agent, HU_ROLE_SYSTEM, note, strlen(note), NULL, 0, NULL, 0);
                    if (hist_err != HU_OK)
                        hu_log_error("agent_turn", NULL, "history append failed: %s",
                                     hu_error_string(hist_err));
                    agent->alloc->free(agent->alloc->ctx, note, strlen(note) + 1);
                }
                agent->alloc->free(agent->alloc->ctx, mid_ctx, mid_ctx_len + 1);
            }

            /* Iterative retrieval: also retrieve against original user message
             * to maintain relevance to the goal across iterations */
            if (iter > 1 && msg_len > 10) {
                char *goal_ctx = NULL;
                size_t goal_ctx_len = 0;
                hu_memory_loader_t goal_loader;
                hu_memory_loader_init(&goal_loader, agent->alloc, agent->memory,
                                      agent->retrieval_engine, 3, 1000);
                hu_memory_loader_set_facade(&goal_loader, agent->w7_facade);
                if (hu_memory_loader_load(&goal_loader, msg, msg_len,
                                          agent->memory_session_id ? agent->memory_session_id : "",
                                          agent->memory_session_id ? agent->memory_session_id_len
                                                                   : 0,
                                          &goal_ctx, &goal_ctx_len) == HU_OK &&
                    goal_ctx && goal_ctx_len > 0) {
                    char *gnote =
                        hu_sprintf(agent->alloc, "[goal-relevant memory]: %.*s",
                                   (int)(goal_ctx_len < 1000 ? goal_ctx_len : 1000), goal_ctx);
                    if (gnote) {
                        hu_agent_internal_append_history(agent, HU_ROLE_SYSTEM, gnote,
                                                         strlen(gnote), NULL, 0, NULL, 0);
                        agent->alloc->free(agent->alloc->ctx, gnote, strlen(gnote) + 1);
                    }
                    agent->alloc->free(agent->alloc->ctx, goal_ctx, goal_ctx_len + 1);
                }
            }
        }
    }

    /* Scratchpad: persist turn metadata as working memory */
    if (agent->sota.scratchpad.max_bytes > 0) {
        char turn_key[32];
        int tk_n = snprintf(turn_key, sizeof(turn_key), "turn_%d", (int)iter);
        if (tk_n > 0 && (size_t)tk_n < sizeof(turn_key)) {
            char turn_val[256];
            int tv_n = snprintf(turn_val, sizeof(turn_val), "tokens=%u,tools=%zu",
                                (unsigned)turn_tokens, turn_tool_results_count);
            if (tv_n > 0 && (size_t)tv_n < sizeof(turn_val))
                hu_scratchpad_set(&agent->sota.scratchpad, agent->alloc, turn_key, (size_t)tk_n,
                                  turn_val, (size_t)tv_n);
        }
    }

    /* Checkpoint: auto-save state after tool iterations */
    if (hu_checkpoint_should_save(&agent->sota.checkpoint_store, (uint32_t)iter)) {
        char cp_state[128];
        int cp_n = snprintf(cp_state, sizeof(cp_state), "{\"iter\":%d,\"tokens\":%llu}", (int)iter,
                            (unsigned long long)agent->total_tokens);
        if (cp_n > 0) {
            if ((size_t)cp_n >= sizeof(cp_state))
                cp_n = (int)(sizeof(cp_state) - 1);
            static const char cp_task[] = "agent_turn";
            hu_checkpoint_save(&agent->sota.checkpoint_store, agent->alloc, cp_task,
                               sizeof(cp_task) - 1, (uint32_t)iter, HU_CHECKPOINT_ACTIVE, cp_state,
                               (size_t)cp_n);
        }
    }

    return HU_OK;
}

hu_error_t hu_turn_exhausted(hu_turn_ctx_t *turn_ctx) {
    if (!turn_ctx || !turn_ctx->in.agent)
        return HU_ERR_INVALID_ARGUMENT;
    hu_agent_t *agent = turn_ctx->in.agent;
    {
        hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_TOOL_ITERATIONS_EXHAUSTED,
                                  .data = {{0}}};
        ev.data.tool_iterations_exhausted.iterations = agent->max_tool_iterations;
        HU_OBS_SAFE_RECORD_EVENT(agent, &ev);
    }
    {
        hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_ERR, .data = {{0}}};
        ev.data.err.component = "agent";
        ev.data.err.message = "tool iterations exhausted";
        HU_OBS_SAFE_RECORD_EVENT(agent, &ev);
    }
    return HU_OK;
}
