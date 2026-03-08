#include "seaclaw/agent/commitment.h"
#include "seaclaw/agent/proactive.h"
#include "seaclaw/context/event_extract.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/string.h"
#include "seaclaw/memory.h"
#include "seaclaw/memory/engines.h"
#include "test_framework.h"
#include <string.h>

static void proactive_milestone_at_10_sessions(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    SC_ASSERT_EQ(sc_proactive_check(&alloc, 10, 14, &result), SC_OK);
    SC_ASSERT_TRUE(result.count > 0);

    bool has_milestone = false;
    for (size_t i = 0; i < result.count; i++) {
        if (result.actions[i].type == SC_PROACTIVE_MILESTONE) {
            has_milestone = true;
            SC_ASSERT_NOT_NULL(result.actions[i].message);
            SC_ASSERT_TRUE(strstr(result.actions[i].message, "conversation #10") != NULL);
            SC_ASSERT_TRUE(strstr(result.actions[i].message, "MILESTONE") != NULL);
            break;
        }
    }
    SC_ASSERT_TRUE(has_milestone);

    sc_proactive_result_deinit(&result, &alloc);
}

static void proactive_morning_briefing_at_9am(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    SC_ASSERT_EQ(sc_proactive_check(&alloc, 5, 9, &result), SC_OK);
    SC_ASSERT_TRUE(result.count > 0);

    bool has_briefing = false;
    for (size_t i = 0; i < result.count; i++) {
        if (result.actions[i].type == SC_PROACTIVE_MORNING_BRIEFING) {
            has_briefing = true;
            SC_ASSERT_NOT_NULL(result.actions[i].message);
            SC_ASSERT_TRUE(strstr(result.actions[i].message, "MORNING CONTEXT") != NULL);
            break;
        }
    }
    SC_ASSERT_TRUE(has_briefing);

    sc_proactive_result_deinit(&result, &alloc);
}

static void proactive_no_milestone_at_7_sessions(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    SC_ASSERT_EQ(sc_proactive_check(&alloc, 7, 14, &result), SC_OK);

    bool has_milestone = false;
    for (size_t i = 0; i < result.count; i++) {
        if (result.actions[i].type == SC_PROACTIVE_MILESTONE) {
            has_milestone = true;
            break;
        }
    }
    SC_ASSERT_FALSE(has_milestone);

    sc_proactive_result_deinit(&result, &alloc);
}

static void proactive_commitment_follow_up(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    sc_commitment_t commitment;
    memset(&commitment, 0, sizeof(commitment));
    commitment.status = SC_COMMITMENT_ACTIVE;
    commitment.summary = "finish the report by Friday";
    commitment.summary_len = strlen(commitment.summary);
    commitment.created_at = "2024-01-15T10:00:00Z";

    SC_ASSERT_EQ(sc_proactive_check_extended(&alloc, 5, 14, &commitment, 1, NULL, NULL, 0, &result),
                 SC_OK);

    bool has_follow_up = false;
    for (size_t i = 0; i < result.count; i++) {
        if (result.actions[i].type == SC_PROACTIVE_COMMITMENT_FOLLOW_UP) {
            has_follow_up = true;
            SC_ASSERT_NOT_NULL(result.actions[i].message);
            SC_ASSERT_TRUE(strstr(result.actions[i].message, "finish the report") != NULL);
            SC_ASSERT_TRUE(strstr(result.actions[i].message, "finish the report") != NULL ||
                           strstr(result.actions[i].message, "COMMITMENT") != NULL);
            SC_ASSERT_EQ(result.actions[i].priority, 0.8);
            break;
        }
    }
    SC_ASSERT_TRUE(has_follow_up);

    sc_proactive_result_deinit(&result, &alloc);
}

static void proactive_pattern_insight(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    const char *subjects[] = {"exercise", "project deadlines"};
    const uint32_t counts[] = {7, 3};

    SC_ASSERT_EQ(sc_proactive_check_extended(&alloc, 5, 14, NULL, 0, subjects, counts, 2, &result),
                 SC_OK);

    bool has_insight = false;
    for (size_t i = 0; i < result.count; i++) {
        if (result.actions[i].type == SC_PROACTIVE_PATTERN_INSIGHT) {
            has_insight = true;
            SC_ASSERT_NOT_NULL(result.actions[i].message);
            SC_ASSERT_TRUE(strstr(result.actions[i].message, "exercise") != NULL);
            SC_ASSERT_TRUE(strstr(result.actions[i].message, "7") != NULL);
            SC_ASSERT_TRUE(strstr(result.actions[i].message, "exercise") != NULL);
            SC_ASSERT_EQ(result.actions[i].priority, 0.6);
            break;
        }
    }
    SC_ASSERT_TRUE(has_insight);

    sc_proactive_result_deinit(&result, &alloc);
}

static void proactive_extended_no_data(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_proactive_result_t result_basic;
    sc_proactive_result_t result_extended;
    memset(&result_basic, 0, sizeof(result_basic));
    memset(&result_extended, 0, sizeof(result_extended));

    SC_ASSERT_EQ(sc_proactive_check(&alloc, 5, 14, &result_basic), SC_OK);
    SC_ASSERT_EQ(
        sc_proactive_check_extended(&alloc, 5, 14, NULL, 0, NULL, NULL, 0, &result_extended),
        SC_OK);

    SC_ASSERT_EQ(result_basic.count, result_extended.count);
    for (size_t i = 0; i < result_basic.count; i++) {
        SC_ASSERT_EQ(result_basic.actions[i].type, result_extended.actions[i].type);
    }

    sc_proactive_result_deinit(&result_basic, &alloc);
    sc_proactive_result_deinit(&result_extended, &alloc);
}

static void proactive_build_context_formats(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    SC_ASSERT_EQ(sc_proactive_check(&alloc, 10, 9, &result), SC_OK);
    SC_ASSERT_TRUE(result.count >= 2);

    char *ctx = NULL;
    size_t ctx_len = 0;
    SC_ASSERT_EQ(sc_proactive_build_context(&result, &alloc, 8, &ctx, &ctx_len), SC_OK);
    SC_ASSERT_NOT_NULL(ctx);
    SC_ASSERT_TRUE(ctx_len > 0);
    SC_ASSERT_TRUE(strstr(ctx, "### Proactive Awareness") != NULL);
    SC_ASSERT_TRUE(strstr(ctx, "conversation #10") != NULL);
    SC_ASSERT_TRUE(strstr(ctx, "MORNING CONTEXT") != NULL);

    alloc.free(alloc.ctx, ctx, ctx_len + 1);
    sc_proactive_result_deinit(&result, &alloc);
}

static void proactive_check_null_out_fails(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_error_t err = sc_proactive_check(&alloc, 5, 14, NULL);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void proactive_result_deinit_null_safe(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_proactive_result_deinit(NULL, &alloc);
}

static void proactive_no_actions_at_normal_time(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    SC_ASSERT_EQ(sc_proactive_check(&alloc, 3, 14, &result), SC_OK);

    bool has_milestone = false;
    bool has_briefing = false;
    for (size_t i = 0; i < result.count; i++) {
        if (result.actions[i].type == SC_PROACTIVE_MILESTONE)
            has_milestone = true;
        if (result.actions[i].type == SC_PROACTIVE_MORNING_BRIEFING)
            has_briefing = true;
    }
    SC_ASSERT_FALSE(has_milestone);
    SC_ASSERT_FALSE(has_briefing);
    SC_ASSERT_TRUE(result.count >= 1u);

    sc_proactive_result_deinit(&result, &alloc);
}

#define MS_PER_HOUR (3600ULL * 1000ULL)

static void proactive_silence_triggers_after_threshold(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    uint64_t now_ms = 1000000000000ULL;
    uint64_t last_contact_ms = now_ms - (4 * 24 * MS_PER_HOUR);
    sc_silence_config_t config = {.threshold_hours = 72, .enabled = true};

    SC_ASSERT_EQ(sc_proactive_check_silence(&alloc, last_contact_ms, now_ms, &config, &result),
                 SC_OK);
    SC_ASSERT_EQ(result.count, 1u);
    SC_ASSERT_EQ(result.actions[0].type, SC_PROACTIVE_CHECK_IN);
    SC_ASSERT_EQ(result.actions[0].priority, 0.85);
    SC_ASSERT_NOT_NULL(result.actions[0].message);

    sc_proactive_result_deinit(&result, &alloc);
}

static void proactive_silence_no_trigger_within_threshold(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    uint64_t now_ms = 1000000000000ULL;
    uint64_t last_contact_ms = now_ms - (1 * 24 * MS_PER_HOUR);
    sc_silence_config_t config = {.threshold_hours = 72, .enabled = true};

    SC_ASSERT_EQ(sc_proactive_check_silence(&alloc, last_contact_ms, now_ms, &config, &result),
                 SC_OK);
    SC_ASSERT_EQ(result.count, 0u);

    sc_proactive_result_deinit(&result, &alloc);
}

static void proactive_silence_week_message(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    uint64_t now_ms = 1000000000000ULL;
    uint64_t last_contact_ms = now_ms - (8 * 24 * MS_PER_HOUR);
    sc_silence_config_t config = {.threshold_hours = 72, .enabled = true};

    SC_ASSERT_EQ(sc_proactive_check_silence(&alloc, last_contact_ms, now_ms, &config, &result),
                 SC_OK);
    SC_ASSERT_EQ(result.count, 1u);
    SC_ASSERT_NOT_NULL(result.actions[0].message);
    SC_ASSERT_TRUE(strstr(result.actions[0].message, "days ago") != NULL);

    sc_proactive_result_deinit(&result, &alloc);
}

static void proactive_starter_with_memory(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_memory_lru_create(&alloc, 100);
    SC_ASSERT_NOT_NULL(mem.ctx);

    static const char topic_cat[] = "conversation";
    sc_memory_category_t cat = {
        .tag = SC_MEMORY_CATEGORY_CUSTOM,
        .data.custom = {.name = topic_cat, .name_len = sizeof(topic_cat) - 1},
    };

    const char *key1 = "topic:contact_a:1";
    const char *content1 =
        "recent topics activities interests: user wanted to try that pasta recipe";
    static const char CONTACT[] = "contact_a";
    mem.vtable->store(mem.ctx, key1, strlen(key1), content1, strlen(content1), &cat, CONTACT,
                      sizeof(CONTACT) - 1);

    const char *key2 = "topic:contact_a:2";
    const char *content2 = "recent topics activities interests: new apartment move";
    mem.vtable->store(mem.ctx, key2, strlen(key2), content2, strlen(content2), &cat, CONTACT,
                      sizeof(CONTACT) - 1);

    char *out = NULL;
    size_t out_len = 0;
    sc_error_t err =
        sc_proactive_build_starter(&alloc, &mem, CONTACT, sizeof(CONTACT) - 1, &out, &out_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(out);
    SC_ASSERT_TRUE(out_len > 0);
    SC_ASSERT_TRUE(strstr(out, "starting points") != NULL || strstr(out, "conversation") != NULL);
    SC_ASSERT_TRUE(strstr(out, "pasta recipe") != NULL || strstr(out, "new apartment") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
}

static void proactive_starter_empty_memory(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_memory_lru_create(&alloc, 100);
    SC_ASSERT_NOT_NULL(mem.ctx);

    char *out = NULL;
    size_t out_len = 0;
    sc_error_t err = sc_proactive_build_starter(&alloc, &mem, "contact_empty", 13, &out, &out_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NULL(out);
    SC_ASSERT_EQ(out_len, 0);

    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
}

static void proactive_starter_null_memory(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    sc_error_t err = sc_proactive_build_starter(&alloc, NULL, "contact_a", 9, &out, &out_len);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
    SC_ASSERT_NULL(out);
}

static void proactive_silence_disabled(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    uint64_t now_ms = 1000000000000ULL;
    uint64_t last_contact_ms = now_ms - (4 * 24 * MS_PER_HOUR);
    sc_silence_config_t config = {.threshold_hours = 72, .enabled = false};

    SC_ASSERT_EQ(sc_proactive_check_silence(&alloc, last_contact_ms, now_ms, &config, &result),
                 SC_OK);
    SC_ASSERT_EQ(result.count, 0u);

    sc_proactive_result_deinit(&result, &alloc);
}

static void proactive_event_follow_up(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    sc_extracted_event_t event;
    memset(&event, 0, sizeof(event));
    event.description = sc_strndup(&alloc, "interview", 9);
    event.description_len = 9;
    event.temporal_ref = sc_strndup(&alloc, "Tuesday", 7);
    event.temporal_ref_len = 7;
    event.confidence = 0.8;

    SC_ASSERT_EQ(sc_proactive_check_events(&alloc, &event, 1, &result), SC_OK);
    SC_ASSERT_EQ(result.count, 1u);
    SC_ASSERT_EQ(result.actions[0].type, SC_PROACTIVE_CHECK_IN);
    SC_ASSERT_NOT_NULL(result.actions[0].message);
    SC_ASSERT_TRUE(strstr(result.actions[0].message, "interview") != NULL);
    SC_ASSERT_TRUE(strstr(result.actions[0].message, "Tuesday") != NULL);
    SC_ASSERT_EQ(result.actions[0].priority, 0.8);

    alloc.free(alloc.ctx, event.description, event.description_len + 1);
    alloc.free(alloc.ctx, event.temporal_ref, event.temporal_ref_len + 1);
    sc_proactive_result_deinit(&result, &alloc);
}

static void proactive_event_low_confidence_skipped(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    sc_extracted_event_t event;
    memset(&event, 0, sizeof(event));
    event.description = sc_strndup(&alloc, "meeting", 7);
    event.description_len = 7;
    event.temporal_ref = sc_strndup(&alloc, "soon", 4);
    event.temporal_ref_len = 4;
    event.confidence = 0.3;

    SC_ASSERT_EQ(sc_proactive_check_events(&alloc, &event, 1, &result), SC_OK);
    SC_ASSERT_EQ(result.count, 0u);

    alloc.free(alloc.ctx, event.description, event.description_len + 1);
    alloc.free(alloc.ctx, event.temporal_ref, event.temporal_ref_len + 1);
    sc_proactive_result_deinit(&result, &alloc);
}

static void proactive_silence_five_days_with_guidance(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    uint64_t now_ms = 1000000000000ULL;
    uint64_t last_contact_ms = now_ms - (5 * 24 * MS_PER_HOUR);
    sc_silence_config_t config = {.threshold_hours = 72, .enabled = true};

    SC_ASSERT_EQ(sc_proactive_check_silence(&alloc, last_contact_ms, now_ms, &config, &result),
                 SC_OK);
    SC_ASSERT_EQ(result.count, 1u);
    SC_ASSERT_NOT_NULL(result.actions[0].message);
    SC_ASSERT_TRUE(strstr(result.actions[0].message, "days ago") != NULL ||
                   strstr(result.actions[0].message, "day ago") != NULL);
    SC_ASSERT_TRUE(strstr(result.actions[0].message, "check-in") != NULL ||
                   strstr(result.actions[0].message, "CHECK-IN") != NULL ||
                   strstr(result.actions[0].message, "natural") != NULL);

    sc_proactive_result_deinit(&result, &alloc);
}

static void proactive_milestone_50_reflects_deep_familiarity(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    SC_ASSERT_EQ(sc_proactive_check(&alloc, 50, 14, &result), SC_OK);
    SC_ASSERT_TRUE(result.count > 0);

    bool has_milestone = false;
    for (size_t i = 0; i < result.count; i++) {
        if (result.actions[i].type == SC_PROACTIVE_MILESTONE) {
            has_milestone = true;
            SC_ASSERT_NOT_NULL(result.actions[i].message);
            SC_ASSERT_TRUE(strstr(result.actions[i].message, "50") != NULL);
            SC_ASSERT_TRUE(strstr(result.actions[i].message, "familiarity") != NULL ||
                           strstr(result.actions[i].message, "know them") != NULL ||
                           strstr(result.actions[i].message, "specificity") != NULL);
            break;
        }
    }
    SC_ASSERT_TRUE(has_milestone);

    sc_proactive_result_deinit(&result, &alloc);
}

static void proactive_morning_briefing_lists_active_commitments(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    sc_commitment_t commitments[3];
    memset(commitments, 0, sizeof(commitments));
    commitments[0].status = SC_COMMITMENT_ACTIVE;
    commitments[0].summary = "finish the quarterly report";
    commitments[0].summary_len = strlen(commitments[0].summary);
    commitments[1].status = SC_COMMITMENT_ACTIVE;
    commitments[1].summary = "call mom this weekend";
    commitments[1].summary_len = strlen(commitments[1].summary);
    commitments[2].status = SC_COMMITMENT_ACTIVE;
    commitments[2].summary = "gym three times";
    commitments[2].summary_len = strlen(commitments[2].summary);

    SC_ASSERT_EQ(sc_proactive_check_extended(&alloc, 5, 9, commitments, 3, NULL, NULL, 0, &result),
                 SC_OK);
    SC_ASSERT_TRUE(result.count > 0);

    bool has_briefing = false;
    for (size_t i = 0; i < result.count; i++) {
        if (result.actions[i].type == SC_PROACTIVE_MORNING_BRIEFING) {
            has_briefing = true;
            SC_ASSERT_NOT_NULL(result.actions[i].message);
            SC_ASSERT_TRUE(strstr(result.actions[i].message, "quarterly report") != NULL ||
                           strstr(result.actions[i].message, "call mom") != NULL ||
                           strstr(result.actions[i].message, "gym") != NULL ||
                           strstr(result.actions[i].message, "commitments") != NULL);
            break;
        }
    }
    SC_ASSERT_TRUE(has_briefing);

    sc_proactive_result_deinit(&result, &alloc);
}

static void proactive_event_yesterday_referenced_naturally(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    sc_extracted_event_t event;
    memset(&event, 0, sizeof(event));
    event.description = sc_strndup(&alloc, "dentist appointment", 19);
    event.description_len = 19;
    event.temporal_ref = sc_strndup(&alloc, "yesterday", 9);
    event.temporal_ref_len = 9;
    event.confidence = 0.9;

    SC_ASSERT_EQ(sc_proactive_check_events(&alloc, &event, 1, &result), SC_OK);
    SC_ASSERT_EQ(result.count, 1u);
    SC_ASSERT_NOT_NULL(result.actions[0].message);
    SC_ASSERT_TRUE(strstr(result.actions[0].message, "dentist") != NULL);
    SC_ASSERT_TRUE(strstr(result.actions[0].message, "yesterday") != NULL);

    alloc.free(alloc.ctx, event.description, event.description_len + 1);
    alloc.free(alloc.ctx, event.temporal_ref, event.temporal_ref_len + 1);
    sc_proactive_result_deinit(&result, &alloc);
}

static void proactive_starter_diverse_memories_produce_context(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_memory_lru_create(&alloc, 100);
    SC_ASSERT_NOT_NULL(mem.ctx);

    static const char topic_cat[] = "conversation";
    sc_memory_category_t cat = {
        .tag = SC_MEMORY_CATEGORY_CUSTOM,
        .data.custom = {.name = topic_cat, .name_len = sizeof(topic_cat) - 1},
    };
    static const char CONTACT[] = "contact_diverse";

    const char *keys[] = {"topic:contact_diverse:1", "topic:contact_diverse:2",
                          "topic:contact_diverse:3", "topic:contact_diverse:4",
                          "topic:contact_diverse:5", "topic:contact_diverse:6"};
    const char *contents[] = {
        "recent topics activities interests: planning a trip to Japan",
        "recent topics activities interests: new job at Acme Corp",
        "recent topics activities interests: training for a marathon",
        "recent topics activities interests: adopting a rescue dog",
        "recent topics activities interests: learning to cook Thai food",
        "recent topics activities interests: house renovation project",
    };
    for (int i = 0; i < 6; i++) {
        mem.vtable->store(mem.ctx, keys[i], strlen(keys[i]), contents[i], strlen(contents[i]), &cat,
                          CONTACT, sizeof(CONTACT) - 1);
    }

    char *out = NULL;
    size_t out_len = 0;
    sc_error_t err =
        sc_proactive_build_starter(&alloc, &mem, CONTACT, sizeof(CONTACT) - 1, &out, &out_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(out);
    SC_ASSERT_TRUE(out_len > 0);
    bool has_context = strstr(out, "Japan") != NULL || strstr(out, "marathon") != NULL ||
                       strstr(out, "dog") != NULL || strstr(out, "Thai") != NULL ||
                       strstr(out, "renovation") != NULL || strstr(out, "Acme") != NULL ||
                       strstr(out, "trip") != NULL || strstr(out, "job") != NULL;
    SC_ASSERT_TRUE(has_context);

    alloc.free(alloc.ctx, out, out_len + 1);
    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
}

static void proactive_event_cap_at_three(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    sc_extracted_event_t events[5];
    memset(events, 0, sizeof(events));
    const char *descs[] = {"a", "b", "c", "d", "e"};
    const char *temps[] = {"Mon", "Tue", "Wed", "Thu", "Fri"};
    for (int i = 0; i < 5; i++) {
        events[i].description = sc_strndup(&alloc, descs[i], 1);
        events[i].description_len = 1;
        events[i].temporal_ref = sc_strndup(&alloc, temps[i], 3);
        events[i].temporal_ref_len = 3;
        events[i].confidence = 0.9;
    }

    SC_ASSERT_EQ(sc_proactive_check_events(&alloc, events, 5, &result), SC_OK);
    SC_ASSERT_EQ(result.count, 3u);

    for (int i = 0; i < 5; i++) {
        alloc.free(alloc.ctx, events[i].description, events[i].description_len + 1);
        alloc.free(alloc.ctx, events[i].temporal_ref, events[i].temporal_ref_len + 1);
    }
    sc_proactive_result_deinit(&result, &alloc);
}

static void proactive_reminder_triggers_with_interests_and_cooldown(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    const char *contact = "alice";
    const char *interests = "hiking, cooking, chess";
    uint64_t now_ms = 48 * 3600 * 1000ULL;           /* 48 hours from epoch */
    uint64_t last_reminder_ms = 24 * 3600 * 1000ULL; /* 24h ago */

    sc_error_t err = sc_proactive_check_reminder(&alloc, contact, 5, interests, strlen(interests),
                                                 now_ms, last_reminder_ms, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(result.count, 1u);
    SC_ASSERT_EQ(result.actions[0].type, SC_PROACTIVE_REMINDER);
    SC_ASSERT_EQ(result.actions[0].priority, 0.75);
    SC_ASSERT_NOT_NULL(result.actions[0].message);
    SC_ASSERT_TRUE(strstr(result.actions[0].message, "PROACTIVE REMINDER") != NULL);
    SC_ASSERT_TRUE(strstr(result.actions[0].message, "alice") != NULL);
    SC_ASSERT_TRUE(strstr(result.actions[0].message, "Reply SKIP") != NULL);

    sc_proactive_result_deinit(&result, &alloc);
}

static void proactive_reminder_no_trigger_within_24h(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    const char *interests = "hiking";
    uint64_t now_ms = 25 * 3600 * 1000ULL;
    uint64_t last_reminder_ms = 24 * 3600 * 1000ULL; /* 1h ago */

    sc_error_t err = sc_proactive_check_reminder(&alloc, "bob", 3, interests, 6, now_ms,
                                                 last_reminder_ms, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(result.count, 0u);

    sc_proactive_result_deinit(&result, &alloc);
}

static void proactive_reminder_no_trigger_without_interests(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    sc_error_t err =
        sc_proactive_check_reminder(&alloc, "bob", 3, NULL, 0, 48 * 3600 * 1000ULL, 0, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(result.count, 0u);

    sc_proactive_result_deinit(&result, &alloc);
}

static void proactive_backoff_hours_returns_correct_thresholds(void) {
    SC_ASSERT_EQ(sc_proactive_backoff_hours(0), 72u);
    SC_ASSERT_EQ(sc_proactive_backoff_hours(1), 144u);
    SC_ASSERT_EQ(sc_proactive_backoff_hours(2), 288u);
    SC_ASSERT_EQ(sc_proactive_backoff_hours(3), UINT32_MAX);
    SC_ASSERT_EQ(sc_proactive_backoff_hours(10), UINT32_MAX);
}

void run_proactive_tests(void) {
    SC_TEST_SUITE("proactive");
    SC_RUN_TEST(proactive_milestone_at_10_sessions);
    SC_RUN_TEST(proactive_milestone_50_reflects_deep_familiarity);
    SC_RUN_TEST(proactive_morning_briefing_at_9am);
    SC_RUN_TEST(proactive_morning_briefing_lists_active_commitments);
    SC_RUN_TEST(proactive_no_milestone_at_7_sessions);
    SC_RUN_TEST(proactive_commitment_follow_up);
    SC_RUN_TEST(proactive_pattern_insight);
    SC_RUN_TEST(proactive_extended_no_data);
    SC_RUN_TEST(proactive_build_context_formats);
    SC_RUN_TEST(proactive_check_null_out_fails);
    SC_RUN_TEST(proactive_result_deinit_null_safe);
    SC_RUN_TEST(proactive_no_actions_at_normal_time);
    SC_RUN_TEST(proactive_silence_triggers_after_threshold);
    SC_RUN_TEST(proactive_silence_no_trigger_within_threshold);
    SC_RUN_TEST(proactive_silence_week_message);
    SC_RUN_TEST(proactive_silence_five_days_with_guidance);
    SC_RUN_TEST(proactive_silence_disabled);
    SC_RUN_TEST(proactive_starter_with_memory);
    SC_RUN_TEST(proactive_starter_diverse_memories_produce_context);
    SC_RUN_TEST(proactive_starter_empty_memory);
    SC_RUN_TEST(proactive_starter_null_memory);
    SC_RUN_TEST(proactive_event_follow_up);
    SC_RUN_TEST(proactive_event_yesterday_referenced_naturally);
    SC_RUN_TEST(proactive_event_low_confidence_skipped);
    SC_RUN_TEST(proactive_event_cap_at_three);
    SC_RUN_TEST(proactive_reminder_triggers_with_interests_and_cooldown);
    SC_RUN_TEST(proactive_reminder_no_trigger_within_24h);
    SC_RUN_TEST(proactive_reminder_no_trigger_without_interests);
    SC_RUN_TEST(proactive_backoff_hours_returns_correct_thresholds);
}
