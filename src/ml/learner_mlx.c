/* W13 — MLX learner backend (stub).
 *
 * Contract (when complete):
 *   - `available()` returns true on macOS arm64 hosts that have the MLX
 *     framework linkable AND `HU_HAVE_MLX` is defined at build time.
 *   - `train()` JIT-compiles a LoRA forward + backward pass against the
 *     model checkpoint at `cfg.base_model_path`, runs `cfg.max_steps` of
 *     AdamW on the metal-backed device, and serialises the final A/B
 *     low-rank factors using the same on-disk format as src/ml/lora.c.
 *   - The same adapter format documented in include/human/ml/learner.h
 *     (HLAD magic + version + model_version + weights) is also emitted
 *     when MLX writes its native artefact, so providers that opt-in to a
 *     path-based discovery flow can sniff the version regardless of
 *     trainer.
 *
 * Today (`HU_HAVE_MLX` is never defined): `available()` returns false and
 * `train()` returns HU_ERR_NOT_SUPPORTED. The vtable is referenced by
 * src/ml/learner.c so the dispatcher always sees a valid symbol.
 *
 * Wiring plan (out of scope for this commit):
 *   1. Detect MLX at configure time (CMake `find_package(MLX)`).
 *   2. Wrap src/ml/lora.c's hu_lora_create / hu_lora_apply / hu_lora_save
 *      with metal-allocated buffers behind an internal abstraction.
 *   3. Replace `compute_targets` in learner_cpu.c with a real per-token
 *      DPO loss against logprobs from the loaded base model. */

#include "human/ml/learner.h"

#include <stdint.h>

static bool mlx_available(void) {
#if defined(HU_HAVE_MLX) && defined(__APPLE__) && defined(__aarch64__)
    return true;
#else
    return false;
#endif
}

static hu_error_t mlx_train(void *ctx, const hu_learner_config_t *cfg,
                            const hu_training_signal_t *signals, size_t signals_count,
                            hu_learner_report_t *out_report) {
    (void)ctx;
    (void)cfg;
    (void)signals;
    (void)signals_count;
    (void)out_report;
    return HU_ERR_NOT_SUPPORTED;
}

static void mlx_deinit(void *ctx) { (void)ctx; }

const hu_learner_vtable_t hu_learner_mlx_vtable = {
    .name = "mlx",
    .available = mlx_available,
    .train = mlx_train,
    .deinit = mlx_deinit,
};

hu_error_t hu_learner_mlx_open(hu_allocator_t *alloc, void **out_ctx) {
    (void)alloc;
    if (out_ctx)
        *out_ctx = NULL;
    return HU_ERR_NOT_SUPPORTED;
}
