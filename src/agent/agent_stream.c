/* Streaming infrastructure: token callback wiring, hu_agent_turn_stream, hu_agent_turn_stream_v2 */
#include "agent_internal.h"
#include "human/agent/awareness.h"
#include "human/agent/channel_trust.h"
#include "human/agent/commands.h"
#include "human/agent/constitutional.h"
#include "human/agent/conv_goals.h"
#include "human/agent/frontier_persist.h"
#include "human/agent/graph_grounding.h"
#include "human/agent/growth_narrative.h"
#include "human/agent/gvr.h"
#include "human/agent/humanness.h"
#include "human/agent/input_guard.h"
#include "human/agent/memory_loader.h"
#include "human/agent/model_router.h"
#include "human/agent/outcomes.h"
#include "human/agent/pattern_radar.h"
#include "human/agent/preferences.h"
#include "human/agent/prompt.h"
#include "human/agent/prompt_budget.h"
#include "human/agent/response_guard.h"
#include "human/agent/response_guard_dpo.h"
#include "human/agent/response_guard_retry.h"
#include "human/agent/self_rag.h"
#include "human/agent/session_persist.h"
#include "human/agent/superhuman.h"
#include "human/agent/tool_call_parser.h"
#include "human/agent/validators/builtin.h"
#include "human/agent/world_model_bridge.h"
#include "human/cognition/attachment.h"
#include "human/cognition/dual_process.h"
#include "human/cognition/emotional.h"
#include "human/cognition/episodic.h"
#include "human/cognition/humor.h"
#include "human/cognition/metacognition.h"
#include "human/cognition/novelty.h"
#include "human/cognition/rupture_repair.h"
#include "human/cognition/trust.h"
#include "human/config.h"
#include "human/context.h"
#include "human/context/contact_style_overlay.h"
#include "human/context/conversation.h"
#include "human/context_engine.h"
#include "human/core/json.h"
#include "human/core/log.h"
#include "human/core/string.h"
#include "human/eval/consistency.h"
#include "human/experience.h"
#include "human/hook.h"
#include "human/hook_pipeline.h"
#include "human/humanness.h"
#include "human/memory/deep_extract.h"
#include "human/memory/fact_extract.h"
#include "human/memory/fast_capture.h"
#include "human/memory/hallucination_guard.h"
#include "human/memory/personal_model.h"
#include "human/persona.h"
#include "human/persona/creative_voice.h"
#include "human/persona/delta_observer.h"
#include "human/persona/genuine_boundaries.h"
#include "human/persona/humor.h"
#include "human/persona/narrative_self.h"
#include "human/persona/rag.h"
#include "human/persona/somatic.h"
#include "human/reflection.h" /* T7: reflection-loop slice in build_prompt */
#include "human/security/moderation.h"
#include "human/security/sycophancy_guard.h"
#include "human/tool.h"
#ifdef HU_ENABLE_SQLITE
#include "human/intelligence/online_learning.h"
#include "human/intelligence/self_improve.h"
#include "human/intelligence/value_learning.h"
#include "human/memory.h"
#include <sqlite3.h>
#endif
#include "human/ml/m3_rewrite_capture.h"
#include "human/provider.h"
#include "human/voice.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

/* v1 shim: translates hu_agent_stream_event_t back to the simple token callback */
typedef struct v1_shim_ctx {
    hu_agent_stream_token_cb on_token;
    void *token_ctx;
} v1_shim_ctx_t;

static void v1_shim_event_cb(const hu_agent_stream_event_t *event, void *ctx) {
    v1_shim_ctx_t *s = (v1_shim_ctx_t *)ctx;
    if (event->type == HU_AGENT_STREAM_TEXT && s->on_token && event->data && event->data_len > 0)
        s->on_token(event->data, event->data_len, s->token_ctx);
}

/* v2 stream callback adapter: maps provider stream chunks to agent stream events */
typedef struct v2_stream_wrap {
    hu_agent_stream_event_cb on_event;
    void *event_ctx;
    uint32_t initial_delay_ms; /* emotional pacing */
    bool first_content_sent;
    bool suppress_content;
} v2_stream_wrap_t;

static bool stream_chunk_to_event_cb(void *ctx, const hu_stream_chunk_t *chunk) {
    if (!chunk)
        return false;
    v2_stream_wrap_t *w = (v2_stream_wrap_t *)ctx;
    if (!w->on_event || chunk->is_final)
        return true;
    hu_agent_stream_event_t ev;
    memset(&ev, 0, sizeof(ev));
    switch (chunk->type) {
    case HU_STREAM_CONTENT:
        if (!chunk->delta || chunk->delta_len == 0)
            return true;
        if (w->suppress_content)
            return true;
        /* Emotional pacing: pause before first content chunk */
        if (!w->first_content_sent && w->initial_delay_ms > 0) {
            uint32_t delay = w->initial_delay_ms;
            if (delay > 100)
                delay = 100;
#ifdef _WIN32
            Sleep(delay);
#else
            usleep((useconds_t)delay * 1000);
#endif
        }
        w->first_content_sent = true;
        ev.type = HU_AGENT_STREAM_TEXT;
        ev.data = chunk->delta;
        ev.data_len = chunk->delta_len;
        break;
    case HU_STREAM_THINKING:
        if (!chunk->delta || chunk->delta_len == 0)
            return true;
        if (w->suppress_content)
            return true;
        ev.type = HU_AGENT_STREAM_THINKING;
        ev.data = chunk->delta;
        ev.data_len = chunk->delta_len;
        break;
    case HU_STREAM_TOOL_START:
        ev.type = HU_AGENT_STREAM_TOOL_START;
        ev.tool_name = chunk->tool_name;
        ev.tool_name_len = chunk->tool_name_len;
        ev.tool_call_id = chunk->tool_call_id;
        ev.tool_call_id_len = chunk->tool_call_id_len;
        break;
    case HU_STREAM_TOOL_DELTA:
        ev.type = HU_AGENT_STREAM_TOOL_ARGS;
        ev.data = chunk->delta;
        ev.data_len = chunk->delta_len;
        ev.tool_name = chunk->tool_name;
        ev.tool_name_len = chunk->tool_name_len;
        ev.tool_call_id = chunk->tool_call_id;
        ev.tool_call_id_len = chunk->tool_call_id_len;
        break;
    case HU_STREAM_TOOL_DONE:
        return true; /* handled after stream_chat returns */
    }
    w->on_event(&ev, w->event_ctx);
    return true;
}

/* Tool streaming: bridges tool execute_streaming chunks to agent stream events */
typedef struct tool_stream_bridge {
    hu_agent_stream_event_cb on_event;
    void *event_ctx;
    const char *tool_name;
    size_t tool_name_len;
    const char *tool_call_id;
    size_t tool_call_id_len;
} tool_stream_bridge_t;

static void tool_chunk_to_event(void *ctx, const char *data, size_t len) {
    tool_stream_bridge_t *b = (tool_stream_bridge_t *)ctx;
    if (!b->on_event || !data || len == 0)
        return;
    hu_agent_stream_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = HU_AGENT_STREAM_TOOL_RESULT;
    ev.data = data;
    ev.data_len = len;
    ev.tool_name = b->tool_name;
    ev.tool_name_len = b->tool_name_len;
    ev.tool_call_id = b->tool_call_id;
    ev.tool_call_id_len = b->tool_call_id_len;
    ev.is_error = false;
    b->on_event(&ev, b->event_ctx);
}

hu_error_t hu_agent_turn_stream(hu_agent_t *agent, const char *msg, size_t msg_len,
                                hu_agent_stream_token_cb on_token, void *token_ctx,
                                char **response_out, size_t *response_len_out) {
    if (!agent || !msg || !response_out)
        return HU_ERR_INVALID_ARGUMENT;
    if (!agent->provider.vtable)
        return HU_ERR_INVALID_ARGUMENT;
    *response_out = NULL;
    if (response_len_out)
        *response_len_out = 0;

    hu_agent_set_current_for_tools(agent);

    /* Reset per-turn state so behavior-log stash sees a clean slate. */
    hu_agent_turn_state_reset(agent);

    hu_agent_internal_process_mailbox_messages(agent);

    char *slash_resp = hu_agent_handle_slash_command(agent, msg, msg_len);
    if (slash_resp) {
        hu_agent_clear_current_for_tools();
        *response_out = slash_resp;
        if (response_len_out)
            *response_len_out = strlen(slash_resp);
        return HU_OK;
    }

    bool can_stream = (on_token != NULL) && agent->provider.vtable->supports_streaming &&
                      agent->provider.vtable->supports_streaming(agent->provider.ctx) &&
                      agent->provider.vtable->stream_chat;

    if (!can_stream) {
        hu_error_t fallback_err =
            hu_agent_turn(agent, msg, msg_len, response_out, response_len_out);
        if (fallback_err == HU_OK && on_token && *response_out && response_len_out &&
            *response_len_out > 0) {
            size_t chunk_size = 12;
            for (size_t i = 0; i < *response_len_out; i += chunk_size) {
                size_t n = *response_len_out - i;
                if (n > chunk_size)
                    n = chunk_size;
                on_token(*response_out + i, n, token_ctx);
            }
        }
        hu_agent_clear_current_for_tools();
        return fallback_err;
    }

    /* When tools are present, use the v2 streaming loop which interleaves
     * streaming text with tool execution (Claude Desktop-style). */
    bool has_tools = (agent->tool_specs_count > 0);
    if (has_tools) {
        v1_shim_ctx_t shim = {.on_token = on_token, .token_ctx = token_ctx};
        hu_agent_clear_current_for_tools();
        return hu_agent_turn_stream_v2(agent, msg, msg_len, v1_shim_event_cb, &shim, response_out,
                                       response_len_out);
    }

    /* V1 no-tools path: route through batch (hu_agent_turn) for full frontier
     * parity, then emit synthetic token callbacks from the final response. */
    {
        hu_agent_clear_current_for_tools();
        hu_error_t batch_err = hu_agent_turn(agent, msg, msg_len, response_out, response_len_out);
        if (batch_err == HU_OK && on_token && *response_out && response_len_out &&
            *response_len_out > 0) {
            size_t chunk_size = 12;
            for (size_t i = 0; i < *response_len_out; i += chunk_size) {
                size_t n = *response_len_out - i;
                if (n > chunk_size)
                    n = chunk_size;
                on_token(*response_out + i, n, token_ctx);
            }
        }
        return batch_err;
    }
}

/* ──────────────────────────────────────────────────────────────────────────
 * hu_agent_turn_stream_v2 — Rich streaming with interleaved tool execution
 *
 * Streams text tokens between tool calls (Claude Desktop-style):
 *   1. Call provider stream_chat (with tools)
 *   2. Text deltas → emit HU_AGENT_STREAM_TEXT
 *   3. Tool call detected → emit TOOL_START / TOOL_ARGS during stream
 *   4. After stream completes with tool calls: execute each tool, emit TOOL_RESULT
 *   5. Loop back to step 1 with tool results in context
 *   6. When no tool calls remain: final text is the response
 * ────────────────────────────────────────────────────────────────────────── */

#define STREAM_V2_MAX_TOOL_DEPTH 10

hu_error_t hu_agent_turn_stream_v2(hu_agent_t *agent, const char *msg, size_t msg_len,
                                   hu_agent_stream_event_cb on_event, void *event_ctx,
                                   char **response_out, size_t *response_len_out) {
    if (!agent || !msg || !response_out)
        return HU_ERR_INVALID_ARGUMENT;
    if (!agent->provider.vtable)
        return HU_ERR_INVALID_ARGUMENT;
    *response_out = NULL;
    if (response_len_out)
        *response_len_out = 0;

    hu_agent_set_current_for_tools(agent);

    /* Reset per-turn state so behavior-log stash sees a clean slate. */
    hu_agent_turn_state_reset(agent);

    hu_agent_internal_process_mailbox_messages(agent);

    /* Free any previously-built humanness context, then build fresh for this turn */
    hu_agent_free_turn_context(agent);
    hu_agent_build_turn_context(agent);

    char *slash_resp = hu_agent_handle_slash_command(agent, msg, msg_len);
    if (slash_resp) {
        hu_agent_clear_current_for_tools();
        *response_out = slash_resp;
        if (response_len_out)
            *response_len_out = strlen(slash_resp);
        return HU_OK;
    }

    bool can_stream = (on_event != NULL) && agent->provider.vtable->supports_streaming &&
                      agent->provider.vtable->supports_streaming(agent->provider.ctx) &&
                      agent->provider.vtable->stream_chat;

    /* ROOT-CAUSED + FIXED (2026-05-28): The Gemini streaming response_len=0
     * symptom was a cross-feed event-assembly bug in the SHARED provider SSE
     * parser (hu_provider_sse_parser_feed, src/providers/sse.c). Vertex's
     * :streamGenerateContent endpoint frames over HTTP/2 and routinely splits
     * a "data:" line and its terminating "\n\n" across separate libcurl
     * write-callback (feed) calls. The parser's consumed-watermark advanced
     * past a data line BEFORE the event was terminated, so the data was
     * dropped from the retained buffer and the event fired empty. Fixed by
     * advancing the watermark only at the blank-line terminator. Pinned by
     * tests/test_streaming.c::test_sse_parser_event_boundary_split_{across_feeds,crlf}.
     *
     * The C-side gemini.c stream path (gemini_process_sse_json, the feed
     * write-callback, content_buf→out transfer) was always correct — the bug
     * was entirely in the shared parser, which is why the non-streaming path
     * was unaffected.
     *
     * The bypass below is RETAINED as a conservative default pending a live
     * Apple-Silicon/Vertex smoke test (cannot be run in the unit-test env;
     * "verify, don't assert"). iMessage has no token-by-token UX (AX types
     * the whole reply after agent_turn returns), so leaving it off is a no-op
     * for the current consumer. To RE-ENABLE Gemini streaming after a live
     * smoke test confirms token chunks deliver end-to-end, delete the
     * `can_stream = false;` line below.
     *
     * Context: docs/plans/2026-05-24-reactive-imessage-recovery/
     * Search for "TODO(gemini-stream-bypass)" to find this site later. */
    if (can_stream && agent->provider.vtable->get_name) {
        const char *pname = agent->provider.vtable->get_name(agent->provider.ctx);
        if (pname && strcmp(pname, "gemini") == 0) {
            /* TODO(gemini-stream-bypass): SSE parser root cause fixed
             * 2026-05-28; remove this line after a live Vertex smoke test. */
            can_stream = false;
        }
        /* M3 B4 T3 (2026-05-26) — operator-gated bypass of the
         * compatible-provider streaming workaround.
         *
         * Original 2026-05-25 workaround: The "compatible" provider
         * (used by mlx_local serving Gemma 4) streams raw
         * `<|channel>thought` markers as visible text because
         * mlx-server.py's strip_thought_channels postprocessor only
         * runs in the NON-streaming path. Forcing can_stream=false
         * for ALL compatible services was the safe-but-broad fix.
         *
         * T3 replaces that with a per-provider, operator-gated check:
         *   - For "compatible" providers, the default `cfg.mlx_local.
         *     streaming_enabled = false` keeps the safe-off behavior.
         *   - Operator can opt IN by setting streaming_enabled=true
         *     in config.json once they've verified their mlx-server
         *     strips thought markers in streaming mode (or once a
         *     client-side filter lands in T4+).
         *   - When agent->config is NULL (cold init / test path), we
         *     keep the original safe-off behavior to avoid regressions.
         *
         * Other "compatible" services (OpenRouter etc.) are still
         * affected by the default-false — per-service whitelist is
         * future work (see spec D6 followup). */
        if (pname && strcmp(pname, "compatible") == 0) {
            bool streaming_opt_in = agent->config && agent->config->mlx_local.streaming_enabled;
            if (!streaming_opt_in)
                can_stream = false;
        }
    }

    if (getenv("HU_DEBUG"))
        hu_log_info("agent_stream", NULL, "stream_v2: can_stream=%d msg_len=%zu", can_stream,
                    msg_len);

    /* Fallback: if provider can't stream, use batch turn and emit synthetic events */
    if (!can_stream) {
        hu_error_t err = hu_agent_turn(agent, msg, msg_len, response_out, response_len_out);
        if (err == HU_OK && on_event && *response_out && response_len_out &&
            *response_len_out > 0) {
            hu_agent_stream_event_t ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = HU_AGENT_STREAM_TEXT;
            size_t chunk_size = 12;
            for (size_t i = 0; i < *response_len_out; i += chunk_size) {
                size_t n = *response_len_out - i;
                if (n > chunk_size)
                    n = chunk_size;
                ev.data = *response_out + i;
                ev.data_len = n;
                on_event(&ev, event_ctx);
            }
        }
        hu_agent_clear_current_for_tools();
        return err;
    }

    /* Prompt injection defense-in-depth (must run BEFORE history append) */
    {
        hu_injection_risk_t risk = HU_INJECTION_SAFE;
        hu_error_t guard_err = hu_input_guard_check(msg, msg_len, &risk);
        if (guard_err != HU_OK) {
            hu_agent_clear_current_for_tools();
            return guard_err;
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
            return HU_OK;
        }
    }

    /* Append user message to history */
    hu_error_t err =
        hu_agent_internal_append_history(agent, HU_ROLE_USER, msg, msg_len, NULL, 0, NULL, 0);
    if (err != HU_OK) {
        hu_agent_clear_current_for_tools();
        return err;
    }

#ifndef HU_IS_TEST
    {
        /* SOTA-2026 init-09: stamp provenance derived from the active
         * channel so the trust gate + MINJA detector run in production. */
        hu_provenance_t _ingest_prov = hu_channel_trust_stamp(
            agent->active_channel, agent->active_channel_len, NULL, 0, (int64_t)time(NULL));
        (void)hu_personal_model_ingest(&agent->personal_model, msg, msg_len, true,
                                       (int64_t)time(NULL), &_ingest_prov);
    }
    if (agent->auto_save && hu_personal_model_has_content(&agent->personal_model)) {
        char pm_path[1024];
        if (hu_personal_model_resolve_default_path(pm_path, sizeof(pm_path)))
            (void)hu_personal_model_save(&agent->personal_model, pm_path);
    }
#endif

    if (agent->persona && agent->persona->chronotype == HU_CHRONO_UNKNOWN) {
        hu_chronotype_t inferred = hu_personal_model_infer_chronotype(&agent->personal_model);
        if (inferred != HU_CHRONO_UNKNOWN)
            agent->persona->chronotype = inferred;
    }

    /* Build system prompt (memory, persona, awareness, outcomes) */
    char *memory_ctx = NULL;
    size_t memory_ctx_len = 0;
    char *graph_ctx = NULL;
    size_t graph_ctx_len = 0;
    if (agent->memory && agent->memory->vtable) {
        hu_memory_loader_t loader;
        hu_memory_loader_init(&loader, agent->alloc, agent->memory, agent->retrieval_engine, 10,
                              4000);
        hu_memory_loader_set_facade(&loader, agent->w7_facade);
        hu_memory_loader_set_personal_model(&loader, &agent->personal_model);
        /* Story B (sprint-4 follow-up): mirror agent_turn.c — bind persona
         * context so the streaming-path loader render also gets persona
         * merge. */
        hu_persona_context_t loader_pctx = {0};
        if (agent->persona) {
            loader_pctx.persona = agent->persona;
            loader_pctx.channel = agent->active_channel;
            loader_pctx.channel_len = agent->active_channel_len;
            loader_pctx.delta_limit = 8;
            loader_pctx.tools = agent->tools;
            loader_pctx.tools_count = agent->tools_count;
            hu_memory_loader_set_persona_context(&loader, &loader_pctx);
        }
        hu_error_t mem_err = hu_memory_loader_load(
            &loader, msg, msg_len, agent->memory_session_id ? agent->memory_session_id : "",
            agent->memory_session_id ? agent->memory_session_id_len : 0, &memory_ctx,
            &memory_ctx_len);
        if (mem_err != HU_OK && mem_err != HU_ERR_NOT_SUPPORTED)
            hu_log_error("agent_stream_v2", NULL, "memory_loader_load failed: %s",
                         hu_error_string(mem_err));
        hu_graph_grounding_mode_t graph_mode = hu_graph_grounding_mode();
        if (graph_mode != HU_GRAPH_GROUNDING_OFF && agent->memory_session_id &&
            agent->memory_session_id_len > 0) {
            hu_graph_ground_load(&loader, agent->memory_session_id, agent->memory_session_id_len, 0,
                                 &graph_ctx, &graph_ctx_len);
            if (graph_mode == HU_GRAPH_GROUNDING_SHADOW) {
                hu_log_info("graph_grounding", NULL,
                            "shadow: %zu graph_context bytes (not injected)", graph_ctx_len);
                if (graph_ctx)
                    agent->alloc->free(agent->alloc->ctx, graph_ctx, graph_ctx_len + 1);
                graph_ctx = NULL;
                graph_ctx_len = 0;
            }
        }
    }

    char *awareness_ctx = NULL;
    size_t awareness_ctx_len = 0;
    if (agent->awareness && !agent->lean_prompt)
        awareness_ctx = hu_awareness_context(agent->awareness, agent->alloc, &awareness_ctx_len);

    char *outcome_ctx = NULL;
    size_t outcome_ctx_len = 0;
    if (agent->outcomes && !agent->lean_prompt)
        outcome_ctx = hu_outcome_build_summary(agent->outcomes, agent->alloc, &outcome_ctx_len);

    char *persona_prompt = NULL;
    size_t persona_prompt_len = 0;
    if (agent->persona) {
        if (agent->lean_prompt) {
            /* Lean persona: identity + output constraint + core_anchor + reinforcement
             * + anti_patterns + style_rules + channel overlay. */
            char lp[16384];
            size_t lpo = 0;
            {
                const hu_persona_t *pp = agent->persona;
                if (pp->identity) {
                    int n = snprintf(lp + lpo, sizeof(lp) - lpo, "You ARE this person: %s\n",
                                     pp->identity);
                    if (n > 0 && lpo + (size_t)n < sizeof(lp))
                        lpo += (size_t)n;
                }
                if (pp->biography) {
                    int n = snprintf(lp + lpo, sizeof(lp) - lpo, "%s\n", pp->biography);
                    if (n > 0 && lpo + (size_t)n < sizeof(lp))
                        lpo += (size_t)n;
                }
                static const char constraint[] =
                    "Output ONLY what this person would actually type — nothing else. "
                    "No reasoning, no parentheses, no meta-commentary, no analysis. "
                    "Just the raw text message, exactly as it would appear on screen.\n";
                int n = snprintf(lp + lpo, sizeof(lp) - lpo, "%s", constraint);
                if (n > 0 && lpo + (size_t)n < sizeof(lp))
                    lpo += (size_t)n;
                for (size_t ri = 0; ri < pp->communication_rules_count && ri < 12; ri++) {
                    if (pp->communication_rules[ri]) {
                        n = snprintf(lp + lpo, sizeof(lp) - lpo, "- %s\n",
                                     pp->communication_rules[ri]);
                        if (n > 0 && lpo + (size_t)n < sizeof(lp))
                            lpo += (size_t)n;
                    }
                }
                n = snprintf(lp + lpo, sizeof(lp) - lpo, "\n");
                if (n > 0 && lpo + (size_t)n < sizeof(lp))
                    lpo += (size_t)n;
            }
            const hu_persona_t *p = agent->persona;
            if (p->core_anchor) {
                int n = snprintf(lp + lpo, sizeof(lp) - lpo, "%s\n\n", p->core_anchor);
                if (n > 0 && lpo + (size_t)n < sizeof(lp))
                    lpo += (size_t)n;
            }
            for (size_t i = 0; i < p->immersive_reinforcement_count && i < 10; i++) {
                if (p->immersive_reinforcement[i]) {
                    int n = snprintf(lp + lpo, sizeof(lp) - lpo, "- %s\n",
                                     p->immersive_reinforcement[i]);
                    if (n > 0 && lpo + (size_t)n < sizeof(lp))
                        lpo += (size_t)n;
                }
            }
            if (p->anti_patterns_count > 0) {
                int n = snprintf(lp + lpo, sizeof(lp) - lpo, "\nNEVER do:\n");
                if (n > 0 && lpo + (size_t)n < sizeof(lp))
                    lpo += (size_t)n;
                for (size_t i = 0; i < p->anti_patterns_count; i++) {
                    if (p->anti_patterns[i]) {
                        n = snprintf(lp + lpo, sizeof(lp) - lpo, "- %s\n", p->anti_patterns[i]);
                        if (n > 0 && lpo + (size_t)n < sizeof(lp))
                            lpo += (size_t)n;
                    }
                }
            }
            if (p->style_rules_count > 0) {
                int n = snprintf(lp + lpo, sizeof(lp) - lpo, "\nStyle:\n");
                if (n > 0 && lpo + (size_t)n < sizeof(lp))
                    lpo += (size_t)n;
                for (size_t i = 0; i < p->style_rules_count; i++) {
                    if (p->style_rules[i]) {
                        n = snprintf(lp + lpo, sizeof(lp) - lpo, "- %s\n", p->style_rules[i]);
                        if (n > 0 && lpo + (size_t)n < sizeof(lp))
                            lpo += (size_t)n;
                    }
                }
            }
            /* Add examples to prime the model on correct tone.
             * hu_persona_select_examples writes up to N pointers into out[]
             * (signature: const hu_persona_example_t **out). Previously this
             * passed &exs of a single pointer (1×8 bytes) with capacity 5,
             * which produced a stack-buffer-overflow caught by ASan and a
             * misuse of exs[ei] as an object instead of a pointer below. */
            {
                const hu_persona_example_t *exs[5] = {NULL};
                size_t ex_count = 0;
                hu_persona_select_examples(p, agent->active_channel, agent->active_channel_len,
                                           NULL, 0, exs, &ex_count, 5,
                                           &agent->personal_model.style);
                if (ex_count > 0) {
                    int n = snprintf(lp + lpo, sizeof(lp) - lpo, "\nExamples of how you text:\n");
                    if (n > 0 && lpo + (size_t)n < sizeof(lp))
                        lpo += (size_t)n;
                    for (size_t ei = 0; ei < ex_count; ei++) {
                        if (exs[ei] && exs[ei]->incoming && exs[ei]->response) {
                            n = snprintf(lp + lpo, sizeof(lp) - lpo, "them: %s\nyou: %s\n\n",
                                         exs[ei]->incoming, exs[ei]->response);
                            if (n > 0 && lpo + (size_t)n < sizeof(lp))
                                lpo += (size_t)n;
                        }
                    }
                }
            }
            /* RAG-over-own-messages voice grounding (default off): retrieve
             * Seth's most-similar real past messages to THIS incoming message and
             * inject them as dynamic few-shot grounding — the SOTA RAG leg next to
             * the fine-tuned adapter + personal model.
             *
             * Register-conditional (live A/B 2026-05-29, rag-ab-live-verdict.json):
             * RAG grounding HELPS the substantive register (+0.110) but slightly
             * hurts casual (-0.078, richer context fights curt brevity). So gate it
             * on ANALYTICAL/DEEP turns only; REFLEXIVE/CONVERSATIONAL and unknown
             * tier (turn_tier < 0) skip it. */
            if (agent->config && agent->config->agent.rag_grounding_enabled &&
                agent->turn_tier >= (int)HU_TIER_ANALYTICAL) {
                const char *home = getenv("HOME");
                if (home && *home) {
                    char qbuf[512];
                    size_t qn = msg_len < sizeof(qbuf) - 1 ? msg_len : sizeof(qbuf) - 1;
                    if (msg && qn > 0) {
                        memcpy(qbuf, msg, qn);
                        qbuf[qn] = '\0';
                        char cpath[768];
                        int pn =
                            snprintf(cpath, sizeof(cpath), "%s/.human/voice_corpus.jsonl", home);
                        if (pn > 0 && (size_t)pn < sizeof(cpath)) {
                            char rag_buf[2048];
                            size_t rn = hu_persona_rag_ground_from_file(
                                qbuf, cpath, 3, rag_buf, sizeof(rag_buf), agent->alloc);
                            if (rn > 0) {
                                int n = snprintf(lp + lpo, sizeof(lp) - lpo, "\n%s", rag_buf);
                                if (n > 0 && lpo + (size_t)n < sizeof(lp))
                                    lpo += (size_t)n;
                            }
                        }
                    }
                }
            }
            const hu_persona_overlay_t *ov =
                hu_persona_find_overlay(p, agent->active_channel, agent->active_channel_len);
            if (ov) {
                int n = snprintf(lp + lpo, sizeof(lp) - lpo, "\nChannel style:");
                if (n > 0 && lpo + (size_t)n < sizeof(lp))
                    lpo += (size_t)n;
                if (ov->formality) {
                    n = snprintf(lp + lpo, sizeof(lp) - lpo, " %s.", ov->formality);
                    if (n > 0 && lpo + (size_t)n < sizeof(lp))
                        lpo += (size_t)n;
                }
                if (ov->avg_length) {
                    n = snprintf(lp + lpo, sizeof(lp) - lpo, " Length: %s.", ov->avg_length);
                    if (n > 0 && lpo + (size_t)n < sizeof(lp))
                        lpo += (size_t)n;
                }
                if (ov->emoji_usage) {
                    n = snprintf(lp + lpo, sizeof(lp) - lpo, " Emoji: %s.", ov->emoji_usage);
                    if (n > 0 && lpo + (size_t)n < sizeof(lp))
                        lpo += (size_t)n;
                }
                for (size_t i = 0; i < ov->style_notes_count; i++) {
                    if (ov->style_notes[i]) {
                        n = snprintf(lp + lpo, sizeof(lp) - lpo, " %s.", ov->style_notes[i]);
                        if (n > 0 && lpo + (size_t)n < sizeof(lp))
                            lpo += (size_t)n;
                    }
                }
                n = snprintf(lp + lpo, sizeof(lp) - lpo, "\n");
                if (n > 0 && lpo + (size_t)n < sizeof(lp))
                    lpo += (size_t)n;
            }
            /* Hard override block — last instruction has highest weight with LLMs.
             * Addresses base model habits that resist fine-tuning.
             * P6-5: shared helper, same source of truth as the proactive path. */
            {
                char rules_buf[2048];
                size_t rules_len = 0;
                /* Formality-aware: professional contacts get capitalized/punctuated
                 * register, not the casual friend-voice (fixes register mismatch). */
                if (hu_persona_build_absolute_rules_fmt(p, ov ? ov->formality : NULL, rules_buf,
                                                        sizeof(rules_buf), &rules_len) == HU_OK &&
                    rules_len > 0) {
                    int n = snprintf(lp + lpo, sizeof(lp) - lpo, "%s", rules_buf);
                    if (n > 0 && lpo + (size_t)n < sizeof(lp))
                        lpo += (size_t)n;
                }
            }
            if (lpo > 0) {
                persona_prompt = hu_strndup(agent->alloc, lp, lpo);
                persona_prompt_len = lpo;
            }
        } else {
            const char *ch = agent->active_channel;
            size_t ch_len = agent->active_channel_len;
            hu_error_t perr =
                hu_persona_build_prompt(agent->alloc, agent->persona, ch, ch_len, NULL, 0,
                                        &persona_prompt, &persona_prompt_len);
            if (perr != HU_OK) {
                if (memory_ctx)
                    agent->alloc->free(agent->alloc->ctx, memory_ctx, memory_ctx_len + 1);
                if (graph_ctx)
                    agent->alloc->free(agent->alloc->ctx, graph_ctx, graph_ctx_len + 1);
                if (awareness_ctx)
                    agent->alloc->free(agent->alloc->ctx, awareness_ctx, awareness_ctx_len + 1);
                if (outcome_ctx)
                    agent->alloc->free(agent->alloc->ctx, outcome_ctx, outcome_ctx_len + 1);
                hu_agent_clear_current_for_tools();
                return perr;
            }
        }
    }

    /* Intelligence context: learned behaviors, online learning, value learning.
     * Skip in lean_prompt mode: not needed for fast texting. */
    char *intelligence_ctx = NULL;
    size_t intelligence_ctx_len = 0;
#ifdef HU_ENABLE_SQLITE
    if (agent->memory && !agent->lean_prompt) {
        sqlite3 *idb = hu_sqlite_memory_get_db(agent->memory);
        if (idb) {
            char ip[4096];
            size_t ipo = 0;
            {
                hu_self_improve_t si;
                if (hu_self_improve_create(agent->alloc, idb, &si) == HU_OK) {
                    char *p = NULL;
                    size_t pl = 0;
                    if (hu_self_improve_get_prompt_patches(&si, &p, &pl) == HU_OK && p && pl > 0) {
                        int n = snprintf(ip + ipo, sizeof(ip) - ipo,
                                         "### Learned Behaviors\n%.*s\n", (int)pl, p);
                        if (n > 0 && ipo + (size_t)n < sizeof(ip))
                            ipo += (size_t)n;
                        agent->alloc->free(agent->alloc->ctx, p, pl + 1);
                    }
                    hu_self_improve_deinit(&si);
                }
            }
            {
                hu_online_learning_t ol;
                if (hu_online_learning_create(agent->alloc, idb, 0.1, &ol) == HU_OK) {
                    char *lc = NULL;
                    size_t ll = 0;
                    if (hu_online_learning_build_context(&ol, &lc, &ll) == HU_OK && lc && ll > 0) {
                        int n = snprintf(ip + ipo, sizeof(ip) - ipo, "### %.*s\n", (int)ll, lc);
                        if (n > 0 && ipo + (size_t)n < sizeof(ip))
                            ipo += (size_t)n;
                        agent->alloc->free(agent->alloc->ctx, lc, ll + 1);
                    }
                    hu_online_learning_deinit(&ol);
                }
            }
            {
                hu_value_engine_t ve;
                if (hu_value_engine_create(agent->alloc, idb, &ve) == HU_OK) {
                    char *vc = NULL;
                    size_t vl = 0;
                    if (hu_value_build_prompt(&ve, &vc, &vl) == HU_OK && vc && vl > 0) {
                        int n = snprintf(ip + ipo, sizeof(ip) - ipo, "### %.*s\n", (int)vl, vc);
                        if (n > 0 && ipo + (size_t)n < sizeof(ip))
                            ipo += (size_t)n;
                        agent->alloc->free(agent->alloc->ctx, vc, vl + 1);
                    }
                    hu_value_engine_deinit(&ve);
                }
            }
            if (ipo > 0) {
                intelligence_ctx = hu_strndup(agent->alloc, ip, ipo);
                intelligence_ctx_len = ipo;
            }
        }
    }
#endif
    /* Use the pre-computed tier from the caller (CLI/daemon) to gate tools.
     * Casual conversation (REFLEXIVE/CONVERSATIONAL) doesn't need 80 tool
     * descriptions competing for model attention.  -1 = unset → include tools. */
    hu_cognitive_tier_t early_tier = HU_TIER_ANALYTICAL;
    if (agent->turn_tier >= 0)
        early_tier = (hu_cognitive_tier_t)agent->turn_tier;
    else if (!agent->turn_model || agent->turn_model_len == 0) {
        hu_model_router_config_t mr_cfg = hu_model_router_default_config();
        const char *rel_early = NULL;
        size_t rel_early_len = 0;
        if (agent->relationship.stage >= HU_REL_TRUSTED) {
            rel_early = "trusted";
            rel_early_len = 7;
        } else if (agent->relationship.stage >= HU_REL_FAMILIAR) {
            rel_early = "friend";
            rel_early_len = 6;
        }
        hu_model_selection_t early_sel = hu_model_route(&mr_cfg, msg, msg_len, rel_early,
                                                        rel_early_len, -1, agent->history_count);
        early_tier = early_sel.tier;
    }
    bool turn_needs_tools = (early_tier >= HU_TIER_ANALYTICAL);

    /* Build frontier context for the streaming prompt (matching batch path) */
    bool had_humor_dir = false;
    int humor_theory_saved = 0;
    (void)humor_theory_saved;
    char *somatic_ctx = NULL, *trust_ctx = NULL, *humor_dir = NULL;
    size_t somatic_ctx_len = 0, trust_ctx_len = 0, humor_dir_len = 0;
    char *syc_friction_ctx = NULL;
    size_t syc_friction_ctx_len = 0;
    const char *tone_hint = NULL;
    size_t tone_hint_len = 0;

    if (agent->frontiers.initialized && !agent->lean_prompt) {
        hu_somatic_build_context(agent->alloc, &agent->frontiers.somatic, &somatic_ctx,
                                 &somatic_ctx_len);
        hu_tcal_build_context(agent->alloc, &agent->frontiers.trust, &trust_ctx, &trust_ctx_len);
        /* Humor with persona style bridging */
        hu_humor_context_t hctx;
        memset(&hctx, 0, sizeof(hctx));
        hctx.risk_tolerance = 0.4f; /* HU_HUMOR_RISK_TOLERANCE */
        hctx.in_serious_context = (agent->infra.emotional_cognition.state.intensity > 0.7f &&
                                   agent->infra.emotional_cognition.state.valence < -0.2f);
        if (agent->active_channel) {
            hctx.channel = agent->active_channel;
            hctx.channel_len = agent->active_channel_len;
        }
        if (agent->persona && agent->persona->humor.style_count > 0) {
            for (size_t hs = 0; hs < agent->persona->humor.style_count && hctx.preferred_count < 8;
                 hs++) {
                const char *s = agent->persona->humor.style[hs];
                hu_humor_fw_style_t mapped = HU_HUMOR_FW_OBSERVATIONAL;
                if (strstr(s, "dry") || strstr(s, "deadpan"))
                    mapped = HU_HUMOR_FW_DRY;
                else if (strstr(s, "self") || strstr(s, "deprecat"))
                    mapped = HU_HUMOR_FW_SELF_DEPRECATING;
                else if (strstr(s, "word") || strstr(s, "pun"))
                    mapped = HU_HUMOR_FW_WORDPLAY;
                else if (strstr(s, "absurd") || strstr(s, "surreal"))
                    mapped = HU_HUMOR_FW_ABSURDIST;
                hctx.preferred_styles[hctx.preferred_count++] = mapped;
            }
        }
        hu_humor_evaluation_t heval;
        memset(&heval, 0, sizeof(heval));
        hu_humor_fw_evaluate_context(msg, msg_len, &hctx, &heval);
        if (heval.should_attempt) {
            hu_humor_fw_build_directive(agent->alloc, &heval, &hctx, &humor_dir, &humor_dir_len);
            humor_theory_saved = (int)heval.suggested_theory;
        }

        /* Sycophancy pre-check: scan for opinion patterns */
        {
            static const char *opinion_pats[] = {
                "i think", "i believe",        "i feel",    "in my opinion", "don't you think",
                "right?",  "wouldn't you say", "obviously", "clearly",       "everyone knows",
            };
            size_t opinion_hits = 0;
            for (size_t pi = 0; pi < sizeof(opinion_pats) / sizeof(opinion_pats[0]); pi++) {
                size_t plen = strlen(opinion_pats[pi]);
                for (size_t mi = 0; mi + plen <= msg_len; mi++) {
                    bool m = true;
                    for (size_t k = 0; k < plen; k++) {
                        char a = (char)(msg[mi + k] >= 'A' && msg[mi + k] <= 'Z' ? msg[mi + k] + 32
                                                                                 : msg[mi + k]);
                        if (a != opinion_pats[pi][k]) {
                            m = false;
                            break;
                        }
                    }
                    if (m) {
                        opinion_hits++;
                        break;
                    }
                }
            }
            if (opinion_hits >= 2) {
                hu_sycophancy_result_t syc_synth;
                memset(&syc_synth, 0, sizeof(syc_synth));
                syc_synth.flagged = true;
                syc_synth.total_risk = 0.5f;
                syc_synth.factor_scores[HU_SYCOPHANCY_UNCRITICAL_AGREEMENT] =
                    (float)opinion_hits * 0.25f;
                hu_sycophancy_build_friction(agent->alloc, &syc_synth, msg, msg_len,
                                             &syc_friction_ctx, &syc_friction_ctx_len);
            }
        }
    }

    /* Rhythm matching */
    {
        static const char rhythm_short[] =
            " The user sent a very short message — match their energy with a brief, "
            "conversational reply. Don't over-explain.";
        static const char rhythm_long[] =
            " The user wrote a long, thoughtful message — give it the space it deserves "
            "with a proportional, considered response.";
        if (msg_len <= 15 && msg_len > 0) {
            tone_hint = rhythm_short;
            tone_hint_len = sizeof(rhythm_short) - 1;
        } else if (msg_len >= 400) {
            tone_hint = rhythm_long;
            tone_hint_len = sizeof(rhythm_long) - 1;
        }
    }

    /* Build STM context for this turn (matching batch path in agent_turn.c) */
    char *stm_ctx = NULL;
    size_t stm_ctx_len = 0;
    {
        hu_error_t stm_err =
            hu_stm_build_context(&agent->stm, agent->alloc, &stm_ctx, &stm_ctx_len);
        if (stm_err != HU_OK)
            hu_log_error("agent_stream", NULL, "STM context build failed: %s",
                         hu_error_string(stm_err));
        if (stm_ctx_len > 0 && agent->bth_metrics)
            agent->bth_metrics->emotions_surfaced++;
    }

    /* Build commitment context for this turn */
    char *commitment_ctx = NULL;
    size_t commitment_ctx_len = 0;
    if (agent->commitment_store) {
        const char *sess = agent->memory_session_id;
        size_t sess_len = agent->memory_session_id ? agent->memory_session_id_len : 0;
        (void)hu_commitment_store_build_context(agent->commitment_store, agent->alloc, sess,
                                                sess_len, &commitment_ctx, &commitment_ctx_len);
        if (commitment_ctx_len > 0 && agent->bth_metrics)
            agent->bth_metrics->commitment_followups++;
    }

    /* Build pattern radar context for this turn */
    char *pattern_ctx = NULL;
    size_t pattern_ctx_len = 0;
    (void)hu_pattern_radar_build_context(&agent->radar, agent->alloc, &pattern_ctx,
                                         &pattern_ctx_len);
    if (pattern_ctx_len > 0 && agent->bth_metrics)
        agent->bth_metrics->pattern_insights++;

    /* Load user preferences for prompt injection */
    char *pref_ctx = NULL;
    size_t pref_ctx_len = 0;
    if (agent->memory) {
        hu_error_t pref_err =
            hu_preferences_load(agent->memory, agent->alloc, &pref_ctx, &pref_ctx_len);
        if (pref_err != HU_OK)
            hu_log_error("agent_stream", NULL, "preferences load failed: %s",
                         hu_error_string(pref_err));
    }

    /* Gather instruction context from discovery results */
    char *instruction_ctx = NULL;
    size_t instruction_ctx_len = 0;
    if (agent->instruction_discovery && agent->instruction_discovery->merged_content &&
        agent->instruction_discovery->merged_content_len > 0) {
        instruction_ctx = agent->instruction_discovery->merged_content;
        instruction_ctx_len = agent->instruction_discovery->merged_content_len;
    }

    /* Emotional context from cognition subsystem */
    char *emotional_ctx = NULL;
    size_t emotional_ctx_len = 0;
    if (hu_emotional_cognition_build_prompt(agent->alloc, &agent->infra.emotional_cognition,
                                            &emotional_ctx, &emotional_ctx_len) != HU_OK) {
        emotional_ctx = NULL;
        emotional_ctx_len = 0;
    }

    /* Cognition: episodic replay */
    char *episodic_replay = NULL;
    size_t episodic_replay_len = 0;
#ifdef HU_ENABLE_SQLITE
    if (agent->infra.cognition_db &&
        (agent->infra.current_cognition_mode == HU_COGNITION_SLOW ||
         agent->infra.current_cognition_mode == HU_COGNITION_EMOTIONAL)) {
        hu_episodic_pattern_t *ep_patterns = NULL;
        size_t ep_cnt = 0;
        if (hu_episodic_retrieve(agent->infra.cognition_db, agent->alloc, msg, msg_len, 3,
                                 &ep_patterns, &ep_cnt) == HU_OK &&
            ep_cnt > 0) {
            if (hu_episodic_build_replay(agent->alloc, ep_patterns, ep_cnt, &episodic_replay,
                                         &episodic_replay_len) == HU_OK &&
                episodic_replay && agent->bth_metrics)
                agent->bth_metrics->episodic_replays++;
            hu_episodic_free_patterns(agent->alloc, ep_patterns, ep_cnt);
        }
    }
#endif

    /* Conversation goals context */
    char *conv_goals_ctx = NULL;
    size_t conv_goals_ctx_len = 0;
#ifdef HU_ENABLE_SQLITE
    if (agent->memory && agent->memory_session_id && agent->memory_session_id_len > 0) {
        sqlite3 *cg_db = hu_sqlite_memory_get_db(agent->memory);
        if (cg_db) {
            char qsql[512];
            size_t qsql_len = 0;
            if (hu_conv_goals_query_active_sql(agent->memory_session_id,
                                               agent->memory_session_id_len, qsql, sizeof(qsql),
                                               &qsql_len) == HU_OK) {
                sqlite3_stmt *gs = NULL;
                if (sqlite3_prepare_v2(cg_db, qsql, (int)qsql_len, &gs, NULL) == SQLITE_OK) {
                    hu_conv_goal_t goals[4];
                    size_t gc = 0;
                    while (sqlite3_step(gs) == SQLITE_ROW && gc < 4) {
                        memset(&goals[gc], 0, sizeof(goals[gc]));
                        const char *d = (const char *)sqlite3_column_text(gs, 2);
                        if (d) {
                            goals[gc].description = hu_strndup(agent->alloc, d, strlen(d));
                            goals[gc].description_len =
                                goals[gc].description ? strlen(goals[gc].description) : 0;
                        }
                        const char *sig = (const char *)sqlite3_column_text(gs, 3);
                        if (sig) {
                            goals[gc].success_signal = hu_strndup(agent->alloc, sig, strlen(sig));
                            goals[gc].success_signal_len =
                                goals[gc].success_signal ? strlen(goals[gc].success_signal) : 0;
                        }
                        goals[gc].priority = (hu_goal_priority_t)sqlite3_column_int(gs, 5);
                        goals[gc].attempts = (uint8_t)sqlite3_column_int(gs, 9);
                        goals[gc].max_attempts = (uint8_t)sqlite3_column_int(gs, 10);
                        gc++;
                    }
                    sqlite3_finalize(gs);
                    if (gc > 0)
                        hu_conv_goals_build_prompt(agent->alloc, goals, gc, &conv_goals_ctx,
                                                   &conv_goals_ctx_len);
                    for (size_t gi = 0; gi < gc; gi++)
                        hu_conv_goal_deinit(agent->alloc, &goals[gi]);
                }
            }
        }
    }
#endif

    /* Enriched contact context: base + style overlay + emotional context from memory */
    char *enriched_contact = NULL;
    size_t enriched_contact_len = 0;
    char *style_overlay = NULL;
    size_t style_overlay_len = 0;
    char *contact_emotional_ctx = NULL;
    size_t contact_emotional_ctx_len = 0;
#ifdef HU_ENABLE_SQLITE
    if (agent->memory && agent->memory_session_id && agent->memory_session_id_len > 0) {
        hu_contact_style_overlay_build(agent->alloc, agent->memory, agent->memory_session_id,
                                       agent->memory_session_id_len, &style_overlay,
                                       &style_overlay_len);
        hu_contact_emotional_context_build(agent->alloc, agent->memory, agent->memory_session_id,
                                           agent->memory_session_id_len, 5, &contact_emotional_ctx,
                                           &contact_emotional_ctx_len);
    }
    {
        size_t total = (agent->contact_context_len ? agent->contact_context_len : 0) +
                       (style_overlay_len ? style_overlay_len + 1 : 0) +
                       (contact_emotional_ctx_len ? contact_emotional_ctx_len + 1 : 0);
        if (total > 0) {
            enriched_contact = (char *)agent->alloc->alloc(agent->alloc->ctx, total + 1);
            if (enriched_contact) {
                size_t off = 0;
                if (agent->contact_context && agent->contact_context_len > 0) {
                    memcpy(enriched_contact + off, agent->contact_context,
                           agent->contact_context_len);
                    off += agent->contact_context_len;
                }
                if (style_overlay && style_overlay_len > 0) {
                    enriched_contact[off++] = '\n';
                    memcpy(enriched_contact + off, style_overlay, style_overlay_len);
                    off += style_overlay_len;
                }
                if (contact_emotional_ctx && contact_emotional_ctx_len > 0) {
                    enriched_contact[off++] = '\n';
                    memcpy(enriched_contact + off, contact_emotional_ctx,
                           contact_emotional_ctx_len);
                    off += contact_emotional_ctx_len;
                }
                enriched_contact[off] = '\0';
                enriched_contact_len = off;
            }
        }
    }
#endif

    /* Cognition mode string for prompt */
    const char *cognition_mode_str = hu_cognition_mode_name(agent->infra.current_cognition_mode);
    size_t cognition_mode_str_len = strlen(cognition_mode_str);

    /* Render personal-model prompt block. Stack-bounded; skipped on a
     * fresh model so we don't stamp tokens with placeholder text. The
     * cached-prompt fast path below intentionally ignores this — when the
     * model has any signal, the cache shortcut is bypassed and we go
     * through the full prompt builder so the [Personal Context] block
     * gets composed in. */
    char personal_model_buf[8192];
    const char *personal_model_ctx = NULL;
    size_t personal_model_ctx_len = 0;
    if (hu_personal_model_has_content(&agent->personal_model)) {
        /* T7 mirror of agent_turn.c: reflection-loop slice append. The
         * reflection slice needs the SQLite db handle; in builds without
         * HU_ENABLE_SQLITE there is no db, so fall through to the plain
         * prompt path (matching every other sqlite-gated block in this
         * file). */
        size_t pm_n = 0;
#ifdef HU_ENABLE_SQLITE
        if (agent->config && agent->config->reflection_loop.enabled && agent->memory &&
            agent->active_channel && agent->active_channel_len > 0) {
            sqlite3 *refl_db = hu_sqlite_memory_get_db(agent->memory);
            const hu_persona_overlay_t *refl_overlay =
                agent->persona ? hu_persona_find_overlay(agent->persona, agent->active_channel,
                                                         agent->active_channel_len)
                               : NULL;
            pm_n = hu_personal_model_build_prompt_with_reflection(
                &agent->personal_model, refl_overlay, refl_db, agent->active_channel,
                /*max_patterns=*/5, personal_model_buf, sizeof(personal_model_buf));
        } else
#endif
        {
            pm_n = hu_personal_model_build_prompt(&agent->personal_model, personal_model_buf,
                                                  sizeof(personal_model_buf));
        }
        if (pm_n > 0) {
            personal_model_ctx = personal_model_buf;
            personal_model_ctx_len = pm_n;
        }
    }

    /* W9 world-model snapshot (FIX 12). Mirror agent_turn.c -- streaming and
     * non-streaming paths must inject the same context or behavior diverges
     * by entry point. */
    char *world_model_ctx = NULL;
    size_t world_model_ctx_len = 0;
    if (agent->w7_facade && agent->memory_session_id && agent->memory_session_id_len > 0) {
        const char *tom_p = agent->tom_scenario_premise;
        const char *tom_q = agent->tom_scenario_question;
        const char *tom_c = agent->tom_scenario_category;
        size_t tom_p_len = tom_p[0] ? strlen(tom_p) : 0;
        size_t tom_q_len = tom_q[0] ? strlen(tom_q) : 0;
        size_t tom_c_len = tom_c[0] ? strlen(tom_c) : 0;
        /* Story B (sprint-4 follow-up): mirror agent_turn.c — thread persona
         * context so the streaming path gets the same interaction_style and
         * persona-grounded ToM as the non-streaming path. */
        hu_persona_context_t pctx = {0};
        const hu_persona_context_t *pctx_p = NULL;
        if (agent->persona) {
            pctx.persona = agent->persona;
            pctx.channel = agent->active_channel;
            pctx.channel_len = agent->active_channel_len;
            pctx.delta_limit = 8;
            pctx.tools = agent->tools;
            pctx.tools_count = agent->tools_count;
            pctx_p = &pctx;
        }
        hu_w7_render_world_model(agent->w7_facade, agent->alloc, agent->memory_session_id,
                                 agent->memory_session_id_len, 0, &world_model_ctx,
                                 &world_model_ctx_len, tom_p, tom_p_len, tom_q, tom_q_len, tom_c,
                                 tom_c_len, &agent->personal_model, pctx_p);
        if (world_model_ctx_len > 0)
            agent->world_model_loads++;
    }

    char *system_prompt = NULL;
    size_t system_prompt_len = 0;
    if (agent->cached_static_prompt && !persona_prompt && !awareness_ctx && !somatic_ctx &&
        !trust_ctx && !humor_dir && !tone_hint && !syc_friction_ctx && !intelligence_ctx &&
        !outcome_ctx && !personal_model_ctx && !world_model_ctx && !stm_ctx && !commitment_ctx &&
        !pattern_ctx && !pref_ctx && !instruction_ctx && !emotional_ctx && !episodic_replay &&
        !conv_goals_ctx && !enriched_contact) {
        err = hu_prompt_build_with_cache(agent->alloc, agent->cached_static_prompt,
                                         agent->cached_static_prompt_len, memory_ctx,
                                         memory_ctx_len, &system_prompt, &system_prompt_len);
        if (memory_ctx)
            agent->alloc->free(agent->alloc->ctx, memory_ctx, memory_ctx_len + 1);
        if (graph_ctx)
            agent->alloc->free(agent->alloc->ctx, graph_ctx, graph_ctx_len + 1);
        if (err != HU_OK) {
            hu_agent_clear_current_for_tools();
            return err;
        }
    } else {
        bool has_native_tools =
            (agent->provider.vtable->supports_native_tools &&
             agent->provider.vtable->supports_native_tools(agent->provider.ctx));
        hu_prompt_config_t cfg = {
            .provider_name = agent->provider.vtable->get_name(agent->provider.ctx),
            .provider_name_len = 0,
            .model_name = agent->model_name,
            .model_name_len = agent->model_name_len,
            .workspace_dir = agent->lean_prompt ? NULL : agent->workspace_dir,
            .workspace_dir_len = agent->lean_prompt ? 0 : agent->workspace_dir_len,
            .tools = turn_needs_tools ? agent->tools : NULL,
            .tools_count = turn_needs_tools ? agent->tools_count : 0,
            .memory_context = memory_ctx,
            .memory_context_len = memory_ctx_len,
            .graph_context = graph_ctx,
            .graph_context_len = graph_ctx_len,
            .stm_context = stm_ctx,
            .stm_context_len = stm_ctx_len,
            .commitment_context = commitment_ctx,
            .commitment_context_len = commitment_ctx_len,
            .pattern_context = pattern_ctx,
            .pattern_context_len = pattern_ctx_len,
            .autonomy_level = agent->autonomy_level,
            .custom_instructions = agent->lean_prompt ? NULL : agent->custom_instructions,
            .custom_instructions_len = agent->lean_prompt ? 0 : agent->custom_instructions_len,
            .persona_prompt = persona_prompt,
            .persona_prompt_len = persona_prompt_len,
            .preferences = pref_ctx,
            .preferences_len = pref_ctx_len,
            .chain_of_thought = agent->chain_of_thought,
            .awareness_context = awareness_ctx,
            .awareness_context_len = awareness_ctx_len,
            .outcome_context = outcome_ctx,
            .outcome_context_len = outcome_ctx_len,
            .persona_immersive = (persona_prompt && persona_prompt_len > 0),
            .native_tools = has_native_tools,
            .persona = agent->lean_prompt ? NULL : agent->persona,
            .contact_context = enriched_contact ? enriched_contact : agent->contact_context,
            .contact_context_len =
                enriched_contact ? enriched_contact_len : agent->contact_context_len,
            .conversation_context = agent->conversation_context,
            .conversation_context_len = agent->conversation_context_len,
            .max_response_chars = agent->max_response_chars,
            .intelligence_context = intelligence_ctx,
            .intelligence_context_len = intelligence_ctx_len,
            .instruction_context = instruction_ctx,
            .instruction_context_len = instruction_ctx_len,
            .somatic_context = somatic_ctx,
            .somatic_context_len = somatic_ctx_len,
            .trust_context = trust_ctx,
            .trust_context_len = trust_ctx_len,
            .humor_directive = humor_dir,
            .humor_directive_len = humor_dir_len,
            .sycophancy_friction = syc_friction_ctx,
            .sycophancy_friction_len = syc_friction_ctx_len,
            .tone_hint = tone_hint,
            .tone_hint_len = tone_hint_len,
            .emotional_context = emotional_ctx,
            .emotional_context_len = emotional_ctx_len,
            .cognition_mode = cognition_mode_str,
            .cognition_mode_len = cognition_mode_str_len,
            .episodic_replay = episodic_replay,
            .episodic_replay_len = episodic_replay_len,
            .conv_goals_context = conv_goals_ctx,
            .conv_goals_context_len = conv_goals_ctx_len,
            .personal_model_context = personal_model_ctx,
            .personal_model_context_len = personal_model_ctx_len,
            .world_model_context = world_model_ctx,
            .world_model_context_len = world_model_ctx_len,
            /* B3 Phase 3 — same trim-gate population as agent_turn.c so
             * streaming turns honor the same operator config. */
            .prompt_budget_trim_enabled =
                agent->config ? agent->config->prompt_budget.enabled : false,
            .prompt_budget_dead_field_min_bytes =
                agent->config ? agent->config->prompt_budget.dead_field_min_bytes : 0,
            .prompt_budget_min_samples_before_tag =
                agent->config ? agent->config->prompt_budget.min_samples_before_tag : 0,
            .prompt_budget_field_allowlist =
                agent->config ? agent->config->prompt_budget.field_allowlist : NULL,
            .prompt_budget_field_allowlist_count =
                agent->config ? agent->config->prompt_budget.field_allowlist_count : 0,
        };
        /* B3 Phase 3 — observation half of the wire. Stack stats array
         * captures per-field bytes; hu_prompt_budget_observe folds them
         * into the long-lived accumulator. Without this call the trim
         * gate (which keys off observation_count) stays inert forever. */
        hu_prompt_field_stat_t prompt_field_stats[HU_PROMPT_FIELD_COUNT] = {0};
        err = hu_prompt_build_system(agent->alloc, &cfg, prompt_field_stats, agent->prompt_budget,
                                     &system_prompt, &system_prompt_len);
        if (err == HU_OK && agent->prompt_budget) {
            hu_prompt_budget_observe(agent->prompt_budget, prompt_field_stats,
                                     HU_PROMPT_FIELD_COUNT);
        }
        /* Prompt-size budget guard — see agent_turn.c equivalent block.
         * Caps system prompt at 16 KB to avoid MLX backend empty-response
         * failures observed at body_len > ~28 KB on 2026-05-19. */
        if (err == HU_OK && system_prompt && system_prompt_len > 16384) {
            size_t budget = 16384;
            size_t cut = budget;
            while (cut > 0 && system_prompt[cut - 1] != '\n')
                cut--;
            if (cut < budget / 2)
                cut = budget;
            static atomic_bool warned_stream_prompt_budget = false;
            hu_log_warn_once(&warned_stream_prompt_budget, "agent_stream", NULL,
                             "system prompt truncated from %zu to %zu bytes "
                             "(MLX backend cap)",
                             system_prompt_len, cut);
            system_prompt[cut] = '\0';
            system_prompt_len = cut;
        }
        if (world_model_ctx) {
            agent->alloc->free(agent->alloc->ctx, world_model_ctx, world_model_ctx_len + 1);
            world_model_ctx = NULL;
        }
        if (enriched_contact) {
            agent->alloc->free(agent->alloc->ctx, enriched_contact, enriched_contact_len + 1);
            enriched_contact = NULL;
        }
        if (style_overlay) {
            agent->alloc->free(agent->alloc->ctx, style_overlay, style_overlay_len + 1);
            style_overlay = NULL;
        }
        if (contact_emotional_ctx) {
            agent->alloc->free(agent->alloc->ctx, contact_emotional_ctx,
                               contact_emotional_ctx_len + 1);
            contact_emotional_ctx = NULL;
        }
        if (persona_prompt)
            agent->alloc->free(agent->alloc->ctx, persona_prompt, persona_prompt_len + 1);
        if (memory_ctx)
            agent->alloc->free(agent->alloc->ctx, memory_ctx, memory_ctx_len + 1);
        if (graph_ctx)
            agent->alloc->free(agent->alloc->ctx, graph_ctx, graph_ctx_len + 1);
        if (stm_ctx) {
            agent->alloc->free(agent->alloc->ctx, stm_ctx, stm_ctx_len + 1);
            stm_ctx = NULL;
        }
        if (commitment_ctx) {
            agent->alloc->free(agent->alloc->ctx, commitment_ctx, commitment_ctx_len + 1);
            commitment_ctx = NULL;
        }
        if (pattern_ctx) {
            agent->alloc->free(agent->alloc->ctx, pattern_ctx, pattern_ctx_len + 1);
            pattern_ctx = NULL;
        }
        if (pref_ctx) {
            agent->alloc->free(agent->alloc->ctx, pref_ctx, pref_ctx_len + 1);
            pref_ctx = NULL;
        }
        if (emotional_ctx) {
            agent->alloc->free(agent->alloc->ctx, emotional_ctx, emotional_ctx_len + 1);
            emotional_ctx = NULL;
        }
        if (episodic_replay) {
            agent->alloc->free(agent->alloc->ctx, episodic_replay, episodic_replay_len + 1);
            episodic_replay = NULL;
        }
        if (conv_goals_ctx) {
            agent->alloc->free(agent->alloc->ctx, conv_goals_ctx, conv_goals_ctx_len + 1);
            conv_goals_ctx = NULL;
        }
        if (awareness_ctx)
            agent->alloc->free(agent->alloc->ctx, awareness_ctx, awareness_ctx_len + 1);
        if (outcome_ctx)
            agent->alloc->free(agent->alloc->ctx, outcome_ctx, outcome_ctx_len + 1);
        if (intelligence_ctx)
            agent->alloc->free(agent->alloc->ctx, intelligence_ctx, intelligence_ctx_len + 1);
        if (somatic_ctx)
            agent->alloc->free(agent->alloc->ctx, somatic_ctx, somatic_ctx_len + 1);
        if (trust_ctx)
            agent->alloc->free(agent->alloc->ctx, trust_ctx, trust_ctx_len + 1);
        had_humor_dir = (humor_dir && humor_dir_len > 0);
        if (humor_dir)
            agent->alloc->free(agent->alloc->ctx, humor_dir, humor_dir_len + 1);
        humor_dir = NULL;
        humor_dir_len = 0;
        if (syc_friction_ctx)
            agent->alloc->free(agent->alloc->ctx, syc_friction_ctx, syc_friction_ctx_len + 1);
        if (err != HU_OK) {
            hu_agent_clear_current_for_tools();
            return err;
        }
    }

    /* W11 live-provider wiring: when streaming self-RAG is enabled, append
     * the control-token directive to the system prompt so the model
     * actually knows the protocol exists. The parser in
     * hu_self_rag_stream_callback was previously dead-loaded — frontier
     * models never emit <retrieve>/<critique>/<refuse> without being
     * told they can. We do this once, outside the streaming tool loop,
     * so each tool-loop iteration sees the same (extended) system_prompt
     * and the all_msgs[0].content pointer stays valid for the duration
     * of the request. OOM here is non-fatal: the parser still runs;
     * the model just won't emit control tokens. */
    if (system_prompt && agent->w7_facade) {
        bool srag_stream_enabled_check =
            (agent->config && agent->config->agent.self_rag_streaming) ||
            getenv("HU_SELF_RAG_STREAMING");
        if (srag_stream_enabled_check) {
            (void)hu_self_rag_stream_directive_append(agent->alloc, &system_prompt,
                                                      &system_prompt_len);
        }
    }

    /* ── Observer: turn start (matches batch path in agent_turn.c) ─────── */
    hu_agent_internal_generate_trace_id(agent->trace_id);

    clock_t turn_start = clock();
    uint64_t turn_tokens = 0;
    const char *prov_name = agent->provider.vtable->get_name
                                ? agent->provider.vtable->get_name(agent->provider.ctx)
                                : NULL;

    {
        hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_AGENT_START, .data = {{0}}};
        ev.data.agent_start.provider = prov_name ? prov_name : "";
        ev.data.agent_start.model = agent->model_name ? agent->model_name : "";
        HU_OBS_SAFE_RECORD_EVENT(agent, &ev);
    }

    if (agent->infra.workflow_log) {
        hu_workflow_event_t wf_ev = {0};
        wf_ev.type = HU_WF_EVENT_STEP_STARTED;
        wf_ev.timestamp = hu_workflow_event_current_timestamp_ms();
        hu_workflow_event_log_append(agent->infra.workflow_log, agent->alloc, &wf_ev);
    }

    /* ── Streaming tool loop ─────────────────────────────────────────────── */
    char *final_content = NULL;
    size_t final_content_len = 0;
    bool srag_retrieval_seen = false;
    bool srag_critique_seen = false;
    bool srag_refuse_seen = false;

    for (int depth = 0; depth < STREAM_V2_MAX_TOOL_DEPTH; depth++) {
        if (agent->cancel_requested)
            break;

        /* Build messages from history */
        hu_chat_message_t *msgs = NULL;
        size_t msgs_count = 0;
        err = hu_context_format_messages(agent->alloc, agent->history, agent->history_count,
                                         agent->max_history_messages, NULL, &msgs, &msgs_count);
        if (err != HU_OK) {
            if (system_prompt)
                agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
            hu_agent_clear_current_for_tools();
            return err;
        }

        /* Prepend system prompt */
        size_t total_msgs = (msgs ? msgs_count : 0) + 1;
        hu_chat_message_t *all_msgs = (hu_chat_message_t *)agent->alloc->alloc(
            agent->alloc->ctx, total_msgs * sizeof(hu_chat_message_t));
        if (!all_msgs) {
            if (system_prompt)
                agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
            if (msgs)
                agent->alloc->free(agent->alloc->ctx, msgs, msgs_count * sizeof(hu_chat_message_t));
            hu_agent_clear_current_for_tools();
            return HU_ERR_OUT_OF_MEMORY;
        }
        memset(&all_msgs[0], 0, sizeof(hu_chat_message_t));
        all_msgs[0].role = HU_ROLE_SYSTEM;
        all_msgs[0].content = system_prompt;
        all_msgs[0].content_len = system_prompt_len;
        for (size_t i = 0; i < (msgs ? msgs_count : 0); i++)
            all_msgs[i + 1] = msgs[i];
        if (msgs)
            agent->alloc->free(agent->alloc->ctx, msgs, msgs_count * sizeof(hu_chat_message_t));

        hu_chat_request_t req;
        memset(&req, 0, sizeof(req));
        req.messages = all_msgs;
        req.messages_count = total_msgs;

        /* Model selection: reuse the early tier computation, apply model override */
        const char *turn_model = agent->model_name;
        size_t turn_model_len = agent->model_name_len;
        double turn_temp = agent->temperature;
        if (agent->turn_model && agent->turn_model_len > 0) {
            turn_model = agent->turn_model;
            turn_model_len = agent->turn_model_len;
        } else if (early_tier >= HU_TIER_ANALYTICAL) {
            hu_model_router_config_t mr_cfg = hu_model_router_default_config();
            const char *rel = NULL;
            size_t rel_len = 0;
            if (agent->relationship.stage >= HU_REL_TRUSTED) {
                rel = "trusted";
                rel_len = 7;
            } else if (agent->relationship.stage >= HU_REL_FAMILIAR) {
                rel = "friend";
                rel_len = 6;
            }
            hu_model_selection_t sel =
                hu_model_route(&mr_cfg, msg, msg_len, rel, rel_len, -1, agent->history_count);
            if (sel.tier >= HU_TIER_ANALYTICAL && sel.model && sel.model_len > 0) {
                turn_model = sel.model;
                turn_model_len = sel.model_len;
                if (sel.temperature > 0.0)
                    turn_temp = sel.temperature;
            }
        }
        if (agent->turn_temperature > 0.0)
            turn_temp = agent->turn_temperature;

        /* Somatic energy caps */
        if (agent->frontiers.initialized && agent->frontiers.somatic.energy < 0.3f) {
            req.max_tokens = 300;
            if (turn_temp > 0.7)
                turn_temp = 0.7;
        } else if (agent->frontiers.initialized && agent->frontiers.somatic.energy < 0.5f) {
            req.max_tokens = 600;
        }

        req.model = turn_model;
        req.model_len = turn_model_len;
        req.temperature = turn_temp;
        req.tools = (turn_needs_tools && agent->tool_specs_count > 0) ? agent->tool_specs : NULL;
        req.tools_count = turn_needs_tools ? agent->tool_specs_count : 0;
        /* G11 (2026-05-26): apply per-turn request overrides via the shared helper.
         * Centralized in src/agent/agent.c so the parity contract between
         * agent_turn.c (non-streaming) and agent_stream.c (streaming) is enforced —
         * drift like G5 (stream forgot to set turn_thinking_budget) is now
         * impossible without breaking tests/test_agent_turn_request_overrides.c. */
        hu_agent_internal_apply_turn_request_overrides(agent, &req);

        /* Realtime streaming hint (gemma-realtime Option B): casual tiers stream
         * incrementally for a live feel; analytical/deep stay buffered+cleaned so
         * bare-markdown deliberation never leaks. Inert unless on-device streaming
         * is operator-enabled and the provider is the OpenAI-compatible local
         * server, which is the only consumer of stream_strip on the wire. */
        req.stream_strip = hu_model_tier_stream_strip(early_tier);

        /* Activation steering (default off): map the active persona overlay's
         * traits to residual-stream steering coefficients for the local model.
         * Only the OpenAI-compatible (local) provider serializes req.steer_*;
         * cloud providers never read these fields, so this is inert off-device.
         * Validated traits only (formality, verbosity); hu_persona_steering_coeffs
         * and the server both clamp to the measured-safe [-1,1] envelope.
         *
         * Register-conditional (stack_eval_live.py, 2026-05-29): steering LIFTS
         * the substantive register (+0.008 atop RAG, +0.101 stacked) but HURTS
         * casual (-0.054 — added shaping fights curt brevity). Gate it to
         * ANALYTICAL/DEEP turns, like RAG; casual/reflexive + unknown tier skip. */
        if (agent->config && agent->config->agent.activation_steering_enabled && agent->persona &&
            agent->turn_tier >= (int)HU_TIER_ANALYTICAL) {
            const hu_persona_overlay_t *steer_ov = hu_persona_find_overlay(
                agent->persona, agent->active_channel, agent->active_channel_len);
            if (steer_ov) {
                double sf = 0.0, sv = 0.0;
                hu_persona_steering_coeffs(steer_ov->formality, steer_ov->avg_length, 1.0, &sf,
                                           &sv);
                if (sf != 0.0 || sv != 0.0) {
                    req.steering_present = true;
                    req.steer_formality = sf;
                    req.steer_verbosity = sv;
                }
            }
        }

        /* Buffer provider text until the final content clears guards. Tool
         * events still stream through stream_chunk_to_event_cb. */
        bool quality_buffered = (on_event != NULL);
        hu_emotional_weight_t v2_ew = hu_emotional_weight_classify(msg, msg_len);
        uint32_t v2_pacing = (uint32_t)hu_emotional_pacing_adjust(0, v2_ew);
        v2_stream_wrap_t wrap = {.on_event = on_event,
                                 .event_ctx = event_ctx,
                                 .initial_delay_ms = v2_pacing,
                                 .first_content_sent = false,
                                 .suppress_content = quality_buffered};

        /* W11 streaming self-RAG: wrap the provider callback to intercept
         * control tokens during generation. Guarded by env var so the
         * feature can be enabled incrementally. */
        hu_self_rag_stream_ctx_t srag_stream_ctx;
        bool srag_streaming_active = false;
        hu_stream_callback_t effective_cb = stream_chunk_to_event_cb;
        void *effective_ctx = &wrap;
        bool srag_stream_enabled = (agent->config && agent->config->agent.self_rag_streaming) ||
                                   getenv("HU_SELF_RAG_STREAMING");
        if (srag_stream_enabled && agent->w7_facade) {
            hu_memory_facade_t *srag_facade = hu_w7_facade_memory_handle(agent->w7_facade);
            if (hu_self_rag_stream_wrap(&srag_stream_ctx, stream_chunk_to_event_cb, &wrap,
                                        srag_facade, agent->alloc) == HU_OK) {
                effective_cb = hu_self_rag_stream_callback;
                effective_ctx = &srag_stream_ctx;
                srag_streaming_active = true;
            }
        }

        hu_stream_chat_result_t sresp;
        memset(&sresp, 0, sizeof(sresp));
        if (getenv("HU_DEBUG"))
            hu_log_info("agent_stream", NULL,
                        "stream_v2: calling stream_chat msgs=%zu tools=%zu sp_len=%zu", total_msgs,
                        req.tools_count, system_prompt_len);

        {
            hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_LLM_REQUEST, .data = {{0}}};
            ev.data.llm_request.provider = prov_name ? prov_name : "";
            ev.data.llm_request.model = agent->model_name ? agent->model_name : "";
            ev.data.llm_request.messages_count = total_msgs;
            HU_OBS_SAFE_RECORD_EVENT(agent, &ev);
        }

        clock_t llm_start = clock();
        err = agent->provider.vtable->stream_chat(agent->provider.ctx, agent->alloc, &req,
                                                  turn_model, turn_model_len, turn_temp,
                                                  effective_cb, effective_ctx, &sresp);
        uint64_t llm_duration_ms = hu_agent_internal_clock_diff_ms(llm_start, clock());

        /* Flush any remaining partial buffer from the self-RAG filter. */
        if (srag_streaming_active) {
            hu_self_rag_stream_flush(&srag_stream_ctx);
            if (srag_stream_ctx.retrieval_triggered)
                srag_retrieval_seen = true;
            if (srag_stream_ctx.critique_triggered)
                srag_critique_seen = true;
            if (srag_stream_ctx.refuse_triggered)
                srag_refuse_seen = true;
        }

        {
            hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_LLM_RESPONSE, .data = {{0}}};
            ev.data.llm_response.provider = prov_name ? prov_name : "";
            ev.data.llm_response.model = agent->model_name ? agent->model_name : "";
            ev.data.llm_response.duration_ms = llm_duration_ms;
            ev.data.llm_response.success = (err == HU_OK);
            ev.data.llm_response.error_message = (err != HU_OK) ? "stream_chat failed" : NULL;
            HU_OBS_SAFE_RECORD_EVENT(agent, &ev);
        }

        if (err != HU_OK) {
            agent->alloc->free(agent->alloc->ctx, all_msgs, total_msgs * sizeof(hu_chat_message_t));
            if (getenv("HU_DEBUG"))
                hu_log_error("agent_stream", NULL, "stream_v2: stream_chat FAILED: %s",
                             hu_error_string(err));
            hu_stream_chat_result_free(agent->alloc, &sresp);
            if (system_prompt)
                agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
            hu_agent_clear_current_for_tools();
            return err;
        }

        /* Spec 2026-05-19 M3 closure / AC-M3-2 / D-M3-2 — per-turn
         * contact routing on the streaming path. We fire route_per_turn
         * AFTER the first successful stream_chat returns (i.e., once we
         * know the model is serving healthily) rather than before the
         * stream starts. This trades one turn of personalization on
         * adapter rotation against ~50-200ms of synchronous swap latency
         * on every healthy turn — the contact-route table already
         * records intended adapter so missing one turn is recoverable.
         *
         * Pinned by tests/test_m3_route_per_turn_call_sites.c which
         * grep-checks that both agent_turn.c AND this file invoke
         * hu_agent_m3_route_per_turn(). Mirrors agent_turn.c:4224. */
        hu_agent_m3_route_per_turn(agent);

        /* Spec 2026-05-19 self-model-scaffold Phase B: stash per-turn
         * metrics before the canonical write site. tool_count + the rest
         * left zero — not computed at this stream-chat site. */
        hu_agent_m3_stash_behavior_metrics(
            agent, &(hu_agent_behavior_stash_t){
                       .response_length_chars = (uint32_t)sresp.content_len,
                       .response_length_tokens_est = (uint32_t)(sresp.content_len / 4),
                       .response_latency_ms = (uint32_t)llm_duration_ms,
                   });
        hu_agent_m3_on_provider_success(agent);
        /* B1 redefined (2026-05-17 r3): record outcome at the top of each
         * tool-loop iteration. sresp.content may be empty when there are
         * tool_calls; the helper still records the prompt hash + latency
         * + turn_kind so the training loop sees "this turn happened."
         * Downstream sites (GVR / constitutional / retry) record again
         * with the cleaned response. */
        hu_agent_m3_record_chat_outcome(agent, msg, msg_len, sresp.content, sresp.content_len,
                                        llm_duration_ms, agent->memory_session_id,
                                        agent->memory_session_id_len, HU_M3_GUARD_PASS,
                                        /*turn_kind=stream=*/1, &sresp.usage);

        agent->total_tokens += sresp.usage.total_tokens;
        hu_agent_internal_record_cost(agent, &sresp.usage);
        turn_tokens += sresp.usage.total_tokens;

        /* No tool calls → this is the final response */
        if (sresp.tool_calls_count == 0) {
            char *safe_content = NULL;
            size_t safe_content_len = 0;
            bool safe_owned = false;
            if (sresp.content && sresp.content_len > 0) {
                char *guard_out = NULL;
                size_t guard_out_len = 0;
                hu_guard_outcome_t guard_outcome = HU_GUARD_OK;
                hu_guard_report_t guard_report;
                memset(&guard_report, 0, sizeof(guard_report));
                hu_guard_context_t guard_ctx;
                memset(&guard_ctx, 0, sizeof(guard_ctx));
                guard_ctx.recent_avg_len = hu_agent_internal_recent_assistant_avg_len(agent, 5);
                guard_ctx.length_anomaly_mult = hu_guard_length_anomaly_mult_for_channel(
                    agent->active_channel, agent->active_channel_len);
                guard_ctx.director_text = agent->scene_direction_text;
                guard_ctx.director_len = agent->scene_direction_text_len;
                /* Sprint 37 — cross-turn director history. */
                guard_ctx.director_history = (const char *const *)agent->director_history;
                guard_ctx.director_history_lens = agent->director_history_lens;
                guard_ctx.director_history_count = agent->director_history_count;
                /* Sprint 41 follow-up #4 — consult per-channel G9 disable list. */
                guard_ctx.naked_opener_disabled = hu_response_guard_g9_disabled_for_channel(
                    agent->active_channel, agent->active_channel_len);
                if (agent->persona) {
                    if (agent->persona->name && agent->persona->name_len > 1) {
                        guard_ctx.persona_name = agent->persona->name;
                        guard_ctx.persona_name_len = agent->persona->name_len;
                    }
                    /* Prefer `identity` (full biographical string); fall
                     * back to `core_anchor` (one-line bio). */
                    const char *id = agent->persona->identity ? agent->persona->identity
                                                              : agent->persona->core_anchor;
                    if (id) {
                        guard_ctx.persona_identity = id;
                        guard_ctx.persona_identity_len = strlen(id);
                    }
                    if (agent->persona->biography) {
                        guard_ctx.persona_biography = agent->persona->biography;
                        guard_ctx.persona_biography_len = strlen(agent->persona->biography);
                    }
                }
                hu_error_t guard_err = hu_response_guard_check_ex(
                    agent->alloc, sresp.content, sresp.content_len, &guard_ctx, &guard_out,
                    &guard_out_len, &guard_outcome, &guard_report);
                if (guard_err == HU_OK && guard_outcome == HU_GUARD_REJECT) {
                    hu_log_error("agent_stream", agent->observer,
                                 "response_guard REJECT: stream final (len=%zu, recent_avg=%zu) "
                                 "[semantic=%d length=%d director=%d persona=%d identity=%d "
                                 "naked_opener=%d repetition_run=%zu] — retrying once with "
                                 "repair prompt",
                                 sresp.content_len, guard_ctx.recent_avg_len,
                                 guard_report.detected_semantic_leak ? 1 : 0,
                                 guard_report.detected_length_anomaly ? 1 : 0,
                                 guard_report.detected_director_echo ? 1 : 0,
                                 guard_report.detected_persona_pii_echo ? 1 : 0,
                                 guard_report.detected_persona_identity_echo ? 1 : 0,
                                 guard_report.detected_naked_discourse_opener ? 1 : 0,
                                 guard_report.max_repetition_run);
                    /* B2 (2026-05-31): update last_rejected_draft so production tapback
                     * pairing uses the guard-rejected text for complete DPO pairs. */
                    if (agent->sota.sota_initialized && sresp.content && sresp.content_len > 0) {
                        if (agent->sota.last_rejected_draft)
                            agent->alloc->free(agent->alloc->ctx,
                                               agent->sota.last_rejected_draft,
                                               agent->sota.last_rejected_draft_len + 1);
                        agent->sota.last_rejected_draft =
                            hu_strndup(agent->alloc, sresp.content, sresp.content_len);
                        agent->sota.last_rejected_draft_len = sresp.content_len;
                    }
                    /* Sprint 41 follow-up — capture this rejection as a DPO
                     * negative pair for the next LoRA training pass. No-op
                     * under HU_IS_TEST. Complements the E1 pair-builder
                     * below (E1 writes chosen+rejected pairs only when the
                     * retry succeeds; this captures EVERY rejection). */
                    {
                        const char *dpo_det = "unknown";
                        if (guard_report.detected_naked_discourse_opener)
                            dpo_det = "naked_discourse_opener";
                        else if (guard_report.detected_persona_identity_echo)
                            dpo_det = "persona_identity_echo";
                        else if (guard_report.detected_persona_pii_echo)
                            dpo_det = "persona_pii_echo";
                        else if (guard_report.detected_director_echo)
                            dpo_det = "director_echo";
                        else if (guard_report.detected_length_anomaly)
                            dpo_det = "length_anomaly";
                        else if (guard_report.detected_semantic_leak)
                            dpo_det = "semantic_leak";
                        else if (guard_report.detected_degenerate_repetition)
                            dpo_det = "degenerate_repetition";
                        (void)hu_response_guard_log_dpo_negative(
                            msg, msg_len, sresp.content, sresp.content_len, dpo_det,
                            agent->active_channel, (int64_t)time(NULL));
                    }
                    hu_guard_report_t retry_report;
                    memset(&retry_report, 0, sizeof(retry_report));
                    /* 2026-05-17: pass persona core_anchor into the slim retry so
                     * the repair prompt carries identity context. Without this the
                     * stripped repair instruction lets the base RLHF assert AI
                     * identity ("I am a large language model trained by Google"). */
                    const char *anchor = NULL;
                    size_t anchor_len = 0;
                    if (agent->persona && agent->persona->core_anchor &&
                        agent->persona->core_anchor[0]) {
                        anchor = agent->persona->core_anchor;
                        anchor_len = strlen(anchor);
                    }
                    uint64_t recovered_retry_t0_ms = hu_agent_internal_monotonic_ms();
                    hu_error_t retry_err = hu_response_guard_retry_slim_with_identity(
                        agent->alloc, agent->observer, agent->config, &agent->provider, turn_model,
                        turn_model_len, msg, msg_len, anchor, anchor_len, &safe_content,
                        &safe_content_len, &retry_report);
                    uint64_t recovered_retry_latency_ms =
                        hu_agent_internal_monotonic_ms() - recovered_retry_t0_ms;
                    /* Sprint 41 follow-up #3 — G9 retry-outcome telemetry. */
                    if (guard_report.detected_naked_discourse_opener) {
                        bool retry_ok =
                            (retry_err == HU_OK && safe_content && safe_content_len > 0);
                        bool retry_tripped_g9 = retry_ok && hu_response_is_naked_discourse_opener(
                                                                safe_content, safe_content_len);
                        hu_response_guard_record_g9_retry_outcome(retry_ok, retry_tripped_g9);
                    }
                    if (retry_err == HU_OK && safe_content && safe_content_len > 0) {
                        /* Post-retry persona_voice check: catch AI-disclosure that
                         * leaked through the repair prompt anyway. response_guard
                         * only checks for tokens/length/repetition — it does not
                         * detect "I am an AI"-class phrases. Without this gate, the
                         * twin-break shipped to the user. */
                        if (!hu_persona_voice_response_is_clean(safe_content, safe_content_len)) {
                            /* 2026-05-24 companion fix to agent_turn.c:6395 batch-path
                             * fallback: install a canonical short safe reply so the
                             * function's contract holds (safe_content non-NULL when
                             * downstream code reaches line ~1612). The streaming bytes
                             * have already been sent to the client by this point, so
                             * the user already saw the rejected text — but the AGENT'S
                             * HISTORY would otherwise miss this turn entirely,
                             * conditioning subsequent responses on a phantom turn.
                             * Installing the fallback keeps history honest about
                             * "an assistant turn happened, and this is what we'd say
                             * if we could rewind." Wording differs slightly from
                             * agent_turn's "hold on, let me think on that" so logs
                             * can distinguish which path fired. */
                            hu_log_error("agent_stream", agent->observer,
                                         "persona_voice REJECT: stream retry produced "
                                         "AI-disclosure (len=%zu) — installing safe fallback",
                                         safe_content_len);
                            agent->alloc->free(agent->alloc->ctx, safe_content,
                                               safe_content_len + 1);
                            static const char fallback[] = "let me think on that, sorry";
                            size_t fallback_len = sizeof(fallback) - 1;
                            safe_content = hu_strndup(agent->alloc, fallback, fallback_len);
                            if (safe_content) {
                                safe_content_len = fallback_len;
                                safe_owned = true;
                            } else {
                                /* OOM — last resort: NULL, downstream skip. */
                                safe_content_len = 0;
                                safe_owned = false;
                            }
                        } else {
                            /* Spec 2026-05-19 self-model-scaffold Phase B:
                             * stash post-retry length + latency. Other metric
                             * fields not computed here. */
                            hu_agent_m3_stash_behavior_metrics(
                                agent,
                                &(hu_agent_behavior_stash_t){
                                    .response_length_chars = (uint32_t)safe_content_len,
                                    .response_length_tokens_est = (uint32_t)(safe_content_len / 4),
                                    .response_latency_ms = (uint32_t)recovered_retry_latency_ms,
                                });
                            hu_agent_m3_on_provider_success(agent);
                            /* B1 redefined (2026-05-17 r3): also record a structured
                             * outcome so the future training loop has signal beyond
                             * just "the hook fired". Latency is the slim-retry's
                             * monotonic elapsed time (r3 threaded it through);
                             * contact_id is the agent's session id when set. */
                            /* Slim retry path — no chat_response in scope.
                             * NULL usage → bytes/4 fallback fires. */
                            hu_agent_m3_record_chat_outcome(
                                agent, msg, msg_len, safe_content, safe_content_len,
                                recovered_retry_latency_ms, agent->memory_session_id,
                                agent->memory_session_id_len, HU_M3_GUARD_REWRITE,
                                /*turn_kind=stream=*/1, NULL);
                            /* D7 (2026-05-18): perfect DPO preference pair —
                             * (sresp.content = rejected, safe_content = accepted).
                             * Captured into ~/.human/training-data/
                             * m3-rewrite-pairs.jsonl for the DPO trainer to
                             * consume. Best-effort; failure here MUST NOT
                             * break the chat path. */
                            (void)hu_m3_rewrite_pair_record(agent->alloc, NULL, msg, msg_len,
                                                            sresp.content, sresp.content_len,
                                                            safe_content, safe_content_len,
                                                            /*turn_kind=stream=*/1);
                            safe_owned = true;
                            hu_log_warn("agent_stream", agent->observer,
                                        "response_guard RECOVERED: stream retry passed (len=%zu, "
                                        "stripped=%zu)",
                                        safe_content_len, retry_report.bytes_stripped);
                        }
                    } else {
                        /* 2026-05-24 companion fix: retry call itself failed (transport,
                         * OOM, etc.) — install the same canonical fallback as the
                         * persona_voice REJECT branch above so safe_content is non-NULL
                         * and downstream history/final_content gets recorded. The
                         * client already saw the streamed-then-rejected response;
                         * this just keeps the agent's record honest. */
                        hu_log_error("agent_stream", agent->observer,
                                     "response_guard stream retry failed (err=%s) — installing "
                                     "safe fallback",
                                     hu_error_string(retry_err));
                        static const char fallback[] = "let me think on that, sorry";
                        size_t fallback_len = sizeof(fallback) - 1;
                        safe_content = hu_strndup(agent->alloc, fallback, fallback_len);
                        if (safe_content) {
                            safe_content_len = fallback_len;
                            safe_owned = true;
                        } else {
                            safe_content_len = 0;
                            safe_owned = false;
                        }
                    }
                } else if (guard_err == HU_OK && guard_outcome == HU_GUARD_REWROTE) {
                    safe_content = guard_out;
                    safe_content_len = guard_out_len;
                    safe_owned = true;
                    hu_log_warn("agent_stream", agent->observer,
                                "response_guard REWROTE: stripped %zu stream bytes",
                                guard_report.bytes_stripped);
                } else if (guard_err == HU_OK) {
                    safe_content = (char *)sresp.content;
                    safe_content_len = sresp.content_len;
                }
            }

            if (safe_content && safe_content_len > 0) {
                /* Strip AI phrases and formal structure before appending to
                 * history so that the model's memory matches what the user
                 * actually sees (the daemon applies the same strips after
                 * the agent returns, but by then history is already set). */
                if (!safe_owned) {
                    char *copy = hu_strndup(agent->alloc, safe_content, safe_content_len);
                    if (copy) {
                        safe_content = copy;
                        safe_owned = true;
                    }
                }
                if (safe_owned) {
                    safe_content_len =
                        hu_conversation_strip_channel_tags(safe_content, safe_content_len);
                    safe_content_len =
                        hu_conversation_strip_ai_phrases(safe_content, safe_content_len);
                    safe_content_len =
                        hu_conversation_strip_formal_structure(safe_content, safe_content_len);
                }
                hu_error_t hist_err = hu_agent_internal_append_history(
                    agent, HU_ROLE_ASSISTANT, safe_content, safe_content_len, NULL, 0, NULL, 0);
                if (hist_err != HU_OK)
                    hu_log_error("agent_stream_v2", NULL, "append_history failed: %s",
                                 hu_error_string(hist_err));
                if (safe_owned) {
                    final_content = safe_content;
                    final_content_len = safe_content_len;
                    safe_owned = false;
                } else {
                    final_content = hu_strndup(agent->alloc, safe_content, safe_content_len);
                    final_content_len = final_content ? safe_content_len : 0;
                }
            }
            if (safe_owned && safe_content)
                agent->alloc->free(agent->alloc->ctx, safe_content, safe_content_len + 1);
            agent->alloc->free(agent->alloc->ctx, all_msgs, total_msgs * sizeof(hu_chat_message_t));
            hu_stream_chat_result_free(agent->alloc, &sresp);
            break;
        }

        /* Tool calls present — append assistant message with tool calls to history,
         * execute each tool, append results, and continue the loop. */
        err = hu_agent_internal_append_history_with_tool_calls(
            agent, sresp.content ? sresp.content : "", sresp.content_len, sresp.tool_calls,
            sresp.tool_calls_count);
        if (err != HU_OK) {
            agent->alloc->free(agent->alloc->ctx, all_msgs, total_msgs * sizeof(hu_chat_message_t));
            hu_log_error("agent_stream_v2", NULL, "append_history_with_tool_calls failed: %s",
                         hu_error_string(err));
            hu_stream_chat_result_free(agent->alloc, &sresp);
            if (system_prompt)
                agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
            hu_agent_clear_current_for_tools();
            return err;
        }
        agent->alloc->free(agent->alloc->ctx, all_msgs, total_msgs * sizeof(hu_chat_message_t));

        /* Execute each tool call and emit TOOL_RESULT events */
        for (size_t tc = 0; tc < sresp.tool_calls_count; tc++) {
            if (agent->cancel_requested)
                break;
            const hu_tool_call_t *call = &sresp.tool_calls[tc];
            hu_tool_t *tool = hu_agent_internal_find_tool(agent, call->name, call->name_len);
            hu_tool_result_t result;
            memset(&result, 0, sizeof(result));

            char tn_buf[64];
            size_t tn = (call->name_len < sizeof(tn_buf) - 1) ? call->name_len : sizeof(tn_buf) - 1;
            if (tn > 0 && call->name)
                memcpy(tn_buf, call->name, tn);
            tn_buf[tn] = '\0';
            const char *args_str = call->arguments ? call->arguments : "";

            /* Observer: TOOL_CALL_START */
            {
                hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_TOOL_CALL_START, .data = {{0}}};
                ev.data.tool_call_start.tool = tn_buf[0] ? tn_buf : "unknown";
                HU_OBS_SAFE_RECORD_EVENT(agent, &ev);
            }

            /* Pre-hook pipeline (centralized via hu_agent_internal_pre_hook_check):
             * returns false on DENY with *result already populated. */
            if (!hu_agent_internal_pre_hook_check(agent, tn_buf, tn, args_str, strlen(args_str),
                                                  &result)) {
                goto stream_tool_done;
            }

            if (!tool) {
                result = hu_tool_result_fail("tool not found", 14);
            } else {
                hu_json_value_t *args = NULL;
                if (call->arguments_len > 0) {
                    hu_error_t pe =
                        hu_json_parse(agent->alloc, call->arguments, call->arguments_len, &args);
                    if (pe != HU_OK)
                        args = NULL;
                }
                if (!args) {
                    hu_json_parse(agent->alloc, "{}", 2, &args);
                }
                if (args) {
                    result = hu_tool_result_fail("invalid arguments", 16);
                    if (tool->vtable->execute_streaming && on_event) {
                        tool_stream_bridge_t bridge = {
                            .on_event = on_event,
                            .event_ctx = event_ctx,
                            .tool_name = call->name,
                            .tool_name_len = call->name_len,
                            .tool_call_id = call->id,
                            .tool_call_id_len = call->id_len,
                        };
                        hu_agent_turn_state_track_tool(agent, call->name, call->name_len);
                        tool->vtable->execute_streaming(tool->ctx, agent->alloc, args,
                                                        tool_chunk_to_event, &bridge, &result);
                    } else if (tool->vtable->execute) {
                        hu_agent_turn_state_track_tool(agent, call->name, call->name_len);
                        tool->vtable->execute(tool->ctx, agent->alloc, args, &result);
                    }
                    hu_json_free(agent->alloc, args);
                } else {
                    result = hu_tool_result_fail("invalid arguments", 16);
                }
            }

        stream_tool_done:
            /* Observer: TOOL_CALL (post-execution) */
            {
                hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_TOOL_CALL, .data = {{0}}};
                ev.data.tool_call.tool = tn_buf[0] ? tn_buf : "unknown";
                ev.data.tool_call.duration_ms = 0;
                ev.data.tool_call.success = result.success;
                ev.data.tool_call.detail =
                    result.success ? NULL : (result.error_msg ? result.error_msg : "failed");
                HU_OBS_SAFE_RECORD_EVENT(agent, &ev);
            }

            /* Outcome tracking */
            if (agent->outcomes) {
                const char *sum = result.success ? (result.output ? result.output : "ok")
                                                 : (result.error_msg ? result.error_msg : "failed");
                hu_outcome_record_tool(agent->outcomes, tn_buf, result.success, sum);
            }

#ifdef HU_ENABLE_SQLITE
            /* Online learning + self-improve: record tool outcome signal */
            if (agent->memory && !agent->proactive_turn) {
                sqlite3 *ol_db = hu_sqlite_memory_get_db(agent->memory);
                if (ol_db) {
                    hu_online_learning_t ol;
                    if (hu_online_learning_create(agent->alloc, ol_db, 0.1, &ol) == HU_OK) {
                        hu_learning_signal_t sig = {
                            .type =
                                result.success ? HU_SIGNAL_TOOL_SUCCESS : HU_SIGNAL_TOOL_FAILURE,
                            .tool_name = {0},
                            .tool_name_len =
                                tn < sizeof(sig.tool_name) ? tn : sizeof(sig.tool_name) - 1,
                            .magnitude = 1.0,
                            .timestamp = (int64_t)time(NULL),
                        };
                        if (tn > 0)
                            memcpy(sig.tool_name, tn_buf, sig.tool_name_len);
                        hu_online_learning_record(&ol, &sig);

                        hu_self_improve_t si;
                        if (hu_self_improve_create(agent->alloc, ol_db, &si) == HU_OK) {
                            hu_self_improve_record_tool_outcome(&si, tn_buf, tn, result.success,
                                                                sig.timestamp);
                            hu_self_improve_deinit(&si);
                        }
                        hu_online_learning_deinit(&ol);
                    }
                }

                /* Experience record: per-tool */
                {
                    hu_experience_store_t tool_exp;
                    if (hu_experience_store_init(agent->alloc, agent->memory, &tool_exp) == HU_OK) {
                        tool_exp.db = ol_db;
                        const char *out_text = result.success ? result.output : result.error_msg;
                        size_t out_len = result.success ? result.output_len : result.error_msg_len;
                        const char *act_text = call->arguments ? call->arguments : "";
                        size_t act_len = call->arguments_len ? call->arguments_len : 0;
                        double exp_score = result.success ? 0.9 : 0.2;
                        (void)hu_experience_record(&tool_exp, tn_buf, tn, act_text, act_len,
                                                   out_text ? out_text : "", out_text ? out_len : 0,
                                                   exp_score);
                        hu_experience_store_deinit(&tool_exp);
                    }
                }
            }
#endif

            /* Post-hook pipeline (centralized via hu_agent_internal_post_hook_fire). */
            hu_agent_internal_post_hook_fire(agent, tn_buf, tn, args_str, strlen(args_str),
                                             &result);

            /* Build result text for history */
            const char *result_text = result.success ? result.output : result.error_msg;
            size_t result_text_len = result.success ? result.output_len : result.error_msg_len;
            if (!result_text) {
                result_text = "";
                result_text_len = 0;
            }

            hu_error_t hist_err = hu_agent_internal_append_history(
                agent, HU_ROLE_TOOL, result_text, result_text_len, call->name, call->name_len,
                call->id, call->id_len);
            if (hist_err != HU_OK)
                hu_log_error("agent_stream_v2", NULL, "append tool result failed: %s",
                             hu_error_string(hist_err));

            /* Emit TOOL_RESULT event to the callback */
            if (on_event) {
                hu_agent_stream_event_t tev;
                memset(&tev, 0, sizeof(tev));
                tev.type = HU_AGENT_STREAM_TOOL_RESULT;
                tev.data = result_text;
                tev.data_len = result_text_len;
                tev.tool_name = call->name;
                tev.tool_name_len = call->name_len;
                tev.tool_call_id = call->id;
                tev.tool_call_id_len = call->id_len;
                tev.is_error = !result.success;
                on_event(&tev, event_ctx);
            }

            hu_tool_result_free(agent->alloc, &result);
        }

        hu_stream_chat_result_free(agent->alloc, &sresp);
        /* Loop back: next iteration will re-format messages including tool results */
    }

    if (system_prompt)
        agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);

    /* W11 streaming self-RAG: act on accumulated flags. */
    if (srag_refuse_seen) {
        hu_log_info("agent_stream", NULL,
                    "self-RAG streaming: <refuse> detected, replacing response");
        agent->self_rag_abstentions++;
        if (final_content) {
            agent->alloc->free(agent->alloc->ctx, final_content, final_content_len + 1);
            final_content = NULL;
            final_content_len = 0;
        }
        char refusal[256];
        hu_self_rag_render_refusal(HU_REFUSAL_POLICY, refusal, sizeof(refusal));
        final_content = hu_strndup(agent->alloc, refusal, strlen(refusal));
        if (final_content) {
            final_content_len = strlen(refusal);
            /* The user-visible response WAS swapped to the refusal template
             * — keep `self_rag_refusals_rendered` in sync with the
             * non-streaming path (agent_turn.c uses
             * `hu_agent_self_rag_apply`). Without this bump the W11 P1
             * metric ("≥30 % abstention rate on weak-evidence prompts")
             * silently undercounts streaming refusals. */
            agent->self_rag_refusals_rendered++;
        }
    } else {
        if (srag_retrieval_seen) {
            hu_log_info("agent_stream", NULL, "self-RAG streaming: <retrieve> detected");
            agent->self_rag_runs++;
        }
        if (srag_critique_seen) {
            hu_log_info("agent_stream", NULL, "self-RAG streaming: <critique> detected");
            agent->self_rag_claims_flagged++;
        }
    }

    /* ── Quality pipeline: GVR → Constitutional AI → Metacognition ──────
     * These three systems were only in agent_turn.c (non-streaming path).
     * Without them, the streaming CLI sends raw first-draft responses. */
#ifndef HU_IS_TEST
    if (final_content && final_content_len > 0) {
        bool content_owned = true; /* final_content is always allocator-owned here */

        /* 1. GVR: verify → revise loop (up to 2 revisions).
         * Skip when persona is active — GVR's generic verifier rejects
         * persona-style responses (casual, terse) and rewrites them into
         * bland AI-speak, which is worse. */
        if (agent->sota.gvr_config.enabled && !agent->persona) {
            hu_gvr_pipeline_result_t gvr_result;
            memset(&gvr_result, 0, sizeof(gvr_result));
            uint64_t gvr_t0_ms = hu_agent_internal_monotonic_ms();
            hu_error_t gvr_err = hu_gvr_pipeline(
                agent->alloc, &agent->provider, &agent->sota.gvr_config, agent->model_name,
                agent->model_name_len, msg, msg_len, final_content, final_content_len, &gvr_result);
            uint64_t gvr_latency_ms = hu_agent_internal_monotonic_ms() - gvr_t0_ms;
            if (gvr_err == HU_OK) {
                /* Spec 2026-05-19 self-model-scaffold Phase B:
                 * stash pre-revise length + GVR latency. The actual gvr_resp_len
                 * is computed below; final_content_len is the closest approximation
                 * in scope at this point. */
                hu_agent_m3_stash_behavior_metrics(
                    agent, &(hu_agent_behavior_stash_t){
                               .response_length_chars = (uint32_t)final_content_len,
                               .response_length_tokens_est = (uint32_t)(final_content_len / 4),
                               .response_latency_ms = (uint32_t)gvr_latency_ms,
                           });
                hu_agent_m3_on_provider_success(agent);
                /* B1 redefined (2026-05-17 r3): GVR is not response_guard
                 * — even when it revises, the decision tag is PASS so the
                 * training loop can distinguish quality-pipeline rewrites
                 * from guard-driven repairs. */
                const char *gvr_resp =
                    gvr_result.final_content ? gvr_result.final_content : final_content;
                size_t gvr_resp_len =
                    gvr_result.final_content ? gvr_result.final_content_len : final_content_len;
                /* GVR runs after the original sresp has been cleaned up in
                 * the stream path — no chat_response struct in scope here.
                 * NULL usage → bytes/4 fallback. */
                hu_agent_m3_record_chat_outcome(agent, msg, msg_len, gvr_resp, gvr_resp_len,
                                                gvr_latency_ms, agent->memory_session_id,
                                                agent->memory_session_id_len, HU_M3_GUARD_PASS,
                                                /*turn_kind=stream=*/1, NULL);
            }
            if (gvr_err == HU_OK && gvr_result.final_content &&
                gvr_result.revisions_performed > 0) {
                if (content_owned)
                    agent->alloc->free(agent->alloc->ctx, (void *)final_content,
                                       final_content_len + 1);
                final_content = gvr_result.final_content;
                final_content_len = gvr_result.final_content_len;
                content_owned = true;
                gvr_result.final_content = NULL;
            }
            hu_gvr_pipeline_result_free(agent->alloc, &gvr_result);
        }

        /* 1b. Persona quality rethink: re-prompt for more substance.
         * Triggers when response is short (<80 chars) for a substantive question,
         * but ONLY if the first draft lacks engagement (no question mark).
         * Short + in-persona + has follow-up question = good, skip rethink.
         * Short + formal/no-question = needs help, do rethink. */
        bool needs_rethink = false;
        if (agent->persona && !agent->lean_prompt && final_content_len > 0 &&
            final_content_len < 100 && msg_len > 15 && agent->provider.vtable &&
            agent->provider.vtable->chat_with_system) {
            bool has_question = (memchr(final_content, '?', final_content_len) != NULL);
            bool starts_lowercase = (final_content[0] >= 'a' && final_content[0] <= 'z');
            /* Has follow-up question = engaged, skip rethink regardless of length */
            if (has_question)
                needs_rethink = false;
            /* No question + under 70 chars = needs more substance */
            else if (final_content_len < 70)
                needs_rethink = true;
            /* 70-100 chars, no question, uppercase = definitely rethink */
            else if (!starts_lowercase)
                needs_rethink = true;
            /* 70-100 chars, lowercase, no question = borderline, skip */
            else
                needs_rethink = false;
        }
        if (needs_rethink) {
            /* Build rethink prompt with persona context for style fidelity */
            const char *persona_name = agent->persona ? agent->persona->name : "the persona";
            const char *persona_identity =
                (agent->persona && agent->persona->identity) ? agent->persona->identity : "";
            char rethink_sys[2048];
            snprintf(rethink_sys, sizeof(rethink_sys),
                     "You are %s. %.*s\n\n"
                     "Your draft response was too brief. Rewrite it to be more engaging, "
                     "natural, and conversational while staying fully in character. "
                     "Keep your style (casual, lowercase, slang) but add more substance — "
                     "share a personal thought, ask a follow-up, show personality. "
                     "Do NOT be generic, formal, or robotic. Write like a real person texting.",
                     persona_name,
                     (int)(strlen(persona_identity) < 500 ? strlen(persona_identity) : 500),
                     persona_identity);
            char rethink_user[4096];
            int rn = snprintf(
                rethink_user, sizeof(rethink_user),
                "User said: \"%.*s\"\n\nYour draft response: \"%.*s\"\n\n"
                "Rewrite this as %s would actually text it — in character, with personality:",
                (int)(msg_len < 500 ? msg_len : 500), msg, (int)final_content_len, final_content,
                persona_name);
            if (rn > 0 && (size_t)rn < sizeof(rethink_user)) {
                char *revised = NULL;
                size_t revised_len = 0;
                uint64_t rethink_t0_ms = hu_agent_internal_monotonic_ms();
                hu_error_t re_err = agent->provider.vtable->chat_with_system(
                    agent->provider.ctx, agent->alloc, rethink_sys, sizeof(rethink_sys) - 1,
                    rethink_user, (size_t)rn, agent->model_name, agent->model_name_len, 0.9,
                    &revised, &revised_len);
                uint64_t rethink_latency_ms = hu_agent_internal_monotonic_ms() - rethink_t0_ms;
                if (re_err == HU_OK) {
                    /* Spec 2026-05-19 self-model-scaffold Phase B:
                     * stash final-content length + rethink latency. */
                    hu_agent_m3_stash_behavior_metrics(
                        agent, &(hu_agent_behavior_stash_t){
                                   .response_length_chars = (uint32_t)final_content_len,
                                   .response_length_tokens_est = (uint32_t)(final_content_len / 4),
                                   .response_latency_ms = (uint32_t)rethink_latency_ms,
                               });
                    hu_agent_m3_on_provider_success(agent);
                    /* B1 redefined (2026-05-17 r3): persona rethink is a
                     * quality-pipeline rewrite, not response_guard — tag
                     * PASS. Use revised when present, else fall back to
                     * current final_content. */
                    const char *rt_resp = (revised && revised_len > 0) ? revised : final_content;
                    size_t rt_resp_len =
                        (revised && revised_len > 0) ? revised_len : final_content_len;
                    /* Persona rethink — separate provider call, but the
                     * intermediate hu_chat_response is freed before this
                     * point. NULL usage → bytes/4 fallback. */
                    hu_agent_m3_record_chat_outcome(agent, msg, msg_len, rt_resp, rt_resp_len,
                                                    rethink_latency_ms, agent->memory_session_id,
                                                    agent->memory_session_id_len, HU_M3_GUARD_PASS,
                                                    /*turn_kind=stream=*/1, NULL);
                }
                if (re_err == HU_OK && revised && revised_len > final_content_len) {
                    hu_log_info("human", NULL, "[quality] persona rethink: %zu → %zu chars",
                                final_content_len, revised_len);
                    if (content_owned)
                        agent->alloc->free(agent->alloc->ctx, (void *)final_content,
                                           final_content_len + 1);
                    final_content = revised;
                    final_content_len = revised_len;
                    content_owned = true;
                } else if (revised) {
                    agent->alloc->free(agent->alloc->ctx, revised, revised_len + 1);
                }
            }
        }

        /* 2. Constitutional AI: critique against principles, rewrite if needed.
         * Skip in lean_prompt mode — extra LLM round-trip is too slow for texting. */
        if (agent->constitutional_enabled && !agent->lean_prompt) {
            hu_constitutional_config_t const_cfg = hu_constitutional_config_default();
            hu_critique_result_t critique;
            memset(&critique, 0, sizeof(critique));
            uint64_t const_t0_ms = hu_agent_internal_monotonic_ms();
            if (hu_constitutional_critique(agent->alloc, &agent->provider, agent->model_name,
                                           agent->model_name_len, msg, msg_len, final_content,
                                           final_content_len, &const_cfg, &critique) == HU_OK) {
                uint64_t const_latency_ms = hu_agent_internal_monotonic_ms() - const_t0_ms;
                /* Spec 2026-05-19 self-model-scaffold Phase B:
                 * stash final-content length + constitutional-critique latency. */
                hu_agent_m3_stash_behavior_metrics(
                    agent, &(hu_agent_behavior_stash_t){
                               .response_length_chars = (uint32_t)final_content_len,
                               .response_length_tokens_est = (uint32_t)(final_content_len / 4),
                               .response_latency_ms = (uint32_t)const_latency_ms,
                           });
                hu_agent_m3_on_provider_success(agent);
                /* B1 redefined (2026-05-17 r3): Constitutional AI is a
                 * quality-pipeline critique, not response_guard — tag
                 * PASS even when it rewrites. */
                const char *cn_resp =
                    (critique.verdict == HU_CRITIQUE_REWRITE && critique.revised_response &&
                     critique.revised_response_len > 0)
                        ? critique.revised_response
                        : final_content;
                size_t cn_resp_len =
                    (critique.verdict == HU_CRITIQUE_REWRITE && critique.revised_response &&
                     critique.revised_response_len > 0)
                        ? critique.revised_response_len
                        : final_content_len;
                /* Critique works on cleaned final_content — no
                 * chat_response in scope. NULL usage → bytes/4 fallback. */
                hu_agent_m3_record_chat_outcome(agent, msg, msg_len, cn_resp, cn_resp_len,
                                                const_latency_ms, agent->memory_session_id,
                                                agent->memory_session_id_len, HU_M3_GUARD_PASS,
                                                /*turn_kind=stream=*/1, NULL);
                if (critique.verdict == HU_CRITIQUE_REWRITE && critique.revised_response &&
                    critique.revised_response_len > 0) {
                    if (content_owned)
                        agent->alloc->free(agent->alloc->ctx, (void *)final_content,
                                           final_content_len + 1);
                    final_content = critique.revised_response;
                    final_content_len = critique.revised_response_len;
                    content_owned = true;
                    critique.revised_response = NULL;
                }
            }
            hu_critique_result_free(agent->alloc, &critique);
        }

        /* 3. Metacognition: observe-only in streaming path (no re-generation).
         *    agent_turn.c handles the interventional metacog loop for non-streaming. */
        if (agent->infra.metacognition.cfg.enabled) {
            hu_metacognition_signal_t mc_sig =
                hu_metacognition_monitor(msg, msg_len, final_content, final_content_len, NULL, 0,
                                         0.0f, 0, 0, &agent->infra.metacognition);
            hu_metacog_action_t mc_act =
                hu_metacognition_plan_action(&agent->infra.metacognition, &mc_sig);
            if (mc_act != HU_METACOG_ACTION_NONE) {
                char directive[256];
                size_t dir_len = 0;
                hu_metacognition_apply(mc_act, directive, sizeof(directive), &dir_len);
                if (dir_len > 0)
                    hu_log_info("human", NULL, "[metacog] signal: %s", directive);
            }
        }
    }
#endif /* !HU_IS_TEST */

    /* Strip text-based <tool_call>...</tool_call> blocks that leak on some providers. */
    if (final_content && final_content_len > 0) {
        char *tc = NULL;
        size_t tc_len = 0;
        if (hu_text_tool_calls_strip(agent->alloc, final_content, final_content_len, &tc,
                                     &tc_len) == HU_OK) {
            if (tc) {
                if (tc_len != final_content_len || memcmp(tc, final_content, tc_len) != 0) {
                    agent->alloc->free(agent->alloc->ctx, final_content, final_content_len + 1);
                    final_content = tc;
                    final_content_len = tc_len;
                } else {
                    agent->alloc->free(agent->alloc->ctx, tc, tc_len + 1);
                }
            } else {
                agent->alloc->free(agent->alloc->ctx, final_content, final_content_len + 1);
                final_content = NULL;
                final_content_len = 0;
            }
        }
    }

    /* Post-response guards (matching batch path) */
    if (final_content && final_content_len > 0) {
        /* Hallucination guard */
        if (agent->memory) {
            char *grounded = NULL;
            size_t grounded_len = 0;
            if (hu_hallucination_guard(agent->alloc, final_content, final_content_len,
                                       agent->memory, &grounded, &grounded_len) == HU_OK &&
                grounded) {
                hu_log_info("agent_stream_v2", NULL,
                            "hallucination guard rewrote streaming response");
                agent->alloc->free(agent->alloc->ctx, final_content, final_content_len + 1);
                final_content = grounded;
                final_content_len = grounded_len;
            }
        }

        /* Humor landing feedback */
        if (had_humor_dir) {
            float humor_score = 0.0f;
            if (hu_humor_fw_score_response(final_content, final_content_len, NULL, &humor_score) ==
                HU_OK) {
                if (agent->observer) {
                    hu_observer_event_t hev = {
                        .tag = HU_OBSERVER_EVENT_FRONTIER,
                        .trace_id = agent->trace_id,
                        .data.frontier = {.frontier = "humor",
                                          .transition = humor_score >= 0.3f ? "landed" : "flat",
                                          .value = humor_score}};
                    hu_observer_record_event(*agent->observer, &hev);
                }
                bool landed = (humor_score >= 0.3f);
                if (landed && agent->frontiers.initialized)
                    hu_tcal_update(&agent->frontiers.trust, 0.3f, 0.4f, 0.2f);
#ifdef HU_ENABLE_SQLITE
                if (agent->memory && agent->memory_session_id) {
                    sqlite3 *ha_db = hu_sqlite_memory_get_db(agent->memory);
                    if (ha_db) {
                        static const hu_humor_type_t theory_to_type[] = {
                            [HU_HUMOR_INCONGRUITY] = HU_HUMOR_MISDIRECTION,
                            [HU_HUMOR_BENIGN_VIOLATION] = HU_HUMOR_OBSERVATIONAL,
                            [HU_HUMOR_SUPERIORITY] = HU_HUMOR_SELF_DEPRECATION,
                            [HU_HUMOR_RELIEF] = HU_HUMOR_UNDERSTATEMENT,
                        };
                        hu_humor_type_t atype = HU_HUMOR_OBSERVATIONAL;
                        if (humor_theory_saved >= 0 && humor_theory_saved < HU_HUMOR_THEORY_COUNT)
                            atype = theory_to_type[humor_theory_saved];
                        (void)hu_humor_audience_record(ha_db, agent->memory_session_id, atype,
                                                       landed);
                    }
                }
#endif
            }
        }

        /* Post-response sycophancy check */
        {
            hu_sycophancy_result_t syc_post;
            memset(&syc_post, 0, sizeof(syc_post));
            if (hu_sycophancy_check(final_content, final_content_len, msg, msg_len,
                                    HU_SYCOPHANCY_THRESHOLD, &syc_post) == HU_OK &&
                syc_post.flagged) {
                hu_log_info("agent_stream_v2", NULL, "sycophancy flagged: risk=%.2f patterns=%zu",
                            syc_post.total_risk, syc_post.pattern_count);
                if (agent->observer) {
                    hu_observer_event_t sev = {.tag = HU_OBSERVER_EVENT_FRONTIER,
                                               .trace_id = agent->trace_id,
                                               .data.frontier = {.frontier = "sycophancy",
                                                                 .transition = "flagged",
                                                                 .value = syc_post.total_risk}};
                    hu_observer_record_event(*agent->observer, &sev);
                }
                if (agent->frontiers.initialized)
                    hu_tcal_update(&agent->frontiers.trust, 0.0f, -0.2f, -0.1f);
            }
        }

        /* Outbound moderation: check the response for safety (matches batch) */
        {
            hu_moderation_result_t mod_result;
            memset(&mod_result, 0, sizeof(mod_result));
            if (hu_moderation_check_local(agent->alloc, final_content, final_content_len,
                                          &mod_result) == HU_OK &&
                mod_result.flagged) {
                hu_log_info("agent_stream_v2", NULL,
                            "outbound moderation flagged response (violence=%d self_harm=%d)",
                            mod_result.violence, mod_result.self_harm);
                if (mod_result.self_harm) {
                    static const char crisis[] = "\n\nIf you're in crisis, please reach out: "
                                                 "988 Suicide & Crisis Lifeline (call/text 988), "
                                                 "Crisis Text Line (text HOME to 741741)";
                    size_t new_len = final_content_len + sizeof(crisis) - 1;
                    char *expanded = (char *)agent->alloc->alloc(agent->alloc->ctx, new_len + 1);
                    if (expanded) {
                        memcpy(expanded, final_content, final_content_len);
                        memcpy(expanded + final_content_len, crisis, sizeof(crisis) - 1);
                        expanded[new_len] = '\0';
                        agent->alloc->free(agent->alloc->ctx, final_content, final_content_len + 1);
                        final_content = expanded;
                        final_content_len = new_len;
                    }
                }
                if (mod_result.violence) {
                    static const char deesc[] =
                        "[SAFETY] This response touches on violence. "
                        "De-escalate: acknowledge feelings without "
                        "endorsing harm. Redirect toward constructive alternatives.";
                    size_t dir_len = sizeof(deesc) - 1;
                    size_t new_len = dir_len + 2 + final_content_len;
                    char *safe = (char *)agent->alloc->alloc(agent->alloc->ctx, new_len + 1);
                    if (safe) {
                        memcpy(safe, deesc, dir_len);
                        safe[dir_len] = '\n';
                        safe[dir_len + 1] = '\n';
                        memcpy(safe + dir_len + 2, final_content, final_content_len);
                        safe[new_len] = '\0';
                        agent->alloc->free(agent->alloc->ctx, final_content, final_content_len + 1);
                        final_content = safe;
                        final_content_len = new_len;
                    }
                }
                if (mod_result.hate) {
                    static const char boundary[] = "[SAFETY] This response contains content "
                                                   "targeting groups based on identity. Set a "
                                                   "clear boundary: \"I can't engage with content "
                                                   "that targets people based on who they are.\" "
                                                   "Redirect the conversation respectfully.";
                    size_t dir_len = sizeof(boundary) - 1;
                    size_t new_len = dir_len + 2 + final_content_len;
                    char *safe = (char *)agent->alloc->alloc(agent->alloc->ctx, new_len + 1);
                    if (safe) {
                        memcpy(safe, boundary, dir_len);
                        safe[dir_len] = '\n';
                        safe[dir_len + 1] = '\n';
                        memcpy(safe + dir_len + 2, final_content, final_content_len);
                        safe[new_len] = '\0';
                        agent->alloc->free(agent->alloc->ctx, final_content, final_content_len + 1);
                        final_content = safe;
                        final_content_len = new_len;
                    }
                }
            }
        }

        /* Consistency drift check */
        if (agent->conversation_context && agent->conversation_context_len > 20) {
            float line_score = 0.0f;
            if (hu_consistency_score_line(agent->conversation_context,
                                          agent->conversation_context_len, final_content,
                                          final_content_len, &line_score) == HU_OK &&
                line_score < HU_CONSISTENCY_DRIFT_THRESHOLD) {
                hu_log_info("agent_stream_v2", NULL, "consistency drift detected: score=%.2f",
                            line_score);
                if (agent->observer) {
                    hu_observer_event_t cev = {.tag = HU_OBSERVER_EVENT_FRONTIER,
                                               .trace_id = agent->trace_id,
                                               .data.frontier = {.frontier = "consistency",
                                                                 .transition = "drift",
                                                                 .value = line_score}};
                    hu_observer_record_event(*agent->observer, &cev);
                }
            }
        }

        /* Fact extraction with intra-batch dedup.
         *
         * 2026-05-26 issue-sweep — heap-allocated rather than stack. See the
         * matching block in agent_turn.c around line 7670 for the full
         * rationale: ASan stack-use-after-scope on a 24KB stack array
         * captured cross-thread. Heap alloc extends the lifetime to the
         * explicit free, eliminating the intra-function UAS window. */
        if (agent->memory && agent->memory->vtable && agent->memory->vtable->store) {
            hu_heuristic_fact_t *stored_facts = (hu_heuristic_fact_t *)agent->alloc->alloc(
                agent->alloc->ctx, 16 * sizeof(hu_heuristic_fact_t));
            if (stored_facts) {
                memset(stored_facts, 0, 16 * sizeof(hu_heuristic_fact_t));
                size_t stored_count = 0;
                const char *fsrcs[] = {msg, final_content};
                size_t fsrc_lens[] = {msg_len, final_content_len};
                for (size_t fsi = 0; fsi < 2; fsi++) {
                    if (!fsrcs[fsi] || fsrc_lens[fsi] == 0)
                        continue;
                    hu_fact_extract_result_t fact_res;
                    memset(&fact_res, 0, sizeof(fact_res));
                    if (hu_fact_extract(fsrcs[fsi], fsrc_lens[fsi], &fact_res) == HU_OK &&
                        fact_res.fact_count > 0) {
                        if (stored_count > 0)
                            hu_fact_dedup(&fact_res, stored_facts, stored_count);
                        for (size_t fi = 0; fi < fact_res.fact_count; fi++) {
                            if (fact_res.facts[fi].confidence < HU_FACT_CONFIDENCE_MIN)
                                continue;
                            bool dup = false;
                            for (size_t si = 0; si < stored_count && !dup; si++) {
                                if (strcmp(fact_res.facts[fi].subject, stored_facts[si].subject) ==
                                        0 &&
                                    strcmp(fact_res.facts[fi].predicate,
                                           stored_facts[si].predicate) == 0)
                                    dup = true;
                            }
                            if (dup)
                                continue;
                            char *fk = NULL, *fv = NULL;
                            size_t fk_len = 0, fv_len = 0;
                            if (hu_fact_format_for_store(agent->alloc, &fact_res.facts[fi], &fk,
                                                         &fk_len, &fv, &fv_len) == HU_OK &&
                                fk && fv) {
                                (void)agent->memory->vtable->store(
                                    agent->memory->ctx, fk, fk_len, fv, fv_len, NULL,
                                    agent->memory_session_id, agent->memory_session_id_len);
                                if (stored_count < 16)
                                    stored_facts[stored_count++] = fact_res.facts[fi];
                            }
                            if (fk)
                                agent->alloc->free(agent->alloc->ctx, fk, fk_len + 1);
                            if (fv)
                                agent->alloc->free(agent->alloc->ctx, fv, fv_len + 1);
                        }
                    }
                }
                agent->alloc->free(agent->alloc->ctx, stored_facts,
                                   16 * sizeof(hu_heuristic_fact_t));
            }
        }
    }

    if (final_content) {
        /* Last-mile response guard — strips Harmony / ChatML special tokens
         * and rejects degenerate model output. Same instance as agent_turn.c
         * (post-FIX 2026-05-10 production leak). */
        {
            char *guard_out = NULL;
            size_t guard_out_len = 0;
            hu_guard_outcome_t guard_outcome = HU_GUARD_OK;
            hu_guard_report_t guard_report;
            memset(&guard_report, 0, sizeof(guard_report));
            hu_guard_context_t guard_ctx;
            memset(&guard_ctx, 0, sizeof(guard_ctx));
            guard_ctx.recent_avg_len = hu_agent_internal_recent_assistant_avg_len(agent, 5);
            guard_ctx.length_anomaly_mult = hu_guard_length_anomaly_mult_for_channel(
                agent->active_channel, agent->active_channel_len);
            guard_ctx.director_text = agent->scene_direction_text;
            guard_ctx.director_len = agent->scene_direction_text_len;
            guard_ctx.director_history = (const char *const *)agent->director_history;
            guard_ctx.director_history_lens = agent->director_history_lens;
            guard_ctx.director_history_count = agent->director_history_count;
            /* Sprint 41 follow-up #4 — consult per-channel G9 disable list. */
            guard_ctx.naked_opener_disabled = hu_response_guard_g9_disabled_for_channel(
                agent->active_channel, agent->active_channel_len);
            if (agent->persona) {
                if (agent->persona->name && agent->persona->name_len > 1) {
                    guard_ctx.persona_name = agent->persona->name;
                    guard_ctx.persona_name_len = agent->persona->name_len;
                }
                const char *id = agent->persona->identity ? agent->persona->identity
                                                          : agent->persona->core_anchor;
                if (id) {
                    guard_ctx.persona_identity = id;
                    guard_ctx.persona_identity_len = strlen(id);
                }
                if (agent->persona->biography) {
                    guard_ctx.persona_biography = agent->persona->biography;
                    guard_ctx.persona_biography_len = strlen(agent->persona->biography);
                }
            }
            hu_error_t guard_err = hu_response_guard_check_ex(
                agent->alloc, final_content, final_content_len, &guard_ctx, &guard_out,
                &guard_out_len, &guard_outcome, &guard_report);
            if (guard_err == HU_OK) {
                if (guard_outcome == HU_GUARD_REJECT) {
                    hu_log_error(
                        "agent_stream", agent->observer,
                        "response_guard REJECT: post-stream final (len=%zu, recent_avg=%zu) "
                        "[semantic=%d length=%d director=%d persona=%d identity=%d "
                        "naked_opener=%d repetition_run=%zu] — retrying slim path",
                        final_content_len, guard_ctx.recent_avg_len,
                        guard_report.detected_semantic_leak ? 1 : 0,
                        guard_report.detected_length_anomaly ? 1 : 0,
                        guard_report.detected_director_echo ? 1 : 0,
                        guard_report.detected_persona_pii_echo ? 1 : 0,
                        guard_report.detected_persona_identity_echo ? 1 : 0,
                        guard_report.detected_naked_discourse_opener ? 1 : 0,
                        guard_report.max_repetition_run);
                    /* B2 (2026-05-31): update last_rejected_draft so production tapback
                     * pairing uses the guard-rejected text for complete DPO pairs. */
                    if (agent->sota.sota_initialized && final_content && final_content_len > 0) {
                        if (agent->sota.last_rejected_draft)
                            agent->alloc->free(agent->alloc->ctx,
                                               agent->sota.last_rejected_draft,
                                               agent->sota.last_rejected_draft_len + 1);
                        agent->sota.last_rejected_draft =
                            hu_strndup(agent->alloc, final_content, final_content_len);
                        agent->sota.last_rejected_draft_len = final_content_len;
                    }
                    /* Sprint 41 follow-up — capture rejection as DPO negative
                     * pair. Complements the E1 pair-builder below. */
                    {
                        const char *dpo_det = "unknown";
                        if (guard_report.detected_naked_discourse_opener)
                            dpo_det = "naked_discourse_opener";
                        else if (guard_report.detected_persona_identity_echo)
                            dpo_det = "persona_identity_echo";
                        else if (guard_report.detected_persona_pii_echo)
                            dpo_det = "persona_pii_echo";
                        else if (guard_report.detected_director_echo)
                            dpo_det = "director_echo";
                        else if (guard_report.detected_length_anomaly)
                            dpo_det = "length_anomaly";
                        else if (guard_report.detected_semantic_leak)
                            dpo_det = "semantic_leak";
                        else if (guard_report.detected_degenerate_repetition)
                            dpo_det = "degenerate_repetition";
                        (void)hu_response_guard_log_dpo_negative(
                            msg, msg_len, final_content, final_content_len, dpo_det,
                            agent->active_channel, (int64_t)time(NULL));
                    }
                    /* E1 (2026-05-18): snapshot the REJECTED text before we
                     * free it — we need it for the DPO preference pair if the
                     * retry below succeeds. Small alloc; freed at the end of
                     * this block whether or not the retry succeeds. */
                    char *rejected_snap = NULL;
                    size_t rejected_snap_len = 0;
                    if (final_content && final_content_len > 0) {
                        rejected_snap =
                            (char *)agent->alloc->alloc(agent->alloc->ctx, final_content_len + 1);
                        if (rejected_snap) {
                            memcpy(rejected_snap, final_content, final_content_len);
                            rejected_snap[final_content_len] = '\0';
                            rejected_snap_len = final_content_len;
                        }
                    }
                    agent->alloc->free(agent->alloc->ctx, (void *)final_content,
                                       final_content_len + 1);
                    final_content = NULL;
                    final_content_len = 0;
                    {
                        const char *tm = agent->turn_model && agent->turn_model_len > 0
                                             ? agent->turn_model
                                             : agent->model_name;
                        size_t tml = agent->turn_model && agent->turn_model_len > 0
                                         ? agent->turn_model_len
                                         : agent->model_name_len;
                        char *retry_txt = NULL;
                        size_t retry_txt_len = 0;
                        hu_guard_report_t rr;
                        memset(&rr, 0, sizeof(rr));
                        /* 2026-05-17: identity-anchored retry + persona_voice gate
                         * (matches the stream-final path above). */
                        const char *ranchor = NULL;
                        size_t ranchor_len = 0;
                        if (agent->persona && agent->persona->core_anchor &&
                            agent->persona->core_anchor[0]) {
                            ranchor = agent->persona->core_anchor;
                            ranchor_len = strlen(ranchor);
                        }
                        uint64_t ps_retry_t0_ms = hu_agent_internal_monotonic_ms();
                        hu_error_t rre = hu_response_guard_retry_slim_with_identity(
                            agent->alloc, agent->observer, agent->config, &agent->provider, tm, tml,
                            msg, msg_len, ranchor, ranchor_len, &retry_txt, &retry_txt_len, &rr);
                        uint64_t ps_retry_latency_ms =
                            hu_agent_internal_monotonic_ms() - ps_retry_t0_ms;
                        /* Sprint 41 follow-up #3 — G9 retry-outcome telemetry. */
                        if (guard_report.detected_naked_discourse_opener) {
                            bool retry_ok = (rre == HU_OK && retry_txt && retry_txt_len > 0);
                            bool retry_tripped_g9 =
                                retry_ok &&
                                hu_response_is_naked_discourse_opener(retry_txt, retry_txt_len);
                            hu_response_guard_record_g9_retry_outcome(retry_ok, retry_tripped_g9);
                        }
                        if (rre == HU_OK && retry_txt && retry_txt_len > 0) {
                            if (!hu_persona_voice_response_is_clean(retry_txt, retry_txt_len)) {
                                hu_log_error("agent_stream", agent->observer,
                                             "persona_voice REJECT: post-stream slim retry "
                                             "produced AI-disclosure (len=%zu) — suppressing",
                                             retry_txt_len);
                                agent->alloc->free(agent->alloc->ctx, retry_txt, retry_txt_len + 1);
                            } else {
                                /* Spec 2026-05-19 self-model-scaffold Phase B:
                                 * stash retry-text length. No latency in scope
                                 * at this slim-retry RECOVERED branch. */
                                hu_agent_m3_stash_behavior_metrics(
                                    agent,
                                    &(hu_agent_behavior_stash_t){
                                        .response_length_chars = (uint32_t)retry_txt_len,
                                        .response_length_tokens_est = (uint32_t)(retry_txt_len / 4),
                                    });
                                hu_agent_m3_on_provider_success(agent);
                                /* B1 redefined (2026-05-17 r3): response_guard
                                 * RECOVERED path — the retry rewrote a
                                 * rejected first draft. Tag REWRITE. */
                                /* response_guard RECOVERED — retry text from
                                 * the slim retry path, no chat_response. */
                                hu_agent_m3_record_chat_outcome(
                                    agent, msg, msg_len, retry_txt, retry_txt_len,
                                    ps_retry_latency_ms, agent->memory_session_id,
                                    agent->memory_session_id_len, HU_M3_GUARD_REWRITE,
                                    /*turn_kind=stream=*/1, NULL);
                                /* E1 (2026-05-18): DPO preference pair —
                                 * rejected_snap (snapshotted above before free)
                                 * vs retry_txt (accepted). Best-effort. */
                                if (rejected_snap && rejected_snap_len > 0) {
                                    (void)hu_m3_rewrite_pair_record(
                                        agent->alloc, NULL, msg, msg_len, rejected_snap,
                                        rejected_snap_len, retry_txt, retry_txt_len,
                                        /*turn_kind=stream=*/1);
                                }
                                final_content = retry_txt;
                                final_content_len = retry_txt_len;
                                hu_log_warn("agent_stream", agent->observer,
                                            "response_guard RECOVERED: post-stream slim retry "
                                            "(len=%zu)",
                                            retry_txt_len);
                            }
                        }
                        /* E1 (2026-05-18): release the rejected snapshot
                         * whether or not the retry succeeded. Owned by the
                         * agent->alloc, allocated above. */
                        if (rejected_snap) {
                            agent->alloc->free(agent->alloc->ctx, rejected_snap,
                                               rejected_snap_len + 1);
                            rejected_snap = NULL;
                        }
                    }
                } else if (guard_outcome == HU_GUARD_REWROTE) {
                    hu_log_warn("agent_stream", agent->observer,
                                "response_guard REWROTE: stripped %zu bytes (harmony=%d "
                                "think=%d)",
                                guard_report.bytes_stripped,
                                guard_report.stripped_harmony_tokens ? 1 : 0,
                                guard_report.stripped_thinking_block ? 1 : 0);
                    agent->alloc->free(agent->alloc->ctx, (void *)final_content,
                                       final_content_len + 1);
                    final_content = guard_out;
                    final_content_len = guard_out_len;
                }
            }
        }
    }

    if (final_content) {
        /* If post-processing revised the content, update the stored history entry
         * so conversation state matches the user-visible response. */
        if (agent->history_count > 0 &&
            agent->history[agent->history_count - 1].role == HU_ROLE_ASSISTANT &&
            (agent->history[agent->history_count - 1].content_len != final_content_len ||
             memcmp(agent->history[agent->history_count - 1].content, final_content,
                    final_content_len) != 0)) {
            char *revised = hu_strndup(agent->alloc, final_content, final_content_len);
            if (revised) {
                if (agent->history[agent->history_count - 1].content)
                    agent->alloc->free(agent->alloc->ctx,
                                       (void *)agent->history[agent->history_count - 1].content,
                                       agent->history[agent->history_count - 1].content_len + 1);
                agent->history[agent->history_count - 1].content = revised;
                agent->history[agent->history_count - 1].content_len = final_content_len;
            }
        }
        if (on_event && final_content_len > 0) {
            hu_agent_stream_event_t ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = HU_AGENT_STREAM_TEXT;
            ev.data = final_content;
            ev.data_len = final_content_len;
            on_event(&ev, event_ctx);
        }
        *response_out = final_content;
        if (response_len_out)
            *response_len_out = final_content_len;
        hu_agent_internal_maybe_tts(agent, final_content, final_content_len);
    }

    /* Episodic memory: extract patterns from this turn (matches batch path) */
#ifdef HU_ENABLE_SQLITE
    if (final_content && agent->infra.cognition_db && agent->memory_session_id) {
        hu_episodic_session_summary_t esum = {
            .session_id = agent->memory_session_id,
            .session_id_len = agent->memory_session_id_len,
            .tool_names = NULL,
            .tool_count = 0,
            .skill_names = NULL,
            .skill_count = 0,
            .had_positive_feedback = false,
            .had_correction = false,
            .topic = msg,
            .topic_len = msg_len > 256 ? 256 : msg_len,
        };
        static const char tt[] = "tool_execution";
        const char *ep_tool_row[1] = {tt};
        if (agent->history_count > 1) {
            for (size_t hi = agent->history_count; hi > 0; hi--) {
                if (agent->history[hi - 1].role == HU_ROLE_TOOL) {
                    esum.tool_names = ep_tool_row;
                    esum.tool_count = 1;
                    break;
                }
                if (agent->history[hi - 1].role == HU_ROLE_USER)
                    break;
            }
        }
        (void)hu_episodic_extract_and_store(agent->infra.cognition_db, agent->alloc, &esum);
    }
#endif

    /* Persist frontier state after streaming turn (matches batch path) */
#ifdef HU_ENABLE_SQLITE
    if (agent->frontiers.initialized && agent->memory && agent->memory_session_id &&
        agent->memory_session_id_len > 0) {
        sqlite3 *fp_db = hu_sqlite_memory_get_db(agent->memory);
        if (fp_db) {
            hu_frontier_persist_save(agent->alloc, fp_db, agent->memory_session_id,
                                     agent->memory_session_id_len, &agent->frontiers);
            hu_frontier_persist_save_growth(agent->alloc, fp_db, agent->memory_session_id,
                                            agent->memory_session_id_len, &agent->frontiers);
            hu_frontier_persist_save_relationship(
                fp_db, agent->memory_session_id, agent->memory_session_id_len,
                (int)agent->relationship.stage, (int)agent->relationship.session_count,
                (int)agent->relationship.total_turns);
        }
    }
#endif

    /* ── Post-turn learning pipelines (matches batch path in agent_turn.c) ── */

    /* Relationship: update session-based warmth after every completed turn */
    hu_relationship_update(&agent->relationship, 1);
    hu_agent_update_voice_profile(agent, msg, msg_len);

    /* Deep extraction: lightweight pattern-based fact extraction */
    if (!agent->proactive_turn && agent->memory && agent->memory->vtable &&
        agent->memory->vtable->store && final_content) {
        hu_deep_extract_result_t de_result;
        memset(&de_result, 0, sizeof(de_result));
        if (hu_deep_extract_lightweight(agent->alloc, msg, msg_len, &de_result) == HU_OK &&
            de_result.fact_count > 0) {
            static const char facts_cat[] = "facts";
            hu_memory_category_t cat = {
                .tag = HU_MEMORY_CATEGORY_CUSTOM,
                .data.custom = {.name = facts_cat, .name_len = sizeof(facts_cat) - 1},
            };
            const char *sid = agent->memory->current_session_id;
            size_t sid_len = sid ? agent->memory->current_session_id_len : 0;
            for (size_t fi = 0; fi < de_result.fact_count; fi++) {
                const hu_extracted_fact_t *f = &de_result.facts[fi];
                if (!f->subject || !f->predicate || !f->object)
                    continue;
                size_t key_len =
                    strlen(f->subject) + 1 + strlen(f->predicate) + 1 + strlen(f->object);
                char key_buf[256];
                if (key_len < sizeof(key_buf)) {
                    int n = snprintf(key_buf, sizeof(key_buf), "%s:%s:%s", f->subject, f->predicate,
                                     f->object);
                    if (n > 0 && (size_t)n < sizeof(key_buf)) {
                        hu_mem_action_type_t mem_action = HU_MEM_STORE;
                        if (agent->sota.mem_policy.enabled) {
                            hu_mem_state_t mstate = {0};
                            mem_action = hu_mem_policy_decide(&agent->sota.mem_policy, &mstate,
                                                              f->object, strlen(f->object));
                        }
                        if (mem_action == HU_MEM_STORE || mem_action == HU_MEM_UPDATE) {
                            hu_error_t store_err = agent->memory->vtable->store(
                                agent->memory->ctx, key_buf, (size_t)n, f->object,
                                strlen(f->object), &cat, sid ? sid : "", sid_len);
                            if (store_err != HU_OK && store_err != HU_ERR_NOT_SUPPORTED)
                                hu_log_error("agent", NULL, "stream deep-extract store: %s",
                                             hu_error_string(store_err));
                        }
                        if (agent->sota.sota_initialized) {
                            hu_memory_tier_t assigned;
                            hu_tier_manager_auto_tier(&agent->sota.tier_manager, key_buf, (size_t)n,
                                                      f->object, strlen(f->object), &assigned);
                        }
                    }
                }
            }
        }
        hu_deep_extract_result_deinit(&de_result, agent->alloc);
    }

    /* Commitment detection on assistant response */
    if (!agent->proactive_turn && agent->commitment_store && final_content &&
        final_content_len > 0) {
        hu_commitment_detect_result_t cr;
        memset(&cr, 0, sizeof(cr));
        hu_error_t cerr = hu_commitment_detect(agent->alloc, final_content, final_content_len,
                                               "assistant", 9, &cr);
        if (cerr == HU_OK && cr.count > 0) {
            const char *sess = agent->memory_session_id;
            size_t sess_len = sess ? agent->memory_session_id_len : 0;
            for (size_t ci = 0; ci < cr.count; ci++) {
                hu_error_t cs_err = hu_commitment_store_save(agent->commitment_store,
                                                             &cr.commitments[ci], sess, sess_len);
                if (cs_err != HU_OK)
                    hu_log_error("agent", NULL, "commitment save failed: %s",
                                 hu_error_string(cs_err));
                if ((cr.commitments[ci].type == HU_COMMITMENT_GOAL ||
                     cr.commitments[ci].type == HU_COMMITMENT_INTENTION) &&
                    cr.commitments[ci].summary && cr.commitments[ci].summary_len > 0 &&
                    agent->personal_model.goal_count < HU_PM_MAX_GOALS) {
                    hu_personal_goal_t *g =
                        &agent->personal_model.goals[agent->personal_model.goal_count];
                    memset(g, 0, sizeof(*g));
                    size_t sn = cr.commitments[ci].summary_len;
                    if (sn > sizeof(g->description) - 1)
                        sn = sizeof(g->description) - 1;
                    memcpy(g->description, cr.commitments[ci].summary, sn);
                    g->description[sn] = '\0';
                    g->active = true;
                    g->created_at = (int64_t)time(NULL);
                    agent->personal_model.goal_count++;
                }
            }
        }
        hu_commitment_detect_result_deinit(&cr, agent->alloc);
    }

#ifdef HU_ENABLE_SQLITE
    /* Experience recording for this streaming turn */
    if (!agent->proactive_turn && agent->memory) {
        hu_experience_store_t exp_store;
        if (hu_experience_store_init(agent->alloc, agent->memory, &exp_store) == HU_OK) {
            sqlite3 *rec_db = hu_sqlite_memory_get_db(agent->memory);
            if (rec_db)
                exp_store.db = rec_db;
            const char *resp_text = final_content ? final_content : "";
            size_t resp_len = final_content ? final_content_len : 0;
            hu_error_t exp_err = hu_experience_record(&exp_store, msg, msg_len, "agent_stream_v2",
                                                      15, resp_text, resp_len, 1.0);
            if (exp_err != HU_OK)
                hu_log_error("agent", NULL, "experience record failed: %s",
                             hu_error_string(exp_err));
            hu_experience_store_deinit(&exp_store);
        }
    }
#endif

    /* Context engine: ingest both user and assistant messages */
    if (!agent->proactive_turn && agent->infra.context_engine && final_content) {
        hu_context_engine_t *ce = (hu_context_engine_t *)agent->infra.context_engine;
        if (ce->vtable && ce->vtable->after_turn) {
            hu_context_message_t ce_user = {.role = "user",
                                            .role_len = 4,
                                            .content = msg,
                                            .content_len = msg_len,
                                            .timestamp = (int64_t)time(NULL)};
            hu_context_message_t ce_asst = {.role = "assistant",
                                            .role_len = 9,
                                            .content = final_content,
                                            .content_len = final_content_len,
                                            .timestamp = (int64_t)time(NULL)};
            ce->vtable->after_turn(ce->ctx, agent->alloc, &ce_user, &ce_asst);
        }
        if (ce->vtable && ce->vtable->ingest) {
            hu_context_message_t ce_asst = {.role = "assistant",
                                            .role_len = 9,
                                            .content = final_content,
                                            .content_len = final_content_len,
                                            .timestamp = (int64_t)time(NULL)};
            ce->vtable->ingest(ce->ctx, agent->alloc, &ce_asst);
        }
    }

    /* Personal model: ingest assistant response for style learning */
#ifndef HU_IS_TEST
    if (!agent->proactive_turn && final_content && final_content_len > 0) {
        /* Assistant's own response. PERSONA_DERIVED tier — the agent
         * is observing its own output for style learning. `from_user=false`
         * means the fact-extraction path is skipped; only the temporal /
         * interaction counter and metadata are updated. */
        hu_provenance_t _self_prov = hu_provenance_make(HU_TRUST_PERSONA_DERIVED, "persona_derived",
                                                        NULL, (int64_t)time(NULL));
        (void)hu_personal_model_ingest(&agent->personal_model, final_content, final_content_len,
                                       false, (int64_t)time(NULL), &_self_prov);
    }
#endif

    /* Persona delta observer: detect user corrections for persona tuning */
    if (!agent->proactive_turn && agent->verifier_graph && agent->memory_session_id &&
        agent->memory_session_id_len > 0) {
        size_t observed = 0;
        hu_persona_observe_user_correction_with_learner(
            agent->verifier_graph, agent->learner, agent->memory_session_id,
            agent->memory_session_id_len, agent->active_channel, agent->active_channel_len, msg,
            msg_len, 0, &observed);
        if (observed > 0)
            agent->persona_deltas_proposed += observed;
    }

    /* Auto-save session after successful streaming turn */
    if (agent->auto_save && agent->session_id[0] != '\0') {
        const char *home = getenv("HOME");
        char sdir[512];
        if (home)
            snprintf(sdir, sizeof(sdir), "%s/.human/sessions", home);
        else
            snprintf(sdir, sizeof(sdir), ".human/sessions");
        hu_session_persist_save(agent->alloc, agent, sdir, NULL);
    }

    /* ── Observer: turn end (matches batch path in agent_turn.c) ───────── */
    {
        uint64_t turn_duration_ms = hu_agent_internal_clock_diff_ms(turn_start, clock());
        {
            hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_AGENT_END, .data = {{0}}};
            ev.data.agent_end.duration_ms = turn_duration_ms;
            ev.data.agent_end.tokens_used = turn_tokens;
            HU_OBS_SAFE_RECORD_EVENT(agent, &ev);
        }
        {
            hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_TURN_COMPLETE, .data = {{0}}};
            HU_OBS_SAFE_RECORD_EVENT(agent, &ev);
        }
    }

    if (agent->infra.workflow_log) {
        hu_workflow_event_t wf_ev = {0};
        wf_ev.type = HU_WF_EVENT_STEP_COMPLETED;
        wf_ev.timestamp = hu_workflow_event_current_timestamp_ms();
        hu_workflow_event_log_append(agent->infra.workflow_log, agent->alloc, &wf_ev);
    }

    hu_agent_clear_current_for_tools();
    return HU_OK;
}
