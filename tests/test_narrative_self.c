/* test_narrative_self.c — pins the narrative-self identity model
 * (src/persona/narrative_self.c). Persistent identity statement + bounded
 * themes/origin/growth arcs, rendered into prompt context. Pure + alloc-based;
 * all allocations freed (ASan). */
#include "human/core/allocator.h"
#include "human/persona/narrative_self.h"
#include "test_framework.h"

#include <string.h>

/* init zeroes the counts and leaves an empty-but-valid struct. */
static void narrative_init_is_empty(void) {
    hu_narrative_self_t s;
    hu_narrative_self_init(&s);
    HU_ASSERT_EQ(s.theme_count, 0u);
    HU_ASSERT_EQ(s.origin_count, 0u);
    HU_ASSERT_EQ(s.growth_count, 0u);
    HU_ASSERT_TRUE(s.identity_statement == NULL);
}

/* identity + themes + growth arcs all surface in the built context. */
static void narrative_build_context_includes_set_fields(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_narrative_self_t s;
    hu_narrative_self_init(&s);

    const char *ident = "someone who shows up consistently";
    const char *theme = "steadiness under pressure";
    const char *arc = "learning to rest without guilt";
    HU_ASSERT_EQ(hu_narrative_self_set_identity(&alloc, &s, ident, strlen(ident)), HU_OK);
    HU_ASSERT_EQ(hu_narrative_self_add_theme(&alloc, &s, theme, strlen(theme)), HU_OK);
    HU_ASSERT_EQ(hu_narrative_self_add_growth_arc(&alloc, &s, arc, strlen(arc)), HU_OK);
    HU_ASSERT_EQ(s.theme_count, 1u);
    HU_ASSERT_EQ(s.growth_count, 1u);

    char *ctx = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_narrative_self_build_context(&alloc, &s, &ctx, &len), HU_OK);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(strstr(ctx, "NARRATIVE SELF") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, ident) != NULL);
    HU_ASSERT_TRUE(strstr(ctx, theme) != NULL);
    HU_ASSERT_TRUE(strstr(ctx, arc) != NULL);

    alloc.free(alloc.ctx, ctx, len + 1);
    hu_narrative_self_deinit(&alloc, &s);
}

/* themes are bounded: the (MAX+1)th add is rejected, not overflowed. */
static void narrative_themes_are_bounded(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_narrative_self_t s;
    hu_narrative_self_init(&s);

    for (size_t i = 0; i < HU_NARRATIVE_MAX_THEMES; i++)
        HU_ASSERT_EQ(hu_narrative_self_add_theme(&alloc, &s, "t", 1), HU_OK);
    HU_ASSERT_EQ(s.theme_count, (size_t)HU_NARRATIVE_MAX_THEMES);
    /* one past the cap is refused without corrupting the count */
    HU_ASSERT_EQ(hu_narrative_self_add_theme(&alloc, &s, "overflow", 8), HU_ERR_LIMIT_REACHED);
    HU_ASSERT_EQ(s.theme_count, (size_t)HU_NARRATIVE_MAX_THEMES);

    hu_narrative_self_deinit(&alloc, &s);
}

/* set_preoccupation replaces (and frees) the prior value — no leak on reset. */
static void narrative_preoccupation_replaces(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_narrative_self_t s;
    hu_narrative_self_init(&s);
    HU_ASSERT_EQ(hu_narrative_self_set_preoccupation(&alloc, &s, "first", 5), HU_OK);
    HU_ASSERT_EQ(hu_narrative_self_set_preoccupation(&alloc, &s, "second", 6), HU_OK);
    HU_ASSERT_TRUE(s.current_preoccupation != NULL);
    HU_ASSERT_STR_EQ(s.current_preoccupation, "second");
    hu_narrative_self_deinit(&alloc, &s);
}

void run_narrative_self_tests(void);
void run_narrative_self_tests(void) {
    HU_TEST_SUITE("narrative_self");
    HU_RUN_TEST(narrative_init_is_empty);
    HU_RUN_TEST(narrative_build_context_includes_set_fields);
    HU_RUN_TEST(narrative_themes_are_bounded);
    HU_RUN_TEST(narrative_preoccupation_replaces);
}
