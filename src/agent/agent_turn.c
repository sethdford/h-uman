/* Core turn execution: sc_agent_turn and turn-local helpers */
#include "agent_internal.h"
#include "seaclaw/agent/awareness.h"
#include "seaclaw/agent/commitment.h"
#include "seaclaw/agent/commitment_store.h"
#include "seaclaw/agent/commands.h"
#include "seaclaw/agent/compaction.h"
#include "seaclaw/agent/dispatcher.h"
#include "seaclaw/agent/input_guard.h"
#include "seaclaw/agent/mailbox.h"
#include "seaclaw/agent/memory_loader.h"
#include "seaclaw/agent/outcomes.h"
#include "seaclaw/agent/planner.h"
#include "seaclaw/agent/preferences.h"
#include "seaclaw/agent/tool_router.h"
#include "seaclaw/agent/pattern_radar.h"
#include "seaclaw/agent/proactive.h"
#include "seaclaw/agent/prompt.h"
#include "seaclaw/agent/superhuman.h"
#ifdef SC_HAS_PERSONA
#include "seaclaw/persona/circadian.h"
#include "seaclaw/persona/relationship.h"
#endif
#include "seaclaw/agent/reflection.h"
#include "seaclaw/context.h"
#include "seaclaw/context_tokens.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/memory/deep_extract.h"
#include "seaclaw/memory/fast_capture.h"
#include "seaclaw/memory/stm.h"
#ifdef SC_HAS_PERSONA
#include "seaclaw/persona.h"
#endif
#include "seaclaw/provider.h"
#include "seaclaw/voice.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

sc_error_t sc_agent_turn(sc_agent_t *agent, const char *msg, size_t msg_len, char **response_out,
                         size_t *response_len_out) {
    if (!agent || !msg || !response_out)
        return SC_ERR_INVALID_ARGUMENT;
    *response_out = NULL;
    if (response_len_out)
        *response_len_out = 0;

    sc_agent_set_current_for_tools(agent);

    sc_agent_internal_process_mailbox_messages(agent);

    char *slash_resp = sc_agent_handle_slash_command(agent, msg, msg_len);
    if (slash_resp) {
        sc_agent_clear_current_for_tools();
        *response_out = slash_resp;
        if (response_len_out)
            *response_len_out = strlen(slash_resp);
        return SC_OK;
    }

    /* Prompt injection defense-in-depth */
    {
        sc_injection_risk_t risk = SC_INJECTION_SAFE;
        sc_error_t guard_err = sc_input_guard_check(msg, msg_len, &risk);
        if (guard_err != SC_OK) {
            sc_agent_clear_current_for_tools();
            return guard_err;
        }
        if (risk == SC_INJECTION_HIGH_RISK && agent->observer) {
            sc_observer_event_t ev = {.tag = SC_OBSERVER_EVENT_ERR};
            ev.data.err.component = "input_guard";
            ev.data.err.message = "high-risk injection pattern detected";
            sc_observer_record_event(*agent->observer, &ev);
        }
    }

    sc_error_t err =
        sc_agent_internal_append_history(agent, SC_ROLE_USER, msg, msg_len, NULL, 0, NULL, 0);
    if (err != SC_OK) {
        sc_agent_clear_current_for_tools();
        return err;
    }

    /* Superhuman: observe user message (emotional, silence services) */
    (void)sc_superhuman_observe_all(&agent->superhuman, agent->alloc, msg, msg_len, "user", 4);

    /* Fast-capture and STM: extract entities/emotions, record turn, populate last turn */
    {
        sc_fc_result_t fc_result;
        memset(&fc_result, 0, sizeof(fc_result));
        (void)sc_fast_capture(agent->alloc, msg, msg_len, &fc_result);

        uint64_t ts_ms = (uint64_t)time(NULL) * 1000;
        err = sc_stm_record_turn(&agent->stm, "user", 4, msg, msg_len, ts_ms);
        if (err == SC_OK) {
            size_t last_idx = sc_stm_count(&agent->stm) - 1;
            for (size_t i = 0; i < fc_result.entity_count; i++) {
                const sc_fc_entity_match_t *e = &fc_result.entities[i];
                uint32_t mention = 1;
                (void)sc_stm_turn_add_entity(&agent->stm, last_idx, e->name, e->name_len,
                                             e->type ? e->type : "entity",
                                             e->type ? e->type_len : 6, mention);
            }
            for (size_t i = 0; i < fc_result.emotion_count; i++) {
                (void)sc_stm_turn_add_emotion(&agent->stm, last_idx, fc_result.emotions[i].tag,
                                              fc_result.emotions[i].intensity);
            }
        }

        /* Pattern radar: observe entities as topic recurrence, emotions as emotional trend */
        {
            char ts_buf[32];
            int ts_n = snprintf(ts_buf, sizeof(ts_buf), "%llu",
                                (unsigned long long)(ts_ms / 1000));
            const char *ts = ts_n > 0 ? ts_buf : NULL;
            size_t ts_len = (ts_n > 0 && ts_n < (int)sizeof(ts_buf)) ? (size_t)ts_n : 0;

            for (size_t i = 0; i < fc_result.entity_count; i++) {
                const sc_fc_entity_match_t *e = &fc_result.entities[i];
                if (e->name && e->name_len > 0) {
                    (void)sc_pattern_radar_observe(&agent->radar, e->name, e->name_len,
                                                    SC_PATTERN_TOPIC_RECURRENCE,
                                                    e->type ? e->type : NULL,
                                                    e->type ? e->type_len : 0, ts, ts_len);
                }
            }
            static const char *emotion_names[] = {"neutral", "joy", "sadness", "anger", "fear",
                                                  "surprise", "frustration", "excitement",
                                                  "anxiety"};
            for (size_t i = 0; i < fc_result.emotion_count; i++) {
                sc_emotion_tag_t tag = fc_result.emotions[i].tag;
                if (tag >= 0 && (size_t)tag < sizeof(emotion_names) / sizeof(emotion_names[0])) {
                    const char *name = emotion_names[tag];
                    (void)sc_pattern_radar_observe(&agent->radar, name, strlen(name),
                                                    SC_PATTERN_EMOTIONAL_TREND, NULL, 0, ts,
                                                    ts_len);
                }
            }
        }
        sc_fc_result_deinit(&fc_result, agent->alloc);
    }

    /* Commitment detection: extract promises, intentions, reminders, goals from user message */
    if (agent->commitment_store) {
        sc_commitment_detect_result_t commit_result;
        memset(&commit_result, 0, sizeof(commit_result));
        sc_error_t cerr = sc_commitment_detect(agent->alloc, msg, msg_len, "user", 4, &commit_result);
        if (cerr == SC_OK && commit_result.count > 0) {
            const char *sess = agent->memory_session_id ? agent->memory_session_id : NULL;
            size_t sess_len = agent->memory_session_id_len;
            for (size_t i = 0; i < commit_result.count; i++) {
                (void)sc_commitment_store_save(agent->commitment_store, &commit_result.commitments[i],
                                                sess, sess_len);
            }
        }
        sc_commitment_detect_result_deinit(&commit_result, agent->alloc);
    }

    /* Detect preferences from user corrections and store them */
    bool is_correction = sc_preferences_is_correction(msg, msg_len);
    if (agent->memory && is_correction) {
        size_t pref_len = 0;
        char *pref = sc_preferences_extract(agent->alloc, msg, msg_len, &pref_len);
        if (pref) {
            sc_preferences_store(agent->memory, agent->alloc, pref, pref_len);
            agent->alloc->free(agent->alloc->ctx, pref, pref_len + 1);
        }
    }

    /* Outcome tracking: record corrections and positive feedback */
    if (agent->outcomes) {
        if (is_correction) {
            const char *prev_response = NULL;
            if (agent->history_count >= 2 &&
                agent->history[agent->history_count - 2].role == SC_ROLE_ASSISTANT)
                prev_response = agent->history[agent->history_count - 2].content;
            sc_outcome_record_correction(agent->outcomes, prev_response, msg);

#ifdef SC_HAS_PERSONA
            if (agent->outcomes->auto_apply_feedback && agent->persona && agent->persona_name &&
                prev_response) {
                sc_persona_feedback_t fb = {
                    .channel = agent->active_channel,
                    .channel_len = agent->active_channel_len,
                    .original_response = prev_response,
                    .original_response_len = strlen(prev_response),
                    .corrected_response = msg,
                    .corrected_response_len = msg_len,
                };
                (void)sc_persona_feedback_record(agent->alloc, agent->persona_name,
                                                 strlen(agent->persona_name), &fb);
            }
#endif
        } else if (msg_len >= 5 && msg_len <= 80) {
            /* Detect simple positive feedback */
            bool positive = false;
            for (size_t k = 0; k + 5 <= msg_len && !positive; k++) {
                char c0 = msg[k] | 0x20, c1 = msg[k + 1] | 0x20, c2 = msg[k + 2] | 0x20;
                char c3 = msg[k + 3] | 0x20, c4 = msg[k + 4] | 0x20;
                if (c0 == 't' && c1 == 'h' && c2 == 'a' && c3 == 'n' && c4 == 'k')
                    positive = true;
                if (c0 == 'g' && c1 == 'r' && c2 == 'e' && c3 == 'a' && c4 == 't')
                    positive = true;
                if (k + 6 <= msg_len && c0 == 'p' && c1 == 'e' && c2 == 'r' && c3 == 'f' &&
                    c4 == 'e' && (msg[k + 5] | 0x20) == 'c')
                    positive = true;
            }
            if (positive)
                sc_outcome_record_positive(agent->outcomes, msg);
        }
    }

    /* Detect tone from recent user messages */
    const char *tone_hint = NULL;
    size_t tone_hint_len = 0;
    {
        const char *recent_msgs[3];
        size_t recent_lens[3];
        size_t rm_count = 0;
        for (size_t i = agent->history_count; i > 0 && rm_count < 3; i--) {
            if (agent->history[i - 1].role == SC_ROLE_USER && agent->history[i - 1].content) {
                recent_msgs[rm_count] = agent->history[i - 1].content;
                recent_lens[rm_count] = agent->history[i - 1].content_len;
                rm_count++;
            }
        }
        if (rm_count > 0) {
            sc_tone_t tone = sc_detect_tone(recent_msgs, recent_lens, rm_count);
            tone_hint = sc_tone_hint_string(tone, &tone_hint_len);
        }
    }

    /* Load user preferences for prompt injection */
    char *pref_ctx = NULL;
    size_t pref_ctx_len = 0;
    if (agent->memory)
        (void)sc_preferences_load(agent->memory, agent->alloc, &pref_ctx, &pref_ctx_len);

    /* Load memory context for this turn */
    char *memory_ctx = NULL;
    size_t memory_ctx_len = 0;
    if (agent->memory && agent->memory->vtable) {
        sc_memory_loader_t loader;
        sc_memory_loader_init(&loader, agent->alloc, agent->memory, agent->retrieval_engine, 10,
                              4000);
        (void)sc_memory_loader_load(&loader, msg, msg_len, "", 0, &memory_ctx, &memory_ctx_len);
    }

    /* Build STM context for this turn */
    char *stm_ctx = NULL;
    size_t stm_ctx_len = 0;
    (void)sc_stm_build_context(&agent->stm, agent->alloc, &stm_ctx, &stm_ctx_len);

    /* Build commitment context for this turn */
    char *commitment_ctx = NULL;
    size_t commitment_ctx_len = 0;
    if (agent->commitment_store) {
        const char *sess = agent->memory_session_id ? agent->memory_session_id : NULL;
        size_t sess_len = agent->memory_session_id_len;
        (void)sc_commitment_store_build_context(agent->commitment_store, agent->alloc, sess,
                                                 sess_len, &commitment_ctx, &commitment_ctx_len);
    }

    /* Build pattern radar context for this turn */
    char *pattern_ctx = NULL;
    size_t pattern_ctx_len = 0;
    (void)sc_pattern_radar_build_context(&agent->radar, agent->alloc, &pattern_ctx,
                                         &pattern_ctx_len);

    /* Build proactive context (milestones, morning briefing, check-in) */
    char *proactive_ctx = NULL;
    size_t proactive_ctx_len = 0;
    {
        uint32_t session_count = 0;
        uint8_t hour = 10;
#ifdef SC_HAS_PERSONA
        session_count = agent->relationship.session_count;
#endif
#ifndef SC_IS_TEST
        {
            time_t now = time(NULL);
            struct tm *lt = localtime(&now);
            if (lt)
                hour = (uint8_t)(lt->tm_hour & 0xFF);
        }
#endif
        sc_proactive_result_t proactive_result;
        memset(&proactive_result, 0, sizeof(proactive_result));
        if (sc_proactive_check(agent->alloc, session_count, hour, &proactive_result) == SC_OK &&
            proactive_result.count > 0) {
            (void)sc_proactive_build_context(&proactive_result, agent->alloc, 8, &proactive_ctx,
                                             &proactive_ctx_len);
            sc_proactive_result_deinit(&proactive_result, agent->alloc);
        }
    }

    /* Build superhuman context (commitment, predictive, emotional, silence) */
    char *superhuman_ctx = NULL;
    size_t superhuman_ctx_len = 0;
    {
        agent->superhuman_commitment_ctx.session_id = agent->memory_session_id;
        agent->superhuman_commitment_ctx.session_id_len = agent->memory_session_id_len;
        (void)sc_superhuman_build_context(&agent->superhuman, agent->alloc, &superhuman_ctx,
                                           &superhuman_ctx_len);
    }

    /* Build adaptive persona context (circadian + relationship) */
    char *adaptive_ctx = NULL;
    size_t adaptive_ctx_len = 0;
#ifdef SC_HAS_PERSONA
    {
        uint8_t hour = 10;
#ifndef SC_IS_TEST
        {
            time_t now = time(NULL);
            struct tm *lt = localtime(&now);
            if (lt)
                hour = (uint8_t)(lt->tm_hour & 0xFF);
        }
#endif
        char *circadian_str = NULL;
        size_t circadian_len = 0;
        char *rel_str = NULL;
        size_t rel_len = 0;
        if (sc_circadian_build_prompt(agent->alloc, hour, &circadian_str, &circadian_len) ==
                SC_OK &&
            circadian_str) {
            if (sc_relationship_build_prompt(agent->alloc, &agent->relationship, &rel_str,
                                             &rel_len) == SC_OK &&
                rel_str) {
                size_t total = circadian_len + rel_len + 1;
                adaptive_ctx = (char *)agent->alloc->alloc(agent->alloc->ctx, total);
                if (adaptive_ctx) {
                    memcpy(adaptive_ctx, circadian_str, circadian_len);
                    memcpy(adaptive_ctx + circadian_len, rel_str, rel_len);
                    adaptive_ctx[circadian_len + rel_len] = '\0';
                    adaptive_ctx_len = circadian_len + rel_len;
                }
                agent->alloc->free(agent->alloc->ctx, rel_str, rel_len + 1);
            }
            if (adaptive_ctx) {
                agent->alloc->free(agent->alloc->ctx, circadian_str, circadian_len + 1);
            } else {
                adaptive_ctx = circadian_str;
                adaptive_ctx_len = circadian_len;
            }
        }
    }
#endif

    /* Build situational awareness context */
    char *awareness_ctx = NULL;
    size_t awareness_ctx_len = 0;
    if (agent->awareness)
        awareness_ctx = sc_awareness_context(agent->awareness, agent->alloc, &awareness_ctx_len);

    /* Build outcome tracking summary */
    char *outcome_ctx = NULL;
    size_t outcome_ctx_len = 0;
    if (agent->outcomes)
        outcome_ctx = sc_outcome_build_summary(agent->outcomes, agent->alloc, &outcome_ctx_len);

    /* Build persona prompt fresh each turn (channel-dependent; no caching) */
    char *persona_prompt = NULL;
    size_t persona_prompt_len = 0;
#ifdef SC_HAS_PERSONA
    if (agent->persona) {
        const char *ch = agent->active_channel;
        size_t ch_len = agent->active_channel_len;
        sc_error_t perr = sc_persona_build_prompt(agent->alloc, agent->persona, ch, ch_len,
                                                  &persona_prompt, &persona_prompt_len);
        if (perr != SC_OK) {
            if (pref_ctx)
                agent->alloc->free(agent->alloc->ctx, pref_ctx, pref_ctx_len + 1);
            if (memory_ctx)
                agent->alloc->free(agent->alloc->ctx, memory_ctx, memory_ctx_len + 1);
            if (awareness_ctx)
                agent->alloc->free(agent->alloc->ctx, awareness_ctx, awareness_ctx_len + 1);
            if (outcome_ctx)
                agent->alloc->free(agent->alloc->ctx, outcome_ctx, outcome_ctx_len + 1);
            sc_agent_clear_current_for_tools();
            return perr;
        }
    }
#endif

    /* Build system prompt using cached static portion when available */
    char *system_prompt = NULL;
    size_t system_prompt_len = 0;
    if (agent->cached_static_prompt && !pref_ctx && !tone_hint && !persona_prompt &&
        !awareness_ctx && !stm_ctx && !commitment_ctx && !pattern_ctx && !adaptive_ctx &&
        !proactive_ctx && !superhuman_ctx) {
        err = sc_prompt_build_with_cache(agent->alloc, agent->cached_static_prompt,
                                         agent->cached_static_prompt_len, memory_ctx,
                                         memory_ctx_len, &system_prompt, &system_prompt_len);
        if (memory_ctx)
            agent->alloc->free(agent->alloc->ctx, memory_ctx, memory_ctx_len + 1);
        if (stm_ctx) {
            agent->alloc->free(agent->alloc->ctx, stm_ctx, stm_ctx_len + 1);
            stm_ctx = NULL;
        }
        if (err != SC_OK) {
            if (pref_ctx)
                agent->alloc->free(agent->alloc->ctx, pref_ctx, pref_ctx_len + 1);
            if (commitment_ctx)
                agent->alloc->free(agent->alloc->ctx, commitment_ctx, commitment_ctx_len + 1);
            if (pattern_ctx)
                agent->alloc->free(agent->alloc->ctx, pattern_ctx, pattern_ctx_len + 1);
            if (adaptive_ctx)
                agent->alloc->free(agent->alloc->ctx, adaptive_ctx, adaptive_ctx_len + 1);
            if (proactive_ctx)
                agent->alloc->free(agent->alloc->ctx, proactive_ctx, proactive_ctx_len + 1);
            if (superhuman_ctx)
                agent->alloc->free(agent->alloc->ctx, superhuman_ctx, superhuman_ctx_len + 1);
            sc_agent_clear_current_for_tools();
            return err;
        }
    } else {
        sc_prompt_config_t cfg = {
            .provider_name = agent->provider.vtable->get_name(agent->provider.ctx),
            .provider_name_len = 0,
            .model_name = agent->model_name,
            .model_name_len = agent->model_name_len,
            .workspace_dir = agent->workspace_dir,
            .workspace_dir_len = agent->workspace_dir_len,
            .tools = agent->tools,
            .tools_count = agent->tools_count,
            .memory_context = memory_ctx,
            .memory_context_len = memory_ctx_len,
            .stm_context = stm_ctx,
            .stm_context_len = stm_ctx_len,
            .commitment_context = commitment_ctx,
            .commitment_context_len = commitment_ctx_len,
            .pattern_context = pattern_ctx,
            .pattern_context_len = pattern_ctx_len,
            .adaptive_persona_context = adaptive_ctx,
            .adaptive_persona_context_len = adaptive_ctx_len,
            .proactive_context = proactive_ctx,
            .proactive_context_len = proactive_ctx_len,
            .superhuman_context = superhuman_ctx,
            .superhuman_context_len = superhuman_ctx_len,
            .autonomy_level = agent->autonomy_level,
            .custom_instructions = agent->custom_instructions,
            .custom_instructions_len = agent->custom_instructions_len,
            .persona_prompt = persona_prompt,
            .persona_prompt_len = persona_prompt_len,
            .preferences = pref_ctx,
            .preferences_len = pref_ctx_len,
            .chain_of_thought = agent->chain_of_thought,
            .tone_hint = tone_hint,
            .tone_hint_len = tone_hint_len,
            .awareness_context = awareness_ctx,
            .awareness_context_len = awareness_ctx_len,
            .outcome_context = outcome_ctx,
            .outcome_context_len = outcome_ctx_len,
            .persona_immersive = (persona_prompt && persona_prompt_len > 0),
            .contact_context = agent->contact_context,
            .contact_context_len = agent->contact_context_len,
            .conversation_context = agent->conversation_context,
            .conversation_context_len = agent->conversation_context_len,
            .max_response_chars = agent->max_response_chars,
        };
        err = sc_prompt_build_system(agent->alloc, &cfg, &system_prompt, &system_prompt_len);
        if (persona_prompt)
            agent->alloc->free(agent->alloc->ctx, persona_prompt, persona_prompt_len + 1);
        persona_prompt = NULL;
        if (memory_ctx)
            agent->alloc->free(agent->alloc->ctx, memory_ctx, memory_ctx_len + 1);
        if (stm_ctx) {
            agent->alloc->free(agent->alloc->ctx, stm_ctx, stm_ctx_len + 1);
            stm_ctx = NULL;
        }
        if (awareness_ctx)
            agent->alloc->free(agent->alloc->ctx, awareness_ctx, awareness_ctx_len + 1);
        if (outcome_ctx)
            agent->alloc->free(agent->alloc->ctx, outcome_ctx, outcome_ctx_len + 1);
        if (err != SC_OK) {
            if (pref_ctx)
                agent->alloc->free(agent->alloc->ctx, pref_ctx, pref_ctx_len + 1);
            if (commitment_ctx)
                agent->alloc->free(agent->alloc->ctx, commitment_ctx, commitment_ctx_len + 1);
            if (pattern_ctx)
                agent->alloc->free(agent->alloc->ctx, pattern_ctx, pattern_ctx_len + 1);
            if (adaptive_ctx)
                agent->alloc->free(agent->alloc->ctx, adaptive_ctx, adaptive_ctx_len + 1);
            if (proactive_ctx)
                agent->alloc->free(agent->alloc->ctx, proactive_ctx, proactive_ctx_len + 1);
            if (superhuman_ctx)
                agent->alloc->free(agent->alloc->ctx, superhuman_ctx, superhuman_ctx_len + 1);
            sc_agent_clear_current_for_tools();
            return err;
        }
    }
    if (stm_ctx)
        agent->alloc->free(agent->alloc->ctx, stm_ctx, stm_ctx_len + 1);
    if (commitment_ctx)
        agent->alloc->free(agent->alloc->ctx, commitment_ctx, commitment_ctx_len + 1);
    if (pattern_ctx)
        agent->alloc->free(agent->alloc->ctx, pattern_ctx, pattern_ctx_len + 1);
    if (adaptive_ctx)
        agent->alloc->free(agent->alloc->ctx, adaptive_ctx, adaptive_ctx_len + 1);
    if (proactive_ctx)
        agent->alloc->free(agent->alloc->ctx, proactive_ctx, proactive_ctx_len + 1);
    if (superhuman_ctx)
        agent->alloc->free(agent->alloc->ctx, superhuman_ctx, superhuman_ctx_len + 1);
    if (pref_ctx)
        agent->alloc->free(agent->alloc->ctx, pref_ctx, pref_ctx_len + 1);

    /* Tool routing: if enabled, select relevant subset of tools for this message */
    sc_tool_selection_t tool_selection;
    memset(&tool_selection, 0, sizeof(tool_selection));
    if (agent->tool_routing_enabled && agent->tools_count > SC_TOOL_ROUTER_MAX_SELECTED) {
        sc_error_t rerr = sc_tool_router_select(agent->alloc, msg, msg_len, agent->tools,
                                               agent->tools_count, &tool_selection);
        if (rerr == SC_OK && tool_selection.count > 0) {
            /* Use only selected tools for this turn; tool_specs are pre-built, placeholder for now */
            (void)tool_selection.count;
        }
    }

    sc_chat_message_t *msgs = NULL;
    size_t msgs_count = 0;

    sc_chat_request_t req;
    memset(&req, 0, sizeof(req));
    req.model = agent->model_name;
    req.model_len = agent->model_name_len;
    req.temperature = agent->temperature;
    req.tools = (agent->tool_specs_count > 0) ? agent->tool_specs : NULL;
    req.tools_count = agent->tool_specs_count;

    uint32_t iter = 0;
    int reflection_retries_left = agent->reflection.max_retries;
    uint64_t max_tokens =
        agent->token_limit ? agent->token_limit
                           : sc_context_tokens_resolve(0, agent->model_name, agent->model_name_len);
    if (max_tokens == 0)
        max_tokens = 128000u;

    sc_compaction_config_t compact_cfg;
    sc_compaction_config_default(&compact_cfg);
    compact_cfg.max_history_messages = agent->max_history_messages;
    compact_cfg.token_limit = max_tokens;

    sc_agent_internal_generate_trace_id(agent->trace_id);
    clock_t turn_start = clock();
    uint64_t turn_tokens = 0;
    const char *prov_name = agent->provider.vtable->get_name
                                ? agent->provider.vtable->get_name(agent->provider.ctx)
                                : NULL;

    {
        sc_observer_event_t ev = {.tag = SC_OBSERVER_EVENT_AGENT_START, .data = {{0}}};
        ev.data.agent_start.provider = prov_name ? prov_name : "";
        ev.data.agent_start.model = agent->model_name ? agent->model_name : "";
        SC_OBS_SAFE_RECORD_EVENT(agent, &ev);
    }

    /* Per-turn arena: reset each iteration to reclaim ephemeral message arrays */
    sc_allocator_t turn_alloc =
        agent->turn_arena ? sc_arena_allocator(agent->turn_arena) : *agent->alloc;

    while (iter < agent->max_tool_iterations) {
        if (agent->cancel_requested) {
            if (system_prompt)
                agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
            if (agent->turn_arena)
                sc_arena_reset(agent->turn_arena);
            return SC_ERR_CANCELLED;
        }
        if (agent->turn_arena)
            sc_arena_reset(agent->turn_arena);
        msgs = NULL;
        msgs_count = 0;
        iter++;
        /* Compact history if it exceeds limits (before each provider call).
         * Uses LLM summarization when the provider is available, with
         * rule-based fallback. */
        if (sc_should_compact(agent->history, agent->history_count, &compact_cfg)) {
            sc_compact_history_llm(agent->alloc, agent->history, &agent->history_count,
                                   &agent->history_cap, &compact_cfg, &agent->provider);
        }

        /* Context pressure: estimate tokens, check thresholds, auto-compact if needed */
        {
            uint64_t current = sc_estimate_tokens(agent->history, agent->history_count) +
                               (uint64_t)((system_prompt_len + 3) / 4);
            sc_context_pressure_t pr = {
                .current_tokens = (size_t)current,
                .max_tokens = (size_t)max_tokens,
                .pressure = 0.0f,
                .warning_85_emitted = agent->context_pressure_warning_85_emitted,
                .warning_95_emitted = agent->context_pressure_warning_95_emitted,
            };
            if (sc_context_check_pressure(&pr, agent->context_pressure_warn,
                                          agent->context_pressure_compact)) {
                sc_context_compact_for_pressure(agent->alloc, agent->history, &agent->history_count,
                                                &agent->history_cap, (size_t)max_tokens,
                                                agent->context_compact_target);
                agent->context_pressure_warning_85_emitted = false;
                agent->context_pressure_warning_95_emitted = false;
            } else {
                agent->context_pressure_warning_85_emitted = pr.warning_85_emitted;
                agent->context_pressure_warning_95_emitted = pr.warning_95_emitted;
            }
        }

        /* Format messages for this iteration using arena allocator */
        {
            sc_chat_message_t *hist_msgs = NULL;
            size_t hist_count = 0;
            err = sc_context_format_messages(&turn_alloc, agent->history, agent->history_count,
                                             agent->max_history_messages, &hist_msgs, &hist_count);
            if (err != SC_OK) {
                sc_agent_clear_current_for_tools();
                if (system_prompt)
                    agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
                return err;
            }
            size_t total = (hist_msgs ? hist_count : 0) + 1;
            sc_chat_message_t *all = (sc_chat_message_t *)turn_alloc.alloc(
                turn_alloc.ctx, total * sizeof(sc_chat_message_t));
            if (!all) {
                sc_agent_clear_current_for_tools();
                if (system_prompt)
                    agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
                return SC_ERR_OUT_OF_MEMORY;
            }
            all[0].role = SC_ROLE_SYSTEM;
            all[0].content = system_prompt;
            all[0].content_len = system_prompt_len;
            all[0].name = NULL;
            all[0].name_len = 0;
            all[0].tool_call_id = NULL;
            all[0].tool_call_id_len = 0;
            all[0].content_parts = NULL;
            all[0].content_parts_count = 0;
            for (size_t i = 0; i < (hist_msgs ? hist_count : 0); i++)
                all[i + 1] = hist_msgs[i];
            msgs = all;
            msgs_count = total;
            req.messages = msgs;
            req.messages_count = msgs_count;
        }

        {
            sc_observer_event_t ev = {.tag = SC_OBSERVER_EVENT_LLM_REQUEST, .data = {{0}}};
            ev.data.llm_request.provider = prov_name ? prov_name : "";
            ev.data.llm_request.model = agent->model_name ? agent->model_name : "";
            ev.data.llm_request.messages_count = msgs_count;
            SC_OBS_SAFE_RECORD_EVENT(agent, &ev);
        }

        clock_t llm_start = clock();
        sc_chat_response_t resp;
        memset(&resp, 0, sizeof(resp));
        err =
            agent->provider.vtable->chat(agent->provider.ctx, agent->alloc, &req, agent->model_name,
                                         agent->model_name_len, agent->temperature, &resp);
        uint64_t llm_duration_ms = sc_agent_internal_clock_diff_ms(llm_start, clock());

        {
            sc_observer_event_t ev = {.tag = SC_OBSERVER_EVENT_LLM_RESPONSE, .data = {{0}}};
            ev.data.llm_response.provider = prov_name ? prov_name : "";
            ev.data.llm_response.model = agent->model_name ? agent->model_name : "";
            ev.data.llm_response.duration_ms = llm_duration_ms;
            ev.data.llm_response.success = (err == SC_OK);
            ev.data.llm_response.error_message = (err != SC_OK) ? "chat failed" : NULL;
            SC_OBS_SAFE_RECORD_EVENT(agent, &ev);
        }

        if (err != SC_OK) {
            {
                sc_observer_event_t ev = {.tag = SC_OBSERVER_EVENT_ERR, .data = {{0}}};
                ev.data.err.component = "agent";
                ev.data.err.message = "provider chat failed";
                SC_OBS_SAFE_RECORD_EVENT(agent, &ev);
            }
            sc_agent_clear_current_for_tools();
            if (system_prompt)
                agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
            if (agent->turn_arena)
                sc_arena_reset(agent->turn_arena);
            return err;
        }

        agent->total_tokens += resp.usage.total_tokens;
        sc_agent_internal_record_cost(agent, &resp.usage);
        turn_tokens += resp.usage.total_tokens;

        if (resp.tool_calls_count == 0) {
            uint64_t turn_duration_ms = sc_agent_internal_clock_diff_ms(turn_start, clock());
            {
                sc_observer_event_t ev = {.tag = SC_OBSERVER_EVENT_AGENT_END, .data = {{0}}};
                ev.data.agent_end.duration_ms = turn_duration_ms;
                ev.data.agent_end.tokens_used = turn_tokens;
                SC_OBS_SAFE_RECORD_EVENT(agent, &ev);
            }
            {
                sc_observer_event_t ev = {.tag = SC_OBSERVER_EVENT_TURN_COMPLETE, .data = {{0}}};
                SC_OBS_SAFE_RECORD_EVENT(agent, &ev);
            }
            if (resp.content && resp.content_len > 0) {
                /* Reflection: evaluate response quality and retry if needed */
                sc_reflection_quality_t quality = sc_reflection_evaluate(
                    msg, msg_len, resp.content, resp.content_len, &agent->reflection);

                if (quality == SC_QUALITY_ACCEPTABLE && agent->reflection.use_llm &&
                    agent->reflection.enabled && reflection_retries_left > 0) {
                    quality =
                        sc_reflection_evaluate_llm(agent->alloc, &agent->provider, msg, msg_len,
                                                   resp.content, resp.content_len, quality);
                }

                if (quality == SC_QUALITY_NEEDS_RETRY && agent->reflection.enabled &&
                    reflection_retries_left > 0 && iter < agent->max_tool_iterations - 1) {
                    reflection_retries_left--;
                    char *critique = NULL;
                    size_t critique_len = 0;
                    sc_error_t cerr = sc_reflection_build_critique_prompt(
                        agent->alloc, msg, msg_len, resp.content, resp.content_len, &critique,
                        &critique_len);
                    if (cerr == SC_OK && critique) {
                        (void)sc_agent_internal_append_history(agent, SC_ROLE_ASSISTANT,
                                                               resp.content, resp.content_len, NULL,
                                                               0, NULL, 0);
                        (void)sc_agent_internal_append_history(agent, SC_ROLE_USER, critique,
                                                               critique_len, NULL, 0, NULL, 0);
                        agent->alloc->free(agent->alloc->ctx, critique, critique_len + 1);
                        sc_chat_response_free(agent->alloc, &resp);
                        iter++;
                        continue; /* retry with critique feedback */
                    }
                    if (critique)
                        agent->alloc->free(agent->alloc->ctx, critique, critique_len + 1);
                }

                (void)sc_agent_internal_append_history(agent, SC_ROLE_ASSISTANT, resp.content,
                                                       resp.content_len, NULL, 0, NULL, 0);
                *response_out = sc_strndup(agent->alloc, resp.content, resp.content_len);
                if (!*response_out) {
                    sc_agent_clear_current_for_tools();
                    sc_chat_response_free(agent->alloc, &resp);
                    if (system_prompt)
                        agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
                    if (agent->turn_arena)
                        sc_arena_reset(agent->turn_arena);
                    return SC_ERR_OUT_OF_MEMORY;
                }
                if (response_len_out)
                    *response_len_out = resp.content_len;
                sc_agent_internal_maybe_tts(agent, resp.content, resp.content_len);
            }
            sc_chat_response_free(agent->alloc, &resp);
            sc_agent_clear_current_for_tools();
#ifdef SC_HAS_PERSONA
            sc_relationship_update(&agent->relationship, 1);
#endif
            /* Deep extraction: lightweight pattern-based fact extraction from user message */
            if (agent->memory && agent->memory->vtable && agent->memory->vtable->store) {
                sc_deep_extract_result_t de_result;
                memset(&de_result, 0, sizeof(de_result));
                if (sc_deep_extract_lightweight(agent->alloc, msg, msg_len, &de_result) == SC_OK &&
                    de_result.fact_count > 0) {
                    static const char facts_cat[] = "facts";
                    sc_memory_category_t cat = {
                        .tag = SC_MEMORY_CATEGORY_CUSTOM,
                        .data.custom = {.name = facts_cat, .name_len = sizeof(facts_cat) - 1},
                    };
                    const char *sid = agent->memory->current_session_id;
                    size_t sid_len = agent->memory->current_session_id_len;
                    for (size_t fi = 0; fi < de_result.fact_count; fi++) {
                        const sc_extracted_fact_t *f = &de_result.facts[fi];
                        if (!f->subject || !f->predicate || !f->object)
                            continue;
                        size_t key_len = strlen(f->subject) + 1 + strlen(f->predicate) + 1 +
                                        strlen(f->object);
                        char key_buf[256];
                        if (key_len < sizeof(key_buf)) {
                            int n = snprintf(key_buf, sizeof(key_buf), "%s:%s:%s", f->subject,
                                             f->predicate, f->object);
                            if (n > 0 && (size_t)n < sizeof(key_buf)) {
                                (void)agent->memory->vtable->store(
                                    agent->memory->ctx, key_buf, (size_t)n,
                                    f->object, strlen(f->object), &cat, sid ? sid : "", sid_len);
                            }
                        }
                    }
                }
                sc_deep_extract_result_deinit(&de_result, agent->alloc);
            }
            if (system_prompt)
                agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
            if (agent->turn_arena)
                sc_arena_reset(agent->turn_arena);
            return SC_OK;
        }

        err = sc_agent_internal_append_history_with_tool_calls(
            agent, resp.content ? resp.content : "", resp.content_len, resp.tool_calls,
            resp.tool_calls_count);
        if (err != SC_OK) {
            sc_agent_clear_current_for_tools();
            sc_chat_response_free(agent->alloc, &resp);
            if (system_prompt)
                agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
            if (agent->turn_arena)
                sc_arena_reset(agent->turn_arena);
            return err;
        }
        sc_chat_response_free(agent->alloc, &resp);

        {
            size_t tc_count = agent->history[agent->history_count - 1].tool_calls_count;
            const sc_tool_call_t *calls = agent->history[agent->history_count - 1].tool_calls;

            /* Emit TOOL_CALL_START events for all calls */
            for (size_t tc = 0; tc < tc_count; tc++) {
                char tn_buf[64];
                size_t tn = (calls[tc].name_len < sizeof(tn_buf) - 1) ? calls[tc].name_len
                                                                      : sizeof(tn_buf) - 1;
                if (tn > 0 && calls[tc].name)
                    memcpy(tn_buf, calls[tc].name, tn);
                tn_buf[tn] = '\0';
                sc_observer_event_t ev = {.tag = SC_OBSERVER_EVENT_TOOL_CALL_START, .data = {{0}}};
                ev.data.tool_call_start.tool = tn_buf[0] ? tn_buf : "unknown";
                SC_OBS_SAFE_RECORD_EVENT(agent, &ev);
            }

            /* LOCKED: skip all tool execution */
            if (agent->autonomy_level == SC_AUTONOMY_LOCKED) {
                for (size_t tc = 0; tc < tc_count; tc++) {
                    const sc_tool_call_t *call = &calls[tc];
                    (void)sc_agent_internal_append_history(
                        agent, SC_ROLE_TOOL, "Action blocked: agent is in locked mode", 38,
                        call->name, call->name_len, call->id, call->id_len);
                    if (agent->cancel_requested)
                        break;
                }
            } else {
                /* LLMCompiler: when enabled, compile tool calls into a DAG for parallel execution.
                 * Note: LLMCompiler is opt-in via config; the existing dispatcher handles parallelism. */
                /* Use dispatcher for parallel execution when enabled (Tier 1.3) */
                sc_dispatcher_t dispatcher;
                sc_dispatcher_default(&dispatcher);
                if (tc_count > 1)
                    dispatcher.max_parallel = 4;
                dispatcher.timeout_secs = 30;

                sc_dispatch_result_t dispatch_result;
                memset(&dispatch_result, 0, sizeof(dispatch_result));
                err = sc_dispatcher_dispatch(&dispatcher, agent->alloc, agent->tools,
                                             agent->tools_count, calls, tc_count, &dispatch_result);

                if (err == SC_OK && dispatch_result.results) {
                    for (size_t tc = 0; tc < tc_count; tc++) {
                        const sc_tool_call_t *call = &calls[tc];
                        sc_tool_result_t *result = &dispatch_result.results[tc];

                        char tn_buf[64];
                        size_t tn = (call->name_len < sizeof(tn_buf) - 1) ? call->name_len
                                                                          : sizeof(tn_buf) - 1;
                        if (tn > 0 && call->name)
                            memcpy(tn_buf, call->name, tn);
                        tn_buf[tn] = '\0';
                        const char *args_str = call->arguments ? call->arguments : "";

                        /* Policy evaluation (dispatcher path) */
                        sc_policy_action_t pa = sc_agent_internal_evaluate_tool_policy(
                            agent, tn_buf[0] ? tn_buf : "unknown", args_str);
                        if (pa == SC_POLICY_DENY) {
                            if (agent->audit_logger) {
                                sc_audit_event_t aev;
                                sc_audit_event_init(&aev, SC_AUDIT_POLICY_VIOLATION);
                                sc_audit_event_with_identity(
                                    &aev, agent->agent_id,
                                    agent->model_name ? agent->model_name : "unknown", NULL);
                                sc_audit_event_with_action(&aev, tn_buf[0] ? tn_buf : "unknown",
                                                           "denied", false, false);
                                sc_audit_logger_log(agent->audit_logger, &aev);
                            }
                            sc_tool_result_free(agent->alloc, result);
                            *result = sc_tool_result_fail("denied by policy", 16);
                        } else if (pa == SC_POLICY_REQUIRE_APPROVAL) {
                            result->needs_approval = true;
                        }

                        /* Autonomy: SUPERVISED forces approval; ASSISTED for medium/high risk */
                        if (agent->autonomy_level == SC_AUTONOMY_SUPERVISED) {
                            result->needs_approval = true;
                        } else if (agent->autonomy_level == SC_AUTONOMY_ASSISTED) {
                            if (sc_tool_risk_level(tn_buf[0] ? tn_buf : "unknown") >=
                                SC_RISK_MEDIUM)
                                result->needs_approval = true;
                        }

                        /* Feature 2: explicit failure when approval required but no callback */
                        if (result->needs_approval && !agent->approval_cb) {
                            sc_tool_result_free(agent->alloc, result);
                            *result = sc_tool_result_fail("requires human approval", 23);
                        }

                        /* Approval flow: if tool needs approval, ask user and retry */
                        if (result->needs_approval && agent->approval_cb) {
                            char tn_tmp[64];
                            size_t tn2 = (call->name_len < sizeof(tn_tmp) - 1) ? call->name_len
                                                                               : sizeof(tn_tmp) - 1;
                            if (tn2 > 0 && call->name)
                                memcpy(tn_tmp, call->name, tn2);
                            tn_tmp[tn2] = '\0';
                            bool user_approved =
                                agent->approval_cb(agent->approval_ctx, tn_tmp, args_str);
                            if (user_approved) {
                                sc_tool_result_free(agent->alloc, result);
                                if (agent->policy)
                                    agent->policy->pre_approved = true;
                                sc_tool_t *tool =
                                    sc_agent_internal_find_tool(agent, call->name, call->name_len);
                                if (tool) {
                                    sc_json_value_t *retry_args = NULL;
                                    if (call->arguments_len > 0)
                                        (void)sc_json_parse(agent->alloc, call->arguments,
                                                            call->arguments_len, &retry_args);
                                    *result = sc_tool_result_fail("invalid arguments", 16);
                                    if (retry_args) {
                                        if (tool->vtable->execute)
                                            tool->vtable->execute(tool->ctx, agent->alloc,
                                                                  retry_args, result);
                                        sc_json_free(agent->alloc, retry_args);
                                    }
                                }
                            } else {
                                sc_tool_result_free(agent->alloc, result);
                                *result = sc_tool_result_fail("user denied action", 18);
                            }
                        }

                        {
                            sc_observer_event_t ev = {.tag = SC_OBSERVER_EVENT_TOOL_CALL,
                                                      .data = {{0}}};
                            ev.data.tool_call.tool = tn_buf[0] ? tn_buf : "unknown";
                            ev.data.tool_call.duration_ms = 0;
                            ev.data.tool_call.success = result->success;
                            ev.data.tool_call.detail =
                                result->success
                                    ? NULL
                                    : (result->error_msg ? result->error_msg : "failed");
                            SC_OBS_SAFE_RECORD_EVENT(agent, &ev);
                        }

                        /* Outcome tracking */
                        if (agent->outcomes) {
                            const char *sum =
                                result->success
                                    ? (result->output ? result->output : "ok")
                                    : (result->error_msg ? result->error_msg : "failed");
                            sc_outcome_record_tool(agent->outcomes, tn_buf, result->success, sum);
                        }

                        const char *res_content =
                            result->success ? result->output : result->error_msg;
                        size_t res_len =
                            result->success ? result->output_len : result->error_msg_len;
                        (void)sc_agent_internal_append_history(agent, SC_ROLE_TOOL, res_content,
                                                               res_len, call->name, call->name_len,
                                                               call->id, call->id_len);

                        if (agent->audit_logger) {
                            sc_audit_event_t aev;
                            sc_audit_event_init(&aev, SC_AUDIT_COMMAND_EXECUTION);
                            sc_audit_event_with_identity(
                                &aev, agent->agent_id,
                                agent->model_name ? agent->model_name : "unknown", NULL);
                            sc_audit_event_with_action(&aev, tn_buf, "tool", result->success, true);
                            sc_audit_event_with_result(&aev, result->success, 0, 0,
                                                       result->success ? NULL : result->error_msg);
                            sc_audit_logger_log(agent->audit_logger, &aev);
                        }

                        if (agent->cancel_requested)
                            break;
                    }
                    sc_dispatch_result_free(agent->alloc, &dispatch_result);
                } else {
                    /* Fallback: sequential if dispatcher fails */
                    for (size_t tc = 0; tc < tc_count; tc++) {
                        const sc_tool_call_t *call = &calls[tc];
                        sc_tool_t *tool =
                            sc_agent_internal_find_tool(agent, call->name, call->name_len);
                        if (!tool) {
                            (void)sc_agent_internal_append_history(
                                agent, SC_ROLE_TOOL, "tool not found", 14, call->name,
                                call->name_len, call->id, call->id_len);
                            continue;
                        }
                        char pol_tn[64];
                        size_t pol_tn_len = call->name_len < sizeof(pol_tn) - 1
                                                ? call->name_len
                                                : sizeof(pol_tn) - 1;
                        if (pol_tn_len > 0 && call->name)
                            memcpy(pol_tn, call->name, pol_tn_len);
                        pol_tn[pol_tn_len] = '\0';

                        sc_policy_action_t pa = sc_agent_internal_evaluate_tool_policy(
                            agent, pol_tn, call->arguments ? call->arguments : "");
                        bool force_approval = (agent->autonomy_level == SC_AUTONOMY_SUPERVISED) ||
                                              (agent->autonomy_level == SC_AUTONOMY_ASSISTED &&
                                               sc_tool_risk_level(pol_tn) >= SC_RISK_MEDIUM);

                        sc_tool_result_t result = sc_tool_result_fail("invalid arguments", 16);
                        if (pa == SC_POLICY_DENY) {
                            if (agent->audit_logger) {
                                sc_audit_event_t aev;
                                sc_audit_event_init(&aev, SC_AUDIT_POLICY_VIOLATION);
                                sc_audit_event_with_identity(
                                    &aev, agent->agent_id,
                                    agent->model_name ? agent->model_name : "unknown", NULL);
                                sc_audit_event_with_action(&aev, pol_tn, "denied", false, false);
                                sc_audit_logger_log(agent->audit_logger, &aev);
                            }
                            result = sc_tool_result_fail("denied by policy", 16);
                        } else if (pa == SC_POLICY_REQUIRE_APPROVAL || force_approval) {
                            result = sc_tool_result_fail("pending approval", 16);
                            result.needs_approval = true;
                        } else {
                            sc_json_value_t *args = NULL;
                            if (call->arguments_len > 0) {
                                sc_error_t pe = sc_json_parse(agent->alloc, call->arguments,
                                                              call->arguments_len, &args);
                                if (pe == SC_OK && args) {
                                    tool->vtable->execute(tool->ctx, agent->alloc, args, &result);
                                    sc_json_free(agent->alloc, args);
                                }
                            }
                        }

                        /* Feature 2: explicit failure when approval required but no callback */
                        if (result.needs_approval && !agent->approval_cb) {
                            sc_tool_result_free(agent->alloc, &result);
                            result = sc_tool_result_fail("requires human approval", 23);
                        }

                        /* Approval retry for sequential fallback path */
                        if (result.needs_approval && agent->approval_cb) {
                            char seq_tn[64];
                            size_t seq_n = (call->name_len < sizeof(seq_tn) - 1)
                                               ? call->name_len
                                               : sizeof(seq_tn) - 1;
                            if (seq_n > 0 && call->name)
                                memcpy(seq_tn, call->name, seq_n);
                            seq_tn[seq_n] = '\0';
                            if (agent->approval_cb(agent->approval_ctx, seq_tn,
                                                   call->arguments ? call->arguments : "")) {
                                sc_tool_result_free(agent->alloc, &result);
                                if (agent->policy)
                                    agent->policy->pre_approved = true;
                                sc_json_value_t *retry_args = NULL;
                                if (call->arguments_len > 0)
                                    (void)sc_json_parse(agent->alloc, call->arguments,
                                                        call->arguments_len, &retry_args);
                                result = sc_tool_result_fail("invalid arguments", 16);
                                if (retry_args) {
                                    tool->vtable->execute(tool->ctx, agent->alloc, retry_args,
                                                          &result);
                                    sc_json_free(agent->alloc, retry_args);
                                }
                            } else {
                                sc_tool_result_free(agent->alloc, &result);
                                result = sc_tool_result_fail("user denied action", 18);
                            }
                        }

                        const char *res_content = result.success ? result.output : result.error_msg;
                        size_t res_len = result.success ? result.output_len : result.error_msg_len;
                        (void)sc_agent_internal_append_history(agent, SC_ROLE_TOOL, res_content,
                                                               res_len, call->name, call->name_len,
                                                               call->id, call->id_len);

                        if (agent->audit_logger) {
                            sc_audit_event_t aev;
                            sc_audit_event_init(&aev, SC_AUDIT_COMMAND_EXECUTION);
                            sc_audit_event_with_identity(
                                &aev, agent->agent_id,
                                agent->model_name ? agent->model_name : "unknown", NULL);
                            sc_audit_event_with_action(&aev, pol_tn, "tool", result.success, true);
                            sc_audit_event_with_result(&aev, result.success, 0, 0,
                                                       result.success ? NULL : result.error_msg);
                            sc_audit_logger_log(agent->audit_logger, &aev);
                        }

                        sc_tool_result_free(agent->alloc, &result);
                        if (agent->cancel_requested)
                            break;
                    }
                }
            }
        }
        /* Messages will be reformatted at top of next iteration via arena */
    }

    {
        sc_observer_event_t ev = {.tag = SC_OBSERVER_EVENT_TOOL_ITERATIONS_EXHAUSTED,
                                  .data = {{0}}};
        ev.data.tool_iterations_exhausted.iterations = agent->max_tool_iterations;
        SC_OBS_SAFE_RECORD_EVENT(agent, &ev);
    }
    {
        sc_observer_event_t ev = {.tag = SC_OBSERVER_EVENT_ERR, .data = {{0}}};
        ev.data.err.component = "agent";
        ev.data.err.message = "tool iterations exhausted";
        SC_OBS_SAFE_RECORD_EVENT(agent, &ev);
    }
    sc_agent_clear_current_for_tools();
    if (system_prompt)
        agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
    if (agent->turn_arena)
        sc_arena_reset(agent->turn_arena);
    return SC_ERR_TIMEOUT;
}
