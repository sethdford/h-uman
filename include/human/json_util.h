#ifndef HU_JSON_UTIL_H
#define HU_JSON_UTIL_H

#include "core/allocator.h"
#include "core/error.h"
#include "core/json.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * JSON helpers — null-terminated string convenience wrappers over core/json.
 * For building JSON output (e.g. auth.json, state.json).
 * ────────────────────────────────────────────────────────────────────────── */

/* Append JSON-escaped string with enclosing quotes. Uses hu_json_buf_t. */
hu_error_t hu_json_util_append_string(hu_json_buf_t *buf, const char *s);

/* Append "key": (JSON-escaped key with colon). */
hu_error_t hu_json_util_append_key(hu_json_buf_t *buf, const char *key);

/* Append "key":"value" (both JSON-escaped). */
hu_error_t hu_json_util_append_key_value(hu_json_buf_t *buf, const char *key, const char *value);

/* Append "key":<integer>. */
hu_error_t hu_json_util_append_key_int(hu_json_buf_t *buf, const char *key, int64_t value);

#endif /* HU_JSON_UTIL_H */
