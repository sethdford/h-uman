/* Tests for the measured emotion card (src/persona/emotion_card.c).
 *
 * The contract under test: casual rule 14 renders its numbers from
 * ~/.human/personas/<persona>.emotion-card.json and reaches the prompt
 * ONLY when HU_EMOTION_REGISTER=live; shadow logs and sends nothing; a
 * missing card means "not measured" — no compiled default, nothing
 * rendered. */
#include "human/core/allocator.h"
#include "human/persona.h"
#include "human/persona/emotion_card.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static hu_allocator_t test_alloc;

/* Numbers unlike any plausible default so a test that sees them in the
 * prompt knows the card won: 62% neutral, intensity 0.23. */
static const char card_json[] =
    "{\"schema\":\"emotion-card/v1\",\"persona\":\"emotest\",\"n\":240,"
    "\"window\":{\"start\":\"2026-07-07\",\"end\":\"2026-09-05\",\"days\":60},"
    "\"judge\":{\"id\":\"m|cowen-keltner-27/v1|abc\"},"
    "\"neutral_share\":{\"value\":0.62,\"ci_lo\":0.55,\"ci_hi\":0.68,\"n\":240},"
    "\"mean_intensity\":{\"value\":0.23,\"ci_lo\":0.2,\"ci_hi\":0.26,\"n\":240},"
    "\"valence_mean\":{\"value\":0.31,\"ci_lo\":0.25,\"ci_hi\":0.37,\"n\":240},"
    "\"top\":[{\"emotion\":\"amusement\",\"share\":0.15},"
    "{\"emotion\":\"interest\",\"share\":0.1},"
    "{\"emotion\":\"satisfaction\",\"share\":0.06},"
    "{\"emotion\":\"calmness\",\"share\":0.03},"
    "{\"emotion\":\"joy\",\"share\":0.02}]}";

static char g_tmpdir[256];

static void make_tmpdir(void) {
    snprintf(g_tmpdir, sizeof(g_tmpdir), "/tmp/hu_emotion_card_XXXXXX");
    HU_ASSERT_NOT_NULL(mkdtemp(g_tmpdir));
    setenv("HU_PERSONA_DIR", g_tmpdir, 1);
}

static void write_card(const char *name, const char *json) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.emotion-card.json", g_tmpdir, name);
    FILE *f = fopen(path, "wb");
    HU_ASSERT_NOT_NULL(f);
    fputs(json, f);
    fclose(f);
}

static void cleanup_tmpdir(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/emotest.emotion-card.json", g_tmpdir);
    unlink(path);
    rmdir(g_tmpdir);
    unsetenv("HU_PERSONA_DIR");
    unsetenv("HU_EMOTION_REGISTER");
}

static void build_casual_rules(char *buf, size_t cap, size_t *len) {
    hu_persona_t p;
    memset(&p, 0, sizeof(p));
    p.name = "emotest";
    p.name_len = 7;
    HU_ASSERT_EQ(hu_persona_build_absolute_rules_fmt(&p, NULL, buf, cap, len), HU_OK);
}

/* ── parse ─────────────────────────────────────────────────────────── */

static void parse_v1_card_reads_axes_top_and_provenance(void) {
    test_alloc = hu_system_allocator();
    hu_emotion_card_t c;
    HU_ASSERT_EQ(hu_emotion_card_parse(&test_alloc, card_json, strlen(card_json), &c), HU_OK);
    HU_ASSERT_TRUE(c.from_card);
    HU_ASSERT_EQ(c.n, 240u);
    HU_ASSERT_FLOAT_EQ(c.neutral_share, 0.62, 1e-9);
    HU_ASSERT_FLOAT_EQ(c.mean_intensity, 0.23, 1e-9);
    HU_ASSERT_FLOAT_EQ(c.valence_mean, 0.31, 1e-9);
    HU_ASSERT_EQ(c.top_count, (unsigned)HU_EMOTION_CARD_TOP_MAX); /* 5 in the file, 4 kept */
    HU_ASSERT_STR_EQ(c.top[0].emotion, "amusement");
    HU_ASSERT_FLOAT_EQ(c.top[0].share, 0.15, 1e-9);
    HU_ASSERT_STR_EQ(c.top[3].emotion, "calmness");
    HU_ASSERT_STR_EQ(c.window_start, "2026-07-07");
    HU_ASSERT_STR_EQ(c.window_end, "2026-09-05");
}

static void parse_rejects_missing_axis_zero_n_and_out_of_range(void) {
    test_alloc = hu_system_allocator();
    hu_emotion_card_t c;
    const char *no_axis = "{\"n\":50,\"neutral_share\":{\"value\":0.5},"
                          "\"mean_intensity\":{\"value\":0.2}}"; /* no valence_mean */
    HU_ASSERT_EQ(hu_emotion_card_parse(&test_alloc, no_axis, strlen(no_axis), &c),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_FALSE(c.from_card);
    const char *zero_n = "{\"n\":0,\"neutral_share\":{\"value\":0.5},"
                         "\"mean_intensity\":{\"value\":0.2},\"valence_mean\":{\"value\":0.1}}";
    HU_ASSERT_EQ(hu_emotion_card_parse(&test_alloc, zero_n, strlen(zero_n), &c),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_FALSE(c.from_card);
    const char *bad_valence =
        "{\"n\":50,\"neutral_share\":{\"value\":0.5},"
        "\"mean_intensity\":{\"value\":0.2},\"valence_mean\":{\"value\":1.5}}";
    HU_ASSERT_EQ(hu_emotion_card_parse(&test_alloc, bad_valence, strlen(bad_valence), &c),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_FALSE(c.from_card);
}

static void parse_skips_malformed_top_entries(void) {
    test_alloc = hu_system_allocator();
    const char *json = "{\"n\":50,\"neutral_share\":{\"value\":0.5},"
                       "\"mean_intensity\":{\"value\":0.2},\"valence_mean\":{\"value\":0.1},"
                       "\"top\":[{\"share\":0.3},{\"emotion\":\"joy\",\"share\":2},"
                       "\"junk\",{\"emotion\":\"joy\",\"share\":0.2}]}";
    hu_emotion_card_t c;
    HU_ASSERT_EQ(hu_emotion_card_parse(&test_alloc, json, strlen(json), &c), HU_OK);
    HU_ASSERT_EQ(c.top_count, 1u);
    HU_ASSERT_STR_EQ(c.top[0].emotion, "joy");
}

static void parse_rejects_malformed_json(void) {
    test_alloc = hu_system_allocator();
    hu_emotion_card_t c;
    HU_ASSERT_NEQ(hu_emotion_card_parse(&test_alloc, "{not json", 9, &c), HU_OK);
    HU_ASSERT_FALSE(c.from_card);
}

/* ── load / resolve ────────────────────────────────────────────────── */

static void load_missing_card_returns_not_found(void) {
    test_alloc = hu_system_allocator();
    make_tmpdir();
    hu_emotion_card_t c;
    HU_ASSERT_EQ(hu_emotion_card_load_for_persona(&test_alloc, "emotest", 7, &c), HU_ERR_NOT_FOUND);
    HU_ASSERT_FALSE(c.from_card);
    HU_ASSERT_FALSE(hu_emotion_card_resolve("emotest", 7, &c));
    HU_ASSERT_FALSE(c.from_card);
    cleanup_tmpdir();
}

static void resolve_reads_card_from_persona_dir(void) {
    make_tmpdir();
    write_card("emotest", card_json);
    hu_emotion_card_t c;
    HU_ASSERT_TRUE(hu_emotion_card_resolve("emotest", 7, &c));
    HU_ASSERT_TRUE(c.from_card);
    HU_ASSERT_FLOAT_EQ(c.neutral_share, 0.62, 1e-9);
    HU_ASSERT_FALSE(hu_emotion_card_resolve(NULL, 0, &c));
    cleanup_tmpdir();
}

/* ── render ────────────────────────────────────────────────────────── */

static void render_rule_states_card_numbers(void) {
    test_alloc = hu_system_allocator();
    hu_emotion_card_t c;
    HU_ASSERT_EQ(hu_emotion_card_parse(&test_alloc, card_json, strlen(card_json), &c), HU_OK);
    char buf[1024];
    size_t len = 0;
    HU_ASSERT_EQ(hu_emotion_card_render_rule(&c, buf, sizeof(buf), &len), HU_OK);
    HU_ASSERT_TRUE(len > 0 && len == strlen(buf));
    HU_ASSERT_TRUE(strncmp(buf, "14. ", 4) == 0);
    HU_ASSERT_STR_CONTAINS(buf, "240 of your texts");
    HU_ASSERT_STR_CONTAINS(buf, "62%");
    HU_ASSERT_STR_CONTAINS(buf, "amusement, interest and satisfaction");
    HU_ASSERT_STR_NOT_CONTAINS(buf, "calmness"); /* only the top 3 are named */
    HU_ASSERT_STR_CONTAINS(buf, "2 out of 10");
}

static void render_without_top_says_rare_and_refuses_unmeasured(void) {
    test_alloc = hu_system_allocator();
    hu_emotion_card_t c;
    HU_ASSERT_EQ(hu_emotion_card_parse(&test_alloc, card_json, strlen(card_json), &c), HU_OK);
    c.top_count = 0;
    char buf[1024];
    HU_ASSERT_EQ(hu_emotion_card_render_rule(&c, buf, sizeof(buf), NULL), HU_OK);
    HU_ASSERT_STR_CONTAINS(buf, "mostly rare");
    char small[16];
    HU_ASSERT_EQ(hu_emotion_card_render_rule(&c, small, sizeof(small), NULL), HU_ERR_OUT_OF_MEMORY);
    hu_emotion_card_t none;
    hu_emotion_card_clear(&none);
    HU_ASSERT_EQ(hu_emotion_card_render_rule(&none, buf, sizeof(buf), NULL),
                 HU_ERR_INVALID_ARGUMENT);
}

/* ── the gate: off by default, shadow sends nothing, live appends ──── */

static void gate_defaults_off_and_parses_the_three_states(void) {
    unsetenv("HU_EMOTION_REGISTER");
    HU_ASSERT_EQ(hu_emotion_register_mode(), HU_GATE_OFF);
    setenv("HU_EMOTION_REGISTER", "shadow", 1);
    HU_ASSERT_EQ(hu_emotion_register_mode(), HU_GATE_SHADOW);
    setenv("HU_EMOTION_REGISTER", "live", 1);
    HU_ASSERT_EQ(hu_emotion_register_mode(), HU_GATE_LIVE);
    setenv("HU_EMOTION_REGISTER", "banana", 1);
    HU_ASSERT_EQ(hu_emotion_register_mode(), HU_GATE_OFF); /* unknown fails closed */
    unsetenv("HU_EMOTION_REGISTER");
}

static void absolute_rules_omit_register_when_gate_off(void) {
    make_tmpdir();
    write_card("emotest", card_json);
    unsetenv("HU_EMOTION_REGISTER");
    char buf[4096];
    size_t len = 0;
    build_casual_rules(buf, sizeof(buf), &len);
    HU_ASSERT_STR_NOT_CONTAINS(buf, "Emotional register");
    HU_ASSERT_STR_NOT_CONTAINS(buf, "62%");
    HU_ASSERT_STR_CONTAINS(buf, "You are HUMAN");
    cleanup_tmpdir();
}

static void absolute_rules_omit_register_in_shadow(void) {
    make_tmpdir();
    write_card("emotest", card_json);
    setenv("HU_EMOTION_REGISTER", "shadow", 1);
    char buf[4096];
    size_t len = 0;
    build_casual_rules(buf, sizeof(buf), &len);
    HU_ASSERT_STR_NOT_CONTAINS(buf, "Emotional register");
    cleanup_tmpdir();
}

static void absolute_rules_append_register_when_live_and_fit_production_buffer(void) {
    make_tmpdir();
    write_card("emotest", card_json);
    setenv("HU_EMOTION_REGISTER", "live", 1);
    char buf[4096];
    size_t len = 0;
    build_casual_rules(buf, sizeof(buf), &len);
    HU_ASSERT_STR_CONTAINS(buf, "14. Emotional register is MEASURED (240 of your texts)");
    HU_ASSERT_STR_CONTAINS(buf, "62%");
    /* Appended after rule 13, so the numbering stays monotonic. */
    const char *r13 = strstr(buf, "13. One topic per message");
    const char *r14 = strstr(buf, "14. Emotional register");
    HU_ASSERT_NOT_NULL(r13);
    HU_ASSERT_NOT_NULL(r14);
    HU_ASSERT_TRUE(r13 < r14);
    /* agent_turn.c and daemon_proactive.c hand this builder a 2048-byte
     * buffer; the live rule must still fit with room to spare. */
    HU_ASSERT_TRUE(len + 1 <= 2048);
    cleanup_tmpdir();
}

static void absolute_rules_live_without_card_render_nothing(void) {
    make_tmpdir(); /* no card */
    setenv("HU_EMOTION_REGISTER", "live", 1);
    char buf[4096];
    size_t len = 0;
    build_casual_rules(buf, sizeof(buf), &len);
    HU_ASSERT_STR_NOT_CONTAINS(buf, "Emotional register");
    cleanup_tmpdir();
}

static void formal_register_never_carries_the_rule(void) {
    make_tmpdir();
    write_card("emotest", card_json);
    setenv("HU_EMOTION_REGISTER", "live", 1);
    hu_persona_t p;
    memset(&p, 0, sizeof(p));
    p.name = "emotest";
    p.name_len = 7;
    char buf[4096];
    size_t len = 0;
    HU_ASSERT_EQ(hu_persona_build_absolute_rules_fmt(&p, "professional", buf, sizeof(buf), &len),
                 HU_OK);
    HU_ASSERT_STR_NOT_CONTAINS(buf, "Emotional register");
    cleanup_tmpdir();
}

void run_emotion_card_tests(void) {
    HU_TEST_SUITE("emotion_card");
    HU_RUN_TEST(parse_v1_card_reads_axes_top_and_provenance);
    HU_RUN_TEST(parse_rejects_missing_axis_zero_n_and_out_of_range);
    HU_RUN_TEST(parse_skips_malformed_top_entries);
    HU_RUN_TEST(parse_rejects_malformed_json);
    HU_RUN_TEST(load_missing_card_returns_not_found);
    HU_RUN_TEST(resolve_reads_card_from_persona_dir);
    HU_RUN_TEST(render_rule_states_card_numbers);
    HU_RUN_TEST(render_without_top_says_rare_and_refuses_unmeasured);
    HU_RUN_TEST(gate_defaults_off_and_parses_the_three_states);
    HU_RUN_TEST(absolute_rules_omit_register_when_gate_off);
    HU_RUN_TEST(absolute_rules_omit_register_in_shadow);
    HU_RUN_TEST(absolute_rules_append_register_when_live_and_fit_production_buffer);
    HU_RUN_TEST(absolute_rules_live_without_card_render_nothing);
    HU_RUN_TEST(formal_register_never_carries_the_rule);
}
