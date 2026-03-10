#include "human/agent/planner.h"
#include "human/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static hu_error_t parse_step(hu_allocator_t *alloc, const hu_json_value_t *step_obj,
                             hu_plan_step_t *out) {
    if (!step_obj || step_obj->type != HU_JSON_OBJECT)
        return HU_ERR_INVALID_ARGUMENT;

    const char *name = hu_json_get_string(step_obj, "tool");
    if (!name)
        name = hu_json_get_string(step_obj, "name");
    if (!name || strlen(name) == 0)
        return HU_ERR_INVALID_ARGUMENT;

    out->tool_name = hu_strdup(alloc, name);
    if (!out->tool_name)
        return HU_ERR_OUT_OF_MEMORY;

    out->args_json = NULL;
    hu_json_value_t *args_val = hu_json_object_get(step_obj, "args");
    if (!args_val)
        args_val = hu_json_object_get(step_obj, "arguments");
    if (args_val) {
        hu_error_t err = hu_json_stringify(alloc, args_val, &out->args_json, NULL);
        if (err != HU_OK) {
            alloc->free(alloc->ctx, out->tool_name, strlen(out->tool_name) + 1);
            return err;
        }
    } else {
        out->args_json = hu_strdup(alloc, "{}");
        if (!out->args_json) {
            alloc->free(alloc->ctx, out->tool_name, strlen(out->tool_name) + 1);
            return HU_ERR_OUT_OF_MEMORY;
        }
    }

    const char *desc = hu_json_get_string(step_obj, "description");
    out->description = desc ? hu_strdup(alloc, desc) : NULL;
    out->status = HU_PLAN_STEP_PENDING;

    return HU_OK;
}

hu_error_t hu_planner_create_plan(hu_allocator_t *alloc, const char *goal_json,
                                  size_t goal_json_len, hu_plan_t **out) {
    if (!alloc || !goal_json || !out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;

    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(alloc, goal_json, goal_json_len, &root);
    if (err != HU_OK)
        return err;

    hu_plan_t *plan = (hu_plan_t *)alloc->alloc(alloc->ctx, sizeof(hu_plan_t));
    if (!plan) {
        hu_json_free(alloc, root);
        return HU_ERR_OUT_OF_MEMORY;
    }
    plan->steps = NULL;
    plan->steps_count = 0;
    plan->steps_cap = 0;

    hu_json_value_t *steps_arr =
        root && root->type == HU_JSON_OBJECT ? hu_json_object_get(root, "steps") : NULL;

    if (!steps_arr || steps_arr->type != HU_JSON_ARRAY) {
        hu_json_free(alloc, root);
        alloc->free(alloc->ctx, plan, sizeof(hu_plan_t));
        return HU_ERR_INVALID_ARGUMENT;
    }

    size_t n = steps_arr->data.array.len;
    if (n == 0) {
        hu_json_free(alloc, root);
        *out = plan;
        return HU_OK;
    }

    hu_plan_step_t *steps = (hu_plan_step_t *)alloc->alloc(alloc->ctx, n * sizeof(hu_plan_step_t));
    if (!steps) {
        hu_json_free(alloc, root);
        alloc->free(alloc->ctx, plan, sizeof(hu_plan_t));
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(steps, 0, n * sizeof(hu_plan_step_t));

    for (size_t i = 0; i < n; i++) {
        hu_json_value_t *step_val = steps_arr->data.array.items[i];
        err = parse_step(alloc, step_val, &steps[i]);
        if (err != HU_OK) {
            for (size_t j = 0; j < i; j++) {
                if (steps[j].tool_name)
                    alloc->free(alloc->ctx, steps[j].tool_name, strlen(steps[j].tool_name) + 1);
                if (steps[j].args_json)
                    alloc->free(alloc->ctx, steps[j].args_json, strlen(steps[j].args_json) + 1);
                if (steps[j].description)
                    alloc->free(alloc->ctx, steps[j].description, strlen(steps[j].description) + 1);
            }
            alloc->free(alloc->ctx, steps, n * sizeof(hu_plan_step_t));
            hu_json_free(alloc, root);
            alloc->free(alloc->ctx, plan, sizeof(hu_plan_t));
            return err;
        }
    }

    hu_json_free(alloc, root);

    plan->steps = steps;
    plan->steps_count = n;
    plan->steps_cap = n;
    *out = plan;
    return HU_OK;
}

hu_plan_step_t *hu_planner_next_step(const hu_plan_t *plan) {
    if (!plan || !plan->steps)
        return NULL;
    for (size_t i = 0; i < plan->steps_count; i++) {
        if (plan->steps[i].status == HU_PLAN_STEP_PENDING)
            return (hu_plan_step_t *)&plan->steps[i];
    }
    return NULL;
}

void hu_planner_mark_step(hu_plan_t *plan, size_t index, hu_plan_step_status_t status) {
    if (!plan || !plan->steps || index >= plan->steps_count)
        return;
    plan->steps[index].status = status;
}

bool hu_planner_is_complete(const hu_plan_t *plan) {
    if (!plan || !plan->steps)
        return true;
    for (size_t i = 0; i < plan->steps_count; i++) {
        if (plan->steps[i].status == HU_PLAN_STEP_PENDING ||
            plan->steps[i].status == HU_PLAN_STEP_RUNNING)
            return false;
    }
    return true;
}

/* ── LLM-based plan generation ──────────────────────────────────────── */

#define HU_PLANNER_SYS_PREFIX                                                              \
    "You are a task planner. Decompose the user's goal into a sequence of tool calls.\n"   \
    "Return ONLY valid JSON with this exact format:\n"                                     \
    "{\"steps\":[{\"tool\":\"tool_name\",\"args\":{...},\"description\":\"what this step " \
    "does\"}]}\n"                                                                          \
    "Available tools: "

#define HU_PLANNER_SYS_SUFFIX "\nKeep plans minimal — fewest steps that accomplish the goal."

hu_error_t hu_planner_generate(hu_allocator_t *alloc, hu_provider_t *provider, const char *model,
                               size_t model_len, const char *goal, size_t goal_len,
                               const char *const *tool_names, size_t tool_count, hu_plan_t **out) {
    if (!alloc || !provider || !provider->vtable || !goal || !out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;

    /* Build system prompt: prefix + comma-separated tool names + suffix */
    size_t sys_cap = strlen(HU_PLANNER_SYS_PREFIX) + strlen(HU_PLANNER_SYS_SUFFIX) + 2;
    for (size_t i = 0; i < tool_count; i++)
        sys_cap += tool_names[i] ? strlen(tool_names[i]) + 2 : 0;

    char *sys = (char *)alloc->alloc(alloc->ctx, sys_cap);
    if (!sys)
        return HU_ERR_OUT_OF_MEMORY;

    size_t pos = 0;
    size_t plen = strlen(HU_PLANNER_SYS_PREFIX);
    memcpy(sys, HU_PLANNER_SYS_PREFIX, plen);
    pos = plen;

    for (size_t i = 0; i < tool_count; i++) {
        if (!tool_names[i])
            continue;
        if (i > 0) {
            sys[pos++] = ',';
            sys[pos++] = ' ';
        }
        size_t nlen = strlen(tool_names[i]);
        memcpy(sys + pos, tool_names[i], nlen);
        pos += nlen;
    }

    size_t slen = strlen(HU_PLANNER_SYS_SUFFIX);
    memcpy(sys + pos, HU_PLANNER_SYS_SUFFIX, slen);
    pos += slen;
    sys[pos] = '\0';

    /* Build chat request */
    hu_chat_message_t msgs[2];
    memset(msgs, 0, sizeof(msgs));
    msgs[0].role = HU_ROLE_SYSTEM;
    msgs[0].content = sys;
    msgs[0].content_len = pos;
    msgs[1].role = HU_ROLE_USER;
    msgs[1].content = goal;
    msgs[1].content_len = goal_len;

    hu_chat_request_t req;
    memset(&req, 0, sizeof(req));
    req.model = model;
    req.model_len = model_len;
    req.messages = msgs;
    req.messages_count = 2;
    req.temperature = 0.2;

    hu_chat_response_t resp;
    memset(&resp, 0, sizeof(resp));

#ifdef HU_IS_TEST
    /* In test mode, return a minimal valid plan without calling the LLM */
    (void)req;
    alloc->free(alloc->ctx, sys, sys_cap);
    const char *stub = "{\"steps\":[{\"tool\":\"shell\",\"args\":{\"command\":\"echo plan\"},"
                       "\"description\":\"stub plan step\"}]}";
    return hu_planner_create_plan(alloc, stub, strlen(stub), out);
#else
    hu_error_t err =
        provider->vtable->chat(provider->ctx, alloc, &req, model, model_len, 0.2, &resp);
    alloc->free(alloc->ctx, sys, sys_cap);

    if (err != HU_OK)
        return err;

    if (!resp.content || resp.content_len == 0) {
        hu_chat_response_free(alloc, &resp);
        return HU_ERR_INVALID_ARGUMENT;
    }

    /* Find the JSON object in the response (skip any markdown fences) */
    const char *json_start = resp.content;
    size_t json_len = resp.content_len;
    for (size_t i = 0; i < resp.content_len; i++) {
        if (resp.content[i] == '{') {
            json_start = resp.content + i;
            json_len = resp.content_len - i;
            break;
        }
    }

    err = hu_planner_create_plan(alloc, json_start, json_len, out);
    hu_chat_response_free(alloc, &resp);
    return err;
#endif
}

/* ── Replan after step failure ───────────────────────────────────────────── */

#define HU_REPLAN_SYS_PREFIX                                                          \
    "You are a task planner. A plan step failed. Create a REVISED plan to achieve "   \
    "the remaining goal.\nReturn ONLY valid JSON with this exact format:\n"           \
    "{\"steps\":[{\"tool\":\"tool_name\",\"args\":{...},\"description\":\"...\"}]}\n" \
    "Available tools: "

#define HU_REPLAN_SYS_SUFFIX "\nKeep plans minimal — fewest steps that accomplish the goal."

hu_error_t hu_planner_replan(hu_allocator_t *alloc, hu_provider_t *provider, const char *model,
                             size_t model_len, const char *original_goal, size_t original_goal_len,
                             const char *progress_summary, size_t progress_summary_len,
                             const char *failure_detail, size_t failure_detail_len,
                             const char *const *tool_names, size_t tool_count, hu_plan_t **out) {
    if (!alloc || !provider || !provider->vtable || !original_goal || !out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;

#ifdef HU_IS_TEST
    (void)model;
    (void)model_len;
    (void)progress_summary;
    (void)progress_summary_len;
    (void)failure_detail;
    (void)failure_detail_len;
    (void)tool_names;
    (void)tool_count;
    const char *stub = "{\"steps\":[{\"tool\":\"shell\",\"args\":{\"command\":\"echo replan\"},"
                       "\"description\":\"replanned step\"}]}";
    return hu_planner_create_plan(alloc, stub, strlen(stub), out);
#else
    /* Build system prompt: prefix + tool names + suffix */
    size_t sys_cap = strlen(HU_REPLAN_SYS_PREFIX) + strlen(HU_REPLAN_SYS_SUFFIX) + 2;
    for (size_t i = 0; i < tool_count; i++)
        sys_cap += tool_names[i] ? strlen(tool_names[i]) + 2 : 0;

    char *sys = (char *)alloc->alloc(alloc->ctx, sys_cap);
    if (!sys)
        return HU_ERR_OUT_OF_MEMORY;

    size_t pos = 0;
    size_t plen = strlen(HU_REPLAN_SYS_PREFIX);
    memcpy(sys, HU_REPLAN_SYS_PREFIX, plen);
    pos = plen;

    for (size_t i = 0; i < tool_count; i++) {
        if (!tool_names[i])
            continue;
        if (i > 0) {
            sys[pos++] = ',';
            sys[pos++] = ' ';
        }
        size_t nlen = strlen(tool_names[i]);
        memcpy(sys + pos, tool_names[i], nlen);
        pos += nlen;
    }

    size_t slen = strlen(HU_REPLAN_SYS_SUFFIX);
    memcpy(sys + pos, HU_REPLAN_SYS_SUFFIX, slen);
    pos += slen;
    sys[pos] = '\0';

    /* Build user message: goal + progress + failure */
    size_t user_cap = 256;
    if (original_goal_len > 0)
        user_cap += original_goal_len;
    if (progress_summary_len > 0)
        user_cap += progress_summary_len;
    if (failure_detail_len > 0)
        user_cap += failure_detail_len;

    char *user = (char *)alloc->alloc(alloc->ctx, user_cap);
    if (!user) {
        alloc->free(alloc->ctx, sys, sys_cap);
        return HU_ERR_OUT_OF_MEMORY;
    }

    int off = snprintf(user, user_cap, "The original goal was: %.*s\n", (int)original_goal_len,
                       original_goal);
    if (off < 0)
        off = 0;

    if (progress_summary && progress_summary_len > 0) {
        off += snprintf(user + off, user_cap - (size_t)off, "Progress so far: %.*s\n",
                        (int)progress_summary_len, progress_summary);
        if (off < 0)
            off = (int)(user_cap - 1);
    }

    if (failure_detail && failure_detail_len > 0) {
        off += snprintf(user + off, user_cap - (size_t)off, "The last step failed: %.*s\n",
                        (int)failure_detail_len, failure_detail);
        if (off < 0)
            off = (int)(user_cap - 1);
    }

    off += snprintf(user + off, user_cap - (size_t)off,
                    "Create a revised plan to achieve the remaining goal.");
    if (off < 0 || (size_t)off >= user_cap)
        off = (int)user_cap - 1;
    size_t user_len = (size_t)off;

    hu_chat_message_t msgs[2];
    memset(msgs, 0, sizeof(msgs));
    msgs[0].role = HU_ROLE_SYSTEM;
    msgs[0].content = sys;
    msgs[0].content_len = pos;
    msgs[1].role = HU_ROLE_USER;
    msgs[1].content = user;
    msgs[1].content_len = user_len;

    hu_chat_request_t req;
    memset(&req, 0, sizeof(req));
    req.model = model;
    req.model_len = model_len;
    req.messages = msgs;
    req.messages_count = 2;
    req.temperature = 0.2;

    hu_chat_response_t resp;
    memset(&resp, 0, sizeof(resp));

    hu_error_t err =
        provider->vtable->chat(provider->ctx, alloc, &req, model, model_len, 0.2, &resp);
    alloc->free(alloc->ctx, sys, sys_cap);
    alloc->free(alloc->ctx, user, user_cap);

    if (err != HU_OK)
        return err;

    if (!resp.content || resp.content_len == 0) {
        hu_chat_response_free(alloc, &resp);
        return HU_ERR_INVALID_ARGUMENT;
    }

    const char *json_start = resp.content;
    size_t json_len = resp.content_len;
    for (size_t i = 0; i < resp.content_len; i++) {
        if (resp.content[i] == '{') {
            json_start = resp.content + i;
            json_len = resp.content_len - i;
            break;
        }
    }

    err = hu_planner_create_plan(alloc, json_start, json_len, out);
    hu_chat_response_free(alloc, &resp);
    return err;
#endif
}

void hu_plan_free(hu_allocator_t *alloc, hu_plan_t *plan) {
    if (!alloc || !plan)
        return;
    if (plan->steps) {
        for (size_t i = 0; i < plan->steps_count; i++) {
            if (plan->steps[i].tool_name)
                alloc->free(alloc->ctx, plan->steps[i].tool_name,
                            strlen(plan->steps[i].tool_name) + 1);
            if (plan->steps[i].args_json)
                alloc->free(alloc->ctx, plan->steps[i].args_json,
                            strlen(plan->steps[i].args_json) + 1);
            if (plan->steps[i].description)
                alloc->free(alloc->ctx, plan->steps[i].description,
                            strlen(plan->steps[i].description) + 1);
        }
        alloc->free(alloc->ctx, plan->steps, plan->steps_cap * sizeof(hu_plan_step_t));
    }
    alloc->free(alloc->ctx, plan, sizeof(hu_plan_t));
}
