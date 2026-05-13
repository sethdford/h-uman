/* PCTT Task 2 — round-trip tests for hu_persona_overlay_t::filler_bank.
 *
 * Covers:
 *   - Save→load round-trip of 5 filler entries preserves order and count.
 *   - Legacy persona JSON without a `filler_bank` key still loads cleanly
 *     (forward-compat: filler_bank=NULL, count=0).
 *   - Explicit empty `filler_bank: []` loads as count=0 without allocating.
 *
 * Implicit ASan coverage: the overlay destructor frees all filler entries
 * plus the array — any leak shows up as a regression in the standard
 * `./build/human_tests` run with the dev preset (ASan enabled).
 */

#ifdef HU_ENABLE_PERSONA

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/persona.h"
#include "test_framework.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#include <unistd.h>
#endif

static void filler_roundtrip_5_fillers_preserves_order(void) {
#if defined(__unix__) || defined(__APPLE__)
    char tmpdir[] = "/tmp/human_persona_filler_test_XXXXXX";
    if (!mkdtemp(tmpdir))
        return; /* skip if temp dir creation fails */

    setenv("HU_PERSONA_DIR", tmpdir, 1);

    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p = {0};
    p.name = hu_strdup(&alloc, "filler_rt");
    p.name_len = p.name ? strlen(p.name) : 0;
    p.identity = hu_strdup(&alloc, "filler test identity");
    if (!p.name || !p.identity) {
        hu_persona_deinit(&alloc, &p);
        unsetenv("HU_PERSONA_DIR");
        rmdir(tmpdir);
        return;
    }

    /* One overlay with 5 fillers in a known order. */
    p.overlays = (hu_persona_overlay_t *)alloc.alloc(alloc.ctx, sizeof(hu_persona_overlay_t));
    if (!p.overlays) {
        hu_persona_deinit(&alloc, &p);
        unsetenv("HU_PERSONA_DIR");
        rmdir(tmpdir);
        return;
    }
    memset(p.overlays, 0, sizeof(hu_persona_overlay_t));
    p.overlays_count = 1;
    p.overlays[0].channel = hu_strdup(&alloc, "imessage");

    const char *fillers[5] = {"a", "b", "c", "d", "e"};
    p.overlays[0].filler_bank = (char **)alloc.alloc(alloc.ctx, 5 * sizeof(char *));
    HU_ASSERT_NOT_NULL(p.overlays[0].filler_bank);
    for (size_t i = 0; i < 5; i++) {
        p.overlays[0].filler_bank[i] = hu_strdup(&alloc, fillers[i]);
        HU_ASSERT_NOT_NULL(p.overlays[0].filler_bank[i]);
    }
    p.overlays[0].filler_bank_count = 5;
    p.overlays[0].filler_bank_cap = 5;

    hu_error_t err = hu_persona_creator_write(&alloc, &p);
    hu_persona_deinit(&alloc, &p);
    if (err != HU_OK) {
        unsetenv("HU_PERSONA_DIR");
        char path[512];
        snprintf(path, sizeof(path), "%s/filler_rt.json", tmpdir);
        unlink(path);
        rmdir(tmpdir);
        HU_ASSERT_EQ(err, HU_OK);
        return;
    }

    hu_persona_t loaded = {0};
    err = hu_persona_load(&alloc, "filler_rt", strlen("filler_rt"), &loaded);
    unsetenv("HU_PERSONA_DIR");
    if (err != HU_OK) {
        char path[512];
        snprintf(path, sizeof(path), "%s/filler_rt.json", tmpdir);
        unlink(path);
        rmdir(tmpdir);
        HU_ASSERT_EQ(err, HU_OK);
        return;
    }

    HU_ASSERT_EQ(loaded.overlays_count, 1);
    HU_ASSERT_STR_EQ(loaded.overlays[0].channel, "imessage");
    HU_ASSERT_EQ(loaded.overlays[0].filler_bank_count, 5);
    HU_ASSERT_NOT_NULL(loaded.overlays[0].filler_bank);
    for (size_t i = 0; i < 5; i++) {
        HU_ASSERT_NOT_NULL(loaded.overlays[0].filler_bank[i]);
        HU_ASSERT_STR_EQ(loaded.overlays[0].filler_bank[i], fillers[i]);
    }

    hu_persona_deinit(&alloc, &loaded);

    char path[512];
    snprintf(path, sizeof(path), "%s/filler_rt.json", tmpdir);
    unlink(path);
    rmdir(tmpdir);
#endif
}

static void load_legacy_persona_without_filler_bank_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* Legacy overlay shape: no filler_bank key. Forward-compat path:
     * loader must leave filler_bank=NULL, count=0, cap=0. */
    const char *json = "{"
                       "  \"version\": 1,"
                       "  \"name\": \"legacy\","
                       "  \"core\": {"
                       "    \"identity\": \"legacy identity\","
                       "    \"traits\": [\"x\"]"
                       "  },"
                       "  \"channel_overlays\": {"
                       "    \"imessage\": {"
                       "      \"formality\": \"casual\","
                       "      \"avg_length\": \"short\","
                       "      \"emoji_usage\": \"minimal\","
                       "      \"style_notes\": [\"drops punctuation\"]"
                       "    }"
                       "  }"
                       "}";

    hu_persona_t p = {0};
    hu_error_t err = hu_persona_load_json(&alloc, json, strlen(json), &p);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(p.overlays_count, 1);
    HU_ASSERT_STR_EQ(p.overlays[0].channel, "imessage");
    HU_ASSERT_EQ(p.overlays[0].filler_bank_count, 0);
    HU_ASSERT_NULL(p.overlays[0].filler_bank);
    HU_ASSERT_EQ(p.overlays[0].filler_bank_cap, 0);

    hu_persona_deinit(&alloc, &p);
}

static void load_overlay_with_empty_filler_array_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{"
                       "  \"version\": 1,"
                       "  \"name\": \"empty_filler\","
                       "  \"core\": {"
                       "    \"identity\": \"x\","
                       "    \"traits\": [\"x\"]"
                       "  },"
                       "  \"channel_overlays\": {"
                       "    \"imessage\": {"
                       "      \"formality\": \"casual\","
                       "      \"avg_length\": \"short\","
                       "      \"emoji_usage\": \"minimal\","
                       "      \"style_notes\": [],"
                       "      \"filler_bank\": []"
                       "    }"
                       "  }"
                       "}";

    hu_persona_t p = {0};
    hu_error_t err = hu_persona_load_json(&alloc, json, strlen(json), &p);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(p.overlays_count, 1);
    HU_ASSERT_EQ(p.overlays[0].filler_bank_count, 0);
    /* Empty array → no allocation; filler_bank should remain NULL. */
    HU_ASSERT_NULL(p.overlays[0].filler_bank);

    hu_persona_deinit(&alloc, &p);
}

static void load_overlay_truncates_filler_bank_above_32(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* 40 entries — parser must silently truncate to the 32 soft cap. */
    char json[4096];
    size_t off = 0;
    int n = snprintf(json + off, sizeof(json) - off,
                     "{"
                     "  \"version\": 1,"
                     "  \"name\": \"cap_test\","
                     "  \"core\": {\"identity\": \"x\", \"traits\": [\"x\"]},"
                     "  \"channel_overlays\": {"
                     "    \"imessage\": {"
                     "      \"formality\": \"casual\","
                     "      \"avg_length\": \"short\","
                     "      \"emoji_usage\": \"minimal\","
                     "      \"style_notes\": [],"
                     "      \"filler_bank\": [");
    if (n < 0 || (size_t)n >= sizeof(json) - off)
        return;
    off += (size_t)n;
    for (int i = 0; i < 40; i++) {
        n = snprintf(json + off, sizeof(json) - off, "%s\"f%d\"", i == 0 ? "" : ",", i);
        if (n < 0 || (size_t)n >= sizeof(json) - off)
            return;
        off += (size_t)n;
    }
    n = snprintf(json + off, sizeof(json) - off, "]}}}");
    if (n < 0 || (size_t)n >= sizeof(json) - off)
        return;
    off += (size_t)n;

    hu_persona_t p = {0};
    hu_error_t err = hu_persona_load_json(&alloc, json, off, &p);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(p.overlays_count, 1);
    HU_ASSERT_EQ(p.overlays[0].filler_bank_count, 32);
    HU_ASSERT_NOT_NULL(p.overlays[0].filler_bank);
    /* First entry preserved; the 33rd-onward silently dropped. */
    HU_ASSERT_STR_EQ(p.overlays[0].filler_bank[0], "f0");
    HU_ASSERT_STR_EQ(p.overlays[0].filler_bank[31], "f31");

    hu_persona_deinit(&alloc, &p);
}

void run_persona_filler_roundtrip_tests(void) {
    HU_TEST_SUITE("PersonaFillerRoundtrip");

    HU_RUN_TEST(filler_roundtrip_5_fillers_preserves_order);
    HU_RUN_TEST(load_legacy_persona_without_filler_bank_succeeds);
    HU_RUN_TEST(load_overlay_with_empty_filler_array_succeeds);
    HU_RUN_TEST(load_overlay_truncates_filler_bank_above_32);
}

#else

void run_persona_filler_roundtrip_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_PERSONA */
