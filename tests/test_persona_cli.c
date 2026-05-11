#ifdef HU_ENABLE_PERSONA

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/persona.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    const char *argv[] = {"human", "persona", "create", "new-persona", "--from-response",
                          "/tmp/response.json"};
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
    const char *argv[] = {"human", "persona", "import", "imported", "--from-file",
                          "/tmp/persona.json"};
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
}

#else

void run_persona_cli_tests(void) { (void)0; }

#endif /* HU_ENABLE_PERSONA */
