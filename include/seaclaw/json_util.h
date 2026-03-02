#ifndef SC_JSON_UTIL_H
#define SC_JSON_UTIL_H

#include "core/allocator.h"
#include "core/error.h"
#include "core/json.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ──────────────────────────────────────────────────────────────────────────
 * JSON helpers — null-terminated string convenience wrappers over core/json.
 * For building JSON output (e.g. auth.json, state.json).
 * ────────────────────────────────────────────────────────────────────────── */

/* Append JSON-escaped string with enclosing quotes. Uses sc_json_buf_t. */
sc_error_t sc_json_util_append_string(sc_json_buf_t *buf, const char *s);

/* Append "key": (JSON-escaped key with colon). */
sc_error_t sc_json_util_append_key(sc_json_buf_t *buf, const char *key);

/* Append "key":"value" (both JSON-escaped). */
sc_error_t sc_json_util_append_key_value(sc_json_buf_t *buf,
                                          const char *key,
                                          const char *value);

/* Append "key":<integer>. */
sc_error_t sc_json_util_append_key_int(sc_json_buf_t *buf,
                                       const char *key,
                                       int64_t value);

#endif /* SC_JSON_UTIL_H */
