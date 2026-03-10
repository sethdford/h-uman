#define HU_IS_TEST 1

#include "human/agent/action_preview.h"
#include "human/agent/undo.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/security/audit.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

static hu_allocator_t sys;

static void test_action_preview_shell_shows_command(void) {
    hu_action_preview_t p;
    memset(&p, 0, sizeof(p));
    const char *args = "{\"command\":\"ls -la /tmp\"}";
    hu_error_t err = hu_action_preview_generate(&sys, "shell", args, strlen(args), &p);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT_NOT_NULL(p.description);
    HU_ASSERT(strstr(p.description, "ls -la") != NULL);
    HU_ASSERT(strstr(p.description, "Run:") != NULL);
    hu_action_preview_free(&sys, &p);
}

static void test_action_preview_file_write_shows_path(void) {
    hu_action_preview_t p;
    memset(&p, 0, sizeof(p));
    const char *args = "{\"path\":\"/home/user/notes.txt\"}";
    hu_error_t err = hu_action_preview_generate(&sys, "file_write", args, strlen(args), &p);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT_NOT_NULL(p.description);
    HU_ASSERT(strstr(p.description, "/home/user/notes.txt") != NULL);
    HU_ASSERT(strstr(p.description, "Write") != NULL);
    hu_action_preview_free(&sys, &p);
}

static void test_action_preview_format_produces_readable_string(void) {
    hu_action_preview_t p;
    memset(&p, 0, sizeof(p));
    const char *args = "{\"command\":\"echo hello\"}";
    hu_error_t err = hu_action_preview_generate(&sys, "shell", args, strlen(args), &p);
    HU_ASSERT(err == HU_OK);
    char *formatted = NULL;
    size_t len = 0;
    err = hu_action_preview_format(&sys, &p, &formatted, &len);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT_NOT_NULL(formatted);
    HU_ASSERT(strstr(formatted, "[high]") != NULL);
    HU_ASSERT(strstr(formatted, "shell") != NULL);
    HU_ASSERT(strstr(formatted, "Run:") != NULL);
    sys.free(sys.ctx, formatted, strlen(formatted) + 1);
    hu_action_preview_free(&sys, &p);
}

static void test_undo_stack_push_pop_works(void) {
    hu_undo_stack_t *stack = hu_undo_stack_create(&sys, 10);
    HU_ASSERT_NOT_NULL(stack);
    HU_ASSERT_EQ(hu_undo_stack_count(stack), 0);

    hu_undo_entry_t e = {
        .type = HU_UNDO_FILE_WRITE,
        .description = hu_strdup(&sys, "test"),
        .path = hu_strdup(&sys, "/tmp/x"),
        .original_content = hu_strdup(&sys, "content"),
        .original_content_len = 7,
        .timestamp = 0,
        .reversible = true,
    };
    hu_error_t err = hu_undo_stack_push(stack, &e);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT_EQ(hu_undo_stack_count(stack), 1);

    sys.free(sys.ctx, e.description, strlen(e.description) + 1);
    sys.free(sys.ctx, e.path, strlen(e.path) + 1);
    sys.free(sys.ctx, e.original_content, e.original_content_len + 1);

    err = hu_undo_stack_execute_undo(stack, &sys);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT_EQ(hu_undo_stack_count(stack), 0);

    hu_undo_stack_destroy(stack);
}

static void test_undo_stack_capacity_ring_buffer(void) {
    hu_undo_stack_t *stack = hu_undo_stack_create(&sys, 3);
    HU_ASSERT_NOT_NULL(stack);

    for (int i = 0; i < 5; i++) {
        char desc[32], path[32];
        snprintf(desc, sizeof(desc), "desc%d", i);
        snprintf(path, sizeof(path), "/tmp/f%d", i);
        hu_undo_entry_t e = {
            .type = HU_UNDO_FILE_WRITE,
            .description = hu_strdup(&sys, desc),
            .path = hu_strdup(&sys, path),
            .original_content = NULL,
            .original_content_len = 0,
            .timestamp = 0,
            .reversible = true,
        };
        hu_error_t err = hu_undo_stack_push(stack, &e);
        HU_ASSERT(err == HU_OK);
        sys.free(sys.ctx, e.description, strlen(e.description) + 1);
        sys.free(sys.ctx, e.path, strlen(e.path) + 1);
    }
    HU_ASSERT_EQ(hu_undo_stack_count(stack), 3);

    hu_undo_stack_destroy(stack);
}

static void test_undo_execute_pops_in_test_mode(void) {
#if defined(HU_IS_TEST) && HU_IS_TEST
    hu_undo_stack_t *stack = hu_undo_stack_create(&sys, 5);
    HU_ASSERT_NOT_NULL(stack);

    hu_undo_entry_t e = {
        .type = HU_UNDO_FILE_WRITE,
        .description = hu_strdup(&sys, "test"),
        .path = hu_strdup(&sys, "/tmp/nonexistent"),
        .original_content = hu_strdup(&sys, "data"),
        .original_content_len = 4,
        .timestamp = 0,
        .reversible = true,
    };
    hu_undo_stack_push(stack, &e);
    sys.free(sys.ctx, e.description, strlen(e.description) + 1);
    sys.free(sys.ctx, e.path, strlen(e.path) + 1);
    sys.free(sys.ctx, e.original_content, e.original_content_len + 1);

    HU_ASSERT_EQ(hu_undo_stack_count(stack), 1);
    hu_error_t err = hu_undo_stack_execute_undo(stack, &sys);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT_EQ(hu_undo_stack_count(stack), 0);

    hu_undo_stack_destroy(stack);
#endif
}

static void test_audit_logger_records_tool_execution(void) {
#if defined(__unix__) || defined(__APPLE__)
    char tmp[] = "/tmp/hu_audit_pipe_XXXXXX";
    char *dir = mkdtemp(tmp);
    HU_ASSERT_NOT_NULL(dir);

    hu_audit_config_t cfg = {
        .enabled = true,
        .log_path = "audit.log",
        .max_size_mb = 10,
    };
    hu_audit_logger_t *log = hu_audit_logger_create(&sys, &cfg, dir);
    HU_ASSERT_NOT_NULL(log);

    hu_audit_event_t ev;
    hu_audit_event_init(&ev, HU_AUDIT_COMMAND_EXECUTION);
    hu_audit_event_with_action(&ev, "shell", "tool", true, true);
    hu_audit_event_with_result(&ev, true, 0, 0, NULL);

    hu_error_t err = hu_audit_logger_log(log, &ev);
    HU_ASSERT(err == HU_OK);

    hu_audit_logger_destroy(log, &sys);
    rmdir(dir);
#endif
}

void run_security_pipeline_tests(void) {
    sys = hu_system_allocator();

    HU_TEST_SUITE("security_pipeline");

    HU_RUN_TEST(test_action_preview_shell_shows_command);
    HU_RUN_TEST(test_action_preview_file_write_shows_path);
    HU_RUN_TEST(test_action_preview_format_produces_readable_string);
    HU_RUN_TEST(test_undo_stack_push_pop_works);
    HU_RUN_TEST(test_undo_stack_capacity_ring_buffer);
    HU_RUN_TEST(test_undo_execute_pops_in_test_mode);
    HU_RUN_TEST(test_audit_logger_records_tool_execution);
}
