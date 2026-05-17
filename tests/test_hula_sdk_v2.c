/*
 * tests/test_hula_sdk_v2.c — Tests for HuLa SDK v0.2.0 surface (US-10.1).
 *
 * Covers AC-10.1.1 through AC-10.1.5 of US-10.1:
 *   - Version macros bumped to 0.2.0
 *   - Opaque ctx handle: create / destroy round-trip
 *   - Argument validation on create
 *   - Error string accessor: known codes return distinct non-NULL strings
 *   - Error string accessor: unknown code returns the "HU_ERR_UNKNOWN" sentinel
 *
 * The unknown-sentinel test asserts the EXACT sentinel literal — not just
 * "some non-empty string" — per `.claude/rules/tests-that-pin-bugs.md`.
 * If the sentinel ever changes silently, this test fails loudly.
 *
 * References production symbols (satisfies test-references-production-symbol):
 *   - hu_hula_ctx_create
 *   - hu_hula_ctx_destroy
 *   - hu_hula_error_string
 * (Module-name heuristic resolves test_hula_sdk_v2 -> agent/hula_sdk.c.)
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/hula_sdk.h"
#include "test_framework.h"

#include <string.h>

/* AC-10.1.4: version macros equal 0.2.0. */
static void hula_sdk_v2_version_is_v02(void) {
    HU_ASSERT_EQ(HU_HULA_SDK_VERSION_MAJOR, 0);
    HU_ASSERT_EQ(HU_HULA_SDK_VERSION_MINOR, 2);
    HU_ASSERT_EQ(HU_HULA_SDK_VERSION_PATCH, 0);
    HU_ASSERT_STR_EQ(HU_HULA_SDK_VERSION_STRING, "0.2.0");
}

/* AC-10.1.1: ctx create/destroy round-trip is ASan-clean. */
static void hula_sdk_v2_ctx_create_destroy_roundtrip(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_hula_ctx_t *ctx = NULL;
    HU_ASSERT_EQ(hu_hula_ctx_create(&alloc, &ctx), HU_OK);
    HU_ASSERT_NOT_NULL(ctx);
    hu_hula_ctx_destroy(ctx);
    /* ASan reports leaks on the dev preset; passing here proves no leak. */
}

/* AC-10.1.1 (negative): NULL allocator rejected; *out untouched. */
static void hula_sdk_v2_ctx_create_rejects_null_alloc(void) {
    hu_hula_ctx_t *ctx = (hu_hula_ctx_t *)0xDEADBEEF;
    HU_ASSERT_EQ(hu_hula_ctx_create(NULL, &ctx), HU_ERR_INVALID_ARGUMENT);
    /* Contract: *out is not modified on error — caller's sentinel survives. */
    HU_ASSERT_TRUE(ctx == (hu_hula_ctx_t *)0xDEADBEEF);
}

/* AC-10.1.1 (negative): NULL out parameter rejected. */
static void hula_sdk_v2_ctx_create_rejects_null_out(void) {
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(hu_hula_ctx_create(&alloc, NULL), HU_ERR_INVALID_ARGUMENT);
}

/* hu_hula_ctx_destroy must be NULL-safe (binding finalizers may call it twice). */
static void hula_sdk_v2_ctx_destroy_null_is_safe(void) {
    hu_hula_ctx_destroy(NULL);
    /* No assertion: the contract is "does not crash". */
}

/* AC-10.1.2 (positive): known codes return non-NULL, non-empty, distinct strings. */
static void hula_sdk_v2_error_string_known_codes(void) {
    const char *ok = hu_hula_error_string(HU_OK);
    const char *inv = hu_hula_error_string(HU_ERR_INVALID_ARGUMENT);
    const char *oom = hu_hula_error_string(HU_ERR_OUT_OF_MEMORY);

    HU_ASSERT_NOT_NULL(ok);
    HU_ASSERT_NOT_NULL(inv);
    HU_ASSERT_NOT_NULL(oom);
    HU_ASSERT_TRUE(strlen(ok) > 0);
    HU_ASSERT_TRUE(strlen(inv) > 0);
    HU_ASSERT_TRUE(strlen(oom) > 0);

    /* Each known code maps to a distinct string. */
    HU_ASSERT_TRUE(strcmp(ok, inv) != 0);
    HU_ASSERT_TRUE(strcmp(ok, oom) != 0);
    HU_ASSERT_TRUE(strcmp(inv, oom) != 0);

    /* The strings equal their C identifier names (bindings rely on this). */
    HU_ASSERT_STR_EQ(ok, "HU_OK");
    HU_ASSERT_STR_EQ(inv, "HU_ERR_INVALID_ARGUMENT");
    HU_ASSERT_STR_EQ(oom, "HU_ERR_OUT_OF_MEMORY");
}

/*
 * AC-10.1.2 (negative / sentinel): unknown enum value returns the
 * EXACT sentinel literal "HU_ERR_UNKNOWN".
 *
 * Per .claude/rules/tests-that-pin-bugs.md: assert the sentinel literal
 * exactly so an accidental change ("unknown error", lowercased, NULL, etc.)
 * fails this test loudly.
 */
static void hula_sdk_v2_error_string_unknown_returns_sentinel(void) {
    const char *s = hu_hula_error_string((hu_error_t)9999);
    HU_ASSERT_NOT_NULL(s);
    HU_ASSERT_STR_EQ(s, "HU_ERR_UNKNOWN");
}

void run_hula_sdk_v2_tests(void);
void run_hula_sdk_v2_tests(void) {
    HU_TEST_SUITE("hula_sdk_v2");
    HU_RUN_TEST(hula_sdk_v2_version_is_v02);
    HU_RUN_TEST(hula_sdk_v2_ctx_create_destroy_roundtrip);
    HU_RUN_TEST(hula_sdk_v2_ctx_create_rejects_null_alloc);
    HU_RUN_TEST(hula_sdk_v2_ctx_create_rejects_null_out);
    HU_RUN_TEST(hula_sdk_v2_ctx_destroy_null_is_safe);
    HU_RUN_TEST(hula_sdk_v2_error_string_known_codes);
    HU_RUN_TEST(hula_sdk_v2_error_string_unknown_returns_sentinel);
}
