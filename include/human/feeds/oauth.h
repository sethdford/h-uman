#ifndef HU_FEEDS_OAUTH_H
#define HU_FEEDS_OAUTH_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

/* Store OAuth token for a provider. INSERT OR REPLACE into oauth_tokens. */
hu_error_t hu_oauth_store_token(sqlite3 *db, const char *provider,
                               const char *access_token, const char *refresh_token,
                               int64_t expires_at, const char *scope);

/* Get access token for provider. Caller provides buffer; at_len is set to actual length. */
hu_error_t hu_oauth_get_token(hu_allocator_t *alloc, sqlite3 *db,
                              const char *provider, char *access_token, size_t at_cap,
                              size_t *at_len);

/* Return true if token is expired (expires_at - 300 < now_ts). */
bool hu_oauth_is_expired(sqlite3 *db, const char *provider, int64_t now_ts);

#endif /* HU_ENABLE_SQLITE */

#endif /* HU_FEEDS_OAUTH_H */
