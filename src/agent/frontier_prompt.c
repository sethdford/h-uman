#include "human/agent/frontier_prompt.h"
#include "agent_internal.h"
#include "human/cognition/attachment.h"
#include "human/cognition/novelty.h"
#include "human/cognition/presence.h"
#include "human/cognition/rupture_repair.h"
#include "human/humanness.h"
#include "human/memory/relational_episode.h"
#include "human/persona.h"
#include "human/persona/creative_voice.h"
#include "human/persona/genuine_boundaries.h"
#include "human/persona/micro_expression.h"
#include "human/persona/narrative_self.h"
#include "human/agent/growth_narrative.h"
#include "human/core/string.h"
#include "human/memory/stm.h"
#ifdef HU_ENABLE_SQLITE
#include "human/memory.h"
#include "human/memory/evolved_opinions.h"
#include <sqlite3.h>
#endif
#include <string.h>
#include <time.h>

static void build_humanness_ctx(hu_allocator_t *alloc, hu_agent_t *agent,
                                const char *msg, size_t msg_len,
                                const char *memory_ctx, size_t memory_ctx_len,
                                hu_frontier_prompt_bundle_t *out)
{
    char hum_buf[4096];
    size_t hum_pos = 0;

    if (memory_ctx && memory_ctx_len > 0) {
        hu_shared_reference_t *refs = NULL;
        size_t ref_count = 0;
        if (hu_shared_references_find(alloc,
                                      agent->memory_session_id ? agent->memory_session_id : "",
                                      agent->memory_session_id_len, msg, msg_len, memory_ctx,
                                      memory_ctx_len, &refs, &ref_count, 3) == HU_OK &&
            ref_count > 0) {
            size_t dir_len = 0;
            char *dir =
                hu_shared_references_build_directive(alloc, refs, ref_count, &dir_len);
            if (dir && dir_len > 0 && hum_pos + dir_len + 2 < sizeof(hum_buf)) {
                memcpy(hum_buf + hum_pos, dir, dir_len);
                hum_pos += dir_len;
                hum_buf[hum_pos++] = '\n';
                hum_buf[hum_pos++] = '\n';
            }
            if (dir)
                alloc->free(alloc->ctx, dir, dir_len + 1);
            hu_shared_references_free(alloc, refs, ref_count);
        }
    }

    if (memory_ctx && memory_ctx_len > 0) {
        hu_curiosity_prompt_t *prompts = NULL;
        size_t cur_count = 0;
        if (hu_curiosity_generate(alloc,
                                  agent->memory_session_id ? agent->memory_session_id : "",
                                  agent->memory_session_id_len, memory_ctx, memory_ctx_len, msg,
                                  msg_len, &prompts, &cur_count, 2) == HU_OK &&
            cur_count > 0) {
            size_t dir_len = 0;
            char *dir =
                hu_curiosity_build_directive(alloc, prompts, cur_count, &dir_len);
            if (dir && dir_len > 0 && hum_pos + dir_len + 2 < sizeof(hum_buf)) {
                memcpy(hum_buf + hum_pos, dir, dir_len);
                hum_pos += dir_len;
                hum_buf[hum_pos++] = '\n';
                hum_buf[hum_pos++] = '\n';
            }
            if (dir)
                alloc->free(alloc->ctx, dir, dir_len + 1);
            hu_curiosity_prompts_free(alloc, prompts, cur_count);
        }
    }

    if (msg_len >= 15) {
        hu_absence_signal_t *abs_signals = NULL;
        size_t abs_count = 0;
        if (hu_absence_detect(alloc, msg, msg_len, &abs_signals, &abs_count) == HU_OK &&
            abs_count > 0) {
            size_t dir_len = 0;
            char *dir =
                hu_absence_build_directive(alloc, abs_signals, abs_count, &dir_len);
            if (dir && dir_len > 0 && hum_pos + dir_len + 2 < sizeof(hum_buf)) {
                memcpy(hum_buf + hum_pos, dir, dir_len);
                hum_pos += dir_len;
                hum_buf[hum_pos++] = '\n';
                hum_buf[hum_pos++] = '\n';
            }
            if (dir)
                alloc->free(alloc->ctx, dir, dir_len + 1);
            hu_absence_signals_free(alloc, abs_signals, abs_count);
        }
    }

#ifdef HU_ENABLE_SQLITE
    if (agent->memory) {
        sqlite3 *eo_db = hu_sqlite_memory_get_db(agent->memory);
        if (eo_db) {
            hu_evolved_opinions_ensure_table(eo_db);
            hu_evolved_opinion_t *opinions = NULL;
            size_t op_count = 0;
            if (hu_evolved_opinions_get(alloc, eo_db, 0.4, 5, &opinions, &op_count) ==
                    HU_OK &&
                opinions && op_count > 0) {
                size_t dir_len = 0;
                char *dir = hu_evolved_opinion_build_directive(alloc, opinions, op_count,
                                                               0.4, &dir_len);
                if (dir && dir_len > 0 && hum_pos + dir_len + 2 < sizeof(hum_buf)) {
                    memcpy(hum_buf + hum_pos, dir, dir_len);
                    hum_pos += dir_len;
                    hum_buf[hum_pos++] = '\n';
                    hum_buf[hum_pos++] = '\n';
                }
                if (dir)
                    alloc->free(alloc->ctx, dir, dir_len + 1);
                hu_evolved_opinions_free(alloc, opinions, op_count);
            }
        }
    }

    if (agent->memory && agent->memory_session_id) {
        sqlite3 *rc_db = hu_sqlite_memory_get_db(agent->memory);
        if (rc_db) {
            sqlite3_stmt *rc_stmt = NULL;
            const char *rc_sql = "SELECT valence, intensity, created_at FROM emotional_residues"
                                 " WHERE contact_id = ?1 ORDER BY created_at DESC LIMIT 10";
            if (sqlite3_prepare_v2(rc_db, rc_sql, -1, &rc_stmt, NULL) == SQLITE_OK) {
                sqlite3_bind_text(rc_stmt, 1, agent->memory_session_id,
                                  (int)agent->memory_session_id_len, SQLITE_STATIC);
                double valences[10];
                double intensities[10];
                int64_t timestamps[10];
                size_t rc_count = 0;
                while (sqlite3_step(rc_stmt) == SQLITE_ROW && rc_count < 10) {
                    valences[rc_count] = sqlite3_column_double(rc_stmt, 0);
                    intensities[rc_count] = sqlite3_column_double(rc_stmt, 1);
                    timestamps[rc_count] = sqlite3_column_int64(rc_stmt, 2);
                    rc_count++;
                }
                sqlite3_finalize(rc_stmt);
                if (rc_count > 0) {
                    hu_residue_carryover_t carryover = {0};
                    if (hu_residue_carryover_compute(valences, intensities, timestamps,
                                                     rc_count, (int64_t)time(NULL),
                                                     &carryover) == HU_OK) {
                        out->residue_dir = hu_residue_carryover_build_directive(
                            alloc, &carryover, &out->residue_dir_len);
                    }
                }
            }
        }
    }
#endif

    {
        uint32_t tool_count = agent->tools_count > 0 ? (uint32_t)agent->tools_count : 0;
        hu_certainty_level_t cert = hu_certainty_classify(
            msg, msg_len, (memory_ctx != NULL && memory_ctx_len > 0), tool_count);
        out->imperfect_dir = hu_imperfect_delivery_directive(alloc, cert,
                                                             &out->imperfect_dir_len);
    }

    if (hum_pos > 0) {
        hum_buf[hum_pos] = '\0';
        out->humanness_ctx = hu_strndup(alloc, hum_buf, hum_pos);
        if (out->humanness_ctx)
            out->humanness_ctx_len = hum_pos;
    }
}

static void build_presence_and_micro(hu_allocator_t *alloc, hu_agent_t *agent,
                                     hu_frontier_prompt_bundle_t *out)
{
    float f8_vulnerability = 0.0f;
    uint32_t f8_rel_depth = 0;
    if (agent->persona && agent->memory_session_id) {
        const hu_contact_profile_t *f8_cp = hu_persona_find_contact(
            agent->persona, agent->memory_session_id, agent->memory_session_id_len);
        if (f8_cp) {
            if (f8_cp->vulnerability_level) {
                if (strstr(f8_cp->vulnerability_level, "high"))
                    f8_vulnerability = 0.8f;
                else if (strstr(f8_cp->vulnerability_level, "medium"))
                    f8_vulnerability = 0.5f;
                else
                    f8_vulnerability = 0.2f;
            }
            if (f8_cp->relationship_stage) {
                if (strstr(f8_cp->relationship_stage, "deep"))
                    f8_rel_depth = 8;
                else if (strstr(f8_cp->relationship_stage, "trusted"))
                    f8_rel_depth = 6;
                else if (strstr(f8_cp->relationship_stage, "familiar"))
                    f8_rel_depth = 4;
                else
                    f8_rel_depth = 1;
            }
        }
    }

    hu_presence_state_t pres;
    hu_presence_init(&pres);
    hu_presence_compute(&pres, agent->infra.emotional_cognition.state.intensity,
                        f8_vulnerability, 0.0f,
                        agent->infra.emotional_cognition.state.intensity, f8_rel_depth);
    hu_presence_build_context(alloc, &pres, &out->presence_ctx, &out->presence_ctx_len);
    hu_presence_deinit(alloc, &pres);

    {
        float f9_closeness = (float)f8_rel_depth / 10.0f;
        if (f9_closeness > 1.0f)
            f9_closeness = 1.0f;
        hu_micro_expression_t mexp;
        hu_micro_expression_init(&mexp);
        hu_micro_expression_compute(
            &mexp, agent->frontiers.somatic.energy, agent->frontiers.somatic.social_battery,
            agent->infra.emotional_cognition.state.valence,
            agent->infra.emotional_cognition.state.intensity, f9_closeness);
        hu_micro_expression_build_context(alloc, &mexp, &out->micro_expr_ctx,
                                          &out->micro_expr_ctx_len);
        hu_micro_expression_deinit(alloc, &mexp);
    }
}

static void build_novelty(hu_allocator_t *alloc, hu_agent_t *agent,
                           const char *msg, size_t msg_len,
                           hu_frontier_prompt_bundle_t *out)
{
    const char *stm_topic_ptrs[HU_STM_MAX_TOPICS];
    size_t stm_topic_n = 0;
    for (size_t ti = 0; ti < agent->stm.topic_count && ti < HU_STM_MAX_TOPICS; ti++) {
        if (agent->stm.topics[ti])
            stm_topic_ptrs[stm_topic_n++] = agent->stm.topics[ti];
    }
    hu_novelty_signal_t nsig = {0};
    hu_novelty_evaluate(alloc, &agent->frontiers.novelty, msg, msg_len,
                        stm_topic_ptrs, stm_topic_n, stm_topic_ptrs, stm_topic_n, &nsig);
    if (nsig.surprise_prompt) {
        out->novelty_ctx =
            hu_strndup(alloc, nsig.surprise_prompt, strlen(nsig.surprise_prompt));
        if (out->novelty_ctx)
            out->novelty_ctx_len = strlen(out->novelty_ctx);
    }
    hu_novelty_signal_free(alloc, &nsig);
}

static void build_boundary(hu_allocator_t *alloc, hu_agent_t *agent,
                            const char *msg, size_t msg_len,
                            hu_frontier_prompt_bundle_t *out)
{
    uint32_t boundary_rel_stage = 0;
    if (agent->persona && agent->memory_session_id) {
        const hu_contact_profile_t *bcp = hu_persona_find_contact(
            agent->persona, agent->memory_session_id, agent->memory_session_id_len);
        if (bcp && bcp->relationship_stage) {
            if (strstr(bcp->relationship_stage, "deep"))
                boundary_rel_stage = 3;
            else if (strstr(bcp->relationship_stage, "trusted") ||
                     strstr(bcp->relationship_stage, "familiar"))
                boundary_rel_stage = 2;
            else
                boundary_rel_stage = 1;
        }
    }
    const hu_genuine_boundary_t *matched = NULL;
    hu_genuine_boundary_check_relevance(&agent->frontiers.boundaries, msg, msg_len,
                                        &matched);
    if (matched)
        hu_genuine_boundary_build_context(alloc, matched, boundary_rel_stage,
                                          &out->boundary_ctx, &out->boundary_ctx_len);
}

static void build_relational_episodes(hu_allocator_t *alloc, hu_agent_t *agent,
                                       hu_frontier_prompt_bundle_t *out)
{
#ifdef HU_ENABLE_SQLITE
    if (agent->memory && agent->memory_session_id && agent->memory_session_id_len > 0) {
        sqlite3 *ep_db = hu_sqlite_memory_get_db(agent->memory);
        if (ep_db) {
            static const char ep_sql[] =
                "SELECT summary, felt_sense, relational_meaning, significance, warmth, "
                "timestamp FROM relational_episodes WHERE contact_id = ?1 "
                "ORDER BY significance DESC LIMIT 5";
            sqlite3_stmt *ep_stmt = NULL;
            int ep_rc = sqlite3_prepare_v2(ep_db, ep_sql, -1, &ep_stmt, NULL);
            if (ep_rc == SQLITE_OK) {
                sqlite3_bind_text(ep_stmt, 1, agent->memory_session_id,
                                  (int)agent->memory_session_id_len, SQLITE_STATIC);
                hu_relational_episode_t loaded_eps[5];
                size_t ep_n = 0;
                while (sqlite3_step(ep_stmt) == SQLITE_ROW && ep_n < 5) {
                    hu_relational_episode_init(&loaded_eps[ep_n]);
                    const char *s = (const char *)sqlite3_column_text(ep_stmt, 0);
                    const char *f = (const char *)sqlite3_column_text(ep_stmt, 1);
                    const char *r = (const char *)sqlite3_column_text(ep_stmt, 2);
                    float sig = (float)sqlite3_column_double(ep_stmt, 3);
                    float wrm = (float)sqlite3_column_double(ep_stmt, 4);
                    uint64_t ts = (uint64_t)sqlite3_column_int64(ep_stmt, 5);
                    hu_relational_episode_set(alloc, &loaded_eps[ep_n],
                                              agent->memory_session_id, s ? s : "", f ? f : "",
                                              r ? r : "", sig, wrm, ts);
                    ep_n++;
                }
                sqlite3_finalize(ep_stmt);
                if (ep_n > 0)
                    hu_relational_episode_build_context(alloc, loaded_eps, ep_n,
                                                        &out->rel_episode_ctx,
                                                        &out->rel_episode_ctx_len);
                for (size_t epi = 0; epi < ep_n; epi++)
                    hu_relational_episode_free(alloc, &loaded_eps[epi]);
            }
        }
    }
    if (!out->rel_episode_ctx)
#endif
    {
        hu_relational_episode_build_context(alloc, NULL, 0, &out->rel_episode_ctx,
                                            &out->rel_episode_ctx_len);
    }
}

hu_error_t hu_frontier_prompt_build(hu_allocator_t *alloc, hu_agent_t *agent,
                                    const char *msg, size_t msg_len,
                                    const char *memory_ctx, size_t memory_ctx_len,
                                    hu_frontier_prompt_bundle_t *out)
{
    if (!alloc || !agent || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    build_humanness_ctx(alloc, agent, msg, msg_len, memory_ctx, memory_ctx_len, out);
    build_presence_and_micro(alloc, agent, out);
    build_novelty(alloc, agent, msg, msg_len, out);

    hu_attachment_build_context(alloc, &agent->frontiers.attachment, &out->attachment_ctx,
                                &out->attachment_ctx_len);
    hu_rupture_build_context(alloc, &agent->frontiers.rupture, &out->rupture_ctx,
                             &out->rupture_ctx_len);
    hu_narrative_self_build_context(alloc, &agent->frontiers.narrative,
                                    &out->narrative_self_ctx, &out->narrative_self_ctx_len);
    hu_creative_voice_build_context(alloc, &agent->frontiers.creative_voice,
                                    &out->creative_voice_ctx, &out->creative_voice_ctx_len);
    hu_growth_narrative_build_context(alloc, &agent->frontiers.growth,
                                      agent->memory_session_id, &out->growth_ctx,
                                      &out->growth_ctx_len);

    build_boundary(alloc, agent, msg, msg_len, out);
    build_relational_episodes(alloc, agent, out);

    return HU_OK;
}

void hu_frontier_prompt_free(hu_allocator_t *alloc, hu_frontier_prompt_bundle_t *b)
{
    if (!alloc || !b)
        return;
    if (b->humanness_ctx)
        alloc->free(alloc->ctx, b->humanness_ctx, b->humanness_ctx_len + 1);
    if (b->imperfect_dir)
        alloc->free(alloc->ctx, b->imperfect_dir, b->imperfect_dir_len + 1);
    if (b->residue_dir)
        alloc->free(alloc->ctx, b->residue_dir, b->residue_dir_len + 1);
    if (b->presence_ctx)
        alloc->free(alloc->ctx, b->presence_ctx, b->presence_ctx_len + 1);
    if (b->micro_expr_ctx)
        alloc->free(alloc->ctx, b->micro_expr_ctx, b->micro_expr_ctx_len + 1);
    if (b->novelty_ctx)
        alloc->free(alloc->ctx, b->novelty_ctx, b->novelty_ctx_len + 1);
    if (b->attachment_ctx)
        alloc->free(alloc->ctx, b->attachment_ctx, b->attachment_ctx_len + 1);
    if (b->rupture_ctx)
        alloc->free(alloc->ctx, b->rupture_ctx, b->rupture_ctx_len + 1);
    if (b->narrative_self_ctx)
        alloc->free(alloc->ctx, b->narrative_self_ctx, b->narrative_self_ctx_len + 1);
    if (b->creative_voice_ctx)
        alloc->free(alloc->ctx, b->creative_voice_ctx, b->creative_voice_ctx_len + 1);
    if (b->growth_ctx)
        alloc->free(alloc->ctx, b->growth_ctx, b->growth_ctx_len + 1);
    if (b->boundary_ctx)
        alloc->free(alloc->ctx, b->boundary_ctx, b->boundary_ctx_len + 1);
    if (b->rel_episode_ctx)
        alloc->free(alloc->ctx, b->rel_episode_ctx, b->rel_episode_ctx_len + 1);
    memset(b, 0, sizeof(*b));
}
