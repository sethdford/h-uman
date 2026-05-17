#ifdef HU_ENABLE_PERSONA

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/persona.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#include <unistd.h>
#endif

static void cli_parse_null_argv_returns_invalid(void) {
    hu_persona_cli_args_t out = {0};
    hu_error_t err = hu_persona_cli_parse(3, NULL, &out);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void cli_parse_null_out_returns_invalid(void) {
    const char *argv[] = {"human", "persona", "list"};
    hu_error_t err = hu_persona_cli_parse(3, argv, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void cli_parse_argc_less_than_3_returns_invalid(void) {
    hu_persona_cli_args_t out = {0};
    const char *argv[] = {"human", "persona"};
    hu_error_t err = hu_persona_cli_parse(2, argv, &out);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void cli_parse_wrong_first_arg_returns_invalid(void) {
    hu_persona_cli_args_t out = {0};
    const char *argv[] = {"human", "config", "list"};
    hu_error_t err = hu_persona_cli_parse(3, argv, &out);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void cli_parse_list_ok(void) {
    hu_persona_cli_args_t out = {0};
    const char *argv[] = {"human", "persona", "list"};
    hu_error_t err = hu_persona_cli_parse(3, argv, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.action, HU_PERSONA_ACTION_LIST);
    HU_ASSERT_NULL(out.name);
}

static void cli_parse_show_ok(void) {
    hu_persona_cli_args_t out = {0};
    const char *argv[] = {"human", "persona", "show", "my-persona"};
    hu_error_t err = hu_persona_cli_parse(4, argv, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.action, HU_PERSONA_ACTION_SHOW);
    HU_ASSERT_STR_EQ(out.name, "my-persona");
}

static void cli_parse_show_requires_name(void) {
    hu_persona_cli_args_t out = {0};
    const char *argv[] = {"human", "persona", "show"};
    hu_error_t err = hu_persona_cli_parse(3, argv, &out);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void cli_parse_delete_ok(void) {
    hu_persona_cli_args_t out = {0};
    const char *argv[] = {"human", "persona", "delete", "old-persona"};
    hu_error_t err = hu_persona_cli_parse(4, argv, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.action, HU_PERSONA_ACTION_DELETE);
    HU_ASSERT_STR_EQ(out.name, "old-persona");
}

static void cli_parse_validate_ok(void) {
    hu_persona_cli_args_t out = {0};
    const char *argv[] = {"human", "persona", "validate", "test-persona"};
    hu_error_t err = hu_persona_cli_parse(4, argv, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.action, HU_PERSONA_ACTION_VALIDATE);
    HU_ASSERT_STR_EQ(out.name, "test-persona");
}

static void cli_parse_feedback_apply_ok(void) {
    hu_persona_cli_args_t out = {0};
    const char *argv[] = {"human", "persona", "feedback", "apply", "my-persona"};
    hu_error_t err = hu_persona_cli_parse(5, argv, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.action, HU_PERSONA_ACTION_FEEDBACK_APPLY);
    HU_ASSERT_STR_EQ(out.name, "my-persona");
}

static void cli_parse_feedback_without_apply_returns_invalid(void) {
    hu_persona_cli_args_t out = {0};
    const char *argv[] = {"human", "persona", "feedback", "record", "my-persona"};
    hu_error_t err = hu_persona_cli_parse(5, argv, &out);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void cli_parse_feedback_apply_requires_name(void) {
    hu_persona_cli_args_t out = {0};
    const char *argv[] = {"human", "persona", "feedback", "apply"};
    hu_error_t err = hu_persona_cli_parse(4, argv, &out);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void cli_parse_diff_ok(void) {
    hu_persona_cli_args_t out = {0};
    const char *argv[] = {"human", "persona", "diff", "a", "b"};
    hu_error_t err = hu_persona_cli_parse(5, argv, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.action, HU_PERSONA_ACTION_DIFF);
    HU_ASSERT_STR_EQ(out.name, "a");
    HU_ASSERT_STR_EQ(out.diff_name, "b");
}

static void cli_parse_diff_requires_two_names(void) {
    hu_persona_cli_args_t out = {0};
    const char *argv[] = {"human", "persona", "diff", "a"};
    hu_error_t err = hu_persona_cli_parse(4, argv, &out);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void cli_parse_export_ok(void) {
    hu_persona_cli_args_t out = {0};
    const char *argv[] = {"human", "persona", "export", "export-persona"};
    hu_error_t err = hu_persona_cli_parse(4, argv, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.action, HU_PERSONA_ACTION_EXPORT);
    HU_ASSERT_STR_EQ(out.name, "export-persona");
}

static void cli_parse_merge_ok(void) {
    hu_persona_cli_args_t out = {0};
    const char *argv[] = {"human", "persona", "merge", "merged", "a", "b"};
    hu_error_t err = hu_persona_cli_parse(6, argv, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.action, HU_PERSONA_ACTION_MERGE);
    HU_ASSERT_STR_EQ(out.name, "merged");
    HU_ASSERT_EQ(out.merge_sources_count, 2);
    HU_ASSERT_STR_EQ(out.merge_sources[0], "a");
    HU_ASSERT_STR_EQ(out.merge_sources[1], "b");
}

static void cli_parse_merge_requires_at_least_six_args(void) {
    hu_persona_cli_args_t out = {0};
    const char *argv[] = {"human", "persona", "merge", "merged", "a"};
    hu_error_t err = hu_persona_cli_parse(5, argv, &out);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void cli_parse_create_ok(void) {
    hu_persona_cli_args_t out = {0};
    const char *argv[] = {"human",       "persona",         "create",
                          "new-persona", "--from-response", "/tmp/response.json"};
    hu_error_t err = hu_persona_cli_parse(6, argv, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.action, HU_PERSONA_ACTION_CREATE);
    HU_ASSERT_STR_EQ(out.name, "new-persona");
    HU_ASSERT_STR_EQ(out.response_file, "/tmp/response.json");
}

static void cli_parse_create_requires_name(void) {
    hu_persona_cli_args_t out = {0};
    const char *argv[] = {"human", "persona", "create"};
    hu_error_t err = hu_persona_cli_parse(3, argv, &out);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void cli_parse_import_ok(void) {
    hu_persona_cli_args_t out = {0};
    const char *argv[] = {"human",    "persona",     "import",
                          "imported", "--from-file", "/tmp/persona.json"};
    hu_error_t err = hu_persona_cli_parse(6, argv, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.action, HU_PERSONA_ACTION_IMPORT);
    HU_ASSERT_STR_EQ(out.name, "imported");
    HU_ASSERT_STR_EQ(out.import_file, "/tmp/persona.json");
}

static void cli_parse_eval_ok(void) {
    hu_persona_cli_args_t out = {0};
    const char *argv[] = {"human", "persona", "eval", "default"};
    hu_error_t err = hu_persona_cli_parse(4, argv, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.action, HU_PERSONA_ACTION_EVAL);
    HU_ASSERT_STR_EQ(out.name, "default");
}

static void cli_parse_eval_requires_name(void) {
    hu_persona_cli_args_t out = {0};
    const char *argv[] = {"human", "persona", "eval"};
    hu_error_t err = hu_persona_cli_parse(3, argv, &out);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void cli_run_eval_embedded_default_passes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_cli_args_t args = {0};
    args.action = HU_PERSONA_ACTION_EVAL;
    args.name = "default";
    hu_error_t err = hu_persona_cli_run(&alloc, &args);
    HU_ASSERT_EQ(err, HU_OK);
}

static void cli_parse_invalid_action_returns_invalid(void) {
    hu_persona_cli_args_t out = {0};
    const char *argv[] = {"human", "persona", "unknown"};
    hu_error_t err = hu_persona_cli_parse(3, argv, &out);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void cli_run_null_alloc_returns_invalid(void) {
    hu_persona_cli_args_t args = {0};
    args.action = HU_PERSONA_ACTION_LIST;
    hu_error_t err = hu_persona_cli_run(NULL, &args);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void cli_run_null_args_returns_invalid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_persona_cli_run(&alloc, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void cli_run_validate_with_name_returns_ok_under_test(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_cli_args_t args = {0};
    args.action = HU_PERSONA_ACTION_VALIDATE;
    args.name = "test-persona";

    hu_error_t err = hu_persona_cli_run(&alloc, &args);
    HU_ASSERT_EQ(err, HU_OK);
}

static void cli_run_validate_without_name_returns_invalid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_cli_args_t args = {0};
    args.action = HU_PERSONA_ACTION_VALIDATE;
    args.name = NULL;

    hu_error_t err = hu_persona_cli_run(&alloc, &args);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void cli_run_feedback_apply_with_name_returns_ok_under_test(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_cli_args_t args = {0};
    args.action = HU_PERSONA_ACTION_FEEDBACK_APPLY;
    args.name = "test-persona";

    hu_error_t err = hu_persona_cli_run(&alloc, &args);
    HU_ASSERT_EQ(err, HU_OK);
}

static void cli_run_feedback_apply_without_name_returns_invalid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_cli_args_t args = {0};
    args.action = HU_PERSONA_ACTION_FEEDBACK_APPLY;
    args.name = NULL;

    hu_error_t err = hu_persona_cli_run(&alloc, &args);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

/* ---- PCTT Task 7: filler parse tests ---- */

static void cli_parse_filler_add_ok(void) {
    hu_persona_cli_args_t out = {0};
    const char *argv[] = {"human", "persona",   "filler",   "mypersona",
                          "add",   "--channel", "imessage", "hmm"};
    hu_error_t err = hu_persona_cli_parse(8, argv, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.action, HU_PERSONA_ACTION_FILLER_ADD);
    HU_ASSERT_STR_EQ(out.filler_channel, "imessage");
    HU_ASSERT_STR_EQ(out.filler_text, "hmm");
    HU_ASSERT_STR_EQ(out.name, "mypersona");
}

static void cli_parse_filler_list_ok(void) {
    hu_persona_cli_args_t out = {0};
    const char *argv[] = {"human", "persona", "filler", "mypersona", "list", "--channel", "slack"};
    hu_error_t err = hu_persona_cli_parse(7, argv, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.action, HU_PERSONA_ACTION_FILLER_LIST);
    HU_ASSERT_STR_EQ(out.filler_channel, "slack");
}

static void cli_parse_filler_remove_ok(void) {
    hu_persona_cli_args_t out = {0};
    const char *argv[] = {"human",     "persona", "filler",  "mypersona", "remove",
                          "--channel", "slack",   "--index", "2"};
    hu_error_t err = hu_persona_cli_parse(9, argv, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.action, HU_PERSONA_ACTION_FILLER_REMOVE);
    HU_ASSERT_STR_EQ(out.filler_channel, "slack");
    HU_ASSERT_EQ(out.filler_index, 2);
}

static void cli_run_filler_add_persists(void) {
#if defined(__unix__) || defined(__APPLE__)
    char tmpdir[] = "/tmp/human_persona_filler_cli_test_XXXXXX";
    if (!mkdtemp(tmpdir))
        return;

    setenv("HU_PERSONA_DIR", tmpdir, 1);

    hu_allocator_t alloc = hu_system_allocator();

    /* Build a minimal persona with one overlay (no fillers yet). */
    hu_persona_t p = {0};
    p.name = hu_strdup(&alloc, "testpersona");
    p.name_len = p.name ? strlen(p.name) : 0;
    p.identity = hu_strdup(&alloc, "test identity");
    if (!p.name || !p.identity) {
        hu_persona_deinit(&alloc, &p);
        unsetenv("HU_PERSONA_DIR");
        rmdir(tmpdir);
        return;
    }
    p.overlays = (hu_persona_overlay_t *)alloc.alloc(alloc.ctx, sizeof(hu_persona_overlay_t));
    HU_ASSERT_NOT_NULL(p.overlays);
    memset(p.overlays, 0, sizeof(hu_persona_overlay_t));
    p.overlays_count = 1;
    p.overlays[0].channel = hu_strdup(&alloc, "imessage");
    HU_ASSERT_NOT_NULL(p.overlays[0].channel);

    hu_error_t err = hu_persona_creator_write(&alloc, &p);
    hu_persona_deinit(&alloc, &p);
    if (err != HU_OK) {
        unsetenv("HU_PERSONA_DIR");
        char path[512];
        snprintf(path, sizeof(path), "%s/testpersona.json", tmpdir);
        unlink(path);
        rmdir(tmpdir);
        HU_ASSERT_EQ(err, HU_OK);
        return;
    }

    /* Run filler add via the CLI run path. */
    hu_persona_cli_args_t args = {0};
    args.action = HU_PERSONA_ACTION_FILLER_ADD;
    args.name = "testpersona";
    args.filler_channel = "imessage";
    args.filler_text = "hmm";
    args.filler_index = -1;

    err = hu_persona_cli_run(&alloc, &args);
    HU_ASSERT_EQ(err, HU_OK);

    /* Reload and verify the filler persisted. */
    hu_persona_t loaded = {0};
    err = hu_persona_load(&alloc, "testpersona", strlen("testpersona"), &loaded);
    unsetenv("HU_PERSONA_DIR");

    char path[512];
    snprintf(path, sizeof(path), "%s/testpersona.json", tmpdir);
    unlink(path);
    rmdir(tmpdir);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(loaded.overlays_count, 1);
    HU_ASSERT_NOT_NULL(loaded.overlays);
    HU_ASSERT_EQ(loaded.overlays[0].filler_bank_count, 1);
    HU_ASSERT_NOT_NULL(loaded.overlays[0].filler_bank);
    HU_ASSERT_STR_EQ(loaded.overlays[0].filler_bank[0], "hmm");

    hu_persona_deinit(&alloc, &loaded);
#endif
}

void run_persona_cli_tests(void) {
    HU_TEST_SUITE("PersonaCli");

    HU_RUN_TEST(cli_parse_null_argv_returns_invalid);
    HU_RUN_TEST(cli_parse_null_out_returns_invalid);
    HU_RUN_TEST(cli_parse_argc_less_than_3_returns_invalid);
    HU_RUN_TEST(cli_parse_wrong_first_arg_returns_invalid);
    HU_RUN_TEST(cli_parse_list_ok);
    HU_RUN_TEST(cli_parse_show_ok);
    HU_RUN_TEST(cli_parse_show_requires_name);
    HU_RUN_TEST(cli_parse_delete_ok);
    HU_RUN_TEST(cli_parse_validate_ok);
    HU_RUN_TEST(cli_parse_feedback_apply_ok);
    HU_RUN_TEST(cli_parse_feedback_without_apply_returns_invalid);
    HU_RUN_TEST(cli_parse_feedback_apply_requires_name);
    HU_RUN_TEST(cli_parse_diff_ok);
    HU_RUN_TEST(cli_parse_diff_requires_two_names);
    HU_RUN_TEST(cli_parse_export_ok);
    HU_RUN_TEST(cli_parse_merge_ok);
    HU_RUN_TEST(cli_parse_merge_requires_at_least_six_args);
    HU_RUN_TEST(cli_parse_create_ok);
    HU_RUN_TEST(cli_parse_create_requires_name);
    HU_RUN_TEST(cli_parse_import_ok);
    HU_RUN_TEST(cli_parse_eval_ok);
    HU_RUN_TEST(cli_parse_eval_requires_name);
    HU_RUN_TEST(cli_parse_invalid_action_returns_invalid);

    HU_RUN_TEST(cli_run_null_alloc_returns_invalid);
    HU_RUN_TEST(cli_run_null_args_returns_invalid);
    HU_RUN_TEST(cli_run_validate_with_name_returns_ok_under_test);
    HU_RUN_TEST(cli_run_validate_without_name_returns_invalid);
    HU_RUN_TEST(cli_run_feedback_apply_with_name_returns_ok_under_test);
    HU_RUN_TEST(cli_run_feedback_apply_without_name_returns_invalid);
    HU_RUN_TEST(cli_run_eval_embedded_default_passes);

    /* PCTT Task 7 — filler subcommand tests */
    HU_RUN_TEST(cli_parse_filler_add_ok);
    HU_RUN_TEST(cli_parse_filler_list_ok);
    HU_RUN_TEST(cli_parse_filler_remove_ok);
    HU_RUN_TEST(cli_run_filler_add_persists);
}

#else

void run_persona_cli_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_PERSONA */
