---
title: Security API
description: Security policy, sandbox, pairing guard, secret store, and audit logging
updated: 2026-03-02
---

# Security API

Security policy, sandbox, pairing guard, secret store, and audit logging.

## Policy (`security.h`)

```c
typedef enum sc_autonomy_level {
    SC_AUTONOMY_READ_ONLY,
    SC_AUTONOMY_SUPERVISED,
    SC_AUTONOMY_FULL
} sc_autonomy_level_t;

typedef enum sc_command_risk_level {
    SC_RISK_LOW,
    SC_RISK_MEDIUM,
    SC_RISK_HIGH
} sc_command_risk_level_t;

typedef struct sc_security_policy {
    sc_autonomy_level_t autonomy;
    const char *workspace_dir;
    bool workspace_only;
    const char **allowed_commands;
    size_t allowed_commands_len;
    uint32_t max_actions_per_hour;
    bool require_approval_for_medium_risk;
    bool block_high_risk_commands;
    sc_rate_tracker_t *tracker;
    bool allow_shell;
    const char *const *allowed_paths;
    size_t allowed_paths_count;
    sc_sandbox_t *sandbox;
    sc_net_proxy_t *net_proxy;
    bool pre_approved;
} sc_security_policy_t;

bool sc_security_path_allowed(const sc_security_policy_t *policy,
    const char *path, size_t path_len);
bool sc_security_shell_allowed(const sc_security_policy_t *policy);
sc_command_risk_level_t sc_policy_command_risk_level(const sc_security_policy_t *policy,
    const char *command);

sc_error_t sc_policy_validate_command(const sc_security_policy_t *policy,
    const char *command, bool approved,
    sc_command_risk_level_t *out_risk);
bool sc_policy_is_command_allowed(const sc_security_policy_t *policy);
bool sc_policy_can_act(const sc_security_policy_t *policy);
bool sc_policy_record_action(sc_security_policy_t *policy);
bool sc_policy_is_rate_limited(const sc_security_policy_t *policy);
```

## Rate Tracker

```c
sc_rate_tracker_t *sc_rate_tracker_create(sc_allocator_t *alloc, uint32_t max_actions);
void sc_rate_tracker_destroy(sc_rate_tracker_t *t);
bool sc_rate_tracker_record_action(sc_rate_tracker_t *t);
bool sc_rate_tracker_is_limited(sc_rate_tracker_t *t);
uint32_t sc_rate_tracker_remaining(sc_rate_tracker_t *t);
```

## Pairing Guard

```c
typedef enum sc_pair_attempt_result {
    SC_PAIR_PAIRED,
    SC_PAIR_MISSING_CODE,
    SC_PAIR_INVALID_CODE,
    SC_PAIR_ALREADY_PAIRED,
    SC_PAIR_DISABLED,
    SC_PAIR_LOCKED_OUT,
    SC_PAIR_INTERNAL_ERROR
} sc_pair_attempt_result_t;

sc_pairing_guard_t *sc_pairing_guard_create(sc_allocator_t *alloc,
    bool require_pairing,
    const char **existing_tokens,
    size_t tokens_len);
void sc_pairing_guard_destroy(sc_pairing_guard_t *guard);

const char *sc_pairing_guard_pairing_code(sc_pairing_guard_t *guard);
sc_pair_attempt_result_t sc_pairing_guard_attempt_pair(sc_pairing_guard_t *guard,
    const char *code, char **out_token);
bool sc_pairing_guard_is_authenticated(const sc_pairing_guard_t *guard,
    const char *token);
bool sc_pairing_guard_is_paired(const sc_pairing_guard_t *guard);
bool sc_pairing_guard_constant_time_eq(const char *a, const char *b);
```

## Secret Store

```c
sc_secret_store_t *sc_secret_store_create(sc_allocator_t *alloc,
    const char *config_dir, bool enabled);
void sc_secret_store_destroy(sc_secret_store_t *store, sc_allocator_t *alloc);

sc_error_t sc_secret_store_encrypt(sc_secret_store_t *store,
    sc_allocator_t *alloc,
    const char *plaintext,
    char **out_ciphertext_hex);

sc_error_t sc_secret_store_decrypt(sc_secret_store_t *store,
    sc_allocator_t *alloc,
    const char *value,
    char **out_plaintext);

bool sc_secret_store_is_encrypted(const char *value);
```

## Audit (`security/audit.h`)

```c
typedef enum sc_audit_event_type {
    SC_AUDIT_COMMAND_EXECUTION,
    SC_AUDIT_FILE_ACCESS,
    SC_AUDIT_CONFIG_CHANGE,
    SC_AUDIT_AUTH_SUCCESS,
    SC_AUDIT_AUTH_FAILURE,
    SC_AUDIT_POLICY_VIOLATION,
    SC_AUDIT_SECURITY_EVENT,
} sc_audit_event_type_t;

typedef struct sc_audit_event {
    int64_t timestamp_s;
    uint64_t event_id;
    sc_audit_event_type_t event_type;
    sc_audit_actor_t actor;
    sc_audit_action_t action;
    sc_audit_result_t result;
    sc_audit_security_ctx_t security;
} sc_audit_event_t;

sc_audit_logger_t *sc_audit_logger_create(sc_allocator_t *alloc,
    const sc_audit_config_t *config, const char *base_dir);
void sc_audit_logger_destroy(sc_audit_logger_t *logger, sc_allocator_t *alloc);

sc_error_t sc_audit_logger_log(sc_audit_logger_t *logger,
    const sc_audit_event_t *event);
sc_error_t sc_audit_logger_log_command(sc_audit_logger_t *logger,
    const sc_audit_cmd_log_t *entry);
```
