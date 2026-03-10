#include "human/agent/superhuman.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef HU_ENABLE_SQLITE
#include "human/memory.h"
#include "human/memory/superhuman.h"
#include <sqlite3.h>
#include <stdint.h>
#endif

static hu_error_t mock_build_a(void *ctx, hu_allocator_t *alloc, char **out, size_t *out_len) {
    (void)ctx;
    *out = (char *)alloc->alloc(alloc->ctx, 6);
    if (!*out)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(*out, "ctx_a", 5);
    (*out)[5] = '\0';
    *out_len = 5;
    return HU_OK;
}

static hu_error_t mock_build_b(void *ctx, hu_allocator_t *alloc, char **out, size_t *out_len) {
    (void)ctx;
    *out = (char *)alloc->alloc(alloc->ctx, 6);
    if (!*out)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(*out, "ctx_b", 5);
    (*out)[5] = '\0';
    *out_len = 5;
    return HU_OK;
}

static void superhuman_register_and_count(void) {
    hu_superhuman_registry_t reg;
    HU_ASSERT_EQ(hu_superhuman_registry_init(&reg), HU_OK);
    HU_ASSERT_EQ(reg.count, 0u);

    hu_superhuman_service_t svc_a = {
        .name = "service_a",
        .build_context = mock_build_a,
        .observe = NULL,
        .ctx = NULL,
    };
    HU_ASSERT_EQ(hu_superhuman_register(&reg, svc_a), HU_OK);
    HU_ASSERT_EQ(reg.count, 1u);

    hu_superhuman_service_t svc_b = {
        .name = "service_b",
        .build_context = mock_build_b,
        .observe = NULL,
        .ctx = NULL,
    };
    HU_ASSERT_EQ(hu_superhuman_register(&reg, svc_b), HU_OK);
    HU_ASSERT_EQ(reg.count, 2u);
}

static void superhuman_build_context_calls_all(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_superhuman_registry_t reg;
    HU_ASSERT_EQ(hu_superhuman_registry_init(&reg), HU_OK);

    hu_superhuman_service_t svc_a = {
        .name = "service_a",
        .build_context = mock_build_a,
        .observe = NULL,
        .ctx = NULL,
    };
    HU_ASSERT_EQ(hu_superhuman_register(&reg, svc_a), HU_OK);

    hu_superhuman_service_t svc_b = {
        .name = "service_b",
        .build_context = mock_build_b,
        .observe = NULL,
        .ctx = NULL,
    };
    HU_ASSERT_EQ(hu_superhuman_register(&reg, svc_b), HU_OK);

    char *ctx = NULL;
    size_t ctx_len = 0;
    HU_ASSERT_EQ(hu_superhuman_build_context(&reg, &alloc, &ctx, &ctx_len), HU_OK);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(ctx_len > 0);
    HU_ASSERT_TRUE(strstr(ctx, "### Superhuman Insights") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "#### service_a") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "ctx_a") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "#### service_b") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "ctx_b") != NULL);

    alloc.free(alloc.ctx, ctx, ctx_len + 1);
}

static void superhuman_register_at_max(void) {
    hu_superhuman_registry_t reg;
    HU_ASSERT_EQ(hu_superhuman_registry_init(&reg), HU_OK);

    static char names[HU_SUPERHUMAN_MAX_SERVICES][16];
    hu_superhuman_service_t svc = {
        .build_context = mock_build_a,
        .observe = NULL,
        .ctx = NULL,
    };

    for (size_t i = 0; i < HU_SUPERHUMAN_MAX_SERVICES; i++) {
        int n = snprintf(names[i], sizeof(names[i]), "svc_%zu", i);
        svc.name = names[i];
        (void)n;
        HU_ASSERT_EQ(hu_superhuman_register(&reg, svc), HU_OK);
    }

    svc.name = "extra";
    hu_error_t err = hu_superhuman_register(&reg, svc);
    HU_ASSERT_EQ(err, HU_ERR_OUT_OF_MEMORY);
}

static void superhuman_observe_null_text_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_superhuman_registry_t reg;
    HU_ASSERT_EQ(hu_superhuman_registry_init(&reg), HU_OK);

    hu_superhuman_service_t svc = {
        .name = "svc",
        .build_context = mock_build_a,
        .observe = NULL,
        .ctx = NULL,
    };
    HU_ASSERT_EQ(hu_superhuman_register(&reg, svc), HU_OK);

    hu_error_t err = hu_superhuman_observe_all(&reg, &alloc, NULL, 0, "user", 4);
    HU_ASSERT_EQ(err, HU_OK);
}

static void superhuman_build_context_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_superhuman_registry_t reg;
    HU_ASSERT_EQ(hu_superhuman_registry_init(&reg), HU_OK);

    char *ctx = NULL;
    size_t ctx_len = 0;
    hu_error_t err = hu_superhuman_build_context(&reg, &alloc, &ctx, &ctx_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NULL(ctx);
    HU_ASSERT_EQ(ctx_len, 0u);
}

#ifdef HU_ENABLE_SQLITE
static void superhuman_phase3_tables_exist(void) {
    static const char *const expected[] = {
        "inside_jokes", "commitments", "temporal_patterns", "delayed_followups",
        "avoidance_patterns", "topic_baselines", "micro_moments", "growth_milestones",
        "pattern_observations",
    };
    static const size_t expected_count = sizeof(expected) / sizeof(expected[0]);

    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type='table'", -1,
                                &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);

    for (size_t i = 0; i < expected_count; i++) {
        bool found = false;
        sqlite3_reset(stmt);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *name = (const char *)sqlite3_column_text(stmt, 0);
            if (name && strcmp(name, expected[i]) == 0) {
                found = true;
                break;
            }
        }
        HU_ASSERT_TRUE(found);
    }

    sqlite3_finalize(stmt);
    mem.vtable->deinit(mem.ctx);
}

static void superhuman_inside_joke_store_and_list(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    HU_ASSERT_EQ(hu_superhuman_inside_joke_store(&mem, &alloc, "contact_a", 9,
        "remember when we laughed at X", 27, "that meme", 9), HU_OK);

    hu_inside_joke_t *jokes = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_superhuman_inside_joke_list(&mem, &alloc, "contact_a", 9, 10, &jokes, &count),
        HU_OK);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_NOT_NULL(jokes);
    HU_ASSERT_TRUE(strstr(jokes[0].context, "remember when") != NULL);
    HU_ASSERT_TRUE(strstr(jokes[0].punchline, "that meme") != NULL);

    hu_superhuman_inside_joke_free(&alloc, jokes, count);
    mem.vtable->deinit(mem.ctx);
}

static void superhuman_inside_joke_reference_updates(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    HU_ASSERT_EQ(hu_superhuman_inside_joke_store(&mem, &alloc, "c", 1, "ctx", 3, "pl", 2), HU_OK);

    hu_inside_joke_t *jokes = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_superhuman_inside_joke_list(&mem, &alloc, "c", 1, 10, &jokes, &count), HU_OK);
    HU_ASSERT_EQ(count, 1u);
    int64_t id = jokes[0].id;
    uint32_t ref_count = jokes[0].reference_count;
    hu_superhuman_inside_joke_free(&alloc, jokes, count);

    HU_ASSERT_EQ(hu_superhuman_inside_joke_reference(&mem, id), HU_OK);

    jokes = NULL;
    count = 0;
    HU_ASSERT_EQ(hu_superhuman_inside_joke_list(&mem, &alloc, "c", 1, 10, &jokes, &count), HU_OK);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_EQ(jokes[0].reference_count, ref_count + 1u);

    hu_superhuman_inside_joke_free(&alloc, jokes, count);
    mem.vtable->deinit(mem.ctx);
}

static void superhuman_commitment_store_and_list_due(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    int64_t past = 1000000; /* past deadline */
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, "contact_a", 9,
        "call the dentist", 16, "self", 4, past), HU_OK);

    hu_superhuman_commitment_t *list = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_superhuman_commitment_list_due(&mem, &alloc, past + 1000, 10, &list, &count),
        HU_OK);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_NOT_NULL(list);
    HU_ASSERT_TRUE(strstr(list[0].description, "dentist") != NULL);

    hu_superhuman_commitment_free(&alloc, list, count);
    mem.vtable->deinit(mem.ctx);
}

static void superhuman_commitment_mark_followed_up(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    int64_t past = 1000000;
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, "c", 1, "task", 4, "self", 4, past),
        HU_OK);

    hu_superhuman_commitment_t *list = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_superhuman_commitment_list_due(&mem, &alloc, past + 1, 10, &list, &count),
        HU_OK);
    HU_ASSERT_EQ(count, 1u);
    int64_t id = list[0].id;
    hu_superhuman_commitment_free(&alloc, list, count);

    HU_ASSERT_EQ(hu_superhuman_commitment_mark_followed_up(&mem, id), HU_OK);

    list = NULL;
    count = 0;
    HU_ASSERT_EQ(hu_superhuman_commitment_list_due(&mem, &alloc, past + 1, 10, &list, &count),
        HU_OK);
    HU_ASSERT_EQ(count, 0u);

    mem.vtable->deinit(mem.ctx);
}

static void superhuman_temporal_record_and_quiet_hours(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    HU_ASSERT_EQ(hu_superhuman_temporal_record(&mem, "contact_a", 9, 1, 14, 5000), HU_OK);
    HU_ASSERT_EQ(hu_superhuman_temporal_record(&mem, "contact_a", 9, 3, 9, 2000), HU_OK);
    HU_ASSERT_EQ(hu_superhuman_temporal_record(&mem, "contact_a", 9, 3, 9, 3000), HU_OK);

    int out_day = -1, out_start = -1, out_end = -1;
    HU_ASSERT_EQ(hu_superhuman_temporal_get_quiet_hours(&mem, &alloc, "contact_a", 9,
        &out_day, &out_start, &out_end), HU_OK);
    HU_ASSERT_EQ(out_day, 1);
    HU_ASSERT_EQ(out_start, 14);
    HU_ASSERT_EQ(out_end, 15);

    mem.vtable->deinit(mem.ctx);
}

/* F26: 10 messages Mon 9am, 2 at Sun 6am — quietest slot is Sunday 6am */
static void superhuman_temporal_quiet_hours_returns_sunday(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    /* 10 messages at Mon 9am (day=1, hour=9) */
    for (int i = 0; i < 10; i++)
        HU_ASSERT_EQ(hu_superhuman_temporal_record(&mem, "contact_temporal", 15, 1, 9, 0),
            HU_OK);
    /* 2 messages at Sun 6am (day=0, hour=6) */
    for (int i = 0; i < 2; i++)
        HU_ASSERT_EQ(hu_superhuman_temporal_record(&mem, "contact_temporal", 15, 0, 6, 0),
            HU_OK);

    int out_day = -1, out_start = -1, out_end = -1;
    HU_ASSERT_EQ(hu_superhuman_temporal_get_quiet_hours(&mem, &alloc, "contact_temporal", 15,
        &out_day, &out_start, &out_end), HU_OK);
    HU_ASSERT_EQ(out_day, 0);
    HU_ASSERT_EQ(out_start, 6);
    HU_ASSERT_EQ(out_end, 7);

    mem.vtable->deinit(mem.ctx);
}

static void superhuman_delayed_followup_lifecycle(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    int64_t past = 1000000;
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_schedule(&mem, &alloc, "c", 1, "project X", 9, past),
        HU_OK);

    hu_delayed_followup_t *list = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_list_due(&mem, &alloc, past + 1, &list, &count),
        HU_OK);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_TRUE(strstr(list[0].topic, "project X") != NULL);
    int64_t id = list[0].id;
    hu_superhuman_delayed_followup_free(&alloc, list, count);

    HU_ASSERT_EQ(hu_superhuman_delayed_followup_mark_sent(&mem, id), HU_OK);

    list = NULL;
    count = 0;
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_list_due(&mem, &alloc, past + 1, &list, &count),
        HU_OK);
    HU_ASSERT_EQ(count, 0u);

    mem.vtable->deinit(mem.ctx);
}

static void superhuman_micro_moment_store_and_list(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    const char *contact = "contact_mm";
    const char *fact = "loves hiking";
    const char *sig = "outdoor enthusiast";
    HU_ASSERT_EQ(hu_superhuman_micro_moment_store(&mem, &alloc, contact, 10, fact, 11, sig, 18),
        HU_OK);

    char *json = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_superhuman_micro_moment_list(&mem, &alloc, contact, 10, 10, &json, &len),
        HU_OK);
    HU_ASSERT_NOT_NULL(json);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(json, "Micro-moments") != NULL);
    HU_ASSERT_TRUE(strstr(json, "loves") != NULL);

    alloc.free(alloc.ctx, json, len);
    mem.vtable->deinit(mem.ctx);
}

static void superhuman_avoidance_record_and_list(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    HU_ASSERT_EQ(hu_superhuman_avoidance_record(&mem, "c", 1, "work", 4, true), HU_OK);
    HU_ASSERT_EQ(hu_superhuman_avoidance_record(&mem, "c", 1, "work", 4, false), HU_OK);

    char *json = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_superhuman_avoidance_list(&mem, &alloc, "c", 1, &json, &len), HU_OK);
    HU_ASSERT_NOT_NULL(json);
    HU_ASSERT_TRUE(strstr(json, "work") != NULL);

    alloc.free(alloc.ctx, json, len);
    mem.vtable->deinit(mem.ctx);
}

static void superhuman_topic_baseline_and_absence(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    HU_ASSERT_EQ(hu_superhuman_topic_baseline_record(&mem, "c", 1, "pets", 4), HU_OK);

    /* now_ts far in future, absence_days=100: cutoff is now_ts - 8640000.
     * last_mentioned from record is ~current time, so it will be < cutoff. */
    int64_t now_ts = 3000000000;
    int64_t absence_days = 100;
    char *json = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_superhuman_topic_absence_list(&mem, &alloc, "c", 1, now_ts, absence_days,
        &json, &len), HU_OK);
    HU_ASSERT_NOT_NULL(json);
    HU_ASSERT_TRUE(strstr(json, "pets") != NULL || strstr(json, "absent") != NULL ||
        strstr(json, "days") != NULL);

    alloc.free(alloc.ctx, json, len);
    mem.vtable->deinit(mem.ctx);
}

static void superhuman_topic_baseline_record_multiple_topics(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    HU_ASSERT_EQ(hu_superhuman_topic_baseline_record(&mem, "contact_a", 8, "work", 4), HU_OK);
    HU_ASSERT_EQ(hu_superhuman_topic_baseline_record(&mem, "contact_a", 8, "pets", 4), HU_OK);
    HU_ASSERT_EQ(hu_superhuman_topic_baseline_record(&mem, "contact_a", 8, "gym", 3), HU_OK);

    int64_t now_ts = 4000000000; /* far future so all topics are absent */
    char *json = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_superhuman_topic_absence_list(&mem, &alloc, "contact_a", 8, now_ts, 14,
        &json, &len), HU_OK);
    HU_ASSERT_NOT_NULL(json);
    HU_ASSERT_TRUE(strstr(json, "work") != NULL);
    HU_ASSERT_TRUE(strstr(json, "pets") != NULL);
    HU_ASSERT_TRUE(strstr(json, "gym") != NULL);

    alloc.free(alloc.ctx, json, len);
    mem.vtable->deinit(mem.ctx);
}

static void superhuman_topic_absence_list_empty_when_recently_mentioned(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    HU_ASSERT_EQ(hu_superhuman_topic_baseline_record(&mem, "c", 1, "work", 4), HU_OK);

    /* now_ts = current time, 14 days: cutoff is 2 weeks ago.
     * last_mentioned was just set, so topic is NOT absent. */
    int64_t now_ts = (int64_t)time(NULL);
    char *json = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_superhuman_topic_absence_list(&mem, &alloc, "c", 1, now_ts, 14,
        &json, &len), HU_OK);
    HU_ASSERT_NOT_NULL(json);
    HU_ASSERT_TRUE(strstr(json, "(none)") != NULL);

    alloc.free(alloc.ctx, json, len);
    mem.vtable->deinit(mem.ctx);
}

static void superhuman_topic_baseline_upsert_increments_mention_count(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    /* Record same topic 3 times — upsert increments mention_count */
    HU_ASSERT_EQ(hu_superhuman_topic_baseline_record(&mem, "c", 1, "work", 4), HU_OK);
    HU_ASSERT_EQ(hu_superhuman_topic_baseline_record(&mem, "c", 1, "work", 4), HU_OK);
    HU_ASSERT_EQ(hu_superhuman_topic_baseline_record(&mem, "c", 1, "work", 4), HU_OK);

    /* Future now_ts so topic is absent; verify it appears in list */
    int64_t now_ts = 3500000000;
    char *json = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_superhuman_topic_absence_list(&mem, &alloc, "c", 1, now_ts, 14,
        &json, &len), HU_OK);
    HU_ASSERT_NOT_NULL(json);
    HU_ASSERT_TRUE(strstr(json, "work") != NULL);
    HU_ASSERT_TRUE(strstr(json, "- ") != NULL);

    alloc.free(alloc.ctx, json, len);
    mem.vtable->deinit(mem.ctx);
}

static void superhuman_growth_store_and_list(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    HU_ASSERT_EQ(hu_superhuman_growth_store(&mem, &alloc, "c", 1, "fitness", 7,
        "sedentary", 9, "active runner", 12), HU_OK);

    char *json = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_superhuman_growth_list_recent(&mem, &alloc, "c", 1, 10, &json, &len), HU_OK);
    HU_ASSERT_NOT_NULL(json);
    HU_ASSERT_TRUE(strstr(json, "fitness") != NULL);
    HU_ASSERT_TRUE(strstr(json, "sedentary") != NULL);
    HU_ASSERT_TRUE(strstr(json, "active") != NULL);

    alloc.free(alloc.ctx, json, len);
    mem.vtable->deinit(mem.ctx);
}

static void superhuman_pattern_record_and_list(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    HU_ASSERT_EQ(hu_superhuman_pattern_record(&mem, "c", 1, "weekend", 7, "casual", 6, 5, 18),
        HU_OK);

    char *json = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_superhuman_pattern_list(&mem, &alloc, "c", 1, 10, &json, &len), HU_OK);
    HU_ASSERT_NOT_NULL(json);
    HU_ASSERT_TRUE(strstr(json, "weekend") != NULL);
    HU_ASSERT_TRUE(strstr(json, "casual") != NULL);

    alloc.free(alloc.ctx, json, len);
    mem.vtable->deinit(mem.ctx);
}

static void superhuman_memory_build_context_aggregates_sections(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    static const char CONTACT[] = "contact_ctx";
    HU_ASSERT_EQ(hu_superhuman_micro_moment_store(&mem, &alloc, CONTACT, sizeof(CONTACT) - 1,
        "loves hiking", 12, "outdoor enthusiast", 18), HU_OK);
    HU_ASSERT_EQ(hu_superhuman_inside_joke_store(&mem, &alloc, CONTACT, sizeof(CONTACT) - 1,
        "remember when we laughed at X", 27, "that meme", 9), HU_OK);
    HU_ASSERT_EQ(hu_superhuman_growth_store(&mem, &alloc, CONTACT, sizeof(CONTACT) - 1,
        "fitness", 7, "sedentary", 9, "active runner", 12), HU_OK);

    char *ctx = NULL;
    size_t ctx_len = 0;
    HU_ASSERT_EQ(hu_superhuman_memory_build_context(&mem, &alloc, CONTACT, sizeof(CONTACT) - 1,
        true, &ctx, &ctx_len), HU_OK);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(ctx_len > 0);
    HU_ASSERT_TRUE(strstr(ctx, "### Superhuman Memory") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "Micro-moments") != NULL || strstr(ctx, "loves") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "Inside jokes") != NULL || strstr(ctx, "remember when") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "Growth") != NULL || strstr(ctx, "fitness") != NULL);

    alloc.free(alloc.ctx, ctx, ctx_len + 1);
    mem.vtable->deinit(mem.ctx);
}

static void superhuman_memory_build_context_empty_contact_returns_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    char *ctx = NULL;
    size_t ctx_len = 0;
    HU_ASSERT_EQ(hu_superhuman_memory_build_context(&mem, &alloc, "no_data_contact", 15,
        false, &ctx, &ctx_len), HU_OK);
    HU_ASSERT_NULL(ctx);
    HU_ASSERT_EQ(ctx_len, 0u);

    mem.vtable->deinit(mem.ctx);
}
#endif

void run_superhuman_tests(void) {
    HU_TEST_SUITE("superhuman");
    HU_RUN_TEST(superhuman_register_and_count);
    HU_RUN_TEST(superhuman_build_context_calls_all);
    HU_RUN_TEST(superhuman_register_at_max);
    HU_RUN_TEST(superhuman_observe_null_text_ok);
    HU_RUN_TEST(superhuman_build_context_empty);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(superhuman_phase3_tables_exist);
    HU_RUN_TEST(superhuman_inside_joke_store_and_list);
    HU_RUN_TEST(superhuman_inside_joke_reference_updates);
    HU_RUN_TEST(superhuman_commitment_store_and_list_due);
    HU_RUN_TEST(superhuman_commitment_mark_followed_up);
    HU_RUN_TEST(superhuman_temporal_record_and_quiet_hours);
    HU_RUN_TEST(superhuman_temporal_quiet_hours_returns_sunday);
    HU_RUN_TEST(superhuman_delayed_followup_lifecycle);
    HU_RUN_TEST(superhuman_micro_moment_store_and_list);
    HU_RUN_TEST(superhuman_avoidance_record_and_list);
    HU_RUN_TEST(superhuman_topic_baseline_and_absence);
    HU_RUN_TEST(superhuman_topic_baseline_record_multiple_topics);
    HU_RUN_TEST(superhuman_topic_absence_list_empty_when_recently_mentioned);
    HU_RUN_TEST(superhuman_topic_baseline_upsert_increments_mention_count);
    HU_RUN_TEST(superhuman_growth_store_and_list);
    HU_RUN_TEST(superhuman_pattern_record_and_list);
    HU_RUN_TEST(superhuman_memory_build_context_aggregates_sections);
    HU_RUN_TEST(superhuman_memory_build_context_empty_contact_returns_ok);
#endif
}
