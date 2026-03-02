#ifndef SC_SECURITY_H
#define SC_SECURITY_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct sc_sandbox sc_sandbox_t;
typedef struct sc_net_proxy sc_net_proxy_t;

/* ── Autonomy level ─────────────────────────────────────────────── */

typedef enum sc_autonomy_level {
    SC_AUTONOMY_READ_ONLY,
    SC_AUTONOMY_SUPERVISED,
    SC_AUTONOMY_FULL
} sc_autonomy_level_t;

/* ── Command risk level ─────────────────────────────────────────── */

typedef enum sc_command_risk_level {
    SC_RISK_LOW,
    SC_RISK_MEDIUM,
    SC_RISK_HIGH
} sc_command_risk_level_t;

/* ── Rate tracker (for policy rate limiting) ─────────────────────── */

typedef struct sc_rate_tracker sc_rate_tracker_t;

sc_rate_tracker_t *sc_rate_tracker_create(sc_allocator_t *alloc, uint32_t max_actions);
void sc_rate_tracker_destroy(sc_rate_tracker_t *t);
bool sc_rate_tracker_record_action(sc_rate_tracker_t *t);
bool sc_rate_tracker_is_limited(sc_rate_tracker_t *t);
size_t sc_rate_tracker_count(sc_rate_tracker_t *t);
/** Remaining allowed actions before hitting the limit. */
uint32_t sc_rate_tracker_remaining(sc_rate_tracker_t *t);

/* ── Security policy ────────────────────────────────────────────── */

#ifndef SC_SANDBOX_H
typedef struct sc_sandbox sc_sandbox_t;
#endif

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
    bool pre_approved;          /* set by agent before re-executing an approved tool */
} sc_security_policy_t;

bool sc_security_path_allowed(const sc_security_policy_t *policy,
    const char *path, size_t path_len);
bool sc_security_shell_allowed(const sc_security_policy_t *policy);

sc_command_risk_level_t sc_policy_command_risk_level(const sc_security_policy_t *policy,
                                                      const char *command);

/** Validate command execution. Returns risk level on success. */
sc_error_t sc_policy_validate_command(const sc_security_policy_t *policy,
                                      const char *command, bool approved,
                                      sc_command_risk_level_t *out_risk);

bool sc_policy_is_command_allowed(const sc_security_policy_t *policy,
                                  const char *command);

bool sc_policy_can_act(const sc_security_policy_t *policy);

/** Record an action. Returns true if allowed, false if rate-limited. */
bool sc_policy_record_action(sc_security_policy_t *policy);

bool sc_policy_is_rate_limited(const sc_security_policy_t *policy);

/* ── Pairing guard ──────────────────────────────────────────────── */

typedef struct sc_pairing_guard sc_pairing_guard_t;

typedef enum sc_pair_attempt_result {
    SC_PAIR_PAIRED,        /* Success, token returned via out_token (caller must free) */
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

/** One-time pairing code (6 digits). Returns NULL if not applicable. */
const char *sc_pairing_guard_pairing_code(sc_pairing_guard_t *guard);

/** Attempt to pair with code. On success, out_token is set (caller frees). */
sc_pair_attempt_result_t sc_pairing_guard_attempt_pair(sc_pairing_guard_t *guard,
                                                       const char *code,
                                                       char **out_token);

bool sc_pairing_guard_is_authenticated(const sc_pairing_guard_t *guard,
                                       const char *token);

bool sc_pairing_guard_is_paired(const sc_pairing_guard_t *guard);

bool sc_pairing_guard_constant_time_eq(const char *a, const char *b);

/* ── Secret store ───────────────────────────────────────────────── */

typedef struct sc_secret_store sc_secret_store_t;

sc_secret_store_t *sc_secret_store_create(sc_allocator_t *alloc, const char *config_dir, bool enabled);
void sc_secret_store_destroy(sc_secret_store_t *store, sc_allocator_t *alloc);

/** Encrypt plaintext. Returns hex-encoded ciphertext with "enc2:" prefix. */
sc_error_t sc_secret_store_encrypt(sc_secret_store_t *store,
                                   sc_allocator_t *alloc,
                                   const char *plaintext,
                                   char **out_ciphertext_hex);

/** Decrypt ciphertext. Handles "enc2:" prefix or passthrough for plaintext. */
sc_error_t sc_secret_store_decrypt(sc_secret_store_t *store,
                                   sc_allocator_t *alloc,
                                   const char *value,
                                   char **out_plaintext);

bool sc_secret_store_is_encrypted(const char *value);

/* Hex encode/decode helpers (exposed for tests) */
void sc_hex_encode(const uint8_t *data, size_t len, char *out_hex);
sc_error_t sc_hex_decode(const char *hex, size_t hex_len,
                         uint8_t *out_data, size_t out_cap, size_t *out_len);

#endif /* SC_SECURITY_H */
