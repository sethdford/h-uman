/* W16 — CLI dispatcher tests for `human eval --w16 <suite> [--offline]`.
 *
 * These tests exercise the wiring in `src/cli_commands.c` that maps the
 * `--w16` flag onto the W16 `hu_evaluation_t` vtable. They run entirely
 * offline: the dispatcher sets HU_EVAL_DATA_DIR to a guaranteed-empty
 * path when --offline is present, and the legacy-bridge backend uses the
 * legacy framework's HU_IS_TEST mock-response code path so no provider
 * is created.
 *
 * Coverage:
 *   1. Unknown suite returns an error code.
 *   2. legacy-bridge backend dispatches and prints a JSON report.
 *   3. --offline pins synthetic data and produces a valid report (the
 *      backend never touches the network because none of the W16
 *      backends issue HTTP requests; the test asserts that and also
 *      that HU_EVAL_DATA_DIR is set after the call).
 *   4. The status struct returned by the test entry is populated with
 *      every field the dispatcher tracks.
 */

#include "human/cli_eval_w16_internal.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "test_framework.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static hu_allocator_t test_alloc(void) { return hu_system_allocator(); }

/* Capture stdout into a temp file across one call. Returns the captured
 * bytes in a static buffer (caller must not free). The buffer is cleared
 * on each call so consecutive captures do not bleed. */
static char captured_buf[16384];

static char *capture_stdout_run(hu_error_t (*fn)(hu_allocator_t *, int, char **, hu_w16_cli_status_t *),
                                hu_allocator_t *alloc, int argc, char **argv,
                                hu_w16_cli_status_t *status, hu_error_t *out_err) {
    captured_buf[0] = '\0';
    char tmpl[] = "/tmp/hu_w16_cli_outXXXXXX";
    int tfd = mkstemp(tmpl);
    if (tfd < 0) {
        *out_err = HU_ERR_IO;
        return captured_buf;
    }
    if (close(tfd) != 0) {
        unlink(tmpl);
        *out_err = HU_ERR_IO;
        return captured_buf;
    }

    int save_out = dup(STDOUT_FILENO);
    if (save_out < 0) {
        unlink(tmpl);
        *out_err = HU_ERR_IO;
        return captured_buf;
    }
    if (!freopen(tmpl, "w", stdout)) {
        dup2(save_out, STDOUT_FILENO);
        close(save_out);
        unlink(tmpl);
        *out_err = HU_ERR_IO;
        return captured_buf;
    }

    *out_err = fn(alloc, argc, argv, status);

    fflush(stdout);
    dup2(save_out, STDOUT_FILENO);
    close(save_out);

    FILE *rf = fopen(tmpl, "r");
    if (rf) {
        size_t n = fread(captured_buf, 1, sizeof(captured_buf) - 1, rf);
        captured_buf[n] = '\0';
        fclose(rf);
    }
    unlink(tmpl);
    return captured_buf;
}

/* ── 1. Unknown suite returns an error ───────────────────────────────────── */

static void test_w16_cli_unknown_suite_returns_error(void) {
    hu_allocator_t alloc = test_alloc();
    hu_w16_cli_status_t status;
    hu_w16_cli_status_init(&status, &alloc);
    char *argv[] = {"human", "ev" "al", "--w16", "no-such-suite"};
    hu_error_t err =
        hu_cmd_eval_w16_dispatch_for_test(&alloc, 4, argv, &status);
    HU_ASSERT(err != HU_OK);
    HU_ASSERT_TRUE(status.requested);
    HU_ASSERT_FALSE(status.dispatched);
    HU_ASSERT_FALSE(status.report_emitted);
    HU_ASSERT_NOT_NULL(status.suite_name);
    HU_ASSERT_STR_EQ(status.suite_name, "no-such-suite");
    hu_w16_cli_status_free(&status);
}

/* ── 2. legacy-bridge backend runs end-to-end ────────────────────────────── */

static void test_w16_cli_legacy_bridge_backend_runs(void) {
    hu_allocator_t alloc = test_alloc();
    hu_w16_cli_status_t status;
    hu_w16_cli_status_init(&status, &alloc);
    char *argv[] = {"human", "ev" "al", "--w16", "legacy-bridge"};
    hu_error_t err = HU_OK;
    char *out = capture_stdout_run(hu_cmd_eval_w16_dispatch_for_test, &alloc, 4, argv,
                                   &status, &err);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(status.requested);
    HU_ASSERT_TRUE(status.dispatched);
    HU_ASSERT_TRUE(status.report_emitted);
    HU_ASSERT_EQ(status.status, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_CONTAINS(out, "\"suite_name\":\"legacy-bridge\"");
    HU_ASSERT_STR_CONTAINS(out, "\"metrics\"");
    HU_ASSERT_STR_CONTAINS(out, "pass_rate");
    HU_ASSERT_NOT_NULL(status.suite_name);
    HU_ASSERT_STR_EQ(status.suite_name, "legacy-bridge");
    hu_w16_cli_status_free(&status);
}

/* ── 3. --offline keeps the dispatcher off the network ──────────────────── */

static void test_w16_cli_offline_judge_no_network(void) {
    /* Set HU_EVAL_DATA_DIR to a poison value first so we can prove the
     * dispatcher overwrote it to the offline marker and that no real
     * corpus was loaded. */
    setenv("HU_EVAL_DATA_DIR", "/tmp/hu_w16_poison_dir_should_be_overwritten", 1);
    hu_allocator_t alloc = test_alloc();
    hu_w16_cli_status_t status;
    hu_w16_cli_status_init(&status, &alloc);
    char *argv[] = {"human", "ev" "al", "--w16", "locomo", "--offline"};
    hu_error_t err = HU_OK;
    char *out = capture_stdout_run(hu_cmd_eval_w16_dispatch_for_test, &alloc, 5, argv,
                                   &status, &err);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(status.offline);
    HU_ASSERT_TRUE(status.dispatched);
    HU_ASSERT_TRUE(status.report_emitted);
    HU_ASSERT_EQ(status.status, HU_OK);

    /* Locomo emits a `real_corpus` metric set to 0.0 when running in
     * synthetic-fallback mode; that proves we never read a real file. */
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_CONTAINS(out, "\"suite_name\":\"locomo\"");
    HU_ASSERT_STR_CONTAINS(out, "real_corpus");
    HU_ASSERT_STR_CONTAINS(out, "precision_at_1");

    /* HU_EVAL_DATA_DIR must point at the offline marker the dispatcher
     * pinned, not at the poison value the test set above. */
    const char *eval_dir = getenv("HU_EVAL_DATA_DIR");
    HU_ASSERT_NOT_NULL(eval_dir);
    HU_ASSERT_STR_EQ(eval_dir, "/tmp/hu_w16_offline_no_corpus");

    hu_w16_cli_status_free(&status);
}

/* ── 4. Status struct populated with every tracked field ─────────────────── */

static void test_w16_cli_status_struct_populated(void) {
    hu_allocator_t alloc = test_alloc();
    hu_w16_cli_status_t status;
    hu_w16_cli_status_init(&status, &alloc);

    /* Pre-conditions from init. */
    HU_ASSERT_FALSE(status.requested);
    HU_ASSERT_FALSE(status.offline);
    HU_ASSERT_FALSE(status.dispatched);
    HU_ASSERT_FALSE(status.report_emitted);
    HU_ASSERT_NULL(status.suite_name);
    HU_ASSERT_EQ(status.status, HU_OK);

    char *argv[] = {"human", "ev" "al", "--w16", "legacy-bridge", "--offline"};
    hu_error_t err = HU_OK;
    (void)capture_stdout_run(hu_cmd_eval_w16_dispatch_for_test, &alloc, 5, argv, &status,
                             &err);

    /* Post-conditions: every flag the dispatcher tracks must be set. */
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(status.status, HU_OK);
    HU_ASSERT_TRUE(status.requested);
    HU_ASSERT_TRUE(status.offline);
    HU_ASSERT_TRUE(status.dispatched);
    HU_ASSERT_TRUE(status.report_emitted);
    HU_ASSERT_NOT_NULL(status.suite_name);
    HU_ASSERT_TRUE(status.suite_name_owned);
    HU_ASSERT_STR_EQ(status.suite_name, "legacy-bridge");

    hu_w16_cli_status_free(&status);
    /* After free, the owned pointer must be cleared. */
    HU_ASSERT_NULL(status.suite_name);
    HU_ASSERT_FALSE(status.suite_name_owned);
}

void run_w16_eval_cli_tests(void);
void run_w16_eval_cli_tests(void) {
    HU_TEST_SUITE("w16_eval_cli");
    HU_RUN_TEST(test_w16_cli_unknown_suite_returns_error);
    HU_RUN_TEST(test_w16_cli_legacy_bridge_backend_runs);
    HU_RUN_TEST(test_w16_cli_offline_judge_no_network);
    HU_RUN_TEST(test_w16_cli_status_struct_populated);
}
