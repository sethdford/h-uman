#ifndef SC_CAPABILITIES_H
#define SC_CAPABILITIES_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/config.h"
#include "seaclaw/tool.h"
#include <stddef.h>

/**
 * Build JSON manifest of runtime capabilities (channels, memory engines, tools).
 * Caller owns returned string; free with allocator.
 */
sc_error_t sc_capabilities_build_manifest_json(sc_allocator_t *alloc,
    const sc_config_t *cfg_opt,
    const sc_tool_t *runtime_tools, size_t runtime_tools_count,
    char **out_json);

/**
 * Build human-readable summary text.
 */
sc_error_t sc_capabilities_build_summary_text(sc_allocator_t *alloc,
    const sc_config_t *cfg_opt,
    const sc_tool_t *runtime_tools, size_t runtime_tools_count,
    char **out_text);

/**
 * Build prompt section for agent context.
 */
sc_error_t sc_capabilities_build_prompt_section(sc_allocator_t *alloc,
    const sc_config_t *cfg_opt,
    const sc_tool_t *runtime_tools, size_t runtime_tools_count,
    char **out_text);

#endif /* SC_CAPABILITIES_H */
