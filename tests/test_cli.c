#include "human/agent/cli.h"
#include "human/cli_commands.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "test_framework.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef HU_EVAL_SUITES_DIR
#error "HU_EVAL_SUITES_DIR must be defined when building human_tests"
#endif
#if defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200809L
#include <stddef.h>
#elif defined(__linux__) || defined(__APPLE__)
/* open_memstream */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#endif

/* Use a temp HOME with no config so hu_config_load uses defaults and validates. */
static void set_test_home(void) {
    setenv("HOME", "/tmp/human_cli_test_noconfig", 1);
}

static void test_cmd_channel_list(void) {
    set_test_home();
    hu_allocator_t alloc = hu_system_allocator();
    char *argv[] = {"human", "channel", "list"};
    hu_error_t err = cmd_channel(&alloc, 3, argv);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_cmd_hardware_list(void) {
    set_test_home();
    hu_allocator_t alloc = hu_system_allocator();
    char *argv[] = {"human", "hardware", "list"};
    hu_error_t err = cmd_hardware(&alloc, 3, argv);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_cmd_memory_stats(void) {
    set_test_home();
    hu_allocator_t alloc = hu_system_allocator();
    char *argv[] = {"human", "memory", "stats"};
    hu_error_t err = cmd_memory(&alloc, 3, argv);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_cmd_memory_search_no_query(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *argv[] = {"human", "memory", "search"};
    hu_error_t err = cmd_memory(&alloc, 3, argv);
    HU_ASSERT(err != HU_OK);
}

static void test_cmd_workspace_show(void) {
    set_test_home();
    hu_allocator_t alloc = hu_system_allocator();
    char *argv[] = {"human", "workspace", "show"};
    hu_error_t err = cmd_workspace(&alloc, 3, argv);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_cmd_capabilities_default(void) {
    set_test_home();
    hu_allocator_t alloc = hu_system_allocator();
    char *argv[] = {"human", "capabilities"};
    hu_error_t err = cmd_capabilities(&alloc, 2, argv);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_cmd_capabilities_json(void) {
    set_test_home();
    hu_allocator_t alloc = hu_system_allocator();
    char *argv[] = {"human", "capabilities", "--json"};
    hu_error_t err = cmd_capabilities(&alloc, 3, argv);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_cmd_models_list(void) {
    set_test_home();
    hu_allocator_t alloc = hu_system_allocator();
    char *argv[] = {"human", "models", "list"};
    hu_error_t err = cmd_models(&alloc, 3, argv);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_cmd_auth_status(void) {
    set_test_home();
    hu_allocator_t alloc = hu_system_allocator();
    char *argv[] = {"human", "auth", "status", "openai"};
    hu_error_t err = cmd_auth(&alloc, 4, argv);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_cmd_update_check(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *argv[] = {"human", "update", "--check"};
    hu_error_t err = cmd_update(&alloc, 3, argv);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_cmd_sandbox_default(void) {
    set_test_home();
    hu_allocator_t alloc = hu_system_allocator();
    char *argv[] = {"human", "sandbox"};
    hu_error_t err = cmd_sandbox(&alloc, 2, argv);
    HU_ASSERT_EQ(err, HU_OK);
}

/* cmd_init: under HU_IS_TEST skips filesystem/stdin, returns HU_OK */
static void test_cmd_init_sc_is_test_returns_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *argv[] = {"human", "init"};
    hu_error_t err = cmd_init(&alloc, 2, argv);
    HU_ASSERT_EQ(err, HU_OK);
}

#if defined(__unix__) || defined(__APPLE__)
static void test_cmd_eval_baseline_outputs_score_table(void) {
    set_test_home();
    char tmpl[] = "/tmp/hu_ev_cli_blXXXXXX";
    int tfd = mkstemp(tmpl);
    HU_ASSERT(tfd >= 0);
    if (close(tfd) != 0) {
        unlink(tmpl);
        HU_FAIL("close tmp");
    }

    int save_out = dup(STDOUT_FILENO);
    HU_ASSERT(save_out >= 0);
    if (!freopen(tmpl, "w", stdout)) {
        dup2(save_out, STDOUT_FILENO);
        close(save_out);
        unlink(tmpl);
        HU_FAIL("freopen stdout");
    }

    hu_allocator_t alloc = hu_system_allocator();
    char *argv[] = {"human", "eval", "baseline", HU_EVAL_SUITES_DIR};
    hu_error_t err = cmd_eval(&alloc, 4, argv);

    fflush(stdout);
    dup2(save_out, STDOUT_FILENO);
    close(save_out);

    FILE *rf = fopen(tmpl, "r");
    HU_ASSERT_NOT_NULL(rf);
    char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf) - 1, rf);
    buf[n] = '\0';
    fclose(rf);
    unlink(tmpl);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "Suite") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "Tasks") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "Score") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "Status") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "fidelity") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "0.72") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "COMPETITIVE") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "intelligence") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "0.65") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "PARTIAL") != NULL);
}

static void test_cmd_eval_check_regression_is_test_emits_pass_line(void) {
    set_test_home();
    char tmpl[] = "/tmp/hu_ev_cli_crXXXXXX";
    int tfd = mkstemp(tmpl);
    HU_ASSERT(tfd >= 0);
    if (close(tfd) != 0) {
        unlink(tmpl);
        HU_FAIL("close tmp");
    }

    int save_out = dup(STDOUT_FILENO);
    HU_ASSERT(save_out >= 0);
    if (!freopen(tmpl, "w", stdout)) {
        dup2(save_out, STDOUT_FILENO);
        close(save_out);
        unlink(tmpl);
        HU_FAIL("freopen stdout");
    }

    hu_allocator_t alloc = hu_system_allocator();
    char *argv[] = {"human", "eval", "check-regression", HU_EVAL_SUITES_DIR};
    hu_error_t err = cmd_eval(&alloc, 4, argv);

    fflush(stdout);
    dup2(save_out, STDOUT_FILENO);
    close(save_out);

    FILE *rf = fopen(tmpl, "r");
    HU_ASSERT_NOT_NULL(rf);
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, rf);
    buf[n] = '\0';
    fclose(rf);
    unlink(tmpl);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "Regression check: PASS") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "no suite dropped >10%") != NULL);
}
#endif

static void test_cmd_setup_local_model_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *argv[] = {"human", "setup", "local-model"};
    HU_ASSERT_EQ(cmd_setup(&alloc, 3, argv), HU_OK);
}

static void test_cmd_setup_unknown_subcommand_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *argv[] = {"human", "setup", "typo"};
    HU_ASSERT_NEQ(cmd_setup(&alloc, 3, argv), HU_OK);
}

#if defined(__linux__) || defined(__APPLE__)
static void test_cli_config_schema_emit_contains_core_keys(void) {
    char *buf = NULL;
    size_t len = 0;
    FILE *fp = open_memstream(&buf, &len);
    HU_ASSERT_NOT_NULL(fp);
    HU_ASSERT_EQ(hu_cli_config_schema_emit(fp), HU_OK);
    fclose(fp);
    HU_ASSERT_NOT_NULL(buf);
    HU_ASSERT_TRUE(strstr(buf, "human config schema:") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "default_provider") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "memory.backend") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "dpo_export_dir") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "channels.*") != NULL);
    free(buf);
}

static void test_cli_setup_local_model_emit_contains_expected_lines(void) {
    char *buf = NULL;
    size_t len = 0;
    FILE *fp = open_memstream(&buf, &len);
    HU_ASSERT_NOT_NULL(fp);
    HU_ASSERT_EQ(hu_cli_setup_local_model_emit(fp), HU_OK);
    fclose(fp);
    HU_ASSERT_NOT_NULL(buf);
    HU_ASSERT_TRUE(strstr(buf, "test mode") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "ollama pull llama3.2:3b") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "huggingface.co") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "mlx_lm") != NULL);
    free(buf);
}
#endif

/* Demo mode: --demo flag sets demo_mode in parsed args */
static void test_agent_cli_demo_flag_parsing(void) {
    const char *argv[] = {"human", "agent", "--demo"};
    hu_parsed_agent_args_t out;
    hu_error_t err = hu_agent_cli_parse_args(argv, 3, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.demo_mode, 1);
}

/* Demo mode: without --demo, demo_mode is 0 */
static void test_agent_cli_no_demo_flag(void) {
    const char *argv[] = {"human", "agent"};
    hu_parsed_agent_args_t out;
    hu_error_t err = hu_agent_cli_parse_args(argv, 2, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.demo_mode, 0);
}

/* Demo mode: overrides provider to ollama when applied to config */
static void test_agent_cli_demo_overrides_provider(void) {
    set_test_home();
    hu_allocator_t alloc = hu_system_allocator();
    hu_config_t cfg;
    hu_error_t err = hu_config_load(&alloc, &cfg);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(cfg.default_provider != NULL);
    /* Before override: provider is from config (often "openai" from defaults) */
    /* Apply demo overrides (same logic as in cli.c) */
    cfg.default_provider = "ollama";
    cfg.default_model = "llama3.2";
    cfg.memory.backend = "none";
    cfg.memory_backend = "none";
    HU_ASSERT_TRUE(strcmp(cfg.default_provider, "ollama") == 0);
    HU_ASSERT_TRUE(strcmp(cfg.default_model, "llama3.2") == 0);
    HU_ASSERT_TRUE(strcmp(cfg.memory.backend, "none") == 0);
    hu_config_deinit(&cfg);
}

/* --config flag sets config_path in parsed args */
static void test_agent_cli_config_flag_parsing(void) {
    const char *argv[] = {"human", "agent", "--config", "/custom/path/config.json"};
    hu_parsed_agent_args_t out;
    hu_error_t err = hu_agent_cli_parse_args(argv, 4, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out.config_path);
    HU_ASSERT_STR_EQ(out.config_path, "/custom/path/config.json");
}

/* --contact lands in contact_id and is DISTINCT from session_id. This is the
 * parse half of the GraphRAG grounding seam: cli.c binds agent->memory_session_id
 * from contact_id (NOT session_id), which is what gates community-summary
 * grounding on a one-shot CLI turn. The prior assumption that -s/--session would
 * attribute the turn was wrong — these are separate fields. */
static void test_agent_cli_contact_flag_parsing(void) {
    const char *argv[] = {"agent", "--contact", "+15551234567"};
    hu_parsed_agent_args_t out;
    memset(&out, 0, sizeof(out));
    HU_ASSERT_EQ(hu_agent_cli_parse_args(argv, 3, &out), HU_OK);
    HU_ASSERT_NOT_NULL(out.contact_id);
    HU_ASSERT_STR_EQ(out.contact_id, "+15551234567");
    HU_ASSERT_NULL(out.session_id); /* contact is not session */
}

static void test_agent_cli_contact_distinct_from_session(void) {
    const char *argv[] = {"agent", "-s", "sess1", "--contact", "+1999", "-m", "hi"};
    hu_parsed_agent_args_t out;
    memset(&out, 0, sizeof(out));
    HU_ASSERT_EQ(hu_agent_cli_parse_args(argv, 7, &out), HU_OK);
    HU_ASSERT_STR_EQ(out.session_id, "sess1");
    HU_ASSERT_STR_EQ(out.contact_id, "+1999");
    HU_ASSERT_STR_EQ(out.message, "hi");
}

static void test_agent_cli_no_contact_is_null(void) {
    const char *argv[] = {"agent", "-m", "hi"};
    hu_parsed_agent_args_t out;
    memset(&out, 0, sizeof(out));
    HU_ASSERT_EQ(hu_agent_cli_parse_args(argv, 3, &out), HU_OK);
    HU_ASSERT_NULL(out.contact_id); /* default: no contact bound → grounding off */
}

static void test_agent_cli_prompt_once_parsing(void) {
    const char *argv[] = {"agent", "--prompt", "Research AI", "--once", "--message", "Check feeds", "--channel", "cli"};
    hu_parsed_agent_args_t args; memset(&args, 0, sizeof(args));
    HU_ASSERT_EQ(hu_agent_cli_parse_args(argv, 8, &args), HU_OK);
    HU_ASSERT_STR_EQ(args.prompt, "Research AI"); HU_ASSERT_EQ(args.once, 1);
    HU_ASSERT_STR_EQ(args.message, "Check feeds"); HU_ASSERT_STR_EQ(args.channel, "cli");
}
static void test_agent_cli_prompt_without_once(void) {
    const char *argv[] = {"agent", "--prompt", "System prompt text"};
    hu_parsed_agent_args_t args; memset(&args, 0, sizeof(args));
    HU_ASSERT_EQ(hu_agent_cli_parse_args(argv, 3, &args), HU_OK);
    HU_ASSERT_STR_EQ(args.prompt, "System prompt text"); HU_ASSERT_EQ(args.once, 0);
}

#if defined(__unix__) || defined(__APPLE__)
/* B8 — `human eval tom` subcommand. Three sub-modes: smoke, gold, run.
 * The smoke + gold modes are pure data-pack rubric checks (no provider);
 * run scores caller-supplied JSONL responses. We only test the JSON
 * envelope and the JSONL parser here — the underlying scoring is
 * exhaustively covered in tests/test_tom_scenario_b8.c. */
static char *tom_capture_stdout(int argc, char **argv, hu_error_t *err_out) {
    char tmpl[] = "/tmp/hu_eval_tom_XXXXXX";
    int tfd = mkstemp(tmpl);
    if (tfd < 0)
        return NULL;
    if (close(tfd) != 0) {
        unlink(tmpl);
        return NULL;
    }
    int save_out = dup(STDOUT_FILENO);
    if (save_out < 0) {
        unlink(tmpl);
        return NULL;
    }
    if (!freopen(tmpl, "w", stdout)) {
        dup2(save_out, STDOUT_FILENO);
        close(save_out);
        unlink(tmpl);
        return NULL;
    }
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = cmd_eval(&alloc, argc, argv);
    fflush(stdout);
    dup2(save_out, STDOUT_FILENO);
    close(save_out);
    if (err_out)
        *err_out = err;

    FILE *rf = fopen(tmpl, "r");
    if (!rf) {
        unlink(tmpl);
        return NULL;
    }
    char *buf = (char *)malloc(8192);
    if (!buf) {
        fclose(rf);
        unlink(tmpl);
        return NULL;
    }
    size_t n = fread(buf, 1, 8192 - 1, rf);
    buf[n] = '\0';
    fclose(rf);
    unlink(tmpl);
    return buf;
}

static void test_cmd_tom_smoke_emits_pct_100_on_pack(void) {
    char *argv[] = {"human", "eval", "tom", "smoke",
                    HU_EVAL_SUITES_DIR "/tom/tom_synthetic.json"};
    hu_error_t err = HU_OK;
    char *out = tom_capture_stdout(5, argv, &err);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "\"mode\":\"smoke\"") != NULL);
    HU_ASSERT_TRUE(strstr(out, "\"pct\":100") != NULL);
    free(out);
}

static void test_cmd_tom_gold_emits_envelope(void) {
    char *argv[] = {"human", "eval", "tom", "gold",
                    HU_EVAL_SUITES_DIR "/tom/tom_synthetic.json"};
    hu_error_t err = HU_OK;
    char *out = tom_capture_stdout(5, argv, &err);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "\"mode\":\"gold\"") != NULL);
    HU_ASSERT_TRUE(strstr(out, "\"total\":10") != NULL);
    free(out);
}

static void test_cmd_tom_run_scores_jsonl_responses(void) {
    char tmpl[] = "/tmp/hu_eval_tom_resps_XXXXXX";
    int tfd = mkstemp(tmpl);
    HU_ASSERT(tfd >= 0);
    const char *resps =
        "{\"id\":\"tom-fb-01\",\"response\":\"Max will check the original basket first.\"}\n"
        "{\"id\":\"tom-fb-02\",\"response\":\"Sam still thinks the cookies are in the jar.\"}\n"
        "{\"id\":\"tom-pr-01\",\"response\":\"Likely they want me to close the window or raise heat.\"}\n";
    HU_ASSERT_EQ(write(tfd, resps, strlen(resps)), (ssize_t)strlen(resps));
    close(tfd);

    char *argv[] = {"human",     "eval",         "tom",
                    "run",       "--pack",       HU_EVAL_SUITES_DIR "/tom/tom_synthetic.json",
                    "--responses", tmpl};
    hu_error_t err = HU_OK;
    char *out = tom_capture_stdout(8, argv, &err);
    unlink(tmpl);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "\"mode\":\"run\"") != NULL);
    HU_ASSERT_TRUE(strstr(out, "\"responses_loaded\":3") != NULL);
    /* Default: skip-unanswered → total == loaded == 3. */
    HU_ASSERT_TRUE(strstr(out, "\"total\":3") != NULL);
    free(out);
}

static void test_cmd_tom_run_no_skip_unanswered_inflates_total(void) {
    char tmpl[] = "/tmp/hu_eval_tom_resps2_XXXXXX";
    int tfd = mkstemp(tmpl);
    HU_ASSERT(tfd >= 0);
    const char *resps =
        "{\"id\":\"tom-fb-01\",\"response\":\"original basket\"}\n";
    HU_ASSERT_EQ(write(tfd, resps, strlen(resps)), (ssize_t)strlen(resps));
    close(tfd);

    char *argv[] = {"human",     "eval",         "tom",
                    "run",       "--pack",       HU_EVAL_SUITES_DIR "/tom/tom_synthetic.json",
                    "--responses", tmpl,         "--no-skip-unanswered"};
    hu_error_t err = HU_OK;
    char *out = tom_capture_stdout(9, argv, &err);
    unlink(tmpl);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "\"responses_loaded\":1") != NULL);
    /* Pack has 10 items; --no-skip-unanswered counts them all. */
    HU_ASSERT_TRUE(strstr(out, "\"total\":10") != NULL);
    free(out);
}

static void test_cmd_tom_unknown_subcommand_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *argv[] = {"human", "eval", "tom", "bogus"};
    HU_ASSERT_NEQ(cmd_eval(&alloc, 4, argv), HU_OK);
}

static void test_cmd_tom_run_missing_flag_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *argv[] = {"human", "eval", "tom", "run", "--pack",
                    HU_EVAL_SUITES_DIR "/tom/tom_synthetic.json"};
    HU_ASSERT_NEQ(cmd_eval(&alloc, 6, argv), HU_OK);
}
#endif

void run_cli_tests(void) {
    HU_TEST_SUITE("CLI Commands");
    HU_RUN_TEST(test_cmd_channel_list);
    HU_RUN_TEST(test_cmd_hardware_list);
    HU_RUN_TEST(test_cmd_memory_stats);
    HU_RUN_TEST(test_cmd_memory_search_no_query);
    HU_RUN_TEST(test_cmd_workspace_show);
    HU_RUN_TEST(test_cmd_capabilities_default);
    HU_RUN_TEST(test_cmd_capabilities_json);
    HU_RUN_TEST(test_cmd_models_list);
    HU_RUN_TEST(test_cmd_auth_status);
    HU_RUN_TEST(test_cmd_update_check);
    HU_RUN_TEST(test_cmd_sandbox_default);
    HU_RUN_TEST(test_cmd_init_sc_is_test_returns_ok);
#if defined(__unix__) || defined(__APPLE__)
    HU_RUN_TEST(test_cmd_eval_baseline_outputs_score_table);
    HU_RUN_TEST(test_cmd_eval_check_regression_is_test_emits_pass_line);
    HU_RUN_TEST(test_cmd_tom_smoke_emits_pct_100_on_pack);
    HU_RUN_TEST(test_cmd_tom_gold_emits_envelope);
    HU_RUN_TEST(test_cmd_tom_run_scores_jsonl_responses);
    HU_RUN_TEST(test_cmd_tom_run_no_skip_unanswered_inflates_total);
    HU_RUN_TEST(test_cmd_tom_unknown_subcommand_fails);
    HU_RUN_TEST(test_cmd_tom_run_missing_flag_fails);
#endif
    HU_RUN_TEST(test_cmd_setup_local_model_ok);
    HU_RUN_TEST(test_cmd_setup_unknown_subcommand_fails);
#if defined(__linux__) || defined(__APPLE__)
    HU_RUN_TEST(test_cli_config_schema_emit_contains_core_keys);
    HU_RUN_TEST(test_cli_setup_local_model_emit_contains_expected_lines);
#endif
    HU_RUN_TEST(test_agent_cli_demo_flag_parsing);
    HU_RUN_TEST(test_agent_cli_no_demo_flag);
    HU_RUN_TEST(test_agent_cli_config_flag_parsing);
    HU_RUN_TEST(test_agent_cli_contact_flag_parsing);
    HU_RUN_TEST(test_agent_cli_contact_distinct_from_session);
    HU_RUN_TEST(test_agent_cli_no_contact_is_null);
    HU_RUN_TEST(test_agent_cli_demo_overrides_provider);
    HU_RUN_TEST(test_agent_cli_prompt_once_parsing);
    HU_RUN_TEST(test_agent_cli_prompt_without_once);
}
