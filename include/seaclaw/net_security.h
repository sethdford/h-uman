#ifndef SC_NET_SECURITY_H
#define SC_NET_SECURITY_H

#include "seaclaw/core/error.h"
#include <stdbool.h>
#include <stddef.h>

/* Validate URL: reject non-HTTPS. Allow http://localhost for dev. */
sc_error_t sc_validate_url(const char *url);

/* Returns true if the IP string is private/reserved (RFC1918, loopback, etc.). */
bool sc_is_private_ip(const char *ip);

/* Check if host matches domain allowlist (exact or *.domain patterns). */
bool sc_validate_domain(const char *host,
    const char *const *allowed,
    size_t allowed_count);

#endif
