---
title: Security API
description: Security policy, sandbox, pairing guard, secret store, and audit logging
updated: 2026-03-02
---

# Security API

Security policy, sandbox, pairing guard, secret store, and audit logging.

## Policy (`security.h`)

```c
typedef enum hu_autonomy_level {
    HU_AUTONOMY_READ_ONLY,
    HU_AUTONOMY_SUPERVISED,
    HU_AUTONOMY_FULL
} hu_autonomy_level_t;

typedef enum hu_command_risk_level {
    HU_RISK_LOW,
    HU_RISK_MEDIUM,
    HU_RISK_HIGH
} hu_command_risk_level_t;

typedef struct hu_security_policy {
    hu_autonomy_level_t autonomy;
    const char *workspace_dir;
    bool workspace_only;
    const char **allowed_commands;
    size_t allowed_commands_len;
    uint32_t max_actions_per_hour;
    bool require_approval_for_medium_risk;
    bool block_high_risk_commands;
    hu_rate_tracker_t *tracker;
    bool allow_shell;
    const char *const *allowed_paths;
    size_t allowed_paths_count;
    hu_sandbox_t *sandbox;
    hu_net_proxy_t *net_proxy;
    bool pre_approved;
} hu_security_policy_t;

bool hu_security_path_allowed(const hu_security_policy_t *policy,
    const char *path, size_t path_len);
bool hu_security_shell_allowed(const hu_security_policy_t *policy);
hu_command_risk_level_t hu_policy_command_risk_level(const hu_security_policy_t *policy,
    const char *command);

hu_error_t hu_policy_validate_command(const hu_security_policy_t *policy,
    const char *command, bool approved,
    hu_command_risk_level_t *out_risk);
bool hu_policy_is_command_allowed(const hu_security_policy_t *policy);
bool hu_policy_can_act(const hu_security_policy_t *policy);
bool hu_policy_record_action(hu_security_policy_t *policy);
bool hu_policy_is_rate_limited(const hu_security_policy_t *policy);
```

## Rate Tracker

```c
hu_rate_tracker_t *hu_rate_tracker_create(hu_allocator_t *alloc, uint32_t max_actions);
void hu_rate_tracker_destroy(hu_rate_tracker_t *t);
bool hu_rate_tracker_record_action(hu_rate_tracker_t *t);
bool hu_rate_tracker_is_limited(hu_rate_tracker_t *t);
uint32_t hu_rate_tracker_remaining(hu_rate_tracker_t *t);
```

## Pairing Guard

```c
typedef enum hu_pair_attempt_result {
    HU_PAIR_PAIRED,
    HU_PAIR_MISSING_CODE,
    HU_PAIR_INVALID_CODE,
    HU_PAIR_ALREADY_PAIRED,
    HU_PAIR_DISABLED,
    HU_PAIR_LOCKED_OUT,
    HU_PAIR_INTERNAL_ERROR
} hu_pair_attempt_result_t;

hu_pairing_guard_t *hu_pairing_guard_create(hu_allocator_t *alloc,
    bool require_pairing,
    const char **existing_tokens,
    size_t tokens_len);
void hu_pairing_guard_destroy(hu_pairing_guard_t *guard);

const char *hu_pairing_guard_pairing_code(hu_pairing_guard_t *guard);
hu_pair_attempt_result_t hu_pairing_guard_attempt_pair(hu_pairing_guard_t *guard,
    const char *code, char **out_token);
bool hu_pairing_guard_is_authenticated(const hu_pairing_guard_t *guard,
    const char *token);
bool hu_pairing_guard_is_paired(const hu_pairing_guard_t *guard);
bool hu_pairing_guard_constant_time_eq(const char *a, const char *b);
```

## Secret Store

```c
hu_secret_store_t *hu_secret_store_create(hu_allocator_t *alloc,
    const char *config_dir, bool enabled);
void hu_secret_store_destroy(hu_secret_store_t *store, hu_allocator_t *alloc);

hu_error_t hu_secret_store_encrypt(hu_secret_store_t *store,
    hu_allocator_t *alloc,
    const char *plaintext,
    char **out_ciphertext_hex);

hu_error_t hu_secret_store_decrypt(hu_secret_store_t *store,
    hu_allocator_t *alloc,
    const char *value,
    char **out_plaintext);

bool hu_secret_store_is_encrypted(const char *value);
```

## Audit (`security/audit.h`)

```c
typedef enum hu_audit_event_type {
    HU_AUDIT_COMMAND_EXECUTION,
    HU_AUDIT_FILE_ACCESS,
    HU_AUDIT_CONFIG_CHANGE,
    HU_AUDIT_AUTH_SUCCESS,
    HU_AUDIT_AUTH_FAILURE,
    HU_AUDIT_POLICY_VIOLATION,
    HU_AUDIT_SECURITY_EVENT,
} hu_audit_event_type_t;

typedef struct hu_audit_event {
    int64_t timestamp_s;
    uint64_t event_id;
    hu_audit_event_type_t event_type;
    hu_audit_actor_t actor;
    hu_audit_action_t action;
    hu_audit_result_t result;
    hu_audit_security_ctx_t security;
} hu_audit_event_t;

hu_audit_logger_t *hu_audit_logger_create(hu_allocator_t *alloc,
    const hu_audit_config_t *config, const char *base_dir);
void hu_audit_logger_destroy(hu_audit_logger_t *logger, hu_allocator_t *alloc);

hu_error_t hu_audit_logger_log(hu_audit_logger_t *logger,
    const hu_audit_event_t *event);
hu_error_t hu_audit_logger_log_command(hu_audit_logger_t *logger,
    const hu_audit_cmd_log_t *entry);
```
