#ifndef HU_CAPABILITIES_H
#define HU_CAPABILITIES_H

#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/tool.h"
#include <stddef.h>

/**
 * Build JSON manifest of runtime capabilities (channels, memory engines, tools).
 * Caller owns returned string; free with allocator.
 */
hu_error_t hu_capabilities_build_manifest_json(hu_allocator_t *alloc, const hu_config_t *cfg_opt,
                                               const hu_tool_t *runtime_tools,
                                               size_t runtime_tools_count, char **out_json);

/**
 * Build human-readable summary text.
 */
hu_error_t hu_capabilities_build_summary_text(hu_allocator_t *alloc, const hu_config_t *cfg_opt,
                                              const hu_tool_t *runtime_tools,
                                              size_t runtime_tools_count, char **out_text);

/**
 * Build prompt section for agent context.
 */
hu_error_t hu_capabilities_build_prompt_section(hu_allocator_t *alloc, const hu_config_t *cfg_opt,
                                                const hu_tool_t *runtime_tools,
                                                size_t runtime_tools_count, char **out_text);

#endif /* HU_CAPABILITIES_H */
