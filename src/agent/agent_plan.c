/* Plan execution: sc_agent_commands_execute_plan_steps, sc_agent_execute_plan */
#include "agent_internal.h"
#include "seaclaw/agent/commands.h"
#include "seaclaw/agent/planner.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/observer.h"
#include "seaclaw/tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

sc_error_t sc_agent_commands_execute_plan_steps(sc_agent_t *agent, sc_plan_t *plan,
                                                char **summary_out, size_t *summary_len_out,
                                                const char *original_goal,
                                                size_t original_goal_len) {
    sc_agent_internal_generate_trace_id(agent->trace_id);
    char result_buf[4096];
    int result_off = 0;
    bool replanned = false;
    result_off += snprintf(result_buf + result_off, sizeof(result_buf) - (size_t)result_off,
                           "Plan: %zu steps\n", plan->steps_count);

    for (size_t i = 0; i < plan->steps_count; i++) {
        if (agent->cancel_requested) {
            sc_planner_mark_step(plan, i, SC_PLAN_STEP_FAILED);
            result_off += snprintf(result_buf + result_off, sizeof(result_buf) - (size_t)result_off,
                                   "  [%zu] %s: CANCELLED\n", i + 1, plan->steps[i].tool_name);
            continue;
        }

        sc_planner_mark_step(plan, i, SC_PLAN_STEP_RUNNING);

        {
            sc_observer_event_t ev = {.tag = SC_OBSERVER_EVENT_TOOL_CALL_START, .data = {{0}}};
            ev.data.tool_call_start.tool = plan->steps[i].tool_name;
            SC_OBS_SAFE_RECORD_EVENT(agent, &ev);
        }

        sc_tool_t *tool = sc_agent_internal_find_tool(agent, plan->steps[i].tool_name,
                                                      strlen(plan->steps[i].tool_name));
        if (!tool) {
            sc_planner_mark_step(plan, i, SC_PLAN_STEP_FAILED);
            result_off += snprintf(result_buf + result_off, sizeof(result_buf) - (size_t)result_off,
                                   "  [%zu] %s: tool not found\n", i + 1, plan->steps[i].tool_name);
            {
                sc_observer_event_t ev = {.tag = SC_OBSERVER_EVENT_TOOL_CALL, .data = {{0}}};
                ev.data.tool_call.tool = plan->steps[i].tool_name;
                ev.data.tool_call.success = false;
                ev.data.tool_call.detail = "tool not found";
                SC_OBS_SAFE_RECORD_EVENT(agent, &ev);
            }
            continue;
        }

        sc_json_value_t *args = NULL;
        if (plan->steps[i].args_json) {
            size_t args_len = strlen(plan->steps[i].args_json);
            sc_error_t pe = sc_json_parse(agent->alloc, plan->steps[i].args_json, args_len, &args);
            if (pe != SC_OK)
                args = NULL;
        }

        sc_tool_result_t result = sc_tool_result_fail("invalid arguments", 16);
        clock_t tool_start = clock();
        if (args) {
            tool->vtable->execute(tool->ctx, agent->alloc, args, &result);
            sc_json_free(agent->alloc, args);
        }
        uint64_t tool_duration_ms = sc_agent_internal_clock_diff_ms(tool_start, clock());

        bool ok = result.success;
        sc_planner_mark_step(plan, i, ok ? SC_PLAN_STEP_DONE : SC_PLAN_STEP_FAILED);

        {
            sc_observer_event_t ev = {.tag = SC_OBSERVER_EVENT_TOOL_CALL, .data = {{0}}};
            ev.data.tool_call.tool = plan->steps[i].tool_name;
            ev.data.tool_call.duration_ms = tool_duration_ms;
            ev.data.tool_call.success = ok;
            ev.data.tool_call.detail = ok ? NULL : (result.error_msg ? result.error_msg : "failed");
            SC_OBS_SAFE_RECORD_EVENT(agent, &ev);
        }

        const char *desc =
            plan->steps[i].description ? plan->steps[i].description : plan->steps[i].tool_name;
        result_off += snprintf(result_buf + result_off, sizeof(result_buf) - (size_t)result_off,
                               "  [%zu] %s: %s (%llums)\n", i + 1, desc, ok ? "done" : "FAILED",
                               (unsigned long long)tool_duration_ms);

        /* Replan on failure: one attempt, only when original goal is available */
        if (!ok && original_goal && original_goal_len > 0 && !replanned && agent->provider.vtable &&
            agent->model_name) {
            char progress_buf[2048];
            int prog_off = 0;
            for (size_t j = 0; j < i && prog_off < (int)sizeof(progress_buf) - 64; j++) {
                if (plan->steps[j].status == SC_PLAN_STEP_DONE && plan->steps[j].description) {
                    prog_off +=
                        snprintf(progress_buf + prog_off, sizeof(progress_buf) - (size_t)prog_off,
                                 "  [%zu] %s: done\n", j + 1, plan->steps[j].description);
                }
            }
            if (prog_off <= 0)
                prog_off = snprintf(progress_buf, sizeof(progress_buf), "(none)");

            char fail_buf[512];
            int fail_off = snprintf(fail_buf, sizeof(fail_buf), "%s: %s", plan->steps[i].tool_name,
                                    result.error_msg ? result.error_msg : "failed");
            if (fail_off < 0)
                fail_off = 0;

            const char **tool_names = NULL;
            size_t tn_count = 0;
            if (agent->tools_count > 0) {
                tool_names = (const char **)agent->alloc->alloc(
                    agent->alloc->ctx, agent->tools_count * sizeof(const char *));
                if (tool_names) {
                    for (size_t k = 0; k < agent->tools_count; k++) {
                        const char *tn = agent->tools[k].vtable->name
                                             ? agent->tools[k].vtable->name(agent->tools[k].ctx)
                                             : NULL;
                        if (tn)
                            tool_names[tn_count++] = tn;
                    }
                }
            }

            sc_plan_t *new_plan = NULL;
            sc_error_t replan_err = sc_planner_replan(
                agent->alloc, &agent->provider, agent->model_name, agent->model_name_len,
                original_goal, original_goal_len, progress_buf, (size_t)prog_off, fail_buf,
                (size_t)fail_off, tool_names, tn_count, &new_plan);

            if (tool_names)
                agent->alloc->free(agent->alloc->ctx, (void *)tool_names,
                                   agent->tools_count * sizeof(const char *));

            if (replan_err == SC_OK && new_plan && new_plan->steps_count > 0) {
                /* Free old steps from i+1 onward */
                for (size_t j = i + 1; j < plan->steps_count; j++) {
                    if (plan->steps[j].tool_name)
                        agent->alloc->free(agent->alloc->ctx, plan->steps[j].tool_name,
                                           strlen(plan->steps[j].tool_name) + 1);
                    if (plan->steps[j].args_json)
                        agent->alloc->free(agent->alloc->ctx, plan->steps[j].args_json,
                                           strlen(plan->steps[j].args_json) + 1);
                    if (plan->steps[j].description)
                        agent->alloc->free(agent->alloc->ctx, plan->steps[j].description,
                                           strlen(plan->steps[j].description) + 1);
                }

                size_t new_total = i + 1 + new_plan->steps_count;
                if (new_total > plan->steps_cap) {
                    size_t new_cap = new_total;
                    sc_plan_step_t *new_steps = (sc_plan_step_t *)agent->alloc->alloc(
                        agent->alloc->ctx, new_cap * sizeof(sc_plan_step_t));
                    if (new_steps) {
                        memcpy(new_steps, plan->steps, (i + 1) * sizeof(sc_plan_step_t));
                        agent->alloc->free(agent->alloc->ctx, plan->steps,
                                           plan->steps_cap * sizeof(sc_plan_step_t));
                        plan->steps = new_steps;
                        plan->steps_cap = new_cap;
                    }
                }

                if (plan->steps_cap >= new_total) {
                    for (size_t j = 0; j < new_plan->steps_count; j++) {
                        sc_plan_step_t *dst = &plan->steps[i + 1 + j];
                        dst->tool_name = sc_strdup(agent->alloc, new_plan->steps[j].tool_name);
                        dst->args_json = new_plan->steps[j].args_json
                                             ? sc_strdup(agent->alloc, new_plan->steps[j].args_json)
                                             : NULL;
                        dst->description =
                            new_plan->steps[j].description
                                ? sc_strdup(agent->alloc, new_plan->steps[j].description)
                                : NULL;
                        dst->status = SC_PLAN_STEP_PENDING;
                    }
                    plan->steps_count = i + 1 + new_plan->steps_count;
                    replanned = true;
                    result_off +=
                        snprintf(result_buf + result_off, sizeof(result_buf) - (size_t)result_off,
                                 "  [replan] %zu new steps\n", new_plan->steps_count);
                }

                sc_plan_free(agent->alloc, new_plan);
            }
        }

        sc_tool_result_free(agent->alloc, &result);
    }

    *summary_out = sc_strndup(agent->alloc, result_buf, (size_t)result_off);
    if (!*summary_out)
        return SC_ERR_OUT_OF_MEMORY;
    if (summary_len_out)
        *summary_len_out = (size_t)result_off;
    return SC_OK;
}

sc_error_t sc_agent_execute_plan(sc_agent_t *agent, const char *plan_json, size_t plan_json_len,
                                 char **summary_out, size_t *summary_len_out) {
    if (!agent || !plan_json || !summary_out)
        return SC_ERR_INVALID_ARGUMENT;
    *summary_out = NULL;
    if (summary_len_out)
        *summary_len_out = 0;

    sc_plan_t *plan = NULL;
    sc_error_t err = sc_planner_create_plan(agent->alloc, plan_json, plan_json_len, &plan);
    if (err != SC_OK)
        return err;

    err = sc_agent_commands_execute_plan_steps(agent, plan, summary_out, summary_len_out, NULL, 0);
    sc_plan_free(agent->alloc, plan);
    return err;
}
