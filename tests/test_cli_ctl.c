/* tests/test_cli_ctl.c
 *
 * Unit tests for `human ctl guard` (src/cli_ctl.c) — the read-only
 * runtime kill-switch inspector for response_guard. The CLI prints
 * current state + copy-paste config snippets; it does NOT mutate config.
 *
 * These tests exercise the deterministic, no-config-load dispatch paths
 * directly: argument validation, --help, and the static-snippet
 * subcommands (enable-g9 / disable-g9). The `status` and `list-channels`
 * subcommands call hu_config_load (reads ~/.human/config.json), so they
 * are NOT exercised here — they depend on the operator's environment and
 * are non-deterministic in a unit test.
 *
 * cli_ctl.c's only public entry point is cmd_ctl(), which is the CLI
 * dispatcher convention (not an hu_-prefixed library symbol) and has no
 * public header — hence the @covers-none opt-out below. The test still
 * calls the real cmd_ctl(), exercising the production code path.
 */

// @covers-none — cli_ctl.c's public entry is cmd_ctl (CLI dispatcher, not hu_-prefixed); this test
// calls cmd_ctl directly

#include "test_framework.h"

#include "human/core/allocator.h"
#include "human/core/error.h"

/* cmd_ctl has no public header (CLI-local dispatcher). Declared extern
 * here; resolved at link against the cli_ctl.c object. */
extern hu_error_t cmd_ctl(hu_allocator_t *alloc, int argc, char **argv);

/* Too few arguments (`human ctl`) prints usage and reports invalid args. */
static void test_cli_ctl_no_subcommand_returns_invalid_argument(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char a0[] = "human";
    char a1[] = "ctl";
    char *argv[] = {a0, a1};
    HU_ASSERT_EQ((int)cmd_ctl(&alloc, 2, argv), (int)HU_ERR_INVALID_ARGUMENT);
}

/* `human ctl help` (and --help/-h) is a successful no-op that prints usage. */
static void test_cli_ctl_help_returns_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char a0[] = "human";
    char a1[] = "ctl";
    char a2[] = "help";
    char *argv[] = {a0, a1, a2};
    HU_ASSERT_EQ((int)cmd_ctl(&alloc, 3, argv), (int)HU_OK);
}

/* An unknown top-level subcommand is rejected. */
static void test_cli_ctl_unknown_subcommand_returns_invalid_argument(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char a0[] = "human";
    char a1[] = "ctl";
    char a2[] = "frobnicate";
    char *argv[] = {a0, a1, a2};
    HU_ASSERT_EQ((int)cmd_ctl(&alloc, 3, argv), (int)HU_ERR_INVALID_ARGUMENT);
}

/* `human ctl guard` with no guard subcommand prints usage + invalid args. */
static void test_cli_ctl_guard_without_subcommand_returns_invalid_argument(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char a0[] = "human";
    char a1[] = "ctl";
    char a2[] = "guard";
    char *argv[] = {a0, a1, a2};
    HU_ASSERT_EQ((int)cmd_ctl(&alloc, 3, argv), (int)HU_ERR_INVALID_ARGUMENT);
}

/* disable-g9 prints a static snippet and succeeds without loading config. */
static void test_cli_ctl_guard_disable_g9_returns_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char a0[] = "human";
    char a1[] = "ctl";
    char a2[] = "guard";
    char a3[] = "disable-g9";
    char *argv[] = {a0, a1, a2, a3};
    HU_ASSERT_EQ((int)cmd_ctl(&alloc, 4, argv), (int)HU_OK);
}

/* enable-g9 prints a static snippet and succeeds without loading config. */
static void test_cli_ctl_guard_enable_g9_returns_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char a0[] = "human";
    char a1[] = "ctl";
    char a2[] = "guard";
    char a3[] = "enable-g9";
    char *argv[] = {a0, a1, a2, a3};
    HU_ASSERT_EQ((int)cmd_ctl(&alloc, 4, argv), (int)HU_OK);
}

/* An unknown guard subcommand is rejected before any config load. */
static void test_cli_ctl_guard_unknown_subcommand_returns_invalid_argument(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char a0[] = "human";
    char a1[] = "ctl";
    char a2[] = "guard";
    char a3[] = "frobnicate";
    char *argv[] = {a0, a1, a2, a3};
    HU_ASSERT_EQ((int)cmd_ctl(&alloc, 4, argv), (int)HU_ERR_INVALID_ARGUMENT);
}

void run_cli_ctl_tests(void) {
    HU_TEST_SUITE("cli_ctl");
    HU_RUN_TEST(test_cli_ctl_no_subcommand_returns_invalid_argument);
    HU_RUN_TEST(test_cli_ctl_help_returns_ok);
    HU_RUN_TEST(test_cli_ctl_unknown_subcommand_returns_invalid_argument);
    HU_RUN_TEST(test_cli_ctl_guard_without_subcommand_returns_invalid_argument);
    HU_RUN_TEST(test_cli_ctl_guard_disable_g9_returns_ok);
    HU_RUN_TEST(test_cli_ctl_guard_enable_g9_returns_ok);
    HU_RUN_TEST(test_cli_ctl_guard_unknown_subcommand_returns_invalid_argument);
}
