#ifndef HU_PROVIDERS_COMPATIBLE_H
#define HU_PROVIDERS_COMPATIBLE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/provider.h"
#include <stddef.h>

/* Whole-request cap for a loopback upstream (mlx_local → 127.0.0.1:8741).
 * Measured prod reply p50 is 1-3 s and p99 time-to-first-token 27 s under
 * contention; 120 s is far above both and far below the 600 s shared default
 * that let a half-open socket freeze the daemon on 2026-09-03. */
#define HU_COMPATIBLE_LOCAL_TIMEOUT_SECS 120L

/* Pure predicate: fill *out with the transport caps compatible_chat will use
 * for `url`. Loopback hosts (127.0.0.1 / localhost) get
 * HU_COMPATIBLE_LOCAL_TIMEOUT_SECS; everything else gets the shared defaults
 * (all-zero opts). NULL-safe. Exposed so the cap is testable under the
 * HU_IS_TEST HTTP mock. */
void hu_compatible_request_opts_for_url(const char *url, size_t url_len,
                                        hu_http_request_opts_t *out);

hu_error_t hu_compatible_create(hu_allocator_t *alloc, const char *api_key, size_t api_key_len,
                                const char *base_url, size_t base_url_len, hu_provider_t *out);

#endif
