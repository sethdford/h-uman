#include "human/eval/eval_judge_external.h"

#include <stddef.h>
#include <string.h>

/*
 * Phase 5 Task 3 — `hu_eval_judge_external_t` impl.
 *
 * Three factories live here:
 *
 *   - `hu_eval_judge_create_canned`  — production-grade impl used by
 *     tests and CI. Deep-copies the verdict array (and any non-NULL
 *     rationale strings) so the caller's storage may be freed
 *     immediately; judge() cycles through the copy deterministically.
 *   - `hu_eval_judge_create_apple_fm`     — stub returning HU_ERR_NOT_SUPPORTED
 *                                           until Task 7 lands the
 *                                           Swift bridge.
 *   - `hu_eval_judge_create_gemini_nano`  — stub returning HU_ERR_NOT_SUPPORTED
 *                                           until Task 8 lands the
 *                                           Chrome AI bridge.
 *
 * Tasks 7 and 8 will land their real impls behind
 * `HU_EVAL_JUDGE_HAVE_APPLE_FM_IMPL` /
 * `HU_EVAL_JUDGE_HAVE_GEMINI_NANO_IMPL` compile-time tokens. The
 * `#ifndef` guards below let those impls slot in without touching
 * Task 3 code (per Phase 4 critic L3 — strict C11 conditional
 * compilation, NOT __attribute__((weak))).
 */

/* ── Canned factory ──────────────────────────────────────────────── */

typedef struct hu_eval_judge_canned_ctx {
    hu_allocator_t *alloc;
    hu_eval_judge_verdict_t *verdicts; /* deep copy, n_verdicts entries */
    size_t *rationale_lens;            /* parallel array; 0 ⇒ rationale was NULL */
    size_t n_verdicts;
    size_t index;
} hu_eval_judge_canned_ctx_t;

static hu_error_t canned_judge(struct hu_eval_judge_external *self,
                               const hu_eval_judge_pair_t *pair,
                               hu_eval_judge_verdict_t *out) {
    if (!self || !self->ctx || !pair || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_eval_judge_canned_ctx_t *ctx = (hu_eval_judge_canned_ctx_t *)self->ctx;
    if (ctx->n_verdicts == 0)
        return HU_ERR_NOT_SUPPORTED;
    *out = ctx->verdicts[ctx->index % ctx->n_verdicts];
    ctx->index++;
    return HU_OK;
}

static const char *canned_name(struct hu_eval_judge_external *self) {
    (void)self;
    return "canned";
}

static void canned_deinit(struct hu_eval_judge_external *self) {
    if (!self || !self->ctx)
        return;
    hu_eval_judge_canned_ctx_t *ctx = (hu_eval_judge_canned_ctx_t *)self->ctx;
    hu_allocator_t *a = ctx->alloc;
    if (ctx->verdicts) {
        for (size_t i = 0; i < ctx->n_verdicts; i++) {
            if (ctx->rationale_lens && ctx->rationale_lens[i] > 0 &&
                ctx->verdicts[i].rationale) {
                a->free(a->ctx, (void *)ctx->verdicts[i].rationale,
                        ctx->rationale_lens[i] + 1);
            }
        }
        a->free(a->ctx, ctx->verdicts,
                ctx->n_verdicts * sizeof(hu_eval_judge_verdict_t));
    }
    if (ctx->rationale_lens) {
        a->free(a->ctx, ctx->rationale_lens,
                ctx->n_verdicts * sizeof(size_t));
    }
    a->free(a->ctx, ctx, sizeof(*ctx));
    self->ctx = NULL;
    self->vtable = NULL;
}

static const hu_eval_judge_external_vtable_t CANNED_VTABLE = {
    .judge = canned_judge,
    .name = canned_name,
    .deinit = canned_deinit,
};

hu_error_t hu_eval_judge_create_canned(hu_allocator_t *alloc,
                                       const hu_eval_judge_canned_config_t *cfg,
                                       hu_eval_judge_external_t *out) {
    if (!alloc || !cfg || !out)
        return HU_ERR_INVALID_ARGUMENT;
    if (cfg->n_verdicts == 0 || !cfg->verdicts)
        return HU_ERR_INVALID_ARGUMENT;

    hu_eval_judge_canned_ctx_t *ctx =
        (hu_eval_judge_canned_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*ctx));
    if (!ctx)
        return HU_ERR_OUT_OF_MEMORY;
    ctx->alloc = alloc;
    ctx->n_verdicts = cfg->n_verdicts;
    ctx->index = 0;
    ctx->verdicts = NULL;
    ctx->rationale_lens = NULL;

    ctx->verdicts = (hu_eval_judge_verdict_t *)alloc->alloc(
        alloc->ctx, cfg->n_verdicts * sizeof(hu_eval_judge_verdict_t));
    if (!ctx->verdicts)
        goto oom;
    memset(ctx->verdicts, 0, cfg->n_verdicts * sizeof(hu_eval_judge_verdict_t));

    ctx->rationale_lens =
        (size_t *)alloc->alloc(alloc->ctx, cfg->n_verdicts * sizeof(size_t));
    if (!ctx->rationale_lens)
        goto oom;
    memset(ctx->rationale_lens, 0, cfg->n_verdicts * sizeof(size_t));

    for (size_t i = 0; i < cfg->n_verdicts; i++) {
        ctx->verdicts[i].prefer_a = cfg->verdicts[i].prefer_a;
        ctx->verdicts[i].confidence = cfg->verdicts[i].confidence;
        ctx->verdicts[i].rationale = NULL;
        if (cfg->verdicts[i].rationale) {
            size_t rlen = strlen(cfg->verdicts[i].rationale);
            char *copy = (char *)alloc->alloc(alloc->ctx, rlen + 1);
            if (!copy)
                goto oom;
            memcpy(copy, cfg->verdicts[i].rationale, rlen);
            copy[rlen] = '\0';
            ctx->verdicts[i].rationale = copy;
            ctx->rationale_lens[i] = rlen;
        }
    }

    out->vtable = &CANNED_VTABLE;
    out->ctx = ctx;
    return HU_OK;

oom:;
    hu_eval_judge_external_t tmp = {.vtable = &CANNED_VTABLE, .ctx = ctx};
    canned_deinit(&tmp);
    return HU_ERR_OUT_OF_MEMORY;
}

/* ── Apple Foundation Models stub (Task 7 lands real impl) ───────── */

#ifndef HU_EVAL_JUDGE_HAVE_APPLE_FM_IMPL
hu_error_t hu_eval_judge_create_apple_fm(hu_allocator_t *alloc,
                                         hu_eval_judge_external_t *out) {
    (void)alloc;
    (void)out;
    return HU_ERR_NOT_SUPPORTED;
}
#endif /* HU_EVAL_JUDGE_HAVE_APPLE_FM_IMPL */

/* ── Gemini Nano stub (Task 8 lands real impl) ───────────────────── */

#ifndef HU_EVAL_JUDGE_HAVE_GEMINI_NANO_IMPL
hu_error_t hu_eval_judge_create_gemini_nano(hu_allocator_t *alloc,
                                            hu_eval_judge_external_t *out) {
    (void)alloc;
    (void)out;
    return HU_ERR_NOT_SUPPORTED;
}
#endif /* HU_EVAL_JUDGE_HAVE_GEMINI_NANO_IMPL */
