/* Recent-thread block for reactive turns — see include/human/daemon/thread_context.h. */
#include "human/core/log.h"
#include "human/daemon/thread_context.h"
#include "human/providers/local_only.h"
#include <stdio.h>
#include <string.h>

hu_gate_mode_t hu_thread_context_mode(void) {
    return hu_gate_mode_from_env("HU_THREAD_CONTEXT", HU_GATE_OFF);
}

/* ── small string builder over the caller's allocator ─────────────────── */

typedef struct tc_buf {
    hu_allocator_t *alloc;
    char *p;
    size_t len;
    size_t cap;
    bool oom;
} tc_buf_t;

static void tc_put(tc_buf_t *b, const char *s, size_t n) {
    if (b->oom || n == 0)
        return;
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 256;
        while (nc < b->len + n + 1)
            nc *= 2;
        char *np = (char *)b->alloc->realloc(b->alloc->ctx, b->p, b->cap, nc);
        if (!np) {
            b->oom = true;
            return;
        }
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void tc_puts(tc_buf_t *b, const char *s) {
    tc_put(b, s, strlen(s));
}

static void tc_free(tc_buf_t *b) {
    if (b->p)
        b->alloc->free(b->alloc->ctx, b->p, b->cap);
    b->p = NULL;
    b->len = b->cap = 0;
}

/* ── per-message normalisation ────────────────────────────────────────── */

/* The loader's attachment placeholders (imessage_load_conversation_history)
 * collapse to the short forms the model reads naturally. */
static const char *collapse_placeholder(const char *t) {
    if (strcmp(t, "[Photo]") == 0 || strcmp(t, "[image or attachment]") == 0)
        return "[photo]";
    if (strcmp(t, "[Voice Message]") == 0)
        return "[voice memo]";
    if (strcmp(t, "[Video]") == 0)
        return "[video]";
    if (strcmp(t, "[you replied]") == 0)
        return "[attachment]";
    return NULL;
}

/* One line of text: whitespace runs (incl. newlines) collapse to one space,
 * U+FFFC (an inline attachment) becomes "[attachment]", capped at
 * HU_THREAD_CONTEXT_TEXT_CAP bytes on a UTF-8 boundary with "..." appended. */
static size_t normalize_text(const char *in, char *out, size_t out_cap) {
    const char *cl = collapse_placeholder(in);
    if (cl) {
        size_t n = strlen(cl);
        memcpy(out, cl, n + 1);
        return n;
    }
    static const char k_att[] = "[attachment]";
    size_t w = 0;
    bool pending_space = false;
    const size_t cap = out_cap - 4; /* room for "..." + NUL */
    bool truncated = false;
    for (size_t i = 0; in[i] && !truncated;) {
        unsigned char c = (unsigned char)in[i];
        const char *piece = NULL;
        size_t piece_len = 0;
        size_t adv = 1;
        if (c == 0xEF && (unsigned char)in[i + 1] == 0xBF && (unsigned char)in[i + 2] == 0xBC) {
            piece = k_att;
            piece_len = sizeof(k_att) - 1;
            adv = 3;
            pending_space = w > 0;
        } else if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            pending_space = w > 0;
            i++;
            continue;
        } else {
            /* Copy one whole UTF-8 sequence so a cut never splits it. */
            adv = (c < 0x80)         ? 1
                  : (c >> 5) == 0x6  ? 2
                  : (c >> 4) == 0xE  ? 3
                  : (c >> 3) == 0x1E ? 4
                                     : 1;
            for (size_t k = 1; k < adv; k++) {
                if (!in[i + k]) {
                    adv = k;
                    break;
                }
            }
            piece = in + i;
            piece_len = adv;
        }
        size_t need = piece_len + (pending_space ? 1 : 0);
        if (w + need > cap || w + need > HU_THREAD_CONTEXT_TEXT_CAP) {
            truncated = true;
            break;
        }
        if (pending_space) {
            out[w++] = ' ';
            pending_space = false;
        }
        memcpy(out + w, piece, piece_len);
        w += piece_len;
        i += adv;
    }
    if (truncated) {
        memcpy(out + w, "...", 3);
        w += 3;
    }
    if (w == 0) {
        memcpy(out, k_att, sizeof(k_att));
        return sizeof(k_att) - 1;
    }
    out[w] = '\0';
    return w;
}

/* "Mike Smith" -> "Mike"; printable ASCII/UTF-8 only, NULL/empty -> "them". */
static void contact_label(const char *name, size_t name_len, char *out, size_t cap) {
    size_t w = 0;
    for (size_t i = 0; name && i < name_len && w + 1 < cap; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',' || c == '(')
            break;
        if (c < 0x20 || c == ':')
            continue;
        out[w++] = (char)c;
    }
    out[w] = '\0';
    if (w == 0)
        snprintf(out, cap, "them");
}

/* chat.db timestamps arrive as local "YYYY-MM-DD HH:MM:SS". -1 if unparsed. */
static time_t parse_ts(const char *ts) {
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    if (!ts || sscanf(ts, "%d-%d-%d %d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour,
                      &tm.tm_min, &tm.tm_sec) != 6)
        return (time_t)-1;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

static void fmt_span(long secs, char *out, size_t cap) {
    if (secs < 60)
        snprintf(out, cap, "<1m");
    else if (secs < 3600)
        snprintf(out, cap, "%ldm", secs / 60);
    else if (secs < 48L * 3600)
        snprintf(out, cap, "%ldh", secs / 3600);
    else
        snprintf(out, cap, "%ldd", secs / 86400);
}

/* Render entries[start..end) into `b`. */
static void render_range(tc_buf_t *b, const hu_channel_history_entry_t *es, size_t start,
                         size_t end, const char *label, time_t now, hu_thread_context_stats_t *st) {
    char hdr[96];
    snprintf(hdr, sizeof(hdr), "%s (you and %s, oldest first)\n", HU_LOCAL_ONLY_THREAD_BEGIN,
             label);
    tc_puts(b, hdr);
    time_t prev = (time_t)-1;
    char text[HU_THREAD_CONTEXT_TEXT_CAP + 8];
    char span[24];
    char marker[40];
    st->lines = st->seth_lines = 0;
    for (size_t i = start; i < end; i++) {
        time_t at = parse_ts(es[i].timestamp);
        if (at != (time_t)-1) {
            if (i == start) {
                fmt_span(now > at ? (long)(now - at) : 0, span, sizeof(span));
                snprintf(marker, sizeof(marker), "[%s ago]\n", span);
                tc_puts(b, marker);
            } else if (prev != (time_t)-1 && at - prev >= HU_THREAD_CONTEXT_GAP_SECS) {
                fmt_span((long)(at - prev), span, sizeof(span));
                snprintf(marker, sizeof(marker), "[%s later]\n", span);
                tc_puts(b, marker);
            }
            prev = at;
        }
        size_t tl = normalize_text(es[i].text, text, sizeof(text));
        tc_puts(b, es[i].from_me ? "you" : label);
        tc_put(b, ": ", 2);
        tc_put(b, text, tl);
        tc_put(b, "\n", 1);
        st->lines++;
        if (es[i].from_me)
            st->seth_lines++;
    }
    tc_puts(b, HU_LOCAL_ONLY_THREAD_END);
}

/* Is this trailing contact entry the inbound the model already gets? */
static bool is_current_inbound(const hu_channel_history_entry_t *e, const char *cur,
                               size_t cur_len) {
    if (e->from_me || !cur || cur_len == 0 || e->text[0] == '\0')
        return false;
    size_t tl = strlen(e->text);
    if (tl > cur_len)
        return false;
    for (size_t i = 0; i + tl <= cur_len; i++) {
        if (memcmp(cur + i, e->text, tl) == 0)
            return true;
    }
    return false;
}

hu_error_t hu_thread_context_render(hu_allocator_t *alloc,
                                    const hu_channel_history_entry_t *entries, size_t count,
                                    const char *contact_name, size_t contact_name_len,
                                    const char *current, size_t current_len, time_t now,
                                    size_t budget, char **out, size_t *out_len,
                                    hu_thread_context_stats_t *stats) {
    hu_thread_context_stats_t st;
    memset(&st, 0, sizeof(st));
    if (stats)
        *stats = st;
    if (!out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    if (!entries || count == 0)
        return HU_OK;
    if (!alloc)
        return HU_ERR_INVALID_ARGUMENT;

    size_t end = count;
    while (end > 0 && is_current_inbound(&entries[end - 1], current, current_len)) {
        end--;
        st.skipped_current = true;
    }
    if (end == 0) {
        if (stats)
            *stats = st;
        return HU_OK;
    }
    size_t start = end > HU_THREAD_CONTEXT_MAX_LINES ? end - HU_THREAD_CONTEXT_MAX_LINES : 0;

    char label[HU_THREAD_CONTEXT_LABEL_CAP];
    contact_label(contact_name, contact_name_len, label, sizeof(label));

    /* Drop the oldest line until the block fits; the newest line always
     * fits (TEXT_CAP + header + footer < any sane budget). */
    tc_buf_t b = {.alloc = alloc};
    for (;;) {
        b.len = 0;
        render_range(&b, entries, start, end, label, now, &st);
        if (b.oom) {
            tc_free(&b);
            return HU_ERR_OUT_OF_MEMORY;
        }
        if (b.len <= budget || start + 1 >= end)
            break;
        start++;
    }
    st.dropped = end - st.lines;
    st.bytes = b.len;

    /* Exact-size the result so the caller frees (len + 1). */
    char *res = (char *)alloc->alloc(alloc->ctx, b.len + 1);
    if (!res) {
        tc_free(&b);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(res, b.p, b.len + 1);
    *out = res;
    *out_len = b.len;
    tc_free(&b);
    if (stats)
        *stats = st;
    return HU_OK;
}

void hu_daemon_thread_context_apply(hu_allocator_t *alloc, hu_gate_mode_t mode, bool provider_local,
                                    const hu_channel_history_entry_t *entries, size_t count,
                                    const char *contact_name, size_t contact_name_len,
                                    const char *current, size_t current_len, time_t now,
                                    char **convo_ctx, size_t *convo_ctx_len,
                                    hu_thread_context_stats_t *stats) {
    hu_thread_context_stats_t st;
    memset(&st, 0, sizeof(st));
    if (stats)
        *stats = st;
    if (mode == HU_GATE_OFF || !alloc || !convo_ctx || !convo_ctx_len)
        return;
    const char *tag = mode == HU_GATE_LIVE ? "live" : "shadow";
    if (!provider_local) {
        /* The first attempt of this turn leaves the machine: never build it. */
        hu_log_info("thread_context", NULL,
                    "[HU_THREAD_CONTEXT %s] lines=0 bytes=0 seth_lines=0 dropped=0 local=0", tag);
        return;
    }
    char *block = NULL;
    size_t block_len = 0;
    hu_error_t err = hu_thread_context_render(alloc, entries, count, contact_name, contact_name_len,
                                              current, current_len, now, HU_THREAD_CONTEXT_BUDGET,
                                              &block, &block_len, &st);
    if (err != HU_OK) {
        hu_log_info("thread_context", NULL, "[HU_THREAD_CONTEXT %s] render failed err=%d", tag,
                    (int)err);
        return;
    }
    bool applied = false;
    if (mode == HU_GATE_LIVE && block) {
        size_t old_len = *convo_ctx ? *convo_ctx_len : 0;
        size_t sep = old_len > 0 ? 2 : 0;
        size_t total = old_len + sep + block_len;
        char *merged = (char *)alloc->alloc(alloc->ctx, total + 1);
        if (merged) {
            if (old_len > 0) {
                memcpy(merged, *convo_ctx, old_len);
                merged[old_len] = '\n';
                merged[old_len + 1] = '\n';
            }
            memcpy(merged + old_len + sep, block, block_len);
            merged[total] = '\0';
            if (*convo_ctx)
                alloc->free(alloc->ctx, *convo_ctx, *convo_ctx_len + 1);
            *convo_ctx = merged;
            *convo_ctx_len = total;
            applied = true;
        }
    }
    if (block)
        alloc->free(alloc->ctx, block, block_len + 1);
    hu_log_info("thread_context", NULL,
                "[HU_THREAD_CONTEXT %s] lines=%zu bytes=%zu seth_lines=%zu dropped=%zu "
                "skipped_current=%d applied=%d local=1",
                tag, st.lines, st.bytes, st.seth_lines, st.dropped, st.skipped_current ? 1 : 0,
                applied ? 1 : 0);
    if (stats)
        *stats = st;
}
