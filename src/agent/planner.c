#include "seaclaw/agent/planner.h"
#include "seaclaw/core/string.h"
#include <string.h>
#include <stdlib.h>

static sc_error_t parse_step(sc_allocator_t *alloc, const sc_json_value_t *step_obj,
    sc_plan_step_t *out)
{
    if (!step_obj || step_obj->type != SC_JSON_OBJECT) return SC_ERR_INVALID_ARGUMENT;

    const char *name = sc_json_get_string(step_obj, "tool");
    if (!name) name = sc_json_get_string(step_obj, "name");
    if (!name || strlen(name) == 0) return SC_ERR_INVALID_ARGUMENT;

    out->tool_name = sc_strdup(alloc, name);
    if (!out->tool_name) return SC_ERR_OUT_OF_MEMORY;

    out->args_json = NULL;
    sc_json_value_t *args_val = sc_json_object_get(step_obj, "args");
    if (!args_val) args_val = sc_json_object_get(step_obj, "arguments");
    if (args_val) {
        sc_error_t err = sc_json_stringify(alloc, args_val, &out->args_json, NULL);
        if (err != SC_OK) {
            alloc->free(alloc->ctx, out->tool_name, strlen(out->tool_name) + 1);
            return err;
        }
    } else {
        out->args_json = sc_strdup(alloc, "{}");
        if (!out->args_json) {
            alloc->free(alloc->ctx, out->tool_name, strlen(out->tool_name) + 1);
            return SC_ERR_OUT_OF_MEMORY;
        }
    }

    const char *desc = sc_json_get_string(step_obj, "description");
    out->description = desc ? sc_strdup(alloc, desc) : NULL;
    out->status = SC_PLAN_STEP_PENDING;

    return SC_OK;
}

sc_error_t sc_planner_create_plan(sc_allocator_t *alloc,
    const char *goal_json, size_t goal_json_len,
    sc_plan_t **out)
{
    if (!alloc || !goal_json || !out) return SC_ERR_INVALID_ARGUMENT;
    *out = NULL;

    sc_json_value_t *root = NULL;
    sc_error_t err = sc_json_parse(alloc, goal_json, goal_json_len, &root);
    if (err != SC_OK) return err;

    sc_plan_t *plan = (sc_plan_t *)alloc->alloc(alloc->ctx, sizeof(sc_plan_t));
    if (!plan) {
        sc_json_free(alloc, root);
        return SC_ERR_OUT_OF_MEMORY;
    }
    plan->steps = NULL;
    plan->steps_count = 0;
    plan->steps_cap = 0;

    sc_json_value_t *steps_arr = root && root->type == SC_JSON_OBJECT
        ? sc_json_object_get(root, "steps")
        : NULL;

    if (!steps_arr || steps_arr->type != SC_JSON_ARRAY) {
        sc_json_free(alloc, root);
        alloc->free(alloc->ctx, plan, sizeof(sc_plan_t));
        return SC_ERR_INVALID_ARGUMENT;
    }

    size_t n = steps_arr->data.array.len;
    if (n == 0) {
        sc_json_free(alloc, root);
        *out = plan;
        return SC_OK;
    }

    sc_plan_step_t *steps = (sc_plan_step_t *)alloc->alloc(alloc->ctx,
        n * sizeof(sc_plan_step_t));
    if (!steps) {
        sc_json_free(alloc, root);
        alloc->free(alloc->ctx, plan, sizeof(sc_plan_t));
        return SC_ERR_OUT_OF_MEMORY;
    }
    memset(steps, 0, n * sizeof(sc_plan_step_t));

    for (size_t i = 0; i < n; i++) {
        sc_json_value_t *step_val = steps_arr->data.array.items[i];
        err = parse_step(alloc, step_val, &steps[i]);
        if (err != SC_OK) {
            for (size_t j = 0; j < i; j++) {
                if (steps[j].tool_name) alloc->free(alloc->ctx, steps[j].tool_name, strlen(steps[j].tool_name) + 1);
                if (steps[j].args_json) alloc->free(alloc->ctx, steps[j].args_json, strlen(steps[j].args_json) + 1);
                if (steps[j].description) alloc->free(alloc->ctx, steps[j].description, strlen(steps[j].description) + 1);
            }
            alloc->free(alloc->ctx, steps, n * sizeof(sc_plan_step_t));
            sc_json_free(alloc, root);
            alloc->free(alloc->ctx, plan, sizeof(sc_plan_t));
            return err;
        }
    }

    sc_json_free(alloc, root);

    plan->steps = steps;
    plan->steps_count = n;
    plan->steps_cap = n;
    *out = plan;
    return SC_OK;
}

sc_plan_step_t *sc_planner_next_step(const sc_plan_t *plan) {
    if (!plan || !plan->steps) return NULL;
    for (size_t i = 0; i < plan->steps_count; i++) {
        if (plan->steps[i].status == SC_PLAN_STEP_PENDING)
            return (sc_plan_step_t *)&plan->steps[i];
    }
    return NULL;
}

void sc_planner_mark_step(sc_plan_t *plan, size_t index, sc_plan_step_status_t status) {
    if (!plan || !plan->steps || index >= plan->steps_count) return;
    plan->steps[index].status = status;
}

bool sc_planner_is_complete(const sc_plan_t *plan) {
    if (!plan || !plan->steps) return true;
    for (size_t i = 0; i < plan->steps_count; i++) {
        if (plan->steps[i].status == SC_PLAN_STEP_PENDING ||
            plan->steps[i].status == SC_PLAN_STEP_RUNNING)
            return false;
    }
    return true;
}

void sc_plan_free(sc_allocator_t *alloc, sc_plan_t *plan) {
    if (!alloc || !plan) return;
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
        alloc->free(alloc->ctx, plan->steps, plan->steps_cap * sizeof(sc_plan_step_t));
    }
    alloc->free(alloc->ctx, plan, sizeof(sc_plan_t));
}
