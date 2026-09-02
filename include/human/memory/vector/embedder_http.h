#ifndef HUMAN_MEMORY_VECTOR_EMBEDDER_HTTP_H
#define HUMAN_MEMORY_VECTOR_EMBEDDER_HTTP_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/vector.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Embedder backed by an OpenAI-shaped POST {base_url}/v1/embeddings — in
 * production the h-uman mlx-server on :8741, which hosts the nomic embedder
 * in-process (no second model loader). Requests carry X-HU-Priority: batch so
 * indexing never delays a live reply.
 *
 * This replaces embedder_local.c on the semantic-recall path: that one is a
 * hash projection with no synonymy ("reports success while doing nothing" #5).
 * Failures are surfaced, never a zero vector: HU_ERR_PROVIDER_UNAVAILABLE when the server
 * is unreachable, HU_ERR_PROVIDER_RESPONSE when the body is not the expected shape. */
hu_embedder_t hu_embedder_http_create(hu_allocator_t *alloc, const char *base_url);

/* Pure parser, exposed for tests: fills `out[i]` (allocating values) from an
 * OpenAI-shaped embeddings response for `expect_count` inputs. Returns
 * HU_ERR_PROVIDER_RESPONSE on any shape mismatch (missing data, wrong count, ragged or
 * zero-length vectors); on error nothing is left allocated. */
hu_error_t hu_embedder_http_parse_response(hu_allocator_t *alloc, const char *body, size_t body_len,
                                           size_t expect_count, hu_embedding_t *out);

#ifdef __cplusplus
}
#endif

#endif /* HUMAN_MEMORY_VECTOR_EMBEDDER_HTTP_H */
