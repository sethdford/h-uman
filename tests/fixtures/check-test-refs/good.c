/* Fixture: good.c — a test file that correctly references a production symbol.
 * Used by check-test-references.sh smoke-test (AC-10.4).
 *
 * This file is named test_daemon.c in the smoke-test invocation so the script
 * resolves its production module to src/daemon.c and checks for hu_daemon_* or
 * similar symbols.  The reference below satisfies the check. */

/* Reference to a real production symbol exported from src/daemon.c */
extern void hu_daemon_run(void);

static void test_daemon_starts_and_stops(void) {
    /* In a real test this would call hu_daemon_run() under HU_IS_TEST.
     * Here we just reference the symbol to satisfy the fixture requirement. */
    (void)hu_daemon_run;
}
