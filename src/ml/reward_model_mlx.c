/* src/ml/reward_model_mlx.c — Phase 3 Task 8
 *
 * Apple-only MLX subprocess backend for hu_reward_model_t. Delegates
 * inference to scripts/rm_mlx_train.py --infer via popen(), parsing the
 * scalar score from stdout. Mirrors the subprocess pattern from
 * src/ml/dpo_real_mlx.c (Phase 2 Task 6): single-quote shell escaping
 * with quote-rejection, HU_IS_TEST guards, __APPLE__ gating.
 *
 * Training lives entirely in scripts/rm_mlx_train.py --train (invoked
 * from the CLI, not from this vtable). The C side only does inference.
 */
#include "human/ml/reward_model.h"
#include "human/core/allocator.h"
#include "human/core/error.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char model_path[512];
    char value_head_path[512];
} rm_mlx_ctx_t;

static int mlx_lm_available(void) {
    return system("python3 -c 'import mlx_lm' 2>/dev/null") == 0;
}

static hu_error_t rm_mlx_score(void *vctx, hu_allocator_t *alloc,
                                const char *prompt, size_t prompt_len,
                                const char *response, size_t response_len,
                                double *out_score) {
    if (!vctx || !alloc || !prompt || prompt_len == 0
        || !response || response_len == 0 || !out_score) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    rm_mlx_ctx_t *c = (rm_mlx_ctx_t *)vctx;

    if (strchr(c->model_path, '\'') || strchr(c->value_head_path, '\'')) {
        return HU_ERR_INVALID_ARGUMENT;
    }

    /* Build a sanitized copy of prompt and response for shell argument.
     * Reject single quotes in user text to prevent shell injection. */
    if (memchr(prompt, '\'', prompt_len) || memchr(response, '\'', response_len)) {
        return HU_ERR_INVALID_ARGUMENT;
    }

    /* Allocate buffers for the null-terminated copies. */
    char *p_buf = (char *)alloc->alloc(alloc->ctx, prompt_len + 1);
    if (!p_buf) return HU_ERR_OUT_OF_MEMORY;
    memcpy(p_buf, prompt, prompt_len);
    p_buf[prompt_len] = '\0';

    char *r_buf = (char *)alloc->alloc(alloc->ctx, response_len + 1);
    if (!r_buf) {
        alloc->free(alloc->ctx, p_buf, prompt_len + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(r_buf, response, response_len);
    r_buf[response_len] = '\0';

    char cmd[4096];
    int n = snprintf(cmd, sizeof(cmd),
                     "python3 scripts/rm_mlx_train.py --infer "
                     "--backbone '%s' "
                     "--value-head '%s' "
                     "--prompt '%s' "
                     "--response '%s' "
                     "2>/dev/null",
                     c->model_path, c->value_head_path, p_buf, r_buf);

    alloc->free(alloc->ctx, p_buf, prompt_len + 1);
    alloc->free(alloc->ctx, r_buf, response_len + 1);

    if (n < 0 || (size_t)n >= sizeof(cmd)) return HU_ERR_INVALID_ARGUMENT;

/* Phase 3 audit fold-in (critic MEDIUM-3): `#if HU_IS_TEST` is the repo
 * standard (numeric check). `defined(HU_IS_TEST)` is true when the build
 * system explicitly defines HU_IS_TEST=0 to disable test mode, which
 * would mistakenly route real scoring requests through the dummy path. */
#if HU_IS_TEST && !defined(HU_HAVE_MLX_LM)
    *out_score = 0.42;
    return HU_OK;
#endif

    FILE *fp = popen(cmd, "r");
    if (!fp) return HU_ERR_IO;

    char buf[256];
    double score = 0.0;
    int got_score = 0;
    while (fgets(buf, sizeof(buf), fp)) {
        char *endp = NULL;
        double v = strtod(buf, &endp);
        if (endp != buf) {
            score = v;
            got_score = 1;
        }
    }
    int status = pclose(fp);
    if (status != 0 || !got_score) return HU_ERR_PROVIDER_RESPONSE;

    *out_score = score;
    return HU_OK;
}

static hu_error_t rm_mlx_score_batch(void *vctx, hu_allocator_t *alloc,
                                      const hu_preference_pair_t *pairs, size_t n,
                                      double *out_chosen_scores,
                                      double *out_rejected_scores) {
    if (!vctx || !alloc || !pairs || n == 0
        || !out_chosen_scores || !out_rejected_scores) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < n; i++) {
        const hu_preference_pair_t *p = &pairs[i];
        if (p->prompt_len == 0) {
            out_chosen_scores[i] = NAN;
            out_rejected_scores[i] = NAN;
            continue;
        }
        if (p->chosen_len > 0) {
            double s = 0.0;
            hu_error_t err = rm_mlx_score(vctx, alloc,
                                           p->prompt, p->prompt_len,
                                           p->chosen, p->chosen_len, &s);
            out_chosen_scores[i] = (err == HU_OK) ? s : NAN;
        } else {
            out_chosen_scores[i] = NAN;
        }
        if (p->rejected_len > 0) {
            double s = 0.0;
            hu_error_t err = rm_mlx_score(vctx, alloc,
                                           p->prompt, p->prompt_len,
                                           p->rejected, p->rejected_len, &s);
            out_rejected_scores[i] = (err == HU_OK) ? s : NAN;
        } else {
            out_rejected_scores[i] = NAN;
        }
    }
    return HU_OK;
}

static const char *rm_mlx_name(void *vctx) {
    (void)vctx;
    return "reward_model_mlx";
}

static void rm_mlx_deinit(void *vctx, hu_allocator_t *alloc) {
    if (!vctx) return;
    alloc->free(alloc->ctx, vctx, sizeof(rm_mlx_ctx_t));
}

static const hu_reward_model_vtable_t mlx_rm_vtable = {
    .score = rm_mlx_score,
    .score_batch = rm_mlx_score_batch,
    .name = rm_mlx_name,
    .deinit = rm_mlx_deinit,
};

hu_error_t hu_reward_model_create_mlx(hu_allocator_t *alloc,
                                       const hu_reward_model_config_t *config,
                                       hu_reward_model_t *out) {
    if (!alloc || !alloc->alloc || !alloc->free || !config || !out) {
        return HU_ERR_INVALID_ARGUMENT;
    }
#if !defined(__APPLE__)
    return HU_ERR_NOT_SUPPORTED;
#endif

    if (!mlx_lm_available()) return HU_ERR_NOT_SUPPORTED;

    if (!config->backbone_path || config->backbone_path[0] == '\0') {
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (strchr(config->backbone_path, '\'')) return HU_ERR_INVALID_ARGUMENT;
    if (config->value_head_path && strchr(config->value_head_path, '\'')) {
        return HU_ERR_INVALID_ARGUMENT;
    }

    rm_mlx_ctx_t *c = (rm_mlx_ctx_t *)alloc->alloc(alloc->ctx, sizeof(rm_mlx_ctx_t));
    if (!c) return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));

    snprintf(c->model_path, sizeof(c->model_path), "%s", config->backbone_path);
    if (config->value_head_path && config->value_head_path[0] != '\0') {
        snprintf(c->value_head_path, sizeof(c->value_head_path), "%s",
                 config->value_head_path);
    }

    out->ctx = c;
    out->vtable = &mlx_rm_vtable;
    return HU_OK;
}
