#ifndef HU_SKILL_TRUST_H
#define HU_SKILL_TRUST_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum hu_skill_sandbox_tier {
    HU_SKILL_SANDBOX_NONE = 0,
    HU_SKILL_SANDBOX_BASIC,
    HU_SKILL_SANDBOX_STRICT
} hu_skill_sandbox_tier_t;

typedef struct hu_publisher_key {
    char *name;
    char *public_key_hex;
} hu_publisher_key_t;

typedef struct hu_skill_trust_config {
    bool require_signature;
    hu_skill_sandbox_tier_t default_sandbox;
    hu_publisher_key_t *trusted_publishers;
    size_t trusted_publishers_count;
} hu_skill_trust_config_t;

typedef struct hu_skill_audit_entry {
    char *skill_name;
    char *args_hash;
    double execution_time_ms;
    int exit_code;
    bool allowed;
} hu_skill_audit_entry_t;

/* Verify Ed25519 signature of manifest JSON against known publishers.
 * Under HU_IS_TEST: returns HU_OK if publisher name matches any trusted publisher. */
hu_error_t hu_skill_trust_verify_signature(const hu_skill_trust_config_t *cfg,
                                           const char *publisher_name, const char *manifest_json,
                                           size_t manifest_json_len, const char *signature_hex,
                                           size_t signature_hex_len);

/* Inspect a shell command for dangerous patterns. Returns HU_OK if safe,
 * HU_ERR_SECURITY_COMMAND_NOT_ALLOWED if dangerous. */
hu_error_t hu_skill_trust_inspect_command(const char *command, size_t command_len);

/* Get the policy name for a sandbox tier. Returns static string. */
const char *hu_skill_trust_get_policy(hu_skill_sandbox_tier_t tier);

/* Record a skill execution to the audit trail.
 * Under HU_IS_TEST: no-op. Non-test: appends JSON line to ~/.human/skill_audit.log */
hu_error_t hu_skill_trust_audit_record(hu_allocator_t *alloc, const hu_skill_audit_entry_t *entry);

#endif
