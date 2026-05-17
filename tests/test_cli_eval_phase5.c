#include "test_framework.h"
#include "human/eval/cli_eval.h"
#include "human/core/allocator.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

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

static void write_prompt_fixture(const char *path) {
    FILE *f = fopen(path, "w");
    HU_ASSERT_NOT_NULL(f);
    for (int i = 0; i < 10; i++)
        fprintf(f, "fixture prompt %d\n", i);
    fclose(f);
}

static void test_competitive_emits_scorecard_with_bootstrap_cis(void) {
    HU_SKIP_IF(!hu_build_has_competitive_eval(), "RL_FULL off");
    hu_allocator_t alloc = hu_system_allocator();
    char fixture[256], json_path[256];
    snprintf(fixture, sizeof(fixture), "/tmp/hu-comp-fixture-%d.txt", (int)getpid());
    snprintf(json_path, sizeof(json_path), "/tmp/hu-comp-scorecard-%d.json", (int)getpid());
    write_prompt_fixture(fixture);

    char argv0[] = "human";
    char argv1[] = "competitive";
    char argv2[] = "--persona";
    char argv3[] = "default";
    char argv4[] = "--prompts";
    char argv5[256];
    snprintf(argv5, sizeof(argv5), "%s", fixture);
    char argv6[] = "--out-json";
    char argv7[256];
    snprintf(argv7, sizeof(argv7), "%s", json_path);
    char *argv[] = {argv0, argv1, argv2, argv3, argv4, argv5, argv6, argv7};
    HU_ASSERT_EQ(hu_eval_cli_competitive(&alloc, 8, argv), HU_OK);

    char buf[8192];
    FILE *f = fopen(json_path, "r");
    HU_ASSERT_NOT_NULL(f);
    size_t rd = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[rd] = '\0';
    HU_ASSERT_TRUE(strstr(buf, "ci_lower") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "ci_upper") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "win_condition_met") != NULL);
    unlink(fixture);
    unlink(json_path);
}

static void test_competitive_renders_unavailable_columns_with_reason(void) {
    HU_SKIP_IF(!hu_build_has_competitive_eval(), "RL_FULL off");
    hu_allocator_t alloc = hu_system_allocator();
    char fixture[256], json_path[256];
    snprintf(fixture, sizeof(fixture), "/tmp/hu-comp-reason-fixture-%d.txt", (int)getpid());
    snprintf(json_path, sizeof(json_path), "/tmp/hu-comp-reason-scorecard-%d.json", (int)getpid());
    write_prompt_fixture(fixture);

    char argv0[] = "human";
    char argv1[] = "competitive";
    char argv2[] = "--persona";
    char argv3[] = "default";
    char argv4[] = "--prompts";
    char argv5[256];
    snprintf(argv5, sizeof(argv5), "%s", fixture);
    char argv6[] = "--out-json";
    char argv7[256];
    snprintf(argv7, sizeof(argv7), "%s", json_path);
    char *argv[] = {argv0, argv1, argv2, argv3, argv4, argv5, argv6, argv7};
    HU_ASSERT_EQ(hu_eval_cli_competitive(&alloc, 8, argv), HU_OK);

    char buf[8192];
    FILE *f = fopen(json_path, "r");
    HU_ASSERT_NOT_NULL(f);
    size_t rd = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[rd] = '\0';
    HU_ASSERT_TRUE(strstr(buf, "unavailable_reason") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "\"available\":false") != NULL);
    unlink(fixture);
    unlink(json_path);
}

static void test_competitive_literal_spec9_form_works(void) {
    HU_SKIP_IF(!hu_build_has_competitive_eval(), "RL_FULL off");
    hu_allocator_t alloc = hu_system_allocator();
    char fixture[256], json_path[256], adapter[256];
    snprintf(fixture, sizeof(fixture), "/tmp/hu-comp-spec9-fixture-%d.txt", (int)getpid());
    snprintf(json_path, sizeof(json_path), "/tmp/hu-comp-spec9-scorecard-%d.json", (int)getpid());
    snprintf(adapter, sizeof(adapter), "/tmp/test.adapter-%d", (int)getpid());
    write_prompt_fixture(fixture);
    FILE *af = fopen(adapter, "w");
    HU_ASSERT_NOT_NULL(af);
    fputs("stub\n", af);
    fclose(af);

    char argv0[] = "human";
    char argv1[] = "competitive";
    char argv2[] = "--persona";
    char argv3[] = "seth";
    char argv4[] = "--adapter";
    char argv5[256];
    snprintf(argv5, sizeof(argv5), "%s", adapter);
    char argv6[] = "--prompts";
    char argv7[256];
    snprintf(argv7, sizeof(argv7), "%s", fixture);
    char argv8[] = "--out-json";
    char argv9[256];
    snprintf(argv9, sizeof(argv9), "%s", json_path);
    char *argv[] = {argv0, argv1, argv2, argv3, argv4, argv5, argv6, argv7, argv8, argv9};
    HU_ASSERT_EQ(hu_eval_cli_competitive(&alloc, 10, argv), HU_OK);
    HU_ASSERT_TRUE(access(json_path, F_OK) == 0);

    unlink(fixture);
    unlink(json_path);
    unlink(adapter);
}

static void test_gate_emits_verdict_json_from_csv_inputs(void) {
    HU_SKIP_IF(!hu_build_has_competitive_eval(), "RL_FULL off");
    hu_allocator_t alloc = hu_system_allocator();
    char out[256];
    snprintf(out, sizeof(out), "/tmp/hu-gate-verdict-%d.json", (int)getpid());
    char scores[] =
        "0.7,0.75,0.8,0.82,0.83,0.84,0.85,0.86,0.87,0.88,0.89,0.90";
    char argv0[] = "human";
    char argv1[] = "gate";
    char argv2[] = "--persona-scores";
    char argv3[] = "--out";
    char *argv[] = {argv0, argv1, argv2, scores, argv3, out};
    HU_ASSERT_EQ(hu_eval_cli_gate(&alloc, 6, argv), HU_OK);
    char buf[1024];
    FILE *f = fopen(out, "r");
    HU_ASSERT_NOT_NULL(f);
    size_t rd = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[rd] = '\0';
    HU_ASSERT_TRUE(strstr(buf, "persona_ci_lower") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "promote") != NULL);
    unlink(out);
}

static void test_leaderboard_canned_run_emits_scores(void) {
    HU_SKIP_IF(!hu_build_has_competitive_eval(), "RL_FULL off");
    hu_allocator_t alloc = hu_system_allocator();
    char canned[256], out[256];
    snprintf(canned, sizeof(canned), "/tmp/hu-lb-canned-%d.json", (int)getpid());
    snprintf(out, sizeof(out), "/tmp/hu-lb-out-%d.json", (int)getpid());
    FILE *f = fopen(canned, "w");
    HU_ASSERT_NOT_NULL(f);
    fputs("{\"mt_bench\":{\"hello\":0.9,\"world\":0.8}}\n", f);
    fclose(f);

    char argv0[] = "human";
    char argv1[] = "leaderboard";
    char argv2[] = "--kind";
    char argv3[] = "mt-bench";
    char argv4[] = "--canned";
    char argv5[256];
    snprintf(argv5, sizeof(argv5), "%s", canned);
    char argv6[] = "--prompts";
    char argv7[] = "hello,world";
    char argv8[] = "--out";
    char argv9[256];
    snprintf(argv9, sizeof(argv9), "%s", out);
    char *argv[] = {argv0, argv1, argv2, argv3, argv4, argv5, argv6, argv7, argv8, argv9};
    HU_ASSERT_EQ(hu_eval_cli_leaderboard(&alloc, 10, argv), HU_OK);

    char buf[512];
    f = fopen(out, "r");
    HU_ASSERT_NOT_NULL(f);
    size_t rd = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[rd] = '\0';
    HU_ASSERT_TRUE(strstr(buf, "\"kind\"") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "\"scores\"") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "\"mean\"") != NULL);
    unlink(canned);
    unlink(out);
}

static void test_unknown_flag_returns_invalid_argument(void) {
    HU_SKIP_IF(!hu_build_has_competitive_eval(), "RL_FULL off");
    hu_allocator_t alloc = hu_system_allocator();
    char argv0[] = "human";
    char argv1[] = "gate";
    char argv2[] = "--not-a-real-flag";
    char *argv[] = {argv0, argv1, argv2};
    HU_ASSERT_EQ(hu_eval_cli_gate(&alloc, 3, argv), HU_ERR_INVALID_ARGUMENT);
}

static void test_missing_required_flag_returns_invalid_argument(void) {
    HU_SKIP_IF(!hu_build_has_competitive_eval(), "RL_FULL off");
    hu_allocator_t alloc = hu_system_allocator();
    char argv0[] = "human";
    char argv1[] = "gate";
    char *argv[] = {argv0, argv1};
    HU_ASSERT_EQ(hu_eval_cli_gate(&alloc, 2, argv), HU_ERR_INVALID_ARGUMENT);
}

void run_cli_eval_phase5_tests(void) {
    HU_TEST_SUITE("cli-eval-phase5");
    HU_RUN_TEST(test_human_eval_competitive_help_lists_phase5_subcommands);
    HU_RUN_TEST(test_competitive_emits_scorecard_with_bootstrap_cis);
    HU_RUN_TEST(test_competitive_renders_unavailable_columns_with_reason);
    HU_RUN_TEST(test_competitive_literal_spec9_form_works);
    HU_RUN_TEST(test_gate_emits_verdict_json_from_csv_inputs);
    HU_RUN_TEST(test_leaderboard_canned_run_emits_scores);
    HU_RUN_TEST(test_unknown_flag_returns_invalid_argument);
    HU_RUN_TEST(test_missing_required_flag_returns_invalid_argument);
}
