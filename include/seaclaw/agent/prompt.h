#ifndef SC_AGENT_PROMPT_H
#define SC_AGENT_PROMPT_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/tool.h"
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * System prompt builder — identity, tools, memory, datetime, constraints
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct sc_prompt_config {
    const char *provider_name;
    size_t provider_name_len;
    const char *model_name;
    size_t model_name_len;
    const char *workspace_dir;
    size_t workspace_dir_len;
    sc_tool_t *tools;
    size_t tools_count;
    const char *memory_context;
    size_t memory_context_len;
    uint8_t autonomy_level; /* 0=readonly, 1=supervised, 2=full */
    const char *custom_instructions;
    size_t custom_instructions_len;
} sc_prompt_config_t;

/* Build the full system prompt. Caller owns returned string; free with alloc. */
sc_error_t sc_prompt_build_system(sc_allocator_t *alloc, const sc_prompt_config_t *config,
                                  char **out, size_t *out_len);

/* Build only the static parts (identity, tools, autonomy, safety, custom).
 * The result can be cached and reused across turns. Caller owns returned string. */
sc_error_t sc_prompt_build_static(sc_allocator_t *alloc, const sc_prompt_config_t *config,
                                  char **out, size_t *out_len);

/* Build full prompt by combining cached static part with dynamic memory context.
 * Avoids rebuilding the static portion each turn. Caller owns returned string. */
sc_error_t sc_prompt_build_with_cache(sc_allocator_t *alloc, const char *static_prompt,
                                      size_t static_prompt_len, const char *memory_context,
                                      size_t memory_context_len, char **out, size_t *out_len);

#endif /* SC_AGENT_PROMPT_H */
