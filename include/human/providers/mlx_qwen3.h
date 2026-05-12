#ifndef HU_PROVIDERS_MLX_QWEN3_H
#define HU_PROVIDERS_MLX_QWEN3_H

#include "human/lora.h" /* hoisted hu_lora_apply_mode_t (S1.5 (a)) */

/*
 * MLX Qwen3-4B-Instruct provider (init-04, M3 Bridge B — S1 scope).
 *
 * First on-device frontier-class provider in the binary. Drives an
 * external Python helper (`scripts/mlx_qwen3_serve.py`) that hosts
 * Qwen3-4B-Instruct under mlx-lm; we speak length-prefixed JSON over
 * stdio. The C-side TU compiles always; runtime behavior depends on
 * `HU_ENABLE_MLX_QWEN3`:
 *
 *   HU_ENABLE_MLX_QWEN3=OFF (default)
 *     Every vtable method returns HU_ERR_NOT_SUPPORTED. The factory
 *     still succeeds so the provider can be selected from config
 *     without crashing the daemon; the daemon's W13 personalization
 *     auto-load path treats NOT_SUPPORTED as benign fallthrough.
 *
 *   HU_ENABLE_MLX_QWEN3=ON
 *     Real subprocess lifecycle on Apple Silicon. `chat_with_system`
 *     spawns the helper lazily, exchanges a JSON ping, sends the chat
 *     opcode, and decodes the response. `load_adapter` / `unload_adapter`
 *     swap LoRA adapters in-process on the helper side; the C side
 *     mirrors the active adapter id for the `active_adapter` accessor.
 *
 *   HU_IS_TEST
 *     The chat / load_adapter path takes a deterministic in-process
 *     fake mode (no fork, no exec, no Metal). Tests exercise the
 *     state machine and the public surface without any external
 *     dependency.
 *
 * Design doc: docs/plans/2026-05-11-init-04-mlx-qwen3-provider.md.
 * Compatibility analysis vs init-02 (MoLoRA) and init-05 (TTT):
 * see design doc §7.
 *
 * S1 scope (this version):
 *   - Vtable: chat_with_system, chat, load_adapter, unload_adapter,
 *     active_adapter, get_name, supports_native_tools, deinit.
 *   - LoRA REPLACE semantics (single adapter resident at a time).
 *   - Build option HU_ENABLE_MLX_QWEN3 (default OFF) → stub mode.
 *
 * S1 explicitly NOT shipping (deferred to init-04 v1.5 / #02 / #05):
 *   - Streaming chat (stream opcode reserved in protocol).
 *   - Multi-adapter X-LoRA stacking (STACK mode reserved).
 *   - Activation steering hook (init-01 owns).
 *   - Speculative-decoding draft model wiring.
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Default quantization for the helper-loaded base model. The helper
 * picks the matching mlx-lm path on spawn; switching quant requires a
 * helper respawn (no hot swap in S1). See design doc §6.4. */
typedef enum hu_mlx_qwen3_quant {
    HU_MLX_QWEN3_QUANT_AWQ_4 = 0, /* default; Qwen/Qwen3-4B-Instruct-AWQ */
    HU_MLX_QWEN3_QUANT_MLX_4,     /* mlx_lm.quantize 4-bit group-wise */
    HU_MLX_QWEN3_QUANT_Q8,
    HU_MLX_QWEN3_QUANT_FP16,
} hu_mlx_qwen3_quant_t;

/* SOTA-2026 S1.5 (a): `hu_lora_apply_mode_t` was hoisted to
 * `include/human/lora.h` so initiatives #02 (MoLoRA), #05 (TTT), and #08
 * (federated LoRA) can consume the same enum without depending on the
 * MLX provider header. The transitive include below preserves source
 * compatibility for anything that included `mlx_qwen3.h` for the enum. */

typedef struct hu_mlx_qwen3_config {
    /* Absolute or ~-relative path to the Qwen3-4B base model on disk.
     * NULL → ~/.human/models/Qwen3-4B-Instruct/<quant>/. */
    const char *model_path;
    size_t model_path_len;

    /* Python interpreter used to spawn the helper. NULL → "python3". */
    const char *python_executable;
    size_t python_executable_len;

    /* Absolute path to scripts/mlx_qwen3_serve.py. NULL → discovery
     * order: $HUMAN_MLX_QWEN3_HELPER, install share dir, source-tree
     * scripts/. See design doc §6.6. */
    const char *helper_script_path;
    size_t helper_script_path_len;

    hu_mlx_qwen3_quant_t quant;

    /* Generation defaults applied when the caller passes 0. */
    uint32_t max_tokens_default; /* 0 → 512 */

    /* Helper subprocess lifecycle budgets. All in milliseconds.
     * 0 → use compiled-in defaults from the design doc §3.4. */
    uint32_t spawn_timeout_ms;
    uint32_t chat_timeout_ms;
    uint32_t resurrect_max_attempts;

    /* When true, helper stderr is inherited from the parent so messages
     * appear in the user-facing log. Default false → /dev/null. */
    bool verbose_helper_stderr;
} hu_mlx_qwen3_config_t;

/* Returns the helper-protocol major version this build understands. The
 * helper announces a matching version in its `ping` response; a
 * mismatch fails fast. Bumped on any breaking protocol change. */
uint32_t hu_mlx_qwen3_helper_protocol_version(void);

/* Create the MLX Qwen3 provider.
 *
 *   alloc   — required.
 *   config  — required; copied (caller may free immediately).
 *   out     — populated on HU_OK with a vtable pointing at the static
 *             mlx_qwen3 vtable and ctx pointing at an alloc-owned
 *             struct. deinit() releases everything.
 *
 * Returns HU_OK on success even when HU_ENABLE_MLX_QWEN3 is OFF — the
 * factory always succeeds, the chat path is what returns
 * HU_ERR_NOT_SUPPORTED in that mode. This mirrors the llamacpp
 * provider's "factory always succeeds, runtime falls through" pattern
 * so the daemon can be configured to prefer this provider without
 * crashing when the option is off. */
hu_error_t hu_mlx_qwen3_provider_create(hu_allocator_t *alloc,
                                        const hu_mlx_qwen3_config_t *config,
                                        hu_provider_t *out);

#endif /* HU_PROVIDERS_MLX_QWEN3_H */
