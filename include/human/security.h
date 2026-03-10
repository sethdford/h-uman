#ifndef HU_SECURITY_H
#define HU_SECURITY_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct hu_sandbox hu_sandbox_t;
typedef struct hu_net_proxy hu_net_proxy_t;

/* ── Autonomy level ─────────────────────────────────────────────── */

typedef enum hu_autonomy_level {
    HU_AUTONOMY_LOCKED = 0,     /* No tool calls at all */
    HU_AUTONOMY_SUPERVISED = 1, /* Every tool needs approval */
    HU_AUTONOMY_ASSISTED = 2,   /* Low-risk auto, medium/high need approval */
    HU_AUTONOMY_AUTONOMOUS = 3, /* Policy engine decides */
} hu_autonomy_level_t;
/* Backward compat aliases */
#define HU_AUTONOMY_READ_ONLY HU_AUTONOMY_LOCKED
#define HU_AUTONOMY_FULL      HU_AUTONOMY_AUTONOMOUS

/* ── Command risk level ─────────────────────────────────────────── */

typedef enum hu_command_risk_level {
    HU_RISK_LOW,
    HU_RISK_MEDIUM,
    HU_RISK_HIGH
} hu_command_risk_level_t;

/* ── Rate tracker (for policy rate limiting) ─────────────────────── */

typedef struct hu_rate_tracker hu_rate_tracker_t;

hu_rate_tracker_t *hu_rate_tracker_create(hu_allocator_t *alloc, uint32_t max_actions);
void hu_rate_tracker_destroy(hu_rate_tracker_t *t);
bool hu_rate_tracker_record_action(hu_rate_tracker_t *t);
bool hu_rate_tracker_is_limited(hu_rate_tracker_t *t);
size_t hu_rate_tracker_count(hu_rate_tracker_t *t);
/** Remaining allowed actions before hitting the limit. */
uint32_t hu_rate_tracker_remaining(hu_rate_tracker_t *t);

/* ── Security policy ────────────────────────────────────────────── */

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
    bool pre_approved; /* set by agent before re-executing an approved tool */
} hu_security_policy_t;

bool hu_security_path_allowed(const hu_security_policy_t *policy, const char *path,
                              size_t path_len);
bool hu_security_shell_allowed(const hu_security_policy_t *policy);

hu_command_risk_level_t hu_policy_command_risk_level(const hu_security_policy_t *policy,
                                                     const char *command);

/** Tool-level risk classification (by tool name). Used for graduated autonomy. */
hu_command_risk_level_t hu_tool_risk_level(const char *tool_name);

/** Validate command execution. Returns risk level on success. */
hu_error_t hu_policy_validate_command(const hu_security_policy_t *policy, const char *command,
                                      bool approved, hu_command_risk_level_t *out_risk);

bool hu_policy_is_command_allowed(const hu_security_policy_t *policy, const char *command);

bool hu_policy_can_act(const hu_security_policy_t *policy);

/** Record an action. Returns true if allowed, false if rate-limited. */
bool hu_policy_record_action(hu_security_policy_t *policy);

bool hu_policy_is_rate_limited(const hu_security_policy_t *policy);

/* ── Pairing guard ──────────────────────────────────────────────── */

typedef struct hu_pairing_guard hu_pairing_guard_t;

typedef enum hu_pair_attempt_result {
    HU_PAIR_PAIRED, /* Success, token returned via out_token (caller must free) */
    HU_PAIR_MISSING_CODE,
    HU_PAIR_INVALID_CODE,
    HU_PAIR_ALREADY_PAIRED,
    HU_PAIR_DISABLED,
    HU_PAIR_LOCKED_OUT,
    HU_PAIR_INTERNAL_ERROR
} hu_pair_attempt_result_t;

hu_pairing_guard_t *hu_pairing_guard_create(hu_allocator_t *alloc, bool require_pairing,
                                            const char **existing_tokens, size_t tokens_len);

void hu_pairing_guard_destroy(hu_pairing_guard_t *guard);

/** One-time pairing code (6 digits). Returns NULL if not applicable. */
const char *hu_pairing_guard_pairing_code(hu_pairing_guard_t *guard);

/** Attempt to pair with code. On success, out_token is set (caller frees). */
hu_pair_attempt_result_t hu_pairing_guard_attempt_pair(hu_pairing_guard_t *guard, const char *code,
                                                       char **out_token);

bool hu_pairing_guard_is_authenticated(const hu_pairing_guard_t *guard, const char *token);

bool hu_pairing_guard_is_paired(const hu_pairing_guard_t *guard);

bool hu_pairing_guard_constant_time_eq(const char *a, const char *b);

/* ── Secret store ───────────────────────────────────────────────── */

typedef struct hu_secret_store hu_secret_store_t;

hu_secret_store_t *hu_secret_store_create(hu_allocator_t *alloc, const char *config_dir,
                                          bool enabled);
void hu_secret_store_destroy(hu_secret_store_t *store, hu_allocator_t *alloc);

/** Encrypt plaintext. Returns hex-encoded ciphertext with "enc2:" prefix. */
hu_error_t hu_secret_store_encrypt(hu_secret_store_t *store, hu_allocator_t *alloc,
                                   const char *plaintext, char **out_ciphertext_hex);

/** Decrypt ciphertext. Handles "enc2:" prefix or passthrough for plaintext. */
hu_error_t hu_secret_store_decrypt(hu_secret_store_t *store, hu_allocator_t *alloc,
                                   const char *value, char **out_plaintext);

bool hu_secret_store_is_encrypted(const char *value);

/* Hex encode/decode helpers (exposed for tests).
   out_hex must be at least len*2+1 bytes. Output is null-terminated. */
void hu_hex_encode(const uint8_t *data, size_t len, char *out_hex);
hu_error_t hu_hex_decode(const char *hex, size_t hex_len, uint8_t *out_data, size_t out_cap,
                         size_t *out_len);

#endif /* HU_SECURITY_H */
