/* Phase D7 (2026-05-18) — REWRITE preference-pair capture.
 *
 * See include/human/ml/m3_rewrite_capture.h for the why. This file is
 * the how:
 *   - Resolve the default JSONL path under $HOME/.human/training-data
 *   - JSON-escape the three text fields (prompt, rejected, accepted)
 *   - Truncate each to HU_M3_REWRITE_PAIR_MAX_FIELD_BYTES
 *   - Build the single record line via hu_json_buf_t
 *   - fopen("a") + fwrite + fclose — POSIX guarantees atomicity
 *     for writes < PIPE_BUF (4096 B), which our cap respects
 *
 * No locks, no buffering — fast enough to call from the chat hot path
 * without measurable overhead (typical record is ~2KB, single write).
 */

#include "human/ml/m3_rewrite_capture.h"

#include "human/core/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#define DEFAULT_FILENAME "m3-rewrite-pairs.jsonl"

static uint64_t now_unix_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

/* Build the JSONL line into the allocator-owned buffer. The caller
 * frees via hu_json_buf_free(). Returns HU_OK on success. */
static hu_error_t build_record_line(hu_json_buf_t *buf, const char *prompt, size_t prompt_len,
                                    const char *rejected, size_t rejected_len, const char *accepted,
                                    size_t accepted_len, unsigned char turn_kind) {
    /* Per-field cap. Truncation is OK; better a truncated training
     * sample than no sample at all. */
    size_t p_cap = prompt_len < HU_M3_REWRITE_PAIR_MAX_FIELD_BYTES
                       ? prompt_len
                       : HU_M3_REWRITE_PAIR_MAX_FIELD_BYTES;
    size_t r_cap = rejected_len < HU_M3_REWRITE_PAIR_MAX_FIELD_BYTES
                       ? rejected_len
                       : HU_M3_REWRITE_PAIR_MAX_FIELD_BYTES;
    size_t a_cap = accepted_len < HU_M3_REWRITE_PAIR_MAX_FIELD_BYTES
                       ? accepted_len
                       : HU_M3_REWRITE_PAIR_MAX_FIELD_BYTES;

#define TRY(expr)               \
    do {                        \
        hu_error_t _e = (expr); \
        if (_e != HU_OK)        \
            return _e;          \
    } while (0)

    TRY(hu_json_buf_append_raw(buf, "{", 1));

    /* "t": timestamp_ms */
    char tbuf[32];
    int tn = snprintf(tbuf, sizeof(tbuf), "\"t\":%llu", (unsigned long long)now_unix_ms());
    if (tn < 0 || (size_t)tn >= sizeof(tbuf))
        return HU_ERR_INTERNAL;
    TRY(hu_json_buf_append_raw(buf, tbuf, (size_t)tn));

    /* "ph": prompt_hash — for cross-reference with the outcome ring */
    extern uint64_t hu_m3_outcome_hash_bytes(const void *data, size_t len);
    uint64_t ph = hu_m3_outcome_hash_bytes(prompt, p_cap);
    char hbuf[48];
    int hn = snprintf(hbuf, sizeof(hbuf), ",\"ph\":%llu", (unsigned long long)ph);
    if (hn < 0 || (size_t)hn >= sizeof(hbuf))
        return HU_ERR_INTERNAL;
    TRY(hu_json_buf_append_raw(buf, hbuf, (size_t)hn));

    /* "k": turn_kind */
    char kbuf[16];
    int kn = snprintf(kbuf, sizeof(kbuf), ",\"k\":%u", (unsigned)turn_kind);
    if (kn < 0 || (size_t)kn >= sizeof(kbuf))
        return HU_ERR_INTERNAL;
    TRY(hu_json_buf_append_raw(buf, kbuf, (size_t)kn));

    /* "prompt": "<text>" — string-escaped */
    TRY(hu_json_buf_append_raw(buf, ",\"prompt\":", 10));
    TRY(hu_json_append_string(buf, prompt, p_cap));

    /* "rejected": "<text>" */
    TRY(hu_json_buf_append_raw(buf, ",\"rejected\":", 12));
    TRY(hu_json_append_string(buf, rejected, r_cap));

    /* "accepted": "<text>" */
    TRY(hu_json_buf_append_raw(buf, ",\"accepted\":", 12));
    TRY(hu_json_append_string(buf, accepted, a_cap));

    TRY(hu_json_buf_append_raw(buf, "}\n", 2));
    return HU_OK;
#undef TRY
}

hu_error_t hu_m3_rewrite_pair_record(hu_allocator_t *alloc, const char *path, const char *prompt,
                                     size_t prompt_len, const char *rejected, size_t rejected_len,
                                     const char *accepted, size_t accepted_len,
                                     unsigned char turn_kind) {
    /* Defensive: NULL/empty content is a degenerate pair — skip. */
    if (!alloc || !prompt || prompt_len == 0 || !rejected || rejected_len == 0 || !accepted ||
        accepted_len == 0) {
        return HU_ERR_INVALID_ARGUMENT;
    }

    /* Resolve the destination path. Caller may override (tests), or
     * we use ~/.human/training-data/m3-rewrite-pairs.jsonl. */
    char default_path[2048];
    const char *out_path = path;
    if (!out_path || !out_path[0]) {
        const char *home = getenv("HOME");
        if (!home || !home[0])
            return HU_ERR_IO;
        int n = snprintf(default_path, sizeof(default_path),
                         "%s/.human/training-data/" DEFAULT_FILENAME, home);
        if (n < 0 || (size_t)n >= sizeof(default_path))
            return HU_ERR_INTERNAL;
        out_path = default_path;
    }

    /* Build the line. */
    hu_json_buf_t buf;
    hu_error_t err = hu_json_buf_init(&buf, alloc);
    if (err != HU_OK)
        return err;
    err = build_record_line(&buf, prompt, prompt_len, rejected, rejected_len, accepted,
                            accepted_len, turn_kind);
    if (err != HU_OK) {
        hu_json_buf_free(&buf);
        return err;
    }

    /* Append. Single fwrite call — atomic for sizes under PIPE_BUF.
     * On failure paths we free the buf BEFORE returning. We capture
     * buf.len into a local so the post-free check doesn't UAF. */
    size_t buf_len = buf.len;
    FILE *fp = fopen(out_path, "a");
    if (!fp) {
        hu_json_buf_free(&buf);
        return HU_ERR_IO;
    }
    size_t wrote = fwrite(buf.ptr, 1, buf_len, fp);
    fclose(fp);
    hu_json_buf_free(&buf);
    return (wrote == buf_len) ? HU_OK : HU_ERR_IO;
}
