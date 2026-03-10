#include "human/core/allocator.h"
#include "human/memory/stm.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>

static void stm_init_sets_session_id(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_stm_buffer_t buf;
    hu_error_t err = hu_stm_init(&buf, alloc, "sess-123", 8);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(buf.session_id);
    HU_ASSERT_EQ(buf.session_id_len, 8);
    HU_ASSERT_EQ(memcmp(buf.session_id, "sess-123", 8), 0);
    hu_stm_deinit(&buf);
}

static void stm_record_turn_stores_content(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_stm_buffer_t buf;
    hu_stm_init(&buf, alloc, "s1", 2);

    hu_error_t err = hu_stm_record_turn(&buf, "user", 4, "Hello there", 10, 1000);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(hu_stm_count(&buf), 1);

    const hu_stm_turn_t *t = hu_stm_get(&buf, 0);
    HU_ASSERT_NOT_NULL(t);
    HU_ASSERT_STR_EQ(t->role, "user");
    HU_ASSERT_EQ(t->content_len, 10);
    HU_ASSERT_EQ(memcmp(t->content, "Hello there", 10), 0);
    HU_ASSERT_EQ(t->timestamp_ms, 1000);

    hu_stm_deinit(&buf);
}

static void stm_wraps_at_max_turns(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_stm_buffer_t buf;
    hu_stm_init(&buf, alloc, "s2", 2);

    for (size_t i = 0; i < HU_STM_MAX_TURNS + 5; i++) {
        char content[32];
        int n = snprintf(content, sizeof(content), "turn-%zu", i);
        hu_stm_record_turn(&buf, "user", 4, content, (size_t)n, (uint64_t)i);
    }

    HU_ASSERT_EQ(hu_stm_count(&buf), HU_STM_MAX_TURNS);
    /* Oldest should be turn-5 (the 21st we added, 0-indexed = 20, but we keep last 20 so oldest is
     * turn 5) */
    const hu_stm_turn_t *oldest = hu_stm_get(&buf, 0);
    HU_ASSERT_NOT_NULL(oldest);
    HU_ASSERT_STR_EQ(oldest->content, "turn-5");

    hu_stm_deinit(&buf);
}

static void stm_build_context_formats_turns(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_stm_buffer_t buf;
    hu_stm_init(&buf, alloc, "s3", 2);

    hu_stm_record_turn(&buf, "user", 4, "Hi", 2, 1000);
    hu_stm_record_turn(&buf, "assistant", 9, "Hello!", 6, 1001);

    char *ctx = NULL;
    size_t ctx_len = 0;
    hu_error_t err = hu_stm_build_context(&buf, &alloc, &ctx, &ctx_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(strstr(ctx, "## Session Context") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "**user**: Hi") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "**assistant**: Hello!") != NULL);

    alloc.free(alloc.ctx, ctx, ctx_len + 1);
    hu_stm_deinit(&buf);
}

static void stm_clear_resets_buffer(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_stm_buffer_t buf;
    hu_stm_init(&buf, alloc, "s4", 2);

    hu_stm_record_turn(&buf, "user", 4, "test", 4, 1000);
    HU_ASSERT_EQ(hu_stm_count(&buf), 1);

    hu_stm_clear(&buf);
    HU_ASSERT_EQ(hu_stm_count(&buf), 0);
    HU_ASSERT_NULL(hu_stm_get(&buf, 0));

    hu_stm_deinit(&buf);
}

static void stm_get_returns_null_for_out_of_range(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_stm_buffer_t buf;
    hu_stm_init(&buf, alloc, "s5", 2);

    hu_stm_record_turn(&buf, "user", 4, "one", 3, 1000);
    HU_ASSERT_NULL(hu_stm_get(&buf, 1));
    HU_ASSERT_NULL(hu_stm_get(&buf, 99));

    hu_stm_deinit(&buf);
}

static void stm_context_includes_emotions(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_stm_buffer_t buf;
    hu_stm_init(&buf, alloc, "s6", 2);

    hu_stm_record_turn(&buf, "user", 4, "Hello", 5, 1000);
    hu_error_t err = hu_stm_turn_add_emotion(&buf, 0, HU_EMOTION_JOY, 0.8);
    HU_ASSERT_EQ(err, HU_OK);

    char *ctx = NULL;
    size_t ctx_len = 0;
    err = hu_stm_build_context(&buf, &alloc, &ctx, &ctx_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_NOT_NULL(strstr(ctx, "joy"));
    HU_ASSERT_NOT_NULL(strstr(ctx, "high"));

    alloc.free(alloc.ctx, ctx, ctx_len + 1);
    hu_stm_deinit(&buf);
}

static void stm_context_no_emotions_skips_section(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_stm_buffer_t buf;
    hu_stm_init(&buf, alloc, "s7", 2);

    hu_stm_record_turn(&buf, "user", 4, "Hello", 5, 1000);

    char *ctx = NULL;
    size_t ctx_len = 0;
    hu_error_t err = hu_stm_build_context(&buf, &alloc, &ctx, &ctx_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_NULL(strstr(ctx, "Emotional"));

    alloc.free(alloc.ctx, ctx, ctx_len + 1);
    hu_stm_deinit(&buf);
}

static void stm_context_multiple_emotions(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_stm_buffer_t buf;
    hu_stm_init(&buf, alloc, "s8", 2);

    hu_stm_record_turn(&buf, "user", 4, "Hi", 2, 1000);
    hu_stm_turn_add_emotion(&buf, 0, HU_EMOTION_JOY, 0.8);
    hu_stm_turn_add_emotion(&buf, 0, HU_EMOTION_ANXIETY, 0.5);

    char *ctx = NULL;
    size_t ctx_len = 0;
    hu_error_t err = hu_stm_build_context(&buf, &alloc, &ctx, &ctx_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_NOT_NULL(strstr(ctx, "joy"));
    HU_ASSERT_NOT_NULL(strstr(ctx, "anxiety"));

    alloc.free(alloc.ctx, ctx, ctx_len + 1);
    hu_stm_deinit(&buf);
}

static void stm_init_null_session_id_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_stm_buffer_t buf;
    hu_error_t err = hu_stm_init(&buf, alloc, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NULL(buf.session_id);
    HU_ASSERT_EQ(buf.session_id_len, 0u);
    hu_stm_deinit(&buf);
}

static void stm_record_turn_null_content_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_stm_buffer_t buf;
    hu_stm_init(&buf, alloc, "s", 1);

    hu_error_t err = hu_stm_record_turn(&buf, "user", 4, NULL, 0, 1000);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_stm_count(&buf), 0u);

    hu_stm_deinit(&buf);
}

static void stm_turn_add_entity_at_max(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_stm_buffer_t buf;
    hu_stm_init(&buf, alloc, "s", 1);
    hu_stm_record_turn(&buf, "user", 4, "Hi", 2, 1000);

    for (size_t i = 0; i < HU_STM_MAX_ENTITIES; i++) {
        char name[16];
        int n = snprintf(name, sizeof(name), "ent%zu", i);
        hu_error_t err = hu_stm_turn_add_entity(&buf, 0, name, (size_t)n, "person", 6, 1);
        HU_ASSERT_EQ(err, HU_OK);
    }

    hu_error_t err = hu_stm_turn_add_entity(&buf, 0, "extra", 5, "person", 6, 1);
    HU_ASSERT_EQ(err, HU_ERR_OUT_OF_MEMORY);

    hu_stm_deinit(&buf);
}

static void stm_turn_add_emotion_deduplicates(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_stm_buffer_t buf;
    hu_stm_init(&buf, alloc, "s", 1);
    hu_stm_record_turn(&buf, "user", 4, "Hi", 2, 1000);

    hu_error_t err1 = hu_stm_turn_add_emotion(&buf, 0, HU_EMOTION_JOY, 0.8);
    HU_ASSERT_EQ(err1, HU_OK);
    hu_error_t err2 = hu_stm_turn_add_emotion(&buf, 0, HU_EMOTION_JOY, 0.9);
    HU_ASSERT_EQ(err2, HU_OK);

    const hu_stm_turn_t *t = hu_stm_get(&buf, 0);
    HU_ASSERT_NOT_NULL(t);
    HU_ASSERT_EQ(t->emotion_count, 1u);

    hu_stm_deinit(&buf);
}

static void stm_set_primary_topic(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_stm_buffer_t buf;
    hu_stm_init(&buf, alloc, "s", 1);
    hu_stm_record_turn(&buf, "user", 4, "Hello", 5, 1000);

    hu_error_t err = hu_stm_turn_set_primary_topic(&buf, 0, "work", 4);
    HU_ASSERT_EQ(err, HU_OK);

    const hu_stm_turn_t *t = hu_stm_get(&buf, 0);
    HU_ASSERT_NOT_NULL(t);
    HU_ASSERT_NOT_NULL(t->primary_topic);
    HU_ASSERT_EQ(strlen(t->primary_topic), 4u);
    HU_ASSERT_EQ(memcmp(t->primary_topic, "work", 4), 0);

    hu_stm_deinit(&buf);
}

static void stm_set_primary_topic_out_of_range(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_stm_buffer_t buf;
    hu_stm_init(&buf, alloc, "s", 1);
    hu_stm_record_turn(&buf, "user", 4, "Hi", 2, 1000);

    hu_error_t err = hu_stm_turn_set_primary_topic(&buf, 99, "work", 4);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    hu_stm_deinit(&buf);
}

static void stm_emotion_overflow(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_stm_buffer_t buf;
    hu_stm_init(&buf, alloc, "s", 1);
    hu_stm_record_turn(&buf, "user", 4, "Hi", 2, 1000);

    hu_emotion_tag_t tags[] = {HU_EMOTION_JOY, HU_EMOTION_SADNESS, HU_EMOTION_ANGER, HU_EMOTION_FEAR,
                               HU_EMOTION_SURPRISE};
    for (size_t i = 0; i < HU_STM_MAX_EMOTIONS; i++) {
        hu_error_t err = hu_stm_turn_add_emotion(&buf, 0, tags[i], 0.8);
        HU_ASSERT_EQ(err, HU_OK);
    }

    hu_error_t err = hu_stm_turn_add_emotion(&buf, 0, HU_EMOTION_FRUSTRATION, 0.8);
    HU_ASSERT_EQ(err, HU_ERR_OUT_OF_MEMORY);

    hu_stm_deinit(&buf);
}

static void stm_build_context_null_alloc(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_stm_buffer_t buf;
    hu_stm_init(&buf, alloc, "s", 1);
    hu_stm_record_turn(&buf, "user", 4, "Hi", 2, 1000);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_stm_build_context(&buf, NULL, &out, &out_len);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NULL(out);

    hu_stm_deinit(&buf);
}

void run_stm_tests(void) {
    HU_TEST_SUITE("stm");
    HU_RUN_TEST(stm_init_sets_session_id);
    HU_RUN_TEST(stm_record_turn_stores_content);
    HU_RUN_TEST(stm_wraps_at_max_turns);
    HU_RUN_TEST(stm_build_context_formats_turns);
    HU_RUN_TEST(stm_clear_resets_buffer);
    HU_RUN_TEST(stm_get_returns_null_for_out_of_range);
    HU_RUN_TEST(stm_context_includes_emotions);
    HU_RUN_TEST(stm_context_no_emotions_skips_section);
    HU_RUN_TEST(stm_context_multiple_emotions);
    HU_RUN_TEST(stm_init_null_session_id_ok);
    HU_RUN_TEST(stm_record_turn_null_content_fails);
    HU_RUN_TEST(stm_turn_add_entity_at_max);
    HU_RUN_TEST(stm_turn_add_emotion_deduplicates);
    HU_RUN_TEST(stm_set_primary_topic);
    HU_RUN_TEST(stm_set_primary_topic_out_of_range);
    HU_RUN_TEST(stm_emotion_overflow);
    HU_RUN_TEST(stm_build_context_null_alloc);
}
