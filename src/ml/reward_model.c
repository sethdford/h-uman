/* src/ml/reward_model.c — Phase 3 Task 2
 *
 * hu_reward_model_t HUML factory + vtable: toy GPT backbone (cross-platform,
 * gradient-checkable) producing last-position logits, fed into Task 1's
 * hu_value_head_t for the scalar score. MLX factory is stubbed at
 * HU_ERR_NOT_SUPPORTED; Task 8 fills it in via scripts/rm_mlx_train.py.
 *
 * Style mirrors src/ml/dpo_real_huml.c and src/ml/value_head.c:
 *   - hu_allocator_t with 3-arg free (ctx, ptr, size) — see
 *     include/human/core/allocator.h:11. Every alloc->alloc has a matching
 *     alloc->free with the EXACT allocated size so the tracking allocator
 *     balances its leak ledger (the test suite uses AddressSanitizer for
 *     leak detection per AGENTS.md §2).
 *   - parse_id_string is inlined here rather than reused from
 *     dpo_real_huml.c: that file's version is `static` (file-local). The
 *     two parsers are structurally identical; if a third caller appears,
 *     extract to src/ml/ml_tokens.c per the Rule of Three.
 *   - hu_gpt_config_t fields use the real names from
 *     include/human/ml/ml.h:31-44 (n_layer, n_head, n_kv_head, n_embd,
 *     head_dim, sequence_len) — same correction as dpo_real_huml.c
 *     header note 1.
 *
 * Plan deviation note: the public header (per the task request) declares
 * `hidden_dim` as part of hu_reward_model_config_t alongside `vocab_size`.
 * For HUML, the "hidden state" passed to the value head IS the
 * last-position logits vector, which has shape [vocab_size] by
 * construction (output.shape[2] = V per src/ml/gpt.c:534). So
 * hidden_dim MUST equal vocab_size for HUML; the factory validates this
 * up front rather than silently using vocab_size and ignoring hidden_dim.
 * The MLX path (Task 8) will use hidden_dim independently from vocab_size
 * since real Qwen exposes the encoder hidden state directly.
 *
 * M3 NaN contract (per public-header docstring): score_batch with a
 * one-sided KTO pair (chosen_len == 0 OR rejected_len == 0) writes NaN
 * to the empty slot and scores the populated side normally. score()
 * itself (single-pair entry point) requires BOTH prompt_len and
 * response_len to be non-empty and returns HU_ERR_INVALID_ARGUMENT on
 * zero-length input — so score_batch DIRECTLY writes NaN rather than
 * routing the empty side through score().
 */
#include "human/ml/reward_model.h"
#include "reward_model_priv.h"

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/dpo.h"
#include "human/ml/ml.h"
#include "human/ml/model.h"
#include "human/ml/value_head.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const hu_reward_model_vtable_t huml_rm_vtable;

/* Tokenize a space-separated int-id string into int32_t array. Out-of-range
 * tokens are clamped to vocab_size - 1 (the toy GPT crashes on >=V).
 *
 * Allocator contract: returns the buffer capacity via *out_cap so callers
 * can free with `cap * sizeof(int32_t)` — the 3-arg
 * alloc->free(ctx, ptr, size) requires the EXACT allocated size for the
 * tracking allocator to balance its ledger (see allocator.h:11). */
static hu_error_t parse_id_string(hu_allocator_t *alloc, const char *s, size_t vocab_size,
                                  int32_t **out, size_t *out_n, size_t *out_cap) {
    if (!alloc || !alloc->alloc || !s || !out || !out_n) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    size_t cap = 16, n = 0;
    int32_t *buf = (int32_t *)alloc->alloc(alloc->ctx, cap * sizeof(int32_t));
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    const char *p = s;
    while (*p) {
        char *endp = NULL;
        long v = strtol(p, &endp, 10);
        if (endp == p)
            break;
        if (n == cap) {
            size_t old_cap = cap;
            cap *= 2;
            int32_t *nb = (int32_t *)alloc->alloc(alloc->ctx, cap * sizeof(int32_t));
            if (!nb) {
                alloc->free(alloc->ctx, buf, old_cap * sizeof(int32_t));
                return HU_ERR_OUT_OF_MEMORY;
            }
            memcpy(nb, buf, n * sizeof(int32_t));
            alloc->free(alloc->ctx, buf, old_cap * sizeof(int32_t));
            buf = nb;
        }
        if (v < 0)
            v = 0;
        if (vocab_size > 0 && (size_t)v >= vocab_size)
            v = (long)(vocab_size - 1);
        buf[n++] = (int32_t)v;
        p = endp;
        while (*p == ' ' || *p == '\t')
            p++;
    }
    *out = buf;
    *out_n = n;
    if (out_cap)
        *out_cap = cap;
    return HU_OK;
}

/* Run the backbone forward on `prompt response` (space-joined), pull out
 * the last-position [vocab_size] logits as the hidden state, and project
 * through the value head. Caller-allocated `out_score` is set on HU_OK. */
/* Core scorer. When out_h != NULL it receives a copy of the last-position
 * [hidden_dim] hidden vector fed to the value head (for analytical backprop). */
static hu_error_t huml_score_core(void *vctx, hu_allocator_t *alloc, const char *prompt,
                                  size_t prompt_len, const char *response, size_t response_len,
                                  double *out_score, float *out_h) {
    if (!vctx || !alloc || !alloc->alloc || !alloc->free || !prompt || prompt_len == 0 ||
        !response || response_len == 0 || !out_score) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    huml_rm_ctx_t *c = (huml_rm_ctx_t *)vctx;
    if (!c->backbone.vtable || !c->backbone.vtable->forward) {
        return HU_ERR_INVALID_ARGUMENT;
    }

    int32_t *prompt_ids = NULL, *response_ids = NULL;
    size_t pl = 0, rl = 0;
    size_t pcap = 0, rcap = 0;
    hu_error_t err = parse_id_string(alloc, prompt, c->vocab_size, &prompt_ids, &pl, &pcap);
    if (err != HU_OK)
        return err;
    err = parse_id_string(alloc, response, c->vocab_size, &response_ids, &rl, &rcap);
    if (err != HU_OK) {
        alloc->free(alloc->ctx, prompt_ids, pcap * sizeof(int32_t));
        return err;
    }
    if (pl == 0 || rl == 0) {
        alloc->free(alloc->ctx, prompt_ids, pcap * sizeof(int32_t));
        alloc->free(alloc->ctx, response_ids, rcap * sizeof(int32_t));
        return HU_ERR_INVALID_ARGUMENT;
    }
    /* Clamp combined length to the GPT's sequence_len to avoid forward
     * failures on the toy model (sequence_len=64 — see gpt_cfg below). */
    if (pl + rl > c->gpt_cfg.sequence_len) {
        size_t over = (pl + rl) - c->gpt_cfg.sequence_len;
        if (over >= rl) {
            /* response shorter than the overflow — drop response entirely
             * is not viable (rl must be > 0); shrink the prompt instead. */
            if (over >= pl) {
                alloc->free(alloc->ctx, prompt_ids, pcap * sizeof(int32_t));
                alloc->free(alloc->ctx, response_ids, rcap * sizeof(int32_t));
                return HU_ERR_INVALID_ARGUMENT;
            }
            pl -= over;
        } else {
            rl -= over;
        }
    }

    size_t total = pl + rl;
    int32_t *ids = (int32_t *)alloc->alloc(alloc->ctx, total * sizeof(int32_t));
    if (!ids) {
        alloc->free(alloc->ctx, prompt_ids, pcap * sizeof(int32_t));
        alloc->free(alloc->ctx, response_ids, rcap * sizeof(int32_t));
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(ids, prompt_ids, pl * sizeof(int32_t));
    memcpy(ids + pl, response_ids, rl * sizeof(int32_t));
    alloc->free(alloc->ctx, prompt_ids, pcap * sizeof(int32_t));
    alloc->free(alloc->ctx, response_ids, rcap * sizeof(int32_t));

    hu_ml_tensor_t input = {
        .data = ids,
        .shape = {1, total, 0, 0},
        .ndim = 2,
        .dtype = HU_ML_DTYPE_I32,
        .size_bytes = total * sizeof(int32_t),
    };
    hu_ml_tensor_t output = {0};
    err = c->backbone.vtable->forward(c->backbone.ctx, &input, &output);
    if (err != HU_OK) {
        alloc->free(alloc->ctx, ids, total * sizeof(int32_t));
        return err;
    }
    /* output.data: float logits, shape [1, total, V] per src/ml/gpt.c:534.
     * "Hidden state" for the HUML RM is the last-position V-vector — same
     * approximation as plan §D3 / R4. */
    float *logits = (float *)output.data;
    size_t V = output.shape[2];
    if (V != c->value_head.hidden_dim) {
        /* This would indicate a config bug, not a runtime input issue:
         * the factory validated vocab_size == hidden_dim at create time. */
        alloc->free(alloc->ctx, output.data, output.size_bytes);
        alloc->free(alloc->ctx, ids, total * sizeof(int32_t));
        return HU_ERR_INVALID_ARGUMENT;
    }
    const float *h = logits + (total - 1) * V;
    if (out_h)
        memcpy(out_h, h, V * sizeof(float));
    double score = 0.0;
    err = hu_value_head_forward(&c->value_head, h, &score);

    alloc->free(alloc->ctx, output.data, output.size_bytes);
    alloc->free(alloc->ctx, ids, total * sizeof(int32_t));
    if (err != HU_OK)
        return err;
    *out_score = score;
    return HU_OK;
}

/* vtable scorer — thin wrapper, no hidden state needed. */
static hu_error_t huml_rm_score(void *vctx, hu_allocator_t *alloc, const char *prompt,
                                size_t prompt_len, const char *response, size_t response_len,
                                double *out_score) {
    return huml_score_core(vctx, alloc, prompt, prompt_len, response, response_len, out_score,
                           NULL);
}

/* Priv (declared in reward_model_priv.h): score AND return the last-position
 * hidden vector, so the training loop can run analytical value-head backprop. */
hu_error_t reward_model_huml_score_hidden(hu_reward_model_t *rm, hu_allocator_t *alloc,
                                          const char *prompt, size_t prompt_len,
                                          const char *response, size_t response_len,
                                          double *out_score, float *out_h) {
    if (!rm || !rm->ctx)
        return HU_ERR_INVALID_ARGUMENT;
    return huml_score_core(rm->ctx, alloc, prompt, prompt_len, response, response_len, out_score,
                           out_h);
}

static hu_error_t huml_rm_score_batch(void *vctx, hu_allocator_t *alloc,
                                      const hu_preference_pair_t *pairs, size_t n,
                                      double *out_chosen_scores, double *out_rejected_scores) {
    if (!vctx || !alloc || !pairs || n == 0 || !out_chosen_scores || !out_rejected_scores) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < n; i++) {
        const hu_preference_pair_t *p = &pairs[i];
        /* M3 NaN contract: write NaN directly for empty sides; never route
         * a zero-length response through score() (which rejects it). The
         * prompt must be non-empty for ANY side to score — degenerate
         * prompts produce NaN on both sides. */
        if (p->prompt_len == 0) {
            out_chosen_scores[i] = NAN;
            out_rejected_scores[i] = NAN;
            continue;
        }
        if (p->chosen_len > 0) {
            double s = 0.0;
            hu_error_t err =
                huml_rm_score(vctx, alloc, p->prompt, p->prompt_len, p->chosen, p->chosen_len, &s);
            out_chosen_scores[i] = (err == HU_OK) ? s : NAN;
        } else {
            out_chosen_scores[i] = NAN;
        }
        if (p->rejected_len > 0) {
            double s = 0.0;
            hu_error_t err = huml_rm_score(vctx, alloc, p->prompt, p->prompt_len, p->rejected,
                                           p->rejected_len, &s);
            out_rejected_scores[i] = (err == HU_OK) ? s : NAN;
        } else {
            out_rejected_scores[i] = NAN;
        }
    }
    return HU_OK;
}

static const char *huml_rm_name(void *vctx) {
    (void)vctx;
    return "reward_model_huml";
}

static void huml_rm_deinit(void *vctx, hu_allocator_t *alloc) {
    if (!vctx)
        return;
    huml_rm_ctx_t *c = (huml_rm_ctx_t *)vctx;
    hu_value_head_deinit(&c->value_head, alloc);
    if (c->backbone.vtable && c->backbone.vtable->deinit) {
        c->backbone.vtable->deinit(c->backbone.ctx, alloc);
    }
    if (alloc && alloc->free) {
        alloc->free(alloc->ctx, c, sizeof(huml_rm_ctx_t));
    }
}

static const hu_reward_model_vtable_t huml_rm_vtable = {
    .score = huml_rm_score,
    .score_batch = huml_rm_score_batch,
    .name = huml_rm_name,
    .deinit = huml_rm_deinit,
};

huml_rm_ctx_t *hu_reward_model_huml_ctx_or_null(hu_reward_model_t *rm) {
    if (!rm || rm->vtable != &huml_rm_vtable)
        return NULL;
    return (huml_rm_ctx_t *)rm->ctx;
}

hu_error_t hu_reward_model_create_huml(hu_allocator_t *alloc,
                                       const hu_reward_model_config_t *config,
                                       hu_reward_model_t *out) {
    if (!alloc || !alloc->alloc || !alloc->free || !config || !out) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (config->backend != HU_REWARD_MODEL_BACKEND_HUML) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (config->vocab_size == 0 || config->hidden_dim == 0 ||
        config->vocab_size != config->hidden_dim) {
        /* For HUML the hidden state IS the last-position logits vector;
         * its length is vocab_size by construction. The two MUST match. */
        return HU_ERR_INVALID_ARGUMENT;
    }

    huml_rm_ctx_t *c = (huml_rm_ctx_t *)alloc->alloc(alloc->ctx, sizeof(huml_rm_ctx_t));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->vocab_size = config->vocab_size;
    c->hidden_dim = config->hidden_dim;
    /* Toy GPT config: same shape as src/ml/dpo_real_huml.c (n_layer=1,
     * n_head=1, n_kv_head=1, n_embd=16, head_dim=16, sequence_len=64) with
     * vocab_size from the caller. hu_gpt_create invariants
     * (n_embd == n_head * head_dim, head_dim % 2 == 0) hold. */
    c->gpt_cfg = (hu_gpt_config_t){
        .vocab_size = config->vocab_size,
        .n_layer = 1,
        .n_head = 1,
        .n_kv_head = 1,
        .n_embd = 16,
        .head_dim = 16,
        .sequence_len = 64,
    };
    hu_error_t err = hu_gpt_create(alloc, &c->gpt_cfg, &c->backbone);
    if (err != HU_OK) {
        alloc->free(alloc->ctx, c, sizeof(huml_rm_ctx_t));
        return err;
    }

    if (config->value_head_path && config->value_head_path[0] != '\0') {
        err = hu_value_head_load(alloc, config->value_head_path, &c->value_head);
    } else {
        err = hu_value_head_create(alloc, config->hidden_dim, &c->value_head);
    }
    if (err != HU_OK) {
        c->backbone.vtable->deinit(c->backbone.ctx, alloc);
        alloc->free(alloc->ctx, c, sizeof(huml_rm_ctx_t));
        return err;
    }

    out->ctx = c;
    out->vtable = &huml_rm_vtable;
    return HU_OK;
}

/* hu_reward_model_create_mlx moved to src/ml/reward_model_mlx.c (Phase 3 Task 8). */

hu_error_t hu_reward_model_save(const hu_reward_model_t *rm, const char *dir) {
    if (!rm || !rm->vtable || !dir || dir[0] == '\0')
        return HU_ERR_INVALID_ARGUMENT;

    huml_rm_ctx_t *c = hu_reward_model_huml_ctx_or_null((hu_reward_model_t *)rm);
    if (!c)
        return HU_ERR_NOT_SUPPORTED;

    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/value_head.vh", dir);
    if (n <= 0 || (size_t)n >= sizeof(path))
        return HU_ERR_INVALID_ARGUMENT;

    hu_error_t err = hu_value_head_save(&c->value_head, path);
    if (err != HU_OK)
        return err;

    n = snprintf(path, sizeof(path), "%s/rm_meta.json", dir);
    if (n <= 0 || (size_t)n >= sizeof(path))
        return HU_ERR_INVALID_ARGUMENT;
    FILE *f = fopen(path, "w");
    if (!f)
        return HU_ERR_IO;
    fprintf(f, "{\"vocab_size\":%zu,\"hidden_dim\":%zu,\"backend\":\"huml\"}\n", c->vocab_size,
            c->hidden_dim);
    fclose(f);
    return HU_OK;
}

/* Phase 4 Task 10: Load an HUML reward-model checkpoint written by
 * hu_reward_model_save. The on-disk schema is two files:
 *   <dir>/value_head.vh  — "VHED" magic + u32 hidden_dim + float[hd] W + float b
 *   <dir>/rm_meta.json   — {"vocab_size":N,"hidden_dim":N,"backend":"huml"}
 *
 * We parse rm_meta.json with the same minimal-JSON pattern used by the
 * sibling JSONL loaders (substring + numeric strtoul); a real JSON
 * dependency is out of scope for a 5 KB fixture loader. Strict-mode
 * checks: backend MUST be "huml", vocab_size MUST equal hidden_dim
 * (HUML invariant — see hu_reward_model_create_huml above), and the
 * value_head.vh hidden_dim header MUST match. Missing dir / missing
 * meta / corrupt header → HU_ERR_IO or HU_ERR_PARSE (no segfault).
 *
 * Approach: construct a fresh HUML RM via hu_reward_model_create_huml
 * (which xavier-inits a value head), then SWAP in the value head we
 * just loaded from disk. This way the backbone is always fresh — the
 * HUML backbone is frozen during RM training (see reward_model_train.c)
 * and the trained surface is the value head only. */
hu_error_t hu_reward_model_load(hu_allocator_t *alloc, const char *dir, hu_reward_model_t *out) {
    if (!alloc || !alloc->alloc || !alloc->free || !dir || !dir[0] || !out)
        return HU_ERR_INVALID_ARGUMENT;

    char meta_path[1024];
    char vh_path[1024];
    int n = snprintf(meta_path, sizeof(meta_path), "%s/rm_meta.json", dir);
    if (n <= 0 || (size_t)n >= sizeof(meta_path))
        return HU_ERR_INVALID_ARGUMENT;
    n = snprintf(vh_path, sizeof(vh_path), "%s/value_head.vh", dir);
    if (n <= 0 || (size_t)n >= sizeof(vh_path))
        return HU_ERR_INVALID_ARGUMENT;

    FILE *f = fopen(meta_path, "r");
    if (!f)
        return HU_ERR_IO;
    char meta_buf[1024];
    size_t got = fread(meta_buf, 1, sizeof(meta_buf) - 1, f);
    fclose(f);
    if (got == 0)
        return HU_ERR_PARSE;
    meta_buf[got] = '\0';

    size_t vocab_size = 0, hidden_dim = 0;
    const char *vf = strstr(meta_buf, "\"vocab_size\"");
    const char *hf = strstr(meta_buf, "\"hidden_dim\"");
    const char *bf = strstr(meta_buf, "\"backend\"");
    if (!vf || !hf || !bf)
        return HU_ERR_PARSE;
    /* Strict: backend must be "huml" (this loader only handles HUML). */
    const char *bq = strchr(bf + 9, '"');
    if (!bq)
        return HU_ERR_PARSE;
    bq++;
    const char *be = strchr(bq, '"');
    if (!be || (size_t)(be - bq) != 4 || strncmp(bq, "huml", 4) != 0)
        return HU_ERR_NOT_SUPPORTED;
    /* Find the first digit after each field name. */
    const char *vp = vf + strlen("\"vocab_size\"");
    while (*vp && (*vp < '0' || *vp > '9'))
        vp++;
    if (!*vp)
        return HU_ERR_PARSE;
    vocab_size = (size_t)strtoul(vp, NULL, 10);
    const char *hp = hf + strlen("\"hidden_dim\"");
    while (*hp && (*hp < '0' || *hp > '9'))
        hp++;
    if (!*hp)
        return HU_ERR_PARSE;
    hidden_dim = (size_t)strtoul(hp, NULL, 10);
    if (vocab_size == 0 || hidden_dim == 0 || vocab_size != hidden_dim)
        return HU_ERR_PARSE;

    hu_reward_model_config_t cfg = {
        .backend = HU_REWARD_MODEL_BACKEND_HUML,
        .vocab_size = vocab_size,
        .hidden_dim = hidden_dim,
    };
    hu_reward_model_t tmp = {0};
    hu_error_t err = hu_reward_model_create_huml(alloc, &cfg, &tmp);
    if (err != HU_OK)
        return err;

    huml_rm_ctx_t *c = hu_reward_model_huml_ctx_or_null(&tmp);
    if (!c) {
        tmp.vtable->deinit(tmp.ctx, alloc);
        return HU_ERR_PROVIDER_RESPONSE; /* defensive — create_huml just made one */
    }

    /* Swap in the loaded value head; free the xavier-init one. */
    hu_value_head_t loaded_vh = {0};
    err = hu_value_head_load(alloc, vh_path, &loaded_vh);
    if (err != HU_OK) {
        tmp.vtable->deinit(tmp.ctx, alloc);
        return err;
    }
    if (loaded_vh.hidden_dim != hidden_dim) {
        hu_value_head_deinit(&loaded_vh, alloc);
        tmp.vtable->deinit(tmp.ctx, alloc);
        return HU_ERR_PARSE;
    }
    hu_value_head_deinit(&c->value_head, alloc);
    c->value_head = loaded_vh;

    *out = tmp;
    return HU_OK;
}
