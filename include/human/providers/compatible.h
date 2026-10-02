#ifndef HU_PROVIDERS_COMPATIBLE_H
#define HU_PROVIDERS_COMPATIBLE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/provider.h"
#include <stdbool.h>
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

/* True when `p` is an OpenAI-compatible provider whose base_url host is the
 * loopback interface (127.0.0.1 / localhost) — a model on this machine. False
 * for any other provider type, including wrappers around one. NULL-safe. */
bool hu_compatible_is_loopback(const hu_provider_t *p);

/* Name the purpose of this thread's next non-stream requests: compatible_chat
 * sends "X-HU-Purpose: <name>" so the local server can log what each call is
 * for (never message text). `name` must be a static string matching
 * [a-z_]{1,24}; anything else clears the tag. NULL clears. Returns the previous
 * tag so the caller can restore it. No X-HU-Priority is added. */
const char *hu_compatible_purpose_set(const char *name);
const char *hu_compatible_purpose_current(void);

#if defined(HU_IS_TEST) && HU_IS_TEST
/* The extra-header block this thread's last compatible_chat carried ("" when
 * untagged). */
const char *hu_compatible_test_last_headers(void);
#endif

hu_error_t hu_compatible_create(hu_allocator_t *alloc, const char *api_key, size_t api_key_len,
                                const char *base_url, size_t base_url_len, hu_provider_t *out);

#endif
