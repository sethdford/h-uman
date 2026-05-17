#include "human/eval/persona_rollout.h"

#include "human/provider.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_POSIX_VERSION) || defined(__APPLE__)
#include <unistd.h>
#endif

static int cmp_double(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);
}

static double percentile_ms(double *samples, size_t n, double pct) {
    if (!samples || n == 0)
        return 0.0;
    qsort(samples, n, sizeof(double), cmp_double);
    size_t idx = (size_t)((pct / 100.0) * (double)(n - 1) + 0.5);
    if (idx >= n)
        idx = n - 1;
    return samples[idx];
}

static int64_t now_ms_monotonic(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

static hu_error_t provider_chat(hu_provider_t *provider, hu_allocator_t *alloc,
                                const char *system, size_t system_len, const char *user,
                                size_t user_len, char **out, size_t *out_len) {
    if (!provider || !provider->vtable || !alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    if (!provider->vtable->chat_with_system)
        return HU_ERR_NOT_SUPPORTED;
    return provider->vtable->chat_with_system(provider->ctx, alloc, system, system_len, user,
                                              user_len, NULL, 0, 0.7, out, out_len);
}

hu_error_t hu_persona_rollout_load_prompt_fixture(hu_allocator_t *alloc, const char *path,
                                                  char ***out_prompts, size_t *out_n) {
    if (!alloc || !path || !out_prompts || !out_n)
        return HU_ERR_INVALID_ARGUMENT;
    *out_prompts = NULL;
    *out_n = 0;

    FILE *f = fopen(path, "r");
    if (!f)
        return HU_ERR_IO;

    size_t cap = 16;
    size_t n = 0;
    char **lines = (char **)alloc->alloc(alloc->ctx, cap * sizeof(char *));
    if (!lines) {
        fclose(f);
        return HU_ERR_OUT_OF_MEMORY;
    }

    char buf[4096];
    while (fgets(buf, sizeof(buf), f)) {
        size_t len = strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
            buf[--len] = '\0';
        if (len == 0 || buf[0] == '#')
            continue;
        if (n >= cap) {
            cap *= 2;
            char **grown =
                (char **)alloc->realloc(alloc->ctx, lines, n * sizeof(char *), cap * sizeof(char *));
            if (!grown) {
                for (size_t i = 0; i < n; i++)
                    alloc->free(alloc->ctx, lines[i], strlen(lines[i]) + 1);
                alloc->free(alloc->ctx, lines, n * sizeof(char *));
                fclose(f);
                return HU_ERR_OUT_OF_MEMORY;
            }
            lines = grown;
        }
        char *copy = (char *)alloc->alloc(alloc->ctx, len + 1);
        if (!copy) {
            for (size_t i = 0; i < n; i++)
                alloc->free(alloc->ctx, lines[i], strlen(lines[i]) + 1);
            alloc->free(alloc->ctx, lines, n * sizeof(char *));
            fclose(f);
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(copy, buf, len + 1);
        lines[n++] = copy;
    }
    fclose(f);
    *out_prompts = lines;
    *out_n = n;
    return HU_OK;
}

hu_error_t hu_persona_rollout_run(hu_allocator_t *alloc, const hu_persona_rollout_config_t *cfg,
                                  hu_persona_rollout_result_t *out) {
    if (!alloc || !cfg || !out)
        return HU_ERR_INVALID_ARGUMENT;
    if (!cfg->provider || !cfg->target || !cfg->prompts || cfg->n_prompts == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (cfg->target->sample_count == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (cfg->n_prompts < 1)
        return HU_ERR_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));

    hu_provider_t *provider = cfg->provider;
    if (cfg->adapter_path && cfg->adapter_path[0]) {
        const char *slash = strrchr(cfg->adapter_path, '/');
        const char *base = slash ? slash + 1 : cfg->adapter_path;
        hu_error_t le = hu_provider_load_adapter(provider, alloc, cfg->adapter_path,
                                               strlen(cfg->adapter_path), base, strlen(base));
        if (le != HU_OK)
            return le;
    }

    size_t n = cfg->n_prompts;
    out->n_prompts = n;
    out->persona_scores = (double *)alloc->alloc(alloc->ctx, n * sizeof(double));
    double *latencies = (double *)alloc->alloc(alloc->ctx, n * sizeof(double));
    if (!out->persona_scores || !latencies) {
        hu_persona_rollout_result_free(alloc, out);
        alloc->free(alloc->ctx, latencies, n * sizeof(double));
        return HU_ERR_OUT_OF_MEMORY;
    }

    if (cfg->capture_responses) {
        out->responses = (char **)alloc->alloc(alloc->ctx, n * sizeof(char *));
        out->response_lens = (size_t *)alloc->alloc(alloc->ctx, n * sizeof(size_t));
        if (!out->responses || !out->response_lens) {
            hu_persona_rollout_result_free(alloc, out);
            alloc->free(alloc->ctx, latencies, n * sizeof(double));
            return HU_ERR_OUT_OF_MEMORY;
        }
        for (size_t i = 0; i < n; i++) {
            out->responses[i] = NULL;
            out->response_lens[i] = 0;
        }
    }

    size_t lat_n = 0;
    double lat_sum = 0.0;

    for (size_t i = 0; i < n; i++) {
        out->persona_scores[i] = -1.0;
        const char *prompt = cfg->prompts[i];
        if (!prompt || !prompt[0]) {
            out->n_errors++;
            continue;
        }
        const char *sys = (cfg->system_prompts && cfg->system_prompts[i]) ? cfg->system_prompts[i] : "";
        size_t sys_len = strlen(sys);

        int64_t t0 = now_ms_monotonic();
        char *content = NULL;
        size_t content_len = 0;
        hu_error_t ce = provider_chat(provider, alloc, sys, sys_len, prompt, strlen(prompt),
                                      &content, &content_len);
        int64_t t1 = now_ms_monotonic();
        int64_t elapsed = t1 - t0;
        if (cfg->timeout_ms_per_prompt > 0 && elapsed > cfg->timeout_ms_per_prompt) {
            if (content)
                alloc->free(alloc->ctx, content, content_len + 1);
            out->n_errors++;
            continue;
        }

        if (ce != HU_OK || !content || content_len == 0) {
            if (content)
                alloc->free(alloc->ctx, content, content_len + 1);
            out->n_errors++;
            continue;
        }

        float fid = hu_communication_style_fidelity_score_v2(cfg->target, content, content_len);
        double score = (double)fid;
        if (score < 0.0)
            score = 0.0;
        if (score > 1.0)
            score = 1.0;
        out->persona_scores[i] = score;
        out->n_scored++;

        latencies[lat_n] = (double)elapsed;
        lat_sum += latencies[lat_n];
        lat_n++;

        if (cfg->capture_responses && out->responses) {
            out->responses[i] = content;
            out->response_lens[i] = content_len;
            content = NULL;
        } else if (content) {
            alloc->free(alloc->ctx, content, content_len + 1);
        }
    }

    if (lat_n > 0) {
        out->mean_ms = lat_sum / (double)lat_n;
        out->p95_ms = percentile_ms(latencies, lat_n, 95.0);
    }
    alloc->free(alloc->ctx, latencies, n * sizeof(double));
    return HU_OK;
}

void hu_persona_rollout_result_free(hu_allocator_t *alloc, hu_persona_rollout_result_t *r) {
    if (!alloc || !r)
        return;
    if (r->persona_scores) {
        size_t cap = r->n_prompts > 0 ? r->n_prompts : r->n_scored;
        alloc->free(alloc->ctx, r->persona_scores, cap * sizeof(double));
        r->persona_scores = NULL;
    }
    if (r->responses && r->response_lens) {
        size_t cap = r->n_prompts > 0 ? r->n_prompts : r->n_scored;
        for (size_t i = 0; i < cap; i++) {
            if (r->responses[i])
                alloc->free(alloc->ctx, r->responses[i], r->response_lens[i] + 1);
        }
        alloc->free(alloc->ctx, r->responses, cap * sizeof(char *));
        alloc->free(alloc->ctx, r->response_lens, cap * sizeof(size_t));
        r->responses = NULL;
        r->response_lens = NULL;
    }
    memset(r, 0, sizeof(*r));
}
