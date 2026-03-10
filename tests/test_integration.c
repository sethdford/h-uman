/*
 * End-to-end integration tests for the human agent loop.
 * Wires the full hu_agent_turn loop with all BTH (Better Than Human) systems.
 * Uses :memory: for SQLite, mock providers, no real network.
 */
#include "human/agent.h"
#include "human/agent/commitment.h"
#include "human/agent/commitment_store.h"
#include "human/agent/pattern_radar.h"
#include "human/agent/proactive.h"
#include "human/channel.h"
#include "human/context/conversation.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include "human/memory/deep_extract.h"
#include "human/memory/fast_capture.h"
#include "human/memory/graph.h"
#include "human/persona/replay.h"
#include "human/provider.h"
#include "human/providers/factory.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE

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

/* ── 1. Full agent turn with memory context ───────────────────────────────── */

static void integration_full_agent_turn_with_memory_context(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "ollama", 6, NULL, 0, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);

    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_agent_t agent;
    err =
        hu_agent_from_config(&agent, &alloc, prov, NULL, 0, &mem, NULL, NULL, NULL, "llama2", 6,
                             "ollama", 6, 0.7, "/tmp", 4, 5, 50, false, 1, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_OK);

    char *resp = NULL;
    size_t resp_len = 0;
    err = hu_agent_turn(&agent, "hey what's up", 13, &resp, &resp_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resp);
    HU_ASSERT_TRUE(resp_len > 0);

    HU_ASSERT_EQ(agent.history_count, 2u);

    alloc.free(alloc.ctx, resp, resp_len + 1);
    hu_agent_deinit(&agent);
    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

/* ── 2. Proactive check-in with memory ────────────────────────────────────── */

static void integration_proactive_check_in_with_memory(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    static const char topic_cat[] = "conversation";
    hu_memory_category_t cat = {
        .tag = HU_MEMORY_CATEGORY_CUSTOM,
        .data.custom = {.name = topic_cat, .name_len = sizeof(topic_cat) - 1},
    };

    const char *key1 = "topic:contact_x:1";
    const char *content1 = "recent topics: user mentioned moving to a new city";
    static const char CONTACT[] = "contact_x";
    mem.vtable->store(mem.ctx, key1, strlen(key1), content1, strlen(content1), &cat, CONTACT,
                      sizeof(CONTACT) - 1);

    hu_proactive_result_t result;
    memset(&result, 0, sizeof(result));
    HU_ASSERT_EQ(hu_proactive_check(&alloc, 5, 14, &result), HU_OK);

    char *starter = NULL;
    size_t starter_len = 0;
    HU_ASSERT_EQ(hu_proactive_build_starter(&alloc, &mem, CONTACT, sizeof(CONTACT) - 1, &starter,
                                            &starter_len),
                 HU_OK);
    HU_ASSERT_NOT_NULL(starter);
    HU_ASSERT_TRUE(starter_len > 0);
    HU_ASSERT_TRUE(strstr(starter, "starting points") != NULL ||
                   strstr(starter, "conversation") != NULL);
    HU_ASSERT_TRUE(strstr(starter, "new city") != NULL || strstr(starter, "moving") != NULL);

    alloc.free(alloc.ctx, starter, starter_len + 1);
    hu_proactive_result_deinit(&result, &alloc);
    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
}

/* ── 3. Conversation callback with history ────────────────────────────────── */

static void integration_conversation_callback_with_dropped_topic(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_history_entry_t entries[10] = {
        make_entry(false, "what about the project deadline?", "12:00"),
        make_entry(true, "still working on it", "12:01"),
        make_entry(false, "how's the project going?", "12:02"),
        make_entry(true, "making progress", "12:03"),
        make_entry(false, "cool", "12:04"),
        make_entry(true, "what's for lunch?", "12:05"),
        make_entry(false, "thinking pizza", "12:06"),
        make_entry(true, "sounds good", "12:07"),
        make_entry(false, "yeah", "12:08"),
        make_entry(true, "nice", "12:09"),
    };

    size_t len = 0;
    char *ctx = hu_conversation_build_callback(&alloc, entries, 10, &len);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(ctx, "Thread Callback") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "project") != NULL);

    alloc.free(alloc.ctx, ctx, len + 1);
}

/* ── 4. Quality evaluation → retry flow ───────────────────────────────────── */

static void integration_quality_evaluation_needs_revision(void) {
    hu_channel_history_entry_t entries[4] = {
        make_entry(false, "hey", "12:00"),
        make_entry(true, "hi", "12:01"),
        make_entry(false, "how are you?", "12:02"),
        make_entry(true, "good", "12:03"),
    };

    const char *long_response =
        "That is an excellent question! I would be delighted to provide you with a comprehensive "
        "and thorough response. Let me elaborate in great detail on each of the points you have "
        "raised. Firstly, we must consider the various aspects and implications. Secondly, it is "
        "important to note that there are multiple factors at play. Thirdly, we should examine "
        "the broader context. In conclusion, I hope this extensive analysis has been helpful.";
    size_t long_len = strlen(long_response);

    hu_quality_score_t score =
        hu_conversation_evaluate_quality(long_response, long_len, entries, 4, 300);
    HU_ASSERT_TRUE(score.needs_revision);
    HU_ASSERT_TRUE(score.guidance[0] != '\0');
    HU_ASSERT_TRUE(strstr(score.guidance, "chars") != NULL ||
                   strstr(score.guidance, "Tighten") != NULL ||
                   strstr(score.guidance, "Match") != NULL);
}

/* ── 5. Fast capture → deep extract → memory store pipeline ───────────────── */

static void integration_fast_capture_deep_extract_memory_store(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *msg = "I work at Acme Corp and I'm so excited! My mom is happy for me.";
    size_t msg_len = strlen(msg);

    hu_fc_result_t fc;
    memset(&fc, 0, sizeof(fc));
    HU_ASSERT_EQ(hu_fast_capture(&alloc, msg, msg_len, &fc), HU_OK);
    HU_ASSERT_TRUE(fc.entity_count >= 1 || fc.emotion_count >= 1 || fc.primary_topic != NULL);

    hu_deep_extract_result_t de;
    memset(&de, 0, sizeof(de));
    HU_ASSERT_EQ(hu_deep_extract_lightweight(&alloc, msg, msg_len, &de), HU_OK);
    HU_ASSERT_TRUE(de.fact_count >= 1);

    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    static const char cat_name[] = "conversation";
    hu_memory_category_t cat = {
        .tag = HU_MEMORY_CATEGORY_CUSTOM,
        .data.custom = {.name = cat_name, .name_len = sizeof(cat_name) - 1},
    };

    const char *key1 = "fact:user:works_at";
    const char *content1 = de.facts[0].object ? de.facts[0].object : "Acme Corp";
    mem.vtable->store(mem.ctx, key1, strlen(key1), content1, strlen(content1), &cat, "user", 4);

    if (fc.primary_topic) {
        const char *key2 = "topic:user:1";
        mem.vtable->store(mem.ctx, key2, strlen(key2), fc.primary_topic, strlen(fc.primary_topic),
                          &cat, "user", 4);
    }

    hu_memory_entry_t *recalled = NULL;
    size_t recalled_count = 0;
    HU_ASSERT_EQ(
        mem.vtable->recall(mem.ctx, &alloc, "Acme", 4, 5, "user", 4, &recalled, &recalled_count),
        HU_OK);
    HU_ASSERT_TRUE(recalled_count >= 1);
    HU_ASSERT_NOT_NULL(recalled);

    if (recalled) {
        for (size_t i = 0; i < recalled_count; i++)
            hu_memory_entry_free_fields(&alloc, &recalled[i]);
        alloc.free(alloc.ctx, recalled, recalled_count * sizeof(hu_memory_entry_t));
    }

    hu_fc_result_deinit(&fc, &alloc);
    hu_deep_extract_result_deinit(&de, &alloc);
    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
}

/* ── 6. Graph RAG context building ───────────────────────────────────────── */

static void integration_graph_rag_context_building(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    HU_ASSERT_EQ(hu_graph_open(&alloc, ":memory:", 8, &g), HU_OK);
    HU_ASSERT_NOT_NULL(g);

    int64_t alice = 0, bob = 0, acme = 0;
    hu_graph_upsert_entity(g, "alice", 5, HU_ENTITY_PERSON, NULL, &alice);
    hu_graph_upsert_entity(g, "bob", 3, HU_ENTITY_PERSON, NULL, &bob);
    hu_graph_upsert_entity(g, "acme", 4, HU_ENTITY_ORGANIZATION, NULL, &acme);
    hu_graph_upsert_relation(g, alice, acme, HU_REL_WORKS_AT, 1.0f, NULL, 0);
    hu_graph_upsert_relation(g, alice, bob, HU_REL_KNOWS, 0.9f, "colleague", 9);

    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_graph_build_context(g, &alloc, "alice bob acme", 14, 2, 4096, &out, &out_len),
                 HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(out, "Knowledge Graph") != NULL);
    HU_ASSERT_TRUE(strstr(out, "alice") != NULL || strstr(out, "bob") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
    hu_graph_close(g, &alloc);
}

/* ── 7. Commitment detection → storage → follow-up ────────────────────────── */

static void integration_commitment_detection_storage_followup(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_commitment_store_t *store = NULL;
    HU_ASSERT_EQ(hu_commitment_store_create(&alloc, &mem, &store), HU_OK);
    HU_ASSERT_NOT_NULL(store);

    const char *msg = "I promise I'll send you the report by Friday";
    hu_commitment_detect_result_t cd;
    memset(&cd, 0, sizeof(cd));
    HU_ASSERT_EQ(hu_commitment_detect(&alloc, msg, strlen(msg), "user", 4, &cd), HU_OK);
    HU_ASSERT_TRUE(cd.count >= 1);

    for (size_t i = 0; i < cd.count; i++)
        (void)hu_commitment_store_save(store, &cd.commitments[i], "sess1", 5);
    hu_commitment_detect_result_deinit(&cd, &alloc);

    hu_commitment_t *active = NULL;
    size_t active_count = 0;
    HU_ASSERT_EQ(hu_commitment_store_list_active(store, &alloc, "sess1", 5, &active, &active_count),
                 HU_OK);
    HU_ASSERT_TRUE(active_count >= 1);

    hu_proactive_result_t result;
    memset(&result, 0, sizeof(result));
    HU_ASSERT_EQ(
        hu_proactive_check_extended(&alloc, 5, 14, active, active_count, NULL, NULL, 0, &result),
        HU_OK);

    bool has_followup = false;
    for (size_t i = 0; i < result.count; i++) {
        if (result.actions[i].type == HU_PROACTIVE_COMMITMENT_FOLLOW_UP &&
            result.actions[i].message && strstr(result.actions[i].message, "report") != NULL) {
            has_followup = true;
            break;
        }
    }
    HU_ASSERT_TRUE(has_followup);

    for (size_t i = 0; i < active_count; i++)
        hu_commitment_deinit(&active[i], &alloc);
    alloc.free(alloc.ctx, active, active_count * sizeof(hu_commitment_t));
    hu_proactive_result_deinit(&result, &alloc);
    hu_commitment_store_destroy(store);
    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
}

/* ── 8. Replay analysis with real conversation ────────────────────────────── */

static void integration_replay_analysis_real_conversation(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_history_entry_t entries[8] = {
        make_entry(false, "how was your interview?", "2025-01-15 10:00"),
        make_entry(true, "it went really well! they seemed interested", "2025-01-15 10:01"),
        make_entry(false, "that's awesome! when do you hear back?", "2025-01-15 10:02"),
        make_entry(true, "I understand how you feel. That must be exciting.", "2025-01-15 10:03"),
        make_entry(false, "haha yeah i'm nervous", "2025-01-15 10:04"),
        make_entry(true, "fingers crossed for you!", "2025-01-15 10:05"),
        make_entry(false, "thanks!", "2025-01-15 10:06"),
        make_entry(true, "anytime", "2025-01-15 10:07"),
    };

    hu_replay_result_t replay;
    memset(&replay, 0, sizeof(replay));
    HU_ASSERT_EQ(hu_replay_analyze(&alloc, entries, 8, 2000, &replay), HU_OK);

    bool has_insight = false;
    for (size_t i = 0; i < replay.insight_count; i++) {
        if (replay.insights[i].observation &&
            (strstr(replay.insights[i].observation, "engagement") != NULL ||
             strstr(replay.insights[i].observation, "enthusiastic") != NULL ||
             strstr(replay.insights[i].observation, "specificity") != NULL ||
             strstr(replay.insights[i].observation, "robotic") != NULL)) {
            has_insight = true;
            break;
        }
    }
    HU_ASSERT_TRUE(has_insight || replay.insight_count > 0);

    size_t ctx_len = 0;
    char *ctx = hu_replay_build_context(&alloc, &replay, &ctx_len);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(strstr(ctx, "Session Replay") != NULL);

    alloc.free(alloc.ctx, ctx, ctx_len + 1);
    hu_replay_result_deinit(&replay, &alloc);
}

/* ── 9. Pattern radar accumulation ─────────────────────────────────────────── */

static void integration_pattern_radar_accumulation(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_pattern_radar_t radar;
    HU_ASSERT_EQ(hu_pattern_radar_init(&radar, alloc), HU_OK);

    const char *topic = "exercise";
    for (int i = 0; i < 5; i++) {
        char ts_buf[32];
        int n = snprintf(ts_buf, sizeof(ts_buf), "%llu", (unsigned long long)(1000 + i));
        size_t ts_len = (n > 0 && n < (int)sizeof(ts_buf)) ? (size_t)n : 0;
        (void)hu_pattern_radar_observe(&radar, topic, strlen(topic), HU_PATTERN_TOPIC_RECURRENCE,
                                       "topic", 5, ts_buf, ts_len);
    }

    char *ctx = NULL;
    size_t ctx_len = 0;
    HU_ASSERT_EQ(hu_pattern_radar_build_context(&radar, &alloc, &ctx, &ctx_len), HU_OK);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(ctx_len > 0);
    HU_ASSERT_TRUE(strstr(ctx, "exercise") != NULL || strstr(ctx, "Pattern") != NULL);

    alloc.free(alloc.ctx, ctx, ctx_len + 1);
    hu_pattern_radar_deinit(&radar);
}

/* ── 10. Awareness builder with realistic data ────────────────────────────── */

static void integration_awareness_builder_realistic_data(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_history_entry_t entries[8] = {
        make_entry(false, "hey!", "2025-01-15T09:00:00"),
        make_entry(true, "hi whats up", "2025-01-15T09:00:30"),
        make_entry(false, "not much, just finished a long meeting about the Q4 roadmap",
                   "2025-01-15T09:01:00"),
        make_entry(true, "ugh meetings", "2025-01-15T09:01:15"),
        make_entry(false, "right? anyway how's your day going?", "2025-01-15T09:02:00"),
        make_entry(true, "pretty good! got some coding done", "2025-01-15T09:02:30"),
        make_entry(false, "nice", "2025-01-15T09:02:45"),
        make_entry(true, "yeah", "2025-01-15T09:03:00"),
    };

    size_t len = 0;
    char *ctx = hu_conversation_build_awareness(&alloc, entries, 8, NULL, &len);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(len > 0);

    bool has_metric = (strstr(ctx, "thread") != NULL || strstr(ctx, "Thread") != NULL ||
                       strstr(ctx, "conversation") != NULL || strstr(ctx, "pace") != NULL ||
                       strstr(ctx, "length") != NULL || strstr(ctx, "question") != NULL);
    HU_ASSERT_TRUE(has_metric);

    alloc.free(alloc.ctx, ctx, len + 1);
}

void run_integration_tests(void) {
    HU_TEST_SUITE("integration");
    HU_RUN_TEST(integration_full_agent_turn_with_memory_context);
    HU_RUN_TEST(integration_proactive_check_in_with_memory);
    HU_RUN_TEST(integration_conversation_callback_with_dropped_topic);
    HU_RUN_TEST(integration_quality_evaluation_needs_revision);
    HU_RUN_TEST(integration_fast_capture_deep_extract_memory_store);
    HU_RUN_TEST(integration_graph_rag_context_building);
    HU_RUN_TEST(integration_commitment_detection_storage_followup);
    HU_RUN_TEST(integration_replay_analysis_real_conversation);
    HU_RUN_TEST(integration_pattern_radar_accumulation);
    HU_RUN_TEST(integration_awareness_builder_realistic_data);
}

#else

void run_integration_tests(void) {
    HU_TEST_SUITE("integration");
}

#endif /* HU_ENABLE_SQLITE */
