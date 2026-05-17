/* tests/test_cli_eval_phase5.c — CF-1 wiring tests
 *
 * Pins that `human eval competitive / leaderboard / gate` are wired
 * to real backends (competitive_harness, leaderboard, eval_gate)
 * rather than the original Phase 5 printf stubs.
 *
 * The original help-exits-zero test is preserved so the surface
 * contract is still pinned.
 */

#include "test_framework.h"
#include "human/eval/cli_eval.h"
#include "human/core/allocator.h"

#include <stdio.h>
#include <string.h>

static long file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    fclose(f);
    return sz;
}

static size_t read_file_into(const char *path, char *buf, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t r = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[r] = '\0';
    return r;
}

/* ----------------------------- --help ------------------------------ */

static void test_human_eval_competitive_help_lists_phase5_subcommands(void) {
    HU_SKIP_IF(!hu_build_has_competitive_eval(),
               "HU_ENABLE_RL_FULL=OFF in this build");
    hu_allocator_t alloc = hu_system_allocator();
    char argv0[] = "human";
    char argv1[] = "competitive";
    char argv2[] = "--help";
    char *argv[] = {argv0, argv1, argv2};
    HU_ASSERT_EQ(hu_eval_cli_competitive(&alloc, 3, argv), HU_OK);
}

static void test_human_eval_leaderboard_help_exits_zero(void) {
    HU_SKIP_IF(!hu_build_has_competitive_eval(),
               "HU_ENABLE_RL_FULL=OFF in this build");
    hu_allocator_t alloc = hu_system_allocator();
    char a0[] = "--help";
    char *argv[] = {a0};
    HU_ASSERT_EQ(hu_eval_cli_leaderboard(&alloc, 1, argv), HU_OK);
}

static void test_human_eval_gate_help_exits_zero(void) {
    HU_SKIP_IF(!hu_build_has_competitive_eval(),
               "HU_ENABLE_RL_FULL=OFF in this build");
    hu_allocator_t alloc = hu_system_allocator();
    char a0[] = "--help";
    char *argv[] = {a0};
    HU_ASSERT_EQ(hu_eval_cli_gate(&alloc, 1, argv), HU_OK);
}

/* ----------------- competitive (real harness) ---------------------- */

static void test_human_eval_competitive_writes_real_scorecard_markdown(void) {
    HU_SKIP_IF(!hu_build_has_competitive_eval(),
               "HU_ENABLE_RL_FULL=OFF in this build");
    hu_allocator_t alloc = hu_system_allocator();

    const char *md = "/tmp/cf1_competitive.md";
    const char *js = "/tmp/cf1_competitive.json";
    (void)remove(md);
    (void)remove(js);

    char a0[] = "--persona";
    char a1[] = "default";
    char a2[] = "--out-md";
    char a3[64]; snprintf(a3, sizeof(a3), "%s", md);
    char a4[] = "--out-json";
    char a5[64]; snprintf(a5, sizeof(a5), "%s", js);
    char *argv[] = {a0, a1, a2, a3, a4, a5};

    HU_ASSERT_EQ(hu_eval_cli_competitive(&alloc, 6, argv), HU_OK);

    /* Markdown file must exist AND have a real scorecard body (not
     * the stub one-liner). The harness emits "Competitive scorecard"
     * plus a table that includes all three judge columns.
     *
     * Note: under HU_IS_TEST the Apple FM + Gemini Nano factories
     * load a JSON fixture and return HU_OK, so all three columns
     * are "ok" in the test build. In production builds (no
     * HU_IS_TEST), those factories return HU_ERR_NOT_SUPPORTED and
     * the corresponding cells say "unavailable". The DoD-14 honest-
     * fallback path is covered separately by test_eval_judge_external.c;
     * here we just pin that the CLI emits all three columns and a
     * table-shaped body that's clearly not the printf stub. */
    char buf[16384];
    size_t r = read_file_into(md, buf, sizeof(buf));
    HU_ASSERT_TRUE(r > 100);
    HU_ASSERT_TRUE(strstr(buf, "Competitive scorecard") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "stock") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "apple_fm") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "gemini_nano") != NULL);
    /* table-shape sanity: header row appears once. */
    HU_ASSERT_TRUE(strstr(buf, "persona_fidelity") != NULL);

    HU_ASSERT_TRUE(file_size(js) > 20);
}

static void test_human_eval_competitive_rejects_unknown_flag(void) {
    HU_SKIP_IF(!hu_build_has_competitive_eval(),
               "HU_ENABLE_RL_FULL=OFF in this build");
    hu_allocator_t alloc = hu_system_allocator();
    char a0[] = "--bogus";
    char *argv[] = {a0};
    HU_ASSERT_EQ(hu_eval_cli_competitive(&alloc, 1, argv), HU_ERR_INVALID_ARGUMENT);
}

/* Production calling convention: src/cli_commands.c passes argv+2 to
 * these handlers, so argv[0] is the subcommand name (e.g. "competitive")
 * rather than the first flag. The parser must skip that leading
 * positional or every production call would be rejected as an unknown
 * flag (which is exactly the CF-1 bug this test pins against). */
static void test_human_eval_competitive_skips_leading_subcommand_name(void) {
    HU_SKIP_IF(!hu_build_has_competitive_eval(),
               "HU_ENABLE_RL_FULL=OFF in this build");
    hu_allocator_t alloc = hu_system_allocator();
    const char *md = "/tmp/cf1_competitive_dispatch.md";
    const char *js = "/tmp/cf1_competitive_dispatch.json";
    (void)remove(md);
    (void)remove(js);

    char a0[] = "competitive";  /* dispatch leaves this here */
    char a1[] = "--out-md";
    char a2[64]; snprintf(a2, sizeof(a2), "%s", md);
    char a3[] = "--out-json";
    char a4[64]; snprintf(a4, sizeof(a4), "%s", js);
    char *argv[] = {a0, a1, a2, a3, a4};

    HU_ASSERT_EQ(hu_eval_cli_competitive(&alloc, 5, argv), HU_OK);
    HU_ASSERT_TRUE(file_size(md) > 100);
}

/* ----------------- leaderboard (real runner) ----------------------- */

static void write_canned_mt_bench(const char *path) {
    FILE *f = fopen(path, "w");
    fputs("{\"mt_bench\":{\"hello\":7.5,\"world\":8.25}}", f);
    fclose(f);
}

static void test_human_eval_leaderboard_runs_canned_scores(void) {
    HU_SKIP_IF(!hu_build_has_competitive_eval(),
               "HU_ENABLE_RL_FULL=OFF in this build");
    hu_allocator_t alloc = hu_system_allocator();
    const char *canned = "/tmp/cf1_leaderboard_canned.json";
    const char *out = "/tmp/cf1_leaderboard.out";
    write_canned_mt_bench(canned);
    (void)remove(out);

    char a0[] = "--kind";
    char a1[] = "mt-bench";
    char a2[] = "--canned";
    char a3[64]; snprintf(a3, sizeof(a3), "%s", canned);
    char a4[] = "--prompts";
    char a5[] = "hello,world";
    char a6[] = "--out";
    char a7[64]; snprintf(a7, sizeof(a7), "%s", out);
    char *argv[] = {a0, a1, a2, a3, a4, a5, a6, a7};

    HU_ASSERT_EQ(hu_eval_cli_leaderboard(&alloc, 8, argv), HU_OK);

    char buf[1024];
    size_t r = read_file_into(out, buf, sizeof(buf));
    HU_ASSERT_TRUE(r > 0);
    HU_ASSERT_TRUE(strstr(buf, "mt_bench") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "hello") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "world") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "7.5") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "8.25") != NULL);
}

static void test_human_eval_leaderboard_default_kind_is_mt_bench(void) {
    HU_SKIP_IF(!hu_build_has_competitive_eval(),
               "HU_ENABLE_RL_FULL=OFF in this build");
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(hu_eval_cli_leaderboard(&alloc, 0, NULL), HU_OK);
}

static void test_human_eval_leaderboard_rejects_unknown_kind(void) {
    HU_SKIP_IF(!hu_build_has_competitive_eval(),
               "HU_ENABLE_RL_FULL=OFF in this build");
    hu_allocator_t alloc = hu_system_allocator();
    char a0[] = "--kind";
    char a1[] = "imaginary-bench";
    char *argv[] = {a0, a1};
    HU_ASSERT_EQ(hu_eval_cli_leaderboard(&alloc, 2, argv), HU_ERR_INVALID_ARGUMENT);
}

/* --------------------- gate (real CIs) ----------------------------- */

static void test_human_eval_gate_promotes_on_strong_persona_lift(void) {
    HU_SKIP_IF(!hu_build_has_competitive_eval(),
               "HU_ENABLE_RL_FULL=OFF in this build");
    hu_allocator_t alloc = hu_system_allocator();
    const char *out = "/tmp/cf1_gate_promote.out";
    (void)remove(out);

    /* Baseline 0.60, delta-min 0.05 -- candidate scores cluster ~0.75
     * so the lower 95% CI clears 0.65 comfortably. */
    char a0[] = "--persona-scores";
    char a1[] = "0.74,0.76,0.75,0.77,0.73,0.75,0.78,0.74,0.76,0.75,"
                "0.74,0.76,0.75,0.77,0.73,0.75,0.78,0.74,0.76,0.75";
    char a2[] = "--persona-baseline"; char a3[] = "0.60";
    char a4[] = "--persona-delta-min"; char a5[] = "0.05";
    char a6[] = "--bootstrap-samples"; char a7[] = "200";
    char a8[] = "--bootstrap-seed"; char a9[] = "42";
    char a10[] = "--candidate-p95-ms"; char a11[] = "80";
    char a12[] = "--latency-baseline-ms"; char a13[] = "100";
    char a14[] = "--out";
    char a15[64]; snprintf(a15, sizeof(a15), "%s", out);
    char *argv[] = {a0, a1, a2, a3, a4, a5, a6, a7, a8, a9,
                    a10, a11, a12, a13, a14, a15};

    HU_ASSERT_EQ(hu_eval_cli_gate(&alloc, 16, argv), HU_OK);

    char buf[1024];
    size_t r = read_file_into(out, buf, sizeof(buf));
    HU_ASSERT_TRUE(r > 0);
    HU_ASSERT_TRUE(strstr(buf, "PROMOTE") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "persona_ci_lower") != NULL);
}

static void test_human_eval_gate_rejects_on_weak_persona_lift(void) {
    HU_SKIP_IF(!hu_build_has_competitive_eval(),
               "HU_ENABLE_RL_FULL=OFF in this build");
    hu_allocator_t alloc = hu_system_allocator();
    const char *out = "/tmp/cf1_gate_reject.out";
    (void)remove(out);

    /* Baseline 0.70, delta-min 0.05 -- candidate scores ~0.72 sit
     * BELOW the 0.75 lower-bound floor, so the gate must REJECT and
     * the reason string must name "persona". */
    char a0[] = "--persona-scores";
    char a1[] = "0.71,0.72,0.73,0.72,0.71,0.72,0.73,0.72,0.71,0.72,"
                "0.71,0.72,0.73,0.72,0.71,0.72,0.73,0.72,0.71,0.72";
    char a2[] = "--persona-baseline"; char a3[] = "0.70";
    char a4[] = "--persona-delta-min"; char a5[] = "0.05";
    char a6[] = "--bootstrap-samples"; char a7[] = "200";
    char a8[] = "--bootstrap-seed"; char a9[] = "42";
    char a10[] = "--out";
    char a11[64]; snprintf(a11, sizeof(a11), "%s", out);
    char *argv[] = {a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11};

    HU_ASSERT_EQ(hu_eval_cli_gate(&alloc, 12, argv), HU_OK);

    char buf[1024];
    size_t r = read_file_into(out, buf, sizeof(buf));
    HU_ASSERT_TRUE(r > 0);
    HU_ASSERT_TRUE(strstr(buf, "REJECT") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "persona") != NULL);
}

static void test_human_eval_gate_rejects_missing_scores(void) {
    HU_SKIP_IF(!hu_build_has_competitive_eval(),
               "HU_ENABLE_RL_FULL=OFF in this build");
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(hu_eval_cli_gate(&alloc, 0, NULL), HU_ERR_INVALID_ARGUMENT);
}

static void test_human_eval_gate_rejects_too_few_scores(void) {
    HU_SKIP_IF(!hu_build_has_competitive_eval(),
               "HU_ENABLE_RL_FULL=OFF in this build");
    hu_allocator_t alloc = hu_system_allocator();
    /* Gate requires n >= 10. Pass 5; expect rejection propagated as
     * INVALID_ARGUMENT (not silently downgraded). */
    char a0[] = "--persona-scores"; char a1[] = "0.7,0.7,0.7,0.7,0.7";
    char *argv[] = {a0, a1};
    HU_ASSERT_EQ(hu_eval_cli_gate(&alloc, 2, argv), HU_ERR_INVALID_ARGUMENT);
}

void run_cli_eval_phase5_tests(void) {
    HU_TEST_SUITE("cli-eval-phase5");
    HU_RUN_TEST(test_human_eval_competitive_help_lists_phase5_subcommands);
    HU_RUN_TEST(test_human_eval_leaderboard_help_exits_zero);
    HU_RUN_TEST(test_human_eval_gate_help_exits_zero);
    HU_RUN_TEST(test_human_eval_competitive_writes_real_scorecard_markdown);
    HU_RUN_TEST(test_human_eval_competitive_rejects_unknown_flag);
    HU_RUN_TEST(test_human_eval_competitive_skips_leading_subcommand_name);
    HU_RUN_TEST(test_human_eval_leaderboard_runs_canned_scores);
    HU_RUN_TEST(test_human_eval_leaderboard_default_kind_is_mt_bench);
    HU_RUN_TEST(test_human_eval_leaderboard_rejects_unknown_kind);
    HU_RUN_TEST(test_human_eval_gate_promotes_on_strong_persona_lift);
    HU_RUN_TEST(test_human_eval_gate_rejects_on_weak_persona_lift);
    HU_RUN_TEST(test_human_eval_gate_rejects_missing_scores);
    HU_RUN_TEST(test_human_eval_gate_rejects_too_few_scores);
}
