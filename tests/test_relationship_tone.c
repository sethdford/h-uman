/* tests/test_relationship_tone.c
 *
 * Truth table for hu_persona_relationship_tone_note — the pure predicate
 * extracted from agent_turn.c's relationship tone-note chain. Pins:
 *   - legacy stage/intimate vocabulary always honored (both gate states)
 *   - the warmth vocabulary {high/warm/close, moderate, low} fires ONLY
 *     when enabled, and legacy vocabulary takes precedence (strict superset)
 *   - low/cold warmth yields NULL (keep the default register)
 *   - "lukewarm" must NOT read as "warm" (substring-classifier-pitfalls)
 *
 * 2026-07-11.
 */

#include "human/persona/relationship_tone.h"
#include "test_framework.h"
#include <string.h>

static hu_contact_profile_t cp_make(const char *stage, const char *warmth) {
    hu_contact_profile_t cp;
    memset(&cp, 0, sizeof(cp));
    cp.relationship_stage = (char *)stage;
    cp.warmth_level = (char *)warmth;
    return cp;
}

static void test_tone_null_profile_returns_null(void) {
    HU_ASSERT_TRUE(hu_persona_relationship_tone_note(NULL, false) == NULL);
    HU_ASSERT_TRUE(hu_persona_relationship_tone_note(NULL, true) == NULL);
}

static void test_tone_legacy_stage_fires_regardless_of_gate(void) {
    hu_contact_profile_t cp = cp_make("deep", NULL);
    const char *off = hu_persona_relationship_tone_note(&cp, false);
    const char *on = hu_persona_relationship_tone_note(&cp, true);
    HU_ASSERT_NOT_NULL(off);
    HU_ASSERT_NOT_NULL(on);
    HU_ASSERT_TRUE(strstr(off, "deep") != NULL);
    /* legacy precedence: gate must not change the note for stage contacts */
    HU_ASSERT_STR_EQ(off, on);
}

static void test_tone_legacy_intimate_fires_regardless_of_gate(void) {
    hu_contact_profile_t cp = cp_make(NULL, "intimate");
    const char *off = hu_persona_relationship_tone_note(&cp, false);
    HU_ASSERT_NOT_NULL(off);
    HU_ASSERT_TRUE(strstr(off, "intimate") != NULL);
}

static void test_tone_warmth_high_fires_only_when_enabled(void) {
    /* the real-persona vocabulary that motivated the fix: disabled → NULL
     * (the pre-fix production behavior), enabled → warm note */
    hu_contact_profile_t cp = cp_make(NULL, "high");
    HU_ASSERT_TRUE(hu_persona_relationship_tone_note(&cp, false) == NULL);
    const char *note = hu_persona_relationship_tone_note(&cp, true);
    HU_ASSERT_NOT_NULL(note);
    HU_ASSERT_TRUE(strstr(note, "warm") != NULL);
}

static void test_tone_warmth_moderate_maps_to_friendly(void) {
    hu_contact_profile_t cp = cp_make(NULL, "moderate");
    const char *note = hu_persona_relationship_tone_note(&cp, true);
    HU_ASSERT_NOT_NULL(note);
    HU_ASSERT_TRUE(strstr(note, "friendly") != NULL);
}

static void test_tone_warmth_low_yields_null(void) {
    hu_contact_profile_t cp = cp_make(NULL, "low");
    HU_ASSERT_TRUE(hu_persona_relationship_tone_note(&cp, true) == NULL);
}

static void test_tone_lukewarm_must_not_read_as_warm(void) {
    /* word-boundary contract: "lukewarm" contains "warm" as a substring but
     * has the opposite intent — must NOT produce the warm note */
    hu_contact_profile_t cp = cp_make(NULL, "lukewarm");
    HU_ASSERT_TRUE(hu_persona_relationship_tone_note(&cp, true) == NULL);
}

static void test_tone_stage_precedes_warmth_vocab(void) {
    /* both set: legacy stage wins even with the gate on */
    hu_contact_profile_t cp = cp_make("trusted", "high");
    const char *note = hu_persona_relationship_tone_note(&cp, true);
    HU_ASSERT_NOT_NULL(note);
    HU_ASSERT_TRUE(strstr(note, "trusted") != NULL);
}

void run_relationship_tone_tests(void) {
    HU_TEST_SUITE("persona relationship tone note");
    HU_RUN_TEST(test_tone_null_profile_returns_null);
    HU_RUN_TEST(test_tone_legacy_stage_fires_regardless_of_gate);
    HU_RUN_TEST(test_tone_legacy_intimate_fires_regardless_of_gate);
    HU_RUN_TEST(test_tone_warmth_high_fires_only_when_enabled);
    HU_RUN_TEST(test_tone_warmth_moderate_maps_to_friendly);
    HU_RUN_TEST(test_tone_warmth_low_yields_null);
    HU_RUN_TEST(test_tone_lukewarm_must_not_read_as_warm);
    HU_RUN_TEST(test_tone_stage_precedes_warmth_vocab);
}
