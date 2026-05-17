#ifndef HU_MLX_PROVIDER_H
#define HU_MLX_PROVIDER_H

/*
 * MLX provider — Apple Silicon on-device frontier-model bridge (Bridge B).
 *
 * Today this file is the adapter boundary, not the full bridge stack. When
 * `HU_ENABLE_MLX_PROVIDER` is undefined (the default) every vtable method
 * returns `HU_ERR_NOT_SUPPORTED` and `hu_mlx_provider_create` still
 * succeeds — selecting `mlx` from config sees the NOT_SUPPORTED return,
 * logs a benign info-level message, and falls through to the base
 * provider. Mirrors the llamacpp provider pattern.
 *
 * When the CMake option `HU_ENABLE_MLX_PROVIDER=ON` is set AND an MLX
 * runtime is reachable (Python via `python3 -m mlx_lm.generate`, or
 * future in-process bindings), the body of each NOT_SUPPORTED stub is
 * replaced with the real subprocess + JSON-parse path.
 *
 * Why this file exists at all when chat is not yet implemented:
 *   The M3 frontier-bridge plan (`docs/plans/2026-05-10-m3-frontier-model-bridge.md`,
 *   Bridge B Phase B.1) requires a provider symbol so the LoRA-trained
 *   safetensors adapter produced by `human ml lora-persona --backend mlx`
 *   has a consumer at chat time. Today that adapter has no consumer
 *   (audit finding 2026-05-16). This stub creates the consumer slot;
 *   wiring it to an actual MLX runtime is the next phase.
 *
 * Adapter format: when wired, this provider expects `safetensors` LoRA
 * adapters — distinct from the GGUF LoRA format `llamacpp.c` uses. The
 * format-bridge between HUML `.lora` and `safetensors` is a separate
 * open task (audit finding 2026-05-16).
 *
 * See `docs/plans/2026-05-10-m3-frontier-model-bridge.md` Bridge B.
 */

#include "human/provider.h"

typedef struct hu_mlx_config {
    /* HuggingFace repo name or local path to an MLX-quantized model.
     * E.g. "mlx-community/gemma-4-31b-it-4bit". May be NULL when the
     * build is unlinked; the chat path returns NOT_SUPPORTED. */
    const char *model_path;
    size_t model_path_len;
    /* Path to a safetensors LoRA adapter to apply at chat time. NULL
     * means "base model only." */
    const char *adapter_path;
    size_t adapter_path_len;
    /* Max output tokens. 0 → MLX default (typically 512). */
    int max_tokens;
} hu_mlx_config_t;

/* Create an MLX provider.
 *
 * Returns:
 *   - HU_OK on success. Caller owns `out->ctx`; release with
 *     `out->vtable->deinit`. The vtable methods themselves may still
 *     return HU_ERR_NOT_SUPPORTED until the MLX runtime is linked.
 *   - HU_ERR_INVALID_ARGUMENT when `alloc` or `out` is NULL.
 *   - HU_ERR_OUT_OF_MEMORY on allocator failure (either the context
 *     struct itself, or the owned config-string copies).
 *
 * The earlier comment promised "always succeeds when `out` is non-NULL,"
 * which was wrong on both NULL-alloc and OOM paths. CodeRabbit
 * 2026-05-17 finding. */
hu_error_t hu_mlx_provider_create(hu_allocator_t *alloc, const hu_mlx_config_t *config,
                                  hu_provider_t *out);

#endif /* HU_MLX_PROVIDER_H */
