/* src/agent/turn/turn_tools.c — S16 tool dispatch, carved verbatim out of
 * hu_agent_turn (src/agent/agent_turn.c) by the phase-1 carve
 * (docs/superpowers/plans/2026-09-30-agent-turn-carve-phase1.md), together
 * with the statics only it used (the parallel-DAG worker and its mutex, the
 * HuLa compiler completion hook, the HuLa IR audit JSON).
 *
 * Three regions run only in daemon builds (`#ifndef HU_IS_TEST`: HuLa compiler
 * + LLMCompiler DAG, the parallel DAG batch, native HuLa IR); the suite cannot
 * reach them, so tests/test_turn_sources.c pins their presence. Two borrowed
 * SQLite handles (world-model ordering, per-tool learning) are passed to the
 * module APIs that own the SQL; the sqlite3 type comes through human/memory.h,
 * never a direct <sqlite3.h> include (plan gap G2). */
#include "../agent_internal.h"
#include "human/agent/awareness.h"
#include "human/agent/dag_executor.h"
#include "human/agent/dispatcher.h"
#include "human/agent/hula_compiler.h"
#include "human/agent/hula_emergence.h"
#include "human/agent/llm_compiler.h"
#include "human/agent/orchestrator_llm.h"
#include "human/agent/outcomes.h"
#include "human/agent/swarm.h"
#include "human/agent/turn.h"
#include "human/core/log.h"
#include "human/core/string.h"
#include "human/security/causal_armor.h"
#include "human/security/history_scorer.h"
#include "human/tools/cache_ttl.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef HU_ENABLE_SQLITE
#include "human/experience.h"
#include "human/intelligence/online_learning.h"
#include "human/intelligence/self_improve.h"
#include "human/intelligence/world_model.h"
#if defined(HU_ENABLE_ML)
#include "human/ml/training_data.h"
#endif
#endif

#if (defined(__unix__) || defined(__APPLE__)) && !defined(HU_IS_TEST)
#include <pthread.h>

typedef struct dag_parallel_work {
    hu_agent_t *agent;
    hu_dag_node_t *node;
    hu_dag_t *dag;
} dag_parallel_work_t;

/* Thread-safety invariant: only tool->execute runs in parallel.
 * Agent lookup, variable resolution, and result collection are
 * serialized by this mutex. Only the actual tool execution benefits
 * from parallelism (which is where the latency savings come from,
 * since tools typically do I/O). */
static pthread_mutex_t g_dag_parallel_prep_mutex = PTHREAD_MUTEX_INITIALIZER;

#ifndef HU_IS_TEST
static void hula_compiler_agent_done(void *ctx, const hu_hula_program_t *prog,
                                     const hu_hula_exec_t *exec) {
    hu_agent_t *agent = ctx;
    hu_agent_internal_hula_append_histories(agent, prog, exec);
    hu_bth_metrics_record_hula_tool_turn(agent->bth_metrics);
}

/* Audit JSON for HuLa IR path: provider native tool_calls folded into a HuLa program. */
static char *agent_turn_hula_ir_tool_calls_audit_json(hu_allocator_t *alloc,
                                                      const hu_tool_call_t *calls, size_t tc_count,
                                                      size_t *out_len) {
    *out_len = 0;
    if (!alloc || !calls || tc_count == 0)
        return NULL;

    hu_json_value_t *root = hu_json_object_new(alloc);
    if (!root)
        return NULL;
    bool oom = false;
    bool calls_attached = false;
    static const char src_lit[] = "native_tool_calls_ir";
    {
        hu_json_value_t *src_val = hu_json_string_new(alloc, src_lit, sizeof(src_lit) - 1);
        if (!src_val || hu_json_object_set(alloc, root, "source", src_val) != HU_OK) {
            if (src_val)
                hu_json_free(alloc, src_val);
            oom = true;
        }
    }

    hu_json_value_t *arr = hu_json_array_new(alloc);
    if (!arr)
        oom = true;

    if (!oom) {
        for (size_t i = 0; i < tc_count; i++) {
            hu_json_value_t *o = hu_json_object_new(alloc);
            if (!o) {
                oom = true;
                break;
            }
            if (calls[i].name && calls[i].name_len > 0) {
                hu_json_value_t *tn = hu_json_string_new(alloc, calls[i].name, calls[i].name_len);
                if (!tn || hu_json_object_set(alloc, o, "tool", tn) != HU_OK) {
                    if (tn)
                        hu_json_free(alloc, tn);
                    hu_json_free(alloc, o);
                    oom = true;
                    break;
                }
            }
            if (calls[i].arguments && calls[i].arguments_len > 0) {
                hu_json_value_t *args_parsed = NULL;
                if (hu_json_parse(alloc, calls[i].arguments, calls[i].arguments_len,
                                  &args_parsed) == HU_OK &&
                    args_parsed) {
                    if (hu_json_object_set(alloc, o, "arguments", args_parsed) != HU_OK) {
                        hu_json_free(alloc, args_parsed);
                        hu_json_free(alloc, o);
                        oom = true;
                        break;
                    }
                } else {
                    hu_json_value_t *raw =
                        hu_json_string_new(alloc, calls[i].arguments, calls[i].arguments_len);
                    if (!raw || hu_json_object_set(alloc, o, "arguments_raw", raw) != HU_OK) {
                        if (raw)
                            hu_json_free(alloc, raw);
                        hu_json_free(alloc, o);
                        oom = true;
                        break;
                    }
                }
            }
            if (hu_json_array_push(alloc, arr, o) != HU_OK) {
                hu_json_free(alloc, o);
                oom = true;
                break;
            }
        }
    }
    if (!oom) {
        if (hu_json_object_set(alloc, root, "calls", arr) != HU_OK)
            oom = true;
        else
            calls_attached = true;
    }

    if (oom) {
        if (arr && !calls_attached)
            hu_json_free(alloc, arr);
        hu_json_free(alloc, root);
        return NULL;
    }

    char *body = NULL;
    size_t blen = 0;
    hu_error_t se = hu_json_stringify(alloc, root, &body, &blen);
    hu_json_free(alloc, root);
    if (se != HU_OK || !body)
        return NULL;
    *out_len = blen;
    return body;
}
#endif

static void *dag_parallel_worker(void *arg) {
    dag_parallel_work_t *w = (dag_parallel_work_t *)arg;
    hu_dag_node_t *node = w->node;
    node->status = HU_DAG_RUNNING;

    char *resolved_args = NULL;
    size_t resolved_len = 0;
    const char *use_args = node->args_json;
    hu_tool_t *dag_tool = NULL;
    pthread_mutex_lock(&g_dag_parallel_prep_mutex);
    if (node->args_json) {
        if (hu_dag_resolve_vars(w->agent->alloc, w->dag, node->args_json, strlen(node->args_json),
                                &resolved_args, &resolved_len) == HU_OK &&
            resolved_args)
            use_args = resolved_args;
    }

    dag_tool = hu_agent_internal_find_tool(w->agent, node->tool_name,
                                           node->tool_name ? strlen(node->tool_name) : 0);

    /* Serialize the security gate (hooks/policy are not thread-safe). */
    hu_tool_result_t dag_result = {0};
    hu_tool_gate_t gate = HU_TOOL_GATE_DENY;
    if (dag_tool && dag_tool->vtable && node->tool_name) {
        const char *gate_args = use_args ? use_args : "";
        gate =
            hu_agent_internal_pre_execute_checks(w->agent, node->tool_name, strlen(node->tool_name),
                                                 gate_args, strlen(gate_args), &dag_result);
    }
    pthread_mutex_unlock(&g_dag_parallel_prep_mutex);

    if (!dag_tool || !dag_tool->vtable) {
        node->status = HU_DAG_FAILED;
        if (resolved_args)
            w->agent->alloc->free(w->agent->alloc->ctx, resolved_args, resolved_len + 1);
        return NULL;
    }

    if (gate != HU_TOOL_GATE_ALLOW) {
        node->status = HU_DAG_FAILED;
        pthread_mutex_lock(&g_dag_parallel_prep_mutex);
        hu_agent_internal_post_hook_fire(w->agent, node->tool_name, strlen(node->tool_name),
                                         use_args ? use_args : "", use_args ? strlen(use_args) : 0,
                                         &dag_result);
        pthread_mutex_unlock(&g_dag_parallel_prep_mutex);
        hu_tool_result_free(w->agent->alloc, &dag_result);
        if (resolved_args)
            w->agent->alloc->free(w->agent->alloc->ctx, resolved_args, resolved_len + 1);
        return NULL;
    }

    hu_json_value_t *dag_args = NULL;
    if (use_args) {
        hu_error_t jerr = hu_json_parse(w->agent->alloc, use_args, strlen(use_args), &dag_args);
        if (jerr != HU_OK)
            hu_log_error("agent_turn", NULL, "DAG tool args parse failed");
    }
    if (dag_args && dag_tool->vtable->execute) {
        if (node->tool_name)
            hu_agent_turn_state_track_tool(w->agent, node->tool_name, strlen(node->tool_name));
        dag_tool->vtable->execute(dag_tool->ctx, w->agent->alloc, dag_args, &dag_result);
    } else if (!dag_args) {
        dag_result = hu_tool_result_fail("invalid", 7);
    }
    if (dag_args)
        hu_json_free(w->agent->alloc, dag_args);

    pthread_mutex_lock(&g_dag_parallel_prep_mutex);
    hu_agent_internal_post_hook_fire(
        w->agent, node->tool_name, node->tool_name ? strlen(node->tool_name) : 0,
        use_args ? use_args : "", use_args ? strlen(use_args) : 0, &dag_result);
    pthread_mutex_unlock(&g_dag_parallel_prep_mutex);

    if (resolved_args)
        w->agent->alloc->free(w->agent->alloc->ctx, resolved_args, resolved_len + 1);

    if (dag_result.success) {
        node->status = HU_DAG_DONE;
        if (dag_result.output && dag_result.output_len > 0) {
            node->result = hu_strndup(w->agent->alloc, dag_result.output, dag_result.output_len);
            node->result_len = dag_result.output_len;
        }
    } else {
        node->status = HU_DAG_FAILED;
    }
    hu_tool_result_free(w->agent->alloc, &dag_result);
    return NULL;
}
#endif

/* CausalArmor (HIGH-risk tools) then the interaction-history scorer (MEDIUM
 * and up) on a successful tool result; either one replaces *result with a
 * failure. One copy for the dispatcher path and the sequential fallback,
 * which carried identical inline blocks before the carve (clone ratchet). */
static void turn_tools_causal_and_history_guard(hu_agent_t *agent, const hu_tool_call_t *call,
                                                const char *tn, size_t tn_len,
                                                hu_tool_result_t *result) {
    if (result->success && hu_tool_risk_level(tn[0] ? tn : "unknown") >= HU_RISK_HIGH) {
        hu_causal_armor_config_t ca_cfg;
        hu_causal_armor_config_default(&ca_cfg);
        hu_causal_segment_t ca_segs[8];
        size_t ca_seg_count = 0;
        for (size_t hi = agent->history_count; hi > 0 && ca_seg_count < 8; hi--) {
            const hu_owned_message_t *he = &agent->history[hi - 1];
            if (he->content && he->content_len > 0) {
                ca_segs[ca_seg_count].content = he->content;
                ca_segs[ca_seg_count].content_len = he->content_len;
                ca_segs[ca_seg_count].is_trusted = (he->role == HU_ROLE_USER);
                ca_seg_count++;
            }
        }
        if (ca_seg_count > 0) {
            const char *args_str = call->arguments ? call->arguments : "";
            size_t argl = call->arguments ? call->arguments_len : strlen(args_str);
            hu_causal_armor_result_t ca_result;
            if (hu_causal_armor_evaluate(&ca_cfg, ca_segs, ca_seg_count, tn, tn_len, args_str, argl,
                                         &ca_result) == HU_OK &&
                !ca_result.is_safe) {
                static const char ca_msg[] = "blocked: untrusted content dominates tool decision";
                hu_tool_result_free(agent->alloc, result);
                *result = hu_tool_result_fail(ca_msg, sizeof(ca_msg) - 1);
            }
        }
    }

    /* Interaction-history safety scorer (post-CausalArmor) */
    if (result->success && hu_tool_risk_level(tn[0] ? tn : "unknown") >= HU_RISK_MEDIUM) {
        hu_tool_history_entry_t thist[16];
        size_t thc = 0;
        for (size_t hi = 0; hi < agent->history_count && thc < 16; hi++) {
            const hu_owned_message_t *m = &agent->history[hi];
            if (m->role != HU_ROLE_TOOL || !m->name || m->name_len == 0)
                continue;
            thist[thc].tool_name = m->name;
            thist[thc].name_len = m->name_len;
            thist[thc].succeeded =
                !(m->content && m->content_len >= 6 && memcmp(m->content, "denied", 6) == 0);
            thist[thc].risk_level = (uint32_t)hu_tool_risk_level(m->name);
            thc++;
        }
        if (thc > 0) {
            hu_history_score_result_t hs;
            if (hu_history_scorer_evaluate(thist, thc, tn, tn_len,
                                           (uint32_t)hu_tool_risk_level(tn[0] ? tn : "unknown"),
                                           &hs) == HU_OK &&
                hs.is_suspicious) {
                static const char hs_msg[] = "blocked: suspicious tool-call history pattern";
                hu_tool_result_free(agent->alloc, result);
                *result = hu_tool_result_fail(hs_msg, sizeof(hs_msg) - 1);
            }
        }
    }
}

hu_error_t hu_turn_tools(hu_turn_ctx_t *turn_ctx) {
    if (!turn_ctx || !turn_ctx->in.agent)
        return HU_ERR_INVALID_ARGUMENT;
    hu_agent_t *agent = turn_ctx->in.agent;
    const char *msg = turn_ctx->in.msg;
    size_t msg_len = turn_ctx->in.msg_len;
    hu_tool_cache_t *turn_cache = turn_ctx->loop.turn_cache;
    size_t turn_tool_results_count = turn_ctx->loop.turn_tool_results_count;
    hu_error_t err = HU_OK;
    {
        size_t tc_count = agent->history[agent->history_count - 1].tool_calls_count;
        const hu_tool_call_t *calls = agent->history[agent->history_count - 1].tool_calls;

        /* Emit TOOL_CALL_START events for all calls */
        for (size_t tc = 0; tc < tc_count; tc++) {
            char tn_buf[64];
            size_t tn =
                (calls[tc].name_len < sizeof(tn_buf) - 1) ? calls[tc].name_len : sizeof(tn_buf) - 1;
            if (tn > 0 && calls[tc].name)
                memcpy(tn_buf, calls[tc].name, tn);
            tn_buf[tn] = '\0';
            hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_TOOL_CALL_START, .data = {{0}}};
            ev.data.tool_call_start.tool = tn_buf[0] ? tn_buf : "unknown";
            HU_OBS_SAFE_RECORD_EVENT(agent, &ev);
        }

        /* LOCKED: skip all tool execution */
        if (agent->autonomy_level == HU_AUTONOMY_LOCKED) {
            for (size_t tc = 0; tc < tc_count; tc++) {
                const hu_tool_call_t *call = &calls[tc];
                hu_error_t hist_err = hu_agent_internal_append_history(
                    agent, HU_ROLE_TOOL, "Action blocked: agent is in locked mode", 38, call->name,
                    call->name_len, call->id, call->id_len);
                if (hist_err != HU_OK)
                    hu_log_error("agent_turn", NULL, "history append failed: %s",
                                 hu_error_string(hist_err));
                if (agent->cancel_requested)
                    break;
            }
        } else {
            bool used_llm_compiler = false;
            bool used_hula_ir = false;
            bool compiler_dag_complete = false;
            /* LLMCompiler: when enabled, compile tool calls into a DAG for parallel execution.
             * Note: LLMCompiler is opt-in via config; the existing dispatcher handles
             * parallelism. */
#ifndef HU_IS_TEST
            /* HuLa compiler: LLM emits full HuLa JSON (preferred over DAG when enabled). */
            if (agent->hula_enabled && tc_count >= 3 && agent->provider.vtable &&
                agent->provider.vtable->chat) {
                hu_spawn_config_t hula_spawn_tpl;
                hu_agent_internal_hula_fill_spawn_tpl(agent, &hula_spawn_tpl);
                bool hula_compiler_ok = false;
                (void)hu_hula_compiler_chat_compile_execute(
                    agent->alloc, msg, msg_len, agent->tools, agent->tools_count, agent->policy,
                    agent->observer, agent->agent_pool, agent->agent_pool ? &hula_spawn_tpl : NULL,
                    agent->provider.vtable->chat, agent->provider.ctx, agent->model_name,
                    agent->model_name_len, agent->temperature, NULL, 0, hula_compiler_agent_done,
                    agent, &hula_compiler_ok);
                if (hula_compiler_ok) {
                    used_llm_compiler = true;
                    used_hula_ir = true;
                }
            }
            /* LLMCompiler: if enabled and 3+ tool calls, use DAG-based execution */
            if (!used_llm_compiler && agent->llm_compiler_enabled && tc_count >= 3) {
                const char *tool_names[32];
                size_t tn_count = 0;
                for (size_t ti = 0; ti < agent->tools_count && tn_count < 32; ti++) {
                    if (agent->tools[ti].vtable && agent->tools[ti].vtable->name)
                        tool_names[tn_count++] =
                            agent->tools[ti].vtable->name(agent->tools[ti].ctx);
                }
                char *compiler_prompt = NULL;
                size_t compiler_prompt_len = 0;
                if (hu_llm_compiler_build_prompt(agent->alloc, msg, msg_len, tool_names, tn_count,
                                                 &compiler_prompt, &compiler_prompt_len) == HU_OK) {
                    hu_chat_message_t compiler_msgs[1] = {{
                        .role = HU_ROLE_USER,
                        .content = compiler_prompt,
                        .content_len = compiler_prompt_len,
                    }};
                    hu_chat_request_t compiler_req = {
                        .messages = compiler_msgs,
                        .messages_count = 1,
                        .tools = NULL,
                        .tools_count = 0,
                    };
                    hu_chat_response_t compiler_resp;
                    memset(&compiler_resp, 0, sizeof(compiler_resp));
                    hu_error_t cerr = agent->provider.vtable->chat(
                        agent->provider.ctx, agent->alloc, &compiler_req, agent->model_name,
                        agent->model_name_len, agent->temperature, &compiler_resp);
                    hu_str_free(agent->alloc, compiler_prompt);
                    if (cerr == HU_OK && compiler_resp.content && compiler_resp.content_len > 0) {
                        hu_dag_t dag;
                        hu_dag_init(&dag, *agent->alloc);
                        if (hu_llm_compiler_parse_plan(agent->alloc, compiler_resp.content,
                                                       compiler_resp.content_len, &dag) == HU_OK &&
                            dag.node_count > 0) {
                            /* Execute DAG in dependency batches; parallelize ready nodes
                             * (POSIX). */
                            bool dag_executed = false;
                            size_t max_dag_iters = dag.node_count * 2;
                            for (size_t di = 0; di < max_dag_iters && !hu_dag_is_complete(&dag);
                                 di++) {
                                hu_dag_batch_t batch;
                                memset(&batch, 0, sizeof(batch));
                                if (hu_dag_next_batch(&dag, &batch) != HU_OK || batch.count == 0)
                                    break;
#if (defined(__unix__) || defined(__APPLE__)) && !defined(HU_IS_TEST)
                                bool batch_thread_safe = (batch.count > 1);
                                if (batch_thread_safe) {
                                    for (size_t bi = 0; bi < batch.count && batch_thread_safe;
                                         bi++) {
                                        hu_dag_node_t *bn = batch.nodes[bi];
                                        hu_tool_t *bt = hu_agent_internal_find_tool(
                                            agent, bn->tool_name,
                                            bn->tool_name ? strlen(bn->tool_name) : 0);
                                        if (!bt || !bt->vtable ||
                                            !(bt->vtable->flags & HU_TOOL_FLAG_THREAD_SAFE))
                                            batch_thread_safe = false;
                                    }
                                }
                                /* The worker contexts live on the heap, not in this
                                 * loop-scoped frame: ASan on Darwin arm64
                                 * false-positives a loop-scoped struct handed to
                                 * pthread_create (.claude/rules/
                                 * asan-pthread-stack-aliasing-darwin.md). If the
                                 * allocation fails the batch takes the sequential
                                 * path below. */
                                dag_parallel_work_t *works =
                                    batch_thread_safe
                                        ? (dag_parallel_work_t *)agent->alloc->alloc(
                                              agent->alloc->ctx,
                                              HU_DAG_MAX_BATCH_SIZE * sizeof(dag_parallel_work_t))
                                        : NULL;
                                if (works) {
                                    pthread_t tids[HU_DAG_MAX_BATCH_SIZE];
                                    bool thread_started[HU_DAG_MAX_BATCH_SIZE];
                                    memset(thread_started, 0, sizeof(thread_started));
                                    for (size_t bi = 0; bi < batch.count; bi++) {
                                        works[bi].agent = agent;
                                        works[bi].node = batch.nodes[bi];
                                        works[bi].dag = &dag;
                                        if (pthread_create(&tids[bi], NULL, dag_parallel_worker,
                                                           &works[bi]) == 0)
                                            thread_started[bi] = true;
                                        else
                                            dag_parallel_worker(&works[bi]);
                                    }
                                    for (size_t bi = 0; bi < batch.count; bi++) {
                                        if (thread_started[bi])
                                            (void)pthread_join(tids[bi], NULL);
                                    }
                                    for (size_t bi = 0; bi < batch.count; bi++) {
                                        if (batch.nodes[bi]->status == HU_DAG_DONE)
                                            dag_executed = true;
                                    }
                                    agent->alloc->free(agent->alloc->ctx, works,
                                                       HU_DAG_MAX_BATCH_SIZE *
                                                           sizeof(dag_parallel_work_t));
                                    continue;
                                }
#endif
                                for (size_t bi = 0; bi < batch.count; bi++) {
                                    hu_dag_node_t *node = batch.nodes[bi];
                                    node->status = HU_DAG_RUNNING;
                                    char *resolved_args = NULL;
                                    size_t resolved_len = 0;
                                    if (node->args_json) {
                                        (void)hu_dag_resolve_vars(
                                            agent->alloc, &dag, node->args_json,
                                            strlen(node->args_json), &resolved_args, &resolved_len);
                                    }
                                    hu_tool_t *dag_tool = hu_agent_internal_find_tool(
                                        agent, node->tool_name,
                                        node->tool_name ? strlen(node->tool_name) : 0);
                                    if (!dag_tool) {
                                        node->status = HU_DAG_FAILED;
                                        if (resolved_args)
                                            hu_str_free(agent->alloc, resolved_args);
                                        continue;
                                    }

                                    const char *args_str =
                                        resolved_args ? resolved_args : node->args_json;
                                    size_t args_len =
                                        resolved_args
                                            ? resolved_len
                                            : (node->args_json ? strlen(node->args_json) : 0);

                                    hu_tool_result_t dag_result = {0};
                                    const char *gate_args = args_str ? args_str : "";
                                    size_t gate_args_len = args_str ? args_len : 0;
                                    hu_tool_gate_t gate = hu_agent_internal_pre_execute_checks(
                                        agent, node->tool_name,
                                        node->tool_name ? strlen(node->tool_name) : 0, gate_args,
                                        gate_args_len, &dag_result);
                                    if (gate != HU_TOOL_GATE_ALLOW) {
                                        node->status = HU_DAG_FAILED;
                                        hu_agent_internal_post_hook_fire(
                                            agent, node->tool_name,
                                            node->tool_name ? strlen(node->tool_name) : 0,
                                            gate_args, gate_args_len, &dag_result);
                                        hu_tool_result_free(agent->alloc, &dag_result);
                                        if (resolved_args)
                                            hu_str_free(agent->alloc, resolved_args);
                                        continue;
                                    }

                                    hu_json_value_t *dag_args = NULL;
                                    if (args_str && args_len > 0) {
                                        hu_error_t jerr = hu_json_parse(agent->alloc, args_str,
                                                                        args_len, &dag_args);
                                        if (jerr != HU_OK)
                                            fprintf(stderr,
                                                    "[agent_turn] DAG tool args parse failed\n");
                                    }
                                    if (dag_args && dag_tool->vtable->execute) {
                                        if (node->tool_name)
                                            hu_agent_turn_state_track_tool(agent, node->tool_name,
                                                                           strlen(node->tool_name));
                                        dag_tool->vtable->execute(dag_tool->ctx, agent->alloc,
                                                                  dag_args, &dag_result);
                                    } else if (!dag_args) {
                                        dag_result = hu_tool_result_fail("invalid", 7);
                                    }
                                    if (dag_args)
                                        hu_json_free(agent->alloc, dag_args);
                                    if (resolved_args)
                                        hu_str_free(agent->alloc, resolved_args);
                                    hu_agent_internal_post_hook_fire(
                                        agent, node->tool_name,
                                        node->tool_name ? strlen(node->tool_name) : 0, gate_args,
                                        gate_args_len, &dag_result);
                                    if (dag_result.success) {
                                        node->status = HU_DAG_DONE;
                                        dag_executed = true;
                                        if (dag_result.output && dag_result.output_len > 0) {
                                            node->result =
                                                hu_strndup(agent->alloc, dag_result.output,
                                                           dag_result.output_len);
                                            node->result_len = dag_result.output_len;
                                        }
                                    } else {
                                        node->status = HU_DAG_FAILED;
                                    }
                                    hu_tool_result_free(agent->alloc, &dag_result);
                                }
                            }
                            if (dag_executed) {
                                used_llm_compiler = true;
                                compiler_dag_complete = hu_dag_is_complete(&dag);
                                hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_TOOL_CALL,
                                                          .data = {{0}}};
                                ev.data.tool_call.tool = "llm_compiler";
                                ev.data.tool_call.duration_ms = 0;
                                ev.data.tool_call.success = compiler_dag_complete;
                                HU_OBS_SAFE_RECORD_EVENT(agent, &ev);

                                for (size_t ni = 0; ni < dag.node_count; ni++) {
                                    hu_dag_node_t *node = &dag.nodes[ni];
                                    const char *r =
                                        node->result
                                            ? node->result
                                            : (node->status == HU_DAG_DONE ? "ok" : "failed");
                                    size_t rlen = node->result ? node->result_len : strlen(r);
                                    hu_error_t hist_err = hu_agent_internal_append_history(
                                        agent, HU_ROLE_TOOL, r, rlen, node->tool_name,
                                        node->tool_name ? strlen(node->tool_name) : 0, node->id,
                                        node->id ? strlen(node->id) : 0);
                                    if (hist_err != HU_OK)
                                        hu_log_error("agent_turn", NULL,
                                                     "history append failed: %s",
                                                     hu_error_string(hist_err));
                                }
                            }
                        }
                        hu_dag_deinit(&dag);
                    }
                    hu_chat_response_free(agent->alloc, &compiler_resp);
                }
            }
#endif
            /* Multi-agent orchestrator: decompose the user goal with the LLM when possible,
             * then assign to registry agents; otherwise split on tool-call names. Consensus
             * merge prefers the longest agreeing sub-agent output. */
            if (agent->multi_agent_enabled && agent->agent_registry &&
                (tc_count >= 2 ||
                 (tc_count >= 1 && hu_agent_internal_message_looks_multistep(msg, msg_len)))) {
                hu_orchestrator_t orch;
                if (hu_orchestrator_create(agent->alloc, &orch) == HU_OK) {
                    hu_orchestrator_load_from_registry(&orch, agent->agent_registry);
                    if (orch.agent_count > 0) {
                        bool have_plan = false;
                        if (agent->provider.vtable && agent->provider.vtable->chat_with_system &&
                            (tc_count >= 2 ||
                             hu_agent_internal_message_looks_multistep(msg, msg_len))) {
                            hu_decomposition_t decomp;
                            memset(&decomp, 0, sizeof(decomp));
                            hu_error_t derr = hu_orchestrator_decompose_goal(
                                agent->alloc, &agent->provider, agent->model_name,
                                agent->model_name_len, msg, msg_len, orch.agents, orch.agent_count,
                                &decomp);
                            if (derr == HU_OK && decomp.task_count > 0) {
                                hu_error_t aerr = hu_orchestrator_auto_assign(&orch, &decomp);
                                if (aerr == HU_OK)
                                    have_plan = true;
                            }
                            hu_decomposition_free(agent->alloc, &decomp);
                        }
                        if (!have_plan && tc_count >= 2) {
                            const char *subtask_descs[HU_ORCHESTRATOR_MAX_TASKS];
                            size_t subtask_lens[HU_ORCHESTRATOR_MAX_TASKS];
                            size_t sub_n = tc_count < HU_ORCHESTRATOR_MAX_TASKS
                                               ? tc_count
                                               : HU_ORCHESTRATOR_MAX_TASKS;
                            for (size_t s = 0; s < sub_n; s++) {
                                subtask_descs[s] = calls[s].name;
                                subtask_lens[s] = calls[s].name_len;
                            }
                            hu_orchestrator_propose_split(&orch, msg, msg_len, subtask_descs,
                                                          subtask_lens, sub_n);
                            for (size_t s = 0; s < orch.task_count && s < sub_n; s++) {
                                if (orch.agents[s % orch.agent_count].agent_id_len > 0) {
                                    hu_orchestrator_assign_task(
                                        &orch, orch.tasks[s].id,
                                        orch.agents[s % orch.agent_count].agent_id,
                                        orch.agents[s % orch.agent_count].agent_id_len);
                                }
                            }
                            have_plan = orch.task_count > 0;
                        }
                        if (have_plan) {
                            /* Use swarm for parallel execution when multiple tasks are assigned
                             */
                            if (orch.task_count >= 2) {
                                hu_swarm_config_t swarm_cfg = hu_swarm_config_default();
                                swarm_cfg.provider = &agent->provider;
                                swarm_cfg.model = agent->model_name;
                                swarm_cfg.model_len = agent->model_name_len;
                                swarm_cfg.tools = agent->tools;
                                swarm_cfg.tools_count = agent->tools_count;
                                hu_swarm_task_t swarm_tasks[HU_ORCHESTRATOR_MAX_TASKS];
                                memset(swarm_tasks, 0, sizeof(swarm_tasks));
                                size_t swarm_n = 0;
                                for (size_t s = 0;
                                     s < orch.task_count && s < HU_ORCHESTRATOR_MAX_TASKS; s++) {
                                    if (orch.tasks[s].status == HU_TASK_ASSIGNED) {
                                        size_t dlen = orch.tasks[s].description_len;
                                        if (dlen >= sizeof(swarm_tasks[swarm_n].description))
                                            dlen = sizeof(swarm_tasks[swarm_n].description) - 1;
                                        memcpy(swarm_tasks[swarm_n].description,
                                               orch.tasks[s].description, dlen);
                                        swarm_tasks[swarm_n].description[dlen] = '\0';
                                        swarm_tasks[swarm_n].description_len = dlen;
                                        swarm_n++;
                                    }
                                }
                                if (swarm_n > 0) {
                                    /* MAR: use structured critique personas when enabled */
                                    if (agent->sota.mar_config.enabled && swarm_n == 1) {
                                        hu_mar_result_t mar_result;
                                        memset(&mar_result, 0, sizeof(mar_result));
                                        hu_error_t mar_err = hu_mar_execute(
                                            agent->alloc, &agent->provider, &agent->sota.mar_config,
                                            swarm_tasks[0].description,
                                            swarm_tasks[0].description_len, &mar_result);
                                        if (mar_err == HU_OK && mar_result.final_output &&
                                            mar_result.final_output_len > 0) {
                                            hu_agent_internal_append_history(
                                                agent, HU_ROLE_TOOL, mar_result.final_output,
                                                mar_result.final_output_len, "mar", 3,
                                                "mar_reflexion", 13);
                                        }
                                        hu_mar_result_free(agent->alloc, &mar_result);
                                    }

                                    hu_swarm_result_t swarm_result = {0};
                                    hu_error_t swarm_err =
                                        hu_swarm_execute(agent->alloc, &swarm_cfg, swarm_tasks,
                                                         swarm_n, &swarm_result);
                                    if (swarm_err == HU_OK) {
                                        char swarm_merged[4096];
                                        size_t swarm_merged_len = 0;
                                        hu_swarm_aggregate(&swarm_result, HU_SWARM_AGG_CONCATENATE,
                                                           swarm_merged, sizeof(swarm_merged),
                                                           &swarm_merged_len);
                                        if (swarm_merged_len > 0) {
                                            hu_error_t hist_err = hu_agent_internal_append_history(
                                                agent, HU_ROLE_TOOL, swarm_merged, swarm_merged_len,
                                                "swarm", 5, "swarm_parallel", 14);
                                            if (hist_err != HU_OK)
                                                hu_log_info("agent_turn", NULL,
                                                            "swarm history append "
                                                            "failed: %s",
                                                            hu_error_string(hist_err));
                                        }
                                        for (size_t s = 0; s < swarm_n; s++) {
                                            size_t task_idx = 0;
                                            for (size_t t = 0; t < orch.task_count; t++) {
                                                if (orch.tasks[t].status == HU_TASK_ASSIGNED) {
                                                    if (task_idx == s) {
                                                        if (swarm_result.tasks[s].completed)
                                                            hu_orchestrator_complete_task(
                                                                &orch, orch.tasks[t].id,
                                                                swarm_result.tasks[s].result,
                                                                swarm_result.tasks[s].result_len);
                                                        else
                                                            hu_orchestrator_fail_task(
                                                                &orch, orch.tasks[t].id,
                                                                "swarm failed", 12);
                                                        break;
                                                    }
                                                    task_idx++;
                                                }
                                            }
                                        }
                                    }
                                    hu_swarm_result_free(agent->alloc, &swarm_result);
                                }
                            } else {
                                /* Execute orchestrated tasks sequentially (single-task
                                 * fallback) */
                                for (size_t s = 0; s < orch.task_count; s++) {
                                    hu_orchestrator_task_t *task = &orch.tasks[s];
                                    if (task->status != HU_TASK_ASSIGNED)
                                        continue;
                                    task->status = HU_TASK_IN_PROGRESS;
                                    hu_tool_t *orch_tool = hu_agent_internal_find_tool(
                                        agent, task->description, task->description_len);
                                    if (!orch_tool) {
                                        hu_orchestrator_fail_task(&orch, task->id, "tool not found",
                                                                  14);
                                        continue;
                                    }
                                    hu_tool_result_t orch_result =
                                        hu_tool_result_fail("no args", 7);
                                    if (s < tc_count && calls[s].arguments_len > 0) {
                                        hu_json_value_t *orch_args = NULL;
                                        hu_error_t jerr =
                                            hu_json_parse(agent->alloc, calls[s].arguments,
                                                          calls[s].arguments_len, &orch_args);
                                        if (jerr != HU_OK)
                                            fprintf(stderr,
                                                    "[agent_turn] tool args JSON parse failed\n");
                                        if (orch_args && orch_tool->vtable->execute) {
                                            if (task->description_len > 0)
                                                hu_agent_turn_state_track_tool(
                                                    agent, task->description,
                                                    task->description_len);
                                            orch_tool->vtable->execute(orch_tool->ctx, agent->alloc,
                                                                       orch_args, &orch_result);
                                        }
                                        if (orch_args)
                                            hu_json_free(agent->alloc, orch_args);
                                    }
                                    if (orch_result.success) {
                                        hu_orchestrator_complete_task(
                                            &orch, task->id,
                                            orch_result.output ? orch_result.output : "ok",
                                            orch_result.output ? orch_result.output_len : 2);
                                    } else {
                                        hu_orchestrator_fail_task(
                                            &orch, task->id,
                                            orch_result.error_msg ? orch_result.error_msg
                                                                  : "failed",
                                            orch_result.error_msg ? orch_result.error_msg_len : 6);
                                    }
                                    hu_tool_result_free(agent->alloc, &orch_result);
                                }
                            }
                            /* Merge and append orchestrated results */
                            char *merged = NULL;
                            size_t merged_len = 0;
                            if (hu_orchestrator_merge_results_consensus(
                                    &orch, agent->alloc, &merged, &merged_len) == HU_OK &&
                                merged && merged_len > 0) {
                                hu_error_t hist_err = hu_agent_internal_append_history(
                                    agent, HU_ROLE_TOOL, merged, merged_len, "orchestrator", 12,
                                    "orch_merge", 10);
                                if (hist_err != HU_OK)
                                    hu_log_error("agent_turn", NULL, "history append failed: %s",
                                                 hu_error_string(hist_err));
                                agent->alloc->free(agent->alloc->ctx, merged, merged_len + 1);
                            }
                            hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_TOOL_CALL,
                                                      .data = {{0}}};
                            ev.data.tool_call.tool = "orchestrator";
                            ev.data.tool_call.duration_ms = 0;
                            ev.data.tool_call.success = hu_orchestrator_all_complete(&orch);
                            HU_OBS_SAFE_RECORD_EVENT(agent, &ev);
                        }
                    }
                    hu_orchestrator_deinit(&orch);
                }
            }

            /* HuLa: compile tool calls into a HuLa program and execute via the IR.
             * This path provides unified policy checking, tracing, and structured
             * execution. Falls through to the dispatcher if disabled or on failure. */
            bool used_hula = false;
#ifndef HU_IS_TEST
            if (!used_llm_compiler && !used_hula_ir && agent->hula_enabled && tc_count >= 1) {
                hu_hula_program_t hula_prog;
                hu_error_t herr = hu_hula_program_init(&hula_prog, *agent->alloc, "turn", 4);
                if (herr == HU_OK) {
                    hu_hula_node_t *hula_root;
                    if (tc_count == 1) {
                        hula_root = hu_hula_program_alloc_node(&hula_prog, HU_HULA_CALL, "t0");
                    } else {
                        hula_root = hu_hula_program_alloc_node(&hula_prog, HU_HULA_PAR, "root");
                    }
                    if (hula_root) {
                        bool hula_build_ok = true;
                        for (size_t hti = 0; hti < tc_count && hula_build_ok; hti++) {
                            hu_hula_node_t *cn;
                            if (tc_count == 1) {
                                cn = hula_root;
                            } else {
                                char cid[32];
                                (void)snprintf(cid, sizeof(cid), "t%zu", hti);
                                cn = hu_hula_program_alloc_node(&hula_prog, HU_HULA_CALL, cid);
                                if (cn)
                                    hula_root->children[hula_root->children_count++] = cn;
                            }
                            if (cn) {
                                cn->tool_name =
                                    hu_strndup(agent->alloc, calls[hti].name, calls[hti].name_len);
                                cn->args_json = hu_strndup(agent->alloc, calls[hti].arguments,
                                                           calls[hti].arguments_len);
                            } else {
                                hula_build_ok = false;
                            }
                        }
                        if (hula_build_ok) {
                            hula_prog.root = hula_root;
                            hu_hula_exec_t hula_exec;
                            hu_spawn_config_t hula_spawn_tpl;
                            memset(&hula_spawn_tpl, 0, sizeof(hula_spawn_tpl));
                            hu_security_policy_t *hula_policy = agent->policy;
                            hu_observer_t *hula_obs = agent->observer;
                            herr = hu_hula_exec_init_full(&hula_exec, *agent->alloc, &hula_prog,
                                                          agent->tools, agent->tools_count,
                                                          hula_policy, hula_obs);
                            if (herr == HU_OK) {
                                hu_agent_internal_hula_exec_bind_spawn(agent, &hula_exec,
                                                                       &hula_spawn_tpl);
                                hu_hula_exec_set_security_agent(&hula_exec, agent);
                                herr = hu_hula_exec_run(&hula_exec);
                                if (herr == HU_OK) {
                                    size_t trl = 0;
                                    const char *tr = hu_hula_exec_trace(&hula_exec, &trl);
                                    bool root_ok = false;
                                    if (hula_prog.root && hula_prog.root->id)
                                        root_ok =
                                            hu_hula_exec_result(&hula_exec, hula_prog.root->id)
                                                ->status == HU_HULA_DONE;
                                    else
                                        root_ok = true;
                                    size_t ir_audit_len = 0;
                                    char *ir_audit = agent_turn_hula_ir_tool_calls_audit_json(
                                        agent->alloc, calls, tc_count, &ir_audit_len);
                                    char *pj = NULL;
                                    size_t pjl = 0;
                                    if (hu_hula_to_json(agent->alloc, &hula_prog, &pj, &pjl) ==
                                            HU_OK &&
                                        pj) {
                                        (void)hu_hula_trace_persist(agent->alloc, NULL, tr, trl,
                                                                    hula_prog.name,
                                                                    hula_prog.name_len, root_ok, pj,
                                                                    pjl, ir_audit, ir_audit_len);
                                        hu_str_free(agent->alloc, pj);
                                    } else {
                                        (void)hu_hula_trace_persist(
                                            agent->alloc, NULL, tr, trl, hula_prog.name,
                                            hula_prog.name_len, root_ok, NULL, 0, ir_audit,
                                            ir_audit_len);
                                    }
                                    if (ir_audit)
                                        hu_str_free(agent->alloc, ir_audit);
                                    used_hula = true;
                                    hu_bth_metrics_record_hula_tool_turn(agent->bth_metrics);
                                    hu_agent_internal_hula_append_histories(agent, &hula_prog,
                                                                            &hula_exec);
                                }
                                hu_hula_exec_deinit(&hula_exec);
                            }
                        }
                    }
                    hu_hula_program_deinit(&hula_prog);
                }
            }
#endif /* HU_IS_TEST */

            if (compiler_dag_complete)
                goto skip_dispatcher;
            if (!used_llm_compiler && !used_hula_ir && !used_hula) {
                /* Use dispatcher for parallel execution when enabled (Tier 1.3). */
                hu_dispatcher_t dispatcher;
                hu_dispatcher_default(&dispatcher);
                if (tc_count > 1)
                    dispatcher.max_parallel = 4;
                dispatcher.timeout_secs = 30;
                dispatcher.cache = turn_cache;
                dispatcher.max_retries = 2;
                dispatcher.retry_base_ms = 200;

                const hu_tool_call_t *calls_to_dispatch = calls;
#ifdef HU_ENABLE_SQLITE
                /* World model: rank tool calls by predicted outcome */
                if (tc_count >= 2 && agent->memory) {
                    sqlite3 *wm_db = hu_sqlite_memory_get_db(agent->memory);
                    if (wm_db) {
                        hu_causal_world_model_t wm;
                        if (hu_causal_world_model_create(agent->alloc, wm_db, &wm) == HU_OK) {
                            size_t opt_count = tc_count < 32 ? tc_count : 32;
                            hu_tool_call_t calls_buf[32];
                            for (size_t tc = 0; tc < opt_count; tc++)
                                calls_buf[tc] = calls[tc];
                            const char *action_names[32];
                            size_t action_lens[32];
                            for (size_t tc = 0; tc < opt_count; tc++) {
                                action_names[tc] = calls_buf[tc].name;
                                action_lens[tc] = calls_buf[tc].name_len;
                            }
                            hu_action_option_t options[32];
                            memset(options, 0, sizeof(options));
                            if (hu_world_evaluate_options(&wm, action_names, action_lens, opt_count,
                                                          msg, msg_len, options) == HU_OK) {
                                /* options are sorted by score descending; reorder calls to
                                 * match */
                                for (size_t i = 0; i < opt_count; i++) {
                                    for (size_t j = 0; j < opt_count; j++) {
                                        if (options[i].action_len == calls[j].name_len &&
                                            memcmp(options[i].action, calls[j].name,
                                                   options[i].action_len) == 0) {
                                            calls_buf[i] = calls[j];
                                            break;
                                        }
                                    }
                                }
                                calls_to_dispatch = calls_buf;
                                hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_TOOL_CALL,
                                                          .data = {{0}}};
                                ev.data.tool_call.tool = "world_model_reorder";
                                ev.data.tool_call.success = true;
                                HU_OBS_SAFE_RECORD_EVENT(agent, &ev);
                            }
                            hu_causal_world_model_deinit(&wm);
                        }
                    }
                }
#endif
                /* TTL cache: check for cached tool results before dispatching */
                hu_tool_cache_ttl_t *ttl_cache = (hu_tool_cache_ttl_t *)agent->infra.tool_cache_ttl;
                bool *ttl_hits = NULL;
                size_t ttl_hit_count = 0;
                hu_tool_result_t *merged_results = NULL;
                if (ttl_cache && tc_count > 0) {
                    ttl_hits =
                        (bool *)agent->alloc->alloc(agent->alloc->ctx, tc_count * sizeof(bool));
                    merged_results = (hu_tool_result_t *)agent->alloc->alloc(
                        agent->alloc->ctx, tc_count * sizeof(hu_tool_result_t));
                    if (ttl_hits && merged_results) {
                        memset(ttl_hits, 0, tc_count * sizeof(bool));
                        memset(merged_results, 0, tc_count * sizeof(hu_tool_result_t));
                        for (size_t ci = 0; ci < tc_count; ci++) {
                            const hu_tool_call_t *cc = &calls_to_dispatch[ci];
                            if (hu_tool_cache_classify(cc->name, cc->name_len, cc->arguments,
                                                       cc->arguments_len) == HU_TOOL_CACHE_NEVER)
                                continue;
                            uint64_t ckey = hu_tool_cache_ttl_key(cc->name, cc->name_len,
                                                                  cc->arguments, cc->arguments_len);
                            size_t clen = 0;
                            const char *cached = hu_tool_cache_ttl_get(ttl_cache, ckey, &clen);
                            if (cached && clen > 0) {
                                merged_results[ci].success = true;
                                merged_results[ci].output = hu_strndup(agent->alloc, cached, clen);
                                merged_results[ci].output_len = clen;
                                ttl_hits[ci] = true;
                                ttl_hit_count++;
                            }
                        }
                    }
                }

                hu_dispatch_result_t dispatch_result;
                memset(&dispatch_result, 0, sizeof(dispatch_result));
                const size_t uncached_count = tc_count - ttl_hit_count;

                /* SECURITY FIX: Pre-check permissions and pre-hooks BEFORE dispatching.
                 * This prevents bypassing permission tiers and hook policies. */
                const hu_tool_call_t *dispatch_calls = calls_to_dispatch;
                size_t dispatch_count = tc_count;
                hu_tool_call_t *filtered_calls = NULL;
                size_t *filtered_map = NULL;
                bool *dispatch_allowed = NULL;
                size_t dispatch_allowed_count = 0;

                if (uncached_count > 0) {
                    if (ttl_hit_count > 0) {
                        filtered_calls = (hu_tool_call_t *)agent->alloc->alloc(
                            agent->alloc->ctx, uncached_count * sizeof(hu_tool_call_t));
                        filtered_map = (size_t *)agent->alloc->alloc(
                            agent->alloc->ctx, uncached_count * sizeof(size_t));
                        if (filtered_calls && filtered_map) {
                            size_t fi = 0;
                            for (size_t ci = 0; ci < tc_count; ci++) {
                                if (!ttl_hits[ci]) {
                                    filtered_calls[fi] = calls_to_dispatch[ci];
                                    filtered_map[fi] = ci;
                                    fi++;
                                }
                            }
                            dispatch_calls = (const hu_tool_call_t *)filtered_calls;
                            dispatch_count = uncached_count;
                        }
                    }

                    /* SECURITY: Full pre-execute envelope BEFORE dispatching.
                     * Build a whitelist of tools allowed to execute. */
                    dispatch_allowed = (bool *)agent->alloc->alloc(agent->alloc->ctx,
                                                                   dispatch_count * sizeof(bool));
                    if (dispatch_allowed) {
                        memset(dispatch_allowed, 1, dispatch_count * sizeof(bool));
                        for (size_t dci = 0; dci < dispatch_count; dci++) {
                            const hu_tool_call_t *dcall = &dispatch_calls[dci];
                            char dn_buf[64];
                            size_t dn = (dcall->name_len < sizeof(dn_buf) - 1) ? dcall->name_len
                                                                               : sizeof(dn_buf) - 1;
                            if (dn > 0 && dcall->name)
                                memcpy(dn_buf, dcall->name, dn);
                            dn_buf[dn] = '\0';

                            const char *dargs_str = dcall->arguments ? dcall->arguments : "";
                            hu_tool_result_t pre_scratch = {0};
                            hu_tool_gate_t dgate = hu_agent_internal_pre_execute_checks(
                                agent, dn_buf, dn, dargs_str, strlen(dargs_str), &pre_scratch);
                            if (dgate != HU_TOOL_GATE_ALLOW) {
                                dispatch_allowed[dci] = false;
                                hu_tool_result_free(agent->alloc, &pre_scratch);
                                continue;
                            }

                            dispatch_allowed_count++;
                        }
                    }

                    /* Only dispatch if we have allowed tools */
                    if (dispatch_allowed_count > 0) {
                        if (agent->tool_stream_cb) {
                            err = hu_dispatcher_dispatch_streaming(
                                &dispatcher, agent->alloc, agent->tools, agent->tools_count,
                                dispatch_calls, dispatch_count, agent->tool_stream_cb,
                                agent->tool_stream_ctx, &dispatch_result);
                        } else {
                            err = hu_dispatcher_dispatch(&dispatcher, agent->alloc, agent->tools,
                                                         agent->tools_count, dispatch_calls,
                                                         dispatch_count, &dispatch_result);
                        }
                    } else {
                        err = HU_OK; /* No tools to dispatch after security checks */
                    }

                    if (err == HU_OK && dispatch_result.results && merged_results &&
                        ttl_hit_count > 0) {
                        for (size_t di = 0; di < dispatch_count; di++) {
                            size_t orig = filtered_map ? filtered_map[di] : di;
                            if (orig < tc_count)
                                merged_results[orig] = dispatch_result.results[di];
                        }
                    }

                    if (filtered_calls)
                        agent->alloc->free(agent->alloc->ctx, filtered_calls,
                                           uncached_count * sizeof(hu_tool_call_t));
                    if (filtered_map)
                        agent->alloc->free(agent->alloc->ctx, filtered_map,
                                           uncached_count * sizeof(size_t));
                } else {
                    err = HU_OK;
                }

                if (err == HU_OK)
                    turn_tool_results_count += tc_count;
                (void)(turn_tool_results_count | 0); /* read to satisfy -Wunused-but-set-variable */

                hu_tool_result_t *result_array = (merged_results && ttl_hit_count > 0)
                                                     ? merged_results
                                                 : dispatch_result.results ? dispatch_result.results
                                                                           : NULL;

                if (err == HU_OK && result_array) {
                    for (size_t tc = 0; tc < tc_count; tc++) {
                        const hu_tool_call_t *call = &calls[tc];
                        hu_tool_result_t *result = &result_array[tc];

                        char tn_buf[64];
                        size_t tn = (call->name_len < sizeof(tn_buf) - 1) ? call->name_len
                                                                          : sizeof(tn_buf) - 1;
                        if (tn > 0 && call->name)
                            memcpy(tn_buf, call->name, tn);
                        tn_buf[tn] = '\0';
                        const char *args_str = call->arguments ? call->arguments : "";

                        /* SECURITY: slots denied by the pre-dispatch envelope never
                         * executed — populate the deny/approval result here. */
                        bool slot_allowed = true;
                        if (dispatch_allowed && ttl_hit_count == 0 && tc < tc_count) {
                            slot_allowed = dispatch_allowed[tc];
                        } else if (dispatch_allowed && ttl_hit_count > 0) {
                            slot_allowed = false;
                            if (filtered_map) {
                                for (size_t mi = 0; mi < uncached_count; mi++) {
                                    if (filtered_map[mi] == tc) {
                                        slot_allowed = dispatch_allowed[mi];
                                        break;
                                    }
                                }
                            } else if (tc < dispatch_count) {
                                slot_allowed = dispatch_allowed[tc];
                            }
                        }
                        if (!slot_allowed) {
                            hu_tool_result_t gate_out = {0};
                            hu_tool_gate_t g = hu_agent_internal_pre_execute_checks(
                                agent, tn_buf, tn, args_str, strlen(args_str), &gate_out);
                            hu_tool_result_free(agent->alloc, result);
                            *result = gate_out;
                            /* NEED_APPROVAL falls through to approval_cb handling below;
                             * hard DENY short-circuits after post-hook. */
                            if (g != HU_TOOL_GATE_NEED_APPROVAL && !result->needs_approval) {
                                hu_agent_internal_post_hook_fire(agent, tn_buf, tn, args_str,
                                                                 strlen(args_str), result);
                                goto dispatch_tool_done;
                            }
                        }

                        turn_tools_causal_and_history_guard(agent, call, tn_buf, tn, result);

                        /* Autonomy: SUPERVISED forces approval; ASSISTED for medium/high risk
                         */
                        if (agent->autonomy_level == HU_AUTONOMY_SUPERVISED) {
                            result->needs_approval = true;
                        } else if (agent->autonomy_level == HU_AUTONOMY_ASSISTED) {
                            if (hu_tool_risk_level(tn_buf[0] ? tn_buf : "unknown") >=
                                HU_RISK_MEDIUM)
                                result->needs_approval = true;
                        }

                        /* Feature 2: explicit failure when approval required but no callback */
                        if (result->needs_approval && !agent->approval_cb) {
                            hu_tool_result_free(agent->alloc, result);
                            *result = hu_tool_result_fail("requires human approval", 23);
                        }

                        /* Create approval gate if gate_manager is available */
                        if (result->needs_approval && agent->infra.gate_manager) {
                            char gate_desc[256];
                            int desc_n = snprintf(gate_desc, sizeof(gate_desc),
                                                  "Approve execution of tool '%.*s'",
                                                  (int)call->name_len, call->name);
                            char gate_id[64];
                            size_t args_len = args_str ? strlen(args_str) : 0;
                            hu_error_t gate_err =
                                hu_gate_create(agent->infra.gate_manager, agent->alloc, gate_desc,
                                               (size_t)(desc_n > 0 ? desc_n : 0), args_str,
                                               args_len, 300, gate_id);
                            if (gate_err == HU_OK && agent->infra.workflow_log) {
                                hu_workflow_event_t wf_ev = {0};
                                wf_ev.type = HU_WF_EVENT_HUMAN_GATE_WAITING;
                                wf_ev.workflow_id = (char *)agent->session_id;
                                wf_ev.workflow_id_len = strlen(agent->session_id);
                                wf_ev.step_id = gate_id;
                                wf_ev.step_id_len = strlen(gate_id);
                                hu_workflow_event_log_append(agent->infra.workflow_log,
                                                             agent->alloc, &wf_ev);
                            }
                        }

                        /* Approval flow: if tool needs approval, ask user and retry */
                        if (result->needs_approval && agent->approval_cb) {
                            char tn_tmp[64];
                            size_t tn2 = (call->name_len < sizeof(tn_tmp) - 1) ? call->name_len
                                                                               : sizeof(tn_tmp) - 1;
                            if (tn2 > 0 && call->name)
                                memcpy(tn_tmp, call->name, tn2);
                            tn_tmp[tn2] = '\0';
                            bool user_approved =
                                agent->approval_cb(agent->approval_ctx, tn_tmp, args_str);
                            if (user_approved) {
                                hu_tool_result_free(agent->alloc, result);
                                if (agent->policy)
                                    agent->policy->pre_approved = true;
                                hu_tool_t *tool =
                                    hu_agent_internal_find_tool(agent, call->name, call->name_len);
                                if (tool) {
                                    hu_json_value_t *retry_args = NULL;
                                    if (call->arguments_len > 0) {
                                        hu_error_t jerr =
                                            hu_json_parse(agent->alloc, call->arguments,
                                                          call->arguments_len, &retry_args);
                                        if (jerr != HU_OK)
                                            fprintf(stderr,
                                                    "[agent_turn] tool args JSON parse failed\n");
                                    }
                                    *result = hu_tool_result_fail("invalid arguments", 16);
                                    if (retry_args) {
                                        if (tool->vtable->execute) {
                                            if (call->name && call->name_len > 0)
                                                hu_agent_turn_state_track_tool(agent, call->name,
                                                                               call->name_len);
                                            tool->vtable->execute(tool->ctx, agent->alloc,
                                                                  retry_args, result);
                                        }
                                        hu_json_free(agent->alloc, retry_args);
                                    }
                                }
                            } else {
                                hu_tool_result_free(agent->alloc, result);
                                *result = hu_tool_result_fail("user denied action", 18);
                            }
                        }

                        {
                            hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_TOOL_CALL,
                                                      .data = {{0}}};
                            ev.data.tool_call.tool = tn_buf[0] ? tn_buf : "unknown";
                            ev.data.tool_call.duration_ms = 0;
                            ev.data.tool_call.success = result->success;
                            ev.data.tool_call.detail =
                                result->success
                                    ? NULL
                                    : (result->error_msg ? result->error_msg : "failed");
                            HU_OBS_SAFE_RECORD_EVENT(agent, &ev);
                        }

                        /* Tool result validation: schema + semantic checks */
                        if (agent->sota.tool_validator.default_level > HU_VALIDATE_NONE &&
                            result->success) {
                            hu_validation_result_t vr;
                            hu_tool_validator_check(&agent->sota.tool_validator, tn_buf, tn, result,
                                                    &vr);
                            if (!vr.passed) {
                                hu_log_error("agent_turn", NULL,
                                             "tool validation failed for %s: %s", tn_buf,
                                             vr.reason);
                            }
                        }

                        /* Outcome tracking */
                        if (agent->outcomes) {
                            const char *sum =
                                result->success
                                    ? (result->output ? result->output : "ok")
                                    : (result->error_msg ? result->error_msg : "failed");
                            hu_outcome_record_tool(agent->outcomes, tn_buf, result->success, sum);
                        }

#ifdef HU_ENABLE_SQLITE
                        /* Online learning: record tool outcome signal */
                        if (agent->memory) {
                            sqlite3 *ol_db = hu_sqlite_memory_get_db(agent->memory);
                            if (ol_db) {
                                hu_online_learning_t ol;
                                if (hu_online_learning_create(agent->alloc, ol_db, 0.1, &ol) ==
                                    HU_OK) {
                                    hu_learning_signal_t sig = {
                                        .type = result->success ? HU_SIGNAL_TOOL_SUCCESS
                                                                : HU_SIGNAL_TOOL_FAILURE,
                                        .tool_name = {0},
                                        .tool_name_len = tn < sizeof(sig.tool_name)
                                                             ? tn
                                                             : sizeof(sig.tool_name) - 1,
                                        .magnitude = 1.0,
                                        .timestamp = (int64_t)time(NULL),
                                    };
                                    if (tn > 0)
                                        memcpy(sig.tool_name, tn_buf, sig.tool_name_len);
                                    hu_online_learning_record(&ol, &sig);

                                    hu_self_improve_t si;
                                    if (hu_self_improve_create(agent->alloc, ol_db, &si) == HU_OK) {
                                        hu_self_improve_record_tool_outcome(
                                            &si, tn_buf, tn, result->success, sig.timestamp);
                                        hu_self_improve_deinit(&si);
                                    }
                                    hu_online_learning_deinit(&ol);
                                }
                            }

                            /* World model: record causal outcome for this tool action */
                            {
                                hu_causal_world_model_t wm;
                                if (hu_causal_world_model_create(agent->alloc, ol_db, &wm) ==
                                    HU_OK) {
                                    hu_causal_world_model_init_tables(&wm);
                                    const char *out_text =
                                        result->success ? result->output : result->error_msg;
                                    size_t out_len = result->success ? result->output_len
                                                                     : result->error_msg_len;
                                    double wm_conf = result->success ? 0.8 : 0.3;
                                    if (out_text && out_len > 0)
                                        (void)hu_world_record_outcome(&wm, tn_buf, tn, out_text,
                                                                      out_len > 512 ? 512 : out_len,
                                                                      wm_conf, (int64_t)time(NULL));
                                    hu_causal_world_model_deinit(&wm);
                                }
                            }

                            /* Fine-grained experience: per-tool recording */
                            if (agent->memory) {
                                hu_experience_store_t tool_exp;
                                if (hu_agent_internal_experience_init(agent, &tool_exp) == HU_OK) {
                                    tool_exp.db = ol_db;
                                    const char *out_text =
                                        result->success ? result->output : result->error_msg;
                                    size_t out_len = result->success ? result->output_len
                                                                     : result->error_msg_len;
                                    const char *act_text = call->arguments ? call->arguments : "";
                                    size_t act_len = call->arguments_len ? call->arguments_len : 0;
                                    double exp_score = result->success ? 0.9 : 0.2;
                                    (void)hu_experience_record(&tool_exp, tn_buf, tn, act_text,
                                                               act_len, out_text ? out_text : "",
                                                               out_text ? out_len : 0, exp_score);
                                    hu_experience_store_deinit(&tool_exp);
                                }
                            }

#if defined(HU_ENABLE_ML)
                            /* Trajectory: record tool step for RL training data */
                            {
                                int64_t traj_id = agent->sota.current_trajectory_id;
                                if (traj_id > 0) {
                                    const char *out_text =
                                        result->success ? result->output : result->error_msg;
                                    size_t out_len = result->success ? result->output_len
                                                                     : result->error_msg_len;
                                    double reward = result->success ? 0.8 : -0.5;
                                    hu_reward_type_t rtype = HU_REWARD_TOOL_SUCCESS;
                                    (void)hu_training_data_record_step(
                                        agent->alloc, ol_db, traj_id, tn_buf, tn,
                                        out_text ? out_text : "", out_text ? out_len : 0, reward,
                                        rtype);
                                }
                            }
#endif
                        }
#endif

                        /* TTL cache: store successful tool results */
                        if (ttl_cache && result->success && result->output &&
                            result->output_len > 0 && !(ttl_hits && ttl_hits[tc]) &&
                            hu_tool_cache_classify(call->name, call->name_len, call->arguments,
                                                   call->arguments_len) != HU_TOOL_CACHE_NEVER) {
                            uint64_t ckey = hu_tool_cache_ttl_key(
                                call->name, call->name_len, call->arguments, call->arguments_len);
                            int64_t ttl = hu_tool_cache_ttl_default_for(tn_buf, tn);
                            if (ttl > 0)
                                (void)hu_tool_cache_ttl_put(ttl_cache, ckey, result->output,
                                                            result->output_len, ttl);
                        }

                        /* Post-hook pipeline (centralized via
                         * hu_agent_internal_post_hook_fire). */
                        hu_agent_internal_post_hook_fire(agent, tn_buf, tn, args_str,
                                                         strlen(args_str), result);

                    dispatch_tool_done:
                        (void)0;
                        /* Capture media path from dispatched tool result */
                        if (result->success && result->media_path && result->media_path_len > 0 &&
                            agent->generated_media_count < 4) {
                            char *mp = hu_strndup(agent->alloc, result->media_path,
                                                  result->media_path_len);
                            if (mp)
                                agent->generated_media[agent->generated_media_count++] = mp;
                        }
                        const char *res_content =
                            result->success ? result->output : result->error_msg;
                        size_t res_len =
                            result->success ? result->output_len : result->error_msg_len;
                        hu_error_t hist_err = hu_agent_internal_append_history(
                            agent, HU_ROLE_TOOL, res_content, res_len, call->name, call->name_len,
                            call->id, call->id_len);
                        if (hist_err != HU_OK)
                            hu_log_error("agent_turn", NULL, "history append failed: %s",
                                         hu_error_string(hist_err));

                        if (agent->audit_logger) {
                            hu_audit_event_t aev;
                            hu_audit_event_init(&aev, HU_AUDIT_COMMAND_EXECUTION);
                            hu_audit_event_with_identity(
                                &aev, agent->agent_id,
                                agent->model_name ? agent->model_name : "unknown", NULL);
                            hu_audit_event_with_action(&aev, tn_buf, "tool", result->success, true);
                            hu_audit_event_with_result(&aev, result->success, 0, 0,
                                                       result->success ? NULL : result->error_msg);
                            hu_audit_logger_log(agent->audit_logger, &aev);
                        }

                        /* Log tool execution to workflow event log */
                        if (agent->infra.workflow_log) {
                            hu_workflow_event_t wf_ev;
                            memset(&wf_ev, 0, sizeof(wf_ev));
                            wf_ev.type = HU_WF_EVENT_TOOL_RESULT;
                            wf_ev.workflow_id = (char *)agent->session_id;
                            wf_ev.workflow_id_len = strlen(agent->session_id);
                            wf_ev.step_id = (char *)call->id;
                            wf_ev.step_id_len = call->id_len;
                            wf_ev.data_json = (char *)res_content;
                            wf_ev.data_json_len = res_len;
                            (void)hu_workflow_event_log_append(agent->infra.workflow_log,
                                                               agent->alloc, &wf_ev);
                        }

                        if (agent->cancel_requested)
                            break;
                    }
                    hu_dispatch_result_free(agent->alloc, &dispatch_result);
                } else {
                    /* Fallback: sequential if dispatcher fails */
                    for (size_t tc = 0; tc < tc_count; tc++) {
                        const hu_tool_call_t *call = &calls[tc];

                        char pol_tn[64];
                        size_t pol_tn_len = call->name_len < sizeof(pol_tn) - 1
                                                ? call->name_len
                                                : sizeof(pol_tn) - 1;
                        if (pol_tn_len > 0 && call->name)
                            memcpy(pol_tn, call->name, pol_tn_len);
                        pol_tn[pol_tn_len] = '\0';

                        /* TTL cache check on sequential path */
                        hu_tool_cache_ttl_t *seq_cache =
                            agent->infra.tool_cache_ttl
                                ? (hu_tool_cache_ttl_t *)agent->infra.tool_cache_ttl
                                : NULL;
                        const char *seq_args = call->arguments ? call->arguments : "";
                        size_t seq_args_len = call->arguments_len;
                        if (seq_cache &&
                            hu_tool_cache_classify(pol_tn, pol_tn_len, seq_args, seq_args_len) !=
                                HU_TOOL_CACHE_NEVER) {
                            uint64_t ckey =
                                hu_tool_cache_ttl_key(pol_tn, pol_tn_len, seq_args, seq_args_len);
                            size_t cached_len = 0;
                            const char *cached =
                                hu_tool_cache_ttl_get(seq_cache, ckey, &cached_len);
                            if (cached && cached_len > 0) {
                                hu_error_t hist_err = hu_agent_internal_append_history(
                                    agent, HU_ROLE_TOOL, cached, cached_len, call->name,
                                    call->name_len, call->id, call->id_len);
                                if (hist_err != HU_OK)
                                    hu_log_error("agent_turn", NULL, "history append failed: %s",
                                                 hu_error_string(hist_err));
                                continue;
                            }
                        }

                        hu_tool_t *tool =
                            hu_agent_internal_find_tool(agent, call->name, call->name_len);
                        if (!tool) {
                            hu_error_t hist_err = hu_agent_internal_append_history(
                                agent, HU_ROLE_TOOL, "tool not found", 14, call->name,
                                call->name_len, call->id, call->id_len);
                            if (hist_err != HU_OK)
                                hu_log_error("agent_turn", NULL, "history append failed: %s",
                                             hu_error_string(hist_err));
                            continue;
                        }

                        /* Full pre-execute envelope (sequential path). */
                        const char *pre_hook_args = call->arguments ? call->arguments : "";
                        hu_tool_result_t result = {0};
                        hu_tool_gate_t seq_gate = hu_agent_internal_pre_execute_checks(
                            agent, pol_tn, pol_tn_len, pre_hook_args, strlen(pre_hook_args),
                            &result);
                        bool force_approval = (agent->autonomy_level == HU_AUTONOMY_SUPERVISED) ||
                                              (agent->autonomy_level == HU_AUTONOMY_ASSISTED &&
                                               hu_tool_risk_level(pol_tn) >= HU_RISK_MEDIUM);

                        if (seq_gate == HU_TOOL_GATE_DENY) {
                            const char *dm =
                                result.error_msg ? result.error_msg : "denied by security";
                            size_t dl = result.error_msg ? result.error_msg_len : 18;
                            hu_agent_internal_append_history(agent, HU_ROLE_TOOL, dm, dl,
                                                             call->name, call->name_len, call->id,
                                                             call->id_len);
                            hu_agent_internal_post_hook_fire(agent, pol_tn, pol_tn_len,
                                                             pre_hook_args, strlen(pre_hook_args),
                                                             &result);
                            hu_tool_result_free(agent->alloc, &result);
                            continue;
                        }

                        if (seq_gate == HU_TOOL_GATE_NEED_APPROVAL || force_approval) {
                            if (seq_gate != HU_TOOL_GATE_NEED_APPROVAL) {
                                result = hu_tool_result_fail("pending approval", 16);
                                result.needs_approval = true;
                            }
                        } else {
                            result = hu_tool_result_fail("invalid arguments", 16);
                            hu_json_value_t *args = NULL;
                            if (call->arguments_len > 0) {
                                hu_error_t pe = hu_json_parse(agent->alloc, call->arguments,
                                                              call->arguments_len, &args);
                                if (pe == HU_OK && args) {
                                    if (call->name && call->name_len > 0)
                                        hu_agent_turn_state_track_tool(agent, call->name,
                                                                       call->name_len);
                                    tool->vtable->execute(tool->ctx, agent->alloc, args, &result);
                                    hu_json_free(agent->alloc, args);
                                }
                            }
                        }

                        turn_tools_causal_and_history_guard(agent, call, pol_tn, pol_tn_len,
                                                            &result);

                        if (result.needs_approval && !agent->approval_cb) {
                            hu_tool_result_free(agent->alloc, &result);
                            result = hu_tool_result_fail("requires human approval", 23);
                        }

                        /* Approval retry for sequential fallback path */
                        if (result.needs_approval && agent->approval_cb) {
                            char seq_tn[64];
                            size_t seq_n = (call->name_len < sizeof(seq_tn) - 1)
                                               ? call->name_len
                                               : sizeof(seq_tn) - 1;
                            if (seq_n > 0 && call->name)
                                memcpy(seq_tn, call->name, seq_n);
                            seq_tn[seq_n] = '\0';
                            if (agent->approval_cb(agent->approval_ctx, seq_tn,
                                                   call->arguments ? call->arguments : "")) {
                                hu_tool_result_free(agent->alloc, &result);
                                if (agent->policy)
                                    agent->policy->pre_approved = true;
                                hu_json_value_t *retry_args = NULL;
                                if (call->arguments_len > 0) {
                                    hu_error_t jerr =
                                        hu_json_parse(agent->alloc, call->arguments,
                                                      call->arguments_len, &retry_args);
                                    if (jerr != HU_OK)
                                        hu_log_error("agent_turn", NULL,
                                                     "tool args JSON parse failed");
                                }
                                result = hu_tool_result_fail("invalid arguments", 16);
                                if (retry_args) {
                                    if (call->name && call->name_len > 0)
                                        hu_agent_turn_state_track_tool(agent, call->name,
                                                                       call->name_len);
                                    tool->vtable->execute(tool->ctx, agent->alloc, retry_args,
                                                          &result);
                                    hu_json_free(agent->alloc, retry_args);
                                }
                            } else {
                                hu_tool_result_free(agent->alloc, &result);
                                result = hu_tool_result_fail("user denied action", 18);
                            }
                        }

                        /* Post-hook pipeline (sequential path, centralized via
                         * hu_agent_internal_post_hook_fire). */
                        {
                            const char *seq_args2 = call->arguments ? call->arguments : "";
                            hu_agent_internal_post_hook_fire(agent, pol_tn, pol_tn_len, seq_args2,
                                                             strlen(seq_args2), &result);
                        }

                        const char *res_content = result.success ? result.output : result.error_msg;
                        size_t res_len = result.success ? result.output_len : result.error_msg_len;

                        /* Capture media path from tool result for channel attachment */
                        if (result.success && result.media_path && result.media_path_len > 0 &&
                            agent->generated_media_count < 4) {
                            char *mp =
                                hu_strndup(agent->alloc, result.media_path, result.media_path_len);
                            if (mp)
                                agent->generated_media[agent->generated_media_count++] = mp;
                        }

                        /* TTL cache store on sequential path */
                        if (seq_cache && result.success && res_content && res_len > 0 &&
                            hu_tool_cache_classify(pol_tn, pol_tn_len, seq_args, seq_args_len) !=
                                HU_TOOL_CACHE_NEVER) {
                            int64_t ttl = hu_tool_cache_ttl_default_for(pol_tn, pol_tn_len);
                            uint64_t skey =
                                hu_tool_cache_ttl_key(pol_tn, pol_tn_len, seq_args, seq_args_len);
                            (void)hu_tool_cache_ttl_put(seq_cache, skey, res_content, res_len, ttl);
                        }

                        hu_error_t hist_err = hu_agent_internal_append_history(
                            agent, HU_ROLE_TOOL, res_content, res_len, call->name, call->name_len,
                            call->id, call->id_len);
                        if (hist_err != HU_OK)
                            hu_log_error("agent_turn", NULL, "history append failed: %s",
                                         hu_error_string(hist_err));

                        if (agent->audit_logger) {
                            hu_audit_event_t aev;
                            hu_audit_event_init(&aev, HU_AUDIT_COMMAND_EXECUTION);
                            hu_audit_event_with_identity(
                                &aev, agent->agent_id,
                                agent->model_name ? agent->model_name : "unknown", NULL);
                            hu_audit_event_with_action(&aev, pol_tn, "tool", result.success, true);
                            hu_audit_event_with_result(&aev, result.success, 0, 0,
                                                       result.success ? NULL : result.error_msg);
                            hu_audit_logger_log(agent->audit_logger, &aev);
                        }

                        hu_tool_result_free(agent->alloc, &result);
                        if (agent->cancel_requested)
                            break;
                    }
                }
                /* Unconditional cleanup: dispatch_allowed must be freed on BOTH the
                 * result-processing branch AND the sequential fallback. When policy
                 * denies every call, nothing is dispatched, result_array stays NULL,
                 * and the fallback branch runs — freeing only inside the result
                 * branch leaked the array there (caught by LSan in the RL nightly
                 * once the full suite ran under it again, 2026-07-25). */
                if (dispatch_allowed) {
                    agent->alloc->free(agent->alloc->ctx, dispatch_allowed,
                                       dispatch_count * sizeof(bool));
                    dispatch_allowed = NULL;
                }
                /* Free TTL cache arrays — merged_results owns cached copies;
                 * dispatch_result.results ownership was transferred into merged_results
                 * for partial-cache scenarios, so only free the container. */
                if (ttl_hits)
                    agent->alloc->free(agent->alloc->ctx, ttl_hits, tc_count * sizeof(bool));
                if (merged_results) {
                    if (ttl_hit_count > 0 && ttl_hit_count < tc_count && dispatch_result.results) {
                        agent->alloc->free(agent->alloc->ctx, dispatch_result.results,
                                           uncached_count * sizeof(hu_tool_result_t));
                        dispatch_result.results = NULL;
                    }
                    agent->alloc->free(agent->alloc->ctx, merged_results,
                                       tc_count * sizeof(hu_tool_result_t));
                }
            }
        skip_dispatcher:
            (void)0;
        }
    }
    turn_ctx->loop.turn_tool_results_count = turn_tool_results_count;
    return HU_OK;
}
