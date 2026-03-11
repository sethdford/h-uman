#include "human/context/protective.h"
#include "human/core/allocator.h"
#include "human/memory.h"
#include "test_framework.h"
#include <string.h>

static void memory_ok_pizza_any_time_returns_true(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);
    bool ok = hu_protective_memory_ok(&alloc, &mem, "c1", 2, "we had pizza last week", 22,
                                      0.5f, 14);
    HU_ASSERT_TRUE(ok);
    ok = hu_protective_memory_ok(&alloc, &mem, "c1", 2, "pizza", 5, -0.5f, 23);
    HU_ASSERT_TRUE(ok);
}

static void memory_ok_death_late_hour_returns_false(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);
    bool ok = hu_protective_memory_ok(&alloc, &mem, "c1", 2,
                                     "attended a death in the family", 30, -0.2f, 23);
    HU_ASSERT_FALSE(ok);
}

static void memory_ok_death_daytime_positive_valence_returns_true(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);
    bool ok = hu_protective_memory_ok(&alloc, &mem, "c1", 2,
                                     "death of a character in the movie", 34, 0.1f, 14);
    HU_ASSERT_TRUE(ok);
}

static void advice_ok_one_venting_returns_false(void) {
    const char *msgs[] = {"I'm so frustrated with work"};
    bool ok = hu_protective_advice_ok(msgs, 1);
    HU_ASSERT_FALSE(ok);
}

static void advice_ok_three_venting_returns_true(void) {
    const char *msgs[] = {"I'm so frustrated", "this is awful", "I'm overwhelmed"};
    bool ok = hu_protective_advice_ok(msgs, 3);
    HU_ASSERT_TRUE(ok);
}

#ifdef HU_ENABLE_SQLITE

static void is_boundary_divorce_returns_true(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_error_t err = hu_protective_add_boundary(&alloc, &mem, "c1", 2, "divorce", 7,
                                                "avoid", "explicit");
    HU_ASSERT_EQ(err, HU_OK);

    bool b = hu_protective_is_boundary(&mem, "c1", 2, "divorce", 7);
    HU_ASSERT_TRUE(b);

    mem.vtable->deinit(mem.ctx);
}

static void is_boundary_pizza_returns_false(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_error_t err = hu_protective_add_boundary(&alloc, &mem, "c1", 2, "divorce", 7,
                                                "avoid", "explicit");
    HU_ASSERT_EQ(err, HU_OK);

    bool b = hu_protective_is_boundary(&mem, "c1", 2, "pizza", 5);
    HU_ASSERT_FALSE(b);

    mem.vtable->deinit(mem.ctx);
}

static void add_boundary_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_error_t err = hu_protective_add_boundary(&alloc, &mem, "contact_a", 9,
                                                "work", 4, "redirect", "inferred");
    HU_ASSERT_EQ(err, HU_OK);

    bool b = hu_protective_is_boundary(&mem, "contact_a", 9, "work", 4);
    HU_ASSERT_TRUE(b);

    mem.vtable->deinit(mem.ctx);
}

void run_protective_tests(void) {
    HU_TEST_SUITE("protective");
    HU_RUN_TEST(memory_ok_pizza_any_time_returns_true);
    HU_RUN_TEST(memory_ok_death_late_hour_returns_false);
    HU_RUN_TEST(memory_ok_death_daytime_positive_valence_returns_true);
    HU_RUN_TEST(advice_ok_one_venting_returns_false);
    HU_RUN_TEST(advice_ok_three_venting_returns_true);
    HU_RUN_TEST(is_boundary_divorce_returns_true);
    HU_RUN_TEST(is_boundary_pizza_returns_false);
    HU_RUN_TEST(add_boundary_succeeds);
}

#else

void run_protective_tests(void) {
    HU_TEST_SUITE("protective");
    HU_RUN_TEST(memory_ok_pizza_any_time_returns_true);
    HU_RUN_TEST(memory_ok_death_late_hour_returns_false);
    HU_RUN_TEST(memory_ok_death_daytime_positive_valence_returns_true);
    HU_RUN_TEST(advice_ok_one_venting_returns_false);
    HU_RUN_TEST(advice_ok_three_venting_returns_true);
}

#endif /* HU_ENABLE_SQLITE */
