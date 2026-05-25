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

/* Phase 1 (Gemma throughput program) — KV-cache quantization.
 *
 * llama.cpp's `llama_context_params.type_k` / `.type_v` accept GGML
 * tensor types for the per-token K and V caches. FP16 is the default
 * and matches pre-quant behavior; Q8_0 halves KV RSS at ~10-15% TPS
 * gain (memory-bandwidth bound at batch=1); Q4_0 halves it again with
 * larger quality risk at long context.
 *
 * The enum is unconditional (independent of HU_LLAMACPP_LINKED) so
 * operator-facing config code can reference it without pulling in
 * llama.cpp headers. The actual GGML type assignment happens inside
 * the llama_context init under the `HU_LLAMACPP_LINKED` gate. */
typedef enum hu_kv_quant {
    HU_KV_QUANT_FP16 = 0, /* default — no quantization (GGML_TYPE_F16) */
    HU_KV_QUANT_Q8_0 = 1, /* INT8 K + V — ~50% RSS, ~10-15% TPS */
    HU_KV_QUANT_Q4_0 = 2, /* INT4 K + V — opt-in, long-context risk */
} hu_kv_quant_t;

/* Parse a config-shaped kv_quant string ("fp16" / "q8_0" / "q4_0",
 * case-insensitive). Unknown / NULL / empty input returns
 * HU_KV_QUANT_FP16 (safe default) and sets *out_recognized = false so
 * the caller can warn the operator. *out_recognized may be NULL. */
hu_kv_quant_t hu_kv_quant_from_string(const char *s, bool *out_recognized);

/* Inverse: stable lowercase name for logging / health endpoints. */
const char *hu_kv_quant_to_string(hu_kv_quant_t q);

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
    /* Phase 1 — KV quantization at context init. Default FP16 keeps
     * pre-quant behavior; the operator opts in to Q8_0 / Q4_0 either
     * via config or a follow-up env-var bridge in the factory. */
    hu_kv_quant_t kv_quant;
    /* Phase 3b (Gemma throughput program) — speculative decoding draft
     * model. When non-NULL, the chat path runs cross-model spec decode:
     * a small draft model (e.g. Gemma-3-270M) proposes draft_max_tokens
     * tokens; the target model verifies in parallel; verified prefix is
     * emitted. Hits ~1.5-2× decode TPS at batch=1 on M-series when the
     * draft is well-aligned with the target distribution.
     *
     * The Phase 6 milestone (A3 from the SOTA roadmap) trains a persona-
     * aligned draft via the same LoRA pipeline used for personalization;
     * acceptance rate climbs from ~0% (unaligned) to ≥50% (aligned).
     *
     * draft_model_path: borrowed pointer at config time, deep-copied by
     * hu_llamacpp_provider_create. NULL disables spec decode (the
     * default — pre-Phase-3b behavior is byte-identical).
     * draft_min_p: confidence threshold for accepting a draft token.
     * draft_max_tokens: how many tokens the draft proposes per round
     * before the target verifies. 0 → upstream default (typically 5). */
    char *draft_model_path;
    float draft_min_p;
    int draft_max_tokens;
} hu_llamacpp_config_t;

hu_error_t hu_llamacpp_provider_create(hu_allocator_t *alloc, const hu_llamacpp_config_t *config,
                                       hu_provider_t *out);

#endif
