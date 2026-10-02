#ifndef HU_CORE_LOG_REDACT_H
#define HU_CORE_LOG_REDACT_H

/* Keep message text and contact handles out of production logs (DEF-12).
 *
 * Prod runs with HU_DEBUG=1, so HU_DEBUG cannot be the switch. Text and raw
 * handles are logged only when HU_LOG_CONTENT=1 (off by default, a local
 * debugging aid). Otherwise a handle renders as a keyed 48-bit tag
 * ("#7a50b4f77146", see hu_log_contact_tag), stable across restarts so a
 * contact's lines can be counted, and text renders as its length.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* HU_LOG_CONTENT=1 (exactly "1"). Read on every call; cheap. */
bool hu_log_content_enabled(void);

/* Tests: force 0/1, or -1 to read the env again. */
void hu_log_content_set_for_test(int on_or_minus1);

/* Keyed SipHash-2-4 of the handle, truncated to 48 bits. The 16-byte key is
 * random per install, created once at <state dir>/log_tag.key (0600), so a
 * tag cannot be reversed or matched without that file. Test builds use a
 * fixed in-memory key and never touch the state dir. */
uint64_t hu_log_contact_tag(const char *handle, size_t len);

/* Read the key from <dir>/log_tag.key, creating it (0600, O_EXCL) with fresh
 * random bytes when absent. False on any I/O error or a short/oversized file. */
bool hu_log_tag_key_load(const char *dir, uint8_t key[16]);

/* The runtime key: <dir>/log_tag.key when it loads, else a per-process random
 * key (unlinkable, not stable) with one warning per process. Returns true when
 * the key came from the file. */
bool hu_log_tag_key_resolve(const char *dir, uint8_t key[16]);

/* Tests: use this key (NULL restores the build default). */
void hu_log_tag_set_key_for_test(const uint8_t *key16);

/* "#" + 12 hex digits, or the raw handle (first 24 bytes) under HU_LOG_CONTENT=1.
 * Writes into buf and returns it; "-" for a NULL/empty handle. */
const char *hu_log_who(const char *handle, size_t len, char *buf, size_t cap);

/* "<N chars>", or the first `max` bytes of text under HU_LOG_CONTENT=1. */
const char *hu_log_text(const char *text, size_t len, size_t max, char *buf, size_t cap);

/* Call-site forms: the compound-literal buffer lives until the end of the
 * enclosing block, which outlives the log call. Format with %s. */
/* NULL-safe strlen as a function, not `(s) ? strlen(s) : 0` in the macro:
 * when a caller passes a char array GCC's -Waddress flags the test as
 * always true, and -Werror turns that into a build break. */
static inline size_t hu_log_cstrlen(const char *s) {
    return s ? strlen(s) : 0;
}

#define HU_LOG_WHO(h, n)         hu_log_who((h), (n), (char[32]){0}, 32)
#define HU_LOG_WHO_CSTR(h)       hu_log_who((h), hu_log_cstrlen(h), (char[32]){0}, 32)
#define HU_LOG_TEXT(t, n, max)   hu_log_text((t), (n), (max), (char[128]){0}, 128)
#define HU_LOG_TEXT_CSTR(t, max) hu_log_text((t), hu_log_cstrlen(t), (max), (char[128]){0}, 128)

#endif /* HU_CORE_LOG_REDACT_H */
