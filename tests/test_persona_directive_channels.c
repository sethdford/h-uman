/*
 * tests/test_persona_directive_channels.c — Sprint 1 Story C
 *
 * Pins the Tier-1 channel overlay → directive-variant routing for the
 * starter persona shipped by `human init` and `human onboard`. Replaces
 * the latent 100% `null_overlay` production telemetry signal with a
 * deterministic, in-suite assertion that the four canonical channels
 * each fire the expected acknowledgment-directive variant.
 *
 * Threshold contract — copied verbatim from
 * src/memory/personal_model.c::directive_variant_for_overlay so anyone
 * editing the per-channel overlay values below reads the routing
 * before changing the inputs:
 *
 *   Order   Variant            Condition (first-match wins)
 *   ─────   ────────────────   ───────────────────────────────────────
 *   0       NULL_OVERLAY       overlay == NULL
 *   1       FORMAL_TERSE       formality == "formal" || "professional"
 *   2       CASUAL_EMOJI       (formality == "casual" || "playful") AND
 *                              emoji_usage == "moderate"|"high"|"frequent"
 *   3       CASUAL_OR_SHORT    formality == "casual"|"playful" OR
 *                              avg_length == "short" OR
 *                              atoi(avg_length) in (0,30]
 *   4       ADAPTIVE_EMOJI     emoji_usage == "moderate"|"high"|"frequent"
 *                              (no formality match)
 *   5       DEFAULT            overlay present, no signal matched
 *
 * Notes the test pins:
 *   - Formal trumps emoji license (Slack: professional + minimal).
 *   - "low" is NOT in the emoji_ok set; Telegram falls through to
 *     CASUAL_OR_SHORT, not CASUAL_EMOJI.
 *   - avg_length="short" matches the literal "short" branch — atoi-trap
 *     avoidance is part of why we do NOT use a numeric string here.
 *
 * AC coverage (sprints/sprint-1/stories.md::Story C):
 *   AC-C.1  starter_persona_loads_four_tier1_overlays
 *   AC-C.2  discord_overlay_fires_casual_emoji
 *           imessage_overlay_fires_casual_emoji
 *   AC-C.3  slack_overlay_fires_formal_terse
 *   AC-C.4  telegram_overlay_fires_casual_or_short
 *   AC-C.5  this entire suite under --suite=persona_directive_channels
 *   AC-C.6  tier1_batch_yields_zero_null_overlay
 */

#include "human/core/allocator.h"
#include "human/memory/personal_model.h"
#include "human/onboard.h"
#include "human/persona.h"
#include "test_framework.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ── In-memory Tier-1 overlay literals ─────────────────────────────────────
 *
 * Values must stay synchronized with the JSON shape in
 * src/onboard.c::hu_starter_persona_json. If you edit one, edit both
 * — the AC-C.1 starter-persona-loads test cross-checks the JSON path,
 * but the per-channel routing tests below operate on these literals so
 * a future overlay-field change can be tested in isolation without
 * round-tripping through the parser.
 *
 * `hu_persona_overlay_t` fields are declared `char *` (non-const) but
 * `directive_variant_for_overlay` reads them via `strcmp` — the cast
 * is the same trick used at tests/test_persona.c:1288-1295 and
 * tests/test_personal_model.c:2085-2147. Stack-allocated to keep
 * future callers from mistakenly trying to `hu_persona_deinit` a
 * read-only literal. */

static void build_imessage_overlay(hu_persona_overlay_t *out) {
    memset(out, 0, sizeof(*out));
    out->channel = (char *)"imessage";
    out->formality = (char *)"casual";
    out->avg_length = (char *)"short";
    out->emoji_usage = (char *)"moderate";
}

static void build_discord_overlay(hu_persona_overlay_t *out) {
    memset(out, 0, sizeof(*out));
    out->channel = (char *)"discord";
    out->formality = (char *)"casual";
    out->avg_length = (char *)"short";
    out->emoji_usage = (char *)"high";
}

static void build_slack_overlay(hu_persona_overlay_t *out) {
    memset(out, 0, sizeof(*out));
    out->channel = (char *)"slack";
    out->formality = (char *)"professional";
    out->avg_length = (char *)"short";
    out->emoji_usage = (char *)"minimal";
}

static void build_telegram_overlay(hu_persona_overlay_t *out) {
    memset(out, 0, sizeof(*out));
    out->channel = (char *)"telegram";
    out->formality = (char *)"casual";
    out->avg_length = (char *)"short";
    out->emoji_usage = (char *)"low";
}

/* Fire the directive once for the supplied overlay. The directive
 * counter is incremented as a side effect of building a prompt with
 * an overlay (acknowledgment_directive_for_overlay → HU_DIRECTIVE_INC).
 *
 * The acknowledgment directive is gated on `Recently completed: …`
 * being present in the prompt — i.e. the model must carry at least
 * one recently-completed goal or the directive line is suppressed
 * and the counter never moves. We seed a single inactive goal with
 * a `last_referenced` close to the model's `updated_at` so
 * `hu_personal_goal_is_recently_completed` returns true and the
 * directive fires. Mirrors `overlay_directive_seed_model` in
 * tests/test_personal_model.c:2059. */
static void fire_directive(const hu_persona_overlay_t *overlay) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    snprintf(m.goals[0].description, sizeof(m.goals[0].description), "ship the new feature");
    m.goals[0].active = false;
    m.goals[0].last_referenced = 1000;
    m.goal_count = 1;
    m.updated_at = 1000 + 86400;
    char buf[2048];
    (void)hu_personal_model_build_prompt_with_overlay(&m, overlay, buf, sizeof(buf));
}

/* ── AC-C.1: starter persona loads, all four Tier-1 channels reachable ──── */

static void persona_directive_starter_persona_loads_four_tier1_overlays(void) {
    /* The blob in src/onboard.c is the production starter persona —
     * if this fails the user-facing `human init` path is broken, not
     * just the test harness. */
    hu_allocator_t alloc = hu_system_allocator();

    /* Validator must accept the shipped blob — guards against the
     * malformed-array regression that produced 100% null_overlay
     * production telemetry. */
    char *err_msg = NULL;
    size_t err_len = 0;
    hu_error_t verr = hu_persona_validate_json(&alloc, hu_starter_persona_json,
                                               strlen(hu_starter_persona_json), &err_msg, &err_len);
    HU_ASSERT_EQ(verr, HU_OK);
    if (err_msg)
        alloc.free(alloc.ctx, err_msg, err_len + 1);

    hu_persona_t p = {0};
    hu_error_t err =
        hu_persona_load_json(&alloc, hu_starter_persona_json, strlen(hu_starter_persona_json), &p);
    HU_ASSERT_EQ(err, HU_OK);

    /* All four Tier-1 channels must be reachable via the public
     * lookup path — this is the assertion the production handler
     * exercises every turn. */
    HU_ASSERT_NOT_NULL(hu_persona_find_overlay(&p, "imessage", strlen("imessage")));
    HU_ASSERT_NOT_NULL(hu_persona_find_overlay(&p, "discord", strlen("discord")));
    HU_ASSERT_NOT_NULL(hu_persona_find_overlay(&p, "slack", strlen("slack")));
    HU_ASSERT_NOT_NULL(hu_persona_find_overlay(&p, "telegram", strlen("telegram")));

    /* And the count itself must be at least four (cli + the four
     * Tier-1 channels = 5 in the canonical blob). */
    HU_ASSERT_TRUE(p.overlays_count >= 4);

    hu_persona_deinit(&alloc, &p);
}

/* ── AC-C.2: Discord overlay fires CASUAL_EMOJI ─────────────────────────── */

static void persona_directive_discord_overlay_fires_casual_emoji(void) {
    hu_personal_model_directive_telemetry_reset();
    hu_persona_overlay_t overlay;
    build_discord_overlay(&overlay);
    fire_directive(&overlay);

    hu_directive_telemetry_t s;
    hu_personal_model_directive_telemetry_snapshot(&s);
    HU_ASSERT_EQ((unsigned long long)s.counts[HU_DIRECTIVE_VARIANT_CASUAL_EMOJI], 1ULL);
    HU_ASSERT_EQ((unsigned long long)s.counts[HU_DIRECTIVE_VARIANT_NULL_OVERLAY], 0ULL);
    HU_ASSERT_EQ((unsigned long long)s.counts[HU_DIRECTIVE_VARIANT_FORMAL_TERSE], 0ULL);
    HU_ASSERT_EQ((unsigned long long)s.total, 1ULL);
}

/* ── AC-C.2: iMessage overlay fires CASUAL_EMOJI ────────────────────────── */

static void persona_directive_imessage_overlay_fires_casual_emoji(void) {
    hu_personal_model_directive_telemetry_reset();
    hu_persona_overlay_t overlay;
    build_imessage_overlay(&overlay);
    fire_directive(&overlay);

    hu_directive_telemetry_t s;
    hu_personal_model_directive_telemetry_snapshot(&s);
    HU_ASSERT_EQ((unsigned long long)s.counts[HU_DIRECTIVE_VARIANT_CASUAL_EMOJI], 1ULL);
    HU_ASSERT_EQ((unsigned long long)s.counts[HU_DIRECTIVE_VARIANT_NULL_OVERLAY], 0ULL);
    HU_ASSERT_EQ((unsigned long long)s.counts[HU_DIRECTIVE_VARIANT_FORMAL_TERSE], 0ULL);
    HU_ASSERT_EQ((unsigned long long)s.total, 1ULL);
}

/* ── AC-C.3: Slack overlay fires FORMAL_TERSE ───────────────────────────── */

static void persona_directive_slack_overlay_fires_formal_terse(void) {
    hu_personal_model_directive_telemetry_reset();
    hu_persona_overlay_t overlay;
    build_slack_overlay(&overlay);
    fire_directive(&overlay);

    hu_directive_telemetry_t s;
    hu_personal_model_directive_telemetry_snapshot(&s);
    /* "professional" formality trumps emoji_usage — even if a
     * future tweak set Slack to high-emoji, the formal branch
     * still fires first. AC-C.3 explicitly accepts FORMAL_TERSE
     * OR ADAPTIVE_EMOJI; we pin the preferred FORMAL_TERSE. */
    HU_ASSERT_EQ((unsigned long long)s.counts[HU_DIRECTIVE_VARIANT_FORMAL_TERSE], 1ULL);
    HU_ASSERT_EQ((unsigned long long)s.counts[HU_DIRECTIVE_VARIANT_NULL_OVERLAY], 0ULL);
    HU_ASSERT_EQ((unsigned long long)s.counts[HU_DIRECTIVE_VARIANT_CASUAL_EMOJI], 0ULL);
    HU_ASSERT_EQ((unsigned long long)s.counts[HU_DIRECTIVE_VARIANT_ADAPTIVE_EMOJI], 0ULL);
    HU_ASSERT_EQ((unsigned long long)s.total, 1ULL);
}

/* ── AC-C.4: Telegram overlay fires CASUAL_OR_SHORT ─────────────────────── */

static void persona_directive_telegram_overlay_fires_casual_or_short(void) {
    hu_personal_model_directive_telemetry_reset();
    hu_persona_overlay_t overlay;
    build_telegram_overlay(&overlay);
    fire_directive(&overlay);

    hu_directive_telemetry_t s;
    hu_personal_model_directive_telemetry_snapshot(&s);
    /* "low" is NOT in the emoji_ok set ({moderate, high, frequent})
     * so the casual+emoji branch does NOT fire; we land in the
     * casual-or-short bucket. AC-C.4 accepts either CASUAL_OR_SHORT
     * or CASUAL_EMOJI; we pin the deliberately-chosen CASUAL_OR_SHORT
     * to keep Telegram's directive distinct from iMessage/Discord. */
    HU_ASSERT_EQ((unsigned long long)s.counts[HU_DIRECTIVE_VARIANT_CASUAL_OR_SHORT], 1ULL);
    HU_ASSERT_EQ((unsigned long long)s.counts[HU_DIRECTIVE_VARIANT_NULL_OVERLAY], 0ULL);
    HU_ASSERT_EQ((unsigned long long)s.counts[HU_DIRECTIVE_VARIANT_CASUAL_EMOJI], 0ULL);
    HU_ASSERT_EQ((unsigned long long)s.total, 1ULL);
}

/* ── AC-C.6: batch all four Tier-1 channels, expect zero null_overlay ──── */

static void persona_directive_tier1_batch_yields_zero_null_overlay(void) {
    hu_personal_model_directive_telemetry_reset();

    hu_persona_overlay_t overlay;
    build_imessage_overlay(&overlay);
    fire_directive(&overlay);
    build_discord_overlay(&overlay);
    fire_directive(&overlay);
    build_slack_overlay(&overlay);
    fire_directive(&overlay);
    build_telegram_overlay(&overlay);
    fire_directive(&overlay);

    hu_directive_telemetry_t s;
    hu_personal_model_directive_telemetry_snapshot(&s);

    /* The headline AC-C.6 assertion: zero null_overlay across the
     * Tier-1 batch. If this ever flips back to non-zero, the
     * starter persona has regressed to the broken array shape. */
    HU_ASSERT_EQ((unsigned long long)s.counts[HU_DIRECTIVE_VARIANT_NULL_OVERLAY], 0ULL);

    /* Per-bucket distribution from the design — pins which branch
     * each channel lands in so a silent threshold change in
     * directive_variant_for_overlay surfaces here. */
    HU_ASSERT_EQ((unsigned long long)s.counts[HU_DIRECTIVE_VARIANT_CASUAL_EMOJI], 2ULL);
    HU_ASSERT_EQ((unsigned long long)s.counts[HU_DIRECTIVE_VARIANT_FORMAL_TERSE], 1ULL);
    HU_ASSERT_EQ((unsigned long long)s.counts[HU_DIRECTIVE_VARIANT_CASUAL_OR_SHORT], 1ULL);

    /* These two MUST stay zero for the chosen Tier-1 inputs — a
     * non-zero count here means a channel collapsed to the generic
     * fallback or the emoji-without-formality branch, which is the
     * regression the populator is supposed to prevent. */
    HU_ASSERT_EQ((unsigned long long)s.counts[HU_DIRECTIVE_VARIANT_DEFAULT], 0ULL);
    HU_ASSERT_EQ((unsigned long long)s.counts[HU_DIRECTIVE_VARIANT_ADAPTIVE_EMOJI], 0ULL);

    /* Total is the loud guard: any stray fire from elsewhere in
     * the same process surfaces as a total mismatch. */
    HU_ASSERT_EQ((unsigned long long)s.total, 4ULL);
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
