/* contact_insights repo + the memory loader's HU_INSIGHT_STREAM block.
 * Item 3 of docs/plans/2026-09-06-better-than-human. */

#include "test_framework.h"

#ifdef HU_ENABLE_SQLITE

#include "human/agent/memory_loader.h"
#include "human/core/allocator.h"
#include "human/core/gate_mode.h"
#include "human/memory/contact_insights_repo.h"
#include "human/memory/engines.h"

#include <string.h>

#define T_2024 1704067200000LL /* 2024-01-01 */
#define T_2025 1735689600000LL /* 2025-01-01 */
#define T_2026 1767225600000LL /* 2026-01-01 */

static const char k_contact[] = "+15550001111";

static void seed_three(hu_memory_t *mem) {
    HU_ASSERT_EQ(hu_contact_insights_add(mem, k_contact, strlen(k_contact), "fact",
                                         "started at Initech in january", 0.8, T_2026, "test",
                                         NULL),
                 HU_OK);
    HU_ASSERT_EQ(hu_contact_insights_add(mem, k_contact, strlen(k_contact), "thread",
                                         "still hunting for a place near the water", 0.7, T_2025,
                                         "test", NULL),
                 HU_OK);
    HU_ASSERT_EQ(hu_contact_insights_add(mem, k_contact, strlen(k_contact), "inside_ref",
                                         "the biscuit sandwich incident", 0.9, T_2024, "test",
                                         NULL),
                 HU_OK);
}

static void render_orders_newest_first_with_month_and_caps(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&a, ":memory:");
    seed_three(&mem);

    char *out = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(
        hu_contact_insights_render(&mem, &a, k_contact, strlen(k_contact), 8, 900, 0.5, &out, &len),
        HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_EQ(strlen(out), len);
    const char *p26 = strstr(out, "Initech");
    const char *p25 = strstr(out, "near the water");
    const char *p24 = strstr(out, "biscuit");
    HU_ASSERT_NOT_NULL(p26);
    HU_ASSERT_NOT_NULL(p25);
    HU_ASSERT_NOT_NULL(p24);
    HU_ASSERT_TRUE(p26 < p25 && p25 < p24); /* newest as_of first */
    HU_ASSERT_NOT_NULL(strstr(out, "(as of Jan 2026)"));
    HU_ASSERT_TRUE(out[0] == '-' && out[len - 1] == '\n');
    a.free(a.ctx, out, len + 1);

    /* max_items caps rows */
    out = NULL;
    HU_ASSERT_EQ(
        hu_contact_insights_render(&mem, &a, k_contact, strlen(k_contact), 1, 900, 0.5, &out, &len),
        HU_OK);
    HU_ASSERT_NOT_NULL(strstr(out, "Initech"));
    HU_ASSERT_NULL(strstr(out, "biscuit"));
    a.free(a.ctx, out, len + 1);

    /* max_bytes drops a whole line, never cuts mid-line */
    out = NULL;
    HU_ASSERT_EQ(
        hu_contact_insights_render(&mem, &a, k_contact, strlen(k_contact), 8, 60, 0.5, &out, &len),
        HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(len <= 60);
    HU_ASSERT_TRUE(out[len - 1] == '\n');
    a.free(a.ctx, out, len + 1);

    /* another contact sees nothing */
    out = (char *)0x1;
    HU_ASSERT_EQ(hu_contact_insights_render(&mem, &a, "+15559999999", 12, 8, 900, 0.5, &out, &len),
                 HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ(len, (size_t)0);

    mem.vtable->deinit(mem.ctx);
}

static void retired_and_low_confidence_rows_are_not_rendered(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&a, ":memory:");
    int64_t id = 0;
    HU_ASSERT_EQ(hu_contact_insights_add(&mem, k_contact, strlen(k_contact), "fact", "old job", 0.9,
                                         T_2025, "test", &id),
                 HU_OK);
    HU_ASSERT_TRUE(id > 0);
    HU_ASSERT_EQ(hu_contact_insights_add(&mem, k_contact, strlen(k_contact), "guess",
                                         "maybe likes jazz", 0.3, T_2026, "test", NULL),
                 HU_OK);
    HU_ASSERT_EQ(hu_contact_insights_retire(&mem, id, T_2026), HU_OK);
    char *out = (char *)0x1;
    size_t len = 99;
    HU_ASSERT_EQ(
        hu_contact_insights_render(&mem, &a, k_contact, strlen(k_contact), 8, 900, 0.5, &out, &len),
        HU_OK);
    HU_ASSERT_NULL(out); /* retired + below-threshold = nothing to say */
    HU_ASSERT_EQ(len, (size_t)0);
    mem.vtable->deinit(mem.ctx);
}

static void add_is_idempotent_for_the_same_note(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&a, ":memory:");
    for (int i = 0; i < 3; i++)
        HU_ASSERT_EQ(hu_contact_insights_add(&mem, k_contact, strlen(k_contact), "fact",
                                             "runs on saturdays", 0.8, T_2026, "test", NULL),
                     HU_OK);
    char *out = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(
        hu_contact_insights_render(&mem, &a, k_contact, strlen(k_contact), 8, 900, 0.5, &out, &len),
        HU_OK);
    HU_ASSERT_NOT_NULL(out);
    size_t n = 0;
    for (const char *p = out; (p = strstr(p, "saturdays")) != NULL; p++)
        n++;
    HU_ASSERT_EQ(n, (size_t)1);
    a.free(a.ctx, out, len + 1);
    mem.vtable->deinit(mem.ctx);
}

static void loader_block_follows_the_gate(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&a, ":memory:");
    seed_three(&mem);
    hu_memory_loader_t loader;
    HU_ASSERT_EQ(hu_memory_loader_init(&loader, &a, &mem, NULL, 8, 4096), HU_OK);

    static const int modes[] = {HU_GATE_OFF, HU_GATE_SHADOW, HU_GATE_LIVE};
    for (size_t i = 0; i < 3; i++) {
        hu_memory_loader_set_insight_mode_for_test(modes[i]);
        char *ctx = NULL;
        size_t ctx_len = 0;
        hu_error_t err =
            hu_memory_loader_load(&loader, "hey", 3, k_contact, strlen(k_contact), &ctx, &ctx_len);
        HU_ASSERT_EQ(err, HU_OK);
        bool has_block = ctx && strstr(ctx, "What you actually remember about them") != NULL;
        bool has_note = ctx && strstr(ctx, "Initech") != NULL;
        if (modes[i] == HU_GATE_LIVE) {
            HU_ASSERT_TRUE(has_block);
            HU_ASSERT_TRUE(has_note);
            HU_ASSERT_EQ(strlen(ctx), ctx_len);
        } else {
            HU_ASSERT_FALSE(has_block); /* off and shadow leave the prompt untouched */
            HU_ASSERT_FALSE(has_note);
        }
        if (ctx)
            a.free(a.ctx, ctx, ctx_len + 1);
    }
    hu_memory_loader_set_insight_mode_for_test(-1);
    HU_ASSERT_EQ((int)hu_memory_loader_insight_mode(), (int)HU_GATE_OFF); /* env unset → off */
    mem.vtable->deinit(mem.ctx);
}

void run_contact_insights_repo_tests(void) {
    HU_TEST_SUITE("contact insights (insight stream)");
    HU_RUN_TEST(render_orders_newest_first_with_month_and_caps);
    HU_RUN_TEST(retired_and_low_confidence_rows_are_not_rendered);
    HU_RUN_TEST(add_is_idempotent_for_the_same_note);
    HU_RUN_TEST(loader_block_follows_the_gate);
}

#else

void run_contact_insights_repo_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_SQLITE */
