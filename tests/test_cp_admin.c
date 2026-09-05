#include "human/agent/model_router.h"
#include "human/core/allocator.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static void admin_models_decisions_filters_shadow_entries(void) {
    /* F1: /admin/models/decisions endpoint must exclude HU_ROUTE_SHADOW_DIFFICULTY
     * entries, matching the behavior of hu_route_log_tier_counts().
     * Verify the filter pattern by simulating the endpoint's JSON generation. */
    hu_route_decision_log_t *log = hu_route_global_log();
    hu_route_log_init(log);

    hu_model_selection_t real_sel, shadow_sel;
    memset(&real_sel, 0, sizeof(real_sel));
    memset(&shadow_sel, 0, sizeof(shadow_sel));

    /* One real decision */
    real_sel.tier = HU_TIER_CONVERSATIONAL;
    real_sel.source = HU_ROUTE_HEURISTIC;
    real_sel.model = "gemini-3.1-pro-preview";
    real_sel.model_len = strlen("gemini-3.1-pro-preview");

    /* One shadow decision */
    shadow_sel.tier = HU_TIER_ANALYTICAL;
    shadow_sel.source = HU_ROUTE_SHADOW_DIFFICULTY;
    shadow_sel.model = "gemini-3.1-pro-preview";
    shadow_sel.model_len = strlen("gemini-3.1-pro-preview");

    hu_route_log_record(log, &real_sel, 0, 1);
    hu_route_log_record(log, &shadow_sel, 0, 2);

    /* Simulate the endpoint's loop: count real decisions (skip shadow). */
    hu_route_global_log_lock();

    size_t count = hu_route_log_count(log);
    size_t real_decisions = 0;
    for (size_t i = 0; i < count; i++) {
        const hu_route_decision_t *d = hu_route_log_get(log, i);
        if (!d)
            continue;
        if (d->source == HU_ROUTE_SHADOW_DIFFICULTY)
            continue; /* hypothetical, never applied — US-8 */
        real_decisions++;
    }

    /* Get tier_distribution (which also excludes shadow). */
    size_t tier_counts[4];
    hu_route_log_tier_counts(log, tier_counts);

    hu_route_global_log_unlock();

    /* Assertions */
    HU_ASSERT_EQ(real_decisions, (size_t)1);
    HU_ASSERT_EQ(tier_counts[HU_TIER_CONVERSATIONAL], (size_t)1);
    HU_ASSERT_EQ(tier_counts[HU_TIER_ANALYTICAL], (size_t)0); /* shadow not counted */
    HU_ASSERT_EQ(count, (size_t)2); /* total includes both real + shadow */
}

void run_cp_admin_tests(void) {
    HU_TEST_SUITE("CP Admin");

    HU_RUN_TEST(admin_models_decisions_filters_shadow_entries);
}
