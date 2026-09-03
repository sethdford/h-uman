/* Tests for the measured style card (src/persona/style_card.c).
 *
 * The contract under test: the casual absolute-rules block renders its
 * numbers from ~/.human/personas/<persona>.style-card.json, and only falls
 * back to the compiled default when the card is missing. The default is
 * pinned so a fallback is a known, visible state — not a silent regression
 * to whatever number happened to be in a comment. */
#include "human/core/allocator.h"
#include "human/persona.h"
#include "human/persona/style_card.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static hu_allocator_t test_alloc;

/* A card whose numbers are unlike any plausible default, so a test that
 * sees them in the prompt knows the card won. */
static const char card_json[] =
    "{\"schema\":\"style-card/v2\",\"persona\":\"cardtest\",\"n\":977,"
    "\"window\":{\"start\":\"2026-07-05\",\"end\":\"2026-09-03\",\"days\":60},"
    "\"axes\":{"
    "\"lowercase_start_rate\":{\"value\":0.086,\"ci_lo\":0.069,\"ci_hi\":0.104,\"n\":977},"
    "\"no_terminal_punct_rate\":{\"value\":0.55,\"ci_lo\":0.52,\"ci_hi\":0.58,\"n\":977},"
    "\"question_rate\":{\"value\":0.099,\"ci_lo\":0.08,\"ci_hi\":0.12,\"n\":977},"
    "\"exclamation_rate\":{\"value\":0.039,\"ci_lo\":0.028,\"ci_hi\":0.051,\"n\":977},"
    "\"emoji_rate\":{\"value\":0.126,\"ci_lo\":0.10,\"ci_hi\":0.15,\"n\":977},"
    "\"length_chars\":{\"value\":31.2,\"ci_lo\":29.0,\"ci_hi\":33.5,\"n\":977}}}";

static char g_tmpdir[256];

static void make_tmpdir(void) {
    snprintf(g_tmpdir, sizeof(g_tmpdir), "/tmp/hu_style_card_XXXXXX");
    HU_ASSERT_NOT_NULL(mkdtemp(g_tmpdir));
    setenv("HU_PERSONA_DIR", g_tmpdir, 1);
}

static void write_card(const char *name, const char *json) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.style-card.json", g_tmpdir, name);
    FILE *f = fopen(path, "wb");
    HU_ASSERT_NOT_NULL(f);
    fputs(json, f);
    fclose(f);
}

static void cleanup_tmpdir(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/cardtest.style-card.json", g_tmpdir);
    unlink(path);
    rmdir(g_tmpdir);
    unsetenv("HU_PERSONA_DIR");
}

/* ── default ───────────────────────────────────────────────────────── */

static void default_card_is_marked_fallback_with_sane_rates(void) {
    hu_style_card_t c;
    hu_style_card_default(&c);
    HU_ASSERT_FALSE(c.from_card);
    HU_ASSERT_EQ(c.n, 0u);
    HU_ASSERT_TRUE(c.no_terminal_punct_rate > 0.5 && c.no_terminal_punct_rate < 1.0);
    HU_ASSERT_TRUE(c.lowercase_start_rate >= 0.0 && c.lowercase_start_rate < 0.5);
    HU_ASSERT_TRUE(c.emoji_rate >= 0.0 && c.emoji_rate < 0.5);
    HU_ASSERT_TRUE(c.question_rate >= 0.0 && c.question_rate < 0.5);
    HU_ASSERT_TRUE(c.exclamation_rate >= 0.0 && c.exclamation_rate < 0.5);
}

/* ── parse ─────────────────────────────────────────────────────────── */

static void parse_v2_card_reads_every_axis_and_provenance(void) {
    test_alloc = hu_system_allocator();
    hu_style_card_t c;
    HU_ASSERT_EQ(hu_style_card_parse(&test_alloc, card_json, strlen(card_json), &c), HU_OK);
    HU_ASSERT_TRUE(c.from_card);
    HU_ASSERT_EQ(c.n, 977u);
    HU_ASSERT_FLOAT_EQ(c.lowercase_start_rate, 0.086, 1e-9);
    HU_ASSERT_FLOAT_EQ(c.no_terminal_punct_rate, 0.55, 1e-9);
    HU_ASSERT_FLOAT_EQ(c.question_rate, 0.099, 1e-9);
    HU_ASSERT_FLOAT_EQ(c.exclamation_rate, 0.039, 1e-9);
    HU_ASSERT_FLOAT_EQ(c.emoji_rate, 0.126, 1e-9);
    HU_ASSERT_STR_EQ(c.window_start, "2026-07-05");
    HU_ASSERT_STR_EQ(c.window_end, "2026-09-03");
}

static void parse_rejects_card_missing_an_axis(void) {
    test_alloc = hu_system_allocator();
    const char *json = "{\"schema\":\"style-card/v2\",\"n\":500,\"axes\":{"
                       "\"lowercase_start_rate\":{\"value\":0.1},"
                       "\"no_terminal_punct_rate\":{\"value\":0.8},"
                       "\"question_rate\":{\"value\":0.1},"
                       "\"exclamation_rate\":{\"value\":0.04}}}"; /* no emoji_rate */
    hu_style_card_t c;
    HU_ASSERT_EQ(hu_style_card_parse(&test_alloc, json, strlen(json), &c), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_FALSE(c.from_card); /* left as the default */
}

static void parse_rejects_rate_outside_unit_interval(void) {
    test_alloc = hu_system_allocator();
    const char *json = "{\"schema\":\"style-card/v2\",\"n\":500,\"axes\":{"
                       "\"lowercase_start_rate\":{\"value\":0.1},"
                       "\"no_terminal_punct_rate\":{\"value\":1.5},"
                       "\"question_rate\":{\"value\":0.1},"
                       "\"exclamation_rate\":{\"value\":0.04},"
                       "\"emoji_rate\":{\"value\":0.1}}}";
    hu_style_card_t c;
    HU_ASSERT_EQ(hu_style_card_parse(&test_alloc, json, strlen(json), &c), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_FALSE(c.from_card);
}

static void parse_rejects_malformed_json(void) {
    test_alloc = hu_system_allocator();
    const char *json = "{not json";
    hu_style_card_t c;
    HU_ASSERT_NEQ(hu_style_card_parse(&test_alloc, json, strlen(json), &c), HU_OK);
    HU_ASSERT_FALSE(c.from_card);
}

/* ── load ──────────────────────────────────────────────────────────── */

static void load_missing_card_returns_not_found_and_default(void) {
    test_alloc = hu_system_allocator();
    make_tmpdir();
    hu_style_card_t c;
    HU_ASSERT_EQ(hu_style_card_load_for_persona(&test_alloc, "cardtest", 8, &c), HU_ERR_NOT_FOUND);
    HU_ASSERT_FALSE(c.from_card);
    cleanup_tmpdir();
}

static void load_reads_card_from_persona_dir(void) {
    test_alloc = hu_system_allocator();
    make_tmpdir();
    write_card("cardtest", card_json);
    hu_style_card_t c;
    HU_ASSERT_EQ(hu_style_card_load_for_persona(&test_alloc, "cardtest", 8, &c), HU_OK);
    HU_ASSERT_TRUE(c.from_card);
    HU_ASSERT_FLOAT_EQ(c.no_terminal_punct_rate, 0.55, 1e-9);
    cleanup_tmpdir();
}

/* ── render ────────────────────────────────────────────────────────── */

static void render_casual_rules_states_card_numbers(void) {
    test_alloc = hu_system_allocator();
    hu_style_card_t c;
    HU_ASSERT_EQ(hu_style_card_parse(&test_alloc, card_json, strlen(card_json), &c), HU_OK);
    char buf[1024];
    size_t len = 0;
    HU_ASSERT_EQ(hu_style_card_render_casual_rules(&c, buf, sizeof(buf), &len), HU_OK);
    HU_ASSERT_TRUE(len > 0 && len == strlen(buf));
    /* Numbered as rule 2 so it slots into the absolute-rules block. */
    HU_ASSERT_TRUE(strncmp(buf, "2. ", 3) == 0);
    /* The marker the existing tests pin. */
    HU_ASSERT_STR_CONTAINS(buf, "no period at the end");
    /* Each axis reaches the prompt as a rounded, human-readable number. */
    HU_ASSERT_STR_CONTAINS(buf, "55%");     /* no_terminal_punct 0.55 */
    HU_ASSERT_STR_CONTAINS(buf, "1 in 12"); /* lowercase_start 0.086 */
    HU_ASSERT_STR_CONTAINS(buf, "1 in 10"); /* question 0.099 */
    HU_ASSERT_STR_CONTAINS(buf, "1 in 8");  /* emoji 0.126 */
    HU_ASSERT_STR_CONTAINS(buf, "1 in 26"); /* exclamation 0.039 */
    /* The old contradictory directive must never come back. */
    HU_ASSERT_STR_NOT_CONTAINS(buf, "All lowercase");
}

static void render_rejects_small_buffer(void) {
    hu_style_card_t c;
    hu_style_card_default(&c);
    char buf[16];
    size_t len = 0;
    HU_ASSERT_EQ(hu_style_card_render_casual_rules(&c, buf, sizeof(buf), &len),
                 HU_ERR_OUT_OF_MEMORY);
}

static void render_near_zero_rate_says_almost_never(void) {
    hu_style_card_t c;
    hu_style_card_default(&c);
    c.emoji_rate = 0.001;
    char buf[1024];
    size_t len = 0;
    HU_ASSERT_EQ(hu_style_card_render_casual_rules(&c, buf, sizeof(buf), &len), HU_OK);
    HU_ASSERT_STR_CONTAINS(buf, "almost never");
    HU_ASSERT_STR_NOT_CONTAINS(buf, "1 in 1000");
}

/* ── the contract: absolute rules prefer the card, pin the fallback ── */

static void absolute_rules_prefer_card_over_compiled_default(void) {
    make_tmpdir();
    write_card("cardtest", card_json);
    hu_persona_t p;
    memset(&p, 0, sizeof(p));
    p.name = "cardtest";
    p.name_len = 8;
    char buf[4096];
    size_t len = 0;
    HU_ASSERT_EQ(hu_persona_build_absolute_rules_fmt(&p, NULL, buf, sizeof(buf), &len), HU_OK);
    HU_ASSERT_STR_CONTAINS(buf, "55%"); /* only the card says 55% */
    HU_ASSERT_STR_CONTAINS(buf, "no period at the end");
    HU_ASSERT_STR_CONTAINS(buf, "You are HUMAN");
    cleanup_tmpdir();
}

static void absolute_rules_fall_back_to_default_when_card_missing(void) {
    make_tmpdir(); /* empty dir: no card for this persona */
    hu_persona_t p;
    memset(&p, 0, sizeof(p));
    p.name = "cardtest";
    p.name_len = 8;
    char buf[4096];
    size_t len = 0;
    HU_ASSERT_EQ(hu_persona_build_absolute_rules_fmt(&p, NULL, buf, sizeof(buf), &len), HU_OK);
    HU_ASSERT_STR_NOT_CONTAINS(buf, "55%");
    /* Pin the fallback: it must equal what the compiled default renders. */
    hu_style_card_t d;
    hu_style_card_default(&d);
    char expect[1024];
    size_t elen = 0;
    HU_ASSERT_EQ(hu_style_card_render_casual_rules(&d, expect, sizeof(expect), &elen), HU_OK);
    HU_ASSERT_STR_CONTAINS(buf, expect);
    cleanup_tmpdir();
}

static void absolute_rules_null_persona_uses_default(void) {
    char buf[4096];
    size_t len = 0;
    HU_ASSERT_EQ(hu_persona_build_absolute_rules_fmt(NULL, NULL, buf, sizeof(buf), &len), HU_OK);
    hu_style_card_t d;
    hu_style_card_default(&d);
    char expect[1024];
    size_t elen = 0;
    HU_ASSERT_EQ(hu_style_card_render_casual_rules(&d, expect, sizeof(expect), &elen), HU_OK);
    HU_ASSERT_STR_CONTAINS(buf, expect);
}

static void formal_register_carries_no_card_numbers(void) {
    make_tmpdir();
    write_card("cardtest", card_json);
    hu_persona_t p;
    memset(&p, 0, sizeof(p));
    p.name = "cardtest";
    p.name_len = 8;
    char buf[4096];
    size_t len = 0;
    HU_ASSERT_EQ(hu_persona_build_absolute_rules_fmt(&p, "professional", buf, sizeof(buf), &len),
                 HU_OK);
    HU_ASSERT_STR_NOT_CONTAINS(buf, "55%");
    HU_ASSERT_STR_NOT_CONTAINS(buf, "no period at the end");
    cleanup_tmpdir();
}

void run_style_card_tests(void) {
    HU_TEST_SUITE("style_card");
    HU_RUN_TEST(default_card_is_marked_fallback_with_sane_rates);
    HU_RUN_TEST(parse_v2_card_reads_every_axis_and_provenance);
    HU_RUN_TEST(parse_rejects_card_missing_an_axis);
    HU_RUN_TEST(parse_rejects_rate_outside_unit_interval);
    HU_RUN_TEST(parse_rejects_malformed_json);
    HU_RUN_TEST(load_missing_card_returns_not_found_and_default);
    HU_RUN_TEST(load_reads_card_from_persona_dir);
    HU_RUN_TEST(render_casual_rules_states_card_numbers);
    HU_RUN_TEST(render_rejects_small_buffer);
    HU_RUN_TEST(render_near_zero_rate_says_almost_never);
    HU_RUN_TEST(absolute_rules_prefer_card_over_compiled_default);
    HU_RUN_TEST(absolute_rules_fall_back_to_default_when_card_missing);
    HU_RUN_TEST(absolute_rules_null_persona_uses_default);
    HU_RUN_TEST(formal_register_carries_no_card_numbers);
}
