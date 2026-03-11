typedef int hu_test_theory_of_mind_unused_;

#ifdef HU_ENABLE_SQLITE

#include "human/context/theory_of_mind.h"
#include "human/core/allocator.h"
#include "human/memory.h"
#include "test_framework.h"
#include <string.h>

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

static void update_with_ten_messages_baseline_set(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_channel_history_entry_t entries[10];
    for (size_t i = 0; i < 10; i++) {
        entries[i] = make_entry(false, "hello there how are you today", "12:00");
    }

    hu_error_t err =
        hu_theory_of_mind_update_baseline(&mem, &alloc, "contact_a", 9, entries, 10);
    HU_ASSERT_EQ(err, HU_OK);

    hu_contact_baseline_t baseline;
    err = hu_theory_of_mind_get_baseline(&mem, &alloc, "contact_a", 9, &baseline);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(baseline.avg_message_length > 0.0);
    HU_ASSERT_TRUE(baseline.messages_sampled >= 10);

    mem.vtable->deinit(mem.ctx);
}

static void detect_deviation_short_messages_length_drop_true(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_channel_history_entry_t long_entries[10];
    for (size_t i = 0; i < 10; i++) {
        long_entries[i] = make_entry(false,
                                     "this is a much longer message that establishes a high baseline "
                                     "for average message length",
                                     "12:00");
    }

    hu_error_t err =
        hu_theory_of_mind_update_baseline(&mem, &alloc, "contact_b", 9, long_entries, 10);
    HU_ASSERT_EQ(err, HU_OK);

    hu_contact_baseline_t baseline;
    err = hu_theory_of_mind_get_baseline(&mem, &alloc, "contact_b", 9, &baseline);
    HU_ASSERT_EQ(err, HU_OK);

    hu_channel_history_entry_t short_entries[5];
    for (size_t i = 0; i < 5; i++) {
        short_entries[i] = make_entry(false, "ok", "12:01");
    }

    hu_theory_of_mind_deviation_t dev =
        hu_theory_of_mind_detect_deviation(&baseline, short_entries, 5);
    HU_ASSERT_TRUE(dev.length_drop);

    mem.vtable->deinit(mem.ctx);
}

static void build_inference_high_severity_returns_non_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_theory_of_mind_deviation_t dev = {0};
    dev.length_drop = true;
    dev.emoji_drop = true;
    dev.severity = 0.8f;

    size_t out_len = 0;
    char *s = hu_theory_of_mind_build_inference(&alloc, "Mindy", 5, "she", 3, &dev, &out_len);
    HU_ASSERT_NOT_NULL(s);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(s, "shorter") != NULL || strstr(s, "emoji") != NULL);
    HU_ASSERT_TRUE(strstr(s, "THEORY OF MIND") != NULL);
    alloc.free(alloc.ctx, s, out_len + 1);
}

static void build_inference_low_severity_returns_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_theory_of_mind_deviation_t dev = {0};
    dev.severity = 0.2f;

    size_t out_len = 0;
    char *s = hu_theory_of_mind_build_inference(&alloc, "Test", 4, NULL, 0, &dev, &out_len);
    HU_ASSERT_NULL(s);
    HU_ASSERT_EQ(out_len, 0u);
}

static void get_baseline_nonexistent_returns_not_found(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_contact_baseline_t baseline;
    hu_error_t err = hu_theory_of_mind_get_baseline(&mem, &alloc, "nonexistent", 11, &baseline);
    HU_ASSERT_EQ(err, HU_ERR_NOT_FOUND);

    mem.vtable->deinit(mem.ctx);
}

static void update_fewer_than_five_messages_no_baseline(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_channel_history_entry_t entries[3];
    for (size_t i = 0; i < 3; i++) {
        entries[i] = make_entry(false, "hi", "12:00");
    }

    hu_error_t err =
        hu_theory_of_mind_update_baseline(&mem, &alloc, "contact_c", 9, entries, 3);
    HU_ASSERT_EQ(err, HU_OK);

    hu_contact_baseline_t baseline;
    err = hu_theory_of_mind_get_baseline(&mem, &alloc, "contact_c", 9, &baseline);
    HU_ASSERT_EQ(err, HU_ERR_NOT_FOUND);

    mem.vtable->deinit(mem.ctx);
}

void run_theory_of_mind_tests(void) {
    HU_TEST_SUITE("theory_of_mind");
    HU_RUN_TEST(update_with_ten_messages_baseline_set);
    HU_RUN_TEST(detect_deviation_short_messages_length_drop_true);
    HU_RUN_TEST(build_inference_high_severity_returns_non_null);
    HU_RUN_TEST(build_inference_low_severity_returns_null);
    HU_RUN_TEST(get_baseline_nonexistent_returns_not_found);
    HU_RUN_TEST(update_fewer_than_five_messages_no_baseline);
}

#else

void run_theory_of_mind_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_SQLITE */
