/* ─────────────────────────────────────────────────────────────────────────
 * test_celebration.c — pins B1b celebration builder + proves B0 is load-bearing.
 * Spec: docs/plans/2026-05-29-prosocial-uplift/
 * ───────────────────────────────────────────────────────────────────────── */

#include "human/behavior/prosocial.h" /* to assert the output is honest */
#include "human/core/allocator.h"
#include "human/persona/celebration.h"
#include "test_framework.h"
#include <string.h>

static void celebration_builds_for_each_kind(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_win_kind_t kinds[] = {HU_WIN_ACHIEVEMENT, HU_WIN_MILESTONE, HU_WIN_GOOD_NEWS};
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        size_t len = 0;
        char *dir = hu_celebration_build_directive(&alloc, kinds[i], HU_BRISK_NONE, &len);
        HU_ASSERT_NOT_NULL(dir);
        HU_ASSERT_TRUE(len > 0);
        /* Honest by construction: the directive itself must not model a claimed feeling. */
        HU_ASSERT_FALSE(hu_prosocial_text_claims_feeling(dir, len));
        alloc.free(alloc.ctx, dir, len + 1);
    }
}

/* The load-bearing proof: a dependency/attachment risk makes B0 SUPPRESS, so
 * the builder refuses to celebrate. */
static void celebration_suppressed_on_dependency_risk(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_behavior_risk_t risks[] = {HU_BRISK_DEPENDENCY_PATTERN, HU_BRISK_ATTACHMENT_HIGH,
                                  HU_BRISK_VULNERABLE_USER};
    for (size_t i = 0; i < sizeof(risks) / sizeof(risks[0]); i++) {
        size_t len = 99;
        char *dir = hu_celebration_build_directive(&alloc, HU_WIN_ACHIEVEMENT, risks[i], &len);
        HU_ASSERT_NULL(dir);
        HU_ASSERT_EQ(len, 0u);
    }
}

static void celebration_none_kind_is_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t len = 99;
    HU_ASSERT_NULL(hu_celebration_build_directive(&alloc, HU_WIN_NONE, HU_BRISK_NONE, &len));
    HU_ASSERT_EQ(len, 0u);
}

void run_celebration_tests(void);
void run_celebration_tests(void) {
    HU_TEST_SUITE("celebration");
    HU_RUN_TEST(celebration_builds_for_each_kind);
    HU_RUN_TEST(celebration_suppressed_on_dependency_risk);
    HU_RUN_TEST(celebration_none_kind_is_null);
}
