/* response_guard_dpo.c — capture response_guard REJECTs as DPO negative
 * pairs. See include/human/agent/response_guard_dpo.h for the contract. */

#include "human/agent/response_guard_dpo.h"
#include "human/core/log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifndef HU_DPO_REJECTIONS_PATH
#define HU_DPO_REJECTIONS_PATH "/.human/training-data/m3-dpo-rejections.jsonl"
#endif

/* Append-escape one JSON string into out[*pos..out_cap). Returns false if
 * the buffer would overflow (caller truncates). */
static bool append_json_string(const char *s, size_t s_len, char *out, size_t out_cap,
                               size_t *pos) {
    if (*pos + 2 >= out_cap)
        return false;
    out[(*pos)++] = '"';
    for (size_t i = 0; i < s_len; i++) {
        unsigned char c = (unsigned char)s[i];
        const char *esc = NULL;
        char esc_buf[8];
        switch (c) {
        case '"':
            esc = "\\\"";
            break;
        case '\\':
            esc = "\\\\";
            break;
        case '\n':
            esc = "\\n";
            break;
        case '\r':
            esc = "\\r";
            break;
        case '\t':
            esc = "\\t";
            break;
        case '\b':
            esc = "\\b";
            break;
        case '\f':
            esc = "\\f";
            break;
        default:
            if (c < 0x20) {
                snprintf(esc_buf, sizeof(esc_buf), "\\u%04x", c);
                esc = esc_buf;
            }
            break;
        }
        if (esc) {
            size_t elen = strlen(esc);
            if (*pos + elen + 2 >= out_cap)
                return false;
            memcpy(out + *pos, esc, elen);
            *pos += elen;
        } else {
            if (*pos + 2 >= out_cap)
                return false;
            out[(*pos)++] = (char)c;
        }
    }
    if (*pos + 2 >= out_cap)
        return false;
    out[(*pos)++] = '"';
    return true;
}

static bool append_literal(const char *lit, char *out, size_t out_cap, size_t *pos) {
    size_t llen = strlen(lit);
    if (*pos + llen + 1 >= out_cap)
        return false;
    memcpy(out + *pos, lit, llen);
    *pos += llen;
    return true;
}

size_t hu_response_guard_format_dpo_negative_jsonl(const char *prompt, size_t prompt_len,
                                                   const char *rejected, size_t rejected_len,
                                                   const char *detector, const char *channel,
                                                   int64_t ts_unix, char *out, size_t out_cap) {
    if (!out || out_cap == 0)
        return 0;
    size_t pos = 0;

#define ABORT_TRUNC()          \
    do {                       \
        if (pos >= out_cap)    \
            pos = out_cap - 1; \
        out[pos] = '\0';       \
        return pos;            \
    } while (0)

    if (!append_literal("{\"prompt\":", out, out_cap, &pos))
        ABORT_TRUNC();
    if (prompt) {
        if (!append_json_string(prompt, prompt_len, out, out_cap, &pos))
            ABORT_TRUNC();
    } else {
        if (!append_literal("null", out, out_cap, &pos))
            ABORT_TRUNC();
    }

    /* `chosen` is always null at capture time — the downstream DPO
     * pairing step attaches the successful retry reply (when one
     * exists) as a separate post-process. */
    if (!append_literal(",\"chosen\":null,\"rejected\":", out, out_cap, &pos))
        ABORT_TRUNC();
    if (rejected) {
        if (!append_json_string(rejected, rejected_len, out, out_cap, &pos))
            ABORT_TRUNC();
    } else {
        if (!append_literal("null", out, out_cap, &pos))
            ABORT_TRUNC();
    }

    if (!append_literal(",\"_source\":\"response_guard\",\"_detector\":", out, out_cap, &pos))
        ABORT_TRUNC();
    if (detector) {
        if (!append_json_string(detector, strlen(detector), out, out_cap, &pos))
            ABORT_TRUNC();
    } else {
        if (!append_literal("null", out, out_cap, &pos))
            ABORT_TRUNC();
    }

    if (!append_literal(",\"_channel\":", out, out_cap, &pos))
        ABORT_TRUNC();
    if (channel) {
        if (!append_json_string(channel, strlen(channel), out, out_cap, &pos))
            ABORT_TRUNC();
    } else {
        if (!append_literal("null", out, out_cap, &pos))
            ABORT_TRUNC();
    }

    char ts_buf[32];
    int n = snprintf(ts_buf, sizeof(ts_buf), ",\"_ts_unix\":%lld}", (long long)ts_unix);
    if (n <= 0 || pos + (size_t)n + 1 >= out_cap)
        ABORT_TRUNC();
    memcpy(out + pos, ts_buf, (size_t)n);
    pos += (size_t)n;
    out[pos] = '\0';
    return pos;
#undef ABORT_TRUNC
}

hu_error_t hu_response_guard_log_dpo_negative(const char *prompt, size_t prompt_len,
                                              const char *rejected, size_t rejected_len,
                                              const char *detector, const char *channel,
                                              int64_t ts_unix) {
#ifdef HU_IS_TEST
    /* Tests must not write to disk — keeps the suite hermetic. The pure
     * formatter is still tested directly. */
    (void)prompt;
    (void)prompt_len;
    (void)rejected;
    (void)rejected_len;
    (void)detector;
    (void)channel;
    (void)ts_unix;
    return HU_OK;
#else
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return HU_ERR_IO;

    /* Build the full path. Use a stack buffer — paths over 1KB on real
     * systems are vanishingly rare and we'd rather fail than alloc. */
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s%s", home, HU_DPO_REJECTIONS_PATH);
    if (n <= 0 || (size_t)n >= sizeof(path))
        return HU_ERR_IO;

    /* Format the JSONL line into a stack buffer. 8 KiB is more than enough
     * for any rejected response (response_guard already rejects on length
     * anomaly far below this). */
    char line[8192];
    size_t line_len = hu_response_guard_format_dpo_negative_jsonl(
        prompt, prompt_len, rejected, rejected_len, detector, channel, ts_unix, line, sizeof(line));
    if (line_len == 0)
        return HU_ERR_IO;

    FILE *f = fopen(path, "a");
    if (!f) {
        hu_log_warn("response_guard_dpo", NULL,
                    "DPO rejection log append failed: fopen %s: %s (rejection NOT captured for "
                    "future LoRA training)",
                    path, strerror(errno));
        return HU_ERR_IO;
    }
    if (fwrite(line, 1, line_len, f) != line_len || fputc('\n', f) == EOF) {
        fclose(f);
        return HU_ERR_IO;
    }
    if (fclose(f) != 0)
        return HU_ERR_IO;
    return HU_OK;
#endif
}
