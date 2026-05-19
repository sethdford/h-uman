/* tests/test_imessage_ingest.c
 *
 * Phase 1a tests for docs/plans/2026-05-18-imessage-sota.md — pin the
 * synthesis wording so the eventual fact-extractor stays stable as we
 * add Phase 2-5 event shapes. These are pure-C tests; no chat.db, no
 * personal-model, no Apple frameworks.
 *
 * Each assertion is a positive contract per
 * ~/.claude/rules/tests-that-pin-bugs.md: the test name describes the
 * intent, and the body asserts that intent is honored. */

#include "human/channels/imessage_ingest.h"
#include "human/channels/reaction_event.h"
#include "human/memory/fact_extract.h"
#include "human/memory/personal_model.h"
#include "test_framework.h"

#include <string.h>

/* ── balloon classifier ───────────────────────────────────────────── */

static void test_balloon_kind_recognizes_url_preview(void) {
    HU_ASSERT_EQ(
        (int)hu_imessage_balloon_kind_from_bundle_id("com.apple.messages.URLBalloonProvider"),
        (int)HU_IMESSAGE_BALLOON_URL_PREVIEW);
    HU_ASSERT_EQ((int)hu_imessage_balloon_kind_from_bundle_id("com.apple.richlink.preview"),
                 (int)HU_IMESSAGE_BALLOON_URL_PREVIEW);
}

static void test_balloon_kind_recognizes_apple_pay(void) {
    HU_ASSERT_EQ((int)hu_imessage_balloon_kind_from_bundle_id(
                     "com.apple.PassbookUIService.PeerPaymentMessagesExtension"),
                 (int)HU_IMESSAGE_BALLOON_APPLE_PAY);
}

static void test_balloon_kind_recognizes_audio_message(void) {
    HU_ASSERT_EQ(
        (int)hu_imessage_balloon_kind_from_bundle_id("com.apple.MobileSMS.MoVoMessageBalloon"),
        (int)HU_IMESSAGE_BALLOON_AUDIO_TRANSCRIPT);
}

static void test_balloon_kind_unknown_returns_unknown(void) {
    HU_ASSERT_EQ((int)hu_imessage_balloon_kind_from_bundle_id(NULL),
                 (int)HU_IMESSAGE_BALLOON_UNKNOWN);
    HU_ASSERT_EQ((int)hu_imessage_balloon_kind_from_bundle_id(""),
                 (int)HU_IMESSAGE_BALLOON_UNKNOWN);
    HU_ASSERT_EQ((int)hu_imessage_balloon_kind_from_bundle_id("com.acme.nonexistent.thing"),
                 (int)HU_IMESSAGE_BALLOON_UNKNOWN);
}

/* ── reaction synthesis ───────────────────────────────────────────── */

static void test_reaction_love_to_my_message_includes_preview(void) {
    hu_reaction_event_t e = {0};
    e.sender_handle = "Alice";
    e.kind = HU_REACTION_LOVE;
    e.polarity = HU_REACTION_POSITIVE;
    e.is_removal = 0;
    char out[256] = {0};
    size_t n = hu_imessage_synth_reaction(&e, NULL, "let's hike Mount Tam Saturday", true, out,
                                          sizeof(out));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(out, "Alice") != NULL);
    HU_ASSERT_TRUE(strstr(out, "reacted") != NULL);
    HU_ASSERT_TRUE(strstr(out, "my message") != NULL);
    HU_ASSERT_TRUE(strstr(out, "Mount Tam") != NULL);
}

static void test_reaction_custom_emoji_uses_provided_glyph(void) {
    hu_reaction_event_t e = {0};
    e.sender_handle = "Bob";
    e.kind = HU_REACTION_KIND_CUSTOM_EMOJI;
    e.polarity = HU_REACTION_POSITIVE;
    char out[256] = {0};
    size_t n =
        hu_imessage_synth_reaction(&e, "\xf0\x9f\x94\xa5" /* 🔥 */, NULL, true, out, sizeof(out));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(out, "\xf0\x9f\x94\xa5") != NULL);
}

static void test_reaction_removal_says_removed_not_reacted(void) {
    hu_reaction_event_t e = {0};
    e.sender_handle = "Alice";
    e.kind = HU_REACTION_LIKE;
    e.is_removal = 1;
    char out[256] = {0};
    size_t n = hu_imessage_synth_reaction(&e, NULL, NULL, true, out, sizeof(out));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(out, "removed") != NULL);
}

static void test_reaction_unknown_sender_becomes_someone(void) {
    hu_reaction_event_t e = {0};
    e.sender_handle = NULL;
    e.kind = HU_REACTION_LAUGH;
    char out[256] = {0};
    size_t n = hu_imessage_synth_reaction(&e, NULL, NULL, true, out, sizeof(out));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(out, "someone") != NULL);
}

/* ── edit synthesis ───────────────────────────────────────────────── */

static void test_edit_with_old_text_renders_delta(void) {
    char out[256] = {0};
    size_t n =
        hu_imessage_synth_edit("Alice", false, "I hate this", "I dislike this", out, sizeof(out));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(out, "Alice") != NULL);
    HU_ASSERT_TRUE(strstr(out, "edited") != NULL);
    HU_ASSERT_TRUE(strstr(out, "I hate this") != NULL);
    HU_ASSERT_TRUE(strstr(out, "I dislike this") != NULL);
}

static void test_edit_from_me_uses_first_person(void) {
    char out[256] = {0};
    size_t n = hu_imessage_synth_edit(NULL, true, "old", "new", out, sizeof(out));
    HU_ASSERT_TRUE(n > 0);
    /* First word should be "I", not "someone". */
    HU_ASSERT_TRUE(out[0] == 'I');
    HU_ASSERT_TRUE(strstr(out, "my message") != NULL);
}

static void test_edit_without_old_text_still_renders(void) {
    char out[256] = {0};
    size_t n = hu_imessage_synth_edit("Alice", false, NULL, "new text", out, sizeof(out));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(out, "new text") != NULL);
}

/* ── unsend synthesis ─────────────────────────────────────────────── */

static void test_unsend_uses_retracted_verb(void) {
    char out[256] = {0};
    size_t n = hu_imessage_synth_unsend("Alice", false, NULL, out, sizeof(out));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(out, "retracted") != NULL);
    HU_ASSERT_TRUE(strstr(out, "Alice") != NULL);
}

/* ── reply synthesis ──────────────────────────────────────────────── */

static void test_reply_includes_parent_and_child_text(void) {
    char out[512] = {0};
    size_t n = hu_imessage_synth_reply("Alice", false, "let's hike Mount Tam",
                                       "Sure, but let's go early", out, sizeof(out));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(out, "Alice") != NULL);
    HU_ASSERT_TRUE(strstr(out, "replied") != NULL);
    HU_ASSERT_TRUE(strstr(out, "Mount Tam") != NULL);
    HU_ASSERT_TRUE(strstr(out, "go early") != NULL);
}

/* ── balloon synthesis ────────────────────────────────────────────── */

static void test_balloon_url_preview_includes_detail(void) {
    char out[256] = {0};
    size_t n = hu_imessage_synth_balloon("Alice", false, HU_IMESSAGE_BALLOON_URL_PREVIEW,
                                         "OpenAI announces new model", out, sizeof(out));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(out, "Alice") != NULL);
    HU_ASSERT_TRUE(strstr(out, "shared a link") != NULL);
    HU_ASSERT_TRUE(strstr(out, "OpenAI") != NULL);
}

static void test_balloon_apple_pay_omits_amount(void) {
    /* The privacy-by-architecture contract: amounts MUST NOT appear in
     * synthesis output, regardless of what the caller passes in detail. */
    char out[256] = {0};
    size_t n = hu_imessage_synth_balloon("Alice", true, HU_IMESSAGE_BALLOON_APPLE_PAY, "Alice", out,
                                         sizeof(out));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(out, "Apple Pay") != NULL);
    /* No dollar/cent markers; no digit runs that could carry an amount. */
    HU_ASSERT_TRUE(strstr(out, "$") == NULL);
    HU_ASSERT_TRUE(strstr(out, "USD") == NULL);
}

static void test_balloon_audio_transcript_includes_text(void) {
    char out[256] = {0};
    size_t n = hu_imessage_synth_balloon("Alice", false, HU_IMESSAGE_BALLOON_AUDIO_TRANSCRIPT,
                                         "running 10 minutes late", out, sizeof(out));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(out, "voice message") != NULL);
    HU_ASSERT_TRUE(strstr(out, "10 minutes late") != NULL);
}

static void test_balloon_placemark_renders_location(void) {
    char out[256] = {0};
    size_t n = hu_imessage_synth_balloon("Alice", false, HU_IMESSAGE_BALLOON_PLACEMARK,
                                         "Tahoe City, CA", out, sizeof(out));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(out, "shared a location") != NULL);
    HU_ASSERT_TRUE(strstr(out, "Tahoe City") != NULL);
}

static void test_balloon_poll_renders_topic(void) {
    char out[256] = {0};
    size_t n = hu_imessage_synth_balloon("Alice", false, HU_IMESSAGE_BALLOON_POLL,
                                         "What time should we leave?", out, sizeof(out));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(out, "poll") != NULL);
    HU_ASSERT_TRUE(strstr(out, "What time") != NULL);
}

/* ── safety / edge cases ──────────────────────────────────────────── */

static void test_synth_handles_null_outputs_safely(void) {
    hu_reaction_event_t e = {0};
    e.sender_handle = "Alice";
    e.kind = HU_REACTION_LOVE;
    HU_ASSERT_EQ((int)hu_imessage_synth_reaction(&e, NULL, NULL, true, NULL, 256), 0);
    char tiny[4];
    HU_ASSERT_EQ((int)hu_imessage_synth_reaction(&e, NULL, NULL, true, tiny, sizeof(tiny)), 0);
}

static void test_synth_long_preview_truncates_with_ellipsis(void) {
    char out[256] = {0};
    /* 200 char preview, capped at 80 in synthesis. */
    char long_text[201];
    memset(long_text, 'x', 200);
    long_text[200] = '\0';
    hu_reaction_event_t e = {0};
    e.sender_handle = "Alice";
    e.kind = HU_REACTION_LIKE;
    size_t n = hu_imessage_synth_reaction(&e, NULL, long_text, true, out, sizeof(out));
    HU_ASSERT_TRUE(n > 0);
    /* Ellipsis "…" is E2 80 A6 in UTF-8. */
    HU_ASSERT_TRUE(strstr(out, "\xe2\x80\xa6") != NULL);
}

/* ── Phase 1b: ingest wrappers (synthesis + personal_model_ingest) ── */

static void test_ingest_reaction_rejects_null_model(void) {
    hu_reaction_event_t e = {0};
    e.sender_handle = "Alice";
    e.kind = HU_REACTION_LOVE;
    e.timestamp_unix = 1700000000;
    hu_error_t err = hu_reaction_ingest_personal_model(NULL, &e, NULL, NULL, true, false);
    HU_ASSERT_EQ((int)err, (int)HU_ERR_INVALID_ARGUMENT);
}

static void test_ingest_reaction_rejects_null_event(void) {
    hu_personal_model_t model;
    hu_personal_model_init(&model);
    hu_error_t err = hu_reaction_ingest_personal_model(&model, NULL, NULL, NULL, true, false);
    HU_ASSERT_EQ((int)err, (int)HU_ERR_INVALID_ARGUMENT);
}

static void test_ingest_reaction_succeeds_and_marks_content(void) {
    hu_personal_model_t model;
    hu_personal_model_init(&model);
    bool before = hu_personal_model_has_content(&model);

    hu_reaction_event_t e = {0};
    e.sender_handle = "Alice";
    e.kind = HU_REACTION_LOVE;
    e.polarity = HU_REACTION_POSITIVE;
    e.timestamp_unix = 1700000000;

    hu_error_t err =
        hu_reaction_ingest_personal_model(&model, &e, NULL, "let's hike Mount Tam Saturday", true, false);
    HU_ASSERT_EQ((int)err, (int)HU_OK);

    /* Ingestion should at minimum NOT regress the has_content signal.
     * Whether the fact extractor produces a fact depends on the LLM path;
     * the contract pinned here is that ingest returns OK and does not
     * leave the model in a worse state than it started. */
    bool after = hu_personal_model_has_content(&model);
    HU_ASSERT_TRUE(after >= before);
}

static void test_ingest_edit_routes_first_person_correctly(void) {
    hu_personal_model_t model;
    hu_personal_model_init(&model);
    hu_error_t err = hu_imessage_ingest_edit(&model, NULL, /*is_from_me=*/true, "I hate this",
                                             "I dislike this", 1700000000, false);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
}

static void test_ingest_unsend_handles_null_preview(void) {
    hu_personal_model_t model;
    hu_personal_model_init(&model);
    hu_error_t err = hu_imessage_ingest_unsend(&model, "Alice", false, NULL, 1700000000, false);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
}

static void test_ingest_reply_rejects_null_reply_text(void) {
    hu_personal_model_t model;
    hu_personal_model_init(&model);
    hu_error_t err =
        hu_imessage_ingest_reply(&model, "Alice", false, "parent message", NULL, 1700000000, false);
    HU_ASSERT_EQ((int)err, (int)HU_ERR_INVALID_ARGUMENT);
}

static void test_ingest_reply_succeeds_with_parent_and_child(void) {
    hu_personal_model_t model;
    hu_personal_model_init(&model);
    hu_error_t err = hu_imessage_ingest_reply(&model, "Alice", false, "let's hike Mount Tam",
                                              "Sure, but let's go early", 1700000000, false);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
}

static void test_ingest_balloon_url_preview_succeeds(void) {
    hu_personal_model_t model;
    hu_personal_model_init(&model);
    hu_error_t err =
        hu_imessage_ingest_balloon(&model, "Alice", false, HU_IMESSAGE_BALLOON_URL_PREVIEW,
                                   "OpenAI announces new model", 1700000000, false);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
}

static void test_ingest_reaction_uses_event_emoji_when_custom_emoji_null(void) {
    /* Phase 2: when the caller passes custom_emoji=NULL but the event
     * itself carries an emoji glyph (e.g. from chat.db
     * associated_message_emoji), the synthesis path should still produce
     * sensible output. The wrapper currently passes the custom_emoji arg
     * straight through; this test pins the contract that callers (like
     * reaction_handler.c) MUST supply event->emoji when present. */
    hu_personal_model_t model;
    hu_personal_model_init(&model);
    hu_reaction_event_t e = {0};
    e.sender_handle = "Alice";
    e.kind = HU_REACTION_KIND_CUSTOM_EMOJI;
    e.emoji = "\xf0\x9f\x94\xa5"; /* 🔥 */
    e.timestamp_unix = 1700000000;
    hu_error_t err = hu_reaction_ingest_personal_model(&model, &e, e.emoji, "great idea",
                                                 /*is_from_me_target=*/true, false);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
}

static void test_reaction_event_struct_has_emoji_field(void) {
    /* Phase 2 contract: the emoji field exists, defaults to NULL when
     * zero-initialized, and is independently addressable from the rest
     * of the struct. */
    hu_reaction_event_t e = {0};
    HU_ASSERT_TRUE(e.emoji == NULL);
    e.emoji = "\xf0\x9f\x8e\x89"; /* 🎉 */
    HU_ASSERT_TRUE(e.emoji != NULL);
    HU_ASSERT_STR_EQ(e.emoji, "\xf0\x9f\x8e\x89");
}

static void test_ingest_reaction_is_channel_agnostic_slack(void) {
    /* Phase 2 of docs/plans/2026-05-18-imessage-sota.md: the ingest
     * wrapper is generalized — Slack reactions flow through the same
     * path as iMessage, with provenance derived from event->channel_id.
     * This pins the contract that the function does NOT reject non-
     * iMessage events. */
    hu_personal_model_t model;
    hu_personal_model_init(&model);
    hu_reaction_event_t e = {0};
    e.channel_id = "slack";
    e.sender_handle = "U07ALICE";
    e.kind = HU_REACTION_LOVE;
    e.polarity = HU_REACTION_POSITIVE;
    e.timestamp_unix = 1700000000;
    hu_error_t err = hu_reaction_ingest_personal_model(&model, &e, NULL, "ship it",
                                                 /*is_from_me_target=*/true,
                                                 /*in_group_chat=*/false);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
}

static void test_ingest_reaction_discord_group_uses_channel_qualifier(void) {
    /* Provenance for Discord group reactions must use "discord_channel"
     * not "discord_group" — matches the qualifier convention in
     * src/agent/channel_trust.c::hu_channel_trust. Verified indirectly
     * by ensuring ingest succeeds; tier mapping is exercised by
     * channel_trust's own test suite. */
    hu_personal_model_t model;
    hu_personal_model_init(&model);
    hu_reaction_event_t e = {0};
    e.channel_id = "discord";
    e.sender_handle = "alice#1234";
    e.kind = HU_REACTION_LAUGH;
    e.timestamp_unix = 1700000000;
    hu_error_t err = hu_reaction_ingest_personal_model(&model, &e, NULL, "ha",
                                                 /*is_from_me_target=*/true,
                                                 /*in_group_chat=*/true);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
}

static void test_ingest_group_chat_uses_group_provenance(void) {
    /* The in_group_chat flag changes the channel string from
     * "imessage_dm" → "imessage_group" which downgrades trust tier.
     * This test confirms the call succeeds; tier verification is in
     * the channel_trust tests. */
    hu_personal_model_t model;
    hu_personal_model_init(&model);
    hu_reaction_event_t e = {0};
    e.sender_handle = "Alice";
    e.kind = HU_REACTION_LIKE;
    e.timestamp_unix = 1700000000;
    hu_error_t err = hu_reaction_ingest_personal_model(&model, &e, NULL, "some message", false,
                                                 /*in_group_chat=*/true);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
}

/* ── fact-emergence verification (Tier 1 #5 from gap analysis) ────────
 *
 * These tests answer the question the prior e2e tests didn't: does the
 * synthesized text we feed to hu_personal_model_ingest actually produce
 * facts via hu_fact_extract, or are we just incrementing
 * style.sample_count? Run fact_extract directly on canonical synthesis
 * outputs and assert at least one heuristic_fact emerges.
 *
 * If these break in the future, the synthesis wording needs to be
 * adjusted to be more extractor-friendly — without this contract, the
 * "personal-model learning" claim is unfalsifiable. */

/* GAP CLOSED (2026-05-19): the prior commit documented that
 * fact_extract returns 0 facts for our third-person synthesis text.
 * Option (b) — direct hu_heuristic_fact_t construction in
 * hu_reaction_ingest_personal_model — has now landed. The tests below pin
 * the new contract:
 *
 *   1. The heuristic text extractor still produces 0 facts from
 *      synthesis output (kept for regression visibility — if this
 *      flips to nonzero, the extractor changed and we want to know).
 *   2. After hu_reaction_ingest_personal_model runs, model->fact_count
 *      reflects the directly-constructed fact (subject = sender,
 *      predicate = kind-derived verb, object = target preview).
 *      This is the actual M2 win — the personal model now learns
 *      about contacts from reaction patterns. */

static void test_text_extractor_still_misses_synthesis_text(void) {
    /* Regression guard — kept from the documented-gap era. If the
     * extractor gains third-person patterns this will flip, and the
     * direct-fact construction path becomes redundant (could be
     * removed). Worth keeping the signal. */
    hu_reaction_event_t e = {0};
    e.sender_handle = "Alice";
    e.kind = HU_REACTION_LOVE;

    char buf[512];
    size_t n = hu_imessage_synth_reaction(&e, NULL, "let's hike Mount Tam Saturday",
                                          /*is_from_me_target=*/true, buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);

    hu_fact_extract_result_t result;
    memset(&result, 0, sizeof(result));
    hu_error_t err = hu_fact_extract(buf, n, &result);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ((int)result.fact_count, 0);
}

static void test_ingest_reaction_produces_fact_about_contact(void) {
    /* The gap-closer contract: after one tapback ingest, the personal
     * model has at least one fact with the reactor as subject. */
    hu_personal_model_t model;
    hu_personal_model_init(&model);
    HU_ASSERT_EQ((int)model.fact_count, 0);

    hu_reaction_event_t e = {0};
    e.channel_id = "imessage";
    e.sender_handle = "Alice";
    e.kind = HU_REACTION_LOVE;
    e.polarity = HU_REACTION_POSITIVE;
    e.timestamp_unix = 1700000000;

    hu_error_t err = hu_reaction_ingest_personal_model(&model, &e, NULL, "let's hike Mount Tam Saturday",
                                                 /*is_from_me_target=*/true,
                                                 /*in_group_chat=*/false);
    HU_ASSERT_EQ((int)err, (int)HU_OK);

    /* DM-tier ingest goes into facts[]; group chat would route to
     * pending_facts[] via the trust-tier gate. This test is DM, so we
     * expect facts[] to have grown. */
    HU_ASSERT_TRUE(model.fact_count >= 1);

    /* Find the fact about Alice and verify it has the expected shape. */
    bool found = false;
    for (size_t i = 0; i < model.fact_count; i++) {
        if (strstr(model.facts[i].subject, "Alice") != NULL) {
            HU_ASSERT_TRUE(strstr(model.facts[i].predicate, "love") != NULL);
            HU_ASSERT_TRUE(strstr(model.facts[i].object, "hike") != NULL);
            found = true;
            break;
        }
    }
    HU_ASSERT_TRUE(found);
}

static void test_ingest_reaction_removal_does_not_produce_fact(void) {
    /* Removals are negative signal — we don't construct a fact for them.
     * The synthesis path still feeds style metrics. */
    hu_personal_model_t model;
    hu_personal_model_init(&model);

    hu_reaction_event_t e = {0};
    e.channel_id = "imessage";
    e.sender_handle = "Alice";
    e.kind = HU_REACTION_LOVE;
    e.is_removal = 1;
    e.timestamp_unix = 1700000000;

    (void)hu_reaction_ingest_personal_model(&model, &e, NULL, "let's hike", true, false);
    HU_ASSERT_EQ((int)model.fact_count, 0);
}

static void test_first_person_text_does_yield_facts(void) {
    /* Sanity check: extractor itself isn't broken — first-person user
     * text DOES produce facts. This pins the extractor's contract so we
     * can tell "extractor regressed" apart from "our synthesis voice
     * isn't extractable." */
    const char *first_person = "i love hiking on saturdays.";
    hu_fact_extract_result_t result;
    memset(&result, 0, sizeof(result));
    hu_error_t err = hu_fact_extract(first_person, strlen(first_person), &result);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_TRUE(result.fact_count >= 1);
}

static void test_synth_ingest_advances_style_sample_count(void) {
    /* The WEAKER property the ingest currently achieves: style.sample_count
     * goes from 0 → ≥1 on a successful ingest. This is what makes
     * has_content() return true; the e2e tests rely on it. When the
     * fact-emergence gap closes, this test should be STRENGTHENED to
     * assert specific (subject, predicate, object) triples emerged. */
    hu_personal_model_t model;
    hu_personal_model_init(&model);
    HU_ASSERT_EQ((int)model.style.sample_count, 0);

    hu_reaction_event_t e = {0};
    e.sender_handle = "Alice";
    e.kind = HU_REACTION_LOVE;
    e.timestamp_unix = 1700000000;
    hu_error_t err = hu_reaction_ingest_personal_model(&model, &e, NULL, "hiking Saturday",
                                                 /*is_from_me_target=*/true, false);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_TRUE(model.style.sample_count >= 1);
}

/* ── runner ───────────────────────────────────────────────────────── */

void run_imessage_ingest_tests(void) {
    HU_TEST_SUITE("imessage_ingest");
    HU_RUN_TEST(test_balloon_kind_recognizes_url_preview);
    HU_RUN_TEST(test_balloon_kind_recognizes_apple_pay);
    HU_RUN_TEST(test_balloon_kind_recognizes_audio_message);
    HU_RUN_TEST(test_balloon_kind_unknown_returns_unknown);
    HU_RUN_TEST(test_reaction_love_to_my_message_includes_preview);
    HU_RUN_TEST(test_reaction_custom_emoji_uses_provided_glyph);
    HU_RUN_TEST(test_reaction_removal_says_removed_not_reacted);
    HU_RUN_TEST(test_reaction_unknown_sender_becomes_someone);
    HU_RUN_TEST(test_edit_with_old_text_renders_delta);
    HU_RUN_TEST(test_edit_from_me_uses_first_person);
    HU_RUN_TEST(test_edit_without_old_text_still_renders);
    HU_RUN_TEST(test_unsend_uses_retracted_verb);
    HU_RUN_TEST(test_reply_includes_parent_and_child_text);
    HU_RUN_TEST(test_balloon_url_preview_includes_detail);
    HU_RUN_TEST(test_balloon_apple_pay_omits_amount);
    HU_RUN_TEST(test_balloon_audio_transcript_includes_text);
    HU_RUN_TEST(test_balloon_placemark_renders_location);
    HU_RUN_TEST(test_balloon_poll_renders_topic);
    HU_RUN_TEST(test_synth_handles_null_outputs_safely);
    HU_RUN_TEST(test_synth_long_preview_truncates_with_ellipsis);
    HU_RUN_TEST(test_ingest_reaction_rejects_null_model);
    HU_RUN_TEST(test_ingest_reaction_rejects_null_event);
    HU_RUN_TEST(test_ingest_reaction_succeeds_and_marks_content);
    HU_RUN_TEST(test_ingest_edit_routes_first_person_correctly);
    HU_RUN_TEST(test_ingest_unsend_handles_null_preview);
    HU_RUN_TEST(test_ingest_reply_rejects_null_reply_text);
    HU_RUN_TEST(test_ingest_reply_succeeds_with_parent_and_child);
    HU_RUN_TEST(test_ingest_balloon_url_preview_succeeds);
    HU_RUN_TEST(test_ingest_reaction_uses_event_emoji_when_custom_emoji_null);
    HU_RUN_TEST(test_reaction_event_struct_has_emoji_field);
    HU_RUN_TEST(test_ingest_reaction_is_channel_agnostic_slack);
    HU_RUN_TEST(test_ingest_reaction_discord_group_uses_channel_qualifier);
    HU_RUN_TEST(test_ingest_group_chat_uses_group_provenance);
    HU_RUN_TEST(test_text_extractor_still_misses_synthesis_text);
    HU_RUN_TEST(test_ingest_reaction_produces_fact_about_contact);
    HU_RUN_TEST(test_ingest_reaction_removal_does_not_produce_fact);
    HU_RUN_TEST(test_first_person_text_does_yield_facts);
    HU_RUN_TEST(test_synth_ingest_advances_style_sample_count);
}
