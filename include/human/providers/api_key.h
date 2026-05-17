#ifndef HU_API_KEY_H
#define HU_API_KEY_H

#include "human/core/allocator.h"
#include <stdbool.h>
#include <stddef.h>

/* Resolve API key: explicit key (trimmed), provider env var, or generic fallbacks.
 * Returns owned string or NULL. Caller must free. */
char *hu_api_key_resolve(hu_allocator_t *alloc, const char *provider_name, size_t provider_name_len,
                         const char *api_key, size_t api_key_len);

/* Return the canonical environment variable name for a provider's API key,
 * or NULL if the provider is unknown. The returned pointer is statically
 * allocated and must NOT be freed.
 *
 * Single source of truth for provider → env-var mapping. Used to eliminate
 * strcmp-based dispatch in config_merge.c (audit 2026-05-16).
 *
 * Examples:
 *   "openai"     → "OPENAI_API_KEY"
 *   "anthropic"  → "ANTHROPIC_API_KEY"
 *   "gemini"     → "GEMINI_API_KEY"
 *   "google"     → "GEMINI_API_KEY"  (alias)
 *   "vertex"     → "GEMINI_API_KEY"  (alias)
 *   "ollama"     → "OLLAMA_HOST"
 *   "unknown"    → NULL
 */
const char *hu_provider_default_api_key_env_name(const char *provider_name,
                                                 size_t provider_name_len);

/* Validate API key format - returns true if non-empty after trim */
bool hu_api_key_valid(const char *key, size_t key_len);

/* Mask key for logs - show only last 4 chars */
char *hu_api_key_mask(hu_allocator_t *alloc, const char *key, size_t key_len);

#endif /* HU_API_KEY_H */
