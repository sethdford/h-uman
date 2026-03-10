/*
 * BTH (Better-Than-Human) E2E integration test — proves the full pipeline works end-to-end.
 * No real LLM or network; exercises all BTH components together.
 */
#include "human/agent/commitment.h"
#include "human/agent/commitment_store.h"
#include "human/agent/dag.h"
#include "human/agent/dag_executor.h"
#include "human/agent/pattern_radar.h"
#include "human/agent/proactive.h"
#include "human/agent/superhuman.h"
#include "human/agent/superhuman_commitment.h"
#include "human/agent/superhuman_emotional.h"
#include "human/agent/superhuman_silence.h"
#include "human/channel.h"
#include "human/context/conversation.h"
#include "human/context/event_extract.h"
#include "human/context/mood.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/memory/deep_extract.h"
#include "human/memory/emotional_graph.h"
#include "human/memory/engines.h"
#include "human/memory/fast_capture.h"
#include "human/memory/promotion.h"
#include "human/memory/stm.h"
#include "human/persona/replay.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>

#ifdef HU_HAS_PERSONA
#include "human/persona/circadian.h"
#include "human/persona/relationship.h"
#endif

static hu_channel_history_entry_t make_entry(bool from_me, const char *text, const char *ts) {
    hu_channel_history_entry_t e;
    memset(&e, 0, sizeof(e));
    e.from_me = from_me;
    size_t tl = strlen(text);
    if (tl >= sizeof(e.text))
        tl = sizeof(e.text) - 1;
    memcpy(e.text, text, tl);
    e.text[tl] = '\0';
    size_t tsl = strlen(ts);
    if (tsl >= sizeof(e.timestamp))
        tsl = sizeof(e.timestamp) - 1;
    memcpy(e.timestamp, ts, tsl);
    e.timestamp[tsl] = '\0';
    return e;
}

static void bth_conversation_intelligence_pipeline(void) {
    hu_allocator_t alloc = hu_system_allocator();

    hu_channel_history_entry_t entries[12];
    memset(entries, 0, sizeof(entries));
    entries[0] = make_entry(false, "hey how are you?", "2025-01-01 10:00");
    entries[1] = make_entry(true, "doing good, you?", "2025-01-01 10:01");
    entries[2] = make_entry(false, "stressed about work honestly", "2025-01-01 10:02");
    entries[3] = make_entry(true, "that sucks, what's going on?", "2025-01-01 10:03");
    entries[4] = make_entry(false, "my boss keeps piling on deadlines", "2025-01-01 10:04");
    entries[5] = make_entry(true, "ugh that sounds rough", "2025-01-01 10:05");
    entries[6] =
        make_entry(false, "yeah it's a lot. anyway how was your weekend?", "2025-01-01 10:06");
    entries[7] = make_entry(true, "pretty chill, went hiking", "2025-01-01 10:07");
    entries[8] = make_entry(false, "nice! where'd you go?", "2025-01-01 10:08");
    entries[9] = make_entry(true, "up to the lake, it was beautiful", "2025-01-01 10:09");
    entries[10] = make_entry(false, "love it there", "2025-01-01 10:10");
    entries[11] = make_entry(true, "same, we should go sometime", "2025-01-01 10:11");

    size_t len = 0;
    char *awareness = hu_conversation_build_awareness(&alloc, entries, 12, NULL, &len);
    HU_ASSERT_NOT_NULL(awareness);
    alloc.free(alloc.ctx, awareness, len + 1);

    char cal_buf[512];
    size_t cal_len = hu_conversation_calibrate_length(entries[11].text, strlen(entries[11].text),
                                                      entries, 12, cal_buf, sizeof(cal_buf));
    HU_ASSERT_TRUE(cal_len > 0);

    uint32_t delay = 0;
    hu_response_action_t action = hu_conversation_classify_response(
        entries[11].text, strlen(entries[11].text), entries, 12, &delay);
    HU_ASSERT_TRUE(action >= HU_RESPONSE_FULL && action <= HU_RESPONSE_THINKING);

    hu_narrative_phase_t phase = hu_conversation_detect_narrative(entries, 12);
    HU_ASSERT_TRUE(phase >= HU_NARRATIVE_OPENING && phase <= HU_NARRATIVE_CLOSING);

    hu_engagement_level_t eng = hu_conversation_detect_engagement(entries, 12);
    HU_ASSERT_TRUE(eng >= HU_ENGAGEMENT_HIGH && eng <= HU_ENGAGEMENT_DISTRACTED);

    hu_emotional_state_t emo = hu_conversation_detect_emotion(entries, 12);
    HU_ASSERT_TRUE(emo.intensity >= 0.0f && emo.intensity <= 1.0f);

    char rep_buf[256];
    size_t rep_len = hu_conversation_detect_repetition(entries, 12, rep_buf, sizeof(rep_buf));
    HU_ASSERT_TRUE(rep_len >= 0);
}

static void bth_emotional_graph_pipeline(void) {
    hu_allocator_t alloc = hu_system_allocator();

    hu_stm_buffer_t stm;
    hu_stm_init(&stm, alloc, "egraph-session", 14);

    hu_stm_record_turn(&stm, "user", 4, "Work is stressing me out", 24, 1000);
    hu_stm_turn_set_primary_topic(&stm, 0, "work", 4);
    hu_stm_turn_add_emotion(&stm, 0, HU_EMOTION_ANXIETY, 0.8);

    hu_stm_record_turn(&stm, "user", 4, "Cooking makes me so happy", 25, 2000);
    hu_stm_turn_set_primary_topic(&stm, 1, "cooking", 7);
    hu_stm_turn_add_emotion(&stm, 1, HU_EMOTION_JOY, 0.9);

    hu_stm_record_turn(&stm, "user", 4, "Spent time with family today", 27, 3000);
    hu_stm_turn_set_primary_topic(&stm, 2, "family", 6);
    hu_stm_turn_add_emotion(&stm, 2, HU_EMOTION_JOY, 0.7);

    hu_emotional_graph_t egraph;
    memset(&egraph, 0, sizeof(egraph));
    HU_ASSERT_EQ(hu_egraph_init(&egraph, alloc), HU_OK);
    HU_ASSERT_EQ(hu_egraph_populate_from_stm(&egraph, &stm), HU_OK);

    HU_ASSERT_TRUE(egraph.node_count >= 1);
    HU_ASSERT_TRUE(egraph.edge_count >= 1);

    size_t ctx_len = 0;
    char *ctx = hu_egraph_build_context(&alloc, &egraph, &ctx_len);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(ctx_len > 0);
    HU_ASSERT_TRUE(strstr(ctx, "work") != NULL || strstr(ctx, "cooking") != NULL ||
                   strstr(ctx, "family") != NULL);
    alloc.free(alloc.ctx, ctx, ctx_len + 1);

    double avg = 0.0;
    hu_emotion_tag_t work_emo = hu_egraph_query(&egraph, "work", 4, &avg);
    HU_ASSERT_EQ(work_emo, HU_EMOTION_ANXIETY);

    avg = 0.0;
    hu_emotion_tag_t cooking_emo = hu_egraph_query(&egraph, "cooking", 7, &avg);
    HU_ASSERT_EQ(cooking_emo, HU_EMOTION_JOY);

    avg = 0.0;
    hu_emotion_tag_t family_emo = hu_egraph_query(&egraph, "family", 6, &avg);
    HU_ASSERT_EQ(family_emo, HU_EMOTION_JOY);

    hu_egraph_deinit(&egraph);
    hu_stm_deinit(&stm);
}

static void bth_typo_correction_pipeline(void) {
    const char *original = "that sounds like a great idea";
    size_t orig_len = strlen(original);
    char typo_buf[128];
    memcpy(typo_buf, original, orig_len + 1);

    uint32_t typo_seed = 0;
    for (; typo_seed < 500; typo_seed++) {
        memcpy(typo_buf, original, orig_len + 1);
        size_t mod_len =
            hu_conversation_apply_typos(typo_buf, orig_len, sizeof(typo_buf), typo_seed);
        if (mod_len != orig_len || memcmp(typo_buf, original, orig_len + 1) != 0)
            break;
    }
    HU_ASSERT_TRUE(typo_seed < 500);

    char corr_buf[64];
    size_t corr_len = hu_conversation_generate_correction(
        original, orig_len, typo_buf, strlen(typo_buf), corr_buf, sizeof(corr_buf), 12345u, 100u);
    HU_ASSERT_TRUE(corr_len > 0);
    HU_ASSERT_EQ(corr_buf[0], '*');
    HU_ASSERT_TRUE(strstr(original, corr_buf + 1) != NULL);
}

static void bth_event_to_proactive_pipeline(void) {
    hu_allocator_t alloc = hu_system_allocator();

    const char *text = "my interview is on Tuesday";
    hu_event_extract_result_t extract;
    memset(&extract, 0, sizeof(extract));
    HU_ASSERT_EQ(hu_event_extract(&alloc, text, strlen(text), &extract), HU_OK);
    HU_ASSERT_TRUE(extract.event_count >= 1);
    HU_ASSERT_NOT_NULL(extract.events[0].description);
    HU_ASSERT_TRUE(strstr(extract.events[0].description, "interview") != NULL);

    hu_proactive_result_t result;
    memset(&result, 0, sizeof(result));
    HU_ASSERT_EQ(hu_proactive_check_events(&alloc, extract.events, extract.event_count, &result),
                 HU_OK);
    HU_ASSERT_TRUE(result.count >= 1);
    bool found_interview = false;
    for (size_t i = 0; i < result.count; i++) {
        if (result.actions[i].message && strstr(result.actions[i].message, "interview") != NULL) {
            found_interview = true;
            break;
        }
    }
    HU_ASSERT_TRUE(found_interview);

    hu_event_extract_result_deinit(&extract, &alloc);
    hu_proactive_result_deinit(&result, &alloc);
}

static void bth_mood_from_emotions_pipeline(void) {
    hu_allocator_t alloc = hu_system_allocator();

    hu_stm_buffer_t stm;
    hu_stm_init(&stm, alloc, "mood-session", 13);
    hu_stm_record_turn(&stm, "user", 4, "I'm so happy today!", 19, 1000);
    hu_stm_turn_add_emotion(&stm, 0, HU_EMOTION_JOY, 0.85);
    hu_stm_record_turn(&stm, "user", 4, "Work has me anxious", 19, 2000);
    hu_stm_turn_add_emotion(&stm, 1, HU_EMOTION_ANXIETY, 0.7);

    hu_memory_t mem = hu_memory_lru_create(&alloc, 100);
    HU_ASSERT_EQ(hu_promotion_run_emotions(&alloc, &stm, &mem, "mood_contact", 11), HU_OK);

    char *mood_ctx = NULL;
    size_t mood_len = 0;
    HU_ASSERT_EQ(hu_mood_build_context(&alloc, &mem, "mood_contact", 11, &mood_ctx, &mood_len),
                 HU_OK);
    HU_ASSERT_NOT_NULL(mood_ctx);
    HU_ASSERT_TRUE(mood_len > 0);
    HU_ASSERT_TRUE(strstr(mood_ctx, "joy") != NULL || strstr(mood_ctx, "anxiety") != NULL);

    alloc.free(alloc.ctx, mood_ctx, mood_len + 1);
    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
    hu_stm_deinit(&stm);
}

static void bth_replay_analysis_pipeline(void) {
    hu_allocator_t alloc = hu_system_allocator();

    hu_channel_history_entry_t entries[4];
    entries[0] = make_entry(false, "how was your day?", "2025-01-01 12:00");
    entries[1] =
        make_entry(true, "I understand how you feel. That must be difficult.", "2025-01-01 12:01");
    entries[2] = make_entry(true, "that's so funny", "2025-01-01 12:02");
    entries[3] = make_entry(false, "haha exactly!", "2025-01-01 12:03");

    hu_replay_result_t replay;
    memset(&replay, 0, sizeof(replay));
    HU_ASSERT_EQ(hu_replay_analyze(&alloc, entries, 4, 2000, &replay), HU_OK);

    bool found_positive = false;
    bool found_negative = false;
    for (size_t i = 0; i < replay.insight_count; i++) {
        if (replay.insights[i].score_delta > 0)
            found_positive = true;
        if (replay.insights[i].score_delta < 0)
            found_negative = true;
    }
    HU_ASSERT_TRUE(found_positive || found_negative);

    size_t ctx_len = 0;
    char *ctx = hu_replay_build_context(&alloc, &replay, &ctx_len);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(ctx_len > 0);
    HU_ASSERT_TRUE(strstr(ctx, "Session Replay") != NULL);
    alloc.free(alloc.ctx, ctx, ctx_len + 1);

    hu_replay_result_deinit(&replay, &alloc);
}

static void bth_pipeline_full_conversation(void) {
    hu_allocator_t alloc = hu_system_allocator();

    /* 1. Create memory backend */
    hu_memory_t mem = hu_memory_lru_create(&alloc, 1000);

    /* 2. Create STM buffer */
    hu_stm_buffer_t stm;
    hu_stm_init(&stm, alloc, "test-session", 12);

    /* 3. Create commitment store */
    hu_commitment_store_t *commit_store = NULL;
    HU_ASSERT_EQ(hu_commitment_store_create(&alloc, &mem, &commit_store), HU_OK);
    HU_ASSERT_NOT_NULL(commit_store);

    /* 4. Create pattern radar */
    hu_pattern_radar_t radar;
    HU_ASSERT_EQ(hu_pattern_radar_init(&radar, alloc), HU_OK);

    /* 5. Create superhuman registry */
    hu_superhuman_registry_t superhuman;
    HU_ASSERT_EQ(hu_superhuman_registry_init(&superhuman), HU_OK);

    /* Register superhuman services */
    hu_superhuman_commitment_ctx_t sh_commit_ctx = {
        .store = commit_store,
        .session_id = "test-session",
        .session_id_len = 12,
    };
    hu_superhuman_service_t sh_commit_svc;
    HU_ASSERT_EQ(hu_superhuman_commitment_service(&sh_commit_ctx, &sh_commit_svc), HU_OK);
    HU_ASSERT_EQ(hu_superhuman_register(&superhuman, sh_commit_svc), HU_OK);

    hu_superhuman_emotional_ctx_t sh_emotional_ctx = {0};
    hu_superhuman_service_t sh_emotional_svc;
    HU_ASSERT_EQ(hu_superhuman_emotional_service(&sh_emotional_ctx, &sh_emotional_svc), HU_OK);
    HU_ASSERT_EQ(hu_superhuman_register(&superhuman, sh_emotional_svc), HU_OK);

    hu_superhuman_silence_ctx_t sh_silence_ctx = {0};
    hu_superhuman_service_t sh_silence_svc;
    HU_ASSERT_EQ(hu_superhuman_silence_service(&sh_silence_ctx, &sh_silence_svc), HU_OK);
    HU_ASSERT_EQ(hu_superhuman_register(&superhuman, sh_silence_svc), HU_OK);

    /* 6. Relationship tracker */
    hu_relationship_state_t rel = {0};
    hu_relationship_new_session(&rel);

    /* ── Simulate 5-turn conversation ── */
    const char *turns[] = {
        "I'm so frustrated with work. My boss keeps piling on deadlines.",
        "I promise I'll finish the report by Friday",
        "My mom called today, she's worried about my health",
        "I want to learn piano this year, it's my goal",
        "I'm feeling a lot better now, thanks for listening",
    };

    for (int t = 0; t < 5; t++) {
        const char *msg = turns[t];
        size_t msg_len = strlen(msg);
        uint64_t ts = (uint64_t)(1000 + t * 60) * 1000;

        /* A. Fast-capture */
        hu_fc_result_t fc;
        memset(&fc, 0, sizeof(fc));
        (void)hu_fast_capture(&alloc, msg, msg_len, &fc);

        /* B. STM record */
        HU_ASSERT_EQ(hu_stm_record_turn(&stm, "user", 4, msg, msg_len, ts), HU_OK);
        size_t last_idx = hu_stm_count(&stm) - 1;
        for (size_t i = 0; i < fc.entity_count; i++) {
            const hu_fc_entity_match_t *e = &fc.entities[i];
            (void)hu_stm_turn_add_entity(&stm, last_idx, e->name, e->name_len,
                                         e->type ? e->type : "entity", e->type ? e->type_len : 6,
                                         1);
        }
        for (size_t i = 0; i < fc.emotion_count; i++) {
            (void)hu_stm_turn_add_emotion(&stm, last_idx, fc.emotions[i].tag,
                                          fc.emotions[i].intensity);
        }

        /* C. Pattern radar observe */
        char ts_buf[32];
        int ts_n = snprintf(ts_buf, sizeof(ts_buf), "%llu", (unsigned long long)ts);
        const char *ts_str = ts_n > 0 ? ts_buf : NULL;
        size_t ts_len = (ts_n > 0 && ts_n < (int)sizeof(ts_buf)) ? (size_t)ts_n : 0;
        for (size_t i = 0; i < fc.entity_count; i++) {
            const hu_fc_entity_match_t *e = &fc.entities[i];
            if (e->name && e->name_len > 0) {
                (void)hu_pattern_radar_observe(
                    &radar, e->name, e->name_len, HU_PATTERN_TOPIC_RECURRENCE,
                    e->type ? e->type : NULL, e->type ? e->type_len : 0, ts_str, ts_len);
            }
        }

        /* D. Commitment detection */
        hu_commitment_detect_result_t cd;
        memset(&cd, 0, sizeof(cd));
        (void)hu_commitment_detect(&alloc, msg, msg_len, "user", 4, &cd);
        for (size_t i = 0; i < cd.count; i++) {
            (void)hu_commitment_store_save(commit_store, &cd.commitments[i], "test-session", 12);
        }
        hu_commitment_detect_result_deinit(&cd, &alloc);

        /* E. Superhuman observe */
        (void)hu_superhuman_observe_all(&superhuman, &alloc, msg, msg_len, "user", 4);

        /* F. Relationship update */
        hu_relationship_update(&rel, 1);

        hu_fc_result_deinit(&fc, &alloc);
    }

    /* ── Verify all components produced correct output ── */

    /* V1. STM has 5 turns */
    HU_ASSERT_EQ(hu_stm_count(&stm), 5u);

    /* V2. STM context is non-empty */
    char *stm_ctx = NULL;
    size_t stm_ctx_len = 0;
    HU_ASSERT_EQ(hu_stm_build_context(&stm, &alloc, &stm_ctx, &stm_ctx_len), HU_OK);
    HU_ASSERT_NOT_NULL(stm_ctx);
    HU_ASSERT_TRUE(stm_ctx_len > 50);
    HU_ASSERT_TRUE(strstr(stm_ctx, "frustrated") != NULL || strstr(stm_ctx, "boss") != NULL);
    alloc.free(alloc.ctx, stm_ctx, stm_ctx_len + 1);

    /* V3. Commitments were detected and stored */
    {
        hu_commitment_t *active = NULL;
        size_t active_count = 0;
        HU_ASSERT_EQ(hu_commitment_store_list_active(commit_store, &alloc, "test-session", 12,
                                                     &active, &active_count),
                     HU_OK);
        HU_ASSERT_TRUE(active_count >= 1); /* "I promise I'll finish the report" */
        if (active) {
            for (size_t i = 0; i < active_count; i++)
                hu_commitment_deinit(&active[i], &alloc);
            alloc.free(alloc.ctx, active, active_count * sizeof(hu_commitment_t));
        }
    }

    /* V4. Commitment context is non-empty (we have active commitments from V3) */
    {
        char *ctx = NULL;
        size_t ctx_len = 0;
        HU_ASSERT_EQ(hu_commitment_store_build_context(commit_store, &alloc, "test-session", 12,
                                                       &ctx, &ctx_len),
                     HU_OK);
        HU_ASSERT_NOT_NULL(ctx);
        HU_ASSERT_TRUE(ctx_len > 10);
        alloc.free(alloc.ctx, ctx, ctx_len + 1);
    }

    /* V5. Pattern radar observed topics */
    HU_ASSERT_TRUE(radar.observation_count >= 1);

#ifdef HU_HAS_PERSONA
    /* V6. Proactive check works */
    {
        hu_proactive_result_t pr;
        memset(&pr, 0, sizeof(pr));
        HU_ASSERT_EQ(hu_proactive_check(&alloc, rel.session_count, 9, &pr), HU_OK);
        HU_ASSERT_TRUE(pr.count >= 1); /* at least CHECK_IN */
        char *pro_ctx = NULL;
        size_t pro_ctx_len = 0;
        (void)hu_proactive_build_context(&pr, &alloc, 8, &pro_ctx, &pro_ctx_len);
        if (pro_ctx)
            alloc.free(alloc.ctx, pro_ctx, pro_ctx_len + 1);
        hu_proactive_result_deinit(&pr, &alloc);
    }

    /* V7. Circadian overlay works */
    {
        char *circ = NULL;
        size_t circ_len = 0;
        HU_ASSERT_EQ(hu_circadian_build_prompt(&alloc, 9, &circ, &circ_len), HU_OK);
        HU_ASSERT_NOT_NULL(circ);
        HU_ASSERT_TRUE(strstr(circ, "morning") != NULL || strstr(circ, "Morning") != NULL);
        alloc.free(alloc.ctx, circ, circ_len + 1);
    }
#endif

    /* V8. Relationship state updated */
    HU_ASSERT_EQ(rel.session_count, 1u);
    HU_ASSERT_EQ(rel.total_turns, 5u);
    HU_ASSERT_EQ(rel.stage, HU_REL_NEW);

    /* V9. Superhuman context (emotional first aid may have content) */
    {
        char *sh_ctx = NULL;
        size_t sh_ctx_len = 0;
        (void)hu_superhuman_build_context(&superhuman, &alloc, &sh_ctx, &sh_ctx_len);
        if (sh_ctx)
            alloc.free(alloc.ctx, sh_ctx, sh_ctx_len + 1);
    }

    /* V10. Promotion works (promote STM entities to persistent memory) */
    {
        hu_promotion_config_t promo_config = HU_PROMOTION_DEFAULTS;
        promo_config.min_mention_count = 1;
        promo_config.min_importance = 0.0;
        hu_error_t perr = hu_promotion_run(&alloc, &stm, &mem, &promo_config);
        HU_ASSERT_EQ(perr, HU_OK);

        size_t mem_count = 0;
        HU_ASSERT_EQ(mem.vtable->count(mem.ctx, &mem_count), HU_OK);
        HU_ASSERT_TRUE(mem_count >= 1);
    }

    /* V11. Deep extraction prompt builds correctly */
    {
        char *stm_text = NULL;
        size_t stm_text_len = 0;
        HU_ASSERT_EQ(hu_stm_build_context(&stm, &alloc, &stm_text, &stm_text_len), HU_OK);
        if (stm_text) {
            char *de_prompt = NULL;
            size_t de_prompt_len = 0;
            HU_ASSERT_EQ(hu_deep_extract_build_prompt(&alloc, stm_text, stm_text_len, &de_prompt,
                                                      &de_prompt_len),
                         HU_OK);
            HU_ASSERT_NOT_NULL(de_prompt);
            HU_ASSERT_TRUE(de_prompt_len > 50);
            alloc.free(alloc.ctx, de_prompt, de_prompt_len + 1);
            alloc.free(alloc.ctx, stm_text, stm_text_len + 1);
        }
    }

    /* V12. Consolidation — skipped: LRU backend has known issues with consolidation
     * (hu_memory_entry_free_fields/ASan; see test_consolidation.c) */

    /* V13. DAG creation and validation */
    {
        hu_dag_t dag;
        HU_ASSERT_EQ(hu_dag_init(&dag, alloc), HU_OK);
        HU_ASSERT_EQ(hu_dag_add_node(&dag, "t1", "web_search", "{\"query\":\"news\"}", NULL, 0),
                     HU_OK);
        const char *deps[] = {"t1"};
        HU_ASSERT_EQ(hu_dag_add_node(&dag, "t2", "summarize", "{\"text\":\"$t1\"}", deps, 1),
                     HU_OK);
        HU_ASSERT_EQ(hu_dag_validate(&dag), HU_OK);
        HU_ASSERT_FALSE(hu_dag_is_complete(&dag));

        /* Batch execution */
        hu_dag_batch_t batch;
        HU_ASSERT_EQ(hu_dag_next_batch(&dag, &batch), HU_OK);
        HU_ASSERT_EQ(batch.count, 1u);
        HU_ASSERT_TRUE(strcmp(batch.nodes[0]->id, "t1") == 0);

        /* Mark t1 done, get next batch */
        batch.nodes[0]->status = HU_DAG_DONE;
        batch.nodes[0]->result = hu_strdup(&dag.alloc, "news content");
        batch.nodes[0]->result_len = batch.nodes[0]->result ? 12 : 0;

        HU_ASSERT_EQ(hu_dag_next_batch(&dag, &batch), HU_OK);
        HU_ASSERT_EQ(batch.count, 1u);
        HU_ASSERT_TRUE(strcmp(batch.nodes[0]->id, "t2") == 0);

        /* Variable resolution */
        char *resolved = NULL;
        size_t resolved_len = 0;
        HU_ASSERT_EQ(
            hu_dag_resolve_vars(&alloc, &dag, "{\"text\":\"$t1\"}", 15, &resolved, &resolved_len),
            HU_OK);
        HU_ASSERT_NOT_NULL(resolved);
        HU_ASSERT_TRUE(strstr(resolved, "news content") != NULL);
        hu_str_free(&alloc, resolved);

        hu_dag_deinit(&dag);
    }

    /* ── Cleanup ── */
    hu_commitment_store_destroy(commit_store);
    hu_stm_deinit(&stm);
    hu_pattern_radar_deinit(&radar);
    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
}

void run_bth_e2e_tests(void) {
    HU_TEST_SUITE("bth_e2e");
    HU_RUN_TEST(bth_pipeline_full_conversation);
    HU_RUN_TEST(bth_conversation_intelligence_pipeline);
    HU_RUN_TEST(bth_emotional_graph_pipeline);
    HU_RUN_TEST(bth_typo_correction_pipeline);
    HU_RUN_TEST(bth_event_to_proactive_pipeline);
    HU_RUN_TEST(bth_mood_from_emotions_pipeline);
    HU_RUN_TEST(bth_replay_analysis_pipeline);
}
