/* Local, on-device photo understanding — HU_LOCAL_VISION (see local_vision.h).
 *
 * Caption from a loopback VLM, text from the Apple Vision OCR helper, both in
 * parallel under one 8 s budget, composed so the VLM's own quotes never reach
 * the prompt unless the OCR read the same words. Every failure is an error the
 * caller turns into today's "[They sent a photo]". */

#include "human/context/local_vision.h"
#include "human/context/vision.h"
#include "human/core/gate_mode.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/core/log.h"
#include "human/core/paths.h"
#include "human/core/process_util.h"
#include "human/core/string.h"
#include "human/core/time.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LV_CAPTION_CAP 280 /* bytes of caption kept */
#define LV_OCR_CAP     300 /* bytes of OCR text kept */
#define LV_PROMPT                                                               \
    "Describe this photo in one short, plain sentence, the way a friend would " \
    "glance at it. Do not quote, read out or guess any words written in it."

typedef hu_error_t (*lv_caption_fn)(hu_allocator_t *, const char *, const char *, size_t, long,
                                    char **, size_t *);
typedef hu_error_t (*lv_ocr_fn)(hu_allocator_t *, const char *, long, char **, size_t *);

hu_gate_mode_t hu_local_vision_mode(void) {
    return hu_gate_mode_from_env("HU_LOCAL_VISION", HU_GATE_OFF);
}

bool hu_local_vision_url_is_loopback(const char *url) {
    static const char pfx[] = "http://127.0.0.1";
    if (!url || strncmp(url, pfx, sizeof(pfx) - 1) != 0)
        return false;
    const char *p = url + sizeof(pfx) - 1;
    if (*p == ':') {
        size_t d = 0;
        while (p[1 + d] >= '0' && p[1 + d] <= '9')
            d++;
        if (d == 0 || d > 5)
            return false;
        p += 1 + d;
    }
    return *p == '\0' || *p == '/'; /* rejects "127.0.0.1.evil", "127.0.0.1@evil" */
}

/* ── Compose ────────────────────────────────────────────────────────────── */

/* One line, no control characters, brackets as parentheses (and, for OCR,
 * double quotes as single), runs of spaces collapsed, trimmed, cut at a
 * UTF-8 boundary under cap. Returns the length written to dst. */
static size_t lv_flatten(const char *src, size_t n, char *dst, size_t cap, bool ocr) {
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c < 0x20 || c == 0x7f)
            c = ' ';
        else if (c == '[')
            c = '(';
        else if (c == ']')
            c = ')';
        else if (ocr && c == '"')
            c = '\'';
        if (c == ' ' && (o == 0 || dst[o - 1] == ' '))
            continue;
        dst[o++] = (char)c;
    }
    if (o + 1 >= cap) /* truncated: never leave half a UTF-8 sequence */
        while (o > 0 && ((unsigned char)dst[o - 1] & 0xC0) == 0x80)
            o--;
    if (o > 0 && ((unsigned char)dst[o - 1] & 0xC0) == 0xC0)
        o--;
    while (o > 0 && dst[o - 1] == ' ')
        o--;
    dst[o] = '\0';
    return o;
}

/* Lowercase ASCII alnum, everything else one space, trimmed. */
static size_t lv_norm(const char *s, size_t n, char *dst, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < cap; i++) {
        unsigned char c = (unsigned char)s[i];
        bool an = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (an)
            dst[o++] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
        else if (o > 0 && dst[o - 1] != ' ')
            dst[o++] = ' ';
    }
    while (o > 0 && dst[o - 1] == ' ')
        o--;
    dst[o] = '\0';
    return o;
}

/* needle occurs in hay on word boundaries (both already lv_norm'd). */
static bool lv_contains_words(const char *hay, size_t hn, const char *needle, size_t nn) {
    if (nn == 0 || nn > hn)
        return false;
    for (size_t i = 0; i + nn <= hn; i++) {
        if (memcmp(hay + i, needle, nn) != 0)
            continue;
        if ((i == 0 || hay[i - 1] == ' ') && (i + nn == hn || hay[i + nn] == ' '))
            return true;
    }
    return false;
}

/* Words that only introduce a quote ("that says", "with the text"). */
static bool lv_is_lead_in(const char *w, size_t n) {
    static const char *const k[] = {
        "reads",    "read",    "reading", "says",      "say",    "saying",    "said",   "text",
        "texts",    "word",    "words",   "the",       "with",   "that",      "which",  "labeled",
        "labelled", "written", "writing", "inscribed", "phrase", "lettering", "letters"};
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++)
        if (strlen(k[i]) == n && strncasecmp(w, k[i], n) == 0)
            return true;
    return false;
}

static size_t lv_lead_in_start(const char *b, size_t open) {
    size_t s = open;
    for (;;) {
        size_t e = s;
        while (e > 0 && b[e - 1] == ' ')
            e--;
        if (e > 0 && b[e - 1] == ':') {
            s = e - 1;
            continue;
        }
        size_t w = e;
        while (w > 0 && ((b[w - 1] | 32) >= 'a' && (b[w - 1] | 32) <= 'z'))
            w--;
        if (w == e || !lv_is_lead_in(b + w, e - w))
            return s;
        s = w;
    }
}

/* Next opening double quote at or after i: '"' or U+201C. *olen = its bytes. */
static bool lv_find_open(const char *b, size_t n, size_t i, size_t *at, size_t *olen) {
    for (; i < n; i++) {
        if (b[i] == '"') {
            *at = i;
            *olen = 1;
            return true;
        }
        if (i + 3 <= n && memcmp(b + i, "\xE2\x80\x9C", 3) == 0) {
            *at = i;
            *olen = 3;
            return true;
        }
    }
    return false;
}

/* Cut every quoted span the OCR does not contain. Returns the new length. */
static size_t lv_cut_unconfirmed(char *b, size_t n, const char *ocr_norm, size_t ocr_n,
                                 bool *disagree) {
    size_t i = 0, open = 0, olen = 0;
    while (lv_find_open(b, n, i, &open, &olen)) {
        size_t c = open + olen, clen = 0;
        while (c < n) {
            if (b[c] == '"') {
                clen = 1;
                break;
            }
            if (c + 3 <= n && memcmp(b + c, "\xE2\x80\x9D", 3) == 0) {
                clen = 3;
                break;
            }
            c++;
        }
        size_t end = c < n ? c + clen : n; /* an unclosed quote runs to the end */
        char q[LV_CAPTION_CAP + 1];
        size_t qn = lv_norm(b + open + olen, c - open - olen, q, sizeof(q));
        if (qn > 0 && lv_contains_words(ocr_norm, ocr_n, q, qn)) {
            i = end; /* the OCR read it too: keep */
            continue;
        }
        *disagree = true;
        size_t s = lv_lead_in_start(b, open);
        memmove(b + s, b + end, n - end);
        n -= end - s;
        b[n] = '\0';
        i = s;
    }
    /* tidy the seams: double spaces, a space before punctuation, a trailing
     * period or dangling separator */
    size_t o = 0;
    for (size_t k = 0; k < n; k++) {
        char ch = b[k];
        if (ch == ' ' && (o == 0 || b[o - 1] == ' '))
            continue;
        if ((ch == '.' || ch == ',' || ch == ';' || ch == '!' || ch == '?') && o > 0 &&
            b[o - 1] == ' ')
            o--;
        b[o++] = ch;
    }
    while (o > 0 && (b[o - 1] == ' ' || b[o - 1] == '.' || b[o - 1] == ',' || b[o - 1] == ';' ||
                     b[o - 1] == ':'))
        o--;
    b[o] = '\0';
    return o;
}

hu_error_t hu_local_vision_compose(hu_allocator_t *alloc, const char *caption, size_t caption_len,
                                   const char *ocr, size_t ocr_len, char **out, size_t *out_len,
                                   bool *disagree) {
    if (!alloc || !out || !out_len || !disagree)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    *disagree = false;
    char cap[LV_CAPTION_CAP + 1], txt[LV_OCR_CAP + 1], txt_norm[LV_OCR_CAP + 1];
    size_t cn = caption ? lv_flatten(caption, caption_len, cap, sizeof(cap), false) : 0;
    size_t tn = ocr ? lv_flatten(ocr, ocr_len, txt, sizeof(txt), true) : 0;
    cap[cn] = '\0';
    txt[tn] = '\0';
    size_t tnn = lv_norm(txt, tn, txt_norm, sizeof(txt_norm));
    cn = lv_cut_unconfirmed(cap, cn, txt_norm, tnn, disagree);
    if (tnn == 0)
        tn = 0; /* punctuation only is not text */
    if (cn == 0 && tn == 0)
        return HU_ERR_NOT_FOUND;
    char buf[LV_CAPTION_CAP + LV_OCR_CAP + 32];
    int n;
    if (cn > 0 && tn > 0)
        n = snprintf(buf, sizeof(buf), "%s. Text in it: \"%s\"", cap, txt);
    else if (cn > 0)
        n = snprintf(buf, sizeof(buf), "%s", cap);
    else
        n = snprintf(buf, sizeof(buf), "text in it: \"%s\"", txt);
    if (n <= 0 || (size_t)n >= sizeof(buf))
        return HU_ERR_INTERNAL;
    *out = hu_strndup(alloc, buf, (size_t)n);
    if (!*out)
        return HU_ERR_OUT_OF_MEMORY;
    *out_len = (size_t)n;
    return HU_OK;
}

/* ── Stages (real transports; test seams replace them) ─────────────────── */

static hu_error_t lv_caption_http(hu_allocator_t *alloc, const char *url, const char *body,
                                  size_t body_len, long timeout_ms, char **resp, size_t *resp_len) {
#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)alloc, (void)url, (void)body, (void)body_len, (void)timeout_ms, (void)resp,
        (void)resp_len;
    return HU_ERR_NOT_SUPPORTED; /* tests never open a socket */
#else
    long secs = timeout_ms / 1000;
    hu_http_request_opts_t opts = {.timeout_secs = secs < 1 ? 1 : secs, .connect_timeout_secs = 1};
    hu_http_response_t r = {0};
    hu_error_t err = hu_http_post_json_opts(alloc, url, NULL, NULL, body, body_len, &opts, &r);
    if (err == HU_OK && (r.status_code != 200 || !r.body || r.body_len == 0))
        err = HU_ERR_PROVIDER_RESPONSE;
    if (err == HU_OK) {
        *resp = hu_strndup(alloc, r.body, r.body_len);
        *resp_len = r.body_len;
        if (!*resp)
            err = HU_ERR_OUT_OF_MEMORY;
    }
    hu_http_response_free(alloc, &r);
    return err;
#endif
}

hu_error_t hu_local_vision_ocr_exit_error(bool success, int exit_code, long elapsed_ms,
                                          long budget_ms) {
    if (success && exit_code == 0)
        return HU_OK;
    /* exit_code -1 is any signal death. The timeout path SIGKILLs at the
     * budget (whole seconds, floored), so only a death near it is a timeout;
     * an early one is the helper crashing. */
    if (exit_code == -1 && elapsed_ms >= budget_ms - 1000)
        return HU_ERR_TIMEOUT;
    return HU_ERR_IO;
}

static hu_error_t lv_ocr_exec(hu_allocator_t *alloc, const char *path, long timeout_ms, char **json,
                              size_t *json_len) {
#if (defined(HU_IS_TEST) && HU_IS_TEST) || !defined(__APPLE__)
    (void)alloc, (void)path, (void)timeout_ms, (void)json, (void)json_len;
    return HU_ERR_NOT_SUPPORTED; /* tests never spawn; Apple Vision is macOS only */
#else
    char bin[1024];
    const char *env = getenv("HU_LOCAL_VISION_OCR");
    const char *home = getenv("HOME");
    int bn = (env && env[0]) ? snprintf(bin, sizeof(bin), "%s", env)
             : home          ? snprintf(bin, sizeof(bin), "%s/.local/bin/hu-vision-ocr", home)
                             : -1;
    if (bn <= 0 || (size_t)bn >= sizeof(bin) || access(bin, X_OK) != 0)
        return HU_ERR_NOT_FOUND;
    const char *argv[] = {bin, path, NULL};
    long secs = timeout_ms / 1000;
    hu_run_result_t r = {0};
    int64_t t = hu_time_get_current_ms();
    hu_error_t err =
        hu_process_run_with_timeout(alloc, argv, NULL, 65536, (unsigned)(secs < 1 ? 1 : secs), &r);
    if (err == HU_OK)
        err = hu_local_vision_ocr_exit_error(r.success, r.exit_code,
                                             (long)(hu_time_get_current_ms() - t), timeout_ms);
    if (err == HU_OK && !r.stdout_buf)
        err = HU_ERR_IO;
    if (err == HU_OK) {
        *json = hu_strndup(alloc, r.stdout_buf, r.stdout_len);
        *json_len = r.stdout_len;
        if (!*json)
            err = HU_ERR_OUT_OF_MEMORY;
    }
    hu_run_result_free(alloc, &r);
    return err;
#endif
}

#if defined(HU_IS_TEST) && HU_IS_TEST
static hu_local_vision_caption_fn g_caption_hook;
static hu_local_vision_ocr_fn g_ocr_hook;
static char g_last_log[256];

void hu_local_vision_set_test_hooks(hu_local_vision_caption_fn caption,
                                    hu_local_vision_ocr_fn ocr) {
    g_caption_hook = caption;
    g_ocr_hook = ocr;
    g_last_log[0] = '\0';
}

const char *hu_local_vision_test_last_log(void) {
    return g_last_log;
}
#define LV_CAPTION_STAGE (g_caption_hook ? (lv_caption_fn)g_caption_hook : lv_caption_http)
#define LV_OCR_STAGE     (g_ocr_hook ? (lv_ocr_fn)g_ocr_hook : lv_ocr_exec)
#else
#define LV_CAPTION_STAGE lv_caption_http
#define LV_OCR_STAGE     lv_ocr_exec
#endif

/* ── Pipeline ───────────────────────────────────────────────────────────── */

typedef struct lv_ocr_job {
    hu_allocator_t *alloc;
    const char *path;
    long timeout_ms;
    hu_error_t err;
    char *json;
    size_t json_len;
    int64_t ms;
} lv_ocr_job_t;

static void *lv_ocr_thread(void *arg) {
    lv_ocr_job_t *j = (lv_ocr_job_t *)arg;
    int64_t t = hu_time_get_current_ms();
    j->err = LV_OCR_STAGE(j->alloc, j->path, j->timeout_ms, &j->json, &j->json_len);
    j->ms = hu_time_get_current_ms() - t;
    return NULL;
}

typedef struct lv_stats {
    int64_t caption_ms, ocr_ms;
    size_t caption_bytes, ocr_bytes;
    bool disagree;
    char caption[LV_CAPTION_CAP * 2]; /* raw VLM text, for the owner-only shadow sample */
    char ocr[LV_OCR_CAP * 2];
} lv_stats_t;

static hu_error_t lv_post_caption(hu_allocator_t *alloc, const char *path, size_t path_len,
                                  const char *url, long budget_ms, char **resp, size_t *resp_len) {
    char *b64 = NULL, *mime = NULL;
    size_t b64_len = 0, mime_len = 0;
    hu_error_t err = hu_vision_read_image(alloc, path, path_len, &b64, &b64_len, &mime, &mime_len);
    if (err != HU_OK)
        return err;
    const char *model = getenv("HU_LOCAL_VISION_MODEL");
    if (!model || !model[0])
        model = HU_LOCAL_VISION_DEFAULT_MODEL;
    static const char p0[] = ",\"max_tokens\":96,\"temperature\":0,\"stream\":false,"
                             "\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":"
                             "\"image_url\",\"image_url\":{\"url\":\"data:";
    static const char p1[] = "\"}},{\"type\":\"text\",\"text\":";
    hu_json_buf_t jb;
    err = hu_json_buf_init(&jb, alloc);
    bool inited = err == HU_OK;
    if (err == HU_OK)
        err = hu_json_buf_append_raw(&jb, "{", 1);
    if (err == HU_OK)
        err = hu_json_append_key_value(&jb, "model", 5, model, strlen(model));
    if (err == HU_OK)
        err = hu_json_buf_append_raw(&jb, p0, sizeof(p0) - 1);
    if (err == HU_OK)
        err = hu_json_buf_append_raw(&jb, mime, mime_len);
    if (err == HU_OK)
        err = hu_json_buf_append_raw(&jb, ";base64,", 8);
    if (err == HU_OK)
        err = hu_json_buf_append_raw(&jb, b64, b64_len);
    if (err == HU_OK)
        err = hu_json_buf_append_raw(&jb, p1, sizeof(p1) - 1);
    if (err == HU_OK)
        err = hu_json_append_string(&jb, LV_PROMPT, sizeof(LV_PROMPT) - 1);
    if (err == HU_OK)
        err = hu_json_buf_append_raw(&jb, "}]}]}", 5);
    if (err == HU_OK)
        err = budget_ms <= 0
                  ? HU_ERR_TIMEOUT
                  : LV_CAPTION_STAGE(alloc, url, jb.ptr, jb.len, budget_ms, resp, resp_len);
    if (inited)
        hu_json_buf_free(&jb);
    alloc->free(alloc->ctx, b64, b64_len + 1);
    alloc->free(alloc->ctx, mime, mime_len + 1);
    return err;
}

/* choices[0].message.content of an OpenAI chat reply, or NULL. */
static const char *lv_reply_content(const hu_json_value_t *root) {
    const hu_json_value_t *ch = root ? hu_json_object_get(root, "choices") : NULL;
    if (!ch || ch->type != HU_JSON_ARRAY || ch->data.array.len == 0)
        return NULL;
    const hu_json_value_t *msg = hu_json_object_get(ch->data.array.items[0], "message");
    return msg ? hu_json_get_string(msg, "content") : NULL;
}

/* The helper's {"lines":[...]} joined with " / " into dst; returns length. */
static size_t lv_ocr_text(const hu_json_value_t *root, char *dst, size_t cap) {
    const hu_json_value_t *lines = root ? hu_json_object_get(root, "lines") : NULL;
    size_t o = 0;
    dst[0] = '\0';
    if (!lines || lines->type != HU_JSON_ARRAY)
        return 0;
    for (size_t i = 0; i < lines->data.array.len; i++) {
        const hu_json_value_t *l = lines->data.array.items[i];
        if (!l || l->type != HU_JSON_STRING || l->data.string.len == 0)
            continue;
        int n = snprintf(dst + o, cap - o, "%s%.*s", o ? " / " : "", (int)l->data.string.len,
                         l->data.string.ptr);
        if (n < 0 || (size_t)n >= cap - o) {
            o = cap - 1;
            break;
        }
        o += (size_t)n;
    }
    return o;
}

static hu_error_t lv_run(hu_allocator_t *alloc, const char *path, size_t path_len, int64_t t0,
                         lv_stats_t *st, char **out, size_t *out_len) {
    const char *url = getenv("HU_LOCAL_VISION_URL");
    if (!url || !url[0])
        url = HU_LOCAL_VISION_DEFAULT_URL;
    if (!hu_local_vision_url_is_loopback(url))
        return HU_ERR_PERMISSION_DENIED; /* a photo never leaves 127.0.0.1 */
    char pbuf[4096];
    if (path_len >= sizeof(pbuf))
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(pbuf, path, path_len);
    pbuf[path_len] = '\0';

    /* OCR on its own thread while the caption request runs. Heap job: see
     * .claude/rules/asan-pthread-stack-aliasing-darwin.md. */
    lv_ocr_job_t *job = (lv_ocr_job_t *)alloc->alloc(alloc->ctx, sizeof(*job));
    if (!job)
        return HU_ERR_OUT_OF_MEMORY;
    memset(job, 0, sizeof(*job));
    job->alloc = alloc;
    job->path = pbuf;
    job->timeout_ms = HU_LOCAL_VISION_TIMEOUT_MS;
    pthread_t tid;
    bool threaded = pthread_create(&tid, NULL, lv_ocr_thread, job) == 0;

    char *resp = NULL;
    size_t resp_len = 0;
    int64_t tc = hu_time_get_current_ms();
    hu_error_t cerr = lv_post_caption(alloc, pbuf, path_len, url,
                                      HU_LOCAL_VISION_TIMEOUT_MS - (tc - t0), &resp, &resp_len);
    st->caption_ms = hu_time_get_current_ms() - tc;
    if (threaded)
        pthread_join(tid, NULL);
    else
        (void)lv_ocr_thread(job);
    st->ocr_ms = job->ms;

    hu_json_value_t *cj = NULL, *oj = NULL;
    const char *caption = NULL;
    if (cerr == HU_OK && hu_json_parse(alloc, resp, resp_len, &cj) == HU_OK)
        caption = lv_reply_content(cj);
    char ocr[LV_OCR_CAP * 2];
    size_t ocr_n = 0;
    if (job->err == HU_OK && hu_json_parse(alloc, job->json, job->json_len, &oj) == HU_OK)
        ocr_n = lv_ocr_text(oj, ocr, sizeof(ocr));
    st->caption_bytes = caption ? strlen(caption) : 0;
    st->ocr_bytes = ocr_n;
    snprintf(st->caption, sizeof(st->caption), "%s", caption ? caption : "");
    snprintf(st->ocr, sizeof(st->ocr), "%.*s", (int)ocr_n, ocr);

    hu_error_t err = hu_local_vision_compose(alloc, caption, st->caption_bytes, ocr_n ? ocr : NULL,
                                             ocr_n, out, out_len, &st->disagree);
    if (err == HU_ERR_NOT_FOUND) /* nothing usable: report why */
        err = cerr != HU_OK ? cerr : job->err != HU_OK ? job->err : HU_ERR_PROVIDER_RESPONSE;
    if (cj)
        hu_json_free(alloc, cj);
    if (oj)
        hu_json_free(alloc, oj);
    if (resp)
        alloc->free(alloc->ctx, resp, resp_len + 1);
    if (job->json)
        alloc->free(alloc->ctx, job->json, job->json_len + 1);
    alloc->free(alloc->ctx, job, sizeof(*job));
    return err;
}

/* One aggregate line per photo: timings, byte counts, a flag. Never the
 * caption, the OCR text, the path or who sent it. */
static void lv_log(hu_gate_mode_t mode, const lv_stats_t *st, int64_t ms, const char *result) {
    char line[256];
    snprintf(line, sizeof(line),
             "[HU_LOCAL_VISION %s] ms=%lld caption_ms=%lld ocr_ms=%lld caption_bytes=%zu "
             "ocr_bytes=%zu disagree=%d result=%s",
             mode == HU_GATE_LIVE ? "live" : "shadow", (long long)ms, (long long)st->caption_ms,
             (long long)st->ocr_ms, st->caption_bytes, st->ocr_bytes, st->disagree ? 1 : 0, result);
#if defined(HU_IS_TEST) && HU_IS_TEST
    snprintf(g_last_log, sizeof(g_last_log), "%s", line);
#endif
    hu_log_info("local_vision", NULL, "%s", line);
}

/* ── Owner-only shadow samples ──────────────────────────────────────────── */

#define LV_SAMPLE_KEEP     50
#define LV_SAMPLE_READ_CAP (512u * 1024u)
#define LV_SAMPLE_FILE     "local_vision_shadow.jsonl"

/* The promotion read (docs/guides/local-vision.md) needs the captions the log
 * must never carry. They go to <state>/local_vision_shadow.jsonl, mode 0600,
 * the latest LV_SAMPLE_KEEP rows: {ts, path, caption, ocr, description,
 * disagree, latency_ms}. No sender: the daemon never passes one in. Rewritten
 * through a temp file + rename, so a crash leaves the old file whole. */
static void lv_sample_append(hu_allocator_t *a, const char *path, const lv_stats_t *st,
                             const char *desc, size_t dl, int64_t ms) {
#if defined(HU_IS_TEST) && HU_IS_TEST
    const char *sd = getenv("HU_STATE_DIR");
    if (!sd || !sd[0])
        return; /* a test never writes the real ~/.human */
#endif
    char fp[1024], tmp[1100];
    int n = hu_paths_state(fp, sizeof(fp), "%s", LV_SAMPLE_FILE);
    if (n <= 0 || snprintf(tmp, sizeof(tmp), "%s.tmp-%d", fp, (int)getpid()) >= (int)sizeof(tmp))
        return;
    hu_json_buf_t row;
    if (hu_json_buf_init(&row, a) != HU_OK)
        return;
    hu_error_t e = hu_json_buf_append_raw(&row, "{", 1);
    if (!e)
        e = hu_json_append_key_int(&row, "ts", 2, (long long)(hu_time_wall_ms() / 1000));
    if (!e)
        e = hu_json_buf_append_raw(&row, ",", 1);
    if (!e)
        e = hu_json_append_key_value(&row, "path", 4, path, strlen(path));
    if (!e)
        e = hu_json_buf_append_raw(&row, ",", 1);
    if (!e)
        e = hu_json_append_key_value(&row, "caption", 7, st->caption, strlen(st->caption));
    if (!e)
        e = hu_json_buf_append_raw(&row, ",", 1);
    if (!e)
        e = hu_json_append_key_value(&row, "ocr", 3, st->ocr, strlen(st->ocr));
    if (!e)
        e = hu_json_buf_append_raw(&row, ",", 1);
    if (!e)
        e = hu_json_append_key_value(&row, "description", 11, desc, dl);
    if (!e)
        e = hu_json_buf_append_raw(&row, ",", 1);
    if (!e)
        e = hu_json_append_key_bool(&row, "disagree", 8, st->disagree);
    if (!e)
        e = hu_json_buf_append_raw(&row, ",", 1);
    if (!e)
        e = hu_json_append_key_int(&row, "latency_ms", 10, (long long)ms);
    if (!e)
        e = hu_json_buf_append_raw(&row, "}\n", 2);

    char *old = NULL;
    size_t old_n = 0, keep = 0;
    FILE *f = e ? NULL : fopen(fp, "rb");
    if (f) {
        old = (char *)a->alloc(a->ctx, LV_SAMPLE_READ_CAP);
        if (old)
            old_n = fread(old, 1, LV_SAMPLE_READ_CAP, f);
        fclose(f);
    }
    if (old_n > 0) { /* keep the last LV_SAMPLE_KEEP - 1 complete lines */
        size_t lines = 0, k = old_n;
        if (old[old_n - 1] != '\n') /* drop a torn last line */
            while (k > 0 && old[k - 1] != '\n')
                k--;
        old_n = k;
        while (k > 0) {
            size_t j = k - 1;
            while (j > 0 && old[j - 1] != '\n')
                j--;
            if (++lines > LV_SAMPLE_KEEP - 1)
                break;
            keep = old_n - j;
            k = j;
        }
    }
    int fd = e ? -1 : open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    bool ok = fd >= 0 && fchmod(fd, 0600) == 0;
    if (ok && keep > 0)
        ok = write(fd, old + old_n - keep, keep) == (ssize_t)keep;
    if (ok)
        ok = write(fd, row.ptr, row.len) == (ssize_t)row.len;
    if (fd >= 0)
        close(fd);
    if (ok)
        ok = rename(tmp, fp) == 0;
    if (!ok && fd >= 0)
        unlink(tmp);
    if (!ok && !e)
        hu_log_warn("local_vision", NULL, "[HU_LOCAL_VISION shadow] sample store not written");
    if (old)
        a->free(a->ctx, old, LV_SAMPLE_READ_CAP);
    hu_json_buf_free(&row);
}

/* ── Detached shadow job ────────────────────────────────────────────────── */

/* SHADOW never holds the reply (#617 review): the pipeline runs on its own
 * thread with a private copy of the path, at most one at a time; a photo that
 * arrives while one is running is logged result=busy and skipped. */
static atomic_bool g_shadow_busy;
#if defined(HU_IS_TEST) && HU_IS_TEST
static pthread_t g_shadow_tid;
static bool g_shadow_joinable;

void hu_local_vision_test_wait_shadow(void) {
    if (g_shadow_joinable) {
        pthread_join(g_shadow_tid, NULL);
        g_shadow_joinable = false;
    }
}
#endif

typedef struct lv_shadow_job {
    char *path;
    size_t path_len;
} lv_shadow_job_t;

static void *lv_shadow_thread(void *arg) {
    lv_shadow_job_t *job = (lv_shadow_job_t *)arg;
    hu_allocator_t a = hu_system_allocator();
    lv_stats_t *st = (lv_stats_t *)a.alloc(a.ctx, sizeof(*st));
    if (st) {
        memset(st, 0, sizeof(*st));
        int64_t t0 = hu_time_get_current_ms();
        char *desc = NULL;
        size_t dl = 0;
        hu_error_t err = lv_run(&a, job->path, job->path_len, t0, st, &desc, &dl);
        int64_t ms = hu_time_get_current_ms() - t0;
        lv_log(HU_GATE_SHADOW, st, ms, err == HU_OK ? "ok" : hu_error_string(err));
        if (err == HU_OK && desc)
            lv_sample_append(&a, job->path, st, desc, dl, ms);
        if (desc)
            a.free(a.ctx, desc, dl + 1);
        a.free(a.ctx, st, sizeof(*st));
    }
    a.free(a.ctx, job->path, job->path_len + 1);
    a.free(a.ctx, job, sizeof(*job));
    atomic_store(&g_shadow_busy, false);
    return NULL;
}

static void lv_shadow_spawn(const char *path, size_t path_len) {
    if (atomic_exchange(&g_shadow_busy, true)) {
        lv_stats_t zero = {0};
        lv_log(HU_GATE_SHADOW, &zero, 0, "busy");
        return;
    }
#if defined(HU_IS_TEST) && HU_IS_TEST
    hu_local_vision_test_wait_shadow(); /* finished but not yet joined */
#endif
    hu_allocator_t a = hu_system_allocator();
    lv_shadow_job_t *job = (lv_shadow_job_t *)a.alloc(a.ctx, sizeof(*job));
    char *p = hu_strndup(&a, path, path_len);
    pthread_attr_t attr;
    bool attr_ok = pthread_attr_init(&attr) == 0;
#if !(defined(HU_IS_TEST) && HU_IS_TEST)
    if (attr_ok)
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
#endif
    pthread_t tid;
    bool started = false;
    if (job && p && attr_ok) {
        job->path = p;
        job->path_len = path_len;
        started = pthread_create(&tid, &attr, lv_shadow_thread, job) == 0;
    }
    if (attr_ok)
        pthread_attr_destroy(&attr);
    if (started) {
#if defined(HU_IS_TEST) && HU_IS_TEST
        g_shadow_tid = tid;
        g_shadow_joinable = true;
#endif
        return;
    }
    if (p)
        a.free(a.ctx, p, path_len + 1);
    if (job)
        a.free(a.ctx, job, sizeof(*job));
    atomic_store(&g_shadow_busy, false);
    lv_stats_t zero = {0};
    lv_log(HU_GATE_SHADOW, &zero, 0, "spawn_failed");
}

hu_error_t hu_local_vision_describe(hu_allocator_t *alloc, const char *path, size_t path_len,
                                    char **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    hu_gate_mode_t mode = hu_local_vision_mode();
    if (mode == HU_GATE_OFF)
        return HU_ERR_NOT_SUPPORTED;
    if (!path || path_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    /* HU_LOCAL_VISION activation gated on the 7-day shadow + owner read in
     * docs/guides/local-vision.md: do not default it on without that. */
    if (mode == HU_GATE_SHADOW) {
        lv_shadow_spawn(path, path_len); /* returns at once; the caller keeps today's note */
        return HU_ERR_NOT_SUPPORTED;
    }
    lv_stats_t *st = (lv_stats_t *)alloc->alloc(alloc->ctx, sizeof(*st));
    if (!st)
        return HU_ERR_OUT_OF_MEMORY;
    memset(st, 0, sizeof(*st));
    int64_t t0 = hu_time_get_current_ms();
    char *desc = NULL;
    size_t dl = 0;
    hu_error_t err = lv_run(alloc, path, path_len, t0, st, &desc, &dl);
    lv_log(mode, st, hu_time_get_current_ms() - t0, err == HU_OK ? "ok" : hu_error_string(err));
    alloc->free(alloc->ctx, st, sizeof(*st));
    if (err != HU_OK) {
        if (desc)
            alloc->free(alloc->ctx, desc, dl + 1);
        return err;
    }
    *out = desc;
    *out_len = dl;
    return HU_OK;
}
