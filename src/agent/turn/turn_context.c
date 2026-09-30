/* src/agent/turn/turn_context.c — S4 context builders, carved verbatim out of
 * hu_agent_turn (src/agent/agent_turn.c) by the phase-1 carve
 * (docs/superpowers/plans/2026-09-30-agent-turn-carve-phase1.md): STM,
 * commitments, pattern radar, proactive, superhuman + cross-channel identity,
 * adaptive/circadian, awareness + PWA, outcomes and AGI-frontier intelligence.
 *
 * Two `#ifndef HU_IS_TEST` local-hour reads run only in the daemon (tests use
 * hour = 10); tests/test_turn_sources.c pins both. Three borrowed SQLite
 * handles (contact graph, intelligence, experience) are passed to the module
 * APIs that own the SQL — the repository shape of contact_optout_repo.h; the
 * sqlite3 type comes through human/memory.h, never a direct <sqlite3.h>
 * include (plan gap G2). */
#include "../agent_internal.h"
#include "human/agent/awareness.h"
#include "human/agent/outcomes.h"
#include "human/agent/proactive.h"
#include "human/agent/turn.h"
#include "human/context/contact_style_overlay.h"
#include "human/context/emotional_state.h"
#include "human/core/log.h"
#include "human/core/string.h"
#include "human/memory/emotional_moments.h"
#include "human/memory/superhuman.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#if HU_HAS_PWA
#include "human/pwa_context.h"
#endif
#ifdef HU_ENABLE_SQLITE
#include "human/agent/goals.h"
#include "human/experience.h"
#include "human/intelligence/online_learning.h"
#include "human/intelligence/self_improve.h"
#include "human/intelligence/value_learning.h"
#include "human/intelligence/world_model.h"
#include "human/memory/contact_graph.h"
#if defined(HU_ENABLE_ML)
#include "human/ml/training_data.h"
#endif
#endif

hu_error_t hu_turn_context(hu_turn_ctx_t *turn_ctx) {
    if (!turn_ctx || !turn_ctx->in.agent)
        return HU_ERR_INVALID_ARGUMENT;
    hu_agent_t *agent = turn_ctx->in.agent;
    const char *msg = turn_ctx->in.msg;
    size_t msg_len = turn_ctx->in.msg_len;
    const char *plan_ctx = turn_ctx->context.plan_ctx;
    size_t plan_ctx_len = turn_ctx->context.plan_ctx_len;
    (void)msg; /* read only by the SQLite blocks below in some builds */
    (void)msg_len;
    (void)plan_ctx;
    (void)plan_ctx_len;
    /* Build STM context for this turn */
    char *stm_ctx = NULL;
    size_t stm_ctx_len = 0;
    hu_error_t stm_err = hu_stm_build_context(&agent->stm, agent->alloc, &stm_ctx, &stm_ctx_len);
    if (stm_err != HU_OK)
        hu_log_error("agent_turn", NULL, "STM context build failed: %s", hu_error_string(stm_err));
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
        session_count = agent->relationship.session_count;
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
            hu_error_t commit_err = hu_commitment_store_list_active(
                agent->commitment_store, agent->alloc, agent->memory_session_id,
                agent->memory_session_id_len, &commitments, &commitment_count);
            if (commit_err != HU_OK)
                hu_log_error("agent_turn", NULL, "commitment list failed: %s",
                             hu_error_string(commit_err));
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
        /* Superhuman memory: micro-moments, inside jokes, avoidance, topic absences, growth,
         * patterns */
        if (agent->memory && agent->memory_session_id && agent->memory_session_id_len > 0) {
            bool include_avoidance = false;
            include_avoidance = (agent->relationship.stage == HU_REL_TRUSTED ||
                                 agent->relationship.stage == HU_REL_DEEP);
            char *memory_sh_ctx = NULL;
            size_t memory_sh_len = 0;
            if (hu_superhuman_memory_build_context(agent->memory, agent->alloc,
                                                   agent->memory_session_id,
                                                   agent->memory_session_id_len, include_avoidance,
                                                   &memory_sh_ctx, &memory_sh_len) == HU_OK &&
                memory_sh_ctx && memory_sh_len > 0) {
                if (superhuman_ctx && superhuman_ctx_len > 0) {
                    size_t mem_sh_slen = strlen(memory_sh_ctx);
                    size_t content_len = superhuman_ctx_len + 2 + mem_sh_slen;
                    char *merged = (char *)agent->alloc->alloc(agent->alloc->ctx, content_len + 1);
                    if (merged) {
                        memcpy(merged, superhuman_ctx, superhuman_ctx_len);
                        merged[superhuman_ctx_len] = '\n';
                        merged[superhuman_ctx_len + 1] = '\n';
                        memcpy(merged + superhuman_ctx_len + 2, memory_sh_ctx, mem_sh_slen);
                        merged[content_len] = '\0';
                        agent->alloc->free(agent->alloc->ctx, superhuman_ctx,
                                           superhuman_ctx_len + 1);
                        agent->alloc->free(agent->alloc->ctx, memory_sh_ctx, memory_sh_len + 1);
                        superhuman_ctx = merged;
                        superhuman_ctx_len = content_len;
                    } else {
                        agent->alloc->free(agent->alloc->ctx, memory_sh_ctx, memory_sh_len + 1);
                    }
                } else {
                    superhuman_ctx = memory_sh_ctx;
                    superhuman_ctx_len = memory_sh_len;
                }
            } else if (memory_sh_ctx) {
                agent->alloc->free(agent->alloc->ctx, memory_sh_ctx, memory_sh_len + 1);
            }
        }
        /* Per-contact style evolution guidance */
        if (agent->memory && agent->memory_session_id && agent->memory_session_id_len > 0) {
            char *style_guidance = NULL;
            size_t style_guidance_len = 0;
            if (hu_superhuman_style_build_guidance(
                    agent->memory, agent->alloc, agent->memory_session_id,
                    agent->memory_session_id_len, &style_guidance, &style_guidance_len) == HU_OK &&
                style_guidance && style_guidance_len > 0) {
                if (superhuman_ctx && superhuman_ctx_len > 0) {
                    size_t content_len = superhuman_ctx_len + 1 + style_guidance_len;
                    char *merged = (char *)agent->alloc->alloc(agent->alloc->ctx, content_len + 1);
                    if (merged) {
                        memcpy(merged, superhuman_ctx, superhuman_ctx_len);
                        merged[superhuman_ctx_len] = '\n';
                        memcpy(merged + superhuman_ctx_len + 1, style_guidance, style_guidance_len);
                        merged[content_len] = '\0';
                        agent->alloc->free(agent->alloc->ctx, superhuman_ctx,
                                           superhuman_ctx_len + 1);
                        agent->alloc->free(agent->alloc->ctx, style_guidance,
                                           style_guidance_len + 1);
                        superhuman_ctx = merged;
                        superhuman_ctx_len = content_len;
                    } else {
                        agent->alloc->free(agent->alloc->ctx, style_guidance,
                                           style_guidance_len + 1);
                    }
                } else {
                    superhuman_ctx = style_guidance;
                    superhuman_ctx_len = style_guidance_len;
                }
            } else if (style_guidance) {
                agent->alloc->free(agent->alloc->ctx, style_guidance, style_guidance_len + 1);
            }
        }
        /* Cross-channel identity: merge canonical contact id from contact graph into superhuman ctx
         */
        if (agent->memory && agent->active_channel && agent->active_channel_len > 0 &&
            agent->memory_session_id && agent->memory_session_id_len > 0) {
            sqlite3 *cg_db = hu_sqlite_memory_get_db(agent->memory);
            if (cg_db) {
                char plat[64];
                char handle[256];
                size_t pl = agent->active_channel_len < sizeof(plat) - 1 ? agent->active_channel_len
                                                                         : sizeof(plat) - 1;
                memcpy(plat, agent->active_channel, pl);
                plat[pl] = '\0';
                size_t hl = agent->memory_session_id_len < sizeof(handle) - 1
                                ? agent->memory_session_id_len
                                : sizeof(handle) - 1;
                memcpy(handle, agent->memory_session_id, hl);
                handle[hl] = '\0';
                char canon[128];
                if (hu_contact_graph_resolve(cg_db, plat, handle, canon, sizeof(canon)) == HU_OK) {
                    char line[320];
                    int nw = snprintf(line, sizeof(line), "Cross-channel identity (canonical): %s",
                                      canon);
                    if (nw > 0 && (size_t)nw < sizeof(line)) {
                        size_t line_len = (size_t)nw;
                        if (superhuman_ctx && superhuman_ctx_len > 0) {
                            size_t content_len = superhuman_ctx_len + 1 + line_len;
                            char *merged =
                                (char *)agent->alloc->alloc(agent->alloc->ctx, content_len + 1);
                            if (merged) {
                                memcpy(merged, superhuman_ctx, superhuman_ctx_len);
                                merged[superhuman_ctx_len] = '\n';
                                memcpy(merged + superhuman_ctx_len + 1, line, line_len);
                                merged[content_len] = '\0';
                                agent->alloc->free(agent->alloc->ctx, superhuman_ctx,
                                                   superhuman_ctx_len + 1);
                                superhuman_ctx = merged;
                                superhuman_ctx_len = content_len;
                            }
                        } else {
                            char *dup =
                                (char *)agent->alloc->alloc(agent->alloc->ctx, line_len + 1);
                            if (dup) {
                                memcpy(dup, line, line_len);
                                dup[line_len] = '\0';
                                superhuman_ctx = dup;
                                superhuman_ctx_len = line_len;
                            }
                        }
                    }
                }
            }
        }
        /* Emotional moments due for check-in (contact-scoped) */
        if (agent->memory && agent->memory_session_id && agent->memory_session_id_len > 0) {
            hu_emotional_moment_t *due = NULL;
            size_t due_count = 0;
            int64_t now_ts = (int64_t)time(NULL);
            if (hu_emotional_moment_get_due(agent->alloc, agent->memory, now_ts, &due,
                                            &due_count) == HU_OK &&
                due && due_count > 0) {
                size_t contact_due = 0;
                for (size_t d = 0; d < due_count; d++) {
                    bool match = (strcmp(due[d].contact_id, agent->memory_session_id) == 0);
                    if (!match) {
                        const char *colon = strchr(agent->memory_session_id, ':');
                        if (colon && strcmp(due[d].contact_id, colon + 1) == 0)
                            match = true;
                    }
                    if (match)
                        contact_due++;
                }
                if (contact_due > 0) {
                    size_t em_len = 64 + contact_due * 128;
                    char *em_ctx = (char *)agent->alloc->alloc(agent->alloc->ctx, em_len);
                    if (em_ctx) {
                        size_t pos = 0;
                        pos = hu_buf_appendf(
                            em_ctx, em_len, pos,
                            "[Emotional check-in due] They shared something difficult "
                            "1–3 days ago. Consider a natural check-in:\n");
                        for (size_t d = 0; d < due_count && pos < em_len - 1; d++) {
                            bool match = (strcmp(due[d].contact_id, agent->memory_session_id) == 0);
                            if (!match) {
                                const char *colon = strchr(agent->memory_session_id, ':');
                                if (colon && strcmp(due[d].contact_id, colon + 1) == 0)
                                    match = true;
                            }
                            if (match) {
                                pos = hu_buf_appendf(em_ctx, em_len, pos,
                                                     "- Topic: %s, emotion: %s\n", due[d].topic,
                                                     due[d].emotion);
                            }
                        }
                        em_ctx[pos] = '\0';
                        if (pos > 0 && superhuman_ctx) {
                            size_t merged_len = superhuman_ctx_len + 2 + pos;
                            char *merged =
                                (char *)agent->alloc->alloc(agent->alloc->ctx, merged_len + 1);
                            if (merged) {
                                memcpy(merged, superhuman_ctx, superhuman_ctx_len);
                                merged[superhuman_ctx_len] = '\n';
                                merged[superhuman_ctx_len + 1] = '\n';
                                memcpy(merged + superhuman_ctx_len + 2, em_ctx, pos);
                                merged[merged_len] = '\0';
                                agent->alloc->free(agent->alloc->ctx, superhuman_ctx,
                                                   superhuman_ctx_len + 1);
                                agent->alloc->free(agent->alloc->ctx, em_ctx, em_len);
                                superhuman_ctx = merged;
                                superhuman_ctx_len = merged_len;
                            } else {
                                agent->alloc->free(agent->alloc->ctx, em_ctx, em_len);
                            }
                        } else if (pos > 0) {
                            superhuman_ctx = em_ctx;
                            superhuman_ctx_len = pos;
                        } else {
                            agent->alloc->free(agent->alloc->ctx, em_ctx, em_len);
                        }
                    }
                }
                agent->alloc->free(agent->alloc->ctx, due,
                                   due_count * sizeof(hu_emotional_moment_t));
            }
        }
        /* F26: If they're quiet and it's during their usual quiet hours, inject hint */
        if (agent->memory && agent->memory_session_id && agent->memory_session_id_len > 0 &&
            superhuman_ctx) {
            int qday = 0, qstart = 0, qend = 1;
            if (hu_superhuman_temporal_get_quiet_hours(
                    agent->memory, agent->alloc, agent->memory_session_id,
                    agent->memory_session_id_len, &qday, &qstart, &qend) == HU_OK) {
                time_t now_t = time(NULL);
                struct tm lt_buf;
                struct tm *lt = localtime_r(&now_t, &lt_buf);
                if (lt && lt->tm_wday == qday && lt->tm_hour >= qstart && lt->tm_hour < qend) {
                    static const char hint[] =
                        "\nThey're often quiet at this time. Don't worry if no reply.";
                    size_t hint_len = sizeof(hint) - 1;
                    size_t ctx_str_len = strlen(superhuman_ctx);
                    size_t new_len = ctx_str_len + hint_len;
                    char *merged = (char *)agent->alloc->alloc(agent->alloc->ctx, new_len + 1);
                    if (merged) {
                        memcpy(merged, superhuman_ctx, ctx_str_len);
                        memcpy(merged + ctx_str_len, hint, hint_len);
                        merged[new_len] = '\0';
                        agent->alloc->free(agent->alloc->ctx, superhuman_ctx,
                                           superhuman_ctx_len + 1);
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

    /* Append temporal mood to adaptive context */
    {
        char temporal_buf[256];
        time_t tnow = time(NULL);
        struct tm lt_buf2;
        struct tm *lt2 = localtime_r(&tnow, &lt_buf2);
        size_t temporal_len = hu_temporal_mood_build(lt2 ? (uint8_t)(lt2->tm_hour & 0xFF) : 12,
                                                     temporal_buf, sizeof(temporal_buf));
        if (temporal_len > 0) {
            if (adaptive_ctx) {
                size_t new_total = adaptive_ctx_len + temporal_len;
                char *merged = (char *)agent->alloc->alloc(agent->alloc->ctx, new_total + 1);
                if (merged) {
                    memcpy(merged, adaptive_ctx, adaptive_ctx_len);
                    memcpy(merged + adaptive_ctx_len, temporal_buf, temporal_len);
                    merged[new_total] = '\0';
                    agent->alloc->free(agent->alloc->ctx, adaptive_ctx, adaptive_ctx_len + 1);
                    adaptive_ctx = merged;
                    adaptive_ctx_len = new_total;
                }
            } else {
                adaptive_ctx = (char *)agent->alloc->alloc(agent->alloc->ctx, temporal_len + 1);
                if (adaptive_ctx) {
                    memcpy(adaptive_ctx, temporal_buf, temporal_len);
                    adaptive_ctx[temporal_len] = '\0';
                    adaptive_ctx_len = temporal_len;
                }
            }
        }
    }

    /* Append cross-conversation emotional carry-over to adaptive context */
#ifdef HU_ENABLE_SQLITE
    if (agent->memory && agent->memory_session_id && agent->memory_session_id_len > 0) {
        char *emo_carryover = NULL;
        size_t emo_carryover_len = 0;
        if (hu_emotional_state_get_recent(agent->alloc, agent->memory, agent->memory_session_id,
                                          agent->memory_session_id_len, &emo_carryover,
                                          &emo_carryover_len) == HU_OK &&
            emo_carryover && emo_carryover_len > 0) {
            if (adaptive_ctx) {
                size_t new_total = adaptive_ctx_len + emo_carryover_len;
                char *merged = (char *)agent->alloc->alloc(agent->alloc->ctx, new_total + 1);
                if (merged) {
                    memcpy(merged, adaptive_ctx, adaptive_ctx_len);
                    memcpy(merged + adaptive_ctx_len, emo_carryover, emo_carryover_len);
                    merged[new_total] = '\0';
                    agent->alloc->free(agent->alloc->ctx, adaptive_ctx, adaptive_ctx_len + 1);
                    adaptive_ctx = merged;
                    adaptive_ctx_len = new_total;
                }
            } else {
                adaptive_ctx = emo_carryover;
                adaptive_ctx_len = emo_carryover_len;
                emo_carryover = NULL;
            }
            if (emo_carryover)
                agent->alloc->free(agent->alloc->ctx, emo_carryover, emo_carryover_len + 1);
        }

        /* Append Seth's aggregate mood baseline */
        char *seth_mood = NULL;
        size_t seth_mood_len = 0;
        if (hu_emotional_state_get_seth_mood(agent->alloc, agent->memory, &seth_mood,
                                             &seth_mood_len) == HU_OK &&
            seth_mood && seth_mood_len > 0) {
            if (adaptive_ctx) {
                size_t new_total = adaptive_ctx_len + seth_mood_len;
                char *merged = (char *)agent->alloc->alloc(agent->alloc->ctx, new_total + 1);
                if (merged) {
                    memcpy(merged, adaptive_ctx, adaptive_ctx_len);
                    memcpy(merged + adaptive_ctx_len, seth_mood, seth_mood_len);
                    merged[new_total] = '\0';
                    agent->alloc->free(agent->alloc->ctx, adaptive_ctx, adaptive_ctx_len + 1);
                    adaptive_ctx = merged;
                    adaptive_ctx_len = new_total;
                }
            } else {
                adaptive_ctx = seth_mood;
                adaptive_ctx_len = seth_mood_len;
                seth_mood = NULL;
            }
            if (seth_mood)
                agent->alloc->free(agent->alloc->ctx, seth_mood, seth_mood_len + 1);
        }
    }
#endif

    /* Build situational awareness context */
    char *awareness_ctx = NULL;
    size_t awareness_ctx_len = 0;
    if (agent->awareness)
        awareness_ctx = hu_awareness_context(agent->awareness, agent->alloc, &awareness_ctx_len);

    /* Build cross-app PWA context */
#if HU_HAS_PWA
    {
        char *pwa_ctx = NULL;
        size_t pwa_ctx_len = 0;
        hu_error_t pwa_err = hu_pwa_context_build(agent->alloc, &pwa_ctx, &pwa_ctx_len);
        if (pwa_err == HU_OK && pwa_ctx && pwa_ctx_len > 0) {
            if (awareness_ctx) {
                /* Append PWA context to existing awareness */
                size_t total = awareness_ctx_len + 1 + pwa_ctx_len;
                char *merged = (char *)agent->alloc->alloc(agent->alloc->ctx, total + 1);
                if (merged) {
                    memcpy(merged, awareness_ctx, awareness_ctx_len);
                    merged[awareness_ctx_len] = '\n';
                    memcpy(merged + awareness_ctx_len + 1, pwa_ctx, pwa_ctx_len);
                    merged[total] = '\0';
                    agent->alloc->free(agent->alloc->ctx, awareness_ctx, awareness_ctx_len + 1);
                    awareness_ctx = merged;
                    awareness_ctx_len = total;
                }
            } else {
                /* Use PWA context as the awareness context */
                awareness_ctx = pwa_ctx;
                awareness_ctx_len = pwa_ctx_len;
                pwa_ctx = NULL; /* ownership transferred */
            }
        }
        if (pwa_ctx)
            agent->alloc->free(agent->alloc->ctx, pwa_ctx, pwa_ctx_len + 1);
    }
#endif

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

#if defined(HU_ENABLE_ML)
            /* Start RL trajectory for this turn (ensure tables exist first) */
            {
                (void)hu_training_data_init_tables(intel_db);
                int64_t traj_id = 0;
                if (hu_training_data_start_trajectory(agent->alloc, intel_db, &traj_id) == HU_OK)
                    agent->sota.current_trajectory_id = traj_id;
            }
#endif

            /* Self-improvement: active prompt patches */
            {
                hu_self_improve_t si;
                if (hu_self_improve_create(agent->alloc, intel_db, &si) == HU_OK) {
                    char *patches = NULL;
                    size_t patches_len = 0;
                    if (hu_self_improve_get_prompt_patches(&si, &patches, &patches_len) == HU_OK &&
                        patches && patches_len > 0) {
                        int n =
                            snprintf(parts + pos, sizeof(parts) - pos,
                                     "### Learned Behaviors\n%.*s\n", (int)patches_len, patches);
                        if (n > 0 && pos + (size_t)n < sizeof(parts))
                            pos += (size_t)n;
                        agent->alloc->free(agent->alloc->ctx, patches, patches_len + 1);
                    }
                    char *tool_prefs = NULL;
                    size_t tool_prefs_len = 0;
                    if (hu_self_improve_get_tool_prefs_prompt(&si, &tool_prefs, &tool_prefs_len) ==
                            HU_OK &&
                        tool_prefs && tool_prefs_len > 0) {
                        int n = snprintf(parts + pos, sizeof(parts) - pos, "\n%s\n", tool_prefs);
                        if (n > 0 && pos + (size_t)n < sizeof(parts))
                            pos += (size_t)n;
                        agent->alloc->free(agent->alloc->ctx, tool_prefs, tool_prefs_len + 1);
                    }
                    hu_self_improve_deinit(&si);
                }
            }

            /* Goals: active goals context — P3-1 scopes per-contact via memory_session_id. */
            {
                hu_goal_engine_t ge;
                if (hu_goal_engine_create(agent->alloc, intel_db, &ge) == HU_OK) {
                    char *gctx = NULL;
                    size_t gctx_len = 0;
                    const char *goal_cid = agent->memory_session_id ? agent->memory_session_id : "";
                    size_t goal_cid_len =
                        agent->memory_session_id ? agent->memory_session_id_len : 0;
                    if (hu_goal_build_context(&ge, goal_cid, goal_cid_len, &gctx, &gctx_len) ==
                            HU_OK &&
                        gctx && gctx_len > 0) {
                        int n = snprintf(parts + pos, sizeof(parts) - pos, "### %.*s\n",
                                         (int)gctx_len, gctx);
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
                    if (hu_online_learning_build_context(&ol, &lctx, &lctx_len) == HU_OK && lctx &&
                        lctx_len > 0) {
                        int n = snprintf(parts + pos, sizeof(parts) - pos, "### %.*s\n",
                                         (int)lctx_len, lctx);
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
                    if (hu_value_build_prompt(&ve, &vctx, &vctx_len) == HU_OK && vctx &&
                        vctx_len > 0) {
                        int n = snprintf(parts + pos, sizeof(parts) - pos, "### %.*s\n",
                                         (int)vctx_len, vctx);
                        if (n > 0 && pos + (size_t)n < sizeof(parts))
                            pos += (size_t)n;
                        agent->alloc->free(agent->alloc->ctx, vctx, vctx_len + 1);
                    }
                    hu_value_engine_deinit(&ve);
                }
            }

            /* World model: predict likely outcome of this request */
            {
                hu_causal_world_model_t wm;
                if (hu_causal_world_model_create(agent->alloc, intel_db, &wm) == HU_OK) {
                    hu_wm_prediction_t pred = {0};
                    double ctx_threshold = 0.3;
#ifdef HU_ENABLE_SQLITE
                    ctx_threshold = agent->meta_params.default_confidence_threshold * 0.6;
                    if (ctx_threshold < 0.1)
                        ctx_threshold = 0.1;
#endif
                    if (hu_world_simulate(&wm, msg, msg_len, NULL, 0, &pred) == HU_OK &&
                        pred.confidence > ctx_threshold) {
                        int n =
                            snprintf(parts + pos, sizeof(parts) - pos,
                                     "### Predicted Outcome\n"
                                     "Based on past patterns, this request likely leads to: %.*s "
                                     "(confidence: %.0f%%)\n",
                                     (int)(pred.outcome_len < 200 ? pred.outcome_len : 200),
                                     pred.outcome, pred.confidence * 100.0);
                        if (n > 0 && pos + (size_t)n < sizeof(parts))
                            pos += (size_t)n;
                    }
                    hu_causal_world_model_deinit(&wm);
                }
            }

            /* Experience: recall similar past experiences (semantic when available) */
            {
                hu_experience_store_t exp_store;
                if (hu_agent_internal_experience_init(agent, &exp_store) == HU_OK) {
#ifdef HU_ENABLE_SQLITE
                    sqlite3 *exp_db = hu_sqlite_memory_get_db(agent->memory);
                    if (exp_db)
                        exp_store.db = exp_db;
#endif
                    char *exp_prompt = NULL;
                    size_t exp_prompt_len = 0;
                    if (hu_experience_build_prompt(&exp_store, msg, msg_len, &exp_prompt,
                                                   &exp_prompt_len) == HU_OK &&
                        exp_prompt && exp_prompt_len > 0) {
                        int n = snprintf(parts + pos, sizeof(parts) - pos, "### %.*s\n",
                                         (int)exp_prompt_len, exp_prompt);
                        if (n > 0 && pos + (size_t)n < sizeof(parts))
                            pos += (size_t)n;
                        agent->alloc->free(agent->alloc->ctx, exp_prompt, exp_prompt_len + 1);
                    } else if (exp_prompt) {
                        agent->alloc->free(agent->alloc->ctx, exp_prompt, 1);
                    }
                    hu_experience_store_deinit(&exp_store);
                }
            }

            if (plan_ctx && plan_ctx_len > 0) {
                int n = snprintf(parts + pos, sizeof(parts) - pos, "### %.*s\n", (int)plan_ctx_len,
                                 plan_ctx);
                if (n > 0 && pos + (size_t)n < sizeof(parts))
                    pos += (size_t)n;
            }

            if (pos > 0) {
                intelligence_ctx = hu_strndup(agent->alloc, parts, pos);
                intelligence_ctx_len = pos;
            }
        }
    }
#endif

    turn_ctx->context.stm_ctx = stm_ctx;
    turn_ctx->context.stm_ctx_len = stm_ctx_len;
    turn_ctx->context.commitment_ctx = commitment_ctx;
    turn_ctx->context.commitment_ctx_len = commitment_ctx_len;
    turn_ctx->context.pattern_ctx = pattern_ctx;
    turn_ctx->context.pattern_ctx_len = pattern_ctx_len;
    turn_ctx->context.proactive_ctx = proactive_ctx;
    turn_ctx->context.proactive_ctx_len = proactive_ctx_len;
    turn_ctx->context.superhuman_ctx = superhuman_ctx;
    turn_ctx->context.superhuman_ctx_len = superhuman_ctx_len;
    turn_ctx->context.adaptive_ctx = adaptive_ctx;
    turn_ctx->context.adaptive_ctx_len = adaptive_ctx_len;
    turn_ctx->context.awareness_ctx = awareness_ctx;
    turn_ctx->context.awareness_ctx_len = awareness_ctx_len;
    turn_ctx->context.outcome_ctx = outcome_ctx;
    turn_ctx->context.outcome_ctx_len = outcome_ctx_len;
    turn_ctx->context.intelligence_ctx = intelligence_ctx;
    turn_ctx->context.intelligence_ctx_len = intelligence_ctx_len;
    return HU_OK;
}
