/* Sprint 1 Story C — Tier-1 channel overlay routing tests.
 *
 * Pin the contract that, after the centralization in
 * `hu_starter_persona_json` (include/human/onboard.h), each Tier-1
 * channel overlay routes to the directive variant the design doc
 * specified:
 *
 *   imessage / discord  -> CASUAL_EMOJI
 *   slack               -> FORMAL_TERSE
 *   telegram            -> CASUAL_OR_SHORT
 *
 * AC-C.1 additionally validates that the production starter blob
 * actually parses (no JSON-array-vs-object regression) and exposes
 * non-NULL overlays for all four Tier-1 channels via
 * `hu_persona_find_overlay`.
 *
 * Variant routing is observed via the public
 * `hu_personal_model_directive_telemetry_*` API rather than calling
 * the static `directive_variant_for_overlay` directly — that's the
 * same surface a dashboard or operator would see, so the test
 * proves end-to-end behavior, not just an internal helper return.
 */

#include "test_framework.h"

#include "human/core/allocator.h"
#include "human/memory/personal_model.h"
#include "human/onboard.h"
#include "human/persona.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static void seed_model_with_recent_goal(hu_personal_model_t *m) {
    memset(m, 0, sizeof(*m));
    m->style.sample_count = 1;
    m->style.formality = 0.3f;
    m->style.verbosity = 0.5f;
    m->style.emoji_frequency = 0.2f;
    m->style.lowercase_ratio = 0.85f;
    m->style.abbreviation_ratio = 0.2f;
    m->style.avg_message_length = 60;

    m->goal_count = 1;
    m->goals[0].active = false;
    m->goals[0].progress = 1.0f;
    m->goals[0].last_referenced = 1000;
    snprintf(m->goals[0].description, sizeof(m->goals[0].description), "ship the sprint");
    m->updated_at = 1000 + 86400; /* < 7 days since completion */
}

static void fire_directive(const hu_persona_overlay_t *overlay) {
    hu_personal_model_t m;
    seed_model_with_recent_goal(&m);
    char buf[2048];
    (void)hu_personal_model_build_prompt_with_overlay(&m, overlay, buf, sizeof(buf));
}

static const hu_persona_overlay_t *find_overlay(const hu_persona_t *p, const char *channel) {
    return hu_persona_find_overlay(p, channel, strlen(channel));
}

static void persona_directive_starter_persona_loads_four_tier1_overlays(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t persona;
    memset(&persona, 0, sizeof(persona));

    size_t starter_len = 0;
    const char *starter_json = hu_starter_persona_get(&starter_len);
    hu_error_t err = hu_persona_load_json(&alloc, starter_json, starter_len, &persona);
    HU_ASSERT_EQ(err, HU_OK);

    HU_ASSERT_NOT_NULL(find_overlay(&persona, "imessage"));
    HU_ASSERT_NOT_NULL(find_overlay(&persona, "discord"));
    HU_ASSERT_NOT_NULL(find_overlay(&persona, "slack"));
    HU_ASSERT_NOT_NULL(find_overlay(&persona, "telegram"));
    HU_ASSERT_NOT_NULL(find_overlay(&persona, "cli"));

    hu_persona_deinit(&alloc, &persona);
}

static void persona_directive_starter_persona_has_five_example_banks(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t persona;
    memset(&persona, 0, sizeof(persona));

    size_t starter_len = 0;
    const char *starter_json = hu_starter_persona_get(&starter_len);
    hu_error_t err = hu_persona_load_json(&alloc, starter_json, starter_len, &persona);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(persona.example_banks_count, 5u);

    hu_persona_deinit(&alloc, &persona);
}

static void persona_directive_discord_overlay_fires_casual_emoji(void) {
    hu_persona_overlay_t overlay = {0};
    overlay.formality = "casual";
    overlay.avg_length = "medium";
    overlay.emoji_usage = "high";

    hu_personal_model_directive_telemetry_reset();
    fire_directive(&overlay);

    hu_directive_telemetry_t snap;
    hu_personal_model_directive_telemetry_snapshot(&snap);
    HU_ASSERT_EQ(snap.counts[HU_DIRECTIVE_VARIANT_CASUAL_EMOJI], 1U);
    HU_ASSERT_EQ(snap.counts[HU_DIRECTIVE_VARIANT_NULL_OVERLAY], 0U);
}

static void persona_directive_imessage_overlay_fires_casual_emoji(void) {
    hu_persona_overlay_t overlay = {0};
    overlay.formality = "casual";
    overlay.avg_length = "short";
    overlay.emoji_usage = "moderate";

    hu_personal_model_directive_telemetry_reset();
    fire_directive(&overlay);

    hu_directive_telemetry_t snap;
    hu_personal_model_directive_telemetry_snapshot(&snap);
    HU_ASSERT_EQ(snap.counts[HU_DIRECTIVE_VARIANT_CASUAL_EMOJI], 1U);
    HU_ASSERT_EQ(snap.counts[HU_DIRECTIVE_VARIANT_NULL_OVERLAY], 0U);
}

static void persona_directive_slack_overlay_fires_formal_terse(void) {
    hu_persona_overlay_t overlay = {0};
    overlay.formality = "professional";
    overlay.avg_length = "medium";
    overlay.emoji_usage = "minimal";

    hu_personal_model_directive_telemetry_reset();
    fire_directive(&overlay);

    hu_directive_telemetry_t snap;
    hu_personal_model_directive_telemetry_snapshot(&snap);
    HU_ASSERT_EQ(snap.counts[HU_DIRECTIVE_VARIANT_FORMAL_TERSE], 1U);
    HU_ASSERT_EQ(snap.counts[HU_DIRECTIVE_VARIANT_NULL_OVERLAY], 0U);
}

static void persona_directive_telegram_overlay_fires_casual_or_short(void) {
    hu_persona_overlay_t overlay = {0};
    overlay.formality = "casual";
    overlay.avg_length = "medium";
    overlay.emoji_usage = "low";

    hu_personal_model_directive_telemetry_reset();
    fire_directive(&overlay);

    hu_directive_telemetry_t snap;
    hu_personal_model_directive_telemetry_snapshot(&snap);
    HU_ASSERT_EQ(snap.counts[HU_DIRECTIVE_VARIANT_CASUAL_OR_SHORT], 1U);
    HU_ASSERT_EQ(snap.counts[HU_DIRECTIVE_VARIANT_NULL_OVERLAY], 0U);
}

static void persona_directive_tier1_batch_yields_zero_null_overlay(void) {
    hu_persona_overlay_t imessage = {0};
    imessage.formality = "casual";
    imessage.avg_length = "short";
    imessage.emoji_usage = "moderate";

    hu_persona_overlay_t discord = {0};
    discord.formality = "casual";
    discord.avg_length = "medium";
    discord.emoji_usage = "high";

    hu_persona_overlay_t slack = {0};
    slack.formality = "professional";
    slack.avg_length = "medium";
    slack.emoji_usage = "minimal";

    hu_persona_overlay_t telegram = {0};
    telegram.formality = "casual";
    telegram.avg_length = "medium";
    telegram.emoji_usage = "low";

    hu_personal_model_directive_telemetry_reset();
    fire_directive(&imessage);
    fire_directive(&discord);
    fire_directive(&slack);
    fire_directive(&telegram);

    hu_directive_telemetry_t snap;
    hu_personal_model_directive_telemetry_snapshot(&snap);
    HU_ASSERT_EQ(snap.total, 4U);
    HU_ASSERT_EQ(snap.counts[HU_DIRECTIVE_VARIANT_NULL_OVERLAY], 0U);
    HU_ASSERT_EQ(snap.counts[HU_DIRECTIVE_VARIANT_CASUAL_EMOJI], 2U);
    HU_ASSERT_EQ(snap.counts[HU_DIRECTIVE_VARIANT_FORMAL_TERSE], 1U);
    HU_ASSERT_EQ(snap.counts[HU_DIRECTIVE_VARIANT_CASUAL_OR_SHORT], 1U);
}

/* M4 (2026-05-26) — pin the Tier-2 overlays added to the starter
 * persona JSON (whatsapp / signal / email). New users on these
 * channels should hit a sane default tone immediately instead of
 * falling through to neutral. */
static void persona_directive_starter_persona_loads_tier2_overlays(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t persona;
    memset(&persona, 0, sizeof(persona));

    size_t starter_len = 0;
    const char *starter_json = hu_starter_persona_get(&starter_len);
    HU_ASSERT_EQ(hu_persona_load_json(&alloc, starter_json, starter_len, &persona), HU_OK);

    const hu_persona_overlay_t *whatsapp = find_overlay(&persona, "whatsapp");
    HU_ASSERT_NOT_NULL(whatsapp);
    HU_ASSERT_STR_EQ(whatsapp->formality, "casual");
    HU_ASSERT_STR_EQ(whatsapp->avg_length, "short");

    const hu_persona_overlay_t *signal = find_overlay(&persona, "signal");
    HU_ASSERT_NOT_NULL(signal);
    HU_ASSERT_STR_EQ(signal->formality, "casual");
    HU_ASSERT_STR_EQ(signal->emoji_usage, "low");

    const hu_persona_overlay_t *email = find_overlay(&persona, "email");
    HU_ASSERT_NOT_NULL(email);
    HU_ASSERT_STR_EQ(email->formality, "professional");
    HU_ASSERT_STR_EQ(email->avg_length, "long");
    HU_ASSERT_STR_EQ(email->emoji_usage, "none");

    hu_persona_deinit(&alloc, &persona);
}

/* M6-A (2026-05-26): pin case-insensitive overlay lookup contract.
 *
 * Bug history: prior to commit M6-A, `hu_persona_find_overlay` used
 * `memcmp` for channel-name comparison — case-SENSITIVE — despite
 * both `src/persona/CLAUDE.md` and `.claude/rules/persona.md` stating
 * that lookup MUST be case-insensitive. Every Tier-1 caller passes
 * lowercase channel names (`"slack"`, `"telegram"`, etc.) by
 * convention, so a persona JSON file with `"Slack"` would silently
 * have its entire overlay bypassed.
 *
 * These three contracts pin the case-insensitive behavior: same
 * overlay returned regardless of how the caller capitalizes the
 * channel name. Without the fix, the SLACK test fails; with it, all
 * three pass and `hu_persona_find_overlay` returns the same pointer
 * for any case-equivalent lookup. */

static const hu_persona_overlay_t *
load_starter_and_find(hu_allocator_t *alloc, hu_persona_t *persona, const char *channel) {
    memset(persona, 0, sizeof(*persona));
    size_t starter_len = 0;
    const char *starter_json = hu_starter_persona_get(&starter_len);
    HU_ASSERT_EQ(hu_persona_load_json(alloc, starter_json, starter_len, persona), HU_OK);
    return hu_persona_find_overlay(persona, channel, strlen(channel));
}

static void persona_overlay_lookup_case_insensitive_uppercase_first_letter(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t persona;
    /* Lowercase baseline — the canonical lookup that callers use. */
    const hu_persona_overlay_t *lower = load_starter_and_find(&alloc, &persona, "slack");
    HU_ASSERT_NOT_NULL(lower);
    /* Uppercase first letter — what a hand-edited persona JSON might emit. */
    const hu_persona_overlay_t *upper = hu_persona_find_overlay(&persona, "Slack", 5);
    HU_ASSERT_NOT_NULL(upper);
    /* Same pointer — both look up the same overlay row. */
    HU_ASSERT_EQ((const void *)lower, (const void *)upper);
    hu_persona_deinit(&alloc, &persona);
}

static void persona_overlay_lookup_case_insensitive_all_uppercase(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t persona;
    const hu_persona_overlay_t *lower = load_starter_and_find(&alloc, &persona, "telegram");
    HU_ASSERT_NOT_NULL(lower);
    const hu_persona_overlay_t *upper = hu_persona_find_overlay(&persona, "TELEGRAM", 8);
    HU_ASSERT_NOT_NULL(upper);
    HU_ASSERT_EQ((const void *)lower, (const void *)upper);
    hu_persona_deinit(&alloc, &persona);
}

static void persona_overlay_lookup_case_insensitive_mixed_case(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t persona;
    const hu_persona_overlay_t *lower = load_starter_and_find(&alloc, &persona, "discord");
    HU_ASSERT_NOT_NULL(lower);
    /* "DiScOrD" — the worst-case typo a user could plausibly introduce. */
    const hu_persona_overlay_t *mixed = hu_persona_find_overlay(&persona, "DiScOrD", 7);
    HU_ASSERT_NOT_NULL(mixed);
    HU_ASSERT_EQ((const void *)lower, (const void *)mixed);
    hu_persona_deinit(&alloc, &persona);
}

void run_persona_directive_channels_tests(void) {
    HU_TEST_SUITE("persona_directive_channels");
    HU_RUN_TEST(persona_directive_starter_persona_loads_four_tier1_overlays);
    HU_RUN_TEST(persona_directive_starter_persona_has_five_example_banks);
    HU_RUN_TEST(persona_directive_discord_overlay_fires_casual_emoji);
    HU_RUN_TEST(persona_directive_imessage_overlay_fires_casual_emoji);
    HU_RUN_TEST(persona_directive_slack_overlay_fires_formal_terse);
    HU_RUN_TEST(persona_directive_telegram_overlay_fires_casual_or_short);
    HU_RUN_TEST(persona_directive_tier1_batch_yields_zero_null_overlay);
    HU_RUN_TEST(persona_directive_starter_persona_loads_tier2_overlays);
    /* M6-A: case-insensitive overlay lookup contract */
    HU_RUN_TEST(persona_overlay_lookup_case_insensitive_uppercase_first_letter);
    HU_RUN_TEST(persona_overlay_lookup_case_insensitive_all_uppercase);
    HU_RUN_TEST(persona_overlay_lookup_case_insensitive_mixed_case);
}
