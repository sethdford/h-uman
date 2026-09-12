#include "human/agent/llm_compiler.h"
#include "human/agent/dag.h"
#include "human/core/string.h"
#include "human/util/llm_json.h"
#include <string.h>

static const char LLM_COMPILER_PROMPT_PREFIX[] =
    "Given this goal and tools, produce a JSON plan. Use the format:\n"
    "{\"tasks\":[{\"id\":\"t1\",\"tool\":\"...\",\"args\":{},\"deps\":[]},...]}\n"
    "Use $tN to reference outputs of task N (e.g. $t1 for task t1). Parallelize where possible.\n\n"
    "Goal: ";

static const char LLM_COMPILER_PROMPT_TOOLS[] = "\n\nAvailable tools: ";

static const char LLM_COMPILER_PROMPT_SUFFIX[] =
    "\n\nRespond with only the JSON plan, no other text.";

hu_error_t hu_llm_compiler_build_prompt(hu_allocator_t *alloc, const char *goal, size_t goal_len,
                                        const char **tool_names, size_t tool_count, char **out,
                                        size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    size_t cap = sizeof(LLM_COMPILER_PROMPT_PREFIX) - 1 + (goal ? goal_len : 0) +
                 sizeof(LLM_COMPILER_PROMPT_TOOLS) - 1 + sizeof(LLM_COMPILER_PROMPT_SUFFIX) - 1 +
                 64;
    for (size_t i = 0; i < tool_count && tool_names && tool_names[i]; i++) {
        cap += strlen(tool_names[i]) + 2;
    }

    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    size_t pos = 0;
    memcpy(buf + pos, LLM_COMPILER_PROMPT_PREFIX, sizeof(LLM_COMPILER_PROMPT_PREFIX) - 1);
    pos += sizeof(LLM_COMPILER_PROMPT_PREFIX) - 1;

    if (goal && goal_len > 0) {
        size_t n = goal_len < cap - pos ? goal_len : (size_t)(cap - pos - 1);
        memcpy(buf + pos, goal, n);
        pos += n;
    }

    memcpy(buf + pos, LLM_COMPILER_PROMPT_TOOLS, sizeof(LLM_COMPILER_PROMPT_TOOLS) - 1);
    pos += sizeof(LLM_COMPILER_PROMPT_TOOLS) - 1;

    for (size_t i = 0; i < tool_count && tool_names && tool_names[i]; i++) {
        size_t len = strlen(tool_names[i]);
        if (pos + len + 3 > cap)
            break;
        if (i > 0) {
            buf[pos++] = ',';
            buf[pos++] = ' ';
        }
        memcpy(buf + pos, tool_names[i], len);
        pos += len;
    }

    memcpy(buf + pos, LLM_COMPILER_PROMPT_SUFFIX, sizeof(LLM_COMPILER_PROMPT_SUFFIX) - 1);
    pos += sizeof(LLM_COMPILER_PROMPT_SUFFIX) - 1;
    buf[pos] = '\0';

    *out = buf;
    *out_len = pos;
    return HU_OK;
}

/* Shared locator (string-aware, strips <think> blocks and fences). The private
 * brace-counter this replaced broke on a "}" inside a JSON string. */
static void extract_json_from_response(const char *s, size_t len, const char **out_ptr,
                                       size_t *out_len) {
    if (!hu_llm_json_locate(s, len, out_ptr, out_len)) {
        *out_ptr = s;
        *out_len = len;
    }
}

hu_error_t hu_llm_compiler_parse_plan(hu_allocator_t *alloc, const char *response,
                                      size_t response_len, hu_dag_t *dag) {
    if (!alloc || !response || !dag)
        return HU_ERR_INVALID_ARGUMENT;

    const char *json = NULL;
    size_t json_len = 0;
    extract_json_from_response(response, response_len, &json, &json_len);

    return hu_dag_parse_json(dag, alloc, json, json_len);
}
