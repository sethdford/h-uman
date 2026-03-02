/*
 * Web fetch tool — HTTP GET + HTML-to-text extraction.
 * Strips script/style tags, converts to readable text with basic markdown.
 */
#include "seaclaw/tool.h"
#include <stdint.h>
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/tools/validation.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define SC_WEB_FETCH_NAME "web_fetch"
#define SC_WEB_FETCH_DESC "Fetch a web page by URL and extract its text content. Converts HTML to readable text. Use this to read the content of a specific URL. Preferred over browser for retrieving page content."
#define SC_WEB_FETCH_PARAMS "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\"},\"max_chars\":{\"type\":\"integer\",\"default\":50000}},\"required\":[\"url\"]}"
#define SC_WEB_FETCH_DEFAULT_MAX 50000
#define SC_WEB_FETCH_MIN_MAX 100
#define SC_WEB_FETCH_MAX_MAX 200000

typedef struct sc_web_fetch_ctx {
    uint32_t max_chars;
} sc_web_fetch_ctx_t;

#if !SC_IS_TEST
static int tag_eq(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    }
    return 1;
}
#endif

#if !SC_IS_TEST
/* Simple HTML-to-text: strip script/style, decode entities */
static char *html_to_text(sc_allocator_t *alloc, const char *html, size_t html_len, size_t *out_len)
{
    size_t cap = html_len + 256;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf) return NULL;
    size_t len = 0;
    int in_script = 0, in_style = 0;
    int last_was_space = 1;

    for (size_t i = 0; i < html_len; i++) {
        if (html[i] == '<') {
            size_t end = i + 1;
            while (end < html_len && html[end] != '>') end++;
            if (end >= html_len) { if (len+1>=cap) { size_t nc=cap*2; char*nb=(char*)alloc->realloc(alloc->ctx,buf,cap,nc); if(!nb) break; buf=nb; cap=nc; } buf[len++]=html[i]; continue; }

            size_t tag_len = end - i - 1;
            const char *tag = html + i + 1;
            int closing = (tag_len > 0 && tag[0] == '/');
            const char *tagname = closing ? tag + 1 : tag;
            size_t tlen = 0;
            while (tlen < tag_len - (closing ? 1 : 0) && tagname[tlen] != ' ' && tagname[tlen] != '/' && tagname[tlen] != '>')
                tlen++;

            if (closing) {
                if (tlen >= 6 && tag_eq(tagname, "script", 6)) in_script = 0;
                if (tlen >= 5 && tag_eq(tagname, "style", 5)) in_style = 0;
            } else {
                if (tlen >= 6 && tag_eq(tagname, "script", 6)) in_script = 1;
                if (tlen >= 5 && tag_eq(tagname, "style", 5)) in_style = 1;
            }
            if (in_script || in_style) { i = end; continue; }

            if (tlen == 2 && tag_eq(tagname, "br", 2)) {
                if (len+1>=cap) { size_t nc=cap*2; char*nb=(char*)alloc->realloc(alloc->ctx,buf,cap,nc); if(!nb) break; buf=nb; cap=nc; }
                buf[len++]='\n'; last_was_space=1; i=end; continue;
            }
            if ((tlen==1&&tagname[0]=='p')||(tlen==3&&tag_eq(tagname,"div",3))||
                (tlen==2&&(tag_eq(tagname,"h1",2)||tag_eq(tagname,"h2",2)))) {
                if (!last_was_space && len > 0) { if (len+1>=cap) { size_t nc=cap*2; char*nb=(char*)alloc->realloc(alloc->ctx,buf,cap,nc); if(!nb) break; buf=nb; cap=nc; } buf[len++]='\n'; last_was_space=1; }
            }
            i = end;
            continue;
        }
        if (in_script || in_style) continue;

        if (html[i] == '&') {
            size_t semi = i + 1;
            while (semi < html_len && html[semi] != ';') semi++;
            if (semi < html_len) {
                int ch = -1;
                size_t elen = semi - i + 1;
                if (elen==5 && memcmp(html+i,"&amp;",5)==0) ch='&';
                if (elen==4 && memcmp(html+i,"&lt;",4)==0) ch='<';
                if (elen==4 && memcmp(html+i,"&gt;",4)==0) ch='>';
                if (elen==6 && memcmp(html+i,"&quot;",6)==0) ch='\"';
                if (elen==6 && memcmp(html+i,"&nbsp;",6)==0) ch=' ';
                if (ch >= 0) {
                    if (len+1>=cap) { size_t nc=cap*2; char*nb=(char*)alloc->realloc(alloc->ctx,buf,cap,nc); if(!nb) break; buf=nb; cap=nc; }
                    buf[len++]=(char)ch; last_was_space=(ch==' '); i=semi; continue;
                }
            }
        }

        char ch = html[i];
        if (ch == '\n' || ch == '\r') {
            if (!last_was_space && len > 0) { if (len+1>=cap) { size_t nc=cap*2; char*nb=(char*)alloc->realloc(alloc->ctx,buf,cap,nc); if(!nb) break; buf=nb; cap=nc; } buf[len++]=' '; last_was_space=1; }
        } else if (ch == ' ' || ch == '\t') {
            if (!last_was_space && len > 0) { if (len+1>=cap) { size_t nc=cap*2; char*nb=(char*)alloc->realloc(alloc->ctx,buf,cap,nc); if(!nb) break; buf=nb; cap=nc; } buf[len++]=' '; last_was_space=1; }
        } else {
            if (len+1>=cap) { size_t nc=cap*2; char*nb=(char*)alloc->realloc(alloc->ctx,buf,cap,nc); if(!nb) break; buf=nb; cap=nc; }
            buf[len++]=ch; last_was_space=0;
        }
    }
    while (len > 0 && (buf[len-1]==' ' || buf[len-1]=='\n')) len--;
    buf[len]='\0';
    *out_len=len;
    return buf;
}
#endif

static sc_error_t web_fetch_execute(void *ctx, sc_allocator_t *alloc,
    const sc_json_value_t *args,
    sc_tool_result_t *out)
{
    sc_web_fetch_ctx_t *c = (sc_web_fetch_ctx_t *)ctx;
    if (!c || !args || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_ERR_INVALID_ARGUMENT;
    }
    const char *url = sc_json_get_string(args, "url");
    if (!url || strlen(url) == 0) {
        *out = sc_tool_result_fail("Missing required 'url' parameter", 30);
        return SC_OK;
    }
    if (sc_tool_validate_url(url) != SC_OK) {
        *out = sc_tool_result_fail("invalid url: HTTPS only, no private IPs", 37);
        return SC_OK;
    }

    uint32_t max_chars = c->max_chars;
    double mc = sc_json_get_number(args, "max_chars", max_chars);
    if (mc < SC_WEB_FETCH_MIN_MAX) mc = SC_WEB_FETCH_MIN_MAX;
    if (mc > SC_WEB_FETCH_MAX_MAX) mc = SC_WEB_FETCH_MAX_MAX;
    max_chars = (uint32_t)mc;

#if SC_IS_TEST
    char *msg = sc_strndup(alloc, "(web_fetch stub in test)", 24);
    if (!msg) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
    *out = sc_tool_result_ok_owned(msg, 24);
    return SC_OK;
#else
    sc_http_response_t resp = {0};
    sc_error_t err = sc_http_get(alloc, url, NULL, &resp);
    if (err != SC_OK) {
        *out = sc_tool_result_fail("Fetch failed", 12);
        return SC_OK;
    }
    if (resp.status_code < 200 || resp.status_code >= 300) {
        sc_http_response_free(alloc, &resp);
        *out = sc_tool_result_fail("HTTP request failed", 18);
        return SC_OK;
    }

    size_t text_len = 0;
    char *text = html_to_text(alloc, resp.body, resp.body_len, &text_len);
    sc_http_response_free(alloc, &resp);
    if (!text) {
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }

        if (text_len > max_chars) {
        char suffix[128];
        int n = snprintf(suffix, sizeof(suffix), "\n\n[Content truncated at %u chars, total %zu chars]", max_chars, text_len);
        size_t suffix_len = (n > 0 && (size_t)n < sizeof(suffix)) ? (size_t)n : 0;
        char *trunc = (char *)alloc->alloc(alloc->ctx, max_chars + suffix_len + 1);
        if (!trunc) { alloc->free(alloc->ctx, text, text_len + 1); *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
        memcpy(trunc, text, max_chars);
        if (suffix_len > 0) memcpy(trunc + max_chars, suffix, suffix_len + 1);
        else trunc[max_chars] = '\0';
        alloc->free(alloc->ctx, text, text_len + 1);
        text = trunc;
        text_len = max_chars + suffix_len;
    }

    *out = sc_tool_result_ok_owned(text, text_len);
    return SC_OK;
#endif
}

static const char *web_fetch_name(void *ctx) { (void)ctx; return SC_WEB_FETCH_NAME; }
static const char *web_fetch_description(void *ctx) { (void)ctx; return SC_WEB_FETCH_DESC; }
static const char *web_fetch_parameters_json(void *ctx) { (void)ctx; return SC_WEB_FETCH_PARAMS; }
static void web_fetch_deinit(void *ctx, sc_allocator_t *alloc) { (void)alloc; if (ctx) free(ctx); }

static const sc_tool_vtable_t web_fetch_vtable = {
    .execute = web_fetch_execute, .name = web_fetch_name,
    .description = web_fetch_description, .parameters_json = web_fetch_parameters_json,
    .deinit = web_fetch_deinit,
};

sc_error_t sc_web_fetch_create(sc_allocator_t *alloc,
    uint32_t max_chars,
    sc_tool_t *out)
{
    (void)alloc;
    sc_web_fetch_ctx_t *c = (sc_web_fetch_ctx_t *)calloc(1, sizeof(*c));
    if (!c) return SC_ERR_OUT_OF_MEMORY;
    c->max_chars = max_chars > 0 ? max_chars : SC_WEB_FETCH_DEFAULT_MAX;
    out->ctx = c;
    out->vtable = &web_fetch_vtable;
    return SC_OK;
}
