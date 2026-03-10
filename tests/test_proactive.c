#include "human/agent/commitment.h"
#include "human/agent/proactive.h"
#include "human/context/event_extract.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/memory.h"
#include "human/memory/engines.h"
#include "test_framework.h"
#include <string.h>

static void proactive_milestone_at_10_sessions(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    HU_ASSERT_EQ(hu_proactive_check(&alloc, 10, 14, &result), HU_OK);
    HU_ASSERT_TRUE(result.count > 0);

    bool has_milestone = false;
    for (size_t i = 0; i < result.count; i++) {
        if (result.actions[i].type == HU_PROACTIVE_MILESTONE) {
            has_milestone = true;
            HU_ASSERT_NOT_NULL(result.actions[i].message);
            HU_ASSERT_TRUE(strstr(result.actions[i].message, "conversation #10") != NULL);
            HU_ASSERT_TRUE(strstr(result.actions[i].message, "MILESTONE") != NULL);
            break;
        }
    }
    HU_ASSERT_TRUE(has_milestone);

    hu_proactive_result_deinit(&result, &alloc);
}

static void proactive_morning_briefing_at_9am(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    HU_ASSERT_EQ(hu_proactive_check(&alloc, 5, 9, &result), HU_OK);
    HU_ASSERT_TRUE(result.count > 0);

    bool has_briefing = false;
    for (size_t i = 0; i < result.count; i++) {
        if (result.actions[i].type == HU_PROACTIVE_MORNING_BRIEFING) {
            has_briefing = true;
            HU_ASSERT_NOT_NULL(result.actions[i].message);
            HU_ASSERT_TRUE(strstr(result.actions[i].message, "MORNING CONTEXT") != NULL);
            break;
        }
    }
    HU_ASSERT_TRUE(has_briefing);

    hu_proactive_result_deinit(&result, &alloc);
}

static void proactive_no_milestone_at_7_sessions(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    HU_ASSERT_EQ(hu_proactive_check(&alloc, 7, 14, &result), HU_OK);

    bool has_milestone = false;
    for (size_t i = 0; i < result.count; i++) {
        if (result.actions[i].type == HU_PROACTIVE_MILESTONE) {
            has_milestone = true;
            break;
        }
    }
    HU_ASSERT_FALSE(has_milestone);

    hu_proactive_result_deinit(&result, &alloc);
}

static void proactive_commitment_follow_up(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    hu_commitment_t commitment;
    memset(&commitment, 0, sizeof(commitment));
    commitment.status = HU_COMMITMENT_ACTIVE;
    commitment.summary = "finish the report by Friday";
    commitment.summary_len = strlen(commitment.summary);
    commitment.created_at = "2024-01-15T10:00:00Z";

    HU_ASSERT_EQ(hu_proactive_check_extended(&alloc, 5, 14, &commitment, 1, NULL, NULL, 0, &result),
                 HU_OK);

    bool has_follow_up = false;
    for (size_t i = 0; i < result.count; i++) {
        if (result.actions[i].type == HU_PROACTIVE_COMMITMENT_FOLLOW_UP) {
            has_follow_up = true;
            HU_ASSERT_NOT_NULL(result.actions[i].message);
            HU_ASSERT_TRUE(strstr(result.actions[i].message, "finish the report") != NULL);
            HU_ASSERT_TRUE(strstr(result.actions[i].message, "finish the report") != NULL ||
                           strstr(result.actions[i].message, "COMMITMENT") != NULL);
            HU_ASSERT_EQ(result.actions[i].priority, 0.8);
            break;
        }
    }
    HU_ASSERT_TRUE(has_follow_up);

    hu_proactive_result_deinit(&result, &alloc);
}

static void proactive_pattern_insight(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    const char *subjects[] = {"exercise", "project deadlines"};
    const uint32_t counts[] = {7, 3};

    HU_ASSERT_EQ(hu_proactive_check_extended(&alloc, 5, 14, NULL, 0, subjects, counts, 2, &result),
                 HU_OK);

    bool has_insight = false;
    for (size_t i = 0; i < result.count; i++) {
        if (result.actions[i].type == HU_PROACTIVE_PATTERN_INSIGHT) {
            has_insight = true;
            HU_ASSERT_NOT_NULL(result.actions[i].message);
            HU_ASSERT_TRUE(strstr(result.actions[i].message, "exercise") != NULL);
            HU_ASSERT_TRUE(strstr(result.actions[i].message, "7") != NULL);
            HU_ASSERT_TRUE(strstr(result.actions[i].message, "exercise") != NULL);
            HU_ASSERT_EQ(result.actions[i].priority, 0.6);
            break;
        }
    }
    HU_ASSERT_TRUE(has_insight);

    hu_proactive_result_deinit(&result, &alloc);
}

static void proactive_extended_no_data(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_proactive_result_t result_basic;
    hu_proactive_result_t result_extended;
    memset(&result_basic, 0, sizeof(result_basic));
    memset(&result_extended, 0, sizeof(result_extended));

    HU_ASSERT_EQ(hu_proactive_check(&alloc, 5, 14, &result_basic), HU_OK);
    HU_ASSERT_EQ(
        hu_proactive_check_extended(&alloc, 5, 14, NULL, 0, NULL, NULL, 0, &result_extended),
        HU_OK);

    HU_ASSERT_EQ(result_basic.count, result_extended.count);
    for (size_t i = 0; i < result_basic.count; i++) {
        HU_ASSERT_EQ(result_basic.actions[i].type, result_extended.actions[i].type);
    }

    hu_proactive_result_deinit(&result_basic, &alloc);
    hu_proactive_result_deinit(&result_extended, &alloc);
}

static void proactive_build_context_formats(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    HU_ASSERT_EQ(hu_proactive_check(&alloc, 10, 9, &result), HU_OK);
    HU_ASSERT_TRUE(result.count >= 2);

    char *ctx = NULL;
    size_t ctx_len = 0;
    HU_ASSERT_EQ(hu_proactive_build_context(&result, &alloc, 8, &ctx, &ctx_len), HU_OK);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(ctx_len > 0);
    HU_ASSERT_TRUE(strstr(ctx, "### Proactive Awareness") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "conversation #10") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "MORNING CONTEXT") != NULL);

    alloc.free(alloc.ctx, ctx, ctx_len + 1);
    hu_proactive_result_deinit(&result, &alloc);
}

static void proactive_check_null_out_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_proactive_check(&alloc, 5, 14, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void proactive_result_deinit_null_safe(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_proactive_result_deinit(NULL, &alloc);
}

static void proactive_no_actions_at_normal_time(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    HU_ASSERT_EQ(hu_proactive_check(&alloc, 3, 14, &result), HU_OK);

    bool has_milestone = false;
    bool has_briefing = false;
    for (size_t i = 0; i < result.count; i++) {
        if (result.actions[i].type == HU_PROACTIVE_MILESTONE)
            has_milestone = true;
        if (result.actions[i].type == HU_PROACTIVE_MORNING_BRIEFING)
            has_briefing = true;
    }
    HU_ASSERT_FALSE(has_milestone);
    HU_ASSERT_FALSE(has_briefing);
    HU_ASSERT_TRUE(result.count >= 1u);

    hu_proactive_result_deinit(&result, &alloc);
}

#define MS_PER_HOUR (3600ULL * 1000ULL)

static void proactive_silence_triggers_after_threshold(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    uint64_t now_ms = 1000000000000ULL;
    uint64_t last_contact_ms = now_ms - (4 * 24 * MS_PER_HOUR);
    hu_silence_config_t config = {.threshold_hours = 72, .enabled = true};

    HU_ASSERT_EQ(hu_proactive_check_silence(&alloc, last_contact_ms, now_ms, &config, &result),
                 HU_OK);
    HU_ASSERT_EQ(result.count, 1u);
    HU_ASSERT_EQ(result.actions[0].type, HU_PROACTIVE_CHECK_IN);
    HU_ASSERT_EQ(result.actions[0].priority, 0.85);
    HU_ASSERT_NOT_NULL(result.actions[0].message);

    hu_proactive_result_deinit(&result, &alloc);
}

static void proactive_silence_no_trigger_within_threshold(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    uint64_t now_ms = 1000000000000ULL;
    uint64_t last_contact_ms = now_ms - (1 * 24 * MS_PER_HOUR);
    hu_silence_config_t config = {.threshold_hours = 72, .enabled = true};

    HU_ASSERT_EQ(hu_proactive_check_silence(&alloc, last_contact_ms, now_ms, &config, &result),
                 HU_OK);
    HU_ASSERT_EQ(result.count, 0u);

    hu_proactive_result_deinit(&result, &alloc);
}

static void proactive_silence_week_message(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    uint64_t now_ms = 1000000000000ULL;
    uint64_t last_contact_ms = now_ms - (8 * 24 * MS_PER_HOUR);
    hu_silence_config_t config = {.threshold_hours = 72, .enabled = true};

    HU_ASSERT_EQ(hu_proactive_check_silence(&alloc, last_contact_ms, now_ms, &config, &result),
                 HU_OK);
    HU_ASSERT_EQ(result.count, 1u);
    HU_ASSERT_NOT_NULL(result.actions[0].message);
    HU_ASSERT_TRUE(strstr(result.actions[0].message, "days ago") != NULL);

    hu_proactive_result_deinit(&result, &alloc);
}

static void proactive_starter_with_memory(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_memory_lru_create(&alloc, 100);
    HU_ASSERT_NOT_NULL(mem.ctx);

    static const char topic_cat[] = "conversation";
    hu_memory_category_t cat = {
        .tag = HU_MEMORY_CATEGORY_CUSTOM,
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
    hu_error_t err =
        hu_proactive_build_starter(&alloc, &mem, CONTACT, sizeof(CONTACT) - 1, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(out, "starting points") != NULL || strstr(out, "conversation") != NULL);
    HU_ASSERT_TRUE(strstr(out, "pasta recipe") != NULL || strstr(out, "new apartment") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
}

static void proactive_starter_empty_memory(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_memory_lru_create(&alloc, 100);
    HU_ASSERT_NOT_NULL(mem.ctx);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_proactive_build_starter(&alloc, &mem, "contact_empty", 13, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ(out_len, 0);

    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
}

static void proactive_starter_null_memory(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_proactive_build_starter(&alloc, NULL, "contact_a", 9, &out, &out_len);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NULL(out);
}

static void proactive_silence_disabled(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    uint64_t now_ms = 1000000000000ULL;
    uint64_t last_contact_ms = now_ms - (4 * 24 * MS_PER_HOUR);
    hu_silence_config_t config = {.threshold_hours = 72, .enabled = false};

    HU_ASSERT_EQ(hu_proactive_check_silence(&alloc, last_contact_ms, now_ms, &config, &result),
                 HU_OK);
    HU_ASSERT_EQ(result.count, 0u);

    hu_proactive_result_deinit(&result, &alloc);
}

static void proactive_event_follow_up(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    hu_extracted_event_t event;
    memset(&event, 0, sizeof(event));
    event.description = hu_strndup(&alloc, "interview", 9);
    event.description_len = 9;
    event.temporal_ref = hu_strndup(&alloc, "Tuesday", 7);
    event.temporal_ref_len = 7;
    event.confidence = 0.8;

    HU_ASSERT_EQ(hu_proactive_check_events(&alloc, &event, 1, &result), HU_OK);
    HU_ASSERT_EQ(result.count, 1u);
    HU_ASSERT_EQ(result.actions[0].type, HU_PROACTIVE_CHECK_IN);
    HU_ASSERT_NOT_NULL(result.actions[0].message);
    HU_ASSERT_TRUE(strstr(result.actions[0].message, "interview") != NULL);
    HU_ASSERT_TRUE(strstr(result.actions[0].message, "Tuesday") != NULL);
    HU_ASSERT_EQ(result.actions[0].priority, 0.8);

    alloc.free(alloc.ctx, event.description, event.description_len + 1);
    alloc.free(alloc.ctx, event.temporal_ref, event.temporal_ref_len + 1);
    hu_proactive_result_deinit(&result, &alloc);
}

static void proactive_event_low_confidence_skipped(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    hu_extracted_event_t event;
    memset(&event, 0, sizeof(event));
    event.description = hu_strndup(&alloc, "meeting", 7);
    event.description_len = 7;
    event.temporal_ref = hu_strndup(&alloc, "soon", 4);
    event.temporal_ref_len = 4;
    event.confidence = 0.3;

    HU_ASSERT_EQ(hu_proactive_check_events(&alloc, &event, 1, &result), HU_OK);
    HU_ASSERT_EQ(result.count, 0u);

    alloc.free(alloc.ctx, event.description, event.description_len + 1);
    alloc.free(alloc.ctx, event.temporal_ref, event.temporal_ref_len + 1);
    hu_proactive_result_deinit(&result, &alloc);
}

static void proactive_silence_five_days_with_guidance(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    uint64_t now_ms = 1000000000000ULL;
    uint64_t last_contact_ms = now_ms - (5 * 24 * MS_PER_HOUR);
    hu_silence_config_t config = {.threshold_hours = 72, .enabled = true};

    HU_ASSERT_EQ(hu_proactive_check_silence(&alloc, last_contact_ms, now_ms, &config, &result),
                 HU_OK);
    HU_ASSERT_EQ(result.count, 1u);
    HU_ASSERT_NOT_NULL(result.actions[0].message);
    HU_ASSERT_TRUE(strstr(result.actions[0].message, "days ago") != NULL ||
                   strstr(result.actions[0].message, "day ago") != NULL);
    HU_ASSERT_TRUE(strstr(result.actions[0].message, "check-in") != NULL ||
                   strstr(result.actions[0].message, "CHECK-IN") != NULL ||
                   strstr(result.actions[0].message, "natural") != NULL);

    hu_proactive_result_deinit(&result, &alloc);
}

static void proactive_milestone_50_reflects_deep_familiarity(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    HU_ASSERT_EQ(hu_proactive_check(&alloc, 50, 14, &result), HU_OK);
    HU_ASSERT_TRUE(result.count > 0);

    bool has_milestone = false;
    for (size_t i = 0; i < result.count; i++) {
        if (result.actions[i].type == HU_PROACTIVE_MILESTONE) {
            has_milestone = true;
            HU_ASSERT_NOT_NULL(result.actions[i].message);
            HU_ASSERT_TRUE(strstr(result.actions[i].message, "50") != NULL);
            HU_ASSERT_TRUE(strstr(result.actions[i].message, "familiarity") != NULL ||
                           strstr(result.actions[i].message, "know them") != NULL ||
                           strstr(result.actions[i].message, "specificity") != NULL);
            break;
        }
    }
    HU_ASSERT_TRUE(has_milestone);

    hu_proactive_result_deinit(&result, &alloc);
}

static void proactive_morning_briefing_lists_active_commitments(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    hu_commitment_t commitments[3];
    memset(commitments, 0, sizeof(commitments));
    commitments[0].status = HU_COMMITMENT_ACTIVE;
    commitments[0].summary = "finish the quarterly report";
    commitments[0].summary_len = strlen(commitments[0].summary);
    commitments[1].status = HU_COMMITMENT_ACTIVE;
    commitments[1].summary = "call mom this weekend";
    commitments[1].summary_len = strlen(commitments[1].summary);
    commitments[2].status = HU_COMMITMENT_ACTIVE;
    commitments[2].summary = "gym three times";
    commitments[2].summary_len = strlen(commitments[2].summary);

    HU_ASSERT_EQ(hu_proactive_check_extended(&alloc, 5, 9, commitments, 3, NULL, NULL, 0, &result),
                 HU_OK);
    HU_ASSERT_TRUE(result.count > 0);

    bool has_briefing = false;
    for (size_t i = 0; i < result.count; i++) {
        if (result.actions[i].type == HU_PROACTIVE_MORNING_BRIEFING) {
            has_briefing = true;
            HU_ASSERT_NOT_NULL(result.actions[i].message);
            HU_ASSERT_TRUE(strstr(result.actions[i].message, "quarterly report") != NULL ||
                           strstr(result.actions[i].message, "call mom") != NULL ||
                           strstr(result.actions[i].message, "gym") != NULL ||
                           strstr(result.actions[i].message, "commitments") != NULL);
            break;
        }
    }
    HU_ASSERT_TRUE(has_briefing);

    hu_proactive_result_deinit(&result, &alloc);
}

static void proactive_event_yesterday_referenced_naturally(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    hu_extracted_event_t event;
    memset(&event, 0, sizeof(event));
    event.description = hu_strndup(&alloc, "dentist appointment", 19);
    event.description_len = 19;
    event.temporal_ref = hu_strndup(&alloc, "yesterday", 9);
    event.temporal_ref_len = 9;
    event.confidence = 0.9;

    HU_ASSERT_EQ(hu_proactive_check_events(&alloc, &event, 1, &result), HU_OK);
    HU_ASSERT_EQ(result.count, 1u);
    HU_ASSERT_NOT_NULL(result.actions[0].message);
    HU_ASSERT_TRUE(strstr(result.actions[0].message, "dentist") != NULL);
    HU_ASSERT_TRUE(strstr(result.actions[0].message, "yesterday") != NULL);

    alloc.free(alloc.ctx, event.description, event.description_len + 1);
    alloc.free(alloc.ctx, event.temporal_ref, event.temporal_ref_len + 1);
    hu_proactive_result_deinit(&result, &alloc);
}

static void proactive_starter_diverse_memories_produce_context(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_memory_lru_create(&alloc, 100);
    HU_ASSERT_NOT_NULL(mem.ctx);

    static const char topic_cat[] = "conversation";
    hu_memory_category_t cat = {
        .tag = HU_MEMORY_CATEGORY_CUSTOM,
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
    hu_error_t err =
        hu_proactive_build_starter(&alloc, &mem, CONTACT, sizeof(CONTACT) - 1, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    bool has_context = strstr(out, "Japan") != NULL || strstr(out, "marathon") != NULL ||
                       strstr(out, "dog") != NULL || strstr(out, "Thai") != NULL ||
                       strstr(out, "renovation") != NULL || strstr(out, "Acme") != NULL ||
                       strstr(out, "trip") != NULL || strstr(out, "job") != NULL;
    HU_ASSERT_TRUE(has_context);

    alloc.free(alloc.ctx, out, out_len + 1);
    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
}

static void proactive_event_cap_at_three(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    hu_extracted_event_t events[5];
    memset(events, 0, sizeof(events));
    const char *descs[] = {"a", "b", "c", "d", "e"};
    const char *temps[] = {"Mon", "Tue", "Wed", "Thu", "Fri"};
    for (int i = 0; i < 5; i++) {
        events[i].description = hu_strndup(&alloc, descs[i], 1);
        events[i].description_len = 1;
        events[i].temporal_ref = hu_strndup(&alloc, temps[i], 3);
        events[i].temporal_ref_len = 3;
        events[i].confidence = 0.9;
    }

    HU_ASSERT_EQ(hu_proactive_check_events(&alloc, events, 5, &result), HU_OK);
    HU_ASSERT_EQ(result.count, 3u);

    for (int i = 0; i < 5; i++) {
        alloc.free(alloc.ctx, events[i].description, events[i].description_len + 1);
        alloc.free(alloc.ctx, events[i].temporal_ref, events[i].temporal_ref_len + 1);
    }
    hu_proactive_result_deinit(&result, &alloc);
}

static void proactive_reminder_triggers_with_interests_and_cooldown(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    const char *contact = "alice";
    const char *interests = "hiking, cooking, chess";
    uint64_t now_ms = 48 * 3600 * 1000ULL;           /* 48 hours from epoch */
    uint64_t last_reminder_ms = 24 * 3600 * 1000ULL; /* 24h ago */

    hu_error_t err = hu_proactive_check_reminder(&alloc, contact, 5, interests, strlen(interests),
                                                 now_ms, last_reminder_ms, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(result.count, 1u);
    HU_ASSERT_EQ(result.actions[0].type, HU_PROACTIVE_REMINDER);
    HU_ASSERT_EQ(result.actions[0].priority, 0.75);
    HU_ASSERT_NOT_NULL(result.actions[0].message);
    HU_ASSERT_TRUE(strstr(result.actions[0].message, "PROACTIVE REMINDER") != NULL);
    HU_ASSERT_TRUE(strstr(result.actions[0].message, "alice") != NULL);
    HU_ASSERT_TRUE(strstr(result.actions[0].message, "Reply SKIP") != NULL);

    hu_proactive_result_deinit(&result, &alloc);
}

static void proactive_reminder_no_trigger_within_24h(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    const char *interests = "hiking";
    uint64_t now_ms = 25 * 3600 * 1000ULL;
    uint64_t last_reminder_ms = 24 * 3600 * 1000ULL; /* 1h ago */

    hu_error_t err = hu_proactive_check_reminder(&alloc, "bob", 3, interests, 6, now_ms,
                                                 last_reminder_ms, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(result.count, 0u);

    hu_proactive_result_deinit(&result, &alloc);
}

static void proactive_reminder_no_trigger_without_interests(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_proactive_result_t result;
    memset(&result, 0, sizeof(result));

    hu_error_t err =
        hu_proactive_check_reminder(&alloc, "bob", 3, NULL, 0, 48 * 3600 * 1000ULL, 0, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(result.count, 0u);

    hu_proactive_result_deinit(&result, &alloc);
}

static void proactive_backoff_hours_returns_correct_thresholds(void) {
    HU_ASSERT_EQ(hu_proactive_backoff_hours(0), 72u);
    HU_ASSERT_EQ(hu_proactive_backoff_hours(1), 144u);
    HU_ASSERT_EQ(hu_proactive_backoff_hours(2), 288u);
    HU_ASSERT_EQ(hu_proactive_backoff_hours(3), UINT32_MAX);
    HU_ASSERT_EQ(hu_proactive_backoff_hours(10), UINT32_MAX);
}

void run_proactive_tests(void) {
    HU_TEST_SUITE("proactive");
    HU_RUN_TEST(proactive_milestone_at_10_sessions);
    HU_RUN_TEST(proactive_milestone_50_reflects_deep_familiarity);
    HU_RUN_TEST(proactive_morning_briefing_at_9am);
    HU_RUN_TEST(proactive_morning_briefing_lists_active_commitments);
    HU_RUN_TEST(proactive_no_milestone_at_7_sessions);
    HU_RUN_TEST(proactive_commitment_follow_up);
    HU_RUN_TEST(proactive_pattern_insight);
    HU_RUN_TEST(proactive_extended_no_data);
    HU_RUN_TEST(proactive_build_context_formats);
    HU_RUN_TEST(proactive_check_null_out_fails);
    HU_RUN_TEST(proactive_result_deinit_null_safe);
    HU_RUN_TEST(proactive_no_actions_at_normal_time);
    HU_RUN_TEST(proactive_silence_triggers_after_threshold);
    HU_RUN_TEST(proactive_silence_no_trigger_within_threshold);
    HU_RUN_TEST(proactive_silence_week_message);
    HU_RUN_TEST(proactive_silence_five_days_with_guidance);
    HU_RUN_TEST(proactive_silence_disabled);
    HU_RUN_TEST(proactive_starter_with_memory);
    HU_RUN_TEST(proactive_starter_diverse_memories_produce_context);
    HU_RUN_TEST(proactive_starter_empty_memory);
    HU_RUN_TEST(proactive_starter_null_memory);
    HU_RUN_TEST(proactive_event_follow_up);
    HU_RUN_TEST(proactive_event_yesterday_referenced_naturally);
    HU_RUN_TEST(proactive_event_low_confidence_skipped);
    HU_RUN_TEST(proactive_event_cap_at_three);
    HU_RUN_TEST(proactive_reminder_triggers_with_interests_and_cooldown);
    HU_RUN_TEST(proactive_reminder_no_trigger_within_24h);
    HU_RUN_TEST(proactive_reminder_no_trigger_without_interests);
    HU_RUN_TEST(proactive_backoff_hours_returns_correct_thresholds);
}
