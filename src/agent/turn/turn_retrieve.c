/* src/agent/turn/turn_retrieve.c — S3 retrieval, carved verbatim out of
 * hu_agent_turn (src/agent/agent_turn.c) by the phase-1 carve
 * (docs/superpowers/plans/2026-09-30-agent-turn-carve-phase1.md): Self-RAG
 * gate, memory loader, graph grounding, instruction discovery, data-quality
 * check, adaptive-RAG pick and the W12 contact-recall merge.
 *
 * graph_ctx is protected-core in the prompt: the W12 merge below folds contact
 * recall into memory_ctx only and leaves graph_ctx alive (#561 removed the free
 * that used to drop it here). tests/test_turn_sources.c pins that the free
 * stays gone. */
#include "../agent_internal.h"
#include "human/agent/graph_grounding.h"
#include "human/agent/turn.h"
#include "human/agent/world_model.h"
#include "human/agent/world_model_bridge.h"
#include "human/core/log.h"
#include "human/core/string.h"

hu_error_t hu_turn_retrieve(hu_turn_ctx_t *turn_ctx) {
    if (!turn_ctx || !turn_ctx->in.agent)
        return HU_ERR_INVALID_ARGUMENT;
    hu_agent_t *agent = turn_ctx->in.agent;
    const char *msg = turn_ctx->in.msg;
    size_t msg_len = turn_ctx->in.msg_len;
    hu_cognition_budget_t cognition_budget = turn_ctx->perception.cognition_budget;
    /* Self-RAG gate: decide whether retrieval is needed before loading memory */
    hu_srag_assessment_t srag_assessment;
    memset(&srag_assessment, 0, sizeof(srag_assessment));
    bool srag_skip_retrieval = false;
    if (agent->sota.sota_initialized && agent->sota.srag_config.enabled) {
        hu_srag_should_retrieve(agent->alloc, &agent->sota.srag_config, msg, msg_len, NULL, 0,
                                &srag_assessment);
        if (srag_assessment.decision == HU_SRAG_NO_RETRIEVAL)
            srag_skip_retrieval = true;
    }

    /* Load memory context for this turn (gated by Self-RAG) */
    char *memory_ctx = NULL;
    size_t memory_ctx_len = 0;
    char *graph_ctx = NULL;
    size_t graph_ctx_len = 0;
    if (agent->memory && agent->memory->vtable && !srag_skip_retrieval) {
        hu_memory_loader_t loader;
        hu_memory_loader_init(&loader, agent->alloc, agent->memory, agent->retrieval_engine,
                              cognition_budget.max_memory_entries,
                              cognition_budget.max_memory_chars);
        hu_memory_loader_set_facade(&loader, agent->w7_facade);
        hu_memory_loader_set_personal_model(&loader, &agent->personal_model);
        /* Story B (sprint-4 follow-up): bind persona context so the loader's
         * supplementary graph render runs with persona-grounded ToM and the
         * channel-aware pragmatics digest. */
        hu_persona_context_t loader_pctx = {0};
        if (agent->persona) {
            const char *loader_recent_tools[HU_SELF_RECENT_TOOLS];
            size_t loader_recent_tools_n = hu_agent_internal_collect_recent_tool_names(
                agent, loader_recent_tools, HU_SELF_RECENT_TOOLS);
            loader_pctx.persona = agent->persona;
            loader_pctx.channel = agent->active_channel;
            loader_pctx.channel_len = agent->active_channel_len;
            loader_pctx.delta_limit = 8;
            loader_pctx.tools = agent->tools;
            loader_pctx.tools_count = agent->tools_count;
            loader_pctx.recent_tools_used = loader_recent_tools_n ? loader_recent_tools : NULL;
            loader_pctx.recent_tools_used_count = loader_recent_tools_n;
            hu_memory_loader_set_persona_context(&loader, &loader_pctx);
        }
        hu_error_t load_err = hu_memory_loader_load(
            &loader, msg, msg_len, agent->memory_session_id ? agent->memory_session_id : "",
            agent->memory_session_id ? agent->memory_session_id_len : 0, &memory_ctx,
            &memory_ctx_len);
        if (load_err != HU_OK)
            hu_log_error("agent_turn", NULL, "memory loader failed: %s", hu_error_string(load_err));

        /* GraphRAG activation gated on a blind A/B measurement. Default is
         * SHADOW since 2026-05-31 (the first A/B measured ON-win-rate 43.3%,
         * below 50% — see hu_graph_grounding_mode). 2026-07-25: the read path
         * became query-conditioned (hu_graph_ground_compose keys retrieval on
         * the incoming msg, empty when nothing matches) but the gate stays
         * SHADOW; do not flip to default-ON without a FRESH blind A/B showing
         * the conversation-specific injection is judged superior by humans.
         * graph_ctx is protected-core in the prompt and is NOT subject to the
         * Self-RAG memory-relevance verdict below. */
        hu_agent_load_graph_grounding(agent, &loader, msg, msg_len, &graph_ctx, &graph_ctx_len);

        /* Self-RAG: verify relevance of retrieved content */
        if (srag_assessment.decision == HU_SRAG_RETRIEVE_AND_VERIFY && memory_ctx &&
            memory_ctx_len > 0) {
            double relevance = 0.0;
            bool should_use = false;
            hu_srag_verify_relevance(agent->alloc, &agent->sota.srag_config, msg, msg_len,
                                     memory_ctx, memory_ctx_len, &relevance, &should_use);
            if (!should_use) {
                /* Drop ONLY flat memory_ctx. hu_srag_verify_relevance scored
                 * memory_ctx, NOT graph_ctx — GraphRAG community-summary
                 * grounding ("who this contact is") is a distinct signal that a
                 * flat-memory relevance miss says nothing about. Freeing it here
                 * silently defeated grounding whenever flat memory happened to be
                 * judged irrelevant. graph_ctx is protected-core in the prompt and
                 * is freed downstream after the build (or on any earlier guarded
                 * exit), so leaving it live here cannot leak. */
                agent->alloc->free(agent->alloc->ctx, memory_ctx, memory_ctx_len + 1);
                memory_ctx = NULL;
                memory_ctx_len = 0;
            }
        }
    }
    const bool behavior_memory_ctx_nonempty = (memory_ctx != NULL && memory_ctx_len > 0);

    /* Check freshness of cached instruction discovery and re-discover if stale */
    if (agent->instruction_discovery &&
        !hu_instruction_discovery_is_fresh(agent->instruction_discovery)) {
        hu_instruction_discovery_destroy(agent->alloc, agent->instruction_discovery);
        agent->instruction_discovery = NULL;
    }

    /* Re-discover instructions if needed */
    if (!agent->instruction_discovery && agent->workspace_dir && agent->workspace_dir_len > 0) {
        hu_error_t disc_err =
            hu_instruction_discovery_run(agent->alloc, agent->workspace_dir,
                                         agent->workspace_dir_len, &agent->instruction_discovery);
        if (disc_err != HU_OK) {
            agent->instruction_discovery = NULL;
        }
    }

    /* Gather instruction context from discovery results */
    char *instruction_ctx = NULL;
    size_t instruction_ctx_len = 0;
    if (agent->instruction_discovery && agent->instruction_discovery->merged_content &&
        agent->instruction_discovery->merged_content_len > 0) {
        instruction_ctx = agent->instruction_discovery->merged_content;
        instruction_ctx_len = agent->instruction_discovery->merged_content_len;
    }

    /* Data quality: validate memory context fragments before assembly */
    if (agent->sota.dq_config.enabled && memory_ctx && memory_ctx_len > 0) {
        hu_dq_fragment_t frag = {
            .content = memory_ctx,
            .content_len = memory_ctx_len,
            .source = "memory",
            .source_len = 6,
        };
        hu_dq_result_t dq_result;
        if (hu_dq_check(&agent->sota.dq_config, &frag, 1, &dq_result) == HU_OK &&
            !dq_result.passed) {
            hu_log_info("agent_turn", NULL, "data quality: %zu issues in memory context",
                        dq_result.issue_count);
        }
    }

    /* Adaptive RAG: select strategy and record for learning */
    hu_rag_strategy_t rag_strategy_used = HU_RAG_NONE;
    if (agent->sota.sota_initialized && !srag_skip_retrieval) {
        rag_strategy_used = hu_adaptive_rag_select(&agent->sota.adaptive_rag, msg, msg_len);
    }

#ifdef HU_ENABLE_SQLITE
    /* W12: goal-conditioned planner recall via the W7 facade bridge.
     * Falls back to v1 hu_memory_recall_for_contact on planner failure
     * or when the facade is not wired. */
    if (agent->memory_session_id && agent->memory_session_id_len > 0) {
        char *contact_text = NULL;
        size_t contact_text_len = 0;
        bool planner_ok = false;

        if (agent->w7_facade) {
            /* P4: route through the LLM planner backend when a provider
             * is available. Under HU_IS_TEST the LLM planner falls back to
             * a deterministic single-step plan so tests stay free of
             * provider I/O. With no provider it degrades to goal-conditioned
             * → heuristic, identical to the original `hu_w12_planner_recall`
             * call. */
            hu_provider_t *provider = hu_agent_internal_recall_provider(agent, msg, msg_len);
            hu_error_t pe = hu_w12_planner_recall_with_provider(
                agent->w7_facade, agent->alloc, provider,
                /*model=*/NULL, /*model_len=*/0, agent->memory_session_id,
                agent->memory_session_id_len, msg, msg_len, 5, 4000, &contact_text,
                &contact_text_len);
            planner_ok = (pe == HU_OK && contact_text && contact_text_len > 0);
        }

        if (!planner_ok && agent->memory) {
            hu_memory_entry_t *contact_entries = NULL;
            size_t contact_count = 0;
            if (hu_memory_recall_for_contact(agent->memory, agent->alloc, agent->memory_session_id,
                                             agent->memory_session_id_len, msg, msg_len, 5, "", 0,
                                             &contact_entries, &contact_count) == HU_OK &&
                contact_entries && contact_count > 0) {
                size_t extra_len = 0;
                for (size_t i = 0; i < contact_count; i++) {
                    if (extra_len > SIZE_MAX - contact_entries[i].content_len - 1)
                        break;
                    extra_len += contact_entries[i].content_len + 1;
                }
                if (extra_len > 0) {
                    contact_text = (char *)agent->alloc->alloc(agent->alloc->ctx, extra_len + 32);
                    if (contact_text) {
                        size_t pos = 0;
                        pos = hu_buf_appendf(contact_text, extra_len + 32, pos,
                                             "[About this contact]\n");
                        for (size_t i = 0; i < contact_count && pos < extra_len + 31; i++) {
                            size_t to_copy = contact_entries[i].content_len;
                            if (pos + to_copy + 1 > extra_len + 31)
                                to_copy = extra_len + 31 - pos;
                            memcpy(contact_text + pos, contact_entries[i].content, to_copy);
                            pos += to_copy;
                            contact_text[pos++] = '\n';
                        }
                        contact_text[pos] = '\0';
                        contact_text_len = pos;
                    }
                }
                for (size_t i = 0; i < contact_count; i++)
                    hu_memory_entry_free_fields(agent->alloc, &contact_entries[i]);
                agent->alloc->free(agent->alloc->ctx, contact_entries,
                                   contact_count * sizeof(hu_memory_entry_t));
            }
        }

        if (contact_text && contact_text_len > 0) {
            size_t old_len = memory_ctx ? memory_ctx_len : 0;
            size_t new_total = old_len + (old_len > 0 ? 2 : 0) + contact_text_len + 1;
            char *merged = (char *)agent->alloc->alloc(agent->alloc->ctx, new_total);
            if (merged) {
                size_t pos = 0;
                if (memory_ctx && memory_ctx_len > 0) {
                    memcpy(merged, memory_ctx, memory_ctx_len);
                    pos = memory_ctx_len;
                    merged[pos++] = '\n';
                    merged[pos++] = '\n';
                }
                memcpy(merged + pos, contact_text, contact_text_len);
                pos += contact_text_len;
                merged[pos] = '\0';
                if (memory_ctx)
                    agent->alloc->free(agent->alloc->ctx, memory_ctx, memory_ctx_len + 1);
                /* graph_ctx is its own protected-core section; merging into
                 * memory_ctx must not drop it (it did until 2026-09-30). */
                memory_ctx = merged;
                memory_ctx_len = pos;
            }
            agent->alloc->free(agent->alloc->ctx, contact_text, contact_text_len + 1);
        }
    }
#endif

    turn_ctx->retrieval.memory_ctx = memory_ctx;
    turn_ctx->retrieval.memory_ctx_len = memory_ctx_len;
    turn_ctx->retrieval.graph_ctx = graph_ctx;
    turn_ctx->retrieval.graph_ctx_len = graph_ctx_len;
    turn_ctx->retrieval.memory_ctx_nonempty = behavior_memory_ctx_nonempty;
    turn_ctx->retrieval.instruction_ctx = instruction_ctx;
    turn_ctx->retrieval.instruction_ctx_len = instruction_ctx_len;
    turn_ctx->retrieval.rag_strategy_used = rag_strategy_used;
    return HU_OK;
}
