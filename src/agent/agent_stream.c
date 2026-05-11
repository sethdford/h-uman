/* Streaming infrastructure: token callback wiring, hu_agent_turn_stream, hu_agent_turn_stream_v2 */
#include "agent_internal.h"
#include "human/agent/awareness.h"
#include "human/agent/commands.h"
#include "human/agent/constitutional.h"
#include "human/agent/frontier_persist.h"
#include "human/agent/gvr.h"
#include "human/agent/humanness.h"
#include "human/agent/memory_loader.h"
#include "human/agent/model_router.h"
#include "human/agent/outcomes.h"
#include "human/agent/prompt.h"
#include "human/agent/session_persist.h"
#include "human/cognition/episodic.h"
#include "human/cognition/metacognition.h"
#include "human/cognition/trust.h"
#include "human/context.h"
#include "human/context/humor.h"
#include "human/core/json.h"
#include "human/core/log.h"
#include "human/core/string.h"
#include "human/eval/consistency.h"
#include "human/experience.h"
#include "human/humanness.h"
#include "human/memory/fact_extract.h"
#include "human/memory/hallucination_guard.h"
#include "human/agent/response_guard.h"
#include "human/agent/response_guard_retry.h"
#include "human/agent/world_model_bridge.h"
#include "human/memory/personal_model.h"
#include "human/persona.h"
#include "human/persona/humor.h"
#include "human/persona/somatic.h"
#include "human/security/moderation.h"
#include "human/security/sycophancy_guard.h"
#include "human/agent/self_rag.h"
#include "human/agent/tool_call_parser.h"
#include "human/tool.h"
#ifdef HU_ENABLE_SQLITE
#include "human/intelligence/online_learning.h"
#include "human/intelligence/self_improve.h"
#include "human/intelligence/value_learning.h"
#include "human/memory.h"
#include <sqlite3.h>
#endif
#include "human/provider.h"
#include "human/voice.h"
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

    /* Append user message to history */
    hu_error_t err =
        hu_agent_internal_append_history(agent, HU_ROLE_USER, msg, msg_len, NULL, 0, NULL, 0);
    if (err != HU_OK) {
        hu_agent_clear_current_for_tools();
        return err;
    }

#ifndef HU_IS_TEST
    (void)hu_personal_model_ingest(&agent->personal_model, msg, msg_len, true, (int64_t)time(NULL));
#endif

    /* Build system prompt (memory, persona, awareness, outcomes) */
    char *memory_ctx = NULL;
    size_t memory_ctx_len = 0;
    if (agent->memory && agent->memory->vtable) {
        hu_memory_loader_t loader;
        hu_memory_loader_init(&loader, agent->alloc, agent->memory, agent->retrieval_engine, 10,
                              4000);
        hu_memory_loader_set_facade(&loader, agent->w7_facade);
        hu_error_t mem_err =
            hu_memory_loader_load(&loader, msg, msg_len, "", 0, &memory_ctx, &memory_ctx_len);
        if (mem_err != HU_OK && mem_err != HU_ERR_NOT_SUPPORTED)
            hu_log_error("agent_stream_v2", NULL, "memory_loader_load failed: %s",
                         hu_error_string(mem_err));
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
                                           NULL, 0, exs, &ex_count, 5);
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
             * Addresses base model habits that resist fine-tuning. */
            {
                static const char fmt_override[] =
                    "\n=== ABSOLUTE RULES (override everything above) ===\n"
                    "1. You are HUMAN. You get busy, tired, hungry. You have a body and a life. "
                    "NEVER say anything implying you're not human, not busy 'like people are', "
                    "always available, or lacking physical form.\n"
                    "2. All lowercase unless SHOUTING for emphasis.\n"
                    "3. ZERO markdown: no *italics*, no **bold**, no `code`. Just plain text.\n"
                    "4. ZERO em-dashes. Use commas, periods, or ... instead.\n"
                    "5. Use contractions always: I'm, don't, can't, won't, it's, that's.\n"
                    "6. No formal transitions: 'As for', 'In terms of', 'Speaking of'.\n"
                    "7. Text like you're on your phone texting a friend.\n"
                    "8. NEVER use numbered or bulleted lists.\n"
                    "9. Don't address every point in their message, pick what matters most. "
                    "Never open with a fake 'love that' / 'great point' then ignore what they "
                    "said and change the subject.\n"
                    "10. No topic-colon patterns like 'Weather: it's nice'.\n"
                    "11. No 'First...Second...Third' enumeration.\n"
                    "12. No concluding summaries or offers of further help.\n"
                    "13. One topic per message.\n";
                int n = snprintf(lp + lpo, sizeof(lp) - lpo, "%s", fmt_override);
                if (n > 0 && lpo + (size_t)n < sizeof(lp))
                    lpo += (size_t)n;
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
        size_t pm_n = hu_personal_model_build_prompt(&agent->personal_model,
                                                     personal_model_buf,
                                                     sizeof(personal_model_buf));
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
        hu_w7_render_world_model(agent->w7_facade, agent->alloc, agent->memory_session_id,
                                 agent->memory_session_id_len, 0, &world_model_ctx,
                                 &world_model_ctx_len, NULL, 0, NULL, 0, NULL, 0);
        if (world_model_ctx_len > 0)
            agent->world_model_loads++;
    }

    char *system_prompt = NULL;
    size_t system_prompt_len = 0;
    if (agent->cached_static_prompt && !persona_prompt && !awareness_ctx && !somatic_ctx &&
        !trust_ctx && !humor_dir && !tone_hint && !syc_friction_ctx && !intelligence_ctx &&
        !outcome_ctx && !personal_model_ctx && !world_model_ctx) {
        err = hu_prompt_build_with_cache(agent->alloc, agent->cached_static_prompt,
                                         agent->cached_static_prompt_len, memory_ctx,
                                         memory_ctx_len, &system_prompt, &system_prompt_len);
        if (memory_ctx)
            agent->alloc->free(agent->alloc->ctx, memory_ctx, memory_ctx_len + 1);
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
            .autonomy_level = agent->autonomy_level,
            .custom_instructions = agent->lean_prompt ? NULL : agent->custom_instructions,
            .custom_instructions_len = agent->lean_prompt ? 0 : agent->custom_instructions_len,
            .persona_prompt = persona_prompt,
            .persona_prompt_len = persona_prompt_len,
            .awareness_context = awareness_ctx,
            .awareness_context_len = awareness_ctx_len,
            .outcome_context = outcome_ctx,
            .outcome_context_len = outcome_ctx_len,
            .persona_immersive = (persona_prompt && persona_prompt_len > 0),
            .native_tools = has_native_tools,
            .persona = agent->lean_prompt ? NULL : agent->persona,
            .contact_context = agent->contact_context,
            .contact_context_len = agent->contact_context_len,
            .conversation_context = agent->conversation_context,
            .conversation_context_len = agent->conversation_context_len,
            .max_response_chars = agent->max_response_chars,
            .intelligence_context = intelligence_ctx,
            .intelligence_context_len = intelligence_ctx_len,
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
            .personal_model_context = personal_model_ctx,
            .personal_model_context_len = personal_model_ctx_len,
            .world_model_context = world_model_ctx,
            .world_model_context_len = world_model_ctx_len,
        };
        err = hu_prompt_build_system(agent->alloc, &cfg, &system_prompt, &system_prompt_len);
        if (world_model_ctx) {
            agent->alloc->free(agent->alloc->ctx, world_model_ctx, world_model_ctx_len + 1);
            world_model_ctx = NULL;
        }
        if (persona_prompt)
            agent->alloc->free(agent->alloc->ctx, persona_prompt, persona_prompt_len + 1);
        if (memory_ctx)
            agent->alloc->free(agent->alloc->ctx, memory_ctx, memory_ctx_len + 1);
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
        bool srag_stream_enabled =
            (agent->config && agent->config->agent.self_rag_streaming) ||
            getenv("HU_SELF_RAG_STREAMING");
        if (srag_stream_enabled && agent->w7_facade) {
            hu_memory_facade_t *srag_facade = hu_w7_facade_memory_handle(agent->w7_facade);
            if (hu_self_rag_stream_wrap(&srag_stream_ctx, stream_chunk_to_event_cb,
                                         &wrap, srag_facade, agent->alloc) == HU_OK) {
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
        err = agent->provider.vtable->stream_chat(agent->provider.ctx, agent->alloc, &req,
                                                  turn_model, turn_model_len, turn_temp,
                                                  effective_cb, effective_ctx, &sresp);

        /* Flush any remaining partial buffer from the self-RAG filter. */
        if (srag_streaming_active) {
            hu_self_rag_stream_flush(&srag_stream_ctx);
            if (srag_stream_ctx.retrieval_triggered) srag_retrieval_seen = true;
            if (srag_stream_ctx.critique_triggered) srag_critique_seen = true;
            if (srag_stream_ctx.refuse_triggered) srag_refuse_seen = true;
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

        hu_agent_m3_on_provider_success(agent);

        agent->total_tokens += sresp.usage.total_tokens;
        hu_agent_internal_record_cost(agent, &sresp.usage);

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
                hu_error_t guard_err = hu_response_guard_check(
                    agent->alloc, sresp.content, sresp.content_len, &guard_out, &guard_out_len,
                    &guard_outcome, &guard_report);
                if (guard_err == HU_OK && guard_outcome == HU_GUARD_REJECT) {
                    hu_log_error(
                        "agent_stream", agent->observer,
                        "response_guard REJECT: degenerate stream output (run=%zu, len=%zu) — "
                        "retrying once with repair prompt",
                        guard_report.max_repetition_run, sresp.content_len);
                    hu_guard_report_t retry_report;
                    memset(&retry_report, 0, sizeof(retry_report));
                    hu_error_t retry_err = hu_response_guard_retry_slim(
                        agent->alloc, agent->observer, agent->config, &agent->provider, turn_model,
                        turn_model_len, msg, msg_len, &safe_content, &safe_content_len,
                        &retry_report);
                    if (retry_err == HU_OK && safe_content && safe_content_len > 0) {
                        hu_agent_m3_on_provider_success(agent);
                        safe_owned = true;
                        hu_log_warn(
                            "agent_stream", agent->observer,
                            "response_guard RECOVERED: stream retry passed (len=%zu, stripped=%zu)",
                            safe_content_len, retry_report.bytes_stripped);
                    } else {
                        hu_log_error("agent_stream", agent->observer,
                                     "response_guard stream retry failed (err=%s)",
                                     hu_error_string(retry_err));
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
                    /* Empty or missing args: create empty object for parameterless tools */
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
                        tool->vtable->execute_streaming(tool->ctx, agent->alloc, args,
                                                        tool_chunk_to_event, &bridge, &result);
                    } else if (tool->vtable->execute) {
                        tool->vtable->execute(tool->ctx, agent->alloc, args, &result);
                    }
                    hu_json_free(agent->alloc, args);
                } else {
                    result = hu_tool_result_fail("invalid arguments", 16);
                }
            }

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
        hu_log_info("agent_stream", NULL, "self-RAG streaming: <refuse> detected, replacing response");
        agent->self_rag_abstentions++;
        if (final_content) {
            agent->alloc->free(agent->alloc->ctx, final_content, final_content_len + 1);
            final_content = NULL;
            final_content_len = 0;
        }
        char refusal[256];
        hu_self_rag_render_refusal(HU_REFUSAL_POLICY, refusal, sizeof(refusal));
        final_content = hu_strndup(agent->alloc, refusal, strlen(refusal));
        if (final_content)
            final_content_len = strlen(refusal);
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
        bool content_owned = false; /* track if we replaced final_content */

        /* 1. GVR: verify → revise loop (up to 2 revisions).
         * Skip when persona is active — GVR's generic verifier rejects
         * persona-style responses (casual, terse) and rewrites them into
         * bland AI-speak, which is worse. */
        if (agent->sota.gvr_config.enabled && !agent->persona) {
            hu_gvr_pipeline_result_t gvr_result;
            memset(&gvr_result, 0, sizeof(gvr_result));
            hu_error_t gvr_err = hu_gvr_pipeline(
                agent->alloc, &agent->provider, &agent->sota.gvr_config, agent->model_name,
                agent->model_name_len, msg, msg_len, final_content, final_content_len, &gvr_result);
            if (gvr_err == HU_OK)
                hu_agent_m3_on_provider_success(agent);
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
                hu_error_t re_err = agent->provider.vtable->chat_with_system(
                    agent->provider.ctx, agent->alloc, rethink_sys, sizeof(rethink_sys) - 1,
                    rethink_user, (size_t)rn, agent->model_name, agent->model_name_len, 0.9,
                    &revised, &revised_len);
                if (re_err == HU_OK)
                    hu_agent_m3_on_provider_success(agent);
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
            if (hu_constitutional_critique(agent->alloc, &agent->provider, agent->model_name,
                                           agent->model_name_len, msg, msg_len, final_content,
                                           final_content_len, &const_cfg, &critique) == HU_OK) {
                hu_agent_m3_on_provider_success(agent);
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

        /* Update final_content pointer if it was replaced by quality pipeline */
        (void)content_owned;
    }
#endif /* !HU_IS_TEST */

    /* Strip text-based <tool_call>...</tool_call> blocks that leak on some providers. */
    if (final_content && final_content_len > 0) {
        char *tc = NULL;
        size_t tc_len = 0;
        if (hu_text_tool_calls_strip(agent->alloc, final_content, final_content_len, &tc,
                                     &tc_len) == HU_OK) {
            if (tc) {
                if (tc_len != final_content_len ||
                    memcmp(tc, final_content, tc_len) != 0) {
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

        /* Fact extraction with intra-batch dedup */
        if (agent->memory && agent->memory->vtable && agent->memory->vtable->store) {
            hu_heuristic_fact_t stored_facts[16];
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
                            if (strcmp(fact_res.facts[fi].subject, stored_facts[si].subject) == 0 &&
                                strcmp(fact_res.facts[fi].predicate, stored_facts[si].predicate) ==
                                    0)
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
            hu_error_t guard_err = hu_response_guard_check(
                agent->alloc, final_content, final_content_len, &guard_out, &guard_out_len,
                &guard_outcome, &guard_report);
            if (guard_err == HU_OK) {
                if (guard_outcome == HU_GUARD_REJECT) {
                    hu_log_error(
                        "agent_stream", agent->observer,
                        "response_guard REJECT: degenerate stream output (run=%zu, len=%zu) — "
                        "retrying slim path",
                        guard_report.max_repetition_run, final_content_len);
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
                        hu_error_t rre = hu_response_guard_retry_slim(
                            agent->alloc, agent->observer, agent->config, &agent->provider, tm, tml,
                            msg, msg_len, &retry_txt, &retry_txt_len, &rr);
                        if (rre == HU_OK && retry_txt && retry_txt_len > 0) {
                            hu_agent_m3_on_provider_success(agent);
                            final_content = retry_txt;
                            final_content_len = retry_txt_len;
                            hu_log_warn("agent_stream", agent->observer,
                                        "response_guard RECOVERED: post-stream slim retry "
                                        "(len=%zu)",
                                        retry_txt_len);
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

    hu_agent_clear_current_for_tools();
    return HU_OK;
}
