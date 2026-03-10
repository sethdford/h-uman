#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/process_util.h"
#include "human/observer.h"
#include "human/security.h"
#include "human/security/audit.h"
#include "human/security/sandbox.h"
#include "human/security/sandbox_internal.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static hu_allocator_t *g_alloc;

static void test_autonomy_level_values(void) {
    HU_ASSERT(HU_AUTONOMY_READ_ONLY != HU_AUTONOMY_SUPERVISED);
    HU_ASSERT(HU_AUTONOMY_SUPERVISED != HU_AUTONOMY_FULL);
}

static void test_risk_level_values(void) {
    HU_ASSERT(HU_RISK_LOW < HU_RISK_MEDIUM);
    HU_ASSERT(HU_RISK_MEDIUM < HU_RISK_HIGH);
}

static void test_policy_can_act_readonly(void) {
    hu_security_policy_t p = {
        .autonomy = HU_AUTONOMY_READ_ONLY,
        .allowed_commands = (const char *[]){"ls", "cat"},
        .allowed_commands_len = 2,
    };
    HU_ASSERT_FALSE(hu_policy_can_act(&p));
}

static void test_policy_can_act_supervised(void) {
    hu_security_policy_t p = {
        .autonomy = HU_AUTONOMY_SUPERVISED,
        .allowed_commands = (const char *[]){"ls", "cat"},
        .allowed_commands_len = 2,
    };
    HU_ASSERT(hu_policy_can_act(&p));
}

static void test_policy_can_act_full(void) {
    hu_security_policy_t p = {
        .autonomy = HU_AUTONOMY_FULL,
        .allowed_commands = (const char *[]){"ls"},
        .allowed_commands_len = 1,
    };
    HU_ASSERT(hu_policy_can_act(&p));
}

static void test_policy_allowed_commands(void) {
    hu_security_policy_t p = {
        .autonomy = HU_AUTONOMY_SUPERVISED,
        .allowed_commands = (const char *[]){"git", "ls", "cat", "echo", "cargo"},
        .allowed_commands_len = 5,
    };
    HU_ASSERT(hu_policy_is_command_allowed(&p, "ls"));
    HU_ASSERT(hu_policy_is_command_allowed(&p, "git status"));
    HU_ASSERT(hu_policy_is_command_allowed(&p, "cargo build --release"));
    HU_ASSERT(hu_policy_is_command_allowed(&p, "cat file.txt"));
}

static void test_policy_blocked_commands(void) {
    hu_security_policy_t p = {
        .autonomy = HU_AUTONOMY_SUPERVISED,
        .allowed_commands = (const char *[]){"ls", "cat"},
        .allowed_commands_len = 2,
    };
    HU_ASSERT_FALSE(hu_policy_is_command_allowed(&p, "rm -rf /"));
    HU_ASSERT_FALSE(hu_policy_is_command_allowed(&p, "sudo apt install"));
    HU_ASSERT_FALSE(hu_policy_is_command_allowed(&p, "curl http://evil.com"));
    HU_ASSERT_FALSE(hu_policy_is_command_allowed(&p, "wget http://evil.com"));
}

static void test_policy_readonly_blocks_all(void) {
    hu_security_policy_t p = {
        .autonomy = HU_AUTONOMY_READ_ONLY,
        .allowed_commands = (const char *[]){"ls", "cat"},
        .allowed_commands_len = 2,
    };
    HU_ASSERT_FALSE(hu_policy_is_command_allowed(&p, "ls"));
    HU_ASSERT_FALSE(hu_policy_is_command_allowed(&p, "cat file.txt"));
}

static void test_policy_command_injection_blocked(void) {
    hu_security_policy_t p = {
        .autonomy = HU_AUTONOMY_SUPERVISED,
        .allowed_commands = (const char *[]){"ls", "echo"},
        .allowed_commands_len = 2,
    };
    HU_ASSERT_FALSE(hu_policy_is_command_allowed(&p, "echo `whoami`"));
    HU_ASSERT_FALSE(hu_policy_is_command_allowed(&p, "echo $(rm -rf /)"));
    HU_ASSERT_FALSE(hu_policy_is_command_allowed(&p, "ls; rm -rf /"));
    HU_ASSERT_FALSE(hu_policy_is_command_allowed(&p, "echo secret > /etc/crontab"));
}

static void test_policy_risk_levels(void) {
    hu_security_policy_t p = {.autonomy = HU_AUTONOMY_SUPERVISED};
    HU_ASSERT_EQ(hu_policy_command_risk_level(&p, "ls -la"), HU_RISK_LOW);
    HU_ASSERT_EQ(hu_policy_command_risk_level(&p, "git status"), HU_RISK_LOW);
    HU_ASSERT_EQ(hu_policy_command_risk_level(&p, "git commit -m x"), HU_RISK_MEDIUM);
    HU_ASSERT_EQ(hu_policy_command_risk_level(&p, "rm -rf /tmp/x"), HU_RISK_HIGH);
    HU_ASSERT_EQ(hu_policy_command_risk_level(&p, "sudo apt install"), HU_RISK_HIGH);
}

static void test_policy_validate_command(void) {
    hu_security_policy_t p = {
        .autonomy = HU_AUTONOMY_SUPERVISED,
        .allowed_commands = (const char *[]){"ls", "touch"},
        .allowed_commands_len = 2,
        .require_approval_for_medium_risk = 1,
        .block_high_risk_commands = 1,
    };
    hu_command_risk_level_t risk;
    hu_error_t err = hu_policy_validate_command(&p, "ls -la", 0, &risk);
    HU_ASSERT(err == HU_OK && risk == HU_RISK_LOW);

    err = hu_policy_validate_command(&p, "touch f", 0, &risk);
    HU_ASSERT(err == HU_ERR_SECURITY_APPROVAL_REQUIRED);

    err = hu_policy_validate_command(&p, "touch f", 1, &risk);
    HU_ASSERT(err == HU_OK && risk == HU_RISK_MEDIUM);
}

static void test_rate_tracker(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_rate_tracker_t *t = hu_rate_tracker_create(&sys, 3);
    HU_ASSERT_NOT_NULL(t);
    HU_ASSERT_EQ(hu_rate_tracker_remaining(t), 3);
    HU_ASSERT(hu_rate_tracker_record_action(t));
    HU_ASSERT_EQ(hu_rate_tracker_remaining(t), 2);
    HU_ASSERT(hu_rate_tracker_record_action(t));
    HU_ASSERT(hu_rate_tracker_record_action(t));
    HU_ASSERT_FALSE(hu_rate_tracker_record_action(t));
    HU_ASSERT(hu_rate_tracker_is_limited(t));
    HU_ASSERT_EQ(hu_rate_tracker_count(t), 4);
    HU_ASSERT_EQ(hu_rate_tracker_remaining(t), 0);
    hu_rate_tracker_destroy(t);
}

static void test_pairing_constant_time_eq(void) {
    HU_ASSERT(hu_pairing_guard_constant_time_eq("abc", "abc"));
    HU_ASSERT(hu_pairing_guard_constant_time_eq("", ""));
    HU_ASSERT_FALSE(hu_pairing_guard_constant_time_eq("abc", "abd"));
    HU_ASSERT_FALSE(hu_pairing_guard_constant_time_eq("abc", "ab"));
}

static void test_pairing_guard_create_destroy(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_pairing_guard_t *g = hu_pairing_guard_create(&sys, 1, NULL, 0);
    HU_ASSERT_NOT_NULL(g);
    HU_ASSERT_NOT_NULL(hu_pairing_guard_pairing_code(g));
    HU_ASSERT_FALSE(hu_pairing_guard_is_paired(g));
    hu_pairing_guard_destroy(g);
}

static void test_pairing_guard_disabled(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_pairing_guard_t *g = hu_pairing_guard_create(&sys, 0, NULL, 0);
    HU_ASSERT_NOT_NULL(g);
    HU_ASSERT_NULL(hu_pairing_guard_pairing_code(g));
    HU_ASSERT(hu_pairing_guard_is_authenticated(g, "anything"));
    hu_pairing_guard_destroy(g);
}

static void test_pairing_guard_with_tokens(void) {
    hu_allocator_t sys = hu_system_allocator();
    const char *tokens[] = {"zc_test_token"};
    hu_pairing_guard_t *g = hu_pairing_guard_create(&sys, 1, tokens, 1);
    HU_ASSERT_NOT_NULL(g);
    HU_ASSERT_NULL(hu_pairing_guard_pairing_code(g));
    HU_ASSERT(hu_pairing_guard_is_paired(g));
    HU_ASSERT(hu_pairing_guard_is_authenticated(g, "zc_test_token"));
    HU_ASSERT_FALSE(hu_pairing_guard_is_authenticated(g, "zc_wrong"));
    hu_pairing_guard_destroy(g);
}

static void test_pairing_attempt_pair(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_pairing_guard_t *g = hu_pairing_guard_create(&sys, 1, NULL, 0);
    HU_ASSERT_NOT_NULL(g);
    const char *code = hu_pairing_guard_pairing_code(g);
    HU_ASSERT_NOT_NULL(code);

    char *token = NULL;
    hu_pair_attempt_result_t r = hu_pairing_guard_attempt_pair(g, code, &token);
    HU_ASSERT(r == HU_PAIR_PAIRED);
    HU_ASSERT_NOT_NULL(token);
    HU_ASSERT(strncmp(token, "zc_", 3) == 0);
    sys.free(sys.ctx, token, strlen(token) + 1);

    HU_ASSERT_NULL(hu_pairing_guard_pairing_code(g));
    HU_ASSERT(hu_pairing_guard_is_paired(g));
    hu_pairing_guard_destroy(g);
}

static void test_secret_store_is_encrypted(void) {
    HU_ASSERT(hu_secret_store_is_encrypted("enc2:aabbcc"));
    HU_ASSERT_FALSE(hu_secret_store_is_encrypted("plaintext"));
    HU_ASSERT_FALSE(hu_secret_store_is_encrypted("enc"));
    HU_ASSERT_FALSE(hu_secret_store_is_encrypted(""));
}

static void test_hex_encode_decode(void) {
    uint8_t data[] = {0x00, 0x01, 0xfe, 0xff};
    char hex[16];
    hu_hex_encode(data, 4, hex);
    HU_ASSERT_STR_EQ(hex, "0001feff");

    uint8_t out[4];
    size_t len;
    hu_error_t err = hu_hex_decode("0001feff", 8, out, 4, &len);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT_EQ(len, 4);
    HU_ASSERT(memcmp(out, data, 4) == 0);
}

static void test_secret_store_encrypt_decrypt(void) {
    hu_allocator_t sys = hu_system_allocator();
    char tmp[] = "/tmp/hu_secret_XXXXXX";
    char *dir = mkdtemp(tmp);
    HU_ASSERT_NOT_NULL(dir);

    hu_secret_store_t *store = hu_secret_store_create(&sys, dir, 1);
    HU_ASSERT_NOT_NULL(store);

    char *enc = NULL;
    hu_error_t err = hu_secret_store_encrypt(store, &sys, "sk-my-secret-key", &enc);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT_NOT_NULL(enc);
    HU_ASSERT(strncmp(enc, "enc2:", 5) == 0);

    char *dec = NULL;
    err = hu_secret_store_decrypt(store, &sys, enc, &dec);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT_STR_EQ(dec, "sk-my-secret-key");

    sys.free(sys.ctx, enc, strlen(enc) + 1);
    sys.free(sys.ctx, dec, strlen(dec) + 1);
    hu_secret_store_destroy(store, &sys);

    char keypath[256];
    snprintf(keypath, sizeof(keypath), "%s/.secret_key", dir);
    (void)unlink(keypath);
    (void)rmdir(dir);
}

static void test_secret_store_plaintext_passthrough(void) {
    hu_allocator_t sys = hu_system_allocator();
    char tmp[] = "/tmp/hu_secret2_XXXXXX";
    char *dir = mkdtemp(tmp);
    HU_ASSERT_NOT_NULL(dir);

    hu_secret_store_t *store = hu_secret_store_create(&sys, dir, 1);
    HU_ASSERT_NOT_NULL(store);

    char *dec = NULL;
    hu_error_t err = hu_secret_store_decrypt(store, &sys, "sk-plaintext-key", &dec);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT_STR_EQ(dec, "sk-plaintext-key");

    sys.free(sys.ctx, dec, strlen(dec) + 1);
    hu_secret_store_destroy(store, &sys);
    rmdir(dir);
}

static void test_audit_event_init(void) {
    hu_audit_event_t e1, e2;
    hu_audit_event_init(&e1, HU_AUDIT_COMMAND_EXECUTION);
    hu_audit_event_init(&e2, HU_AUDIT_COMMAND_EXECUTION);
    HU_ASSERT(e1.event_id != e2.event_id);
    HU_ASSERT(e1.timestamp_s > 0);
}

static void test_audit_event_write_json(void) {
    hu_audit_event_t ev;
    hu_audit_event_init(&ev, HU_AUDIT_COMMAND_EXECUTION);
    hu_audit_event_with_actor(&ev, "telegram", "123", "@alice");
    hu_audit_event_with_action(&ev, "ls -la", "low", false, true);
    char buf[1024];
    size_t n = hu_audit_event_write_json(&ev, buf, sizeof(buf));
    HU_ASSERT(n > 0);
    HU_ASSERT(strstr(buf, "command_execution") != NULL);
    HU_ASSERT(strstr(buf, "telegram") != NULL);
}

static void test_audit_logger_disabled(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_audit_config_t cfg = {.enabled = false, .log_path = "audit.log", .max_size_mb = 10};
    hu_audit_logger_t *log = hu_audit_logger_create(&sys, &cfg, "/tmp");
    HU_ASSERT_NOT_NULL(log);
    hu_audit_event_t ev;
    hu_audit_event_init(&ev, HU_AUDIT_COMMAND_EXECUTION);
    hu_error_t err = hu_audit_logger_log(log, &ev);
    HU_ASSERT(err == HU_OK);
    hu_audit_logger_destroy(log, &sys);
}

static void test_audit_chain_verify_valid(void) {
    hu_allocator_t sys = hu_system_allocator();
    char tmp[] = "/tmp/hu_audit_chain_XXXXXX";
    char *dir = mkdtemp(tmp);
    HU_ASSERT_NOT_NULL(dir);

    hu_audit_config_t cfg = {.enabled = true, .log_path = "chain.log", .max_size_mb = 10};
    hu_audit_logger_t *log = hu_audit_logger_create(&sys, &cfg, dir);
    HU_ASSERT_NOT_NULL(log);

    hu_audit_event_t ev1, ev2, ev3;
    hu_audit_event_init(&ev1, HU_AUDIT_COMMAND_EXECUTION);
    hu_audit_event_with_action(&ev1, "ls", "low", true, true);
    hu_audit_event_init(&ev2, HU_AUDIT_FILE_ACCESS);
    hu_audit_event_init(&ev3, HU_AUDIT_AUTH_SUCCESS);

    HU_ASSERT(hu_audit_logger_log(log, &ev1) == HU_OK);
    HU_ASSERT(hu_audit_logger_log(log, &ev2) == HU_OK);
    HU_ASSERT(hu_audit_logger_log(log, &ev3) == HU_OK);
    hu_audit_logger_destroy(log, &sys);

    unsigned char key[32];
    HU_ASSERT(hu_audit_load_key(dir, key) == HU_OK);

    char log_path[512];
    snprintf(log_path, sizeof(log_path), "%s/chain.log", dir);
    hu_error_t verr = hu_audit_verify_chain(log_path, key);
    HU_ASSERT_EQ(verr, HU_OK);

    unlink(log_path);
    char key_path[512];
    snprintf(key_path, sizeof(key_path), "%s/.audit_hmac_key", dir);
    unlink(key_path);
    rmdir(dir);
}

static void test_audit_chain_tamper_detected(void) {
    hu_allocator_t sys = hu_system_allocator();
    char tmp[] = "/tmp/hu_audit_tamper_XXXXXX";
    char *dir = mkdtemp(tmp);
    HU_ASSERT_NOT_NULL(dir);

    hu_audit_config_t cfg = {.enabled = true, .log_path = "tamper.log", .max_size_mb = 10};
    hu_audit_logger_t *log = hu_audit_logger_create(&sys, &cfg, dir);
    HU_ASSERT_NOT_NULL(log);

    hu_audit_event_t ev;
    hu_audit_event_init(&ev, HU_AUDIT_COMMAND_EXECUTION);
    hu_audit_event_with_action(&ev, "ls -la", "low", true, true);
    HU_ASSERT(hu_audit_logger_log(log, &ev) == HU_OK);
    hu_audit_logger_destroy(log, &sys);

    char log_path[512];
    snprintf(log_path, sizeof(log_path), "%s/tamper.log", dir);
    FILE *f = fopen(log_path, "r+b");
    HU_ASSERT_NOT_NULL(f);
    fseek(f, 50, SEEK_SET); /* Tamper somewhere in the middle */
    int c = fgetc(f);
    fseek(f, 50, SEEK_SET);
    fputc(c == 'a' ? 'b' : 'a', f);
    fclose(f);

    unsigned char key[32];
    HU_ASSERT(hu_audit_load_key(dir, key) == HU_OK);
    hu_error_t tamper_verr = hu_audit_verify_chain(log_path, key);
    HU_ASSERT_EQ(tamper_verr, HU_ERR_CRYPTO_DECRYPT);

    unlink(log_path);
    char key_path[512];
    snprintf(key_path, sizeof(key_path), "%s/.audit_hmac_key", dir);
    unlink(key_path);
    rmdir(dir);
}

static void test_audit_key_rotation_basic(void) {
    hu_allocator_t sys = hu_system_allocator();
    char tmp[] = "/tmp/hu_audit_rot_XXXXXX";
    char *dir = mkdtemp(tmp);
    HU_ASSERT_NOT_NULL(dir);

    hu_audit_config_t cfg = {.enabled = true, .log_path = "rot.log", .max_size_mb = 10};
    hu_audit_logger_t *log = hu_audit_logger_create(&sys, &cfg, dir);
    HU_ASSERT_NOT_NULL(log);

    hu_audit_event_t ev1, ev2;
    hu_audit_event_init(&ev1, HU_AUDIT_COMMAND_EXECUTION);
    hu_audit_event_with_action(&ev1, "ls", "low", true, true);
    hu_audit_event_init(&ev2, HU_AUDIT_FILE_ACCESS);
    HU_ASSERT(hu_audit_logger_log(log, &ev1) == HU_OK);
    HU_ASSERT(hu_audit_rotate_key(log) == HU_OK);
    HU_ASSERT(hu_audit_logger_log(log, &ev2) == HU_OK);
    hu_audit_logger_destroy(log, &sys);

    char log_path[512];
    snprintf(log_path, sizeof(log_path), "%s/rot.log", dir);
    hu_error_t verr = hu_audit_verify_chain(log_path, NULL);
    HU_ASSERT_EQ(verr, HU_OK);

    unlink(log_path);
    char key_path[512], hist_path[512];
    snprintf(key_path, sizeof(key_path), "%s/.audit_hmac_key", dir);
    snprintf(hist_path, sizeof(hist_path), "%s/.audit_key_history", dir);
    unlink(key_path);
    unlink(hist_path);
    rmdir(dir);
}

static void test_audit_key_rotation_verify_detects_tamper_after_rotation(void) {
    hu_allocator_t sys = hu_system_allocator();
    char tmp[] = "/tmp/hu_audit_tamper2_XXXXXX";
    char *dir = mkdtemp(tmp);
    HU_ASSERT_NOT_NULL(dir);

    hu_audit_config_t cfg = {.enabled = true, .log_path = "tamper2.log", .max_size_mb = 10};
    hu_audit_logger_t *log = hu_audit_logger_create(&sys, &cfg, dir);
    HU_ASSERT_NOT_NULL(log);

    hu_audit_event_t ev1, ev2;
    hu_audit_event_init(&ev1, HU_AUDIT_COMMAND_EXECUTION);
    hu_audit_event_with_action(&ev1, "ls", "low", true, true);
    hu_audit_event_init(&ev2, HU_AUDIT_FILE_ACCESS);
    HU_ASSERT(hu_audit_logger_log(log, &ev1) == HU_OK);
    HU_ASSERT(hu_audit_rotate_key(log) == HU_OK);
    HU_ASSERT(hu_audit_logger_log(log, &ev2) == HU_OK);
    hu_audit_logger_destroy(log, &sys);

    char log_path[512];
    snprintf(log_path, sizeof(log_path), "%s/tamper2.log", dir);
    FILE *f = fopen(log_path, "r+b");
    HU_ASSERT_NOT_NULL(f);
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

    hu_error_t tamper_verr = hu_audit_verify_chain(log_path, NULL);
    HU_ASSERT_EQ(tamper_verr, HU_ERR_CRYPTO_DECRYPT);

    unlink(log_path);
    char key_path[512], hist_path[512];
    snprintf(key_path, sizeof(key_path), "%s/.audit_hmac_key", dir);
    snprintf(hist_path, sizeof(hist_path), "%s/.audit_key_history", dir);
    unlink(key_path);
    unlink(hist_path);
    rmdir(dir);
}

static void test_audit_rotation_interval(void) {
    hu_allocator_t sys = hu_system_allocator();
    char tmp[] = "/tmp/hu_audit_intv_XXXXXX";
    char *dir = mkdtemp(tmp);
    HU_ASSERT_NOT_NULL(dir);

    hu_audit_config_t cfg = {.enabled = true, .log_path = "intv.log", .max_size_mb = 10};
    hu_audit_logger_t *log = hu_audit_logger_create(&sys, &cfg, dir);
    HU_ASSERT_NOT_NULL(log);

    hu_audit_set_rotation_interval(log, 1); /* 1 hour */

    hu_audit_event_t ev;
    hu_audit_event_init(&ev, HU_AUDIT_COMMAND_EXECUTION);
    hu_audit_event_with_action(&ev, "echo a", "low", true, true);
    HU_ASSERT(hu_audit_logger_log(log, &ev) == HU_OK);

    hu_audit_test_set_last_rotation_epoch(log, time(NULL) - 7200); /* 2 hours ago */
    HU_ASSERT(hu_audit_logger_log(log, &ev) == HU_OK);

    hu_audit_logger_destroy(log, &sys);

    char log_path[512];
    snprintf(log_path, sizeof(log_path), "%s/intv.log", dir);
    hu_error_t verr = hu_audit_verify_chain(log_path, NULL);
    HU_ASSERT_EQ(verr, HU_OK);

    /* Verify key_rotation entry exists */
    FILE *fr = fopen(log_path, "rb");
    HU_ASSERT_NOT_NULL(fr);
    char buf[4096];
    size_t total = fread(buf, 1, sizeof(buf) - 1, fr);
    fclose(fr);
    buf[total] = '\0';
    HU_ASSERT(strstr(buf, "key_rotation") != NULL);

    unlink(log_path);
    char key_path[512], hist_path[512];
    snprintf(key_path, sizeof(key_path), "%s/.audit_hmac_key", dir);
    snprintf(hist_path, sizeof(hist_path), "%s/.audit_key_history", dir);
    unlink(key_path);
    unlink(hist_path);
    rmdir(dir);
}

static void test_audit_chain_delete_detected(void) {
    hu_allocator_t sys = hu_system_allocator();
    char tmp[] = "/tmp/hu_audit_del_XXXXXX";
    char *dir = mkdtemp(tmp);
    HU_ASSERT_NOT_NULL(dir);

    hu_audit_config_t cfg = {.enabled = true, .log_path = "del.log", .max_size_mb = 10};
    hu_audit_logger_t *log = hu_audit_logger_create(&sys, &cfg, dir);
    HU_ASSERT_NOT_NULL(log);

    hu_audit_event_t ev1, ev2;
    hu_audit_event_init(&ev1, HU_AUDIT_COMMAND_EXECUTION);
    hu_audit_event_init(&ev2, HU_AUDIT_FILE_ACCESS);
    HU_ASSERT(hu_audit_logger_log(log, &ev1) == HU_OK);
    HU_ASSERT(hu_audit_logger_log(log, &ev2) == HU_OK);
    hu_audit_logger_destroy(log, &sys);

    char log_path[512];
    snprintf(log_path, sizeof(log_path), "%s/del.log", dir);
    char buf[8192];
    size_t total = 0;
    FILE *fr = fopen(log_path, "rb");
    HU_ASSERT_NOT_NULL(fr);
    char line[4096];
    bool first = true;
    while (fgets(line, sizeof(line), fr)) {
        if (first) {
            first = false;
            continue; /* Skip first line (delete it) */
        }
        size_t n = strlen(line);
        memcpy(buf + total, line, n + 1);
        total += n + 1;
    }
    fclose(fr);

    FILE *fw = fopen(log_path, "wb");
    HU_ASSERT_NOT_NULL(fw);
    fwrite(buf, 1, total, fw);
    fclose(fw);

    unsigned char key[32];
    HU_ASSERT(hu_audit_load_key(dir, key) == HU_OK);
    hu_error_t del_verr = hu_audit_verify_chain(log_path, key);
    HU_ASSERT_EQ(del_verr, HU_ERR_CRYPTO_DECRYPT);

    unlink(log_path);
    char key_path[512];
    snprintf(key_path, sizeof(key_path), "%s/.audit_hmac_key", dir);
    unlink(key_path);
    rmdir(dir);
}

static void test_sandbox_noop(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_sandbox_alloc_t alloc = {
        .ctx = sys.ctx,
        .alloc = sys.alloc,
        .free = sys.free,
    };
    hu_sandbox_storage_t *st = hu_sandbox_storage_create(&alloc);
    HU_ASSERT_NOT_NULL(st);
    hu_sandbox_t sb = hu_sandbox_create(HU_SANDBOX_NONE, "/tmp/workspace", st, &alloc);
    HU_ASSERT(sb.ctx != NULL);
    HU_ASSERT(sb.vtable != NULL);
    HU_ASSERT(strcmp(hu_sandbox_name(&sb), "none") == 0);
    const char *argv[] = {"echo", "test"};
    const char *out[16];
    size_t out_count = 0;
    hu_error_t err = hu_sandbox_wrap_command(&sb, argv, 2, out, 16, &out_count);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT_EQ(out_count, 2);
    hu_sandbox_storage_destroy(st, &alloc);
}

/* Test backend vtables directly (bypass create fallback to noop) */
static void test_sandbox_landlock_non_linux_or_test(void) {
    hu_landlock_ctx_t ctx;
    hu_landlock_sandbox_init(&ctx, "/tmp");
    hu_sandbox_t sb = hu_landlock_sandbox_get(&ctx);
    bool avail = hu_sandbox_is_available(&sb);
#if defined(__linux__) && !HU_IS_TEST
    (void)avail; /* May be true if kernel has Landlock */
#else
    HU_ASSERT_FALSE(avail); /* macOS or HU_IS_TEST: not available */
#endif
    const char *argv[] = {"echo", "x"};
    const char *out[16];
    size_t out_count = 0;
    hu_error_t err = hu_sandbox_wrap_command(&sb, argv, 2, out, 16, &out_count);
#ifndef __linux__
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
#else
    HU_ASSERT(err == HU_OK || err == HU_ERR_NOT_SUPPORTED);
#endif
}

static void test_sandbox_firejail_non_linux(void) {
#ifndef __linux__
    hu_firejail_ctx_t ctx;
    hu_firejail_sandbox_init(&ctx, "/tmp");
    hu_sandbox_t sb = hu_firejail_sandbox_get(&ctx);
    HU_ASSERT_FALSE(hu_sandbox_is_available(&sb));
    const char *argv[] = {"echo", "x"};
    const char *out[16];
    size_t out_count = 0;
    hu_error_t err = hu_sandbox_wrap_command(&sb, argv, 2, out, 16, &out_count);
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
#endif
}

static void test_sandbox_bubblewrap_non_linux(void) {
#ifndef __linux__
    hu_bubblewrap_ctx_t ctx;
    hu_bubblewrap_sandbox_init(&ctx, "/tmp");
    hu_sandbox_t sb = hu_bubblewrap_sandbox_get(&ctx);
    HU_ASSERT_FALSE(hu_sandbox_is_available(&sb));
    const char *argv[] = {"echo", "x"};
    const char *out[16];
    size_t out_count = 0;
    hu_error_t err = hu_sandbox_wrap_command(&sb, argv, 2, out, 16, &out_count);
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
#endif
}

static void test_seatbelt_test_mode(void) {
    hu_seatbelt_ctx_t ctx;
    hu_seatbelt_sandbox_init(&ctx, "/tmp/workspace");
    hu_sandbox_t sb = hu_seatbelt_sandbox_get(&ctx);
    const char *argv[] = {"echo", "hello"};
    const char *out[16];
    size_t out_count = 0;
    hu_error_t err = hu_sandbox_wrap_command(&sb, argv, 2, out, 16, &out_count);
#ifdef __APPLE__
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out_count, 2u);
    HU_ASSERT(strcmp(out[0], "echo") == 0);
    HU_ASSERT(strcmp(out[1], "hello") == 0);
#else
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
#endif
}

static void test_bubblewrap_test_mode(void) {
    hu_bubblewrap_ctx_t ctx;
    hu_bubblewrap_sandbox_init(&ctx, "/tmp/workspace");
    hu_sandbox_t sb = hu_bubblewrap_sandbox_get(&ctx);
    const char *argv[] = {"echo", "hello"};
    const char *out[16];
    size_t out_count = 0;
    hu_error_t err = hu_sandbox_wrap_command(&sb, argv, 2, out, 16, &out_count);
#if defined(__linux__) && defined(HU_GATEWAY_POSIX)
    if (err == HU_OK) {
        HU_ASSERT_EQ(out_count, 2u);
        HU_ASSERT(strcmp(out[0], "echo") == 0);
        HU_ASSERT(strcmp(out[1], "hello") == 0);
    }
#else
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
#endif
}

static void test_bubblewrap_is_available(void) {
    hu_bubblewrap_ctx_t ctx;
    hu_bubblewrap_sandbox_init(&ctx, "/tmp");
    hu_sandbox_t sb = hu_bubblewrap_sandbox_get(&ctx);
#if defined(__linux__) && defined(HU_GATEWAY_POSIX)
    /* On Linux: true in HU_IS_TEST; in production depends on bwrap binary */
    bool avail = hu_sandbox_is_available(&sb);
#if HU_IS_TEST
    HU_ASSERT(avail);
#else
    (void)avail;
#endif
#else
    HU_ASSERT_FALSE(hu_sandbox_is_available(&sb));
#endif
}

/* --- Seatbelt (macOS) sandbox tests --- */
static void test_sandbox_seatbelt_vtable_wiring(void) {
    hu_seatbelt_ctx_t ctx;
    hu_seatbelt_sandbox_init(&ctx, "/tmp/workspace");
    hu_sandbox_t sb = hu_seatbelt_sandbox_get(&ctx);
    HU_ASSERT(sb.ctx != NULL);
    HU_ASSERT(sb.vtable != NULL);
    HU_ASSERT(strcmp(hu_sandbox_name(&sb), "seatbelt") == 0);
    HU_ASSERT(strlen(hu_sandbox_description(&sb)) > 0);
}

static void test_sandbox_seatbelt_availability(void) {
    hu_seatbelt_ctx_t ctx;
    hu_seatbelt_sandbox_init(&ctx, "/tmp");
    hu_sandbox_t sb = hu_seatbelt_sandbox_get(&ctx);
#ifdef __APPLE__
    HU_ASSERT(hu_sandbox_is_available(&sb));
#else
    HU_ASSERT_FALSE(hu_sandbox_is_available(&sb));
#endif
}

static void test_sandbox_seatbelt_wrap_command(void) {
    hu_seatbelt_ctx_t ctx;
    hu_seatbelt_sandbox_init(&ctx, "/tmp/workspace");
    hu_sandbox_t sb = hu_seatbelt_sandbox_get(&ctx);
    const char *argv[] = {"echo", "test"};
    const char *out[16];
    size_t out_count = 0;
    hu_error_t err = hu_sandbox_wrap_command(&sb, argv, 2, out, 16, &out_count);
#ifdef __APPLE__
    HU_ASSERT_EQ(err, HU_OK);
#if HU_IS_TEST
    /* In test mode: pass-through without sandbox-exec */
    HU_ASSERT_EQ(out_count, 2u);
    HU_ASSERT(strcmp(out[0], "echo") == 0);
    HU_ASSERT(strcmp(out[1], "test") == 0);
#else
    HU_ASSERT_EQ(out_count, 5u);
    HU_ASSERT(strcmp(out[0], "sandbox-exec") == 0);
    HU_ASSERT(strcmp(out[1], "-p") == 0);
    HU_ASSERT(strstr(out[2], "deny default") != NULL);
    HU_ASSERT(strstr(out[2], "/tmp/workspace") != NULL);
    HU_ASSERT(strcmp(out[3], "echo") == 0);
    HU_ASSERT(strcmp(out[4], "test") == 0);
#endif
#else
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
#endif
}

static void test_sandbox_seatbelt_profile_contains_workspace(void) {
    hu_seatbelt_ctx_t ctx;
    hu_seatbelt_sandbox_init(&ctx, "/home/user/project");
    HU_ASSERT(strstr(ctx.profile, "/home/user/project") != NULL);
    HU_ASSERT(strstr(ctx.profile, "deny default") != NULL);
    HU_ASSERT(strstr(ctx.profile, "deny network") != NULL);
}

static void test_sandbox_seatbelt_null_args(void) {
    hu_seatbelt_ctx_t ctx;
    hu_seatbelt_sandbox_init(&ctx, "/tmp");
    hu_sandbox_t sb = hu_seatbelt_sandbox_get(&ctx);
    hu_error_t err = hu_sandbox_wrap_command(&sb, NULL, 0, NULL, 0, NULL);
#ifdef __APPLE__
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
#else
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
#endif
}

/* --- Landlock apply tests --- */
static void test_sandbox_landlock_apply_in_test_mode(void) {
    hu_landlock_ctx_t ctx;
    hu_landlock_sandbox_init(&ctx, "/tmp/workspace");
    hu_sandbox_t sb = hu_landlock_sandbox_get(&ctx);
    hu_error_t err = hu_sandbox_apply(&sb);
#ifdef __linux__
    HU_ASSERT_EQ(err, HU_OK);
#else
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
#endif
}

static void test_sandbox_landlock_vtable_has_apply(void) {
    hu_landlock_ctx_t ctx;
    hu_landlock_sandbox_init(&ctx, "/tmp");
    hu_sandbox_t sb = hu_landlock_sandbox_get(&ctx);
    HU_ASSERT(sb.vtable->apply != NULL);
}

static void test_sandbox_landlock_wrap_passthrough(void) {
    hu_landlock_ctx_t ctx;
    hu_landlock_sandbox_init(&ctx, "/tmp/ws");
    hu_sandbox_t sb = hu_landlock_sandbox_get(&ctx);
    const char *argv[] = {"ls", "-la"};
    const char *out[8];
    size_t out_count = 0;
    hu_error_t err = hu_sandbox_wrap_command(&sb, argv, 2, out, 8, &out_count);
#ifdef __linux__
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out_count, 2u);
    HU_ASSERT(strcmp(out[0], "ls") == 0);
    HU_ASSERT(strcmp(out[1], "-la") == 0);
#else
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
#endif
}

/* --- seccomp sandbox tests --- */
static void test_sandbox_seccomp_vtable_wiring(void) {
    hu_seccomp_ctx_t ctx;
    hu_seccomp_sandbox_init(&ctx, "/tmp/workspace", false);
    hu_sandbox_t sb = hu_seccomp_sandbox_get(&ctx);
    HU_ASSERT(sb.ctx != NULL);
    HU_ASSERT(sb.vtable != NULL);
    HU_ASSERT(strcmp(hu_sandbox_name(&sb), "seccomp") == 0);
}

static void test_sandbox_seccomp_availability(void) {
    hu_seccomp_ctx_t ctx;
    hu_seccomp_sandbox_init(&ctx, "/tmp", false);
    hu_sandbox_t sb = hu_seccomp_sandbox_get(&ctx);
#ifdef __linux__
#if HU_IS_TEST
    HU_ASSERT_FALSE(hu_sandbox_is_available(&sb));
#endif
#else
    HU_ASSERT_FALSE(hu_sandbox_is_available(&sb));
#endif
}

static void test_sandbox_seccomp_wrap_passthrough(void) {
    hu_seccomp_ctx_t ctx;
    hu_seccomp_sandbox_init(&ctx, "/tmp", true);
    hu_sandbox_t sb = hu_seccomp_sandbox_get(&ctx);
    const char *argv[] = {"echo", "hello"};
    const char *out[8];
    size_t out_count = 0;
    hu_error_t err = hu_sandbox_wrap_command(&sb, argv, 2, out, 8, &out_count);
#ifdef __linux__
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out_count, 2u);
#else
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
#endif
}

static void test_sandbox_seccomp_has_apply(void) {
    hu_seccomp_ctx_t ctx;
    hu_seccomp_sandbox_init(&ctx, "/tmp", false);
    hu_sandbox_t sb = hu_seccomp_sandbox_get(&ctx);
#ifdef __linux__
    HU_ASSERT(sb.vtable->apply != NULL);
#else
    HU_ASSERT(sb.vtable->apply == NULL);
#endif
}

static void test_sandbox_seccomp_network_flag(void) {
    hu_seccomp_ctx_t ctx;
    hu_seccomp_sandbox_init(&ctx, "/tmp", true);
    HU_ASSERT(ctx.allow_network == true);
    hu_seccomp_sandbox_init(&ctx, "/tmp", false);
    HU_ASSERT(ctx.allow_network == false);
}

/* --- WASI sandbox tests --- */
static void test_sandbox_wasi_vtable_wiring(void) {
    hu_wasi_sandbox_ctx_t ctx;
    hu_wasi_sandbox_init(&ctx, "/tmp/workspace");
    hu_sandbox_t sb = hu_wasi_sandbox_get(&ctx);
    HU_ASSERT(sb.ctx != NULL);
    HU_ASSERT(sb.vtable != NULL);
    HU_ASSERT(strcmp(hu_sandbox_name(&sb), "wasi") == 0);
    HU_ASSERT(strstr(hu_sandbox_description(&sb), "WASI") != NULL);
}

static void test_sandbox_wasi_wrap_command_format(void) {
    hu_wasi_sandbox_ctx_t ctx;
    hu_wasi_sandbox_init(&ctx, "/tmp/workspace");
    hu_sandbox_t sb = hu_wasi_sandbox_get(&ctx);
    const char *argv[] = {"program.wasm", "--arg1"};
    const char *out[16];
    size_t out_count = 0;
    hu_error_t err = hu_sandbox_wrap_command(&sb, argv, 2, out, 16, &out_count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out_count, 6u);
    HU_ASSERT(strstr(out[0], "wasmtime") != NULL || strstr(out[0], "wasmer") != NULL);
    HU_ASSERT(strcmp(out[1], "run") == 0);
    HU_ASSERT(strstr(out[2], "--dir=") != NULL);
    HU_ASSERT(strstr(out[2], "/tmp/workspace") != NULL);
    HU_ASSERT(strcmp(out[3], "--dir=/tmp") == 0);
    HU_ASSERT(strcmp(out[4], "program.wasm") == 0);
    HU_ASSERT(strcmp(out[5], "--arg1") == 0);
}

static void test_sandbox_wasi_no_apply(void) {
    hu_wasi_sandbox_ctx_t ctx;
    hu_wasi_sandbox_init(&ctx, "/tmp");
    hu_sandbox_t sb = hu_wasi_sandbox_get(&ctx);
    HU_ASSERT(sb.vtable->apply == NULL);
    HU_ASSERT_EQ(hu_sandbox_apply(&sb), HU_OK);
}

/* --- Firecracker sandbox tests --- */
static void test_sandbox_firecracker_vtable_wiring(void) {
    hu_firecracker_ctx_t ctx;
    hu_firecracker_sandbox_init(&ctx, "/tmp/workspace");
    hu_sandbox_t sb = hu_firecracker_sandbox_get(&ctx);
    HU_ASSERT(sb.ctx != NULL);
    HU_ASSERT(sb.vtable != NULL);
    HU_ASSERT(strcmp(hu_sandbox_name(&sb), "firecracker") == 0);
    HU_ASSERT(strstr(hu_sandbox_description(&sb), "microVM") != NULL);
}

static void test_sandbox_firecracker_not_available_in_test(void) {
    hu_firecracker_ctx_t ctx;
    hu_firecracker_sandbox_init(&ctx, "/tmp");
    hu_sandbox_t sb = hu_firecracker_sandbox_get(&ctx);
    HU_ASSERT_FALSE(hu_sandbox_is_available(&sb));
}

static void test_sandbox_firecracker_defaults(void) {
    hu_firecracker_ctx_t ctx;
    hu_firecracker_sandbox_init(&ctx, "/tmp/ws");
    HU_ASSERT_EQ(ctx.vcpu_count, 1u);
    HU_ASSERT_EQ(ctx.mem_size_mib, 128u);
    HU_ASSERT(strcmp(ctx.workspace_dir, "/tmp/ws") == 0);
    HU_ASSERT(strlen(ctx.kernel_path) > 0);
    HU_ASSERT(strlen(ctx.rootfs_path) > 0);
}

static void test_sandbox_firecracker_wrap_non_linux(void) {
#ifndef __linux__
    hu_firecracker_ctx_t ctx;
    hu_firecracker_sandbox_init(&ctx, "/tmp");
    hu_sandbox_t sb = hu_firecracker_sandbox_get(&ctx);
    const char *argv[] = {"echo", "x"};
    const char *out[16];
    size_t out_count = 0;
    hu_error_t err = hu_sandbox_wrap_command(&sb, argv, 2, out, 16, &out_count);
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
#endif
}

/* --- Auto-detection tests --- */
static void test_sandbox_auto_select_returns_valid(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_sandbox_alloc_t alloc = {
        .ctx = sys.ctx,
        .alloc = sys.alloc,
        .free = sys.free,
    };
    hu_sandbox_storage_t *st = hu_sandbox_storage_create(&alloc);
    HU_ASSERT_NOT_NULL(st);
    hu_sandbox_t sb = hu_sandbox_create(HU_SANDBOX_AUTO, "/tmp/ws", st, &alloc);
    HU_ASSERT(sb.ctx != NULL);
    HU_ASSERT(sb.vtable != NULL);
    const char *name = hu_sandbox_name(&sb);
    HU_ASSERT_NOT_NULL(name);
    HU_ASSERT(strlen(name) > 0);
    hu_sandbox_storage_destroy(st, &alloc);
}

static void test_sandbox_detect_available_runs(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_sandbox_alloc_t alloc = {
        .ctx = sys.ctx,
        .alloc = sys.alloc,
        .free = sys.free,
    };
    hu_available_backends_t avail = hu_sandbox_detect_available("/tmp", &alloc);
#ifdef __APPLE__
    HU_ASSERT(avail.seatbelt == true);
#endif
    (void)avail;
}

static void test_sandbox_apply_noop_returns_ok(void) {
    hu_noop_sandbox_ctx_t ctx;
    hu_sandbox_t sb = hu_noop_sandbox_get(&ctx);
    HU_ASSERT_EQ(hu_sandbox_apply(&sb), HU_OK);
}

static void test_sandbox_apply_null_returns_ok(void) {
    hu_sandbox_t sb = {.ctx = NULL, .vtable = NULL};
    HU_ASSERT_EQ(hu_sandbox_apply(&sb), HU_OK);
}

/* --- Landlock+seccomp combined --- */
static void test_sandbox_landlock_seccomp_vtable_wiring(void) {
    hu_landlock_seccomp_ctx_t ctx;
    hu_landlock_seccomp_sandbox_init(&ctx, "/tmp/ws", false);
    hu_sandbox_t sb = hu_landlock_seccomp_sandbox_get(&ctx);
    HU_ASSERT(sb.ctx != NULL);
    HU_ASSERT(sb.vtable != NULL);
    HU_ASSERT(strcmp(hu_sandbox_name(&sb), "landlock+seccomp") == 0);
    HU_ASSERT(strlen(hu_sandbox_description(&sb)) > 0);
}

static void test_sandbox_landlock_seccomp_has_apply(void) {
    hu_landlock_seccomp_ctx_t ctx;
    hu_landlock_seccomp_sandbox_init(&ctx, "/tmp", true);
    hu_sandbox_t sb = hu_landlock_seccomp_sandbox_get(&ctx);
    HU_ASSERT(sb.vtable->apply != NULL);
}

static void test_sandbox_landlock_seccomp_apply(void) {
    hu_landlock_seccomp_ctx_t ctx;
    hu_landlock_seccomp_sandbox_init(&ctx, "/tmp/ws", false);
    hu_sandbox_t sb = hu_landlock_seccomp_sandbox_get(&ctx);
    hu_error_t err = hu_sandbox_apply(&sb);
#ifdef __linux__
    HU_ASSERT_EQ(err, HU_OK);
#else
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
#endif
}

/* --- AppContainer (Windows) --- */
static void test_sandbox_appcontainer_vtable_wiring(void) {
    hu_appcontainer_ctx_t ctx;
    hu_appcontainer_sandbox_init(&ctx, "/tmp/ws");
    hu_sandbox_t sb = hu_appcontainer_sandbox_get(&ctx);
    HU_ASSERT(sb.ctx != NULL);
    HU_ASSERT(sb.vtable != NULL);
    HU_ASSERT(strcmp(hu_sandbox_name(&sb), "appcontainer") == 0);
}

static void test_sandbox_appcontainer_not_available_non_win(void) {
#ifndef _WIN32
    hu_appcontainer_ctx_t ctx;
    hu_appcontainer_sandbox_init(&ctx, "/tmp");
    hu_sandbox_t sb = hu_appcontainer_sandbox_get(&ctx);
    HU_ASSERT_FALSE(hu_sandbox_is_available(&sb));
#endif
}

static void test_sandbox_appcontainer_wrap_non_win(void) {
#ifndef _WIN32
    hu_appcontainer_ctx_t ctx;
    hu_appcontainer_sandbox_init(&ctx, "/tmp");
    hu_sandbox_t sb = hu_appcontainer_sandbox_get(&ctx);
    const char *argv[] = {"echo", "x"};
    const char *out[8];
    size_t out_count = 0;
    hu_error_t err = hu_sandbox_wrap_command(&sb, argv, 2, out, 8, &out_count);
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
#endif
}

/* --- Network proxy --- */
static void test_net_proxy_deny_all_blocks(void) {
    hu_net_proxy_t proxy;
    hu_net_proxy_init_deny_all(&proxy);
    HU_ASSERT_FALSE(hu_net_proxy_domain_allowed(&proxy, "evil.com"));
    HU_ASSERT_FALSE(hu_net_proxy_domain_allowed(&proxy, "example.com"));
}

static void test_net_proxy_allowlist(void) {
    hu_net_proxy_t proxy;
    hu_net_proxy_init_deny_all(&proxy);
    hu_net_proxy_allow_domain(&proxy, "api.openai.com");
    hu_net_proxy_allow_domain(&proxy, "api.anthropic.com");
    HU_ASSERT(hu_net_proxy_domain_allowed(&proxy, "api.openai.com"));
    HU_ASSERT(hu_net_proxy_domain_allowed(&proxy, "api.anthropic.com"));
    HU_ASSERT_FALSE(hu_net_proxy_domain_allowed(&proxy, "evil.com"));
}

static void test_net_proxy_wildcard_subdomain(void) {
    hu_net_proxy_t proxy;
    hu_net_proxy_init_deny_all(&proxy);
    hu_net_proxy_allow_domain(&proxy, "*.example.com");
    HU_ASSERT(hu_net_proxy_domain_allowed(&proxy, "api.example.com"));
    HU_ASSERT(hu_net_proxy_domain_allowed(&proxy, "sub.example.com"));
    HU_ASSERT_FALSE(hu_net_proxy_domain_allowed(&proxy, "example.org"));
}

static void test_net_proxy_disabled_allows_all(void) {
    hu_net_proxy_t proxy = {0};
    HU_ASSERT(hu_net_proxy_domain_allowed(&proxy, "anything.com"));
    HU_ASSERT(hu_net_proxy_domain_allowed(NULL, "anything.com"));
}

static void test_net_proxy_max_domains(void) {
    hu_net_proxy_t proxy;
    hu_net_proxy_init_deny_all(&proxy);
    for (int i = 0; i < HU_NET_PROXY_MAX_DOMAINS; i++) {
        HU_ASSERT(hu_net_proxy_allow_domain(&proxy, "domain.com"));
    }
    HU_ASSERT_FALSE(hu_net_proxy_allow_domain(&proxy, "overflow.com"));
}

/* --- sandbox API smoke tests --- */
static void test_sandbox_noop_available(void) {
    hu_noop_sandbox_ctx_t ctx;
    hu_sandbox_t sb = hu_noop_sandbox_get(&ctx);
    HU_ASSERT_TRUE(hu_sandbox_is_available(&sb));
    HU_ASSERT_STR_EQ(hu_sandbox_name(&sb), "none");
}

static void test_sandbox_noop_wrap_passthrough(void) {
    hu_noop_sandbox_ctx_t ctx;
    hu_sandbox_t sb = hu_noop_sandbox_get(&ctx);
    const char *argv[] = {"echo", "hello"};
    const char *out[8];
    size_t out_count = 0;
    hu_error_t err = hu_sandbox_wrap_command(&sb, argv, 2, out, 8, &out_count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out_count, 2u);
    HU_ASSERT_STR_EQ(out[0], "echo");
    HU_ASSERT_STR_EQ(out[1], "hello");
}

static void test_observer_noop(void) {
    hu_observer_t obs = hu_observer_noop();
    HU_ASSERT(strcmp(hu_observer_name(obs), "noop") == 0);
    hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_HEARTBEAT_TICK};
    hu_observer_record_event(obs, &ev);
}

static void test_observer_metrics(void) {
    hu_metrics_observer_ctx_t ctx;
    hu_observer_t obs = hu_observer_metrics_create(&ctx);
    HU_ASSERT(strcmp(hu_observer_name(obs), "metrics") == 0);
    hu_observer_metric_t m = {.tag = HU_OBSERVER_METRIC_TOKENS_USED, .value = 42};
    hu_observer_record_metric(obs, &m);
    HU_ASSERT_EQ(hu_observer_metrics_get(&ctx, HU_OBSERVER_METRIC_TOKENS_USED), 42);
}

static void test_observer_registry(void) {
    hu_observer_t o = hu_observer_registry_create("noop", NULL);
    HU_ASSERT(strcmp(hu_observer_name(o), "noop") == 0);
    o = hu_observer_registry_create("log", NULL);
    HU_ASSERT(strcmp(hu_observer_name(o), "log") == 0);
}

static void test_secret_store_disabled_returns_plaintext(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_secret_store_t *store = hu_secret_store_create(&sys, "/tmp", 0);
    HU_ASSERT_NOT_NULL(store);

    char *enc = NULL;
    hu_error_t err = hu_secret_store_encrypt(store, &sys, "sk-secret", &enc);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT_STR_EQ(enc, "sk-secret");

    sys.free(sys.ctx, enc, strlen(enc) + 1);
    hu_secret_store_destroy(store, &sys);
}

/* --- Docker sandbox tests --- */
static void test_sandbox_docker_vtable_wiring(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_docker_ctx_t ctx;
    hu_docker_sandbox_init(&ctx, "/tmp/workspace", "ubuntu:22.04", sys.ctx, sys.alloc, sys.free);
    hu_sandbox_t sb = hu_docker_sandbox_get(&ctx);
    HU_ASSERT(sb.ctx != NULL);
    HU_ASSERT(sb.vtable != NULL);
    HU_ASSERT(sb.vtable->wrap_command != NULL);
    HU_ASSERT(sb.vtable->is_available != NULL);
    HU_ASSERT(sb.vtable->name != NULL);
    HU_ASSERT(sb.vtable->description != NULL);
}

static void test_sandbox_docker_not_available_in_test(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_docker_ctx_t ctx;
    hu_docker_sandbox_init(&ctx, "/tmp", "alpine", sys.ctx, sys.alloc, sys.free);
    hu_sandbox_t sb = hu_docker_sandbox_get(&ctx);
    HU_ASSERT_FALSE(hu_sandbox_is_available(&sb));
}

static void test_sandbox_docker_wrap_format(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_docker_ctx_t ctx;
    hu_docker_sandbox_init(&ctx, "/tmp/ws", "myimage:latest", sys.ctx, sys.alloc, sys.free);
    hu_sandbox_t sb = hu_docker_sandbox_get(&ctx);
    const char *argv[] = {"echo", "hello"};
    const char *out[32];
    size_t out_count = 0;
    hu_error_t err = hu_sandbox_wrap_command(&sb, argv, 2, out, 32, &out_count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(out_count >= 12);
    HU_ASSERT_STR_EQ(out[0], "docker");
    HU_ASSERT_STR_EQ(out[1], "run");
    HU_ASSERT_STR_EQ(out[2], "--rm");
    HU_ASSERT_STR_EQ(out[out_count - 2], "echo");
    HU_ASSERT_STR_EQ(out[out_count - 1], "hello");
}

static void test_sandbox_docker_name_and_desc(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_docker_ctx_t ctx;
    hu_docker_sandbox_init(&ctx, "/tmp", "alpine", sys.ctx, sys.alloc, sys.free);
    hu_sandbox_t sb = hu_docker_sandbox_get(&ctx);
    HU_ASSERT_STR_EQ(hu_sandbox_name(&sb), "docker");
    HU_ASSERT(strlen(hu_sandbox_description(&sb)) > 0);
}

static void test_docker_sandbox_create(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_docker_ctx_t ctx;
    hu_docker_sandbox_init(&ctx, "/tmp/workspace", "alpine:latest", sys.ctx, sys.alloc, sys.free);
    hu_sandbox_t sb = hu_docker_sandbox_get(&ctx);
    HU_ASSERT(sb.ctx != NULL);
    HU_ASSERT(sb.vtable != NULL);
    HU_ASSERT_STR_EQ(hu_sandbox_name(&sb), "docker");
}

static void test_docker_sandbox_wrap_command(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_docker_ctx_t ctx;
    hu_docker_sandbox_init(&ctx, "/tmp/ws", "ubuntu:22.04", sys.ctx, sys.alloc, sys.free);
    hu_sandbox_t sb = hu_docker_sandbox_get(&ctx);
    const char *argv[] = {"echo", "hello"};
    const char *out[32];
    size_t out_count = 0;
    hu_error_t err = hu_sandbox_wrap_command(&sb, argv, 2, out, 32, &out_count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(out_count >= 12);
    HU_ASSERT_STR_EQ(out[0], "docker");
    HU_ASSERT_STR_EQ(out[1], "run");
    HU_ASSERT_STR_EQ(out[out_count - 2], "echo");
    HU_ASSERT_STR_EQ(out[out_count - 1], "hello");
}

static void test_docker_sandbox_has_methods(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_docker_ctx_t ctx;
    hu_docker_sandbox_init(&ctx, "/tmp", "alpine", sys.ctx, sys.alloc, sys.free);
    hu_sandbox_t sb = hu_docker_sandbox_get(&ctx);
    HU_ASSERT(sb.vtable->wrap_command != NULL);
    HU_ASSERT(sb.vtable->is_available != NULL);
    HU_ASSERT(sb.vtable->name != NULL);
    HU_ASSERT(sb.vtable->description != NULL);
    HU_ASSERT(sb.vtable->apply == NULL);
}

/* --- Firecracker wrap output validation (Linux) --- */
static void test_sandbox_firecracker_wrap_on_linux(void) {
#ifdef __linux__
    hu_firecracker_ctx_t ctx;
    hu_firecracker_sandbox_init(&ctx, "/tmp/ws");
    hu_sandbox_t sb = hu_firecracker_sandbox_get(&ctx);
    const char *argv[] = {"echo", "hello"};
    const char *out[16];
    size_t out_count = 0;
    hu_error_t err = hu_sandbox_wrap_command(&sb, argv, 2, out, 16, &out_count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out_count, 4);
    HU_ASSERT_STR_EQ(out[0], "firecracker");
    HU_ASSERT_STR_EQ(out[1], "--no-api");
    HU_ASSERT_STR_EQ(out[2], "--boot-timer");
    HU_ASSERT(strstr(out[3], "--config-file=") != NULL);
#endif
}

static void test_sandbox_firecracker_wrap_buffer_too_small(void) {
#ifdef __linux__
    hu_firecracker_ctx_t ctx;
    hu_firecracker_sandbox_init(&ctx, "/tmp/ws");
    hu_sandbox_t sb = hu_firecracker_sandbox_get(&ctx);
    const char *argv[] = {"echo"};
    const char *out[2];
    size_t out_count = 0;
    hu_error_t err = hu_sandbox_wrap_command(&sb, argv, 1, out, 2, &out_count);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
#endif
}

/* --- Seatbelt profile temp dir validation --- */
static void test_sandbox_seatbelt_profile_allows_tmp_write(void) {
    hu_seatbelt_ctx_t ctx;
    hu_seatbelt_sandbox_init(&ctx, "/tmp/workspace");
    HU_ASSERT(strstr(ctx.profile, "file-write* (subpath \"/tmp\")") != NULL);
    HU_ASSERT(strstr(ctx.profile, "file-write* (subpath \"/var/folders\")") != NULL);
    HU_ASSERT(strstr(ctx.profile, "file-write* (subpath \"/private/tmp\")") != NULL);
}

/* --- Config sandbox backend parsing coverage --- */
static void test_sandbox_create_each_backend(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_sandbox_alloc_t alloc = {
        .ctx = sys.ctx,
        .alloc = sys.alloc,
        .free = sys.free,
    };

    hu_sandbox_backend_t backends[] = {
        HU_SANDBOX_NONE,        HU_SANDBOX_LANDLOCK,     HU_SANDBOX_FIREJAIL,
        HU_SANDBOX_BUBBLEWRAP,  HU_SANDBOX_DOCKER,       HU_SANDBOX_SEATBELT,
        HU_SANDBOX_SECCOMP,     HU_SANDBOX_WASI,         HU_SANDBOX_LANDLOCK_SECCOMP,
        HU_SANDBOX_FIRECRACKER, HU_SANDBOX_APPCONTAINER, HU_SANDBOX_AUTO,
    };
    size_t n = sizeof(backends) / sizeof(backends[0]);

    for (size_t i = 0; i < n; i++) {
        hu_sandbox_storage_t *st = hu_sandbox_storage_create(&alloc);
        HU_ASSERT_NOT_NULL(st);
        hu_sandbox_t sb = hu_sandbox_create(backends[i], "/tmp/ws", st, &alloc);
        HU_ASSERT(sb.vtable != NULL);
        HU_ASSERT(strlen(hu_sandbox_name(&sb)) > 0);
        HU_ASSERT(strlen(hu_sandbox_description(&sb)) >= 0);
        hu_sandbox_storage_destroy(st, &alloc);
    }
}

/* --- hu_process_run_sandboxed tests --- */
static void test_process_run_sandboxed_null_args(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_run_result_t result = {0};
    hu_error_t err = hu_process_run_sandboxed(&sys, NULL, NULL, 1024, NULL, NULL, &result);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    const char *argv[] = {NULL};
    err = hu_process_run_sandboxed(&sys, argv, NULL, 1024, NULL, NULL, &result);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_process_run_sandboxed_with_null_setup(void) {
    hu_allocator_t sys = hu_system_allocator();
    const char *argv[] = {"echo", "test-sandbox", NULL};
    hu_run_result_t result = {0};
    hu_error_t err = hu_process_run_sandboxed(&sys, argv, NULL, 1024, NULL, NULL, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(result.stdout_cap > 0);
    HU_ASSERT(result.stderr_cap > 0);
    hu_run_result_free(&sys, &result);
}

static void test_process_run_with_policy_null(void) {
    hu_allocator_t sys = hu_system_allocator();
    const char *argv[] = {"echo", "policy-test", NULL};
    hu_run_result_t result = {0};
    hu_error_t err = hu_process_run_with_policy(&sys, argv, NULL, 1024, NULL, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(result.stdout_cap > 0);
    hu_run_result_free(&sys, &result);
}

static void test_process_run_with_policy_noop_sandbox(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_security_policy_t policy = {0};
    const char *argv[] = {"echo", "sandboxed", NULL};
    hu_run_result_t result = {0};
    hu_error_t err = hu_process_run_with_policy(&sys, argv, NULL, 1024, &policy, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(result.stdout_cap > 0);
    hu_run_result_free(&sys, &result);
}

/* --- Sandbox storage lifecycle --- */
static void test_sandbox_storage_create_destroy(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_sandbox_alloc_t alloc = {
        .ctx = sys.ctx,
        .alloc = sys.alloc,
        .free = sys.free,
    };
    hu_sandbox_storage_t *st = hu_sandbox_storage_create(&alloc);
    HU_ASSERT_NOT_NULL(st);
    hu_sandbox_storage_destroy(st, &alloc);
    hu_sandbox_storage_destroy(NULL, &alloc);
}

/* --- Net proxy edge cases --- */
static void test_net_proxy_null_domain(void) {
    hu_net_proxy_t proxy;
    hu_net_proxy_init_deny_all(&proxy);
    HU_ASSERT_FALSE(hu_net_proxy_domain_allowed(&proxy, NULL));
}

static void test_net_proxy_add_null_domain(void) {
    hu_net_proxy_t proxy;
    hu_net_proxy_init_deny_all(&proxy);
    HU_ASSERT_FALSE(hu_net_proxy_allow_domain(&proxy, NULL));
    HU_ASSERT_FALSE(hu_net_proxy_allow_domain(NULL, "test.com"));
}

static void test_net_proxy_case_insensitive(void) {
    hu_net_proxy_t proxy;
    hu_net_proxy_init_deny_all(&proxy);
    hu_net_proxy_allow_domain(&proxy, "Example.COM");
    HU_ASSERT(hu_net_proxy_domain_allowed(&proxy, "example.com"));
    HU_ASSERT(hu_net_proxy_domain_allowed(&proxy, "EXAMPLE.COM"));
    HU_ASSERT(hu_net_proxy_domain_allowed(&proxy, "Example.Com"));
}

static void test_net_proxy_wildcard_case_insensitive(void) {
    hu_net_proxy_t proxy;
    hu_net_proxy_init_deny_all(&proxy);
    hu_net_proxy_allow_domain(&proxy, "*.Example.COM");
    HU_ASSERT(hu_net_proxy_domain_allowed(&proxy, "sub.example.com"));
    HU_ASSERT(hu_net_proxy_domain_allowed(&proxy, "SUB.EXAMPLE.COM"));
}

static void test_net_proxy_empty_domain_rejected(void) {
    hu_net_proxy_t proxy;
    hu_net_proxy_init_deny_all(&proxy);
    hu_net_proxy_allow_domain(&proxy, "example.com");
    HU_ASSERT_FALSE(hu_net_proxy_domain_allowed(&proxy, ""));
}

/* --- Seatbelt truncated profile guard --- */
static void test_seatbelt_wrap_fails_on_truncated_profile(void) {
    hu_seatbelt_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.profile_len = 0;
    hu_sandbox_t sb = hu_seatbelt_sandbox_get(&ctx);
    const char *argv[] = {"echo", "hello"};
    const char *out[8];
    size_t out_count = 0;
    hu_error_t err = hu_sandbox_wrap_command(&sb, argv, 2, out, 8, &out_count);
#ifdef __APPLE__
#if HU_IS_TEST
    /* In test mode: pass-through, no profile validation */
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out_count, 2u);
#else
    HU_ASSERT_EQ(err, HU_ERR_INTERNAL);
    HU_ASSERT_EQ(out_count, 0);
#endif
#else
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
#endif
}

/* --- Firejail extra_args wiring --- */
static void test_firejail_extra_args_wired(void) {
    hu_firejail_ctx_t ctx;
    hu_firejail_sandbox_init(&ctx, "/tmp/ws");
    const char *extras[] = {"--whitelist=/opt"};
    hu_firejail_sandbox_set_extra_args(&ctx, extras, 1);
    HU_ASSERT_EQ(ctx.extra_args_len, 1);
    HU_ASSERT(ctx.extra_args == extras);
#ifdef __linux__
    hu_sandbox_t sb = hu_firejail_sandbox_get(&ctx);
    const char *argv[] = {"ls"};
    const char *out[16];
    size_t out_count = 0;
    hu_error_t err = hu_sandbox_wrap_command(&sb, argv, 1, out, 16, &out_count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out_count, 7);
    HU_ASSERT(strcmp(out[5], "--whitelist=/opt") == 0);
    HU_ASSERT(strcmp(out[6], "ls") == 0);
#endif
}

/* --- WASI NULL workspace guard --- */
static void test_wasi_wrap_fails_null_workspace(void) {
    hu_wasi_sandbox_ctx_t ctx;
    hu_wasi_sandbox_init(&ctx, NULL);
    hu_sandbox_t sb = hu_wasi_sandbox_get(&ctx);
    const char *argv[] = {"test.wasm"};
    const char *out[8];
    size_t out_count = 0;
    hu_error_t err = hu_sandbox_wrap_command(&sb, argv, 1, out, 8, &out_count);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

/* --- Landlock NULL workspace guard --- */
static void test_landlock_apply_fails_null_workspace(void) {
    hu_landlock_ctx_t ctx;
    hu_landlock_sandbox_init(&ctx, NULL);
    hu_sandbox_t sb = hu_landlock_sandbox_get(&ctx);
    if (sb.vtable && sb.vtable->apply) {
        hu_error_t err = sb.vtable->apply(sb.ctx);
#ifdef __linux__
        HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
#else
        HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
#endif
    }
}

void run_security_tests(void) {
    static hu_allocator_t sys = {0};
    sys = hu_system_allocator();
    g_alloc = &sys;

    HU_TEST_SUITE("Security Policy");
    HU_RUN_TEST(test_autonomy_level_values);
    HU_RUN_TEST(test_risk_level_values);
    HU_RUN_TEST(test_policy_can_act_readonly);
    HU_RUN_TEST(test_policy_can_act_supervised);
    HU_RUN_TEST(test_policy_can_act_full);
    HU_RUN_TEST(test_policy_allowed_commands);
    HU_RUN_TEST(test_policy_blocked_commands);
    HU_RUN_TEST(test_policy_readonly_blocks_all);
    HU_RUN_TEST(test_policy_command_injection_blocked);
    HU_RUN_TEST(test_policy_risk_levels);
    HU_RUN_TEST(test_policy_validate_command);
    HU_RUN_TEST(test_rate_tracker);

    HU_TEST_SUITE("Pairing");
    HU_RUN_TEST(test_pairing_constant_time_eq);
    HU_RUN_TEST(test_pairing_guard_create_destroy);
    HU_RUN_TEST(test_pairing_guard_disabled);
    HU_RUN_TEST(test_pairing_guard_with_tokens);
    HU_RUN_TEST(test_pairing_attempt_pair);

    HU_TEST_SUITE("Audit");
    HU_RUN_TEST(test_audit_event_init);
    HU_RUN_TEST(test_audit_event_write_json);
    HU_RUN_TEST(test_audit_logger_disabled);
    HU_RUN_TEST(test_audit_chain_verify_valid);
    HU_RUN_TEST(test_audit_chain_tamper_detected);
    HU_RUN_TEST(test_audit_key_rotation_basic);
    HU_RUN_TEST(test_audit_key_rotation_verify_detects_tamper_after_rotation);
    HU_RUN_TEST(test_audit_rotation_interval);
    HU_RUN_TEST(test_audit_chain_delete_detected);

    HU_TEST_SUITE("Sandbox");
    HU_RUN_TEST(test_sandbox_noop);
    HU_RUN_TEST(test_sandbox_landlock_non_linux_or_test);
    HU_RUN_TEST(test_sandbox_firejail_non_linux);
    HU_RUN_TEST(test_sandbox_bubblewrap_non_linux);
    HU_RUN_TEST(test_bubblewrap_test_mode);
    HU_RUN_TEST(test_bubblewrap_is_available);

    HU_TEST_SUITE("Sandbox — Seatbelt (macOS)");
    HU_RUN_TEST(test_sandbox_seatbelt_vtable_wiring);
    HU_RUN_TEST(test_sandbox_seatbelt_availability);
    HU_RUN_TEST(test_seatbelt_test_mode);
    HU_RUN_TEST(test_sandbox_seatbelt_wrap_command);
    HU_RUN_TEST(test_sandbox_seatbelt_profile_contains_workspace);
    HU_RUN_TEST(test_sandbox_seatbelt_null_args);

    HU_TEST_SUITE("Sandbox — Landlock (apply)");
    HU_RUN_TEST(test_sandbox_landlock_apply_in_test_mode);
    HU_RUN_TEST(test_sandbox_landlock_vtable_has_apply);
    HU_RUN_TEST(test_sandbox_landlock_wrap_passthrough);

    HU_TEST_SUITE("Sandbox — seccomp-BPF");
    HU_RUN_TEST(test_sandbox_seccomp_vtable_wiring);
    HU_RUN_TEST(test_sandbox_seccomp_availability);
    HU_RUN_TEST(test_sandbox_seccomp_wrap_passthrough);
    HU_RUN_TEST(test_sandbox_seccomp_has_apply);
    HU_RUN_TEST(test_sandbox_seccomp_network_flag);

    HU_TEST_SUITE("Sandbox — WASI");
    HU_RUN_TEST(test_sandbox_wasi_vtable_wiring);
    HU_RUN_TEST(test_sandbox_wasi_wrap_command_format);
    HU_RUN_TEST(test_sandbox_wasi_no_apply);

    HU_TEST_SUITE("Sandbox — Firecracker");
    HU_RUN_TEST(test_sandbox_firecracker_vtable_wiring);
    HU_RUN_TEST(test_sandbox_firecracker_not_available_in_test);
    HU_RUN_TEST(test_sandbox_firecracker_defaults);
    HU_RUN_TEST(test_sandbox_firecracker_wrap_non_linux);
    HU_RUN_TEST(test_sandbox_firecracker_wrap_on_linux);
    HU_RUN_TEST(test_sandbox_firecracker_wrap_buffer_too_small);

    HU_TEST_SUITE("Sandbox — Landlock+seccomp combined");
    HU_RUN_TEST(test_sandbox_landlock_seccomp_vtable_wiring);
    HU_RUN_TEST(test_sandbox_landlock_seccomp_has_apply);
    HU_RUN_TEST(test_sandbox_landlock_seccomp_apply);

    HU_TEST_SUITE("Sandbox — AppContainer (Windows)");
    HU_RUN_TEST(test_sandbox_appcontainer_vtable_wiring);
    HU_RUN_TEST(test_sandbox_appcontainer_not_available_non_win);
    HU_RUN_TEST(test_sandbox_appcontainer_wrap_non_win);

    HU_TEST_SUITE("Sandbox — Network Proxy");
    HU_RUN_TEST(test_net_proxy_deny_all_blocks);
    HU_RUN_TEST(test_net_proxy_allowlist);
    HU_RUN_TEST(test_net_proxy_wildcard_subdomain);
    HU_RUN_TEST(test_net_proxy_disabled_allows_all);
    HU_RUN_TEST(test_net_proxy_max_domains);

    HU_TEST_SUITE("Sandbox — Seatbelt Profile");
    HU_RUN_TEST(test_sandbox_seatbelt_profile_allows_tmp_write);
    HU_RUN_TEST(test_seatbelt_wrap_fails_on_truncated_profile);

    HU_TEST_SUITE("Sandbox — Firejail Extra Args");
    HU_RUN_TEST(test_firejail_extra_args_wired);

    HU_TEST_SUITE("Sandbox — NULL Workspace Guards");
    HU_RUN_TEST(test_wasi_wrap_fails_null_workspace);
    HU_RUN_TEST(test_landlock_apply_fails_null_workspace);

    HU_TEST_SUITE("Sandbox — Backend Coverage");
    HU_RUN_TEST(test_sandbox_create_each_backend);
    HU_RUN_TEST(test_sandbox_storage_create_destroy);

    HU_TEST_SUITE("Sandbox — Auto-detection & Apply");
    HU_RUN_TEST(test_sandbox_auto_select_returns_valid);
    HU_RUN_TEST(test_sandbox_detect_available_runs);
    HU_RUN_TEST(test_sandbox_apply_noop_returns_ok);
    HU_RUN_TEST(test_sandbox_apply_null_returns_ok);
    HU_RUN_TEST(test_sandbox_noop_available);
    HU_RUN_TEST(test_sandbox_noop_wrap_passthrough);

    HU_TEST_SUITE("Sandbox — hu_process_run_sandboxed");
    HU_RUN_TEST(test_process_run_sandboxed_null_args);
    HU_RUN_TEST(test_process_run_sandboxed_with_null_setup);

    HU_TEST_SUITE("Sandbox — hu_process_run_with_policy");
    HU_RUN_TEST(test_process_run_with_policy_null);
    HU_RUN_TEST(test_process_run_with_policy_noop_sandbox);

    HU_TEST_SUITE("Network Proxy — Edge Cases");
    HU_RUN_TEST(test_net_proxy_null_domain);
    HU_RUN_TEST(test_net_proxy_add_null_domain);
    HU_RUN_TEST(test_net_proxy_case_insensitive);
    HU_RUN_TEST(test_net_proxy_wildcard_case_insensitive);
    HU_RUN_TEST(test_net_proxy_empty_domain_rejected);

    HU_TEST_SUITE("Observer");
    HU_RUN_TEST(test_observer_noop);
    HU_RUN_TEST(test_observer_metrics);
    HU_RUN_TEST(test_observer_registry);

    HU_TEST_SUITE("Secrets");
    HU_RUN_TEST(test_secret_store_is_encrypted);
    HU_RUN_TEST(test_hex_encode_decode);
    HU_RUN_TEST(test_secret_store_encrypt_decrypt);
    HU_RUN_TEST(test_secret_store_plaintext_passthrough);
    HU_RUN_TEST(test_secret_store_disabled_returns_plaintext);

    HU_TEST_SUITE("Sandbox — Docker");
    HU_RUN_TEST(test_sandbox_docker_vtable_wiring);
    HU_RUN_TEST(test_sandbox_docker_not_available_in_test);
    HU_RUN_TEST(test_sandbox_docker_wrap_format);
    HU_RUN_TEST(test_sandbox_docker_name_and_desc);
    HU_RUN_TEST(test_docker_sandbox_create);
    HU_RUN_TEST(test_docker_sandbox_wrap_command);
    HU_RUN_TEST(test_docker_sandbox_has_methods);
}
