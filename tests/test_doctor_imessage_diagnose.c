/* US-9.6: `human doctor imessage` chat.db locked diagnostic.
 *
 * Pure-predicate tests for `hu_imessage_diag_from_poll_status` — the
 * exported test seam over the doctor's poll-status-to-presentation
 * mapping. No filesystem, no sqlite, no `$HOME` access; we hand the
 * predicate a JSON blob and assert presentation strings + severity +
 * category.
 *
 * Each test follows `.claude/rules/tests-that-pin-bugs.md`: the name is
 * a CLAIM the assertions must enforce, including the *negative*
 * cross-pollination assertions ("BUSY output must NOT mention 'Full
 * Disk Access'" and vice versa).
 *
 * This file references `hu_imessage_diag_from_poll_status` and
 * `hu_doctor_check_imessage` (production symbols) to satisfy
 * `.claude/rules/test-references-production-symbol.md`. */

#include "human/core/allocator.h"
#include "human/doctor.h"
#include "test_framework.h"
#include <stdlib.h>
#include <string.h>

/* ── shared helpers ─────────────────────────────────────────────────── */

static void diag_item_free(hu_allocator_t *alloc, hu_diag_item_t *it) {
    if (!it)
        return;
    if (it->category)
        alloc->free(alloc->ctx, (void *)it->category, strlen(it->category) + 1);
    if (it->message)
        alloc->free(alloc->ctx, (void *)it->message, strlen(it->message) + 1);
    it->category = NULL;
    it->message = NULL;
}

static bool msg_contains(const hu_diag_item_t *it, const char *needle) {
    return it && it->message && strstr(it->message, needle) != NULL;
}

/* ── AC-9.6.1: AUTH → red, Full Disk Access + System Settings path ──── */

static void test_doctor_imessage_auth_explains_full_disk_access(void) {
    /* AUTH state must produce: severity=ERR, category=imessage_fda, message
     * contains "Full Disk Access" + "System Settings". This pins AC-9.6.1. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_diag_item_t it = {0};
    const char *blob = "{\n"
                       "  \"last_rowid\": 0,\n"
                       "  \"last_successful_poll_epoch\": 0,\n"
                       "  \"consecutive_open_failures\": 1,\n"
                       "  \"circuit_breaker_tripped\": false,\n"
                       "  \"last_error_class\": \"AUTH\"\n"
                       "}\n";

    HU_ASSERT_EQ(hu_imessage_diag_from_poll_status(&alloc, blob, &it), HU_OK);
    HU_ASSERT_EQ(it.severity, HU_DIAG_ERR);
    HU_ASSERT(it.category != NULL);
    HU_ASSERT(strcmp(it.category, "imessage_fda") == 0);
    HU_ASSERT_TRUE(msg_contains(&it, "Full Disk Access"));
    HU_ASSERT_TRUE(msg_contains(&it, "System Settings"));
    diag_item_free(&alloc, &it);
}

static void test_doctor_imessage_auth_does_not_mention_busy_syncing(void) {
    /* Adversarial cross-pollination guard: AUTH output must NOT contain
     * BUSY phrasing. Pinned per `.claude/rules/tests-that-pin-bugs.md` —
     * test name is a claim. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_diag_item_t it = {0};
    const char *blob = "{ \"last_error_class\": \"AUTH\", \"circuit_breaker_tripped\": false,"
                       " \"consecutive_open_failures\": 1 }";

    HU_ASSERT_EQ(hu_imessage_diag_from_poll_status(&alloc, blob, &it), HU_OK);
    HU_ASSERT_FALSE(msg_contains(&it, "Messages.app may be syncing"));
    HU_ASSERT_FALSE(msg_contains(&it, "transient"));
    diag_item_free(&alloc, &it);
}

/* ── AC-9.6.2: BUSY → yellow (WARN), "Messages.app may be syncing" ──── */

static void test_doctor_imessage_busy_explains_transient_sync(void) {
    /* BUSY state must produce: severity=WARN (distinct from ERR), category=
     * imessage_busy, message contains "Messages.app may be syncing". This
     * pins AC-9.6.2. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_diag_item_t it = {0};
    const char *blob = "{\n"
                       "  \"last_rowid\": 12345,\n"
                       "  \"last_successful_poll_epoch\": 990,\n"
                       "  \"consecutive_open_failures\": 1,\n"
                       "  \"circuit_breaker_tripped\": false,\n"
                       "  \"last_error_class\": \"BUSY\"\n"
                       "}\n";

    HU_ASSERT_EQ(hu_imessage_diag_from_poll_status(&alloc, blob, &it), HU_OK);
    HU_ASSERT_EQ(it.severity, HU_DIAG_WARN);
    HU_ASSERT(it.category != NULL);
    HU_ASSERT(strcmp(it.category, "imessage_busy") == 0);
    HU_ASSERT_TRUE(msg_contains(&it, "Messages.app may be syncing"));
    diag_item_free(&alloc, &it);
}

static void test_doctor_imessage_busy_does_not_mention_permission_or_fda(void) {
    /* Adversarial cross-pollination guard: BUSY output must NOT contain
     * "permission denied" or "Full Disk Access" — those are AUTH-state
     * phrasings. If they leak here, the user is sent on a wrong-fix path.
     * Pinned per `.claude/rules/tests-that-pin-bugs.md`. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_diag_item_t it = {0};
    const char *blob = "{ \"last_error_class\": \"BUSY\", \"circuit_breaker_tripped\": false,"
                       " \"consecutive_open_failures\": 0 }";

    HU_ASSERT_EQ(hu_imessage_diag_from_poll_status(&alloc, blob, &it), HU_OK);
    HU_ASSERT_FALSE(msg_contains(&it, "permission denied"));
    HU_ASSERT_FALSE(msg_contains(&it, "Full Disk Access"));
    HU_ASSERT_FALSE(msg_contains(&it, "System Settings"));
    diag_item_free(&alloc, &it);
}

/* ── CANTOPEN → red, "chat.db" + "not found", NOT "permission" ──────── */

static void test_doctor_imessage_cantopen_says_chat_db_not_found(void) {
    /* File-not-found is a third distinct state — message must say "not
     * found" (Messages.app never run, chat.db missing) and must NOT route
     * the user to FDA settings (wrong fix). */
    hu_allocator_t alloc = hu_system_allocator();
    hu_diag_item_t it = {0};
    const char *blob = "{\n"
                       "  \"last_rowid\": 0,\n"
                       "  \"last_successful_poll_epoch\": 0,\n"
                       "  \"consecutive_open_failures\": 1,\n"
                       "  \"circuit_breaker_tripped\": false,\n"
                       "  \"last_error_class\": \"CANTOPEN\"\n"
                       "}\n";

    HU_ASSERT_EQ(hu_imessage_diag_from_poll_status(&alloc, blob, &it), HU_OK);
    HU_ASSERT_EQ(it.severity, HU_DIAG_ERR);
    HU_ASSERT(it.category != NULL);
    HU_ASSERT(strcmp(it.category, "imessage_not_found") == 0);
    HU_ASSERT_TRUE(msg_contains(&it, "chat.db"));
    HU_ASSERT_TRUE(msg_contains(&it, "not found"));
    /* Adversarial: must NOT cross-pollinate with AUTH or BUSY phrasing. */
    HU_ASSERT_FALSE(msg_contains(&it, "permission"));
    HU_ASSERT_FALSE(msg_contains(&it, "Full Disk Access"));
    HU_ASSERT_FALSE(msg_contains(&it, "Messages.app may be syncing"));
    diag_item_free(&alloc, &it);
}

/* ── NONE → OK, no "error"/"denied" phrasing ────────────────────────── */

static void test_doctor_imessage_none_is_ok_and_silent_on_errors(void) {
    /* The healthy state should not contaminate the output with error
     * vocabulary. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_diag_item_t it = {0};
    const char *blob = "{\n"
                       "  \"last_rowid\": 99999,\n"
                       "  \"last_successful_poll_epoch\": 1010,\n"
                       "  \"consecutive_open_failures\": 0,\n"
                       "  \"circuit_breaker_tripped\": false,\n"
                       "  \"last_error_class\": \"NONE\"\n"
                       "}\n";

    HU_ASSERT_EQ(hu_imessage_diag_from_poll_status(&alloc, blob, &it), HU_OK);
    HU_ASSERT_EQ(it.severity, HU_DIAG_OK);
    HU_ASSERT(it.category != NULL);
    HU_ASSERT(strcmp(it.category, "imessage_chat_db") == 0);
    HU_ASSERT_FALSE(msg_contains(&it, "denied"));
    HU_ASSERT_FALSE(msg_contains(&it, "Full Disk Access"));
    HU_ASSERT_FALSE(msg_contains(&it, "Messages.app may be syncing"));
    diag_item_free(&alloc, &it);
}

/* ── OTHER → red, references state but doesn't pretend to know cause ── */

static void test_doctor_imessage_other_does_not_guess_cause(void) {
    /* OTHER is the catch-all for sqlite codes we don't classify. Output
     * must NOT pretend to know the cause (no FDA recommendation, no
     * "Messages.app syncing" guess). */
    hu_allocator_t alloc = hu_system_allocator();
    hu_diag_item_t it = {0};
    const char *blob = "{\n"
                       "  \"last_rowid\": 0,\n"
                       "  \"last_successful_poll_epoch\": 0,\n"
                       "  \"consecutive_open_failures\": 2,\n"
                       "  \"circuit_breaker_tripped\": false,\n"
                       "  \"last_error_class\": \"OTHER\"\n"
                       "}\n";

    HU_ASSERT_EQ(hu_imessage_diag_from_poll_status(&alloc, blob, &it), HU_OK);
    HU_ASSERT_EQ(it.severity, HU_DIAG_ERR);
    HU_ASSERT(it.category != NULL);
    HU_ASSERT(strcmp(it.category, "imessage_other") == 0);
    HU_ASSERT_FALSE(msg_contains(&it, "Full Disk Access"));
    HU_ASSERT_FALSE(msg_contains(&it, "Messages.app may be syncing"));
    diag_item_free(&alloc, &it);
}

/* ── AC-9.6.3: breaker tripped → consecutive count + --fix suggestion ─ */

static void test_doctor_imessage_breaker_tripped_shows_count_and_fix(void) {
    /* Breaker-tripped state takes precedence over the underlying class:
     * category=imessage_breaker, severity=ERR, message includes the
     * consecutive_open_failures count AND suggests `human doctor --fix`.
     * The underlying class (AUTH here) is also surfaced so the user
     * still knows what to do. Pins AC-9.6.3. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_diag_item_t it = {0};
    const char *blob = "{\n"
                       "  \"last_rowid\": 12345,\n"
                       "  \"last_successful_poll_epoch\": 0,\n"
                       "  \"consecutive_open_failures\": 9,\n"
                       "  \"circuit_breaker_tripped\": true,\n"
                       "  \"last_error_class\": \"AUTH\"\n"
                       "}\n";

    HU_ASSERT_EQ(hu_imessage_diag_from_poll_status(&alloc, blob, &it), HU_OK);
    HU_ASSERT_EQ(it.severity, HU_DIAG_ERR);
    HU_ASSERT(it.category != NULL);
    HU_ASSERT(strcmp(it.category, "imessage_breaker") == 0);
    HU_ASSERT_TRUE(msg_contains(&it, "9"));
    HU_ASSERT_TRUE(msg_contains(&it, "Full Disk Access"));
    HU_ASSERT_TRUE(msg_contains(&it, "human doctor --fix"));
    diag_item_free(&alloc, &it);
}

/* ── poll-status BUSY without breaker → WARN, no FDA leak ───────────── */

static void test_doctor_imessage_busy_severity_is_warn_not_err(void) {
    /* Distinct from AUTH: BUSY is transient → WARN, not ERR. AC-9.6.2's
     * "yellow distinct from red" claim is enforced by this assertion. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_diag_item_t it_busy = {0};
    hu_diag_item_t it_auth = {0};
    const char *busy_blob = "{ \"last_error_class\": \"BUSY\", \"circuit_breaker_tripped\": false,"
                            " \"consecutive_open_failures\": 1 }";
    const char *auth_blob = "{ \"last_error_class\": \"AUTH\", \"circuit_breaker_tripped\": false,"
                            " \"consecutive_open_failures\": 1 }";

    HU_ASSERT_EQ(hu_imessage_diag_from_poll_status(&alloc, busy_blob, &it_busy), HU_OK);
    HU_ASSERT_EQ(hu_imessage_diag_from_poll_status(&alloc, auth_blob, &it_auth), HU_OK);
    /* Severities are strictly different and BUSY is the lower one. */
    HU_ASSERT_EQ(it_busy.severity, HU_DIAG_WARN);
    HU_ASSERT_EQ(it_auth.severity, HU_DIAG_ERR);
    HU_ASSERT(it_busy.severity != it_auth.severity);
    diag_item_free(&alloc, &it_busy);
    diag_item_free(&alloc, &it_auth);
}

/* ── defensive: NULL args rejected ──────────────────────────────────── */

static void test_doctor_imessage_diag_null_args_rejected(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_diag_item_t it = {0};
    HU_ASSERT_EQ(hu_imessage_diag_from_poll_status(NULL, "{}", &it), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_imessage_diag_from_poll_status(&alloc, NULL, &it), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_imessage_diag_from_poll_status(&alloc, "{}", NULL), HU_ERR_INVALID_ARGUMENT);
}

/* ── defensive: corrupt JSON does not crash, returns OTHER/UNKNOWN ──── */

static void test_doctor_imessage_diag_corrupt_json_is_safe(void) {
    /* `last_error_class` missing or unparseable → treat as unknown/OTHER.
     * Must not crash, must not falsely report OK. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_diag_item_t it = {0};

    HU_ASSERT_EQ(hu_imessage_diag_from_poll_status(&alloc, "", &it), HU_OK);
    /* Empty blob → no class extracted → must surface as a non-OK state so
     * the user knows the channel hasn't healthily polled. */
    HU_ASSERT(it.severity != HU_DIAG_OK);
    HU_ASSERT_FALSE(msg_contains(&it, "Full Disk Access"));
    HU_ASSERT_FALSE(msg_contains(&it, "Messages.app may be syncing"));
    diag_item_free(&alloc, &it);

    hu_diag_item_t it2 = {0};
    HU_ASSERT_EQ(hu_imessage_diag_from_poll_status(&alloc, "{not json at all", &it2), HU_OK);
    HU_ASSERT(it2.severity != HU_DIAG_OK);
    diag_item_free(&alloc, &it2);
}

/* ── integration touchpoint: hu_doctor_check_imessage symbol reference ─
 *
 * `.claude/rules/test-references-production-symbol.md` requires this
 * test file (matching `tests/test_doctor_imessage_diagnose.c`) reference
 * the production module. The diagnose predicate lives in `src/doctor.c`
 * alongside `hu_doctor_check_imessage`; this test takes the function
 * pointer to ensure the symbol resolves at link time.
 *
 * We deliberately do not invoke it (it touches `$HOME/Library/Messages`
 * and the host PATH); the reference alone satisfies the rule. */
static void test_doctor_imessage_diagnose_references_check_imessage_symbol(void) {
    /* Pointer comparison forces the symbol to be linked without calling
     * it. */
    hu_error_t (*p)(hu_allocator_t *, int64_t, int64_t, hu_diag_item_t **, size_t *, size_t *) =
        hu_doctor_check_imessage;
    HU_ASSERT(p != NULL);
}

void run_doctor_imessage_diagnose_tests(void) {
    HU_TEST_SUITE("Doctor iMessage Diagnose (US-9.6)");
    HU_RUN_TEST(test_doctor_imessage_diag_null_args_rejected);
    HU_RUN_TEST(test_doctor_imessage_auth_explains_full_disk_access);
    HU_RUN_TEST(test_doctor_imessage_auth_does_not_mention_busy_syncing);
    HU_RUN_TEST(test_doctor_imessage_busy_explains_transient_sync);
    HU_RUN_TEST(test_doctor_imessage_busy_does_not_mention_permission_or_fda);
    HU_RUN_TEST(test_doctor_imessage_busy_severity_is_warn_not_err);
    HU_RUN_TEST(test_doctor_imessage_cantopen_says_chat_db_not_found);
    HU_RUN_TEST(test_doctor_imessage_none_is_ok_and_silent_on_errors);
    HU_RUN_TEST(test_doctor_imessage_other_does_not_guess_cause);
    HU_RUN_TEST(test_doctor_imessage_breaker_tripped_shows_count_and_fix);
    HU_RUN_TEST(test_doctor_imessage_diag_corrupt_json_is_safe);
    HU_RUN_TEST(test_doctor_imessage_diagnose_references_check_imessage_symbol);
}
