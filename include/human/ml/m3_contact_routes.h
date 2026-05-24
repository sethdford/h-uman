#ifndef HU_M3_CONTACT_ROUTES_H
#define HU_M3_CONTACT_ROUTES_H

/* Phase F2 (2026-05-18) — C-side mirror of scripts/m3_contact_routing.py.
 *
 * Reads the persisted ~/.human/training-data/m3_contact_routes.json
 * (written by the Python operator CLI) and exposes a single lookup
 * function for the agent's chat path to call before each turn:
 *
 *     const char *adapter = hu_m3_contact_routes_lookup(routes,
 *                                                       contact_id_hash);
 *     if (adapter) {
 *         hu_mlx_admin_swap_adapter(http, mlx_url, adapter, NULL);
 *     }
 *
 * The contact_id_hash is computed by the chat path via
 * hu_m3_outcome_hash_bytes(contact_id) — same algorithm Python uses,
 * pinned cross-language by tests/test_ml.c::
 * test_m3_outcome_hash_bytes_pins_cross_language_vectors (D4).
 *
 * Lookup precedence (same as Python's m3_contact_routing.py):
 *   1. Specific contact's route
 *   2. Top-level default_adapter
 *   3. NULL (caller uses base model — no swap)
 *
 * Resilience: load failures (missing file, malformed JSON) are
 * NON-FATAL. The lookup returns NULL and the chat path continues
 * with the base model. The C side mirrors Python's "corrupt routes
 * file MUST NOT block inference" stance.
 *
 * Lifecycle:
 *   - Caller owns the hu_m3_contact_routes_t* via create/destroy
 *   - Reload at runtime via reload() if operator updated the file
 *     (no automatic reload — explicit by design; the inference path
 *     wants stable routing during a turn)
 *
 * Thread-safety:
 *   - lookup is read-only and safe to call concurrently
 *   - reload is NOT thread-safe vs lookup (caller serializes)
 */

#include "human/core/allocator.h"
#include "human/core/error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hu_m3_contact_routes hu_m3_contact_routes_t;

/* Open the routes file at `path`. NULL path → use default
 * (~/.human/training-data/m3_contact_routes.json). Missing file or
 * malformed JSON → returns HU_OK with empty routes (lookup returns
 * NULL for everything). Memory allocation failure → HU_ERR_OUT_OF_MEMORY. */
hu_error_t hu_m3_contact_routes_create(hu_allocator_t *alloc, const char *path,
                                       hu_m3_contact_routes_t **out);

void hu_m3_contact_routes_destroy(hu_m3_contact_routes_t *routes);

/* Re-read the routes file. Caller is responsible for ensuring no
 * concurrent lookup is in flight. Returns HU_OK whether or not the
 * file changed; HU_ERR_IO if the file became unreadable. */
hu_error_t hu_m3_contact_routes_reload(hu_m3_contact_routes_t *routes);

/* Resolve a contact_id_hash to an adapter path string.
 *
 * Returns:
 *   - pointer to the adapter path (owned by routes; valid until
 *     next reload or destroy) when the contact has a specific route
 *   - pointer to the default adapter when set and no specific route
 *   - NULL when no route applies (caller continues with base model)
 *
 * Returns NULL for NULL routes input.
 *
 * The contact_id_hash is the same uint64_t the outcome ring records
 * — typically computed via hu_m3_outcome_hash_bytes(contact_id). */
const char *hu_m3_contact_routes_lookup(const hu_m3_contact_routes_t *routes,
                                        uint64_t contact_id_hash);

/* Read-only accessors for tests + status surfaces. */
size_t hu_m3_contact_routes_count(const hu_m3_contact_routes_t *routes);
const char *hu_m3_contact_routes_default(const hu_m3_contact_routes_t *routes);

#ifdef __cplusplus
}
#endif

#endif /* HU_M3_CONTACT_ROUTES_H */
