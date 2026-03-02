#ifndef SC_TOOLS_SCHEMA_CLEAN_H
#define SC_TOOLS_SCHEMA_CLEAN_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdbool.h>

/**
 * Clean a JSON schema for a specific LLM provider.
 * Removes unsupported keywords, resolves $ref, converts const to enum, etc.
 *
 * @param alloc Allocator for all allocations
 * @param schema_json Input schema as JSON string
 * @param schema_len Length of schema_json
 * @param provider_name One of: "gemini", "anthropic", "openai", "conservative"
 * @param out_cleaned Output: cleaned JSON string (caller must free via alloc)
 * @param out_len Output length of cleaned string
 * @return SC_OK on success
 */
sc_error_t sc_schema_clean(sc_allocator_t *alloc,
    const char *schema_json, size_t schema_len,
    const char *provider_name,
    char **out_cleaned, size_t *out_len);

/**
 * Validate that a schema has a "type" field (root is object with "type" key).
 */
bool sc_schema_validate(sc_allocator_t *alloc,
    const char *schema_json, size_t schema_len);

#endif /* SC_TOOLS_SCHEMA_CLEAN_H */
