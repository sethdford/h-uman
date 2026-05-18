#ifndef HU_M3_ID_MAP_H
#define HU_M3_ID_MAP_H

/* Phase C2 (2026-05-18) — string→uint16 id map for outcome clustering.
 *
 * Outcomes carry small (uint16) ids for the active model and adapter.
 * This module is the lookup/assignment authority for those ids. The
 * map persists to disk so the same model name reliably gets the same
 * id across daemon restarts — that's what makes outcome clustering
 * meaningful for the training loop (otherwise "model_id 3" today
 * could be a different model tomorrow).
 *
 * Why uint16 (not the full string in every outcome record):
 *   - The outcome struct is pinned at 96 bytes (HU_M3_OUTCOME_RECORD_BYTES,
 *     _Static_assert-ed). Strings can't go in there.
 *   - 65535 distinct models/adapters is more than any deployment will
 *     ever see; the ceiling never bites in practice.
 *   - Stable ids let the training loop's analytics (per-model dedup,
 *     per-adapter accuracy gates, drift detection) work as cheap
 *     integer comparisons instead of string-tree lookups.
 *
 * Persistence shape (~/.human/training-data/m3_id_map.json):
 *   {
 *     "models":   {"name1": 1, "name2": 2, ...},
 *     "adapters": {"/path/a": 1, "/path/b": 2, ...}
 *   }
 *
 * Two separate id spaces because model and adapter are independent —
 * a fine-tuned LoRA adapter can be applied to multiple base models,
 * and a base model can have multiple adapters layered on it. Keeping
 * the spaces separate avoids ambiguity.
 *
 * Reserved id 0 means "unknown" — empty/NULL inputs map to 0 without
 * polluting the table. Numbering starts at 1. */

#include "human/core/allocator.h"
#include "human/core/error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hu_m3_id_map hu_m3_id_map_t;

/* Open (or create) the id map at `path`. NULL path → in-memory only
 * (the map still works for the session but doesn't persist). Loads
 * existing entries if the file exists; ignores malformed entries
 * (logs a warning) rather than refusing to start — a corrupted map
 * is recoverable; a missing daemon isn't.
 *
 * `path` must be a writable location if non-NULL. The parent directory
 * is NOT created — caller's responsibility (the daemon creates
 * ~/.human/training-data/ at bootstrap anyway). */
hu_error_t hu_m3_id_map_create(hu_allocator_t *alloc, const char *path, hu_m3_id_map_t **out);

void hu_m3_id_map_destroy(hu_m3_id_map_t *map);

/* Lookup the id for `model_name`, or assign the next available id if
 * unseen. Returns 0 for:
 *   - NULL map
 *   - NULL / empty / zero-length model_name
 *   - the rare ceiling case (65535 distinct models — vanishingly
 *     unlikely; we return 0 = unknown rather than crash)
 *
 * On NEW assignment, marks the map dirty. Call hu_m3_id_map_save() to
 * flush. Idempotent for already-seen names: same name → same id, no
 * dirty bit set.
 *
 * Thread-safety: the agent calls this on its own thread, never
 * concurrently from multiple threads, so no internal lock. Document
 * but don't enforce. */
uint16_t hu_m3_id_map_lookup_or_insert_model(hu_m3_id_map_t *map, const char *model_name,
                                             size_t name_len);

/* Same shape as lookup_or_insert_model but for adapter paths.
 * Separate id space (a model id of 7 and an adapter id of 7 are
 * unrelated). */
uint16_t hu_m3_id_map_lookup_or_insert_adapter(hu_m3_id_map_t *map, const char *adapter_path,
                                               size_t path_len);

/* Save the map atomically to its path (tmp + fsync + rename, same
 * pattern as personal_model.c). No-op for in-memory-only maps and
 * for maps with no dirty changes since last save. Safe to call
 * frequently; the dirty check is cheap.
 *
 * Returns HU_ERR_IO if the write fails — caller should log but not
 * abort. A failed save means new ids regress to 0 on next boot, which
 * is degraded but not broken (the training loop still functions on
 * the live in-memory ids for the rest of the session). */
hu_error_t hu_m3_id_map_save(hu_m3_id_map_t *map);

/* Read-only accessors — used by tests and the future per-model
 * analytics surfaces. */
size_t hu_m3_id_map_model_count(const hu_m3_id_map_t *map);
size_t hu_m3_id_map_adapter_count(const hu_m3_id_map_t *map);
bool hu_m3_id_map_is_dirty(const hu_m3_id_map_t *map);

#ifdef __cplusplus
}
#endif

#endif /* HU_M3_ID_MAP_H */
