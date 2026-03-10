#ifndef HU_NET_SECURITY_H
#define HU_NET_SECURITY_H

#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>

/* Validate URL: reject non-HTTPS. Allow http://localhost for dev. */
hu_error_t hu_validate_url(const char *url);

/* Returns true if the IP string is private/reserved (RFC1918, loopback, etc.). */
bool hu_is_private_ip(const char *ip);

/* Check if host matches domain allowlist (exact or *.domain patterns). */
bool hu_validate_domain(const char *host, const char *const *allowed, size_t allowed_count);

#endif
