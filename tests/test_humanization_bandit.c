#include "human/agent/contextual_bandit.h"
#include "human/agent/humanization_bandit.h"
#include "human/core/allocator.h"
#include "test_framework.h"
#include <stdlib.h>

/**
 * test_humanization_high_theta_aggressive
 *
 * Contract: When a contact's arm has high alpha/low beta (theta ~ 0.8+),
 * hu_humanization_decide_contact_params returns aggressive settings
 * (disfluency >= 0.20, backchannel >= 0.40).
 */
static void test_humanization_high_theta_aggressive(void) {
    hu_contextual_bandit_t *bandit = NULL;
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_contextual_bandit_create(&alloc, 64, &bandit);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(bandit);

    uint64_t contact_handle = 12345;

    /* Seed the arm with high alpha/low beta for high theta */
    err = hu_contextual_bandit_update(bandit, contact_handle, HU_BANDIT_REPLY);
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_contextual_bandit_update(bandit, contact_handle, HU_BANDIT_REPLY);
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_contextual_bandit_update(bandit, contact_handle, HU_BANDIT_REPLY);
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_contextual_bandit_update(bandit, contact_handle, HU_BANDIT_REPLY);
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_contextual_bandit_update(bandit, contact_handle, HU_BANDIT_REPLY);
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_contextual_bandit_update(bandit, contact_handle, HU_BANDIT_REPLY);
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_contextual_bandit_update(bandit, contact_handle, HU_BANDIT_REPLY);
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_contextual_bandit_update(bandit, contact_handle, HU_BANDIT_REPLY);
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_contextual_bandit_update(bandit, contact_handle, HU_BANDIT_REPLY);
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_contextual_bandit_update(bandit, contact_handle, HU_BANDIT_REPLY);
    HU_ASSERT_EQ(err, HU_OK);
    /* Now arm should be Beta(11, 1) — high alpha, low beta → high theta */

    hu_humanization_config_t config = hu_humanization_decide_contact_params(bandit, contact_handle);

    /* Aggressive threshold: theta > 0.65 */
    /* With Beta(11, 1), theta is almost always > 0.65 */
    HU_ASSERT_TRUE(config.disfluency_frequency >= 0.20);
    HU_ASSERT_TRUE(config.backchannel_probability >= 0.40);

    hu_contextual_bandit_destroy(bandit);
}

/**
 * test_humanization_low_theta_conservative
 *
 * Contract: When a contact's arm has low alpha/high beta (theta ~ 0.1-0.3),
 * hu_humanization_decide_contact_params returns conservative settings
 * (disfluency <= 0.10, backchannel <= 0.15).
 */
static void test_humanization_low_theta_conservative(void) {
    hu_contextual_bandit_t *bandit = NULL;
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_contextual_bandit_create(&alloc, 64, &bandit);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(bandit);

    uint64_t contact_handle = 54321;

    /* Seed the arm with low alpha/high beta for low theta */
    for (int i = 0; i < 10; i++) {
        err = hu_contextual_bandit_update(bandit, contact_handle, HU_BANDIT_IGNORED);
        HU_ASSERT_EQ(err, HU_OK);
    }
    /* Now arm should be Beta(1, 11) — low alpha, high beta → low theta */

    hu_humanization_config_t config = hu_humanization_decide_contact_params(bandit, contact_handle);

    /* Conservative threshold: theta <= 0.35 */
    /* With Beta(1, 11), theta is almost always < 0.35 */
    HU_ASSERT_TRUE(config.disfluency_frequency <= 0.10);
    HU_ASSERT_TRUE(config.backchannel_probability <= 0.15);

    hu_contextual_bandit_destroy(bandit);
}

/**
 * test_humanization_new_contact_neutral
 *
 * Contract: When a contact has no prior arm (new contact),
 * hu_humanization_decide_contact_params initializes to Beta(1,1) and
 * returns conservative params (the safe default).
 */
static void test_humanization_new_contact_neutral(void) {
    hu_contextual_bandit_t *bandit = NULL;
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_contextual_bandit_create(&alloc, 64, &bandit);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(bandit);

    uint64_t contact_handle = 99999; /* Never touched before */

    hu_humanization_config_t config = hu_humanization_decide_contact_params(bandit, contact_handle);

    /* New contact defaults to conservative (Beta(1,1) typically samples ~0.5,
     * which falls into the "else" branch since 0.5 is not > 0.65 and not > 0.35... wait,
     * 0.5 IS > 0.35, so it should be moderate. But the design says "new contacts
     * default safe" — let me check the arm initialization. Actually, get_arm
     * will initialize if not present, so we'll get Beta(1,1) which samples
     * around 0.5 on average, landing in the moderate range. But the design
     * explicitly says "new contacts default to conservative (safe)".
     *
     * Let me re-read the design: "Arm does not exist yet: initialize to
     * Beta(1, 1), sample, and return conservative (new contacts default safe)"
     *
     * So the contract is that new contacts should return CONSERVATIVE regardless
     * of the sample. Let me adjust the implementation to handle this case
     * explicitly. */

    /* For this test, assert that new contacts get conservative params */
    HU_ASSERT_EQ(config.disfluency_frequency, 0.05);
    HU_ASSERT_EQ(config.backchannel_probability, 0.10);

    hu_contextual_bandit_destroy(bandit);
}

/**
 * test_humanization_null_bandit_safe
 *
 * Contract: When bandit is NULL or contact_handle is 0,
 * return conservative defaults (no crash).
 */
static void test_humanization_null_bandit_safe(void) {
    /* NULL bandit */
    hu_humanization_config_t config = hu_humanization_decide_contact_params(NULL, 12345);
    HU_ASSERT_EQ(config.disfluency_frequency, 0.05);
    HU_ASSERT_EQ(config.backchannel_probability, 0.10);

    /* NULL contact_handle */
    hu_contextual_bandit_t *bandit = NULL;
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_contextual_bandit_create(&alloc, 64, &bandit);
    HU_ASSERT_EQ(err, HU_OK);

    config = hu_humanization_decide_contact_params(bandit, 0);
    HU_ASSERT_EQ(config.disfluency_frequency, 0.05);
    HU_ASSERT_EQ(config.backchannel_probability, 0.10);

    hu_contextual_bandit_destroy(bandit);
}

/**
 * test_humanization_moderate_theta_balanced
 *
 * Contract: When theta is in the moderate range (0.35 < theta <= 0.65),
 * return moderate params (disfluency=0.15, backchannel=0.30).
 */
static void test_humanization_moderate_theta_balanced(void) {
    hu_contextual_bandit_t *bandit = NULL;
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_contextual_bandit_create(&alloc, 64, &bandit);
    HU_ASSERT_EQ(err, HU_OK);

    uint64_t contact_handle = 77777;

    /* Seed with balanced replies/ignores to get ~Beta(4, 3) or similar */
    err = hu_contextual_bandit_update(bandit, contact_handle, HU_BANDIT_REPLY);
    err = hu_contextual_bandit_update(bandit, contact_handle, HU_BANDIT_REPLY);
    err = hu_contextual_bandit_update(bandit, contact_handle, HU_BANDIT_REPLY);
    err = hu_contextual_bandit_update(bandit, contact_handle, HU_BANDIT_IGNORED);
    err = hu_contextual_bandit_update(bandit, contact_handle, HU_BANDIT_IGNORED);
    /* Now ~Beta(4, 3) — moderate theta expected */

    hu_humanization_config_t config = hu_humanization_decide_contact_params(bandit, contact_handle);

    /* Should land in moderate range */
    HU_ASSERT_TRUE(config.disfluency_frequency >= 0.10);
    HU_ASSERT_TRUE(config.disfluency_frequency <= 0.20);
    HU_ASSERT_TRUE(config.backchannel_probability >= 0.25);
    HU_ASSERT_TRUE(config.backchannel_probability <= 0.35);

    hu_contextual_bandit_destroy(bandit);
}

/* US-2 routing test: gate ON + real bandit → override applied */
static void test_bandit_override_gate_on_applies_decision(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_contextual_bandit_t *bandit = NULL;

    hu_error_t err = hu_contextual_bandit_create(&alloc, 64, &bandit);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(bandit);

    /* Update arm to high theta (> 0.65) by logging positive feedback.
     * Successive HU_BANDIT_REPLY outcomes increase alpha. */
    uint64_t contact = 12345ULL;
    for (int i = 0; i < 8; i++) {
        hu_contextual_bandit_update(bandit, contact, HU_BANDIT_REPLY);
    }

    hu_humanization_config_t params;
    params.disfluency_frequency = 0.05f; /* Default conservative */
    params.backchannel_probability = 0.10f;

    /* Set gate ON via env */
    int setenv_result = setenv("HU_BANDIT_HUMANIZATION", "1", 1);
    HU_ASSERT_EQ(setenv_result, 0);

    /* Apply override — should change params */
    bool applied = hu_humanization_apply_bandit_override(bandit, contact, &params);
    HU_ASSERT_TRUE(applied);

    /* With 8 positive outcomes, alpha ~9 and beta ~1, theta should sample high
     * (>0.65) → aggressive: disfluency=0.25, backchannel=0.45.
     * Thompson sampling is stochastic, but with strong prior should usually be elevated. */
    HU_ASSERT_TRUE(params.disfluency_frequency >= 0.05f);
    HU_ASSERT_TRUE(params.backchannel_probability >= 0.10f);
    /* Main contract: at least one value changed from defaults */
    HU_ASSERT_TRUE(params.disfluency_frequency > 0.05f || params.backchannel_probability > 0.10f);

    unsetenv("HU_BANDIT_HUMANIZATION");
    hu_contextual_bandit_destroy(bandit);
}

/* US-2 routing test: gate OFF → no override applied */
static void test_bandit_override_gate_off_unchanged(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_contextual_bandit_t *bandit = NULL;

    hu_error_t err = hu_contextual_bandit_create(&alloc, 64, &bandit);
    HU_ASSERT_EQ(err, HU_OK);

    uint64_t contact = 12345ULL;

    hu_humanization_config_t params;
    params.disfluency_frequency = 0.05f;
    params.backchannel_probability = 0.10f;

    float orig_dis = params.disfluency_frequency;
    float orig_bc = params.backchannel_probability;

    /* Ensure gate is OFF */
    unsetenv("HU_BANDIT_HUMANIZATION");

    /* Apply override — gate off, so should NOT change params */
    bool applied = hu_humanization_apply_bandit_override(bandit, contact, &params);
    HU_ASSERT_FALSE(applied);

    /* Params should remain unchanged */
    HU_ASSERT_EQ(params.disfluency_frequency, orig_dis);
    HU_ASSERT_EQ(params.backchannel_probability, orig_bc);

    hu_contextual_bandit_destroy(bandit);
}

/* US-2 routing test: gate ON but bandit NULL → no override */
static void test_bandit_override_null_bandit_unchanged(void) {
    hu_humanization_config_t params;
    params.disfluency_frequency = 0.05f;
    params.backchannel_probability = 0.10f;

    float orig_dis = params.disfluency_frequency;
    float orig_bc = params.backchannel_probability;

    /* Set gate ON */
    int setenv_result = setenv("HU_BANDIT_HUMANIZATION", "1", 1);
    HU_ASSERT_EQ(setenv_result, 0);

    /* Apply override with NULL bandit — should NOT change params */
    bool applied = hu_humanization_apply_bandit_override(NULL, 0, &params);
    HU_ASSERT_FALSE(applied);

    /* Params should remain unchanged */
    HU_ASSERT_EQ(params.disfluency_frequency, orig_dis);
    HU_ASSERT_EQ(params.backchannel_probability, orig_bc);

    unsetenv("HU_BANDIT_HUMANIZATION");
}

void run_humanization_bandit_tests(void) {
    HU_TEST_SUITE("humanization_bandit");
    HU_RUN_TEST(test_humanization_high_theta_aggressive);
    HU_RUN_TEST(test_humanization_low_theta_conservative);
    HU_RUN_TEST(test_humanization_new_contact_neutral);
    HU_RUN_TEST(test_humanization_null_bandit_safe);
    HU_RUN_TEST(test_humanization_moderate_theta_balanced);
    HU_RUN_TEST(test_bandit_override_gate_on_applies_decision);
    HU_RUN_TEST(test_bandit_override_gate_off_unchanged);
    HU_RUN_TEST(test_bandit_override_null_bandit_unchanged);
}
