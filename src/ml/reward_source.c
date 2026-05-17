/* src/ml/reward_source.c — Phase 4 Task 4 (RL SOTA)
 *
 * Three reward-source backends wired through one leaf vtable:
 *   1. Synthetic   — count tokens 1..5 minus tokens 26..30 per completion.
 *                    Pure function; ctx is NULL.
 *   2. Reward model — composes a borrowed Phase 3 hu_reward_model_t; renders
 *                    prompt+response token IDs as space-separated decimal
 *                    strings (mirrors Phase 3 KTO Task 4) and forwards to
 *                    rm->vtable->score(). Caller owns the rm lifecycle.
 *   3. Judge       — Phase 5 placeholder; factory returns HU_ERR_NOT_SUPPORTED.
 *
 * R9 reward-hacking mitigation (umbrella §10): no implicit default for the
 * reward source — picking the wrong source silently is the named risk.
 * CLI Task 9 must require --reward-fn or --reward-model explicitly.
 *
 * Allocator ownership: the synthetic backend stores no state (ctx NULL,
 * deinit is a no-op). The RM backend allocates a tiny ctx record from
 * `alloc` at create time and frees it at deinit using the same allocator
 * (stored by value inside the ctx) — the deinit signature does NOT
 * carry an explicit allocator, so we capture it once and self-free.
 */
#include "human/ml/reward_source.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---------------- Synthetic backend ----------------
 *
 * Stateless: ctx is NULL. The vtable function pointers are static, so
 * the entire backend occupies zero heap bytes at runtime. */

static hu_error_t synthetic_score(struct hu_reward_source *self,
                                   const int32_t *prompt_ids, size_t prompt_len,
                                   const hu_rollout_completion_t *completions, size_t n,
                                   double *out_rewards) {
    (void)self;
    (void)prompt_ids; /* synthetic ignores the prompt — pure completion scoring */
    (void)prompt_len;
    if (!out_rewards) return HU_ERR_INVALID_ARGUMENT;
    if (n > 0 && !completions) return HU_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < n; i++) {
        double r = 0.0;
        const int32_t *toks = completions[i].token_ids;
        const size_t nt = completions[i].n_tokens;
        if (nt > 0 && !toks) return HU_ERR_INVALID_ARGUMENT;
        for (size_t j = 0; j < nt; j++) {
            const int32_t t = toks[j];
            if (t >= 1 && t <= 5) {
                r += 1.0;
            } else if (t >= 26 && t <= 30) {
                r -= 1.0;
            }
        }
        out_rewards[i] = r;
    }
    return HU_OK;
}

static const char *synthetic_name(struct hu_reward_source *self) {
    (void)self;
    return "synthetic";
}

static void synthetic_deinit(struct hu_reward_source *self) {
    if (!self) return;
    self->vtable = NULL;
    self->ctx = NULL;
}

static const hu_reward_source_vtable_t kSyntheticVtable = {
    .score = synthetic_score,
    .name = synthetic_name,
    .deinit = synthetic_deinit,
};

hu_error_t hu_reward_source_create_synthetic(hu_allocator_t *alloc,
                                              hu_reward_source_t *out) {
    if (!alloc || !out) return HU_ERR_INVALID_ARGUMENT;
    out->vtable = &kSyntheticVtable;
    out->ctx = NULL; /* synthetic is stateless */
    return HU_OK;
}

/* ---------------- Reward-model backend ----------------
 *
 * The RM backend owns a tiny ctx struct holding (a) the allocator by value
 * (so deinit can self-free without an explicit alloc argument) and (b) the
 * borrowed hu_reward_model_t pointer.
 *
 * score() renders each completion as a space-separated decimal string and
 * forwards to rm->vtable->score(). The 2 KB / 4 KB stack scratch buffers
 * mirror the Phase 3 KTO Task 4 pattern — sized for the toy-GPT vocab/
 * sequence lengths exercised by GRPO HUML rollouts; if a real frontier-
 * scale path is wired in Phase 4 Task 8 it will need its own larger
 * scratch (and likely a tokenizer, not stringified IDs). */

typedef struct {
    hu_allocator_t alloc;       /* stored by value so deinit can self-free */
    hu_reward_model_t *rm;       /* borrowed; caller owns lifecycle */
} reward_source_rm_ctx_t;

/* Render token IDs as space-separated decimals into `buf`. Returns the
 * number of bytes written (excluding terminator). Stops cleanly if the
 * buffer would overflow; the resulting prefix is still a valid
 * space-separated token string for the RM scorer. */
static size_t render_tokens(char *buf, size_t cap,
                             const int32_t *tokens, size_t n) {
    if (!buf || cap == 0) return 0;
    buf[0] = '\0';
    size_t off = 0;
    for (size_t i = 0; i < n; i++) {
        /* Reserve 12 bytes headroom for "-2147483648" + space + terminator. */
        if (off + 12 >= cap) break;
        int written = snprintf(buf + off, cap - off,
                                i == 0 ? "%d" : " %d", (int)tokens[i]);
        if (written < 0) break;
        off += (size_t)written;
    }
    return off;
}

static hu_error_t rm_score(struct hu_reward_source *self,
                            const int32_t *prompt_ids, size_t prompt_len,
                            const hu_rollout_completion_t *completions, size_t n,
                            double *out_rewards) {
    if (!self || !self->ctx || !out_rewards) return HU_ERR_INVALID_ARGUMENT;
    if (n > 0 && !completions) return HU_ERR_INVALID_ARGUMENT;
    if (prompt_len > 0 && !prompt_ids) return HU_ERR_INVALID_ARGUMENT;

    reward_source_rm_ctx_t *c = (reward_source_rm_ctx_t *)self->ctx;
    if (!c->rm || !c->rm->vtable || !c->rm->vtable->score) {
        return HU_ERR_INVALID_ARGUMENT;
    }

    /* Render prompt once — shared across all completions for this batch. */
    char prompt_s[2048];
    size_t prompt_s_len = render_tokens(prompt_s, sizeof(prompt_s),
                                          prompt_ids, prompt_len);

    for (size_t i = 0; i < n; i++) {
        char response_s[4096];
        size_t response_s_len = render_tokens(response_s, sizeof(response_s),
                                                completions[i].token_ids,
                                                completions[i].n_tokens);
        double score = 0.0;
        hu_error_t err = c->rm->vtable->score(c->rm->ctx, &c->alloc,
                                                prompt_s, prompt_s_len,
                                                response_s, response_s_len,
                                                &score);
        if (err != HU_OK) return err;
        out_rewards[i] = score;
    }
    return HU_OK;
}

static const char *rm_name(struct hu_reward_source *self) {
    (void)self;
    return "reward_model";
}

static void rm_deinit(struct hu_reward_source *self) {
    if (!self || !self->ctx) {
        if (self) {
            self->vtable = NULL;
            self->ctx = NULL;
        }
        return;
    }
    reward_source_rm_ctx_t *c = (reward_source_rm_ctx_t *)self->ctx;
    hu_allocator_t a = c->alloc; /* copy before we free `c` */
    /* NOTE: do NOT touch c->rm — the reward model is borrowed and the
     * caller owns its lifecycle (per header contract). */
    a.free(a.ctx, c, sizeof(*c));
    self->vtable = NULL;
    self->ctx = NULL;
}

static const hu_reward_source_vtable_t kRmVtable = {
    .score = rm_score,
    .name = rm_name,
    .deinit = rm_deinit,
};

hu_error_t hu_reward_source_create_rm(hu_allocator_t *alloc,
                                       hu_reward_model_t *rm,
                                       hu_reward_source_t *out) {
    if (!alloc || !rm || !out) return HU_ERR_INVALID_ARGUMENT;
    if (!rm->vtable || !rm->vtable->score) return HU_ERR_INVALID_ARGUMENT;

    reward_source_rm_ctx_t *c =
        (reward_source_rm_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c) return HU_ERR_OUT_OF_MEMORY;
    c->alloc = *alloc;
    c->rm = rm; /* borrowed */
    out->vtable = &kRmVtable;
    out->ctx = c;
    return HU_OK;
}

/* ---------------- Judge backend (Phase 5 stub) ----------------
 *
 * The factory returns HU_ERR_NOT_SUPPORTED without populating `out`. The
 * symbol exists in Phase 4 so the CLI can dispatch on `--reward-source
 * judge` and surface a meaningful "not yet" error rather than a link
 * failure. Phase 5 Task X will land the real multi-judge consensus impl
 * per umbrella §10 R3 mitigation. */
hu_error_t hu_reward_source_create_judge(hu_allocator_t *alloc,
                                          hu_reward_source_t *out) {
    if (!alloc || !out) return HU_ERR_INVALID_ARGUMENT;
    /* Leave `out` untouched on NOT_SUPPORTED — caller's `out` was already
     * zero-initialized at the call site (R-pattern matches other Phase 4
     * Task 2/3 stubs like hu_rollout_create_mlx). */
    return HU_ERR_NOT_SUPPORTED;
}
