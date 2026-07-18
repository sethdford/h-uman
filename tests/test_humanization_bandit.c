#include "human/agent/contextual_bandit.h"
#include "human/agent/humanization_bandit.h"
#include "human/core/allocator.h"
#include "test_framework.h"
#include <stdio.h>
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

    /* Activated 2026-05-31 after blind A/B: ENABLED by default. The OFF path is
     * now reached via the EXPLICIT disable value, not by unsetting the env. */
    setenv("HU_BANDIT_HUMANIZATION", "off", 1);

    /* Apply override — gate explicitly off, so should NOT change params */
    bool applied = hu_humanization_apply_bandit_override(bandit, contact, &params);
    HU_ASSERT_FALSE(applied);

    /* Params should remain unchanged */
    HU_ASSERT_EQ(params.disfluency_frequency, orig_dis);
    HU_ASSERT_EQ(params.backchannel_probability, orig_bc);

    /* Default (env unset) is now ON: a real bandit applies the override. */
    unsetenv("HU_BANDIT_HUMANIZATION");
    bool applied_default = hu_humanization_apply_bandit_override(bandit, contact, &params);
    HU_ASSERT_TRUE(applied_default);

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

/* ── persistence (2026-07-18 audit: arm posteriors died on daemon restart,
 * so the bandit was permanently stuck re-exploring). Mirrors the
 * tests/test_somatic.c somatic_save_load_roundtrip family. ── */

static void bandit_save_load_roundtrip(void) {
    const char *path = "/tmp/hu_test_bandit_roundtrip.json";
    remove(path);
    hu_allocator_t alloc = hu_system_allocator();
    hu_contextual_bandit_t *bandit = NULL;
    HU_ASSERT_EQ(hu_contextual_bandit_create(&alloc, 64, &bandit), HU_OK);

    /* A handle above 2^53 pins the string-encoded-handle decision: stored
     * as a JSON number it would be silently corrupted by double rounding,
     * and the daemon would train one arm while reading another. */
    uint64_t big_handle = 18446744073709551614ULL;
    for (int i = 0; i < 5; i++)
        HU_ASSERT_EQ(hu_contextual_bandit_update(bandit, big_handle, HU_BANDIT_REPLY), HU_OK);
    HU_ASSERT_EQ(hu_contextual_bandit_update(bandit, 777ULL, HU_BANDIT_IGNORED), HU_OK);

    HU_ASSERT_EQ(hu_humanization_bandit_save_file(bandit, path), HU_OK);

    hu_contextual_bandit_t *loaded = NULL;
    HU_ASSERT_EQ(hu_contextual_bandit_create(&alloc, 64, &loaded), HU_OK);
    HU_ASSERT_EQ(hu_humanization_bandit_load_file(loaded, path), HU_OK);

    hu_contextual_bandit_arm_t arm;
    HU_ASSERT_EQ(hu_contextual_bandit_get_arm(loaded, big_handle, &arm), HU_OK);
    HU_ASSERT_EQ(arm.contact_handle, big_handle);
    HU_ASSERT_TRUE(arm.alpha > 5.99 && arm.alpha < 6.01); /* 1 + 5 replies */
    HU_ASSERT_TRUE(arm.beta > 0.99 && arm.beta < 1.01);
    HU_ASSERT_EQ(arm.updates, 5ULL);

    HU_ASSERT_EQ(hu_contextual_bandit_get_arm(loaded, 777ULL, &arm), HU_OK);
    HU_ASSERT_TRUE(arm.beta > 1.99 && arm.beta < 2.01); /* 1 + 1 ignored */
    HU_ASSERT_EQ(arm.updates, 1ULL);

    hu_contextual_bandit_destroy(bandit);
    hu_contextual_bandit_destroy(loaded);
    remove(path);
}

static void bandit_load_missing_file_keeps_state(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_contextual_bandit_t *bandit = NULL;
    HU_ASSERT_EQ(hu_contextual_bandit_create(&alloc, 64, &bandit), HU_OK);
    HU_ASSERT_EQ(hu_contextual_bandit_update(bandit, 42ULL, HU_BANDIT_REPLY), HU_OK);

    HU_ASSERT_TRUE(hu_humanization_bandit_load_file(bandit, "/tmp/hu_test_bandit_missing.json") !=
                   HU_OK);

    /* State untouched on miss. */
    HU_ASSERT_EQ(bandit->count, (size_t)1);
    hu_contextual_bandit_arm_t arm;
    HU_ASSERT_EQ(hu_contextual_bandit_get_arm(bandit, 42ULL, &arm), HU_OK);
    HU_ASSERT_TRUE(arm.alpha > 1.99 && arm.alpha < 2.01);
    hu_contextual_bandit_destroy(bandit);
}

static void bandit_load_corrupt_file_keeps_state(void) {
    const char *path = "/tmp/hu_test_bandit_corrupt.json";
    FILE *f = fopen(path, "w");
    HU_ASSERT_NOT_NULL(f);
    fputs("{\"version\": 1, \"arms\": [{oops", f);
    fclose(f);

    hu_allocator_t alloc = hu_system_allocator();
    hu_contextual_bandit_t *bandit = NULL;
    HU_ASSERT_EQ(hu_contextual_bandit_create(&alloc, 64, &bandit), HU_OK);
    HU_ASSERT_EQ(hu_contextual_bandit_update(bandit, 42ULL, HU_BANDIT_REPLY), HU_OK);

    HU_ASSERT_TRUE(hu_humanization_bandit_load_file(bandit, path) != HU_OK);

    HU_ASSERT_EQ(bandit->count, (size_t)1);
    hu_contextual_bandit_arm_t arm;
    HU_ASSERT_EQ(hu_contextual_bandit_get_arm(bandit, 42ULL, &arm), HU_OK);
    HU_ASSERT_TRUE(arm.alpha > 1.99 && arm.alpha < 2.01);
    hu_contextual_bandit_destroy(bandit);
    remove(path);
}

static void bandit_load_clamps_out_of_range(void) {
    /* A hand-edited or corrupted-but-parseable file must not inject values
     * outside the arm's legal range (alpha/beta >= 1.0, updates >= 0) —
     * the gamma sampler misbehaves at alpha/beta <= 0. */
    const char *path = "/tmp/hu_test_bandit_clamp.json";
    FILE *f = fopen(path, "w");
    HU_ASSERT_NOT_NULL(f);
    fputs("{\"version\": 1, \"arms\": ["
          "{\"h\": \"101\", \"alpha\": -5.0, \"beta\": 0.0, \"updates\": -3},"
          "{\"h\": \"102\", \"alpha\": 1e300, \"beta\": 2.5, \"updates\": 7},"
          "{\"h\": \"0\", \"alpha\": 4.0, \"beta\": 4.0, \"updates\": 2}]}",
          f);
    fclose(f);

    hu_allocator_t alloc = hu_system_allocator();
    hu_contextual_bandit_t *bandit = NULL;
    HU_ASSERT_EQ(hu_contextual_bandit_create(&alloc, 64, &bandit), HU_OK);
    HU_ASSERT_EQ(hu_humanization_bandit_load_file(bandit, path), HU_OK);

    /* Handle "0" marks empty slots and must be skipped, not inserted. */
    HU_ASSERT_EQ(bandit->count, (size_t)2);

    hu_contextual_bandit_arm_t arm;
    HU_ASSERT_EQ(hu_contextual_bandit_get_arm(bandit, 101ULL, &arm), HU_OK);
    HU_ASSERT_TRUE(arm.alpha > 0.99 && arm.alpha < 1.01); /* clamped up */
    HU_ASSERT_TRUE(arm.beta > 0.99 && arm.beta < 1.01);
    HU_ASSERT_EQ(arm.updates, 0ULL); /* negative clamped to 0 */

    HU_ASSERT_EQ(hu_contextual_bandit_get_arm(bandit, 102ULL, &arm), HU_OK);
    HU_ASSERT_TRUE(arm.alpha < 1e10); /* capped, not 1e300 */
    HU_ASSERT_TRUE(arm.beta > 2.49 && arm.beta < 2.51);
    HU_ASSERT_EQ(arm.updates, 7ULL);

    hu_contextual_bandit_destroy(bandit);
    remove(path);
}

/* Integration pin (non-vacuous per integration-done-contract): a LOADED
 * posterior must change the next arm choice vs a cold start. Cold bandit →
 * fresh Beta(1,1) arm → conservative tier unconditionally. Loaded strong
 * success posterior Beta(60,1) → theta ≈ 0.98 → aggressive tier. If load
 * didn't reach the decision path, both would be conservative and the
 * strict inequality below would fail. */
static void bandit_loaded_posterior_changes_next_choice(void) {
    const char *path = "/tmp/hu_test_bandit_pin.json";
    uint64_t contact = 9001ULL;

    hu_allocator_t alloc = hu_system_allocator();
    hu_contextual_bandit_t *cold = NULL;
    HU_ASSERT_EQ(hu_contextual_bandit_create(&alloc, 64, &cold), HU_OK);
    hu_humanization_config_t cold_cfg = hu_humanization_decide_contact_params(cold, contact);
    HU_ASSERT_TRUE(cold_cfg.disfluency_frequency < 0.10f); /* conservative */

    FILE *f = fopen(path, "w");
    HU_ASSERT_NOT_NULL(f);
    fputs("{\"version\": 1, \"arms\": "
          "[{\"h\": \"9001\", \"alpha\": 60.0, \"beta\": 1.0, \"updates\": 59}]}",
          f);
    fclose(f);

    hu_contextual_bandit_t *warm = NULL;
    HU_ASSERT_EQ(hu_contextual_bandit_create(&alloc, 64, &warm), HU_OK);
    HU_ASSERT_EQ(hu_humanization_bandit_load_file(warm, path), HU_OK);
    hu_humanization_config_t warm_cfg = hu_humanization_decide_contact_params(warm, contact);

    /* Beta(60,1) samples land far above the 0.65 aggressive threshold with
     * the fixed HU_IS_TEST seed. */
    HU_ASSERT_TRUE(warm_cfg.disfluency_frequency > cold_cfg.disfluency_frequency);
    HU_ASSERT_TRUE(warm_cfg.disfluency_frequency >= 0.20f);

    hu_contextual_bandit_destroy(cold);
    hu_contextual_bandit_destroy(warm);
    remove(path);
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
    HU_RUN_TEST(bandit_save_load_roundtrip);
    HU_RUN_TEST(bandit_load_missing_file_keeps_state);
    HU_RUN_TEST(bandit_load_corrupt_file_keeps_state);
    HU_RUN_TEST(bandit_load_clamps_out_of_range);
    HU_RUN_TEST(bandit_loaded_posterior_changes_next_choice);
}
