/* tests/turn_recording_provider.c — see turn_recording_provider.h. */
#include "turn_recording_provider.h"
#include "human/core/allocator.h"
#include "human/core/string.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void trp_reserve(trp_t *t, size_t extra) {
    if (t->oom)
        return;
    if (t->log && t->log_len + extra + 1 <= t->log_cap)
        return;
    size_t cap = t->log_cap ? t->log_cap : 4096;
    while (cap < t->log_len + extra + 1)
        cap *= 2;
    char *n = (char *)realloc(t->log, cap);
    if (!n) {
        t->oom = true;
        return;
    }
    if (!t->log)
        n[0] = '\0';
    t->log = n;
    t->log_cap = cap;
}

void trp_log_raw(trp_t *t, const char *s, size_t n) {
    if (!t || (!s && n > 0))
        return;
    trp_reserve(t, n);
    if (t->oom)
        return;
    if (n > 0)
        memcpy(t->log + t->log_len, s, n);
    t->log_len += n;
    t->log[t->log_len] = '\0';
}

void trp_log_fmt(trp_t *t, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        t->oom = true; /* every format in this harness is short by construction */
        return;
    }
    trp_log_raw(t, buf, (size_t)n);
}

void trp_log_escaped(trp_t *t, const char *s, size_t n) {
    if (!s) {
        trp_log_raw(t, "(null)", 6);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\')
            trp_log_raw(t, "\\\\", 2);
        else if (c == '\n')
            trp_log_raw(t, "\\n", 2);
        else if (c == '\r')
            trp_log_raw(t, "\\r", 2);
        else if (c == '\t')
            trp_log_raw(t, "\\t", 2);
        else if (c < 0x20 || c == 0x7f) {
            char e[5];
            (void)snprintf(e, sizeof(e), "\\x%02x", c);
            trp_log_raw(t, e, 4);
        } else {
            trp_log_raw(t, &s[i], 1);
        }
    }
}

static void trp_log_opt(trp_t *t, const char *label, const char *s, size_t n) {
    trp_log_fmt(t, " %s=", label);
    if (s)
        trp_log_escaped(t, s, n);
    else
        trp_log_raw(t, "-", 1);
}

static const char *trp_role(hu_role_t r) {
    switch (r) {
    case HU_ROLE_SYSTEM:
        return "system";
    case HU_ROLE_USER:
        return "user";
    case HU_ROLE_ASSISTANT:
        return "assistant";
    case HU_ROLE_TOOL:
        return "tool";
    }
    return "unknown";
}

static void trp_log_request(trp_t *t, const hu_chat_request_t *req, const char *model,
                            size_t model_len, double temperature) {
    trp_log_fmt(t, "=== chat #%zu\nmodel=", t->calls);
    trp_log_escaped(t, model ? model : "", model ? model_len : 0);
    trp_log_fmt(t, " temperature=%.3f\n", temperature);
    if (!req) {
        trp_log_raw(t, "request=NULL\n", 13);
        return;
    }
    trp_log_raw(t, "req.model=", 10);
    trp_log_escaped(t, req->model ? req->model : "", req->model ? req->model_len : 0);
    trp_log_fmt(t,
                " req.temperature=%.3f max_tokens=%u timeout_secs=%llu thinking_budget=%d"
                " logprobs=%d stream_strip=%d budget_usd=%.4f\n",
                req->temperature, (unsigned)req->max_tokens, (unsigned long long)req->timeout_secs,
                req->thinking_budget, req->include_completion_logprobs ? 1 : 0, req->stream_strip,
                req->budget_remaining_usd);
    trp_log_raw(t, "opts:", 5);
    trp_log_opt(t, "reasoning_effort", req->reasoning_effort, req->reasoning_effort_len);
    trp_log_opt(t, "response_format", req->response_format, req->response_format_len);
    trp_log_opt(t, "response_schema", req->response_schema, req->response_schema_len);
    trp_log_opt(t, "prompt_cache_id", req->prompt_cache_id, req->prompt_cache_id_len);
    trp_log_fmt(t, "\nsteering=%d formality=%.3f verbosity=%.3f warmth=%.3f humor=%.3f\n",
                req->steering_present ? 1 : 0, req->steer_formality, req->steer_verbosity,
                req->steer_warmth, req->steer_humor);
    trp_log_fmt(t, "stop_sequences=%zu\n", req->stop_sequences ? req->stop_sequences_count : 0);
    for (size_t i = 0; req->stop_sequences && i < req->stop_sequences_count; i++) {
        const char *ss = req->stop_sequences[i];
        trp_log_raw(t, "  stop=", 7);
        trp_log_escaped(t, ss, ss ? strlen(ss) : 0);
        trp_log_raw(t, "\n", 1);
    }
    trp_log_fmt(t, "tools=%zu\n", req->tools ? req->tools_count : 0);
    for (size_t i = 0; req->tools && i < req->tools_count; i++) {
        const hu_tool_spec_t *ts = &req->tools[i];
        trp_log_fmt(t, "tool[%zu] name=", i);
        trp_log_escaped(t, ts->name, ts->name ? ts->name_len : 0);
        trp_log_raw(t, "\n  desc=", 8);
        trp_log_escaped(t, ts->description, ts->description ? ts->description_len : 0);
        trp_log_raw(t, "\n  params=", 10);
        trp_log_escaped(t, ts->parameters_json, ts->parameters_json ? ts->parameters_json_len : 0);
        trp_log_raw(t, "\n", 1);
    }
    trp_log_fmt(t, "messages=%zu\n", req->messages ? req->messages_count : 0);
    for (size_t i = 0; req->messages && i < req->messages_count; i++) {
        const hu_chat_message_t *m = &req->messages[i];
        trp_log_fmt(t, "msg[%zu] role=%s", i, trp_role(m->role));
        trp_log_opt(t, "name", m->name, m->name_len);
        trp_log_opt(t, "tool_call_id", m->tool_call_id, m->tool_call_id_len);
        trp_log_fmt(t, " parts=%zu tool_calls=%zu\n  content=",
                    m->content_parts ? m->content_parts_count : 0,
                    m->tool_calls ? m->tool_calls_count : 0);
        trp_log_escaped(t, m->content ? m->content : "", m->content ? m->content_len : 0);
        trp_log_raw(t, "\n", 1);
        for (size_t j = 0; m->content_parts && j < m->content_parts_count; j++) {
            const hu_content_part_t *cp = &m->content_parts[j];
            trp_log_fmt(t, "  part[%zu] tag=%d", j, (int)cp->tag);
            if (cp->tag == HU_CONTENT_PART_TEXT) {
                trp_log_raw(t, " text=", 6);
                trp_log_escaped(t, cp->data.text.ptr, cp->data.text.ptr ? cp->data.text.len : 0);
            }
            trp_log_raw(t, "\n", 1);
        }
        for (size_t j = 0; m->tool_calls && j < m->tool_calls_count; j++) {
            const hu_tool_call_t *tc = &m->tool_calls[j];
            trp_log_fmt(t, "  tool_call[%zu] id=", j);
            trp_log_escaped(t, tc->id, tc->id ? tc->id_len : 0);
            trp_log_raw(t, " name=", 6);
            trp_log_escaped(t, tc->name, tc->name ? tc->name_len : 0);
            trp_log_raw(t, " args=", 6);
            trp_log_escaped(t, tc->arguments, tc->arguments ? tc->arguments_len : 0);
            trp_log_raw(t, "\n", 1);
        }
    }
}

static hu_error_t trp_fill(hu_allocator_t *alloc, const char *content, const trp_tool_call_t *calls,
                           size_t n, hu_chat_response_t *out) {
    if (content) {
        size_t len = strlen(content);
        char *c = hu_strndup(alloc, content, len);
        if (!c)
            return HU_ERR_OUT_OF_MEMORY;
        out->content = c;
        out->content_len = len;
    }
    if (n > 0) {
        hu_tool_call_t *tcs =
            (hu_tool_call_t *)alloc->alloc(alloc->ctx, n * sizeof(hu_tool_call_t));
        if (!tcs) {
            hu_chat_response_free(alloc, out);
            return HU_ERR_OUT_OF_MEMORY;
        }
        memset(tcs, 0, n * sizeof(hu_tool_call_t));
        out->tool_calls = tcs;
        out->tool_calls_count = n;
        for (size_t i = 0; i < n; i++) {
            tcs[i].id = hu_strndup(alloc, calls[i].id, strlen(calls[i].id));
            tcs[i].id_len = strlen(calls[i].id);
            tcs[i].name = hu_strndup(alloc, calls[i].name, strlen(calls[i].name));
            tcs[i].name_len = strlen(calls[i].name);
            tcs[i].arguments = hu_strndup(alloc, calls[i].arguments, strlen(calls[i].arguments));
            tcs[i].arguments_len = strlen(calls[i].arguments);
            if (!tcs[i].id || !tcs[i].name || !tcs[i].arguments) {
                hu_chat_response_free(alloc, out);
                return HU_ERR_OUT_OF_MEMORY;
            }
        }
    }
    out->usage.prompt_tokens = 10;
    out->usage.completion_tokens = 5;
    out->usage.total_tokens = 15;
    return HU_OK;
}

static hu_error_t trp_chat(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *request,
                           const char *model, size_t model_len, double temperature,
                           hu_chat_response_t *out) {
    trp_t *t = (trp_t *)ctx;
    memset(out, 0, sizeof(*out));
    t->calls++;
    trp_log_request(t, request, model, model_len, temperature);
    if (t->next >= t->script_count) {
        trp_log_raw(t, "reply=off-script\n", 17);
        if (!t->off_script_content)
            return HU_ERR_INVALID_ARGUMENT;
        return trp_fill(alloc, t->off_script_content, NULL, 0, out);
    }
    const trp_step_t *st = &t->script[t->next++];
    trp_log_fmt(t, "reply=step%zu err=%d\n", t->next - 1, (int)st->err);
    if (st->err != HU_OK)
        return st->err;
    return trp_fill(alloc, st->content, st->tool_calls, st->tool_calls_count, out);
}

static hu_error_t trp_chat_with_system(void *ctx, hu_allocator_t *alloc, const char *system_prompt,
                                       size_t system_prompt_len, const char *message,
                                       size_t message_len, const char *model, size_t model_len,
                                       double temperature, char **out, size_t *out_len) {
    trp_t *t = (trp_t *)ctx;
    t->calls++;
    trp_log_fmt(t, "=== chat_with_system #%zu\nmodel=", t->calls);
    trp_log_escaped(t, model ? model : "", model ? model_len : 0);
    trp_log_fmt(t, " temperature=%.3f\nsystem=", temperature);
    trp_log_escaped(t, system_prompt, system_prompt ? system_prompt_len : 0);
    trp_log_raw(t, "\nmessage=", 9);
    trp_log_escaped(t, message, message ? message_len : 0);
    trp_log_raw(t, "\n", 1);
    *out = hu_strndup(alloc, "ok", 2);
    if (!*out)
        return HU_ERR_OUT_OF_MEMORY;
    *out_len = 2;
    return HU_OK;
}

static bool trp_supports_native_tools(void *ctx) {
    (void)ctx;
    return true;
}

static const char *trp_get_name(void *ctx) {
    (void)ctx;
    return "trp";
}

static void trp_deinit_ctx(void *ctx, hu_allocator_t *alloc) {
    (void)ctx;
    (void)alloc;
}

static const hu_provider_vtable_t trp_vtable = {
    .chat_with_system = trp_chat_with_system,
    .chat = trp_chat,
    .supports_native_tools = trp_supports_native_tools,
    .get_name = trp_get_name,
    .deinit = trp_deinit_ctx,
};

void trp_init(trp_t *t, const trp_step_t *script, size_t script_count,
              const char *off_script_content) {
    memset(t, 0, sizeof(*t));
    t->script = script;
    t->script_count = script ? script_count : 0;
    t->off_script_content = off_script_content;
    trp_log_raw(t, "", 0); /* allocate so log is never NULL after init */
}

void trp_deinit(trp_t *t) {
    if (!t)
        return;
    free(t->log);
    memset(t, 0, sizeof(*t));
}

hu_provider_t trp_provider(trp_t *t) {
    hu_provider_t p = {.ctx = t, .vtable = &trp_vtable};
    return p;
}

/* ── scrubber ─────────────────────────────────────────────────────────── */

static const char *const k_trp_dow[] = {
    "monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday", "mon", "tue",
    "tues",   "wed",     "thu",       "thur",     "thurs",  "fri",      "sat",    "sun"};
static const char *const k_trp_mon[] = {
    "january",   "february", "march",    "april",    "may",  "june", "july", "august",
    "september", "october",  "november", "december", "jan",  "feb",  "mar",  "apr",
    "jun",       "jul",      "aug",      "sep",      "sept", "oct",  "nov",  "dec"};
static const char *const k_trp_tod[] = {"morning", "afternoon", "evening", "night",
                                        "tonight", "midnight",  "noon"};

static bool trp_alnum(char c) {
    return isalnum((unsigned char)c) != 0;
}

static size_t trp_digits(const char *s, size_t i, size_t n) {
    size_t k = i;
    while (k < n && s[k] >= '0' && s[k] <= '9')
        k++;
    return k - i;
}

static bool trp_word_in(const char *w, size_t wl, const char *const *list, size_t count) {
    for (size_t i = 0; i < count; i++) {
        size_t l = strlen(list[i]);
        if (l == wl && strncasecmp(w, list[i], l) == 0)
            return true;
    }
    return false;
}

/* Length of a clock time starting at s[i] ("2:05", "14:05:00", optional " pm"/"a.m."), 0 if none.
 */
static size_t trp_clock(const char *s, size_t i, size_t n) {
    size_t d = trp_digits(s, i, n);
    if (d < 1 || d > 2 || i + d >= n || s[i + d] != ':' || trp_digits(s, i + d + 1, n) != 2)
        return 0;
    size_t j = i + d + 3;
    if (j < n && s[j] == ':' && trp_digits(s, j + 1, n) == 2)
        j += 3;
    size_t k = j;
    if (k < n && s[k] == ' ')
        k++;
    if (k + 1 < n && (s[k] == 'a' || s[k] == 'A' || s[k] == 'p' || s[k] == 'P')) {
        if ((s[k + 1] == 'm' || s[k + 1] == 'M') && (k + 2 >= n || !trp_alnum(s[k + 2])))
            j = k + 2;
        else if (k + 3 < n && s[k + 1] == '.' && (s[k + 2] == 'm' || s[k + 2] == 'M') &&
                 s[k + 3] == '.')
            j = k + 4;
    }
    return j - i;
}

char *trp_scrub(const char *in, size_t n, size_t *out_len) {
    trp_t b;
    trp_init(&b, NULL, 0, NULL);
    size_t i = 0;
    bool boundary = true; /* the previous byte ended a token */
    while (i < n && !b.oom) {
        char c = in[i];
        bool left_ok = boundary || i == 0 || !trp_alnum(in[i - 1]);
        boundary = false;
        if (c == '\\' && i + 1 < n) { /* escape sequence written by trp_log_escaped */
            size_t el = (in[i + 1] == 'x') ? 4 : 2;
            if (i + el > n)
                el = n - i;
            trp_log_raw(&b, in + i, el);
            i += el;
            boundary = true;
            continue;
        }
        if (left_ok && c >= '0' && c <= '9') {
            size_t d = trp_digits(in, i, n);
            /* YYYY-MM-DD[(T| )HH:MM[:SS][.fff][Z]] */
            if (d == 4 && i + 10 <= n && in[i + 4] == '-' && trp_digits(in, i + 5, n) == 2 &&
                in[i + 7] == '-' && trp_digits(in, i + 8, n) == 2) {
                size_t j = i + 10;
                if (j + 6 <= n && (in[j] == 'T' || in[j] == ' ') && trp_digits(in, j + 1, n) == 2 &&
                    in[j + 3] == ':' && trp_digits(in, j + 4, n) == 2) {
                    j += 6;
                    if (j + 3 <= n && in[j] == ':' && trp_digits(in, j + 1, n) == 2)
                        j += 3;
                    if (j < n && in[j] == '.') {
                        j++;
                        j += trp_digits(in, j, n);
                    }
                    if (j < n && in[j] == 'Z')
                        j++;
                }
                trp_log_raw(&b, "<DATE>", 6);
                i = j;
                continue;
            }
            /* D{1,2}/D{1,2}[/YY|/YYYY] */
            if (d <= 2 && i + d < n && in[i + d] == '/') {
                size_t d2 = trp_digits(in, i + d + 1, n);
                if (d2 >= 1 && d2 <= 2) {
                    size_t j = i + d + 1 + d2;
                    if (j < n && in[j] == '/') {
                        size_t d3 = trp_digits(in, j + 1, n);
                        if (d3 == 2 || d3 == 4)
                            j += 1 + d3;
                    }
                    if (j >= n || !trp_alnum(in[j])) {
                        trp_log_raw(&b, "<DATE>", 6);
                        i = j;
                        continue;
                    }
                }
            }
            size_t ck = trp_clock(in, i, n);
            if (ck > 0) {
                trp_log_raw(&b, "<TIME>", 6);
                i += ck;
                continue;
            }
            /* ordinal day: 1st 2nd 3rd 30th */
            if (d <= 2 && i + d + 2 <= n &&
                (strncasecmp(in + i + d, "st", 2) == 0 || strncasecmp(in + i + d, "nd", 2) == 0 ||
                 strncasecmp(in + i + d, "rd", 2) == 0 || strncasecmp(in + i + d, "th", 2) == 0) &&
                (i + d + 2 >= n || !trp_alnum(in[i + d + 2]))) {
                trp_log_raw(&b, "<DOM>", 5);
                i += d + 2;
                continue;
            }
            bool right_ok = i + d >= n || !trp_alnum(in[i + d]);
            if (right_ok && d >= 9 && d <= 13) {
                trp_log_raw(&b, "<EPOCH>", 7);
                i += d;
                continue;
            }
            trp_log_raw(&b, in + i, d);
            i += d;
            continue;
        }
        if (left_ok && isalpha((unsigned char)c)) {
            size_t j = i;
            while (j < n && isalpha((unsigned char)in[j]))
                j++;
            size_t wl = j - i;
            bool right_ok = j >= n || !isdigit((unsigned char)in[j]);
            if (right_ok &&
                trp_word_in(in + i, wl, k_trp_dow, sizeof(k_trp_dow) / sizeof(k_trp_dow[0]))) {
                trp_log_raw(&b, "<DOW>", 5);
                i = j;
                continue;
            }
            if (right_ok &&
                trp_word_in(in + i, wl, k_trp_mon, sizeof(k_trp_mon) / sizeof(k_trp_mon[0]))) {
                trp_log_raw(&b, "<MON>", 5);
                i = j;
                if (i + 1 < n && in[i] == ' ') { /* "September 30", "Sept 30, 2026" */
                    size_t dd = trp_digits(in, i + 1, n);
                    if (dd >= 1 && dd <= 2 && (i + 1 + dd >= n || !trp_alnum(in[i + 1 + dd]))) {
                        trp_log_raw(&b, " <DOM>", 6);
                        i += 1 + dd;
                        if (i + 6 <= n && in[i] == ',' && in[i + 1] == ' ' &&
                            trp_digits(in, i + 2, n) == 4) {
                            trp_log_raw(&b, ", <YEAR>", 8);
                            i += 6;
                        }
                    }
                }
                continue;
            }
            if (right_ok &&
                trp_word_in(in + i, wl, k_trp_tod, sizeof(k_trp_tod) / sizeof(k_trp_tod[0]))) {
                trp_log_raw(&b, "<TOD>", 5);
                i = j;
                continue;
            }
            trp_log_raw(&b, in + i, wl);
            i = j;
            continue;
        }
        trp_log_raw(&b, &in[i], 1);
        i++;
    }
    if (b.oom) {
        trp_deinit(&b);
        return NULL;
    }
    char *s = b.log;
    if (out_len)
        *out_len = b.log_len;
    return s; /* ownership moves to the caller; b is not deinit'd */
}
