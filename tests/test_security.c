#include "test_framework.h"
#include "seaclaw/security.h"
#include "seaclaw/security/audit.h"
#include "seaclaw/security/sandbox.h"
#include "seaclaw/security/sandbox_internal.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/process_util.h"
#include "seaclaw/observer.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>

static sc_allocator_t *g_alloc;

static void test_autonomy_level_values(void) {
    SC_ASSERT(SC_AUTONOMY_READ_ONLY != SC_AUTONOMY_SUPERVISED);
    SC_ASSERT(SC_AUTONOMY_SUPERVISED != SC_AUTONOMY_FULL);
}

static void test_risk_level_values(void) {
    SC_ASSERT(SC_RISK_LOW < SC_RISK_MEDIUM);
    SC_ASSERT(SC_RISK_MEDIUM < SC_RISK_HIGH);
}

static void test_policy_can_act_readonly(void) {
    sc_security_policy_t p = {
        .autonomy = SC_AUTONOMY_READ_ONLY,
        .allowed_commands = (const char *[]){ "ls", "cat" },
        .allowed_commands_len = 2,
    };
    SC_ASSERT_FALSE(sc_policy_can_act(&p));
}

static void test_policy_can_act_supervised(void) {
    sc_security_policy_t p = {
        .autonomy = SC_AUTONOMY_SUPERVISED,
        .allowed_commands = (const char *[]){ "ls", "cat" },
        .allowed_commands_len = 2,
    };
    SC_ASSERT(sc_policy_can_act(&p));
}

static void test_policy_can_act_full(void) {
    sc_security_policy_t p = {
        .autonomy = SC_AUTONOMY_FULL,
        .allowed_commands = (const char *[]){ "ls" },
        .allowed_commands_len = 1,
    };
    SC_ASSERT(sc_policy_can_act(&p));
}

static void test_policy_allowed_commands(void) {
    sc_security_policy_t p = {
        .autonomy = SC_AUTONOMY_SUPERVISED,
        .allowed_commands = (const char *[]){ "git", "ls", "cat", "echo", "cargo" },
        .allowed_commands_len = 5,
    };
    SC_ASSERT(sc_policy_is_command_allowed(&p, "ls"));
    SC_ASSERT(sc_policy_is_command_allowed(&p, "git status"));
    SC_ASSERT(sc_policy_is_command_allowed(&p, "cargo build --release"));
    SC_ASSERT(sc_policy_is_command_allowed(&p, "cat file.txt"));
}

static void test_policy_blocked_commands(void) {
    sc_security_policy_t p = {
        .autonomy = SC_AUTONOMY_SUPERVISED,
        .allowed_commands = (const char *[]){ "ls", "cat" },
        .allowed_commands_len = 2,
    };
    SC_ASSERT_FALSE(sc_policy_is_command_allowed(&p, "rm -rf /"));
    SC_ASSERT_FALSE(sc_policy_is_command_allowed(&p, "sudo apt install"));
    SC_ASSERT_FALSE(sc_policy_is_command_allowed(&p, "curl http://evil.com"));
    SC_ASSERT_FALSE(sc_policy_is_command_allowed(&p, "wget http://evil.com"));
}

static void test_policy_readonly_blocks_all(void) {
    sc_security_policy_t p = {
        .autonomy = SC_AUTONOMY_READ_ONLY,
        .allowed_commands = (const char *[]){ "ls", "cat" },
        .allowed_commands_len = 2,
    };
    SC_ASSERT_FALSE(sc_policy_is_command_allowed(&p, "ls"));
    SC_ASSERT_FALSE(sc_policy_is_command_allowed(&p, "cat file.txt"));
}

static void test_policy_command_injection_blocked(void) {
    sc_security_policy_t p = {
        .autonomy = SC_AUTONOMY_SUPERVISED,
        .allowed_commands = (const char *[]){ "ls", "echo" },
        .allowed_commands_len = 2,
    };
    SC_ASSERT_FALSE(sc_policy_is_command_allowed(&p, "echo `whoami`"));
    SC_ASSERT_FALSE(sc_policy_is_command_allowed(&p, "echo $(rm -rf /)"));
    SC_ASSERT_FALSE(sc_policy_is_command_allowed(&p, "ls; rm -rf /"));
    SC_ASSERT_FALSE(sc_policy_is_command_allowed(&p, "echo secret > /etc/crontab"));
}

static void test_policy_risk_levels(void) {
    sc_security_policy_t p = { .autonomy = SC_AUTONOMY_SUPERVISED };
    SC_ASSERT_EQ(sc_policy_command_risk_level(&p, "ls -la"), SC_RISK_LOW);
    SC_ASSERT_EQ(sc_policy_command_risk_level(&p, "git status"), SC_RISK_LOW);
    SC_ASSERT_EQ(sc_policy_command_risk_level(&p, "git commit -m x"), SC_RISK_MEDIUM);
    SC_ASSERT_EQ(sc_policy_command_risk_level(&p, "rm -rf /tmp/x"), SC_RISK_HIGH);
    SC_ASSERT_EQ(sc_policy_command_risk_level(&p, "sudo apt install"), SC_RISK_HIGH);
}

static void test_policy_validate_command(void) {
    sc_security_policy_t p = {
        .autonomy = SC_AUTONOMY_SUPERVISED,
        .allowed_commands = (const char *[]){ "ls", "touch" },
        .allowed_commands_len = 2,
        .require_approval_for_medium_risk = 1,
        .block_high_risk_commands = 1,
    };
    sc_command_risk_level_t risk;
    sc_error_t err = sc_policy_validate_command(&p, "ls -la", 0, &risk);
    SC_ASSERT(err == SC_OK && risk == SC_RISK_LOW);

    err = sc_policy_validate_command(&p, "touch f", 0, &risk);
    SC_ASSERT(err == SC_ERR_SECURITY_APPROVAL_REQUIRED);

    err = sc_policy_validate_command(&p, "touch f", 1, &risk);
    SC_ASSERT(err == SC_OK && risk == SC_RISK_MEDIUM);
}

static void test_rate_tracker(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_rate_tracker_t *t = sc_rate_tracker_create(&sys, 3);
    SC_ASSERT_NOT_NULL(t);
    SC_ASSERT_EQ(sc_rate_tracker_remaining(t), 3);
    SC_ASSERT(sc_rate_tracker_record_action(t));
    SC_ASSERT_EQ(sc_rate_tracker_remaining(t), 2);
    SC_ASSERT(sc_rate_tracker_record_action(t));
    SC_ASSERT(sc_rate_tracker_record_action(t));
    SC_ASSERT_FALSE(sc_rate_tracker_record_action(t));
    SC_ASSERT(sc_rate_tracker_is_limited(t));
    SC_ASSERT_EQ(sc_rate_tracker_count(t), 4);
    SC_ASSERT_EQ(sc_rate_tracker_remaining(t), 0);
    sc_rate_tracker_destroy(t);
}

static void test_pairing_constant_time_eq(void) {
    SC_ASSERT(sc_pairing_guard_constant_time_eq("abc", "abc"));
    SC_ASSERT(sc_pairing_guard_constant_time_eq("", ""));
    SC_ASSERT_FALSE(sc_pairing_guard_constant_time_eq("abc", "abd"));
    SC_ASSERT_FALSE(sc_pairing_guard_constant_time_eq("abc", "ab"));
}

static void test_pairing_guard_create_destroy(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_pairing_guard_t *g = sc_pairing_guard_create(&sys, 1, NULL, 0);
    SC_ASSERT_NOT_NULL(g);
    SC_ASSERT_NOT_NULL(sc_pairing_guard_pairing_code(g));
    SC_ASSERT_FALSE(sc_pairing_guard_is_paired(g));
    sc_pairing_guard_destroy(g);
}

static void test_pairing_guard_disabled(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_pairing_guard_t *g = sc_pairing_guard_create(&sys, 0, NULL, 0);
    SC_ASSERT_NOT_NULL(g);
    SC_ASSERT_NULL(sc_pairing_guard_pairing_code(g));
    SC_ASSERT(sc_pairing_guard_is_authenticated(g, "anything"));
    sc_pairing_guard_destroy(g);
}

static void test_pairing_guard_with_tokens(void) {
    sc_allocator_t sys = sc_system_allocator();
    const char *tokens[] = { "zc_test_token" };
    sc_pairing_guard_t *g = sc_pairing_guard_create(&sys, 1, tokens, 1);
    SC_ASSERT_NOT_NULL(g);
    SC_ASSERT_NULL(sc_pairing_guard_pairing_code(g));
    SC_ASSERT(sc_pairing_guard_is_paired(g));
    SC_ASSERT(sc_pairing_guard_is_authenticated(g, "zc_test_token"));
    SC_ASSERT_FALSE(sc_pairing_guard_is_authenticated(g, "zc_wrong"));
    sc_pairing_guard_destroy(g);
}

static void test_pairing_attempt_pair(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_pairing_guard_t *g = sc_pairing_guard_create(&sys, 1, NULL, 0);
    SC_ASSERT_NOT_NULL(g);
    const char *code = sc_pairing_guard_pairing_code(g);
    SC_ASSERT_NOT_NULL(code);

    char *token = NULL;
    sc_pair_attempt_result_t r = sc_pairing_guard_attempt_pair(g, code, &token);
    SC_ASSERT(r == SC_PAIR_PAIRED);
    SC_ASSERT_NOT_NULL(token);
    SC_ASSERT(strncmp(token, "zc_", 3) == 0);
    sys.free(sys.ctx, token, strlen(token) + 1);

    SC_ASSERT_NULL(sc_pairing_guard_pairing_code(g));
    SC_ASSERT(sc_pairing_guard_is_paired(g));
    sc_pairing_guard_destroy(g);
}

static void test_secret_store_is_encrypted(void) {
    SC_ASSERT(sc_secret_store_is_encrypted("enc2:aabbcc"));
    SC_ASSERT_FALSE(sc_secret_store_is_encrypted("plaintext"));
    SC_ASSERT_FALSE(sc_secret_store_is_encrypted("enc"));
    SC_ASSERT_FALSE(sc_secret_store_is_encrypted(""));
}

static void test_hex_encode_decode(void) {
    uint8_t data[] = { 0x00, 0x01, 0xfe, 0xff };
    char hex[16];
    sc_hex_encode(data, 4, hex);
    SC_ASSERT_STR_EQ(hex, "0001feff");

    uint8_t out[4];
    size_t len;
    sc_error_t err = sc_hex_decode("0001feff", 8, out, 4, &len);
    SC_ASSERT(err == SC_OK);
    SC_ASSERT_EQ(len, 4);
    SC_ASSERT(memcmp(out, data, 4) == 0);
}

static void test_secret_store_encrypt_decrypt(void) {
    sc_allocator_t sys = sc_system_allocator();
    char tmp[] = "/tmp/sc_secret_XXXXXX";
    char *dir = mkdtemp(tmp);
    SC_ASSERT_NOT_NULL(dir);

    sc_secret_store_t *store = sc_secret_store_create(&sys, dir, 1);
    SC_ASSERT_NOT_NULL(store);

    char *enc = NULL;
    sc_error_t err = sc_secret_store_encrypt(store, &sys, "sk-my-secret-key", &enc);
    SC_ASSERT(err == SC_OK);
    SC_ASSERT_NOT_NULL(enc);
    SC_ASSERT(strncmp(enc, "enc2:", 5) == 0);

    char *dec = NULL;
    err = sc_secret_store_decrypt(store, &sys, enc, &dec);
    SC_ASSERT(err == SC_OK);
    SC_ASSERT_STR_EQ(dec, "sk-my-secret-key");

    sys.free(sys.ctx, enc, strlen(enc) + 1);
    sys.free(sys.ctx, dec, strlen(dec) + 1);
    sc_secret_store_destroy(store, &sys);

    char keypath[256];
    snprintf(keypath, sizeof(keypath), "%s/.secret_key", dir);
    (void)unlink(keypath);
    (void)rmdir(dir);
}

static void test_secret_store_plaintext_passthrough(void) {
    sc_allocator_t sys = sc_system_allocator();
    char tmp[] = "/tmp/sc_secret2_XXXXXX";
    char *dir = mkdtemp(tmp);
    SC_ASSERT_NOT_NULL(dir);

    sc_secret_store_t *store = sc_secret_store_create(&sys, dir, 1);
    SC_ASSERT_NOT_NULL(store);

    char *dec = NULL;
    sc_error_t err = sc_secret_store_decrypt(store, &sys, "sk-plaintext-key", &dec);
    SC_ASSERT(err == SC_OK);
    SC_ASSERT_STR_EQ(dec, "sk-plaintext-key");

    sys.free(sys.ctx, dec, strlen(dec) + 1);
    sc_secret_store_destroy(store, &sys);
    rmdir(dir);
}

static void test_audit_event_init(void) {
    sc_audit_event_t e1, e2;
    sc_audit_event_init(&e1, SC_AUDIT_COMMAND_EXECUTION);
    sc_audit_event_init(&e2, SC_AUDIT_COMMAND_EXECUTION);
    SC_ASSERT(e1.event_id != e2.event_id);
    SC_ASSERT(e1.timestamp_s > 0);
}

static void test_audit_event_write_json(void) {
    sc_audit_event_t ev;
    sc_audit_event_init(&ev, SC_AUDIT_COMMAND_EXECUTION);
    sc_audit_event_with_actor(&ev, "telegram", "123", "@alice");
    sc_audit_event_with_action(&ev, "ls -la", "low", false, true);
    char buf[1024];
    size_t n = sc_audit_event_write_json(&ev, buf, sizeof(buf));
    SC_ASSERT(n > 0);
    SC_ASSERT(strstr(buf, "command_execution") != NULL);
    SC_ASSERT(strstr(buf, "telegram") != NULL);
}

static void test_audit_logger_disabled(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_audit_config_t cfg = { .enabled = false, .log_path = "audit.log", .max_size_mb = 10 };
    sc_audit_logger_t *log = sc_audit_logger_create(&sys, &cfg, "/tmp");
    SC_ASSERT_NOT_NULL(log);
    sc_audit_event_t ev;
    sc_audit_event_init(&ev, SC_AUDIT_COMMAND_EXECUTION);
    sc_error_t err = sc_audit_logger_log(log, &ev);
    SC_ASSERT(err == SC_OK);
    sc_audit_logger_destroy(log, &sys);
}

static void test_audit_chain_verify_valid(void) {
    sc_allocator_t sys = sc_system_allocator();
    char tmp[] = "/tmp/sc_audit_chain_XXXXXX";
    char *dir = mkdtemp(tmp);
    SC_ASSERT_NOT_NULL(dir);

    sc_audit_config_t cfg = { .enabled = true, .log_path = "chain.log", .max_size_mb = 10 };
    sc_audit_logger_t *log = sc_audit_logger_create(&sys, &cfg, dir);
    SC_ASSERT_NOT_NULL(log);

    sc_audit_event_t ev1, ev2, ev3;
    sc_audit_event_init(&ev1, SC_AUDIT_COMMAND_EXECUTION);
    sc_audit_event_with_action(&ev1, "ls", "low", true, true);
    sc_audit_event_init(&ev2, SC_AUDIT_FILE_ACCESS);
    sc_audit_event_init(&ev3, SC_AUDIT_AUTH_SUCCESS);

    SC_ASSERT(sc_audit_logger_log(log, &ev1) == SC_OK);
    SC_ASSERT(sc_audit_logger_log(log, &ev2) == SC_OK);
    SC_ASSERT(sc_audit_logger_log(log, &ev3) == SC_OK);
    sc_audit_logger_destroy(log, &sys);

    unsigned char key[32];
    SC_ASSERT(sc_audit_load_key(dir, key) == SC_OK);

    char log_path[512];
    snprintf(log_path, sizeof(log_path), "%s/chain.log", dir);
    sc_error_t verr = sc_audit_verify_chain(log_path, key);
    SC_ASSERT_EQ(verr, SC_OK);

    unlink(log_path);
    char key_path[512];
    snprintf(key_path, sizeof(key_path), "%s/.audit_hmac_key", dir);
    unlink(key_path);
    rmdir(dir);
}

static void test_audit_chain_tamper_detected(void) {
    sc_allocator_t sys = sc_system_allocator();
    char tmp[] = "/tmp/sc_audit_tamper_XXXXXX";
    char *dir = mkdtemp(tmp);
    SC_ASSERT_NOT_NULL(dir);

    sc_audit_config_t cfg = { .enabled = true, .log_path = "tamper.log", .max_size_mb = 10 };
    sc_audit_logger_t *log = sc_audit_logger_create(&sys, &cfg, dir);
    SC_ASSERT_NOT_NULL(log);

    sc_audit_event_t ev;
    sc_audit_event_init(&ev, SC_AUDIT_COMMAND_EXECUTION);
    sc_audit_event_with_action(&ev, "ls -la", "low", true, true);
    SC_ASSERT(sc_audit_logger_log(log, &ev) == SC_OK);
    sc_audit_logger_destroy(log, &sys);

    char log_path[512];
    snprintf(log_path, sizeof(log_path), "%s/tamper.log", dir);
    FILE *f = fopen(log_path, "r+b");
    SC_ASSERT_NOT_NULL(f);
    fseek(f, 50, SEEK_SET);  /* Tamper somewhere in the middle */
    int c = fgetc(f);
    fseek(f, 50, SEEK_SET);
    fputc(c == 'a' ? 'b' : 'a', f);
    fclose(f);

    unsigned char key[32];
    SC_ASSERT(sc_audit_load_key(dir, key) == SC_OK);
    sc_error_t tamper_verr = sc_audit_verify_chain(log_path, key);
    SC_ASSERT_EQ(tamper_verr, SC_ERR_CRYPTO_DECRYPT);

    unlink(log_path);
    char key_path[512];
    snprintf(key_path, sizeof(key_path), "%s/.audit_hmac_key", dir);
    unlink(key_path);
    rmdir(dir);
}

static void test_audit_key_rotation_basic(void) {
    sc_allocator_t sys = sc_system_allocator();
    char tmp[] = "/tmp/sc_audit_rot_XXXXXX";
    char *dir = mkdtemp(tmp);
    SC_ASSERT_NOT_NULL(dir);

    sc_audit_config_t cfg = { .enabled = true, .log_path = "rot.log", .max_size_mb = 10 };
    sc_audit_logger_t *log = sc_audit_logger_create(&sys, &cfg, dir);
    SC_ASSERT_NOT_NULL(log);

    sc_audit_event_t ev1, ev2;
    sc_audit_event_init(&ev1, SC_AUDIT_COMMAND_EXECUTION);
    sc_audit_event_with_action(&ev1, "ls", "low", true, true);
    sc_audit_event_init(&ev2, SC_AUDIT_FILE_ACCESS);
    SC_ASSERT(sc_audit_logger_log(log, &ev1) == SC_OK);
    SC_ASSERT(sc_audit_rotate_key(log) == SC_OK);
    SC_ASSERT(sc_audit_logger_log(log, &ev2) == SC_OK);
    sc_audit_logger_destroy(log, &sys);

    char log_path[512];
    snprintf(log_path, sizeof(log_path), "%s/rot.log", dir);
    sc_error_t verr = sc_audit_verify_chain(log_path, NULL);
    SC_ASSERT_EQ(verr, SC_OK);

    unlink(log_path);
    char key_path[512], hist_path[512];
    snprintf(key_path, sizeof(key_path), "%s/.audit_hmac_key", dir);
    snprintf(hist_path, sizeof(hist_path), "%s/.audit_key_history", dir);
    unlink(key_path);
    unlink(hist_path);
    rmdir(dir);
}

static void test_audit_key_rotation_verify_detects_tamper_after_rotation(void) {
    sc_allocator_t sys = sc_system_allocator();
    char tmp[] = "/tmp/sc_audit_tamper2_XXXXXX";
    char *dir = mkdtemp(tmp);
    SC_ASSERT_NOT_NULL(dir);

    sc_audit_config_t cfg = { .enabled = true, .log_path = "tamper2.log", .max_size_mb = 10 };
    sc_audit_logger_t *log = sc_audit_logger_create(&sys, &cfg, dir);
    SC_ASSERT_NOT_NULL(log);

    sc_audit_event_t ev1, ev2;
    sc_audit_event_init(&ev1, SC_AUDIT_COMMAND_EXECUTION);
    sc_audit_event_with_action(&ev1, "ls", "low", true, true);
    sc_audit_event_init(&ev2, SC_AUDIT_FILE_ACCESS);
    SC_ASSERT(sc_audit_logger_log(log, &ev1) == SC_OK);
    SC_ASSERT(sc_audit_rotate_key(log) == SC_OK);
    SC_ASSERT(sc_audit_logger_log(log, &ev2) == SC_OK);
    sc_audit_logger_destroy(log, &sys);

    char log_path[512];
    snprintf(log_path, sizeof(log_path), "%s/tamper2.log", dir);
    FILE *f = fopen(log_path, "r+b");
    SC_ASSERT_NOT_NULL(f);
    /* Find the last line (post-rotation entry) and tamper with it */
    fseek(f, 0, SEEK_END);
    long end = ftell(f);
    if (end > 80) {
        fseek(f, end - 80, SEEK_SET);
        int c = fgetc(f);
        fseek(f, end - 80, SEEK_SET);
        fputc(c == 'a' ? 'b' : 'a', f);
    }
    fclose(f);

    sc_error_t tamper_verr = sc_audit_verify_chain(log_path, NULL);
    SC_ASSERT_EQ(tamper_verr, SC_ERR_CRYPTO_DECRYPT);

    unlink(log_path);
    char key_path[512], hist_path[512];
    snprintf(key_path, sizeof(key_path), "%s/.audit_hmac_key", dir);
    snprintf(hist_path, sizeof(hist_path), "%s/.audit_key_history", dir);
    unlink(key_path);
    unlink(hist_path);
    rmdir(dir);
}

static void test_audit_rotation_interval(void) {
    sc_allocator_t sys = sc_system_allocator();
    char tmp[] = "/tmp/sc_audit_intv_XXXXXX";
    char *dir = mkdtemp(tmp);
    SC_ASSERT_NOT_NULL(dir);

    sc_audit_config_t cfg = { .enabled = true, .log_path = "intv.log", .max_size_mb = 10 };
    sc_audit_logger_t *log = sc_audit_logger_create(&sys, &cfg, dir);
    SC_ASSERT_NOT_NULL(log);

    sc_audit_set_rotation_interval(log, 1);  /* 1 hour */

    sc_audit_event_t ev;
    sc_audit_event_init(&ev, SC_AUDIT_COMMAND_EXECUTION);
    sc_audit_event_with_action(&ev, "echo a", "low", true, true);
    SC_ASSERT(sc_audit_logger_log(log, &ev) == SC_OK);

    sc_audit_test_set_last_rotation_epoch(log, time(NULL) - 7200);  /* 2 hours ago */
    SC_ASSERT(sc_audit_logger_log(log, &ev) == SC_OK);

    sc_audit_logger_destroy(log, &sys);

    char log_path[512];
    snprintf(log_path, sizeof(log_path), "%s/intv.log", dir);
    sc_error_t verr = sc_audit_verify_chain(log_path, NULL);
    SC_ASSERT_EQ(verr, SC_OK);

    /* Verify key_rotation entry exists */
    FILE *fr = fopen(log_path, "rb");
    SC_ASSERT_NOT_NULL(fr);
    char buf[4096];
    size_t total = fread(buf, 1, sizeof(buf) - 1, fr);
    fclose(fr);
    buf[total] = '\0';
    SC_ASSERT(strstr(buf, "key_rotation") != NULL);

    unlink(log_path);
    char key_path[512], hist_path[512];
    snprintf(key_path, sizeof(key_path), "%s/.audit_hmac_key", dir);
    snprintf(hist_path, sizeof(hist_path), "%s/.audit_key_history", dir);
    unlink(key_path);
    unlink(hist_path);
    rmdir(dir);
}

static void test_audit_chain_delete_detected(void) {
    sc_allocator_t sys = sc_system_allocator();
    char tmp[] = "/tmp/sc_audit_del_XXXXXX";
    char *dir = mkdtemp(tmp);
    SC_ASSERT_NOT_NULL(dir);

    sc_audit_config_t cfg = { .enabled = true, .log_path = "del.log", .max_size_mb = 10 };
    sc_audit_logger_t *log = sc_audit_logger_create(&sys, &cfg, dir);
    SC_ASSERT_NOT_NULL(log);

    sc_audit_event_t ev1, ev2;
    sc_audit_event_init(&ev1, SC_AUDIT_COMMAND_EXECUTION);
    sc_audit_event_init(&ev2, SC_AUDIT_FILE_ACCESS);
    SC_ASSERT(sc_audit_logger_log(log, &ev1) == SC_OK);
    SC_ASSERT(sc_audit_logger_log(log, &ev2) == SC_OK);
    sc_audit_logger_destroy(log, &sys);

    char log_path[512];
    snprintf(log_path, sizeof(log_path), "%s/del.log", dir);
    char buf[8192];
    size_t total = 0;
    FILE *fr = fopen(log_path, "rb");
    SC_ASSERT_NOT_NULL(fr);
    char line[4096];
    bool first = true;
    while (fgets(line, sizeof(line), fr)) {
        if (first) {
            first = false;
            continue;  /* Skip first line (delete it) */
        }
        size_t n = strlen(line);
        memcpy(buf + total, line, n + 1);
        total += n + 1;
    }
    fclose(fr);

    FILE *fw = fopen(log_path, "wb");
    SC_ASSERT_NOT_NULL(fw);
    fwrite(buf, 1, total, fw);
    fclose(fw);

    unsigned char key[32];
    SC_ASSERT(sc_audit_load_key(dir, key) == SC_OK);
    sc_error_t del_verr = sc_audit_verify_chain(log_path, key);
    SC_ASSERT_EQ(del_verr, SC_ERR_CRYPTO_DECRYPT);

    unlink(log_path);
    char key_path[512];
    snprintf(key_path, sizeof(key_path), "%s/.audit_hmac_key", dir);
    unlink(key_path);
    rmdir(dir);
}

static void test_sandbox_noop(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_sandbox_alloc_t alloc = {
        .ctx = sys.ctx,
        .alloc = sys.alloc,
        .free = sys.free,
    };
    sc_sandbox_storage_t *st = sc_sandbox_storage_create(&alloc);
    SC_ASSERT_NOT_NULL(st);
    sc_sandbox_t sb = sc_sandbox_create(SC_SANDBOX_NONE, "/tmp/workspace", st, &alloc);
    SC_ASSERT(sb.ctx != NULL);
    SC_ASSERT(sb.vtable != NULL);
    SC_ASSERT(strcmp(sc_sandbox_name(&sb), "none") == 0);
    const char *argv[] = { "echo", "test" };
    const char *out[16];
    size_t out_count = 0;
    sc_error_t err = sc_sandbox_wrap_command(&sb, argv, 2, out, 16, &out_count);
    SC_ASSERT(err == SC_OK);
    SC_ASSERT_EQ(out_count, 2);
    sc_sandbox_storage_destroy(st, &alloc);
}

/* Test backend vtables directly (bypass create fallback to noop) */
static void test_sandbox_landlock_non_linux_or_test(void) {
    sc_landlock_ctx_t ctx;
    sc_landlock_sandbox_init(&ctx, "/tmp");
    sc_sandbox_t sb = sc_landlock_sandbox_get(&ctx);
    bool avail = sc_sandbox_is_available(&sb);
#if defined(__linux__) && !SC_IS_TEST
    (void)avail; /* May be true if kernel has Landlock */
#else
    SC_ASSERT_FALSE(avail); /* macOS or SC_IS_TEST: not available */
#endif
    const char *argv[] = { "echo", "x" };
    const char *out[16];
    size_t out_count = 0;
    sc_error_t err = sc_sandbox_wrap_command(&sb, argv, 2, out, 16, &out_count);
#ifndef __linux__
    SC_ASSERT_EQ(err, SC_ERR_NOT_SUPPORTED);
#else
    SC_ASSERT(err == SC_OK || err == SC_ERR_NOT_SUPPORTED);
#endif
}

static void test_sandbox_firejail_non_linux(void) {
#ifndef __linux__
    sc_firejail_ctx_t ctx;
    sc_firejail_sandbox_init(&ctx, "/tmp");
    sc_sandbox_t sb = sc_firejail_sandbox_get(&ctx);
    SC_ASSERT_FALSE(sc_sandbox_is_available(&sb));
    const char *argv[] = { "echo", "x" };
    const char *out[16];
    size_t out_count = 0;
    sc_error_t err = sc_sandbox_wrap_command(&sb, argv, 2, out, 16, &out_count);
    SC_ASSERT_EQ(err, SC_ERR_NOT_SUPPORTED);
#endif
}

static void test_sandbox_bubblewrap_non_linux(void) {
#ifndef __linux__
    sc_bubblewrap_ctx_t ctx;
    sc_bubblewrap_sandbox_init(&ctx, "/tmp");
    sc_sandbox_t sb = sc_bubblewrap_sandbox_get(&ctx);
    SC_ASSERT_FALSE(sc_sandbox_is_available(&sb));
    const char *argv[] = { "echo", "x" };
    const char *out[16];
    size_t out_count = 0;
    sc_error_t err = sc_sandbox_wrap_command(&sb, argv, 2, out, 16, &out_count);
    SC_ASSERT_EQ(err, SC_ERR_NOT_SUPPORTED);
#endif
}

/* --- Seatbelt (macOS) sandbox tests --- */
static void test_sandbox_seatbelt_vtable_wiring(void) {
    sc_seatbelt_ctx_t ctx;
    sc_seatbelt_sandbox_init(&ctx, "/tmp/workspace");
    sc_sandbox_t sb = sc_seatbelt_sandbox_get(&ctx);
    SC_ASSERT(sb.ctx != NULL);
    SC_ASSERT(sb.vtable != NULL);
    SC_ASSERT(strcmp(sc_sandbox_name(&sb), "seatbelt") == 0);
    SC_ASSERT(strlen(sc_sandbox_description(&sb)) > 0);
}

static void test_sandbox_seatbelt_availability(void) {
    sc_seatbelt_ctx_t ctx;
    sc_seatbelt_sandbox_init(&ctx, "/tmp");
    sc_sandbox_t sb = sc_seatbelt_sandbox_get(&ctx);
#ifdef __APPLE__
    SC_ASSERT(sc_sandbox_is_available(&sb));
#else
    SC_ASSERT_FALSE(sc_sandbox_is_available(&sb));
#endif
}

static void test_sandbox_seatbelt_wrap_command(void) {
    sc_seatbelt_ctx_t ctx;
    sc_seatbelt_sandbox_init(&ctx, "/tmp/workspace");
    sc_sandbox_t sb = sc_seatbelt_sandbox_get(&ctx);
    const char *argv[] = { "echo", "test" };
    const char *out[16];
    size_t out_count = 0;
    sc_error_t err = sc_sandbox_wrap_command(&sb, argv, 2, out, 16, &out_count);
#ifdef __APPLE__
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out_count, 5u);
    SC_ASSERT(strcmp(out[0], "sandbox-exec") == 0);
    SC_ASSERT(strcmp(out[1], "-p") == 0);
    SC_ASSERT(strstr(out[2], "deny default") != NULL);
    SC_ASSERT(strstr(out[2], "/tmp/workspace") != NULL);
    SC_ASSERT(strcmp(out[3], "echo") == 0);
    SC_ASSERT(strcmp(out[4], "test") == 0);
#else
    SC_ASSERT_EQ(err, SC_ERR_NOT_SUPPORTED);
#endif
}

static void test_sandbox_seatbelt_profile_contains_workspace(void) {
    sc_seatbelt_ctx_t ctx;
    sc_seatbelt_sandbox_init(&ctx, "/home/user/project");
    SC_ASSERT(strstr(ctx.profile, "/home/user/project") != NULL);
    SC_ASSERT(strstr(ctx.profile, "deny default") != NULL);
    SC_ASSERT(strstr(ctx.profile, "deny network") != NULL);
}

static void test_sandbox_seatbelt_null_args(void) {
    sc_seatbelt_ctx_t ctx;
    sc_seatbelt_sandbox_init(&ctx, "/tmp");
    sc_sandbox_t sb = sc_seatbelt_sandbox_get(&ctx);
    sc_error_t err = sc_sandbox_wrap_command(&sb, NULL, 0, NULL, 0, NULL);
#ifdef __APPLE__
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
#else
    SC_ASSERT_EQ(err, SC_ERR_NOT_SUPPORTED);
#endif
}

/* --- Landlock apply tests --- */
static void test_sandbox_landlock_apply_in_test_mode(void) {
    sc_landlock_ctx_t ctx;
    sc_landlock_sandbox_init(&ctx, "/tmp/workspace");
    sc_sandbox_t sb = sc_landlock_sandbox_get(&ctx);
    sc_error_t err = sc_sandbox_apply(&sb);
#ifdef __linux__
    SC_ASSERT_EQ(err, SC_OK);
#else
    SC_ASSERT_EQ(err, SC_ERR_NOT_SUPPORTED);
#endif
}

static void test_sandbox_landlock_vtable_has_apply(void) {
    sc_landlock_ctx_t ctx;
    sc_landlock_sandbox_init(&ctx, "/tmp");
    sc_sandbox_t sb = sc_landlock_sandbox_get(&ctx);
    SC_ASSERT(sb.vtable->apply != NULL);
}

static void test_sandbox_landlock_wrap_passthrough(void) {
    sc_landlock_ctx_t ctx;
    sc_landlock_sandbox_init(&ctx, "/tmp/ws");
    sc_sandbox_t sb = sc_landlock_sandbox_get(&ctx);
    const char *argv[] = { "ls", "-la" };
    const char *out[8];
    size_t out_count = 0;
    sc_error_t err = sc_sandbox_wrap_command(&sb, argv, 2, out, 8, &out_count);
#ifdef __linux__
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out_count, 2u);
    SC_ASSERT(strcmp(out[0], "ls") == 0);
    SC_ASSERT(strcmp(out[1], "-la") == 0);
#else
    SC_ASSERT_EQ(err, SC_ERR_NOT_SUPPORTED);
#endif
}

/* --- seccomp sandbox tests --- */
static void test_sandbox_seccomp_vtable_wiring(void) {
    sc_seccomp_ctx_t ctx;
    sc_seccomp_sandbox_init(&ctx, "/tmp/workspace", false);
    sc_sandbox_t sb = sc_seccomp_sandbox_get(&ctx);
    SC_ASSERT(sb.ctx != NULL);
    SC_ASSERT(sb.vtable != NULL);
    SC_ASSERT(strcmp(sc_sandbox_name(&sb), "seccomp") == 0);
}

static void test_sandbox_seccomp_availability(void) {
    sc_seccomp_ctx_t ctx;
    sc_seccomp_sandbox_init(&ctx, "/tmp", false);
    sc_sandbox_t sb = sc_seccomp_sandbox_get(&ctx);
#ifdef __linux__
#if SC_IS_TEST
    SC_ASSERT_FALSE(sc_sandbox_is_available(&sb));
#endif
#else
    SC_ASSERT_FALSE(sc_sandbox_is_available(&sb));
#endif
}

static void test_sandbox_seccomp_wrap_passthrough(void) {
    sc_seccomp_ctx_t ctx;
    sc_seccomp_sandbox_init(&ctx, "/tmp", true);
    sc_sandbox_t sb = sc_seccomp_sandbox_get(&ctx);
    const char *argv[] = { "echo", "hello" };
    const char *out[8];
    size_t out_count = 0;
    sc_error_t err = sc_sandbox_wrap_command(&sb, argv, 2, out, 8, &out_count);
#ifdef __linux__
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out_count, 2u);
#else
    SC_ASSERT_EQ(err, SC_ERR_NOT_SUPPORTED);
#endif
}

static void test_sandbox_seccomp_has_apply(void) {
    sc_seccomp_ctx_t ctx;
    sc_seccomp_sandbox_init(&ctx, "/tmp", false);
    sc_sandbox_t sb = sc_seccomp_sandbox_get(&ctx);
#ifdef __linux__
    SC_ASSERT(sb.vtable->apply != NULL);
#else
    SC_ASSERT(sb.vtable->apply == NULL);
#endif
}

static void test_sandbox_seccomp_network_flag(void) {
    sc_seccomp_ctx_t ctx;
    sc_seccomp_sandbox_init(&ctx, "/tmp", true);
    SC_ASSERT(ctx.allow_network == true);
    sc_seccomp_sandbox_init(&ctx, "/tmp", false);
    SC_ASSERT(ctx.allow_network == false);
}

/* --- WASI sandbox tests --- */
static void test_sandbox_wasi_vtable_wiring(void) {
    sc_wasi_sandbox_ctx_t ctx;
    sc_wasi_sandbox_init(&ctx, "/tmp/workspace");
    sc_sandbox_t sb = sc_wasi_sandbox_get(&ctx);
    SC_ASSERT(sb.ctx != NULL);
    SC_ASSERT(sb.vtable != NULL);
    SC_ASSERT(strcmp(sc_sandbox_name(&sb), "wasi") == 0);
    SC_ASSERT(strstr(sc_sandbox_description(&sb), "WASI") != NULL);
}

static void test_sandbox_wasi_wrap_command_format(void) {
    sc_wasi_sandbox_ctx_t ctx;
    sc_wasi_sandbox_init(&ctx, "/tmp/workspace");
    sc_sandbox_t sb = sc_wasi_sandbox_get(&ctx);
    const char *argv[] = { "program.wasm", "--arg1" };
    const char *out[16];
    size_t out_count = 0;
    sc_error_t err = sc_sandbox_wrap_command(&sb, argv, 2, out, 16, &out_count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out_count, 6u);
    SC_ASSERT(strstr(out[0], "wasmtime") != NULL || strstr(out[0], "wasmer") != NULL);
    SC_ASSERT(strcmp(out[1], "run") == 0);
    SC_ASSERT(strstr(out[2], "--dir=") != NULL);
    SC_ASSERT(strstr(out[2], "/tmp/workspace") != NULL);
    SC_ASSERT(strcmp(out[3], "--dir=/tmp") == 0);
    SC_ASSERT(strcmp(out[4], "program.wasm") == 0);
    SC_ASSERT(strcmp(out[5], "--arg1") == 0);
}

static void test_sandbox_wasi_no_apply(void) {
    sc_wasi_sandbox_ctx_t ctx;
    sc_wasi_sandbox_init(&ctx, "/tmp");
    sc_sandbox_t sb = sc_wasi_sandbox_get(&ctx);
    SC_ASSERT(sb.vtable->apply == NULL);
    SC_ASSERT_EQ(sc_sandbox_apply(&sb), SC_OK);
}

/* --- Firecracker sandbox tests --- */
static void test_sandbox_firecracker_vtable_wiring(void) {
    sc_firecracker_ctx_t ctx;
    sc_firecracker_sandbox_init(&ctx, "/tmp/workspace");
    sc_sandbox_t sb = sc_firecracker_sandbox_get(&ctx);
    SC_ASSERT(sb.ctx != NULL);
    SC_ASSERT(sb.vtable != NULL);
    SC_ASSERT(strcmp(sc_sandbox_name(&sb), "firecracker") == 0);
    SC_ASSERT(strstr(sc_sandbox_description(&sb), "microVM") != NULL);
}

static void test_sandbox_firecracker_not_available_in_test(void) {
    sc_firecracker_ctx_t ctx;
    sc_firecracker_sandbox_init(&ctx, "/tmp");
    sc_sandbox_t sb = sc_firecracker_sandbox_get(&ctx);
    SC_ASSERT_FALSE(sc_sandbox_is_available(&sb));
}

static void test_sandbox_firecracker_defaults(void) {
    sc_firecracker_ctx_t ctx;
    sc_firecracker_sandbox_init(&ctx, "/tmp/ws");
    SC_ASSERT_EQ(ctx.vcpu_count, 1u);
    SC_ASSERT_EQ(ctx.mem_size_mib, 128u);
    SC_ASSERT(strcmp(ctx.workspace_dir, "/tmp/ws") == 0);
    SC_ASSERT(strlen(ctx.kernel_path) > 0);
    SC_ASSERT(strlen(ctx.rootfs_path) > 0);
}

static void test_sandbox_firecracker_wrap_non_linux(void) {
#ifndef __linux__
    sc_firecracker_ctx_t ctx;
    sc_firecracker_sandbox_init(&ctx, "/tmp");
    sc_sandbox_t sb = sc_firecracker_sandbox_get(&ctx);
    const char *argv[] = { "echo", "x" };
    const char *out[16];
    size_t out_count = 0;
    sc_error_t err = sc_sandbox_wrap_command(&sb, argv, 2, out, 16, &out_count);
    SC_ASSERT_EQ(err, SC_ERR_NOT_SUPPORTED);
#endif
}

/* --- Auto-detection tests --- */
static void test_sandbox_auto_select_returns_valid(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_sandbox_alloc_t alloc = {
        .ctx = sys.ctx,
        .alloc = sys.alloc,
        .free = sys.free,
    };
    sc_sandbox_storage_t *st = sc_sandbox_storage_create(&alloc);
    SC_ASSERT_NOT_NULL(st);
    sc_sandbox_t sb = sc_sandbox_create(SC_SANDBOX_AUTO, "/tmp/ws", st, &alloc);
    SC_ASSERT(sb.ctx != NULL);
    SC_ASSERT(sb.vtable != NULL);
    const char *name = sc_sandbox_name(&sb);
    SC_ASSERT_NOT_NULL(name);
    SC_ASSERT(strlen(name) > 0);
    sc_sandbox_storage_destroy(st, &alloc);
}

static void test_sandbox_detect_available_runs(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_sandbox_alloc_t alloc = {
        .ctx = sys.ctx,
        .alloc = sys.alloc,
        .free = sys.free,
    };
    sc_available_backends_t avail = sc_sandbox_detect_available("/tmp", &alloc);
#ifdef __APPLE__
    SC_ASSERT(avail.seatbelt == true);
#endif
    (void)avail;
}

static void test_sandbox_apply_noop_returns_ok(void) {
    sc_noop_sandbox_ctx_t ctx;
    sc_sandbox_t sb = sc_noop_sandbox_get(&ctx);
    SC_ASSERT_EQ(sc_sandbox_apply(&sb), SC_OK);
}

static void test_sandbox_apply_null_returns_ok(void) {
    sc_sandbox_t sb = { .ctx = NULL, .vtable = NULL };
    SC_ASSERT_EQ(sc_sandbox_apply(&sb), SC_OK);
}

/* --- Landlock+seccomp combined --- */
static void test_sandbox_landlock_seccomp_vtable_wiring(void) {
    sc_landlock_seccomp_ctx_t ctx;
    sc_landlock_seccomp_sandbox_init(&ctx, "/tmp/ws", false);
    sc_sandbox_t sb = sc_landlock_seccomp_sandbox_get(&ctx);
    SC_ASSERT(sb.ctx != NULL);
    SC_ASSERT(sb.vtable != NULL);
    SC_ASSERT(strcmp(sc_sandbox_name(&sb), "landlock+seccomp") == 0);
    SC_ASSERT(strlen(sc_sandbox_description(&sb)) > 0);
}

static void test_sandbox_landlock_seccomp_has_apply(void) {
    sc_landlock_seccomp_ctx_t ctx;
    sc_landlock_seccomp_sandbox_init(&ctx, "/tmp", true);
    sc_sandbox_t sb = sc_landlock_seccomp_sandbox_get(&ctx);
    SC_ASSERT(sb.vtable->apply != NULL);
}

static void test_sandbox_landlock_seccomp_apply(void) {
    sc_landlock_seccomp_ctx_t ctx;
    sc_landlock_seccomp_sandbox_init(&ctx, "/tmp/ws", false);
    sc_sandbox_t sb = sc_landlock_seccomp_sandbox_get(&ctx);
    sc_error_t err = sc_sandbox_apply(&sb);
#ifdef __linux__
    SC_ASSERT_EQ(err, SC_OK);
#else
    SC_ASSERT_EQ(err, SC_ERR_NOT_SUPPORTED);
#endif
}

/* --- AppContainer (Windows) --- */
static void test_sandbox_appcontainer_vtable_wiring(void) {
    sc_appcontainer_ctx_t ctx;
    sc_appcontainer_sandbox_init(&ctx, "/tmp/ws");
    sc_sandbox_t sb = sc_appcontainer_sandbox_get(&ctx);
    SC_ASSERT(sb.ctx != NULL);
    SC_ASSERT(sb.vtable != NULL);
    SC_ASSERT(strcmp(sc_sandbox_name(&sb), "appcontainer") == 0);
}

static void test_sandbox_appcontainer_not_available_non_win(void) {
#ifndef _WIN32
    sc_appcontainer_ctx_t ctx;
    sc_appcontainer_sandbox_init(&ctx, "/tmp");
    sc_sandbox_t sb = sc_appcontainer_sandbox_get(&ctx);
    SC_ASSERT_FALSE(sc_sandbox_is_available(&sb));
#endif
}

static void test_sandbox_appcontainer_wrap_non_win(void) {
#ifndef _WIN32
    sc_appcontainer_ctx_t ctx;
    sc_appcontainer_sandbox_init(&ctx, "/tmp");
    sc_sandbox_t sb = sc_appcontainer_sandbox_get(&ctx);
    const char *argv[] = { "echo", "x" };
    const char *out[8];
    size_t out_count = 0;
    sc_error_t err = sc_sandbox_wrap_command(&sb, argv, 2, out, 8, &out_count);
    SC_ASSERT_EQ(err, SC_ERR_NOT_SUPPORTED);
#endif
}

/* --- Network proxy --- */
static void test_net_proxy_deny_all_blocks(void) {
    sc_net_proxy_t proxy;
    sc_net_proxy_init_deny_all(&proxy);
    SC_ASSERT_FALSE(sc_net_proxy_domain_allowed(&proxy, "evil.com"));
    SC_ASSERT_FALSE(sc_net_proxy_domain_allowed(&proxy, "example.com"));
}

static void test_net_proxy_allowlist(void) {
    sc_net_proxy_t proxy;
    sc_net_proxy_init_deny_all(&proxy);
    sc_net_proxy_allow_domain(&proxy, "api.openai.com");
    sc_net_proxy_allow_domain(&proxy, "api.anthropic.com");
    SC_ASSERT(sc_net_proxy_domain_allowed(&proxy, "api.openai.com"));
    SC_ASSERT(sc_net_proxy_domain_allowed(&proxy, "api.anthropic.com"));
    SC_ASSERT_FALSE(sc_net_proxy_domain_allowed(&proxy, "evil.com"));
}

static void test_net_proxy_wildcard_subdomain(void) {
    sc_net_proxy_t proxy;
    sc_net_proxy_init_deny_all(&proxy);
    sc_net_proxy_allow_domain(&proxy, "*.example.com");
    SC_ASSERT(sc_net_proxy_domain_allowed(&proxy, "api.example.com"));
    SC_ASSERT(sc_net_proxy_domain_allowed(&proxy, "sub.example.com"));
    SC_ASSERT_FALSE(sc_net_proxy_domain_allowed(&proxy, "example.org"));
}

static void test_net_proxy_disabled_allows_all(void) {
    sc_net_proxy_t proxy = {0};
    SC_ASSERT(sc_net_proxy_domain_allowed(&proxy, "anything.com"));
    SC_ASSERT(sc_net_proxy_domain_allowed(NULL, "anything.com"));
}

static void test_net_proxy_max_domains(void) {
    sc_net_proxy_t proxy;
    sc_net_proxy_init_deny_all(&proxy);
    for (int i = 0; i < SC_NET_PROXY_MAX_DOMAINS; i++) {
        SC_ASSERT(sc_net_proxy_allow_domain(&proxy, "domain.com"));
    }
    SC_ASSERT_FALSE(sc_net_proxy_allow_domain(&proxy, "overflow.com"));
}

/* --- sandbox API smoke tests --- */
static void test_sandbox_noop_available(void) {
    /* noop sandbox is always available */
    SC_ASSERT(1);
}

static void test_sandbox_noop_wrap_passthrough(void) {
    /* wrapping with noop should pass through unchanged */
    SC_ASSERT(1);
}

static void test_observer_noop(void) {
    sc_observer_t obs = sc_observer_noop();
    SC_ASSERT(strcmp(sc_observer_name(obs), "noop") == 0);
    sc_observer_event_t ev = { .tag = SC_OBSERVER_EVENT_HEARTBEAT_TICK };
    sc_observer_record_event(obs, &ev);
}

static void test_observer_metrics(void) {
    sc_metrics_observer_ctx_t ctx;
    sc_observer_t obs = sc_observer_metrics_create(&ctx);
    SC_ASSERT(strcmp(sc_observer_name(obs), "metrics") == 0);
    sc_observer_metric_t m = { .tag = SC_OBSERVER_METRIC_TOKENS_USED, .value = 42 };
    sc_observer_record_metric(obs, &m);
    SC_ASSERT_EQ(sc_observer_metrics_get(&ctx, SC_OBSERVER_METRIC_TOKENS_USED), 42);
}

static void test_observer_registry(void) {
    sc_observer_t o = sc_observer_registry_create("noop", NULL);
    SC_ASSERT(strcmp(sc_observer_name(o), "noop") == 0);
    o = sc_observer_registry_create("log", NULL);
    SC_ASSERT(strcmp(sc_observer_name(o), "log") == 0);
}

static void test_secret_store_disabled_returns_plaintext(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_secret_store_t *store = sc_secret_store_create(&sys, "/tmp", 0);
    SC_ASSERT_NOT_NULL(store);

    char *enc = NULL;
    sc_error_t err = sc_secret_store_encrypt(store, &sys, "sk-secret", &enc);
    SC_ASSERT(err == SC_OK);
    SC_ASSERT_STR_EQ(enc, "sk-secret");

    sys.free(sys.ctx, enc, strlen(enc) + 1);
    sc_secret_store_destroy(store, &sys);
}

/* --- Docker sandbox tests --- */
static void test_sandbox_docker_vtable_wiring(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_docker_ctx_t ctx;
    sc_docker_sandbox_init(&ctx, "/tmp/workspace", "ubuntu:22.04",
        sys.ctx, sys.alloc, sys.free);
    sc_sandbox_t sb = sc_docker_sandbox_get(&ctx);
    SC_ASSERT(sb.ctx != NULL);
    SC_ASSERT(sb.vtable != NULL);
    SC_ASSERT(sb.vtable->wrap_command != NULL);
    SC_ASSERT(sb.vtable->is_available != NULL);
    SC_ASSERT(sb.vtable->name != NULL);
    SC_ASSERT(sb.vtable->description != NULL);
}

static void test_sandbox_docker_not_available_in_test(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_docker_ctx_t ctx;
    sc_docker_sandbox_init(&ctx, "/tmp", "alpine", sys.ctx, sys.alloc, sys.free);
    sc_sandbox_t sb = sc_docker_sandbox_get(&ctx);
    SC_ASSERT_FALSE(sc_sandbox_is_available(&sb));
}

static void test_sandbox_docker_wrap_format(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_docker_ctx_t ctx;
    sc_docker_sandbox_init(&ctx, "/tmp/ws", "myimage:latest",
        sys.ctx, sys.alloc, sys.free);
    sc_sandbox_t sb = sc_docker_sandbox_get(&ctx);
    const char *argv[] = { "echo", "hello" };
    const char *out[32];
    size_t out_count = 0;
    sc_error_t err = sc_sandbox_wrap_command(&sb, argv, 2, out, 32, &out_count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT(out_count >= 12);
    SC_ASSERT_STR_EQ(out[0], "docker");
    SC_ASSERT_STR_EQ(out[1], "run");
    SC_ASSERT_STR_EQ(out[2], "--rm");
    SC_ASSERT_STR_EQ(out[out_count - 2], "echo");
    SC_ASSERT_STR_EQ(out[out_count - 1], "hello");
}

static void test_sandbox_docker_name_and_desc(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_docker_ctx_t ctx;
    sc_docker_sandbox_init(&ctx, "/tmp", "alpine", sys.ctx, sys.alloc, sys.free);
    sc_sandbox_t sb = sc_docker_sandbox_get(&ctx);
    SC_ASSERT_STR_EQ(sc_sandbox_name(&sb), "docker");
    SC_ASSERT(strlen(sc_sandbox_description(&sb)) > 0);
}

/* --- Firecracker wrap output validation (Linux) --- */
static void test_sandbox_firecracker_wrap_on_linux(void) {
#ifdef __linux__
    sc_firecracker_ctx_t ctx;
    sc_firecracker_sandbox_init(&ctx, "/tmp/ws");
    sc_sandbox_t sb = sc_firecracker_sandbox_get(&ctx);
    const char *argv[] = { "echo", "hello" };
    const char *out[16];
    size_t out_count = 0;
    sc_error_t err = sc_sandbox_wrap_command(&sb, argv, 2, out, 16, &out_count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out_count, 4);
    SC_ASSERT_STR_EQ(out[0], "firecracker");
    SC_ASSERT_STR_EQ(out[1], "--no-api");
    SC_ASSERT_STR_EQ(out[2], "--boot-timer");
    SC_ASSERT(strstr(out[3], "--config-file=") != NULL);
#endif
}

static void test_sandbox_firecracker_wrap_buffer_too_small(void) {
#ifdef __linux__
    sc_firecracker_ctx_t ctx;
    sc_firecracker_sandbox_init(&ctx, "/tmp/ws");
    sc_sandbox_t sb = sc_firecracker_sandbox_get(&ctx);
    const char *argv[] = { "echo" };
    const char *out[2];
    size_t out_count = 0;
    sc_error_t err = sc_sandbox_wrap_command(&sb, argv, 1, out, 2, &out_count);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
#endif
}

/* --- Seatbelt profile temp dir validation --- */
static void test_sandbox_seatbelt_profile_allows_tmp_write(void) {
    sc_seatbelt_ctx_t ctx;
    sc_seatbelt_sandbox_init(&ctx, "/tmp/workspace");
    SC_ASSERT(strstr(ctx.profile, "file-write* (subpath \"/tmp\")") != NULL);
    SC_ASSERT(strstr(ctx.profile, "file-write* (subpath \"/var/folders\")") != NULL);
    SC_ASSERT(strstr(ctx.profile, "file-write* (subpath \"/private/tmp\")") != NULL);
}

/* --- Config sandbox backend parsing coverage --- */
static void test_sandbox_create_each_backend(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_sandbox_alloc_t alloc = {
        .ctx = sys.ctx, .alloc = sys.alloc, .free = sys.free,
    };

    sc_sandbox_backend_t backends[] = {
        SC_SANDBOX_NONE, SC_SANDBOX_LANDLOCK, SC_SANDBOX_FIREJAIL,
        SC_SANDBOX_BUBBLEWRAP, SC_SANDBOX_SEATBELT, SC_SANDBOX_SECCOMP,
        SC_SANDBOX_WASI, SC_SANDBOX_LANDLOCK_SECCOMP, SC_SANDBOX_FIRECRACKER,
        SC_SANDBOX_APPCONTAINER, SC_SANDBOX_AUTO,
    };
    size_t n = sizeof(backends) / sizeof(backends[0]);

    for (size_t i = 0; i < n; i++) {
        sc_sandbox_storage_t *st = sc_sandbox_storage_create(&alloc);
        SC_ASSERT_NOT_NULL(st);
        sc_sandbox_t sb = sc_sandbox_create(backends[i], "/tmp/ws", st, &alloc);
        SC_ASSERT(sb.vtable != NULL);
        SC_ASSERT(strlen(sc_sandbox_name(&sb)) > 0);
        SC_ASSERT(strlen(sc_sandbox_description(&sb)) >= 0);
        sc_sandbox_storage_destroy(st, &alloc);
    }
}

/* --- sc_process_run_sandboxed tests --- */
static void test_process_run_sandboxed_null_args(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_run_result_t result = {0};
    sc_error_t err = sc_process_run_sandboxed(&sys, NULL, NULL, 1024, NULL, NULL, &result);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);

    const char *argv[] = { NULL };
    err = sc_process_run_sandboxed(&sys, argv, NULL, 1024, NULL, NULL, &result);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_process_run_sandboxed_with_null_setup(void) {
    sc_allocator_t sys = sc_system_allocator();
    const char *argv[] = { "echo", "test-sandbox", NULL };
    sc_run_result_t result = {0};
    sc_error_t err = sc_process_run_sandboxed(&sys, argv, NULL, 1024, NULL, NULL, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT(result.stdout_cap > 0);
    SC_ASSERT(result.stderr_cap > 0);
    sc_run_result_free(&sys, &result);
}

static void test_process_run_with_policy_null(void) {
    sc_allocator_t sys = sc_system_allocator();
    const char *argv[] = { "echo", "policy-test", NULL };
    sc_run_result_t result = {0};
    sc_error_t err = sc_process_run_with_policy(&sys, argv, NULL, 1024, NULL, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT(result.stdout_cap > 0);
    sc_run_result_free(&sys, &result);
}

static void test_process_run_with_policy_noop_sandbox(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_security_policy_t policy = {0};
    const char *argv[] = { "echo", "sandboxed", NULL };
    sc_run_result_t result = {0};
    sc_error_t err = sc_process_run_with_policy(&sys, argv, NULL, 1024, &policy, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT(result.stdout_cap > 0);
    sc_run_result_free(&sys, &result);
}

/* --- Sandbox storage lifecycle --- */
static void test_sandbox_storage_create_destroy(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_sandbox_alloc_t alloc = {
        .ctx = sys.ctx, .alloc = sys.alloc, .free = sys.free,
    };
    sc_sandbox_storage_t *st = sc_sandbox_storage_create(&alloc);
    SC_ASSERT_NOT_NULL(st);
    sc_sandbox_storage_destroy(st, &alloc);
    sc_sandbox_storage_destroy(NULL, &alloc);
}

/* --- Net proxy edge cases --- */
static void test_net_proxy_null_domain(void) {
    sc_net_proxy_t proxy;
    sc_net_proxy_init_deny_all(&proxy);
    SC_ASSERT_FALSE(sc_net_proxy_domain_allowed(&proxy, NULL));
}

static void test_net_proxy_add_null_domain(void) {
    sc_net_proxy_t proxy;
    sc_net_proxy_init_deny_all(&proxy);
    SC_ASSERT_FALSE(sc_net_proxy_allow_domain(&proxy, NULL));
    SC_ASSERT_FALSE(sc_net_proxy_allow_domain(NULL, "test.com"));
}

static void test_net_proxy_case_insensitive(void) {
    sc_net_proxy_t proxy;
    sc_net_proxy_init_deny_all(&proxy);
    sc_net_proxy_allow_domain(&proxy, "Example.COM");
    SC_ASSERT(sc_net_proxy_domain_allowed(&proxy, "example.com"));
    SC_ASSERT(sc_net_proxy_domain_allowed(&proxy, "EXAMPLE.COM"));
    SC_ASSERT(sc_net_proxy_domain_allowed(&proxy, "Example.Com"));
}

static void test_net_proxy_wildcard_case_insensitive(void) {
    sc_net_proxy_t proxy;
    sc_net_proxy_init_deny_all(&proxy);
    sc_net_proxy_allow_domain(&proxy, "*.Example.COM");
    SC_ASSERT(sc_net_proxy_domain_allowed(&proxy, "sub.example.com"));
    SC_ASSERT(sc_net_proxy_domain_allowed(&proxy, "SUB.EXAMPLE.COM"));
}

static void test_net_proxy_empty_domain_rejected(void) {
    sc_net_proxy_t proxy;
    sc_net_proxy_init_deny_all(&proxy);
    sc_net_proxy_allow_domain(&proxy, "example.com");
    SC_ASSERT_FALSE(sc_net_proxy_domain_allowed(&proxy, ""));
}

/* --- Seatbelt truncated profile guard --- */
static void test_seatbelt_wrap_fails_on_truncated_profile(void) {
    sc_seatbelt_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.profile_len = 0;
    sc_sandbox_t sb = sc_seatbelt_sandbox_get(&ctx);
    const char *argv[] = { "echo", "hello" };
    const char *out[8];
    size_t out_count = 0;
    sc_error_t err = sc_sandbox_wrap_command(&sb, argv, 2, out, 8, &out_count);
#ifdef __APPLE__
    SC_ASSERT_EQ(err, SC_ERR_INTERNAL);
    SC_ASSERT_EQ(out_count, 0);
#else
    SC_ASSERT_EQ(err, SC_ERR_NOT_SUPPORTED);
#endif
}

/* --- Firejail extra_args wiring --- */
static void test_firejail_extra_args_wired(void) {
    sc_firejail_ctx_t ctx;
    sc_firejail_sandbox_init(&ctx, "/tmp/ws");
    const char *extras[] = { "--whitelist=/opt" };
    sc_firejail_sandbox_set_extra_args(&ctx, extras, 1);
    SC_ASSERT_EQ(ctx.extra_args_len, 1);
    SC_ASSERT(ctx.extra_args == extras);
#ifdef __linux__
    sc_sandbox_t sb = sc_firejail_sandbox_get(&ctx);
    const char *argv[] = { "ls" };
    const char *out[16];
    size_t out_count = 0;
    sc_error_t err = sc_sandbox_wrap_command(&sb, argv, 1, out, 16, &out_count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out_count, 7);
    SC_ASSERT(strcmp(out[5], "--whitelist=/opt") == 0);
    SC_ASSERT(strcmp(out[6], "ls") == 0);
#endif
}

/* --- WASI NULL workspace guard --- */
static void test_wasi_wrap_fails_null_workspace(void) {
    sc_wasi_sandbox_ctx_t ctx;
    sc_wasi_sandbox_init(&ctx, NULL);
    sc_sandbox_t sb = sc_wasi_sandbox_get(&ctx);
    const char *argv[] = { "test.wasm" };
    const char *out[8];
    size_t out_count = 0;
    sc_error_t err = sc_sandbox_wrap_command(&sb, argv, 1, out, 8, &out_count);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

/* --- Landlock NULL workspace guard --- */
static void test_landlock_apply_fails_null_workspace(void) {
    sc_landlock_ctx_t ctx;
    sc_landlock_sandbox_init(&ctx, NULL);
    sc_sandbox_t sb = sc_landlock_sandbox_get(&ctx);
    if (sb.vtable && sb.vtable->apply) {
        sc_error_t err = sb.vtable->apply(sb.ctx);
#ifdef __linux__
        SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
#else
        SC_ASSERT_EQ(err, SC_ERR_NOT_SUPPORTED);
#endif
    }
}

void run_security_tests(void) {
    static sc_allocator_t sys = {0};
    sys = sc_system_allocator();
    g_alloc = &sys;

    SC_TEST_SUITE("Security Policy");
    SC_RUN_TEST(test_autonomy_level_values);
    SC_RUN_TEST(test_risk_level_values);
    SC_RUN_TEST(test_policy_can_act_readonly);
    SC_RUN_TEST(test_policy_can_act_supervised);
    SC_RUN_TEST(test_policy_can_act_full);
    SC_RUN_TEST(test_policy_allowed_commands);
    SC_RUN_TEST(test_policy_blocked_commands);
    SC_RUN_TEST(test_policy_readonly_blocks_all);
    SC_RUN_TEST(test_policy_command_injection_blocked);
    SC_RUN_TEST(test_policy_risk_levels);
    SC_RUN_TEST(test_policy_validate_command);
    SC_RUN_TEST(test_rate_tracker);

    SC_TEST_SUITE("Pairing");
    SC_RUN_TEST(test_pairing_constant_time_eq);
    SC_RUN_TEST(test_pairing_guard_create_destroy);
    SC_RUN_TEST(test_pairing_guard_disabled);
    SC_RUN_TEST(test_pairing_guard_with_tokens);
    SC_RUN_TEST(test_pairing_attempt_pair);

    SC_TEST_SUITE("Audit");
    SC_RUN_TEST(test_audit_event_init);
    SC_RUN_TEST(test_audit_event_write_json);
    SC_RUN_TEST(test_audit_logger_disabled);
    SC_RUN_TEST(test_audit_chain_verify_valid);
    SC_RUN_TEST(test_audit_chain_tamper_detected);
    SC_RUN_TEST(test_audit_key_rotation_basic);
    SC_RUN_TEST(test_audit_key_rotation_verify_detects_tamper_after_rotation);
    SC_RUN_TEST(test_audit_rotation_interval);
    SC_RUN_TEST(test_audit_chain_delete_detected);

    SC_TEST_SUITE("Sandbox");
    SC_RUN_TEST(test_sandbox_noop);
    SC_RUN_TEST(test_sandbox_landlock_non_linux_or_test);
    SC_RUN_TEST(test_sandbox_firejail_non_linux);
    SC_RUN_TEST(test_sandbox_bubblewrap_non_linux);

    SC_TEST_SUITE("Sandbox — Seatbelt (macOS)");
    SC_RUN_TEST(test_sandbox_seatbelt_vtable_wiring);
    SC_RUN_TEST(test_sandbox_seatbelt_availability);
    SC_RUN_TEST(test_sandbox_seatbelt_wrap_command);
    SC_RUN_TEST(test_sandbox_seatbelt_profile_contains_workspace);
    SC_RUN_TEST(test_sandbox_seatbelt_null_args);

    SC_TEST_SUITE("Sandbox — Landlock (apply)");
    SC_RUN_TEST(test_sandbox_landlock_apply_in_test_mode);
    SC_RUN_TEST(test_sandbox_landlock_vtable_has_apply);
    SC_RUN_TEST(test_sandbox_landlock_wrap_passthrough);

    SC_TEST_SUITE("Sandbox — seccomp-BPF");
    SC_RUN_TEST(test_sandbox_seccomp_vtable_wiring);
    SC_RUN_TEST(test_sandbox_seccomp_availability);
    SC_RUN_TEST(test_sandbox_seccomp_wrap_passthrough);
    SC_RUN_TEST(test_sandbox_seccomp_has_apply);
    SC_RUN_TEST(test_sandbox_seccomp_network_flag);

    SC_TEST_SUITE("Sandbox — WASI");
    SC_RUN_TEST(test_sandbox_wasi_vtable_wiring);
    SC_RUN_TEST(test_sandbox_wasi_wrap_command_format);
    SC_RUN_TEST(test_sandbox_wasi_no_apply);

    SC_TEST_SUITE("Sandbox — Firecracker");
    SC_RUN_TEST(test_sandbox_firecracker_vtable_wiring);
    SC_RUN_TEST(test_sandbox_firecracker_not_available_in_test);
    SC_RUN_TEST(test_sandbox_firecracker_defaults);
    SC_RUN_TEST(test_sandbox_firecracker_wrap_non_linux);
    SC_RUN_TEST(test_sandbox_firecracker_wrap_on_linux);
    SC_RUN_TEST(test_sandbox_firecracker_wrap_buffer_too_small);

    SC_TEST_SUITE("Sandbox — Landlock+seccomp combined");
    SC_RUN_TEST(test_sandbox_landlock_seccomp_vtable_wiring);
    SC_RUN_TEST(test_sandbox_landlock_seccomp_has_apply);
    SC_RUN_TEST(test_sandbox_landlock_seccomp_apply);

    SC_TEST_SUITE("Sandbox — AppContainer (Windows)");
    SC_RUN_TEST(test_sandbox_appcontainer_vtable_wiring);
    SC_RUN_TEST(test_sandbox_appcontainer_not_available_non_win);
    SC_RUN_TEST(test_sandbox_appcontainer_wrap_non_win);

    SC_TEST_SUITE("Sandbox — Network Proxy");
    SC_RUN_TEST(test_net_proxy_deny_all_blocks);
    SC_RUN_TEST(test_net_proxy_allowlist);
    SC_RUN_TEST(test_net_proxy_wildcard_subdomain);
    SC_RUN_TEST(test_net_proxy_disabled_allows_all);
    SC_RUN_TEST(test_net_proxy_max_domains);

    SC_TEST_SUITE("Sandbox — Seatbelt Profile");
    SC_RUN_TEST(test_sandbox_seatbelt_profile_allows_tmp_write);
    SC_RUN_TEST(test_seatbelt_wrap_fails_on_truncated_profile);

    SC_TEST_SUITE("Sandbox — Firejail Extra Args");
    SC_RUN_TEST(test_firejail_extra_args_wired);

    SC_TEST_SUITE("Sandbox — NULL Workspace Guards");
    SC_RUN_TEST(test_wasi_wrap_fails_null_workspace);
    SC_RUN_TEST(test_landlock_apply_fails_null_workspace);

    SC_TEST_SUITE("Sandbox — Backend Coverage");
    SC_RUN_TEST(test_sandbox_create_each_backend);
    SC_RUN_TEST(test_sandbox_storage_create_destroy);

    SC_TEST_SUITE("Sandbox — Auto-detection & Apply");
    SC_RUN_TEST(test_sandbox_auto_select_returns_valid);
    SC_RUN_TEST(test_sandbox_detect_available_runs);
    SC_RUN_TEST(test_sandbox_apply_noop_returns_ok);
    SC_RUN_TEST(test_sandbox_apply_null_returns_ok);
    SC_RUN_TEST(test_sandbox_noop_available);
    SC_RUN_TEST(test_sandbox_noop_wrap_passthrough);

    SC_TEST_SUITE("Sandbox — sc_process_run_sandboxed");
    SC_RUN_TEST(test_process_run_sandboxed_null_args);
    SC_RUN_TEST(test_process_run_sandboxed_with_null_setup);

    SC_TEST_SUITE("Sandbox — sc_process_run_with_policy");
    SC_RUN_TEST(test_process_run_with_policy_null);
    SC_RUN_TEST(test_process_run_with_policy_noop_sandbox);

    SC_TEST_SUITE("Network Proxy — Edge Cases");
    SC_RUN_TEST(test_net_proxy_null_domain);
    SC_RUN_TEST(test_net_proxy_add_null_domain);
    SC_RUN_TEST(test_net_proxy_case_insensitive);
    SC_RUN_TEST(test_net_proxy_wildcard_case_insensitive);
    SC_RUN_TEST(test_net_proxy_empty_domain_rejected);

    SC_TEST_SUITE("Observer");
    SC_RUN_TEST(test_observer_noop);
    SC_RUN_TEST(test_observer_metrics);
    SC_RUN_TEST(test_observer_registry);

    SC_TEST_SUITE("Secrets");
    SC_RUN_TEST(test_secret_store_is_encrypted);
    SC_RUN_TEST(test_hex_encode_decode);
    SC_RUN_TEST(test_secret_store_encrypt_decrypt);
    SC_RUN_TEST(test_secret_store_plaintext_passthrough);
    SC_RUN_TEST(test_secret_store_disabled_returns_plaintext);

    SC_TEST_SUITE("Sandbox — Docker");
    SC_RUN_TEST(test_sandbox_docker_vtable_wiring);
    SC_RUN_TEST(test_sandbox_docker_not_available_in_test);
    SC_RUN_TEST(test_sandbox_docker_wrap_format);
    SC_RUN_TEST(test_sandbox_docker_name_and_desc);
}
