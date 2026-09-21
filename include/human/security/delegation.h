#ifndef HU_DELEGATION_H
#define HU_DELEGATION_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* ─────────────────────────────────────────────────────────────────────────
 * Delegation Token Registry
 *
 * Enables agent-to-agent delegation with capability attenuation.
 * Supports delegation chains, caveat verification, and revocation cascades.
 * ───────────────────────────────────────────────────────────────────────── */

typedef struct hu_delegation_caveat {
    char *key; /* "tool", "path", "ttl", "max_cost", etc. */
    size_t key_len;
    char *value; /* specific constraint value */
    size_t value_len;
} hu_delegation_caveat_t;

typedef struct hu_delegation_token {
    char token_id[64];        /* unique token identifier */
    char issuer_agent_id[64]; /* agent that issued this token */
    char target_agent_id[64]; /* agent that can use this token */
    hu_delegation_caveat_t *caveats;
    size_t caveat_count;
    int64_t issued_at;        /* Unix timestamp */
    int64_t expires_at;       /* Unix timestamp; 0 = no expiry */
    bool revoked;             /* manually revoked */
    char parent_token_id[64]; /* empty string if root token */
} hu_delegation_token_t;

typedef struct hu_delegation_registry hu_delegation_registry_t;

/* ─────────────────────────────────────────────────────────────────────────
 * Registry lifecycle
 * ───────────────────────────────────────────────────────────────────────── */

/**
 * Create a new delegation token registry.
 * Max 256 tokens per registry.
 */
hu_delegation_registry_t *hu_delegation_registry_create(hu_allocator_t *alloc);

/**
 * Destroy registry and all owned tokens.
 */
void hu_delegation_registry_destroy(hu_delegation_registry_t *reg);

/* ─────────────────────────────────────────────────────────────────────────
 * Token issuance
 * ───────────────────────────────────────────────────────────────────────── */

/**
 * Issue a new delegation token from issuer to target.
 *
 * issuer_id:    agent spawning the delegation
 * target_id:    agent receiving the delegation
 * ttl_seconds:  time-to-live (0 = no expiry)
 * caveats:      constraints on the token (optional)
 * caveat_count: number of caveats
 *
 * Returns token_id on success, NULL on failure (alloc error, registry full).
 */
const char *hu_delegation_issue(hu_delegation_registry_t *reg, hu_allocator_t *alloc,
                                const char *issuer_id, const char *target_id, uint32_t ttl_seconds,
                                const hu_delegation_caveat_t *caveats, size_t caveat_count);

/* ─────────────────────────────────────────────────────────────────────────
 * Token attenuation (capability delegation with narrowing)
 * ───────────────────────────────────────────────────────────────────────── */

/**
 * Derive a new token from an existing token with additional restrictions.
 * Only allows adding caveats, never removing them.
 * Can only be called by the current token holder (target_id).
 *
 * parent_token_id: token to derive from
 * new_issuer_id:   agent narrowing the token
 * new_target_id:   agent receiving the attenuated token
 * additional_caveats: new caveats to add to parent's caveats
 * additional_caveat_count: count of new caveats
 *
 * Returns new token_id on success, NULL on failure.
 */
const char *hu_delegation_attenuate(hu_delegation_registry_t *reg, hu_allocator_t *alloc,
                                    const char *parent_token_id, const char *new_issuer_id,
                                    const char *new_target_id,
                                    const hu_delegation_caveat_t *additional_caveats,
                                    size_t additional_caveat_count);

/* ─────────────────────────────────────────────────────────────────────────
 * Verification
 * ───────────────────────────────────────────────────────────────────────── */

/**
 * Check if a token authorizes an action on a resource.
 *
 * token_id:    the delegation token
 * agent_id:    agent attempting to use the token
 * tool_name:   tool/action being requested
 * resource:    path or resource being accessed (optional, checked against path caveats)
 * cost_usd:    estimated cost of operation (checked against max_cost caveat)
 *
 * Returns HU_OK if authorized, HU_ERR_UNAUTHORIZED if not, other on error.
 */
hu_error_t hu_delegation_verify(hu_delegation_registry_t *reg, const char *token_id,
                                const char *agent_id, const char *tool_name, const char *resource,
                                double cost_usd);

/* ─────────────────────────────────────────────────────────────────────────
 * Revocation
 * ───────────────────────────────────────────────────────────────────────── */

/**
 * Revoke a token and all tokens derived from it.
 * Cascade delete: marks parent as revoked, which invalidates all children.
 *
 * token_id: the token to revoke
 *
 * Returns HU_OK if revoked, HU_ERR_NOT_FOUND if token doesn't exist.
 */
hu_error_t hu_delegation_revoke(hu_delegation_registry_t *reg, const char *token_id);

/* ─────────────────────────────────────────────────────────────────────────
 * Chain traversal
 * ───────────────────────────────────────────────────────────────────────── */

/**
 * Get the full delegation chain from a token.
 * Returns array of token_ids: root -> ... -> token_id
 *
 * token_id: starting token
 * out:      output array (caller frees with alloc->free)
 * out_count: length of output array
 *
 * Returns HU_OK on success, error otherwise.
 */
hu_error_t hu_delegation_chain(hu_delegation_registry_t *reg, hu_allocator_t *alloc,
                               const char *token_id, const char ***out, size_t *out_count);

/* ─────────────────────────────────────────────────────────────────────────
 * Query
 * ───────────────────────────────────────────────────────────────────────── */

/**
 * Get token by ID. Returns NULL if not found.
 */
const hu_delegation_token_t *hu_delegation_get_token(hu_delegation_registry_t *reg,
                                                     const char *token_id);

/**
 * Get current token count.
 */
size_t hu_delegation_token_count(hu_delegation_registry_t *reg);

#endif /* HU_DELEGATION_H */
