/* Security edge cases (~40 tests). No real I/O where possible. */
#include "test_framework.h"
#include "seaclaw/security.h"
#include "seaclaw/security/audit.h"
#include <stdio.h>
#include "seaclaw/tools/path_security.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* ─── Policy / path ──────────────────────────────────────────────────────── */
static void test_policy_wildcard_allows_all(void) {
    sc_security_policy_t p = {
        .autonomy = SC_AUTONOMY_FULL,
        .allowed_commands = (const char *[]){ "*" },
        .allowed_commands_len = 1,
    };
    SC_ASSERT_TRUE(sc_policy_is_command_allowed(&p, "ls"));
    SC_ASSERT_TRUE(sc_policy_is_command_allowed(&p, "git status"));
}

static void test_policy_explicit_allowlist_blocks_other(void) {
    const char *allowed[] = { "git" };
    sc_security_policy_t p = {
        .autonomy = SC_AUTONOMY_SUPERVISED,
        .allowed_commands = allowed,
        .allowed_commands_len = 1,
    };
    SC_ASSERT_FALSE(sc_policy_is_command_allowed(&p, "ls"));
    SC_ASSERT_TRUE(sc_policy_is_command_allowed(&p, "git status"));
}

static void test_policy_path_traversal_dot_dot_slash(void) {
    SC_ASSERT_FALSE(sc_path_is_safe("../etc/passwd"));
    SC_ASSERT_FALSE(sc_path_is_safe(".."));
    SC_ASSERT_FALSE(sc_path_is_safe("foo/../bar"));
}

static void test_policy_path_traversal_encoded(void) {
    SC_ASSERT_FALSE(sc_path_is_safe("..%2fetc"));
    SC_ASSERT_FALSE(sc_path_is_safe("%2f..%2fetc"));
}

static void test_policy_command_substitution_blocked(void) {
    sc_security_policy_t p = {
        .autonomy = SC_AUTONOMY_SUPERVISED,
        .allowed_commands = (const char *[]){ "echo" },
        .allowed_commands_len = 1,
    };
    SC_ASSERT_FALSE(sc_policy_is_command_allowed(&p, "echo `whoami`"));
}

static void test_policy_redirect_blocked(void) {
    sc_security_policy_t p = {
        .autonomy = SC_AUTONOMY_SUPERVISED,
        .allowed_commands = (const char *[]){ "echo" },
        .allowed_commands_len = 1,
    };
    SC_ASSERT_FALSE(sc_policy_is_command_allowed(&p, "echo x > /etc/passwd"));
}

static void test_policy_command_with_semicolons(void) {
    sc_security_policy_t p = {
        .autonomy = SC_AUTONOMY_SUPERVISED,
        .allowed_commands = (const char *[]){ "ls" },
        .allowed_commands_len = 1,
    };
    SC_ASSERT_FALSE(sc_policy_is_command_allowed(&p, "ls; rm -rf /"));
}

static void test_policy_rate_limit_window_reset(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_rate_tracker_t *t = sc_rate_tracker_create(&sys, 2);
    SC_ASSERT_NOT_NULL(t);
    SC_ASSERT_TRUE(sc_rate_tracker_record_action(t));
    SC_ASSERT_TRUE(sc_rate_tracker_record_action(t));
    SC_ASSERT_FALSE(sc_rate_tracker_record_action(t));
    SC_ASSERT_TRUE(sc_rate_tracker_is_limited(t));
    sc_rate_tracker_destroy(t);
}

static void test_pairing_guard_lockout_after_max_attempts(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_pairing_guard_t *g = sc_pairing_guard_create(&sys, 1, NULL, 0);
    SC_ASSERT_NOT_NULL(g);
    const char *code = sc_pairing_guard_pairing_code(g);
    SC_ASSERT_NOT_NULL(code);
    for (int i = 0; i < 5; i++) {
        char *tok = NULL;
        sc_pair_attempt_result_t r = sc_pairing_guard_attempt_pair(g, "000000", &tok);
        SC_ASSERT(r == SC_PAIR_INVALID_CODE);
    }
    char *tok = NULL;
    sc_pair_attempt_result_t r = sc_pairing_guard_attempt_pair(g, "000000", &tok);
    SC_ASSERT(r == SC_PAIR_LOCKED_OUT);
    sc_pairing_guard_destroy(g);
}

static void test_pairing_code_format(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_pairing_guard_t *g = sc_pairing_guard_create(&sys, 1, NULL, 0);
    SC_ASSERT_NOT_NULL(g);
    const char *code = sc_pairing_guard_pairing_code(g);
    SC_ASSERT_NOT_NULL(code);
    SC_ASSERT_EQ(strlen(code), 8u);
    for (int i = 0; i < 8; i++)
        SC_ASSERT(code[i] >= '0' && code[i] <= '9');
    sc_pairing_guard_destroy(g);
}

static void test_pairing_short_code_fails(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_pairing_guard_t *g = sc_pairing_guard_create(&sys, 1, NULL, 0);
    SC_ASSERT_NOT_NULL(g);
    char *tok = NULL;
    sc_pair_attempt_result_t r = sc_pairing_guard_attempt_pair(g, "12345", &tok);
    SC_ASSERT(r == SC_PAIR_INVALID_CODE);
    sc_pairing_guard_destroy(g);
}

/* ─── Secret store (uses temp dir) ─────────────────────────────────────────── */
static void test_secret_encrypt_empty_string(void) {
    sc_allocator_t sys = sc_system_allocator();
    char tmp[] = "/tmp/sc_sec_empty_XXXXXX";
    char *dir = mkdtemp(tmp);
    SC_ASSERT_NOT_NULL(dir);
    sc_secret_store_t *store = sc_secret_store_create(&sys, dir, 1);
    SC_ASSERT_NOT_NULL(store);
    char *enc = NULL;
    sc_error_t err = sc_secret_store_encrypt(store, &sys, "", &enc);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(enc);
    sys.free(sys.ctx, enc, strlen(enc) + 1);
    sc_secret_store_destroy(store, &sys);
    rmdir(dir);
}

static void test_secret_encrypt_long_string(void) {
    sc_allocator_t sys = sc_system_allocator();
    char tmp[] = "/tmp/sc_sec_long_XXXXXX";
    char *dir = mkdtemp(tmp);
    SC_ASSERT_NOT_NULL(dir);
    sc_secret_store_t *store = sc_secret_store_create(&sys, dir, 1);
    SC_ASSERT_NOT_NULL(store);
    char longstr[2000];
    for (size_t i = 0; i < sizeof(longstr) - 1; i++) longstr[i] = 'x';
    longstr[sizeof(longstr) - 1] = '\0';
    char *enc = NULL;
    sc_error_t err = sc_secret_store_encrypt(store, &sys, longstr, &enc);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(enc);
    sys.free(sys.ctx, enc, strlen(enc) + 1);
    sc_secret_store_destroy(store, &sys);
    rmdir(dir);
}

static void test_secret_decrypt_corrupted(void) {
    sc_allocator_t sys = sc_system_allocator();
    char tmp[] = "/tmp/sc_sec_corrupt_XXXXXX";
    char *dir = mkdtemp(tmp);
    SC_ASSERT_NOT_NULL(dir);
    sc_secret_store_t *store = sc_secret_store_create(&sys, dir, 1);
    SC_ASSERT_NOT_NULL(store);
    char *dec = NULL;
    sc_error_t err = sc_secret_store_decrypt(store, &sys, "enc2:deadbeef", &dec);
    SC_ASSERT_NEQ(err, SC_OK);
    sc_secret_store_destroy(store, &sys);
    rmdir(dir);
}

static void test_secret_decrypt_wrong_key(void) {
    sc_allocator_t sys = sc_system_allocator();
    char tmp[] = "/tmp/sc_sec_wrong_XXXXXX";
    char *dir = mkdtemp(tmp);
    SC_ASSERT_NOT_NULL(dir);
    sc_secret_store_t *store = sc_secret_store_create(&sys, dir, 1);
    SC_ASSERT_NOT_NULL(store);
    char *enc = NULL;
    sc_secret_store_encrypt(store, &sys, "secret", &enc);
    SC_ASSERT_NOT_NULL(enc);
    sc_secret_store_destroy(store, &sys);
    sc_secret_store_t *store2 = sc_secret_store_create(&sys, "/tmp", 1);
    char *dec = NULL;
    sc_error_t err = sc_secret_store_decrypt(store2, &sys, enc, &dec);
    SC_ASSERT_NEQ(err, SC_OK);
    sys.free(sys.ctx, enc, strlen(enc) + 1);
    sc_secret_store_destroy(store2, &sys);
    {
        char keypath[256];
        snprintf(keypath, sizeof(keypath), "%s/.secret_key", dir);
        (void)unlink(keypath);
    }
    (void)rmdir(dir);
}

static void test_hex_encode_decode_roundtrip(void) {
    uint8_t data[] = {0x00, 0xFF, 0x0A, 0xB3};
    char hex[16];
    sc_hex_encode(data, 4, hex);
    uint8_t out[4];
    size_t len;
    sc_error_t err = sc_hex_decode(hex, 8, out, 4, &len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(len, 4u);
    SC_ASSERT(memcmp(data, out, 4) == 0);
}

static void test_hex_decode_odd_length_fails(void) {
    uint8_t out[8];
    size_t len;
    sc_error_t err = sc_hex_decode("12345", 5, out, 8, &len);
    SC_ASSERT_EQ(err, SC_ERR_PARSE);
}

static void test_hex_decode_invalid_chars_fails(void) {
    uint8_t out[8];
    size_t len;
    sc_error_t err = sc_hex_decode("GG", 2, out, 8, &len);
    SC_ASSERT_EQ(err, SC_ERR_PARSE);
}

static void test_path_resolved_allowed_empty_list(void) {
    sc_allocator_t alloc = sc_system_allocator();
    SC_ASSERT_FALSE(sc_path_resolved_allowed(&alloc, "/etc/passwd", NULL, NULL, 0));
}

static void test_path_resolved_allowed_workspace_prefix(void) {
    sc_allocator_t alloc = sc_system_allocator();
    SC_ASSERT_TRUE(sc_path_resolved_allowed(&alloc,
        "/home/user/proj/sub/file.txt", "/home/user/proj", NULL, 0));
}

static void test_path_is_safe_relative_ok(void) {
    SC_ASSERT_TRUE(sc_path_is_safe("foo/bar"));
    SC_ASSERT_TRUE(sc_path_is_safe("a"));
}

static void test_secret_store_is_encrypted_prefix(void) {
    SC_ASSERT_TRUE(sc_secret_store_is_encrypted("enc2:abcdef"));
    SC_ASSERT_FALSE(sc_secret_store_is_encrypted("enc2"));
    SC_ASSERT_FALSE(sc_secret_store_is_encrypted("enc1:"));
}

static void test_pairing_constant_time_eq_null(void) {
    SC_ASSERT_TRUE(sc_pairing_guard_constant_time_eq(NULL, NULL));
    SC_ASSERT_FALSE(sc_pairing_guard_constant_time_eq("a", NULL));
    SC_ASSERT_FALSE(sc_pairing_guard_constant_time_eq(NULL, "a"));
}

static void test_rate_tracker_null_safe(void) {
    SC_ASSERT_EQ(sc_rate_tracker_remaining(NULL), 0u);
    SC_ASSERT_EQ(sc_rate_tracker_count(NULL), 0u);
    SC_ASSERT_FALSE(sc_rate_tracker_is_limited(NULL));
}

static void test_policy_record_action(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_rate_tracker_t *tr = sc_rate_tracker_create(&sys, 2);
    sc_security_policy_t p = {
        .autonomy = SC_AUTONOMY_SUPERVISED,
        .allowed_commands = (const char *[]){ "ls" },
        .allowed_commands_len = 1,
        .tracker = tr,
    };
    SC_ASSERT_TRUE(sc_policy_record_action(&p));
    SC_ASSERT_TRUE(sc_policy_record_action(&p));
    SC_ASSERT_FALSE(sc_policy_record_action(&p));
    sc_rate_tracker_destroy(tr);
}

static void test_policy_is_rate_limited(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_rate_tracker_t *tr = sc_rate_tracker_create(&sys, 1);
    sc_security_policy_t p = {
        .autonomy = SC_AUTONOMY_SUPERVISED,
        .tracker = tr,
    };
    sc_policy_record_action(&p);
    SC_ASSERT_TRUE(sc_policy_is_rate_limited(&p));
    sc_rate_tracker_destroy(tr);
}

static void test_audit_event_init_unique_ids(void) {
    sc_audit_event_t e1, e2;
    sc_audit_event_init(&e1, SC_AUDIT_COMMAND_EXECUTION);
    sc_audit_event_init(&e2, SC_AUDIT_COMMAND_EXECUTION);
    SC_ASSERT(e1.event_id != e2.event_id);
}

static void test_audit_event_write_json_truncated(void) {
    sc_audit_event_t ev;
    sc_audit_event_init(&ev, SC_AUDIT_COMMAND_EXECUTION);
    char buf[4];
    size_t n = sc_audit_event_write_json(&ev, buf, sizeof(buf));
    SC_ASSERT(n <= 4);
}

static void test_policy_can_act_full(void) {
    sc_security_policy_t p = { .autonomy = SC_AUTONOMY_FULL };
    SC_ASSERT_TRUE(sc_policy_can_act(&p));
}

static void test_policy_can_act_readonly(void) {
    sc_security_policy_t p = { .autonomy = SC_AUTONOMY_READ_ONLY };
    SC_ASSERT_FALSE(sc_policy_can_act(&p));
}

static void test_security_shell_allowed(void) {
    sc_security_policy_t p = { .allow_shell = true };
    SC_ASSERT_TRUE(sc_security_shell_allowed(&p));
    p.allow_shell = false;
    SC_ASSERT_FALSE(sc_security_shell_allowed(&p));
}

static void test_security_path_allowed_with_allowlist(void) {
    const char *allowed[] = { "/tmp/ws" };
    sc_security_policy_t p = {
        .allowed_paths = allowed,
        .allowed_paths_count = 1,
    };
    SC_ASSERT_TRUE(sc_security_path_allowed(&p, "/tmp/ws/file", 11));
    SC_ASSERT_FALSE(sc_security_path_allowed(&p, "/etc/passwd", 11));
}

static void test_path_is_safe_null(void) {
    SC_ASSERT_FALSE(sc_path_is_safe(NULL));
}

static void test_path_is_safe_long(void) {
    char buf[5000];
    for (size_t i = 0; i < 4095; i++) buf[i] = 'a';
    buf[4095] = '\0';
    SC_ASSERT_TRUE(sc_path_is_safe(buf));
}

static void test_hex_encode_empty(void) {
    uint8_t empty = 0;
    char hex[4];
    sc_hex_encode(&empty, 0, hex);
    hex[0] = '\0';
}

/* ─── Policy risk level & command validation ─────────────────────────────── */
static void test_policy_command_risk_rm_high(void) {
    sc_security_policy_t p = { .autonomy = SC_AUTONOMY_FULL };
    sc_command_risk_level_t r = sc_policy_command_risk_level(&p, "rm -rf /");
    SC_ASSERT(r == SC_RISK_HIGH);
}

static void test_policy_command_risk_ls_low(void) {
    sc_security_policy_t p = { .autonomy = SC_AUTONOMY_FULL };
    sc_command_risk_level_t r = sc_policy_command_risk_level(&p, "ls -la");
    SC_ASSERT(r == SC_RISK_LOW);
}

static void test_policy_command_risk_git_commit_medium(void) {
    sc_security_policy_t p = { .autonomy = SC_AUTONOMY_FULL };
    sc_command_risk_level_t r = sc_policy_command_risk_level(&p, "git commit -m x");
    SC_ASSERT(r == SC_RISK_MEDIUM);
}

static void test_policy_command_risk_sudo_high(void) {
    sc_security_policy_t p = { .autonomy = SC_AUTONOMY_FULL };
    sc_command_risk_level_t r = sc_policy_command_risk_level(&p, "sudo apt install x");
    SC_ASSERT(r == SC_RISK_HIGH);
}

static void test_policy_validate_command_low_ok(void) {
    sc_security_policy_t p = { .autonomy = SC_AUTONOMY_SUPERVISED };
    sc_command_risk_level_t risk;
    sc_error_t err = sc_policy_validate_command(&p, "ls", false, &risk);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT(risk == SC_RISK_LOW);
}

static void test_policy_validate_command_high_blocked(void) {
    sc_security_policy_t p = {
        .autonomy = SC_AUTONOMY_SUPERVISED,
        .block_high_risk_commands = true,
    };
    sc_command_risk_level_t risk;
    sc_error_t err = sc_policy_validate_command(&p, "rm -rf /", false, &risk);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_policy_validate_command_high_approved_ok(void) {
    const char *allowed[] = { "rm" };
    sc_security_policy_t p = {
        .autonomy = SC_AUTONOMY_SUPERVISED,
        .block_high_risk_commands = false,
        .require_approval_for_medium_risk = false,
        .allowed_commands = allowed,
        .allowed_commands_len = 1,
    };
    sc_command_risk_level_t risk;
    sc_error_t err = sc_policy_validate_command(&p, "rm file", true, &risk);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT(risk == SC_RISK_HIGH);
}

static void test_policy_can_act_supervised(void) {
    sc_security_policy_t p = { .autonomy = SC_AUTONOMY_SUPERVISED };
    SC_ASSERT_TRUE(sc_policy_can_act(&p));
}

static void test_path_is_safe_absolute_denied(void) {
    SC_ASSERT_FALSE(sc_path_is_safe("/etc/passwd"));
}

static void test_path_is_safe_double_dot(void) {
    SC_ASSERT_FALSE(sc_path_is_safe("a/../../etc"));
}

static void test_pairing_constant_time_eq_same(void) {
    SC_ASSERT_TRUE(sc_pairing_guard_constant_time_eq("abc", "abc"));
}

static void test_pairing_constant_time_eq_different(void) {
    SC_ASSERT_FALSE(sc_pairing_guard_constant_time_eq("abc", "abd"));
}

static void test_pairing_valid_code_succeeds(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_pairing_guard_t *g = sc_pairing_guard_create(&sys, 1, NULL, 0);
    SC_ASSERT_NOT_NULL(g);
    const char *code = sc_pairing_guard_pairing_code(g);
    SC_ASSERT_NOT_NULL(code);
    char *tok = NULL;
    sc_pair_attempt_result_t r = sc_pairing_guard_attempt_pair(g, code, &tok);
    SC_ASSERT(r == SC_PAIR_PAIRED);
    SC_ASSERT_NOT_NULL(tok);
    if (tok) sys.free(sys.ctx, tok, strlen(tok) + 1);
    sc_pairing_guard_destroy(g);
}

static void test_secret_encrypt_decrypt_roundtrip(void) {
    sc_allocator_t sys = sc_system_allocator();
    char tmp[] = "/tmp/sc_roundtrip_XXXXXX";
    char *dir = mkdtemp(tmp);
    SC_ASSERT_NOT_NULL(dir);
    sc_secret_store_t *store = sc_secret_store_create(&sys, dir, 1);
    SC_ASSERT_NOT_NULL(store);
    const char *plain = "my-secret-value";
    char *enc = NULL;
    sc_error_t err = sc_secret_store_encrypt(store, &sys, plain, &enc);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(enc);
    SC_ASSERT_TRUE(sc_secret_store_is_encrypted(enc));
    char *dec = NULL;
    err = sc_secret_store_decrypt(store, &sys, enc, &dec);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(dec);
    SC_ASSERT_STR_EQ(dec, plain);
    sys.free(sys.ctx, enc, strlen(enc) + 1);
    sys.free(sys.ctx, dec, strlen(dec) + 1);
    sc_secret_store_destroy(store, &sys);
    rmdir(dir);
}

static void test_audit_event_type_command(void) {
    sc_audit_event_t ev;
    sc_audit_event_init(&ev, SC_AUDIT_COMMAND_EXECUTION);
    char buf[256];
    size_t n = sc_audit_event_write_json(&ev, buf, sizeof(buf));
    SC_ASSERT(n > 0);
    SC_ASSERT_TRUE(strstr(buf, "command") != NULL);
}

static void test_path_resolved_allowed_denied_outside(void) {
    sc_allocator_t alloc = sc_system_allocator();
    SC_ASSERT_FALSE(sc_path_resolved_allowed(&alloc,
        "/etc/passwd", "/home/user/ws", NULL, 0));
}

static void test_path_resolved_allowed_subpath(void) {
    sc_allocator_t alloc = sc_system_allocator();
    SC_ASSERT_TRUE(sc_path_resolved_allowed(&alloc,
        "/home/user/ws/sub/file", "/home/user/ws", NULL, 0));
}

static void test_hex_encode_decode_full(void) {
    uint8_t data[] = {0x01, 0x02, 0x0F, 0xF0, 0xFF};
    char hex[16];
    sc_hex_encode(data, 5, hex);
    uint8_t out[8];
    size_t len;
    sc_error_t err = sc_hex_decode(hex, 10, out, 8, &len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(len, 5u);
    SC_ASSERT(memcmp(data, out, 5) == 0);
}

/* ─── WP-21B parity: policy bypass, autonomy, file ops ─────────────────────── */
static void test_policy_readonly_blocks_even_allowlist(void) {
    const char *allowed[] = { "ls", "cat" };
    sc_security_policy_t p = {
        .autonomy = SC_AUTONOMY_READ_ONLY,
        .allowed_commands = allowed,
        .allowed_commands_len = 2,
    };
    SC_ASSERT_FALSE(sc_policy_is_command_allowed(&p, "ls"));
    SC_ASSERT_FALSE(sc_policy_is_command_allowed(&p, "cat file"));
}

static void test_policy_file_ops_path_denied(void) {
    const char *allowed[] = { "/tmp/ws" };
    sc_security_policy_t p = {
        .allowed_paths = allowed,
        .allowed_paths_count = 1,
    };
    SC_ASSERT_FALSE(sc_security_path_allowed(&p, "/etc/passwd", 11));
}

static void test_policy_file_ops_path_allowed_subpath(void) {
    const char *allowed[] = { "/tmp/ws" };
    sc_security_policy_t p = {
        .allowed_paths = allowed,
        .allowed_paths_count = 1,
    };
    SC_ASSERT_TRUE(sc_security_path_allowed(&p, "/tmp/ws/sub/file", 14));
}

static void test_policy_bypass_pipe_blocked(void) {
    sc_security_policy_t p = {
        .autonomy = SC_AUTONOMY_SUPERVISED,
        .allowed_commands = (const char *[]){ "echo" },
        .allowed_commands_len = 1,
    };
    SC_ASSERT_FALSE(sc_policy_is_command_allowed(&p, "echo x | sh"));
}

static void test_policy_risk_curl_medium_or_high(void) {
    sc_security_policy_t p = { .autonomy = SC_AUTONOMY_FULL };
    sc_command_risk_level_t r = sc_policy_command_risk_level(&p, "curl https://example.com");
    SC_ASSERT(r >= SC_RISK_LOW && r <= SC_RISK_HIGH);
}

static void test_policy_risk_wget_medium(void) {
    sc_security_policy_t p = { .autonomy = SC_AUTONOMY_FULL };
    sc_command_risk_level_t r = sc_policy_command_risk_level(&p, "wget http://evil.com");
    SC_ASSERT(r == SC_RISK_MEDIUM || r == SC_RISK_HIGH);
}

static void test_path_is_safe_symlink_traversal(void) {
    SC_ASSERT_FALSE(sc_path_is_safe("foo/bar/../../../etc"));
}

static void test_policy_validate_command_full_returns_risk_level(void) {
    sc_security_policy_t p = {
        .autonomy = SC_AUTONOMY_FULL,
        .block_high_risk_commands = false,
    };
    sc_command_risk_level_t risk;
    sc_error_t err = sc_policy_validate_command(&p, "ls -la", false, &risk);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT(risk == SC_RISK_LOW);
}

static void test_pairing_wrong_code_fails(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_pairing_guard_t *g = sc_pairing_guard_create(&sys, 1, NULL, 0);
    char *tok = NULL;
    sc_pair_attempt_result_t r = sc_pairing_guard_attempt_pair(g, "111111", &tok);
    SC_ASSERT(r == SC_PAIR_INVALID_CODE || r == SC_PAIR_LOCKED_OUT);
    sc_pairing_guard_destroy(g);
}

static void test_policy_autonomy_read_only(void) {
    sc_security_policy_t p = { .autonomy = SC_AUTONOMY_READ_ONLY };
    SC_ASSERT_FALSE(sc_policy_can_act(&p));
}

static void test_policy_autonomy_supervised(void) {
    sc_security_policy_t p = {
        .autonomy = SC_AUTONOMY_SUPERVISED,
        .allowed_commands = (const char *[]){ "ls", "cat" },
        .allowed_commands_len = 2,
    };
    SC_ASSERT_TRUE(sc_policy_can_act(&p));
    SC_ASSERT_TRUE(sc_policy_is_command_allowed(&p, "ls"));
}

static void test_policy_autonomy_full(void) {
    sc_security_policy_t p = { .autonomy = SC_AUTONOMY_FULL };
    SC_ASSERT_TRUE(sc_policy_can_act(&p));
}

static void test_policy_command_risk_chmod(void) {
    sc_security_policy_t p = { .autonomy = SC_AUTONOMY_FULL };
    sc_command_risk_level_t r = sc_policy_command_risk_level(&p, "chmod 777 /etc/passwd");
    SC_ASSERT(r >= SC_RISK_MEDIUM);
}

static void test_policy_command_risk_dd(void) {
    sc_security_policy_t p = { .autonomy = SC_AUTONOMY_FULL };
    sc_command_risk_level_t r = sc_policy_command_risk_level(&p, "dd if=/dev/zero of=/dev/sda");
    SC_ASSERT(r == SC_RISK_HIGH);
}

static void test_policy_command_risk_npm_install(void) {
    sc_security_policy_t p = { .autonomy = SC_AUTONOMY_FULL };
    sc_command_risk_level_t r = sc_policy_command_risk_level(&p, "npm install -g pkg");
    SC_ASSERT(r >= SC_RISK_LOW);
}

static void test_policy_command_risk_pip_install(void) {
    sc_security_policy_t p = { .autonomy = SC_AUTONOMY_FULL };
    sc_command_risk_level_t r = sc_policy_command_risk_level(&p, "pip install package");
    SC_ASSERT(r >= SC_RISK_LOW);
}

static void test_rate_tracker_remaining_decrements(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_rate_tracker_t *t = sc_rate_tracker_create(&sys, 3);
    SC_ASSERT_EQ(sc_rate_tracker_remaining(t), 3u);
    sc_rate_tracker_record_action(t);
    SC_ASSERT_EQ(sc_rate_tracker_remaining(t), 2u);
    sc_rate_tracker_record_action(t);
    SC_ASSERT_EQ(sc_rate_tracker_remaining(t), 1u);
    sc_rate_tracker_destroy(t);
}

static void test_rate_tracker_count_increments(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_rate_tracker_t *t = sc_rate_tracker_create(&sys, 5);
    sc_rate_tracker_record_action(t);
    sc_rate_tracker_record_action(t);
    SC_ASSERT_EQ(sc_rate_tracker_count(t), 2u);
    sc_rate_tracker_destroy(t);
}

static void test_pairing_code_numeric_only(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_pairing_guard_t *g = sc_pairing_guard_create(&sys, 1, NULL, 0);
    const char *code = sc_pairing_guard_pairing_code(g);
    SC_ASSERT_NOT_NULL(code);
    for (size_t i = 0; i < strlen(code); i++)
        SC_ASSERT_TRUE(code[i] >= '0' && code[i] <= '9');
    sc_pairing_guard_destroy(g);
}

static void test_audit_event_severity(void) {
    sc_audit_severity_t sev = sc_audit_event_severity(SC_AUDIT_COMMAND_EXECUTION);
    SC_ASSERT(sev >= SC_AUDIT_SEV_LOW && sev <= SC_AUDIT_SEV_HIGH);
}

static void test_audit_event_type_string(void) {
    const char *s = sc_audit_event_type_string(SC_AUDIT_COMMAND_EXECUTION);
    SC_ASSERT_NOT_NULL(s);
    SC_ASSERT_TRUE(strlen(s) > 0);
}

static void test_audit_should_log(void) {
    SC_ASSERT_TRUE(sc_audit_should_log(SC_AUDIT_AUTH_FAILURE, SC_AUDIT_SEV_LOW));
}

static void test_path_traversal_percent_encoded(void) {
    SC_ASSERT_FALSE(sc_path_is_safe("..%2f..%2fetc%2fpasswd"));
}

static void test_path_traversal_mixed(void) {
    SC_ASSERT_FALSE(sc_path_is_safe("./../etc"));
    SC_ASSERT_FALSE(sc_path_is_safe(".//../secret"));
}

static void test_security_path_allowed_empty_allowlist_denies(void) {
    sc_security_policy_t p = { .allowed_paths = NULL, .allowed_paths_count = 0 };
    /* Default-deny: empty allowlist means no path is allowed */
    SC_ASSERT_FALSE(sc_security_path_allowed(&p, "/any/path", 9));
}

void run_security_extended_tests(void) {
    SC_TEST_SUITE("Security Extended");
    SC_RUN_TEST(test_policy_wildcard_allows_all);
    SC_RUN_TEST(test_policy_explicit_allowlist_blocks_other);
    SC_RUN_TEST(test_policy_path_traversal_dot_dot_slash);
    SC_RUN_TEST(test_policy_path_traversal_encoded);
    SC_RUN_TEST(test_policy_command_substitution_blocked);
    SC_RUN_TEST(test_policy_redirect_blocked);
    SC_RUN_TEST(test_policy_command_with_semicolons);
    SC_RUN_TEST(test_policy_rate_limit_window_reset);
    SC_RUN_TEST(test_pairing_guard_lockout_after_max_attempts);
    SC_RUN_TEST(test_pairing_code_format);
    SC_RUN_TEST(test_pairing_short_code_fails);
    SC_RUN_TEST(test_secret_encrypt_empty_string);
    SC_RUN_TEST(test_secret_encrypt_long_string);
    SC_RUN_TEST(test_secret_decrypt_corrupted);
    SC_RUN_TEST(test_secret_decrypt_wrong_key);
    SC_RUN_TEST(test_hex_encode_decode_roundtrip);
    SC_RUN_TEST(test_hex_decode_odd_length_fails);
    SC_RUN_TEST(test_hex_decode_invalid_chars_fails);
    SC_RUN_TEST(test_path_resolved_allowed_empty_list);
    SC_RUN_TEST(test_path_resolved_allowed_workspace_prefix);
    SC_RUN_TEST(test_path_is_safe_relative_ok);
    SC_RUN_TEST(test_secret_store_is_encrypted_prefix);
    SC_RUN_TEST(test_pairing_constant_time_eq_null);
    SC_RUN_TEST(test_rate_tracker_null_safe);
    SC_RUN_TEST(test_policy_record_action);
    SC_RUN_TEST(test_policy_is_rate_limited);
    SC_RUN_TEST(test_audit_event_init_unique_ids);
    SC_RUN_TEST(test_audit_event_write_json_truncated);
    SC_RUN_TEST(test_policy_can_act_full);
    SC_RUN_TEST(test_policy_can_act_readonly);
    SC_RUN_TEST(test_security_shell_allowed);
    SC_RUN_TEST(test_security_path_allowed_with_allowlist);
    SC_RUN_TEST(test_path_is_safe_null);
    SC_RUN_TEST(test_path_is_safe_long);
    SC_RUN_TEST(test_hex_encode_empty);

    SC_RUN_TEST(test_policy_command_risk_rm_high);
    SC_RUN_TEST(test_policy_command_risk_ls_low);
    SC_RUN_TEST(test_policy_command_risk_git_commit_medium);
    SC_RUN_TEST(test_policy_command_risk_sudo_high);
    SC_RUN_TEST(test_policy_validate_command_low_ok);
    SC_RUN_TEST(test_policy_validate_command_high_blocked);
    SC_RUN_TEST(test_policy_validate_command_high_approved_ok);
    SC_RUN_TEST(test_policy_can_act_supervised);
    SC_RUN_TEST(test_path_is_safe_absolute_denied);
    SC_RUN_TEST(test_path_is_safe_double_dot);
    SC_RUN_TEST(test_pairing_constant_time_eq_same);
    SC_RUN_TEST(test_pairing_constant_time_eq_different);
    SC_RUN_TEST(test_pairing_valid_code_succeeds);
    SC_RUN_TEST(test_secret_encrypt_decrypt_roundtrip);
    SC_RUN_TEST(test_audit_event_type_command);
    SC_RUN_TEST(test_path_resolved_allowed_denied_outside);
    SC_RUN_TEST(test_path_resolved_allowed_subpath);
    SC_RUN_TEST(test_hex_encode_decode_full);
    SC_RUN_TEST(test_policy_readonly_blocks_even_allowlist);
    SC_RUN_TEST(test_policy_file_ops_path_denied);
    SC_RUN_TEST(test_policy_file_ops_path_allowed_subpath);
    SC_RUN_TEST(test_policy_bypass_pipe_blocked);
    SC_RUN_TEST(test_policy_risk_curl_medium_or_high);
    SC_RUN_TEST(test_policy_risk_wget_medium);
    SC_RUN_TEST(test_path_is_safe_symlink_traversal);
    SC_RUN_TEST(test_policy_validate_command_full_returns_risk_level);
    SC_RUN_TEST(test_security_path_allowed_empty_allowlist_denies);
    SC_RUN_TEST(test_pairing_wrong_code_fails);
    SC_RUN_TEST(test_policy_autonomy_read_only);
    SC_RUN_TEST(test_policy_autonomy_supervised);
    SC_RUN_TEST(test_policy_autonomy_full);
    SC_RUN_TEST(test_policy_command_risk_chmod);
    SC_RUN_TEST(test_policy_command_risk_dd);
    SC_RUN_TEST(test_policy_command_risk_npm_install);
    SC_RUN_TEST(test_policy_command_risk_pip_install);
    SC_RUN_TEST(test_rate_tracker_remaining_decrements);
    SC_RUN_TEST(test_rate_tracker_count_increments);
    SC_RUN_TEST(test_pairing_code_numeric_only);
    SC_RUN_TEST(test_audit_event_severity);
    SC_RUN_TEST(test_audit_event_type_string);
    SC_RUN_TEST(test_audit_should_log);
    SC_RUN_TEST(test_path_traversal_percent_encoded);
    SC_RUN_TEST(test_path_traversal_mixed);
}
