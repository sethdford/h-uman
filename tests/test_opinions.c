typedef int hu_test_opinions_unused_;

#ifdef HU_ENABLE_SQLITE

#include "human/core/allocator.h"
#include "human/humanness.h" /* hu_evolved_opinion_t + build_directive (firmness map) */
#include "human/memory.h"
#include "human/memory/opinions.h"
#include "test_framework.h"
#include <string.h>
#include <time.h>

static void test_opinions_upsert_get_pizza_best_food(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    int64_t now = (int64_t)time(NULL);
    hu_error_t err = hu_opinions_upsert(&alloc, &mem, "pizza", 5, "best food", 9, 0.8f, now);
    HU_ASSERT_EQ(err, HU_OK);

    hu_opinion_t *ops = NULL;
    size_t count = 0;
    err = hu_opinions_get(&alloc, &mem, "pizza", 5, &ops, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_NOT_NULL(ops[0].topic);
    HU_ASSERT_STR_EQ(ops[0].topic, "pizza");
    HU_ASSERT_STR_EQ(ops[0].position, "best food");
    HU_ASSERT_EQ(ops[0].superseded_by, 0);

    hu_opinions_free(&alloc, ops, count);
    mem.vtable->deinit(mem.ctx);
}

static void test_opinions_upsert_supersede_pizza_overrated(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    int64_t now = (int64_t)time(NULL);
    HU_ASSERT_EQ(hu_opinions_upsert(&alloc, &mem, "pizza", 5, "best food", 9, 0.8f, now), HU_OK);

    now += 100;
    HU_ASSERT_EQ(hu_opinions_upsert(&alloc, &mem, "pizza", 5, "overrated", 9, 0.6f, now), HU_OK);

    hu_opinion_t *ops = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_opinions_get(&alloc, &mem, "pizza", 5, &ops, &count), HU_OK);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_STR_EQ(ops[0].position, "overrated");
    hu_opinions_free(&alloc, ops, count);

    ops = NULL;
    count = 0;
    HU_ASSERT_EQ(hu_opinions_get_superseded(&alloc, &mem, "pizza", 5, &ops, &count), HU_OK);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_STR_EQ(ops[0].position, "best food");
    HU_ASSERT_NEQ(ops[0].superseded_by, 0);
    hu_opinions_free(&alloc, ops, count);

    mem.vtable->deinit(mem.ctx);
}

static void test_opinions_is_core_value_family(void) {
    const char *core_values[] = {"family", "honesty", "integrity"};
    HU_ASSERT_TRUE(hu_opinions_is_core_value("family", 6, core_values, 3));
    HU_ASSERT_TRUE(hu_opinions_is_core_value("Family", 6, core_values, 3));
    HU_ASSERT_TRUE(hu_opinions_is_core_value("HONESTY", 7, core_values, 3));
    HU_ASSERT_FALSE(hu_opinions_is_core_value("pizza", 5, core_values, 3));
    HU_ASSERT_FALSE(hu_opinions_is_core_value("fam", 3, core_values, 3));
}

/* A1 conviction loop AC-6: regression guard on the conviction->firmness
 * wording in hu_evolved_opinion_build_directive. The pre-generation stance
 * injection (agent_turn.c:2698) relies on this mapping; pin it so a future
 * edit can't silently flatten "firmly/moderately/tentatively". */
static void test_evolved_opinion_directive_firmness_mapping(void) {
    hu_allocator_t alloc = hu_system_allocator();

    char t_firm[] = "remote work", s_firm[] = "net positive";
    char t_mod[] = "tabs vs spaces", s_mod[] = "tabs win";
    char t_tent[] = "best pizza", s_tent[] = "thin crust";

    hu_evolved_opinion_t ops[3] = {
        {t_firm, strlen(t_firm), s_firm, strlen(s_firm), 0.9, 0, 7}, /* > 0.8 -> firmly */
        {t_mod, strlen(t_mod), s_mod, strlen(s_mod), 0.6, 0, 4},     /* > 0.5 -> moderately */
        {t_tent, strlen(t_tent), s_tent, strlen(s_tent), 0.3, 0, 2}, /* else -> tentatively */
    };

    size_t len = 0;
    char *dir = hu_evolved_opinion_build_directive(&alloc, ops, 3, 0.0, &len);
    HU_ASSERT_NOT_NULL(dir);
    HU_ASSERT_GT(len, 0);
    HU_ASSERT_STR_CONTAINS(dir, "firmly");
    HU_ASSERT_STR_CONTAINS(dir, "moderately");
    HU_ASSERT_STR_CONTAINS(dir, "tentatively");
    alloc.free(alloc.ctx, dir, len + 1);
}

void run_opinions_tests(void) {
    HU_TEST_SUITE("opinions");
    HU_RUN_TEST(test_opinions_upsert_get_pizza_best_food);
    HU_RUN_TEST(test_opinions_upsert_supersede_pizza_overrated);
    HU_RUN_TEST(test_opinions_is_core_value_family);
    HU_RUN_TEST(test_evolved_opinion_directive_firmness_mapping);
}

#else

void run_opinions_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_SQLITE */
