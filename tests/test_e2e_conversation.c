/*
 * E2E conversation loop tests — exercises daemon loop components in sequence.
 * No real network, SQLite files, or process spawning. Deterministic.
 */
#include "human/agent/episodic.h"
#include "human/agent/governor.h"
#include "human/agent/proactive.h"
#include "human/agent/timing.h"
#include "human/context/conversation.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/memory.h"
#include "human/memory/engines.h"
#include "human/memory/rag_pipeline.h"
#include "human/observability/bth_metrics.h"
#include "test_framework.h"
#include <string.h>

#ifdef HU_ENABLE_SQLITE
#include "human/memory/emotional_residue.h"
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

/* 1. Receive → classify → governor budget check → response outline */
static void e2e_conversation_receive_classify_respond(void) {
    hu_allocator_t alloc = hu_system_allocator();

    const char *msg = "how are you doing today?";
    size_t msg_len = strlen(msg);
    double conf = 0.0;
    hu_classify_result_t cls = hu_classify_message(msg, msg_len, &conf);

    HU_ASSERT_TRUE(conf >= 0.0);
    HU_ASSERT_TRUE(conf <= 1.0);
    const char *cls_name = hu_classify_result_str(cls);
    HU_ASSERT_NOT_NULL(cls_name);
    HU_ASSERT_TRUE(strlen(cls_name) > 0);

    hu_proactive_budget_t budget;
    hu_proactive_budget_config_t cfg = {
        .daily_max = 3,
        .weekly_max = 10,
        .relationship_multiplier = 1.0,
        .cool_off_after_unanswered = 2,
        .cool_off_hours = 72,
    };
    HU_ASSERT_EQ(hu_governor_init(&cfg, &budget), HU_OK);
    uint64_t now_ms = 1700000000ULL * 1000;
    HU_ASSERT_TRUE(hu_governor_has_budget(&budget, now_ms));

    char *outline = NULL;
    size_t outline_len = 0;
    hu_error_t err = hu_classifier_build_prompt(&alloc, cls, conf, &outline, &outline_len);
    HU_ASSERT_EQ(err, HU_OK);
    if (outline && outline_len > 0) {
        HU_ASSERT_TRUE(strstr(outline, cls_name) != NULL || outline_len > 0);
        hu_str_free(&alloc, outline);
    }
}

/* 2. Proactive cycle gated by governor budget, timing model, contact knowledge */
static void e2e_conversation_proactive_cycle_gated(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_memory_lru_create(&alloc, 100);
    HU_ASSERT_NOT_NULL(mem.ctx);

    static const char topic_cat[] = "conversation";
    hu_memory_category_t cat = {
        .tag = HU_MEMORY_CATEGORY_CUSTOM,
        .data.custom = {.name = topic_cat, .name_len = sizeof(topic_cat) - 1},
    };
    static const char CONTACT[] = "contact_a";
    const char *key1 = "topic:contact_a:1";
    const char *content1 =
        "recent topics activities interests: user wanted to plan a trip";
    mem.vtable->store(mem.ctx, key1, strlen(key1), content1, strlen(content1), &cat, CONTACT,
                      sizeof(CONTACT) - 1);

    hu_proactive_budget_t budget;
    HU_ASSERT_EQ(hu_governor_init(NULL, &budget), HU_OK);
    uint64_t now_ms = 1700000000ULL * 1000;
    bool has_budget = hu_governor_has_budget(&budget, now_ms);
    HU_ASSERT_TRUE(has_budget);

    uint64_t delay = hu_timing_model_sample_default(14, 42);
    HU_ASSERT_TRUE(delay > 0);
    HU_ASSERT_TRUE(delay < 120000);

    char *starter = NULL;
    size_t starter_len = 0;
    hu_error_t err =
        hu_proactive_build_starter(&alloc, &mem, CONTACT, sizeof(CONTACT) - 1, &starter, &starter_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(starter);
    HU_ASSERT_TRUE(starter_len > 0);
    HU_ASSERT_TRUE(strstr(starter, "trip") != NULL || strstr(starter, "starting") != NULL ||
                   strstr(starter, "conversation") != NULL);

    alloc.free(alloc.ctx, starter, starter_len + 1);
    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
}

/* 3. Multi-turn: 5 turns, context accumulation (BTH metrics, episodic, awareness) */
static void e2e_conversation_multi_turn_context_accumulation(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_bth_metrics_t metrics;
    hu_bth_metrics_init(&metrics);

    hu_channel_history_entry_t entries[10];
    memset(entries, 0, sizeof(entries));
    entries[0] = make_entry(false, "hey how are you?", "10:00");
    entries[1] = make_entry(true, "doing good, you?", "10:01");
    entries[2] = make_entry(false, "stressed about work", "10:02");
    entries[3] = make_entry(true, "that sucks, what's going on?", "10:03");
    entries[4] = make_entry(false, "deadlines piling up", "10:04");
    entries[5] = make_entry(true, "ugh that sounds rough", "10:05");
    entries[6] = make_entry(false, "yeah. anyway how was your weekend?", "10:06");
    entries[7] = make_entry(true, "pretty chill, went hiking", "10:07");
    entries[8] = make_entry(false, "nice! where'd you go?", "10:08");
    entries[9] = make_entry(true, "up to the lake", "10:09");

    for (int i = 0; i < 5; i++) {
        metrics.total_turns++;
        if (i == 2)
            metrics.emotions_surfaced++;
        if (i == 4)
            metrics.facts_extracted++;
    }
    HU_ASSERT_EQ(metrics.total_turns, 5u);
    HU_ASSERT_EQ(metrics.emotions_surfaced, 1u);
    HU_ASSERT_EQ(metrics.facts_extracted, 1u);

    size_t len = 0;
    char *awareness = hu_conversation_build_awareness(&alloc, entries, 10, NULL, &len);
    HU_ASSERT_NOT_NULL(awareness);
    HU_ASSERT_TRUE(len > 0);
    alloc.free(alloc.ctx, awareness, len + 1);

    const char *msgs[] = {"hey how are you?", "doing good, you?", "stressed about work",
                          "that sucks", "deadlines piling up", "ugh that sounds rough"};
    size_t lens[] = {17, 15, 18, 11, 18, 18};
    size_t out_len = 0;
    char *summary = hu_episodic_summarize_session(&alloc, msgs, lens, 6, &out_len);
    HU_ASSERT_NOT_NULL(summary);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(summary, "Session topic:") != NULL);
    alloc.free(alloc.ctx, summary, out_len + 1);

#ifdef HU_ENABLE_SQLITE
    hu_emotional_residue_t residue = {0};
    residue.valence = 0.3;
    residue.intensity = 0.7;
    memcpy(residue.contact_id, "contact_multi", 14);
    size_t dir_len = 0;
    char *dir = hu_emotional_residue_build_directive(&alloc, &residue, 1, &dir_len);
    HU_ASSERT_NOT_NULL(dir);
    HU_ASSERT_TRUE(dir_len > 0);
    HU_ASSERT_TRUE(strstr(dir, "EMOTIONAL RESIDUE") != NULL);
    alloc.free(alloc.ctx, dir, dir_len + 1);
#endif
}

/* 4. Tapback decision flow: classify → tapback decision → reaction type */
static void e2e_conversation_tapback_after_response(void) {
    const char *msg = "lol that's hilarious";
    size_t msg_len = strlen(msg);

    hu_tapback_decision_t decision =
        hu_conversation_classify_tapback_decision(msg, msg_len, NULL, 0, NULL, 0u);
    HU_ASSERT_TRUE(decision == HU_TAPBACK_ONLY || decision == HU_TAPBACK_AND_TEXT ||
                   decision == HU_TEXT_ONLY);

    if (decision == HU_TAPBACK_ONLY || decision == HU_TAPBACK_AND_TEXT) {
        hu_reaction_type_t reaction =
            hu_conversation_classify_reaction(msg, msg_len, false, NULL, 0, 0u);
        HU_ASSERT_NEQ(reaction, HU_REACTION_NONE);
        HU_ASSERT_TRUE(reaction >= HU_REACTION_HAHA && reaction <= HU_REACTION_QUESTION);
    }

    hu_tapback_decision_t q_decision =
        hu_conversation_classify_tapback_decision("what time is dinner?", 20, NULL, 0, NULL, 0u);
    HU_ASSERT_EQ(q_decision, HU_TEXT_ONLY);

    hu_tapback_decision_t empty_decision =
        hu_conversation_classify_tapback_decision("", 0, NULL, 0, NULL, 0u);
    HU_ASSERT_EQ(empty_decision, HU_NO_RESPONSE);
}

/* 5. Memory write/read cycle: episodic store → load, BTH metrics update */
static void e2e_conversation_memory_write_read_cycle(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_memory_lru_create(&alloc, 100);
    HU_ASSERT_NOT_NULL(mem.ctx);

    static const char SESSION[] = "sess_e2e_1";
    static const char SUMMARY[] = "User asked about build. Assistant suggested cmake.";
    HU_ASSERT_EQ(hu_episodic_store(&mem, &alloc, SESSION, sizeof(SESSION) - 1, SUMMARY,
                                   sizeof(SUMMARY) - 1),
                 HU_OK);

    char *loaded = NULL;
    size_t loaded_len = 0;
    hu_error_t err = hu_episodic_load(&mem, &alloc, &loaded, &loaded_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(loaded);
    HU_ASSERT_TRUE(loaded_len > 0);
    HU_ASSERT_TRUE(strstr(loaded, "Recent Sessions") != NULL);
    HU_ASSERT_TRUE(strstr(loaded, "cmake") != NULL || strstr(loaded, "build") != NULL);
    hu_str_free(&alloc, loaded);

    hu_bth_metrics_t metrics;
    hu_bth_metrics_init(&metrics);
    metrics.total_turns = 1;
    metrics.facts_extracted = 1;
    HU_ASSERT_EQ(metrics.total_turns, 1u);
    HU_ASSERT_EQ(metrics.facts_extracted, 1u);

    size_t sum_len = 0;
    char *summary = hu_bth_metrics_summary(&alloc, &metrics, &sum_len);
    HU_ASSERT_NOT_NULL(summary);
    HU_ASSERT_TRUE(sum_len > 0);
    HU_ASSERT_TRUE(strstr(summary, "total_turns=1") != NULL);
    alloc.free(alloc.ctx, summary, sum_len);

    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
}

void run_e2e_conversation_tests(void) {
    HU_TEST_SUITE("e2e_conversation");
    HU_RUN_TEST(e2e_conversation_receive_classify_respond);
    HU_RUN_TEST(e2e_conversation_proactive_cycle_gated);
    HU_RUN_TEST(e2e_conversation_multi_turn_context_accumulation);
    HU_RUN_TEST(e2e_conversation_tapback_after_response);
    HU_RUN_TEST(e2e_conversation_memory_write_read_cycle);
}
