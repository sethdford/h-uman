#include "human/agent/swarm.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/provider.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#if !(defined(HU_IS_TEST) && HU_IS_TEST)

static hu_error_t swarm_provider_chat_tools(void *pctx, const hu_provider_vtable_t *vt,
                                            hu_allocator_t *alloc, hu_chat_request_t *req,
                                            hu_chat_response_t *resp) {
    if (vt->chat_with_tools)
        return vt->chat_with_tools(pctx, alloc, req, resp);
    return vt->chat(pctx, alloc, req, req->model, req->model_len, req->temperature, resp);
}

static void swarm_run_one_task(hu_allocator_t *alloc, const hu_swarm_config_t *cfg,
                               hu_swarm_task_t *t) {
    t->completed = false;
    t->failed = false;

    if (!cfg->provider || !cfg->provider->vtable || !cfg->provider->vtable->chat_with_system) {
        t->completed = true;
        t->failed = false;
        int n =
            snprintf(t->result, sizeof(t->result), "processed: %.*s",
                     (int)(t->description_len < sizeof(t->result) - 16u ? t->description_len
                                                                        : sizeof(t->result) - 16u),
                     t->description);
        t->result_len =
            (n > 0 && (size_t)n < sizeof(t->result)) ? (size_t)n : sizeof(t->result) - 1u;
        t->result[t->result_len] = '\0';
        return;
    }

    const hu_provider_vtable_t *vt = cfg->provider->vtable;
    void *pctx = cfg->provider->ctx;

    hu_tool_spec_t *specs = NULL;
    size_t specs_count = 0;
    if (cfg->tools && cfg->tools_count > 0 && vt->chat) {
        specs =
            (hu_tool_spec_t *)alloc->alloc(alloc->ctx, cfg->tools_count * sizeof(hu_tool_spec_t));
        if (specs) {
            for (size_t ti = 0; ti < cfg->tools_count; ti++) {
                if (!cfg->tools[ti].vtable || !cfg->tools[ti].vtable->name ||
                    !cfg->tools[ti].vtable->parameters_json)
                    continue;
                const char *nm = cfg->tools[ti].vtable->name(cfg->tools[ti].ctx);
                const char *desc = cfg->tools[ti].vtable->description
                                       ? cfg->tools[ti].vtable->description(cfg->tools[ti].ctx)
                                       : "";
                const char *pj = cfg->tools[ti].vtable->parameters_json(cfg->tools[ti].ctx);
                if (!nm || !pj)
                    continue;
                specs[specs_count].name = nm;
                specs[specs_count].name_len = strlen(nm);
                specs[specs_count].description = desc;
                specs[specs_count].description_len = desc ? strlen(desc) : 0;
                specs[specs_count].parameters_json = pj;
                specs[specs_count].parameters_json_len = strlen(pj);
                specs_count++;
            }
        }
    }

    static const char sys[] = "You are a focused sub-agent working on a specific subtask. "
                              "Use available tools to gather information and complete the task. "
                              "Return your final answer concisely.";

    char context[4096];
    size_t dlen = t->description_len;
    if (dlen >= sizeof(context))
        dlen = sizeof(context) - 1u;
    memcpy(context, t->description, dlen);
    context[dlen] = '\0';
    size_t ctx_pos = dlen;

    const char *model = cfg->model ? cfg->model : "";
    size_t model_len = cfg->model_len;

    bool got_final = false;
    for (int iter = 0; iter < 3 && !got_final; iter++) {
        if (specs_count > 0 && vt->chat) {
            hu_chat_message_t msgs[2];
            memset(&msgs, 0, sizeof(msgs));
            msgs[0].role = HU_ROLE_SYSTEM;
            msgs[0].content = sys;
            msgs[0].content_len = sizeof(sys) - 1u;

            msgs[1].role = HU_ROLE_USER;
            msgs[1].content = context;
            msgs[1].content_len = ctx_pos;

            hu_chat_request_t req = {0};
            req.messages = msgs;
            req.messages_count = 2;
            req.model = model;
            req.model_len = model_len;
            req.temperature = 0.0;
            req.tools = specs;
            req.tools_count = specs_count;

            hu_chat_response_t resp = {0};
            hu_error_t err = swarm_provider_chat_tools(pctx, vt, alloc, &req, &resp);
            if (err != HU_OK) {
                t->failed = true;
                t->completed = false;
                int n = snprintf(t->result, sizeof(t->result), "sub-agent failed: %s",
                                 hu_error_string(err));
                t->result_len = n > 0 ? (size_t)n : 0;
                hu_chat_response_free(alloc, &resp);
                goto done;
            }

            if (resp.tool_calls && resp.tool_calls_count > 0) {
                for (size_t tci = 0; tci < resp.tool_calls_count && tci < 4u; tci++) {
                    const char *call_name = resp.tool_calls[tci].name;
                    size_t call_len = resp.tool_calls[tci].name_len;
                    if (!call_name || call_len == 0)
                        continue;

                    hu_tool_t *tool = NULL;
                    for (size_t ti = 0; ti < cfg->tools_count; ti++) {
                        if (!cfg->tools[ti].vtable || !cfg->tools[ti].vtable->name)
                            continue;
                        const char *tn = cfg->tools[ti].vtable->name(cfg->tools[ti].ctx);
                        if (!tn)
                            continue;
                        size_t tn_len = strlen(tn);
                        if (tn_len == call_len && memcmp(tn, call_name, call_len) == 0) {
                            tool = &cfg->tools[ti];
                            break;
                        }
                    }
                    if (tool && tool->vtable->execute) {
                        hu_json_value_t *args = NULL;
                        if (resp.tool_calls[tci].arguments &&
                            resp.tool_calls[tci].arguments_len > 0) {
                            (void)hu_json_parse(alloc, resp.tool_calls[tci].arguments,
                                                resp.tool_calls[tci].arguments_len, &args);
                        }
                        hu_tool_result_t tr = {0};
                        if (args)
                            tool->vtable->execute(tool->ctx, alloc, args, &tr);
                        if (args)
                            hu_json_free(alloc, args);

                        if (tr.output && tr.output_len > 0 && ctx_pos < sizeof(context) - 1u) {
                            size_t room = sizeof(context) - 1u - ctx_pos;
                            int add = snprintf(
                                context + ctx_pos, room + 1u, "\n[Tool %.*s result]: %.*s",
                                (int)(call_len < 64 ? (int)call_len : 64), call_name,
                                (int)(tr.output_len < 512u ? (int)tr.output_len : 512), tr.output);
                            if (add > 0) {
                                if ((size_t)add < room + 1u)
                                    ctx_pos += (size_t)add;
                                else
                                    ctx_pos = sizeof(context) - 1u;
                            }
                        }
                        hu_tool_result_free(alloc, &tr);
                    }
                }
                hu_chat_response_free(alloc, &resp);
                continue;
            }

            if (resp.content && resp.content_len > 0) {
                t->completed = true;
                t->failed = false;
                size_t copy_len = resp.content_len < sizeof(t->result) - 1u
                                      ? resp.content_len
                                      : sizeof(t->result) - 1u;
                memcpy(t->result, resp.content, copy_len);
                t->result[copy_len] = '\0';
                t->result_len = copy_len;
                hu_chat_response_free(alloc, &resp);
                got_final = true;
                break;
            }
            hu_chat_response_free(alloc, &resp);
            break;
        } else {
            /* No tools or no vt->chat: single chat_with_system call */
            char *llm_out = NULL;
            size_t llm_out_len = 0;
            hu_error_t err =
                vt->chat_with_system(pctx, alloc, sys, sizeof(sys) - 1u, context, ctx_pos, model,
                                     model_len, 0.0, &llm_out, &llm_out_len);
            if (err == HU_OK && llm_out && llm_out_len > 0) {
                t->completed = true;
                t->failed = false;
                size_t copy_len =
                    llm_out_len < sizeof(t->result) - 1u ? llm_out_len : sizeof(t->result) - 1u;
                memcpy(t->result, llm_out, copy_len);
                t->result[copy_len] = '\0';
                t->result_len = copy_len;
                alloc->free(alloc->ctx, llm_out, llm_out_len + 1u);
            } else {
                t->completed = false;
                t->failed = true;
                int n = snprintf(t->result, sizeof(t->result), "sub-agent failed: %s",
                                 hu_error_string(err));
                t->result_len = n > 0 ? (size_t)n : 0;
                if (llm_out)
                    alloc->free(alloc->ctx, llm_out, llm_out_len + 1u);
            }
            got_final = true;
        }
    }

    if (!got_final && !t->failed) {
        t->completed = true;
        t->failed = false;
        size_t copy_len = ctx_pos < sizeof(t->result) - 1u ? ctx_pos : sizeof(t->result) - 1u;
        memcpy(t->result, context, copy_len);
        t->result[copy_len] = '\0';
        t->result_len = copy_len;
    }

done:
    if (specs)
        alloc->free(alloc->ctx, specs, cfg->tools_count * sizeof(hu_tool_spec_t));
}

#if defined(__unix__) || defined(__APPLE__)
#include <pthread.h>
#include <sys/time.h>

typedef struct {
    hu_allocator_t *alloc;
    const hu_swarm_config_t *config;
    const hu_swarm_task_t *tasks;
    hu_swarm_task_t *result_tasks;
    size_t task_count;
    pthread_mutex_t next_mu;
    size_t next_idx;
    size_t *completed;
    size_t *failed;
} swarm_ctx_t;

static void *swarm_worker(void *arg) {
    swarm_ctx_t *ctx = (swarm_ctx_t *)arg;
    struct timeval tv_start, tv_end;
    for (;;) {
        size_t idx;
        pthread_mutex_lock(&ctx->next_mu);
        idx = ctx->next_idx;
        if (idx >= ctx->task_count) {
            pthread_mutex_unlock(&ctx->next_mu);
            break;
        }
        ctx->next_idx++;
        pthread_mutex_unlock(&ctx->next_mu);

        hu_swarm_task_t *t = &ctx->result_tasks[idx];
        *t = ctx->tasks[idx];
        gettimeofday(&tv_start, NULL);

        swarm_run_one_task(ctx->alloc, ctx->config, t);

        gettimeofday(&tv_end, NULL);
        t->elapsed_ms = (int64_t)(tv_end.tv_sec - tv_start.tv_sec) * 1000 +
                        (int64_t)(tv_end.tv_usec - tv_start.tv_usec) / 1000;
        if (t->elapsed_ms < 1)
            t->elapsed_ms = 1;

        pthread_mutex_lock(&ctx->next_mu);
        (*ctx->completed)++;
        if (t->failed)
            (*ctx->failed)++;
        pthread_mutex_unlock(&ctx->next_mu);
    }
    return NULL;
}
#endif
#endif /* !(HU_IS_TEST) */

hu_swarm_config_t hu_swarm_config_default(void) {
    hu_swarm_config_t c = {0};
    c.max_parallel = 4;
    c.timeout_ms = 30000;
    c.retry_on_failure = 1;
    return c;
}

hu_error_t hu_swarm_execute(hu_allocator_t *alloc, const hu_swarm_config_t *config,
                            hu_swarm_task_t *tasks, size_t task_count, hu_swarm_result_t *result) {
    if (!alloc || !result)
        return HU_ERR_INVALID_ARGUMENT;
    if (task_count > 0 && !tasks)
        return HU_ERR_INVALID_ARGUMENT;

    memset(result, 0, sizeof(*result));
    result->tasks = NULL;
    result->task_count = 0;
    result->completed = 0;
    result->failed = 0;
    result->total_elapsed_ms = 0;

    if (task_count == 0)
        return HU_OK;

    hu_swarm_config_t cfg = config ? *config : hu_swarm_config_default();

    result->tasks =
        (hu_swarm_task_t *)alloc->alloc(alloc->ctx, task_count * sizeof(hu_swarm_task_t));
    if (!result->tasks)
        return HU_ERR_OUT_OF_MEMORY;

    result->task_count = task_count;

#if defined(HU_IS_TEST) && HU_IS_TEST
    int64_t total_ms = 0;
    int max_retries = cfg.retry_on_failure > 0 ? cfg.retry_on_failure : 0;
    for (size_t i = 0; i < task_count; i++) {
        hu_swarm_task_t *t = &result->tasks[i];
        *t = tasks[i];

        /* Simulate failure for tasks containing "fail" in description */
        bool sim_fail = (t->description_len >= 4 && strstr(t->description, "fail") != NULL);
        bool task_done = false;

        for (int attempt = 0; attempt <= max_retries && !task_done; attempt++) {
            t->elapsed_ms = 1;
            if (sim_fail && attempt < max_retries) {
                continue;
            }
            if (sim_fail) {
                t->completed = false;
                t->failed = true;
                strncpy(t->result, "error: task failed", sizeof(t->result) - 1);
                t->result_len = 18;
                result->failed++;
            } else {
                t->completed = true;
                t->failed = false;
                strncpy(t->result, "mock result", sizeof(t->result) - 1);
                t->result_len = 11;
                result->completed++;
            }
            task_done = true;
        }

        /* Timeout enforcement: mark timed out if elapsed > timeout */
        if (cfg.timeout_ms > 0 && t->elapsed_ms > cfg.timeout_ms) {
            t->completed = false;
            t->failed = true;
        }

        total_ms += t->elapsed_ms;
    }
    result->total_elapsed_ms = total_ms;

#elif defined(__unix__) || defined(__APPLE__)
    /* POSIX: pthread-based parallel execution */
    swarm_ctx_t ctx = {
        .alloc = alloc,
        .config = &cfg,
        .tasks = tasks,
        .result_tasks = result->tasks,
        .task_count = task_count,
        .next_idx = 0,
        .completed = &result->completed,
        .failed = &result->failed,
    };
    if (pthread_mutex_init(&ctx.next_mu, NULL) != 0) {
        alloc->free(alloc->ctx, result->tasks, task_count * sizeof(hu_swarm_task_t));
        result->tasks = NULL;
        result->task_count = 0;
        return HU_ERR_IO;
    }

    size_t num_threads = (size_t)cfg.max_parallel;
    if (num_threads > task_count)
        num_threads = task_count;
    if (num_threads < 1)
        num_threads = 1;

    pthread_t *threads = (pthread_t *)alloc->alloc(alloc->ctx, num_threads * sizeof(pthread_t));
    if (!threads) {
        pthread_mutex_destroy(&ctx.next_mu);
        alloc->free(alloc->ctx, result->tasks, task_count * sizeof(hu_swarm_task_t));
        result->tasks = NULL;
        result->task_count = 0;
        return HU_ERR_OUT_OF_MEMORY;
    }

    for (size_t i = 0; i < num_threads; i++) {
        if (pthread_create(&threads[i], NULL, swarm_worker, &ctx) != 0) {
            for (size_t j = 0; j < i; j++)
                pthread_join(threads[j], NULL);
            alloc->free(alloc->ctx, threads, num_threads * sizeof(pthread_t));
            pthread_mutex_destroy(&ctx.next_mu);
            alloc->free(alloc->ctx, result->tasks, task_count * sizeof(hu_swarm_task_t));
            result->tasks = NULL;
            result->task_count = 0;
            return HU_ERR_IO;
        }
    }

    int64_t total_ms = 0;
    for (size_t i = 0; i < num_threads; i++)
        pthread_join(threads[i], NULL);
    alloc->free(alloc->ctx, threads, num_threads * sizeof(pthread_t));
    pthread_mutex_destroy(&ctx.next_mu);

    for (size_t i = 0; i < task_count; i++)
        total_ms += result->tasks[i].elapsed_ms;
    result->total_elapsed_ms = total_ms;

#else
    /* Non-POSIX: sequential fallback */
    int64_t total_ms = 0;
    for (size_t i = 0; i < task_count; i++) {
        hu_swarm_task_t *t = &result->tasks[i];
        *t = tasks[i];
        swarm_run_one_task(alloc, &cfg, t);
        t->elapsed_ms = 1;
        total_ms += t->elapsed_ms;
        result->completed++;
        if (t->failed)
            result->failed++;
    }
    result->total_elapsed_ms = total_ms;
#endif

    return HU_OK;
}

#define HU_SWARM_JACCARD_MAX_WORDS 64
#define HU_SWARM_JACCARD_WORD_LEN  48
#define HU_SWARM_VOTE_MAX_INDICES  128

static int swarm_vote_extract_words(const char *s, size_t s_len,
                                    char words[][HU_SWARM_JACCARD_WORD_LEN]) {
    int n = 0;
    size_t i = 0;
    while (i < s_len && n < HU_SWARM_JACCARD_MAX_WORDS) {
        while (i < s_len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
            i++;
        if (i >= s_len)
            break;
        size_t start = i;
        while (i < s_len && !(s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
            i++;
        size_t wlen = i - start;
        if (wlen == 0)
            continue;
        if (wlen >= HU_SWARM_JACCARD_WORD_LEN)
            wlen = HU_SWARM_JACCARD_WORD_LEN - 1u;
        bool dup = false;
        for (int k = 0; k < n; k++) {
            if (strncmp(words[k], s + start, wlen) == 0 && words[k][wlen] == '\0') {
                dup = true;
                break;
            }
        }
        if (dup)
            continue;
        memcpy(words[n], s + start, wlen);
        words[n][wlen] = '\0';
        n++;
    }
    return n;
}

static double swarm_jaccard_similarity(const char *a, size_t a_len, const char *b, size_t b_len) {
    char wa[HU_SWARM_JACCARD_MAX_WORDS][HU_SWARM_JACCARD_WORD_LEN];
    char wb[HU_SWARM_JACCARD_MAX_WORDS][HU_SWARM_JACCARD_WORD_LEN];
    int na = swarm_vote_extract_words(a, a_len, wa);
    int nb = swarm_vote_extract_words(b, b_len, wb);
    if (na == 0 && nb == 0)
        return 1.0;
    if (na == 0 || nb == 0)
        return 0.0;
    int inter = 0;
    for (int i = 0; i < na; i++) {
        for (int j = 0; j < nb; j++) {
            if (strcmp(wa[i], wb[j]) == 0) {
                inter++;
                break;
            }
        }
    }
    int uni = na + nb - inter;
    if (uni <= 0)
        return 0.0;
    return (double)inter / (double)uni;
}

static int swarm_vote_find_root(int *parent, int i) {
    while (parent[i] != i) {
        parent[i] = parent[parent[i]];
        i = parent[i];
    }
    return i;
}

static void swarm_vote_union(int *parent, int a, int b) {
    int ra = swarm_vote_find_root(parent, a);
    int rb = swarm_vote_find_root(parent, b);
    if (ra != rb)
        parent[rb] = ra;
}

static double swarm_vote_pair_similarity(const hu_swarm_task_t *x, const hu_swarm_task_t *y) {
    if (x->result_len == y->result_len && memcmp(x->result, y->result, x->result_len) == 0)
        return 1.0;
    return swarm_jaccard_similarity(x->result, x->result_len, y->result, y->result_len);
}

hu_error_t hu_swarm_aggregate(const hu_swarm_result_t *result, hu_swarm_aggregation_t strategy,
                              char *out, size_t out_size, size_t *out_len) {
    if (!result || !out || !out_len || out_size == 0)
        return HU_ERR_INVALID_ARGUMENT;

    out[0] = '\0';
    *out_len = 0;

    if (result->task_count == 0 || !result->tasks)
        return HU_OK;

    switch (strategy) {
    case HU_SWARM_AGG_CONCATENATE: {
        size_t pos = 0;
        for (size_t i = 0; i < result->task_count && pos < out_size - 1; i++) {
            if (!result->tasks[i].completed || result->tasks[i].result_len == 0)
                continue;
            if (pos > 0 && pos < out_size - 2) {
                out[pos++] = '\n';
            }
            size_t to_copy = result->tasks[i].result_len;
            if (to_copy > out_size - 1 - pos)
                to_copy = out_size - 1 - pos;
            memcpy(out + pos, result->tasks[i].result, to_copy);
            pos += to_copy;
        }
        out[pos] = '\0';
        *out_len = pos;
        break;
    }
    case HU_SWARM_AGG_FIRST_SUCCESS: {
        for (size_t i = 0; i < result->task_count; i++) {
            if (result->tasks[i].completed && !result->tasks[i].failed &&
                result->tasks[i].result_len > 0) {
                size_t to_copy = result->tasks[i].result_len;
                if (to_copy > out_size - 1)
                    to_copy = out_size - 1;
                memcpy(out, result->tasks[i].result, to_copy);
                out[to_copy] = '\0';
                *out_len = to_copy;
                return HU_OK;
            }
        }
        break;
    }
    case HU_SWARM_AGG_VOTE: {
        /* Similarity-threshold clustering (Jaccard on word sets) + exact-match fast path;
         * largest cluster wins; centroid = highest mean pairwise similarity within cluster. */
        size_t idxs[HU_SWARM_VOTE_MAX_INDICES];
        size_t nv = 0;
        for (size_t i = 0; i < result->task_count && nv < HU_SWARM_VOTE_MAX_INDICES; i++) {
            if (!result->tasks[i].completed || result->tasks[i].result_len == 0)
                continue;
            idxs[nv++] = i;
        }
        if (nv == 0)
            break;
        if (nv == 1) {
            const hu_swarm_task_t *t = &result->tasks[idxs[0]];
            size_t to_copy = t->result_len < out_size - 1 ? t->result_len : out_size - 1;
            memcpy(out, t->result, to_copy);
            out[to_copy] = '\0';
            *out_len = to_copy;
            break;
        }

        int parent[HU_SWARM_VOTE_MAX_INDICES];
        for (size_t i = 0; i < nv; i++)
            parent[i] = (int)i;

        for (size_t ii = 0; ii < nv; ii++) {
            for (size_t jj = ii + 1; jj < nv; jj++) {
                double sim =
                    swarm_vote_pair_similarity(&result->tasks[idxs[ii]], &result->tasks[idxs[jj]]);
                if (sim >= 1.0 || sim > 0.5)
                    swarm_vote_union(parent, (int)ii, (int)jj);
            }
        }

        int best_sz = 0;
        int wr_best = -1;
        for (size_t i = 0; i < nv; i++) {
            int ri = swarm_vote_find_root(parent, (int)i);
            int sz = 0;
            for (size_t j = 0; j < nv; j++) {
                if (swarm_vote_find_root(parent, (int)j) == ri)
                    sz++;
            }
            if (sz > best_sz || (sz == best_sz && (wr_best < 0 || ri < wr_best))) {
                best_sz = sz;
                wr_best = ri;
            }
        }

        if (wr_best < 0)
            break;

        double best_avg = -1.0;
        size_t pick_orig = idxs[0];
        for (size_t ii = 0; ii < nv; ii++) {
            if (swarm_vote_find_root(parent, (int)ii) != wr_best)
                continue;
            double sum = 0.0;
            int cnt = 0;
            for (size_t jj = 0; jj < nv; jj++) {
                if (swarm_vote_find_root(parent, (int)jj) != wr_best || jj == ii)
                    continue;
                sum +=
                    swarm_vote_pair_similarity(&result->tasks[idxs[ii]], &result->tasks[idxs[jj]]);
                cnt++;
            }
            double avg = cnt > 0 ? sum / (double)cnt : 1.0;
            if (avg > best_avg || (avg == best_avg && idxs[ii] < pick_orig)) {
                best_avg = avg;
                pick_orig = idxs[ii];
            }
        }

        const hu_swarm_task_t *win = &result->tasks[pick_orig];
        if (win->result_len > 0) {
            size_t to_copy = win->result_len < out_size - 1 ? win->result_len : out_size - 1;
            memcpy(out, win->result, to_copy);
            out[to_copy] = '\0';
            *out_len = to_copy;
        }
        break;
    }
    case HU_SWARM_AGG_LLM_SYNTHESIZE:
        /* LLM synthesis requires a provider — use hu_swarm_aggregate_llm() directly.
         * Fall back to concatenation here. */
        return hu_swarm_aggregate(result, HU_SWARM_AGG_CONCATENATE, out, out_size, out_len);
    }

    return HU_OK;
}

hu_error_t hu_swarm_aggregate_llm(hu_allocator_t *alloc, const hu_swarm_result_t *result,
                                  hu_provider_t *provider, const char *model, size_t model_len,
                                  char *out, size_t out_size, size_t *out_len) {
    if (!alloc || !result || !out || !out_len || out_size == 0)
        return HU_ERR_INVALID_ARGUMENT;
    out[0] = '\0';
    *out_len = 0;

    if (result->task_count == 0 || !result->tasks)
        return HU_OK;

    /* Collect completed results */
    size_t valid = 0;
    for (size_t i = 0; i < result->task_count; i++) {
        if (result->tasks[i].completed && result->tasks[i].result_len > 0)
            valid++;
    }
    if (valid == 0)
        return HU_OK;
    if (valid == 1) {
        /* Single result — return directly */
        for (size_t i = 0; i < result->task_count; i++) {
            if (result->tasks[i].completed && result->tasks[i].result_len > 0) {
                size_t to_copy = result->tasks[i].result_len;
                if (to_copy > out_size - 1)
                    to_copy = out_size - 1;
                memcpy(out, result->tasks[i].result, to_copy);
                out[to_copy] = '\0';
                *out_len = to_copy;
                return HU_OK;
            }
        }
    }

    /* No provider — fall back to concatenation */
    if (!provider || !provider->vtable || !provider->vtable->chat_with_system)
        return hu_swarm_aggregate(result, HU_SWARM_AGG_CONCATENATE, out, out_size, out_len);

    /* Build synthesis prompt with all results */
    char prompt[8192];
    int plen = snprintf(
        prompt, sizeof(prompt),
        "You are synthesizing results from %zu parallel sub-agents into one coherent response.\n\n"
        "Sub-agent results:\n",
        valid);

    size_t idx = 0;
    for (size_t i = 0; i < result->task_count && plen < (int)sizeof(prompt) - 256; i++) {
        if (!result->tasks[i].completed || result->tasks[i].result_len == 0)
            continue;
        idx++;
        int added = snprintf(
            prompt + plen, sizeof(prompt) - (size_t)plen,
            "\n--- Agent %zu (task: %.*s) ---\n%.*s\n", idx,
            (int)(result->tasks[i].description_len < 100 ? result->tasks[i].description_len : 100),
            result->tasks[i].description,
            (int)(result->tasks[i].result_len < 1500 ? result->tasks[i].result_len : 1500),
            result->tasks[i].result);
        if (added > 0)
            plen += added;
    }
    int added =
        snprintf(prompt + plen, sizeof(prompt) - (size_t)plen,
                 "\nSynthesize these into a single coherent, comprehensive response. "
                 "Resolve any contradictions. Keep the most relevant and accurate information.");
    if (added > 0)
        plen += added;

    static const char sys[] = "You synthesize multiple agent outputs into one coherent answer.";
    char *llm_out = NULL;
    size_t llm_out_len = 0;
    hu_error_t err = provider->vtable->chat_with_system(provider->ctx, alloc, sys, sizeof(sys) - 1u,
                                                        prompt, (size_t)plen, model ? model : "",
                                                        model_len, 0.0, &llm_out, &llm_out_len);

    if (err != HU_OK || !llm_out || llm_out_len == 0) {
        if (llm_out)
            alloc->free(alloc->ctx, llm_out, llm_out_len + 1);
        return hu_swarm_aggregate(result, HU_SWARM_AGG_CONCATENATE, out, out_size, out_len);
    }

    size_t to_copy = llm_out_len < out_size - 1 ? llm_out_len : out_size - 1;
    memcpy(out, llm_out, to_copy);
    out[to_copy] = '\0';
    *out_len = to_copy;
    alloc->free(alloc->ctx, llm_out, llm_out_len + 1);
    return HU_OK;
}

void hu_swarm_result_free(hu_allocator_t *alloc, hu_swarm_result_t *result) {
    if (!alloc || !result)
        return;
    if (result->tasks) {
        alloc->free(alloc->ctx, result->tasks, result->task_count * sizeof(hu_swarm_task_t));
        result->tasks = NULL;
        result->task_count = 0;
        result->completed = 0;
        result->failed = 0;
    }
}
