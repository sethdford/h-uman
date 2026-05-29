/* ─────────────────────────────────────────────────────────────────────────
 * test_prosocial_moment.c — pins B2/B4/B5 detection + B0-gated warm-response.
 * Spec: docs/plans/2026-05-29-prosocial-uplift/
 * ───────────────────────────────────────────────────────────────────────── */

#include "human/behavior/prosocial.h" /* assert directives are honest */
#include "human/behavior/prosocial_moment.h"
#include "human/core/allocator.h"
#include "human/persona/warm_response.h"
#include "test_framework.h"
#include <string.h>

static hu_pmoment_t det(const char *s) {
    return hu_pmoment_detect(s, strlen(s));
}

/* ── Detection ──────────────────────────────────────────────────────────── */

static void pmoment_gratitude(void) {
    hu_pmoment_t m = det("thank you so much, that really helped");
    HU_ASSERT_TRUE(m.present);
    HU_ASSERT_EQ((int)m.kind, (int)HU_PMOMENT_GRATITUDE);
}

static void pmoment_affirm(void) {
    hu_pmoment_t m = det("it was hard but I stood up for what I believed");
    HU_ASSERT_TRUE(m.present);
    HU_ASSERT_EQ((int)m.kind, (int)HU_PMOMENT_AFFIRM);
}

static void pmoment_savor(void) {
    hu_pmoment_t m = det("we had a great time at the lake today");
    HU_ASSERT_TRUE(m.present);
    HU_ASSERT_EQ((int)m.kind, (int)HU_PMOMENT_SAVOR);
}

static void pmoment_encourage(void) {
    hu_pmoment_t m = det("I'm working on getting back into running");
    HU_ASSERT_TRUE(m.present);
    HU_ASSERT_EQ((int)m.kind, (int)HU_PMOMENT_ENCOURAGE);
}

/* Gratitude outranks the other kinds when several cues co-occur. */
static void pmoment_precedence_gratitude_first(void) {
    hu_pmoment_t m = det("thanks for the push — I kept going because of it");
    HU_ASSERT_EQ((int)m.kind, (int)HU_PMOMENT_GRATITUDE);
}

/* A real setback must NOT be turned into affirmation/savoring. */
static void pmoment_setback_not_affirmed(void) {
    HU_ASSERT_FALSE(det("I can't do this anymore, I feel worthless").present);
    HU_ASSERT_FALSE(det("everything is falling apart").present);
}

static void pmoment_neutral_none(void) {
    HU_ASSERT_FALSE(det("what's on my calendar tomorrow?").present);
    HU_ASSERT_FALSE(det("").present);
    HU_ASSERT_FALSE(hu_pmoment_detect(NULL, 0).present);
}

/* ── Warm-response builder (B0-gated) ───────────────────────────────────── */

static void warm_response_builds_each_kind_honestly(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_pmoment_kind_t kinds[] = {HU_PMOMENT_ENCOURAGE, HU_PMOMENT_AFFIRM, HU_PMOMENT_SAVOR,
                                 HU_PMOMENT_GRATITUDE};
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        size_t len = 0;
        char *d = hu_warm_response_build_directive(&alloc, kinds[i], HU_BRISK_NONE, &len);
        HU_ASSERT_NOT_NULL(d);
        HU_ASSERT_TRUE(len > 0);
        HU_ASSERT_FALSE(hu_prosocial_text_claims_feeling(d, len)); /* honest by construction */
        alloc.free(alloc.ctx, d, len + 1);
    }
}

/* B0 load-bearing: dependency risk suppresses the warm response. */
static void warm_response_suppressed_on_dependency(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t len = 99;
    char *d = hu_warm_response_build_directive(&alloc, HU_PMOMENT_ENCOURAGE,
                                               HU_BRISK_DEPENDENCY_PATTERN, &len);
    HU_ASSERT_NULL(d);
    HU_ASSERT_EQ(len, 0u);
}

static void warm_response_none_kind_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_NULL(hu_warm_response_build_directive(&alloc, HU_PMOMENT_NONE, HU_BRISK_NONE, NULL));
}

void run_prosocial_moment_tests(void);
void run_prosocial_moment_tests(void) {
    HU_TEST_SUITE("prosocial_moment");
    HU_RUN_TEST(pmoment_gratitude);
    HU_RUN_TEST(pmoment_affirm);
    HU_RUN_TEST(pmoment_savor);
    HU_RUN_TEST(pmoment_encourage);
    HU_RUN_TEST(pmoment_precedence_gratitude_first);
    HU_RUN_TEST(pmoment_setback_not_affirmed);
    HU_RUN_TEST(pmoment_neutral_none);
    HU_RUN_TEST(warm_response_builds_each_kind_honestly);
    HU_RUN_TEST(warm_response_suppressed_on_dependency);
    HU_RUN_TEST(warm_response_none_kind_null);
}
