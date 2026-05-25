#ifndef HU_MEMORY_VECTOR_EMBEDDINGS_GEMINI_H
#define HU_MEMORY_VECTOR_EMBEDDINGS_GEMINI_H

#include "human/core/allocator.h"
#include "human/memory/vector/embeddings.h"

/* Create Gemini embedding provider (legacy: AI Studio API-key auth).
 * api_key: required for real API
 * model: optional, default "text-embedding-004"
 * dims: 0 = use default 768
 *
 * Calls https://generativelanguage.googleapis.com/v1beta/models/{model}:embedContent
 * with ?key=api_key. Returns 403 PERMISSION_DENIED if api_key is empty —
 * prefer hu_embedding_gemini_create_vertex() for h-uman's typical
 * ADC-authenticated deployment.
 */
hu_embedding_provider_t hu_embedding_gemini_create(hu_allocator_t *alloc, const char *api_key,
                                                   const char *model, size_t dims);

/* Create Gemini embedding provider via Vertex AI + ADC bearer token.
 *
 * project_id: GCP project (e.g. "johnb-2025"). NULL → tries env GOOGLE_CLOUD_PROJECT
 *             then the ADC file's quota_project_id via hu_vertex_adc_default_project().
 * location:   Vertex region (e.g. "us-central1"). NULL defaults to "us-central1"
 *             which is where text-embedding-004 is GA.
 * model:      defaults to "text-embedding-004".
 * dims:       0 → default 768.
 *
 * Calls
 * https://{location}-aiplatform.googleapis.com/v1/projects/{project}/locations/{location}/publishers/google/models/{model}:predict
 * with a per-call fresh Bearer token from hu_vertex_adc_token(). Returns the
 * noop provider if no project can be resolved (fail-closed: avoids unauth
 * loops).
 */
hu_embedding_provider_t hu_embedding_gemini_create_vertex(hu_allocator_t *alloc,
                                                          const char *project_id,
                                                          const char *location, const char *model,
                                                          size_t dims);

#endif
