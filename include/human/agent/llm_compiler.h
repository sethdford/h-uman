#ifndef HU_LLM_COMPILER_H
#define HU_LLM_COMPILER_H

#include "human/agent/dag.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/tool.h"
#include <stddef.h>

hu_error_t hu_llm_compiler_build_prompt(hu_allocator_t *alloc, const char *goal, size_t goal_len,
                                        const char **tool_names, size_t tool_count, char **out,
                                        size_t *out_len);
hu_error_t hu_llm_compiler_parse_plan(hu_allocator_t *alloc, const char *response,
                                      size_t response_len, hu_dag_t *dag);

#endif
