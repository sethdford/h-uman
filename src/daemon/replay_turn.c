/* replay_turn.c — one real inbound turn through the daemon's llm_decides
 * reply path, offline. Contract: include/human/daemon/replay_turn.h; runbook
 * and the fidelity table (what is and is not replayed): docs/guides/replay-harness.md.
 *
 * Order follows hu_service_run's reactive batch body: director → silence /
 * tapback decision → slice A context load → slice B prompt build → length
 * calibration → reply budget → voice-first → arm G6 → agent turn (tools off)
 * with the AI-tell and quality retries → end G6 → validator chain → shaper →
 * typos → sanitize → split. Every step calls the production function; the
 * glue between them is all this file adds. */
#include "human/daemon/replay_turn.h"

#include "../agent/agent_internal.h"
#include "human/agent/choreography.h"
#include "human/agent/validators/builtin.h"
#include "human/channel_class.h"
#include "human/context/conversation.h"
#include "human/core/gate_mode.h"
#include "human/core/string.h"
#include "human/daemon/daemon_shape.h"
#include "human/daemon/expressive.h"
#include "human/daemon/message_router.h"
#include "human/daemon/outbound_sanitize.h"
#include "human/daemon/reactive_calibration.h"
#include "human/daemon/reactive_gates.h"
#include "human/daemon/reactive_turn.h"
#include "human/daemon/voice_first.h"
#include "human/persona.h"
#include "human/security/moderation.h"
#include <string.h>

#define REPLAY_CONVO_CAP 32768 /* daemon.c: context capped at ~8K tokens */

const char *hu_replay_action_name(hu_replay_action_t action) {
    switch (action) {
    case HU_REPLAY_ACTION_TEXT:
        return "text";
    case HU_REPLAY_ACTION_TAPBACK:
        return "tapback";
    case HU_REPLAY_ACTION_SILENCE:
        return "silence";
    case HU_REPLAY_ACTION_DROPPED:
        return "dropped";
    case HU_REPLAY_ACTION_ERROR:
        return "error";
    }
    return "unknown";
}

/* Production restores the newest HU_DAEMON_RESTORE_RECENT messages from the
 * session store. A replay has no store for the past, so the thread before the
 * inbound stands in for it: Seth's lines as assistant turns, theirs as user. */
static void replay_seed_history(hu_agent_t *agent, const hu_channel_history_entry_t *history,
                                size_t count) {
    size_t first = count > HU_DAEMON_RESTORE_RECENT ? count - HU_DAEMON_RESTORE_RECENT : 0;
    for (size_t i = first; history && i < count; i++) {
        size_t n = strnlen(history[i].text, sizeof(history[i].text));
        if (n == 0)
            continue;
        hu_role_t role = history[i].from_me ? HU_ROLE_ASSISTANT : HU_ROLE_USER;
        if (hu_agent_internal_append_history(agent, role, history[i].text, n, NULL, 0, NULL, 0) !=
            HU_OK)
            return;
    }
}

/* *ctx = head + sep + *ctx (either side may be empty). Leaves *ctx untouched
 * on allocation failure. */
static void replay_ctx_prepend(hu_allocator_t *alloc, char **ctx, size_t *ctx_len, const char *head,
                               size_t head_len, const char *sep) {
    if (!head || head_len == 0)
        return;
    size_t sep_len = (*ctx && *ctx_len > 0) ? strlen(sep) : 0;
    size_t old = *ctx ? *ctx_len : 0;
    size_t total = head_len + sep_len + old;
    char *joined = (char *)alloc->alloc(alloc->ctx, total + 1);
    if (!joined)
        return;
    memcpy(joined, head, head_len);
    memcpy(joined + head_len, sep, sep_len);
    if (old > 0)
        memcpy(joined + head_len + sep_len, *ctx, old);
    joined[total] = '\0';
    if (*ctx)
        alloc->free(alloc->ctx, *ctx, *ctx_len + 1);
    *ctx = joined;
    *ctx_len = total;
}

static void replay_noop_stream_cb(const hu_agent_stream_event_t *event, void *ctx) {
    (void)event, (void)ctx;
}

static bool replay_overlay_has_typos(const hu_persona_overlay_t *overlay) {
    for (size_t i = 0; overlay && overlay->typing_quirks && i < overlay->typing_quirks_count; i++) {
        const char *q = overlay->typing_quirks[i];
        if (q && strcmp(q, "occasional_typos") == 0)
            return true;
    }
    return false;
}

/* Validator chain → shaper → typos → sanitize, as daemon.c's send block.
 * `*alloc_len` tracks the allocation (bytes - 1) across reallocs. */
static void replay_shape_reply(hu_allocator_t *alloc, hu_agent_t *agent, const char *key,
                               size_t key_len, uint32_t seed, char **resp, size_t *resp_len,
                               size_t *alloc_len) {
    hu_validator_chain_apply_default_in_place(alloc, agent->observer, NULL, 0, "replay", *resp,
                                              resp_len, *resp_len + 1);
    const hu_persona_overlay_t *overlay =
        (agent->persona && agent->active_channel)
            ? hu_persona_find_overlay(agent->persona, agent->active_channel,
                                      agent->active_channel_len)
            : NULL;
    const hu_contact_profile_t *contact =
        agent->persona ? hu_persona_find_contact(agent->persona, key, key_len) : NULL;
    const char *formality = (overlay && overlay->formality) ? overlay->formality : NULL;
    float disfluency = agent->persona ? agent->persona->humanization.disfluency_frequency : 0.15f;
    size_t cap = *alloc_len + 1;
    hu_daemon_shape_text_inplace(agent->alloc, resp, resp_len, &cap, seed, overlay, contact,
                                 formality, formality ? strlen(formality) : 0,
                                 agent->active_channel ? agent->active_channel : "unknown",
                                 agent->active_channel ? agent->active_channel_len : 7, disfluency);
    *alloc_len = cap - 1;
    if (*resp && *resp_len > 0 && replay_overlay_has_typos(overlay)) {
        if (*alloc_len + 1 <= *resp_len + 1) {
            char *grown = (char *)agent->alloc->realloc(agent->alloc->ctx, *resp, *alloc_len + 1,
                                                        *resp_len + 2);
            if (grown) {
                *resp = grown;
                *alloc_len = *resp_len + 1;
            }
        }
        if (*alloc_len + 1 > *resp_len + 1)
            *resp_len = hu_conversation_apply_typos(*resp, *resp_len, *alloc_len + 1, seed);
    }
    hu_daemon_outbound_sanitize(*resp, resp_len, *alloc_len + 1, true, agent->observer);
}

static void replay_add_bubble(hu_allocator_t *alloc, hu_replay_turn_result_t *out, const char *s,
                              size_t n) {
    if (n == 0 || out->bubble_count >= HU_REPLAY_MAX_BUBBLES)
        return;
    char *copy = hu_strndup(alloc, s, n);
    if (!copy)
        return;
    out->bubbles[out->bubble_count] = copy;
    out->bubble_lens[out->bubble_count] = n;
    out->bubble_count++;
}

/* The bubbles the daemon would send: plaintext once, then choreography when
 * the frontiers are up, else the fragment splitter with iMessage cadence. */
static void replay_split(hu_allocator_t *alloc, hu_agent_t *agent, hu_channel_t *ch,
                         uint32_t max_chars, uint32_t seed, const char *text, size_t len,
                         hu_replay_turn_result_t *out) {
    char *clean = NULL;
    size_t clean_len = 0;
    const char *src = text;
    size_t src_len = len;
    if (hu_daemon_plaintext_for_split_channel(ch, alloc, text, len, &clean, &clean_len)) {
        src = clean;
        src_len = clean_len;
    }
    hu_message_plan_t plan = {0};
    bool choreo = false;
    if (agent->frontiers.initialized) {
        hu_choreography_config_t cfg = hu_choreography_config_default();
        cfg.energy_level = agent->frontiers.somatic.energy;
        choreo = hu_choreography_plan(alloc, src, src_len, &cfg, seed, &plan) == HU_OK &&
                 plan.segment_count > 1;
    }
    if (choreo) {
        for (size_t i = 0; i < plan.segment_count; i++)
            replay_add_bubble(alloc, out, plan.segments[i].text, plan.segments[i].text_len);
    } else {
        hu_message_fragment_t frags[4];
        size_t nf = hu_conversation_split_response(alloc, src, src_len, frags, 4, max_chars);
        hu_channel_class_t cls = hu_channel_class_for_name(ch->vtable->name(ch->ctx));
        for (size_t f = 0; f < nf; f++) {
            char chunks[4][512];
            size_t nc =
                hu_conversation_split_for_cadence(frags[f].text, frags[f].text_len, cls, chunks, 4);
            if (nc >= 2) {
                for (size_t c = 0; c < nc; c++)
                    replay_add_bubble(alloc, out, chunks[c], strlen(chunks[c]));
            } else {
                replay_add_bubble(alloc, out, frags[f].text, frags[f].text_len);
            }
            alloc->free(alloc->ctx, frags[f].text, frags[f].text_len + 1);
        }
    }
    if (plan.segments)
        hu_choreography_plan_free(alloc, &plan);
    if (clean)
        alloc->free(alloc->ctx, clean, clean_len + 1);
}

/* The agent turn with the daemon's two safety retries (AI tell, quality).
 * Returns the turn error; *resp is the reply (or NULL when dropped). */
static hu_error_t replay_agent_turn(hu_allocator_t *alloc, hu_agent_t *agent, const char *key,
                                    size_t key_len, char *combined, size_t combined_len,
                                    char **convo, size_t *convo_len, bool voice_memo,
                                    hu_replay_turn_result_t *out, char **resp, size_t *resp_len) {
    hu_error_t err = HU_OK;
    bool retried = false;
    size_t saved_tools = agent->tools_count;
    size_t saved_specs = agent->tool_specs_count;
    /* llm_decides reply turns run with the tool registry empty (daemon.c). */
    agent->tools_count = 0;
    agent->tool_specs_count = 0;
    for (;;) {
        if (*resp) {
            agent->alloc->free(agent->alloc->ctx, *resp, *resp_len + 1);
            *resp = NULL;
            *resp_len = 0;
        }
        err = retried ? hu_agent_turn(agent, combined, combined_len, resp, resp_len)
                      : hu_agent_turn_stream_v2(agent, combined, combined_len,
                                                replay_noop_stream_cb, NULL, resp, resp_len);
        if (err == HU_OK)
            (void)hu_daemon_quality_draft_settle(agent->alloc, key, key_len, resp, resp_len);
        if (err != HU_OK || !*resp || *resp_len == 0)
            break;
        if (hu_reactive_gate_active(HU_REACTIVE_GATE_AI_TELL_RETRY, true)) {
            const char *tell = hu_reactive_response_ai_tell(*resp);
            hu_ai_tell_action_t ta = hu_reactive_ai_tell_action(tell, retried);
            if (tell)
                out->ai_tell = tell;
            if (ta != HU_AI_TELL_SEND) {
                agent->alloc->free(agent->alloc->ctx, *resp, *resp_len + 1);
                *resp = NULL;
                *resp_len = 0;
            }
            if (ta == HU_AI_TELL_RETRY) {
                retried = true;
                const char *hint = hu_reactive_ai_tell_retry_hint();
                replay_ctx_prepend(alloc, convo, convo_len, hint, strlen(hint), "\n");
                agent->conversation_context = *convo;
                agent->conversation_context_len = *convo_len;
                continue;
            }
            if (ta == HU_AI_TELL_DROP)
                break;
        }
        if (!voice_memo && agent->ab_history_entries &&
            hu_reactive_gate_active(HU_REACTIVE_GATE_QUALITY_RETRY, true)) {
            hu_quality_score_t q = hu_conversation_evaluate_quality(
                *resp, *resp_len, agent->ab_history_entries, agent->ab_history_count,
                agent->max_response_chars);
            if (q.needs_revision && !retried) {
                retried = true;
                hu_daemon_quality_draft_keep(agent->alloc, key, key_len, *resp, *resp_len);
                *resp = NULL;
                *resp_len = 0;
                if (*convo) {
                    const char *hint =
                        q.guidance[0]    ? q.guidance
                        : q.brevity < 10 ? "Your response was much longer than their "
                                           "messages. Match their energy."
                        : q.warmth < 10  ? "Your response felt distant. Show you care."
                                         : "Your phrasing felt formal. Drop the "
                                           "formality.";
                    replay_ctx_prepend(alloc, convo, convo_len, hint, strlen(hint), "\n");
                    agent->conversation_context = *convo;
                    agent->conversation_context_len = *convo_len;
                }
                continue;
            }
        }
        break;
    }
    agent->tools_count = saved_tools;
    agent->tool_specs_count = saved_specs;
    out->retried = retried;
    return err;
}

/* Director: the call, the unknown-event guard, then the daemon's
 * silence/tapback routing. Returns true when no text turn should run. */
static bool replay_director(hu_allocator_t *alloc, hu_agent_t *agent, hu_channel_t *ch,
                            const hu_replay_turn_input_t *in, const char *combined,
                            size_t combined_len, hu_director_result_t *dr,
                            hu_replay_turn_result_t *out) {
    size_t key_len = strlen(in->contact_id);
    char situation[160] = "";
    if (hu_gate_mode_from_env("HU_DIRECTOR_FORMS", HU_GATE_OFF) != HU_GATE_OFF)
        (void)hu_expressive_situation(
            situation, sizeof(situation),
            hu_daemon_voice_first_available(agent, in->contact_id, key_len, false), true, false,
            false);
    out->director_valid =
        hu_daemon_director_decide(alloc, agent, ch, in->contact_id, key_len, combined, combined_len,
                                  in->history, in->history_count, situation, dr);
    if (!out->director_valid)
        return false;
    hu_expressive_unknown_event_guard(dr, combined, combined_len, in->history, in->history_count);
    if (dr->action == DIR_SILENCE) {
        if (!hu_daemon_director_silence_overridden(combined, combined_len)) {
            out->director = *dr;
            out->action = HU_REPLAY_ACTION_SILENCE;
            return true;
        }
        dr->action = DIR_TEXT;
    }
    out->director = *dr;
    if (dr->action == DIR_TAPBACK && dr->reaction != HU_REACTION_NONE) {
        out->action = HU_REPLAY_ACTION_TAPBACK;
        return true;
    }
    return false;
}

hu_error_t hu_replay_turn_run(hu_allocator_t *alloc, hu_agent_t *agent, const hu_config_t *config,
                              hu_replay_provider_t *rp, const hu_replay_turn_input_t *in,
                              hu_replay_turn_result_t *out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (!alloc || !agent || !in || !in->contact_id || !in->contact_id[0] || !in->inbound ||
        in->inbound_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    const char *key = in->contact_id;
    size_t key_len = strlen(key);
    char *combined = hu_strndup(alloc, in->inbound, in->inbound_len);
    if (!combined)
        return HU_ERR_OUT_OF_MEMORY;
    size_t combined_len = in->inbound_len;
    if (rp)
        hu_replay_provider_reset_turn(rp);

    hu_replay_channel_t rc;
    hu_replay_channel_init(&rc, in->history, in->history_count);
    hu_channel_t ch = hu_replay_channel_as_channel(&rc);
    hu_service_channel_t svc;
    memset(&svc, 0, sizeof(svc));
    svc.channel = &ch;
    svc.channel_ctx = &rc;

    hu_session_store_t *store = agent->session_store;
    agent->session_store = NULL; /* never read or write memory.db's session table */
    agent->lean_prompt = true;   /* llm_decides */
    hu_daemon_director_contact_boundary(agent, key, key_len);

    hu_director_result_t dr;
    memset(&dr, 0, sizeof(dr));
    char *resp = NULL;
    size_t resp_len = 0;
    hu_reactive_turn_ctx_t rt;
    memset(&rt, 0, sizeof(rt));
    char *convo = NULL;
    size_t convo_len = 0;
    if (in->director && replay_director(alloc, agent, &ch, in, combined, combined_len, &dr, out))
        goto done;
    out->action = HU_REPLAY_ACTION_TEXT;

    hu_moderation_result_t mod;
    memset(&mod, 0, sizeof(mod));
    bool crisis =
        hu_moderation_check(alloc, combined, combined_len, &mod) == HU_OK && mod.self_harm;

    hu_daemon_comfort_pending_t comfort[HU_COMFORT_PENDING_MAX];
    memset(comfort, 0, sizeof(comfort));
    hu_proactive_context_t proactive;
    memset(&proactive, 0, sizeof(proactive));
    hu_inner_thought_store_t inner;
    memset(&inner, 0, sizeof(inner));
    hu_repair_signal_t repair;
    memset(&repair, 0, sizeof(repair));
    rt.ch = &svc;
    rt.batch_key = key;
    rt.key_len = key_len;
    rt.combined = combined;
    rt.combined_len = combined_len;
    rt.llm_decides = true;
    rt.comfort_pending = comfort;
    rt.proactive_ctx = &proactive;
    hu_daemon_reactive_context_load(alloc, agent, config, &svc, 1, &rt);
    replay_seed_history(agent, in->history, in->history_count);
    rt.inner_thought_store = &inner;
    rt.repair_signal = &repair;
    hu_daemon_reactive_prompt_build(alloc, agent, config, &rt);
    convo = rt.convo_ctx;
    convo_len = rt.convo_ctx_len;
    rt.convo_ctx = NULL;
    hu_daemon_append_length_calibration(alloc, agent, key, key_len, combined, combined_len, false,
                                        &convo, &convo_len);
    if (rt.cross_channel_ctx) {
        replay_ctx_prepend(alloc, &convo, &convo_len, rt.cross_channel_ctx,
                           rt.cross_channel_ctx_len, "\n\n");
    }
    if (convo && convo_len > REPLAY_CONVO_CAP) {
        convo[REPLAY_CONVO_CAP] = '\0';
        convo_len = REPLAY_CONVO_CAP;
    }

    /* Media messages force brief mode (daemon.c media awareness). */
    uint32_t max_chars =
        hu_daemon_reply_budget(agent, &ch, key, key_len, combined_len, false,
                               hu_conversation_is_media_message(combined, combined_len, NULL, 0));
    if (!crisis) {
        hu_daemon_voice_first_t vf;
        hu_daemon_voice_first_prepare(alloc, agent, key, key_len, false, false, combined,
                                      combined_len, &convo, &convo_len, &max_chars, &rt, &vf);
        out->voice_memo = vf.memo;
        out->voice_reason = vf.reason;
    }
    out->max_chars = max_chars;
    agent->contact_context = rt.contact_ctx;
    agent->contact_context_len = rt.contact_ctx_len;
    agent->conversation_context = convo;
    agent->conversation_context_len = convo_len;
    agent->ab_history_entries = rt.history_entries;
    agent->ab_history_count = rt.history_count;
    agent->max_response_chars = max_chars;
    agent->voice_memo_turn = out->voice_memo;
    agent->history_msg_cap = 0;
    agent->self_test_turn = false;
    agent->memory_session_id = key;
    agent->memory_session_id_len = key_len;
    if (agent->memory && agent->memory->vtable) {
        agent->memory->current_session_id = key;
        agent->memory->current_session_id_len = key_len;
    }

    if (out->director_valid && out->voice_memo)
        hu_daemon_voice_first_direction(dr.direction, sizeof(dr.direction));
    if (out->director_valid)
        hu_daemon_director_arm_guard(alloc, agent, &dr, &convo, &convo_len);
    out->err = replay_agent_turn(alloc, agent, key, key_len, combined, combined_len, &convo,
                                 &convo_len, out->voice_memo, out, &resp, &resp_len);
    hu_daemon_director_end_turn(agent);

    if (out->err != HU_OK) {
        out->action = HU_REPLAY_ACTION_ERROR;
    } else if (!resp || resp_len == 0) {
        out->action = HU_REPLAY_ACTION_DROPPED;
    } else {
        size_t alloc_len = resp_len;
        replay_shape_reply(alloc, agent, key, key_len, in->seed, &resp, &resp_len, &alloc_len);
        if (resp_len == 0) {
            out->action = HU_REPLAY_ACTION_DROPPED;
        } else {
            out->text = hu_strndup(alloc, resp, resp_len);
            out->text_len = out->text ? resp_len : 0;
            replay_split(alloc, agent, &ch, max_chars, in->seed, resp, resp_len, out);
        }
        agent->alloc->free(agent->alloc->ctx, resp, alloc_len + 1);
        resp = NULL;
    }

done:
    if (resp)
        agent->alloc->free(agent->alloc->ctx, resp, resp_len + 1);
    hu_daemon_reactive_turn_end(agent);
    if (rt.contact_ctx)
        alloc->free(alloc->ctx, rt.contact_ctx, rt.contact_ctx_len + 1);
    if (convo)
        alloc->free(alloc->ctx, convo, convo_len + 1);
    if (rt.cross_channel_ctx)
        alloc->free(alloc->ctx, rt.cross_channel_ctx, rt.cross_channel_ctx_len + 1);
    if (rt.history_entries)
        alloc->free(alloc->ctx, rt.history_entries,
                    rt.history_count * sizeof(hu_channel_history_entry_t));
    alloc->free(alloc->ctx, combined, in->inbound_len + 1);
    agent->session_store = store;
    out->channel_outbound_calls = rc.outbound_calls;
    if (rp) {
        out->provider_calls = rp->calls;
        out->reply_calls = rp->reply_calls;
        out->reply_fp = rp->reply_fp;
        out->reply_bytes = rp->reply_bytes;
        out->reply_system_bytes = rp->reply_system_bytes;
    }
    return HU_OK;
}

void hu_replay_turn_result_deinit(hu_allocator_t *alloc, hu_replay_turn_result_t *r) {
    if (!alloc || !r)
        return;
    if (r->text)
        alloc->free(alloc->ctx, r->text, r->text_len + 1);
    for (size_t i = 0; i < r->bubble_count; i++) {
        if (r->bubbles[i])
            alloc->free(alloc->ctx, r->bubbles[i], r->bubble_lens[i] + 1);
    }
    memset(r, 0, sizeof(*r));
}
