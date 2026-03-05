#ifndef SC_AUDIT_H
#define SC_AUDIT_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* ── Audit event types ─────────────────────────────────────────────── */

typedef enum sc_audit_event_type {
    SC_AUDIT_COMMAND_EXECUTION,
    SC_AUDIT_FILE_ACCESS,
    SC_AUDIT_CONFIG_CHANGE,
    SC_AUDIT_AUTH_SUCCESS,
    SC_AUDIT_AUTH_FAILURE,
    SC_AUDIT_POLICY_VIOLATION,
    SC_AUDIT_SECURITY_EVENT,
} sc_audit_event_type_t;

const char *sc_audit_event_type_string(sc_audit_event_type_t t);

/* ── Severity (for filtering) ───────────────────────────────────────── */

typedef enum sc_audit_severity {
    SC_AUDIT_SEV_LOW,
    SC_AUDIT_SEV_MEDIUM,
    SC_AUDIT_SEV_HIGH,
} sc_audit_severity_t;

sc_audit_severity_t sc_audit_event_severity(sc_audit_event_type_t t);

/* ── Audit event structures ─────────────────────────────────────────── */

typedef struct sc_audit_actor {
    const char *channel;
    const char *user_id;  /* may be NULL */
    const char *username; /* may be NULL */
} sc_audit_actor_t;

typedef struct sc_audit_action {
    const char *command;    /* may be NULL */
    const char *risk_level; /* may be NULL */
    bool approved;
    bool allowed;
} sc_audit_action_t;

typedef struct sc_audit_result {
    bool success;
    int32_t exit_code;    /* -1 if not set */
    uint64_t duration_ms; /* 0 if not set */
    const char *err_msg;  /* may be NULL */
} sc_audit_result_t;

typedef struct sc_audit_security_ctx {
    bool policy_violation;
    uint32_t rate_limit_remaining; /* 0 means not set */
    const char *sandbox_backend;   /* may be NULL */
} sc_audit_security_ctx_t;

typedef struct sc_audit_event {
    int64_t timestamp_s;
    uint64_t event_id;
    sc_audit_event_type_t event_type;
    sc_audit_actor_t actor;   /* channel/username set to NULL if not used */
    sc_audit_action_t action; /* command set to NULL if not used */
    sc_audit_result_t result; /* exit_code -1, duration_ms 0, err_msg NULL if not set */
    sc_audit_security_ctx_t security;
} sc_audit_event_t;

/* ── Audit event API ───────────────────────────────────────────────── */

/** Create a new audit event with current timestamp and unique ID. */
void sc_audit_event_init(sc_audit_event_t *ev, sc_audit_event_type_t type);

/** Set actor on event (copies pointers only; strings must stay valid). */
void sc_audit_event_with_actor(sc_audit_event_t *ev, const char *channel, const char *user_id,
                               const char *username);

/** Set action on event. */
void sc_audit_event_with_action(sc_audit_event_t *ev, const char *command, const char *risk_level,
                                bool approved, bool allowed);

/** Set result on event. */
void sc_audit_event_with_result(sc_audit_event_t *ev, bool success, int32_t exit_code,
                                uint64_t duration_ms, const char *err_msg);

/** Set security context (sandbox_backend). */
void sc_audit_event_with_security(sc_audit_event_t *ev, const char *sandbox_backend);

/** Write JSON representation into buf. Returns bytes written, or 0 on failure. */
size_t sc_audit_event_write_json(const sc_audit_event_t *ev, char *buf, size_t buf_size);

/* ── Command execution log (convenience) ────────────────────────────── */

typedef struct sc_audit_cmd_log {
    const char *channel;
    const char *command;
    const char *risk_level;
    bool approved;
    bool allowed;
    bool success;
    uint64_t duration_ms;
} sc_audit_cmd_log_t;

/* ── Audit config ─────────────────────────────────────────────────── */

typedef struct sc_audit_config {
    bool enabled;
    char *log_path;
    uint32_t max_size_mb;
} sc_audit_config_t;

#define SC_AUDIT_CONFIG_DEFAULT  \
    {                            \
        .enabled = true,         \
        .log_path = "audit.log", \
        .max_size_mb = 10,       \
    }

/* ── Audit logger ──────────────────────────────────────────────────── */

typedef struct sc_audit_logger sc_audit_logger_t;

sc_audit_logger_t *sc_audit_logger_create(sc_allocator_t *alloc, const sc_audit_config_t *config,
                                          const char *base_dir);

void sc_audit_logger_destroy(sc_audit_logger_t *logger, sc_allocator_t *alloc);

/** Log an event. No-op if disabled. */
sc_error_t sc_audit_logger_log(sc_audit_logger_t *logger, const sc_audit_event_t *event);

/** Log a command execution event (convenience). */
sc_error_t sc_audit_logger_log_command(sc_audit_logger_t *logger, const sc_audit_cmd_log_t *entry);

/** Rotate audit HMAC key. Writes key_rotation entry, saves new key, clears old. */
sc_error_t sc_audit_rotate_key(sc_audit_logger_t *logger);

/** Set rotation interval in hours. 0 disables scheduled rotation. */
void sc_audit_set_rotation_interval(sc_audit_logger_t *logger, uint32_t hours);

#if defined(SC_IS_TEST) && SC_IS_TEST
/** Test-only: set last_rotation_time to force scheduled rotation on next log. */
void sc_audit_test_set_last_rotation_epoch(sc_audit_logger_t *logger, time_t epoch);
#endif

/** Filter: should this severity be logged? */
bool sc_audit_should_log(sc_audit_event_type_t type, sc_audit_severity_t min_sev);

/* ── HMAC chain verification ─────────────────────────────────────────────── */

/** Load audit HMAC key from base_dir/.audit_hmac_key. For verification. */
sc_error_t sc_audit_load_key(const char *base_dir, unsigned char key[32]);

/** Verify HMAC chain in audit log. Returns SC_ERR_CRYPTO_DECRYPT if tampering detected.
 * When key is NULL, loads key and key history from base_dir (derived from audit_file_path). */
sc_error_t sc_audit_verify_chain(const char *audit_file_path, const unsigned char *key);

#endif /* SC_AUDIT_H */
