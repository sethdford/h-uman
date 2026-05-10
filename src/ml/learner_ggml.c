/* W13 — ggml / llama.cpp learner backend (stub).
 *
 * Contract (when complete):
 *   - `available()` returns true when the build has linked ggml + llama.cpp
 *     and the `HU_HAVE_GGML` macro is defined.
 *   - `train()` loads `cfg.base_model_path` as a GGUF, attaches LoRA tensors
 *     of `cfg.rank` to the attention Q/V matrices, and runs `cfg.max_steps`
 *     of optimization (Adam by default; DP-SGD when `cfg.dp_enabled`).
 *   - The output adapter is written in GGUF-LoRA format AND a sidecar HLAD
 *     header (see include/human/ml/learner.h) so the path-based discovery
 *     flow in W14 can identify it without parsing GGUF.
 *
 * Today: returns HU_ERR_NOT_SUPPORTED. The CPU backend is the canonical
 * fallback. */

#include "human/ml/learner.h"

#include <stdint.h>

static bool ggml_available(void) {
#if defined(HU_HAVE_GGML)
    return true;
#else
    return false;
#endif
}

static hu_error_t ggml_train(void *ctx, const hu_learner_config_t *cfg,
                             const hu_training_signal_t *signals, size_t signals_count,
                             hu_learner_report_t *out_report) {
    (void)ctx;
    (void)cfg;
    (void)signals;
    (void)signals_count;
    (void)out_report;
    return HU_ERR_NOT_SUPPORTED;
}

static void ggml_deinit(void *ctx) { (void)ctx; }

const hu_learner_vtable_t hu_learner_ggml_vtable = {
    .name = "ggml",
    .available = ggml_available,
    .train = ggml_train,
    .deinit = ggml_deinit,
};

hu_error_t hu_learner_ggml_open(hu_allocator_t *alloc, void **out_ctx) {
    (void)alloc;
    if (out_ctx)
        *out_ctx = NULL;
    return HU_ERR_NOT_SUPPORTED;
}
