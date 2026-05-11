/*
 * Phase 1 (RL SOTA) — decode loop implementation.
 *
 * Critic-pinned invariant: the loop MUST feed each sampled token back
 * to the model via `advance` before fetching the next batch of logits.
 * Without it, llama_get_logits_ith(ctx, -1) returns the SAME frozen
 * distribution on every iteration and we emit the same token forever.
 * Unit tests where the mock logits provider self-advances may pass
 * advance=NULL; production code MUST supply a real advance.
 */

#include "human/providers/llamacpp_decode.h"

#include "human/core/error.h"
#include "human/providers/llamacpp_sampling.h"

#include <stdlib.h>

#define HU_LLAMACPP_DECODE_DEFAULT_VOCAB 100 /* unit-test default */

hu_error_t hu_llamacpp_decode_run(const hu_llamacpp_decode_config_t *cfg,
                                  int32_t *out_tokens,
                                  size_t *out_tokens_len) {
    if (!cfg || !cfg->logits_provider || !cfg->sampler ||
        !out_tokens || !out_tokens_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out_tokens_len = 0;
    size_t vocab = cfg->vocab_size ? cfg->vocab_size
                                   : (size_t)HU_LLAMACPP_DECODE_DEFAULT_VOCAB;
    float *logits = (float *)malloc(sizeof(float) * vocab);
    if (!logits) return HU_ERR_OUT_OF_MEMORY;
    hu_error_t err = HU_OK;
    for (size_t step = 0; step < cfg->max_tokens; step++) {
        err = cfg->logits_provider(cfg->logits_ctx, step, logits, vocab);
        if (err != HU_OK) break;
        int32_t tok = -1;
        err = hu_llamacpp_sampler_pick(cfg->sampler, logits, vocab, &tok);
        if (err != HU_OK) break;
        if (tok == cfg->eos_token) break;
        out_tokens[step] = tok;
        (*out_tokens_len)++;
        if (cfg->advance) {
            err = cfg->advance(cfg->advance_ctx, tok);
            if (err != HU_OK) break;
        }
    }
    free(logits);
    return err;
}
