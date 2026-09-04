/* Reactive reply budget — sliding one-hour window, per contact + global.
 *
 * Incident 2026-09-01: a stale-cursor replay sent 17 messages to one contact
 * in an hour and nothing on the reactive path counted them. The budget is a
 * runaway brake, not a conversation shaper: deny → the daemon stays silent. */
#include "human/daemon/send_budget.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>

#define T0 1788310000LL

static void test_budget_allows_under_limit(void) {
    hu_send_budget_t b;
    hu_send_budget_init(&b, 3, 10);
    hu_send_budget_reason_t why = HU_SEND_BUDGET_OK;
    HU_ASSERT_TRUE(hu_send_budget_allows(&b, "+15551110001", 12, T0, &why));
    HU_ASSERT_EQ((int)why, (int)HU_SEND_BUDGET_OK);
}

static void test_budget_denies_at_per_contact_limit(void) {
    hu_send_budget_t b;
    hu_send_budget_init(&b, 3, 10);
    for (int i = 0; i < 3; i++)
        hu_send_budget_record(&b, "+15551110001", 12, T0 + i);
    hu_send_budget_reason_t why = HU_SEND_BUDGET_OK;
    HU_ASSERT_FALSE(hu_send_budget_allows(&b, "+15551110001", 12, T0 + 10, &why));
    HU_ASSERT_EQ((int)why, (int)HU_SEND_BUDGET_CONTACT_EXHAUSTED);
    /* A different contact is unaffected by A's exhaustion. */
    HU_ASSERT_TRUE(hu_send_budget_allows(&b, "+15551110002", 12, T0 + 10, &why));
}

static void test_budget_window_slides_after_one_hour(void) {
    hu_send_budget_t b;
    hu_send_budget_init(&b, 2, 10);
    hu_send_budget_record(&b, "+15551110001", 12, T0);
    hu_send_budget_record(&b, "+15551110001", 12, T0 + 1);
    HU_ASSERT_FALSE(hu_send_budget_allows(&b, "+15551110001", 12, T0 + 3599, NULL));
    /* First send falls out of the window at T0+3600 → one slot free again. */
    HU_ASSERT_TRUE(hu_send_budget_allows(&b, "+15551110001", 12, T0 + 3601, NULL));
}

static void test_budget_denies_at_global_limit(void) {
    hu_send_budget_t b;
    hu_send_budget_init(&b, 100, 4);
    char handle[16];
    for (int i = 0; i < 4; i++) {
        snprintf(handle, sizeof(handle), "+1555000%04d", i);
        hu_send_budget_record(&b, handle, strlen(handle), T0 + i);
    }
    hu_send_budget_reason_t why = HU_SEND_BUDGET_OK;
    /* A brand-new contact is still denied: the global scope is exhausted. */
    HU_ASSERT_FALSE(hu_send_budget_allows(&b, "+15559999999", 12, T0 + 10, &why));
    HU_ASSERT_EQ((int)why, (int)HU_SEND_BUDGET_GLOBAL_EXHAUSTED);
}

static void test_budget_zero_disables_scope(void) {
    hu_send_budget_t b;
    hu_send_budget_init(&b, 0, 0);
    for (int i = 0; i < 50; i++)
        hu_send_budget_record(&b, "+15551110001", 12, T0 + i);
    HU_ASSERT_TRUE(hu_send_budget_allows(&b, "+15551110001", 12, T0 + 60, NULL));
}

static void test_budget_per_contact_zero_but_global_set(void) {
    hu_send_budget_t b;
    hu_send_budget_init(&b, 0, 2);
    hu_send_budget_record(&b, "+15551110001", 12, T0);
    hu_send_budget_record(&b, "+15551110001", 12, T0 + 1);
    hu_send_budget_reason_t why = HU_SEND_BUDGET_OK;
    HU_ASSERT_FALSE(hu_send_budget_allows(&b, "+15551110001", 12, T0 + 2, &why));
    HU_ASSERT_EQ((int)why, (int)HU_SEND_BUDGET_GLOBAL_EXHAUSTED);
}

static void test_budget_null_contact_only_global_applies(void) {
    hu_send_budget_t b;
    hu_send_budget_init(&b, 1, 10);
    hu_send_budget_record(&b, NULL, 0, T0);
    hu_send_budget_record(&b, NULL, 0, T0 + 1);
    /* No contact key → per-contact scope cannot apply; global (10) still has room. */
    HU_ASSERT_TRUE(hu_send_budget_allows(&b, NULL, 0, T0 + 2, NULL));
    HU_ASSERT_TRUE(hu_send_budget_allows(&b, "", 0, T0 + 2, NULL));
}

static void test_budget_incident_shape_is_cut(void) {
    /* 2026-09-01: 9 replies to one contact in an hour with defaults 10/30
     * per hour would pass; the SAME replay to 6 contacts (~25 replies) must
     * hit the global cap. */
    hu_send_budget_t b;
    hu_send_budget_init(&b, HU_SEND_BUDGET_DEFAULT_PER_CONTACT, HU_SEND_BUDGET_DEFAULT_GLOBAL);
    const char *contacts[] = {"+18562546742", "+14848158444", "+12393005206",
                              "+18018285260", "e@icloud.com", "+13857220896"};
    int sent = 0;
    for (int round = 0; round < 9; round++) {
        for (int c = 0; c < 6; c++) {
            const char *h = contacts[c];
            if (hu_send_budget_allows(&b, h, strlen(h), T0 + sent, NULL)) {
                hu_send_budget_record(&b, h, strlen(h), T0 + sent);
                sent++;
            }
        }
    }
    HU_ASSERT_EQ(sent, (int)HU_SEND_BUDGET_DEFAULT_GLOBAL);
}

static void test_budget_eviction_beyond_capacity_keeps_working(void) {
    hu_send_budget_t b;
    hu_send_budget_init(&b, 2, 0);
    char handle[24];
    for (int i = 0; i < HU_SEND_BUDGET_MAX_CONTACTS + 20; i++) {
        snprintf(handle, sizeof(handle), "+1555%07d", i);
        hu_send_budget_record(&b, handle, strlen(handle), T0 + i);
        hu_send_budget_record(&b, handle, strlen(handle), T0 + i);
    }
    /* The most recent contact is tracked and exhausted... */
    HU_ASSERT_FALSE(hu_send_budget_allows(&b, handle, strlen(handle), T0 + 1000, NULL));
    /* ...and a fresh contact is still allowed (a slot was evicted, no crash). */
    HU_ASSERT_TRUE(hu_send_budget_allows(&b, "+19990000000", 12, T0 + 1000, NULL));
}

static void test_budget_records_beyond_per_contact_ring_still_deny(void) {
    /* More records than the ring holds must not wrap into "allowed". */
    hu_send_budget_t b;
    hu_send_budget_init(&b, 5, 0);
    for (int i = 0; i < HU_SEND_BUDGET_MAX_PER_CONTACT + 5; i++)
        hu_send_budget_record(&b, "+15551110001", 12, T0 + i);
    HU_ASSERT_FALSE(hu_send_budget_allows(&b, "+15551110001", 12, T0 + 100, NULL));
}

/* ── module singleton (what daemon.c calls) ─────────────────────────── */

static void test_singleton_configure_then_deny(void) {
    hu_send_budget_reset();
    hu_send_budget_configure(2, 0);
    hu_send_budget_record_send("+15551110001", 12, T0);
    hu_send_budget_record_send("+15551110001", 12, T0 + 1);
    hu_send_budget_reason_t why = HU_SEND_BUDGET_OK;
    uint32_t used = 0, cap = 0;
    HU_ASSERT_FALSE(hu_send_budget_check("+15551110001", 12, T0 + 2, &why, &used, &cap));
    HU_ASSERT_EQ((int)why, (int)HU_SEND_BUDGET_CONTACT_EXHAUSTED);
    HU_ASSERT_EQ(used, 2u);
    HU_ASSERT_EQ(cap, 2u);
    hu_send_budget_reset();
}

static void test_singleton_reset_clears_history(void) {
    hu_send_budget_reset();
    hu_send_budget_configure(1, 0);
    hu_send_budget_record_send("+15551110001", 12, T0);
    HU_ASSERT_FALSE(hu_send_budget_check("+15551110001", 12, T0 + 1, NULL, NULL, NULL));
    hu_send_budget_reset();
    hu_send_budget_configure(1, 0);
    HU_ASSERT_TRUE(hu_send_budget_check("+15551110001", 12, T0 + 1, NULL, NULL, NULL));
    hu_send_budget_reset();
}

void run_send_budget_tests(void) {
    HU_TEST_SUITE("Reactive Send Budget");
    HU_RUN_TEST(test_budget_allows_under_limit);
    HU_RUN_TEST(test_budget_denies_at_per_contact_limit);
    HU_RUN_TEST(test_budget_window_slides_after_one_hour);
    HU_RUN_TEST(test_budget_denies_at_global_limit);
    HU_RUN_TEST(test_budget_zero_disables_scope);
    HU_RUN_TEST(test_budget_per_contact_zero_but_global_set);
    HU_RUN_TEST(test_budget_null_contact_only_global_applies);
    HU_RUN_TEST(test_budget_incident_shape_is_cut);
    HU_RUN_TEST(test_budget_eviction_beyond_capacity_keeps_working);
    HU_RUN_TEST(test_budget_records_beyond_per_contact_ring_still_deny);
    HU_RUN_TEST(test_singleton_configure_then_deny);
    HU_RUN_TEST(test_singleton_reset_clears_history);
}
