#ifndef HU_LLAMACPP_PROVIDER_H
#define HU_LLAMACPP_PROVIDER_H

/*
 * llama.cpp provider — frontier-model bridge with chat-time LoRA merging.
 *
 * When `HU_ENABLE_LLAMACPP` is defined and the build is linked against
 * libllama (vendored under `third_party/llama.cpp/` or found via
 * pkg-config), this provider hosts a GGUF model in-process and merges
 * loaded LoRA adapters at chat time via
 * `llama_lora_adapter_set` / `llama_lora_adapter_remove`.
 *
 * When the flag is OFF (default), every method on the vtable returns
 * `HU_ERR_NOT_SUPPORTED`. The factory still succeeds so the provider
 * can be selected from config without crashing the daemon — the
 * NOT_SUPPORTED return is treated as a benign "no llama.cpp linked"
 * signal by the agent path.
 *
 * Bridge plan: see `docs/plans/2026-05-10-m3-frontier-model-bridge.md`.
 */

#include "human/provider.h"

typedef struct hu_llamacpp_config {
    /* Path to a GGUF model on disk. May be NULL when the build is
     * unlinked; the chat path returns NOT_SUPPORTED in that case. */
    char *model_path;
    /* Max context window (tokens). 0 → use upstream default (typically 4096). */
    size_t context_size;
    /* Worker thread count. 0 → upstream default (physical core count). */
    int threads;
    /* GPU layer offload, when the libllama build supports it. */
    bool use_gpu;
    int n_gpu_layers;
} hu_llamacpp_config_t;

hu_error_t hu_llamacpp_provider_create(hu_allocator_t *alloc,
                                       const hu_llamacpp_config_t *config,
                                       hu_provider_t *out);

#endif
