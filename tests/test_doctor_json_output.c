/* tests/test_doctor_json_output.c
 *
 * Sprint 54 US-C3.7 (Phase 1) — Doctor JSON v1 emitter tests.
 *
 * Tests use a memory FILE * via fmemopen() to capture emitted JSON
 * and assert on the bytes directly. No subprocess, no fixtures, no
 * runtime dependencies on cmd_doctor() (which is Phase 2).
 *
 * Test discipline:
 *   - No allow-silent-pass opt-outs.
 *   - Each test asserts a real schema contract.
 *   - Schema v1 is LOCKED — these tests pin the contract.
 */

#include "test_framework.h"

#include "human/doctor.h"
#include "human/doctor/check.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* Capture the emitter's output into a heap buffer. The caller frees. */
static char *emit_to_buf(const hu_doctor_json_entry_t *entries, size_t count, time_t epoch,
                         size_t *out_len) {
    char *buf = (char *)malloc(8192);
    if (!buf)
        return NULL;
    FILE *f = fmemopen(buf, 8192, "w");
    if (!f) {
        free(buf);
        return NULL;
    }
    hu_error_t err = hu_doctor_emit_json_v1(entries, count, epoch, f);
    fflush(f);
    long pos = ftell(f);
    fclose(f);
    if (err != HU_OK) {
        free(buf);
        return NULL;
    }
    if (pos < 0)
        pos = 0;
    if ((size_t)pos < 8192)
        buf[pos] = '\0';
    if (out_len)
        *out_len = (size_t)pos;
    return buf;
}

/* Fixed epoch for deterministic ts: 2026-05-25T14:30:00Z = 1779890000. */
#define HU_TEST_FIXED_EPOCH 1779890000L

/* ── basic shape + null guards ────────────────────────────────────── */

static void test_emit_rejects_null_out(void) {
    hu_doctor_json_entry_t e = {.name = "x", .verdict = HU_DOCTOR_PASS, .reason = ""};
    HU_ASSERT_EQ((int)hu_doctor_emit_json_v1(&e, 1, HU_TEST_FIXED_EPOCH, NULL),
                 (int)HU_ERR_INVALID_ARGUMENT);
}

static void test_emit_rejects_null_entries_with_nonzero_count(void) {
    char buf[256];
    FILE *f = fmemopen(buf, sizeof(buf), "w");
    HU_ASSERT_NOT_NULL(f);
    HU_ASSERT_EQ((int)hu_doctor_emit_json_v1(NULL, 5, HU_TEST_FIXED_EPOCH, f),
                 (int)HU_ERR_INVALID_ARGUMENT);
    fclose(f);
}

static void test_emit_zero_checks_returns_ok_and_empty_array(void) {
    /* Zero checks is structurally valid → emits {checks:[],aggregate:"pass"}. */
    size_t len = 0;
    char *out = emit_to_buf(NULL, 0, HU_TEST_FIXED_EPOCH, &len);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "\"checks\":[]") != NULL);
    HU_ASSERT_TRUE(strstr(out, "\"aggregate\":\"pass\"") != NULL);
    free(out);
}

/* ── schema v1 invariants ─────────────────────────────────────────── */

static void test_emit_version_field_is_one(void) {
    hu_doctor_json_entry_t e = {.name = "install", .verdict = HU_DOCTOR_PASS, .reason = ""};
    char *out = emit_to_buf(&e, 1, HU_TEST_FIXED_EPOCH, NULL);
    HU_ASSERT_NOT_NULL(out);
    /* The literal "version":1 MUST appear and the value MUST be the
     * integer 1 (not a string, not 2). */
    HU_ASSERT_TRUE(strstr(out, "\"version\":1") != NULL);
    /* Defense-in-depth: ensure no rogue "version":2 snuck in via a
     * concurrent schema bump. */
    HU_ASSERT_TRUE(strstr(out, "\"version\":2") == NULL);
    free(out);
}

static void test_emit_timestamp_is_iso8601_utc(void) {
    /* epoch 1779890000 = 2026-05-27T19:13:20Z (fixed deterministic value).
     * This test pins the wire format so a future fmt change breaks the test. */
    hu_doctor_json_entry_t e = {.name = "x", .verdict = HU_DOCTOR_PASS, .reason = ""};
    char *out = emit_to_buf(&e, 1, HU_TEST_FIXED_EPOCH, NULL);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "\"ts\":\"2026-05-27T13:53:20Z\"") != NULL);
    free(out);
}

static void test_emit_includes_check_name_verdict_reason(void) {
    hu_doctor_json_entry_t e = {
        .name = "chatdb_readable", .verdict = HU_DOCTOR_FAIL, .reason = "permission denied"};
    char *out = emit_to_buf(&e, 1, HU_TEST_FIXED_EPOCH, NULL);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "\"name\":\"chatdb_readable\"") != NULL);
    HU_ASSERT_TRUE(strstr(out, "\"verdict\":\"fail\"") != NULL);
    HU_ASSERT_TRUE(strstr(out, "\"reason\":\"permission denied\"") != NULL);
    free(out);
}

static void test_emit_na_verdict_collapses_to_pass(void) {
    /* HU_DOCTOR_NA → "pass" in the wire (NA counts as PASS in aggregate). */
    hu_doctor_json_entry_t e = {
        .name = "x", .verdict = HU_DOCTOR_NA, .reason = "platform-not-applicable"};
    char *out = emit_to_buf(&e, 1, HU_TEST_FIXED_EPOCH, NULL);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "\"verdict\":\"pass\"") != NULL);
    free(out);
}

/* ── aggregate logic ──────────────────────────────────────────────── */

static void test_emit_all_pass_aggregate_pass(void) {
    hu_doctor_json_entry_t entries[] = {
        {.name = "a", .verdict = HU_DOCTOR_PASS, .reason = ""},
        {.name = "b", .verdict = HU_DOCTOR_PASS, .reason = ""},
        {.name = "c", .verdict = HU_DOCTOR_PASS, .reason = ""},
    };
    char *out = emit_to_buf(entries, 3, HU_TEST_FIXED_EPOCH, NULL);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "\"aggregate\":\"pass\"") != NULL);
    free(out);
}

static void test_emit_pass_plus_na_aggregate_pass(void) {
    hu_doctor_json_entry_t entries[] = {
        {.name = "a", .verdict = HU_DOCTOR_PASS, .reason = ""},
        {.name = "b", .verdict = HU_DOCTOR_NA, .reason = "n/a"},
    };
    char *out = emit_to_buf(entries, 2, HU_TEST_FIXED_EPOCH, NULL);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "\"aggregate\":\"pass\"") != NULL);
    free(out);
}

static void test_emit_single_fail_aggregate_fail(void) {
    hu_doctor_json_entry_t entries[] = {
        {.name = "a", .verdict = HU_DOCTOR_PASS, .reason = ""},
        {.name = "b", .verdict = HU_DOCTOR_FAIL, .reason = "broke"},
    };
    char *out = emit_to_buf(entries, 2, HU_TEST_FIXED_EPOCH, NULL);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "\"aggregate\":\"fail\"") != NULL);
    free(out);
}

static void test_emit_unknown_verdict_aggregate_fail(void) {
    /* Defensive: garbage verdict value → aggregate fail. */
    hu_doctor_json_entry_t e = {.name = "a", .verdict = 9999, .reason = ""};
    char *out = emit_to_buf(&e, 1, HU_TEST_FIXED_EPOCH, NULL);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "\"aggregate\":\"fail\"") != NULL);
    free(out);
}

/* ── escaping ─────────────────────────────────────────────────────── */

static void test_emit_escapes_quote_in_reason(void) {
    hu_doctor_json_entry_t e = {
        .name = "x", .verdict = HU_DOCTOR_FAIL, .reason = "got \"unexpected\" token"};
    char *out = emit_to_buf(&e, 1, HU_TEST_FIXED_EPOCH, NULL);
    HU_ASSERT_NOT_NULL(out);
    /* The literal sequence "got \"unexpected\" token" should appear
     * with the inner quotes escaped: got \"unexpected\" token. */
    HU_ASSERT_TRUE(strstr(out, "got \\\"unexpected\\\" token") != NULL);
    free(out);
}

static void test_emit_escapes_backslash_in_reason(void) {
    hu_doctor_json_entry_t e = {
        .name = "x", .verdict = HU_DOCTOR_FAIL, .reason = "C:\\Users\\path"};
    char *out = emit_to_buf(&e, 1, HU_TEST_FIXED_EPOCH, NULL);
    HU_ASSERT_NOT_NULL(out);
    /* Backslashes must be doubled: C:\\Users\\path in source → emitted as
     * the literal byte sequence  C:\\\\Users\\\\path  in JSON. */
    HU_ASSERT_TRUE(strstr(out, "C:\\\\Users\\\\path") != NULL);
    free(out);
}

static void test_emit_handles_null_reason(void) {
    /* NULL reason → empty string in output (not literal "null"). */
    hu_doctor_json_entry_t e = {.name = "x", .verdict = HU_DOCTOR_PASS, .reason = NULL};
    char *out = emit_to_buf(&e, 1, HU_TEST_FIXED_EPOCH, NULL);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "\"reason\":\"\"") != NULL);
    HU_ASSERT_TRUE(strstr(out, "null") == NULL);
    free(out);
}

/* ── detail_json passthrough (additive v1 extension) ──────────────── */

static void test_emit_includes_detail_when_present(void) {
    /* detail_json is a pre-encoded JSON value. The emitter must write it
     * verbatim under "detail" — NOT wrap it in quotes — so consumers can
     * read .detail.k directly instead of having to JSON.parse a string. */
    hu_doctor_json_entry_t e = {.name = "prompt_budget",
                                .verdict = HU_DOCTOR_PASS,
                                .reason = "enabled",
                                .detail_json = "{\"enabled\":true,\"observation_count\":null}"};
    char *out = emit_to_buf(&e, 1, HU_TEST_FIXED_EPOCH, NULL);
    HU_ASSERT_NOT_NULL(out);
    /* Raw object literal lands inline under "detail": */
    HU_ASSERT_TRUE(strstr(out, "\"detail\":{\"enabled\":true,\"observation_count\":null}") != NULL);
    /* Defense-in-depth: detail must NOT be emitted as a string-wrapped JSON
     * (that would force consumers to JSON.parse twice). */
    HU_ASSERT_TRUE(strstr(out, "\"detail\":\"{") == NULL);
    free(out);
}

static void test_emit_omits_detail_when_null(void) {
    /* Pre-existing v1 shape MUST be preserved for checks that don't set
     * detail_json — otherwise this would be a breaking schema change. */
    hu_doctor_json_entry_t e = {
        .name = "install", .verdict = HU_DOCTOR_PASS, .reason = "ok", .detail_json = NULL};
    char *out = emit_to_buf(&e, 1, HU_TEST_FIXED_EPOCH, NULL);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "\"detail\"") == NULL);
    free(out);
}

static void test_emit_omits_detail_when_empty_string(void) {
    /* Empty-string detail is treated as "not set" — checks that compute
     * a result-dependent detail and end up with nothing to report should
     * not surface an empty value to consumers. */
    hu_doctor_json_entry_t e = {
        .name = "install", .verdict = HU_DOCTOR_PASS, .reason = "ok", .detail_json = ""};
    char *out = emit_to_buf(&e, 1, HU_TEST_FIXED_EPOCH, NULL);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "\"detail\"") == NULL);
    free(out);
}

static void test_emit_detail_appears_after_reason_inside_check_object(void) {
    /* Pin the field order so consumers that stream-parse can rely on it.
     * Order: name, verdict, reason, detail (when present). */
    hu_doctor_json_entry_t e = {
        .name = "pb", .verdict = HU_DOCTOR_PASS, .reason = "ok", .detail_json = "{\"k\":1}"};
    char *out = emit_to_buf(&e, 1, HU_TEST_FIXED_EPOCH, NULL);
    HU_ASSERT_NOT_NULL(out);
    const char *reason = strstr(out, "\"reason\":");
    const char *detail = strstr(out, "\"detail\":");
    HU_ASSERT_NOT_NULL(reason);
    HU_ASSERT_NOT_NULL(detail);
    HU_ASSERT_TRUE(detail > reason);
    free(out);
}

static void test_emit_mixed_checks_some_with_detail_some_without(void) {
    /* Realistic shape: aggregate of registry results where only some
     * checks supply detail. The emitter must handle the heterogeneous
     * array without leaking a stray "detail":null or trailing comma. */
    hu_doctor_json_entry_t entries[] = {
        {.name = "a", .verdict = HU_DOCTOR_PASS, .reason = "", .detail_json = NULL},
        {.name = "b", .verdict = HU_DOCTOR_PASS, .reason = "", .detail_json = "{\"x\":1}"},
        {.name = "c", .verdict = HU_DOCTOR_PASS, .reason = "", .detail_json = NULL},
    };
    char *out = emit_to_buf(entries, 3, HU_TEST_FIXED_EPOCH, NULL);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "\"name\":\"a\"") != NULL);
    HU_ASSERT_TRUE(strstr(out, "\"name\":\"b\"") != NULL);
    HU_ASSERT_TRUE(strstr(out, "\"name\":\"c\"") != NULL);
    HU_ASSERT_TRUE(strstr(out, "\"detail\":{\"x\":1}") != NULL);
    /* Exactly ONE "detail" key in the whole output (only b sets it). */
    const char *first = strstr(out, "\"detail\"");
    HU_ASSERT_NOT_NULL(first);
    HU_ASSERT_TRUE(strstr(first + 1, "\"detail\"") == NULL);
    free(out);
}

/* ── output discipline ────────────────────────────────────────────── */

static void test_emit_ends_with_newline(void) {
    /* Consumers expect a trailing newline so they can detect end-of-message
     * cleanly when streaming over a pipe. */
    hu_doctor_json_entry_t e = {.name = "x", .verdict = HU_DOCTOR_PASS, .reason = ""};
    size_t len = 0;
    char *out = emit_to_buf(&e, 1, HU_TEST_FIXED_EPOCH, &len);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_EQ((int)out[len - 1], (int)'\n');
    free(out);
}

/* ── runner ───────────────────────────────────────────────────────── */

void run_doctor_json_output_tests(void) {
    HU_TEST_SUITE("doctor_json_output");

    HU_RUN_TEST(test_emit_rejects_null_out);
    HU_RUN_TEST(test_emit_rejects_null_entries_with_nonzero_count);
    HU_RUN_TEST(test_emit_zero_checks_returns_ok_and_empty_array);

    HU_RUN_TEST(test_emit_version_field_is_one);
    HU_RUN_TEST(test_emit_timestamp_is_iso8601_utc);
    HU_RUN_TEST(test_emit_includes_check_name_verdict_reason);
    HU_RUN_TEST(test_emit_na_verdict_collapses_to_pass);

    HU_RUN_TEST(test_emit_all_pass_aggregate_pass);
    HU_RUN_TEST(test_emit_pass_plus_na_aggregate_pass);
    HU_RUN_TEST(test_emit_single_fail_aggregate_fail);
    HU_RUN_TEST(test_emit_unknown_verdict_aggregate_fail);

    HU_RUN_TEST(test_emit_escapes_quote_in_reason);
    HU_RUN_TEST(test_emit_escapes_backslash_in_reason);
    HU_RUN_TEST(test_emit_handles_null_reason);

    HU_RUN_TEST(test_emit_includes_detail_when_present);
    HU_RUN_TEST(test_emit_omits_detail_when_null);
    HU_RUN_TEST(test_emit_omits_detail_when_empty_string);
    HU_RUN_TEST(test_emit_detail_appears_after_reason_inside_check_object);
    HU_RUN_TEST(test_emit_mixed_checks_some_with_detail_some_without);

    HU_RUN_TEST(test_emit_ends_with_newline);
}
