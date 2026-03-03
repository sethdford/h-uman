/* Browser tool: open URL, read page, CDP automation (click/type/scroll).
 * CDP uses a headless Chrome instance with --remote-debugging-port, communicating
 * over the project's WebSocket client. Requires Chrome/Chromium on PATH. */
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/process_util.h"
#include "seaclaw/core/string.h"
#include "seaclaw/security.h"
#include "seaclaw/tool.h"
#include "seaclaw/tools/validation.h"
#if !SC_IS_TEST
#include "seaclaw/websocket/websocket.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#if !SC_IS_TEST
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

#define SC_BROWSER_READ_MAX 8192
#define SC_CDP_PORT         9222
#define SC_CDP_MAX_REPLY    65536

typedef struct sc_browser_ctx {
    sc_security_policy_t *policy;
    sc_allocator_t *alloc;
#if !SC_IS_TEST
    sc_ws_client_t *cdp_ws;
    pid_t chrome_pid;
    int cdp_msg_id;
    char *current_url;
#endif
} sc_browser_ctx_t;

#if !SC_IS_TEST
static const char *find_chrome_binary(void) {
#ifdef __APPLE__
    if (access("/Applications/Google Chrome.app/Contents/MacOS/Google Chrome", X_OK) == 0)
        return "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";
    if (access("/Applications/Chromium.app/Contents/MacOS/Chromium", X_OK) == 0)
        return "/Applications/Chromium.app/Contents/MacOS/Chromium";
#endif
    const char *candidates[] = {"google-chrome", "chromium-browser", "chromium", "chrome", NULL};
    for (int i = 0; candidates[i]; i++) {
        char path[256];
        snprintf(path, sizeof(path), "/usr/bin/%s", candidates[i]);
        if (access(path, X_OK) == 0)
            return candidates[i];
    }
    return NULL;
}

static sc_error_t cdp_ensure_connected(sc_browser_ctx_t *bc, sc_allocator_t *alloc,
                                       const char *url) {
    if (bc->cdp_ws)
        return SC_OK;

    const char *chrome = find_chrome_binary();
    if (!chrome)
        return SC_ERR_NOT_SUPPORTED;

    char port_arg[32];
    snprintf(port_arg, sizeof(port_arg), "--remote-debugging-port=%d", SC_CDP_PORT);
    pid_t pid = fork();
    if (pid < 0)
        return SC_ERR_IO;
    if (pid == 0) {
        setsid();
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execlp(chrome, chrome, "--headless", "--disable-gpu", "--no-sandbox", port_arg,
               url ? url : "about:blank", (char *)NULL);
        _exit(127);
    }
    bc->chrome_pid = pid;

    /* Wait for CDP to become available (up to 5 seconds) */
    for (int attempt = 0; attempt < 50; attempt++) {
        usleep(100000);
        sc_http_response_t resp = {0};
        sc_error_t err = sc_http_get_ex(alloc, "http://localhost:9222/json", NULL, &resp);
        if (err == SC_OK && resp.status_code == 200 && resp.body && resp.body_len > 10) {
            /* Extract webSocketDebuggerUrl from the JSON list */
            const char *ws_key = "\"webSocketDebuggerUrl\":\"";
            const char *found = strstr(resp.body, ws_key);
            if (found) {
                found += strlen(ws_key);
                const char *end = strchr(found, '"');
                if (end) {
                    size_t url_len = (size_t)(end - found);
                    char *ws_url = sc_strndup(alloc, found, url_len);
                    sc_http_response_free(alloc, &resp);
                    if (!ws_url)
                        return SC_ERR_OUT_OF_MEMORY;
                    err = sc_ws_connect(alloc, ws_url, &bc->cdp_ws);
                    alloc->free(alloc->ctx, ws_url, url_len + 1);
                    if (err == SC_OK) {
                        bc->cdp_msg_id = 1;
                        return SC_OK;
                    }
                    return err;
                }
            }
            sc_http_response_free(alloc, &resp);
        } else {
            sc_http_response_free(alloc, &resp);
        }
    }
    return SC_ERR_TIMEOUT;
}

static sc_error_t cdp_send_command(sc_browser_ctx_t *bc, sc_allocator_t *alloc, const char *method,
                                   const char *params_json, char **result_out,
                                   size_t *result_len_out) {
    if (!bc->cdp_ws)
        return SC_ERR_NOT_SUPPORTED;
    int id = bc->cdp_msg_id++;
    char msg[4096];
    int n;
    if (params_json && params_json[0])
        n = snprintf(msg, sizeof(msg), "{\"id\":%d,\"method\":\"%s\",\"params\":%s}", id, method,
                     params_json);
    else
        n = snprintf(msg, sizeof(msg), "{\"id\":%d,\"method\":\"%s\"}", id, method);
    if (n <= 0 || (size_t)n >= sizeof(msg))
        return SC_ERR_INVALID_ARGUMENT;

    sc_error_t err = sc_ws_send(bc->cdp_ws, msg, (size_t)n);
    if (err != SC_OK)
        return err;

    /* Read responses until we get one matching our id */
    for (int tries = 0; tries < 20; tries++) {
        char *data = NULL;
        size_t data_len = 0;
        err = sc_ws_recv(bc->cdp_ws, alloc, &data, &data_len);
        if (err != SC_OK)
            return err;
        if (!data)
            continue;

        /* Quick check if this response matches our id */
        char id_pattern[32];
        snprintf(id_pattern, sizeof(id_pattern), "\"id\":%d", id);
        if (strstr(data, id_pattern)) {
            *result_out = data;
            *result_len_out = data_len;
            return SC_OK;
        }
        alloc->free(alloc->ctx, data, data_len + 1);
    }
    return SC_ERR_TIMEOUT;
}
#endif /* !SC_IS_TEST */

static sc_error_t browser_execute(void *ctx, sc_allocator_t *alloc, const sc_json_value_t *args,
                                  sc_tool_result_t *out) {
    sc_browser_ctx_t *bc = (sc_browser_ctx_t *)ctx;
    (void)bc;
    if (!args || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_ERR_INVALID_ARGUMENT;
    }
    const char *action = sc_json_get_string(args, "action");
    if (!action || action[0] == '\0') {
        *out = sc_tool_result_fail("missing action", 14);
        return SC_OK;
    }
    if (strcmp(action, "open") == 0) {
        const char *url = sc_json_get_string(args, "url");
        if (!url || url[0] == '\0') {
            *out = sc_tool_result_fail("missing url", 11);
            return SC_OK;
        }
        /* Secure by default: HTTPS only, or http://localhost for local dev */
        if (sc_tool_validate_url(url) == SC_OK) {
            /* Valid HTTPS URL */
        } else if (strncasecmp(url, "http://localhost", 16) == 0 &&
                   (url[16] == '\0' || url[16] == '/' || url[16] == ':' || url[16] == '?' ||
                    url[16] == '#')) {
            /* Allow http://localhost for local development */
        } else {
            *out = sc_tool_result_fail("invalid url: use https:// or http://localhost only", 48);
            return SC_OK;
        }
#if SC_IS_TEST
        size_t need = 27 + strlen(url);
        char *msg = (char *)alloc->alloc(alloc->ctx, need + 1);
        if (!msg) {
            *out = sc_tool_result_fail("out of memory", 12);
            return SC_ERR_OUT_OF_MEMORY;
        }
        int n = snprintf(msg, need + 1, "Opened %s in system browser", url);
        size_t len = (n > 0 && (size_t)n <= need) ? (size_t)n : need;
        msg[len] = '\0';
        *out = sc_tool_result_ok_owned(msg, len);
        return SC_OK;
#else
        {
            const char *argv[4];
#ifdef __APPLE__
            argv[0] = "open";
#else
            argv[0] = "xdg-open";
#endif
            argv[1] = url;
            argv[2] = NULL;
            sc_run_result_t run = {0};
            sc_error_t err =
                sc_process_run_with_policy(alloc, argv, NULL, 4096, bc ? bc->policy : NULL, &run);
            sc_run_result_free(alloc, &run);
            if (err != SC_OK) {
                *out = sc_tool_result_fail("Failed to open browser", 22);
                return SC_OK;
            }
            if (!run.success) {
                *out = sc_tool_result_fail("Browser open failed", 18);
                return SC_OK;
            }
            char *msg = sc_strndup(alloc, "Opened in system browser", 24);
            if (!msg) {
                *out = sc_tool_result_fail("out of memory", 12);
                return SC_ERR_OUT_OF_MEMORY;
            }
            *out = sc_tool_result_ok_owned(msg, 24);
            return SC_OK;
        }
#endif
    }
    if (strcmp(action, "read") == 0) {
        const char *url = sc_json_get_string(args, "url");
        if (!url || url[0] == '\0') {
            *out = sc_tool_result_fail("missing url", 11);
            return SC_OK;
        }
        if (sc_tool_validate_url(url) != SC_OK) {
            *out = sc_tool_result_fail("invalid url: HTTPS only, no private IPs", 37);
            return SC_OK;
        }
#if SC_IS_TEST
        size_t need = 28 + strlen(url);
        char *msg = (char *)alloc->alloc(alloc->ctx, need + 1);
        if (!msg) {
            *out = sc_tool_result_fail("out of memory", 12);
            return SC_ERR_OUT_OF_MEMORY;
        }
        int n = snprintf(msg, need + 1, "<html><body>Mock page for %s</body></html>", url);
        size_t len = (n > 0 && (size_t)n <= need) ? (size_t)n : need;
        msg[len] = '\0';
        *out = sc_tool_result_ok_owned(msg, len);
        return SC_OK;
#else
        {
            sc_http_response_t resp = {0};
            sc_error_t err = sc_http_get_ex(alloc, url, NULL, &resp);
            if (err != SC_OK) {
                *out = sc_tool_result_fail("Fetch failed", 12);
                return SC_OK;
            }
            if (resp.status_code < 200 || resp.status_code >= 300) {
                sc_http_response_free(alloc, &resp);
                *out = sc_tool_result_fail("HTTP request failed", 18);
                return SC_OK;
            }
            size_t copy_len = resp.body_len;
            if (copy_len > SC_BROWSER_READ_MAX)
                copy_len = SC_BROWSER_READ_MAX;
            char *body = (char *)alloc->alloc(alloc->ctx, copy_len + 1);
            if (!body) {
                sc_http_response_free(alloc, &resp);
                *out = sc_tool_result_fail("out of memory", 12);
                return SC_ERR_OUT_OF_MEMORY;
            }
            if (resp.body && copy_len > 0)
                memcpy(body, resp.body, copy_len);
            body[copy_len] = '\0';
            sc_http_response_free(alloc, &resp);
            *out = sc_tool_result_ok_owned(body, copy_len);
            return SC_OK;
        }
#endif
    }
    if (strcmp(action, "screenshot") == 0) {
        *out = sc_tool_result_fail("Use the screenshot tool instead", 31);
        return SC_OK;
    }
    if (strcmp(action, "click") == 0) {
        const char *selector = sc_json_get_string(args, "selector");
        if (!selector || selector[0] == '\0') {
            *out = sc_tool_result_fail("missing selector for click", 26);
            return SC_OK;
        }
#if SC_IS_TEST
        size_t need = 32 + strlen(selector);
        char *msg = (char *)alloc->alloc(alloc->ctx, need + 1);
        if (!msg) {
            *out = sc_tool_result_fail("out of memory", 12);
            return SC_ERR_OUT_OF_MEMORY;
        }
        int n = snprintf(msg, need + 1, "Clicked element: %s", selector);
        size_t len = (n > 0 && (size_t)n <= need) ? (size_t)n : need;
        msg[len] = '\0';
        *out = sc_tool_result_ok_owned(msg, len);
        return SC_OK;
#else
        {
            sc_error_t err = cdp_ensure_connected(bc, alloc, bc->current_url);
            if (err != SC_OK) {
                *out = sc_tool_result_fail("Chrome/Chromium not available for CDP automation", 48);
                return SC_OK;
            }
            char js[1024];
            snprintf(js, sizeof(js),
                     "var el = document.querySelector('%s'); "
                     "if (el) { el.click(); 'clicked' } else { 'element not found' }",
                     selector);
            char params[1200];
            snprintf(params, sizeof(params), "{\"expression\":\"%s\",\"returnByValue\":true}", js);
            char *result = NULL;
            size_t rlen = 0;
            err = cdp_send_command(bc, alloc, "Runtime.evaluate", params, &result, &rlen);
            if (err != SC_OK) {
                *out = sc_tool_result_fail("CDP click command failed", 24);
                return SC_OK;
            }
            char *msg = sc_strndup(alloc, result, rlen);
            alloc->free(alloc->ctx, result, rlen + 1);
            if (!msg) {
                *out = sc_tool_result_fail("out of memory", 12);
                return SC_ERR_OUT_OF_MEMORY;
            }
            *out = sc_tool_result_ok_owned(msg, rlen);
            return SC_OK;
        }
#endif
    }
    if (strcmp(action, "type") == 0) {
        const char *selector = sc_json_get_string(args, "selector");
        const char *text = sc_json_get_string(args, "text");
        if (!text || text[0] == '\0') {
            *out = sc_tool_result_fail("missing text for type", 21);
            return SC_OK;
        }
#if SC_IS_TEST
        size_t need = 48 + strlen(text) + (selector ? strlen(selector) : 4);
        char *msg = (char *)alloc->alloc(alloc->ctx, need + 1);
        if (!msg) {
            *out = sc_tool_result_fail("out of memory", 12);
            return SC_ERR_OUT_OF_MEMORY;
        }
        int n = snprintf(msg, need + 1, "Typed \"%s\" into %s", text, selector ? selector : "page");
        size_t len = (n > 0 && (size_t)n <= need) ? (size_t)n : need;
        msg[len] = '\0';
        *out = sc_tool_result_ok_owned(msg, len);
        return SC_OK;
#else
        {
            sc_error_t err = cdp_ensure_connected(bc, alloc, bc->current_url);
            if (err != SC_OK) {
                *out = sc_tool_result_fail("Chrome/Chromium not available for CDP automation", 48);
                return SC_OK;
            }
            if (selector && selector[0]) {
                char js[2048];
                snprintf(js, sizeof(js),
                         "var el = document.querySelector('%s'); "
                         "if (el) { el.focus(); el.value = '%s'; "
                         "el.dispatchEvent(new Event('input',{bubbles:true})); 'typed' } "
                         "else { 'element not found' }",
                         selector, text);
                char params[2200];
                snprintf(params, sizeof(params), "{\"expression\":\"%s\",\"returnByValue\":true}",
                         js);
                char *result = NULL;
                size_t rlen = 0;
                err = cdp_send_command(bc, alloc, "Runtime.evaluate", params, &result, &rlen);
                if (err != SC_OK) {
                    *out = sc_tool_result_fail("CDP type command failed", 23);
                    return SC_OK;
                }
                char *msg = sc_strndup(alloc, result, rlen);
                alloc->free(alloc->ctx, result, rlen + 1);
                if (!msg) {
                    *out = sc_tool_result_fail("out of memory", 12);
                    return SC_ERR_OUT_OF_MEMORY;
                }
                *out = sc_tool_result_ok_owned(msg, rlen);
                return SC_OK;
            }
            /* No selector: dispatch key events for each character */
            for (const char *p = text; *p; p++) {
                char params[256];
                snprintf(params, sizeof(params), "{\"type\":\"keyDown\",\"text\":\"%c\"}", *p);
                char *result = NULL;
                size_t rlen = 0;
                cdp_send_command(bc, alloc, "Input.dispatchKeyEvent", params, &result, &rlen);
                if (result)
                    alloc->free(alloc->ctx, result, rlen + 1);
                snprintf(params, sizeof(params), "{\"type\":\"keyUp\",\"text\":\"%c\"}", *p);
                cdp_send_command(bc, alloc, "Input.dispatchKeyEvent", params, &result, &rlen);
                if (result)
                    alloc->free(alloc->ctx, result, rlen + 1);
            }
            char *msg = sc_strndup(alloc, "Typed text via key events", 25);
            if (!msg) {
                *out = sc_tool_result_fail("out of memory", 12);
                return SC_ERR_OUT_OF_MEMORY;
            }
            *out = sc_tool_result_ok_owned(msg, 25);
            return SC_OK;
        }
#endif
    }
    if (strcmp(action, "scroll") == 0) {
        double dx = sc_json_get_number(args, "deltaX", 0);
        double dy = sc_json_get_number(args, "deltaY", 300);
#if SC_IS_TEST
        size_t need = 64;
        char *msg = (char *)alloc->alloc(alloc->ctx, need + 1);
        if (!msg) {
            *out = sc_tool_result_fail("out of memory", 12);
            return SC_ERR_OUT_OF_MEMORY;
        }
        int n = snprintf(msg, need + 1, "Scrolled by (%d,%d)", (int)dx, (int)dy);
        size_t len = (n > 0 && (size_t)n <= need) ? (size_t)n : need;
        msg[len] = '\0';
        *out = sc_tool_result_ok_owned(msg, len);
        return SC_OK;
#else
        {
            sc_error_t err = cdp_ensure_connected(bc, alloc, bc->current_url);
            if (err != SC_OK) {
                *out = sc_tool_result_fail("Chrome/Chromium not available for CDP automation", 48);
                return SC_OK;
            }
            char js[256];
            snprintf(js, sizeof(js), "window.scrollBy(%d,%d); 'scrolled'", (int)dx, (int)dy);
            char params[400];
            snprintf(params, sizeof(params), "{\"expression\":\"%s\",\"returnByValue\":true}", js);
            char *result = NULL;
            size_t rlen = 0;
            err = cdp_send_command(bc, alloc, "Runtime.evaluate", params, &result, &rlen);
            if (err != SC_OK) {
                *out = sc_tool_result_fail("CDP scroll command failed", 25);
                return SC_OK;
            }
            char *msg = sc_strndup(alloc, result, rlen);
            alloc->free(alloc->ctx, result, rlen + 1);
            if (!msg) {
                *out = sc_tool_result_fail("out of memory", 12);
                return SC_ERR_OUT_OF_MEMORY;
            }
            *out = sc_tool_result_ok_owned(msg, rlen);
            return SC_OK;
        }
#endif
    }
    *out = sc_tool_result_fail("unknown action", 14);
    return SC_OK;
}
static const char *browser_name(void *ctx) {
    (void)ctx;
    return "browser";
}
static const char *browser_desc(void *ctx) {
    (void)ctx;
    return "Open a URL in the user's system browser for visual display. NOT for searching or "
           "retrieving content — use web_search or web_fetch instead.";
}
static const char *browser_params(void *ctx) {
    (void)ctx;
    return "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":["
           "\"open\",\"read\",\"screenshot\",\"click\",\"type\",\"scroll\"]},\"url\":{\"type\":"
           "\"string\"},\"selector\":{\"type\":\"string\"},\"text\":{\"type\":\"string\"},"
           "\"deltaX\":{\"type\":\"number\"},\"deltaY\":{\"type\":\"number\"}},\"required\":["
           "\"action\"]}";
}
static void browser_deinit(void *ctx, sc_allocator_t *alloc) {
    sc_browser_ctx_t *bc = (sc_browser_ctx_t *)ctx;
    if (!bc)
        return;
#if !SC_IS_TEST
    if (bc->cdp_ws)
        sc_ws_close(bc->cdp_ws, alloc);
    if (bc->chrome_pid > 0)
        kill(bc->chrome_pid, SIGTERM);
    if (bc->current_url)
        alloc->free(alloc->ctx, bc->current_url, strlen(bc->current_url) + 1);
#else
    (void)alloc;
#endif
    free(bc);
}

static const sc_tool_vtable_t browser_vtable = {
    .execute = browser_execute,
    .name = browser_name,
    .description = browser_desc,
    .parameters_json = browser_params,
    .deinit = browser_deinit,
};

sc_error_t sc_browser_create(sc_allocator_t *alloc, bool enabled, sc_security_policy_t *policy,
                             sc_tool_t *out) {
    (void)enabled;
    sc_browser_ctx_t *c = (sc_browser_ctx_t *)calloc(1, sizeof(*c));
    if (!c)
        return SC_ERR_OUT_OF_MEMORY;
    c->policy = policy;
    c->alloc = alloc;
    out->ctx = c;
    out->vtable = &browser_vtable;
    return SC_OK;
}
