/* Tests for the local-voice doctor check (AC-3). Drives the pure test seam
 * hu_doctor_check_local_voice_run_for_test with explicit facts so every verdict
 * branch is covered deterministically in one binary. */

#include "human/config_types.h" /* HU_MLX_LOCAL_ROUTING_* */
#include "human/doctor/check_local_voice.h"
#include "test_framework.h"

#include <string.h>

static void local_voice_routing_off_is_na(void) {
    hu_doctor_check_result_t r =
        hu_doctor_check_local_voice_run_for_test(HU_MLX_LOCAL_ROUTING_OFF, true, true, true, true);
    HU_ASSERT_EQ((int)r.verdict, (int)HU_DOCTOR_NA);
}

static void local_voice_unconfigured_is_na(void) {
    /* AUTO default but nothing local configured = cloud-only user, not a failure. */
    hu_doctor_check_result_t r = hu_doctor_check_local_voice_run_for_test(
        HU_MLX_LOCAL_ROUTING_AUTO, false, false, false, true);
    HU_ASSERT_EQ((int)r.verdict, (int)HU_DOCTOR_NA);
}

static void local_voice_no_curl_fails(void) {
    hu_doctor_check_result_t r = hu_doctor_check_local_voice_run_for_test(
        HU_MLX_LOCAL_ROUTING_AUTO, true, true, true, /*curl_built=*/false);
    HU_ASSERT_EQ((int)r.verdict, (int)HU_DOCTOR_FAIL);
    HU_ASSERT_NOT_NULL(r.detail_json);
}

static void local_voice_no_url_fails(void) {
    hu_doctor_check_result_t r = hu_doctor_check_local_voice_run_for_test(
        HU_MLX_LOCAL_ROUTING_FORCE, true, /*url_configured=*/false, true, true);
    HU_ASSERT_EQ((int)r.verdict, (int)HU_DOCTOR_FAIL);
}

static void local_voice_missing_adapter_fails(void) {
    hu_doctor_check_result_t r = hu_doctor_check_local_voice_run_for_test(
        HU_MLX_LOCAL_ROUTING_AUTO, true, true, /*adapter_present=*/false, true);
    HU_ASSERT_EQ((int)r.verdict, (int)HU_DOCTOR_FAIL);
}

static void local_voice_all_present_passes(void) {
    hu_doctor_check_result_t r =
        hu_doctor_check_local_voice_run_for_test(HU_MLX_LOCAL_ROUTING_AUTO, true, true, true, true);
    HU_ASSERT_EQ((int)r.verdict, (int)HU_DOCTOR_PASS);
    HU_ASSERT_NOT_NULL(r.detail_json);
}

void run_doctor_local_voice_tests(void) {
    HU_TEST_SUITE("doctor local_voice");
    HU_RUN_TEST(local_voice_routing_off_is_na);
    HU_RUN_TEST(local_voice_unconfigured_is_na);
    HU_RUN_TEST(local_voice_no_curl_fails);
    HU_RUN_TEST(local_voice_no_url_fails);
    HU_RUN_TEST(local_voice_missing_adapter_fails);
    HU_RUN_TEST(local_voice_all_present_passes);
}
