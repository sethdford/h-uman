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

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hu_m3_frontier_adapter hu_m3_frontier_adapter_t;

/* On-disk magic for fixture adapters (8 bytes, no NUL). */
#define HU_M3_ADAPTER_MAGIC "HU_M3AD\x01"

/* Try to open a stub adapter from `path` (NUL-terminated or `path_len` bytes).
 * Returns HU_ERR_IO when the file is missing or the header does not match.
 * On success, `*out` is owned; free with `hu_m3_frontier_adapter_close`. */
hu_error_t hu_m3_frontier_adapter_try_open(hu_allocator_t *alloc, const char *path, size_t path_len,
                                          hu_m3_frontier_adapter_t **out);

/* Deterministic no-op “inference” — always HU_OK; exists so call sites can be
 * wired before real tensor work lands. */
hu_error_t hu_m3_frontier_adapter_noop_infer(hu_m3_frontier_adapter_t *adapter);

void hu_m3_frontier_adapter_close(hu_allocator_t *alloc, hu_m3_frontier_adapter_t *adapter);

/* Read-only: schema version from the opened file (0 if NULL). */
uint32_t hu_m3_frontier_adapter_schema_version(const hu_m3_frontier_adapter_t *adapter);

#ifdef __cplusplus
}
#endif

#endif /* HU_M3_FRONTIER_ADAPTER_H */
