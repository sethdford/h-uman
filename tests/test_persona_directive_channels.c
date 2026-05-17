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

    hu_error_t err = hu_persona_load_json(&alloc, hu_starter_persona_json,
                                          strlen(hu_starter_persona_json), &persona);
    HU_ASSERT_EQ(err, HU_OK);

    HU_ASSERT_NOT_NULL(find_overlay(&persona, "imessage"));
    HU_ASSERT_NOT_NULL(find_overlay(&persona, "discord"));
    HU_ASSERT_NOT_NULL(find_overlay(&persona, "slack"));
    HU_ASSERT_NOT_NULL(find_overlay(&persona, "telegram"));

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

void run_persona_directive_channels_tests(void) {
    HU_TEST_SUITE("persona_directive_channels");
    HU_RUN_TEST(persona_directive_starter_persona_loads_four_tier1_overlays);
    HU_RUN_TEST(persona_directive_discord_overlay_fires_casual_emoji);
    HU_RUN_TEST(persona_directive_imessage_overlay_fires_casual_emoji);
    HU_RUN_TEST(persona_directive_slack_overlay_fires_formal_terse);
    HU_RUN_TEST(persona_directive_telegram_overlay_fires_casual_or_short);
    HU_RUN_TEST(persona_directive_tier1_batch_yields_zero_null_overlay);
}
