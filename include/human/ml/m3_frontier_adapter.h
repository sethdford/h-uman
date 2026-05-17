#ifndef HU_M3_FRONTIER_ADAPTER_H
#define HU_M3_FRONTIER_ADAPTER_H

/* M3 — Frontier persona adapter (stub / fixture path).
 *
 * Track D vertical slice: prove we can **load** a versioned placeholder
 * descriptor from disk and run a **no-op inference** hook without network.
 * Real GGUF / llama.cpp / MLX wiring replaces the file format later; this
 * header is the stable seam tests and the agent can depend on. */

#include "human/core/allocator.h"
#include "human/core/error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Track D D1.3 — single source of truth for the rollback decision.
 * Returns true when the M3 frontier-bridge attach should be skipped:
 *   1. `cfg_disabled` matches the parsed value of
 *      `personalization.m3_adapter_disabled`. Pass `false` when no
 *      config is available.
 *   2. The `HUMAN_M3_ADAPTER_DISABLE` env var, when set to anything
 *      other than empty / "0", forces-disable regardless of config —
 *      the operational kill switch.
 *
 * Pure: no side effects beyond reading the env var. Safe to call
 * before bootstrap, in tests, and from anywhere a caller wants to
 * decide whether to attach the bridge. */
bool hu_m3_adapter_should_disable(bool cfg_disabled);

typedef struct hu_m3_frontier_adapter hu_m3_frontier_adapter_t;

/* On-disk magic for fixture adapters (8 bytes, no NUL). */
#define HU_M3_ADAPTER_MAGIC "HU_M3AD\x01"

/* Try to open a stub adapter from `path` (NUL-terminated or `path_len` bytes).
 * Returns HU_ERR_IO when the file is missing or the header does not match.
 * On success, `*out` is owned; free with `hu_m3_frontier_adapter_close`. */
hu_error_t hu_m3_frontier_adapter_try_open(hu_allocator_t *alloc, const char *path, size_t path_len,
                                           hu_m3_frontier_adapter_t **out);

/* Deterministic probe "inference" — always HU_OK; increments an internal
 * call counter on the adapter (observable via
 * `hu_m3_frontier_adapter_probe_count`). Replaces the older
 * `hu_m3_frontier_adapter_noop_infer`, which silently returned HU_OK with
 * no side effect — meaning a regression that dropped one of the 11
 * provider-success call sites would be undetectable at the test layer.
 *
 * The counter is a *signal*, not a model: no tensors, no learning, no
 * gradient. It exists so:
 *   1. A test can pin "the chat path actually reaches the M3 hook"
 *      (see tests/test_m3_frontier_probe.c) instead of trusting that
 *      a (void)return; means the wiring works.
 *   2. The eventual real-tensor implementation has a known-good seam
 *      to slot under — replace the body of `probe_infer` with the
 *      tensor call; the counter side effect can stay or go.
 *
 * Backwards-compat: `hu_m3_frontier_adapter_noop_infer` is preserved as
 * a thin wrapper that calls `probe_infer` and discards the count delta,
 * so the agent-side `hu_agent_m3_on_provider_success` callers do not
 * need to be re-edited for this slice. See
 * docs/plans/2026-05-17-m3-mlx-bridge-execution-plan.md Phase B-pre. */
hu_error_t hu_m3_frontier_adapter_probe_infer(hu_m3_frontier_adapter_t *adapter);
hu_error_t hu_m3_frontier_adapter_noop_infer(hu_m3_frontier_adapter_t *adapter);

/* Read-only: number of times probe_infer (or noop_infer) was called on
 * this adapter since open. Zero for NULL. Test-observable seam. */
uint64_t hu_m3_frontier_adapter_probe_count(const hu_m3_frontier_adapter_t *adapter);

void hu_m3_frontier_adapter_close(hu_allocator_t *alloc, hu_m3_frontier_adapter_t *adapter);

/* Read-only: schema version from the opened file (0 if NULL). */
uint32_t hu_m3_frontier_adapter_schema_version(const hu_m3_frontier_adapter_t *adapter);

/* Track D D2.1 — user-facing honest-gap caveat strings.
 *
 * The `human ml lora-persona` command emits these strings at training
 * start (and from `--help`). They make explicit that the LoRA path
 * trains a reference HUML GPT, NOT the frontier model the user
 * actually chats with. Centralizing them here (rather than embedding
 * literal printfs in cli.c) means:
 *
 *   - Tests can pin the substrings that matter (no silent drift to
 *     overclaiming language during refactors).
 *   - The caveat doc path is a single constant, so when the path
 *     changes (rare) only one place updates.
 *
 * Thread-safe and allocator-free — both functions return pointers
 * to static storage. */

/* Path to the honest-gap planning doc, relative to repo root. */
const char *hu_ml_lora_persona_caveat_doc_path(void);

/* Multi-line caveat block printed before training starts. Lines are
 * `\n`-terminated and each is prefixed with `[lora-persona]` so the
 * block aligns visually with the rest of the CLI output. */
const char *hu_ml_lora_persona_caveat_block(void);

#ifdef __cplusplus
}
#endif

#endif /* HU_M3_FRONTIER_ADAPTER_H */
