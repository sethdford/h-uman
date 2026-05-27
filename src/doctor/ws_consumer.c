/* h-uman doctor WebSocket consumer — see header for spec.
 *
 * T1 (this file): pure helpers only — format_event_line and
 * event_matches_filter. hu_doctor_ws_watch returns
 * HU_ERR_NOT_SUPPORTED for now; T2-T6 land the socket loop.
 *
 * Spec: docs/plans/2026-05-27-doctor-ws-consumer/
 */

#include "human/doctor/ws_consumer.h"
#include "human/core/string.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

hu_doctor_ws_config_t hu_doctor_ws_config_default(void) {
    hu_doctor_ws_config_t c = {0};
    c.host = "127.0.0.1";
    c.port = 3006;
    c.path = "/ws";
    c.event_filter = NULL;
    c.log_path = NULL;
    c.max_reconnect_attempts = 3;
    c.quiet_stdout = false;
    return c;
}

/* Walk a comma-separated filter, calling `match` for each token (with
 * surrounding whitespace trimmed). Returns immediately when match returns
 * true. Returns false if no token matches (or filter_csv is empty).
 *
 * Pure scan helper used by event_matches_filter; kept private. */
static bool filter_csv_any_token(const char *filter_csv,
                                 bool (*match)(const char *token, size_t len, void *ctx),
                                 void *ctx) {
    if (!filter_csv || !*filter_csv)
        return false;
    const char *p = filter_csv;
    while (*p) {
        /* skip leading whitespace */
        while (*p && isspace((unsigned char)*p))
            p++;
        const char *start = p;
        while (*p && *p != ',')
            p++;
        const char *end = p;
        /* trim trailing whitespace */
        while (end > start && isspace((unsigned char)*(end - 1)))
            end--;
        if (end > start) {
            if (match(start, (size_t)(end - start), ctx))
                return true;
        }
        if (*p == ',')
            p++;
    }
    return false;
}

struct match_eq_ctx {
    const char *name;
    size_t name_len;
};

static bool match_eq(const char *token, size_t len, void *ctx_in) {
    struct match_eq_ctx *ctx = (struct match_eq_ctx *)ctx_in;
    return len == ctx->name_len && strncmp(token, ctx->name, len) == 0;
}

bool hu_doctor_ws_event_matches_filter(const char *event_name, const char *filter_csv) {
    if (!event_name)
        return false;
    /* NULL or empty filter = match all */
    if (!filter_csv || !*filter_csv)
        return true;
    struct match_eq_ctx ctx = {.name = event_name, .name_len = strlen(event_name)};
    return filter_csv_any_token(filter_csv, match_eq, &ctx);
}

/* Trim and shorten a JSON payload to a one-line summary for the unknown-
 * event-type fallback. Drops newlines, collapses runs of whitespace,
 * truncates to `max_len` characters with a trailing "..." if cut. Caller
 * owns the returned buffer (via alloc->free with len+1). */
static char *payload_compact_oneline(hu_allocator_t *alloc, const char *payload_json,
                                     size_t max_len) {
    if (!payload_json)
        return hu_strdup(alloc, "");
    size_t src_len = strlen(payload_json);
    size_t cap = src_len + 4; /* room for "..." + NUL */
    char *out = (char *)alloc->alloc(alloc->ctx, cap);
    if (!out)
        return NULL;
    size_t w = 0;
    bool last_was_space = false;
    for (size_t i = 0; i < src_len && w + 4 < cap; i++) {
        char c = payload_json[i];
        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
        if (c == ' ') {
            if (last_was_space)
                continue;
            last_was_space = true;
        } else {
            last_was_space = false;
        }
        if (w >= max_len) {
            out[w++] = '.';
            out[w++] = '.';
            out[w++] = '.';
            break;
        }
        out[w++] = c;
    }
    out[w] = '\0';
    return out;
}

/* For known event types, emit a more useful summary. For unknown types,
 * fall back to the compacted JSON. */
static char *summarize_payload(hu_allocator_t *alloc, const char *event_name,
                               const char *payload_json) {
    /* For now, defer specialized formatters to a follow-up commit and
     * just return a compact-JSON fallback. T1 ships the structure;
     * T3+ will plug in per-event formatters once the parser is in. */
    (void)event_name;
    return payload_compact_oneline(alloc, payload_json, 80);
}

char *hu_doctor_ws_format_event_line(hu_allocator_t *alloc, const char *event_name,
                                     const char *payload_json, uint64_t seq, int64_t now_epoch) {
    if (!alloc || !event_name)
        return NULL;
    time_t t = (now_epoch > 0) ? (time_t)now_epoch : time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);

    char *summary = summarize_payload(alloc, event_name, payload_json);
    if (!summary)
        return NULL;
    size_t summary_len = strlen(summary);

    /* "[HH:MM:SS] <name> seq=<seq> <summary>" — generous buffer. */
    size_t cap = summary_len + strlen(event_name) + 64;
    char *out = (char *)alloc->alloc(alloc->ctx, cap);
    if (!out) {
        alloc->free(alloc->ctx, summary, summary_len + 1);
        return NULL;
    }
    int n = snprintf(out, cap, "[%02d:%02d:%02d] %s seq=%llu %s", lt.tm_hour, lt.tm_min, lt.tm_sec,
                     event_name, (unsigned long long)seq, summary);
    alloc->free(alloc->ctx, summary, summary_len + 1);
    if (n <= 0 || (size_t)n >= cap) {
        alloc->free(alloc->ctx, out, cap);
        return NULL;
    }
    return out;
}

hu_error_t hu_doctor_ws_watch(hu_allocator_t *alloc, const hu_doctor_ws_config_t *cfg) {
    (void)alloc;
    (void)cfg;
    /* T2-T6 — see docs/plans/2026-05-27-doctor-ws-consumer/tasks.md */
    return HU_ERR_NOT_SUPPORTED;
}
