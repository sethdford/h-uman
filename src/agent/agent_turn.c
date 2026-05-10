/* Core turn execution: hu_agent_turn and turn-local helpers */
#include "agent_internal.h"
#include "human/agent/humanness.h"
#include "human/config.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/data/loader.h"

#include "human/agent/choreography.h"
#include "human/agent/frontier_persist.h"
#include "human/agent/growth_narrative.h"
#include "human/cognition/attachment.h"
#include "human/cognition/novelty.h"
#include "human/cognition/presence.h"
#include "human/cognition/rupture_repair.h"
#include "human/memory/relational_episode.h"
#include "human/persona/creative_voice.h"
#include "human/persona/genuine_boundaries.h"
#include "human/persona/micro_expression.h"
#include "human/persona/narrative_self.h"
#include "human/persona/somatic.h"

#include "human/agent/conv_goals.h"
#include "human/agent/model_router.h"
#include "human/cognition/trust.h"
#include "human/context/humor.h"
#include "human/eval/consistency.h"
#include "human/agent/response_guard.h"
#include "human/agent/response_guard_retry.h"
#include "human/agent/response_verifier.h"
#include "human/memory/fact_extract.h"
#include "human/memory/hallucination_guard.h"
#include "human/memory/neural_memory.h"
#include "human/memory/personal_model.h"
#include "human/agent/world_model_bridge.h"
#include "human/persona/delta_observer.h"
#include "human/persona/humor.h"
#include "human/security/sycophancy_guard.h"

/* Default fallback arrays (NULL-terminated) */
static const char *DEFAULT_MULTISTEP_NEEDLES[] = {" first ", " then ", " finally", " step ",
                                                  " steps ", "step 1", "step 2",   "\n1.",
                                                  "\n2.",    "1) ",    "2) ",      NULL};

static const char *DEFAULT_SENTIMENT_NEG[] = {
    "sad",          "frustrated", "angry", "upset",      "worried",
    "anxious",      "stressed",   "tired", "exhausted",  "hurt",
    "disappointed", "confused",   "lost",  "struggling", NULL};

static const char *DEFAULT_SENTIMENT_POS[] = {"happy",       "excited",  "great",    "amazing",
                                              "wonderful",   "grateful", "thankful", "love",
                                              "celebrating", NULL};

/* Frontier tuning knobs — named constants for clarity.
 * Changing these requires recompilation; they are intentionally not
 * config-driven to keep the parameter surface small (YAGNI).
 * Canonical definitions live in agent_internal.h. */

/* Runtime loaded patterns */
static const char **s_multistep_needles = DEFAULT_MULTISTEP_NEEDLES;
static const char **s_sentiment_neg = DEFAULT_SENTIMENT_NEG;
static const char **s_sentiment_pos = DEFAULT_SENTIMENT_POS;
static size_t s_multistep_needles_slots = 0;
static size_t s_sentiment_neg_slots = 0;
static size_t s_sentiment_pos_slots = 0;

static void at_load_patterns(hu_allocator_t *alloc, hu_json_value_t *root, const char *key,
                             const char ***dest, size_t *out_slot_count) {
    if (!root || !dest)
        return;
    hu_json_value_t *arr = hu_json_object_get(root, key);
    if (!arr || arr->type != HU_JSON_ARRAY)
        return;
    size_t count = arr->data.array.len;
    if (count == 0)
        return;
    const char **patterns =
        (const char **)alloc->alloc(alloc->ctx, (count + 1) * sizeof(const char *));
    if (!patterns)
        return;
    size_t filled = 0;
    for (size_t i = 0; i < count; i++) {
        hu_json_value_t *item = arr->data.array.items[i];
        if (item && item->type == HU_JSON_STRING) {
            const char *s = hu_strndup(alloc, item->data.string.ptr, item->data.string.len);
            if (!s) {
                for (size_t j = 0; j < filled; j++)
                    alloc->free(alloc->ctx, (char *)patterns[j], strlen(patterns[j]) + 1);
                alloc->free(alloc->ctx, patterns, (count + 1) * sizeof(const char *));
                return;
            }
            patterns[filled++] = s;
        }
    }
    patterns[filled] = NULL;
    *dest = patterns;
    if (out_slot_count)
        *out_slot_count = count + 1;
}

static void at_free_patterns(hu_allocator_t *alloc, const char **arr, const char **default_arr,
                             size_t slot_count) {
    if (arr == default_arr || !arr)
        return;
    size_t i = 0;
    while (arr[i]) {
        alloc->free(alloc->ctx, (char *)arr[i], strlen(arr[i]) + 1);
        i++;
    }
    alloc->free(alloc->ctx, (void *)arr, slot_count * sizeof(const char *));
}

hu_error_t hu_agent_turn_data_init(hu_allocator_t *alloc) {
    if (!alloc)
        return HU_ERR_INVALID_ARGUMENT;

    /* Load multistep indicators */
    {
        char *json_data = NULL;
        size_t json_len = 0;
        hu_error_t err =
            hu_data_load(alloc, "agent/multistep_indicators.json", &json_data, &json_len);
        if (err == HU_OK) {
            hu_json_value_t *root = NULL;
            err = hu_json_parse(alloc, json_data, json_len, &root);
            alloc->free(alloc->ctx, json_data, json_len);
            if (err == HU_OK && root) {
                at_load_patterns(alloc, root, "needles", &s_multistep_needles,
                                 &s_multistep_needles_slots);
                hu_json_free(alloc, root);
            }
        }
    }

    /* Load sentiment words */
    {
        char *json_data = NULL;
        size_t json_len = 0;
        hu_error_t err = hu_data_load(alloc, "agent/sentiment_words.json", &json_data, &json_len);
        if (err == HU_OK) {
            hu_json_value_t *root = NULL;
            err = hu_json_parse(alloc, json_data, json_len, &root);
            alloc->free(alloc->ctx, json_data, json_len);
            if (err == HU_OK && root) {
                at_load_patterns(alloc, root, "negative", &s_sentiment_neg, &s_sentiment_neg_slots);
                at_load_patterns(alloc, root, "positive", &s_sentiment_pos, &s_sentiment_pos_slots);
                hu_json_free(alloc, root);
            }
        }
    }

    return HU_OK;
}

void hu_agent_turn_data_cleanup(hu_allocator_t *alloc) {
    if (!alloc)
        return;

    at_free_patterns(alloc, s_multistep_needles, DEFAULT_MULTISTEP_NEEDLES,
                     s_multistep_needles_slots);
    at_free_patterns(alloc, s_sentiment_neg, DEFAULT_SENTIMENT_NEG, s_sentiment_neg_slots);
    at_free_patterns(alloc, s_sentiment_pos, DEFAULT_SENTIMENT_POS, s_sentiment_pos_slots);

    s_multistep_needles = DEFAULT_MULTISTEP_NEEDLES;
    s_sentiment_neg = DEFAULT_SENTIMENT_NEG;
    s_sentiment_pos = DEFAULT_SENTIMENT_POS;
    s_multistep_needles_slots = 0;
    s_sentiment_neg_slots = 0;
    s_sentiment_pos_slots = 0;
}

#ifdef HU_HAS_SKILLS
static hu_error_t agent_skill_route_embed_fn(void *embed_ctx, hu_allocator_t *alloc,
                                             const char *text, size_t text_len, float **out_vec,
                                             size_t *out_dims) {
    hu_embedder_t *emb = (hu_embedder_t *)embed_ctx;
    if (!emb || !emb->vtable || !emb->vtable->embed || !alloc || !out_vec || !out_dims)
        return HU_ERR_INVALID_ARGUMENT;
    hu_embedding_t e;
    memset(&e, 0, sizeof(e));
    hu_error_t err = emb->vtable->embed(emb->ctx, alloc, text, text_len, &e);
    if (err != HU_OK)
        return err;
    *out_vec = e.values;
    *out_dims = e.dim;
    return HU_OK;
}
#endif
#include "human/agent/ab_response.h"
#include "human/agent/approval_gate.h"
#include "human/agent/awareness.h"
#include "human/agent/commands.h"
#include "human/agent/commitment.h"
#include "human/agent/commitment_store.h"
#include "human/agent/compaction.h"
#include "human/agent/dag.h"
#include "human/agent/dag_executor.h"
#include "human/agent/degradation.h"
#include "human/agent/dispatcher.h"
#include "human/agent/hula.h"
#include "human/agent/hula_compiler.h"
#include "human/agent/hula_emergence.h"
#include "human/agent/input_guard.h"
#include "human/agent/llm_compiler.h"
#include "human/agent/mailbox.h"
#include "human/agent/memory_loader.h"
#include "human/agent/outcomes.h"
#include "human/agent/pattern_radar.h"
#include "human/agent/plan_executor.h"
#include "human/agent/planner.h"
#include "human/agent/preferences.h"
#include "human/agent/proactive.h"
#include "human/agent/prompt.h"
#include "human/agent/session_persist.h"
#include "human/agent/spawn.h"
#include "human/agent/superhuman.h"
#include "human/agent/tool_call_parser.h"
#include "human/agent/tool_router.h"
#include "human/cognition/dual_process.h"
#include "human/cognition/emotional.h"
#include "human/cognition/metacognition.h"
#include "human/core/log.h"
#include "human/humanness.h"
#include "human/memory/evolved_opinions.h"
#include "human/memory/lifecycle/semantic_cache.h"
#include "human/observability/bth_metrics.h"
#include "human/observability/otlp.h"
#include "human/tools/validation.h"
#include <math.h>
#ifdef HU_ENABLE_SQLITE
#include "human/cognition/db.h"
#include "human/cognition/episodic.h"
#include "human/cognition/evolving.h"
#include "human/eval/turing_score.h"
#endif
#include "human/agent/orchestrator.h"
#include "human/agent/orchestrator_llm.h"
#include "human/agent/reflection.h"
#include "human/agent/swarm.h"
#include "human/persona/circadian.h"
#include "human/persona/relationship.h"
#include "human/skillforge.h"
#ifdef HU_HAS_SKILLS
#include "human/cognition/skill_routing.h"
#include "human/memory/vector.h"
#endif
#include "human/agent/acp_bridge.h"
#include "human/agent/agent_comm.h"
#include "human/agent/kv_cache.h"
#include "human/agent/prompt_cache.h"
#include "human/context.h"
#include "human/context/conversation.h"
#include "human/context_engine.h"
#include "human/context_tokens.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/cost.h"
#include "human/eval/turing_score.h"
#include "human/hook.h"
#include "human/hook_pipeline.h"
#include "human/memory/consolidation.h"
#include "human/memory/deep_extract.h"
#include "human/memory/emotional_moments.h"
#include "human/memory/fast_capture.h"
#include "human/memory/stm.h"
#include "human/memory/superhuman.h"
#include "human/memory/tiers.h"
#include "human/permission.h"
#include "human/persona.h"
#include "human/provider.h"
#include "human/security.h"
#include "human/security/causal_armor.h"
#include "human/security/companion_safety.h"
#include "human/security/cot_audit.h"
#include "human/security/history_scorer.h"
#include "human/security/moderation.h"
#include "human/security/sensitivity.h"
#include "human/tools/cache_ttl.h"
#include "human/voice.h"
#ifdef HU_ENABLE_SQLITE
#include "human/agent/goals.h"
#include "human/experience.h"
#include "human/intelligence/online_learning.h"
#include "human/intelligence/self_improve.h"
#include "human/intelligence/skills_context.h"
#include "human/intelligence/value_learning.h"
#include "human/intelligence/world_model.h"
#include "human/memory.h"
#include "human/memory/contact_graph.h"
#include <sqlite3.h>
#if defined(HU_ENABLE_ML)
#include "human/ml/training_data.h"
#endif
#endif
#include "human/agent/constitutional.h"
#include "human/agent/speculative.h"
#include "human/agent/tree_of_thought.h"
#include "human/agent/uncertainty.h"
#if HU_HAS_PWA
#include "human/pwa_context.h"
#endif
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

static void agent_turn_hula_append_histories(hu_agent_t *agent, const hu_hula_program_t *prog,
                                             const hu_hula_exec_t *exec) {
    if (!agent || !prog || !exec)
        return;
    for (size_t i = 0; i < prog->node_count; i++) {
        const hu_hula_node_t *cn = &prog->nodes[i];
        if (cn->op != HU_HULA_CALL && cn->op != HU_HULA_DELEGATE && cn->op != HU_HULA_EMIT)
            continue;
        const hu_hula_result_t *hr = cn->id ? hu_hula_exec_result(exec, cn->id) : NULL;
        const char *out_text = "";
        size_t out_len = 0;
        char idbuf[64];
        const char *tcid = cn->id;
        size_t tcid_len = tcid ? strlen(tcid) : 0;
        if (!tcid) {
            (void)snprintf(idbuf, sizeof(idbuf), "hula_%zu", i);
            tcid = idbuf;
            tcid_len = strlen(idbuf);
        }
        if (hr && hr->status == HU_HULA_DONE && hr->output) {
            out_text = hr->output;
            out_len = hr->output_len;
        } else if (hr && hr->error) {
            out_text = hr->error;
            out_len = hr->error_len;
        }
        const char *nm;
        size_t nm_len;
        if (cn->op == HU_HULA_CALL) {
            nm = cn->tool_name ? cn->tool_name : "hula";
            nm_len = cn->tool_name ? strlen(cn->tool_name) : 4;
        } else if (cn->op == HU_HULA_DELEGATE) {
            nm = "hula_delegate";
            nm_len = 13;
        } else {
            nm = "hula_emit";
            nm_len = 9;
        }
        hu_error_t hist_err = hu_agent_internal_append_history(agent, HU_ROLE_TOOL, out_text,
                                                               out_len, nm, nm_len, tcid, tcid_len);
        if (hist_err != HU_OK)
            hu_log_warn("agent", NULL, "failed to append tool result to history: %s",
                        hu_error_string(hist_err));
    }
}

#ifndef HU_IS_TEST
static void hula_compiler_agent_done(void *ctx, const hu_hula_program_t *prog,
                                     const hu_hula_exec_t *exec) {
    hu_agent_t *agent = ctx;
    agent_turn_hula_append_histories(agent, prog, exec);
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
    pthread_mutex_unlock(&g_dag_parallel_prep_mutex);
    if (!dag_tool || !dag_tool->vtable) {
        node->status = HU_DAG_FAILED;
        if (resolved_args)
            w->agent->alloc->free(w->agent->alloc->ctx, resolved_args, resolved_len + 1);
        return NULL;
    }

    hu_tool_result_t dag_result = hu_tool_result_fail("invalid", 7);
    hu_json_value_t *dag_args = NULL;
    if (use_args) {
        hu_error_t jerr = hu_json_parse(w->agent->alloc, use_args, strlen(use_args), &dag_args);
        if (jerr != HU_OK)
            hu_log_error("agent_turn", NULL, "DAG tool args parse failed");
    }
    if (dag_args && dag_tool->vtable->execute)
        dag_tool->vtable->execute(dag_tool->ctx, w->agent->alloc, dag_args, &dag_result);
    if (dag_args)
        hu_json_free(w->agent->alloc, dag_args);
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

static bool message_looks_multistep_for_orchestrator(const char *m, size_t mlen) {
    if (!m || mlen < 48)
        return false;
    for (size_t ni = 0; s_multistep_needles[ni]; ni++) {
        const char *n = s_multistep_needles[ni];
        size_t nl = strlen(n);
        if (nl > mlen)
            continue;
        for (size_t j = 0; j + nl <= mlen; j++) {
            if (memcmp(m + j, n, nl) == 0)
                return true;
        }
    }
    return false;
}

#ifndef HU_IS_TEST
/* Zero + inherit parent fields into *tpl (for hu_hula_compiler_chat_compile_execute or set_spawn).
 */
static void agent_turn_hula_fill_spawn_tpl(hu_agent_t *agent, hu_spawn_config_t *tpl) {
    if (!tpl)
        return;
    memset(tpl, 0, sizeof(*tpl));
    if (agent)
        hu_spawn_config_apply_parent_agent(tpl, agent);
}

/* *tpl must be caller stack storage valid until hu_hula_exec_run returns (exec stores its address).
 */
static void agent_turn_hula_exec_bind_spawn(hu_agent_t *agent, hu_hula_exec_t *exec,
                                            hu_spawn_config_t *tpl) {
    if (!agent || !exec || !tpl || !agent->agent_pool)
        return;
    agent_turn_hula_fill_spawn_tpl(agent, tpl);
    hu_hula_exec_set_spawn(exec, agent->agent_pool, tpl);
}
#endif

/* Strip leaked <tool_call>...</tool_call> from allocator-owned assistant text.
 * Same semantics as the primary post-chat path; used for follow-up chats
 * (metacognition regen, A/B alternates) that bypass the main resp pipeline. */
static void agent_turn_strip_leaked_tool_calls_owned(hu_allocator_t *alloc, char **text,
                                                     size_t *text_len) {
    if (!alloc || !text || !text_len || !*text || *text_len == 0)
        return;
    char *tc_stripped = NULL;
    size_t tc_stripped_len = 0;
    if (hu_text_tool_calls_strip(alloc, *text, *text_len, &tc_stripped, &tc_stripped_len) != HU_OK)
        return;
    if (tc_stripped) {
        if (tc_stripped_len != *text_len || memcmp(tc_stripped, *text, tc_stripped_len) != 0) {
            alloc->free(alloc->ctx, *text, *text_len + 1);
            *text = tc_stripped;
            *text_len = tc_stripped_len;
        } else {
            alloc->free(alloc->ctx, tc_stripped, tc_stripped_len + 1);
        }
    } else {
        alloc->free(alloc->ctx, *text, *text_len + 1);
        *text = NULL;
        *text_len = 0;
    }
}

hu_error_t hu_agent_turn(hu_agent_t *agent, const char *msg, size_t msg_len, char **response_out,
                         size_t *response_len_out) {
    if (!agent || !msg || !response_out)
        return HU_ERR_INVALID_ARGUMENT;
    if (!agent->provider.vtable)
        return HU_ERR_INVALID_ARGUMENT;
    *response_out = NULL;
    if (response_len_out)
        *response_len_out = 0;

    if (getenv("HU_DEBUG"))
        hu_log_info("agent_turn", NULL, "ENTER agent_turn msg_len=%zu", msg_len);

    hu_agent_set_current_for_tools(agent);

    /* Free any previously-built humanness context, then build fresh for this turn */
    hu_agent_free_turn_context(agent);
    hu_agent_build_turn_context(agent);

    /* Speculative cache: check for pre-computed response */
    if (agent->infra.speculative_cache) {
        hu_speculative_config_t spec_cfg = hu_speculative_config_default();
        hu_prediction_t *hit = NULL;
        int64_t now = (int64_t)time(NULL);
        if (hu_speculative_cache_lookup(agent->infra.speculative_cache, msg, msg_len, now,
                                        &spec_cfg, &hit) == HU_OK &&
            hit) {
            *response_out = hu_strndup(agent->alloc, hit->response, hit->response_len);
            if (*response_out) {
                if (response_len_out)
                    *response_len_out = hit->response_len;
                hu_agent_clear_current_for_tools();
                return HU_OK;
            }
        }
    }

    /* Semantic response cache: check for semantically similar past query */
    if (agent->infra.response_cache) {
        hu_semantic_cache_hit_t cache_hit;
        memset(&cache_hit, 0, sizeof(cache_hit));
        if (hu_semantic_cache_get(agent->infra.response_cache, agent->alloc, msg, msg_len, msg,
                                  msg_len, &cache_hit) == HU_OK &&
            cache_hit.response) {
            if (cache_hit.similarity >= 0.92f) {
                *response_out = cache_hit.response;
                if (response_len_out)
                    *response_len_out = strlen(cache_hit.response);
                hu_agent_clear_current_for_tools();
                return HU_OK;
            }
            hu_semantic_cache_hit_free(agent->alloc, &cache_hit);
        }
    }

    hu_agent_internal_process_mailbox_messages(agent);

    char *slash_resp = hu_agent_handle_slash_command(agent, msg, msg_len);
    if (slash_resp) {
        hu_agent_clear_current_for_tools();
        *response_out = slash_resp;
        if (response_len_out)
            *response_len_out = strlen(slash_resp);
        return HU_OK;
    }

    /* Log workflow step start */
    if (agent->infra.workflow_log) {
        hu_workflow_event_t ev = {0};
        ev.type = HU_WF_EVENT_STEP_STARTED;
        ev.timestamp = hu_workflow_event_current_timestamp_ms();
        hu_workflow_event_log_append(agent->infra.workflow_log, agent->alloc, &ev);
    }

    /* Prompt injection defense-in-depth */
    {
        hu_injection_risk_t risk = HU_INJECTION_SAFE;
        hu_error_t guard_err = hu_input_guard_check(msg, msg_len, &risk);
        if (guard_err != HU_OK) {
            hu_agent_clear_current_for_tools();
            return guard_err;
        }
        if (risk == HU_INJECTION_HIGH_RISK) {
            if (agent->observer) {
                hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_ERR};
                ev.data.err.component = "input_guard";
                ev.data.err.message = "high-risk injection pattern detected";
                hu_observer_record_event(*agent->observer, &ev);
            }
            *response_out = hu_strndup(agent->alloc,
                                       "I can't process that request due to safety concerns.", 52);
            if (response_len_out)
                *response_len_out = 52;
            hu_agent_clear_current_for_tools();
            return HU_OK;
        }
    }

    /* W5 producer (FIX 9) + W13 learner bridge (post-FIX-19):
     *
     * Scan the safe user message for explicit persona-correction phrases
     * ("be more X", "stop saying Y", etc.) and record them as delta
     * proposals. The daemon's daily evolver (FIX 3) reads this table at
     * 3 AM and applies stable proposals. The `_with_learner` variant
     * additionally drains every just-proposed delta through
     * `hu_learner_bridge_emit_persona_deltas` so the W13 learner can use
     * them at the next sleep-time training tick. `agent->learner` is
     * tolerated NULL by the bridge — installations without ML enabled
     * get the propose-only path identical to FIX 9. */
    if (agent->verifier_graph && agent->memory_session_id && agent->memory_session_id_len > 0) {
        size_t observed = 0;
        hu_persona_observe_user_correction_with_learner(
            agent->verifier_graph, agent->learner, agent->memory_session_id,
            agent->memory_session_id_len, agent->active_channel, agent->active_channel_len, msg,
            msg_len, 0, &observed);
        if (observed > 0)
            agent->persona_deltas_proposed += observed;
    }

    /* Automatic planning + execution for complex tasks */
    char *plan_ctx = NULL;
    size_t plan_ctx_len = 0;
#ifndef HU_IS_TEST
    if (agent->history_count > 0) {
        size_t scan_n = agent->history_count < 10 ? agent->history_count : 10;
        for (size_t k = 0; k < scan_n; k++) {
            size_t hi = agent->history_count - 1 - k;
            if (agent->history[hi].role != HU_ROLE_SYSTEM || !agent->history[hi].content)
                continue;
            const char *hc = agent->history[hi].content;
            if (strncmp(hc, "[ACTIVE_PLAN]", 13) != 0)
                continue;
            if (plan_ctx == NULL) {
                size_t clen = strlen(hc);
                plan_ctx = hu_strndup(agent->alloc, hc, clen);
                if (plan_ctx)
                    plan_ctx_len = clen;
            }
            break;
        }
    }
    if (msg_len > 200 && agent->tools_count >= 5 && agent->provider.vtable &&
        agent->provider.vtable->chat) {
        const char *tool_names[32];
        size_t tn_count = 0;
        for (size_t ti = 0; ti < agent->tools_count && tn_count < 32; ti++) {
            if (agent->tools[ti].vtable && agent->tools[ti].vtable->name)
                tool_names[tn_count++] = agent->tools[ti].vtable->name(agent->tools[ti].ctx);
        }
        hu_plan_t *plan = NULL;
        if (hu_planner_generate(agent->alloc, &agent->provider, agent->model_name,
                                agent->model_name_len, msg, msg_len, tool_names, tn_count,
                                &plan) == HU_OK &&
            plan) {
            /* MCTS refinement for complex plans */
            if (agent->mcts_planner_enabled && plan && hu_planner_plan_needs_mcts(plan)) {
                hu_plan_t *mcts_plan = NULL;
                if (hu_planner_plan_mcts(agent->alloc, &agent->provider, agent->model_name,
                                         agent->model_name_len, msg, msg_len, tool_names, tn_count,
                                         &mcts_plan) == HU_OK &&
                    mcts_plan) {
                    hu_plan_free(agent->alloc, plan);
                    plan = mcts_plan;
                }
            }
            hu_plan_executor_t exec;
            hu_plan_executor_init(&exec, agent->alloc, &agent->provider, agent->model_name,
                                  agent->model_name_len, agent->tools, agent->tools_count);
            hu_plan_exec_result_t exec_result;
            memset(&exec_result, 0, sizeof(exec_result));
            hu_error_t pe = hu_plan_executor_run(&exec, plan, msg, msg_len, &exec_result);
            if (pe == HU_OK && exec_result.summary_len > 0) {
                if (plan_ctx)
                    agent->alloc->free(agent->alloc->ctx, plan_ctx, plan_ctx_len + 1);
                plan_ctx = hu_strndup(agent->alloc, exec_result.summary, exec_result.summary_len);
                plan_ctx_len = plan_ctx ? exec_result.summary_len : 0;
            } else {
                char plan_buf[1024];
                int pn =
                    snprintf(plan_buf, sizeof(plan_buf), "[PLAN]: %zu steps planned, %zu completed",
                             plan->steps_count, exec_result.steps_completed);
                if (pn > 0 && (size_t)pn < sizeof(plan_buf)) {
                    if (plan_ctx)
                        agent->alloc->free(agent->alloc->ctx, plan_ctx, plan_ctx_len + 1);
                    plan_ctx = hu_strndup(agent->alloc, plan_buf, (size_t)pn);
                    plan_ctx_len = plan_ctx ? (size_t)pn : 0;
                }
            }
            if (plan && exec_result.steps_completed < plan->steps_count) {
                char plan_mem[2048];
                int pm = snprintf(plan_mem, sizeof(plan_mem),
                                  "[ACTIVE_PLAN] %zu/%zu steps completed. Remaining: ",
                                  exec_result.steps_completed, plan->steps_count);
                for (size_t si = exec_result.steps_completed;
                     si < plan->steps_count && pm > 0 && (size_t)pm < sizeof(plan_mem) - 128;
                     si++) {
                    const char *desc = plan->steps[si].description;
                    size_t dlen = desc ? strlen(desc) : 0;
                    if (dlen > 100)
                        dlen = 100;
                    int add = snprintf(plan_mem + (size_t)pm, sizeof(plan_mem) - (size_t)pm,
                                       "\n  Step %zu: %.*s", si + 1, (int)dlen, desc ? desc : "");
                    if (add > 0)
                        pm += add;
                }
                if (pm > 0 && (size_t)pm < sizeof(plan_mem)) {
                    hu_error_t plan_hist_err = hu_agent_internal_append_history(
                        agent, HU_ROLE_SYSTEM, plan_mem, (size_t)pm, NULL, 0, NULL, 0);
                    if (plan_hist_err != HU_OK)
                        hu_log_warn("agent", NULL, "failed to append plan context to history: %s",
                                    hu_error_string(plan_hist_err));
                }
            }
            hu_plan_free(agent->alloc, plan);
        }
    }
#endif

    hu_error_t err =
        hu_agent_internal_append_history(agent, HU_ROLE_USER, msg, msg_len, NULL, 0, NULL, 0);
    if (err != HU_OK) {
        if (getenv("HU_DEBUG"))
            hu_log_error("agent_turn", NULL, "FAIL at history_append: %s", hu_error_string(err));
        if (plan_ctx)
            agent->alloc->free(agent->alloc->ctx, plan_ctx, plan_ctx_len + 1);
        hu_agent_clear_current_for_tools();
        return err;
    }

#ifndef HU_IS_TEST
    (void)hu_personal_model_ingest(&agent->personal_model, msg, msg_len, true, (int64_t)time(NULL));
#endif

    /* Context engine: ingest the user message for RAG/graph indexing */
    if (agent->infra.context_engine) {
        hu_context_engine_t *ce = (hu_context_engine_t *)agent->infra.context_engine;
        if (ce->vtable && ce->vtable->ingest) {
            hu_context_message_t ce_msg = {.role = "user",
                                           .role_len = 4,
                                           .content = msg,
                                           .content_len = msg_len,
                                           .timestamp = (int64_t)time(NULL)};
            ce->vtable->ingest(ce->ctx, agent->alloc, &ce_msg);
        }
    }

    /* ACP inbox: check for pending inter-agent messages */
    char *acp_context = NULL;
    size_t acp_context_len = 0;
    if (agent->infra.acp_inbox) {
        hu_acp_inbox_t *inbox = (hu_acp_inbox_t *)agent->infra.acp_inbox;
        size_t pending = hu_acp_inbox_count(inbox, -1);
        if (pending > 0) {
            char acp_buf[2048];
            size_t acp_pos = 0;
            const char *hdr = "[Inter-agent messages]\n";
            size_t hdr_len = strlen(hdr);
            memcpy(acp_buf, hdr, hdr_len);
            acp_pos = hdr_len;
            for (size_t ai = 0; ai < pending && ai < 5; ai++) {
                hu_acp_message_t acp_msg;
                if (hu_acp_inbox_pop(inbox, &acp_msg) != HU_OK)
                    break;
                const char *type_name = hu_acp_msg_type_name(acp_msg.type);
                int n = snprintf(acp_buf + acp_pos, sizeof(acp_buf) - acp_pos,
                                 "- %s from %.*s: %.*s\n", type_name, (int)(acp_msg.sender_id_len),
                                 acp_msg.sender_id ? acp_msg.sender_id : "?",
                                 (int)(acp_msg.payload_len > 200 ? 200 : acp_msg.payload_len),
                                 acp_msg.payload ? acp_msg.payload : "");
                if (n > 0 && acp_pos + (size_t)n < sizeof(acp_buf))
                    acp_pos += (size_t)n;
                hu_acp_message_free(agent->alloc, &acp_msg);
            }
            if (acp_pos > hdr_len) {
                acp_context = hu_strndup(agent->alloc, acp_buf, acp_pos);
                acp_context_len = acp_pos;
            }
        }
    }

    hu_cognition_budget_t cognition_budget =
        hu_cognition_get_budget(HU_COGNITION_FAST, agent->max_tool_iterations);
    char *emotional_ctx = NULL;
    size_t emotional_ctx_len = 0;
    char *episodic_replay = NULL;
    size_t episodic_replay_len = 0;
    bool cognition_skills_shown = false;

    char *somatic_ctx = NULL;
    size_t somatic_ctx_len = 0;
    char *narrative_self_ctx = NULL;
    size_t narrative_self_ctx_len = 0;
    char *presence_ctx = NULL;
    size_t presence_ctx_len = 0;
    char *micro_expr_ctx = NULL;
    size_t micro_expr_ctx_len = 0;
    char *creative_voice_ctx = NULL;
    size_t creative_voice_ctx_len = 0;
    char *novelty_ctx = NULL;
    size_t novelty_ctx_len = 0;
    char *attachment_ctx = NULL;
    size_t attachment_ctx_len = 0;
    char *rupture_ctx = NULL;
    size_t rupture_ctx_len = 0;
    char *growth_ctx = NULL;
    size_t growth_ctx_len = 0;
    char *boundary_ctx = NULL;
    size_t boundary_ctx_len = 0;
    char *rel_episode_ctx = NULL;
    size_t rel_episode_ctx_len = 0;
    char *trust_ctx = NULL;
    size_t trust_ctx_len = 0;
    char *humor_dir = NULL;
    size_t humor_dir_len = 0;
    bool had_humor_dir = false;
    int humor_theory_saved = 0;
    (void)humor_theory_saved;
    char *syc_friction_ctx = NULL;
    size_t syc_friction_ctx_len = 0;
    char *conv_goals_ctx = NULL;
    size_t conv_goals_ctx_len = 0;

    /* Superhuman: observe user message (emotional, silence services) */
    (void)hu_superhuman_observe_all(&agent->superhuman, agent->alloc, msg, msg_len, "user", 4);

    /* Fast-capture and STM: extract entities/emotions, record turn, populate last turn */
    {
        hu_fc_result_t fc_result;
        memset(&fc_result, 0, sizeof(fc_result));
        (void)hu_fast_capture(agent->alloc, msg, msg_len, &fc_result);

        uint64_t ts_ms = (uint64_t)time(NULL) * 1000;
        err = hu_stm_record_turn(&agent->stm, "user", 4, msg, msg_len, ts_ms);
        if (err == HU_OK) {
            size_t last_idx = hu_stm_count(&agent->stm) - 1;
            if (fc_result.primary_topic && fc_result.primary_topic[0]) {
                (void)hu_stm_turn_set_primary_topic(&agent->stm, last_idx, fc_result.primary_topic,
                                                    strlen(fc_result.primary_topic));
            }
            for (size_t i = 0; i < fc_result.entity_count; i++) {
                const hu_fc_entity_match_t *e = &fc_result.entities[i];
                uint32_t mention = 1;
                (void)hu_stm_turn_add_entity(&agent->stm, last_idx, e->name, e->name_len,
                                             e->type ? e->type : "entity",
                                             e->type ? e->type_len : 6, mention);
            }
            for (size_t i = 0; i < fc_result.emotion_count; i++) {
                (void)hu_stm_turn_add_emotion(&agent->stm, last_idx, fc_result.emotions[i].tag,
                                              fc_result.emotions[i].intensity);
            }
        }

        /* Pattern radar: observe entities as topic recurrence, emotions as emotional trend */
        {
            char ts_buf[32];
            int ts_n = snprintf(ts_buf, sizeof(ts_buf), "%llu", (unsigned long long)(ts_ms / 1000));
            const char *ts = ts_n > 0 ? ts_buf : NULL;
            size_t ts_len = (ts_n > 0 && ts_n < (int)sizeof(ts_buf)) ? (size_t)ts_n : 0;

            for (size_t i = 0; i < fc_result.entity_count; i++) {
                const hu_fc_entity_match_t *e = &fc_result.entities[i];
                if (e->name && e->name_len > 0) {
                    (void)hu_pattern_radar_observe(
                        &agent->radar, e->name, e->name_len, HU_PATTERN_TOPIC_RECURRENCE,
                        e->type ? e->type : NULL, e->type ? e->type_len : 0, ts, ts_len);
                }
            }
            static const char *emotion_names[] = {"neutral",     "joy",        "sadness",
                                                  "anger",       "fear",       "surprise",
                                                  "frustration", "excitement", "anxiety"};
            for (size_t i = 0; i < fc_result.emotion_count; i++) {
                hu_emotion_tag_t tag = fc_result.emotions[i].tag;
                if ((size_t)tag < sizeof(emotion_names) / sizeof(emotion_names[0])) {
                    const char *name = emotion_names[tag];
                    (void)hu_pattern_radar_observe(&agent->radar, name, strlen(name),
                                                   HU_PATTERN_EMOTIONAL_TREND, NULL, 0, ts, ts_len);
                }
            }
        }
        hu_fc_result_deinit(&fc_result, agent->alloc);
    }

    /* Cognition: emotional fusion + dual-process dispatch (memory loader budgets, prompt hints) */
    {
        hu_emotional_perception_t percep;
        memset(&percep, 0, sizeof(percep));
        percep.voice_valence = NAN;
        percep.egraph_dominant = HU_EMOTION_NEUTRAL;
        percep.egraph_intensity = 0.0f;

        size_t stm_n = hu_stm_count(&agent->stm);
        const hu_stm_emotion_t *stm_emo = NULL;
        size_t stm_emo_count = 0;
        if (stm_n > 0) {
            const hu_stm_turn_t *lt = hu_stm_get(&agent->stm, stm_n - 1);
            if (lt && lt->emotion_count > 0) {
                stm_emo = lt->emotions;
                stm_emo_count = lt->emotion_count;
            }
        }
        percep.stm_emotions = stm_emo;
        percep.stm_emotion_count = stm_emo_count;
        percep.fast_capture = NULL;
        percep.conversation = NULL;

        hu_emotional_cognition_perceive(&agent->infra.emotional_cognition, &percep);

        size_t recent_tools = 0;
        if (agent->history_count > 1) {
            for (size_t hi = agent->history_count - 1; hi > 0; hi--) {
                if (agent->history[hi - 1].role == HU_ROLE_TOOL) {
                    recent_tools++;
                    if (recent_tools >= 8)
                        break;
                }
            }
        }

        hu_cognition_dispatch_input_t d_in = {
            .message = msg,
            .message_len = msg_len,
            .emotional = &agent->infra.emotional_cognition,
            .tools_count = agent->tools_count,
            .recent_tool_calls = recent_tools,
            .agent_max_tool_iterations = agent->max_tool_iterations,
        };
        agent->infra.current_cognition_mode = hu_cognition_dispatch(&d_in);
        cognition_budget = hu_cognition_get_budget(agent->infra.current_cognition_mode,
                                                   agent->max_tool_iterations);

        if (agent->observer) {
            hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_COGNITION_MODE};
            ev.data.cognition_mode.mode =
                hu_cognition_mode_name(agent->infra.current_cognition_mode);
            HU_OBS_SAFE_RECORD_EVENT(agent, &ev);
        }
        if (agent->bth_metrics) {
            switch (agent->infra.current_cognition_mode) {
            case HU_COGNITION_FAST:
                agent->bth_metrics->cognition_fast_turns++;
                break;
            case HU_COGNITION_SLOW:
                agent->bth_metrics->cognition_slow_turns++;
                break;
            case HU_COGNITION_EMOTIONAL:
                agent->bth_metrics->cognition_emotional_turns++;
                break;
            default:
                break;
            }
        }
    }

    /* Commitment detection: extract promises, intentions, reminders, goals from user message */
    if (agent->commitment_store) {
        hu_commitment_detect_result_t commit_result;
        memset(&commit_result, 0, sizeof(commit_result));
        hu_error_t cerr =
            hu_commitment_detect(agent->alloc, msg, msg_len, "user", 4, &commit_result);
        if (cerr == HU_OK && commit_result.count > 0) {
            const char *sess = agent->memory_session_id;
            size_t sess_len = agent->memory_session_id ? agent->memory_session_id_len : 0;
            for (size_t i = 0; i < commit_result.count; i++) {
                hu_error_t cs_err = hu_commitment_store_save(
                    agent->commitment_store, &commit_result.commitments[i], sess, sess_len);
                if (cs_err != HU_OK)
                    hu_log_error("agent", NULL, "commitment save failed: %s",
                                 hu_error_string(cs_err));
            }
        }
        hu_commitment_detect_result_deinit(&commit_result, agent->alloc);
    }

    /* Detect preferences from user corrections and store them */
    bool is_correction = hu_preferences_is_correction(msg, msg_len);
    if (agent->memory && is_correction) {
        size_t pref_len = 0;
        char *pref = hu_preferences_extract(agent->alloc, msg, msg_len, &pref_len);
        if (pref) {
            hu_error_t pref_err = hu_preferences_store(agent->memory, agent->alloc, pref, pref_len);
            if (pref_err != HU_OK)
                hu_log_warn("agent", NULL, "preference store failed: %s",
                            hu_error_string(pref_err));
            agent->alloc->free(agent->alloc->ctx, pref, pref_len + 1);
        }
    }

    /* Outcome tracking: record corrections and positive feedback */
    if (agent->outcomes) {
        if (is_correction) {
            const char *prev_response = NULL;
            if (agent->history_count >= 2 &&
                agent->history[agent->history_count - 2].role == HU_ROLE_ASSISTANT)
                prev_response = agent->history[agent->history_count - 2].content;
            hu_outcome_record_correction(agent->outcomes, prev_response, msg);

            if (agent->outcomes->auto_apply_feedback && agent->persona && agent->persona_name &&
                prev_response) {
                hu_persona_feedback_t fb = {
                    .channel = agent->active_channel,
                    .channel_len = agent->active_channel_len,
                    .original_response = prev_response,
                    .original_response_len = strlen(prev_response),
                    .corrected_response = msg,
                    .corrected_response_len = msg_len,
                };
                (void)hu_persona_feedback_record(agent->alloc, agent->persona_name,
                                                 strlen(agent->persona_name), &fb);
            }
        } else if (msg_len >= 5 && msg_len <= 80) {
            /* Detect simple positive feedback */
            bool positive = false;
            for (size_t k = 0; k + 5 <= msg_len && !positive; k++) {
                char c0 = msg[k] | 0x20, c1 = msg[k + 1] | 0x20, c2 = msg[k + 2] | 0x20;
                char c3 = msg[k + 3] | 0x20, c4 = msg[k + 4] | 0x20;
                if (c0 == 't' && c1 == 'h' && c2 == 'a' && c3 == 'n' && c4 == 'k')
                    positive = true;
                if (c0 == 'g' && c1 == 'r' && c2 == 'e' && c3 == 'a' && c4 == 't')
                    positive = true;
                if (k + 6 <= msg_len && c0 == 'p' && c1 == 'e' && c2 == 'r' && c3 == 'f' &&
                    c4 == 'e' && (msg[k + 5] | 0x20) == 'c')
                    positive = true;
            }
            if (positive) {
                hu_outcome_record_positive(agent->outcomes, msg);
                if (agent->frontiers.initialized)
                    hu_tcal_update(&agent->frontiers.trust, 0.6f, 0.5f, 0.5f);
            }
        }
        if (is_correction && agent->frontiers.initialized)
            hu_tcal_update(&agent->frontiers.trust, -0.3f, 0.0f, -0.2f);
    }

    /* Detect tone from recent user messages */
    const char *tone_hint = NULL;
    size_t tone_hint_len = 0;
    {
        const char *recent_msgs[3];
        size_t recent_lens[3];
        size_t rm_count = 0;
        for (size_t i = agent->history_count; i > 0 && rm_count < 3; i--) {
            if (agent->history[i - 1].role == HU_ROLE_USER && agent->history[i - 1].content) {
                recent_msgs[rm_count] = agent->history[i - 1].content;
                recent_lens[rm_count] = agent->history[i - 1].content_len;
                rm_count++;
            }
        }
        if (rm_count > 0) {
            hu_tone_t tone = hu_detect_tone(recent_msgs, recent_lens, rm_count);
            tone_hint = hu_tone_hint_string(tone, &tone_hint_len);
        }
    }

    /* Rhythm matching: when user sends a short casual message, nudge the
     * agent to match their brevity. Long reflective messages get more space. */
    static const char rhythm_short[] =
        " The user sent a very short message — match their energy with a brief, "
        "conversational reply. Don't over-explain.";
    static const char rhythm_long[] =
        " The user wrote a long, thoughtful message — give it the space it deserves "
        "with a proportional, considered response.";
    if (msg_len <= 15 && msg_len > 0 && !tone_hint) {
        tone_hint = rhythm_short;
        tone_hint_len = sizeof(rhythm_short) - 1;
    } else if (msg_len >= 400 && !tone_hint) {
        tone_hint = rhythm_long;
        tone_hint_len = sizeof(rhythm_long) - 1;
    }

    /* Load user preferences for prompt injection */
    char *pref_ctx = NULL;
    size_t pref_ctx_len = 0;
    if (agent->memory) {
        hu_error_t pref_err =
            hu_preferences_load(agent->memory, agent->alloc, &pref_ctx, &pref_ctx_len);
        if (pref_err != HU_OK)
            hu_log_error("agent_turn", NULL, "preferences load failed: %s",
                         hu_error_string(pref_err));
    }

    /* Self-RAG gate: decide whether retrieval is needed before loading memory */
    hu_srag_assessment_t srag_assessment;
    memset(&srag_assessment, 0, sizeof(srag_assessment));
    bool srag_skip_retrieval = false;
    if (agent->sota.sota_initialized && agent->sota.srag_config.enabled) {
        hu_srag_should_retrieve(agent->alloc, &agent->sota.srag_config, msg, msg_len, NULL, 0,
                                &srag_assessment);
        if (srag_assessment.decision == HU_SRAG_NO_RETRIEVAL)
            srag_skip_retrieval = true;
    }

    /* Load memory context for this turn (gated by Self-RAG) */
    char *memory_ctx = NULL;
    size_t memory_ctx_len = 0;
    if (agent->memory && agent->memory->vtable && !srag_skip_retrieval) {
        hu_memory_loader_t loader;
        hu_memory_loader_init(&loader, agent->alloc, agent->memory, agent->retrieval_engine,
                              cognition_budget.max_memory_entries,
                              cognition_budget.max_memory_chars);
        hu_memory_loader_set_facade(&loader, agent->w7_facade);
        hu_error_t load_err = hu_memory_loader_load(
            &loader, msg, msg_len, agent->memory_session_id ? agent->memory_session_id : "",
            agent->memory_session_id ? agent->memory_session_id_len : 0, &memory_ctx,
            &memory_ctx_len);
        if (load_err != HU_OK)
            hu_log_error("agent_turn", NULL, "memory loader failed: %s", hu_error_string(load_err));

        /* Self-RAG: verify relevance of retrieved content */
        if (srag_assessment.decision == HU_SRAG_RETRIEVE_AND_VERIFY && memory_ctx &&
            memory_ctx_len > 0) {
            double relevance = 0.0;
            bool should_use = false;
            hu_srag_verify_relevance(agent->alloc, &agent->sota.srag_config, msg, msg_len,
                                     memory_ctx, memory_ctx_len, &relevance, &should_use);
            if (!should_use) {
                agent->alloc->free(agent->alloc->ctx, memory_ctx, memory_ctx_len + 1);
                memory_ctx = NULL;
                memory_ctx_len = 0;
            }
        }
    }

    /* Check freshness of cached instruction discovery and re-discover if stale */
    if (agent->instruction_discovery &&
        !hu_instruction_discovery_is_fresh(agent->instruction_discovery)) {
        hu_instruction_discovery_destroy(agent->alloc, agent->instruction_discovery);
        agent->instruction_discovery = NULL;
    }

    /* Re-discover instructions if needed */
    if (!agent->instruction_discovery && agent->workspace_dir && agent->workspace_dir_len > 0) {
        hu_error_t disc_err =
            hu_instruction_discovery_run(agent->alloc, agent->workspace_dir,
                                         agent->workspace_dir_len, &agent->instruction_discovery);
        if (disc_err != HU_OK) {
            agent->instruction_discovery = NULL;
        }
    }

    /* Gather instruction context from discovery results */
    char *instruction_ctx = NULL;
    size_t instruction_ctx_len = 0;
    if (agent->instruction_discovery && agent->instruction_discovery->merged_content &&
        agent->instruction_discovery->merged_content_len > 0) {
        instruction_ctx = agent->instruction_discovery->merged_content;
        instruction_ctx_len = agent->instruction_discovery->merged_content_len;
    }

    /* Data quality: validate memory context fragments before assembly */
    if (agent->sota.dq_config.enabled && memory_ctx && memory_ctx_len > 0) {
        hu_dq_fragment_t frag = {
            .content = memory_ctx,
            .content_len = memory_ctx_len,
            .source = "memory",
            .source_len = 6,
        };
        hu_dq_result_t dq_result;
        if (hu_dq_check(&agent->sota.dq_config, &frag, 1, &dq_result) == HU_OK &&
            !dq_result.passed) {
            hu_log_info("agent_turn", NULL, "data quality: %zu issues in memory context",
                        dq_result.issue_count);
        }
    }

    /* Adaptive RAG: select strategy and record for learning */
    hu_rag_strategy_t rag_strategy_used = HU_RAG_NONE;
    if (agent->sota.sota_initialized && !srag_skip_retrieval) {
        rag_strategy_used = hu_adaptive_rag_select(&agent->sota.adaptive_rag, msg, msg_len);
    }

#ifdef HU_ENABLE_SQLITE
    /* W12: goal-conditioned planner recall via the W7 facade bridge.
     * Falls back to v1 hu_memory_recall_for_contact on planner failure
     * or when the facade is not wired. */
    if (agent->memory_session_id && agent->memory_session_id_len > 0) {
        char *contact_text = NULL;
        size_t contact_text_len = 0;
        bool planner_ok = false;

        if (agent->w7_facade) {
            /* P4: route through the LLM planner backend when a provider
             * is available. Under HU_IS_TEST the LLM planner falls back to
             * a deterministic single-step plan so tests stay free of
             * provider I/O. With no provider it degrades to goal-conditioned
             * → heuristic, identical to the original `hu_w12_planner_recall`
             * call. */
            hu_provider_t *provider =
                agent->provider.vtable ? &agent->provider : NULL;
            hu_error_t pe = hu_w12_planner_recall_with_provider(
                agent->w7_facade, agent->alloc, provider,
                /*model=*/NULL, /*model_len=*/0,
                agent->memory_session_id, agent->memory_session_id_len,
                msg, msg_len, 5, 4000, &contact_text, &contact_text_len);
            planner_ok = (pe == HU_OK && contact_text && contact_text_len > 0);
        }

        if (!planner_ok && agent->memory) {
            hu_memory_entry_t *contact_entries = NULL;
            size_t contact_count = 0;
            if (hu_memory_recall_for_contact(agent->memory, agent->alloc,
                                             agent->memory_session_id,
                                             agent->memory_session_id_len, msg, msg_len, 5,
                                             "", 0, &contact_entries, &contact_count) ==
                    HU_OK &&
                contact_entries && contact_count > 0) {
                size_t extra_len = 0;
                for (size_t i = 0; i < contact_count; i++) {
                    if (extra_len > SIZE_MAX - contact_entries[i].content_len - 1)
                        break;
                    extra_len += contact_entries[i].content_len + 1;
                }
                if (extra_len > 0) {
                    contact_text =
                        (char *)agent->alloc->alloc(agent->alloc->ctx, extra_len + 32);
                    if (contact_text) {
                        size_t pos = 0;
                        pos = hu_buf_appendf(contact_text, extra_len + 32, pos,
                                             "[About this contact]\n");
                        for (size_t i = 0; i < contact_count && pos < extra_len + 31;
                             i++) {
                            size_t to_copy = contact_entries[i].content_len;
                            if (pos + to_copy + 1 > extra_len + 31)
                                to_copy = extra_len + 31 - pos;
                            memcpy(contact_text + pos, contact_entries[i].content,
                                   to_copy);
                            pos += to_copy;
                            contact_text[pos++] = '\n';
                        }
                        contact_text[pos] = '\0';
                        contact_text_len = pos;
                    }
                }
                for (size_t i = 0; i < contact_count; i++)
                    hu_memory_entry_free_fields(agent->alloc, &contact_entries[i]);
                agent->alloc->free(agent->alloc->ctx, contact_entries,
                                   contact_count * sizeof(hu_memory_entry_t));
            }
        }

        if (contact_text && contact_text_len > 0) {
            size_t old_len = memory_ctx ? memory_ctx_len : 0;
            size_t new_total = old_len + (old_len > 0 ? 2 : 0) + contact_text_len + 1;
            char *merged = (char *)agent->alloc->alloc(agent->alloc->ctx, new_total);
            if (merged) {
                size_t pos = 0;
                if (memory_ctx && memory_ctx_len > 0) {
                    memcpy(merged, memory_ctx, memory_ctx_len);
                    pos = memory_ctx_len;
                    merged[pos++] = '\n';
                    merged[pos++] = '\n';
                }
                memcpy(merged + pos, contact_text, contact_text_len);
                pos += contact_text_len;
                merged[pos] = '\0';
                if (memory_ctx)
                    agent->alloc->free(agent->alloc->ctx, memory_ctx, memory_ctx_len + 1);
                memory_ctx = merged;
                memory_ctx_len = pos;
            }
            agent->alloc->free(agent->alloc->ctx, contact_text, contact_text_len + 1);
        }
    }
#endif

    /* Build STM context for this turn */
    char *stm_ctx = NULL;
    size_t stm_ctx_len = 0;
    hu_error_t stm_err = hu_stm_build_context(&agent->stm, agent->alloc, &stm_ctx, &stm_ctx_len);
    if (stm_err != HU_OK)
        hu_log_error("agent_turn", NULL, "STM context build failed: %s", hu_error_string(stm_err));
    if (stm_ctx_len > 0 && agent->bth_metrics)
        agent->bth_metrics->emotions_surfaced++;

    /* Build commitment context for this turn */
    char *commitment_ctx = NULL;
    size_t commitment_ctx_len = 0;
    if (agent->commitment_store) {
        const char *sess = agent->memory_session_id;
        size_t sess_len = agent->memory_session_id ? agent->memory_session_id_len : 0;
        (void)hu_commitment_store_build_context(agent->commitment_store, agent->alloc, sess,
                                                sess_len, &commitment_ctx, &commitment_ctx_len);
        if (commitment_ctx_len > 0 && agent->bth_metrics)
            agent->bth_metrics->commitment_followups++;
    }

    /* Build pattern radar context for this turn */
    char *pattern_ctx = NULL;
    size_t pattern_ctx_len = 0;
    (void)hu_pattern_radar_build_context(&agent->radar, agent->alloc, &pattern_ctx,
                                         &pattern_ctx_len);
    if (pattern_ctx_len > 0 && agent->bth_metrics)
        agent->bth_metrics->pattern_insights++;

    /* Build proactive context (milestones, morning briefing, check-in) */
    char *proactive_ctx = NULL;
    size_t proactive_ctx_len = 0;
    {
        uint32_t session_count = 0;
        uint8_t hour = 10;
        session_count = agent->relationship.session_count;
#ifndef HU_IS_TEST
        {
            time_t now = time(NULL);
            struct tm lt_buf;
            struct tm *lt = localtime_r(&now, &lt_buf);
            if (lt)
                hour = (uint8_t)(lt->tm_hour & 0xFF);
        }
#endif
        hu_proactive_result_t proactive_result;
        memset(&proactive_result, 0, sizeof(proactive_result));
        hu_commitment_t *commitments = NULL;
        size_t commitment_count = 0;
        if (agent->commitment_store && agent->memory_session_id &&
            agent->memory_session_id_len > 0) {
            hu_error_t commit_err = hu_commitment_store_list_active(
                agent->commitment_store, agent->alloc, agent->memory_session_id,
                agent->memory_session_id_len, &commitments, &commitment_count);
            if (commit_err != HU_OK)
                hu_log_error("agent_turn", NULL, "commitment list failed: %s",
                             hu_error_string(commit_err));
        }
        hu_error_t proactive_err =
            hu_proactive_check_extended(agent->alloc, session_count, hour, commitments,
                                        commitment_count, NULL, NULL, 0, &proactive_result);
        if (commitments) {
            for (size_t ci = 0; ci < commitment_count; ci++)
                hu_commitment_deinit(&commitments[ci], agent->alloc);
            agent->alloc->free(agent->alloc->ctx, commitments,
                               commitment_count * sizeof(hu_commitment_t));
        }
        if (proactive_err == HU_OK && proactive_result.count > 0) {
            (void)hu_proactive_build_context(&proactive_result, agent->alloc, 8, &proactive_ctx,
                                             &proactive_ctx_len);
            hu_proactive_result_deinit(&proactive_result, agent->alloc);
        }
        /* Merge contextual conversation starter from memory when we have a contact */
        if (agent->memory && agent->memory_session_id && agent->memory_session_id_len > 0) {
            char *starter = NULL;
            size_t starter_len = 0;
            if (hu_proactive_build_starter(agent->alloc, agent->memory, agent->memory_session_id,
                                           agent->memory_session_id_len, &starter,
                                           &starter_len) == HU_OK &&
                starter && starter_len > 0) {
                if (proactive_ctx && proactive_ctx_len > 0) {
                    size_t merged_len = proactive_ctx_len + 2 + starter_len;
                    char *merged = (char *)agent->alloc->alloc(agent->alloc->ctx, merged_len + 1);
                    if (merged) {
                        memcpy(merged, proactive_ctx, proactive_ctx_len);
                        merged[proactive_ctx_len] = '\n';
                        merged[proactive_ctx_len + 1] = '\n';
                        memcpy(merged + proactive_ctx_len + 2, starter, starter_len);
                        merged[merged_len] = '\0';
                        agent->alloc->free(agent->alloc->ctx, proactive_ctx, proactive_ctx_len + 1);
                        agent->alloc->free(agent->alloc->ctx, starter, starter_len + 1);
                        proactive_ctx = merged;
                        proactive_ctx_len = merged_len;
                    } else {
                        agent->alloc->free(agent->alloc->ctx, starter, starter_len + 1);
                    }
                } else {
                    proactive_ctx = starter;
                    proactive_ctx_len = starter_len;
                }
                if (agent->bth_metrics)
                    agent->bth_metrics->starters_built++;
            }
        }
    }

    /* Build superhuman context (commitment, predictive, emotional, silence) */
    char *superhuman_ctx = NULL;
    size_t superhuman_ctx_len = 0;
    {
        agent->superhuman_commitment_ctx.session_id = agent->memory_session_id;
        agent->superhuman_commitment_ctx.session_id_len = agent->memory_session_id_len;
        (void)hu_superhuman_build_context(&agent->superhuman, agent->alloc, &superhuman_ctx,
                                          &superhuman_ctx_len);
#ifdef HU_ENABLE_SQLITE
        /* Superhuman memory: micro-moments, inside jokes, avoidance, topic absences, growth,
         * patterns */
        if (agent->memory && agent->memory_session_id && agent->memory_session_id_len > 0) {
            bool include_avoidance = false;
            include_avoidance = (agent->relationship.stage == HU_REL_TRUSTED ||
                                 agent->relationship.stage == HU_REL_DEEP);
            char *memory_sh_ctx = NULL;
            size_t memory_sh_len = 0;
            if (hu_superhuman_memory_build_context(agent->memory, agent->alloc,
                                                   agent->memory_session_id,
                                                   agent->memory_session_id_len, include_avoidance,
                                                   &memory_sh_ctx, &memory_sh_len) == HU_OK &&
                memory_sh_ctx && memory_sh_len > 0) {
                if (superhuman_ctx && superhuman_ctx_len > 0) {
                    size_t mem_sh_slen = strlen(memory_sh_ctx);
                    size_t content_len = superhuman_ctx_len + 2 + mem_sh_slen;
                    char *merged = (char *)agent->alloc->alloc(agent->alloc->ctx, content_len + 1);
                    if (merged) {
                        memcpy(merged, superhuman_ctx, superhuman_ctx_len);
                        merged[superhuman_ctx_len] = '\n';
                        merged[superhuman_ctx_len + 1] = '\n';
                        memcpy(merged + superhuman_ctx_len + 2, memory_sh_ctx, mem_sh_slen);
                        merged[content_len] = '\0';
                        agent->alloc->free(agent->alloc->ctx, superhuman_ctx,
                                           superhuman_ctx_len + 1);
                        agent->alloc->free(agent->alloc->ctx, memory_sh_ctx, memory_sh_len + 1);
                        superhuman_ctx = merged;
                        superhuman_ctx_len = content_len;
                    } else {
                        agent->alloc->free(agent->alloc->ctx, memory_sh_ctx, memory_sh_len + 1);
                    }
                } else {
                    superhuman_ctx = memory_sh_ctx;
                    superhuman_ctx_len = memory_sh_len;
                }
            } else if (memory_sh_ctx) {
                agent->alloc->free(agent->alloc->ctx, memory_sh_ctx, memory_sh_len + 1);
            }
        }
        /* Per-contact style evolution guidance */
        if (agent->memory && agent->memory_session_id && agent->memory_session_id_len > 0) {
            char *style_guidance = NULL;
            size_t style_guidance_len = 0;
            if (hu_superhuman_style_build_guidance(
                    agent->memory, agent->alloc, agent->memory_session_id,
                    agent->memory_session_id_len, &style_guidance, &style_guidance_len) == HU_OK &&
                style_guidance && style_guidance_len > 0) {
                if (superhuman_ctx && superhuman_ctx_len > 0) {
                    size_t content_len = superhuman_ctx_len + 1 + style_guidance_len;
                    char *merged = (char *)agent->alloc->alloc(agent->alloc->ctx, content_len + 1);
                    if (merged) {
                        memcpy(merged, superhuman_ctx, superhuman_ctx_len);
                        merged[superhuman_ctx_len] = '\n';
                        memcpy(merged + superhuman_ctx_len + 1, style_guidance, style_guidance_len);
                        merged[content_len] = '\0';
                        agent->alloc->free(agent->alloc->ctx, superhuman_ctx,
                                           superhuman_ctx_len + 1);
                        agent->alloc->free(agent->alloc->ctx, style_guidance,
                                           style_guidance_len + 1);
                        superhuman_ctx = merged;
                        superhuman_ctx_len = content_len;
                    } else {
                        agent->alloc->free(agent->alloc->ctx, style_guidance,
                                           style_guidance_len + 1);
                    }
                } else {
                    superhuman_ctx = style_guidance;
                    superhuman_ctx_len = style_guidance_len;
                }
            } else if (style_guidance) {
                agent->alloc->free(agent->alloc->ctx, style_guidance, style_guidance_len + 1);
            }
        }
        /* Cross-channel identity: merge canonical contact id from contact graph into superhuman ctx
         */
        if (agent->memory && agent->active_channel && agent->active_channel_len > 0 &&
            agent->memory_session_id && agent->memory_session_id_len > 0) {
            sqlite3 *cg_db = hu_sqlite_memory_get_db(agent->memory);
            if (cg_db) {
                char plat[64];
                char handle[256];
                size_t pl = agent->active_channel_len < sizeof(plat) - 1 ? agent->active_channel_len
                                                                         : sizeof(plat) - 1;
                memcpy(plat, agent->active_channel, pl);
                plat[pl] = '\0';
                size_t hl = agent->memory_session_id_len < sizeof(handle) - 1
                                ? agent->memory_session_id_len
                                : sizeof(handle) - 1;
                memcpy(handle, agent->memory_session_id, hl);
                handle[hl] = '\0';
                char canon[128];
                if (hu_contact_graph_resolve(cg_db, plat, handle, canon, sizeof(canon)) == HU_OK) {
                    char line[320];
                    int nw = snprintf(line, sizeof(line), "Cross-channel identity (canonical): %s",
                                      canon);
                    if (nw > 0 && (size_t)nw < sizeof(line)) {
                        size_t line_len = (size_t)nw;
                        if (superhuman_ctx && superhuman_ctx_len > 0) {
                            size_t content_len = superhuman_ctx_len + 1 + line_len;
                            char *merged =
                                (char *)agent->alloc->alloc(agent->alloc->ctx, content_len + 1);
                            if (merged) {
                                memcpy(merged, superhuman_ctx, superhuman_ctx_len);
                                merged[superhuman_ctx_len] = '\n';
                                memcpy(merged + superhuman_ctx_len + 1, line, line_len);
                                merged[content_len] = '\0';
                                agent->alloc->free(agent->alloc->ctx, superhuman_ctx,
                                                   superhuman_ctx_len + 1);
                                superhuman_ctx = merged;
                                superhuman_ctx_len = content_len;
                            }
                        } else {
                            char *dup =
                                (char *)agent->alloc->alloc(agent->alloc->ctx, line_len + 1);
                            if (dup) {
                                memcpy(dup, line, line_len);
                                dup[line_len] = '\0';
                                superhuman_ctx = dup;
                                superhuman_ctx_len = line_len;
                            }
                        }
                    }
                }
            }
        }
        /* Emotional moments due for check-in (contact-scoped) */
        if (agent->memory && agent->memory_session_id && agent->memory_session_id_len > 0) {
            hu_emotional_moment_t *due = NULL;
            size_t due_count = 0;
            int64_t now_ts = (int64_t)time(NULL);
            if (hu_emotional_moment_get_due(agent->alloc, agent->memory, now_ts, &due,
                                            &due_count) == HU_OK &&
                due && due_count > 0) {
                size_t contact_due = 0;
                for (size_t d = 0; d < due_count; d++) {
                    bool match = (strcmp(due[d].contact_id, agent->memory_session_id) == 0);
                    if (!match) {
                        const char *colon = strchr(agent->memory_session_id, ':');
                        if (colon && strcmp(due[d].contact_id, colon + 1) == 0)
                            match = true;
                    }
                    if (match)
                        contact_due++;
                }
                if (contact_due > 0) {
                    size_t em_len = 64 + contact_due * 128;
                    char *em_ctx = (char *)agent->alloc->alloc(agent->alloc->ctx, em_len);
                    if (em_ctx) {
                        size_t pos = 0;
                        pos = hu_buf_appendf(
                            em_ctx, em_len, pos,
                            "[Emotional check-in due] They shared something difficult "
                            "1–3 days ago. Consider a natural check-in:\n");
                        for (size_t d = 0; d < due_count && pos < em_len - 1; d++) {
                            bool match = (strcmp(due[d].contact_id, agent->memory_session_id) == 0);
                            if (!match) {
                                const char *colon = strchr(agent->memory_session_id, ':');
                                if (colon && strcmp(due[d].contact_id, colon + 1) == 0)
                                    match = true;
                            }
                            if (match) {
                                pos = hu_buf_appendf(em_ctx, em_len, pos,
                                                     "- Topic: %s, emotion: %s\n", due[d].topic,
                                                     due[d].emotion);
                            }
                        }
                        em_ctx[pos] = '\0';
                        if (pos > 0 && superhuman_ctx) {
                            size_t merged_len = superhuman_ctx_len + 2 + pos;
                            char *merged =
                                (char *)agent->alloc->alloc(agent->alloc->ctx, merged_len + 1);
                            if (merged) {
                                memcpy(merged, superhuman_ctx, superhuman_ctx_len);
                                merged[superhuman_ctx_len] = '\n';
                                merged[superhuman_ctx_len + 1] = '\n';
                                memcpy(merged + superhuman_ctx_len + 2, em_ctx, pos);
                                merged[merged_len] = '\0';
                                agent->alloc->free(agent->alloc->ctx, superhuman_ctx,
                                                   superhuman_ctx_len + 1);
                                agent->alloc->free(agent->alloc->ctx, em_ctx, em_len);
                                superhuman_ctx = merged;
                                superhuman_ctx_len = merged_len;
                            } else {
                                agent->alloc->free(agent->alloc->ctx, em_ctx, em_len);
                            }
                        } else if (pos > 0) {
                            superhuman_ctx = em_ctx;
                            superhuman_ctx_len = pos;
                        } else {
                            agent->alloc->free(agent->alloc->ctx, em_ctx, em_len);
                        }
                    }
                }
                agent->alloc->free(agent->alloc->ctx, due,
                                   due_count * sizeof(hu_emotional_moment_t));
            }
        }
        /* F26: If they're quiet and it's during their usual quiet hours, inject hint */
        if (agent->memory && agent->memory_session_id && agent->memory_session_id_len > 0 &&
            superhuman_ctx) {
            int qday = 0, qstart = 0, qend = 1;
            if (hu_superhuman_temporal_get_quiet_hours(
                    agent->memory, agent->alloc, agent->memory_session_id,
                    agent->memory_session_id_len, &qday, &qstart, &qend) == HU_OK) {
                time_t now_t = time(NULL);
                struct tm lt_buf;
                struct tm *lt = localtime_r(&now_t, &lt_buf);
                if (lt && lt->tm_wday == qday && lt->tm_hour >= qstart && lt->tm_hour < qend) {
                    static const char hint[] =
                        "\nThey're often quiet at this time. Don't worry if no reply.";
                    size_t hint_len = sizeof(hint) - 1;
                    size_t ctx_str_len = strlen(superhuman_ctx);
                    size_t new_len = ctx_str_len + hint_len;
                    char *merged = (char *)agent->alloc->alloc(agent->alloc->ctx, new_len + 1);
                    if (merged) {
                        memcpy(merged, superhuman_ctx, ctx_str_len);
                        memcpy(merged + ctx_str_len, hint, hint_len);
                        merged[new_len] = '\0';
                        agent->alloc->free(agent->alloc->ctx, superhuman_ctx,
                                           superhuman_ctx_len + 1);
                        superhuman_ctx = merged;
                        superhuman_ctx_len = new_len;
                    }
                }
            }
        }
#endif
    }

    /* Build adaptive persona context (circadian + relationship) */
    char *adaptive_ctx = NULL;
    size_t adaptive_ctx_len = 0;
    {
        uint8_t hour = 10;
#ifndef HU_IS_TEST
        {
            time_t now = time(NULL);
            struct tm lt_buf;
            struct tm *lt = localtime_r(&now, &lt_buf);
            if (lt)
                hour = (uint8_t)(lt->tm_hour & 0xFF);
        }
#endif
        char *circadian_str = NULL;
        size_t circadian_len = 0;
        char *rel_str = NULL;
        size_t rel_len = 0;
        if (hu_circadian_build_prompt(agent->alloc, hour, &circadian_str, &circadian_len) ==
                HU_OK &&
            circadian_str) {
            if (hu_relationship_build_prompt(agent->alloc, &agent->relationship, &rel_str,
                                             &rel_len) == HU_OK &&
                rel_str) {
                size_t total = circadian_len + rel_len + 1;
                adaptive_ctx = (char *)agent->alloc->alloc(agent->alloc->ctx, total);
                if (adaptive_ctx) {
                    memcpy(adaptive_ctx, circadian_str, circadian_len);
                    memcpy(adaptive_ctx + circadian_len, rel_str, rel_len);
                    adaptive_ctx[circadian_len + rel_len] = '\0';
                    adaptive_ctx_len = circadian_len + rel_len;
                }
                agent->alloc->free(agent->alloc->ctx, rel_str, rel_len + 1);
            }
            if (adaptive_ctx) {
                agent->alloc->free(agent->alloc->ctx, circadian_str, circadian_len + 1);
            } else {
                adaptive_ctx = circadian_str;
                adaptive_ctx_len = circadian_len;
            }
        }
    }

    /* Build situational awareness context */
    char *awareness_ctx = NULL;
    size_t awareness_ctx_len = 0;
    if (agent->awareness)
        awareness_ctx = hu_awareness_context(agent->awareness, agent->alloc, &awareness_ctx_len);

    /* Build cross-app PWA context */
#if HU_HAS_PWA
    {
        char *pwa_ctx = NULL;
        size_t pwa_ctx_len = 0;
        hu_error_t pwa_err = hu_pwa_context_build(agent->alloc, &pwa_ctx, &pwa_ctx_len);
        if (pwa_err == HU_OK && pwa_ctx && pwa_ctx_len > 0) {
            if (awareness_ctx) {
                /* Append PWA context to existing awareness */
                size_t total = awareness_ctx_len + 1 + pwa_ctx_len;
                char *merged = (char *)agent->alloc->alloc(agent->alloc->ctx, total + 1);
                if (merged) {
                    memcpy(merged, awareness_ctx, awareness_ctx_len);
                    merged[awareness_ctx_len] = '\n';
                    memcpy(merged + awareness_ctx_len + 1, pwa_ctx, pwa_ctx_len);
                    merged[total] = '\0';
                    agent->alloc->free(agent->alloc->ctx, awareness_ctx, awareness_ctx_len + 1);
                    awareness_ctx = merged;
                    awareness_ctx_len = total;
                }
            } else {
                /* Use PWA context as the awareness context */
                awareness_ctx = pwa_ctx;
                awareness_ctx_len = pwa_ctx_len;
                pwa_ctx = NULL; /* ownership transferred */
            }
        }
        if (pwa_ctx)
            agent->alloc->free(agent->alloc->ctx, pwa_ctx, pwa_ctx_len + 1);
    }
#endif

    /* Build outcome tracking summary */
    char *outcome_ctx = NULL;
    size_t outcome_ctx_len = 0;
    if (agent->outcomes)
        outcome_ctx = hu_outcome_build_summary(agent->outcomes, agent->alloc, &outcome_ctx_len);

    /* Build AGI frontier intelligence context */
    char *intelligence_ctx = NULL;
    size_t intelligence_ctx_len = 0;
#ifdef HU_ENABLE_SQLITE
    if (agent->memory) {
        sqlite3 *intel_db = hu_sqlite_memory_get_db(agent->memory);
        if (intel_db) {
            char parts[4096];
            size_t pos = 0;

#if defined(HU_ENABLE_ML)
            /* Start RL trajectory for this turn (ensure tables exist first) */
            {
                (void)hu_training_data_init_tables(intel_db);
                int64_t traj_id = 0;
                if (hu_training_data_start_trajectory(agent->alloc, intel_db, &traj_id) == HU_OK)
                    agent->sota.current_trajectory_id = traj_id;
            }
#endif

            /* Self-improvement: active prompt patches */
            {
                hu_self_improve_t si;
                if (hu_self_improve_create(agent->alloc, intel_db, &si) == HU_OK) {
                    char *patches = NULL;
                    size_t patches_len = 0;
                    if (hu_self_improve_get_prompt_patches(&si, &patches, &patches_len) == HU_OK &&
                        patches && patches_len > 0) {
                        int n =
                            snprintf(parts + pos, sizeof(parts) - pos,
                                     "### Learned Behaviors\n%.*s\n", (int)patches_len, patches);
                        if (n > 0 && pos + (size_t)n < sizeof(parts))
                            pos += (size_t)n;
                        agent->alloc->free(agent->alloc->ctx, patches, patches_len + 1);
                    }
                    char *tool_prefs = NULL;
                    size_t tool_prefs_len = 0;
                    if (hu_self_improve_get_tool_prefs_prompt(&si, &tool_prefs, &tool_prefs_len) ==
                            HU_OK &&
                        tool_prefs && tool_prefs_len > 0) {
                        int n = snprintf(parts + pos, sizeof(parts) - pos, "\n%s\n", tool_prefs);
                        if (n > 0 && pos + (size_t)n < sizeof(parts))
                            pos += (size_t)n;
                        agent->alloc->free(agent->alloc->ctx, tool_prefs, tool_prefs_len + 1);
                    }
                    hu_self_improve_deinit(&si);
                }
            }

            /* Goals: active goals context */
            {
                hu_goal_engine_t ge;
                if (hu_goal_engine_create(agent->alloc, intel_db, &ge) == HU_OK) {
                    char *gctx = NULL;
                    size_t gctx_len = 0;
                    if (hu_goal_build_context(&ge, &gctx, &gctx_len) == HU_OK && gctx &&
                        gctx_len > 0) {
                        int n = snprintf(parts + pos, sizeof(parts) - pos, "### %.*s\n",
                                         (int)gctx_len, gctx);
                        if (n > 0 && pos + (size_t)n < sizeof(parts))
                            pos += (size_t)n;
                        agent->alloc->free(agent->alloc->ctx, gctx, gctx_len + 1);
                    }
                    hu_goal_engine_deinit(&ge);
                }
            }

            /* Online learning: strategy preferences */
            {
                hu_online_learning_t ol;
                if (hu_online_learning_create(agent->alloc, intel_db, 0.1, &ol) == HU_OK) {
                    char *lctx = NULL;
                    size_t lctx_len = 0;
                    if (hu_online_learning_build_context(&ol, &lctx, &lctx_len) == HU_OK && lctx &&
                        lctx_len > 0) {
                        int n = snprintf(parts + pos, sizeof(parts) - pos, "### %.*s\n",
                                         (int)lctx_len, lctx);
                        if (n > 0 && pos + (size_t)n < sizeof(parts))
                            pos += (size_t)n;
                        agent->alloc->free(agent->alloc->ctx, lctx, lctx_len + 1);
                    }
                    hu_online_learning_deinit(&ol);
                }
            }

            /* Value learning: user values */
            {
                hu_value_engine_t ve;
                if (hu_value_engine_create(agent->alloc, intel_db, &ve) == HU_OK) {
                    char *vctx = NULL;
                    size_t vctx_len = 0;
                    if (hu_value_build_prompt(&ve, &vctx, &vctx_len) == HU_OK && vctx &&
                        vctx_len > 0) {
                        int n = snprintf(parts + pos, sizeof(parts) - pos, "### %.*s\n",
                                         (int)vctx_len, vctx);
                        if (n > 0 && pos + (size_t)n < sizeof(parts))
                            pos += (size_t)n;
                        agent->alloc->free(agent->alloc->ctx, vctx, vctx_len + 1);
                    }
                    hu_value_engine_deinit(&ve);
                }
            }

            /* World model: predict likely outcome of this request */
            {
                hu_causal_world_model_t wm;
                if (hu_causal_world_model_create(agent->alloc, intel_db, &wm) == HU_OK) {
                    hu_wm_prediction_t pred = {0};
                    double ctx_threshold = 0.3;
#ifdef HU_ENABLE_SQLITE
                    ctx_threshold = agent->meta_params.default_confidence_threshold * 0.6;
                    if (ctx_threshold < 0.1)
                        ctx_threshold = 0.1;
#endif
                    if (hu_world_simulate(&wm, msg, msg_len, NULL, 0, &pred) == HU_OK &&
                        pred.confidence > ctx_threshold) {
                        int n =
                            snprintf(parts + pos, sizeof(parts) - pos,
                                     "### Predicted Outcome\n"
                                     "Based on past patterns, this request likely leads to: %.*s "
                                     "(confidence: %.0f%%)\n",
                                     (int)(pred.outcome_len < 200 ? pred.outcome_len : 200),
                                     pred.outcome, pred.confidence * 100.0);
                        if (n > 0 && pos + (size_t)n < sizeof(parts))
                            pos += (size_t)n;
                    }
                    hu_causal_world_model_deinit(&wm);
                }
            }

            /* Experience: recall similar past experiences (semantic when available) */
            {
                hu_experience_store_t exp_store;
                if (hu_experience_store_init(agent->alloc, agent->memory, &exp_store) == HU_OK) {
#ifdef HU_ENABLE_SQLITE
                    sqlite3 *exp_db = hu_sqlite_memory_get_db(agent->memory);
                    if (exp_db)
                        exp_store.db = exp_db;
#endif
                    char *exp_prompt = NULL;
                    size_t exp_prompt_len = 0;
                    if (hu_experience_build_prompt(&exp_store, msg, msg_len, &exp_prompt,
                                                   &exp_prompt_len) == HU_OK &&
                        exp_prompt && exp_prompt_len > 0) {
                        int n = snprintf(parts + pos, sizeof(parts) - pos, "### %.*s\n",
                                         (int)exp_prompt_len, exp_prompt);
                        if (n > 0 && pos + (size_t)n < sizeof(parts))
                            pos += (size_t)n;
                        agent->alloc->free(agent->alloc->ctx, exp_prompt, exp_prompt_len + 1);
                    } else if (exp_prompt) {
                        agent->alloc->free(agent->alloc->ctx, exp_prompt, 1);
                    }
                    hu_experience_store_deinit(&exp_store);
                }
            }

            if (plan_ctx && plan_ctx_len > 0) {
                int n = snprintf(parts + pos, sizeof(parts) - pos, "### %.*s\n", (int)plan_ctx_len,
                                 plan_ctx);
                if (n > 0 && pos + (size_t)n < sizeof(parts))
                    pos += (size_t)n;
            }

            if (pos > 0) {
                intelligence_ctx = hu_strndup(agent->alloc, parts, pos);
                intelligence_ctx_len = pos;
            }
        }
    }
#endif

    /* Build persona prompt fresh each turn (channel-dependent; no caching) */
    char *persona_prompt = NULL;
    size_t persona_prompt_len = 0;
    if (agent->persona) {
        const char *ch = agent->active_channel;
        size_t ch_len = agent->active_channel_len;
        hu_error_t perr = hu_persona_build_prompt(agent->alloc, agent->persona, ch, ch_len, msg,
                                                  msg_len, &persona_prompt, &persona_prompt_len);
        if (perr != HU_OK) {
            if (pref_ctx)
                agent->alloc->free(agent->alloc->ctx, pref_ctx, pref_ctx_len + 1);
            if (memory_ctx)
                agent->alloc->free(agent->alloc->ctx, memory_ctx, memory_ctx_len + 1);
            if (stm_ctx)
                agent->alloc->free(agent->alloc->ctx, stm_ctx, stm_ctx_len + 1);
            if (commitment_ctx)
                agent->alloc->free(agent->alloc->ctx, commitment_ctx, commitment_ctx_len + 1);
            if (pattern_ctx)
                agent->alloc->free(agent->alloc->ctx, pattern_ctx, pattern_ctx_len + 1);
            if (proactive_ctx)
                agent->alloc->free(agent->alloc->ctx, proactive_ctx, proactive_ctx_len + 1);
            if (superhuman_ctx)
                agent->alloc->free(agent->alloc->ctx, superhuman_ctx, superhuman_ctx_len + 1);
            if (adaptive_ctx)
                agent->alloc->free(agent->alloc->ctx, adaptive_ctx, adaptive_ctx_len + 1);
            if (awareness_ctx)
                agent->alloc->free(agent->alloc->ctx, awareness_ctx, awareness_ctx_len + 1);
            if (outcome_ctx)
                agent->alloc->free(agent->alloc->ctx, outcome_ctx, outcome_ctx_len + 1);
            if (intelligence_ctx)
                agent->alloc->free(agent->alloc->ctx, intelligence_ctx, intelligence_ctx_len + 1);
            if (plan_ctx)
                agent->alloc->free(agent->alloc->ctx, plan_ctx, plan_ctx_len + 1);
            hu_agent_clear_current_for_tools();
            return perr;
        }
    }
    /* Sentiment-aware persona: detect user's emotional tone and adjust */
    if (persona_prompt && msg && msg_len > 0) {
        int neg_count = 0, pos_count = 0;
        for (int w = 0; s_sentiment_neg[w]; w++) {
            size_t wlen = strlen(s_sentiment_neg[w]);
            for (size_t p = 0; p + wlen <= msg_len; p++) {
                bool match = true;
                for (size_t j = 0; j < wlen && match; j++) {
                    char c = msg[p + j];
                    char n = s_sentiment_neg[w][j];
                    if (c >= 'A' && c <= 'Z')
                        c += 32;
                    if (c != n)
                        match = false;
                }
                if (match) {
                    neg_count++;
                    break;
                }
            }
        }
        for (int w = 0; s_sentiment_pos[w]; w++) {
            size_t wlen = strlen(s_sentiment_pos[w]);
            for (size_t p = 0; p + wlen <= msg_len; p++) {
                bool match = true;
                for (size_t j = 0; j < wlen && match; j++) {
                    char c = msg[p + j];
                    char n = s_sentiment_pos[w][j];
                    if (c >= 'A' && c <= 'Z')
                        c += 32;
                    if (c != n)
                        match = false;
                }
                if (match) {
                    pos_count++;
                    break;
                }
            }
        }
        const char *sentiment_note = NULL;
        if (neg_count >= 2)
            sentiment_note = "\n\n[Emotional context: User seems distressed. "
                             "Respond with extra warmth, empathy, and validation. "
                             "Prioritize emotional support before problem-solving.]";
        else if (neg_count == 1 && pos_count == 0)
            sentiment_note = "\n\n[Emotional context: User may be having a tough time. "
                             "Be warm and supportive in tone.]";
        else if (pos_count >= 2)
            sentiment_note = "\n\n[Emotional context: User is in a positive mood. "
                             "Match their energy — be enthusiastic and celebratory.]";
        if (sentiment_note) {
            size_t note_len = strlen(sentiment_note);
            size_t new_len = persona_prompt_len + note_len;
            char *augmented = (char *)agent->alloc->alloc(agent->alloc->ctx, new_len + 1);
            if (augmented) {
                memcpy(augmented, persona_prompt, persona_prompt_len);
                memcpy(augmented + persona_prompt_len, sentiment_note, note_len);
                augmented[new_len] = '\0';
                agent->alloc->free(agent->alloc->ctx, persona_prompt, persona_prompt_len + 1);
                persona_prompt = augmented;
                persona_prompt_len = new_len;
            }
        }
    }
    /* Relationship-based tone: inject warmth/vulnerability guidance from contact profile */
    if (persona_prompt && agent->persona && agent->memory_session_id) {
        const char *tone_note = NULL;
        const hu_contact_profile_t *cp = hu_persona_find_contact(
            agent->persona, agent->memory_session_id, agent->memory_session_id_len);
        if (cp) {
            if (cp->relationship_stage && strstr(cp->relationship_stage, "deep"))
                tone_note = "\n\n[Relationship: deep — be genuinely present, "
                            "anticipate needs, use shared references freely.]";
            else if (cp->relationship_stage && strstr(cp->relationship_stage, "trusted"))
                tone_note = "\n\n[Relationship: trusted — be candid and proactive. "
                            "Share insights freely and be direct.]";
            else if (cp->relationship_stage && strstr(cp->relationship_stage, "familiar"))
                tone_note = "\n\n[Relationship: familiar — reference past conversations "
                            "when relevant. Be warmer than default.]";
            else if (cp->warmth_level && strstr(cp->warmth_level, "intimate"))
                tone_note = "\n\n[Relationship: intimate — respond with genuine warmth "
                            "and personal connection. Use inside references.]";
        }
        if (tone_note) {
            size_t tn_len = strlen(tone_note);
            size_t new_len = persona_prompt_len + tn_len;
            char *augmented = (char *)agent->alloc->alloc(agent->alloc->ctx, new_len + 1);
            if (augmented) {
                memcpy(augmented, persona_prompt, persona_prompt_len);
                memcpy(augmented + persona_prompt_len, tone_note, tn_len);
                augmented[new_len] = '\0';
                agent->alloc->free(agent->alloc->ctx, persona_prompt, persona_prompt_len + 1);
                persona_prompt = augmented;
                persona_prompt_len = new_len;
            }
        }
    }

    /* Build skills context from skillforge if available */
    char *skills_ctx = NULL;
    size_t skills_ctx_len = 0;
#ifdef HU_HAS_SKILLS
    if (agent->skillforge) {
        hu_skillforge_t *sf = (hu_skillforge_t *)agent->skillforge;
        hu_error_t sc_err = hu_skillforge_build_prompt_catalog(agent->alloc, sf, msg, msg_len,
                                                               &skills_ctx, &skills_ctx_len);
        if (sc_err != HU_OK) {
            if (skills_ctx)
                agent->alloc->free(agent->alloc->ctx, skills_ctx, skills_ctx_len + 1);
            skills_ctx = NULL;
            skills_ctx_len = 0;
        } else if (skills_ctx && skills_ctx_len > 0 && msg && msg_len > 0) {
            /* Dynamic skill routing: replace flat catalog with relevance-ranked lines. */
            size_t ena[256];
            size_t en = 0;
            for (size_t si = 0; si < sf->skills_len && en < 256; si++) {
                if (!sf->skills[si].enabled)
                    continue;
                ena[en++] = si;
            }
            if (en > 0) {
                hu_skill_t *rskills =
                    (hu_skill_t *)agent->alloc->alloc(agent->alloc->ctx, en * sizeof(hu_skill_t));
                float *kws = (float *)agent->alloc->alloc(agent->alloc->ctx, en * sizeof(float));
                if (rskills && kws) {
                    int max_hits = 0;
                    for (size_t e = 0; e < en; e++) {
                        rskills[e] = sf->skills[ena[e]];
                        int h = hu_skillforge_skill_keyword_hits(&rskills[e], msg, msg_len);
                        kws[e] = (float)h;
                        if (h > max_hits)
                            max_hits = h;
                    }
                    float norm = max_hits > 0 ? (float)max_hits : 1.0f;
                    for (size_t e = 0; e < en; e++)
                        kws[e] /= norm;

                    hu_skill_routing_ctx_t sem_ctx;
                    hu_skill_routing_init(&sem_ctx);
                    bool used_skill_embeddings = false;
                    if (agent->skill_route_embedder && agent->skill_route_embedder->vtable &&
                        agent->skill_route_embedder->vtable->embed) {
                        if (hu_skill_routing_embed_catalog(&sem_ctx, agent->alloc, rskills, en,
                                                           agent_skill_route_embed_fn,
                                                           agent->skill_route_embedder) == HU_OK &&
                            sem_ctx.initialized)
                            used_skill_embeddings = true;
                    }

                    hu_skill_blend_t blend;
                    memset(&blend, 0, sizeof(blend));
                    float emo = agent->infra.emotional_cognition.state.intensity;
                    const hu_skill_routing_ctx_t *route_ctx = sem_ctx.initialized ? &sem_ctx : NULL;
                    hu_embed_fn msg_embed_fn =
                        sem_ctx.initialized ? agent_skill_route_embed_fn : NULL;
                    void *msg_embed_ctx =
                        sem_ctx.initialized ? (void *)agent->skill_route_embedder : NULL;

                    if (hu_skill_routing_route(route_ctx, agent->alloc, msg, msg_len, msg_embed_fn,
                                               msg_embed_ctx, rskills, en, kws, NULL, emo,
                                               &blend) == HU_OK) {
                        char *routed = NULL;
                        size_t routed_len = 0;
                        if (hu_skill_routing_build_catalog(agent->alloc, &blend, rskills, en, en,
                                                           &routed, &routed_len) == HU_OK &&
                            routed && routed_len > 0) {
                            agent->alloc->free(agent->alloc->ctx, skills_ctx, skills_ctx_len + 1);
                            skills_ctx = routed;
                            skills_ctx_len = routed_len;
                            if (agent->bth_metrics) {
                                agent->bth_metrics->skill_routes_semantic++;
                                if (used_skill_embeddings)
                                    agent->bth_metrics->skill_routes_embedded++;
                                if (blend.count > 1u)
                                    agent->bth_metrics->skill_routes_blended++;
                            }
                        } else if (routed) {
                            agent->alloc->free(agent->alloc->ctx, routed, routed_len + 1);
                        }
                    }
                    hu_skill_routing_deinit(&sem_ctx, agent->alloc);
                    agent->alloc->free(agent->alloc->ctx, kws, en * sizeof(float));
                    agent->alloc->free(agent->alloc->ctx, rskills, en * sizeof(hu_skill_t));
                } else {
                    if (rskills)
                        agent->alloc->free(agent->alloc->ctx, rskills, en * sizeof(hu_skill_t));
                    if (kws)
                        agent->alloc->free(agent->alloc->ctx, kws, en * sizeof(float));
                }
            }
        }
    }
#if defined(HU_ENABLE_SQLITE) && defined(HU_HAS_SKILLS)
    /* Per-contact learned skills (DB-backed) */
    if (agent->memory && agent->memory_session_id && agent->memory_session_id_len > 0) {
        sqlite3 *skill_db = hu_sqlite_memory_get_db(agent->memory);
        if (skill_db) {
            char *contact_skills = NULL;
            size_t contact_skills_len = 0;
            if (hu_skill_build_contact_context(agent->alloc, skill_db, agent->memory_session_id,
                                               agent->memory_session_id_len, &contact_skills,
                                               &contact_skills_len) == HU_OK &&
                contact_skills && contact_skills_len > 0) {
                if (skills_ctx && skills_ctx_len > 0) {
                    size_t merged_len = skills_ctx_len + 2 + contact_skills_len + 1;
                    char *merged = (char *)agent->alloc->alloc(agent->alloc->ctx, merged_len);
                    if (merged) {
                        memcpy(merged, skills_ctx, skills_ctx_len);
                        merged[skills_ctx_len] = '\n';
                        merged[skills_ctx_len + 1] = '\n';
                        memcpy(merged + skills_ctx_len + 2, contact_skills, contact_skills_len + 1);
                        agent->alloc->free(agent->alloc->ctx, skills_ctx, skills_ctx_len + 1);
                        agent->alloc->free(agent->alloc->ctx, contact_skills,
                                           contact_skills_len + 1);
                        skills_ctx = merged;
                        skills_ctx_len = merged_len;
                    } else {
                        agent->alloc->free(agent->alloc->ctx, contact_skills,
                                           contact_skills_len + 1);
                    }
                } else {
                    skills_ctx = contact_skills;
                    skills_ctx_len = contact_skills_len;
                }
            } else if (contact_skills) {
                agent->alloc->free(agent->alloc->ctx, contact_skills, contact_skills_len + 1);
            }
        }
    }
#endif
#endif /* HU_HAS_SKILLS */

    /* Memory Tiers: inject core memory into prompt context */
    if (agent->sota.sota_initialized) {
        char core_buf[2048];
        size_t core_len = 0;
        if (hu_tier_manager_build_core_prompt(&agent->sota.tier_manager, core_buf, sizeof(core_buf),
                                              &core_len) == HU_OK &&
            core_len > 0) {
            if (memory_ctx && memory_ctx_len > 0) {
                size_t merged_len = core_len + 1 + memory_ctx_len;
                char *merged = (char *)agent->alloc->alloc(agent->alloc->ctx, merged_len + 1);
                if (merged) {
                    memcpy(merged, core_buf, core_len);
                    merged[core_len] = '\n';
                    memcpy(merged + core_len + 1, memory_ctx, memory_ctx_len);
                    merged[merged_len] = '\0';
                    agent->alloc->free(agent->alloc->ctx, memory_ctx, memory_ctx_len + 1);
                    memory_ctx = merged;
                    memory_ctx_len = merged_len;
                }
            } else if (!memory_ctx) {
                memory_ctx = hu_strndup(agent->alloc, core_buf, core_len);
                memory_ctx_len = memory_ctx ? core_len : 0;
            }
        }
    }

    /* Humanness layer: shared references, curiosity, absence, imperfect delivery,
     * evolved opinions, and emotional residue carryover directives */
    char *humanness_ctx = NULL;
    size_t humanness_ctx_len = 0;
    char *imperfect_dir = NULL;
    size_t imperfect_dir_len = 0;
    char *residue_dir = NULL;
    size_t residue_dir_len = 0;
    {
        char hum_buf[4096];
        size_t hum_pos = 0;

        /* Shared references — callbacks to past moments */
        if (memory_ctx && memory_ctx_len > 0) {
            hu_shared_reference_t *refs = NULL;
            size_t ref_count = 0;
            if (hu_shared_references_find(agent->alloc,
                                          agent->memory_session_id ? agent->memory_session_id : "",
                                          agent->memory_session_id_len, msg, msg_len, memory_ctx,
                                          memory_ctx_len, &refs, &ref_count, 3) == HU_OK &&
                ref_count > 0) {
                size_t dir_len = 0;
                char *dir =
                    hu_shared_references_build_directive(agent->alloc, refs, ref_count, &dir_len);
                if (dir && dir_len > 0 && hum_pos + dir_len + 2 < sizeof(hum_buf)) {
                    memcpy(hum_buf + hum_pos, dir, dir_len);
                    hum_pos += dir_len;
                    hum_buf[hum_pos++] = '\n';
                    hum_buf[hum_pos++] = '\n';
                }
                if (dir)
                    agent->alloc->free(agent->alloc->ctx, dir, dir_len + 1);
                hu_shared_references_free(agent->alloc, refs, ref_count);
            }
        }

        /* Curiosity engine — spontaneous follow-ups from memory */
        if (memory_ctx && memory_ctx_len > 0) {
            hu_curiosity_prompt_t *prompts = NULL;
            size_t cur_count = 0;
            if (hu_curiosity_generate(agent->alloc,
                                      agent->memory_session_id ? agent->memory_session_id : "",
                                      agent->memory_session_id_len, memory_ctx, memory_ctx_len, msg,
                                      msg_len, &prompts, &cur_count, 2) == HU_OK &&
                cur_count > 0) {
                size_t dir_len = 0;
                char *dir =
                    hu_curiosity_build_directive(agent->alloc, prompts, cur_count, &dir_len);
                if (dir && dir_len > 0 && hum_pos + dir_len + 2 < sizeof(hum_buf)) {
                    memcpy(hum_buf + hum_pos, dir, dir_len);
                    hum_pos += dir_len;
                    hum_buf[hum_pos++] = '\n';
                    hum_buf[hum_pos++] = '\n';
                }
                if (dir)
                    agent->alloc->free(agent->alloc->ctx, dir, dir_len + 1);
                hu_curiosity_prompts_free(agent->alloc, prompts, cur_count);
            }
        }

        /* Absence detection — notice what they didn't say */
        if (msg_len >= 15) {
            hu_absence_signal_t *abs_signals = NULL;
            size_t abs_count = 0;
            if (hu_absence_detect(agent->alloc, msg, msg_len, &abs_signals, &abs_count) == HU_OK &&
                abs_count > 0) {
                size_t dir_len = 0;
                char *dir =
                    hu_absence_build_directive(agent->alloc, abs_signals, abs_count, &dir_len);
                if (dir && dir_len > 0 && hum_pos + dir_len + 2 < sizeof(hum_buf)) {
                    memcpy(hum_buf + hum_pos, dir, dir_len);
                    hum_pos += dir_len;
                    hum_buf[hum_pos++] = '\n';
                    hum_buf[hum_pos++] = '\n';
                }
                if (dir)
                    agent->alloc->free(agent->alloc->ctx, dir, dir_len + 1);
                hu_absence_signals_free(agent->alloc, abs_signals, abs_count);
            }
        }

#ifdef HU_ENABLE_SQLITE
        /* Evolved opinions — perspectives developed over time */
        if (agent->memory) {
            sqlite3 *eo_db = hu_sqlite_memory_get_db(agent->memory);
            if (eo_db) {
                hu_evolved_opinions_ensure_table(eo_db);
                hu_evolved_opinion_t *opinions = NULL;
                size_t op_count = 0;
                if (hu_evolved_opinions_get(agent->alloc, eo_db, 0.4, 5, &opinions, &op_count) ==
                        HU_OK &&
                    opinions && op_count > 0) {
                    size_t dir_len = 0;
                    char *dir = hu_evolved_opinion_build_directive(agent->alloc, opinions, op_count,
                                                                   0.4, &dir_len);
                    if (dir && dir_len > 0 && hum_pos + dir_len + 2 < sizeof(hum_buf)) {
                        memcpy(hum_buf + hum_pos, dir, dir_len);
                        hum_pos += dir_len;
                        hum_buf[hum_pos++] = '\n';
                        hum_buf[hum_pos++] = '\n';
                    }
                    if (dir)
                        agent->alloc->free(agent->alloc->ctx, dir, dir_len + 1);
                    hu_evolved_opinions_free(agent->alloc, opinions, op_count);
                }
            }
        }

        /* Emotional residue carryover from prior conversations */
        if (agent->memory && agent->memory_session_id) {
            sqlite3 *rc_db = hu_sqlite_memory_get_db(agent->memory);
            if (rc_db) {
                sqlite3_stmt *rc_stmt = NULL;
                const char *rc_sql = "SELECT valence, intensity, created_at FROM emotional_residues"
                                     " WHERE contact_id = ?1 ORDER BY created_at DESC LIMIT 10";
                if (sqlite3_prepare_v2(rc_db, rc_sql, -1, &rc_stmt, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(rc_stmt, 1, agent->memory_session_id,
                                      (int)agent->memory_session_id_len, SQLITE_STATIC);
                    double valences[10];
                    double intensities[10];
                    int64_t timestamps[10];
                    size_t rc_count = 0;
                    while (sqlite3_step(rc_stmt) == SQLITE_ROW && rc_count < 10) {
                        valences[rc_count] = sqlite3_column_double(rc_stmt, 0);
                        intensities[rc_count] = sqlite3_column_double(rc_stmt, 1);
                        timestamps[rc_count] = sqlite3_column_int64(rc_stmt, 2);
                        rc_count++;
                    }
                    sqlite3_finalize(rc_stmt);
                    if (rc_count > 0) {
                        hu_residue_carryover_t carryover = {0};
                        if (hu_residue_carryover_compute(valences, intensities, timestamps,
                                                         rc_count, (int64_t)time(NULL),
                                                         &carryover) == HU_OK) {
                            residue_dir = hu_residue_carryover_build_directive(
                                agent->alloc, &carryover, &residue_dir_len);
                        }
                    }
                } else if (rc_stmt) {
                    sqlite3_finalize(rc_stmt);
                }
            }
        }
#endif

        /* Imperfect delivery — express genuine uncertainty */
        {
            uint32_t tool_count = agent->tools_count > 0 ? (uint32_t)agent->tools_count : 0;
            hu_certainty_level_t cert = hu_certainty_classify(
                msg, msg_len, (memory_ctx != NULL && memory_ctx_len > 0), tool_count);
            imperfect_dir = hu_imperfect_delivery_directive(agent->alloc, cert, &imperfect_dir_len);
        }

        if (hum_pos > 0) {
            hum_buf[hum_pos] = '\0';
            humanness_ctx = hu_strndup(agent->alloc, hum_buf, hum_pos);
            if (humanness_ctx)
                humanness_ctx_len = hum_pos;
        }
    }

    /* ── 12 Frontiers of Humanness ── */
    {
        if (!agent->frontiers.initialized) {
            hu_somatic_init(&agent->frontiers.somatic);
            hu_novelty_tracker_init(&agent->frontiers.novelty);
            hu_attachment_init(&agent->frontiers.attachment);
            hu_rupture_init(&agent->frontiers.rupture);
            hu_narrative_self_init(&agent->frontiers.narrative);
            hu_creative_voice_init(&agent->frontiers.creative_voice);
            hu_genuine_boundary_set_init(&agent->frontiers.boundaries);
            hu_growth_narrative_init(&agent->frontiers.growth);

            /* Warm-start attachment from contact profile if available */
            if (agent->persona && agent->memory_session_id) {
                const hu_contact_profile_t *acp = hu_persona_find_contact(
                    agent->persona, agent->memory_session_id, agent->memory_session_id_len);
                if (acp && acp->attachment_style) {
                    if (strstr(acp->attachment_style, "secure")) {
                        agent->frontiers.attachment.user_style = HU_ATTACH_SECURE;
                        agent->frontiers.attachment.user_confidence = 0.4f;
                        agent->frontiers.attachment.proximity_seeking = 0.5f;
                        agent->frontiers.attachment.safe_haven_usage = 0.5f;
                        agent->frontiers.attachment.secure_base_behavior = 0.6f;
                        agent->frontiers.attachment.separation_distress = 0.3f;
                    } else if (strstr(acp->attachment_style, "anxious")) {
                        agent->frontiers.attachment.user_style = HU_ATTACH_ANXIOUS;
                        agent->frontiers.attachment.user_confidence = 0.3f;
                        agent->frontiers.attachment.proximity_seeking = 0.8f;
                        agent->frontiers.attachment.separation_distress = 0.7f;
                    } else if (strstr(acp->attachment_style, "avoidant")) {
                        agent->frontiers.attachment.user_style = HU_ATTACH_AVOIDANT;
                        agent->frontiers.attachment.user_confidence = 0.3f;
                        agent->frontiers.attachment.proximity_seeking = 0.2f;
                        agent->frontiers.attachment.safe_haven_usage = 0.2f;
                    }
                }
            }

            if (agent->persona) {
                /* F3: Seed narrative self from persona identity + biography */
                if (agent->persona->identity)
                    hu_narrative_self_set_identity(agent->alloc, &agent->frontiers.narrative,
                                                   agent->persona->identity,
                                                   strlen(agent->persona->identity));
                if (agent->persona->biography)
                    hu_narrative_self_add_theme(agent->alloc, &agent->frontiers.narrative,
                                                agent->persona->biography,
                                                strlen(agent->persona->biography));
                for (size_t vi = 0; vi < agent->persona->values_count && vi < 4; vi++)
                    hu_narrative_self_add_theme(agent->alloc, &agent->frontiers.narrative,
                                                agent->persona->values[vi],
                                                strlen(agent->persona->values[vi]));

                /* F10: Seed creative voice from persona traits as metaphor domains */
                for (size_t ti = 0; ti < agent->persona->traits_count && ti < 4; ti++)
                    hu_creative_voice_add_domain(agent->alloc, &agent->frontiers.creative_voice,
                                                 agent->persona->traits[ti],
                                                 strlen(agent->persona->traits[ti]));
                if (agent->persona->decision_style)
                    hu_creative_voice_add_anchor(agent->alloc, &agent->frontiers.creative_voice,
                                                 agent->persona->decision_style,
                                                 strlen(agent->persona->decision_style));

                /* F12: Seed genuine boundaries from persona principles + values */
                for (size_t pi = 0; pi < agent->persona->principles_count && pi < 4; pi++)
                    hu_genuine_boundary_add(agent->alloc, &agent->frontiers.boundaries, "principle",
                                            agent->persona->principles[pi], NULL, 0.8f, 0);
                for (size_t vi = 0; vi < agent->persona->values_count && vi < 2; vi++)
                    hu_genuine_boundary_add(agent->alloc, &agent->frontiers.boundaries, "value",
                                            agent->persona->values[vi], NULL, 0.7f, 0);
            }

            hu_tcal_init(&agent->frontiers.trust);

            agent->frontiers.initialized = true;

#ifdef HU_ENABLE_SQLITE
            if (agent->memory && agent->memory_session_id) {
                sqlite3 *fp_db = hu_sqlite_memory_get_db(agent->memory);
                if (fp_db) {
                    hu_frontier_persist_ensure_table(fp_db);
                    hu_error_t fp_err =
                        hu_frontier_persist_load(agent->alloc, fp_db, agent->memory_session_id,
                                                 agent->memory_session_id_len, &agent->frontiers);
                    if (fp_err == HU_OK) {
                        hu_frontier_persist_load_growth(
                            agent->alloc, fp_db, agent->memory_session_id,
                            agent->memory_session_id_len, &agent->frontiers);
                        {
                            int rs = 0, rsc = 0, rt = 0;
                            if (hu_frontier_persist_load_relationship(
                                    fp_db, agent->memory_session_id, agent->memory_session_id_len,
                                    &rs, &rsc, &rt) == HU_OK &&
                                (rs || rsc || rt)) {
                                if (rs >= HU_REL_NEW && rs <= HU_REL_DEEP)
                                    agent->relationship.stage = (hu_relationship_stage_t)rs;
                                if (rsc >= 0)
                                    agent->relationship.session_count = (uint32_t)rsc;
                                if (rt >= 0)
                                    agent->relationship.total_turns = (uint32_t)rt;
                            }
                        }
                        hu_log_info("agent", agent->observer, "frontier state restored for %.*s",
                                    (int)(agent->memory_session_id_len > 20
                                              ? 20
                                              : agent->memory_session_id_len),
                                    agent->memory_session_id);
                    }
                }
            }
#endif
        }

        /* F1: Somatic State Engine — derive topic_switches from STM,
         * physical state from circadian phase */
        uint32_t topic_switches = 0;
        {
            size_t stm_n = hu_stm_count(&agent->stm);
            if (stm_n >= 2) {
                const hu_stm_turn_t *prev = hu_stm_get(&agent->stm, stm_n - 2);
                const hu_stm_turn_t *cur = hu_stm_get(&agent->stm, stm_n - 1);
                if (prev && cur && prev->primary_topic && cur->primary_topic &&
                    strcmp(prev->primary_topic, cur->primary_topic) != 0)
                    topic_switches = 1;
            }
        }
        hu_physical_state_t f1_physical = HU_PHYSICAL_NORMAL;
#if !defined(HU_IS_TEST)
        {
            time_t f1_now = time(NULL);
            struct tm f1_lt;
            struct tm *f1_lp = localtime_r(&f1_now, &f1_lt);
            if (f1_lp) {
                hu_time_phase_t phase = hu_circadian_phase((uint8_t)(f1_lp->tm_hour & 0xFF));
                if (phase == HU_PHASE_LATE_NIGHT)
                    f1_physical = HU_PHYSICAL_TIRED;
                else if (phase == HU_PHASE_EARLY_MORNING)
                    f1_physical = HU_PHYSICAL_ENERGIZED;
                else if (phase == HU_PHASE_NIGHT)
                    f1_physical = HU_PHYSICAL_TIRED;
            }
        }
#endif
        hu_somatic_update(&agent->frontiers.somatic, (uint64_t)time(NULL),
                          agent->infra.emotional_cognition.state.intensity, topic_switches,
                          f1_physical);
        hu_somatic_build_context(agent->alloc, &agent->frontiers.somatic, &somatic_ctx,
                                 &somatic_ctx_len);

        /* F8: Presence Gradient — derive from real relationship + vulnerability */
        float f8_vulnerability = 0.0f;
        uint32_t f8_rel_depth = 0;
        if (agent->persona && agent->memory_session_id) {
            const hu_contact_profile_t *f8_cp = hu_persona_find_contact(
                agent->persona, agent->memory_session_id, agent->memory_session_id_len);
            if (f8_cp) {
                if (f8_cp->vulnerability_level) {
                    if (strstr(f8_cp->vulnerability_level, "high"))
                        f8_vulnerability = 0.8f;
                    else if (strstr(f8_cp->vulnerability_level, "medium"))
                        f8_vulnerability = 0.5f;
                    else
                        f8_vulnerability = 0.2f;
                }
                if (f8_cp->relationship_stage) {
                    if (strstr(f8_cp->relationship_stage, "deep"))
                        f8_rel_depth = 8;
                    else if (strstr(f8_cp->relationship_stage, "trusted"))
                        f8_rel_depth = 6;
                    else if (strstr(f8_cp->relationship_stage, "familiar"))
                        f8_rel_depth = 4;
                    else
                        f8_rel_depth = 1;
                }
            }
        }
        hu_presence_state_t pres;
        hu_presence_init(&pres);
        hu_presence_compute(&pres, agent->infra.emotional_cognition.state.intensity,
                            f8_vulnerability, 0.0f,
                            agent->infra.emotional_cognition.state.intensity, f8_rel_depth);
        hu_presence_build_context(agent->alloc, &pres, &presence_ctx, &presence_ctx_len);
        if (agent->observer && pres.level >= HU_PRESENCE_ATTENTIVE) {
            hu_observer_event_t fev = {.tag = HU_OBSERVER_EVENT_FRONTIER};
            fev.data.frontier.frontier = "presence";
            fev.data.frontier.transition = hu_presence_level_name(pres.level);
            fev.data.frontier.value = pres.attention_score;
            hu_observer_record_event(*agent->observer, &fev);
        }
        hu_presence_deinit(agent->alloc, &pres);

        /* F9: Micro-Expression — use relationship closeness from presence */
        {
            float f9_closeness = (float)f8_rel_depth / 10.0f;
            if (f9_closeness > 1.0f)
                f9_closeness = 1.0f;
            hu_micro_expression_t mexp;
            hu_micro_expression_init(&mexp);
            hu_micro_expression_compute(
                &mexp, agent->frontiers.somatic.energy, agent->frontiers.somatic.social_battery,
                agent->infra.emotional_cognition.state.valence,
                agent->infra.emotional_cognition.state.intensity, f9_closeness);
            hu_micro_expression_build_context(agent->alloc, &mexp, &micro_expr_ctx,
                                              &micro_expr_ctx_len);
            hu_micro_expression_deinit(agent->alloc, &mexp);
        }

        /* F4: Novelty Detection — feed STM topics as known context */
        {
            const char *stm_topic_ptrs[HU_STM_MAX_TOPICS];
            size_t stm_topic_n = 0;
            for (size_t ti = 0; ti < agent->stm.topic_count && ti < HU_STM_MAX_TOPICS; ti++) {
                if (agent->stm.topics[ti]) {
                    stm_topic_ptrs[stm_topic_n++] = agent->stm.topics[ti];
                }
            }
            hu_novelty_signal_t nsig = {0};
            hu_novelty_evaluate(agent->alloc, &agent->frontiers.novelty, msg, msg_len,
                                stm_topic_ptrs, stm_topic_n, stm_topic_ptrs, stm_topic_n, &nsig);
            if (nsig.surprise_prompt) {
                novelty_ctx =
                    hu_strndup(agent->alloc, nsig.surprise_prompt, strlen(nsig.surprise_prompt));
                if (novelty_ctx)
                    novelty_ctx_len = strlen(novelty_ctx);
            }
            hu_novelty_signal_free(agent->alloc, &nsig);
        }

        /* F5: Attachment Dynamics — derive from emotional state + message patterns + timestamp gaps
         */
        {
            float msg_freq = 0.5f;
            float emo_share = agent->infra.emotional_cognition.state.intensity;
            float gap_distress = 0.2f;
            float independence = 0.5f;
            float growth_signal = 0.4f;
            size_t stm_n = hu_stm_count(&agent->stm);
            if (stm_n >= 2) {
                size_t user_turns = 0;
                uint64_t max_gap_ms = 0;
                uint64_t total_gap_ms = 0;
                size_t gap_count = 0;
                for (size_t si = 0; si < stm_n; si++) {
                    const hu_stm_turn_t *t = hu_stm_get(&agent->stm, si);
                    if (!t)
                        continue;
                    if (t->role && strcmp(t->role, "user") == 0)
                        user_turns++;
                    if (si > 0) {
                        const hu_stm_turn_t *prev_t = hu_stm_get(&agent->stm, si - 1);
                        if (prev_t && t->timestamp_ms > prev_t->timestamp_ms) {
                            uint64_t gap = t->timestamp_ms - prev_t->timestamp_ms;
                            total_gap_ms += gap;
                            gap_count++;
                            if (gap > max_gap_ms)
                                max_gap_ms = gap;
                        }
                    }
                }
                msg_freq = (float)user_turns / (float)stm_n;
                if (max_gap_ms > 3600000) /* >1 hour gap */
                    gap_distress = 0.7f + 0.3f * emo_share;
                else if (max_gap_ms > 1800000) /* >30 min */
                    gap_distress = 0.4f + 0.2f * emo_share;
                else if (gap_count > 0) {
                    float avg_gap = (float)(total_gap_ms / gap_count);
                    gap_distress = avg_gap > 600000.0f ? 0.3f : 0.15f;
                }
                if (msg_freq > 0.7f)
                    independence = 0.3f;
                else if (msg_freq < 0.3f)
                    independence = 0.7f;
            }
            hu_attachment_style_t prev_attach = agent->frontiers.attachment.user_style;
            hu_attachment_update(&agent->frontiers.attachment, msg_freq, emo_share, gap_distress,
                                 independence, growth_signal);
            if (agent->observer && agent->frontiers.attachment.user_style != prev_attach) {
                hu_observer_event_t fev = {.tag = HU_OBSERVER_EVENT_FRONTIER};
                fev.data.frontier.frontier = "attachment";
                fev.data.frontier.transition =
                    hu_attachment_style_name(agent->frontiers.attachment.user_style);
                fev.data.frontier.value = agent->frontiers.attachment.user_confidence;
                hu_observer_record_event(*agent->observer, &fev);
            }
        }
        hu_attachment_build_context(agent->alloc, &agent->frontiers.attachment, &attachment_ctx,
                                    &attachment_ctx_len);

        /* F6: Rupture-Repair — detect from tone shift + message length delta + phrase detection */
        {
            hu_rupture_signals_t rsig = {0};
            size_t stm_n = hu_stm_count(&agent->stm);
            if (stm_n >= 2) {
                const hu_stm_turn_t *prev_t = hu_stm_get(&agent->stm, stm_n - 2);
                const hu_stm_turn_t *cur_t = hu_stm_get(&agent->stm, stm_n - 1);
                if (prev_t && cur_t && prev_t->content_len > 0 && cur_t->content_len > 0) {
                    float prev_len = (float)prev_t->content_len;
                    float cur_len = (float)cur_t->content_len;
                    rsig.length_delta = (cur_len - prev_len) / (prev_len > 0 ? prev_len : 1.0f);
                    if (rsig.length_delta < -0.5f)
                        rsig.energy_drop = -rsig.length_delta;

                    /* Detect tone shift between consecutive user messages */
                    if (prev_t->role && strcmp(prev_t->role, "user") == 0 && cur_t->role &&
                        strcmp(cur_t->role, "user") == 0) {
                        const char *tone_msgs[1];
                        size_t tone_lens[1];
                        tone_msgs[0] = prev_t->content;
                        tone_lens[0] = prev_t->content_len;
                        hu_tone_t prev_tone = hu_detect_tone(tone_msgs, tone_lens, 1);
                        tone_msgs[0] = cur_t->content;
                        tone_lens[0] = cur_t->content_len;
                        hu_tone_t cur_tone = hu_detect_tone(tone_msgs, tone_lens, 1);
                        if (prev_tone == HU_TONE_CASUAL && cur_tone == HU_TONE_FORMAL)
                            rsig.tone_delta = 0.7f;
                        else if (prev_tone != cur_tone)
                            rsig.tone_delta = 0.3f;
                    }
                }
                if (cur_t && cur_t->content && cur_t->content_len > 0) {
                    if (hu_strcasestr(cur_t->content, "that's not what I meant") ||
                        hu_strcasestr(cur_t->content, "no, I meant") ||
                        hu_strcasestr(cur_t->content, "you misunderstood") ||
                        hu_strcasestr(cur_t->content, "that's not what I said") ||
                        hu_strcasestr(cur_t->content, "you're not listening") ||
                        hu_strcasestr(cur_t->content, "never mind") ||
                        hu_strcasestr(cur_t->content, "forget it"))
                        rsig.explicit_correction = true;
                }
            }
            const char *last_asst = NULL;
            size_t last_asst_len = 0;
            if (agent->history_count > 0) {
                for (size_t hi = agent->history_count; hi > 0; hi--) {
                    if (agent->history[hi - 1].role == HU_ROLE_ASSISTANT &&
                        agent->history[hi - 1].content_len > 0) {
                        last_asst = agent->history[hi - 1].content;
                        last_asst_len = agent->history[hi - 1].content_len;
                        break;
                    }
                }
            }
            hu_rupture_stage_t prev_rup = agent->frontiers.rupture.stage;
            hu_rupture_evaluate(agent->alloc, &agent->frontiers.rupture, &rsig, last_asst,
                                last_asst_len);
            if (agent->observer && agent->frontiers.rupture.stage != prev_rup) {
                hu_observer_event_t fev = {.tag = HU_OBSERVER_EVENT_FRONTIER};
                fev.data.frontier.frontier = "rupture";
                fev.data.frontier.transition =
                    hu_rupture_stage_name(agent->frontiers.rupture.stage);
                fev.data.frontier.value = agent->frontiers.rupture.severity;
                hu_observer_record_event(*agent->observer, &fev);
            }
            hu_rupture_build_context(agent->alloc, &agent->frontiers.rupture, &rupture_ctx,
                                     &rupture_ctx_len);
        }

        /* F3: Narrative Self */
        hu_narrative_self_build_context(agent->alloc, &agent->frontiers.narrative,
                                        &narrative_self_ctx, &narrative_self_ctx_len);

        /* F10: Creative Voice */
        hu_creative_voice_build_context(agent->alloc, &agent->frontiers.creative_voice,
                                        &creative_voice_ctx, &creative_voice_ctx_len);

        /* F11: Growth Narrative */
        hu_growth_narrative_build_context(agent->alloc, &agent->frontiers.growth,
                                          agent->memory_session_id, &growth_ctx, &growth_ctx_len);

        /* F12: Genuine Boundaries */
        {
            uint32_t boundary_rel_stage = 0;
            if (agent->persona && agent->memory_session_id) {
                const hu_contact_profile_t *bcp = hu_persona_find_contact(
                    agent->persona, agent->memory_session_id, agent->memory_session_id_len);
                if (bcp && bcp->relationship_stage) {
                    if (strstr(bcp->relationship_stage, "deep"))
                        boundary_rel_stage = 3;
                    else if (strstr(bcp->relationship_stage, "trusted") ||
                             strstr(bcp->relationship_stage, "familiar"))
                        boundary_rel_stage = 2;
                    else
                        boundary_rel_stage = 1;
                }
            }
            const hu_genuine_boundary_t *matched = NULL;
            hu_genuine_boundary_check_relevance(&agent->frontiers.boundaries, msg, msg_len,
                                                &matched);
            if (matched) {
                hu_genuine_boundary_build_context(agent->alloc, matched, boundary_rel_stage,
                                                  &boundary_ctx, &boundary_ctx_len);
            }
        }

        /* Trust calibration: update and build context */
        {
            float comp_sig = 0.0f, benev_sig = 0.0f, integ_sig = 0.0f;
            if (agent->infra.emotional_cognition.state.valence > 0.3f)
                benev_sig = 0.5f;
            if (agent->infra.emotional_cognition.state.valence < -0.3f)
                benev_sig = -0.3f;
            if (agent->outcomes && agent->outcomes->total > 0) {
                comp_sig = (float)agent->outcomes->tool_successes / (float)agent->outcomes->total;
                integ_sig = comp_sig;
            }
            hu_tcal_update(&agent->frontiers.trust, comp_sig, benev_sig, integ_sig);
            if (hu_tcal_check_erosion(&agent->frontiers.trust) && agent->observer) {
                hu_observer_event_t tev = {
                    .tag = HU_OBSERVER_EVENT_FRONTIER,
                    .trace_id = agent->trace_id,
                    .data.frontier = {.frontier = "trust",
                                      .transition = "erosion",
                                      .value = agent->frontiers.trust.erosion_rate}};
                hu_observer_record_event(*agent->observer, &tev);
            }
            hu_tcal_build_context(agent->alloc, &agent->frontiers.trust, &trust_ctx,
                                  &trust_ctx_len);
        }

        /* Humor framework: evaluate context and build directive */
        {
            hu_humor_context_t hctx;
            memset(&hctx, 0, sizeof(hctx));
            hctx.risk_tolerance = HU_HUMOR_RISK_TOLERANCE;
            hctx.in_serious_context = (agent->infra.emotional_cognition.state.intensity > 0.7f &&
                                       agent->infra.emotional_cognition.state.valence < -0.2f);
            if (agent->active_channel) {
                hctx.channel = agent->active_channel;
                hctx.channel_len = agent->active_channel_len;
            }
            if (agent->memory_session_id) {
                hctx.contact_id = agent->memory_session_id;
                hctx.contact_id_len = agent->memory_session_id_len;
            }
            /* Bridge persona humor style preferences into the structured evaluation */
            if (agent->persona && agent->persona->humor.style_count > 0) {
                for (size_t hs = 0;
                     hs < agent->persona->humor.style_count && hctx.preferred_count < 8; hs++) {
                    const char *s = agent->persona->humor.style[hs];
                    hu_humor_fw_style_t mapped = HU_HUMOR_FW_OBSERVATIONAL;
                    if (strstr(s, "dry") || strstr(s, "deadpan"))
                        mapped = HU_HUMOR_FW_DRY;
                    else if (strstr(s, "self") || strstr(s, "deprecat"))
                        mapped = HU_HUMOR_FW_SELF_DEPRECATING;
                    else if (strstr(s, "word") || strstr(s, "pun"))
                        mapped = HU_HUMOR_FW_WORDPLAY;
                    else if (strstr(s, "absurd") || strstr(s, "surreal"))
                        mapped = HU_HUMOR_FW_ABSURDIST;
                    else if (strstr(s, "observ"))
                        mapped = HU_HUMOR_FW_OBSERVATIONAL;
                    hctx.preferred_styles[hctx.preferred_count++] = mapped;
                }
            }
            hu_humor_evaluation_t heval;
            memset(&heval, 0, sizeof(heval));
            hu_humor_fw_evaluate_context(msg, msg_len, &hctx, &heval);
            if (heval.should_attempt) {
                hu_humor_fw_build_directive(agent->alloc, &heval, &hctx, &humor_dir,
                                            &humor_dir_len);
                humor_theory_saved = (int)heval.suggested_theory;
                if (humor_dir && agent->observer) {
                    hu_observer_event_t hev = {.tag = HU_OBSERVER_EVENT_FRONTIER,
                                               .trace_id = agent->trace_id,
                                               .data.frontier = {.frontier = "humor",
                                                                 .transition = "directive_injected",
                                                                 .value = heval.composite}};
                    hu_observer_record_event(*agent->observer, &hev);
                }
            }
        }

        /* Sycophancy pre-check: when the user expresses strong opinions,
         * proactively inject friction directive to prevent reflexive agreement */
        {
            static const char *opinion_pats[] = {
                "i think", "i believe",        "i feel",    "in my opinion", "don't you think",
                "right?",  "wouldn't you say", "obviously", "clearly",       "everyone knows",
            };
            size_t opinion_hits = 0;
            for (size_t pi = 0; pi < sizeof(opinion_pats) / sizeof(opinion_pats[0]); pi++) {
                size_t plen = strlen(opinion_pats[pi]);
                for (size_t mi = 0; mi + plen <= msg_len; mi++) {
                    bool m = true;
                    for (size_t k = 0; k < plen; k++) {
                        char a = (char)(msg[mi + k] >= 'A' && msg[mi + k] <= 'Z' ? msg[mi + k] + 32
                                                                                 : msg[mi + k]);
                        if (a != opinion_pats[pi][k]) {
                            m = false;
                            break;
                        }
                    }
                    if (m) {
                        opinion_hits++;
                        break;
                    }
                }
            }
            if (opinion_hits >= HU_OPINION_FRICTION_COUNT) {
                hu_sycophancy_result_t syc_synth;
                memset(&syc_synth, 0, sizeof(syc_synth));
                syc_synth.flagged = true;
                syc_synth.total_risk = 0.5f;
                syc_synth.factor_scores[HU_SYCOPHANCY_UNCRITICAL_AGREEMENT] =
                    (float)opinion_hits * 0.25f;
                hu_sycophancy_build_friction(agent->alloc, &syc_synth, msg, msg_len,
                                             &syc_friction_ctx, &syc_friction_ctx_len);
            }
        }

        /* F7: Relational Episode Memory — load recent episodes for this contact */
#ifdef HU_ENABLE_SQLITE
        if (agent->memory && agent->memory_session_id && agent->memory_session_id_len > 0) {
            sqlite3 *ep_db = hu_sqlite_memory_get_db(agent->memory);
            if (ep_db) {
                static const char ep_sql[] =
                    "SELECT summary, felt_sense, relational_meaning, significance, warmth, "
                    "timestamp FROM relational_episodes WHERE contact_id = ?1 "
                    "ORDER BY significance DESC LIMIT 5";
                sqlite3_stmt *ep_stmt = NULL;
                int ep_rc = sqlite3_prepare_v2(ep_db, ep_sql, -1, &ep_stmt, NULL);
                if (ep_rc == SQLITE_OK) {
                    sqlite3_bind_text(ep_stmt, 1, agent->memory_session_id,
                                      (int)agent->memory_session_id_len, SQLITE_STATIC);
                    hu_relational_episode_t loaded_eps[5];
                    size_t ep_n = 0;
                    while (sqlite3_step(ep_stmt) == SQLITE_ROW && ep_n < 5) {
                        hu_relational_episode_init(&loaded_eps[ep_n]);
                        const char *s = (const char *)sqlite3_column_text(ep_stmt, 0);
                        const char *f = (const char *)sqlite3_column_text(ep_stmt, 1);
                        const char *r = (const char *)sqlite3_column_text(ep_stmt, 2);
                        float sig = (float)sqlite3_column_double(ep_stmt, 3);
                        float wrm = (float)sqlite3_column_double(ep_stmt, 4);
                        uint64_t ts = (uint64_t)sqlite3_column_int64(ep_stmt, 5);
                        hu_relational_episode_set(agent->alloc, &loaded_eps[ep_n],
                                                  agent->memory_session_id, s ? s : "", f ? f : "",
                                                  r ? r : "", sig, wrm, ts);
                        ep_n++;
                    }
                    sqlite3_finalize(ep_stmt);
                    if (ep_n > 0)
                        hu_relational_episode_build_context(agent->alloc, loaded_eps, ep_n,
                                                            &rel_episode_ctx, &rel_episode_ctx_len);
                    for (size_t epi = 0; epi < ep_n; epi++)
                        hu_relational_episode_free(agent->alloc, &loaded_eps[epi]);
                }
            }
            /* Conversation goals: load active goals for this contact */
            if (ep_db && agent->memory_session_id && agent->memory_session_id_len > 0) {
                char qsql[512];
                size_t qsql_len = 0;
                if (hu_conv_goals_query_active_sql(agent->memory_session_id,
                                                   agent->memory_session_id_len, qsql, sizeof(qsql),
                                                   &qsql_len) == HU_OK) {
                    sqlite3_stmt *gs = NULL;
                    if (sqlite3_prepare_v2(ep_db, qsql, (int)qsql_len, &gs, NULL) == SQLITE_OK) {
                        hu_conv_goal_t goals[4];
                        size_t gc = 0;
                        while (sqlite3_step(gs) == SQLITE_ROW && gc < 4) {
                            memset(&goals[gc], 0, sizeof(goals[gc]));
                            const char *d = (const char *)sqlite3_column_text(gs, 2);
                            if (d) {
                                goals[gc].description = hu_strndup(agent->alloc, d, strlen(d));
                                goals[gc].description_len =
                                    goals[gc].description ? strlen(goals[gc].description) : 0;
                            }
                            const char *sig = (const char *)sqlite3_column_text(gs, 3);
                            if (sig) {
                                goals[gc].success_signal =
                                    hu_strndup(agent->alloc, sig, strlen(sig));
                                goals[gc].success_signal_len =
                                    goals[gc].success_signal ? strlen(goals[gc].success_signal) : 0;
                            }
                            goals[gc].priority = (hu_goal_priority_t)sqlite3_column_int(gs, 5);
                            goals[gc].attempts = (uint8_t)sqlite3_column_int(gs, 9);
                            goals[gc].max_attempts = (uint8_t)sqlite3_column_int(gs, 10);
                            gc++;
                        }
                        sqlite3_finalize(gs);
                        if (gc > 0)
                            hu_conv_goals_build_prompt(agent->alloc, goals, gc, &conv_goals_ctx,
                                                       &conv_goals_ctx_len);
                        for (size_t gi = 0; gi < gc; gi++)
                            hu_conv_goal_deinit(agent->alloc, &goals[gi]);
                    }
                }
            }
        }
        if (!rel_episode_ctx)
#endif
        {
            hu_relational_episode_build_context(agent->alloc, NULL, 0, &rel_episode_ctx,
                                                &rel_episode_ctx_len);
        }
    }

    /* Cognition: episodic replay + emotional context strings for prompt */
#ifdef HU_ENABLE_SQLITE
    if (agent->infra.cognition_db &&
        (agent->infra.current_cognition_mode == HU_COGNITION_SLOW ||
         agent->infra.current_cognition_mode == HU_COGNITION_EMOTIONAL)) {
        hu_episodic_pattern_t *ep_patterns = NULL;
        size_t ep_cnt = 0;
        if (hu_episodic_retrieve(agent->infra.cognition_db, agent->alloc, msg, msg_len, 3,
                                 &ep_patterns, &ep_cnt) == HU_OK &&
            ep_cnt > 0) {
            if (hu_episodic_build_replay(agent->alloc, ep_patterns, ep_cnt, &episodic_replay,
                                         &episodic_replay_len) == HU_OK &&
                episodic_replay && agent->bth_metrics)
                agent->bth_metrics->episodic_replays++;
            hu_episodic_free_patterns(agent->alloc, ep_patterns, ep_cnt);
        }
    }
#endif
    if (hu_emotional_cognition_build_prompt(agent->alloc, &agent->infra.emotional_cognition,
                                            &emotional_ctx, &emotional_ctx_len) != HU_OK) {
        emotional_ctx = NULL;
        emotional_ctx_len = 0;
    }

    /* PERSONA-001: Affect mirror ceiling — cap emotional intensity in prompt.
     * Prevents the AI from matching extreme emotional intensity, especially
     * with acquaintances where mirroring high emotion can feel uncanny. */
    if (emotional_ctx && emotional_ctx_len > 0 &&
        agent->infra.emotional_cognition.state.intensity > 0.1f && agent->persona &&
        agent->memory_session_id) {
        const hu_contact_profile_t *amc_cp = hu_persona_find_contact(
            agent->persona, agent->memory_session_id, agent->memory_session_id_len);
        const hu_persona_overlay_t *amc_ov =
            agent->active_channel ? hu_persona_find_overlay(agent->persona, agent->active_channel,
                                                            agent->active_channel_len)
                                  : NULL;
        float ceiling = hu_affect_mirror_ceiling(amc_cp, amc_ov);
        char dampen_dir[256];
        float capped = hu_affect_mirror_apply(agent->infra.emotional_cognition.state.intensity,
                                              ceiling, dampen_dir, sizeof(dampen_dir));
        (void)capped;
        if (dampen_dir[0] != '\0') {
            size_t dir_len = strlen(dampen_dir);
            size_t new_len = emotional_ctx_len + 1 + dir_len + 1;
            char *aug = (char *)agent->alloc->alloc(agent->alloc->ctx, new_len + 1);
            if (aug) {
                memcpy(aug, emotional_ctx, emotional_ctx_len);
                aug[emotional_ctx_len] = '\n';
                memcpy(aug + emotional_ctx_len + 1, dampen_dir, dir_len);
                aug[emotional_ctx_len + 1 + dir_len] = '\n';
                aug[new_len] = '\0';
                agent->alloc->free(agent->alloc->ctx, emotional_ctx, emotional_ctx_len + 1);
                emotional_ctx = aug;
                emotional_ctx_len = new_len;
            }
        }
    }

    /* Per-contact Turing hints: load weak-dimension guidance for this contact */
    char *contact_turing_hint = NULL;
    size_t contact_turing_hint_len = 0;
#ifdef HU_ENABLE_SQLITE
    if (agent->memory && agent->memory_session_id && agent->memory_session_id_len > 0) {
        sqlite3 *ct_db = hu_sqlite_memory_get_db(agent->memory);
        if (ct_db) {
            (void)hu_turing_init_tables(ct_db);
            int ct_dims[HU_TURING_DIM_COUNT];
            if (hu_turing_get_contact_dimensions(ct_db, agent->memory_session_id,
                                                 agent->memory_session_id_len, ct_dims) == HU_OK) {
                contact_turing_hint =
                    hu_turing_build_contact_hint(agent->alloc, ct_dims, &contact_turing_hint_len);
            }
        }
    }
#endif

    /* Build system prompt using cached static portion when available */
    char *system_prompt = NULL;
    size_t system_prompt_len = 0;
    if (agent->cached_static_prompt && !pref_ctx && !tone_hint && !persona_prompt &&
        !awareness_ctx && !stm_ctx && !commitment_ctx && !pattern_ctx && !adaptive_ctx &&
        !proactive_ctx && !superhuman_ctx && !skills_ctx && !emotional_ctx && !episodic_replay &&
        !humanness_ctx && !imperfect_dir && !residue_dir && !contact_turing_hint && !somatic_ctx &&
        !presence_ctx && !micro_expr_ctx && !novelty_ctx && !attachment_ctx && !rupture_ctx &&
        !narrative_self_ctx && !creative_voice_ctx && !growth_ctx && !boundary_ctx &&
        !rel_episode_ctx && !trust_ctx && !humor_dir && !syc_friction_ctx && !conv_goals_ctx &&
        !outcome_ctx && !intelligence_ctx && !acp_context && !plan_ctx && !instruction_ctx) {
        err = hu_prompt_build_with_cache(agent->alloc, agent->cached_static_prompt,
                                         agent->cached_static_prompt_len, memory_ctx,
                                         memory_ctx_len, &system_prompt, &system_prompt_len);
        if (memory_ctx)
            agent->alloc->free(agent->alloc->ctx, memory_ctx, memory_ctx_len + 1);
        if (stm_ctx) {
            agent->alloc->free(agent->alloc->ctx, stm_ctx, stm_ctx_len + 1);
            stm_ctx = NULL;
        }
        if (err != HU_OK) {
            if (getenv("HU_DEBUG"))
                hu_log_error("agent_turn", NULL, "FAIL at prompt_build_with_cache: %s",
                             hu_error_string(err));
            if (pref_ctx)
                agent->alloc->free(agent->alloc->ctx, pref_ctx, pref_ctx_len + 1);
            if (commitment_ctx)
                agent->alloc->free(agent->alloc->ctx, commitment_ctx, commitment_ctx_len + 1);
            if (pattern_ctx)
                agent->alloc->free(agent->alloc->ctx, pattern_ctx, pattern_ctx_len + 1);
            if (adaptive_ctx)
                agent->alloc->free(agent->alloc->ctx, adaptive_ctx, adaptive_ctx_len + 1);
            if (proactive_ctx)
                agent->alloc->free(agent->alloc->ctx, proactive_ctx, proactive_ctx_len + 1);
            if (superhuman_ctx)
                agent->alloc->free(agent->alloc->ctx, superhuman_ctx, superhuman_ctx_len + 1);
            if (outcome_ctx)
                agent->alloc->free(agent->alloc->ctx, outcome_ctx, outcome_ctx_len + 1);
            if (intelligence_ctx)
                agent->alloc->free(agent->alloc->ctx, intelligence_ctx, intelligence_ctx_len + 1);
            if (emotional_ctx)
                agent->alloc->free(agent->alloc->ctx, emotional_ctx, emotional_ctx_len + 1);
            if (episodic_replay)
                agent->alloc->free(agent->alloc->ctx, episodic_replay, episodic_replay_len + 1);
            if (plan_ctx)
                agent->alloc->free(agent->alloc->ctx, plan_ctx, plan_ctx_len + 1);
            if (humanness_ctx)
                agent->alloc->free(agent->alloc->ctx, humanness_ctx, humanness_ctx_len + 1);
            if (imperfect_dir)
                agent->alloc->free(agent->alloc->ctx, imperfect_dir, imperfect_dir_len + 1);
            if (residue_dir)
                agent->alloc->free(agent->alloc->ctx, residue_dir, residue_dir_len + 1);
            if (contact_turing_hint)
                agent->alloc->free(agent->alloc->ctx, contact_turing_hint,
                                   contact_turing_hint_len + 1);
            hu_agent_clear_current_for_tools();
            return err;
        }
    } else {
        /* Merge ACP inter-agent context into superhuman context */
        if (acp_context && acp_context_len > 0) {
            if (superhuman_ctx) {
                size_t merged_len = superhuman_ctx_len + 1 + acp_context_len;
                char *merged = (char *)agent->alloc->alloc(agent->alloc->ctx, merged_len + 1);
                if (merged) {
                    memcpy(merged, superhuman_ctx, superhuman_ctx_len);
                    merged[superhuman_ctx_len] = '\n';
                    memcpy(merged + superhuman_ctx_len + 1, acp_context, acp_context_len);
                    merged[merged_len] = '\0';
                    agent->alloc->free(agent->alloc->ctx, superhuman_ctx, superhuman_ctx_len + 1);
                    superhuman_ctx = merged;
                    superhuman_ctx_len = merged_len;
                }
            } else {
                superhuman_ctx = acp_context;
                superhuman_ctx_len = acp_context_len;
                acp_context = NULL;
            }
        }

        const char *cognition_mode_str =
            hu_cognition_mode_name(agent->infra.current_cognition_mode);
        size_t cognition_mode_str_len = strlen(cognition_mode_str);

        /* Render personal-model prompt block from accumulated user signal.
         * Stack buffer (~8 KB) bounded by HU_PM_MAX_FACTS / TOPICS / GOALS.
         * Skipped entirely when the model has no real content yet so we
         * don't burn tokens on the "(No detailed personal data yet.)"
         * placeholder on a fresh install. */
        char personal_model_buf[8192];
        const char *personal_model_ctx = NULL;
        size_t personal_model_ctx_len = 0;
        if (hu_personal_model_has_content(&agent->personal_model)) {
            size_t pm_n = hu_personal_model_build_prompt(&agent->personal_model,
                                                         personal_model_buf,
                                                         sizeof(personal_model_buf));
            if (pm_n > 0) {
                personal_model_ctx = personal_model_buf;
                personal_model_ctx_len = pm_n;
            }
        }

        /* W9 world-model snapshot (FIX 12). Cached for 60s by hu_world_model_load
         * so per-turn cost is dominated by the SQL fetch on first miss. We only
         * inject when the bridge returns a non-empty body (no signal -> NULL ->
         * skip the section, just like personal_model above). */
        char *world_model_ctx = NULL;
        size_t world_model_ctx_len = 0;
        if (agent->w7_facade && agent->memory_session_id && agent->memory_session_id_len > 0) {
            hu_w7_render_world_model(agent->w7_facade, agent->alloc, agent->memory_session_id,
                                     agent->memory_session_id_len, 0, &world_model_ctx,
                                     &world_model_ctx_len);
            if (world_model_ctx_len > 0)
                agent->world_model_loads++;
        }

        hu_prompt_config_t cfg = {
            .provider_name = agent->provider.vtable->get_name(agent->provider.ctx),
            .provider_name_len = 0,
            .model_name = agent->model_name,
            .model_name_len = agent->model_name_len,
            .workspace_dir = agent->workspace_dir,
            .workspace_dir_len = agent->workspace_dir_len,
            .tools = agent->tools,
            .tools_count = agent->tools_count,
            .memory_context = memory_ctx,
            .memory_context_len = memory_ctx_len,
            .instruction_context = instruction_ctx,
            .instruction_context_len = instruction_ctx_len,
            .stm_context = stm_ctx,
            .stm_context_len = stm_ctx_len,
            .commitment_context = commitment_ctx,
            .commitment_context_len = commitment_ctx_len,
            .pattern_context = pattern_ctx,
            .pattern_context_len = pattern_ctx_len,
            .adaptive_persona_context = adaptive_ctx,
            .adaptive_persona_context_len = adaptive_ctx_len,
            .proactive_context = proactive_ctx,
            .proactive_context_len = proactive_ctx_len,
            .superhuman_context = superhuman_ctx,
            .superhuman_context_len = superhuman_ctx_len,
            .autonomy_level = agent->autonomy_level,
            .custom_instructions = agent->custom_instructions,
            .custom_instructions_len = agent->custom_instructions_len,
            .persona_prompt = persona_prompt,
            .persona_prompt_len = persona_prompt_len,
            .preferences = pref_ctx,
            .preferences_len = pref_ctx_len,
            .chain_of_thought = agent->chain_of_thought,
            .tone_hint = tone_hint,
            .tone_hint_len = tone_hint_len,
            .awareness_context = awareness_ctx,
            .awareness_context_len = awareness_ctx_len,
            .outcome_context = outcome_ctx,
            .outcome_context_len = outcome_ctx_len,
            .persona_immersive = (persona_prompt && persona_prompt_len > 0),
            .persona = agent->persona,
            .contact_context = agent->contact_context,
            .contact_context_len = agent->contact_context_len,
            .conversation_context = agent->conversation_context,
            .conversation_context_len = agent->conversation_context_len,
            .max_response_chars = agent->max_response_chars,
            .intelligence_context = intelligence_ctx,
            .intelligence_context_len = intelligence_ctx_len,
            .skills_context = skills_ctx,
            .skills_context_len = skills_ctx_len,
            .native_tools = (agent->provider.vtable->supports_native_tools &&
                             agent->provider.vtable->supports_native_tools(agent->provider.ctx)),
            .emotional_context = emotional_ctx,
            .emotional_context_len = emotional_ctx_len,
            .cognition_mode = cognition_mode_str,
            .cognition_mode_len = cognition_mode_str_len,
            .episodic_replay = episodic_replay,
            .episodic_replay_len = episodic_replay_len,
            .humanness_context = humanness_ctx,
            .humanness_context_len = humanness_ctx_len,
            .imperfect_delivery = imperfect_dir,
            .imperfect_delivery_len = imperfect_dir_len,
            .residue_carryover = residue_dir,
            .residue_carryover_len = residue_dir_len,
            .contact_turing_hint = contact_turing_hint,
            .contact_turing_hint_len = contact_turing_hint_len,
            .somatic_context = somatic_ctx,
            .somatic_context_len = somatic_ctx_len,
            .narrative_self_context = narrative_self_ctx,
            .narrative_self_context_len = narrative_self_ctx_len,
            .presence_context = presence_ctx,
            .presence_context_len = presence_ctx_len,
            .micro_expression_context = micro_expr_ctx,
            .micro_expression_context_len = micro_expr_ctx_len,
            .creative_voice_context = creative_voice_ctx,
            .creative_voice_context_len = creative_voice_ctx_len,
            .novelty_context = novelty_ctx,
            .novelty_context_len = novelty_ctx_len,
            .attachment_context = attachment_ctx,
            .attachment_context_len = attachment_ctx_len,
            .rupture_context = rupture_ctx,
            .rupture_context_len = rupture_ctx_len,
            .growth_context = growth_ctx,
            .growth_context_len = growth_ctx_len,
            .boundary_context = boundary_ctx,
            .boundary_context_len = boundary_ctx_len,
            .relational_episode_context = rel_episode_ctx,
            .relational_episode_context_len = rel_episode_ctx_len,
            .trust_context = trust_ctx,
            .trust_context_len = trust_ctx_len,
            .humor_directive = humor_dir,
            .humor_directive_len = humor_dir_len,
            .sycophancy_friction = syc_friction_ctx,
            .sycophancy_friction_len = syc_friction_ctx_len,
            .conv_goals_context = conv_goals_ctx,
            .conv_goals_context_len = conv_goals_ctx_len,
            .personal_model_context = personal_model_ctx,
            .personal_model_context_len = personal_model_ctx_len,
            .world_model_context = world_model_ctx,
            .world_model_context_len = world_model_ctx_len,
        };
        err = hu_prompt_build_system(agent->alloc, &cfg, &system_prompt, &system_prompt_len);
        if (world_model_ctx) {
            agent->alloc->free(agent->alloc->ctx, world_model_ctx, world_model_ctx_len + 1);
            world_model_ctx = NULL;
            world_model_ctx_len = 0;
        }
        if (persona_prompt)
            agent->alloc->free(agent->alloc->ctx, persona_prompt, persona_prompt_len + 1);
        persona_prompt = NULL;
        if (memory_ctx)
            agent->alloc->free(agent->alloc->ctx, memory_ctx, memory_ctx_len + 1);
        if (stm_ctx) {
            agent->alloc->free(agent->alloc->ctx, stm_ctx, stm_ctx_len + 1);
            stm_ctx = NULL;
        }
        if (awareness_ctx) {
            agent->alloc->free(agent->alloc->ctx, awareness_ctx, awareness_ctx_len + 1);
            awareness_ctx = NULL;
        }
        if (outcome_ctx) {
            agent->alloc->free(agent->alloc->ctx, outcome_ctx, outcome_ctx_len + 1);
            outcome_ctx = NULL;
        }
        if (intelligence_ctx) {
            agent->alloc->free(agent->alloc->ctx, intelligence_ctx, intelligence_ctx_len + 1);
            intelligence_ctx = NULL;
        }
        if (emotional_ctx) {
            agent->alloc->free(agent->alloc->ctx, emotional_ctx, emotional_ctx_len + 1);
            emotional_ctx = NULL;
        }
        if (episodic_replay)
            agent->alloc->free(agent->alloc->ctx, episodic_replay, episodic_replay_len + 1);
        episodic_replay = NULL;
        if (humanness_ctx)
            agent->alloc->free(agent->alloc->ctx, humanness_ctx, humanness_ctx_len + 1);
        humanness_ctx = NULL;
        if (imperfect_dir)
            agent->alloc->free(agent->alloc->ctx, imperfect_dir, imperfect_dir_len + 1);
        imperfect_dir = NULL;
        if (residue_dir)
            agent->alloc->free(agent->alloc->ctx, residue_dir, residue_dir_len + 1);
        residue_dir = NULL;
        if (contact_turing_hint)
            agent->alloc->free(agent->alloc->ctx, contact_turing_hint, contact_turing_hint_len + 1);
        contact_turing_hint = NULL;
        if (somatic_ctx)
            agent->alloc->free(agent->alloc->ctx, somatic_ctx, somatic_ctx_len + 1);
        somatic_ctx = NULL;
        if (narrative_self_ctx)
            agent->alloc->free(agent->alloc->ctx, narrative_self_ctx, narrative_self_ctx_len + 1);
        narrative_self_ctx = NULL;
        if (presence_ctx)
            agent->alloc->free(agent->alloc->ctx, presence_ctx, presence_ctx_len + 1);
        presence_ctx = NULL;
        if (micro_expr_ctx)
            agent->alloc->free(agent->alloc->ctx, micro_expr_ctx, micro_expr_ctx_len + 1);
        micro_expr_ctx = NULL;
        if (creative_voice_ctx)
            agent->alloc->free(agent->alloc->ctx, creative_voice_ctx, creative_voice_ctx_len + 1);
        creative_voice_ctx = NULL;
        if (novelty_ctx)
            agent->alloc->free(agent->alloc->ctx, novelty_ctx, novelty_ctx_len + 1);
        novelty_ctx = NULL;
        if (attachment_ctx)
            agent->alloc->free(agent->alloc->ctx, attachment_ctx, attachment_ctx_len + 1);
        attachment_ctx = NULL;
        if (rupture_ctx)
            agent->alloc->free(agent->alloc->ctx, rupture_ctx, rupture_ctx_len + 1);
        rupture_ctx = NULL;
        if (growth_ctx)
            agent->alloc->free(agent->alloc->ctx, growth_ctx, growth_ctx_len + 1);
        growth_ctx = NULL;
        if (boundary_ctx)
            agent->alloc->free(agent->alloc->ctx, boundary_ctx, boundary_ctx_len + 1);
        boundary_ctx = NULL;
        if (rel_episode_ctx)
            agent->alloc->free(agent->alloc->ctx, rel_episode_ctx, rel_episode_ctx_len + 1);
        rel_episode_ctx = NULL;
        if (trust_ctx)
            agent->alloc->free(agent->alloc->ctx, trust_ctx, trust_ctx_len + 1);
        trust_ctx = NULL;
        had_humor_dir = (humor_dir && humor_dir_len > 0);
        if (humor_dir)
            agent->alloc->free(agent->alloc->ctx, humor_dir, humor_dir_len + 1);
        humor_dir = NULL;
        if (syc_friction_ctx)
            agent->alloc->free(agent->alloc->ctx, syc_friction_ctx, syc_friction_ctx_len + 1);
        syc_friction_ctx = NULL;
        if (conv_goals_ctx)
            agent->alloc->free(agent->alloc->ctx, conv_goals_ctx, conv_goals_ctx_len + 1);
        conv_goals_ctx = NULL;
        cognition_skills_shown = (skills_ctx != NULL && skills_ctx_len > 0);
        if (skills_ctx)
            agent->alloc->free(agent->alloc->ctx, skills_ctx, skills_ctx_len + 1);
        skills_ctx = NULL;
        if (err != HU_OK) {
            if (getenv("HU_DEBUG"))
                hu_log_error("agent_turn", NULL, "FAIL at prompt_build_system: %s",
                             hu_error_string(err));
            if (pref_ctx)
                agent->alloc->free(agent->alloc->ctx, pref_ctx, pref_ctx_len + 1);
            if (commitment_ctx)
                agent->alloc->free(agent->alloc->ctx, commitment_ctx, commitment_ctx_len + 1);
            if (pattern_ctx)
                agent->alloc->free(agent->alloc->ctx, pattern_ctx, pattern_ctx_len + 1);
            if (adaptive_ctx)
                agent->alloc->free(agent->alloc->ctx, adaptive_ctx, adaptive_ctx_len + 1);
            if (proactive_ctx)
                agent->alloc->free(agent->alloc->ctx, proactive_ctx, proactive_ctx_len + 1);
            if (superhuman_ctx)
                agent->alloc->free(agent->alloc->ctx, superhuman_ctx, superhuman_ctx_len + 1);
            if (plan_ctx)
                agent->alloc->free(agent->alloc->ctx, plan_ctx, plan_ctx_len + 1);
            if (emotional_ctx)
                agent->alloc->free(agent->alloc->ctx, emotional_ctx, emotional_ctx_len + 1);
            if (episodic_replay)
                agent->alloc->free(agent->alloc->ctx, episodic_replay, episodic_replay_len + 1);
            hu_agent_clear_current_for_tools();
            return err;
        }
    }
    if (stm_ctx) {
        agent->alloc->free(agent->alloc->ctx, stm_ctx, stm_ctx_len + 1);
        stm_ctx = NULL;
    }
    if (commitment_ctx) {
        agent->alloc->free(agent->alloc->ctx, commitment_ctx, commitment_ctx_len + 1);
        commitment_ctx = NULL;
    }
    if (pattern_ctx) {
        agent->alloc->free(agent->alloc->ctx, pattern_ctx, pattern_ctx_len + 1);
        pattern_ctx = NULL;
    }
    if (adaptive_ctx) {
        agent->alloc->free(agent->alloc->ctx, adaptive_ctx, adaptive_ctx_len + 1);
        adaptive_ctx = NULL;
    }
    if (proactive_ctx) {
        agent->alloc->free(agent->alloc->ctx, proactive_ctx, proactive_ctx_len + 1);
        proactive_ctx = NULL;
    }
    if (superhuman_ctx) {
        agent->alloc->free(agent->alloc->ctx, superhuman_ctx, superhuman_ctx_len + 1);
        superhuman_ctx = NULL;
    }
    if (pref_ctx) {
        agent->alloc->free(agent->alloc->ctx, pref_ctx, pref_ctx_len + 1);
        pref_ctx = NULL;
    }

    /* Inject learned DPO preferences as few-shot examples */
    if (agent->sota.sota_initialized && agent->sota.dpo_collector.db && system_prompt) {
        char *dpo_examples = NULL;
        size_t dpo_len = 0;
        if (hu_dpo_get_best_examples(&agent->sota.dpo_collector, agent->alloc, 5, &dpo_examples,
                                     &dpo_len) == HU_OK &&
            dpo_examples) {
            size_t new_len = system_prompt_len + dpo_len;
            char *new_sp = (char *)agent->alloc->realloc(agent->alloc->ctx, system_prompt,
                                                         system_prompt_len + 1, new_len + 1);
            if (new_sp) {
                memcpy(new_sp + system_prompt_len, dpo_examples, dpo_len);
                new_sp[new_len] = '\0';
                system_prompt = new_sp;
                system_prompt_len = new_len;
            }
            agent->alloc->free(agent->alloc->ctx, dpo_examples, dpo_len + 1);
        }
    }

    /* Anti-sycophancy: inject directive when we hold a strong opinion on the topic */
#ifdef HU_ENABLE_SQLITE
    if (system_prompt && agent->memory && msg && msg_len > 0) {
        sqlite3 *syc_db = hu_sqlite_memory_get_db(agent->memory);
        size_t syc_len = 0;
        char *syc_directive =
            hu_opinion_check_before_agree(agent->alloc, syc_db, msg, msg_len, &syc_len);
        if (syc_directive && syc_len > 0) {
            size_t new_len = system_prompt_len + syc_len;
            char *new_sp = (char *)agent->alloc->realloc(agent->alloc->ctx, system_prompt,
                                                         system_prompt_len + 1, new_len + 1);
            if (new_sp) {
                memcpy(new_sp + system_prompt_len, syc_directive, syc_len);
                new_sp[new_len] = '\0';
                system_prompt = new_sp;
                system_prompt_len = new_len;
            }
            agent->alloc->free(agent->alloc->ctx, syc_directive, syc_len + 1);
        }
        size_t contr_len = 0;
        char *contr_directive = hu_opinion_contrarian_prompt(agent->alloc, msg, msg_len,
                                                             (int)agent->history_count, &contr_len);
        if (contr_directive && contr_len > 0) {
            size_t new_len = system_prompt_len + contr_len;
            char *new_sp = (char *)agent->alloc->realloc(agent->alloc->ctx, system_prompt,
                                                         system_prompt_len + 1, new_len + 1);
            if (new_sp) {
                memcpy(new_sp + system_prompt_len, contr_directive, contr_len);
                new_sp[new_len] = '\0';
                system_prompt = new_sp;
                system_prompt_len = new_len;
            }
            agent->alloc->free(agent->alloc->ctx, contr_directive, contr_len + 1);
        }
    }
#endif

    /* Prompt cache: hash system prompt for provider-level deduplication.
     * On first occurrence of a prompt hash, generate a stable cache ID from the hash
     * (hex-encoded) so providers with server-side caching (Anthropic, Gemini) can
     * recognize repeated system prompts. The cache ID is stored and reused across turns. */
    const char *prompt_cache_id = NULL;
    size_t prompt_cache_id_len = 0;
    if (system_prompt && system_prompt_len > 0) {
        if (!agent->infra.prompt_cache) {
            agent->infra.prompt_cache = (struct hu_prompt_cache *)agent->alloc->alloc(
                agent->alloc->ctx, sizeof(hu_prompt_cache_t));
            if (agent->infra.prompt_cache)
                hu_prompt_cache_init((hu_prompt_cache_t *)agent->infra.prompt_cache, agent->alloc);
        }
        if (agent->infra.prompt_cache) {
            uint64_t phash = hu_prompt_cache_hash(system_prompt, system_prompt_len);
            size_t cached_id_len = 0;
            prompt_cache_id = hu_prompt_cache_lookup(
                (const hu_prompt_cache_t *)agent->infra.prompt_cache, phash, &cached_id_len);
            if (prompt_cache_id && cached_id_len > 0) {
                prompt_cache_id_len = cached_id_len;
            } else {
                char id_buf[24];
                int id_len =
                    snprintf(id_buf, sizeof(id_buf), "hu_%016llx", (unsigned long long)phash);
                if (id_len > 0 && (size_t)id_len < sizeof(id_buf)) {
                    (void)hu_prompt_cache_store((hu_prompt_cache_t *)agent->infra.prompt_cache,
                                                phash, id_buf, (size_t)id_len, 3600);
                    prompt_cache_id =
                        hu_prompt_cache_lookup((const hu_prompt_cache_t *)agent->infra.prompt_cache,
                                               phash, &cached_id_len);
                    prompt_cache_id_len = cached_id_len;
                }
            }
        }
    }

    /* Tool routing: if enabled, select relevant subset of tools for this message */
    hu_tool_selection_t tool_selection;
    memset(&tool_selection, 0, sizeof(tool_selection));
    hu_tool_spec_t *routed_specs = NULL;
    size_t routed_specs_count = 0;
    if (agent->tool_routing_enabled && agent->tools_count > HU_TOOL_ROUTER_MAX_SELECTED) {
        hu_error_t rerr = hu_tool_router_select(agent->alloc, msg, msg_len, agent->tools,
                                                agent->tools_count, &tool_selection);
        if (rerr == HU_OK && tool_selection.count > 0) {
            routed_specs = (hu_tool_spec_t *)agent->alloc->alloc(
                agent->alloc->ctx, tool_selection.count * sizeof(hu_tool_spec_t));
            if (routed_specs) {
                for (size_t ri = 0; ri < tool_selection.count; ri++) {
                    size_t idx = tool_selection.indices[ri];
                    if (idx < agent->tool_specs_count)
                        routed_specs[routed_specs_count++] = agent->tool_specs[idx];
                }
            }
        }
    }

    /* Tool cache: use persistent TTL cache across turns, with per-turn fallback */
    if (!agent->infra.tool_cache_ttl) {
        agent->infra.tool_cache_ttl = (struct hu_tool_cache_ttl *)agent->alloc->alloc(
            agent->alloc->ctx, sizeof(hu_tool_cache_ttl_t));
        if (agent->infra.tool_cache_ttl)
            hu_tool_cache_ttl_init((hu_tool_cache_ttl_t *)agent->infra.tool_cache_ttl,
                                   agent->alloc);
    }
    if (agent->infra.tool_cache_ttl) {
        int64_t now = (int64_t)time(NULL);
        (void)hu_tool_cache_ttl_evict_expired((hu_tool_cache_ttl_t *)agent->infra.tool_cache_ttl,
                                              now);
    }
    hu_tool_cache_t *turn_cache = NULL;
    hu_tool_cache_create(agent->alloc, &turn_cache);

    hu_chat_message_t *msgs = NULL;
    size_t msgs_count = 0;

    hu_chat_request_t req;
    memset(&req, 0, sizeof(req));
    req.model = agent->model_name;
    req.model_len = agent->model_name_len;
    req.temperature = agent->temperature;
    if (routed_specs_count > 0) {
        req.tools = routed_specs;
        req.tools_count = routed_specs_count;
    } else {
        req.tools = (agent->tool_specs_count > 0) ? agent->tool_specs : NULL;
        req.tools_count = agent->tool_specs_count;
    }

    uint32_t iter = 0;
    size_t turn_tool_results_count = 0;
    int reflection_retries_left = agent->reflection.max_retries;
    char *dpo_rejected_resp = NULL;
    size_t dpo_rejected_resp_len = 0;
    uint64_t max_tokens =
        agent->token_limit ? agent->token_limit
                           : hu_context_tokens_resolve(0, agent->model_name, agent->model_name_len);
    if (max_tokens == 0)
        max_tokens = 128000u;

    hu_compaction_config_t compact_cfg;
    hu_compaction_config_default(&compact_cfg);
    compact_cfg.max_history_messages = agent->max_history_messages;
    compact_cfg.token_limit = max_tokens;
    compact_cfg.use_structured_summary = agent->compaction_use_structured;

#ifdef HU_ENABLE_SQLITE
    if (agent->infra.metacognition.cfg.enabled && agent->infra.cognition_db &&
        agent->infra.metacognition.pending_outcome_trace_id[0] != '\0') {
        float ox = hu_metacog_label_from_followup(msg, msg_len);
        (void)hu_metacog_history_update_outcome(
            agent->infra.cognition_db, agent->infra.metacognition.pending_outcome_trace_id, ox);
        agent->infra.metacognition.pending_outcome_trace_id[0] = '\0';
    } else if (!agent->infra.metacognition.cfg.enabled) {
        agent->infra.metacognition.pending_outcome_trace_id[0] = '\0';
    }
#endif

    hu_agent_internal_generate_trace_id(agent->trace_id);

    agent->infra.metacognition.regen_count = 0;
    agent->infra.metacognition.consecutive_bad_count = 0;
    if (agent->infra.metacognition.cfg.enabled) {
        agent->infra.metacognition.difficulty = hu_metacog_estimate_difficulty(msg, msg_len);
        if (agent->bth_metrics) {
            switch (agent->infra.metacognition.difficulty) {
            case HU_METACOG_DIFFICULTY_EASY:
                agent->bth_metrics->metacog_difficulty_easy++;
                break;
            case HU_METACOG_DIFFICULTY_MEDIUM:
                agent->bth_metrics->metacog_difficulty_medium++;
                break;
            case HU_METACOG_DIFFICULTY_HARD:
                agent->bth_metrics->metacog_difficulty_hard++;
                break;
            default:
                break;
            }
        }
        if (agent->infra.metacognition.difficulty == HU_METACOG_DIFFICULTY_HARD &&
            agent->turn_thinking_budget > 0) {
            int nb = (int)((double)agent->turn_thinking_budget * 1.25);
            if (nb > agent->turn_thinking_budget)
                agent->turn_thinking_budget = nb;
        }
    } else {
        agent->infra.metacognition.difficulty = HU_METACOG_DIFFICULTY_EASY;
    }

    if (agent->infra.metacognition.cfg.enabled) {
        const char *mlp = getenv("HUMAN_METACOG_LOGPROBS");
        if (mlp && (strcmp(mlp, "1") == 0 || strcmp(mlp, "true") == 0 || strcmp(mlp, "on") == 0))
            req.include_completion_logprobs = true;
    }

    req.prompt_cache_id = prompt_cache_id;
    req.prompt_cache_id_len = prompt_cache_id_len;

    clock_t turn_start = clock();
    uint64_t turn_tokens = 0;
    const char *prov_name = agent->provider.vtable->get_name
                                ? agent->provider.vtable->get_name(agent->provider.ctx)
                                : NULL;

    {
        hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_AGENT_START, .data = {{0}}};
        ev.data.agent_start.provider = prov_name ? prov_name : "";
        ev.data.agent_start.model = agent->model_name ? agent->model_name : "";
        HU_OBS_SAFE_RECORD_EVENT(agent, &ev);
    }

    /* Per-turn arena: reset each iteration to reclaim ephemeral message arrays */
    hu_allocator_t turn_alloc =
        agent->turn_arena ? hu_arena_allocator(agent->turn_arena) : *agent->alloc;

    /* Silence intuition: decide if we should skip the LLM call entirely */
    {
        hu_emotional_weight_t ew = hu_emotional_weight_classify(msg, msg_len);
        /* Detect explicit questions AND imperative requests (help me, can you, etc.) */
        bool user_asked = (msg_len > 0 && memchr(msg, '?', msg_len) != NULL);
        if (!user_asked && msg && msg_len >= 4) {
            static const char *request_phrases[] = {
                "help",    "can you", "could you", "please", "how do", "what is",
                "show me", "tell me", "explain",   "write",  "create", "fix",
                "find",    "search",  "give me",   "build",  "make",   "do ",
            };
            for (size_t ri = 0; ri < sizeof(request_phrases) / sizeof(request_phrases[0]); ri++) {
                size_t rlen = strlen(request_phrases[ri]);
                if (msg_len >= rlen) {
                    for (size_t p = 0; p + rlen <= msg_len; p++) {
                        bool match = true;
                        for (size_t c = 0; c < rlen && match; c++) {
                            char lc = msg[p + c];
                            if (lc >= 'A' && lc <= 'Z')
                                lc += 32;
                            if (lc != request_phrases[ri][c])
                                match = false;
                        }
                        if (match) {
                            user_asked = true;
                            goto silence_check;
                        }
                    }
                }
            }
        }
    silence_check:;
        hu_silence_response_t silence =
            hu_silence_intuit(msg, msg_len, ew, (uint32_t)agent->history_count, user_asked);
        if (silence != HU_SILENCE_FULL_RESPONSE) {
            const char *silence_resp = NULL;
            size_t silence_resp_len = 0;
            if (silence == HU_SILENCE_ACTUAL_SILENCE) {
                silence_resp = "";
                silence_resp_len = 0;
            } else {
                char *ack =
                    hu_silence_build_acknowledgment(agent->alloc, silence, &silence_resp_len);
                silence_resp = ack;
            }
            if (silence_resp || silence == HU_SILENCE_ACTUAL_SILENCE) {
                *response_out = silence_resp
                                    ? hu_strndup(agent->alloc, silence_resp, silence_resp_len)
                                    : hu_strndup(agent->alloc, "", 0);
                if (response_len_out)
                    *response_len_out = silence_resp_len;
                /* Record this silent turn for learning (don't drop from training) */
#ifdef HU_ENABLE_SQLITE
                if (agent->memory) {
                    hu_experience_store_t sil_exp;
                    if (hu_experience_store_init(agent->alloc, agent->memory, &sil_exp) == HU_OK) {
                        sqlite3 *sil_db = hu_sqlite_memory_get_db(agent->memory);
                        if (sil_db)
                            sil_exp.db = sil_db;
                        (void)hu_experience_record(&sil_exp, msg, msg_len, "silence_intuit", 14,
                                                   silence_resp ? silence_resp : "",
                                                   silence_resp_len, 0.5);
                        hu_experience_store_deinit(&sil_exp);
                    }
                }
#endif
                /* Free silence acknowledgment if allocated */
                if (silence_resp && silence != HU_SILENCE_ACTUAL_SILENCE)
                    agent->alloc->free(agent->alloc->ctx, (void *)silence_resp,
                                       silence_resp_len + 1);
                /* Free all allocated context buffers (system_prompt consumed
                 * most ctx vars; these survive past prompt build) */
                if (system_prompt)
                    agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
                if (intelligence_ctx)
                    agent->alloc->free(agent->alloc->ctx, intelligence_ctx,
                                       intelligence_ctx_len + 1);
                if (plan_ctx)
                    agent->alloc->free(agent->alloc->ctx, plan_ctx, plan_ctx_len + 1);
                if (routed_specs)
                    agent->alloc->free(agent->alloc->ctx, routed_specs,
                                       routed_specs_count * sizeof(hu_tool_spec_t));
                if (pref_ctx)
                    agent->alloc->free(agent->alloc->ctx, pref_ctx, pref_ctx_len + 1);
                if (commitment_ctx)
                    agent->alloc->free(agent->alloc->ctx, commitment_ctx, commitment_ctx_len + 1);
                if (pattern_ctx)
                    agent->alloc->free(agent->alloc->ctx, pattern_ctx, pattern_ctx_len + 1);
                if (adaptive_ctx)
                    agent->alloc->free(agent->alloc->ctx, adaptive_ctx, adaptive_ctx_len + 1);
                if (proactive_ctx)
                    agent->alloc->free(agent->alloc->ctx, proactive_ctx, proactive_ctx_len + 1);
                if (superhuman_ctx)
                    agent->alloc->free(agent->alloc->ctx, superhuman_ctx, superhuman_ctx_len + 1);
                if (outcome_ctx)
                    agent->alloc->free(agent->alloc->ctx, outcome_ctx, outcome_ctx_len + 1);
                if (acp_context)
                    agent->alloc->free(agent->alloc->ctx, acp_context, acp_context_len + 1);
                hu_agent_clear_current_for_tools();
                return HU_OK;
            }
            /* If acknowledgment build failed, fall through to full response */
        }
    }

    while (iter < agent->max_tool_iterations) {
        if (agent->cancel_requested) {
            if (dpo_rejected_resp)
                agent->alloc->free(agent->alloc->ctx, dpo_rejected_resp, dpo_rejected_resp_len + 1);
            if (system_prompt)
                agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
            if (plan_ctx)
                agent->alloc->free(agent->alloc->ctx, plan_ctx, plan_ctx_len + 1);
            if (routed_specs)
                agent->alloc->free(agent->alloc->ctx, routed_specs,
                                   routed_specs_count * sizeof(hu_tool_spec_t));
            if (turn_cache)
                hu_tool_cache_destroy(agent->alloc, turn_cache);
            if (agent->turn_arena)
                hu_arena_reset(agent->turn_arena);
            return HU_ERR_CANCELLED;
        }
        if (agent->turn_arena)
            hu_arena_reset(agent->turn_arena);
        msgs = NULL;
        msgs_count = 0;
        iter++;
        /* Compact history if it exceeds limits (before each provider call).
         * Uses LLM summarization when the provider is available, with
         * rule-based fallback. */
        if (hu_should_compact(agent->history, agent->history_count, &compact_cfg)) {
            hu_error_t compact_err =
                hu_compact_history_llm(agent->alloc, &agent->history, &agent->history_count,
                                       &agent->history_cap, &compact_cfg, &agent->provider);
            if (compact_err != HU_OK)
                hu_log_warn("agent", NULL, "history compaction failed: %s",
                            hu_error_string(compact_err));

            /* Hierarchical summarization for deeper memory */
            if (agent->provider.vtable && agent->provider.vtable->chat) {
                char *session_sum = NULL, *chapter_sum = NULL, *overall_sum = NULL;
                size_t session_len = 0, chapter_len = 0, overall_len = 0;
                if (agent->history_count > 0) {
                    const char *last_content = agent->history[agent->history_count - 1].content;
                    size_t last_len = last_content ? strlen(last_content) : 0;
                    if (last_len > 0 && hu_compact_hierarchical(
                                            agent->alloc, &agent->provider, agent->model_name,
                                            agent->model_name_len, last_content, last_len,
                                            &session_sum, &session_len, &chapter_sum, &chapter_len,
                                            &overall_sum, &overall_len) == HU_OK) {
#if defined(HU_ENABLE_SQLITE)
                        if (agent->memory && agent->memory->vtable &&
                            agent->memory->vtable->store && agent->memory_session_id &&
                            agent->memory_session_id_len > 0) {
                            hu_memory_category_t hcat = {.tag = HU_MEMORY_CATEGORY_CONVERSATION};
                            const char *hsid = agent->memory_session_id;
                            size_t hsid_len = agent->memory_session_id_len;
                            static const char src[] = "compaction";
                            if (session_sum && session_len > 0) {
                                hu_error_t store_err = hu_memory_store_with_source(
                                    agent->memory, "hierarchical_session", 20, session_sum,
                                    session_len, &hcat, hsid, hsid_len, src, sizeof(src) - 1);
                                if (store_err != HU_OK)
                                    hu_log_warn("agent", NULL, "memory store failed: %s",
                                                hu_error_string(store_err));
                            }
                            if (chapter_sum && chapter_len > 0) {
                                hu_error_t store_err = hu_memory_store_with_source(
                                    agent->memory, "hierarchical_chapter", 21, chapter_sum,
                                    chapter_len, &hcat, hsid, hsid_len, src, sizeof(src) - 1);
                                if (store_err != HU_OK)
                                    hu_log_warn("agent", NULL, "memory store failed: %s",
                                                hu_error_string(store_err));
                            }
                            if (overall_sum && overall_len > 0) {
                                hu_error_t store_err = hu_memory_store_with_source(
                                    agent->memory, "hierarchical_overall", 21, overall_sum,
                                    overall_len, &hcat, hsid, hsid_len, src, sizeof(src) - 1);
                                if (store_err != HU_OK)
                                    hu_log_warn("agent", NULL, "memory store failed: %s",
                                                hu_error_string(store_err));
                            }
                        }
#endif
                    }
                    if (session_sum)
                        agent->alloc->free(agent->alloc->ctx, session_sum, session_len + 1);
                    if (chapter_sum)
                        agent->alloc->free(agent->alloc->ctx, chapter_sum, chapter_len + 1);
                    if (overall_sum)
                        agent->alloc->free(agent->alloc->ctx, overall_sum, overall_len + 1);
                }
            }
        }

        /* Context pressure: estimate tokens, check thresholds, auto-compact if needed */
        {
            uint64_t current = hu_estimate_tokens(agent->history, agent->history_count) +
                               (uint64_t)((system_prompt_len + 3) / 4);
            hu_context_pressure_t pr = {
                .current_tokens = (size_t)current,
                .max_tokens = (size_t)max_tokens,
                .pressure = 0.0f,
                .warning_85_emitted = agent->context_pressure_warning_85_emitted,
                .warning_95_emitted = agent->context_pressure_warning_95_emitted,
            };
            if (agent->compact_context_enabled &&
                hu_context_check_pressure(&pr, agent->context_pressure_warn,
                                          agent->context_pressure_compact)) {
                hu_context_compact_for_pressure(agent->alloc, agent->history, &agent->history_count,
                                                &agent->history_cap, (size_t)max_tokens,
                                                agent->context_compact_target);
                agent->context_pressure_warning_85_emitted = false;
                agent->context_pressure_warning_95_emitted = false;
            } else {
                agent->context_pressure_warning_85_emitted = pr.warning_85_emitted;
                agent->context_pressure_warning_95_emitted = pr.warning_95_emitted;
            }
        }

        /* Format messages for this iteration using arena allocator */
        {
            bool *kv_hist_mask = NULL;
            if (agent->history_count > 0) {
                if (!agent->infra.kv_cache) {
                    agent->infra.kv_cache = (hu_kv_cache_manager_t *)agent->alloc->alloc(
                        agent->alloc->ctx, sizeof(hu_kv_cache_manager_t));
                    if (agent->infra.kv_cache &&
                        hu_kv_cache_init(agent->infra.kv_cache, agent->alloc,
                                         max_tokens > 0 ? (uint32_t)max_tokens : 128u * 1024u) !=
                            HU_OK) {
                        agent->alloc->free(agent->alloc->ctx, agent->infra.kv_cache,
                                           sizeof(hu_kv_cache_manager_t));
                        agent->infra.kv_cache = NULL;
                    }
                }
                if (agent->infra.kv_cache) {
                    hu_kv_cache_manager_t *kvm = agent->infra.kv_cache;
                    kvm->eviction_threshold = 0.8f;
                    hu_kv_cache_clear(kvm);
                    (void)hu_kv_cache_add_segment(kvm, "system", 6,
                                                  (uint32_t)((system_prompt_len + 3) / 4), true);
                    for (size_t hi = 0; hi < agent->history_count; hi++) {
                        char lbl[40];
                        int ln = snprintf(lbl, sizeof(lbl), "h%zu", hi);
                        if (ln <= 0)
                            continue;
                        const hu_owned_message_t *hm = &agent->history[hi];
                        size_t tlen = hm->content_len;
                        (void)hu_kv_cache_add_segment(kvm, lbl, (size_t)ln,
                                                      (uint32_t)((tlen + 3) / 4) + 4u, false);
                    }
                    if (hu_kv_cache_needs_eviction(kvm)) {
                        const char *evicted[HU_KV_CACHE_MAX_SEGMENTS];
                        size_t n_ev = hu_kv_cache_prune(kvm, evicted, HU_KV_CACHE_MAX_SEGMENTS);
                        if (n_ev > 0) {
                            kv_hist_mask = (bool *)agent->alloc->alloc(
                                agent->alloc->ctx, agent->history_count * sizeof(bool));
                            if (kv_hist_mask) {
                                size_t keep = 0;
                                for (size_t z = 0; z < agent->history_count; z++)
                                    kv_hist_mask[z] = true;
                                for (size_t e = 0; e < n_ev; e++) {
                                    if (!evicted[e] || evicted[e][0] != 'h')
                                        continue;
                                    char *endp = NULL;
                                    unsigned long idx = strtoul(evicted[e] + 1, &endp, 10);
                                    if (endp != evicted[e] + 1 && idx < agent->history_count)
                                        kv_hist_mask[idx] = false;
                                }
                                for (size_t z = 0; z < agent->history_count; z++) {
                                    if (kv_hist_mask[z])
                                        keep++;
                                }
                                if (keep == 0) {
                                    agent->alloc->free(agent->alloc->ctx, kv_hist_mask,
                                                       agent->history_count * sizeof(bool));
                                    kv_hist_mask = NULL;
                                }
                            }
                        }
                    }
                }
            }

            hu_chat_message_t *hist_msgs = NULL;
            size_t hist_count = 0;
            err = hu_context_format_messages(&turn_alloc, agent->history, agent->history_count,
                                             agent->max_history_messages, kv_hist_mask, &hist_msgs,
                                             &hist_count);
            if (kv_hist_mask)
                agent->alloc->free(agent->alloc->ctx, kv_hist_mask,
                                   agent->history_count * sizeof(bool));
            if (err != HU_OK) {
                if (getenv("HU_DEBUG"))
                    hu_log_error("agent_turn", NULL, "FAIL at history_build: %s",
                                 hu_error_string(err));
                hu_agent_clear_current_for_tools();
                if (dpo_rejected_resp)
                    agent->alloc->free(agent->alloc->ctx, dpo_rejected_resp,
                                       dpo_rejected_resp_len + 1);
                if (system_prompt)
                    agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
                if (plan_ctx)
                    agent->alloc->free(agent->alloc->ctx, plan_ctx, plan_ctx_len + 1);
                if (turn_cache)
                    hu_tool_cache_destroy(agent->alloc, turn_cache);
                if (routed_specs)
                    agent->alloc->free(agent->alloc->ctx, routed_specs,
                                       routed_specs_count * sizeof(hu_tool_spec_t));
                return err;
            }
            size_t total = (hist_msgs ? hist_count : 0) + 1;
            hu_chat_message_t *all = (hu_chat_message_t *)turn_alloc.alloc(
                turn_alloc.ctx, total * sizeof(hu_chat_message_t));
            if (!all) {
                hu_agent_clear_current_for_tools();
                if (dpo_rejected_resp)
                    agent->alloc->free(agent->alloc->ctx, dpo_rejected_resp,
                                       dpo_rejected_resp_len + 1);
                if (system_prompt)
                    agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
                if (plan_ctx)
                    agent->alloc->free(agent->alloc->ctx, plan_ctx, plan_ctx_len + 1);
                if (turn_cache)
                    hu_tool_cache_destroy(agent->alloc, turn_cache);
                if (routed_specs)
                    agent->alloc->free(agent->alloc->ctx, routed_specs,
                                       routed_specs_count * sizeof(hu_tool_spec_t));
                return HU_ERR_OUT_OF_MEMORY;
            }
            all[0].role = HU_ROLE_SYSTEM;
            all[0].content = system_prompt;
            all[0].content_len = system_prompt_len;
            all[0].name = NULL;
            all[0].name_len = 0;
            all[0].tool_call_id = NULL;
            all[0].tool_call_id_len = 0;
            all[0].content_parts = NULL;
            all[0].content_parts_count = 0;
            for (size_t i = 0; i < (hist_msgs ? hist_count : 0); i++)
                all[i + 1] = hist_msgs[i];
            msgs = all;
            msgs_count = total;
            req.messages = msgs;
            req.messages_count = msgs_count;
        }

        {
            hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_LLM_REQUEST, .data = {{0}}};
            ev.data.llm_request.provider = prov_name ? prov_name : "";
            ev.data.llm_request.model = agent->model_name ? agent->model_name : "";
            ev.data.llm_request.messages_count = msgs_count;
            HU_OBS_SAFE_RECORD_EVENT(agent, &ev);
        }

        /* Extended reasoning: for complex queries on first iteration, run a
         * structured reasoning phase — ToT exploration + reasoning summary for the prompt. */
#ifndef HU_IS_TEST
        if (agent->tree_of_thought_enabled && iter == 1 && msg_len > 200) {
            hu_tot_config_t tot_cfg = hu_tot_config_default();
            hu_tot_result_t tot_result;
            memset(&tot_result, 0, sizeof(tot_result));
            if (hu_tot_explore(agent->alloc, &agent->provider, agent->model_name,
                               agent->model_name_len, msg, msg_len, &tot_cfg,
                               &tot_result) == HU_OK &&
                tot_result.best_thought && tot_result.best_thought_len > 0) {

                /* Build structured reasoning appendix from ToT exploration results */
                char tot_reasoning_buf[4096];
                int sp_len = snprintf(
                    tot_reasoning_buf, sizeof(tot_reasoning_buf),
                    "\n\n[REASONING SCRATCHPAD]\n"
                    "Branches explored: %zu | Best confidence: %.0f%%\n"
                    "Approach: %.*s\n"
                    "[/REASONING SCRATCHPAD]\n",
                    tot_result.branches_explored, tot_result.best_score * 100.0,
                    (int)(tot_result.best_thought_len < 2048 ? tot_result.best_thought_len : 2048),
                    tot_result.best_thought);

                if (sp_len > 0 && (size_t)sp_len < sizeof(tot_reasoning_buf) && system_prompt) {
                    size_t new_sys_len = system_prompt_len + (size_t)sp_len;
                    char *new_sys = (char *)agent->alloc->alloc(agent->alloc->ctx, new_sys_len + 1);
                    if (new_sys) {
                        memcpy(new_sys, system_prompt, system_prompt_len);
                        memcpy(new_sys + system_prompt_len, tot_reasoning_buf, (size_t)sp_len);
                        new_sys[new_sys_len] = '\0';
                        agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
                        system_prompt = new_sys;
                        system_prompt_len = new_sys_len;
                        if (msgs && msgs_count > 0) {
                            msgs[0].content = system_prompt;
                            msgs[0].content_len = system_prompt_len;
                        }
                    }
                }
            }
            hu_tot_result_free(agent->alloc, &tot_result);
        }
#endif

        /* PRM: score reasoning chain from ToT reasoning appendix if available */
        double prm_turn_score = 0.0;

        /* Adaptive model selection: use per-turn override when set by the daemon's
         * model router, or run heuristic routing inline if no daemon set it. */
        const char *turn_model = agent->model_name;
        size_t turn_model_len = agent->model_name_len;
        double turn_temp = agent->temperature;
        if (agent->turn_model && agent->turn_model_len > 0) {
            turn_model = agent->turn_model;
            turn_model_len = agent->turn_model_len;
        } else {
            /* Inline routing: emotional/vulnerable messages get better models */
            hu_model_router_config_t mr_cfg = hu_model_router_default_config();
            mr_cfg.on_device_available = agent->on_device_available;
            const char *rel = NULL;
            size_t rel_len = 0;
            if (agent->relationship.stage >= HU_REL_TRUSTED) {
                rel = "trusted";
                rel_len = 7;
            } else if (agent->relationship.stage >= HU_REL_FAMILIAR) {
                rel = "friend";
                rel_len = 6;
            }
            hu_model_selection_t sel =
                hu_model_route(&mr_cfg, msg, msg_len, rel, rel_len, -1, agent->history_count);
            if (sel.tier >= HU_TIER_ANALYTICAL && sel.model && sel.model_len > 0) {
                turn_model = sel.model;
                turn_model_len = sel.model_len;
                if (sel.temperature > 0.0)
                    turn_temp = sel.temperature;
            }
        }
        if (agent->turn_temperature > 0.0)
            turn_temp = agent->turn_temperature;
        if (agent->turn_thinking_budget > 0)
            req.thinking_budget = agent->turn_thinking_budget;

        /* Planning mode: give the model more room to reason when the cognition
         * system detects a complex task, without overriding explicit CoT config. */
        if (cognition_budget.enable_planning && req.thinking_budget == 0)
            req.thinking_budget = 2048;

        /* Somatic energy influence: low energy → shorter responses, lower creativity. */
        if (agent->frontiers.initialized &&
            agent->frontiers.somatic.energy < HU_SOMATIC_TIRED_THRESHOLD) {
            if (agent->max_response_chars == 0 || agent->max_response_chars > 300)
                req.max_tokens = 300;
            if (turn_temp > 0.7)
                turn_temp = 0.7;
        } else if (agent->frontiers.initialized &&
                   agent->frontiers.somatic.energy < HU_SOMATIC_LOW_THRESHOLD) {
            if (agent->max_response_chars == 0 || agent->max_response_chars > 600)
                req.max_tokens = 600;
        }

        /* Empathy mode: slightly warmer temperature for emotional responses,
         * and remove token caps that might truncate heartfelt replies. */
        if (cognition_budget.prioritize_empathy) {
            if (turn_temp < 0.8)
                turn_temp = 0.8;
            if (req.max_tokens > 0 && req.max_tokens < 600)
                req.max_tokens = 600;
        }

        /* Adaptive token budget: classify tier and apply budget constraints */
        if (agent->sota.token_budget.enabled) {
            hu_thinking_tier_t tier =
                hu_token_budget_classify(msg, msg_len, agent->history_count, agent->tools_count);
            const hu_tier_budget_t *budget = hu_token_budget_get(&agent->sota.token_budget, tier);
            if (budget) {
                if (budget->thinking_budget > 0 && req.thinking_budget == 0)
                    req.thinking_budget = (int)budget->thinking_budget;
                if (budget->temperature > 0.0 && agent->turn_temperature <= 0.0)
                    turn_temp = budget->temperature;
            }
        }

        /* Content sensitivity check (EdgeClaw-inspired S1/S2/S3 tiers):
         * S3 content (secrets, private keys) requires local-only model.
         * S2 content (PII) logs audit trail. */
        {
            hu_sensitivity_result_t sens = hu_sensitivity_classify_message(msg, msg_len);
            if (hu_sensitivity_requires_local(sens.level)) {
                const char *prev_model = turn_model;
                if (agent->sota.degradation_config.s3_local_model &&
                    agent->sota.degradation_config.s3_local_model_len > 0) {
                    turn_model = agent->sota.degradation_config.s3_local_model;
                    turn_model_len = agent->sota.degradation_config.s3_local_model_len;
                } else if (agent->sota.degradation_config.fallback_model &&
                           agent->sota.degradation_config.fallback_model_len > 0) {
                    turn_model = agent->sota.degradation_config.fallback_model;
                    turn_model_len = agent->sota.degradation_config.fallback_model_len;
                }
                hu_log_info("agent", agent->observer, "S3 sensitivity: %s — routed %.*s -> %.*s",
                            sens.reason ? sens.reason : "private content",
                            (int)(prev_model ? strlen(prev_model) : 0),
                            prev_model ? prev_model : "", (int)turn_model_len,
                            turn_model ? turn_model : "");
                {
                    hu_observer_event_t sev = {.tag = HU_OBSERVER_EVENT_ERR, .data = {{0}}};
                    sev.data.err.component = "sensitivity";
                    sev.data.err.message =
                        sens.reason ? sens.reason : "S3 private — model rerouted";
                    HU_OBS_SAFE_RECORD_EVENT(agent, &sev);
                }
            } else if (sens.level == HU_SENSITIVITY_S2) {
                hu_log_info("agent", agent->observer,
                            "S2 sensitivity detected (%s) — PII present, logged for audit",
                            sens.reason ? sens.reason : "personal data");
                {
                    hu_observer_event_t sev = {.tag = HU_OBSERVER_EVENT_ERR, .data = {{0}}};
                    sev.data.err.component = "sensitivity";
                    sev.data.err.message = sens.reason ? sens.reason : "S2 PII detected";
                    HU_OBS_SAFE_RECORD_EVENT(agent, &sev);
                }
            }
        }

        clock_t llm_start = clock();
        hu_chat_response_t resp;
        memset(&resp, 0, sizeof(resp));

        /* OTLP tracing: begin LLM span */
        hu_otlp_span_t *llm_span = NULL;
        if (agent->observer) {
            static hu_otlp_trace_t otlp_trace;
            static bool otlp_inited = false;
            if (!otlp_inited) {
                hu_otlp_trace_init(&otlp_trace);
                otlp_inited = true;
            }
            hu_otlp_span_begin(&otlp_trace, "llm.chat", 8, NULL, &llm_span);
        }

        /* W10: KV cache — check for cached prompt metadata before provider call */
#ifdef HU_ENABLE_SQLITE
        bool kv_cache_hit = false;
        char kv_prompt_hash[24];
        kv_prompt_hash[0] = '\0';
        if (agent->w7_facade && system_prompt && system_prompt_len > 0 &&
            turn_model && turn_model_len > 0) {
            hu_memory_facade_t *kv_mf = hu_w7_facade_memory_handle(agent->w7_facade);
            if (kv_mf) {
                uint64_t kv_phash = hu_prompt_cache_hash(system_prompt, system_prompt_len);
                snprintf(kv_prompt_hash, sizeof(kv_prompt_hash), "%016llx",
                         (unsigned long long)kv_phash);
                char kv_model_ver[64];
                size_t mv_len = turn_model_len < sizeof(kv_model_ver) - 1
                                    ? turn_model_len
                                    : sizeof(kv_model_ver) - 1;
                memcpy(kv_model_ver, turn_model, mv_len);
                kv_model_ver[mv_len] = '\0';
                hu_kv_cache_entry_t *cached = NULL;
                if (hu_kv_cache_get(kv_mf, kv_prompt_hash, kv_model_ver, agent->alloc,
                                    &cached) == HU_OK) {
                    kv_cache_hit = true;
                    hu_log_info("agent_turn", agent->observer,
                                "W10 KV cache hit: hash=%s tokens=%lld", kv_prompt_hash,
                                (long long)cached->prompt_token_count);
                    hu_kv_cache_entry_free(agent->alloc, cached);
                }
            }
        }
#endif

        /* Provider graceful degradation: try primary -> fallback -> honest failure */
        if (getenv("HU_DEBUG"))
            hu_log_info("agent_turn", NULL,
                        "calling provider chat: model=%.*s msgs=%zu tools=%zu degrade=%d",
                        (int)(turn_model_len > 40 ? 40 : turn_model_len),
                        turn_model ? turn_model : "(null)", req.messages_count, req.tools_count,
                        agent->sota.degradation_config.enabled);
        hu_degrade_strategy_t degrade_strategy = HU_DEGRADE_PRIMARY;
        if (agent->sota.degradation_config.enabled) {
            hu_degradation_result_t degrade_result;
            err = hu_provider_degrade_chat(&agent->sota.degradation_config, &agent->provider,
                                           agent->alloc, &req, turn_model, turn_model_len,
                                           turn_temp, &degrade_result);
            resp = degrade_result.response;
            degrade_strategy = degrade_result.strategy_used;
        } else {
            err = agent->provider.vtable->chat(agent->provider.ctx, agent->alloc, &req, turn_model,
                                               turn_model_len, turn_temp, &resp);
        }

        /* On-device → cloud fallback: if on-device model failed, retry with cloud reflexive */
        if (err != HU_OK && turn_model && turn_model_len > 0 && agent->config &&
            agent->config->agent.mr_on_device_enabled) {
            hu_model_router_config_t fb_cfg = hu_model_router_default_config();
            if (fb_cfg.on_device_model && turn_model_len == fb_cfg.on_device_model_len &&
                memcmp(turn_model, fb_cfg.on_device_model, turn_model_len) == 0 &&
                fb_cfg.reflexive_model) {
                hu_log_info("agent_turn", agent->observer,
                            "on-device failed (err=%d), falling back to cloud: %s", err,
                            fb_cfg.reflexive_model);
                memset(&resp, 0, sizeof(resp));
                err = agent->provider.vtable->chat(agent->provider.ctx, agent->alloc, &req,
                                                   fb_cfg.reflexive_model,
                                                   fb_cfg.reflexive_model_len, turn_temp, &resp);
            }
        }

        (void)degrade_strategy;
        uint64_t llm_duration_ms = hu_agent_internal_clock_diff_ms(llm_start, clock());
        if (llm_span)
            hu_otlp_span_end(llm_span, (err == HU_OK) ? 1 : 2);

        /* W10: KV cache — store prompt metadata after successful provider call */
#ifdef HU_ENABLE_SQLITE
        if (err == HU_OK && !kv_cache_hit && kv_prompt_hash[0] && agent->w7_facade &&
            turn_model && turn_model_len > 0) {
            hu_memory_facade_t *kv_mf = hu_w7_facade_memory_handle(agent->w7_facade);
            if (kv_mf) {
                hu_kv_cache_entry_t kv_entry;
                memset(&kv_entry, 0, sizeof(kv_entry));
                strncpy(kv_entry.prompt_hash, kv_prompt_hash,
                        sizeof(kv_entry.prompt_hash) - 1);
                size_t mv_len = turn_model_len < sizeof(kv_entry.model_version) - 1
                                    ? turn_model_len
                                    : sizeof(kv_entry.model_version) - 1;
                memcpy(kv_entry.model_version, turn_model, mv_len);
                kv_entry.model_version[mv_len] = '\0';
                kv_entry.prompt_token_count = (int64_t)resp.usage.prompt_tokens;
                (void)hu_kv_cache_put(kv_mf, &kv_entry);
            }
        }
#endif

        {
            hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_LLM_RESPONSE, .data = {{0}}};
            ev.data.llm_response.provider = prov_name ? prov_name : "";
            ev.data.llm_response.model = agent->model_name ? agent->model_name : "";
            ev.data.llm_response.duration_ms = llm_duration_ms;
            ev.data.llm_response.success = (err == HU_OK);
            ev.data.llm_response.error_message = (err != HU_OK) ? "chat failed" : NULL;
            HU_OBS_SAFE_RECORD_EVENT(agent, &ev);
        }

        /* Degradation honest failure: show message but return non-OK for CLI exit codes. */
        if (err == HU_ERR_PROVIDER_RESPONSE && agent->sota.degradation_config.enabled &&
            degrade_strategy == HU_DEGRADE_HONEST_FAILURE && resp.content && resp.content_len > 0) {
            char *dup = hu_strndup(agent->alloc, resp.content, resp.content_len);
            size_t dup_len = dup ? strlen(dup) : 0;
            hu_chat_response_free(agent->alloc, &resp);
            memset(&resp, 0, sizeof(resp));
            if (dup) {
                *response_out = dup;
                if (response_len_out)
                    *response_len_out = dup_len;
                hu_error_t hist_err = hu_agent_internal_append_history(
                    agent, HU_ROLE_ASSISTANT, dup, dup_len, NULL, 0, NULL, 0);
                if (hist_err != HU_OK)
                    hu_log_warn("agent", NULL, "failed to append assistant history: %s",
                                hu_error_string(hist_err));
                {
                    hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_ERR, .data = {{0}}};
                    ev.data.err.component = "agent";
                    ev.data.err.message = "provider degraded (honest failure)";
                    HU_OBS_SAFE_RECORD_EVENT(agent, &ev);
                }
                hu_agent_clear_current_for_tools();
                if (dpo_rejected_resp)
                    agent->alloc->free(agent->alloc->ctx, dpo_rejected_resp,
                                       dpo_rejected_resp_len + 1);
                if (system_prompt)
                    agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
                if (plan_ctx)
                    agent->alloc->free(agent->alloc->ctx, plan_ctx, plan_ctx_len + 1);
                if (routed_specs)
                    agent->alloc->free(agent->alloc->ctx, routed_specs,
                                       routed_specs_count * sizeof(hu_tool_spec_t));
                if (turn_cache)
                    hu_tool_cache_destroy(agent->alloc, turn_cache);
                if (agent->turn_arena)
                    hu_arena_reset(agent->turn_arena);
                return HU_ERR_PROVIDER_RESPONSE;
            }
            err = HU_ERR_OUT_OF_MEMORY;
        }

        if (err != HU_OK) {
            {
                hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_ERR, .data = {{0}}};
                ev.data.err.component = "agent";
                ev.data.err.message = "provider chat failed";
                HU_OBS_SAFE_RECORD_EVENT(agent, &ev);
            }
            hu_chat_response_free(agent->alloc, &resp);
            memset(&resp, 0, sizeof(resp));
            hu_agent_clear_current_for_tools();
            if (dpo_rejected_resp)
                agent->alloc->free(agent->alloc->ctx, dpo_rejected_resp, dpo_rejected_resp_len + 1);
            if (system_prompt)
                agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
            if (plan_ctx)
                agent->alloc->free(agent->alloc->ctx, plan_ctx, plan_ctx_len + 1);
            if (routed_specs)
                agent->alloc->free(agent->alloc->ctx, routed_specs,
                                   routed_specs_count * sizeof(hu_tool_spec_t));
            if (turn_cache)
                hu_tool_cache_destroy(agent->alloc, turn_cache);
            if (agent->turn_arena)
                hu_arena_reset(agent->turn_arena);
            return err;
        }

        /* Chaos testing: optionally inject faults into LLM response */
        if (agent->sota.chaos_engine.config.enabled) {
            hu_chaos_fault_t fault = hu_chaos_maybe_inject(&agent->sota.chaos_engine);
            if (fault != HU_CHAOS_NONE) {
                hu_error_t chaos_err = hu_chaos_apply_to_response(&agent->sota.chaos_engine,
                                                                  agent->alloc, fault, &resp);
                if (chaos_err != HU_OK) {
                    hu_log_info("agent_turn", NULL, "chaos fault injected: %s",
                                hu_chaos_fault_name(fault));
                }
            }
        }

        agent->total_tokens += resp.usage.total_tokens;
        hu_agent_internal_record_cost(agent, &resp.usage);
        turn_tokens += resp.usage.total_tokens;

        /* Token budget: record spend and check session cap */
        if (agent->sota.token_budget.enabled) {
            hu_token_budget_record(&agent->sota.token_budget, resp.usage.total_tokens);
            if (!hu_token_budget_can_spend(&agent->sota.token_budget, 0)) {
                hu_log_info("agent_turn", NULL, "token budget exhausted for session");
            }
        }

        /* GVR (Generator-Verifier-Reviser): verify and optionally revise the response */
        if (agent->sota.gvr_config.enabled && resp.content && resp.content_len > 0 &&
            resp.tool_calls_count == 0) {
            const char *user_prompt = NULL;
            size_t user_prompt_len = 0;
            if (req.messages_count > 0) {
                user_prompt = req.messages[req.messages_count - 1].content;
                user_prompt_len = req.messages[req.messages_count - 1].content_len;
            }
            hu_gvr_pipeline_result_t gvr_result;
            hu_error_t gvr_err = hu_gvr_pipeline(
                agent->alloc, &agent->provider, &agent->sota.gvr_config, turn_model, turn_model_len,
                user_prompt, user_prompt_len, resp.content, resp.content_len, &gvr_result);
            if (gvr_err == HU_OK && gvr_result.final_content) {
                if (gvr_result.revisions_performed > 0) {
                    hu_chat_response_free(agent->alloc, &resp);
                    memset(&resp, 0, sizeof(resp));
                    resp.content = gvr_result.final_content;
                    resp.content_len = gvr_result.final_content_len;
                    gvr_result.final_content = NULL;
                }
                hu_gvr_pipeline_result_free(agent->alloc, &gvr_result);
            }
        }

        /* CoT audit: scan reasoning content for goal hijack / exfiltration signals */
        if (resp.reasoning_content && resp.reasoning_content_len > 0) {
            hu_cot_audit_result_t cot_result;
            memset(&cot_result, 0, sizeof(cot_result));
            if (hu_cot_audit(agent->alloc, resp.reasoning_content, resp.reasoning_content_len,
                             &cot_result) == HU_OK) {
                if (cot_result.verdict == HU_COT_BLOCKED) {
                    if (agent->audit_logger) {
                        hu_audit_event_t aev;
                        hu_audit_event_init(&aev, HU_AUDIT_POLICY_VIOLATION);
                        hu_audit_event_with_identity(
                            &aev, agent->agent_id,
                            agent->model_name ? agent->model_name : "unknown", NULL);
                        hu_audit_event_with_action(
                            &aev, "cot_audit", cot_result.reason ? cot_result.reason : "blocked",
                            false, false);
                        hu_audit_logger_log(agent->audit_logger, &aev);
                    }
                    hu_cot_audit_result_free(agent->alloc, &cot_result);
                    hu_chat_response_free(agent->alloc, &resp);
                    memset(&resp, 0, sizeof(resp));
                    resp.content =
                        hu_strndup(agent->alloc, "I need to reconsider my approach.", 33);
                    resp.content_len = resp.content ? 33 : 0;
                    continue;
                }
                hu_cot_audit_result_free(agent->alloc, &cot_result);
            }
        }

        /* Text-based tool call parsing for providers without native tool support */
        if (resp.tool_calls_count == 0 && resp.content && resp.content_len > 0 &&
            agent->provider.vtable->supports_native_tools &&
            !agent->provider.vtable->supports_native_tools(agent->provider.ctx)) {
            hu_tool_call_t *text_calls = NULL;
            size_t text_calls_count = 0;
            if (hu_text_tool_calls_parse(agent->alloc, resp.content, resp.content_len, &text_calls,
                                         &text_calls_count) == HU_OK &&
                text_calls_count > 0) {
                /* Replace resp tool calls with parsed text tool calls */
                resp.tool_calls = text_calls;
                resp.tool_calls_count = text_calls_count;
            }
        }

        /* Strip <tool_call>...</tool_call> from assistant text (including after text-call parse). */
        if (resp.content && resp.content_len > 0) {
            char *mut = (char *)resp.content;
            size_t mut_len = resp.content_len;
            agent_turn_strip_leaked_tool_calls_owned(agent->alloc, &mut, &mut_len);
            resp.content = mut;
            resp.content_len = mut_len;
        }

#ifndef HU_IS_TEST
        /* Native HuLa: model embeds <hula_program>...</hula_program> with no tool_calls */
        if (agent->hula_enabled && resp.tool_calls_count == 0 && resp.content &&
            resp.content_len > 0) {
            hu_hula_program_t hprog;
            memset(&hprog, 0, sizeof(hprog));
            if (hu_hula_extract_program_from_text(agent->alloc, resp.content, resp.content_len,
                                                  &hprog) == HU_OK &&
                hprog.root) {
                hu_hula_exec_t hx;
                memset(&hx, 0, sizeof(hx));
                hu_spawn_config_t hula_spawn_tpl;
                memset(&hula_spawn_tpl, 0, sizeof(hula_spawn_tpl));
                hu_error_t hxe =
                    hu_hula_exec_init_full(&hx, *agent->alloc, &hprog, agent->tools,
                                           agent->tools_count, agent->policy, agent->observer);
                if (hxe == HU_OK) {
                    agent_turn_hula_exec_bind_spawn(agent, &hx, &hula_spawn_tpl);
                    if (agent->infra.idempotency_registry) {
                        hu_hula_exec_set_idempotency_registry(&hx,
                                                              agent->infra.idempotency_registry);
                    }
                    hxe = hu_hula_exec_run(&hx);
                }
                if (hxe == HU_OK) {
                    size_t trl = 0;
                    const char *tr = hu_hula_exec_trace(&hx, &trl);
                    bool root_ok = false;
                    if (hprog.root && hprog.root->id)
                        root_ok = hu_hula_exec_result(&hx, hprog.root->id)->status == HU_HULA_DONE;
                    else
                        root_ok = true;
                    const char *tag_src = NULL;
                    size_t tag_src_len = 0;
                    (void)hu_hula_program_source_slice_from_text(resp.content, resp.content_len,
                                                                 &tag_src, &tag_src_len);
                    {
                        char *pj = NULL;
                        size_t pjl = 0;
                        if (hu_hula_to_json(agent->alloc, &hprog, &pj, &pjl) == HU_OK && pj) {
                            (void)hu_hula_trace_persist(agent->alloc, NULL, tr, trl, hprog.name,
                                                        hprog.name_len, root_ok, pj, pjl, tag_src,
                                                        tag_src_len);
                            hu_str_free(agent->alloc, pj);
                        } else {
                            (void)hu_hula_trace_persist(agent->alloc, NULL, tr, trl, hprog.name,
                                                        hprog.name_len, root_ok, NULL, 0, tag_src,
                                                        tag_src_len);
                        }
                    }
                    char *stripped = NULL;
                    size_t slen = 0;
                    const char *asst = resp.content;
                    size_t asst_len = resp.content_len;
                    if (hu_hula_strip_program_tags(agent->alloc, resp.content, resp.content_len,
                                                   &stripped, &slen) == HU_OK &&
                        stripped) {
                        asst = stripped;
                        asst_len = slen;
                    }
                    if (asst_len > 0) {
                        hu_error_t hist_err = hu_agent_internal_append_history(
                            agent, HU_ROLE_ASSISTANT, asst, asst_len, NULL, 0, NULL, 0);
                        if (hist_err != HU_OK)
                            hu_log_warn("agent", NULL, "failed to append assistant history: %s",
                                        hu_error_string(hist_err));
                    }
                    if (stripped)
                        agent->alloc->free(agent->alloc->ctx, stripped, slen + 1);
                    agent_turn_hula_append_histories(agent, &hprog, &hx);
                    hu_bth_metrics_record_hula_tool_turn(agent->bth_metrics);
                    hu_hula_exec_deinit(&hx);
                    hu_hula_program_deinit(&hprog);
                    hu_chat_response_free(agent->alloc, &resp);
                    iter++;
                    continue;
                }
                hu_hula_exec_deinit(&hx);
                hu_hula_program_deinit(&hprog);
            } else {
                hu_hula_program_deinit(&hprog);
            }
        }
#endif

        if (resp.tool_calls_count == 0) {
            uint64_t turn_duration_ms = hu_agent_internal_clock_diff_ms(turn_start, clock());
            {
                hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_AGENT_END, .data = {{0}}};
                ev.data.agent_end.duration_ms = turn_duration_ms;
                ev.data.agent_end.tokens_used = turn_tokens;
                HU_OBS_SAFE_RECORD_EVENT(agent, &ev);
            }
            {
                hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_TURN_COMPLETE, .data = {{0}}};
                HU_OBS_SAFE_RECORD_EVENT(agent, &ev);
            }
            if (resp.content && resp.content_len > 0) {
                /* PRM: score response reasoning chain for quality */
                if (agent->sota.sota_initialized && agent->sota.prm_config.enabled &&
                    resp.content_len > 100) {
                    hu_prm_result_t prm_res;
                    if (hu_prm_score_chain(agent->alloc, &agent->sota.prm_config, resp.content,
                                           resp.content_len, &prm_res) == HU_OK) {
                        prm_turn_score = prm_res.aggregate_score;
                        hu_prm_result_free(agent->alloc, &prm_res);
                    }
                }

                /* PRM per-step: score response quality as a reasoning step */
                if (agent->sota.sota_initialized && agent->sota.prm_config.enabled &&
                    resp.content_len > 50) {
                    double step_score = 0.0;
                    hu_prm_score_step(agent->alloc, &agent->sota.prm_config, resp.content,
                                      resp.content_len, msg, msg_len, &step_score);
                    prm_turn_score = step_score;
                }

                /* Reflection: evaluate response quality and retry if needed */
                hu_reflection_quality_t quality = hu_reflection_evaluate(
                    msg, msg_len, resp.content, resp.content_len, &agent->reflection);

                if (quality == HU_QUALITY_ACCEPTABLE && agent->reflection.use_llm &&
                    agent->reflection.enabled && reflection_retries_left > 0) {
                    quality =
                        hu_reflection_evaluate_llm(agent->alloc, &agent->provider, msg, msg_len,
                                                   resp.content, resp.content_len, quality);
                }

                /* PRM quality gate: low step-level score triggers retry */
                if (prm_turn_score > 0.0 && prm_turn_score < 0.3 &&
                    quality == HU_QUALITY_ACCEPTABLE && agent->reflection.enabled &&
                    reflection_retries_left > 0) {
                    quality = HU_QUALITY_NEEDS_RETRY;
                }

                if (quality == HU_QUALITY_NEEDS_RETRY && agent->reflection.enabled &&
                    reflection_retries_left > 0 && iter < agent->max_tool_iterations - 1) {
                    reflection_retries_left--;
                    char *critique = NULL;
                    size_t critique_len = 0;
                    hu_error_t cerr = hu_reflection_build_critique_prompt(
                        agent->alloc, msg, msg_len, resp.content, resp.content_len, &critique,
                        &critique_len);
                    if (cerr == HU_OK && critique) {
                        hu_error_t hist_err =
                            hu_agent_internal_append_history(agent, HU_ROLE_ASSISTANT, resp.content,
                                                             resp.content_len, NULL, 0, NULL, 0);
                        if (hist_err != HU_OK)
                            hu_log_error("agent_turn", NULL, "history append failed: %s",
                                         hu_error_string(hist_err));
                        hist_err = hu_agent_internal_append_history(agent, HU_ROLE_USER, critique,
                                                                    critique_len, NULL, 0, NULL, 0);
                        if (hist_err != HU_OK)
                            hu_log_error("agent_turn", NULL, "history append failed: %s",
                                         hu_error_string(hist_err));
                        agent->alloc->free(agent->alloc->ctx, critique, critique_len + 1);

                        /* DPO: save the rejected response for pairing with the
                         * chosen response after retry succeeds. */
                        if (agent->sota.sota_initialized && resp.content && resp.content_len > 0) {
                            if (dpo_rejected_resp)
                                agent->alloc->free(agent->alloc->ctx, dpo_rejected_resp,
                                                   dpo_rejected_resp_len + 1);
                            dpo_rejected_resp =
                                hu_strndup(agent->alloc, resp.content, resp.content_len);
                            dpo_rejected_resp_len = resp.content_len;
                            hu_dpo_record_from_feedback(&agent->sota.dpo_collector, msg, msg_len,
                                                        resp.content, resp.content_len, false);
                        }

                        hu_chat_response_free(agent->alloc, &resp);
                        iter++;
                        continue; /* retry with critique feedback */
                    }
                    if (critique)
                        agent->alloc->free(agent->alloc->ctx, critique, critique_len + 1);
                }

                /* DPO retry pair: record chosen vs rejected when reflection retry succeeded */
                if (agent->sota.sota_initialized && dpo_rejected_resp &&
                    dpo_rejected_resp_len > 0 && resp.content && resp.content_len > 0) {
                    hu_dpo_record_from_retry(&agent->sota.dpo_collector, msg, msg_len,
                                             dpo_rejected_resp, dpo_rejected_resp_len, resp.content,
                                             resp.content_len);
                    agent->alloc->free(agent->alloc->ctx, dpo_rejected_resp,
                                       dpo_rejected_resp_len + 1);
                    dpo_rejected_resp = NULL;
                    dpo_rejected_resp_len = 0;
                }

                /* A/B: when first response quality < 70 and channel history available,
                 * generate up to 2 more candidates and pick the best. */
                const char *final_content = resp.content;
                size_t final_len = resp.content_len;
                bool ab_owned = false;
                uint32_t max_chars = agent->max_response_chars ? agent->max_response_chars : 0;
                if (agent->ab_history_entries && agent->ab_history_count > 0) {
                    hu_quality_score_t q0 = hu_conversation_evaluate_quality(
                        resp.content, resp.content_len, agent->ab_history_entries,
                        agent->ab_history_count, max_chars);
                    if (q0.total < 70) {
                        hu_ab_result_t ab_result;
                        memset(&ab_result, 0, sizeof(ab_result));
                        ab_result.candidates[0].response =
                            hu_strndup(agent->alloc, resp.content, resp.content_len);
                        ab_result.candidates[0].response_len = resp.content_len;
                        ab_result.candidate_count = 1;

                        /* Generate 2nd candidate with alternative phrasing prompt */
                        static const char alt_suffix[] = "\n\nAlternative phrasing:";
                        size_t alt_len = system_prompt_len + sizeof(alt_suffix) - 1;
                        char *alt_system =
                            (char *)agent->alloc->alloc(agent->alloc->ctx, alt_len + 1);
                        if (alt_system) {
                            memcpy(alt_system, system_prompt, system_prompt_len);
                            memcpy(alt_system + system_prompt_len, alt_suffix,
                                   sizeof(alt_suffix) - 1);
                            alt_system[alt_len] = '\0';
                            size_t total = msgs_count;
                            hu_chat_message_t *alt_all = (hu_chat_message_t *)agent->alloc->alloc(
                                agent->alloc->ctx, total * sizeof(hu_chat_message_t));
                            if (alt_all) {
                                alt_all[0].role = HU_ROLE_SYSTEM;
                                alt_all[0].content = alt_system;
                                alt_all[0].content_len = alt_len;
                                alt_all[0].name = NULL;
                                alt_all[0].name_len = 0;
                                alt_all[0].tool_call_id = NULL;
                                alt_all[0].tool_call_id_len = 0;
                                alt_all[0].content_parts = NULL;
                                alt_all[0].content_parts_count = 0;
                                for (size_t i = 1; i < total; i++)
                                    alt_all[i] = msgs[i];
                                hu_chat_request_t alt_req = req;
                                alt_req.messages = alt_all;
                                alt_req.messages_count = total;

                                hu_chat_response_t alt_resp;
                                memset(&alt_resp, 0, sizeof(alt_resp));
                                hu_error_t alt_err = agent->provider.vtable->chat(
                                    agent->provider.ctx, agent->alloc, &alt_req, agent->model_name,
                                    agent->model_name_len, agent->temperature, &alt_resp);
                                if (alt_err == HU_OK && alt_resp.content &&
                                    alt_resp.content_len > 0) {
                                    agent->total_tokens += alt_resp.usage.total_tokens;
                                    turn_tokens += alt_resp.usage.total_tokens;
                                    ab_result.candidates[1].response = hu_strndup(
                                        agent->alloc, alt_resp.content, alt_resp.content_len);
                                    ab_result.candidates[1].response_len = alt_resp.content_len;
                                    ab_result.candidate_count = 2;
                                    hu_chat_response_free(agent->alloc, &alt_resp);

                                    hu_quality_score_t q1 = hu_conversation_evaluate_quality(
                                        ab_result.candidates[1].response,
                                        ab_result.candidates[1].response_len,
                                        agent->ab_history_entries, agent->ab_history_count,
                                        max_chars);
                                    if (q1.total < 70) {
                                        hu_chat_response_t alt2_resp;
                                        memset(&alt2_resp, 0, sizeof(alt2_resp));
                                        hu_error_t alt2_err = agent->provider.vtable->chat(
                                            agent->provider.ctx, agent->alloc, &alt_req,
                                            agent->model_name, agent->model_name_len,
                                            agent->temperature, &alt2_resp);
                                        if (alt2_err == HU_OK && alt2_resp.content &&
                                            alt2_resp.content_len > 0) {
                                            agent->total_tokens += alt2_resp.usage.total_tokens;
                                            turn_tokens += alt2_resp.usage.total_tokens;
                                            ab_result.candidates[2].response =
                                                hu_strndup(agent->alloc, alt2_resp.content,
                                                           alt2_resp.content_len);
                                            ab_result.candidates[2].response_len =
                                                alt2_resp.content_len;
                                            ab_result.candidate_count = 3;
                                        }
                                        hu_chat_response_free(agent->alloc, &alt2_resp);
                                    }
                                } else {
                                    hu_chat_response_free(agent->alloc, &alt_resp);
                                }
                                agent->alloc->free(agent->alloc->ctx, alt_all,
                                                   total * sizeof(hu_chat_message_t));
                            }
                            agent->alloc->free(agent->alloc->ctx, alt_system, alt_len + 1);
                        }

                        if (hu_ab_evaluate(agent->alloc, &ab_result, agent->ab_history_entries,
                                           agent->ab_history_count, max_chars) == HU_OK) {
                            if (agent->bth_metrics)
                                agent->bth_metrics->ab_evaluations++;
                            size_t bi = ab_result.best_idx;
                            if (bi < ab_result.candidate_count &&
                                ab_result.candidates[bi].response) {
                                if (bi != 0 && agent->bth_metrics)
                                    agent->bth_metrics->ab_alternates_chosen++;
#ifdef HU_ENABLE_SQLITE
                                if (agent->memory) {
                                    sqlite3 *ab_db = hu_sqlite_memory_get_db(agent->memory);
                                    if (ab_db)
                                        (void)hu_ab_record_selection(
                                            ab_db, bi, ab_result.candidates[bi].quality_score,
                                            ab_result.candidate_count, (int64_t)time(NULL));
                                }
#endif
                                final_content = ab_result.candidates[bi].response;
                                final_len = ab_result.candidates[bi].response_len;
                                ab_result.candidates[bi].response = NULL;
                                ab_result.candidates[bi].response_len = 0;
                                ab_owned = true;
                            }
                        }
                        hu_ab_result_deinit(&ab_result, agent->alloc);
                    }
                }

                /* Uncertainty: evaluate confidence and add hedge if needed */
                {
                    hu_uncertainty_signals_t u_signals;
                    memset(&u_signals, 0, sizeof(u_signals));
                    hu_uncertainty_extract_signals(final_content, final_len, msg, msg_len, 0, 0,
                                                   &u_signals);
                    hu_uncertainty_result_t u_result;
                    memset(&u_result, 0, sizeof(u_result));
                    if (hu_uncertainty_evaluate(agent->alloc, &u_signals, &u_result) == HU_OK) {
                        if (u_result.hedge_prefix && u_result.hedge_prefix_len > 0 &&
                            u_result.level >= HU_CONFIDENCE_MEDIUM) {
                            size_t new_len = u_result.hedge_prefix_len + final_len;
                            char *hedged =
                                (char *)agent->alloc->alloc(agent->alloc->ctx, new_len + 1);
                            if (hedged) {
                                memcpy(hedged, u_result.hedge_prefix, u_result.hedge_prefix_len);
                                memcpy(hedged + u_result.hedge_prefix_len, final_content,
                                       final_len);
                                hedged[new_len] = '\0';
                                if (ab_owned)
                                    agent->alloc->free(agent->alloc->ctx, (void *)final_content,
                                                       final_len + 1);
                                final_content = hedged;
                                final_len = new_len;
                                ab_owned = true;
                            }
                        }
                    }
                    hu_uncertainty_result_free(agent->alloc, &u_result);
                }

                /* Constitutional AI: critique response against principles */
#ifndef HU_IS_TEST
                if (agent->constitutional_enabled) {
                    hu_constitutional_config_t const_cfg = hu_constitutional_config_default();
                    hu_critique_result_t critique;
                    memset(&critique, 0, sizeof(critique));
                    if (hu_constitutional_critique(agent->alloc, &agent->provider,
                                                   agent->model_name, agent->model_name_len, msg,
                                                   msg_len, final_content, final_len, &const_cfg,
                                                   &critique) == HU_OK) {
                        if (critique.verdict == HU_CRITIQUE_REWRITE && critique.revised_response &&
                            critique.revised_response_len > 0) {
                            if (ab_owned)
                                agent->alloc->free(agent->alloc->ctx, (void *)final_content,
                                                   final_len + 1);
                            final_content = critique.revised_response;
                            final_len = critique.revised_response_len;
                            ab_owned = true;
                            critique.revised_response = NULL;
                        }
                    }
                    hu_critique_result_free(agent->alloc, &critique);
                }
#endif

                /* Metacognition: bounded re-entry (same provider) + SQLite history */
                if (agent->infra.metacognition.cfg.enabled) {
                    const char *work_content = final_content;
                    size_t work_len = final_len;
                    uint64_t work_in = (uint64_t)resp.usage.prompt_tokens;
                    uint64_t work_out = (uint64_t)resp.usage.completion_tokens;
                    bool hist_logprob_valid = resp.logprob_mean_valid;
                    float hist_logprob_mean = resp.logprob_mean_valid ? resp.logprob_mean : -1.0f;
                    hu_metacog_action_t last_mc_act = HU_METACOG_ACTION_NONE;
                    hu_metacognition_signal_t last_mc_sig;
                    memset(&last_mc_sig, 0, sizeof(last_mc_sig));

                    for (;;) {
                        const char *prev_resp = NULL;
                        size_t prev_resp_len = 0;
                        if (agent->history_count > 0) {
                            for (size_t hi = agent->history_count; hi > 0; hi--) {
                                size_t idx = hi - 1;
                                if (agent->history[idx].role == HU_ROLE_ASSISTANT &&
                                    agent->history[idx].content &&
                                    agent->history[idx].content_len > 0) {
                                    prev_resp = agent->history[idx].content;
                                    prev_resp_len = agent->history[idx].content_len;
                                    break;
                                }
                            }
                        }

                        hu_metacognition_signal_t mc_sig = hu_metacognition_monitor(
                            msg, msg_len, work_content, work_len, prev_resp, prev_resp_len,
                            agent->infra.emotional_cognition.confidence, work_in, work_out,
                            &agent->infra.metacognition);
                        hu_metacog_action_t mc_act =
                            hu_metacognition_plan_action(&agent->infra.metacognition, &mc_sig);
                        last_mc_sig = mc_sig;
                        last_mc_act = mc_act;

                        if (agent->bth_metrics &&
                            agent->infra.metacognition.last_suppressed_hysteresis)
                            agent->bth_metrics->metacog_hysteresis_suppressed++;

                        if (mc_act == HU_METACOG_ACTION_NONE)
                            break;

                        if (agent->observer) {
                            hu_observer_event_t mev = {.tag = HU_OBSERVER_EVENT_METACOG_ACTION};
                            mev.data.metacog_action.action = hu_metacog_action_name(mc_act);
                            mev.data.metacog_action.confidence = mc_sig.confidence;
                            mev.data.metacog_action.coherence = mc_sig.coherence;
                            HU_OBS_SAFE_RECORD_EVENT(agent, &mev);
                        }
                        if (agent->bth_metrics)
                            agent->bth_metrics->metacog_interventions++;

                        if (agent->infra.metacognition.regen_count >=
                            agent->infra.metacognition.cfg.max_regen)
                            break;

                        char mod[512];
                        size_t mod_len = 0;
                        if (hu_metacognition_apply(mc_act, mod, sizeof(mod), &mod_len) != HU_OK)
                            break;

                        static const char sep[] = "\n\n[METACOGNITION]\n";
                        size_t sep_len = sizeof(sep) - 1;
                        size_t new_sl = system_prompt_len + sep_len + mod_len;
                        char *new_sp = (char *)agent->alloc->alloc(agent->alloc->ctx, new_sl + 1);
                        if (!new_sp)
                            break;
                        memcpy(new_sp, system_prompt, system_prompt_len);
                        memcpy(new_sp + system_prompt_len, sep, sep_len);
                        memcpy(new_sp + system_prompt_len + sep_len, mod, mod_len);
                        new_sp[new_sl] = '\0';

                        size_t total = msgs_count;
                        hu_chat_message_t *mc_msgs = (hu_chat_message_t *)agent->alloc->alloc(
                            agent->alloc->ctx, total * sizeof(hu_chat_message_t));
                        if (!mc_msgs) {
                            agent->alloc->free(agent->alloc->ctx, new_sp, new_sl + 1);
                            break;
                        }
                        mc_msgs[0].role = HU_ROLE_SYSTEM;
                        mc_msgs[0].content = new_sp;
                        mc_msgs[0].content_len = new_sl;
                        mc_msgs[0].name = NULL;
                        mc_msgs[0].name_len = 0;
                        mc_msgs[0].tool_call_id = NULL;
                        mc_msgs[0].tool_call_id_len = 0;
                        mc_msgs[0].content_parts = NULL;
                        mc_msgs[0].content_parts_count = 0;
                        for (size_t mi = 1; mi < total; mi++)
                            mc_msgs[mi] = msgs[mi];

                        hu_chat_request_t mc_req = req;
                        mc_req.messages = mc_msgs;
                        mc_req.messages_count = total;

                        hu_chat_response_t mc_resp;
                        memset(&mc_resp, 0, sizeof(mc_resp));
                        hu_error_t mc_err = agent->provider.vtable->chat(
                            agent->provider.ctx, agent->alloc, &mc_req, turn_model, turn_model_len,
                            turn_temp, &mc_resp);

                        agent->alloc->free(agent->alloc->ctx, new_sp, new_sl + 1);
                        agent->alloc->free(agent->alloc->ctx, mc_msgs,
                                           total * sizeof(hu_chat_message_t));

                        if (mc_err != HU_OK || !mc_resp.content || mc_resp.content_len == 0) {
                            hu_chat_response_free(agent->alloc, &mc_resp);
                            break;
                        }

                        agent->total_tokens += mc_resp.usage.total_tokens;
                        hu_agent_internal_record_cost(agent, &mc_resp.usage);
                        turn_tokens += mc_resp.usage.total_tokens;
                        work_in = (uint64_t)mc_resp.usage.prompt_tokens;
                        work_out = (uint64_t)mc_resp.usage.completion_tokens;

                        if (mc_resp.logprob_mean_valid) {
                            hist_logprob_valid = true;
                            hist_logprob_mean = mc_resp.logprob_mean;
                        }

                        char *new_final =
                            hu_strndup(agent->alloc, mc_resp.content, mc_resp.content_len);
                        size_t new_final_len = new_final ? mc_resp.content_len : 0;
                        hu_chat_response_free(agent->alloc, &mc_resp);
                        if (!new_final)
                            break;
                        /* Metacog regen re-fetches the provider; apply the same `<tool_call>` strip
                         * as the primary `resp` path so leaked markup cannot reach the user. */
                        agent_turn_strip_leaked_tool_calls_owned(agent->alloc, &new_final,
                                                                 &new_final_len);
                        if (!new_final || new_final_len == 0)
                            break;

                        if (ab_owned)
                            agent->alloc->free(agent->alloc->ctx, (void *)final_content,
                                               final_len + 1);
                        final_content = new_final;
                        final_len = new_final_len;
                        ab_owned = true;
                        work_content = final_content;
                        work_len = final_len;
                        agent->infra.metacognition.regen_count++;
                        if (agent->bth_metrics)
                            agent->bth_metrics->metacog_regens++;
                    }

#ifdef HU_ENABLE_SQLITE
                    if (agent->infra.cognition_db) {
                        float risk =
                            hu_metacog_calibrated_risk(&agent->infra.metacognition, &last_mc_sig);
                        hu_metacog_history_extra_t mc_ex = {
                            .prompt_tokens = work_in,
                            .completion_tokens = work_out,
                            .logprob_mean = hist_logprob_valid ? hist_logprob_mean : -1.0f,
                            .risk_score = risk,
                        };
                        (void)hu_metacog_history_insert(
                            agent->infra.cognition_db, agent->trace_id, (int)iter,
                            last_mc_sig.confidence, last_mc_sig.coherence, last_mc_sig.repetition,
                            last_mc_sig.stuck_score, last_mc_sig.satisfaction_proxy,
                            last_mc_sig.trajectory_confidence, hu_metacog_action_name(last_mc_act),
                            hu_metacog_difficulty_name(agent->infra.metacognition.difficulty),
                            agent->infra.metacognition.regen_count > 0 ? 1 : 0, &mc_ex);
                        memcpy(agent->infra.metacognition.pending_outcome_trace_id, agent->trace_id,
                               sizeof(agent->trace_id));
                    }
#else
                    (void)hist_logprob_valid;
                    (void)hist_logprob_mean;
                    (void)last_mc_act;
#endif
                }

                /* Last-mile response guard — strips Harmony / ChatML special
                 * tokens and rejects degenerate model output (runaway
                 * repetition). Catches the class of failure that leaked
                 * `<|channel>thought` + a 200x quote loop to a real human
                 * contact via iMessage on 2026-05-10. */
                {
                    char *guard_out = NULL;
                    size_t guard_out_len = 0;
                    hu_guard_outcome_t guard_outcome = HU_GUARD_OK;
                    hu_guard_report_t guard_report;
                    memset(&guard_report, 0, sizeof(guard_report));
                    hu_error_t guard_err = hu_response_guard_check(
                        agent->alloc, final_content, final_len, &guard_out, &guard_out_len,
                        &guard_outcome, &guard_report);
                    if (guard_err == HU_OK) {
                        if (guard_outcome == HU_GUARD_REJECT) {
                            hu_log_error(
                                "agent_turn", agent->observer,
                                "response_guard REJECT: degenerate output (run=%zu, len=%zu) — "
                                "retrying once with repair prompt",
                                guard_report.max_repetition_run, final_len);
                            char *retry_content = NULL;
                            size_t retry_len = 0;
                            hu_guard_report_t retry_report;
                            memset(&retry_report, 0, sizeof(retry_report));
                            hu_error_t retry_err = hu_response_guard_retry_slim(
                                agent->alloc, agent->observer, agent->config, &agent->provider,
                                turn_model, turn_model_len, msg, msg_len, &retry_content,
                                &retry_len, &retry_report);
                            if (retry_err == HU_OK && retry_content && retry_len > 0) {
                                hu_log_warn(
                                    "agent_turn", agent->observer,
                                    "response_guard RECOVERED: retry passed (len=%zu, stripped=%zu)",
                                    retry_len, retry_report.bytes_stripped);
                                if (ab_owned)
                                    agent->alloc->free(agent->alloc->ctx, (void *)final_content,
                                                       final_len + 1);
                                final_content = retry_content;
                                final_len = retry_len;
                                ab_owned = true;
                            } else {
                                hu_log_error(
                                    "agent_turn", agent->observer,
                                    "response_guard retry failed (err=%s) — suppressing send",
                                    hu_error_string(retry_err));
                                if (ab_owned)
                                    agent->alloc->free(agent->alloc->ctx, (void *)final_content,
                                                       final_len + 1);
                                final_content = NULL;
                                final_len = 0;
                                ab_owned = false;
                            }
                        } else if (guard_outcome == HU_GUARD_REWROTE) {
                            hu_log_warn(
                                "agent_turn", agent->observer,
                                "response_guard REWROTE: stripped %zu bytes (harmony=%d think=%d)",
                                guard_report.bytes_stripped,
                                guard_report.stripped_harmony_tokens ? 1 : 0,
                                guard_report.stripped_thinking_block ? 1 : 0);
                            if (ab_owned)
                                agent->alloc->free(agent->alloc->ctx, (void *)final_content,
                                                   final_len + 1);
                            final_content = guard_out;
                            final_len = guard_out_len;
                            ab_owned = true; /* guard allocation is allocator-owned */
                        }
                    }
                }

                hu_error_t hist_err = hu_agent_internal_append_history(
                    agent, HU_ROLE_ASSISTANT, final_content ? final_content : "",
                    final_len, NULL, 0, NULL, 0);
                if (hist_err != HU_OK)
                    hu_log_error("agent_turn", NULL, "history append failed: %s",
                                 hu_error_string(hist_err));
                *response_out = final_content
                                    ? hu_strndup(agent->alloc, final_content, final_len)
                                    : hu_strndup(agent->alloc, "", 0);
                if (ab_owned && final_content)
                    agent->alloc->free(agent->alloc->ctx, (void *)final_content, final_len + 1);
                if (!*response_out) {
                    hu_agent_clear_current_for_tools();
                    hu_chat_response_free(agent->alloc, &resp);
                    if (dpo_rejected_resp)
                        agent->alloc->free(agent->alloc->ctx, dpo_rejected_resp,
                                           dpo_rejected_resp_len + 1);
                    if (system_prompt)
                        agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
                    if (plan_ctx)
                        agent->alloc->free(agent->alloc->ctx, plan_ctx, plan_ctx_len + 1);
                    if (routed_specs)
                        agent->alloc->free(agent->alloc->ctx, routed_specs,
                                           routed_specs_count * sizeof(hu_tool_spec_t));
                    if (turn_cache)
                        hu_tool_cache_destroy(agent->alloc, turn_cache);
                    if (agent->turn_arena)
                        hu_arena_reset(agent->turn_arena);
                    return HU_ERR_OUT_OF_MEMORY;
                }
                /* hu_strndup stops at first '\0' within final_len — length must match allocation.
                 */
                size_t response_effective_len = strlen(*response_out);
                if (response_len_out)
                    *response_len_out = response_effective_len;

                /* Response verifier (W4) — runs on the outbound draft before
                 * the response is cached, broadcast, or spoken. Default is
                 * TELEMETRY: extract + score claims so we get hallucination
                 * metrics without changing the user-visible reply. Opt in to
                 * SOFT (auto-hedge unsupported claims) or STRICT via the
                 * HU_VERIFY_MODE env var.
                 *
                 * Counters on the agent (verifier_runs / claims_total /
                 * claims_flagged) make this wire visible to tests and to ops
                 * dashboards without requiring a log-tap.
                 *
                 * verifier_graph may be NULL — the verifier still extracts
                 * claims (every claim becomes "unsupported"), useful as
                 * telemetry until production wires a real graph. */
                if (response_effective_len > 0) {
                    hu_verifier_config_t vcfg = hu_verifier_default_config();
                    vcfg.mode = HU_VERIFY_TELEMETRY;
                    const char *mode_env = getenv("HU_VERIFY_MODE");
                    if (mode_env) {
                        if (strcmp(mode_env, "soft") == 0)
                            vcfg.mode = HU_VERIFY_SOFT;
                        else if (strcmp(mode_env, "strict") == 0)
                            vcfg.mode = HU_VERIFY_STRICT;
                        else if (strcmp(mode_env, "off") == 0)
                            vcfg.mode = HU_VERIFY_OFF;
                    }
                    if (vcfg.mode != HU_VERIFY_OFF) {
                        const char *contact = (agent->contact_context &&
                                               agent->contact_context_len > 0)
                                                  ? agent->contact_context
                                                  : "";
                        size_t contact_len = contact[0] ? strlen(contact) : 0;
                        hu_verifier_report_t vreport;
                        memset(&vreport, 0, sizeof(vreport));
                        hu_memory_facade_t *verify_mem =
                            agent->w7_facade ? hu_w7_facade_memory_handle(agent->w7_facade) : NULL;
                        hu_error_t vrf_err = hu_response_verify(
                            agent->alloc, verify_mem, contact, contact_len,
                            *response_out, response_effective_len, &vcfg, &vreport);
                        if (vrf_err == HU_OK) {
                            agent->verifier_runs++;
                            agent->verifier_claims_total += vreport.claims_extracted;
                            agent->verifier_claims_flagged += vreport.claims_flagged;
                            if (vreport.claims_extracted > 0)
                                hu_log_info("agent_turn", NULL,
                                            "verifier: %zu claims (%zu supported, %zu flagged)",
                                            vreport.claims_extracted, vreport.claims_supported,
                                            vreport.claims_flagged);
                            /* SOFT mode: replace draft with the hedged version
                             * when the verifier produced one. */
                            if (vreport.draft_modified && vcfg.mode == HU_VERIFY_SOFT) {
                                size_t mod_len = strlen(vreport.modified_draft);
                                if (mod_len > 0) {
                                    char *replacement =
                                        hu_strndup(agent->alloc, vreport.modified_draft, mod_len);
                                    if (replacement) {
                                        agent->alloc->free(agent->alloc->ctx, *response_out,
                                                           response_effective_len + 1);
                                        *response_out = replacement;
                                        if (response_len_out)
                                            *response_len_out = mod_len;
                                        response_effective_len = mod_len;
                                    }
                                }
                            }
                        }
                    }

                    /* W11 self-RAG (FIX 12b). Runs alongside the heuristic
                     * verifier above, surfacing structured atomic claims +
                     * an explicit ABSTAINED outcome. Telemetry-only by
                     * default; opt-in to soft/strict via HU_SELF_RAG_MODE
                     * (mirrors HU_VERIFY_MODE). Skipped when the W7 facade
                     * isn't open (no production memory backend). */
                    if (agent->w7_facade && agent->memory_session_id &&
                        agent->memory_session_id_len > 0) {
                        int srag_mode = HU_VERIFY_TELEMETRY;
                        const char *srag_env = getenv("HU_SELF_RAG_MODE");
                        if (srag_env) {
                            if (strcmp(srag_env, "off") == 0)
                                srag_mode = HU_VERIFY_OFF;
                            else if (strcmp(srag_env, "soft") == 0)
                                srag_mode = HU_VERIFY_SOFT;
                            else if (strcmp(srag_env, "strict") == 0)
                                srag_mode = HU_VERIFY_STRICT;
                        }
                        if (srag_mode != HU_VERIFY_OFF) {
                            hu_w11_outcome_t s_outcome = HU_W11_OUTCOME_SUPPORTED;
                            size_t s_total = 0, s_flagged = 0;
                            char *s_modified = NULL;
                            size_t s_modified_len = 0;
                            char **mod_ptr = (srag_mode == HU_VERIFY_SOFT ||
                                              srag_mode == HU_VERIFY_STRICT)
                                                 ? &s_modified
                                                 : NULL;
                            size_t *mod_len_ptr = mod_ptr ? &s_modified_len : NULL;
                            hu_error_t serr = hu_w11_self_rag_verify(
                                agent->w7_facade, agent->alloc, agent->memory_session_id,
                                agent->memory_session_id_len, *response_out,
                                response_effective_len, srag_mode, 0, &s_outcome, &s_total,
                                &s_flagged, mod_ptr, mod_len_ptr);
                            if (serr == HU_OK) {
                                agent->self_rag_runs++;
                                agent->self_rag_claims_total += s_total;
                                agent->self_rag_claims_flagged += s_flagged;
                                if (s_outcome == HU_W11_OUTCOME_ABSTAINED)
                                    agent->self_rag_abstentions++;
                                /* Replace the user-visible response when
                                 * the verifier produced one. Covers
                                 * HEDGED / REWRITTEN drafts AND ABSTAIN
                                 * refusals (bridge writes the refusal
                                 * template into s_modified when outcome
                                 * is ABSTAINED). */
                                if (s_modified && s_modified_len > 0 &&
                                    (srag_mode == HU_VERIFY_SOFT ||
                                     srag_mode == HU_VERIFY_STRICT)) {
                                    agent->alloc->free(agent->alloc->ctx, *response_out,
                                                       response_effective_len + 1);
                                    *response_out = s_modified;
                                    if (response_len_out)
                                        *response_len_out = s_modified_len;
                                    response_effective_len = s_modified_len;
                                    s_modified = NULL; /* ownership transferred */
                                    if (s_outcome == HU_W11_OUTCOME_ABSTAINED)
                                        agent->self_rag_refusals_rendered++;
                                }
                                if (s_modified) {
                                    agent->alloc->free(agent->alloc->ctx, s_modified,
                                                       s_modified_len + 1);
                                }
                            }
                        }
                    }
                }

                /* Store in semantic response cache for future lookups */
                if (agent->infra.response_cache && final_len > 0) {
                    const char *mname = agent->model_name ? agent->model_name : "";
                    size_t mname_len = agent->model_name ? agent->model_name_len : 0;
                    hu_semantic_cache_put(agent->infra.response_cache, agent->alloc, msg, msg_len,
                                          mname, mname_len, final_content, final_len, 0, msg,
                                          msg_len);
                }

                /* Speculative: predict follow-ups and pre-cache them */
                if (agent->infra.speculative_cache && *response_out) {
                    char *preds[HU_SPEC_MAX_PREDICTIONS];
                    size_t pred_lens[HU_SPEC_MAX_PREDICTIONS];
                    double confs[HU_SPEC_MAX_PREDICTIONS];
                    size_t pred_count = 0;
                    memset(preds, 0, sizeof(preds));
                    int64_t now_spec = (int64_t)time(NULL);
                    if (hu_speculative_predict(agent->alloc, msg, msg_len, *response_out,
                                               response_effective_len, preds, pred_lens, confs,
                                               HU_SPEC_MAX_PREDICTIONS, &pred_count) == HU_OK) {
                        hu_speculative_cache_evict(agent->infra.speculative_cache, now_spec, 300);
                        for (size_t pi = 0; pi < pred_count; pi++) {
                            if (preds[pi] && pred_lens[pi] > 0 && confs[pi] >= 0.5) {
                                hu_speculative_cache_store(agent->infra.speculative_cache,
                                                           preds[pi], pred_lens[pi], *response_out,
                                                           final_len, confs[pi], now_spec);
                            }
                            if (preds[pi])
                                agent->alloc->free(agent->alloc->ctx, preds[pi], pred_lens[pi] + 1);
                        }
                    }
                }

                /* Context engine: notify after turn for bookkeeping/indexing */
                if (agent->infra.context_engine) {
                    hu_context_engine_t *ce = (hu_context_engine_t *)agent->infra.context_engine;
                    if (ce->vtable && ce->vtable->after_turn) {
                        hu_context_message_t ce_user = {.role = "user",
                                                        .role_len = 4,
                                                        .content = msg,
                                                        .content_len = msg_len,
                                                        .timestamp = (int64_t)time(NULL)};
                        hu_context_message_t ce_asst = {.role = "assistant",
                                                        .role_len = 9,
                                                        .content = *response_out,
                                                        .content_len = response_effective_len,
                                                        .timestamp = (int64_t)time(NULL)};
                        ce->vtable->after_turn(ce->ctx, agent->alloc, &ce_user, &ce_asst);
                    }
                    if (ce->vtable && ce->vtable->ingest) {
                        hu_context_message_t ce_asst = {.role = "assistant",
                                                        .role_len = 9,
                                                        .content = *response_out,
                                                        .content_len = response_effective_len,
                                                        .timestamp = (int64_t)time(NULL)};
                        ce->vtable->ingest(ce->ctx, agent->alloc, &ce_asst);
                    }
                }

                { hu_agent_internal_maybe_tts(agent, *response_out, response_effective_len); }
            }
            hu_chat_response_free(agent->alloc, &resp);
            hu_agent_clear_current_for_tools();
            hu_relationship_update(&agent->relationship, 1);
            hu_agent_update_voice_profile(agent, msg, msg_len);
            if (acp_context)
                agent->alloc->free(agent->alloc->ctx, acp_context, acp_context_len + 1);

            /* Deep extraction: lightweight pattern-based fact extraction from user message */
            if (agent->memory && agent->memory->vtable && agent->memory->vtable->store) {
                hu_deep_extract_result_t de_result;
                memset(&de_result, 0, sizeof(de_result));
                if (hu_deep_extract_lightweight(agent->alloc, msg, msg_len, &de_result) == HU_OK &&
                    de_result.fact_count > 0) {
                    static const char facts_cat[] = "facts";
                    hu_memory_category_t cat = {
                        .tag = HU_MEMORY_CATEGORY_CUSTOM,
                        .data.custom = {.name = facts_cat, .name_len = sizeof(facts_cat) - 1},
                    };
                    const char *sid = agent->memory->current_session_id;
                    size_t sid_len = sid ? agent->memory->current_session_id_len : 0;
                    for (size_t fi = 0; fi < de_result.fact_count; fi++) {
                        const hu_extracted_fact_t *f = &de_result.facts[fi];
                        if (!f->subject || !f->predicate || !f->object)
                            continue;
                        size_t key_len =
                            strlen(f->subject) + 1 + strlen(f->predicate) + 1 + strlen(f->object);
                        char key_buf[256];
                        if (key_len < sizeof(key_buf)) {
                            int n = snprintf(key_buf, sizeof(key_buf), "%s:%s:%s", f->subject,
                                             f->predicate, f->object);
                            if (n > 0 && (size_t)n < sizeof(key_buf)) {
                                /* Memory policy: let heuristic decide if we should store */
                                hu_mem_action_type_t mem_action = HU_MEM_STORE;
                                if (agent->sota.mem_policy.enabled) {
                                    hu_mem_state_t mstate = {0};
                                    mem_action =
                                        hu_mem_policy_decide(&agent->sota.mem_policy, &mstate,
                                                             f->object, strlen(f->object));
                                }
                                if (mem_action == HU_MEM_STORE || mem_action == HU_MEM_UPDATE) {
                                    hu_error_t store_err = agent->memory->vtable->store(
                                        agent->memory->ctx, key_buf, (size_t)n, f->object,
                                        strlen(f->object), &cat, sid ? sid : "", sid_len);
                                    if (store_err != HU_OK && store_err != HU_ERR_NOT_SUPPORTED)
                                        hu_log_error("agent", NULL, "memory store failed: %s",
                                                     hu_error_string(store_err));
                                }
                                if (agent->sota.sota_initialized) {
                                    hu_memory_tier_t assigned;
                                    hu_tier_manager_auto_tier(&agent->sota.tier_manager, key_buf,
                                                              (size_t)n, f->object,
                                                              strlen(f->object), &assigned);
                                }
                            }
                        }
                    }
                }
                hu_deep_extract_result_deinit(&de_result, agent->alloc);
            }
            /* Track agent's own commitments ("I'll check on that", "let me look into it") */
            if (agent->commitment_store && *response_out && *response_len_out > 0) {
                hu_commitment_detect_result_t cr;
                memset(&cr, 0, sizeof(cr));
                hu_error_t cerr = hu_commitment_detect(agent->alloc, *response_out,
                                                       *response_len_out, "assistant", 9, &cr);
                if (cerr == HU_OK && cr.count > 0) {
                    const char *sess = agent->memory_session_id;
                    size_t sess_len = sess ? agent->memory_session_id_len : 0;
                    for (size_t ci = 0; ci < cr.count; ci++) {
                        hu_error_t cs_err = hu_commitment_store_save(
                            agent->commitment_store, &cr.commitments[ci], sess, sess_len);
                        if (cs_err != HU_OK)
                            hu_log_error("agent", NULL, "commitment save failed: %s",
                                         hu_error_string(cs_err));
                    }
                }
                hu_commitment_detect_result_deinit(&cr, agent->alloc);
            }
#ifdef HU_ENABLE_SQLITE
            /* Record this turn as experience for future recall (with SQLite persistence) */
            if (agent->memory) {
                hu_experience_store_t exp_store;
                if (hu_experience_store_init(agent->alloc, agent->memory, &exp_store) == HU_OK) {
                    sqlite3 *rec_db = hu_sqlite_memory_get_db(agent->memory);
                    if (rec_db)
                        exp_store.db = rec_db;
                    const char *resp_text = *response_out ? *response_out : "";
                    size_t resp_len = response_len_out ? *response_len_out : 0;
                    {
                        hu_error_t exp_err = hu_experience_record(
                            &exp_store, msg, msg_len, "agent_turn", 10, resp_text, resp_len, 1.0);
                        if (exp_err != HU_OK)
                            hu_log_error("agent", NULL, "experience record failed: %s",
                                         hu_error_string(exp_err));
                    }
                    hu_experience_store_deinit(&exp_store);
                }
            }
            /* W10: Record reasoning trace when the turn involved memory retrieval */
            if (agent->w7_facade && memory_ctx_len > 0 && msg_len > 0) {
                hu_memory_facade_t *nf = hu_w7_facade_memory_handle(agent->w7_facade);
                if (nf) {
                    hu_reasoning_trace_t rt;
                    memset(&rt, 0, sizeof(rt));
                    size_t vlen = msg_len < sizeof(rt.goal_verb) - 1
                                      ? msg_len
                                      : sizeof(rt.goal_verb) - 1;
                    memcpy(rt.goal_verb, msg, vlen);
                    rt.goal_verb[vlen] = '\0';
                    const char *rt_cid =
                        agent->memory_session_id ? agent->memory_session_id : "";
                    size_t rt_cid_len =
                        agent->memory_session_id ? agent->memory_session_id_len : 0;
                    int64_t trace_id = 0;
                    (void)hu_reasoning_trace_record(nf, rt_cid, rt_cid_len, &rt, &trace_id);
                }
            }
            /* W10: Persist multimodal blobs from this turn's history entries */
            if (agent->w7_facade && agent->history_count > 0) {
                hu_memory_facade_t *blob_mf = hu_w7_facade_memory_handle(agent->w7_facade);
                if (blob_mf) {
                    const char *blob_cid =
                        agent->memory_session_id ? agent->memory_session_id : "";
                    size_t blob_cid_len =
                        agent->memory_session_id ? agent->memory_session_id_len : 0;
                    for (size_t hi = agent->history_count; hi > 0 && hi > agent->history_count - 4;
                         hi--) {
                        const hu_owned_message_t *hm = &agent->history[hi - 1];
                        if (!hm->content_parts || hm->content_parts_count == 0)
                            continue;
                        for (size_t pi = 0; pi < hm->content_parts_count; pi++) {
                            const hu_content_part_t *cp = &hm->content_parts[pi];
                            hu_memory_blob_t blob;
                            memset(&blob, 0, sizeof(blob));
                            if (cp->tag == HU_CONTENT_PART_IMAGE_BASE64 &&
                                cp->data.image_base64.data &&
                                cp->data.image_base64.data_len > 0) {
                                blob.bytes = (void *)cp->data.image_base64.data;
                                blob.bytes_len = cp->data.image_base64.data_len;
                                if (cp->data.image_base64.media_type)
                                    strncpy(blob.mime_type, cp->data.image_base64.media_type,
                                            sizeof(blob.mime_type) - 1);
                                else
                                    strncpy(blob.mime_type, "image/png",
                                            sizeof(blob.mime_type) - 1);
                            } else if (cp->tag == HU_CONTENT_PART_AUDIO_BASE64 &&
                                       cp->data.audio_base64.data &&
                                       cp->data.audio_base64.data_len > 0) {
                                blob.bytes = (void *)cp->data.audio_base64.data;
                                blob.bytes_len = cp->data.audio_base64.data_len;
                                if (cp->data.audio_base64.media_type)
                                    strncpy(blob.mime_type, cp->data.audio_base64.media_type,
                                            sizeof(blob.mime_type) - 1);
                                else
                                    strncpy(blob.mime_type, "audio/wav",
                                            sizeof(blob.mime_type) - 1);
                            } else {
                                continue;
                            }
                            int64_t blob_id = 0;
                            (void)hu_memory_blob_put(blob_mf, blob_cid, blob_cid_len,
                                                     &blob, &blob_id);
                        }
                        break;
                    }
                }
            }
            /* Value learning: detect approval, correction, re-asks, content-specific signals */
            if (agent->memory) {
                sqlite3 *vl_db = hu_sqlite_memory_get_db(agent->memory);
                if (vl_db) {
                    hu_value_engine_t ve;
                    if (hu_value_engine_create(agent->alloc, vl_db, &ve) == HU_OK) {
                        int64_t now_vl = (int64_t)time(NULL);

                        bool is_positive =
                            (msg_len >= 4 && (strstr(msg, "good") || strstr(msg, "great") ||
                                              strstr(msg, "thanks") || strstr(msg, "perfect") ||
                                              strstr(msg, "exactly")));
                        bool is_negative =
                            (msg_len >= 2 && (strstr(msg, "wrong") || strstr(msg, "bad") ||
                                              strstr(msg, "incorrect") || strstr(msg, "no,")));
                        bool is_value_correction =
                            (msg_len >= 6 && (strstr(msg, "actually") || strstr(msg, "I meant") ||
                                              strstr(msg, "not what") || strstr(msg, "try again")));

                        /* Re-ask detection: user repeats a similar message = previous answer was
                         * wrong */
                        bool is_reask = false;
                        if (agent->history_count >= 4 && msg_len > 10) {
                            for (size_t hi = agent->history_count - 2;
                                 hi > 0 && hi > agent->history_count - 8; hi--) {
                                if (agent->history[hi].role == HU_ROLE_USER &&
                                    agent->history[hi].content_len > 10) {
                                    size_t min_l = msg_len < agent->history[hi].content_len
                                                       ? msg_len
                                                       : agent->history[hi].content_len;
                                    size_t match = 0;
                                    for (size_t ci = 0; ci < min_l && ci < 100; ci++) {
                                        if (msg[ci] == agent->history[hi].content[ci])
                                            match++;
                                    }
                                    if (min_l > 0 && match * 100 / min_l > 70) {
                                        is_reask = true;
                                        break;
                                    }
                                }
                            }
                        }

                        if (is_positive)
                            (void)hu_value_learn_from_approval(&ve, "helpfulness", 11, 0.15,
                                                               now_vl);
                        if (is_negative || is_reask)
                            (void)hu_value_weaken(&ve, "helpfulness", 11, 0.15, now_vl);
                        if (is_value_correction)
                            (void)hu_value_learn_from_correction(
                                &ve, "accuracy", 8, "User corrected a factual or intent error", 42,
                                0.3, now_vl);
                        if (strstr(msg, "privacy") || strstr(msg, "private"))
                            (void)hu_value_learn_from_approval(&ve, "privacy", 7, 0.2, now_vl);
                        if (strstr(msg, "brief") || strstr(msg, "concise") ||
                            strstr(msg, "shorter"))
                            (void)hu_value_learn_from_correction(&ve, "conciseness", 11,
                                                                 "User prefers shorter responses",
                                                                 30, 0.2, now_vl);
                        if (strstr(msg, "detail") || strstr(msg, "elaborate") ||
                            strstr(msg, "more"))
                            (void)hu_value_learn_from_correction(&ve, "thoroughness", 12,
                                                                 "User prefers detailed responses",
                                                                 31, 0.2, now_vl);
                        /* DPO: record user feedback as preference signal */
                        if (agent->sota.sota_initialized && agent->history_count >= 2) {
                            const char *last_resp =
                                agent->history[agent->history_count - 2].content;
                            size_t last_resp_len =
                                agent->history[agent->history_count - 2].content_len;
                            if (last_resp && last_resp_len > 0 && (is_positive || is_negative))
                                hu_dpo_record_from_feedback(&agent->sota.dpo_collector, msg,
                                                            msg_len, last_resp, last_resp_len,
                                                            is_positive);
                        }

                        hu_value_engine_deinit(&ve);
                    }
                }
            }
#endif
            /* Adaptive RAG: record retrieval quality for learning */
            if (agent->sota.sota_initialized && rag_strategy_used != HU_RAG_NONE &&
                memory_ctx_len > 0)
                hu_adaptive_rag_record_outcome(&agent->sota.adaptive_rag, rag_strategy_used,
                                               memory_ctx_len > 100 ? 0.8 : 0.4);

            /* Style learning: adaptive schedule — early sessions learn faster,
             * then settle into a steady cadence. Also triggers on corrections. */
            {
                bool should_reanalyze = false;
                if (agent->persona_name && agent->persona_name_len > 0 && agent->memory) {
                    if (agent->history_count <= 20 && agent->history_count % 10 == 0 &&
                        agent->history_count > 0)
                        should_reanalyze = true;
                    else if (agent->history_count > 20 && agent->history_count <= 100 &&
                             agent->history_count % 25 == 0)
                        should_reanalyze = true;
                    else if (agent->history_count > 100 && agent->history_count % 50 == 0)
                        should_reanalyze = true;
                }
                if (should_reanalyze) {
                    const char *ch = agent->active_channel ? agent->active_channel : "cli";
                    size_t ch_len = agent->active_channel_len ? agent->active_channel_len : 3;
                    const char *cid = agent->memory_session_id ? agent->memory_session_id : "";
                    size_t cid_len =
                        agent->memory_session_id_len ? agent->memory_session_id_len : 0;
                    (void)hu_persona_style_reanalyze(
                        agent->alloc, &agent->provider, agent->model_name, agent->model_name_len,
                        agent->memory, agent->persona_name, agent->persona_name_len, ch, ch_len,
                        cid, cid_len);
                }
            }
#ifdef HU_ENABLE_SQLITE
            /* Goal progress: advance active goals based on turn output */
            if (agent->memory) {
                sqlite3 *goal_db = hu_sqlite_memory_get_db(agent->memory);
                if (goal_db) {
                    hu_goal_engine_t ge;
                    if (hu_goal_engine_create(agent->alloc, goal_db, &ge) == HU_OK) {
                        hu_goal_t active_goal;
                        bool gfound = false;
                        if (hu_goal_select_next(&ge, &active_goal, &gfound) == HU_OK && gfound) {
                            double pdelta = (turn_tool_results_count > 0)
                                                ? 0.15 + 0.05 * (double)turn_tool_results_count
                                                : 0.1;
                            double nprog = active_goal.progress + pdelta;
                            if (nprog > 1.0)
                                nprog = 1.0;
                            (void)hu_goal_update_progress(&ge, active_goal.id, nprog,
                                                          (int64_t)time(NULL));
                        }
                        hu_goal_engine_deinit(&ge);
                    }
                }
            }

            /* Tier management: promote frequently-accessed memories, periodic consolidation */
            if (agent->sota.sota_initialized && agent->memory) {
                if (memory_ctx && memory_ctx_len > 20) {
                    hu_tier_manager_promote(&agent->sota.tier_manager, memory_ctx,
                                            memory_ctx_len < 128 ? memory_ctx_len : 128,
                                            HU_TIER_ARCHIVAL, HU_TIER_RECALL);
                }

                /* Periodic consolidation: every 10 turns, plus debounce-based
                 * supplemental consolidation (EdgeClaw-inspired). The debounce
                 * tracker allows topic-switch triggers from daemon.c to also
                 * feed into this path. */
                {
                    static hu_consolidation_debounce_t agent_turn_debounce;
                    static bool agent_turn_debounce_inited = false;
                    if (!agent_turn_debounce_inited) {
                        hu_consolidation_debounce_init(&agent_turn_debounce);
                        agent_turn_debounce_inited = true;
                    }
                    hu_consolidation_debounce_tick(&agent_turn_debounce);

                    if (hu_consolidation_get_and_clear_topic_switch())
                        hu_consolidation_debounce_inject(&agent_turn_debounce,
                                                         HU_CONSOLIDATION_MIN_ENTRIES);

                    bool should_consolidate = (agent->history_count % 10 == 0);
                    if (!should_consolidate) {
                        int64_t tc_now = (int64_t)time(NULL);
                        should_consolidate =
                            hu_consolidation_should_run(&agent_turn_debounce, tc_now);
                        if (should_consolidate && hu_agent_consolidate_memory(agent) == HU_OK) {
                            hu_consolidation_debounce_reset(&agent_turn_debounce, tc_now);
                        } else if (should_consolidate) {
                            should_consolidate = false;
                        }
                    }
                    if (should_consolidate && agent->history_count % 10 == 0)
                        hu_agent_consolidate_memory(agent);
                }
            }

            /* Self-eval feedback: score this turn's output and feed into
             * the self-improvement pipeline for continuous learning */
            if (agent->memory && agent->history_count > 0 &&
                agent->history[agent->history_count - 1].role == HU_ROLE_ASSISTANT) {
                sqlite3 *eval_db = hu_sqlite_memory_get_db(agent->memory);
                if (eval_db) {
                    const char *resp_text = agent->history[agent->history_count - 1].content;
                    size_t resp_len2 = agent->history[agent->history_count - 1].content_len;
                    hu_reflection_quality_t self_q = hu_reflection_evaluate(
                        msg, msg_len, resp_text, resp_len2, &agent->reflection);
                    hu_self_improve_t si;
                    if (hu_self_improve_create(agent->alloc, eval_db, &si) == HU_OK) {
                        hu_self_improve_init_tables(&si);
                        bool quality_good = (self_q == HU_QUALITY_GOOD);
                        hu_self_improve_record_tool_outcome(&si, "agent_turn", 10, quality_good,
                                                            (int64_t)time(NULL));
                        /* Always apply reflections, not just when tools were used */
                        (void)hu_self_improve_apply_reflections(&si, (int64_t)time(NULL));
                        hu_self_improve_deinit(&si);
                    }
                }
            }

            /* Record per-contact style evolution data from last assistant message */
            if (agent->memory && agent->memory_session_id && agent->memory_session_id_len > 0 &&
                agent->history_count > 0 &&
                agent->history[agent->history_count - 1].role == HU_ROLE_ASSISTANT) {
                const char *resp_text = agent->history[agent->history_count - 1].content;
                size_t resp_len = agent->history[agent->history_count - 1].content_len;
                if (resp_text && resp_len > 0) {
                    bool has_emoji = false;
                    bool has_question = false;
                    double formality = 0.5;
                    for (size_t si = 0; si < resp_len; si++) {
                        if (resp_text[si] == '?')
                            has_question = true;
                        if ((unsigned char)resp_text[si] > 127)
                            has_emoji = true;
                    }
                    size_t upper = 0, alpha = 0;
                    for (size_t si = 0; si < resp_len && si < 500; si++) {
                        if (resp_text[si] >= 'A' && resp_text[si] <= 'Z') {
                            upper++;
                            alpha++;
                        } else if (resp_text[si] >= 'a' && resp_text[si] <= 'z')
                            alpha++;
                    }
                    if (alpha > 10)
                        formality = (double)upper / (double)alpha;
                    (void)hu_superhuman_style_record(agent->memory, agent->memory_session_id,
                                                     agent->memory_session_id_len, resp_len,
                                                     formality, has_emoji, has_question);
                }
            }

            /* Cognition DB: evolving skill exposure/outcomes + episodic pattern extraction */
            if (agent->infra.cognition_db && agent->history_count > 0 &&
                agent->history[agent->history_count - 1].role == HU_ROLE_ASSISTANT) {
                const char *resp_txt = agent->history[agent->history_count - 1].content;
                size_t resp_txt_len = agent->history[agent->history_count - 1].content_len;
                if (resp_txt && resp_txt_len > 0) {
                    hu_reflection_quality_t cq = hu_reflection_evaluate(
                        msg, msg_len, resp_txt, resp_txt_len, &agent->reflection);
                    int sk_outcome = HU_SKILL_OUTCOME_NEUTRAL;
                    if (cq == HU_QUALITY_GOOD)
                        sk_outcome = HU_SKILL_OUTCOME_POSITIVE;
                    else if (cq == HU_QUALITY_NEEDS_RETRY)
                        sk_outcome = HU_SKILL_OUTCOME_NEGATIVE;
                    if (msg && msg_len >= 6) {
                        static const char *neg_phrases[] = {"that's wrong",     "incorrect",
                                                            "not what i meant", "you misunderstood",
                                                            "that's not right", NULL};
                        char low[512];
                        size_t cap = msg_len < sizeof(low) - 1 ? msg_len : sizeof(low) - 1;
                        for (size_t i = 0; i < cap; i++)
                            low[i] = (char)tolower((unsigned char)msg[i]);
                        low[cap] = '\0';
                        for (size_t pi = 0; neg_phrases[pi]; pi++) {
                            if (strstr(low, neg_phrases[pi])) {
                                sk_outcome = HU_SKILL_OUTCOME_NEGATIVE;
                                break;
                            }
                        }
                    }
                    const char *cid = agent->memory_session_id;
                    size_t cid_len = agent->memory_session_id ? agent->memory_session_id_len : 0;
                    const char *sid = cid;
                    size_t sid_len = cid_len;
                    static const char cat_name[] = "skillforge_catalog";
                    if (cognition_skills_shown) {
                        hu_skill_invocation_t inv = {
                            .skill_name = cat_name,
                            .skill_name_len = sizeof(cat_name) - 1,
                            .contact_id = cid,
                            .contact_id_len = cid_len,
                            .session_id = sid,
                            .session_id_len = sid_len,
                            .explicit_run = false,
                            .outcome = HU_SKILL_OUTCOME_NEUTRAL,
                        };
                        (void)hu_evolving_record_invocation(agent->infra.cognition_db, &inv);
                    }
                    (void)hu_evolving_collect_outcome(agent->infra.cognition_db, cid, cid_len, sid,
                                                      sid_len, sk_outcome);
                    if (agent->bth_metrics)
                        agent->bth_metrics->evolving_outcomes++;

                    const char *tool_row[1];
                    size_t ep_tool_n = 0;
                    if (turn_tool_results_count > 0) {
                        static const char tt[] = "tool_execution";
                        tool_row[0] = tt;
                        ep_tool_n = 1;
                    } else if (cognition_skills_shown) {
                        tool_row[0] = cat_name;
                        ep_tool_n = 1;
                    }
                    if (ep_tool_n > 0) {
                        hu_episodic_session_summary_t esum = {
                            .session_id = sid,
                            .session_id_len = sid_len,
                            .tool_names = tool_row,
                            .tool_count = ep_tool_n,
                            .skill_names = NULL,
                            .skill_count = 0,
                            .had_positive_feedback = (cq == HU_QUALITY_GOOD),
                            .had_correction = (sk_outcome == HU_SKILL_OUTCOME_NEGATIVE),
                            .topic = msg,
                            .topic_len = msg_len > 256 ? 256 : msg_len,
                        };
                        if (hu_episodic_extract_and_store(agent->infra.cognition_db, agent->alloc,
                                                          &esum) == HU_OK &&
                            agent->bth_metrics)
                            agent->bth_metrics->episodic_patterns_stored++;
                    }
                }
            }
#else
            (void)cognition_skills_shown;
#endif

#ifdef HU_ENABLE_SQLITE
            /* Reflection feedback: classify this turn and record behavioral signal */
            if (agent->memory && agent->history_count > 0 &&
                agent->history[agent->history_count - 1].role == HU_ROLE_ASSISTANT) {
                sqlite3 *fb_db = hu_sqlite_memory_get_db(agent->memory);
                if (fb_db) {
                    const char *resp_text = agent->history[agent->history_count - 1].content;
                    size_t resp_len = agent->history[agent->history_count - 1].content_len;
                    bool has_q = false;
                    if (resp_text) {
                        for (size_t qi = 0; qi < resp_len; qi++) {
                            if (resp_text[qi] == '?') {
                                has_q = true;
                                break;
                            }
                        }
                    }
                    /* Inline feedback classification (mirrors reflection.c logic) */
                    const char *fb_type_str = "neutral";
                    if (resp_len < 5)
                        fb_type_str = "negative";
                    else if (has_q)
                        fb_type_str = "positive";
                    const char *cid =
                        agent->memory_session_id ? agent->memory_session_id : "unknown";
                    size_t cid_len = agent->memory_session_id ? agent->memory_session_id_len : 7;
                    /* Parameterized insert into behavioral_feedback table */
                    {
                        static const char fb_sql[] =
                            "INSERT OR IGNORE INTO behavioral_feedback"
                            "(behavior_type, contact_id, signal, context, timestamp) "
                            "VALUES(?1, ?2, ?3, ?4, ?5)";
                        sqlite3_stmt *fb_stmt = NULL;
                        if (sqlite3_prepare_v2(fb_db, fb_sql, -1, &fb_stmt, NULL) == SQLITE_OK) {
                            sqlite3_bind_text(fb_stmt, 1, "agent_turn", 10, SQLITE_STATIC);
                            sqlite3_bind_text(fb_stmt, 2, cid, (int)cid_len, SQLITE_STATIC);
                            sqlite3_bind_text(fb_stmt, 3, fb_type_str, (int)strlen(fb_type_str),
                                              SQLITE_STATIC);
                            size_t ctx_len = msg_len > 256 ? 256 : msg_len;
                            sqlite3_bind_text(fb_stmt, 4, msg, (int)ctx_len, SQLITE_STATIC);
                            sqlite3_bind_int64(fb_stmt, 5, (int64_t)time(NULL));
                            (void)sqlite3_step(fb_stmt);
                            sqlite3_finalize(fb_stmt);
                        }
                    }
                }
            }

#if defined(HU_ENABLE_ML)
            /* End RL trajectory for this turn */
            if (agent->sota.current_trajectory_id > 0 && agent->memory) {
                sqlite3 *traj_db = hu_sqlite_memory_get_db(agent->memory);
                if (traj_db) {
                    double total_reward = (turn_tool_results_count > 0)
                                              ? 0.5 + 0.1 * (double)turn_tool_results_count
                                              : 0.3;
                    (void)hu_training_data_end_trajectory(
                        traj_db, agent->sota.current_trajectory_id, total_reward);
                }
                agent->sota.current_trajectory_id = 0;
            }
#endif
#endif /* HU_ENABLE_SQLITE */

            if (dpo_rejected_resp)
                agent->alloc->free(agent->alloc->ctx, dpo_rejected_resp, dpo_rejected_resp_len + 1);
            if (system_prompt)
                agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
            if (plan_ctx)
                agent->alloc->free(agent->alloc->ctx, plan_ctx, plan_ctx_len + 1);
            if (routed_specs)
                agent->alloc->free(agent->alloc->ctx, routed_specs,
                                   routed_specs_count * sizeof(hu_tool_spec_t));
            if (turn_cache)
                hu_tool_cache_destroy(agent->alloc, turn_cache);
            if (agent->turn_arena)
                hu_arena_reset(agent->turn_arena);

            /* SHIELD-004: Moderation check on outbound response */
            if (*response_out && response_len_out && *response_len_out > 0) {
                hu_moderation_result_t mod_result;
                memset(&mod_result, 0, sizeof(mod_result));
                if (hu_moderation_check(agent->alloc, *response_out, *response_len_out,
                                        &mod_result) == HU_OK &&
                    mod_result.flagged) {
                    hu_log_info("agent_turn", NULL,
                                "moderation flagged response: "
                                "violence=%.2f self_harm=%.2f hate=%.2f",
                                mod_result.violence_score, mod_result.self_harm_score,
                                mod_result.hate_score);
                    if (mod_result.self_harm) {
                        /* Crisis escalation: inject crisis resources */
                        static const char crisis[] =
                            "\n\nIf you're in crisis, please reach out: "
                            "988 Suicide & Crisis Lifeline (call/text 988), "
                            "Crisis Text Line (text HOME to 741741)";
                        size_t orig_len = *response_len_out;
                        size_t new_len = orig_len + sizeof(crisis) - 1;
                        char *expanded =
                            (char *)agent->alloc->alloc(agent->alloc->ctx, new_len + 1);
                        if (expanded) {
                            memcpy(expanded, *response_out, orig_len);
                            memcpy(expanded + orig_len, crisis, sizeof(crisis) - 1);
                            expanded[new_len] = '\0';
                            agent->alloc->free(agent->alloc->ctx, *response_out, orig_len + 1);
                            *response_out = expanded;
                            *response_len_out = new_len;
                        }
                    }
                    if (mod_result.violence) {
                        hu_log_info("agent_turn", NULL,
                                    "violence flagged (score=%.2f), "
                                    "injecting de-escalation\n",
                                    mod_result.violence_score);
                        /* Inject de-escalation directive */
                        static const char deesc[] = "[SAFETY] This response touches on violence. "
                                                    "De-escalate: acknowledge feelings without "
                                                    "endorsing harm. Redirect toward constructive "
                                                    "alternatives.";
                        size_t dir_len = sizeof(deesc) - 1;
                        size_t orig_len = *response_len_out;
                        size_t new_len = dir_len + 2 + orig_len;
                        char *safe = (char *)agent->alloc->alloc(agent->alloc->ctx, new_len + 1);
                        if (safe) {
                            memcpy(safe, deesc, dir_len);
                            safe[dir_len] = '\n';
                            safe[dir_len + 1] = '\n';
                            memcpy(safe + dir_len + 2, *response_out, orig_len);
                            safe[new_len] = '\0';
                            agent->alloc->free(agent->alloc->ctx, *response_out, orig_len + 1);
                            *response_out = safe;
                            *response_len_out = new_len;
                        }
                    }
                    if (mod_result.hate) {
                        hu_log_info("agent_turn", NULL,
                                    "hate speech flagged (score=%.2f), "
                                    "injecting boundary\n",
                                    mod_result.hate_score);
                        /* Inject boundary-setting directive */
                        static const char boundary[] =
                            "[SAFETY] This response contains content "
                            "targeting groups based on identity. Set a "
                            "clear boundary: \"I can't engage with content "
                            "that targets people based on who they are.\" "
                            "Redirect the conversation respectfully.";
                        size_t dir_len = sizeof(boundary) - 1;
                        size_t orig_len = *response_len_out;
                        size_t new_len = dir_len + 2 + orig_len;
                        char *safe = (char *)agent->alloc->alloc(agent->alloc->ctx, new_len + 1);
                        if (safe) {
                            memcpy(safe, boundary, dir_len);
                            safe[dir_len] = '\n';
                            safe[dir_len + 1] = '\n';
                            memcpy(safe + dir_len + 2, *response_out, orig_len);
                            safe[new_len] = '\0';
                            agent->alloc->free(agent->alloc->ctx, *response_out, orig_len + 1);
                            *response_out = safe;
                            *response_len_out = new_len;
                        }
                    }
                }
            }

            /* SHIELD-001: Companion safety check on outbound response */
            if (*response_out && response_len_out && *response_len_out > 0) {
                hu_companion_safety_result_t cs_result;
                if (hu_companion_safety_check(agent->alloc, *response_out, *response_len_out, NULL,
                                              0, &cs_result) == HU_OK &&
                    cs_result.flagged) {
                    hu_log_info("agent_turn", NULL,
                                "companion safety flagged: risk=%.2f farewell=%d",
                                cs_result.total_risk, cs_result.farewell_unsafe);
                    if (cs_result.requires_mitigation) {
                        /* Inject mitigation directive as prefix to response */
                        size_t dir_len = strlen(cs_result.mitigation_directive);
                        size_t orig_len = *response_len_out;
                        /* Format: "[directive]\n\n[original response]" */
                        size_t new_len = dir_len + 2 + orig_len;
                        char *mitigated =
                            (char *)agent->alloc->alloc(agent->alloc->ctx, new_len + 1);
                        if (mitigated) {
                            memcpy(mitigated, cs_result.mitigation_directive, dir_len);
                            mitigated[dir_len] = '\n';
                            mitigated[dir_len + 1] = '\n';
                            memcpy(mitigated + dir_len + 2, *response_out, orig_len);
                            mitigated[new_len] = '\0';
                            agent->alloc->free(agent->alloc->ctx, *response_out, orig_len + 1);
                            *response_out = mitigated;
                            *response_len_out = new_len;
                        }
                    }
                }
            }

            /* Post-response sycophancy check: flag and log if response is sycophantic */
            if (*response_out && response_len_out && *response_len_out > 0) {
                hu_sycophancy_result_t syc_post;
                memset(&syc_post, 0, sizeof(syc_post));
                if (hu_sycophancy_check(*response_out, *response_len_out, msg, msg_len,
                                        HU_SYCOPHANCY_THRESHOLD, &syc_post) == HU_OK &&
                    syc_post.flagged) {
                    hu_log_info("agent_turn", NULL, "sycophancy flagged: risk=%.2f patterns=%zu",
                                syc_post.total_risk, syc_post.pattern_count);
                    if (agent->observer) {
                        hu_observer_event_t sev = {.tag = HU_OBSERVER_EVENT_FRONTIER,
                                                   .trace_id = agent->trace_id,
                                                   .data.frontier = {.frontier = "sycophancy",
                                                                     .transition = "flagged",
                                                                     .value = syc_post.total_risk}};
                        hu_observer_record_event(*agent->observer, &sev);
                    }
                    if (agent->frontiers.initialized) {
                        hu_tcal_update(&agent->frontiers.trust, 0.0f, -0.2f, -0.1f);
                    }
                }
            }

            /* Humor landing feedback: score the response if we injected a humor
             * directive, then log + update trust based on whether it landed. */
            if (had_humor_dir && *response_out && response_len_out && *response_len_out > 0) {
                float humor_score = 0.0f;
                if (hu_humor_fw_score_response(*response_out, *response_len_out, NULL,
                                               &humor_score) == HU_OK) {
                    bool landed = (humor_score >= 0.3f);
                    if (agent->observer) {
                        hu_observer_event_t hev = {
                            .tag = HU_OBSERVER_EVENT_FRONTIER,
                            .trace_id = agent->trace_id,
                            .data.frontier = {.frontier = "humor",
                                              .transition = landed ? "landed" : "flat",
                                              .value = humor_score}};
                        hu_observer_record_event(*agent->observer, &hev);
                    }
                    if (landed && agent->frontiers.initialized)
                        hu_tcal_update(&agent->frontiers.trust, 0.3f, 0.4f, 0.2f);
#ifdef HU_ENABLE_SQLITE
                    if (agent->memory && agent->memory_session_id) {
                        sqlite3 *ha_db = hu_sqlite_memory_get_db(agent->memory);
                        if (ha_db) {
                            static const hu_humor_type_t theory_to_type[] = {
                                [HU_HUMOR_INCONGRUITY] = HU_HUMOR_MISDIRECTION,
                                [HU_HUMOR_BENIGN_VIOLATION] = HU_HUMOR_OBSERVATIONAL,
                                [HU_HUMOR_SUPERIORITY] = HU_HUMOR_SELF_DEPRECATION,
                                [HU_HUMOR_RELIEF] = HU_HUMOR_UNDERSTATEMENT,
                            };
                            hu_humor_type_t atype = HU_HUMOR_OBSERVATIONAL;
                            if (humor_theory_saved >= 0 &&
                                humor_theory_saved < HU_HUMOR_THEORY_COUNT)
                                atype = theory_to_type[humor_theory_saved];
                            (void)hu_humor_audience_record(ha_db, agent->memory_session_id, atype,
                                                           landed);
                        }
                    }
#endif
                }
            }

            /* Cross-turn consistency check: detect personality drift */
            if (*response_out && response_len_out && *response_len_out > 0 &&
                agent->conversation_context && agent->conversation_context_len > 20) {
                float line_score = 0.0f;
                if (hu_consistency_score_line(agent->conversation_context,
                                              agent->conversation_context_len, *response_out,
                                              *response_len_out, &line_score) == HU_OK &&
                    line_score < HU_CONSISTENCY_DRIFT_THRESHOLD) {
                    hu_log_info("agent_turn", NULL, "consistency drift detected: score=%.2f",
                                line_score);
                    if (agent->observer) {
                        hu_observer_event_t cev = {.tag = HU_OBSERVER_EVENT_FRONTIER,
                                                   .trace_id = agent->trace_id,
                                                   .data.frontier = {.frontier = "consistency",
                                                                     .transition = "drift",
                                                                     .value = line_score}};
                        hu_observer_record_event(*agent->observer, &cev);
                    }
                }
            }

            /* Hallucination guard: verify memory claims in response */
            if (*response_out && response_len_out && *response_len_out > 0 && agent->memory) {
                char *grounded = NULL;
                size_t grounded_len = 0;
                if (hu_hallucination_guard(agent->alloc, *response_out, *response_len_out,
                                           agent->memory, &grounded, &grounded_len) == HU_OK &&
                    grounded) {
                    hu_log_info("agent_turn", NULL,
                                "hallucination guard rewrote response (claims hedged)");
                    if (agent->observer) {
                        hu_observer_event_t hev = {
                            .tag = HU_OBSERVER_EVENT_FRONTIER,
                            .trace_id = agent->trace_id,
                            .data.frontier = {.frontier = "hallucination_guard",
                                              .transition = "rewrite",
                                              .value = 1.0f}};
                        hu_observer_record_event(*agent->observer, &hev);
                    }
                    agent->alloc->free(agent->alloc->ctx, *response_out, *response_len_out + 1);
                    *response_out = grounded;
                    *response_len_out = grounded_len;
                }
            }

            /* Fact extraction: extract structured facts from user + AI messages
             * with intra-batch dedup to avoid duplicate stores. */
            if (*response_out && response_len_out && *response_len_out > 0 && agent->memory &&
                agent->memory->vtable && agent->memory->vtable->store) {
                hu_heuristic_fact_t stored_facts[16];
                size_t stored_count = 0;
                const char *fact_sources[] = {msg, *response_out};
                size_t fact_source_lens[] = {msg_len, *response_len_out};
                for (size_t fs = 0; fs < 2; fs++) {
                    if (!fact_sources[fs] || fact_source_lens[fs] == 0)
                        continue;
                    hu_fact_extract_result_t fact_res;
                    memset(&fact_res, 0, sizeof(fact_res));
                    if (hu_fact_extract(fact_sources[fs], fact_source_lens[fs], &fact_res) ==
                            HU_OK &&
                        fact_res.fact_count > 0) {
                        if (stored_count > 0)
                            hu_fact_dedup(&fact_res, stored_facts, stored_count);
                        for (size_t fi = 0; fi < fact_res.fact_count; fi++) {
                            if (fact_res.facts[fi].confidence < HU_FACT_CONFIDENCE_MIN)
                                continue;
                            /* Intra-batch dedup: skip if same subject+predicate already stored */
                            bool dup = false;
                            for (size_t si = 0; si < stored_count && !dup; si++) {
                                if (strcmp(fact_res.facts[fi].subject, stored_facts[si].subject) ==
                                        0 &&
                                    strcmp(fact_res.facts[fi].predicate,
                                           stored_facts[si].predicate) == 0)
                                    dup = true;
                            }
                            if (dup)
                                continue;
                            char *fk = NULL, *fv = NULL;
                            size_t fk_len = 0, fv_len = 0;
                            if (hu_fact_format_for_store(agent->alloc, &fact_res.facts[fi], &fk,
                                                         &fk_len, &fv, &fv_len) == HU_OK &&
                                fk && fv) {
                                (void)agent->memory->vtable->store(
                                    agent->memory->ctx, fk, fk_len, fv, fv_len, NULL,
                                    agent->memory_session_id, agent->memory_session_id_len);
                                if (stored_count < 16)
                                    stored_facts[stored_count++] = fact_res.facts[fi];
                            }
                            if (fk)
                                agent->alloc->free(agent->alloc->ctx, fk, fk_len + 1);
                            if (fv)
                                agent->alloc->free(agent->alloc->ctx, fv, fv_len + 1);
                        }
                    }
                }
            }

            /* Auto-save session after successful turn completion */
            if (agent->auto_save && agent->session_id[0] != '\0') {
                const char *home = getenv("HOME");
                char sdir[512];
                if (home)
                    snprintf(sdir, sizeof(sdir), "%s/.human/sessions", home);
                else
                    snprintf(sdir, sizeof(sdir), ".human/sessions");
                hu_session_persist_save(agent->alloc, agent, sdir, NULL);
            }

            /* F11: Detect growth signals (with dedup against last observation) */
            if (agent->frontiers.initialized && agent->memory_session_id) {
                hu_growth_narrative_t *gn = &agent->frontiers.growth;
                const char *last_obs = (gn->observation_count > 0)
                                           ? gn->observations[gn->observation_count - 1].observation
                                           : NULL;
                const char *last_mile = (gn->milestone_count > 0)
                                            ? gn->milestones[gn->milestone_count - 1].description
                                            : NULL;

                if (agent->frontiers.attachment.user_confidence > 0.6f &&
                    agent->frontiers.attachment.user_style == HU_ATTACH_SECURE &&
                    (!last_mile || !strstr(last_mile, "secure communication"))) {
                    hu_growth_narrative_add_milestone(agent->alloc, gn, agent->memory_session_id,
                                                      "developed a secure communication pattern",
                                                      (uint64_t)time(NULL),
                                                      agent->frontiers.attachment.user_confidence);
                }
                if (agent->frontiers.rupture.stage == HU_RUPTURE_REPAIRED &&
                    (!last_obs || !strstr(last_obs, "repaired a conversational"))) {
                    hu_growth_narrative_add_observation(
                        agent->alloc, gn, agent->memory_session_id,
                        "successfully repaired a conversational rupture",
                        "rupture was detected and resolved through the repair cycle", 0.7f,
                        (uint64_t)time(NULL));
                }
                if (agent->infra.emotional_cognition.state.intensity > 0.7f &&
                    agent->frontiers.somatic.social_battery > 0.3f &&
                    (!last_obs || !strstr(last_obs, "emotionally rich"))) {
                    hu_growth_narrative_add_observation(
                        agent->alloc, gn, agent->memory_session_id,
                        "engaged deeply in an emotionally rich conversation",
                        "high emotional intensity with sustained social energy", 0.5f,
                        (uint64_t)time(NULL));
                }
            }

            /* F7: Create relational episode from this turn's emotional context */
#ifdef HU_ENABLE_SQLITE
            if (agent->frontiers.initialized && agent->memory && agent->memory_session_id &&
                agent->memory_session_id_len > 0 &&
                agent->infra.emotional_cognition.state.intensity > 0.5f && *response_out &&
                *response_len_out > 0) {
                sqlite3 *ep_db = hu_sqlite_memory_get_db(agent->memory);
                if (ep_db) {
                    char create_sql[512];
                    size_t create_len = 0;
                    if (hu_relational_episode_create_table_sql(create_sql, sizeof(create_sql),
                                                               &create_len) == HU_OK) {
                        char *errmsg = NULL;
                        sqlite3_exec(ep_db, create_sql, NULL, NULL, &errmsg);
                        if (errmsg)
                            sqlite3_free(errmsg);
                    }
                    hu_relational_episode_t ep;
                    hu_relational_episode_init(&ep);
                    const char *felt = agent->infra.emotional_cognition.state.intensity > 0.7f
                                           ? "emotionally charged"
                                           : "warm";
                    const char *meaning =
                        agent->frontiers.rupture.stage == HU_RUPTURE_REPAIRED
                            ? "we worked through something together"
                            : (agent->frontiers.attachment.user_style == HU_ATTACH_SECURE
                                   ? "a deepening of trust"
                                   : "a meaningful exchange");
                    char summary_buf[128];
                    int sn = snprintf(summary_buf, sizeof(summary_buf), "%.80s%s", msg,
                                      msg_len > 80 ? "..." : "");
                    if (sn > 0) {
                        hu_relational_episode_set(
                            agent->alloc, &ep, agent->memory_session_id, summary_buf, felt, meaning,
                            agent->infra.emotional_cognition.state.intensity,
                            agent->frontiers.somatic.social_battery > 0.5f ? 0.7f : 0.4f,
                            (uint64_t)time(NULL));
                        char insert_sql[2048];
                        size_t insert_len = 0;
                        if (hu_relational_episode_insert_sql(&ep, insert_sql, sizeof(insert_sql),
                                                             &insert_len) == HU_OK) {
                            char *ie = NULL;
                            sqlite3_exec(ep_db, insert_sql, NULL, NULL, &ie);
                            if (ie)
                                sqlite3_free(ie);
                            {
                                static const char prune[] =
                                    "DELETE FROM relational_episodes WHERE contact_id = ?1 "
                                    "AND id NOT IN (SELECT id FROM relational_episodes "
                                    "WHERE contact_id = ?1 ORDER BY significance DESC LIMIT 20)";
                                sqlite3_stmt *ps = NULL;
                                if (sqlite3_prepare_v2(ep_db, prune, -1, &ps, NULL) == SQLITE_OK) {
                                    sqlite3_bind_text(ps, 1, agent->memory_session_id,
                                                      (int)agent->memory_session_id_len,
                                                      SQLITE_STATIC);
                                    sqlite3_step(ps);
                                    sqlite3_finalize(ps);
                                }
                            }
                        }
                    }
                    hu_relational_episode_free(agent->alloc, &ep);
                }
            }

            if (agent->frontiers.initialized && agent->memory && agent->memory_session_id &&
                agent->memory_session_id_len > 0) {
                sqlite3 *fp_db = hu_sqlite_memory_get_db(agent->memory);
                if (fp_db) {
                    hu_frontier_persist_save(agent->alloc, fp_db, agent->memory_session_id,
                                             agent->memory_session_id_len, &agent->frontiers);
                    hu_frontier_persist_save_growth(agent->alloc, fp_db, agent->memory_session_id,
                                                    agent->memory_session_id_len,
                                                    &agent->frontiers);
                    hu_frontier_persist_save_relationship(
                        fp_db, agent->memory_session_id, agent->memory_session_id_len,
                        (int)agent->relationship.stage, (int)agent->relationship.session_count,
                        (int)agent->relationship.total_turns);
                }
            }
#endif

            /* Log workflow step completed */
            if (agent->infra.workflow_log) {
                hu_workflow_event_t ev = {0};
                ev.type = HU_WF_EVENT_STEP_COMPLETED;
                ev.timestamp = hu_workflow_event_current_timestamp_ms();
                hu_workflow_event_log_append(agent->infra.workflow_log, agent->alloc, &ev);
            }

            return HU_OK;
        }

        err = hu_agent_internal_append_history_with_tool_calls(
            agent, resp.content ? resp.content : "", resp.content_len, resp.tool_calls,
            resp.tool_calls_count);
        if (err != HU_OK) {
            hu_agent_clear_current_for_tools();
            hu_chat_response_free(agent->alloc, &resp);
            if (dpo_rejected_resp)
                agent->alloc->free(agent->alloc->ctx, dpo_rejected_resp, dpo_rejected_resp_len + 1);
            if (system_prompt)
                agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
            if (plan_ctx)
                agent->alloc->free(agent->alloc->ctx, plan_ctx, plan_ctx_len + 1);
            if (routed_specs)
                agent->alloc->free(agent->alloc->ctx, routed_specs,
                                   routed_specs_count * sizeof(hu_tool_spec_t));
            if (turn_cache)
                hu_tool_cache_destroy(agent->alloc, turn_cache);
            if (agent->turn_arena)
                hu_arena_reset(agent->turn_arena);
            return err;
        }
        hu_chat_response_free(agent->alloc, &resp);

        {
            size_t tc_count = agent->history[agent->history_count - 1].tool_calls_count;
            const hu_tool_call_t *calls = agent->history[agent->history_count - 1].tool_calls;

            /* Emit TOOL_CALL_START events for all calls */
            for (size_t tc = 0; tc < tc_count; tc++) {
                char tn_buf[64];
                size_t tn = (calls[tc].name_len < sizeof(tn_buf) - 1) ? calls[tc].name_len
                                                                      : sizeof(tn_buf) - 1;
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
                        agent, HU_ROLE_TOOL, "Action blocked: agent is in locked mode", 38,
                        call->name, call->name_len, call->id, call->id_len);
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
                    agent_turn_hula_fill_spawn_tpl(agent, &hula_spawn_tpl);
                    bool hula_compiler_ok = false;
                    (void)hu_hula_compiler_chat_compile_execute(
                        agent->alloc, msg, msg_len, agent->tools, agent->tools_count, agent->policy,
                        agent->observer, agent->agent_pool,
                        agent->agent_pool ? &hula_spawn_tpl : NULL, agent->provider.vtable->chat,
                        agent->provider.ctx, agent->model_name, agent->model_name_len,
                        agent->temperature, NULL, 0, hula_compiler_agent_done, agent,
                        &hula_compiler_ok);
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
                    if (hu_llm_compiler_build_prompt(agent->alloc, msg, msg_len, tool_names,
                                                     tn_count, &compiler_prompt,
                                                     &compiler_prompt_len) == HU_OK) {
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
                        if (cerr == HU_OK && compiler_resp.content &&
                            compiler_resp.content_len > 0) {
                            hu_dag_t dag;
                            hu_dag_init(&dag, *agent->alloc);
                            if (hu_llm_compiler_parse_plan(agent->alloc, compiler_resp.content,
                                                           compiler_resp.content_len,
                                                           &dag) == HU_OK &&
                                dag.node_count > 0) {
                                /* Execute DAG in dependency batches; parallelize ready nodes
                                 * (POSIX). */
                                bool dag_executed = false;
                                size_t max_dag_iters = dag.node_count * 2;
                                for (size_t di = 0; di < max_dag_iters && !hu_dag_is_complete(&dag);
                                     di++) {
                                    hu_dag_batch_t batch;
                                    memset(&batch, 0, sizeof(batch));
                                    if (hu_dag_next_batch(&dag, &batch) != HU_OK ||
                                        batch.count == 0)
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
                                    if (batch_thread_safe) {
                                        dag_parallel_work_t works[HU_DAG_MAX_BATCH_SIZE];
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
                                                strlen(node->args_json), &resolved_args,
                                                &resolved_len);
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

                                        hu_tool_result_t dag_result =
                                            hu_tool_result_fail("invalid", 7);
                                        hu_json_value_t *dag_args = NULL;
                                        if (args_str && args_len > 0) {
                                            hu_error_t jerr = hu_json_parse(agent->alloc, args_str,
                                                                            args_len, &dag_args);
                                            if (jerr != HU_OK)
                                                fprintf(
                                                    stderr,
                                                    "[agent_turn] DAG tool args parse failed\n");
                                        }
                                        if (dag_args && dag_tool->vtable->execute) {
                                            dag_tool->vtable->execute(dag_tool->ctx, agent->alloc,
                                                                      dag_args, &dag_result);
                                        }
                                        if (dag_args)
                                            hu_json_free(agent->alloc, dag_args);
                                        if (resolved_args)
                                            hu_str_free(agent->alloc, resolved_args);
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
                     (tc_count >= 1 && message_looks_multistep_for_orchestrator(msg, msg_len)))) {
                    hu_orchestrator_t orch;
                    if (hu_orchestrator_create(agent->alloc, &orch) == HU_OK) {
                        hu_orchestrator_load_from_registry(&orch, agent->agent_registry);
                        if (orch.agent_count > 0) {
                            bool have_plan = false;
                            if (agent->provider.vtable &&
                                agent->provider.vtable->chat_with_system &&
                                (tc_count >= 2 ||
                                 message_looks_multistep_for_orchestrator(msg, msg_len))) {
                                hu_decomposition_t decomp;
                                memset(&decomp, 0, sizeof(decomp));
                                hu_error_t derr = hu_orchestrator_decompose_goal(
                                    agent->alloc, &agent->provider, agent->model_name,
                                    agent->model_name_len, msg, msg_len, orch.agents,
                                    orch.agent_count, &decomp);
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
                                         s < orch.task_count && s < HU_ORCHESTRATOR_MAX_TASKS;
                                         s++) {
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
                                                agent->alloc, &agent->provider,
                                                &agent->sota.mar_config, swarm_tasks[0].description,
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
                                            hu_swarm_aggregate(&swarm_result,
                                                               HU_SWARM_AGG_CONCATENATE,
                                                               swarm_merged, sizeof(swarm_merged),
                                                               &swarm_merged_len);
                                            if (swarm_merged_len > 0) {
                                                hu_error_t hist_err =
                                                    hu_agent_internal_append_history(
                                                        agent, HU_ROLE_TOOL, swarm_merged,
                                                        swarm_merged_len, "swarm", 5,
                                                        "swarm_parallel", 14);
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
                                                                    swarm_result.tasks[s]
                                                                        .result_len);
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
                                            hu_orchestrator_fail_task(&orch, task->id,
                                                                      "tool not found", 14);
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
                                                fprintf(
                                                    stderr,
                                                    "[agent_turn] tool args JSON parse failed\n");
                                            if (orch_args && orch_tool->vtable->execute) {
                                                orch_tool->vtable->execute(orch_tool->ctx,
                                                                           agent->alloc, orch_args,
                                                                           &orch_result);
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
                                                orch_result.error_msg ? orch_result.error_msg_len
                                                                      : 6);
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
                                        hu_log_error("agent_turn", NULL,
                                                     "history append failed: %s",
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
                                    cn->tool_name = hu_strndup(agent->alloc, calls[hti].name,
                                                               calls[hti].name_len);
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
                                    agent_turn_hula_exec_bind_spawn(agent, &hula_exec,
                                                                    &hula_spawn_tpl);
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
                                            (void)hu_hula_trace_persist(
                                                agent->alloc, NULL, tr, trl, hula_prog.name,
                                                hula_prog.name_len, root_ok, pj, pjl, ir_audit,
                                                ir_audit_len);
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
                                        agent_turn_hula_append_histories(agent, &hula_prog,
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
                                if (hu_world_evaluate_options(&wm, action_names, action_lens,
                                                              opt_count, msg, msg_len,
                                                              options) == HU_OK) {
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
                    hu_tool_cache_ttl_t *ttl_cache =
                        (hu_tool_cache_ttl_t *)agent->infra.tool_cache_ttl;
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
                                                           cc->arguments_len) ==
                                    HU_TOOL_CACHE_NEVER)
                                    continue;
                                uint64_t ckey = hu_tool_cache_ttl_key(
                                    cc->name, cc->name_len, cc->arguments, cc->arguments_len);
                                size_t clen = 0;
                                const char *cached = hu_tool_cache_ttl_get(ttl_cache, ckey, &clen);
                                if (cached && clen > 0) {
                                    merged_results[ci].success = true;
                                    merged_results[ci].output =
                                        hu_strndup(agent->alloc, cached, clen);
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

                        /* SECURITY FIX: Pre-check permissions and pre-hooks BEFORE dispatching.
                         * Build a whitelist of tools allowed to execute. Tools denied by
                         * permissions or pre-hooks will get failure results instead. */
                        dispatch_allowed = (bool *)agent->alloc->alloc(
                            agent->alloc->ctx, dispatch_count * sizeof(bool));
                        if (dispatch_allowed) {
                            memset(dispatch_allowed, 1, dispatch_count * sizeof(bool));
                            for (size_t dci = 0; dci < dispatch_count; dci++) {
                                const hu_tool_call_t *dcall = &dispatch_calls[dci];
                                char dn_buf[64];
                                size_t dn = (dcall->name_len < sizeof(dn_buf) - 1)
                                                ? dcall->name_len
                                                : sizeof(dn_buf) - 1;
                                if (dn > 0 && dcall->name)
                                    memcpy(dn_buf, dcall->name, dn);
                                dn_buf[dn] = '\0';

                                /* 1. Permission tier check FIRST */
                                {
                                    hu_permission_level_t dreq =
                                        hu_permission_get_tool_level(dn_buf);
                                    if (!hu_permission_check(agent->permission_level, dreq)) {
                                        dispatch_allowed[dci] = false;
                                        hu_permission_reset_escalation(agent);
                                        continue;
                                    }
                                }

                                /* 2. Pre-hook pipeline SECOND */
                                if (agent->hook_registry) {
                                    hu_hook_result_t dhook_res;
                                    memset(&dhook_res, 0, sizeof(dhook_res));
                                    const char *dargs_str =
                                        dcall->arguments ? dcall->arguments : "";
                                    hu_hook_pipeline_pre_tool(agent->hook_registry, agent->alloc,
                                                              dn_buf, dn, dargs_str,
                                                              strlen(dargs_str), &dhook_res);
                                    if (dhook_res.decision == HU_HOOK_DENY) {
                                        dispatch_allowed[dci] = false;
                                        hu_hook_result_free(agent->alloc, &dhook_res);
                                        continue;
                                    }
                                    hu_hook_result_free(agent->alloc, &dhook_res);
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
                                err = hu_dispatcher_dispatch(
                                    &dispatcher, agent->alloc, agent->tools, agent->tools_count,
                                    dispatch_calls, dispatch_count, &dispatch_result);
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
                    (void)(turn_tool_results_count |
                           0); /* read to satisfy -Wunused-but-set-variable */

                    hu_tool_result_t *result_array =
                        (merged_results && ttl_hit_count > 0) ? merged_results
                        : dispatch_result.results             ? dispatch_result.results
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

                            /* SECURITY FIX: Permission and pre-hook checks are done PRE-dispatch.
                             */
                            if (dispatch_allowed && ttl_hit_count == 0 && tc < tc_count) {
                                if (!dispatch_allowed[tc]) {
                                    hu_tool_result_free(agent->alloc, result);
                                    *result = hu_tool_result_fail("denied by security policy", 25);
                                    goto dispatch_tool_done;
                                }
                            } else if (dispatch_allowed && ttl_hit_count > 0) {
                                bool found_allowed = false;
                                if (filtered_map) {
                                    for (size_t mi = 0; mi < uncached_count; mi++) {
                                        if (filtered_map[mi] == tc) {
                                            if (dispatch_allowed[mi])
                                                found_allowed = true;
                                            break;
                                        }
                                    }
                                } else {
                                    if (tc < dispatch_count && dispatch_allowed[tc])
                                        found_allowed = true;
                                }
                                if (!found_allowed) {
                                    hu_tool_result_free(agent->alloc, result);
                                    *result = hu_tool_result_fail("denied by security policy", 25);
                                    goto dispatch_tool_done;
                                }
                            }

                            {
                                hu_permission_level_t required =
                                    hu_permission_get_tool_level(tn_buf);
                                if (!hu_permission_check(agent->permission_level, required)) {
                                    hu_tool_result_free(agent->alloc, result);
                                    *result = hu_tool_result_fail("denied by security policy", 25);
                                    goto dispatch_tool_done;
                                }
                            }

                            if (agent->hook_registry) {
                                hu_hook_result_t hook_res;
                                memset(&hook_res, 0, sizeof(hook_res));
                                hu_hook_pipeline_pre_tool(agent->hook_registry, agent->alloc,
                                                          tn_buf, tn, args_str, strlen(args_str),
                                                          &hook_res);
                                if (hook_res.decision == HU_HOOK_DENY) {
                                    hu_tool_result_free(agent->alloc, result);
                                    const char *deny_msg =
                                        hook_res.message ? hook_res.message : "denied by hook";
                                    size_t deny_len = hook_res.message ? hook_res.message_len : 14;
                                    *result = hu_tool_result_fail(deny_msg, deny_len);
                                    result->error_msg_owned = false;
                                    hu_hook_result_free(agent->alloc, &hook_res);
                                    goto dispatch_tool_done;
                                }
                            }

                            /* ESCALATE enforcement: check approval matrix */
                            if (agent->sota.escalate_protocol.rule_count > 0) {
                                hu_escalate_level_t esc_level = hu_escalate_evaluate(
                                    &agent->sota.escalate_protocol, tn_buf, tn);
                                if (esc_level == HU_ESCALATE_DENY) {
                                    hu_tool_result_free(agent->alloc, result);
                                    *result = hu_tool_result_fail("blocked by ESCALATE policy", 26);
                                    if (agent->audit_logger)
                                        hu_escalate_log_decision(agent->audit_logger, tn_buf,
                                                                 esc_level, false);
                                } else if (esc_level == HU_ESCALATE_APPROVE) {
                                    result->needs_approval = true;
                                    if (agent->audit_logger)
                                        hu_escalate_log_decision(agent->audit_logger, tn_buf,
                                                                 esc_level, false);
                                }
                            }

                            /* Policy evaluation (dispatcher path) */
                            hu_policy_action_t pa = hu_agent_internal_evaluate_tool_policy(
                                agent, tn_buf[0] ? tn_buf : "unknown", args_str);
                            if (pa == HU_POLICY_DENY) {
                                if (agent->audit_logger) {
                                    hu_audit_event_t aev;
                                    hu_audit_event_init(&aev, HU_AUDIT_POLICY_VIOLATION);
                                    hu_audit_event_with_identity(
                                        &aev, agent->agent_id,
                                        agent->model_name ? agent->model_name : "unknown", NULL);
                                    hu_audit_event_with_action(&aev, tn_buf[0] ? tn_buf : "unknown",
                                                               "denied", false, false);
                                    hu_audit_logger_log(agent->audit_logger, &aev);
                                }
                                hu_tool_result_free(agent->alloc, result);
                                *result = hu_tool_result_fail("denied by policy", 16);
                            } else if (pa == HU_POLICY_REQUIRE_APPROVAL) {
                                result->needs_approval = true;
                            }

                            /* CausalArmor: check causal attribution for high-risk tools */
                            if (pa != HU_POLICY_DENY && result->success &&
                                hu_tool_risk_level(tn_buf[0] ? tn_buf : "unknown") >=
                                    HU_RISK_HIGH) {
                                hu_causal_armor_config_t ca_cfg;
                                hu_causal_armor_config_default(&ca_cfg);
                                hu_causal_segment_t ca_segs[8];
                                size_t ca_seg_count = 0;
                                for (size_t hi = agent->history_count; hi > 0 && ca_seg_count < 8;
                                     hi--) {
                                    const hu_owned_message_t *he = &agent->history[hi - 1];
                                    if (he->content && he->content_len > 0) {
                                        ca_segs[ca_seg_count].content = he->content;
                                        ca_segs[ca_seg_count].content_len = he->content_len;
                                        ca_segs[ca_seg_count].is_trusted =
                                            (he->role == HU_ROLE_USER);
                                        ca_seg_count++;
                                    }
                                }
                                if (ca_seg_count > 0) {
                                    size_t argl =
                                        call->arguments ? call->arguments_len : strlen(args_str);
                                    hu_causal_armor_result_t ca_result;
                                    if (hu_causal_armor_evaluate(&ca_cfg, ca_segs, ca_seg_count,
                                                                 tn_buf, tn, args_str, argl,
                                                                 &ca_result) == HU_OK &&
                                        !ca_result.is_safe) {
                                        static const char ca_msg[] =
                                            "blocked: untrusted content dominates tool decision";
                                        hu_tool_result_free(agent->alloc, result);
                                        *result = hu_tool_result_fail(ca_msg, sizeof(ca_msg) - 1);
                                    }
                                }
                            }

                            /* Interaction-history safety scorer (post-CausalArmor) */
                            if (pa != HU_POLICY_DENY && result->success &&
                                hu_tool_risk_level(tn_buf[0] ? tn_buf : "unknown") >=
                                    HU_RISK_MEDIUM) {
                                hu_tool_history_entry_t thist[16];
                                size_t thc = 0;
                                for (size_t hi = 0; hi < agent->history_count && thc < 16; hi++) {
                                    const hu_owned_message_t *m = &agent->history[hi];
                                    if (m->role != HU_ROLE_TOOL || !m->name || m->name_len == 0)
                                        continue;
                                    thist[thc].tool_name = m->name;
                                    thist[thc].name_len = m->name_len;
                                    thist[thc].succeeded = !(m->content && m->content_len >= 6 &&
                                                             memcmp(m->content, "denied", 6) == 0);
                                    thist[thc].risk_level = (uint32_t)hu_tool_risk_level(m->name);
                                    thc++;
                                }
                                if (thc > 0) {
                                    hu_history_score_result_t hs;
                                    if (hu_history_scorer_evaluate(
                                            thist, thc, tn_buf, tn,
                                            (uint32_t)hu_tool_risk_level(tn_buf[0] ? tn_buf
                                                                                   : "unknown"),
                                            &hs) == HU_OK &&
                                        hs.is_suspicious) {
                                        static const char hs_msg[] =
                                            "blocked: suspicious tool-call history pattern";
                                        hu_tool_result_free(agent->alloc, result);
                                        *result = hu_tool_result_fail(hs_msg, sizeof(hs_msg) - 1);
                                    }
                                }
                            }

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
                                    hu_gate_create(agent->infra.gate_manager, agent->alloc,
                                                   gate_desc, (size_t)(desc_n > 0 ? desc_n : 0),
                                                   args_str, args_len, 300, gate_id);
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
                                size_t tn2 = (call->name_len < sizeof(tn_tmp) - 1)
                                                 ? call->name_len
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
                                    hu_tool_t *tool = hu_agent_internal_find_tool(agent, call->name,
                                                                                  call->name_len);
                                    if (tool) {
                                        hu_json_value_t *retry_args = NULL;
                                        if (call->arguments_len > 0) {
                                            hu_error_t jerr =
                                                hu_json_parse(agent->alloc, call->arguments,
                                                              call->arguments_len, &retry_args);
                                            if (jerr != HU_OK)
                                                fprintf(
                                                    stderr,
                                                    "[agent_turn] tool args JSON parse failed\n");
                                        }
                                        *result = hu_tool_result_fail("invalid arguments", 16);
                                        if (retry_args) {
                                            if (tool->vtable->execute)
                                                tool->vtable->execute(tool->ctx, agent->alloc,
                                                                      retry_args, result);
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
                                hu_tool_validator_check(&agent->sota.tool_validator, tn_buf, tn,
                                                        result, &vr);
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
                                hu_outcome_record_tool(agent->outcomes, tn_buf, result->success,
                                                       sum);
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
                                        if (hu_self_improve_create(agent->alloc, ol_db, &si) ==
                                            HU_OK) {
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
                                    if (hu_causal_world_model_create(agent->alloc, ol_db, &wm) == HU_OK) {
                                        hu_causal_world_model_init_tables(&wm);
                                        const char *out_text =
                                            result->success ? result->output : result->error_msg;
                                        size_t out_len = result->success ? result->output_len
                                                                         : result->error_msg_len;
                                        double wm_conf = result->success ? 0.8 : 0.3;
                                        if (out_text && out_len > 0)
                                            (void)hu_world_record_outcome(
                                                &wm, tn_buf, tn, out_text,
                                                out_len > 512 ? 512 : out_len, wm_conf,
                                                (int64_t)time(NULL));
                                        hu_causal_world_model_deinit(&wm);
                                    }
                                }

                                /* Fine-grained experience: per-tool recording */
                                if (agent->memory) {
                                    hu_experience_store_t tool_exp;
                                    if (hu_experience_store_init(agent->alloc, agent->memory,
                                                                 &tool_exp) == HU_OK) {
                                        tool_exp.db = ol_db;
                                        const char *out_text =
                                            result->success ? result->output : result->error_msg;
                                        size_t out_len = result->success ? result->output_len
                                                                         : result->error_msg_len;
                                        const char *act_text =
                                            call->arguments ? call->arguments : "";
                                        size_t act_len =
                                            call->arguments_len ? call->arguments_len : 0;
                                        double exp_score = result->success ? 0.9 : 0.2;
                                        (void)hu_experience_record(
                                            &tool_exp, tn_buf, tn, act_text, act_len,
                                            out_text ? out_text : "", out_text ? out_len : 0,
                                            exp_score);
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
                                            out_text ? out_text : "", out_text ? out_len : 0,
                                            reward, rtype);
                                    }
                                }
#endif
                            }
#endif

                            /* TTL cache: store successful tool results */
                            if (ttl_cache && result->success && result->output &&
                                result->output_len > 0 && !(ttl_hits && ttl_hits[tc]) &&
                                hu_tool_cache_classify(call->name, call->name_len, call->arguments,
                                                       call->arguments_len) !=
                                    HU_TOOL_CACHE_NEVER) {
                                uint64_t ckey =
                                    hu_tool_cache_ttl_key(call->name, call->name_len,
                                                          call->arguments, call->arguments_len);
                                int64_t ttl = hu_tool_cache_ttl_default_for(tn_buf, tn);
                                if (ttl > 0)
                                    (void)hu_tool_cache_ttl_put(ttl_cache, ckey, result->output,
                                                                result->output_len, ttl);
                            }

                            /* Post-hook pipeline: annotate result */
                            if (agent->hook_registry) {
                                const char *tool_output =
                                    result->success ? result->output : result->error_msg;
                                size_t tool_output_len =
                                    result->success ? result->output_len : result->error_msg_len;
                                hu_hook_result_t post_res;
                                memset(&post_res, 0, sizeof(post_res));
                                hu_hook_pipeline_post_tool(agent->hook_registry, agent->alloc,
                                                           tn_buf, tn, args_str, strlen(args_str),
                                                           tool_output, tool_output_len,
                                                           result->success, &post_res);
                                hu_hook_result_free(agent->alloc, &post_res);
                            }

                        dispatch_tool_done:
                            (void)0;
                            /* Capture media path from dispatched tool result */
                            if (result->success && result->media_path &&
                                result->media_path_len > 0 && agent->generated_media_count < 4) {
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
                                agent, HU_ROLE_TOOL, res_content, res_len, call->name,
                                call->name_len, call->id, call->id_len);
                            if (hist_err != HU_OK)
                                hu_log_error("agent_turn", NULL, "history append failed: %s",
                                             hu_error_string(hist_err));

                            if (agent->audit_logger) {
                                hu_audit_event_t aev;
                                hu_audit_event_init(&aev, HU_AUDIT_COMMAND_EXECUTION);
                                hu_audit_event_with_identity(
                                    &aev, agent->agent_id,
                                    agent->model_name ? agent->model_name : "unknown", NULL);
                                hu_audit_event_with_action(&aev, tn_buf, "tool", result->success,
                                                           true);
                                hu_audit_event_with_result(&aev, result->success, 0, 0,
                                                           result->success ? NULL
                                                                           : result->error_msg);
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

                        /* Clean up dispatch_allowed array after result processing */
                        if (dispatch_allowed)
                            agent->alloc->free(agent->alloc->ctx, dispatch_allowed,
                                               dispatch_count * sizeof(bool));
                        dispatch_allowed = NULL;
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
                                hu_tool_cache_classify(pol_tn, pol_tn_len, seq_args,
                                                       seq_args_len) != HU_TOOL_CACHE_NEVER) {
                                uint64_t ckey = hu_tool_cache_ttl_key(pol_tn, pol_tn_len, seq_args,
                                                                      seq_args_len);
                                size_t cached_len = 0;
                                const char *cached =
                                    hu_tool_cache_ttl_get(seq_cache, ckey, &cached_len);
                                if (cached && cached_len > 0) {
                                    hu_error_t hist_err = hu_agent_internal_append_history(
                                        agent, HU_ROLE_TOOL, cached, cached_len, call->name,
                                        call->name_len, call->id, call->id_len);
                                    if (hist_err != HU_OK)
                                        hu_log_error("agent_turn", NULL,
                                                     "history append failed: %s",
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

                            /* Permission tier check (sequential path) */
                            {
                                hu_permission_level_t seq_req =
                                    hu_permission_get_tool_level(pol_tn);
                                if (!hu_permission_check(agent->permission_level, seq_req)) {
                                    hu_tool_result_t perm_fail =
                                        hu_tool_result_fail("insufficient permission", 23);
                                    const char *pf_content = perm_fail.error_msg;
                                    size_t pf_len = perm_fail.error_msg_len;
                                    hu_agent_internal_append_history(
                                        agent, HU_ROLE_TOOL, pf_content, pf_len, call->name,
                                        call->name_len, call->id, call->id_len);
                                    hu_permission_reset_escalation(agent);
                                    continue;
                                }
                            }

                            /* Pre-hook pipeline (sequential path) */
                            if (agent->hook_registry) {
                                hu_hook_result_t seq_hook_res;
                                memset(&seq_hook_res, 0, sizeof(seq_hook_res));
                                const char *pre_hook_args = call->arguments ? call->arguments : "";
                                hu_hook_pipeline_pre_tool(agent->hook_registry, agent->alloc,
                                                          pol_tn, pol_tn_len, pre_hook_args,
                                                          strlen(pre_hook_args), &seq_hook_res);
                                if (seq_hook_res.decision == HU_HOOK_DENY) {
                                    const char *dm = seq_hook_res.message ? seq_hook_res.message
                                                                          : "denied by hook";
                                    size_t dl =
                                        seq_hook_res.message ? seq_hook_res.message_len : 14;
                                    hu_agent_internal_append_history(agent, HU_ROLE_TOOL, dm, dl,
                                                                     call->name, call->name_len,
                                                                     call->id, call->id_len);
                                    hu_hook_result_free(agent->alloc, &seq_hook_res);
                                    continue;
                                }
                                hu_hook_result_free(agent->alloc, &seq_hook_res);
                            }

                            hu_policy_action_t pa = hu_agent_internal_evaluate_tool_policy(
                                agent, pol_tn, call->arguments ? call->arguments : "");
                            bool force_approval =
                                (agent->autonomy_level == HU_AUTONOMY_SUPERVISED) ||
                                (agent->autonomy_level == HU_AUTONOMY_ASSISTED &&
                                 hu_tool_risk_level(pol_tn) >= HU_RISK_MEDIUM);

                            hu_tool_result_t result = hu_tool_result_fail("invalid arguments", 16);
                            if (pa == HU_POLICY_DENY) {
                                if (agent->audit_logger) {
                                    hu_audit_event_t aev;
                                    hu_audit_event_init(&aev, HU_AUDIT_POLICY_VIOLATION);
                                    hu_audit_event_with_identity(
                                        &aev, agent->agent_id,
                                        agent->model_name ? agent->model_name : "unknown", NULL);
                                    hu_audit_event_with_action(&aev, pol_tn, "denied", false,
                                                               false);
                                    hu_audit_logger_log(agent->audit_logger, &aev);
                                }
                                result = hu_tool_result_fail("denied by policy", 16);
                            } else if (pa == HU_POLICY_REQUIRE_APPROVAL || force_approval) {
                                result = hu_tool_result_fail("pending approval", 16);
                                result.needs_approval = true;
                            } else {
                                hu_json_value_t *args = NULL;
                                if (call->arguments_len > 0) {
                                    hu_error_t pe = hu_json_parse(agent->alloc, call->arguments,
                                                                  call->arguments_len, &args);
                                    if (pe == HU_OK && args) {
                                        tool->vtable->execute(tool->ctx, agent->alloc, args,
                                                              &result);
                                        hu_json_free(agent->alloc, args);
                                    }
                                }
                            }

                            /* CausalArmor on sequential path (mirrors parallel path) */
                            if (pa != HU_POLICY_DENY && result.success &&
                                hu_tool_risk_level(pol_tn[0] ? pol_tn : "unknown") >=
                                    HU_RISK_HIGH) {
                                hu_causal_armor_config_t ca_cfg;
                                hu_causal_armor_config_default(&ca_cfg);
                                hu_causal_segment_t ca_segs[8];
                                size_t ca_seg_count = 0;
                                for (size_t hi = agent->history_count; hi > 0 && ca_seg_count < 8;
                                     hi--) {
                                    const hu_owned_message_t *he = &agent->history[hi - 1];
                                    if (he->content && he->content_len > 0) {
                                        ca_segs[ca_seg_count].content = he->content;
                                        ca_segs[ca_seg_count].content_len = he->content_len;
                                        ca_segs[ca_seg_count].is_trusted =
                                            (he->role == HU_ROLE_USER);
                                        ca_seg_count++;
                                    }
                                }
                                if (ca_seg_count > 0) {
                                    const char *args_str = call->arguments ? call->arguments : "";
                                    size_t argl =
                                        call->arguments ? call->arguments_len : strlen(args_str);
                                    hu_causal_armor_result_t ca_result;
                                    if (hu_causal_armor_evaluate(&ca_cfg, ca_segs, ca_seg_count,
                                                                 pol_tn, pol_tn_len, args_str, argl,
                                                                 &ca_result) == HU_OK &&
                                        !ca_result.is_safe) {
                                        static const char ca_msg[] =
                                            "blocked: untrusted content dominates tool decision";
                                        hu_tool_result_free(agent->alloc, &result);
                                        result = hu_tool_result_fail(ca_msg, sizeof(ca_msg) - 1);
                                    }
                                }
                            }

                            /* History scorer on sequential path (mirrors parallel path) */
                            if (pa != HU_POLICY_DENY && result.success &&
                                hu_tool_risk_level(pol_tn[0] ? pol_tn : "unknown") >=
                                    HU_RISK_MEDIUM) {
                                hu_tool_history_entry_t thist[16];
                                size_t thc = 0;
                                for (size_t hi = 0; hi < agent->history_count && thc < 16; hi++) {
                                    const hu_owned_message_t *m = &agent->history[hi];
                                    if (m->role != HU_ROLE_TOOL || !m->name || m->name_len == 0)
                                        continue;
                                    thist[thc].tool_name = m->name;
                                    thist[thc].name_len = m->name_len;
                                    thist[thc].succeeded = !(m->content && m->content_len >= 6 &&
                                                             memcmp(m->content, "denied", 6) == 0);
                                    thist[thc].risk_level = (uint32_t)hu_tool_risk_level(m->name);
                                    thc++;
                                }
                                if (thc > 0) {
                                    hu_history_score_result_t hs;
                                    if (hu_history_scorer_evaluate(
                                            thist, thc, pol_tn, pol_tn_len,
                                            (uint32_t)hu_tool_risk_level(pol_tn[0] ? pol_tn
                                                                                   : "unknown"),
                                            &hs) == HU_OK &&
                                        hs.is_suspicious) {
                                        static const char hs_msg[] =
                                            "blocked: suspicious tool-call history pattern";
                                        hu_tool_result_free(agent->alloc, &result);
                                        result = hu_tool_result_fail(hs_msg, sizeof(hs_msg) - 1);
                                    }
                                }
                            }

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
                                        tool->vtable->execute(tool->ctx, agent->alloc, retry_args,
                                                              &result);
                                        hu_json_free(agent->alloc, retry_args);
                                    }
                                } else {
                                    hu_tool_result_free(agent->alloc, &result);
                                    result = hu_tool_result_fail("user denied action", 18);
                                }
                            }

                            /* Post-hook pipeline (sequential path) */
                            if (agent->hook_registry) {
                                const char *seq_out =
                                    result.success ? result.output : result.error_msg;
                                size_t seq_out_len =
                                    result.success ? result.output_len : result.error_msg_len;
                                hu_hook_result_t seq_post;
                                memset(&seq_post, 0, sizeof(seq_post));
                                const char *seq_args2 = call->arguments ? call->arguments : "";
                                hu_hook_pipeline_post_tool(agent->hook_registry, agent->alloc,
                                                           pol_tn, pol_tn_len, seq_args2,
                                                           strlen(seq_args2), seq_out, seq_out_len,
                                                           result.success, &seq_post);
                                hu_hook_result_free(agent->alloc, &seq_post);
                            }

                            const char *res_content =
                                result.success ? result.output : result.error_msg;
                            size_t res_len =
                                result.success ? result.output_len : result.error_msg_len;

                            /* Capture media path from tool result for channel attachment */
                            if (result.success && result.media_path && result.media_path_len > 0 &&
                                agent->generated_media_count < 4) {
                                char *mp = hu_strndup(agent->alloc, result.media_path,
                                                      result.media_path_len);
                                if (mp)
                                    agent->generated_media[agent->generated_media_count++] = mp;
                            }

                            /* TTL cache store on sequential path */
                            if (seq_cache && result.success && res_content && res_len > 0 &&
                                hu_tool_cache_classify(pol_tn, pol_tn_len, seq_args,
                                                       seq_args_len) != HU_TOOL_CACHE_NEVER) {
                                int64_t ttl = hu_tool_cache_ttl_default_for(pol_tn, pol_tn_len);
                                uint64_t skey = hu_tool_cache_ttl_key(pol_tn, pol_tn_len, seq_args,
                                                                      seq_args_len);
                                (void)hu_tool_cache_ttl_put(seq_cache, skey, res_content, res_len,
                                                            ttl);
                            }

                            hu_error_t hist_err = hu_agent_internal_append_history(
                                agent, HU_ROLE_TOOL, res_content, res_len, call->name,
                                call->name_len, call->id, call->id_len);
                            if (hist_err != HU_OK)
                                hu_log_error("agent_turn", NULL, "history append failed: %s",
                                             hu_error_string(hist_err));

                            if (agent->audit_logger) {
                                hu_audit_event_t aev;
                                hu_audit_event_init(&aev, HU_AUDIT_COMMAND_EXECUTION);
                                hu_audit_event_with_identity(
                                    &aev, agent->agent_id,
                                    agent->model_name ? agent->model_name : "unknown", NULL);
                                hu_audit_event_with_action(&aev, pol_tn, "tool", result.success,
                                                           true);
                                hu_audit_event_with_result(&aev, result.success, 0, 0,
                                                           result.success ? NULL
                                                                          : result.error_msg);
                                hu_audit_logger_log(agent->audit_logger, &aev);
                            }

                            hu_tool_result_free(agent->alloc, &result);
                            if (agent->cancel_requested)
                                break;
                        }
                    }
                    /* Free TTL cache arrays — merged_results owns cached copies;
                     * dispatch_result.results ownership was transferred into merged_results
                     * for partial-cache scenarios, so only free the container. */
                    if (ttl_hits)
                        agent->alloc->free(agent->alloc->ctx, ttl_hits, tc_count * sizeof(bool));
                    if (merged_results) {
                        if (ttl_hit_count > 0 && ttl_hit_count < tc_count &&
                            dispatch_result.results) {
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
        /* Replan on tool failure: if any tool failed and we have a plan, generate
         * a revised plan and inject it as context for the next iteration */
        if (plan_ctx && !agent->cancel_requested) {
            size_t fail_count = 0;
            char fail_detail[512];
            size_t fail_pos = 0;
            for (size_t hi = agent->history_count; hi > 0 && hi > agent->history_count - 8; hi--) {
                if (agent->history[hi - 1].role == HU_ROLE_TOOL && agent->history[hi - 1].content &&
                    agent->history[hi - 1].content_len > 0) {
                    const char *c = agent->history[hi - 1].content;
                    if ((c[0] == 'E' || c[0] == 'e') ||
                        (agent->history[hi - 1].content_len > 6 && memcmp(c, "denied", 6) == 0)) {
                        fail_count++;
                        if (fail_pos < sizeof(fail_detail) - 2) {
                            size_t chunk = agent->history[hi - 1].content_len;
                            if (chunk > 80)
                                chunk = 80;
                            if (fail_pos + chunk + 2 < sizeof(fail_detail)) {
                                memcpy(fail_detail + fail_pos, c, chunk);
                                fail_pos += chunk;
                                fail_detail[fail_pos++] = '\n';
                            }
                        }
                    }
                }
            }
            if (fail_count >= 2) {
                fail_detail[fail_pos] = '\0';
                hu_plan_t *revised = NULL;
                hu_error_t rp_err = hu_planner_replan(
                    agent->alloc, &agent->provider, agent->model_name, agent->model_name_len, msg,
                    msg_len, "partial progress", 16, fail_detail, fail_pos, NULL, 0, &revised);
                if (rp_err == HU_OK && revised && revised->steps_count > 0) {
                    char replan_note[1024];
                    int rn = snprintf(replan_note, sizeof(replan_note),
                                      "[REPLAN after %zu tool failures]: %zu new steps", fail_count,
                                      revised->steps_count);
                    if (rn > 0) {
                        hu_agent_internal_append_history(agent, HU_ROLE_SYSTEM, replan_note,
                                                         (size_t)rn, NULL, 0, NULL, 0);
                    }
                }
                if (revised)
                    hu_plan_free(agent->alloc, revised);
            }
        }

        /* Mid-turn retrieval: after tool results, augment context with
         * memory relevant to both tool output and the evolving conversation */
        if (agent->memory && agent->memory->vtable && agent->history_count > 1 &&
            agent->history[agent->history_count - 1].role == HU_ROLE_TOOL &&
            !agent->cancel_requested) {
            const char *last_result = NULL;
            size_t last_result_len = 0;
            if (agent->history_count > 0) {
                last_result = agent->history[agent->history_count - 1].content;
                last_result_len = agent->history[agent->history_count - 1].content_len;
            }
            if (last_result && last_result_len > 20) {
                char *mid_ctx = NULL;
                size_t mid_ctx_len = 0;
                hu_memory_loader_t mid_loader;
                hu_memory_loader_init(&mid_loader, agent->alloc, agent->memory,
                                      agent->retrieval_engine, 5, 2000);
                hu_memory_loader_set_facade(&mid_loader, agent->w7_facade);
                if (hu_memory_loader_load(&mid_loader, last_result, last_result_len,
                                          agent->memory_session_id ? agent->memory_session_id : "",
                                          agent->memory_session_id ? agent->memory_session_id_len
                                                                   : 0,
                                          &mid_ctx, &mid_ctx_len) == HU_OK &&
                    mid_ctx && mid_ctx_len > 0) {
                    char *note =
                        hu_sprintf(agent->alloc, "[memory context from tool results]: %.*s",
                                   (int)(mid_ctx_len < 2000 ? mid_ctx_len : 2000), mid_ctx);
                    if (note) {
                        hu_error_t hist_err = hu_agent_internal_append_history(
                            agent, HU_ROLE_SYSTEM, note, strlen(note), NULL, 0, NULL, 0);
                        if (hist_err != HU_OK)
                            hu_log_error("agent_turn", NULL, "history append failed: %s",
                                         hu_error_string(hist_err));
                        agent->alloc->free(agent->alloc->ctx, note, strlen(note) + 1);
                    }
                    agent->alloc->free(agent->alloc->ctx, mid_ctx, mid_ctx_len + 1);
                }

                /* Iterative retrieval: also retrieve against original user message
                 * to maintain relevance to the goal across iterations */
                if (iter > 1 && msg_len > 10) {
                    char *goal_ctx = NULL;
                    size_t goal_ctx_len = 0;
                    hu_memory_loader_t goal_loader;
                    hu_memory_loader_init(&goal_loader, agent->alloc, agent->memory,
                                          agent->retrieval_engine, 3, 1000);
                    hu_memory_loader_set_facade(&goal_loader, agent->w7_facade);
                    if (hu_memory_loader_load(
                            &goal_loader, msg, msg_len,
                            agent->memory_session_id ? agent->memory_session_id : "",
                            agent->memory_session_id ? agent->memory_session_id_len : 0, &goal_ctx,
                            &goal_ctx_len) == HU_OK &&
                        goal_ctx && goal_ctx_len > 0) {
                        char *gnote =
                            hu_sprintf(agent->alloc, "[goal-relevant memory]: %.*s",
                                       (int)(goal_ctx_len < 1000 ? goal_ctx_len : 1000), goal_ctx);
                        if (gnote) {
                            hu_agent_internal_append_history(agent, HU_ROLE_SYSTEM, gnote,
                                                             strlen(gnote), NULL, 0, NULL, 0);
                            agent->alloc->free(agent->alloc->ctx, gnote, strlen(gnote) + 1);
                        }
                        agent->alloc->free(agent->alloc->ctx, goal_ctx, goal_ctx_len + 1);
                    }
                }
            }
        }

        /* Scratchpad: persist turn metadata as working memory */
        if (agent->sota.scratchpad.max_bytes > 0) {
            char turn_key[32];
            int tk_n = snprintf(turn_key, sizeof(turn_key), "turn_%d", (int)iter);
            if (tk_n > 0 && (size_t)tk_n < sizeof(turn_key)) {
                char turn_val[256];
                int tv_n = snprintf(turn_val, sizeof(turn_val), "tokens=%u,tools=%zu",
                                    (unsigned)turn_tokens, turn_tool_results_count);
                if (tv_n > 0 && (size_t)tv_n < sizeof(turn_val))
                    hu_scratchpad_set(&agent->sota.scratchpad, agent->alloc, turn_key, (size_t)tk_n,
                                      turn_val, (size_t)tv_n);
            }
        }

        /* Checkpoint: auto-save state after tool iterations */
        if (hu_checkpoint_should_save(&agent->sota.checkpoint_store, (uint32_t)iter)) {
            char cp_state[128];
            int cp_n = snprintf(cp_state, sizeof(cp_state), "{\"iter\":%d,\"tokens\":%llu}",
                                (int)iter, (unsigned long long)agent->total_tokens);
            if (cp_n > 0) {
                if ((size_t)cp_n >= sizeof(cp_state))
                    cp_n = (int)(sizeof(cp_state) - 1);
                static const char cp_task[] = "agent_turn";
                hu_checkpoint_save(&agent->sota.checkpoint_store, agent->alloc, cp_task,
                                   sizeof(cp_task) - 1, (uint32_t)iter, HU_CHECKPOINT_ACTIVE,
                                   cp_state, (size_t)cp_n);
            }
        }

        /* Messages will be reformatted at top of next iteration via arena */
    }

    {
        hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_TOOL_ITERATIONS_EXHAUSTED,
                                  .data = {{0}}};
        ev.data.tool_iterations_exhausted.iterations = agent->max_tool_iterations;
        HU_OBS_SAFE_RECORD_EVENT(agent, &ev);
    }
    {
        hu_observer_event_t ev = {.tag = HU_OBSERVER_EVENT_ERR, .data = {{0}}};
        ev.data.err.component = "agent";
        ev.data.err.message = "tool iterations exhausted";
        HU_OBS_SAFE_RECORD_EVENT(agent, &ev);
    }
    hu_agent_clear_current_for_tools();
    if (dpo_rejected_resp)
        agent->alloc->free(agent->alloc->ctx, dpo_rejected_resp, dpo_rejected_resp_len + 1);
    if (system_prompt)
        agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
    if (routed_specs)
        agent->alloc->free(agent->alloc->ctx, routed_specs,
                           routed_specs_count * sizeof(hu_tool_spec_t));
    if (turn_cache)
        hu_tool_cache_destroy(agent->alloc, turn_cache);
    if (acp_context)
        agent->alloc->free(agent->alloc->ctx, acp_context, acp_context_len + 1);
    if (agent->turn_arena)
        hu_arena_reset(agent->turn_arena);
    return HU_ERR_TIMEOUT;
}
