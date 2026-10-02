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

/* Null-distribution sample for relevance calibration (HU_CONTEXT_RELEVANCE):
 * cosine similarity of `query` against up to `k` stored vectors chosen at
 * random. Dot products only: no ids, keys or text leave this function.
 * Writes *n_out <= k scores to out[]. HU_ERR_NOT_SUPPORTED when `vs` is not a
 * sqlite-vec store (or the build has none); HU_ERR_INVALID_ARGUMENT on a
 * dimension mismatch. */
hu_error_t hu_vector_store_sqlite_vec_sample_scores(const hu_vector_store_t *vs,
                                                    const hu_embedding_t *query, size_t k,
                                                    float *out, size_t *n_out);

#ifdef __cplusplus
}
#endif

#endif /* HUMAN_MEMORY_VECTOR_STORE_SQLITE_VEC_H */
