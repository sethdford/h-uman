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

/* Sprint 2b Story A' — starter persona ships with example banks for the
 * four Tier-1 channels so a fresh user (no `personal_model.bin` yet)
 * has concrete tone/length anchors instead of a content-free prompt.
 *
 * Pin: each Tier-1 channel has a bank, each bank has at least one
 * complete example (non-empty context + incoming + response). The
 * persona prompt builder consumes these via
 * `hu_persona_select_examples`; if a bank silently drops out at JSON-
 * parse time (e.g. quote-escaping regression in the C string literal),
 * the prompt builder falls back to a generic shape and Tier-1 voice
 * regresses. This test guards against that path. */
static const hu_persona_example_bank_t *find_bank(const hu_persona_t *p, const char *channel) {
    if (!p || !p->example_banks)
        return NULL;
    size_t channel_len = strlen(channel);
    for (size_t i = 0; i < p->example_banks_count; i++) {
        const hu_persona_example_bank_t *bank = &p->example_banks[i];
        if (!bank->channel)
            continue;
        if (strlen(bank->channel) == channel_len && memcmp(bank->channel, channel, channel_len) == 0)
            return bank;
    }
    return NULL;
}

static void assert_complete_example(const hu_persona_example_t *ex) {
    HU_ASSERT_NOT_NULL(ex->context);
    HU_ASSERT_NOT_NULL(ex->incoming);
    HU_ASSERT_NOT_NULL(ex->response);
    HU_ASSERT(strlen(ex->context) > 0);
    HU_ASSERT(strlen(ex->incoming) > 0);
    HU_ASSERT(strlen(ex->response) > 0);
}

static void persona_directive_starter_persona_ships_tier1_example_banks(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t persona;
    memset(&persona, 0, sizeof(persona));

    hu_error_t err = hu_persona_load_json(&alloc, hu_starter_persona_json,
                                          strlen(hu_starter_persona_json), &persona);
    HU_ASSERT_EQ(err, HU_OK);

    HU_ASSERT(persona.example_banks_count >= 4);

    static const char *const tier1[] = {"imessage", "telegram", "discord", "slack"};
    for (size_t i = 0; i < sizeof(tier1) / sizeof(tier1[0]); i++) {
        const hu_persona_example_bank_t *bank = find_bank(&persona, tier1[i]);
        HU_ASSERT_NOT_NULL(bank);
        HU_ASSERT(bank->examples_count >= 1);
        for (size_t e = 0; e < bank->examples_count; e++)
            assert_complete_example(&bank->examples[e]);
    }

    hu_persona_deinit(&alloc, &persona);
}

/* Pin overlay ↔ example-bank coherence: every Tier-1 channel that
 * exposes an overlay also exposes a bank, and vice versa. A future
 * editor adding a channel to one block but not the other would
 * desynchronize the prompt builder (overlay routes to a directive
 * variant but examples are missing, or examples are present but the
 * overlay is null and falls into HU_DIRECTIVE_VARIANT_NULL_OVERLAY). */
static void persona_directive_tier1_overlay_bank_coherence(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t persona;
    memset(&persona, 0, sizeof(persona));

    hu_error_t err = hu_persona_load_json(&alloc, hu_starter_persona_json,
                                          strlen(hu_starter_persona_json), &persona);
    HU_ASSERT_EQ(err, HU_OK);

    static const char *const tier1[] = {"imessage", "telegram", "discord", "slack"};
    for (size_t i = 0; i < sizeof(tier1) / sizeof(tier1[0]); i++) {
        const hu_persona_overlay_t *overlay = find_overlay(&persona, tier1[i]);
        const hu_persona_example_bank_t *bank = find_bank(&persona, tier1[i]);
        HU_ASSERT_NOT_NULL(overlay);
        HU_ASSERT_NOT_NULL(bank);
    }

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
    HU_RUN_TEST(persona_directive_starter_persona_ships_tier1_example_banks);
    HU_RUN_TEST(persona_directive_tier1_overlay_bank_coherence);
}
