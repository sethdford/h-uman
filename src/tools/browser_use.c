#include "human/tools/browser_use.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(HU_IS_TEST) && HU_IS_TEST
#include "human/tools/visual_grounding.h"
#endif

typedef struct hu_browser_use_ctx {
    hu_provider_t *ground_provider;
    const char *ground_model;
    size_t ground_model_len;
} hu_browser_use_ctx_t;

#ifndef HU_IS_TEST
#include "human/multimodal.h"
#include "human/pwa/cdp.h"
#include "human/tools/visual_grounding.h"
#include <stdint.h>
#if !defined(_WIN32) && !defined(__CYGWIN__)
#include <unistd.h>
#endif

static int bu_b64_val(unsigned char c) {
    if (c >= 'A' && c <= 'Z')
        return (int)(c - 'A');
    if (c >= 'a' && c <= 'z')
        return (int)(c - 'a' + 26);
    if (c >= '0' && c <= '9')
        return (int)(c - '0' + 52);
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

static hu_error_t bu_b64_png_write_path(hu_allocator_t *alloc, const char *b64, size_t b64_len,
                                        const char *path) {
    while (b64_len > 0 &&
           (b64[b64_len - 1] == '=' || b64[b64_len - 1] == '\n' || b64[b64_len - 1] == '\r'))
        b64_len--;
    size_t raw_cap = (b64_len * 3) / 4 + 4;
    if (raw_cap > HU_MULTIMODAL_MAX_IMAGE_SIZE)
        return HU_ERR_INVALID_ARGUMENT;
    unsigned char *raw = (unsigned char *)alloc->alloc(alloc->ctx, raw_cap);
    if (!raw)
        return HU_ERR_OUT_OF_MEMORY;
    size_t j = 0;
    for (size_t i = 0; i + 4 <= b64_len; i += 4) {
        int a = bu_b64_val((unsigned char)b64[i]);
        int b = bu_b64_val((unsigned char)b64[i + 1]);
        int c = bu_b64_val((unsigned char)b64[i + 2]);
        int d = bu_b64_val((unsigned char)b64[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) {
            alloc->free(alloc->ctx, raw, raw_cap);
            return HU_ERR_PARSE;
        }
        uint32_t v = (uint32_t)(((uint32_t)(unsigned)a << 18) | ((uint32_t)(unsigned)b << 12) |
                                ((uint32_t)(unsigned)c << 6) | (uint32_t)(unsigned)d);
        raw[j++] = (unsigned char)(v >> 16);
        raw[j++] = (unsigned char)(v >> 8);
        raw[j++] = (unsigned char)v;
    }
    if (b64_len % 4 == 2) {
        int a = bu_b64_val((unsigned char)b64[b64_len - 2]);
        int b = bu_b64_val((unsigned char)b64[b64_len - 1]);
        if (a < 0 || b < 0) {
            alloc->free(alloc->ctx, raw, raw_cap);
            return HU_ERR_PARSE;
        }
        uint32_t v = (uint32_t)(((uint32_t)(unsigned)a << 18) | ((uint32_t)(unsigned)b << 12));
        raw[j++] = (unsigned char)(v >> 16);
    } else if (b64_len % 4 == 3) {
        int a = bu_b64_val((unsigned char)b64[b64_len - 3]);
        int b = bu_b64_val((unsigned char)b64[b64_len - 2]);
        int c = bu_b64_val((unsigned char)b64[b64_len - 1]);
        if (a < 0 || b < 0 || c < 0) {
            alloc->free(alloc->ctx, raw, raw_cap);
            return HU_ERR_PARSE;
        }
        uint32_t v = (uint32_t)(((uint32_t)(unsigned)a << 18) | ((uint32_t)(unsigned)b << 12) |
                                ((uint32_t)(unsigned)c << 6));
        raw[j++] = (unsigned char)(v >> 16);
        raw[j++] = (unsigned char)(v >> 8);
    }
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        alloc->free(alloc->ctx, raw, raw_cap);
        return HU_ERR_IO;
    }
    size_t nw = fwrite(raw, 1, j, fp);
    fclose(fp);
    alloc->free(alloc->ctx, raw, raw_cap);
    if (nw != j)
        return HU_ERR_IO;
    return HU_OK;
}

#endif /* !HU_IS_TEST */

#define HU_BU_MAX_URL      2048U
#define HU_BU_MAX_SELECTOR 512U
#define HU_BU_MAX_TEXT     8192U
#define HU_BU_MAX_SCRIPT   16384U

static const char k_params[] =
    "{\"type\":\"object\",\"required\":[\"action\"],\"properties\":{"
    "\"action\":{\"type\":\"string\","
    "\"enum\":[\"navigate\",\"screenshot\",\"click\",\"type\",\"extract_text\",\"execute_js\"]},"
    "\"url\":{\"type\":\"string\",\"description\":\"Target URL (navigate)\"},"
    "\"selector\":{\"type\":\"string\",\"description\":\"CSS selector (click, type, "
    "extract_text)\"},"
    "\"target\":{\"type\":\"string\",\"description\":\"Natural language description of UI element "
    "to interact with (uses vision to locate)\"},"
    "\"text\":{\"type\":\"string\",\"description\":\"Text to type (type)\"},"
    "\"script\":{\"type\":\"string\",\"description\":\"JavaScript source (execute_js)\"}}}";

static int bu_url_allowed(const char *url, size_t url_len) {
    if (!url || url_len == 0 || url_len > HU_BU_MAX_URL)
        return 0;
    if (url_len >= 8 && strncmp(url, "https://", 8) == 0)
        ;
    else if (url_len >= 7 && strncmp(url, "http://", 7) == 0)
        ;
    else
        return 0;
    for (size_t i = 0; i < url_len; i++) {
        unsigned char c = (unsigned char)url[i];
        if (c < 32U || c == '"' || c == '\\')
            return 0;
    }
    return 1;
}

#ifndef HU_IS_TEST
static int bu_str_has_disallowed_cdp_chars(const char *s, size_t n) {
    if (!s)
        return 1;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\')
            return 1;
    }
    return 0;
}

static hu_error_t bu_escape_json_string_body(hu_allocator_t *alloc, const char *s, size_t n,
                                             char **out, size_t *out_cap) {
    size_t cap = n * 4 + 8;
    char *d = (char *)alloc->alloc(alloc->ctx, cap);
    if (!d)
        return HU_ERR_OUT_OF_MEMORY;
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        char esc[8];
        size_t elen = 0;
        if (c == '"' || c == '\\') {
            esc[0] = '\\';
            esc[1] = (char)c;
            elen = 2;
        } else if (c == '\n') {
            esc[0] = '\\';
            esc[1] = 'n';
            elen = 2;
        } else if (c == '\r') {
            esc[0] = '\\';
            esc[1] = 'r';
            elen = 2;
        } else if (c == '\t') {
            esc[0] = '\\';
            esc[1] = 't';
            elen = 2;
        } else if (c < 32U) {
            (void)snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)c);
            elen = strlen(esc);
        } else {
            esc[0] = (char)c;
            elen = 1;
        }
        if (j + elen + 2 >= cap) {
            size_t ncap = (cap + elen + 64) * 2;
            char *nd = (char *)alloc->realloc(alloc->ctx, d, cap, ncap);
            if (!nd) {
                alloc->free(alloc->ctx, d, cap);
                return HU_ERR_OUT_OF_MEMORY;
            }
            d = nd;
            cap = ncap;
        }
        memcpy(d + j, esc, elen);
        j += elen;
    }
    d[j] = '\0';
    *out = d;
    *out_cap = cap;
    return HU_OK;
}

static hu_error_t bu_eval_expression(hu_cdp_session_t *s, const char *expr, size_t expr_len,
                                     hu_json_value_t **value_out) {
    char *raw = NULL;
    size_t raw_len = 0;
    hu_error_t err = hu_cdp_evaluate(s, expr, expr_len, &raw, &raw_len);
    if (err != HU_OK || !raw)
        return err != HU_OK ? err : HU_ERR_IO;

    hu_json_value_t *root = NULL;
    err = hu_json_parse(s->alloc, raw, raw_len, &root);
    s->alloc->free(s->alloc->ctx, raw, raw_len + 1);
    if (err != HU_OK)
        return err;

    const hu_json_value_t *r1 = hu_json_object_get(root, "result");
    const hu_json_value_t *r2 = r1 ? hu_json_object_get(r1, "result") : NULL;
    const hu_json_value_t *val = r2 ? hu_json_object_get(r2, "value") : NULL;
    if (!val) {
        hu_json_free(s->alloc, root);
        return HU_ERR_PARSE;
    }

    char *frag = NULL;
    size_t frag_len = 0;
    err = hu_json_stringify(s->alloc, val, &frag, &frag_len);
    if (err != HU_OK || !frag) {
        hu_json_free(s->alloc, root);
        return err != HU_OK ? err : HU_ERR_OUT_OF_MEMORY;
    }
    hu_json_free(s->alloc, root);

    hu_json_value_t *parsed = NULL;
    err = hu_json_parse(s->alloc, frag, frag_len, &parsed);
    s->alloc->free(s->alloc->ctx, frag, frag_len + 1);
    if (err != HU_OK)
        return err;
    *value_out = parsed;
    return HU_OK;
}

#endif /* !HU_IS_TEST */

#ifdef HU_IS_TEST

static hu_error_t browser_use_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                      hu_tool_result_t *out) {
    (void)ctx;
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    if (!args || args->type != HU_JSON_OBJECT) {
        *out = hu_tool_result_fail("invalid arguments", 17);
        return HU_OK;
    }

    const char *action = hu_json_get_string(args, "action");
    if (!action || !action[0]) {
        *out = hu_tool_result_fail("missing action", 14);
        return HU_OK;
    }

    if (strcmp(action, "navigate") == 0) {
        const char *url = hu_json_get_string(args, "url");
        if (!url || url[0] == '\0') {
            *out = hu_tool_result_fail("invalid or missing url", 22);
            return HU_ERR_INVALID_ARGUMENT;
        }
        if (!bu_url_allowed(url, strlen(url))) {
            *out = hu_tool_result_fail("invalid or missing url", 22);
            return HU_OK;
        }
        char *msg = hu_sprintf(alloc, "{\"status\":\"ok\",\"url\":\"%s\"}", url);
        if (!msg) {
            *out = hu_tool_result_fail("out of memory", 13);
            return HU_OK;
        }
        *out = hu_tool_result_ok_owned(msg, strlen(msg));
        return HU_OK;
    }

    if (strcmp(action, "screenshot") == 0) {
        const char *msg = "{\"status\":\"ok\",\"base64\":\"iVBORw0KGgoAAAANSUhEUg==mock\"}";
        *out = hu_tool_result_ok(msg, strlen(msg));
        return HU_OK;
    }

    if (strcmp(action, "click") == 0) {
        const char *sel = hu_json_get_string(args, "selector");
        const char *target = hu_json_get_string(args, "target");
        size_t sellen = sel ? strlen(sel) : 0;
        size_t tgtlen = target ? strlen(target) : 0;
        if (sellen > 0) {
            if (sellen > HU_BU_MAX_SELECTOR) {
                *out = hu_tool_result_fail("invalid or missing selector", 27);
                return HU_OK;
            }
            char *msg = hu_sprintf(alloc, "{\"status\":\"ok\",\"selector\":\"%s\"}", sel);
            if (!msg) {
                *out = hu_tool_result_fail("out of memory", 13);
                return HU_OK;
            }
            *out = hu_tool_result_ok_owned(msg, strlen(msg));
            return HU_OK;
        }
        if (tgtlen > 0) {
            if (tgtlen > HU_BU_MAX_TEXT) {
                *out = hu_tool_result_fail("target too long", 15);
                return HU_OK;
            }
            char *gsel = NULL;
            size_t gsel_len = 0;
            double gx = 0, gy = 0;
            hu_error_t ge = hu_visual_ground_action(alloc, NULL, NULL, 0, "mock.png", 9, target,
                                                    tgtlen, &gx, &gy, &gsel, &gsel_len);
            if (ge != HU_OK) {
                if (gsel)
                    alloc->free(alloc->ctx, gsel, gsel_len + 1);
                *out = hu_tool_result_fail("visual grounding failed", 22);
                return HU_OK;
            }
            if (gsel && gsel[0]) {
                char *msg = hu_sprintf(
                    alloc, "{\"status\":\"ok\",\"click_via\":\"selector\",\"selector\":\"%s\"}",
                    gsel);
                alloc->free(alloc->ctx, gsel, gsel_len + 1);
                if (!msg) {
                    *out = hu_tool_result_fail("out of memory", 13);
                    return HU_OK;
                }
                *out = hu_tool_result_ok_owned(msg, strlen(msg));
                return HU_OK;
            }
            if (gsel)
                alloc->free(alloc->ctx, gsel, gsel_len + 1);
            char *msg = hu_sprintf(
                alloc, "{\"status\":\"ok\",\"click_via\":\"coordinates\",\"x\":%.0f,\"y\":%.0f}",
                gx, gy);
            if (!msg) {
                *out = hu_tool_result_fail("out of memory", 13);
                return HU_OK;
            }
            *out = hu_tool_result_ok_owned(msg, strlen(msg));
            return HU_OK;
        }
        *out = hu_tool_result_fail("invalid or missing selector", 27);
        return HU_OK;
    }

    if (strcmp(action, "type") == 0) {
        const char *sel = hu_json_get_string(args, "selector");
        const char *text = hu_json_get_string(args, "text");
        if (!sel || !sel[0] || strlen(sel) > HU_BU_MAX_SELECTOR || !text ||
            strlen(text) > HU_BU_MAX_TEXT) {
            *out = hu_tool_result_fail("invalid selector or text", 24);
            return HU_OK;
        }
        const char *msg = "{\"status\":\"ok\"}";
        *out = hu_tool_result_ok(msg, strlen(msg));
        return HU_OK;
    }

    if (strcmp(action, "extract_text") == 0) {
        (void)hu_json_get_string(args, "selector"); /* optional in mock */
        const char *msg = "{\"status\":\"ok\",\"text\":\"mock extracted text\"}";
        *out = hu_tool_result_ok(msg, strlen(msg));
        return HU_OK;
    }

    if (strcmp(action, "execute_js") == 0) {
        const char *script = hu_json_get_string(args, "script");
        if (!script || !script[0] || strlen(script) > HU_BU_MAX_SCRIPT) {
            *out = hu_tool_result_fail("invalid or missing script", 25);
            return HU_OK;
        }
        const char *msg = "{\"status\":\"ok\",\"result\":\"mock result\"}";
        *out = hu_tool_result_ok(msg, strlen(msg));
        return HU_OK;
    }

    *out = hu_tool_result_fail("unknown action", 14);
    return HU_OK;
}

#else /* !HU_IS_TEST */

static hu_error_t browser_use_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                      hu_tool_result_t *out) {
    hu_browser_use_ctx_t *bctx = (hu_browser_use_ctx_t *)ctx;
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    if (!args || args->type != HU_JSON_OBJECT) {
        *out = hu_tool_result_fail("invalid arguments", 17);
        return HU_OK;
    }

    const char *action = hu_json_get_string(args, "action");
    if (!action || !action[0]) {
        *out = hu_tool_result_fail("missing action", 14);
        return HU_OK;
    }

    hu_cdp_session_t session;
    memset(&session, 0, sizeof(session));
    hu_error_t cerr = hu_cdp_connect(alloc, "127.0.0.1", 9222, &session);
    if (cerr != HU_OK || !session.connected) {
        static const char msg_cdp[] = "could not connect to Chrome DevTools (127.0.0.1:9222)";
        *out = hu_tool_result_fail(msg_cdp, sizeof(msg_cdp) - 1);
        hu_cdp_disconnect(&session);
        return HU_OK;
    }

    hu_error_t err = HU_OK;
    if (strcmp(action, "navigate") == 0) {
        const char *url = hu_json_get_string(args, "url");
        if (!url || !bu_url_allowed(url, strlen(url))) {
            *out = hu_tool_result_fail("invalid or missing url", 22);
            goto done;
        }
        err = hu_cdp_navigate(&session, url, strlen(url));
        if (err != HU_OK) {
            *out = hu_tool_result_fail("navigate failed", 15);
            goto done;
        }
        char *msg = hu_sprintf(alloc, "{\"status\":\"ok\",\"url\":\"%s\"}", url);
        if (!msg) {
            *out = hu_tool_result_fail("out of memory", 13);
            goto done;
        }
        *out = hu_tool_result_ok_owned(msg, strlen(msg));
        goto done;
    }

    if (strcmp(action, "screenshot") == 0) {
        hu_cdp_screenshot_t shot;
        memset(&shot, 0, sizeof(shot));
        err = hu_cdp_screenshot(&session, &shot);
        if (err != HU_OK || !shot.data_base64) {
            *out = hu_tool_result_fail("screenshot failed", 17);
            if (shot.data_base64)
                alloc->free(alloc->ctx, shot.data_base64, shot.data_len + 1);
            goto done;
        }
        char *msg = hu_sprintf(alloc, "{\"status\":\"ok\",\"base64\":\"%s\"}", shot.data_base64);
        alloc->free(alloc->ctx, shot.data_base64, shot.data_len + 1);
        if (!msg) {
            *out = hu_tool_result_fail("out of memory", 13);
            goto done;
        }
        *out = hu_tool_result_ok_owned(msg, strlen(msg));
        goto done;
    }

    if (strcmp(action, "click") == 0) {
        const char *sel = hu_json_get_string(args, "selector");
        const char *target = hu_json_get_string(args, "target");
        size_t sellen = sel ? strlen(sel) : 0;
        size_t tgtlen = target ? strlen(target) : 0;
        if (tgtlen > HU_BU_MAX_TEXT) {
            *out = hu_tool_result_fail("target too long", 15);
            goto done;
        }
        int cx = 0, cy = 0;

        if (sellen > 0) {
            if (sellen > HU_BU_MAX_SELECTOR || bu_str_has_disallowed_cdp_chars(sel, sellen)) {
                *out = hu_tool_result_fail("invalid selector", 16);
                goto done;
            }
            hu_cdp_element_t el;
            size_t count = 0;
            err = hu_cdp_query_elements(&session, sel, sellen, &el, 1, &count);
            if (err != HU_OK || count == 0) {
                *out = hu_tool_result_fail("element not found", 17);
                goto done;
            }
            cx = el.x + (el.width > 0 ? el.width / 2 : 0);
            cy = el.y + (el.height > 0 ? el.height / 2 : 0);
            err = hu_cdp_click(&session, cx, cy);
            if (err != HU_OK) {
                *out = hu_tool_result_fail("click failed", 12);
                goto done;
            }
            char *msg0 =
                hu_sprintf(alloc, "{\"status\":\"ok\",\"selector\":\"%s\"}", sel ? sel : "");
            if (!msg0) {
                *out = hu_tool_result_fail("out of memory", 13);
                goto done;
            }
            *out = hu_tool_result_ok_owned(msg0, strlen(msg0));
            goto done;
        } else if (tgtlen > 0 && bctx && bctx->ground_provider) {
            hu_cdp_screenshot_t shot;
            memset(&shot, 0, sizeof(shot));
            err = hu_cdp_screenshot(&session, &shot);
            if (err != HU_OK || !shot.data_base64) {
                *out = hu_tool_result_fail("screenshot failed", 17);
                if (shot.data_base64)
                    alloc->free(alloc->ctx, shot.data_base64, shot.data_len + 1);
                goto done;
            }
            char tmpl[] = "/tmp/hu_bu_vgXXXXXX.png";
            int fd = mkstemps(tmpl, 4);
            if (fd < 0) {
                alloc->free(alloc->ctx, shot.data_base64, shot.data_len + 1);
                *out = hu_tool_result_fail("temp file failed", 16);
                goto done;
            }
            (void)close(fd);
            err = bu_b64_png_write_path(alloc, shot.data_base64, shot.data_len, tmpl);
            alloc->free(alloc->ctx, shot.data_base64, shot.data_len + 1);
            if (err != HU_OK) {
                (void)unlink(tmpl);
                *out = hu_tool_result_fail("screenshot decode failed", 24);
                goto done;
            }
            double gx = -1.0, gy = -1.0;
            char *gsel = NULL;
            size_t gsel_len = 0;
            err = hu_visual_ground_action(alloc, bctx->ground_provider, bctx->ground_model,
                                          bctx->ground_model_len, tmpl, strlen(tmpl), target,
                                          tgtlen, &gx, &gy, &gsel, &gsel_len);
            (void)unlink(tmpl);
            if (err != HU_OK || gx < 0.0 || gy < 0.0) {
                if (gsel)
                    alloc->free(alloc->ctx, gsel, gsel_len + 1);
                *out = hu_tool_result_fail("visual grounding failed", 22);
                goto done;
            }
            cx = (int)gx;
            cy = (int)gy;

            int selector_ok = 0;
            if (gsel && gsel[0] && gsel_len <= HU_BU_MAX_SELECTOR &&
                !bu_str_has_disallowed_cdp_chars(gsel, gsel_len)) {
                char *esel = NULL;
                size_t esel_cap = 0;
                hu_error_t ee = bu_escape_json_string_body(alloc, gsel, gsel_len, &esel, &esel_cap);
                if (ee == HU_OK && esel) {
                    char expr[4096];
                    int en =
                        snprintf(expr, sizeof(expr),
                                 "(function(){var e=document.querySelector(\"%s\");if(!e)return "
                                 "false;e.click();return true;})()",
                                 esel);
                    alloc->free(alloc->ctx, esel, esel_cap);
                    if (en > 0 && (size_t)en < sizeof(expr)) {
                        hu_json_value_t *vreq = NULL;
                        hu_error_t ev = bu_eval_expression(&session, expr, (size_t)en, &vreq);
                        if (ev == HU_OK && vreq && vreq->type == HU_JSON_BOOL &&
                            vreq->data.boolean) {
                            selector_ok = 1;
                        }
                        if (vreq)
                            hu_json_free(alloc, vreq);
                    }
                }
            }

            if (!selector_ok) {
                err = hu_cdp_click(&session, cx, cy);
                if (err != HU_OK) {
                    if (gsel)
                        alloc->free(alloc->ctx, gsel, gsel_len + 1);
                    *out = hu_tool_result_fail("click failed", 12);
                    goto done;
                }
            } else {
                err = HU_OK;
            }

            char *msg = NULL;
            if (selector_ok && gsel) {
                msg = hu_sprintf(
                    alloc, "{\"status\":\"ok\",\"click_via\":\"selector\",\"selector\":\"%s\"}",
                    gsel);
            } else {
                msg = hu_sprintf(
                    alloc, "{\"status\":\"ok\",\"click_via\":\"coordinates\",\"x\":%d,\"y\":%d}",
                    cx, cy);
            }
            if (gsel)
                alloc->free(alloc->ctx, gsel, gsel_len + 1);
            if (!msg) {
                *out = hu_tool_result_fail("out of memory", 13);
                goto done;
            }
            *out = hu_tool_result_ok_owned(msg, strlen(msg));
            goto done;
        } else if (tgtlen > 0) {
            *out = hu_tool_result_fail("target requires visual grounding provider", 40);
            goto done;
        } else {
            *out = hu_tool_result_fail("invalid or missing selector", 27);
            goto done;
        }
    }

    if (strcmp(action, "type") == 0) {
        const char *sel = hu_json_get_string(args, "selector");
        const char *text = hu_json_get_string(args, "text");
        size_t sellen = sel ? strlen(sel) : 0;
        size_t textlen = text ? strlen(text) : 0;
        if (!sel || sellen == 0 || sellen > HU_BU_MAX_SELECTOR || !text ||
            textlen > HU_BU_MAX_TEXT || bu_str_has_disallowed_cdp_chars(sel, sellen) ||
            bu_str_has_disallowed_cdp_chars(text, textlen)) {
            *out = hu_tool_result_fail("invalid selector or text", 24);
            goto done;
        }
        char *esel = NULL;
        size_t esel_cap = 0;
        char *etext = NULL;
        size_t etext_cap = 0;
        err = bu_escape_json_string_body(alloc, sel, sellen, &esel, &esel_cap);
        if (err != HU_OK) {
            *out = hu_tool_result_fail("out of memory", 13);
            goto done;
        }
        err = bu_escape_json_string_body(alloc, text, textlen, &etext, &etext_cap);
        if (err != HU_OK) {
            alloc->free(alloc->ctx, esel, esel_cap);
            *out = hu_tool_result_fail("out of memory", 13);
            goto done;
        }
        char expr[8192];
        int en =
            snprintf(expr, sizeof(expr),
                     "(function(){var s=\"%s\";var t=\"%s\";"
                     "var e=document.querySelector(s);if(!e)return\"\";"
                     "e.focus();if(\"value\" in e&&e.tagName!==\"DIV\"){e.value=t;}"
                     "else{e.textContent=t;}return(e.innerText||e.value||\"\").slice(0,4096);})()",
                     esel, etext);
        alloc->free(alloc->ctx, esel, esel_cap);
        alloc->free(alloc->ctx, etext, etext_cap);
        if (en <= 0 || (size_t)en >= sizeof(expr)) {
            *out = hu_tool_result_fail("type expression too large", 25);
            goto done;
        }

        hu_json_value_t *val = NULL;
        err = bu_eval_expression(&session, expr, (size_t)en, &val);
        if (err != HU_OK) {
            *out = hu_tool_result_fail("type failed", 11);
            goto done;
        }
        char *inner = NULL;
        size_t inner_len = 0;
        err = hu_json_stringify(alloc, val, &inner, &inner_len);
        hu_json_free(alloc, val);
        if (err != HU_OK || !inner) {
            *out = hu_tool_result_fail("type failed", 11);
            goto done;
        }
        char *msg = hu_sprintf(alloc, "{\"status\":\"ok\",\"text\":%s}", inner);
        alloc->free(alloc->ctx, inner, inner_len + 1);
        if (!msg) {
            *out = hu_tool_result_fail("out of memory", 13);
            goto done;
        }
        *out = hu_tool_result_ok_owned(msg, strlen(msg));
        goto done;
    }

    if (strcmp(action, "extract_text") == 0) {
        const char *sel = hu_json_get_string(args, "selector");
        const char *use_sel = (sel && sel[0]) ? sel : "body";
        size_t sellen = strlen(use_sel);
        if (sellen > HU_BU_MAX_SELECTOR || bu_str_has_disallowed_cdp_chars(use_sel, sellen)) {
            *out = hu_tool_result_fail("invalid selector", 16);
            goto done;
        }
        char *esel = NULL;
        size_t esel_cap = 0;
        err = bu_escape_json_string_body(alloc, use_sel, sellen, &esel, &esel_cap);
        if (err != HU_OK) {
            *out = hu_tool_result_fail("out of memory", 13);
            goto done;
        }
        char expr[1024];
        int en = snprintf(expr, sizeof(expr),
                          "(function(){var e=document.querySelector(\"%s\");"
                          "if(!e)e=document.body;return (e.innerText||\"\").slice(0,65536);})()",
                          esel);
        alloc->free(alloc->ctx, esel, esel_cap);
        if (en <= 0 || (size_t)en >= sizeof(expr)) {
            *out = hu_tool_result_fail("extract_text expression too large", 31);
            goto done;
        }

        hu_json_value_t *val = NULL;
        err = bu_eval_expression(&session, expr, (size_t)en, &val);
        if (err != HU_OK) {
            *out = hu_tool_result_fail("extract_text failed", 19);
            goto done;
        }
        char *inner = NULL;
        size_t inner_len = 0;
        err = hu_json_stringify(alloc, val, &inner, &inner_len);
        hu_json_free(alloc, val);
        if (err != HU_OK || !inner) {
            *out = hu_tool_result_fail("extract_text failed", 19);
            goto done;
        }
        char *msg = hu_sprintf(alloc, "{\"status\":\"ok\",\"text\":%s}", inner);
        alloc->free(alloc->ctx, inner, inner_len + 1);
        if (!msg) {
            *out = hu_tool_result_fail("out of memory", 13);
            goto done;
        }
        *out = hu_tool_result_ok_owned(msg, strlen(msg));
        goto done;
    }

    if (strcmp(action, "execute_js") == 0) {
        const char *script = hu_json_get_string(args, "script");
        size_t slen = script ? strlen(script) : 0;
        if (!script || slen == 0 || slen > HU_BU_MAX_SCRIPT ||
            bu_str_has_disallowed_cdp_chars(script, slen)) {
            *out = hu_tool_result_fail("invalid or missing script", 25);
            goto done;
        }
        char *raw = NULL;
        size_t raw_len = 0;
        err = hu_cdp_evaluate(&session, script, slen, &raw, &raw_len);
        if (err != HU_OK || !raw) {
            *out = hu_tool_result_fail("execute_js failed", 17);
            if (raw)
                alloc->free(alloc->ctx, raw, raw_len + 1);
            goto done;
        }

        hu_json_value_t *root = NULL;
        err = hu_json_parse(alloc, raw, raw_len, &root);
        alloc->free(alloc->ctx, raw, raw_len + 1);
        if (err != HU_OK) {
            *out = hu_tool_result_fail("execute_js parse failed", 21);
            goto done;
        }
        const hu_json_value_t *r1 = hu_json_object_get(root, "result");
        const hu_json_value_t *r2 = r1 ? hu_json_object_get(r1, "result") : NULL;
        const hu_json_value_t *val = r2 ? hu_json_object_get(r2, "value") : NULL;
        if (!val) {
            hu_json_free(alloc, root);
            *out = hu_tool_result_fail("execute_js no result", 18);
            goto done;
        }
        char *inner = NULL;
        size_t inner_len = 0;
        err = hu_json_stringify(alloc, val, &inner, &inner_len);
        hu_json_free(alloc, root);
        if (err != HU_OK || !inner) {
            *out = hu_tool_result_fail("execute_js failed", 17);
            goto done;
        }
        char *msg = hu_sprintf(alloc, "{\"status\":\"ok\",\"result\":%s}", inner);
        alloc->free(alloc->ctx, inner, inner_len + 1);
        if (!msg) {
            *out = hu_tool_result_fail("out of memory", 13);
            goto done;
        }
        *out = hu_tool_result_ok_owned(msg, strlen(msg));
        goto done;
    }

    *out = hu_tool_result_fail("unknown action", 14);

done:
    hu_cdp_disconnect(&session);
    return HU_OK;
}

#endif /* HU_IS_TEST */

static const char *browser_use_name(void *ctx) {
    (void)ctx;
    return "browser_use";
}

static const char *browser_use_description(void *ctx) {
    (void)ctx;
    return "Control a web browser via Chrome DevTools Protocol. Actions: navigate, screenshot, "
           "click, type, extract_text, execute_js";
}

static const char *browser_use_parameters_json(void *ctx) {
    (void)ctx;
    return k_params;
}

static void browser_use_deinit(void *ctx, hu_allocator_t *alloc) {
    if (!ctx || !alloc)
        return;
    alloc->free(alloc->ctx, ctx, sizeof(hu_browser_use_ctx_t));
}

static const hu_tool_vtable_t browser_use_vtable = {
    .execute = browser_use_execute,
    .name = browser_use_name,
    .description = browser_use_description,
    .parameters_json = browser_use_parameters_json,
    .deinit = browser_use_deinit,
};

hu_error_t hu_browser_use_create(hu_allocator_t *alloc, hu_tool_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_browser_use_ctx_t *bx =
        (hu_browser_use_ctx_t *)alloc->alloc(alloc->ctx, sizeof(hu_browser_use_ctx_t));
    if (!bx)
        return HU_ERR_OUT_OF_MEMORY;
    memset(bx, 0, sizeof(*bx));
    out->ctx = bx;
    out->vtable = &browser_use_vtable;
    return HU_OK;
}

void hu_browser_use_destroy(hu_allocator_t *alloc, hu_tool_t *tool) {
    if (!alloc || !tool || !tool->ctx)
        return;
    alloc->free(alloc->ctx, tool->ctx, sizeof(hu_browser_use_ctx_t));
    tool->ctx = NULL;
}

void hu_browser_use_set_grounding(hu_tool_t *tool, hu_provider_t *provider, const char *model,
                                  size_t model_len) {
    if (!tool || !tool->ctx)
        return;
    hu_browser_use_ctx_t *b = (hu_browser_use_ctx_t *)tool->ctx;
    b->ground_provider = provider;
    b->ground_model = model;
    b->ground_model_len = model_len;
}
