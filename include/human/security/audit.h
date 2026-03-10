#ifndef HU_AUDIT_H
#define HU_AUDIT_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* ── Audit event types ─────────────────────────────────────────────── */

typedef enum hu_audit_event_type {
    HU_AUDIT_COMMAND_EXECUTION,
    HU_AUDIT_FILE_ACCESS,
    HU_AUDIT_CONFIG_CHANGE,
    HU_AUDIT_AUTH_SUCCESS,
    HU_AUDIT_AUTH_FAILURE,
    HU_AUDIT_POLICY_VIOLATION,
    HU_AUDIT_SECURITY_EVENT,
} hu_audit_event_type_t;

const char *hu_audit_event_type_string(hu_audit_event_type_t t);

/* ── Severity (for filtering) ───────────────────────────────────────── */

typedef enum hu_audit_severity {
    HU_AUDIT_SEV_LOW,
    HU_AUDIT_SEV_MEDIUM,
    HU_AUDIT_SEV_HIGH,
} hu_audit_severity_t;

hu_audit_severity_t hu_audit_event_severity(hu_audit_event_type_t t);

/* ── Audit event structures ─────────────────────────────────────────── */

typedef struct hu_audit_actor {
    const char *channel;
    const char *user_id;  /* may be NULL */
    const char *username; /* may be NULL */
} hu_audit_actor_t;

typedef struct hu_audit_identity {
    uint64_t agent_id;           /* unique agent instance ID */
    const char *model_version;   /* e.g. "gpt-4o-2024-08-06" */
    const char *auth_token_hash; /* first 8 chars of SHA256 of auth token (for correlation) */
} hu_audit_identity_t;

typedef struct hu_audit_input {
    const char *trigger_type;   /* "user_prompt", "webhook", "cron", "mailbox" */
    const char *trigger_source; /* channel name or cron ID */
    const char *prompt_hash;    /* SHA256 of prompt (for correlation without storing PII) */
    size_t prompt_length;       /* length of prompt in chars */
} hu_audit_input_t;

typedef struct hu_audit_reasoning {
    const char *decision;  /* "policy_allow", "policy_deny", "approval_required", "auto_approved" */
    const char *rule_name; /* which policy rule matched */
    float confidence;      /* 0.0-1.0 (set to -1 if not applicable) */
    uint32_t context_tokens; /* how many tokens in context when decision was made */
} hu_audit_reasoning_t;

typedef struct hu_audit_action {
    const char *command;    /* may be NULL */
    const char *risk_level; /* may be NULL */
    bool approved;
    bool allowed;
} hu_audit_action_t;

typedef struct hu_audit_result {
    bool success;
    int32_t exit_code;    /* -1 if not set */
    uint64_t duration_ms; /* 0 if not set */
    const char *err_msg;  /* may be NULL */
} hu_audit_result_t;

typedef struct hu_audit_security_ctx {
    bool policy_violation;
    uint32_t rate_limit_remaining; /* 0 means not set */
    const char *sandbox_backend;   /* may be NULL */
} hu_audit_security_ctx_t;

typedef struct hu_audit_event {
    int64_t timestamp_s;
    uint64_t event_id;
    hu_audit_event_type_t event_type;
    hu_audit_actor_t actor;   /* channel/username set to NULL if not used */
    hu_audit_action_t action; /* command set to NULL if not used */
    hu_audit_result_t result; /* exit_code -1, duration_ms 0, err_msg NULL if not set */
    hu_audit_security_ctx_t security;
    hu_audit_identity_t identity;
    hu_audit_input_t input;
    hu_audit_reasoning_t reasoning;
} hu_audit_event_t;

/* ── Audit event API ───────────────────────────────────────────────── */

/** Create a new audit event with current timestamp and unique ID. */
void hu_audit_event_init(hu_audit_event_t *ev, hu_audit_event_type_t type);

/** Set actor on event (copies pointers only; strings must stay valid). */
void hu_audit_event_with_actor(hu_audit_event_t *ev, const char *channel, const char *user_id,
                               const char *username);

/** Set action on event. */
void hu_audit_event_with_action(hu_audit_event_t *ev, const char *command, const char *risk_level,
                                bool approved, bool allowed);

/** Set result on event. */
void hu_audit_event_with_result(hu_audit_event_t *ev, bool success, int32_t exit_code,
                                uint64_t duration_ms, const char *err_msg);

/** Set security context (sandbox_backend). */
void hu_audit_event_with_security(hu_audit_event_t *ev, const char *sandbox_backend);

/** Set identity (agent_id, model_version, auth_token_hash). */
void hu_audit_event_with_identity(hu_audit_event_t *ev, uint64_t agent_id,
                                  const char *model_version, const char *auth_token_hash);

/** Set input (trigger_type, trigger_source, prompt_hash, prompt_length). */
void hu_audit_event_with_input(hu_audit_event_t *ev, const char *trigger_type,
                               const char *trigger_source, const char *prompt_hash,
                               size_t prompt_length);

/** Set reasoning (decision, rule_name, confidence, context_tokens). */
void hu_audit_event_with_reasoning(hu_audit_event_t *ev, const char *decision,
                                   const char *rule_name, float confidence,
                                   uint32_t context_tokens);

/** Write JSON representation into buf. Returns bytes written, or 0 on failure. */
size_t hu_audit_event_write_json(const hu_audit_event_t *ev, char *buf, size_t buf_size);

/* ── Command execution log (convenience) ────────────────────────────── */

typedef struct hu_audit_cmd_log {
    const char *channel;
    const char *command;
    const char *risk_level;
    bool approved;
    bool allowed;
    bool success;
    uint64_t duration_ms;
} hu_audit_cmd_log_t;

/* ── Audit config ─────────────────────────────────────────────────── */

typedef struct hu_audit_config {
    bool enabled;
    char *log_path;
    uint32_t max_size_mb;
} hu_audit_config_t;

#define HU_AUDIT_CONFIG_DEFAULT  \
    {                            \
        .enabled = true,         \
        .log_path = "audit.log", \
        .max_size_mb = 10,       \
    }

/* ── Audit logger ──────────────────────────────────────────────────── */

typedef struct hu_audit_logger hu_audit_logger_t;

hu_audit_logger_t *hu_audit_logger_create(hu_allocator_t *alloc, const hu_audit_config_t *config,
                                          const char *base_dir);

void hu_audit_logger_destroy(hu_audit_logger_t *logger, hu_allocator_t *alloc);

/** Log an event. No-op if disabled. */
hu_error_t hu_audit_logger_log(hu_audit_logger_t *logger, const hu_audit_event_t *event);

/** Log a command execution event (convenience). */
hu_error_t hu_audit_logger_log_command(hu_audit_logger_t *logger, const hu_audit_cmd_log_t *entry);

/** Rotate audit HMAC key. Writes key_rotation entry, saves new key, clears old. */
hu_error_t hu_audit_rotate_key(hu_audit_logger_t *logger);

/** Set rotation interval in hours. 0 disables scheduled rotation. */
void hu_audit_set_rotation_interval(hu_audit_logger_t *logger, uint32_t hours);

#if defined(HU_IS_TEST) && HU_IS_TEST
/** Test-only: set last_rotation_time to force scheduled rotation on next log. */
void hu_audit_test_set_last_rotation_epoch(hu_audit_logger_t *logger, time_t epoch);
#endif

/** Filter: should this severity be logged? */
bool hu_audit_should_log(hu_audit_event_type_t type, hu_audit_severity_t min_sev);

/* ── HMAC chain verification ─────────────────────────────────────────────── */

/** Load audit HMAC key from base_dir/.audit_hmac_key. For verification. */
hu_error_t hu_audit_load_key(const char *base_dir, unsigned char key[32]);

/** Verify HMAC chain in audit log. Returns HU_ERR_CRYPTO_DECRYPT if tampering detected.
 * When key is NULL, loads key and key history from base_dir (derived from audit_file_path). */
hu_error_t hu_audit_verify_chain(const char *audit_file_path, const unsigned char *key);

#endif /* HU_AUDIT_H */
