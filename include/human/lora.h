#ifndef HU_LORA_H
#define HU_LORA_H

/* SOTA-2026 S1.5 (a) — public deployment-time LoRA types shared across the
 * provider vtable, the personalization daemon, and downstream initiatives
 * #02 (MoLoRA), #05 (verifier-driven TTT), and #08 (federated LoRA).
 *
 * Naming note: this is NOT the same surface as `hu_lora_adapter_t` in
 * `include/human/ml/lora.h`. That type is the on-device *training* artifact —
 * opaque, owns float arrays, lives inside the ML subsystem. `hu_lora_adapter_spec_t`
 * here is a *deployment spec* — a pure descriptor the caller fills out on the
 * stack to tell a provider "bind this blob under this id". The two never
 * appear in the same vtable signature; they answer different questions
 * (how do I learn this? vs. how do I serve this?).
 *
 * `hu_lora_apply_mode_t` was previously defined in
 * `include/human/providers/mlx_qwen3.h`. It is hoisted here so initiatives
 * #02/#05/#08 can consume it without taking a dependency on the MLX provider
 * header. The mlx_qwen3 header now re-exports via `#include "human/lora.h"`.
 */

#include "human/core/allocator.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Apply mode for `hu_provider_t.load_adapter`.
 *
 * REPLACE — drop any incumbent adapter and install this one. The default;
 *           every provider that implements `load_adapter` MUST honor REPLACE.
 * STACK   — additive composition on top of the incumbent (MoLoRA-style). A
 *           provider that does not support stacking MUST return
 *           HU_ERR_NOT_SUPPORTED when STACK is requested — it MUST NOT
 *           silently downgrade to REPLACE. Downgrade-on-unsupported is a
 *           contract bug: the caller (init-02 MoLoRA dispatcher) needs to
 *           know whether stacking actually happened to schedule the next
 *           expert correctly.
 */
typedef enum hu_lora_apply_mode {
    HU_LORA_APPLY_MODE_REPLACE = 0, /* swap any incumbent (default) */
    HU_LORA_APPLY_MODE_STACK   = 1, /* additive composition; explicit opt-in */
} hu_lora_apply_mode_t;

/* Deployment spec passed to `hu_provider_t.load_adapter`.
 *
 * Either `path` or `bytes` MUST be set; setting both is rejected with
 * HU_ERR_INVALID_ARGUMENT (ambiguous source). `id` is always required —
 * it's the opaque label the daemon's personalization-status log echoes
 * back to the user, so the provider must persist it for `active_adapter`.
 *
 * The struct is stack-friendly and short-lived: providers MUST copy any
 * fields they want to retain past the `load_adapter` call. The caller
 * frees nothing on the provider's behalf.
 *
 * Reserved for future expansion (do NOT add fields today — YAGNI):
 *   - `rank`, `alpha`, `target_modules` (compatibility check at load)
 *   - `signature` (HMAC for trusted-source provenance, init-09 follow-up)
 */
typedef struct hu_lora_adapter_spec {
    /* Optional: absolute or ~/-relative filesystem path to the adapter
     * blob. NUL-terminated guaranteed only when `path_len == 0` (in
     * which case `path` MUST be NULL). When `path_len > 0`, the provider
     * MUST NOT assume NUL termination — it must copy `path_len` bytes
     * and append its own NUL if a C-string is required. */
    const char *path;
    size_t      path_len;

    /* Optional: pre-read adapter blob bytes. When non-NULL, the
     * provider MUST NOT touch the filesystem on this caller's behalf;
     * caller has already done the I/O. Mutually exclusive with `path`. */
    const uint8_t *bytes;
    size_t         bytes_len;

    /* Required: opaque caller-supplied label. Typically the persona id
     * (`"steth"`, `"work-formal"`, …). The provider echoes this verbatim
     * from `active_adapter()` and uses it for unload matching. Must be
     * non-empty. */
    const char *id;
    size_t      id_len;

    /* Required when `path` is set OR when the provider needs to allocate
     * workspace during the load. Most provider impls need it for both
     * (read the blob + allocate per-layer LoRA tensors). When `bytes` is
     * the source, providers may still need `alloc` for output workspace,
     * so the contract is "always supply it when you can".
     *
     * NULL is permitted only when the impl explicitly documents that it
     * uses an internal allocator — none of the in-tree impls do this
     * today. The helper `hu_provider_load_adapter` enforces non-NULL
     * for safety. */
    hu_allocator_t *alloc;
} hu_lora_adapter_spec_t;

#ifdef __cplusplus
}
#endif

#endif /* HU_LORA_H */
