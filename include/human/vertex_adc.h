#ifndef HU_VERTEX_ADC_H
#define HU_VERTEX_ADC_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

/* Vertex AI ADC (Application Default Credentials) token helper.
 *
 * Reads ~/.config/gcloud/application_default_credentials.json (authorized_user
 * type) once on first call, then refreshes the access token against
 * https://oauth2.googleapis.com/token with a 5-minute safety margin before
 * expiry. The token + refresh-token + client creds are cached in a
 * pthread_mutex-protected process-static block.
 *
 * Why a shared module rather than per-provider: more than one subsystem
 * (gemini chat provider, embeddings, future Vertex tool calls) needs a Vertex
 * bearer token; without sharing, each would re-read the ADC file and
 * re-refresh, multiplying OAuth traffic and clock skew.
 */

/* Fetch a fresh Vertex AI access token via ADC. Token is duplicated into
 * *out_token (allocator-owned, NUL-terminated). Caller frees with
 * alloc->free(*out_token, *out_len + 1).
 *
 * Returns:
 *   HU_OK on success.
 *   HU_ERR_PROVIDER_AUTH if ADC file is missing/unreadable/invalid, or if the
 *     OAuth refresh fails.
 *   HU_ERR_IO if the network request to oauth2.googleapis.com fails outright.
 *   HU_ERR_OUT_OF_MEMORY on alloc failure.
 *   HU_ERR_INVALID_ARGUMENT on NULL args.
 */
hu_error_t hu_vertex_adc_token(hu_allocator_t *alloc, char **out_token, size_t *out_len);

/* Returns the quota_project_id field from the ADC credentials file, or NULL
 * if not set or ADC hasn't been loaded yet. Pointer is owned by the cache and
 * valid for the lifetime of the process. Callers MUST NOT free it.
 *
 * If GOOGLE_CLOUD_PROJECT env var is set, that takes precedence and is
 * returned instead (consistent with how src/tools/media_*.c resolves the
 * project). Returns NULL if neither source has a value.
 */
const char *hu_vertex_adc_default_project(hu_allocator_t *alloc);

/* Test-only: clear the cached token (forces a refresh on next call). No-op
 * outside HU_IS_TEST builds. */
void hu_vertex_adc_reset_cache_for_test(void);

#endif /* HU_VERTEX_ADC_H */
