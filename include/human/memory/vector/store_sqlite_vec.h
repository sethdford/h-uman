#ifndef HUMAN_MEMORY_VECTOR_STORE_SQLITE_VEC_H
#define HUMAN_MEMORY_VECTOR_STORE_SQLITE_VEC_H

#include "human/core/allocator.h"
#include "human/memory/vector.h"

#include <stddef.h>

struct sqlite3;

#ifdef __cplusplus
extern "C" {
#endif

/* Persistent vector store on the memory database itself, via the vendored
 * sqlite-vec extension (third_party/sqlite-vec, pure C, exact KNN).
 * Tables: memories_vec (vec0, float[dim]) + memories_vec_meta(id, content).
 * The store does NOT own `db`; the engine that opened it does.
 * Returns a store with ctx == NULL when sqlite-vec is unavailable or `dim` is
 * 0 — callers must check, never silently fall back to the hash embedder. */
hu_vector_store_t hu_vector_store_sqlite_vec_create(hu_allocator_t *alloc, struct sqlite3 *db,
                                                    size_t dim);

/* Registers sqlite3_vec_init as an auto-extension for every connection opened
 * afterwards. Idempotent. Returns false if the extension could not register. */
bool hu_sqlite_vec_register(void);

#ifdef __cplusplus
}
#endif

#endif /* HUMAN_MEMORY_VECTOR_STORE_SQLITE_VEC_H */
