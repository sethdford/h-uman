/* molora — Sprint 7 US-7.8 (Init #02 Phase 1): MoLoRA static per-channel router.
 *
 * Phase 1 is **static dispatch only**: a small router struct that maps a
 * normalized channel id ("telegram" / "imessage" / "slack" / "discord" / ...)
 * to a LoRA adapter path, plus one hook in the agent's pre-chat dispatch site
 * (`src/agent/agent_turn.c`) that consults the router and calls the existing
 * `hu_provider_load_adapter` path when the selected adapter differs from the
 * currently-active one.
 *
 * No learned MLP, no message-class classifier, no scoring network — those
 * land in later Init #02 phases. The router state is initialized once from
 * `config.personalization.molora.channel_adapters` at agent construction and
 * is read-only thereafter.
 *
 * Compile-guarded by `HU_ENABLE_MOLORA` (CMake option, default OFF). When the
 * option is OFF, this header is not included in any TU and the symbols are
 * absent from the binary entirely (verified by AC-7.8.5 binary-size delta).
 *
 * AC traceability (see `sprints/sprint-7/stories.md` and
 * `sprints/sprint-7/designs/US-7.8.md`):
 *   AC-7.8.1 → `hu_molora_router_select` returns channel-specific adapter
 *   AC-7.8.2 → falls back to `default_adapter_path` when channel absent
 *   AC-7.8.3 → disabled-by-default; gated behind HU_ENABLE_MOLORA
 *   AC-7.8.4 → struct is zero-initializable; `(hu_molora_router_t){0}` is
 *              a valid disabled router
 *   AC-7.8.5 → enforced by `scripts/check-molora-binary-budget.sh`
 *
 * Lifecycle: the router borrows pointers from `hu_personalization_config_t`.
 * `hu_molora_router_init` does NOT copy strings — it stores the borrowed
 * pointers and asserts that the config outlives the router. The router
 * itself owns no heap memory and `hu_molora_router_select` never allocates,
 * making it safe to call on every turn at the inference hot path.
 */

#ifndef HU_ML_MOLORA_H
#define HU_ML_MOLORA_H

#ifdef HU_ENABLE_MOLORA

#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Phase 1 cap: 16 channels covers the 4 Tier-1 channels (telegram, imessage,
 * slack, discord) plus the 12-channel headroom Init #02 phase 1 budgeted for.
 * Init #02 phase 2 (learned routing) may revisit this. */
#define HU_MOLORA_MAX_CHANNELS 16

/* Normalized channel id buffer size. Covers all 31 known channel modules
 * (longest current: "mattermost" at 10 chars) with 21 chars of slack for
 * future channels. Strings longer than this are truncated by the normalizer
 * (the truncated form still matches because router entries are normalized
 * the same way). */
#define HU_MOLORA_CHANNEL_NAME_MAX 32

typedef struct hu_molora_entry {
    /* Normalized channel id (lowercase, no `:` suffix, no whitespace).
     * Always null-terminated in the buffer. */
    char channel[HU_MOLORA_CHANNEL_NAME_MAX];
    /* Adapter path — BORROWED from hu_personalization_config_t.molora.
     * Not owned by the router; caller must keep config alive. */
    const char *adapter_path;
} hu_molora_entry_t;

typedef struct hu_molora_router {
    bool enabled;
    size_t count;
    hu_molora_entry_t entries[HU_MOLORA_MAX_CHANNELS];
    /* Fallback adapter when no channel entry matches.
     * Borrowed from `personalization.lora_adapter_path` (NULL allowed). */
    const char *default_adapter_path;
} hu_molora_router_t;

/* Forward declaration — full definition in human/config_types.h. We accept
 * an opaque pointer to avoid pulling the full config header into the ML
 * include tree (config_types.h includes security/sandbox.h transitively). */
struct hu_config;

/* Initialize the router from a fully-parsed config. The config must outlive
 * the router (the router stores borrowed adapter-path pointers).
 *
 * If `cfg == NULL` or `cfg->personalization.molora.enabled == false`, the
 * router is initialized to the disabled state (a `{0}` struct also produces
 * a valid disabled router — see AC-7.8.4).
 *
 * Returns:
 *   HU_OK on success (including the disabled case)
 *   HU_ERR_INVALID_ARGUMENT if `r == NULL`
 */
hu_error_t hu_molora_router_init(hu_molora_router_t *r, const struct hu_config *cfg);

/* Select the adapter path for the given channel id.
 *
 * The lookup is O(N) over `r->entries` where N <= HU_MOLORA_MAX_CHANNELS.
 * NEVER allocates. Safe to call from the inference hot path.
 *
 * Channel id normalization is applied internally to both the stored entries
 * (during `_init`) and the lookup key (during `_select`), so callers can pass
 * the raw `agent->active_channel` / `agent->active_channel_len` pair without
 * pre-processing. The normalizer:
 *   - lowercases ASCII letters
 *   - strips everything from the first `:` onward (so "telegram:42" → "telegram")
 *   - trims leading/trailing ASCII whitespace
 *   - truncates to HU_MOLORA_CHANNEL_NAME_MAX - 1 chars
 *
 * Returns:
 *   - The matching `entry->adapter_path` if the channel is in the map.
 *   - `r->default_adapter_path` if no entry matches and a default is set.
 *   - NULL if the router is disabled, the channel id is empty, or no entry
 *     matches AND `default_adapter_path` is NULL. Callers must tolerate NULL
 *     (fall through to today's "no adapter swap" behavior).
 *
 * Calling on a `{0}` router (disabled, no entries) is safe and returns NULL.
 */
const char *hu_molora_router_select(const hu_molora_router_t *r, const char *channel,
                                    size_t channel_len);

/* Normalize a channel id into `out`. Public so tests can exercise the
 * normalization table directly.
 *
 *   in       — pointer to channel id chars (need not be null-terminated)
 *   in_len   — number of bytes to read from `in`
 *   out      — destination buffer (always null-terminated on return)
 *   out_cap  — capacity of `out` in bytes (must be >= 1)
 *
 * Returns the length of the normalized string (excluding the terminator).
 * Returns 0 if `out_cap == 0`, `out == NULL`, or the input is empty after
 * trimming. On overflow the output is truncated to `out_cap - 1` bytes.
 *
 * Examples (assuming out_cap >= HU_MOLORA_CHANNEL_NAME_MAX):
 *   "telegram"       -> "telegram" (returns 8)
 *   "telegram:42"    -> "telegram" (returns 8)
 *   "Telegram"       -> "telegram" (returns 8)
 *   " Telegram \t"   -> "telegram" (returns 8)
 *   ""               -> ""         (returns 0)
 *   ":42"            -> ""         (returns 0; prefix is empty)
 */
size_t hu_molora_router_normalize_channel(const char *in, size_t in_len, char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* HU_ENABLE_MOLORA */
#endif /* HU_ML_MOLORA_H */
