#include "human/agent/planner.h"
#include "human/agent/mcts_planner.h"
#include "human/agent/orchestrator_llm.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include <limits.h>
#include <stdint.h>
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

    out->depends_count = 0;
    hu_json_value_t *deps_val = hu_json_object_get(step_obj, "depends_on");
    if (deps_val && deps_val->type == HU_JSON_ARRAY) {
        size_t dn = deps_val->data.array.len;
        if (dn > HU_PLAN_STEP_MAX_DEPS)
            dn = HU_PLAN_STEP_MAX_DEPS;
        for (size_t di = 0; di < dn; di++) {
            hu_json_value_t *v = deps_val->data.array.items[di];
            if (v && v->type == HU_JSON_NUMBER && out->depends_count < HU_PLAN_STEP_MAX_DEPS) {
                double d = v->data.number;
                if (d >= 0.0 && d <= (double)INT_MAX)
                    out->depends_on[out->depends_count++] = (int)d;
            }
        }
    } else if (deps_val && deps_val->type == HU_JSON_NUMBER &&
               out->depends_count < HU_PLAN_STEP_MAX_DEPS) {
        double d = deps_val->data.number;
        if (d >= 0.0 && d <= (double)INT_MAX)
            out->depends_on[out->depends_count++] = (int)d;
    }

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
    "does\",\"depends_on\":[]}]}\n"                                                        \
    "depends_on is optional ([] or omitted = none); otherwise 0-based indices of prior "   \
    "steps this step depends on. "                                                         \
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

bool hu_planner_plan_needs_mcts(const hu_plan_t *plan) {
    if (!plan || plan->steps_count < 5)
        return false;
    for (size_t i = 0; i < plan->steps_count; i++) {
        if (plan->steps[i].depends_count > 0)
            return true;
    }
    return false;
}

/* Build structured plan JSON from MCTS action strings ("tool: description" or plain text). */
static hu_error_t planner_create_plan_from_mcts(hu_allocator_t *alloc, const hu_mcts_result_t *mr,
                                                hu_plan_t **out) {
    if (!alloc || !mr || !out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;

    hu_json_value_t *root = hu_json_object_new(alloc);
    if (!root)
        return HU_ERR_OUT_OF_MEMORY;
    hu_json_value_t *steps_arr = hu_json_array_new(alloc);
    if (!steps_arr) {
        hu_json_free(alloc, root);
        return HU_ERR_OUT_OF_MEMORY;
    }
    hu_error_t err = hu_json_object_set(alloc, root, "steps", steps_arr);
    if (err != HU_OK) {
        hu_json_free(alloc, steps_arr);
        hu_json_free(alloc, root);
        return err;
    }

    for (size_t ai = 0; ai < mr->action_count; ai++) {
        const char *action = mr->actions[ai];
        size_t alen = mr->action_lens[ai];
        if (!action || alen == 0)
            continue;

        hu_json_value_t *step_obj = hu_json_object_new(alloc);
        hu_json_value_t *args_obj = hu_json_object_new(alloc);
        if (!step_obj || !args_obj) {
            if (step_obj)
                hu_json_free(alloc, step_obj);
            if (args_obj)
                hu_json_free(alloc, args_obj);
            hu_json_free(alloc, root);
            return HU_ERR_OUT_OF_MEMORY;
        }

        const char *colon = memchr(action, ':', alen);
        hu_json_value_t *tool_val;
        hu_json_value_t *desc_val;

        if (colon && (size_t)(colon - action) > 0 && (size_t)(colon - action) < 64) {
            size_t tool_len = (size_t)(colon - action);
            const char *desc_start = colon + 1;
            size_t rest = alen - tool_len - 1;
            if (rest > 0 && desc_start[0] == ' ') {
                desc_start++;
                rest--;
            }
            tool_val = hu_json_string_new(alloc, action, tool_len);
            desc_val = hu_json_string_new(alloc, desc_start, rest);
        } else {
            size_t desc_clip = alen < 200 ? alen : 200;
            tool_val = hu_json_string_new(alloc, "shell", 5);
            desc_val = hu_json_string_new(alloc, action, desc_clip);
        }

        if (!tool_val || !desc_val) {
            if (tool_val)
                hu_json_free(alloc, tool_val);
            if (desc_val)
                hu_json_free(alloc, desc_val);
            hu_json_free(alloc, args_obj);
            hu_json_free(alloc, step_obj);
            hu_json_free(alloc, root);
            return HU_ERR_OUT_OF_MEMORY;
        }

        err = hu_json_object_set(alloc, step_obj, "tool", tool_val);
        if (err != HU_OK) {
            hu_json_free(alloc, tool_val);
            hu_json_free(alloc, desc_val);
            hu_json_free(alloc, args_obj);
            hu_json_free(alloc, step_obj);
            hu_json_free(alloc, root);
            return err;
        }
        err = hu_json_object_set(alloc, step_obj, "args", args_obj);
        if (err != HU_OK) {
            hu_json_free(alloc, desc_val);
            hu_json_free(alloc, step_obj);
            hu_json_free(alloc, root);
            return err;
        }
        err = hu_json_object_set(alloc, step_obj, "description", desc_val);
        if (err != HU_OK) {
            hu_json_free(alloc, step_obj);
            hu_json_free(alloc, root);
            return err;
        }
        err = hu_json_array_push(alloc, steps_arr, step_obj);
        if (err != HU_OK) {
            hu_json_free(alloc, step_obj);
            hu_json_free(alloc, root);
            return err;
        }
    }

    char *json_str = NULL;
    size_t json_len = 0;
    err = hu_json_stringify(alloc, root, &json_str, &json_len);
    hu_json_free(alloc, root);
    if (err != HU_OK)
        return err;

    err = hu_planner_create_plan(alloc, json_str, json_len, out);
    alloc->free(alloc->ctx, json_str, json_len + 1);
    return err;
}

hu_error_t hu_planner_plan_mcts(hu_allocator_t *alloc, hu_provider_t *provider, const char *model,
                                size_t model_len, const char *goal, size_t goal_len,
                                const char *const *tool_names, size_t tool_count, hu_plan_t **out) {
    if (!alloc || !goal || !out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    if (!provider || !provider->vtable)
        return hu_planner_generate(alloc, provider, model, model_len, goal, goal_len, tool_names,
                                   tool_count, out);

    char ctx_buf[512];
    size_t cp = 0;
    for (size_t i = 0; i < tool_count && cp + 1 < sizeof(ctx_buf); i++) {
        if (!tool_names[i])
            continue;
        size_t nl = strlen(tool_names[i]);
        if (cp > 0) {
            if (cp + 2 >= sizeof(ctx_buf))
                break;
            ctx_buf[cp++] = ',';
            ctx_buf[cp++] = ' ';
        }
        if (cp + nl >= sizeof(ctx_buf))
            break;
        memcpy(ctx_buf + cp, tool_names[i], nl);
        cp += nl;
    }
    ctx_buf[cp] = '\0';

    hu_mcts_config_t mcfg = hu_mcts_config_default();
#ifndef HU_IS_TEST
    mcfg.provider = provider;
    mcfg.model = model;
    mcfg.model_len = model_len;
#endif

    hu_mcts_result_t mr;
    memset(&mr, 0, sizeof(mr));
    hu_error_t merr = hu_mcts_plan(alloc, goal, goal_len, ctx_buf, cp, &mcfg, &mr);
    if (merr != HU_OK || mr.best_action_len == 0) {
        hu_mcts_result_free_path(alloc, &mr);
        return hu_planner_generate(alloc, provider, model, model_len, goal, goal_len, tool_names,
                                   tool_count, out);
    }

    if (mr.best_value > 0.7 && mr.actions && mr.action_count > 0) {
        hu_plan_t *direct = NULL;
        hu_error_t perr = planner_create_plan_from_mcts(alloc, &mr, &direct);
        if (perr == HU_OK && direct) {
            hu_mcts_result_free_path(alloc, &mr);
            *out = direct;
            return HU_OK;
        }
        if (direct)
            hu_plan_free(alloc, direct);
    }
    hu_mcts_result_free_path(alloc, &mr);

    static const char prefix[] = "\n\n[MCTS suggested first focus]: ";
    const size_t prefix_len = sizeof(prefix) - 1;
    if (mr.best_action_len > SIZE_MAX - prefix_len ||
        goal_len > SIZE_MAX - prefix_len - mr.best_action_len - 1)
        return hu_planner_generate(alloc, provider, model, model_len, goal, goal_len, tool_names,
                                   tool_count, out);

    size_t aug_len = goal_len + prefix_len + mr.best_action_len;
    size_t aug_cap = aug_len + 1;
    char *aug = (char *)alloc->alloc(alloc->ctx, aug_cap);
    if (!aug)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(aug, goal, goal_len);
    memcpy(aug + goal_len, prefix, prefix_len);
    memcpy(aug + goal_len + prefix_len, mr.best_action, mr.best_action_len);
    aug[aug_len] = '\0';

    hu_error_t gerr = hu_planner_generate(alloc, provider, model, model_len, aug, aug_len,
                                          tool_names, tool_count, out);
    alloc->free(alloc->ctx, aug, aug_cap);
    return gerr;
}

/* ── Replan after step failure ───────────────────────────────────────────── */

#define HU_REPLAN_SYS_PREFIX                                                        \
    "You are a task planner. A plan step failed. Create a REVISED plan to achieve " \
    "the remaining goal.\nReturn ONLY valid JSON with this exact format:\n"         \
    "{\"steps\":[{\"tool\":\"tool_name\",\"args\":{...},\"description\":\"...\",\"" \
    "depends_on\":[]}]}\n"                                                          \
    "depends_on is optional ([] or omitted = none); otherwise 0-based indices of "  \
    "prior steps. "                                                                 \
    "Available tools: "

#define HU_REPLAN_SYS_SUFFIX "\nKeep plans minimal — fewest steps that accomplish the goal."

hu_error_t hu_planner_replan(hu_allocator_t *alloc, hu_provider_t *provider, const char *model,
                             size_t model_len, const char *original_goal, size_t original_goal_len,
                             const char *progress_summary, size_t progress_summary_len,
                             const char *failure_detail, size_t failure_detail_len,
                             const char *const *tool_names, size_t tool_count, hu_plan_t **out) {
    (void)original_goal_len;
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

    size_t off = hu_buf_appendf(user, user_cap, 0, "The original goal was: %.*s\n",
                                (int)original_goal_len, original_goal);

    if (progress_summary && progress_summary_len > 0) {
        off = hu_buf_appendf(user, user_cap, off, "Progress so far: %.*s\n",
                             (int)progress_summary_len, progress_summary);
    }

    if (failure_detail && failure_detail_len > 0) {
        off = hu_buf_appendf(user, user_cap, off, "The last step failed: %.*s\n",
                             (int)failure_detail_len, failure_detail);
    }

    off =
        hu_buf_appendf(user, user_cap, off, "Create a revised plan to achieve the remaining goal.");
    size_t user_len = off;

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
