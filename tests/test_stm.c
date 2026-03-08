#include "seaclaw/core/allocator.h"
#include "seaclaw/memory/stm.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>

static void stm_init_sets_session_id(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_stm_buffer_t buf;
    sc_error_t err = sc_stm_init(&buf, alloc, "sess-123", 8);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(buf.session_id);
    SC_ASSERT_EQ(buf.session_id_len, 8);
    SC_ASSERT_EQ(memcmp(buf.session_id, "sess-123", 8), 0);
    sc_stm_deinit(&buf);
}

static void stm_record_turn_stores_content(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_stm_buffer_t buf;
    sc_stm_init(&buf, alloc, "s1", 2);

    sc_error_t err = sc_stm_record_turn(&buf, "user", 4, "Hello there", 10, 1000);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(sc_stm_count(&buf), 1);

    const sc_stm_turn_t *t = sc_stm_get(&buf, 0);
    SC_ASSERT_NOT_NULL(t);
    SC_ASSERT_STR_EQ(t->role, "user");
    SC_ASSERT_EQ(t->content_len, 10);
    SC_ASSERT_EQ(memcmp(t->content, "Hello there", 10), 0);
    SC_ASSERT_EQ(t->timestamp_ms, 1000);

    sc_stm_deinit(&buf);
}

static void stm_wraps_at_max_turns(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_stm_buffer_t buf;
    sc_stm_init(&buf, alloc, "s2", 2);

    for (size_t i = 0; i < SC_STM_MAX_TURNS + 5; i++) {
        char content[32];
        int n = snprintf(content, sizeof(content), "turn-%zu", i);
        sc_stm_record_turn(&buf, "user", 4, content, (size_t)n, (uint64_t)i);
    }

    SC_ASSERT_EQ(sc_stm_count(&buf), SC_STM_MAX_TURNS);
    /* Oldest should be turn-5 (the 21st we added, 0-indexed = 20, but we keep last 20 so oldest is
     * turn 5) */
    const sc_stm_turn_t *oldest = sc_stm_get(&buf, 0);
    SC_ASSERT_NOT_NULL(oldest);
    SC_ASSERT_STR_EQ(oldest->content, "turn-5");

    sc_stm_deinit(&buf);
}

static void stm_build_context_formats_turns(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_stm_buffer_t buf;
    sc_stm_init(&buf, alloc, "s3", 2);

    sc_stm_record_turn(&buf, "user", 4, "Hi", 2, 1000);
    sc_stm_record_turn(&buf, "assistant", 9, "Hello!", 6, 1001);

    char *ctx = NULL;
    size_t ctx_len = 0;
    sc_error_t err = sc_stm_build_context(&buf, &alloc, &ctx, &ctx_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(ctx);
    SC_ASSERT_TRUE(strstr(ctx, "## Session Context") != NULL);
    SC_ASSERT_TRUE(strstr(ctx, "**user**: Hi") != NULL);
    SC_ASSERT_TRUE(strstr(ctx, "**assistant**: Hello!") != NULL);

    alloc.free(alloc.ctx, ctx, ctx_len + 1);
    sc_stm_deinit(&buf);
}

static void stm_clear_resets_buffer(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_stm_buffer_t buf;
    sc_stm_init(&buf, alloc, "s4", 2);

    sc_stm_record_turn(&buf, "user", 4, "test", 4, 1000);
    SC_ASSERT_EQ(sc_stm_count(&buf), 1);

    sc_stm_clear(&buf);
    SC_ASSERT_EQ(sc_stm_count(&buf), 0);
    SC_ASSERT_NULL(sc_stm_get(&buf, 0));

    sc_stm_deinit(&buf);
}

static void stm_get_returns_null_for_out_of_range(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_stm_buffer_t buf;
    sc_stm_init(&buf, alloc, "s5", 2);

    sc_stm_record_turn(&buf, "user", 4, "one", 3, 1000);
    SC_ASSERT_NULL(sc_stm_get(&buf, 1));
    SC_ASSERT_NULL(sc_stm_get(&buf, 99));

    sc_stm_deinit(&buf);
}

void run_stm_tests(void) {
    SC_TEST_SUITE("stm");
    SC_RUN_TEST(stm_init_sets_session_id);
    SC_RUN_TEST(stm_record_turn_stores_content);
    SC_RUN_TEST(stm_wraps_at_max_turns);
    SC_RUN_TEST(stm_build_context_formats_turns);
    SC_RUN_TEST(stm_clear_resets_buffer);
    SC_RUN_TEST(stm_get_returns_null_for_out_of_range);
}
