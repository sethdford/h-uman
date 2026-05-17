/*
 * Pins the contract of `hu_persona_render_for_channel` — the helper that lets
 * Tier-1 channel send paths apply per-channel persona overlays to outbound
 * text. See docs/plans/2026-05-16-audit-followups/01-persona-overlay-wiring.md
 * for the acceptance criteria this file implements (AC-5, AC-6).
 *
 * The tests exercise the same production symbol the channels call — there is
 * no helper duplicated in this file. This satisfies the
 * `tests/CLAUDE.md`-mandated "tests reference production symbols" rule.
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/persona.h"
#include "test_framework.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static hu_allocator_t test_alloc;

static void *alloc_fn(void *ctx, size_t size) {
    (void)ctx;
    return malloc(size);
}
static void free_fn(void *ctx, void *ptr, size_t size) {
    (void)ctx;
    (void)size;
    free(ptr);
}
static void *realloc_fn(void *ctx, void *ptr, size_t old_size, size_t new_size) {
    (void)ctx;
    (void)old_size;
    return realloc(ptr, new_size);
}

static void setup_alloc(void) {
    test_alloc.alloc = alloc_fn;
    test_alloc.free = free_fn;
    test_alloc.realloc = realloc_fn;
    test_alloc.ctx = NULL;
}

static void render_with_null_overlay_returns_identity_copy(void) {
    setup_alloc();
    const char *in = "Hey there, this is a test message.";
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_persona_render_for_channel(NULL, in, strlen(in), &test_alloc, &out, &out_len);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_EQ(out, in);
    HU_ASSERT_EQ((long)out_len, (long)strlen(in));
    /* Must be a *copy*, not the input pointer itself, so callers can free. */
    HU_ASSERT(out != in);
    free(out);
}

static void render_with_empty_input_returns_empty_string(void) {
    setup_alloc();
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_persona_render_for_channel(NULL, "", 0, &test_alloc, &out, &out_len);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_EQ((long)out_len, 0L);
    HU_ASSERT_EQ((int)out[0], 0);
    free(out);
}

static void render_with_null_allocator_returns_invalid_argument(void) {
    char *out = NULL;
    hu_error_t err = hu_persona_render_for_channel(NULL, "x", 1, NULL, &out, NULL);
    HU_ASSERT_EQ((int)err, (int)HU_ERR_INVALID_ARGUMENT);
}

/* AC-3: emoji_usage=="none" strips emoji from outbound text. */
static void render_emoji_none_strips_emoji(void) {
    setup_alloc();
    hu_persona_overlay_t ov = {0};
    ov.emoji_usage = "none";
    /* "hello \U0001F600 world" — UTF-8 of grinning face is F0 9F 98 80 */
    const char *in = "hello \xF0\x9F\x98\x80 world";
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_persona_render_for_channel(&ov, in, strlen(in), &test_alloc, &out, &out_len);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    /* No 0xF0 bytes should remain. */
    for (size_t i = 0; i < out_len; i++) {
        HU_ASSERT((unsigned char)out[i] != 0xF0);
    }
    /* "hello" and "world" must both survive. */
    HU_ASSERT_STR_CONTAINS(out, "hello");
    HU_ASSERT_STR_CONTAINS(out, "world");
    free(out);
}

/* AC-1 (Telegram inverse): formality=="formal" swaps "hey" -> "hello" and
 * capitalizes the leading character. */
static void render_formal_swaps_casual_words(void) {
    setup_alloc();
    hu_persona_overlay_t ov = {0};
    ov.formality = "formal";
    const char *in = "hey there, gonna grab coffee";
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_persona_render_for_channel(&ov, in, strlen(in), &test_alloc, &out, &out_len);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_CONTAINS(out, "Hello");
    HU_ASSERT_STR_CONTAINS(out, "going to");
    HU_ASSERT_STR_NOT_CONTAINS(out, "hey");
    HU_ASSERT_STR_NOT_CONTAINS(out, "gonna");
    free(out);
}

/* AC-3 reinforcement: a "formal" / "professional" overlay strips emoji even
 * when emoji_usage isn't explicitly "none" — Slack-style overlays should
 * never emit emoji per the design doc. */
static void render_formal_strips_emoji_implicitly(void) {
    setup_alloc();
    hu_persona_overlay_t ov = {0};
    ov.formality = "professional";
    const char *in = "yes \xF0\x9F\x91\x8D agreed";
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_persona_render_for_channel(&ov, in, strlen(in), &test_alloc, &out, &out_len);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    for (size_t i = 0; i < out_len; i++) {
        HU_ASSERT((unsigned char)out[i] != 0xF0);
    }
    HU_ASSERT_STR_CONTAINS(out, "agreed");
    free(out);
}

/* AC-2: explicit max_chars=NNN truncates output. */
static void render_length_max_chars_truncates(void) {
    setup_alloc();
    hu_persona_overlay_t ov = {0};
    ov.avg_length = "max_chars=40";
    const char *in = "this is a long message that should be truncated. it has multiple sentences. "
                     "yes it really should.";
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_persona_render_for_channel(&ov, in, strlen(in), &test_alloc, &out, &out_len);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_LE((long)out_len, 40L);
    HU_ASSERT_GT((long)out_len, 0L);
    free(out);
}

/* AC-2 reinforcement: named "short" bucket caps at 200 bytes and prefers
 * sentence-boundary truncation. */
static void render_length_short_caps_at_200(void) {
    setup_alloc();
    hu_persona_overlay_t ov = {0};
    ov.avg_length = "short";
    /* 300-byte input made of full sentences. */
    char in[301];
    memset(in, 0, sizeof(in));
    const char *seg = "this is a sentence. ";
    while (strlen(in) + strlen(seg) < 300)
        strcat(in, seg);
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_persona_render_for_channel(&ov, in, strlen(in), &test_alloc, &out, &out_len);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_LE((long)out_len, 200L);
    free(out);
}

/* AC-2 + AC-3 combined: max_segment_chars also clamps. */
static void render_max_segment_chars_acts_as_hard_cap(void) {
    setup_alloc();
    hu_persona_overlay_t ov = {0};
    ov.max_segment_chars = 20;
    const char *in = "this is a sentence that is more than twenty bytes long";
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_persona_render_for_channel(&ov, in, strlen(in), &test_alloc, &out, &out_len);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_LE((long)out_len, 20L);
    free(out);
}

/* Composed overlay: formal + max_chars truncation. */
static void render_combined_formal_and_length_applies_both(void) {
    setup_alloc();
    hu_persona_overlay_t ov = {0};
    ov.formality = "formal";
    ov.avg_length = "max_chars=80";
    ov.emoji_usage = "none";
    const char *in = "hey, gonna grab some coffee \xF0\x9F\x98\x80 and then catch up on emails. "
                     "talk later!";
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_persona_render_for_channel(&ov, in, strlen(in), &test_alloc, &out, &out_len);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_LE((long)out_len, 80L);
    /* "Hello" appears, "hey" gone, no emoji. */
    HU_ASSERT_STR_CONTAINS(out, "Hello");
    HU_ASSERT_STR_NOT_CONTAINS(out, "hey");
    HU_ASSERT_STR_NOT_CONTAINS(out, "gonna");
    for (size_t i = 0; i < out_len; i++) {
        HU_ASSERT((unsigned char)out[i] != 0xF0);
    }
    free(out);
}

/* AC-6 regression guard: an entirely zero-initialized overlay produces an
 * identity copy. Channels that wire the helper without a configured overlay
 * field must not surprise the caller. */
static void render_zero_overlay_returns_identity(void) {
    setup_alloc();
    hu_persona_overlay_t ov = {0};
    const char *in = "Just some words.";
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_persona_render_for_channel(&ov, in, strlen(in), &test_alloc, &out, &out_len);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_STR_EQ(out, in);
    HU_ASSERT_EQ((long)out_len, (long)strlen(in));
    free(out);
}

/* Casual overlay lowercases the first letter and swaps "hello" -> "hey". */
static void render_casual_swaps_formal_words(void) {
    setup_alloc();
    hu_persona_overlay_t ov = {0};
    ov.formality = "casual";
    const char *in = "Hello, going to grab coffee";
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_persona_render_for_channel(&ov, in, strlen(in), &test_alloc, &out, &out_len);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_STR_CONTAINS(out, "gonna");
    /* First char is lower-case in casual mode. */
    HU_ASSERT(out[0] >= 'a' && out[0] <= 'z');
    free(out);
}

/* ── Effective formality predicate ──────────────────────────────────────── */

static void effective_formality_null_warmth_returns_overlay(void) {
    /* No warmth context → overlay's formality unchanged (preserves the
     * existing channel-overlay-driven behavior for all the channels that
     * don't have contact context yet — slack/telegram/discord/etc.). */
    HU_ASSERT(hu_persona_effective_formality(NULL, NULL) == NULL);
    HU_ASSERT_STR_EQ(hu_persona_effective_formality("formal", NULL), "formal");
    HU_ASSERT_STR_EQ(hu_persona_effective_formality("casual", NULL), "casual");
}

static void effective_formality_close_overrides_formal(void) {
    /* The load-bearing case: a "close" contact downgrades the formal overlay
     * to casual. This is what makes the persona's warmth_level field finally
     * shape outbound rendering instead of being parsed-but-ignored. */
    HU_ASSERT_STR_EQ(hu_persona_effective_formality("formal", "close"), "casual");
    HU_ASSERT_STR_EQ(hu_persona_effective_formality("professional", "close friend"), "casual");
    HU_ASSERT_STR_EQ(hu_persona_effective_formality("formal", "warm"), "casual");
    HU_ASSERT_STR_EQ(hu_persona_effective_formality("formal", "high"), "casual");
}

static void effective_formality_close_with_no_overlay_returns_casual(void) {
    /* No channel overlay but a close contact → "casual" still wins. */
    HU_ASSERT_STR_EQ(hu_persona_effective_formality(NULL, "close"), "casual");
    HU_ASSERT_STR_EQ(hu_persona_effective_formality("", "warm"), "casual");
}

static void effective_formality_close_with_casual_overlay_unchanged(void) {
    /* Close + already-casual → don't double-cast; preserve the overlay. */
    HU_ASSERT_STR_EQ(hu_persona_effective_formality("casual", "close"), "casual");
    HU_ASSERT_STR_EQ(hu_persona_effective_formality("informal", "warm"), "informal");
}

static void effective_formality_distant_warmth_preserves_overlay(void) {
    /* Acquaintance / professional contact → don't downgrade formal overlay. */
    HU_ASSERT_STR_EQ(hu_persona_effective_formality("formal", "acquaintance"), "formal");
    HU_ASSERT_STR_EQ(hu_persona_effective_formality("formal", "professional"), "formal");
    HU_ASSERT_STR_EQ(hu_persona_effective_formality("formal", "unknown"), "formal");
}

/* ── Render-with-warmth integration ─────────────────────────────────────── */

static void render_with_warmth_close_downgrades_formal_overlay(void) {
    /* The end-to-end invariant: a formal Slack/Teams-style overlay applied
     * to a close contact produces casual output (formal swaps NOT applied;
     * lowercase-first applied). If a future change drops the warmth wiring,
     * this test fails before that change ships. */
    hu_persona_overlay_t ov = {0};
    ov.formality = "formal";
    const char *in = "Going to grab coffee";
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_persona_render_for_channel_with_warmth(&ov, "close", in, strlen(in),
                                                               &test_alloc, &out, &out_len);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    /* "Going" → "gonna" is the canonical FORMAL_TO_CASUAL swap; if warmth
     * override worked, the casual swap fired even though overlay was formal. */
    HU_ASSERT_STR_CONTAINS(out, "gonna");
    /* Casual mode lowercases the first letter (unless it's "I"). */
    HU_ASSERT(out[0] >= 'a' && out[0] <= 'z');
    free(out);
}

static void render_with_warmth_null_matches_original_render(void) {
    /* Backward compatibility: passing NULL warmth must produce byte-identical
     * output to the original hu_persona_render_for_channel. Catches a future
     * change that accidentally adds warmth-related processing to the NULL
     * path. */
    hu_persona_overlay_t ov = {0};
    ov.formality = "formal";
    const char *in = "Going to grab coffee yeah";

    char *out_old = NULL, *out_new = NULL;
    size_t out_old_len = 0, out_new_len = 0;
    hu_error_t e1 =
        hu_persona_render_for_channel(&ov, in, strlen(in), &test_alloc, &out_old, &out_old_len);
    hu_error_t e2 = hu_persona_render_for_channel_with_warmth(&ov, NULL, in, strlen(in),
                                                              &test_alloc, &out_new, &out_new_len);
    HU_ASSERT_EQ((int)e1, (int)HU_OK);
    HU_ASSERT_EQ((int)e2, (int)HU_OK);
    HU_ASSERT_EQ(out_old_len, out_new_len);
    HU_ASSERT_EQ(memcmp(out_old, out_new, out_old_len), 0);
    free(out_old);
    free(out_new);
}

static void render_with_warmth_distant_preserves_formal(void) {
    /* A distant / acquaintance contact must NOT downgrade the formal overlay.
     * Pin: if a future change overly-eager-matches the warmth string
     * (e.g. accidentally maps "acquaintance" to casual), this catches it. */
    hu_persona_overlay_t ov = {0};
    ov.formality = "formal";
    const char *in = "going to grab coffee";

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_persona_render_for_channel_with_warmth(&ov, "acquaintance", in, strlen(in),
                                                               &test_alloc, &out, &out_len);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    /* Formal path capitalizes first letter; if the warmth override fired
     * by mistake the first letter would stay lowercase. */
    HU_ASSERT(out[0] >= 'A' && out[0] <= 'Z');
    free(out);
}

void run_persona_overlay_render_tests(void);
void run_persona_overlay_render_tests(void) {
    HU_TEST_SUITE("persona_overlay_render");
    HU_RUN_TEST(render_with_null_overlay_returns_identity_copy);
    HU_RUN_TEST(render_with_empty_input_returns_empty_string);
    HU_RUN_TEST(render_with_null_allocator_returns_invalid_argument);
    HU_RUN_TEST(render_emoji_none_strips_emoji);
    HU_RUN_TEST(render_formal_swaps_casual_words);
    HU_RUN_TEST(render_formal_strips_emoji_implicitly);
    HU_RUN_TEST(render_length_max_chars_truncates);
    HU_RUN_TEST(render_length_short_caps_at_200);
    HU_RUN_TEST(render_max_segment_chars_acts_as_hard_cap);
    HU_RUN_TEST(render_combined_formal_and_length_applies_both);
    HU_RUN_TEST(render_zero_overlay_returns_identity);
    HU_RUN_TEST(render_casual_swaps_formal_words);

    HU_RUN_TEST(effective_formality_null_warmth_returns_overlay);
    HU_RUN_TEST(effective_formality_close_overrides_formal);
    HU_RUN_TEST(effective_formality_close_with_no_overlay_returns_casual);
    HU_RUN_TEST(effective_formality_close_with_casual_overlay_unchanged);
    HU_RUN_TEST(effective_formality_distant_warmth_preserves_overlay);

    HU_RUN_TEST(render_with_warmth_close_downgrades_formal_overlay);
    HU_RUN_TEST(render_with_warmth_null_matches_original_render);
    HU_RUN_TEST(render_with_warmth_distant_preserves_formal);
}
