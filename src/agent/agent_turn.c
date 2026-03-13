/* Core turn execution: hu_agent_turn and turn-local helpers */
#include "agent_internal.h"
#include "human/agent/ab_response.h"
#include "human/agent/awareness.h"
#include "human/agent/commands.h"
#include "human/agent/commitment.h"
#include "human/agent/commitment_store.h"
#include "human/agent/compaction.h"
#include "human/agent/dag.h"
#include "human/agent/dispatcher.h"
#include "human/agent/input_guard.h"
#include "human/agent/llm_compiler.h"
#include "human/agent/mailbox.h"
#include "human/agent/memory_loader.h"
#include "human/agent/outcomes.h"
#include "human/agent/pattern_radar.h"
#include "human/agent/planner.h"
#include "human/agent/preferences.h"
#include "human/agent/proactive.h"
#include "human/agent/prompt.h"
#include "human/agent/superhuman.h"
#include "human/agent/tool_router.h"
#include "human/observability/bth_metrics.h"
#ifdef HU_HAS_PERSONA
#include "human/persona/circadian.h"
#include "human/persona/relationship.h"
#endif
#include "human/agent/reflection.h"
#include "human/skillforge.h"
#include "human/context.h"
#include "human/context/conversation.h"
#include "human/context_tokens.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/memory/deep_extract.h"
#include "human/memory/fast_capture.h"
#include "human/memory/stm.h"
#include "human/memory/superhuman.h"
#ifdef HU_HAS_PERSONA
#include "human/persona.h"
#endif
#include "human/provider.h"
#include "human/voice.h"
#ifdef HU_ENABLE_SQLITE
#include "human/intelligence/self_improve.h"
#include "human/intelligence/online_learning.h"
#include "human/intelligence/value_learning.h"
#include "human/intelligence/world_model.h"
#include "human/agent/goals.h"
#endif
#include "human/agent/constitutional.h"
#include "human/agent/speculative.h"
#include "human/agent/tree_of_thought.h"
#include "human/agent/uncertainty.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

hu_error_t hu_agent_turn(hu_agent_t *agent, const char *msg, size_t msg_len, char **response_out,
                         size_t *response_len_out) {
    if (!agent || !msg || !response_out)
        return HU_ERR_INVALID_ARGUMENT;
    *response_out = NULL;
    if (response_len_out)
        *response_len_out = 0;

    hu_agent_set_current_for_tools(agent);

    /* Speculative cache: check for pre-computed response */
    if (agent->speculative_cache) {
        hu_speculative_config_t spec_cfg = hu_speculative_config_default();
        hu_prediction_t *hit = NULL;
        int64_t now = (int64_t)time(NULL);
        if (hu_speculative_cache_lookup(agent->speculative_cache, msg, msg_len,
                                         now, &spec_cfg, &hit) == HU_OK && hit) {
            *response_out = hu_strndup(agent->alloc, hit->response, hit->response_len);
            if (*response_out) {
                if (response_len_out)
                    *response_len_out = hit->response_len;
                hu_agent_clear_current_for_tools();
                return HU_OK;
            }
        }
    }

    hu_agent_internal_process_mailbox_messages(agent);

    char *slash_resp = hu_agent_handle_slash_command(agent, msg, msg_len);
    if (slash_resp) {
        hu_agent_clear_current_for_tools();
        *response_out = slash_resp;
        if (response_len_out)
            *response_len_out = strlen(slash_resp);
        return HU_OK;
    }

    /* Prompt injection defense-in-depth */
    {
        hu_injection_risk_t risk = HU_INJECTION_SAFE;
        hu_error_t guard_err = hu_input_guard_check(msg, msg_len, &risk);
        if (guard_err != HU_OK) {
            hu_agent_clear_current_for_tools();
            return guard_err;
        }
        if (risk == HU_INJECTION_HIGH_RISK && agent->observer) {
            hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_ERR};
            ev.data.err.component = "input_guard";
            ev.data.err.message = "high-risk injection pattern detected";
            hu_observer_record_event(*agent->observer, &ev);
        }
    }

    hu_error_t err =
        hu_agent_internal_append_history(agent, HU_ROLE_USER, msg, msg_len, NULL, 0, NULL, 0);
    if (err != HU_OK) {
        hu_agent_clear_current_for_tools();
        return err;
    }

    /* Superhuman: observe user message (emotional, silence services) */
    (void)hu_superhuman_observe_all(&agent->superhuman, agent->alloc, msg, msg_len, "user", 4);

    /* Fast-capture and STM: extract entities/emotions, record turn, populate last turn */
    {
        hu_fc_result_t fc_result;
        memset(&fc_result, 0, sizeof(fc_result));
        (void)hu_fast_capture(agent->alloc, msg, msg_len, &fc_result);

        uint64_t ts_ms = (uint64_t)time(NULL) * 1000;
        err = hu_stm_record_turn(&agent->stm, "user", 4, msg, msg_len, ts_ms);
        if (err == HU_OK) {
            size_t last_idx = hu_stm_count(&agent->stm) - 1;
            if (fc_result.primary_topic && fc_result.primary_topic[0]) {
                (void)hu_stm_turn_set_primary_topic(&agent->stm, last_idx, fc_result.primary_topic,
                                                    strlen(fc_result.primary_topic));
            }
            for (size_t i = 0; i < fc_result.entity_count; i++) {
                const hu_fc_entity_match_t *e = &fc_result.entities[i];
                uint32_t mention = 1;
                (void)hu_stm_turn_add_entity(&agent->stm, last_idx, e->name, e->name_len,
                                             e->type ? e->type : "entity",
                                             e->type ? e->type_len : 6, mention);
            }
            for (size_t i = 0; i < fc_result.emotion_count; i++) {
                (void)hu_stm_turn_add_emotion(&agent->stm, last_idx, fc_result.emotions[i].tag,
                                              fc_result.emotions[i].intensity);
            }
        }

        /* Pattern radar: observe entities as topic recurrence, emotions as emotional trend */
        {
            char ts_buf[32];
            int ts_n = snprintf(ts_buf, sizeof(ts_buf), "%llu", (unsigned long long)(ts_ms / 1000));
            const char *ts = ts_n > 0 ? ts_buf : NULL;
            size_t ts_len = (ts_n > 0 && ts_n < (int)sizeof(ts_buf)) ? (size_t)ts_n : 0;

            for (size_t i = 0; i < fc_result.entity_count; i++) {
                const hu_fc_entity_match_t *e = &fc_result.entities[i];
                if (e->name && e->name_len > 0) {
                    (void)hu_pattern_radar_observe(
                        &agent->radar, e->name, e->name_len, HU_PATTERN_TOPIC_RECURRENCE,
                        e->type ? e->type : NULL, e->type ? e->type_len : 0, ts, ts_len);
                }
            }
            static const char *emotion_names[] = {"neutral",     "joy",        "sadness",
                                                  "anger",       "fear",       "surprise",
                                                  "frustration", "excitement", "anxiety"};
            for (size_t i = 0; i < fc_result.emotion_count; i++) {
                hu_emotion_tag_t tag = fc_result.emotions[i].tag;
                if (tag >= 0 && (size_t)tag < sizeof(emotion_names) / sizeof(emotion_names[0])) {
                    const char *name = emotion_names[tag];
                    (void)hu_pattern_radar_observe(&agent->radar, name, strlen(name),
                                                   HU_PATTERN_EMOTIONAL_TREND, NULL, 0, ts, ts_len);
                }
            }
        }
        hu_fc_result_deinit(&fc_result, agent->alloc);
    }

    /* Commitment detection: extract promises, intentions, reminders, goals from user message */
    if (agent->commitment_store) {
        hu_commitment_detect_result_t commit_result;
        memset(&commit_result, 0, sizeof(commit_result));
        hu_error_t cerr =
            hu_commitment_detect(agent->alloc, msg, msg_len, "user", 4, &commit_result);
        if (cerr == HU_OK && commit_result.count > 0) {
            const char *sess = agent->memory_session_id;
            size_t sess_len = agent->memory_session_id ? agent->memory_session_id_len : 0;
            for (size_t i = 0; i < commit_result.count; i++) {
                (void)hu_commitment_store_save(agent->commitment_store,
                                               &commit_result.commitments[i], sess, sess_len);
            }
        }
        hu_commitment_detect_result_deinit(&commit_result, agent->alloc);
    }

    /* Detect preferences from user corrections and store them */
    bool is_correction = hu_preferences_is_correction(msg, msg_len);
    if (agent->memory && is_correction) {
        size_t pref_len = 0;
        char *pref = hu_preferences_extract(agent->alloc, msg, msg_len, &pref_len);
        if (pref) {
            hu_preferences_store(agent->memory, agent->alloc, pref, pref_len);
            agent->alloc->free(agent->alloc->ctx, pref, pref_len + 1);
        }
    }

    /* Outcome tracking: record corrections and positive feedback */
    if (agent->outcomes) {
        if (is_correction) {
            const char *prev_response = NULL;
            if (agent->history_count >= 2 &&
                agent->history[agent->history_count - 2].role == HU_ROLE_ASSISTANT)
                prev_response = agent->history[agent->history_count - 2].content;
            hu_outcome_record_correction(agent->outcomes, prev_response, msg);

#ifdef HU_HAS_PERSONA
            if (agent->outcomes->auto_apply_feedback && agent->persona && agent->persona_name &&
                prev_response) {
                hu_persona_feedback_t fb = {
                    .channel = agent->active_channel,
                    .channel_len = agent->active_channel_len,
                    .original_response = prev_response,
                    .original_response_len = strlen(prev_response),
                    .corrected_response = msg,
                    .corrected_response_len = msg_len,
                };
                (void)hu_persona_feedback_record(agent->alloc, agent->persona_name,
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
                hu_outcome_record_positive(agent->outcomes, msg);
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
            if (agent->history[i - 1].role == HU_ROLE_USER && agent->history[i - 1].content) {
                recent_msgs[rm_count] = agent->history[i - 1].content;
                recent_lens[rm_count] = agent->history[i - 1].content_len;
                rm_count++;
            }
        }
        if (rm_count > 0) {
            hu_tone_t tone = hu_detect_tone(recent_msgs, recent_lens, rm_count);
            tone_hint = hu_tone_hint_string(tone, &tone_hint_len);
        }
    }

    /* Load user preferences for prompt injection */
    char *pref_ctx = NULL;
    size_t pref_ctx_len = 0;
    if (agent->memory)
        (void)hu_preferences_load(agent->memory, agent->alloc, &pref_ctx, &pref_ctx_len);

    /* Load memory context for this turn */
    char *memory_ctx = NULL;
    size_t memory_ctx_len = 0;
    if (agent->memory && agent->memory->vtable) {
        hu_memory_loader_t loader;
        hu_memory_loader_init(&loader, agent->alloc, agent->memory, agent->retrieval_engine, 10,
                              4000);
        (void)hu_memory_loader_load(&loader, msg, msg_len,
                                    agent->memory_session_id ? agent->memory_session_id : "",
                                    agent->memory_session_id ? agent->memory_session_id_len : 0,
                                    &memory_ctx, &memory_ctx_len);
    }

    /* Build STM context for this turn */
    char *stm_ctx = NULL;
    size_t stm_ctx_len = 0;
    (void)hu_stm_build_context(&agent->stm, agent->alloc, &stm_ctx, &stm_ctx_len);
    if (stm_ctx_len > 0 && agent->bth_metrics)
        agent->bth_metrics->emotions_surfaced++;

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

    /* Build proactive context (milestones, morning briefing, check-in) */
    char *proactive_ctx = NULL;
    size_t proactive_ctx_len = 0;
    {
        uint32_t session_count = 0;
        uint8_t hour = 10;
#ifdef HU_HAS_PERSONA
        session_count = agent->relationship.session_count;
#endif
#ifndef HU_IS_TEST
        {
            time_t now = time(NULL);
            struct tm lt_buf;
            struct tm *lt = localtime_r(&now, &lt_buf);
            if (lt)
                hour = (uint8_t)(lt->tm_hour & 0xFF);
        }
#endif
        hu_proactive_result_t proactive_result;
        memset(&proactive_result, 0, sizeof(proactive_result));
        hu_commitment_t *commitments = NULL;
        size_t commitment_count = 0;
        if (agent->commitment_store && agent->memory_session_id &&
            agent->memory_session_id_len > 0) {
            (void)hu_commitment_store_list_active(
                agent->commitment_store, agent->alloc, agent->memory_session_id,
                agent->memory_session_id_len, &commitments, &commitment_count);
        }
        hu_error_t proactive_err =
            hu_proactive_check_extended(agent->alloc, session_count, hour, commitments,
                                        commitment_count, NULL, NULL, 0, &proactive_result);
        if (commitments) {
            for (size_t ci = 0; ci < commitment_count; ci++)
                hu_commitment_deinit(&commitments[ci], agent->alloc);
            agent->alloc->free(agent->alloc->ctx, commitments,
                               commitment_count * sizeof(hu_commitment_t));
        }
        if (proactive_err == HU_OK && proactive_result.count > 0) {
            (void)hu_proactive_build_context(&proactive_result, agent->alloc, 8, &proactive_ctx,
                                             &proactive_ctx_len);
            hu_proactive_result_deinit(&proactive_result, agent->alloc);
        }
        /* Merge contextual conversation starter from memory when we have a contact */
        if (agent->memory && agent->memory_session_id && agent->memory_session_id_len > 0) {
            char *starter = NULL;
            size_t starter_len = 0;
            if (hu_proactive_build_starter(agent->alloc, agent->memory, agent->memory_session_id,
                                           agent->memory_session_id_len, &starter,
                                           &starter_len) == HU_OK &&
                starter && starter_len > 0) {
                if (proactive_ctx && proactive_ctx_len > 0) {
                    size_t merged_len = proactive_ctx_len + 2 + starter_len;
                    char *merged = (char *)agent->alloc->alloc(agent->alloc->ctx, merged_len + 1);
                    if (merged) {
                        memcpy(merged, proactive_ctx, proactive_ctx_len);
                        merged[proactive_ctx_len] = '\n';
                        merged[proactive_ctx_len + 1] = '\n';
                        memcpy(merged + proactive_ctx_len + 2, starter, starter_len);
                        merged[merged_len] = '\0';
                        agent->alloc->free(agent->alloc->ctx, proactive_ctx, proactive_ctx_len + 1);
                        agent->alloc->free(agent->alloc->ctx, starter, starter_len + 1);
                        proactive_ctx = merged;
                        proactive_ctx_len = merged_len;
                    } else {
                        agent->alloc->free(agent->alloc->ctx, starter, starter_len + 1);
                    }
                } else {
                    proactive_ctx = starter;
                    proactive_ctx_len = starter_len;
                }
                if (agent->bth_metrics)
                    agent->bth_metrics->starters_built++;
            }
        }
    }

    /* Build superhuman context (commitment, predictive, emotional, silence) */
    char *superhuman_ctx = NULL;
    size_t superhuman_ctx_len = 0;
    {
        agent->superhuman_commitment_ctx.session_id = agent->memory_session_id;
        agent->superhuman_commitment_ctx.session_id_len = agent->memory_session_id_len;
        (void)hu_superhuman_build_context(&agent->superhuman, agent->alloc, &superhuman_ctx,
                                          &superhuman_ctx_len);
#ifdef HU_ENABLE_SQLITE
        /* Superhuman memory: micro-moments, inside jokes, avoidance, topic absences, growth, patterns */
        if (agent->memory && agent->memory_session_id && agent->memory_session_id_len > 0) {
            bool include_avoidance = false;
#ifdef HU_HAS_PERSONA
            include_avoidance =
                (agent->relationship.stage == HU_REL_TRUSTED ||
                 agent->relationship.stage == HU_REL_DEEP);
#endif
            char *memory_sh_ctx = NULL;
            size_t memory_sh_len = 0;
            if (hu_superhuman_memory_build_context(agent->memory, agent->alloc,
                    agent->memory_session_id, agent->memory_session_id_len, include_avoidance,
                    &memory_sh_ctx, &memory_sh_len) == HU_OK &&
                memory_sh_ctx && memory_sh_len > 0) {
                if (superhuman_ctx && superhuman_ctx_len > 0) {
                    size_t merged_len = superhuman_ctx_len + 2 + strlen(memory_sh_ctx) + 1;
                    char *merged = (char *)agent->alloc->alloc(agent->alloc->ctx, merged_len);
                    if (merged) {
                        memcpy(merged, superhuman_ctx, superhuman_ctx_len);
                        merged[superhuman_ctx_len] = '\n';
                        merged[superhuman_ctx_len + 1] = '\n';
                        memcpy(merged + superhuman_ctx_len + 2, memory_sh_ctx,
                            strlen(memory_sh_ctx) + 1);
                        agent->alloc->free(agent->alloc->ctx, superhuman_ctx,
                            superhuman_ctx_len);
                        agent->alloc->free(agent->alloc->ctx, memory_sh_ctx, memory_sh_len);
                        superhuman_ctx = merged;
                        superhuman_ctx_len = merged_len;
                    } else {
                        agent->alloc->free(agent->alloc->ctx, memory_sh_ctx, memory_sh_len);
                    }
                } else {
                    superhuman_ctx = memory_sh_ctx;
                    superhuman_ctx_len = memory_sh_len;
                }
            } else if (memory_sh_ctx) {
                agent->alloc->free(agent->alloc->ctx, memory_sh_ctx, memory_sh_len);
            }
        }
        /* F26: If they're quiet and it's during their usual quiet hours, inject hint */
        if (agent->memory && agent->memory_session_id && agent->memory_session_id_len > 0 &&
            superhuman_ctx) {
            int qday = 0, qstart = 0, qend = 1;
            if (hu_superhuman_temporal_get_quiet_hours(agent->memory, agent->alloc,
                    agent->memory_session_id, agent->memory_session_id_len,
                    &qday, &qstart, &qend) == HU_OK) {
                time_t now_t = time(NULL);
                struct tm lt_buf;
                struct tm *lt = localtime_r(&now_t, &lt_buf);
                if (lt && lt->tm_wday == qday && lt->tm_hour >= qstart && lt->tm_hour < qend) {
                    static const char hint[] =
                        "\nThey're often quiet at this time. Don't worry if no reply.";
                    size_t hint_len = sizeof(hint) - 1;
                    size_t ctx_str_len = strlen(superhuman_ctx);
                    size_t new_len = ctx_str_len + hint_len + 1;
                    char *merged = (char *)agent->alloc->alloc(agent->alloc->ctx, new_len);
                    if (merged) {
                        memcpy(merged, superhuman_ctx, ctx_str_len);
                        memcpy(merged + ctx_str_len, hint, hint_len + 1);
                        agent->alloc->free(agent->alloc->ctx, superhuman_ctx,
                            superhuman_ctx_len);
                        superhuman_ctx = merged;
                        superhuman_ctx_len = new_len;
                    }
                }
            }
        }
#endif
    }

    /* Build adaptive persona context (circadian + relationship) */
    char *adaptive_ctx = NULL;
    size_t adaptive_ctx_len = 0;
#ifdef HU_HAS_PERSONA
    {
        uint8_t hour = 10;
#ifndef HU_IS_TEST
        {
            time_t now = time(NULL);
            struct tm lt_buf;
            struct tm *lt = localtime_r(&now, &lt_buf);
            if (lt)
                hour = (uint8_t)(lt->tm_hour & 0xFF);
        }
#endif
        char *circadian_str = NULL;
        size_t circadian_len = 0;
        char *rel_str = NULL;
        size_t rel_len = 0;
        if (hu_circadian_build_prompt(agent->alloc, hour, &circadian_str, &circadian_len) ==
                HU_OK &&
            circadian_str) {
            if (hu_relationship_build_prompt(agent->alloc, &agent->relationship, &rel_str,
                                             &rel_len) == HU_OK &&
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
        awareness_ctx = hu_awareness_context(agent->awareness, agent->alloc, &awareness_ctx_len);

    /* Build outcome tracking summary */
    char *outcome_ctx = NULL;
    size_t outcome_ctx_len = 0;
    if (agent->outcomes)
        outcome_ctx = hu_outcome_build_summary(agent->outcomes, agent->alloc, &outcome_ctx_len);

    /* Build AGI frontier intelligence context */
    char *intelligence_ctx = NULL;
    size_t intelligence_ctx_len = 0;
#ifdef HU_ENABLE_SQLITE
    if (agent->memory) {
        sqlite3 *intel_db = hu_sqlite_memory_get_db(agent->memory);
        if (intel_db) {
            char parts[4096];
            size_t pos = 0;

            /* Self-improvement: active prompt patches */
            {
                hu_self_improve_t si;
                if (hu_self_improve_create(agent->alloc, intel_db, &si) == HU_OK) {
                    char *patches = NULL;
                    size_t patches_len = 0;
                    if (hu_self_improve_get_prompt_patches(&si, &patches, &patches_len) == HU_OK &&
                        patches && patches_len > 0) {
                        int n = snprintf(parts + pos, sizeof(parts) - pos,
                                         "### Learned Behaviors\n%.*s\n",
                                         (int)patches_len, patches);
                        if (n > 0 && pos + (size_t)n < sizeof(parts))
                            pos += (size_t)n;
                        agent->alloc->free(agent->alloc->ctx, patches, patches_len + 1);
                    }
                    hu_self_improve_deinit(&si);
                }
            }

            /* Goals: active goals context */
            {
                hu_goal_engine_t ge;
                if (hu_goal_engine_create(agent->alloc, intel_db, &ge) == HU_OK) {
                    char *gctx = NULL;
                    size_t gctx_len = 0;
                    if (hu_goal_build_context(&ge, &gctx, &gctx_len) == HU_OK &&
                        gctx && gctx_len > 0) {
                        int n = snprintf(parts + pos, sizeof(parts) - pos,
                                         "### %.*s\n", (int)gctx_len, gctx);
                        if (n > 0 && pos + (size_t)n < sizeof(parts))
                            pos += (size_t)n;
                        agent->alloc->free(agent->alloc->ctx, gctx, gctx_len + 1);
                    }
                    hu_goal_engine_deinit(&ge);
                }
            }

            /* Online learning: strategy preferences */
            {
                hu_online_learning_t ol;
                if (hu_online_learning_create(agent->alloc, intel_db, 0.1, &ol) == HU_OK) {
                    char *lctx = NULL;
                    size_t lctx_len = 0;
                    if (hu_online_learning_build_context(&ol, &lctx, &lctx_len) == HU_OK &&
                        lctx && lctx_len > 0) {
                        int n = snprintf(parts + pos, sizeof(parts) - pos,
                                         "### %.*s\n", (int)lctx_len, lctx);
                        if (n > 0 && pos + (size_t)n < sizeof(parts))
                            pos += (size_t)n;
                        agent->alloc->free(agent->alloc->ctx, lctx, lctx_len + 1);
                    }
                    hu_online_learning_deinit(&ol);
                }
            }

            /* Value learning: user values */
            {
                hu_value_engine_t ve;
                if (hu_value_engine_create(agent->alloc, intel_db, &ve) == HU_OK) {
                    char *vctx = NULL;
                    size_t vctx_len = 0;
                    if (hu_value_build_prompt(&ve, &vctx, &vctx_len) == HU_OK &&
                        vctx && vctx_len > 0) {
                        int n = snprintf(parts + pos, sizeof(parts) - pos,
                                         "### %.*s\n", (int)vctx_len, vctx);
                        if (n > 0 && pos + (size_t)n < sizeof(parts))
                            pos += (size_t)n;
                        agent->alloc->free(agent->alloc->ctx, vctx, vctx_len + 1);
                    }
                    hu_value_engine_deinit(&ve);
                }
            }

            if (pos > 0) {
                intelligence_ctx = hu_strndup(agent->alloc, parts, pos);
                intelligence_ctx_len = pos;
            }
        }
    }
#endif

    /* Build persona prompt fresh each turn (channel-dependent; no caching) */
    char *persona_prompt = NULL;
    size_t persona_prompt_len = 0;
#ifdef HU_HAS_PERSONA
    if (agent->persona) {
        const char *ch = agent->active_channel;
        size_t ch_len = agent->active_channel_len;
        hu_error_t perr = hu_persona_build_prompt(agent->alloc, agent->persona, ch, ch_len, msg,
                                                  msg_len, &persona_prompt, &persona_prompt_len);
        if (perr != HU_OK) {
            if (pref_ctx)
                agent->alloc->free(agent->alloc->ctx, pref_ctx, pref_ctx_len + 1);
            if (memory_ctx)
                agent->alloc->free(agent->alloc->ctx, memory_ctx, memory_ctx_len + 1);
            if (stm_ctx)
                agent->alloc->free(agent->alloc->ctx, stm_ctx, stm_ctx_len + 1);
            if (commitment_ctx)
                agent->alloc->free(agent->alloc->ctx, commitment_ctx, commitment_ctx_len + 1);
            if (pattern_ctx)
                agent->alloc->free(agent->alloc->ctx, pattern_ctx, pattern_ctx_len + 1);
            if (proactive_ctx)
                agent->alloc->free(agent->alloc->ctx, proactive_ctx, proactive_ctx_len + 1);
            if (superhuman_ctx)
                agent->alloc->free(agent->alloc->ctx, superhuman_ctx, superhuman_ctx_len);
            if (adaptive_ctx)
                agent->alloc->free(agent->alloc->ctx, adaptive_ctx, adaptive_ctx_len + 1);
            if (awareness_ctx)
                agent->alloc->free(agent->alloc->ctx, awareness_ctx, awareness_ctx_len + 1);
            if (outcome_ctx)
                agent->alloc->free(agent->alloc->ctx, outcome_ctx, outcome_ctx_len + 1);
            if (intelligence_ctx)
                agent->alloc->free(agent->alloc->ctx, intelligence_ctx, intelligence_ctx_len + 1);
            hu_agent_clear_current_for_tools();
            return perr;
        }
    }
#endif

    /* Build skills context from skillforge if available */
    char *skills_ctx = NULL;
    size_t skills_ctx_len = 0;
#ifdef HU_HAS_SKILLS
    if (agent->skillforge) {
        hu_skillforge_t *sf = (hu_skillforge_t *)agent->skillforge;
        hu_skill_t *all_skills = NULL;
        size_t all_count = 0;
        if (hu_skillforge_list_skills(sf, &all_skills, &all_count) == HU_OK && all_count > 0) {
            size_t total = 0;
            for (size_t i = 0; i < all_count; i++) {
                if (!all_skills[i].enabled)
                    continue;
                total += 4 + (all_skills[i].name ? strlen(all_skills[i].name) : 0) + 3 +
                         (all_skills[i].description ? strlen(all_skills[i].description) : 0) + 1;
            }
            if (total > 0) {
                skills_ctx = (char *)agent->alloc->alloc(agent->alloc->ctx, total + 1);
                if (skills_ctx) {
                    size_t pos = 0;
                    for (size_t i = 0; i < all_count; i++) {
                        if (!all_skills[i].enabled)
                            continue;
                        int n = snprintf(skills_ctx + pos, total + 1 - pos, "- %s: %s\n",
                                         all_skills[i].name ? all_skills[i].name : "",
                                         all_skills[i].description ? all_skills[i].description : "");
                        if (n > 0)
                            pos += (size_t)n;
                    }
                    skills_ctx[pos] = '\0';
                    skills_ctx_len = pos;
                }
            }
        }
    }
#endif /* HU_HAS_SKILLS */

    /* Build system prompt using cached static portion when available */
    char *system_prompt = NULL;
    size_t system_prompt_len = 0;
    if (agent->cached_static_prompt && !pref_ctx && !tone_hint && !persona_prompt &&
        !awareness_ctx && !stm_ctx && !commitment_ctx && !pattern_ctx && !adaptive_ctx &&
        !proactive_ctx && !superhuman_ctx && !skills_ctx) {
        err = hu_prompt_build_with_cache(agent->alloc, agent->cached_static_prompt,
                                         agent->cached_static_prompt_len, memory_ctx,
                                         memory_ctx_len, &system_prompt, &system_prompt_len);
        if (memory_ctx)
            agent->alloc->free(agent->alloc->ctx, memory_ctx, memory_ctx_len + 1);
        if (stm_ctx) {
            agent->alloc->free(agent->alloc->ctx, stm_ctx, stm_ctx_len + 1);
            stm_ctx = NULL;
        }
        if (err != HU_OK) {
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
                agent->alloc->free(agent->alloc->ctx, superhuman_ctx, superhuman_ctx_len);
            if (outcome_ctx)
                agent->alloc->free(agent->alloc->ctx, outcome_ctx, outcome_ctx_len + 1);
            if (intelligence_ctx)
                agent->alloc->free(agent->alloc->ctx, intelligence_ctx, intelligence_ctx_len + 1);
            hu_agent_clear_current_for_tools();
            return err;
        }
    } else {
        hu_prompt_config_t cfg = {
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
            .persona =
#ifdef HU_HAS_PERSONA
                agent->persona
#else
                NULL
#endif
            ,
            .contact_context = agent->contact_context,
            .contact_context_len = agent->contact_context_len,
            .conversation_context = agent->conversation_context,
            .conversation_context_len = agent->conversation_context_len,
            .max_response_chars = agent->max_response_chars,
            .intelligence_context = intelligence_ctx,
            .intelligence_context_len = intelligence_ctx_len,
            .skills_context = skills_ctx,
            .skills_context_len = skills_ctx_len,
        };
        err = hu_prompt_build_system(agent->alloc, &cfg, &system_prompt, &system_prompt_len);
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
        if (intelligence_ctx)
            agent->alloc->free(agent->alloc->ctx, intelligence_ctx, intelligence_ctx_len + 1);
        if (skills_ctx)
            agent->alloc->free(agent->alloc->ctx, skills_ctx, skills_ctx_len + 1);
        skills_ctx = NULL;
        if (err != HU_OK) {
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
                agent->alloc->free(agent->alloc->ctx, superhuman_ctx, superhuman_ctx_len);
            hu_agent_clear_current_for_tools();
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
        agent->alloc->free(agent->alloc->ctx, superhuman_ctx, superhuman_ctx_len);
    if (pref_ctx)
        agent->alloc->free(agent->alloc->ctx, pref_ctx, pref_ctx_len + 1);

    /* Tool routing: if enabled, select relevant subset of tools for this message */
    hu_tool_selection_t tool_selection;
    memset(&tool_selection, 0, sizeof(tool_selection));
    if (agent->tool_routing_enabled && agent->tools_count > HU_TOOL_ROUTER_MAX_SELECTED) {
        hu_error_t rerr = hu_tool_router_select(agent->alloc, msg, msg_len, agent->tools,
                                                agent->tools_count, &tool_selection);
        if (rerr == HU_OK && tool_selection.count > 0) {
            /* Use only selected tools for this turn; tool_specs are pre-built, placeholder for now
             */
            (void)tool_selection.count;
        }
    }

    hu_chat_message_t *msgs = NULL;
    size_t msgs_count = 0;

    hu_chat_request_t req;
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
                           : hu_context_tokens_resolve(0, agent->model_name, agent->model_name_len);
    if (max_tokens == 0)
        max_tokens = 128000u;

    hu_compaction_config_t compact_cfg;
    hu_compaction_config_default(&compact_cfg);
    compact_cfg.max_history_messages = agent->max_history_messages;
    compact_cfg.token_limit = max_tokens;

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

    /* Per-turn arena: reset each iteration to reclaim ephemeral message arrays */
    hu_allocator_t turn_alloc =
        agent->turn_arena ? hu_arena_allocator(agent->turn_arena) : *agent->alloc;

    while (iter < agent->max_tool_iterations) {
        if (agent->cancel_requested) {
            if (system_prompt)
                agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
            if (agent->turn_arena)
                hu_arena_reset(agent->turn_arena);
            return HU_ERR_CANCELLED;
        }
        if (agent->turn_arena)
            hu_arena_reset(agent->turn_arena);
        msgs = NULL;
        msgs_count = 0;
        iter++;
        /* Compact history if it exceeds limits (before each provider call).
         * Uses LLM summarization when the provider is available, with
         * rule-based fallback. */
        if (hu_should_compact(agent->history, agent->history_count, &compact_cfg)) {
            hu_compact_history_llm(agent->alloc, agent->history, &agent->history_count,
                                   &agent->history_cap, &compact_cfg, &agent->provider);
        }

        /* Context pressure: estimate tokens, check thresholds, auto-compact if needed */
        {
            uint64_t current = hu_estimate_tokens(agent->history, agent->history_count) +
                               (uint64_t)((system_prompt_len + 3) / 4);
            hu_context_pressure_t pr = {
                .current_tokens = (size_t)current,
                .max_tokens = (size_t)max_tokens,
                .pressure = 0.0f,
                .warning_85_emitted = agent->context_pressure_warning_85_emitted,
                .warning_95_emitted = agent->context_pressure_warning_95_emitted,
            };
            if (hu_context_check_pressure(&pr, agent->context_pressure_warn,
                                          agent->context_pressure_compact)) {
                hu_context_compact_for_pressure(agent->alloc, agent->history, &agent->history_count,
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
            hu_chat_message_t *hist_msgs = NULL;
            size_t hist_count = 0;
            err = hu_context_format_messages(&turn_alloc, agent->history, agent->history_count,
                                             agent->max_history_messages, &hist_msgs, &hist_count);
            if (err != HU_OK) {
                hu_agent_clear_current_for_tools();
                if (system_prompt)
                    agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
                return err;
            }
            size_t total = (hist_msgs ? hist_count : 0) + 1;
            hu_chat_message_t *all = (hu_chat_message_t *)turn_alloc.alloc(
                turn_alloc.ctx, total * sizeof(hu_chat_message_t));
            if (!all) {
                hu_agent_clear_current_for_tools();
                if (system_prompt)
                    agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
                return HU_ERR_OUT_OF_MEMORY;
            }
            all[0].role = HU_ROLE_SYSTEM;
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
            hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_LLM_REQUEST, .data = {{0}}};
            ev.data.llm_request.provider = prov_name ? prov_name : "";
            ev.data.llm_request.model = agent->model_name ? agent->model_name : "";
            ev.data.llm_request.messages_count = msgs_count;
            HU_OBS_SAFE_RECORD_EVENT(agent, &ev);
        }

        /* Tree-of-Thought: for complex queries, explore reasoning branches first */
#ifndef HU_IS_TEST
        if (agent->tree_of_thought_enabled && iter == 1 && msg_len > 200) {
            hu_tot_config_t tot_cfg = hu_tot_config_default();
            hu_tot_result_t tot_result;
            memset(&tot_result, 0, sizeof(tot_result));
            if (hu_tot_explore(agent->alloc, &agent->provider, agent->model_name,
                               agent->model_name_len, msg, msg_len, &tot_cfg, &tot_result) ==
                    HU_OK &&
                tot_result.best_thought && tot_result.best_thought_len > 0) {
                static const char tot_prefix[] = "\n\n[Reasoning approach: ";
                static const char tot_suffix[] = "]\n";
                size_t hint_len =
                    sizeof(tot_prefix) - 1 + tot_result.best_thought_len + sizeof(tot_suffix) - 1;
                if (hint_len < 2048 && system_prompt) {
                    size_t new_sys_len = system_prompt_len + hint_len;
                    char *new_sys =
                        (char *)agent->alloc->alloc(agent->alloc->ctx, new_sys_len + 1);
                    if (new_sys) {
                        memcpy(new_sys, system_prompt, system_prompt_len);
                        memcpy(new_sys + system_prompt_len, tot_prefix, sizeof(tot_prefix) - 1);
                        memcpy(new_sys + system_prompt_len + sizeof(tot_prefix) - 1,
                               tot_result.best_thought, tot_result.best_thought_len);
                        memcpy(new_sys + system_prompt_len + sizeof(tot_prefix) - 1 +
                                   tot_result.best_thought_len,
                               tot_suffix, sizeof(tot_suffix) - 1);
                        new_sys[new_sys_len] = '\0';
                        agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
                        system_prompt = new_sys;
                        system_prompt_len = new_sys_len;
                        if (msgs && msgs_count > 0) {
                            msgs[0].content = system_prompt;
                            msgs[0].content_len = system_prompt_len;
                        }
                    }
                }
            }
            hu_tot_result_free(agent->alloc, &tot_result);
        }
#endif

        clock_t llm_start = clock();
        hu_chat_response_t resp;
        memset(&resp, 0, sizeof(resp));
        err =
            agent->provider.vtable->chat(agent->provider.ctx, agent->alloc, &req, agent->model_name,
                                         agent->model_name_len, agent->temperature, &resp);
        uint64_t llm_duration_ms = hu_agent_internal_clock_diff_ms(llm_start, clock());

        {
            hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_LLM_RESPONSE, .data = {{0}}};
            ev.data.llm_response.provider = prov_name ? prov_name : "";
            ev.data.llm_response.model = agent->model_name ? agent->model_name : "";
            ev.data.llm_response.duration_ms = llm_duration_ms;
            ev.data.llm_response.success = (err == HU_OK);
            ev.data.llm_response.error_message = (err != HU_OK) ? "chat failed" : NULL;
            HU_OBS_SAFE_RECORD_EVENT(agent, &ev);
        }

        if (err != HU_OK) {
            {
                hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_ERR, .data = {{0}}};
                ev.data.err.component = "agent";
                ev.data.err.message = "provider chat failed";
                HU_OBS_SAFE_RECORD_EVENT(agent, &ev);
            }
            hu_agent_clear_current_for_tools();
            if (system_prompt)
                agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
            if (agent->turn_arena)
                hu_arena_reset(agent->turn_arena);
            return err;
        }

        agent->total_tokens += resp.usage.total_tokens;
        hu_agent_internal_record_cost(agent, &resp.usage);
        turn_tokens += resp.usage.total_tokens;

        if (resp.tool_calls_count == 0) {
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
            if (resp.content && resp.content_len > 0) {
                /* Reflection: evaluate response quality and retry if needed */
                hu_reflection_quality_t quality = hu_reflection_evaluate(
                    msg, msg_len, resp.content, resp.content_len, &agent->reflection);

                if (quality == HU_QUALITY_ACCEPTABLE && agent->reflection.use_llm &&
                    agent->reflection.enabled && reflection_retries_left > 0) {
                    quality =
                        hu_reflection_evaluate_llm(agent->alloc, &agent->provider, msg, msg_len,
                                                   resp.content, resp.content_len, quality);
                }

                if (quality == HU_QUALITY_NEEDS_RETRY && agent->reflection.enabled &&
                    reflection_retries_left > 0 && iter < agent->max_tool_iterations - 1) {
                    reflection_retries_left--;
                    char *critique = NULL;
                    size_t critique_len = 0;
                    hu_error_t cerr = hu_reflection_build_critique_prompt(
                        agent->alloc, msg, msg_len, resp.content, resp.content_len, &critique,
                        &critique_len);
                    if (cerr == HU_OK && critique) {
                        (void)hu_agent_internal_append_history(agent, HU_ROLE_ASSISTANT,
                                                               resp.content, resp.content_len, NULL,
                                                               0, NULL, 0);
                        (void)hu_agent_internal_append_history(agent, HU_ROLE_USER, critique,
                                                               critique_len, NULL, 0, NULL, 0);
                        agent->alloc->free(agent->alloc->ctx, critique, critique_len + 1);
                        hu_chat_response_free(agent->alloc, &resp);
                        iter++;
                        continue; /* retry with critique feedback */
                    }
                    if (critique)
                        agent->alloc->free(agent->alloc->ctx, critique, critique_len + 1);
                }

                /* A/B: when first response quality < 70 and channel history available,
                 * generate up to 2 more candidates and pick the best. */
                const char *final_content = resp.content;
                size_t final_len = resp.content_len;
                bool ab_owned = false;
                uint32_t max_chars = agent->max_response_chars ? agent->max_response_chars : 0;
                if (agent->ab_history_entries && agent->ab_history_count > 0) {
                    hu_quality_score_t q0 = hu_conversation_evaluate_quality(
                        resp.content, resp.content_len, agent->ab_history_entries,
                        agent->ab_history_count, max_chars);
                    if (q0.total < 70) {
                        hu_ab_result_t ab_result;
                        memset(&ab_result, 0, sizeof(ab_result));
                        ab_result.candidates[0].response =
                            hu_strndup(agent->alloc, resp.content, resp.content_len);
                        ab_result.candidates[0].response_len = resp.content_len;
                        ab_result.candidate_count = 1;

                        /* Generate 2nd candidate with alternative phrasing prompt */
                        static const char alt_suffix[] = "\n\nAlternative phrasing:";
                        size_t alt_len = system_prompt_len + sizeof(alt_suffix) - 1;
                        char *alt_system =
                            (char *)agent->alloc->alloc(agent->alloc->ctx, alt_len + 1);
                        if (alt_system) {
                            memcpy(alt_system, system_prompt, system_prompt_len);
                            memcpy(alt_system + system_prompt_len, alt_suffix,
                                   sizeof(alt_suffix) - 1);
                            alt_system[alt_len] = '\0';
                            size_t total = msgs_count;
                            hu_chat_message_t *alt_all = (hu_chat_message_t *)agent->alloc->alloc(
                                agent->alloc->ctx, total * sizeof(hu_chat_message_t));
                            if (alt_all) {
                                alt_all[0].role = HU_ROLE_SYSTEM;
                                alt_all[0].content = alt_system;
                                alt_all[0].content_len = alt_len;
                                alt_all[0].name = NULL;
                                alt_all[0].name_len = 0;
                                alt_all[0].tool_call_id = NULL;
                                alt_all[0].tool_call_id_len = 0;
                                alt_all[0].content_parts = NULL;
                                alt_all[0].content_parts_count = 0;
                                for (size_t i = 1; i < total; i++)
                                    alt_all[i] = msgs[i];
                                hu_chat_request_t alt_req = req;
                                alt_req.messages = alt_all;
                                alt_req.messages_count = total;

                                hu_chat_response_t alt_resp;
                                memset(&alt_resp, 0, sizeof(alt_resp));
                                hu_error_t alt_err = agent->provider.vtable->chat(
                                    agent->provider.ctx, agent->alloc, &alt_req, agent->model_name,
                                    agent->model_name_len, agent->temperature, &alt_resp);
                                if (alt_err == HU_OK && alt_resp.content &&
                                    alt_resp.content_len > 0) {
                                    agent->total_tokens += alt_resp.usage.total_tokens;
                                    turn_tokens += alt_resp.usage.total_tokens;
                                    ab_result.candidates[1].response = hu_strndup(
                                        agent->alloc, alt_resp.content, alt_resp.content_len);
                                    ab_result.candidates[1].response_len = alt_resp.content_len;
                                    ab_result.candidate_count = 2;
                                    hu_chat_response_free(agent->alloc, &alt_resp);

                                    hu_quality_score_t q1 = hu_conversation_evaluate_quality(
                                        ab_result.candidates[1].response,
                                        ab_result.candidates[1].response_len,
                                        agent->ab_history_entries, agent->ab_history_count,
                                        max_chars);
                                    if (q1.total < 70) {
                                        hu_chat_response_t alt2_resp;
                                        memset(&alt2_resp, 0, sizeof(alt2_resp));
                                        hu_error_t alt2_err = agent->provider.vtable->chat(
                                            agent->provider.ctx, agent->alloc, &alt_req,
                                            agent->model_name, agent->model_name_len,
                                            agent->temperature, &alt2_resp);
                                        if (alt2_err == HU_OK && alt2_resp.content &&
                                            alt2_resp.content_len > 0) {
                                            agent->total_tokens += alt2_resp.usage.total_tokens;
                                            turn_tokens += alt2_resp.usage.total_tokens;
                                            ab_result.candidates[2].response =
                                                hu_strndup(agent->alloc, alt2_resp.content,
                                                           alt2_resp.content_len);
                                            ab_result.candidates[2].response_len =
                                                alt2_resp.content_len;
                                            ab_result.candidate_count = 3;
                                        }
                                        hu_chat_response_free(agent->alloc, &alt2_resp);
                                    }
                                } else {
                                    hu_chat_response_free(agent->alloc, &alt_resp);
                                }
                                agent->alloc->free(agent->alloc->ctx, alt_all,
                                                   total * sizeof(hu_chat_message_t));
                            }
                            agent->alloc->free(agent->alloc->ctx, alt_system, alt_len + 1);
                        }

                        if (hu_ab_evaluate(agent->alloc, &ab_result, agent->ab_history_entries,
                                           agent->ab_history_count, max_chars) == HU_OK) {
                            if (agent->bth_metrics)
                                agent->bth_metrics->ab_evaluations++;
                            size_t bi = ab_result.best_idx;
                            if (bi < ab_result.candidate_count &&
                                ab_result.candidates[bi].response) {
                                if (bi != 0 && agent->bth_metrics)
                                    agent->bth_metrics->ab_alternates_chosen++;
                                final_content = ab_result.candidates[bi].response;
                                final_len = ab_result.candidates[bi].response_len;
                                ab_result.candidates[bi].response = NULL;
                                ab_result.candidates[bi].response_len = 0;
                                ab_owned = true;
                            }
                        }
                        hu_ab_result_deinit(&ab_result, agent->alloc);
                    }
                }

                /* Uncertainty: evaluate confidence and add hedge if needed */
                {
                    hu_uncertainty_signals_t u_signals;
                    memset(&u_signals, 0, sizeof(u_signals));
                    hu_uncertainty_extract_signals(final_content, final_len, msg, msg_len,
                                                   0, 0, &u_signals);
                    hu_uncertainty_result_t u_result;
                    memset(&u_result, 0, sizeof(u_result));
                    if (hu_uncertainty_evaluate(agent->alloc, &u_signals, &u_result) == HU_OK) {
                        if (u_result.hedge_prefix && u_result.hedge_prefix_len > 0 &&
                            u_result.level >= HU_CONFIDENCE_MEDIUM) {
                            size_t new_len = u_result.hedge_prefix_len + final_len;
                            char *hedged = (char *)agent->alloc->alloc(agent->alloc->ctx, new_len + 1);
                            if (hedged) {
                                memcpy(hedged, u_result.hedge_prefix, u_result.hedge_prefix_len);
                                memcpy(hedged + u_result.hedge_prefix_len, final_content, final_len);
                                hedged[new_len] = '\0';
                                if (ab_owned)
                                    agent->alloc->free(agent->alloc->ctx, (void *)final_content, final_len + 1);
                                final_content = hedged;
                                final_len = new_len;
                                ab_owned = true;
                            }
                        }
                    }
                    hu_uncertainty_result_free(agent->alloc, &u_result);
                }

                /* Constitutional AI: critique response against principles */
#ifndef HU_IS_TEST
                if (agent->constitutional_enabled) {
                    hu_constitutional_config_t const_cfg = hu_constitutional_config_default();
                    hu_critique_result_t critique;
                    memset(&critique, 0, sizeof(critique));
                    if (hu_constitutional_critique(agent->alloc, &agent->provider,
                                                    agent->model_name, agent->model_name_len,
                                                    msg, msg_len, final_content, final_len,
                                                    &const_cfg, &critique) == HU_OK) {
                        if (critique.verdict == HU_CRITIQUE_REWRITE &&
                            critique.revised_response && critique.revised_response_len > 0) {
                            if (ab_owned)
                                agent->alloc->free(agent->alloc->ctx, (void *)final_content, final_len + 1);
                            final_content = critique.revised_response;
                            final_len = critique.revised_response_len;
                            ab_owned = true;
                            critique.revised_response = NULL;
                        }
                    }
                    hu_critique_result_free(agent->alloc, &critique);
                }
#endif

                (void)hu_agent_internal_append_history(agent, HU_ROLE_ASSISTANT, final_content,
                                                       final_len, NULL, 0, NULL, 0);
                *response_out = hu_strndup(agent->alloc, final_content, final_len);
                if (ab_owned)
                    agent->alloc->free(agent->alloc->ctx, (void *)final_content, final_len + 1);
                if (!*response_out) {
                    hu_agent_clear_current_for_tools();
                    hu_chat_response_free(agent->alloc, &resp);
                    if (system_prompt)
                        agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
                    if (agent->turn_arena)
                        hu_arena_reset(agent->turn_arena);
                    return HU_ERR_OUT_OF_MEMORY;
                }
                if (response_len_out)
                    *response_len_out = final_len;

                /* Speculative: predict follow-ups and cache them */
                if (agent->speculative_cache && *response_out) {
                    char *preds[HU_SPEC_MAX_PREDICTIONS];
                    size_t pred_lens[HU_SPEC_MAX_PREDICTIONS];
                    double confs[HU_SPEC_MAX_PREDICTIONS];
                    size_t pred_count = 0;
                    memset(preds, 0, sizeof(preds));
                    if (hu_speculative_predict(agent->alloc, msg, msg_len,
                                               *response_out, final_len,
                                               preds, pred_lens, confs,
                                               HU_SPEC_MAX_PREDICTIONS, &pred_count) == HU_OK) {
                        for (size_t pi = 0; pi < pred_count; pi++) {
                            if (preds[pi])
                                agent->alloc->free(agent->alloc->ctx, preds[pi], pred_lens[pi] + 1);
                        }
                    }
                }

                hu_agent_internal_maybe_tts(agent, *response_out, final_len);
            }
            hu_chat_response_free(agent->alloc, &resp);
            hu_agent_clear_current_for_tools();
#ifdef HU_HAS_PERSONA
            hu_relationship_update(&agent->relationship, 1);
#endif
            /* Deep extraction: lightweight pattern-based fact extraction from user message */
            if (agent->memory && agent->memory->vtable && agent->memory->vtable->store) {
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
                            int n = snprintf(key_buf, sizeof(key_buf), "%s:%s:%s", f->subject,
                                             f->predicate, f->object);
                            if (n > 0 && (size_t)n < sizeof(key_buf)) {
                                (void)agent->memory->vtable->store(
                                    agent->memory->ctx, key_buf, (size_t)n, f->object,
                                    strlen(f->object), &cat, sid ? sid : "", sid_len);
                            }
                        }
                    }
                }
                hu_deep_extract_result_deinit(&de_result, agent->alloc);
            }
            /* Track agent's own commitments ("I'll check on that", "let me look into it") */
            if (agent->commitment_store && *response_out && *response_len_out > 0) {
                hu_commitment_detect_result_t cr;
                memset(&cr, 0, sizeof(cr));
                hu_error_t cerr = hu_commitment_detect(agent->alloc, *response_out,
                                                       *response_len_out, "assistant", 9, &cr);
                if (cerr == HU_OK && cr.count > 0) {
                    const char *sess = agent->memory_session_id;
                    size_t sess_len = sess ? agent->memory_session_id_len : 0;
                    for (size_t ci = 0; ci < cr.count; ci++)
                        (void)hu_commitment_store_save(agent->commitment_store, &cr.commitments[ci],
                                                       sess, sess_len);
                }
                hu_commitment_detect_result_deinit(&cr, agent->alloc);
            }
            if (system_prompt)
                agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
            if (agent->turn_arena)
                hu_arena_reset(agent->turn_arena);
            return HU_OK;
        }

        err = hu_agent_internal_append_history_with_tool_calls(
            agent, resp.content ? resp.content : "", resp.content_len, resp.tool_calls,
            resp.tool_calls_count);
        if (err != HU_OK) {
            hu_agent_clear_current_for_tools();
            hu_chat_response_free(agent->alloc, &resp);
            if (system_prompt)
                agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
            if (agent->turn_arena)
                hu_arena_reset(agent->turn_arena);
            return err;
        }
        hu_chat_response_free(agent->alloc, &resp);

        {
            size_t tc_count = agent->history[agent->history_count - 1].tool_calls_count;
            const hu_tool_call_t *calls = agent->history[agent->history_count - 1].tool_calls;

            /* Emit TOOL_CALL_START events for all calls */
            for (size_t tc = 0; tc < tc_count; tc++) {
                char tn_buf[64];
                size_t tn = (calls[tc].name_len < sizeof(tn_buf) - 1) ? calls[tc].name_len
                                                                      : sizeof(tn_buf) - 1;
                if (tn > 0 && calls[tc].name)
                    memcpy(tn_buf, calls[tc].name, tn);
                tn_buf[tn] = '\0';
                hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_TOOL_CALL_START, .data = {{0}}};
                ev.data.tool_call_start.tool = tn_buf[0] ? tn_buf : "unknown";
                HU_OBS_SAFE_RECORD_EVENT(agent, &ev);
            }

            /* LOCKED: skip all tool execution */
            if (agent->autonomy_level == HU_AUTONOMY_LOCKED) {
                for (size_t tc = 0; tc < tc_count; tc++) {
                    const hu_tool_call_t *call = &calls[tc];
                    (void)hu_agent_internal_append_history(
                        agent, HU_ROLE_TOOL, "Action blocked: agent is in locked mode", 38,
                        call->name, call->name_len, call->id, call->id_len);
                    if (agent->cancel_requested)
                        break;
                }
            } else {
                /* LLMCompiler: when enabled, compile tool calls into a DAG for parallel execution.
                 * Note: LLMCompiler is opt-in via config; the existing dispatcher handles
                 * parallelism. */
#ifndef HU_IS_TEST
                /* LLMCompiler: if enabled and 3+ tool calls, use DAG-based execution */
                if (agent->llm_compiler_enabled && tc_count >= 3) {
                    const char *tool_names[32];
                    size_t tn_count = 0;
                    for (size_t ti = 0; ti < agent->tools_count && tn_count < 32; ti++) {
                        if (agent->tools[ti].vtable && agent->tools[ti].vtable->name)
                            tool_names[tn_count++] =
                                agent->tools[ti].vtable->name(agent->tools[ti].ctx);
                    }
                    char *compiler_prompt = NULL;
                    size_t compiler_prompt_len = 0;
                    const char *goal_text = msg;
                    size_t goal_text_len = msg_len;
                    if (hu_llm_compiler_build_prompt(agent->alloc, goal_text, goal_text_len,
                                                    tool_names, tn_count, &compiler_prompt,
                                                    &compiler_prompt_len) == HU_OK) {
                        hu_chat_message_t compiler_msgs[1] = {{
                            .role = HU_ROLE_USER,
                            .content = compiler_prompt,
                            .content_len = compiler_prompt_len,
                        }};
                        hu_chat_request_t compiler_req = {
                            .messages = compiler_msgs,
                            .messages_count = 1,
                            .tools = NULL,
                            .tools_count = 0,
                        };
                        hu_chat_response_t compiler_resp;
                        memset(&compiler_resp, 0, sizeof(compiler_resp));
                        hu_error_t cerr = agent->provider.vtable->chat(
                            agent->provider.ctx, agent->alloc, &compiler_req, agent->model_name,
                            agent->model_name_len, agent->temperature, &compiler_resp);
                        hu_str_free(agent->alloc, compiler_prompt);
                        if (cerr == HU_OK && compiler_resp.content &&
                            compiler_resp.content_len > 0) {
                            hu_dag_t dag;
                            hu_dag_init(&dag, *agent->alloc);
                            if (hu_llm_compiler_parse_plan(agent->alloc, compiler_resp.content,
                                                           compiler_resp.content_len,
                                                           &dag) == HU_OK &&
                                dag.node_count > 0) {
                                hu_observer_event_t ev = {
                                    .tag = HU_OBSERVER_EVENT_TOOL_CALL,
                                    .data = {{0}},
                                };
                                ev.data.tool_call.tool = "llm_compiler";
                                ev.data.tool_call.duration_ms = 0;
                                ev.data.tool_call.success = true;
                                HU_OBS_SAFE_RECORD_EVENT(agent, &ev);
                            }
                            hu_dag_deinit(&dag);
                        }
                        hu_chat_response_free(agent->alloc, &compiler_resp);
                    }
                }
#endif
                /* Use dispatcher for parallel execution when enabled (Tier 1.3) */
                hu_dispatcher_t dispatcher;
                hu_dispatcher_default(&dispatcher);
                if (tc_count > 1)
                    dispatcher.max_parallel = 4;
                dispatcher.timeout_secs = 30;

                hu_dispatch_result_t dispatch_result;
                memset(&dispatch_result, 0, sizeof(dispatch_result));
                err = hu_dispatcher_dispatch(&dispatcher, agent->alloc, agent->tools,
                                             agent->tools_count, calls, tc_count, &dispatch_result);

                if (err == HU_OK && dispatch_result.results) {
                    for (size_t tc = 0; tc < tc_count; tc++) {
                        const hu_tool_call_t *call = &calls[tc];
                        hu_tool_result_t *result = &dispatch_result.results[tc];

                        char tn_buf[64];
                        size_t tn = (call->name_len < sizeof(tn_buf) - 1) ? call->name_len
                                                                          : sizeof(tn_buf) - 1;
                        if (tn > 0 && call->name)
                            memcpy(tn_buf, call->name, tn);
                        tn_buf[tn] = '\0';
                        const char *args_str = call->arguments ? call->arguments : "";

                        /* Policy evaluation (dispatcher path) */
                        hu_policy_action_t pa = hu_agent_internal_evaluate_tool_policy(
                            agent, tn_buf[0] ? tn_buf : "unknown", args_str);
                        if (pa == HU_POLICY_DENY) {
                            if (agent->audit_logger) {
                                hu_audit_event_t aev;
                                hu_audit_event_init(&aev, HU_AUDIT_POLICY_VIOLATION);
                                hu_audit_event_with_identity(
                                    &aev, agent->agent_id,
                                    agent->model_name ? agent->model_name : "unknown", NULL);
                                hu_audit_event_with_action(&aev, tn_buf[0] ? tn_buf : "unknown",
                                                           "denied", false, false);
                                hu_audit_logger_log(agent->audit_logger, &aev);
                            }
                            hu_tool_result_free(agent->alloc, result);
                            *result = hu_tool_result_fail("denied by policy", 16);
                        } else if (pa == HU_POLICY_REQUIRE_APPROVAL) {
                            result->needs_approval = true;
                        }

                        /* Autonomy: SUPERVISED forces approval; ASSISTED for medium/high risk */
                        if (agent->autonomy_level == HU_AUTONOMY_SUPERVISED) {
                            result->needs_approval = true;
                        } else if (agent->autonomy_level == HU_AUTONOMY_ASSISTED) {
                            if (hu_tool_risk_level(tn_buf[0] ? tn_buf : "unknown") >=
                                HU_RISK_MEDIUM)
                                result->needs_approval = true;
                        }

                        /* Feature 2: explicit failure when approval required but no callback */
                        if (result->needs_approval && !agent->approval_cb) {
                            hu_tool_result_free(agent->alloc, result);
                            *result = hu_tool_result_fail("requires human approval", 23);
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
                                hu_tool_result_free(agent->alloc, result);
                                if (agent->policy)
                                    agent->policy->pre_approved = true;
                                hu_tool_t *tool =
                                    hu_agent_internal_find_tool(agent, call->name, call->name_len);
                                if (tool) {
                                    hu_json_value_t *retry_args = NULL;
                                    if (call->arguments_len > 0)
                                        (void)hu_json_parse(agent->alloc, call->arguments,
                                                            call->arguments_len, &retry_args);
                                    *result = hu_tool_result_fail("invalid arguments", 16);
                                    if (retry_args) {
                                        if (tool->vtable->execute)
                                            tool->vtable->execute(tool->ctx, agent->alloc,
                                                                  retry_args, result);
                                        hu_json_free(agent->alloc, retry_args);
                                    }
                                }
                            } else {
                                hu_tool_result_free(agent->alloc, result);
                                *result = hu_tool_result_fail("user denied action", 18);
                            }
                        }

                        {
                            hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_TOOL_CALL,
                                                      .data = {{0}}};
                            ev.data.tool_call.tool = tn_buf[0] ? tn_buf : "unknown";
                            ev.data.tool_call.duration_ms = 0;
                            ev.data.tool_call.success = result->success;
                            ev.data.tool_call.detail =
                                result->success
                                    ? NULL
                                    : (result->error_msg ? result->error_msg : "failed");
                            HU_OBS_SAFE_RECORD_EVENT(agent, &ev);
                        }

                        /* Outcome tracking */
                        if (agent->outcomes) {
                            const char *sum =
                                result->success
                                    ? (result->output ? result->output : "ok")
                                    : (result->error_msg ? result->error_msg : "failed");
                            hu_outcome_record_tool(agent->outcomes, tn_buf, result->success, sum);
                        }

#ifdef HU_ENABLE_SQLITE
                        /* Online learning: record tool outcome signal */
                        if (agent->memory) {
                            sqlite3 *ol_db = hu_sqlite_memory_get_db(agent->memory);
                            if (ol_db) {
                                hu_online_learning_t ol;
                                if (hu_online_learning_create(agent->alloc, ol_db, 0.1, &ol) ==
                                    HU_OK) {
                                    hu_learning_signal_t sig = {
                                        .type = result->success ? HU_SIGNAL_TOOL_SUCCESS
                                                               : HU_SIGNAL_TOOL_FAILURE,
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
                                        hu_self_improve_record_tool_outcome(&si, tn_buf, tn,
                                                                             result->success,
                                                                             sig.timestamp);
                                        hu_self_improve_deinit(&si);
                                    }
                                    hu_online_learning_deinit(&ol);
                                }
                            }
                        }
#endif

                        const char *res_content =
                            result->success ? result->output : result->error_msg;
                        size_t res_len =
                            result->success ? result->output_len : result->error_msg_len;
                        (void)hu_agent_internal_append_history(agent, HU_ROLE_TOOL, res_content,
                                                               res_len, call->name, call->name_len,
                                                               call->id, call->id_len);

                        if (agent->audit_logger) {
                            hu_audit_event_t aev;
                            hu_audit_event_init(&aev, HU_AUDIT_COMMAND_EXECUTION);
                            hu_audit_event_with_identity(
                                &aev, agent->agent_id,
                                agent->model_name ? agent->model_name : "unknown", NULL);
                            hu_audit_event_with_action(&aev, tn_buf, "tool", result->success, true);
                            hu_audit_event_with_result(&aev, result->success, 0, 0,
                                                       result->success ? NULL : result->error_msg);
                            hu_audit_logger_log(agent->audit_logger, &aev);
                        }

                        if (agent->cancel_requested)
                            break;
                    }
                    hu_dispatch_result_free(agent->alloc, &dispatch_result);
                } else {
                    /* Fallback: sequential if dispatcher fails */
                    for (size_t tc = 0; tc < tc_count; tc++) {
                        const hu_tool_call_t *call = &calls[tc];
                        hu_tool_t *tool =
                            hu_agent_internal_find_tool(agent, call->name, call->name_len);
                        if (!tool) {
                            (void)hu_agent_internal_append_history(
                                agent, HU_ROLE_TOOL, "tool not found", 14, call->name,
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

                        hu_policy_action_t pa = hu_agent_internal_evaluate_tool_policy(
                            agent, pol_tn, call->arguments ? call->arguments : "");
                        bool force_approval = (agent->autonomy_level == HU_AUTONOMY_SUPERVISED) ||
                                              (agent->autonomy_level == HU_AUTONOMY_ASSISTED &&
                                               hu_tool_risk_level(pol_tn) >= HU_RISK_MEDIUM);

                        hu_tool_result_t result = hu_tool_result_fail("invalid arguments", 16);
                        if (pa == HU_POLICY_DENY) {
                            if (agent->audit_logger) {
                                hu_audit_event_t aev;
                                hu_audit_event_init(&aev, HU_AUDIT_POLICY_VIOLATION);
                                hu_audit_event_with_identity(
                                    &aev, agent->agent_id,
                                    agent->model_name ? agent->model_name : "unknown", NULL);
                                hu_audit_event_with_action(&aev, pol_tn, "denied", false, false);
                                hu_audit_logger_log(agent->audit_logger, &aev);
                            }
                            result = hu_tool_result_fail("denied by policy", 16);
                        } else if (pa == HU_POLICY_REQUIRE_APPROVAL || force_approval) {
                            result = hu_tool_result_fail("pending approval", 16);
                            result.needs_approval = true;
                        } else {
                            hu_json_value_t *args = NULL;
                            if (call->arguments_len > 0) {
                                hu_error_t pe = hu_json_parse(agent->alloc, call->arguments,
                                                              call->arguments_len, &args);
                                if (pe == HU_OK && args) {
                                    tool->vtable->execute(tool->ctx, agent->alloc, args, &result);
                                    hu_json_free(agent->alloc, args);
                                }
                            }
                        }

                        /* Feature 2: explicit failure when approval required but no callback */
                        if (result.needs_approval && !agent->approval_cb) {
                            hu_tool_result_free(agent->alloc, &result);
                            result = hu_tool_result_fail("requires human approval", 23);
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
                                hu_tool_result_free(agent->alloc, &result);
                                if (agent->policy)
                                    agent->policy->pre_approved = true;
                                hu_json_value_t *retry_args = NULL;
                                if (call->arguments_len > 0)
                                    (void)hu_json_parse(agent->alloc, call->arguments,
                                                        call->arguments_len, &retry_args);
                                result = hu_tool_result_fail("invalid arguments", 16);
                                if (retry_args) {
                                    tool->vtable->execute(tool->ctx, agent->alloc, retry_args,
                                                          &result);
                                    hu_json_free(agent->alloc, retry_args);
                                }
                            } else {
                                hu_tool_result_free(agent->alloc, &result);
                                result = hu_tool_result_fail("user denied action", 18);
                            }
                        }

                        const char *res_content = result.success ? result.output : result.error_msg;
                        size_t res_len = result.success ? result.output_len : result.error_msg_len;
                        (void)hu_agent_internal_append_history(agent, HU_ROLE_TOOL, res_content,
                                                               res_len, call->name, call->name_len,
                                                               call->id, call->id_len);

                        if (agent->audit_logger) {
                            hu_audit_event_t aev;
                            hu_audit_event_init(&aev, HU_AUDIT_COMMAND_EXECUTION);
                            hu_audit_event_with_identity(
                                &aev, agent->agent_id,
                                agent->model_name ? agent->model_name : "unknown", NULL);
                            hu_audit_event_with_action(&aev, pol_tn, "tool", result.success, true);
                            hu_audit_event_with_result(&aev, result.success, 0, 0,
                                                       result.success ? NULL : result.error_msg);
                            hu_audit_logger_log(agent->audit_logger, &aev);
                        }

                        hu_tool_result_free(agent->alloc, &result);
                        if (agent->cancel_requested)
                            break;
                    }
                }
            }
        }
        /* Messages will be reformatted at top of next iteration via arena */
    }

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
    hu_agent_clear_current_for_tools();
    if (system_prompt)
        agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
    if (agent->turn_arena)
        hu_arena_reset(agent->turn_arena);
    return HU_ERR_TIMEOUT;
}
