/* src/agent/turn/turn_perceive.c — S2 perception, carved verbatim out of
 * hu_agent_turn (src/agent/agent_turn.c) by the phase-1 carve
 * (docs/superpowers/plans/2026-09-30-agent-turn-carve-phase1.md): ACP inbox,
 * cognition budget and dual-process dispatch, fast capture / STM / pattern
 * radar, commitment detection, preference and outcome learning, tone and
 * rhythm hints. The function statics (emotion_names, rhythm_short,
 * rhythm_long) moved with the block; tone_hint may point at the rhythm
 * literals, which outlive the call. */
#include "../agent_internal.h"
#include "human/agent/acp_bridge.h"
#include "human/agent/outcomes.h"
#include "human/agent/preferences.h"
#include "human/agent/prompt.h"
#include "human/agent/turn.h"
#include "human/core/log.h"
#include "human/core/string.h"
#include "human/memory/fast_capture.h"
#include <math.h>

hu_error_t hu_turn_perceive(hu_turn_ctx_t *turn_ctx) {
    if (!turn_ctx || !turn_ctx->in.agent || !turn_ctx->in.msg)
        return HU_ERR_INVALID_ARGUMENT;
    const char *msg = turn_ctx->in.msg;
    size_t msg_len = turn_ctx->in.msg_len;
    hu_agent_t *agent = turn_ctx->in.agent;
    hu_error_t err = HU_OK;
    /* ACP inbox: check for pending inter-agent messages */
    char *acp_context = NULL;
    size_t acp_context_len = 0;
    if (agent->infra.acp_inbox) {
        hu_acp_inbox_t *inbox = (hu_acp_inbox_t *)agent->infra.acp_inbox;
        size_t pending = hu_acp_inbox_count(inbox, -1);
        if (pending > 0) {
            char acp_buf[2048];
            size_t acp_pos = 0;
            const char *hdr = "[Inter-agent messages]\n";
            size_t hdr_len = strlen(hdr);
            memcpy(acp_buf, hdr, hdr_len);
            acp_pos = hdr_len;
            for (size_t ai = 0; ai < pending && ai < 5; ai++) {
                hu_acp_message_t acp_msg;
                if (hu_acp_inbox_pop(inbox, &acp_msg) != HU_OK)
                    break;
                const char *type_name = hu_acp_msg_type_name(acp_msg.type);
                int n = snprintf(acp_buf + acp_pos, sizeof(acp_buf) - acp_pos,
                                 "- %s from %.*s: %.*s\n", type_name, (int)(acp_msg.sender_id_len),
                                 acp_msg.sender_id ? acp_msg.sender_id : "?",
                                 (int)(acp_msg.payload_len > 200 ? 200 : acp_msg.payload_len),
                                 acp_msg.payload ? acp_msg.payload : "");
                if (n > 0 && acp_pos + (size_t)n < sizeof(acp_buf))
                    acp_pos += (size_t)n;
                hu_acp_message_free(agent->alloc, &acp_msg);
            }
            if (acp_pos > hdr_len) {
                acp_context = hu_strndup(agent->alloc, acp_buf, acp_pos);
                acp_context_len = acp_pos;
            }
        }
    }

    hu_cognition_budget_t cognition_budget =
        hu_cognition_get_budget(HU_COGNITION_FAST, agent->max_tool_iterations);

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
                if ((size_t)tag < sizeof(emotion_names) / sizeof(emotion_names[0])) {
                    const char *name = emotion_names[tag];
                    (void)hu_pattern_radar_observe(&agent->radar, name, strlen(name),
                                                   HU_PATTERN_EMOTIONAL_TREND, NULL, 0, ts, ts_len);
                }
            }
        }
        hu_fc_result_deinit(&fc_result, agent->alloc);
    }

    /* Cognition: emotional fusion + dual-process dispatch (memory loader budgets, prompt hints) */
    {
        hu_emotional_perception_t percep;
        memset(&percep, 0, sizeof(percep));
        percep.voice_valence = NAN;
        percep.egraph_dominant = HU_EMOTION_NEUTRAL;
        percep.egraph_intensity = 0.0f;

        size_t stm_n = hu_stm_count(&agent->stm);
        const hu_stm_emotion_t *stm_emo = NULL;
        size_t stm_emo_count = 0;
        if (stm_n > 0) {
            const hu_stm_turn_t *lt = hu_stm_get(&agent->stm, stm_n - 1);
            if (lt && lt->emotion_count > 0) {
                stm_emo = lt->emotions;
                stm_emo_count = lt->emotion_count;
            }
        }
        percep.stm_emotions = stm_emo;
        percep.stm_emotion_count = stm_emo_count;
        percep.fast_capture = NULL;
        percep.conversation = NULL;

        hu_emotional_cognition_perceive(&agent->infra.emotional_cognition, &percep);

        size_t recent_tools = 0;
        if (agent->history_count > 1) {
            for (size_t hi = agent->history_count - 1; hi > 0; hi--) {
                if (agent->history[hi - 1].role == HU_ROLE_TOOL) {
                    recent_tools++;
                    if (recent_tools >= 8)
                        break;
                }
            }
        }

        hu_cognition_dispatch_input_t d_in = {
            .message = msg,
            .message_len = msg_len,
            .emotional = &agent->infra.emotional_cognition,
            .tools_count = agent->tools_count,
            .recent_tool_calls = recent_tools,
            .agent_max_tool_iterations = agent->max_tool_iterations,
        };
        agent->infra.current_cognition_mode = hu_cognition_dispatch(&d_in);
        cognition_budget = hu_cognition_get_budget(agent->infra.current_cognition_mode,
                                                   agent->max_tool_iterations);

        if (agent->observer) {
            hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_COGNITION_MODE};
            ev.data.cognition_mode.mode =
                hu_cognition_mode_name(agent->infra.current_cognition_mode);
            HU_OBS_SAFE_RECORD_EVENT(agent, &ev);
        }
        if (agent->bth_metrics) {
            switch (agent->infra.current_cognition_mode) {
            case HU_COGNITION_FAST:
                agent->bth_metrics->cognition_fast_turns++;
                break;
            case HU_COGNITION_SLOW:
                agent->bth_metrics->cognition_slow_turns++;
                break;
            case HU_COGNITION_EMOTIONAL:
                agent->bth_metrics->cognition_emotional_turns++;
                break;
            default:
                break;
            }
        }
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
                hu_error_t cs_err = hu_commitment_store_save(
                    agent->commitment_store, &commit_result.commitments[i], sess, sess_len);
                if (cs_err != HU_OK)
                    hu_log_error("agent", NULL, "commitment save failed: %s",
                                 hu_error_string(cs_err));
                /* Mirror goal-type commitments into the personal model so
                 * build_prompt can surface "Active goals" in context. */
                if ((commit_result.commitments[i].type == HU_COMMITMENT_GOAL ||
                     commit_result.commitments[i].type == HU_COMMITMENT_INTENTION) &&
                    commit_result.commitments[i].summary &&
                    commit_result.commitments[i].summary_len > 0 &&
                    agent->personal_model.goal_count < HU_PM_MAX_GOALS) {
                    hu_personal_goal_t *g =
                        &agent->personal_model.goals[agent->personal_model.goal_count];
                    memset(g, 0, sizeof(*g));
                    size_t sn = commit_result.commitments[i].summary_len;
                    if (sn > sizeof(g->description) - 1)
                        sn = sizeof(g->description) - 1;
                    memcpy(g->description, commit_result.commitments[i].summary, sn);
                    g->description[sn] = '\0';
                    g->active = true;
                    g->created_at = (int64_t)time(NULL);
                    agent->personal_model.goal_count++;
                }
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
            hu_error_t pref_err = hu_preferences_store(agent->memory, agent->alloc, pref, pref_len);
            if (pref_err != HU_OK)
                hu_log_warn("agent", NULL, "preference store failed: %s",
                            hu_error_string(pref_err));
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
            if (positive) {
                hu_outcome_record_positive(agent->outcomes, msg);
                if (agent->frontiers.initialized)
                    hu_tcal_update(&agent->frontiers.trust, 0.6f, 0.5f, 0.5f);
            }
        }
        if (is_correction && agent->frontiers.initialized)
            hu_tcal_update(&agent->frontiers.trust, -0.3f, 0.0f, -0.2f);
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

    /* Rhythm matching: when user sends a short casual message, nudge the
     * agent to match their brevity. Long reflective messages get more space. */
    static const char rhythm_short[] =
        " The user sent a very short message — match their energy with a brief, "
        "conversational reply. Don't over-explain.";
    static const char rhythm_long[] =
        " The user wrote a long, thoughtful message — give it the space it deserves "
        "with a proportional, considered response.";
    if (msg_len <= 15 && msg_len > 0 && !tone_hint) {
        tone_hint = rhythm_short;
        tone_hint_len = sizeof(rhythm_short) - 1;
    } else if (msg_len >= 400 && !tone_hint) {
        tone_hint = rhythm_long;
        tone_hint_len = sizeof(rhythm_long) - 1;
    }

    /* Load user preferences for prompt injection */
    char *pref_ctx = NULL;
    size_t pref_ctx_len = 0;
    if (agent->memory) {
        hu_error_t pref_err =
            hu_preferences_load(agent->memory, agent->alloc, &pref_ctx, &pref_ctx_len);
        if (pref_err != HU_OK)
            hu_log_error("agent_turn", NULL, "preferences load failed: %s",
                         hu_error_string(pref_err));
    }

    turn_ctx->perception.cognition_budget = cognition_budget;
    turn_ctx->perception.acp_context = acp_context;
    turn_ctx->perception.acp_context_len = acp_context_len;
    turn_ctx->perception.tone_hint = tone_hint;
    turn_ctx->perception.tone_hint_len = tone_hint_len;
    turn_ctx->perception.pref_ctx = pref_ctx;
    turn_ctx->perception.pref_ctx_len = pref_ctx_len;
    return HU_OK;
}
