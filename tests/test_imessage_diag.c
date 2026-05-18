/*
 * US-43.5 — chat.db locked diagnostic
 *
 * Pure-function tests over the poll-status -> user-actionable message
 * formatter. Pins the AC-43.5.1..5 contract:
 *
 *   AC-43.5.1 (positive): AUTH output contains "Full Disk Access"
 *   AC-43.5.1 (adversary): AUTH output does NOT mention "Messages.app may be syncing"
 *   AC-43.5.2 (positive): BUSY output contains "Messages.app may be syncing"
 *   AC-43.5.2 (adversary): BUSY output does NOT mention "Full Disk Access" or "permission"
 *   AC-43.5.3:            CANTOPEN output contains "chat.db" and a "~/Library/Messages" hint
 *   AC-43.5.4:            NONE returns *out == NULL with HU_OK
 *   AC-43.5.5:            OTHER uses hu_imessage_error_class_name(HU_IMESSAGE_ERR_OTHER) ("OTHER")
 *                         AND from_name(name(v)) roundtrip is correct for every enum variant.
 *
 * Test seam: this test exercises hu_imessage_diag_from_poll_status from
 * human/doctor.h plus hu_imessage_error_class_{name,from_name} from
 * human/channels/imessage.h. The check-test-references.sh heuristic
 * derives "imessage" from the filename and resolves the candidate
 * production file by `find src -name imessage.c -type f | head -1`, which
 * is filesystem-order-dependent and selects src/feeds/imessage.c on some
 * machines (bash) and src/channels/imessage.c on others (zsh). Since this
 * test covers symbols from BOTH src/doctor.c and src/channels/imessage.c
 * (a genuine cross-module test) — neither of which matches the basename
 * heuristic — opt out explicitly.
 */
// @covers-none — cross-module test of doctor + channels/imessage formatter
#if HU_HAS_IMESSAGE
#include "human/channels/imessage.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/doctor.h"
#include "test_framework.h"
#include <string.h>

/* ── 1. AUTH (AC-43.5.1) ─────────────────────────────────────────────── */
static void imessage_diag_auth_contains_full_disk_access(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    hu_error_t err =
        hu_imessage_diag_from_poll_status(&alloc, "{\"last_error_class\":\"AUTH\"}", &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(out != NULL);
    HU_ASSERT_TRUE(strstr(out, "Full Disk Access") != NULL);
    alloc.free(alloc.ctx, out, strlen(out) + 1);
}

static void imessage_diag_auth_does_not_mention_syncing(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    hu_error_t err =
        hu_imessage_diag_from_poll_status(&alloc, "{\"last_error_class\":\"AUTH\"}", &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(out != NULL);
    /* Critical cross-contamination guard — AUTH must not bleed BUSY copy. */
    HU_ASSERT_TRUE(strstr(out, "Messages.app may be syncing") == NULL);
    HU_ASSERT_TRUE(strstr(out, "syncing") == NULL);
    alloc.free(alloc.ctx, out, strlen(out) + 1);
}

/* ── 2. BUSY (AC-43.5.2) ─────────────────────────────────────────────── */
static void imessage_diag_busy_contains_syncing(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    hu_error_t err =
        hu_imessage_diag_from_poll_status(&alloc, "{\"last_error_class\":\"BUSY\"}", &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(out != NULL);
    HU_ASSERT_TRUE(strstr(out, "Messages.app may be syncing") != NULL);
    alloc.free(alloc.ctx, out, strlen(out) + 1);
}

static void imessage_diag_busy_does_not_mention_full_disk_access(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    hu_error_t err =
        hu_imessage_diag_from_poll_status(&alloc, "{\"last_error_class\":\"BUSY\"}", &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(out != NULL);
    /* Critical cross-contamination guard — BUSY must not bleed AUTH copy
     * or imply the user needs to change macOS permissions. */
    HU_ASSERT_TRUE(strstr(out, "Full Disk Access") == NULL);
    HU_ASSERT_TRUE(strstr(out, "permission") == NULL);
    alloc.free(alloc.ctx, out, strlen(out) + 1);
}

/* ── 3. CANTOPEN (AC-43.5.3) ─────────────────────────────────────────── */
static void imessage_diag_cantopen_contains_chat_db_and_path_hint(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    hu_error_t err =
        hu_imessage_diag_from_poll_status(&alloc, "{\"last_error_class\":\"CANTOPEN\"}", &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(out != NULL);
    HU_ASSERT_TRUE(strstr(out, "chat.db") != NULL);
    HU_ASSERT_TRUE(strstr(out, "~/Library/Messages") != NULL);
    /* And — CANTOPEN must not bleed AUTH or BUSY copy either. */
    HU_ASSERT_TRUE(strstr(out, "Full Disk Access") == NULL);
    HU_ASSERT_TRUE(strstr(out, "syncing") == NULL);
    alloc.free(alloc.ctx, out, strlen(out) + 1);
}

/* ── 4. NONE (AC-43.5.4) ─────────────────────────────────────────────── */
static void imessage_diag_none_returns_null_output(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = (char *)0x1; /* non-NULL sentinel — formatter must zero this */
    hu_error_t err =
        hu_imessage_diag_from_poll_status(&alloc, "{\"last_error_class\":\"NONE\"}", &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(out == NULL);
}

static void imessage_diag_missing_class_field_returns_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = (char *)0x1;
    hu_error_t err = hu_imessage_diag_from_poll_status(&alloc, "{\"last_rowid\":42}", &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(out == NULL);
}

/* ── 5. OTHER (AC-43.5.5) ────────────────────────────────────────────── */
static void imessage_diag_other_uses_class_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    hu_error_t err =
        hu_imessage_diag_from_poll_status(&alloc, "{\"last_error_class\":\"OTHER\"}", &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(out != NULL);
    /* The class name must come from the canonical helper, not a parallel
     * table. Asserting on the string "OTHER" indirectly pins that the
     * implementation calls hu_imessage_error_class_name(HU_IMESSAGE_ERR_OTHER). */
    HU_ASSERT_TRUE(strstr(out, "OTHER") != NULL);
    alloc.free(alloc.ctx, out, strlen(out) + 1);
}

/* Unrecognized class strings (e.g. a future SQLite class the parser does
 * not yet know) must fall through to OTHER, not crash and not invent a
 * new bucket. */
static void imessage_diag_unrecognized_class_falls_through_to_other(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    hu_error_t err = hu_imessage_diag_from_poll_status(
        &alloc, "{\"last_error_class\":\"FUTURE_VARIANT\"}", &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(out != NULL);
    HU_ASSERT_TRUE(strstr(out, "OTHER") != NULL);
    alloc.free(alloc.ctx, out, strlen(out) + 1);
}

/* ── 6. from_name roundtrip (AC-43.5.5 — no parallel enum) ───────────── */
static void imessage_error_class_from_name_roundtrip(void) {
    /* For every enum variant, name -> from_name -> back must be lossless.
     * Pins that there is exactly ONE enum and ONE name table. */
    const hu_imessage_error_class_t variants[] = {
        HU_IMESSAGE_ERR_NONE, HU_IMESSAGE_ERR_AUTH,  HU_IMESSAGE_ERR_CANTOPEN,
        HU_IMESSAGE_ERR_BUSY, HU_IMESSAGE_ERR_OTHER,
    };
    for (size_t i = 0; i < sizeof(variants) / sizeof(variants[0]); i++) {
        const char *name = hu_imessage_error_class_name(variants[i]);
        HU_ASSERT_TRUE(name != NULL);
        HU_ASSERT_EQ((int)hu_imessage_error_class_from_name(name), (int)variants[i]);
    }
    /* NULL and unknown both map to OTHER (per header contract). */
    HU_ASSERT_EQ((int)hu_imessage_error_class_from_name(NULL), (int)HU_IMESSAGE_ERR_OTHER);
    HU_ASSERT_EQ((int)hu_imessage_error_class_from_name(""), (int)HU_IMESSAGE_ERR_OTHER);
    HU_ASSERT_EQ((int)hu_imessage_error_class_from_name("notarealclass"),
                 (int)HU_IMESSAGE_ERR_OTHER);
}

/* ── 7. NULL input safety ────────────────────────────────────────────── */
static void imessage_diag_null_inputs_are_safe(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = (char *)0x1;
    /* NULL alloc -> INVALID_ARGUMENT. */
    hu_error_t err = hu_imessage_diag_from_poll_status(NULL, "{}", &out);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    /* NULL out -> INVALID_ARGUMENT. */
    err = hu_imessage_diag_from_poll_status(&alloc, "{}", NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    /* NULL json -> treat as NONE; *out cleared. */
    out = (char *)0x1;
    err = hu_imessage_diag_from_poll_status(&alloc, NULL, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(out == NULL);
}

void run_imessage_diag_tests(void) {
    HU_TEST_SUITE("iMessage diag formatter (US-43.5)");
    HU_RUN_TEST(imessage_diag_auth_contains_full_disk_access);
    HU_RUN_TEST(imessage_diag_auth_does_not_mention_syncing);
    HU_RUN_TEST(imessage_diag_busy_contains_syncing);
    HU_RUN_TEST(imessage_diag_busy_does_not_mention_full_disk_access);
    HU_RUN_TEST(imessage_diag_cantopen_contains_chat_db_and_path_hint);
    HU_RUN_TEST(imessage_diag_none_returns_null_output);
    HU_RUN_TEST(imessage_diag_missing_class_field_returns_null);
    HU_RUN_TEST(imessage_diag_other_uses_class_name);
    HU_RUN_TEST(imessage_diag_unrecognized_class_falls_through_to_other);
    HU_RUN_TEST(imessage_error_class_from_name_roundtrip);
    HU_RUN_TEST(imessage_diag_null_inputs_are_safe);
}
#else
void run_imessage_diag_tests(void) {
    (void)0; /* iMessage channel not built */
}
#endif
