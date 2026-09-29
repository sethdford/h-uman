#include "human/agent/orchestrator_llm.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/util/llm_json.h"
#include <stdio.h>
#include <string.h>

#define HU_ORCH_LLM_CAPABILITY_SIZE 64

__attribute__((unused))
/* Shared locator (string-aware, strips <think> blocks and fences). The private
 * brace-counter this replaced broke on a "}" inside a JSON string. */
static void
extract_json_from_response(const char *s, size_t len, const char **out_ptr, size_t *out_len) {
    if (!hu_llm_json_locate(s, len, out_ptr, out_len)) {
        *out_ptr = s;
        *out_len = len;
    }
}

__attribute__((unused)) static bool validate_decomposition_json(hu_json_value_t *root) {
    if (!root || root->type != HU_JSON_OBJECT)
        return false;
    hu_json_value_t *tasks = hu_json_object_get(root, "tasks");
    if (!tasks || tasks->type != HU_JSON_ARRAY)
        return false;
    for (size_t i = 0; i < tasks->data.array.len; i++) {
        hu_json_value_t *t = tasks->data.array.items[i];
        if (!t || t->type != HU_JSON_OBJECT)
            return false;
        if (!hu_json_get_string(t, "description"))
            return false;
    }
    return true;
}

static bool contains_substr(const char *haystack, size_t hlen, const char *needle, size_t nlen) {
    if (nlen == 0 || nlen > hlen)
        return false;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        if (memcmp(haystack + i, needle, nlen) == 0)
            return true;
    }
    return false;
}

static bool agent_matches_capability(const hu_agent_capability_t *agent, const char *capability,
                                     size_t cap_len) {
    if (!capability || cap_len == 0)
        return true;
    if (agent->skills_len > 0 &&
        contains_substr(agent->skills, agent->skills_len, capability, cap_len))
        return true;
    if (agent->role_len > 0 && contains_substr(agent->role, agent->role_len, capability, cap_len))
        return true;
    return false;
}

static size_t safe_strnlen(const char *s, size_t maxlen) {
    size_t n = 0;
    while (n < maxlen && s[n])
        n++;
    return n;
}

hu_error_t hu_orchestrator_decompose_goal(hu_allocator_t *alloc, hu_provider_t *provider,
                                          const char *model, size_t model_len, const char *goal,
                                          size_t goal_len,
                                          const hu_agent_capability_t *capabilities,
                                          size_t capability_count,
                                          struct hu_decomposition *result) {
    if (!alloc || !result)
        return HU_ERR_INVALID_ARGUMENT;
    memset(result, 0, sizeof(*result));

#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)provider;
    (void)model;
    (void)model_len;
    (void)goal;
    (void)goal_len;
    (void)capabilities;
    (void)capability_count;

    result->task_count = 2;
    result->tasks[0].status = HU_TASK_UNASSIGNED;
    result->tasks[0].depends_on = 0;
    result->tasks[0].priority = 1.0;
    strncpy(result->tasks[0].description, "research", sizeof(result->tasks[0].description) - 1);
    result->tasks[0].description[sizeof(result->tasks[0].description) - 1] = '\0';
    result->tasks[0].description_len = 8;

    strncpy(result->capabilities[0], "research", sizeof(result->capabilities[0]) - 1);
    result->capabilities[0][sizeof(result->capabilities[0]) - 1] = '\0';

    result->tasks[1].status = HU_TASK_UNASSIGNED;
    result->tasks[1].depends_on = 1; /* 1 = index of dependency (task 0) */
    result->tasks[1].priority = 1.0;
    strncpy(result->tasks[1].description, "synthesize", sizeof(result->tasks[1].description) - 1);
    result->tasks[1].description[sizeof(result->tasks[1].description) - 1] = '\0';
    result->tasks[1].description_len = 10;

    strncpy(result->capabilities[1], "synthesize", sizeof(result->capabilities[1]) - 1);
    result->capabilities[1][sizeof(result->capabilities[1]) - 1] = '\0';

    result->reasoning = hu_strndup(alloc, "Split into research and synthesis phases", 40);
    result->reasoning_len = result->reasoning ? 40 : 0;
    return HU_OK;
#else
    if (!provider || !provider->vtable || !provider->vtable->chat_with_system)
        return HU_ERR_NOT_SUPPORTED;
    if (goal_len > 0 && !goal)
        return HU_ERR_INVALID_ARGUMENT;

    size_t cap_buf = 256;
    for (size_t i = 0; i < capability_count && capabilities; i++) {
        cap_buf += capabilities[i].agent_id_len + capabilities[i].role_len +
                   capabilities[i].skills_len + 32;
    }
    size_t prompt_cap = 512 + (goal_len > 0 ? goal_len : 0) + cap_buf;
    char *prompt = (char *)alloc->alloc(alloc->ctx, prompt_cap);
    if (!prompt)
        return HU_ERR_OUT_OF_MEMORY;

    int pos = snprintf(prompt, prompt_cap,
                       "Decompose this goal into subtasks. Reply with JSON only: "
                       "{\"reasoning\":\"...\",\"tasks\":[{\"description\":\"...\","
                       "\"capability\":\"...\",\"depends_on\":[]}]}. "
                       "depends_on: array of task indices (0-based) this task depends on. "
                       "Available agents: ");
    if (pos < 0 || (size_t)pos >= prompt_cap) {
        alloc->free(alloc->ctx, prompt, prompt_cap);
        return HU_ERR_OUT_OF_MEMORY;
    }

    for (size_t i = 0; i < capability_count && capabilities; i++) {
        const hu_agent_capability_t *c = &capabilities[i];
        int n = snprintf(prompt + pos, prompt_cap - (size_t)pos, "[%.*s: role=%.*s skills=%.*s] ",
                         (int)c->agent_id_len, c->agent_id, (int)c->role_len, c->role,
                         (int)c->skills_len, c->skills);
        if (n > 0 && pos + n < (int)prompt_cap)
            pos += n;
    }

    int n = snprintf(prompt + pos, prompt_cap - (size_t)pos, " Goal: %.*s", (int)goal_len,
                     goal && goal_len > 0 ? goal : "");
    if (n > 0 && pos + n < (int)prompt_cap)
        pos += n;

    const char *sys =
        "You are a multi-agent orchestration planner. Each listed agent has an id, role, and "
        "comma-separated skills. Break the user's goal into ordered subtasks. For every subtask, "
        "set \"capability\" to a short token that matches one agent's role or skills when "
        "possible, or \"general\". Prefer descriptions that name a concrete tool or actionable "
        "step sub-agents can run. Reply with JSON only — no markdown, no prose outside the object.";
    char *llm_out = NULL;
    size_t llm_out_len = 0;

    hu_error_t err = provider->vtable->chat_with_system(
        provider->ctx, alloc, sys, 55, prompt, (size_t)pos,
        model && model_len > 0 ? model : "gpt-4o-mini", model && model_len > 0 ? model_len : 11,
        0.2, &llm_out, &llm_out_len);

    alloc->free(alloc->ctx, prompt, prompt_cap);

    if (err != HU_OK)
        return err;
    if (!llm_out || llm_out_len == 0) {
        if (llm_out)
            alloc->free(alloc->ctx, llm_out, llm_out_len + 1);
        return HU_ERR_PROVIDER_RESPONSE;
    }

    const char *json_ptr = NULL;
    size_t json_len = 0;
    extract_json_from_response(llm_out, llm_out_len, &json_ptr, &json_len);

    hu_json_value_t *root = NULL;
    err = hu_json_parse(alloc, json_ptr, json_len, &root);
    alloc->free(alloc->ctx, llm_out, llm_out_len + 1);

    if (err != HU_OK)
        return err;

    if (!validate_decomposition_json(root)) {
        hu_json_free(alloc, root);
        return HU_ERR_PARSE;
    }

    const char *reasoning_str =
        root && root->type == HU_JSON_OBJECT ? hu_json_get_string(root, "reasoning") : NULL;
    if (reasoning_str) {
        result->reasoning = hu_strndup(alloc, reasoning_str, strlen(reasoning_str));
        result->reasoning_len = result->reasoning ? strlen(reasoning_str) : 0;
    }

    hu_json_value_t *tasks_arr =
        root && root->type == HU_JSON_OBJECT ? hu_json_object_get(root, "tasks") : NULL;
    if (!tasks_arr || tasks_arr->type != HU_JSON_ARRAY) {
        hu_json_free(alloc, root);
        return HU_ERR_PARSE;
    }

    size_t n_tasks = tasks_arr->data.array.len;
    if (n_tasks > HU_ORCH_LLM_MAX_SUBTASKS)
        n_tasks = HU_ORCH_LLM_MAX_SUBTASKS;

    for (size_t i = 0; i < n_tasks; i++) {
        hu_json_value_t *task_val = tasks_arr->data.array.items[i];
        if (!task_val || task_val->type != HU_JSON_OBJECT)
            continue;

        hu_orchestrator_task_t *t = &result->tasks[result->task_count];
        memset(t, 0, sizeof(*t));
        t->status = HU_TASK_UNASSIGNED;
        t->depends_on = 0;
        t->priority = 1.0;

        const char *desc = hu_json_get_string(task_val, "description");
        if (desc) {
            size_t dlen = strlen(desc);
            size_t copy = dlen < sizeof(t->description) - 1 ? dlen : sizeof(t->description) - 1;
            strncpy(t->description, desc, copy);
            t->description[copy] = '\0';
            t->description_len = copy;
        }

        const char *cap = hu_json_get_string(task_val, "capability");
        if (cap) {
            size_t clen = strlen(cap);
            size_t copy =
                clen < HU_ORCH_LLM_CAPABILITY_SIZE - 1 ? clen : HU_ORCH_LLM_CAPABILITY_SIZE - 1;
            strncpy(result->capabilities[result->task_count], cap, copy);
            result->capabilities[result->task_count][copy] = '\0';
        }

        hu_json_value_t *deps = hu_json_object_get(task_val, "depends_on");
        if (deps && deps->type == HU_JSON_ARRAY && deps->data.array.len > 0) {
            hu_json_value_t *first = deps->data.array.items[0];
            if (first && first->type == HU_JSON_NUMBER)
                t->depends_on = (uint32_t)(int)first->data.number + 1; /* 1-based for resolution */
        }

        result->task_count++;
    }

    hu_json_free(alloc, root);
    return HU_OK;
#endif
}

void hu_decomposition_free(hu_allocator_t *alloc, hu_decomposition_t *result) {
    if (!alloc || !result)
        return;
    if (result->reasoning) {
        alloc->free(alloc->ctx, result->reasoning, result->reasoning_len + 1);
        result->reasoning = NULL;
        result->reasoning_len = 0;
    }
}

#if !defined(HU_IS_TEST) || !HU_IS_TEST
static void decomposition_result_from_orchestrator(const hu_decomposition_t *decomp,
                                                   hu_decomposition_strategy_t strategy,
                                                   hu_decomposition_result_t *result) {
    memset(result, 0, sizeof(*result));
    result->strategy = strategy;
    result->task_count =
        decomp->task_count < HU_DECOMP_MAX_TASKS ? decomp->task_count : HU_DECOMP_MAX_TASKS;
    for (size_t i = 0; i < result->task_count; i++) {
        size_t copy = decomp->tasks[i].description_len < sizeof(result->tasks[i].description) - 1
                          ? decomp->tasks[i].description_len
                          : sizeof(result->tasks[i].description) - 1;
        memcpy(result->tasks[i].description, decomp->tasks[i].description, copy);
        result->tasks[i].description[copy] = '\0';
        result->tasks[i].description_len = copy;
        result->tasks[i].depends_on = decomp->tasks[i].depends_on;
    }
}
#endif

hu_error_t hu_decompose_task(hu_allocator_t *alloc, hu_provider_t *provider, const char *model,
                             size_t model_len, const char *prompt, size_t prompt_len,
                             hu_decomposition_strategy_t strategy,
                             hu_decomposition_result_t *result) {
    if (!alloc || !result)
        return HU_ERR_INVALID_ARGUMENT;
    if (prompt_len > 0 && !prompt)
        return HU_ERR_INVALID_ARGUMENT;

    memset(result, 0, sizeof(*result));
    result->strategy = strategy;

#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)provider;
    (void)model;
    (void)model_len;
    (void)prompt;
    (void)prompt_len;

    /* Mock: 3 subtasks, 2 parallel + 1 dependent */
    result->task_count = 3;
    result->strategy = strategy;

    strncpy(result->tasks[0].description, "parallel task A",
            sizeof(result->tasks[0].description) - 1);
    result->tasks[0].description[sizeof(result->tasks[0].description) - 1] = '\0';
    result->tasks[0].description_len = 14;
    result->tasks[0].depends_on = 0;

    strncpy(result->tasks[1].description, "parallel task B",
            sizeof(result->tasks[1].description) - 1);
    result->tasks[1].description[sizeof(result->tasks[1].description) - 1] = '\0';
    result->tasks[1].description_len = 14;
    result->tasks[1].depends_on = 0;

    strncpy(result->tasks[2].description, "dependent synthesis task",
            sizeof(result->tasks[2].description) - 1);
    result->tasks[2].description[sizeof(result->tasks[2].description) - 1] = '\0';
    result->tasks[2].description_len = 24;
    result->tasks[2].depends_on = 1; /* depends on task 1 (1-based) */

    return HU_OK;
#else
    if (provider && provider->vtable && provider->vtable->chat_with_system) {
        hu_decomposition_t decomp = {0};
        hu_error_t err = hu_orchestrator_decompose_goal(alloc, provider, model, model_len, prompt,
                                                        prompt_len, NULL, 0, &decomp);
        if (err == HU_OK && decomp.task_count > 0) {
            decomposition_result_from_orchestrator(&decomp, strategy, result);
            hu_decomposition_free(alloc, &decomp);
            return HU_OK;
        }
        hu_decomposition_free(alloc, &decomp);
    }

    /* Fallback: heuristic (same structure as mock) */
    result->task_count = 3;
    result->strategy = strategy;
    strncpy(result->tasks[0].description, "parallel task A",
            sizeof(result->tasks[0].description) - 1);
    result->tasks[0].description[sizeof(result->tasks[0].description) - 1] = '\0';
    result->tasks[0].description_len = 14;
    result->tasks[0].depends_on = 0;

    strncpy(result->tasks[1].description, "parallel task B",
            sizeof(result->tasks[1].description) - 1);
    result->tasks[1].description[sizeof(result->tasks[1].description) - 1] = '\0';
    result->tasks[1].description_len = 14;
    result->tasks[1].depends_on = 0;

    strncpy(result->tasks[2].description, "dependent synthesis task",
            sizeof(result->tasks[2].description) - 1);
    result->tasks[2].description[sizeof(result->tasks[2].description) - 1] = '\0';
    result->tasks[2].description_len = 24;
    result->tasks[2].depends_on = 1;
    return HU_OK;
#endif
}

hu_error_t hu_decompose_with_replan(hu_allocator_t *alloc, hu_provider_t *provider,
                                    const char *model, size_t model_len,
                                    const char *original_prompt, size_t original_prompt_len,
                                    const char *failed_task, size_t failed_task_len,
                                    const char *failure_reason, size_t failure_reason_len,
                                    hu_decomposition_strategy_t strategy,
                                    hu_decomposition_result_t *result) {
    if (!alloc || !result)
        return HU_ERR_INVALID_ARGUMENT;

    memset(result, 0, sizeof(*result));
    result->strategy = strategy;

#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)provider;
    (void)model;
    (void)model_len;
    (void)original_prompt;
    (void)original_prompt_len;
    (void)failed_task;
    (void)failed_task_len;
    (void)failure_reason;
    (void)failure_reason_len;

    result->task_count = 2;
    strncpy(result->tasks[0].description, "alternative approach A",
            sizeof(result->tasks[0].description) - 1);
    result->tasks[0].description[sizeof(result->tasks[0].description) - 1] = '\0';
    result->tasks[0].description_len = 22;
    result->tasks[0].depends_on = 0;

    strncpy(result->tasks[1].description, "finalize with workaround",
            sizeof(result->tasks[1].description) - 1);
    result->tasks[1].description[sizeof(result->tasks[1].description) - 1] = '\0';
    result->tasks[1].description_len = 24;
    result->tasks[1].depends_on = 1;
    return HU_OK;
#else
    /* Build a re-planning prompt that includes the failure context */
    size_t prompt_cap = 512 + original_prompt_len + failed_task_len + failure_reason_len;
    char *replan_prompt = (char *)alloc->alloc(alloc->ctx, prompt_cap);
    if (!replan_prompt)
        return HU_ERR_OUT_OF_MEMORY;

    int pos = snprintf(replan_prompt, prompt_cap,
                       "Re-decompose this task. A previous subtask failed.\n"
                       "Original goal: %.*s\n"
                       "Failed subtask: %.*s\n"
                       "Failure reason: %.*s\n"
                       "Create a new plan that works around this failure.",
                       (int)(original_prompt_len < 1000 ? original_prompt_len : 1000),
                       original_prompt ? original_prompt : "",
                       (int)(failed_task_len < 500 ? failed_task_len : 500),
                       failed_task ? failed_task : "",
                       (int)(failure_reason_len < 500 ? failure_reason_len : 500),
                       failure_reason ? failure_reason : "");

    hu_error_t err = hu_decompose_task(alloc, provider, model, model_len, replan_prompt,
                                       pos > 0 ? (size_t)pos : 0, strategy, result);
    alloc->free(alloc->ctx, replan_prompt, prompt_cap);
    return err;
#endif
}

static int tolower_orch(int c) {
    return (c >= 'A' && c <= 'Z') ? (c + 32) : c;
}

bool hu_decomposition_check_coverage(const char *goal, size_t goal_len,
                                     const hu_decomposition_result_t *result) {
    if (!goal || goal_len == 0 || !result || result->task_count == 0)
        return false;

    /* Extract words from goal (>3 chars), count how many appear in subtask descriptions */
    size_t goal_words = 0;
    size_t matched_words = 0;
    const char *p = goal;
    const char *end = goal + goal_len;

    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n'))
            p++;
        if (p >= end)
            break;
        const char *ws = p;
        while (p < end && *p != ' ' && *p != '\t' && *p != '\n')
            p++;
        size_t wlen = (size_t)(p - ws);
        if (wlen <= 3)
            continue; /* skip short words */
        goal_words++;

        /* Check if this word appears in any subtask */
        bool found = false;
        for (size_t t = 0; t < result->task_count && !found; t++) {
            const char *desc = result->tasks[t].description;
            size_t dlen = result->tasks[t].description_len;
            if (dlen == 0)
                continue;
            for (size_t d = 0; d + wlen <= dlen; d++) {
                bool match = true;
                for (size_t j = 0; j < wlen; j++) {
                    if (tolower_orch((unsigned char)desc[d + j]) !=
                        tolower_orch((unsigned char)ws[j])) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    found = true;
                    break;
                }
            }
        }
        if (found)
            matched_words++;
    }

    if (goal_words == 0)
        return false;
    /* Require at least 30% word coverage */
    return ((double)matched_words / (double)goal_words) >= 0.3;
}

hu_error_t hu_orchestrator_auto_assign(hu_orchestrator_t *orch,
                                       const hu_decomposition_t *decomposition) {
    if (!orch || !decomposition || decomposition->task_count == 0)
        return HU_ERR_INVALID_ARGUMENT;

    const char *subtasks[HU_ORCH_LLM_MAX_SUBTASKS];
    size_t subtask_lens[HU_ORCH_LLM_MAX_SUBTASKS];

    for (size_t i = 0; i < decomposition->task_count; i++) {
        subtasks[i] = decomposition->tasks[i].description;
        subtask_lens[i] = decomposition->tasks[i].description_len;
    }

    hu_error_t err = hu_orchestrator_propose_split(orch, "", 0, subtasks, subtask_lens,
                                                   decomposition->task_count);
    if (err != HU_OK)
        return err;

    size_t base = orch->task_count - decomposition->task_count;

    for (size_t i = 0; i < decomposition->task_count; i++) {
        hu_orchestrator_task_t *t = &orch->tasks[base + i];
        uint32_t dep_idx = decomposition->tasks[i].depends_on;
        if (dep_idx > 0 && dep_idx <= decomposition->task_count) {
            t->depends_on = orch->tasks[base + dep_idx - 1].id;
        }
    }

    for (size_t i = 0; i < decomposition->task_count; i++) {
        const char *cap = decomposition->capabilities[i];
        size_t cap_len = safe_strnlen(cap, HU_ORCH_LLM_CAPABILITY_SIZE - 1);

        size_t agent_idx = orch->agent_count;
        for (size_t a = 0; a < orch->agent_count; a++) {
            if (agent_matches_capability(&orch->agents[a], cap, cap_len)) {
                agent_idx = a;
                break;
            }
        }

        if (agent_idx >= orch->agent_count)
            return HU_ERR_NOT_FOUND;

        err = hu_orchestrator_assign_task(orch, orch->tasks[base + i].id,
                                          orch->agents[agent_idx].agent_id,
                                          orch->agents[agent_idx].agent_id_len);
        if (err != HU_OK)
            return err;
    }

    return HU_OK;
}
