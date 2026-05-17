#include "human/eval/leaderboard.h"

#include "human/core/json.h"
#include "human/core/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(HU_IS_TEST) && HU_IS_TEST
#define HU_LB_TEST_MODE 1
#else
#define HU_LB_TEST_MODE 0
#endif

typedef struct hu_lb_ctx {
    hu_allocator_t *alloc;
    hu_leaderboard_kind_t kind;
    char *canned_path;
    hu_json_value_t *canned_root;
} hu_lb_ctx_t;

static const char *kind_section(hu_leaderboard_kind_t k) {
    switch (k) {
    case HU_LEADERBOARD_MT_BENCH: return "mt_bench";
    case HU_LEADERBOARD_ALPACA_EVAL: return "alpaca_eval";
    case HU_LEADERBOARD_IFEVAL: return "ifeval";
    default: return "";
    }
}

static const char *kind_name(hu_leaderboard_kind_t k) {
    switch (k) {
    case HU_LEADERBOARD_MT_BENCH: return "mt_bench";
    case HU_LEADERBOARD_ALPACA_EVAL: return "alpaca_eval";
    case HU_LEADERBOARD_IFEVAL: return "ifeval";
    default: return "unknown";
    }
}

static hu_error_t lb_read_file(hu_allocator_t *alloc, const char *path, char **out, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return HU_ERR_IO;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return HU_ERR_IO; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return HU_ERR_IO; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return HU_ERR_IO; }
    char *buf = (char *)alloc->alloc(alloc->ctx, (size_t)sz + 1);
    if (!buf) { fclose(f); return HU_ERR_OUT_OF_MEMORY; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    *out = buf;
    *out_len = rd;
    return HU_OK;
}

static double lb_lookup_canned(const hu_json_value_t *section, const char *prompt) {
    if (!section || section->type != HU_JSON_OBJECT || !prompt) return -1.0;
    hu_json_value_t *v = hu_json_object_get(section, prompt);
    if (!v || v->type != HU_JSON_NUMBER) return -1.0;
    return v->data.number;
}

static hu_error_t lb_run(struct hu_leaderboard_runner *self, hu_allocator_t *alloc,
                         const char *const *prompts, const char *const *responses, size_t n,
                         double *out_scores) {
    (void)responses;
    if (!self || !self->ctx || !alloc || !prompts || !out_scores)
        return HU_ERR_INVALID_ARGUMENT;
    if (n == 0) return HU_OK;

    hu_lb_ctx_t *ctx = (hu_lb_ctx_t *)self->ctx;
    const hu_json_value_t *section = NULL;
    if (ctx->canned_root && ctx->canned_root->type == HU_JSON_OBJECT)
        section = hu_json_object_get(ctx->canned_root, kind_section(ctx->kind));

    for (size_t i = 0; i < n; i++) {
        if (!prompts[i]) return HU_ERR_INVALID_ARGUMENT;
        if (section) {
            double s = lb_lookup_canned(section, prompts[i]);
            if (s < 0.0) return HU_ERR_NOT_SUPPORTED;
            out_scores[i] = s;
            continue;
        }
#if !HU_LB_TEST_MODE
        char cache_path[512];
        unsigned long h = 5381;
        for (const char *p = prompts[i]; *p; p++) h = ((h << 5) + h) + (unsigned char)*p;
        int nw = snprintf(cache_path, sizeof(cache_path),
                          "%s/.human/eval_cache/%s/%lu.json",
                          getenv("HOME") ? getenv("HOME") : ".", kind_name(ctx->kind), h);
        if (nw <= 0 || (size_t)nw >= sizeof(cache_path)) return HU_ERR_NOT_SUPPORTED;
        char *raw = NULL;
        size_t raw_len = 0;
        if (lb_read_file(alloc, cache_path, &raw, &raw_len) != HU_OK) return HU_ERR_NOT_SUPPORTED;
        hu_json_value_t *jv = NULL;
        hu_error_t pe = hu_json_parse(alloc, raw, raw_len, &jv);
        alloc->free(alloc->ctx, raw, raw_len + 1);
        if (pe != HU_OK || !jv || jv->type != HU_JSON_NUMBER) {
            if (jv) hu_json_free(alloc, jv);
            return HU_ERR_NOT_SUPPORTED;
        }
        out_scores[i] = jv->data.number;
        hu_json_free(alloc, jv);
#else
        (void)ctx;
        return HU_ERR_NOT_SUPPORTED;
#endif
    }
    return HU_OK;
}

static const char *lb_name(struct hu_leaderboard_runner *self) {
    if (!self || !self->ctx) return "leaderboard";
    return kind_name(((hu_lb_ctx_t *)self->ctx)->kind);
}

static void lb_deinit(struct hu_leaderboard_runner *self, hu_allocator_t *alloc) {
    if (!self || !self->ctx) return;
    hu_lb_ctx_t *ctx = (hu_lb_ctx_t *)self->ctx;
    if (ctx->canned_root) hu_json_free(alloc, ctx->canned_root);
    if (ctx->canned_path) alloc->free(alloc->ctx, ctx->canned_path, strlen(ctx->canned_path) + 1);
    alloc->free(alloc->ctx, ctx, sizeof(*ctx));
    self->ctx = NULL;
    self->vtable = NULL;
}

static const hu_leaderboard_runner_vtable_t LB_VTABLE = {
    .run = lb_run,
    .name = lb_name,
    .deinit = lb_deinit,
};

static hu_error_t lb_create(hu_allocator_t *alloc, const hu_leaderboard_config_t *cfg,
                            hu_leaderboard_kind_t kind, hu_leaderboard_runner_t *out) {
    if (!alloc || !out) return HU_ERR_INVALID_ARGUMENT;
    hu_lb_ctx_t *ctx = (hu_lb_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*ctx));
    if (!ctx) return HU_ERR_OUT_OF_MEMORY;
    memset(ctx, 0, sizeof(*ctx));
    ctx->alloc = alloc;
    ctx->kind = kind;

    if (cfg && cfg->canned_path) {
        size_t pl = strlen(cfg->canned_path);
        ctx->canned_path = (char *)alloc->alloc(alloc->ctx, pl + 1);
        if (!ctx->canned_path) goto oom;
        memcpy(ctx->canned_path, cfg->canned_path, pl + 1);
        char *raw = NULL;
        size_t raw_len = 0;
        if (lb_read_file(alloc, cfg->canned_path, &raw, &raw_len) != HU_OK) goto oom;
        if (hu_json_parse(alloc, raw, raw_len, &ctx->canned_root) != HU_OK) {
            alloc->free(alloc->ctx, raw, raw_len + 1);
            goto oom;
        }
        alloc->free(alloc->ctx, raw, raw_len + 1);
    }

    out->vtable = &LB_VTABLE;
    out->ctx = ctx;
    return HU_OK;
oom:
    if (ctx->canned_root) hu_json_free(alloc, ctx->canned_root);
    if (ctx->canned_path) alloc->free(alloc->ctx, ctx->canned_path, strlen(ctx->canned_path) + 1);
    alloc->free(alloc->ctx, ctx, sizeof(*ctx));
    return HU_ERR_OUT_OF_MEMORY;
}

hu_error_t hu_leaderboard_create_mt_bench(hu_allocator_t *alloc, const hu_leaderboard_config_t *cfg,
                                          hu_leaderboard_runner_t *out) {
    return lb_create(alloc, cfg, HU_LEADERBOARD_MT_BENCH, out);
}

hu_error_t hu_leaderboard_create_alpaca_eval(hu_allocator_t *alloc,
                                           const hu_leaderboard_config_t *cfg,
                                           hu_leaderboard_runner_t *out) {
    return lb_create(alloc, cfg, HU_LEADERBOARD_ALPACA_EVAL, out);
}

hu_error_t hu_leaderboard_create_ifeval(hu_allocator_t *alloc, const hu_leaderboard_config_t *cfg,
                                        hu_leaderboard_runner_t *out) {
    return lb_create(alloc, cfg, HU_LEADERBOARD_IFEVAL, out);
}
