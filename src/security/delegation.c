#include "human/security/delegation.h"
#include "human/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HU_DELEGATION_MAX_TOKENS  256
#define HU_DELEGATION_MAX_CAVEATS 16

struct hu_delegation_registry {
    hu_allocator_t *alloc;
    hu_delegation_token_t *tokens;
    size_t token_count;
    size_t token_cap;
    uint32_t token_counter; /* for generating unique IDs */
};

/* ─────────────────────────────────────────────────────────────────────────
 * Utility functions
 * ───────────────────────────────────────────────────────────────────────── */

static void free_caveats(hu_allocator_t *alloc, hu_delegation_caveat_t *caveats, size_t count) {
    if (!alloc || !caveats)
        return;
    for (size_t i = 0; i < count; i++) {
        if (caveats[i].key) {
            alloc->free(alloc->ctx, caveats[i].key, caveats[i].key_len + 1);
        }
        if (caveats[i].value) {
            alloc->free(alloc->ctx, caveats[i].value, caveats[i].value_len + 1);
        }
    }
    alloc->free(alloc->ctx, caveats, count * sizeof(hu_delegation_caveat_t));
}

static hu_error_t copy_caveats(hu_allocator_t *alloc, const hu_delegation_caveat_t *src,
                               size_t src_count, hu_delegation_caveat_t **out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;

    if (!src || src_count == 0) {
        *out = NULL;
        return HU_OK;
    }

    hu_delegation_caveat_t *caveats =
        (hu_delegation_caveat_t *)alloc->alloc(alloc->ctx, src_count * sizeof(*caveats));
    if (!caveats)
        return HU_ERR_OUT_OF_MEMORY;

    for (size_t i = 0; i < src_count; i++) {
        caveats[i].key_len = src[i].key_len;
        caveats[i].key = (char *)alloc->alloc(alloc->ctx, src[i].key_len + 1);
        if (!caveats[i].key) {
            free_caveats(alloc, caveats, i);
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(caveats[i].key, src[i].key, src[i].key_len);
        caveats[i].key[src[i].key_len] = '\0';

        caveats[i].value_len = src[i].value_len;
        caveats[i].value = (char *)alloc->alloc(alloc->ctx, src[i].value_len + 1);
        if (!caveats[i].value) {
            free_caveats(alloc, caveats, i + 1);
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(caveats[i].value, src[i].value, src[i].value_len);
        caveats[i].value[src[i].value_len] = '\0';
    }

    *out = caveats;
    return HU_OK;
}

static void generate_token_id(char token_id[64], uint32_t counter) {
    time_t now = time(NULL);
    snprintf(token_id, 64, "tok_%ld_%u", (long)now, counter);
}

static bool is_revoked_recursive(hu_delegation_registry_t *reg, const char *parent_id) {
    if (!parent_id || strlen(parent_id) == 0)
        return false;

    const hu_delegation_token_t *tok = hu_delegation_get_token(reg, parent_id);
    if (!tok)
        return false;
    if (tok->revoked)
        return true;
    return is_revoked_recursive(reg, tok->parent_token_id);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Path glob matching (simple implementation)
 * ───────────────────────────────────────────────────────────────────────── */

static bool path_matches_glob(const char *path, const char *pattern) {
    if (!pattern || strlen(pattern) == 0)
        return true; /* no constraint = allow all */
    if (!path)
        return false;

    /* Simple glob: * matches any substring, ? matches single char */
    const char *p = pattern;
    const char *t = path;

    while (*p) {
        if (*p == '*') {
            /* Skip consecutive * */
            while (*p == '*')
                p++;
            if (!*p)
                return true; /* trailing * matches rest */
            /* Find next match */
            while (*t) {
                if (path_matches_glob(t, p))
                    return true;
                t++;
            }
            return false;
        } else if (*p == '?') {
            if (!*t)
                return false;
            p++;
            t++;
        } else {
            if (*p != *t)
                return false;
            p++;
            t++;
        }
    }
    return *t == '\0'; /* both must be consumed */
}

/* ─────────────────────────────────────────────────────────────────────────
 * Registry lifecycle
 * ───────────────────────────────────────────────────────────────────────── */

hu_delegation_registry_t *hu_delegation_registry_create(hu_allocator_t *alloc) {
    if (!alloc)
        return NULL;

    hu_delegation_registry_t *reg =
        (hu_delegation_registry_t *)alloc->alloc(alloc->ctx, sizeof(*reg));
    if (!reg)
        return NULL;

    reg->alloc = alloc;
    reg->token_cap = HU_DELEGATION_MAX_TOKENS;
    reg->tokens = (hu_delegation_token_t *)alloc->alloc(
        alloc->ctx, reg->token_cap * sizeof(hu_delegation_token_t));
    if (!reg->tokens) {
        alloc->free(alloc->ctx, reg, sizeof(*reg));
        return NULL;
    }

    reg->token_count = 0;
    reg->token_counter = 0;
    return reg;
}

void hu_delegation_registry_destroy(hu_delegation_registry_t *reg) {
    if (!reg || !reg->alloc)
        return;

    for (size_t i = 0; i < reg->token_count; i++) {
        free_caveats(reg->alloc, reg->tokens[i].caveats, reg->tokens[i].caveat_count);
    }

    if (reg->tokens) {
        reg->alloc->free(reg->alloc->ctx, reg->tokens,
                         reg->token_cap * sizeof(hu_delegation_token_t));
    }
    reg->alloc->free(reg->alloc->ctx, reg, sizeof(*reg));
}

/* ─────────────────────────────────────────────────────────────────────────
 * Token issuance
 * ───────────────────────────────────────────────────────────────────────── */

const char *hu_delegation_issue(hu_delegation_registry_t *reg, hu_allocator_t *alloc,
                                const char *issuer_id, const char *target_id, uint32_t ttl_seconds,
                                const hu_delegation_caveat_t *caveats, size_t caveat_count) {
    if (!reg || !alloc || !issuer_id || !target_id)
        return NULL;

    if (reg->token_count >= reg->token_cap)
        return NULL; /* registry full */

    if (caveat_count > HU_DELEGATION_MAX_CAVEATS)
        return NULL; /* too many caveats */

    /* Copy caveats */
    hu_delegation_caveat_t *copied_caveats = NULL;
    if (caveat_count > 0) {
        if (copy_caveats(alloc, caveats, caveat_count, &copied_caveats) != HU_OK)
            return NULL;
    }

    /* Add token */
    hu_delegation_token_t *tok = &reg->tokens[reg->token_count];
    generate_token_id(tok->token_id, reg->token_counter++);
    strncpy(tok->issuer_agent_id, issuer_id, 63);
    tok->issuer_agent_id[63] = '\0';
    strncpy(tok->target_agent_id, target_id, 63);
    tok->target_agent_id[63] = '\0';
    tok->caveats = copied_caveats;
    tok->caveat_count = caveat_count;
    tok->issued_at = time(NULL);
    tok->expires_at = ttl_seconds > 0 ? tok->issued_at + ttl_seconds : 0;
    tok->revoked = false;
    tok->parent_token_id[0] = '\0'; /* root token */

    reg->token_count++;
    return tok->token_id;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Token attenuation
 * ───────────────────────────────────────────────────────────────────────── */

const char *hu_delegation_attenuate(hu_delegation_registry_t *reg, hu_allocator_t *alloc,
                                    const char *parent_token_id, const char *new_issuer_id,
                                    const char *new_target_id,
                                    const hu_delegation_caveat_t *additional_caveats,
                                    size_t additional_caveat_count) {
    if (!reg || !alloc || !parent_token_id || !new_issuer_id || !new_target_id)
        return NULL;

    /* Find parent token */
    hu_delegation_token_t *parent = NULL;
    for (size_t i = 0; i < reg->token_count; i++) {
        if (strcmp(reg->tokens[i].token_id, parent_token_id) == 0) {
            parent = &reg->tokens[i];
            break;
        }
    }
    if (!parent)
        return NULL;

    if (parent->revoked)
        return NULL; /* cannot attenuate revoked token */

    /* new_issuer must be the current target of parent */
    if (strcmp(parent->target_agent_id, new_issuer_id) != 0)
        return NULL;

    /* Check we won't exceed caveats limit */
    size_t total_caveats = parent->caveat_count + additional_caveat_count;
    if (total_caveats > HU_DELEGATION_MAX_CAVEATS)
        return NULL;

    /* Combine parent caveats with additional ones */
    hu_delegation_caveat_t *combined =
        (hu_delegation_caveat_t *)alloc->alloc(alloc->ctx, total_caveats * sizeof(*combined));
    if (!combined)
        return NULL;

    /* Copy parent caveats */
    for (size_t i = 0; i < parent->caveat_count; i++) {
        combined[i].key_len = parent->caveats[i].key_len;
        combined[i].key = (char *)alloc->alloc(alloc->ctx, parent->caveats[i].key_len + 1);
        if (!combined[i].key) {
            free_caveats(alloc, combined, i);
            return NULL;
        }
        memcpy(combined[i].key, parent->caveats[i].key, parent->caveats[i].key_len);
        combined[i].key[parent->caveats[i].key_len] = '\0';

        combined[i].value_len = parent->caveats[i].value_len;
        combined[i].value = (char *)alloc->alloc(alloc->ctx, parent->caveats[i].value_len + 1);
        if (!combined[i].value) {
            free_caveats(alloc, combined, i + 1);
            return NULL;
        }
        memcpy(combined[i].value, parent->caveats[i].value, parent->caveats[i].value_len);
        combined[i].value[parent->caveats[i].value_len] = '\0';
    }

    /* Copy additional caveats */
    for (size_t i = 0; i < additional_caveat_count; i++) {
        size_t idx = parent->caveat_count + i;
        combined[idx].key_len = additional_caveats[i].key_len;
        combined[idx].key = (char *)alloc->alloc(alloc->ctx, additional_caveats[i].key_len + 1);
        if (!combined[idx].key) {
            free_caveats(alloc, combined, idx);
            return NULL;
        }
        memcpy(combined[idx].key, additional_caveats[i].key, additional_caveats[i].key_len);
        combined[idx].key[additional_caveats[i].key_len] = '\0';

        combined[idx].value_len = additional_caveats[i].value_len;
        combined[idx].value = (char *)alloc->alloc(alloc->ctx, additional_caveats[i].value_len + 1);
        if (!combined[idx].value) {
            free_caveats(alloc, combined, idx + 1);
            return NULL;
        }
        memcpy(combined[idx].value, additional_caveats[i].value, additional_caveats[i].value_len);
        combined[idx].value[additional_caveats[i].value_len] = '\0';
    }

    if (reg->token_count >= reg->token_cap) {
        free_caveats(alloc, combined, total_caveats);
        return NULL;
    }

    hu_delegation_token_t *child = &reg->tokens[reg->token_count];
    generate_token_id(child->token_id, reg->token_counter++);
    strncpy(child->issuer_agent_id, new_issuer_id, 63);
    child->issuer_agent_id[63] = '\0';
    strncpy(child->target_agent_id, new_target_id, 63);
    child->target_agent_id[63] = '\0';
    child->caveats = combined;
    child->caveat_count = total_caveats;
    child->issued_at = time(NULL);
    child->expires_at = parent->expires_at; /* inherit parent's expiry or earlier */
    if (parent->expires_at == 0) {
        /* parent has no expiry, use child's issued time + parent's remaining TTL */
    }
    child->revoked = false;
    strncpy(child->parent_token_id, parent->token_id, 63);
    child->parent_token_id[63] = '\0';

    reg->token_count++;
    return child->token_id;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Verification
 * ───────────────────────────────────────────────────────────────────────── */

hu_error_t hu_delegation_verify(hu_delegation_registry_t *reg, const char *token_id,
                                const char *agent_id, const char *tool_name, const char *resource,
                                double cost_usd) {
    if (!reg || !token_id || !agent_id || !tool_name)
        return HU_ERR_INVALID_ARGUMENT;

    const hu_delegation_token_t *tok = hu_delegation_get_token(reg, token_id);
    if (!tok)
        return HU_ERR_NOT_FOUND;

    /* Check target matches */
    if (strcmp(tok->target_agent_id, agent_id) != 0)
        return HU_ERR_PERMISSION_DENIED;

    /* Check revocation (cascade) */
    if (tok->revoked || is_revoked_recursive(reg, tok->parent_token_id))
        return HU_ERR_PERMISSION_DENIED;

    /* Check expiry */
    if (tok->expires_at > 0 && time(NULL) > tok->expires_at)
        return HU_ERR_PERMISSION_DENIED;

    /* Check caveats */
    for (size_t i = 0; i < tok->caveat_count; i++) {
        const hu_delegation_caveat_t *c = &tok->caveats[i];

        if (strcmp(c->key, "tool") == 0) {
            /* tool constraint: exact match or glob */
            if (!path_matches_glob(tool_name, c->value))
                return HU_ERR_PERMISSION_DENIED;
        } else if (strcmp(c->key, "path") == 0) {
            /* path constraint */
            if (!resource)
                return HU_ERR_PERMISSION_DENIED; /* path caveat requires resource */
            if (!path_matches_glob(resource, c->value))
                return HU_ERR_PERMISSION_DENIED;
        } else if (strcmp(c->key, "max_cost") == 0) {
            /* max_cost constraint */
            double max_cost = strtod(c->value, NULL);
            if (cost_usd > max_cost)
                return HU_ERR_PERMISSION_DENIED;
        } else if (strcmp(c->key, "ttl") == 0) {
            /* ttl caveat (redundant with expires_at, but check anyway) */
            uint32_t ttl = (uint32_t)strtoul(c->value, NULL, 10);
            if (time(NULL) > tok->issued_at + ttl)
                return HU_ERR_PERMISSION_DENIED;
        }
        /* unknown caveats are ignored for forward compatibility */
    }

    return HU_OK;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Revocation
 * ───────────────────────────────────────────────────────────────────────── */

static void revoke_children_recursive(hu_delegation_registry_t *reg, const char *parent_id) {
    for (size_t i = 0; i < reg->token_count; i++) {
        if (strcmp(reg->tokens[i].parent_token_id, parent_id) == 0) {
            reg->tokens[i].revoked = true;
            revoke_children_recursive(reg, reg->tokens[i].token_id);
        }
    }
}

hu_error_t hu_delegation_revoke(hu_delegation_registry_t *reg, const char *token_id) {
    if (!reg || !token_id)
        return HU_ERR_INVALID_ARGUMENT;

    hu_delegation_token_t *tok = NULL;
    for (size_t i = 0; i < reg->token_count; i++) {
        if (strcmp(reg->tokens[i].token_id, token_id) == 0) {
            tok = &reg->tokens[i];
            break;
        }
    }
    if (!tok)
        return HU_ERR_NOT_FOUND;

    tok->revoked = true;
    /* Cascade: mark all derived tokens as revoked */
    revoke_children_recursive(reg, token_id);

    return HU_OK;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Chain traversal
 * ───────────────────────────────────────────────────────────────────────── */

hu_error_t hu_delegation_chain(hu_delegation_registry_t *reg, hu_allocator_t *alloc,
                               const char *token_id, const char ***out, size_t *out_count) {
    if (!reg || !alloc || !token_id || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;

    /* Collect chain by walking up parent pointers */
    const hu_delegation_token_t *tokens[64]; /* max chain depth */
    size_t chain_len = 0;

    const hu_delegation_token_t *current = hu_delegation_get_token(reg, token_id);
    if (!current)
        return HU_ERR_NOT_FOUND;

    while (current && chain_len < 64) {
        tokens[chain_len++] = current;
        if (strlen(current->parent_token_id) == 0)
            break;
        current = hu_delegation_get_token(reg, current->parent_token_id);
    }

    /* Reverse to get root->...->leaf order */
    const char **result = (const char **)alloc->alloc(alloc->ctx, chain_len * sizeof(char *));
    if (!result)
        return HU_ERR_OUT_OF_MEMORY;

    for (size_t i = 0; i < chain_len; i++) {
        result[i] = tokens[chain_len - 1 - i]->token_id;
    }

    *out = result;
    *out_count = chain_len;
    return HU_OK;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Query
 * ───────────────────────────────────────────────────────────────────────── */

const hu_delegation_token_t *hu_delegation_get_token(hu_delegation_registry_t *reg,
                                                     const char *token_id) {
    if (!reg || !token_id)
        return NULL;

    for (size_t i = 0; i < reg->token_count; i++) {
        if (strcmp(reg->tokens[i].token_id, token_id) == 0) {
            return &reg->tokens[i];
        }
    }
    return NULL;
}

hu_error_t hu_delegation_tokens_by_issuer(hu_delegation_registry_t *reg, hu_allocator_t *alloc,
                                          const char *issuer_id, const char ***out,
                                          size_t *out_count) {
    if (!reg || !alloc || !issuer_id || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;

    size_t count = 0;
    for (size_t i = 0; i < reg->token_count; i++) {
        if (strcmp(reg->tokens[i].issuer_agent_id, issuer_id) == 0)
            count++;
    }

    const char **result = (const char **)alloc->alloc(alloc->ctx, count * sizeof(char *));
    if (!result)
        return HU_ERR_OUT_OF_MEMORY;

    size_t idx = 0;
    for (size_t i = 0; i < reg->token_count; i++) {
        if (strcmp(reg->tokens[i].issuer_agent_id, issuer_id) == 0) {
            result[idx++] = reg->tokens[i].token_id;
        }
    }

    *out = result;
    *out_count = count;
    return HU_OK;
}

hu_error_t hu_delegation_tokens_by_target(hu_delegation_registry_t *reg, hu_allocator_t *alloc,
                                          const char *target_id, const char ***out,
                                          size_t *out_count) {
    if (!reg || !alloc || !target_id || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;

    size_t count = 0;
    for (size_t i = 0; i < reg->token_count; i++) {
        if (strcmp(reg->tokens[i].target_agent_id, target_id) == 0)
            count++;
    }

    const char **result = (const char **)alloc->alloc(alloc->ctx, count * sizeof(char *));
    if (!result)
        return HU_ERR_OUT_OF_MEMORY;

    size_t idx = 0;
    for (size_t i = 0; i < reg->token_count; i++) {
        if (strcmp(reg->tokens[i].target_agent_id, target_id) == 0) {
            result[idx++] = reg->tokens[i].token_id;
        }
    }

    *out = result;
    *out_count = count;
    return HU_OK;
}

size_t hu_delegation_token_count(hu_delegation_registry_t *reg) {
    if (!reg)
        return 0;
    return reg->token_count;
}
