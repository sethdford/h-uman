#ifndef HU_TOOLS_SCHEMA_CLEAN_H
#define HU_TOOLS_SCHEMA_CLEAN_H

#include "human/core/allocator.h"
#include "human/core/error.h"
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
 * @return HU_OK on success
 */
hu_error_t hu_schema_clean(hu_allocator_t *alloc, const char *schema_json, size_t schema_len,
                           const char *provider_name, char **out_cleaned, size_t *out_len);

/**
 * Validate that a schema has a "type" field (root is object with "type" key).
 */
bool hu_schema_validate(hu_allocator_t *alloc, const char *schema_json, size_t schema_len);

#endif /* HU_TOOLS_SCHEMA_CLEAN_H */
