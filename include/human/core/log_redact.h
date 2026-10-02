#ifndef HU_CORE_LOG_REDACT_H
#define HU_CORE_LOG_REDACT_H

/* Keep message text and contact handles out of production logs (DEF-12).
 *
 * Prod runs with HU_DEBUG=1, so HU_DEBUG cannot be the switch. Text and raw
 * handles are logged only when HU_LOG_CONTENT=1 (off by default, a local
 * debugging aid). Otherwise a handle renders as a 16-bit tag ("#3fa2") — the
 * same FNV fold as the voice-first shadow line, stable across restarts so a
 * contact's lines can be counted — and text renders as its length. The tag is
 * pseudonymous, not anonymous: anyone holding the contact list can hash it. */

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* HU_LOG_CONTENT=1 (exactly "1"). Read on every call; cheap. */
bool hu_log_content_enabled(void);

/* Tests: force 0/1, or -1 to read the env again. */
void hu_log_content_set_for_test(int on_or_minus1);

/* FNV-1a folded to 16 bits. */
unsigned hu_log_contact_tag(const char *handle, size_t len);

/* "#%04x", or the raw handle (first 24 bytes) under HU_LOG_CONTENT=1.
 * Writes into buf and returns it; "-" for a NULL/empty handle. */
const char *hu_log_who(const char *handle, size_t len, char *buf, size_t cap);

/* "<N chars>", or the first `max` bytes of text under HU_LOG_CONTENT=1. */
const char *hu_log_text(const char *text, size_t len, size_t max, char *buf, size_t cap);

/* Call-site forms: the compound-literal buffer lives until the end of the
 * enclosing block, which outlives the log call. Format with %s. */
#define HU_LOG_WHO(h, n)         hu_log_who((h), (n), (char[32]){0}, 32)
#define HU_LOG_WHO_CSTR(h)       hu_log_who((h), (h) ? strlen(h) : 0, (char[32]){0}, 32)
#define HU_LOG_TEXT(t, n, max)   hu_log_text((t), (n), (max), (char[128]){0}, 128)
#define HU_LOG_TEXT_CSTR(t, max) hu_log_text((t), (t) ? strlen(t) : 0, (max), (char[128]){0}, 128)

#endif /* HU_CORE_LOG_REDACT_H */
