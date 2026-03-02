#include "seaclaw/providers/scrub.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define SC_SCRUB_REDACTED "[REDACTED]"
#define SC_SCRUB_MAX_ERROR 200

static bool is_secret_char(char c) {
    return isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == ':';
}

static size_t token_end(const char *s, size_t from, size_t len) {
    while (from < len && is_secret_char(s[from])) from++;
    return from;
}

static bool match_bearer(const char *s, size_t len, size_t pos, size_t *prefix_len, size_t *end) {
    static const char *prefixes[] = { "Bearer ", "bearer ", "BEARER " };
    for (size_t p = 0; p < sizeof(prefixes)/sizeof(prefixes[0]); p++) {
        size_t pl = strlen(prefixes[p]);
        if (pos + pl <= len && strncasecmp(s + pos, prefixes[p], pl) == 0) {
            size_t tok = token_end(s, pos + pl, len);
            if (tok > pos + pl) {
                *prefix_len = pl;
                *end = tok;
                return true;
            }
        }
    }
    return false;
}

static const char *secret_prefixes[] = {
    "sk-", "xoxb-", "xoxp-", "ghp_", "gho_", "ghs_", "ghu_", "glpat-",
    "AKIA", "pypi-", "npm_", "shpat_",
};
#define NUM_PREFIXES (sizeof(secret_prefixes)/sizeof(secret_prefixes[0]))

char *sc_scrub_secret_patterns(sc_allocator_t *alloc, const char *input, size_t input_len) {
    if (!input) input = "";
    if (input_len == 0) input_len = strlen(input);

    sc_json_buf_t buf;
    if (sc_json_buf_init(&buf, alloc) != 0) return NULL;

    size_t i = 0;
    while (i < input_len) {
        size_t prefix_len, end;
        if (match_bearer(input, input_len, i, &prefix_len, &end)) {
            sc_json_buf_append_raw(&buf, input + i, prefix_len);
            if (end - (i + prefix_len) > 4)
                sc_json_buf_append_raw(&buf, input + i + prefix_len, 4);
            sc_json_buf_append_raw(&buf, SC_SCRUB_REDACTED, sizeof(SC_SCRUB_REDACTED) - 1);
            i = end;
            continue;
        }

        bool matched = false;
        for (size_t p = 0; p < NUM_PREFIXES; p++) {
            size_t pl = strlen(secret_prefixes[p]);
            if (i + pl <= input_len && memcmp(input + i, secret_prefixes[p], pl) == 0) {
                size_t tok = token_end(input, i + pl, input_len);
                if (tok > i + pl) {
                    sc_json_buf_append_raw(&buf, SC_SCRUB_REDACTED, sizeof(SC_SCRUB_REDACTED) - 1);
                    i = tok;
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) {
            char c = input[i];
            sc_json_buf_append_raw(&buf, &c, 1);
            i++;
        }
    }

    size_t n = buf.len;
    char *out = (char *)alloc->alloc(alloc->ctx, n + 1);
    if (!out) { sc_json_buf_free(&buf); return NULL; }
    memcpy(out, buf.ptr, n);
    out[n] = '\0';
    sc_json_buf_free(&buf);
    return out;
}

char *sc_scrub_sanitize_api_error(sc_allocator_t *alloc, const char *input, size_t input_len) {
    char *scrubbed = sc_scrub_secret_patterns(alloc, input, input_len);
    if (!scrubbed) return NULL;
    size_t len = strlen(scrubbed);
    if (len <= SC_SCRUB_MAX_ERROR) return scrubbed;
    char *trunc = (char *)alloc->alloc(alloc->ctx, SC_SCRUB_MAX_ERROR + 4);
    if (!trunc) { alloc->free(alloc->ctx, scrubbed, len + 1); return NULL; }
    memcpy(trunc, scrubbed, SC_SCRUB_MAX_ERROR);
    memcpy(trunc + SC_SCRUB_MAX_ERROR, "...", 3);
    trunc[SC_SCRUB_MAX_ERROR + 3] = '\0';
    alloc->free(alloc->ctx, scrubbed, len + 1);
    return trunc;
}
