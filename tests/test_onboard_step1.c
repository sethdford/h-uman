/* tests/test_onboard_step1.c
 *
 * Sprint 51 US-C2.2 — Welcome step tests.
 *
 * Verifies:
 *  - The vtable factory returns a non-NULL step with name="welcome".
 *  - Test injection: when user_data points to a result, run() returns it
 *    without touching stdin (so the test is deterministic in CI).
 *  - The default copy path resolves to the canonical docs/copy file.
 *  - The copy path setter accepts NULL (resets to default).
 *  - The copy file (docs/copy/onboarding-step1.md) contains the 4
 *    privacy bullets verbatim — pins the regression contract that a
 *    copy-file edit forces the test author to think about it.
 */

#include "human/onboard/state.h"
#include "human/onboard/step.h"
#include "human/onboard/step_welcome.h"
#include "test_framework.h"

#include <stdio.h>
#include <string.h>

static void test_welcome_create_returns_valid_vtable(void) {
    hu_onboard_step_t *step = hu_onboard_step_welcome_create();
    HU_ASSERT_NOT_NULL(step);
    HU_ASSERT_NOT_NULL(step->run);
    HU_ASSERT_NOT_NULL(step->name);
    HU_ASSERT_STR_EQ(step->name, "welcome");
}

static void test_welcome_injected_next_returns_next(void) {
    hu_onboard_step_t *step = hu_onboard_step_welcome_create();
    hu_onboard_step_result_t injected = HU_ONBOARD_NEXT;
    step->user_data = &injected;

    hu_onboard_state_t state;
    hu_onboard_state_init(&state);

    HU_ASSERT_EQ((int)step->run(step, &state), (int)HU_ONBOARD_NEXT);

    /* Reset to avoid bleeding into other tests (the vtable is static). */
    step->user_data = NULL;
}

static void test_welcome_injected_quit_returns_quit(void) {
    hu_onboard_step_t *step = hu_onboard_step_welcome_create();
    hu_onboard_step_result_t injected = HU_ONBOARD_QUIT;
    step->user_data = &injected;

    hu_onboard_state_t state;
    hu_onboard_state_init(&state);

    HU_ASSERT_EQ((int)step->run(step, &state), (int)HU_ONBOARD_QUIT);

    step->user_data = NULL;
}

static void test_welcome_copy_path_default(void) {
    /* Reset to default. */
    hu_onboard_step_welcome_set_copy_path(NULL);
    const char *p = hu_onboard_step_welcome_copy_path();
    HU_ASSERT_NOT_NULL(p);
    HU_ASSERT_TRUE(strstr(p, "onboarding-step1.md") != NULL);
}

static void test_welcome_copy_path_setter_accepts_custom(void) {
    hu_onboard_step_welcome_set_copy_path("/tmp/custom-welcome-copy.md");
    HU_ASSERT_STR_EQ(hu_onboard_step_welcome_copy_path(), "/tmp/custom-welcome-copy.md");
    /* Reset. */
    hu_onboard_step_welcome_set_copy_path(NULL);
}

static void test_welcome_copy_path_setter_null_resets_to_default(void) {
    hu_onboard_step_welcome_set_copy_path("/tmp/some/other/path.md");
    hu_onboard_step_welcome_set_copy_path(NULL);
    const char *p = hu_onboard_step_welcome_copy_path();
    HU_ASSERT_TRUE(strstr(p, "onboarding-step1.md") != NULL);
}

/* Regression-pin the copy file's privacy bullets. If a future PR edits
 * onboarding-step1.md, this test forces the author to update it here
 * too — so they consciously verify the privacy promise hasn't drifted. */
static void test_welcome_copy_file_contains_4_privacy_bullets(void) {
    FILE *f = fopen("docs/copy/onboarding-step1.md", "r");
    if (!f) {
        /* Test running from build dir or fixture path differs. Try
         * the relative-up path. */
        f = fopen("../docs/copy/onboarding-step1.md", "r");
    }
    HU_ASSERT_NOT_NULL(f);

    char buf[16384];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    HU_ASSERT_TRUE(n > 0);

    /* Four key privacy phrases (substring match, tolerant of phrasing
     * tweaks but pins the conceptual coverage). */
    HU_ASSERT_TRUE(strstr(buf, "stays on this device") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "LoRA training") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "~/.human/") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "Cloud calls") != NULL);
}

void run_onboard_step1_tests(void) {
    HU_TEST_SUITE("onboard_step1");
    HU_RUN_TEST(test_welcome_create_returns_valid_vtable);
    HU_RUN_TEST(test_welcome_injected_next_returns_next);
    HU_RUN_TEST(test_welcome_injected_quit_returns_quit);
    HU_RUN_TEST(test_welcome_copy_path_default);
    HU_RUN_TEST(test_welcome_copy_path_setter_accepts_custom);
    HU_RUN_TEST(test_welcome_copy_path_setter_null_resets_to_default);
    HU_RUN_TEST(test_welcome_copy_file_contains_4_privacy_bullets);
}
