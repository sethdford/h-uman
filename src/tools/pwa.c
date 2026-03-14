/*
 * PWA Tool — agent-facing tool for driving installed Progressive Web Apps.
 *
 * Actions:
 *   list_apps    — show available PWA drivers
 *   list_tabs    — discover open PWA tabs in the browser
 *   read         — read messages/content from a PWA
 *   send         — send a message via a PWA
 *   exec_js      — execute raw JavaScript in a PWA tab (advanced)
 *   navigate     — navigate to a channel/contact within a PWA
 */
#include "human/tools/pwa.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/pwa.h"
#include <stdio.h>
#include <string.h>

#define HU_PWA_TOOL_NAME "pwa"
#define HU_PWA_TOOL_DESC                                                                          \
    "Drive installed Progressive Web Apps (Slack, Discord, WhatsApp, Gmail, Calendar, Notion, "   \
    "Twitter, Telegram, LinkedIn, Facebook) through browser automation. No API keys needed — "    \
    "uses your existing browser sessions. Actions: list_apps, list_tabs, read, send, navigate, "  \
    "exec_js."

#define HU_PWA_TOOL_PARAMS                                                                        \
    "{\"type\":\"object\",\"properties\":{"                                                       \
    "\"action\":{\"type\":\"string\",\"enum\":[\"list_apps\",\"list_tabs\",\"read\",\"send\","   \
    "\"navigate\",\"exec_js\"]},"                                                                 \
    "\"app\":{\"type\":\"string\",\"description\":\"App name (slack, discord, whatsapp, gmail, " \
    "calendar, notion, twitter, telegram, linkedin, facebook)\"},"                                          \
    "\"target\":{\"type\":\"string\",\"description\":\"Channel, contact, or search query\"},"    \
    "\"message\":{\"type\":\"string\",\"description\":\"Message to send\"},"                     \
    "\"javascript\":{\"type\":\"string\",\"description\":\"Raw JS to execute (exec_js only)\"}," \
    "\"url_pattern\":{\"type\":\"string\",\"description\":\"URL substring to match tabs\"}"      \
    "},\"required\":[\"action\"]}"

typedef struct hu_pwa_tool_ctx {
    hu_pwa_browser_t browser;
    bool browser_detected;
} hu_pwa_tool_ctx_t;

static hu_error_t ensure_browser(hu_pwa_tool_ctx_t *ctx) {
    if (ctx->browser_detected)
        return HU_OK;
    hu_error_t err = hu_pwa_detect_browser(&ctx->browser);
    if (err == HU_OK)
        ctx->browser_detected = true;
    return err;
}

static hu_error_t action_list_apps(hu_allocator_t *alloc, hu_tool_result_t *out) {
    size_t count = 0;
    const hu_pwa_driver_t *first = hu_pwa_drivers_all(&count);
    if (!first || count == 0) {
        *out = hu_tool_result_ok("No PWA drivers available", 24);
        return HU_OK;
    }

    /* Build a formatted list. Overestimate size. */
    size_t buf_size = count * 128;
    char *buf = (char *)alloc->alloc(alloc->ctx, buf_size);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    size_t pos = 0;
    int n = snprintf(buf, buf_size, "Available PWA drivers (%zu apps):\n", count);
    if (n > 0)
        pos += (size_t)n;

    /* The drivers array is contiguous but accessed via pointer array.
     * We iterate using hu_pwa_driver_find by index. */
    const char *apps[] = {"slack", "discord", "whatsapp", "gmail",     "calendar",
                          "notion", "twitter", "telegram", "linkedin", "facebook"};
    size_t napps = sizeof(apps) / sizeof(apps[0]);
    for (size_t i = 0; i < napps && pos < buf_size - 128; i++) {
        const hu_pwa_driver_t *d = hu_pwa_driver_resolve(apps[i]);
        if (!d)
            continue;
        n = snprintf(buf + pos, buf_size - pos,
                     "  - %s (%s) [%s] %s%s\n",
                     d->app_name, d->display_name, d->url_pattern,
                     d->send_message_js ? "send " : "",
                     d->read_messages_js ? "read" : "");
        if (n > 0)
            pos += (size_t)n;
    }
    buf[pos] = '\0';
    *out = hu_tool_result_ok_owned(buf, pos);
    return HU_OK;
}

static hu_error_t action_list_tabs(hu_allocator_t *alloc, hu_pwa_tool_ctx_t *ctx,
                                   const char *url_pattern, hu_tool_result_t *out) {
    hu_error_t err = ensure_browser(ctx);
    if (err != HU_OK) {
        *out = hu_tool_result_fail("No supported browser found", 26);
        return HU_OK;
    }

    hu_pwa_tab_t *tabs = NULL;
    size_t count = 0;
    err = hu_pwa_list_tabs(alloc, ctx->browser, url_pattern, &tabs, &count);
    if (err != HU_OK) {
        *out = hu_tool_result_fail("Failed to list tabs", 19);
        return HU_OK;
    }

    if (count == 0) {
        *out = hu_tool_result_ok("No matching tabs found", 22);
        return HU_OK;
    }

    size_t buf_size = count * 256;
    char *buf = (char *)alloc->alloc(alloc->ctx, buf_size);
    if (!buf) {
        hu_pwa_tabs_free(alloc, tabs, count);
        return HU_ERR_OUT_OF_MEMORY;
    }

    size_t pos = 0;
    int n = snprintf(buf, buf_size, "Open tabs (%zu found in %s):\n",
                     count, hu_pwa_browser_name(ctx->browser));
    if (n > 0)
        pos += (size_t)n;

    for (size_t i = 0; i < count && pos < buf_size - 128; i++) {
        const hu_pwa_driver_t *drv = tabs[i].url ? hu_pwa_driver_find_by_url(tabs[i].url) : NULL;
        n = snprintf(buf + pos, buf_size - pos, "  [%d:%d] %s — %s%s\n",
                     tabs[i].window_idx, tabs[i].tab_idx,
                     tabs[i].title ? tabs[i].title : "?",
                     tabs[i].url ? tabs[i].url : "?",
                     drv ? " [PWA]" : "");
        if (n > 0)
            pos += (size_t)n;
    }
    buf[pos] = '\0';

    hu_pwa_tabs_free(alloc, tabs, count);
    *out = hu_tool_result_ok_owned(buf, pos);
    return HU_OK;
}

static hu_error_t action_read(hu_allocator_t *alloc, hu_pwa_tool_ctx_t *ctx,
                              const char *app, hu_tool_result_t *out) {
    if (!app || !app[0]) {
        *out = hu_tool_result_fail("'app' is required for read action", 33);
        return HU_OK;
    }
    hu_error_t err = ensure_browser(ctx);
    if (err != HU_OK) {
        *out = hu_tool_result_fail("No supported browser found", 26);
        return HU_OK;
    }

    char *result = NULL;
    size_t result_len = 0;
    err = hu_pwa_read_messages(alloc, ctx->browser, app, &result, &result_len);
    if (err == HU_ERR_NOT_FOUND) {
        char *msg = hu_sprintf(alloc, "App '%s' not found. Use list_apps to see available apps.", app);
        if (msg) {
            *out = hu_tool_result_fail_owned(msg, strlen(msg));
        } else {
            *out = hu_tool_result_fail("App not found", 13);
        }
        return HU_OK;
    }
    if (err != HU_OK) {
        char *msg = hu_sprintf(alloc, "Could not read from %s — is the tab open?", app);
        if (msg) {
            *out = hu_tool_result_fail_owned(msg, strlen(msg));
        } else {
            *out = hu_tool_result_fail("Read failed", 11);
        }
        return HU_OK;
    }

    *out = hu_tool_result_ok_owned(result, result_len);
    return HU_OK;
}

static hu_error_t action_send(hu_allocator_t *alloc, hu_pwa_tool_ctx_t *ctx,
                              const char *app, const char *target, const char *message,
                              hu_tool_result_t *out) {
    if (!app || !app[0]) {
        *out = hu_tool_result_fail("'app' is required for send action", 33);
        return HU_OK;
    }
    if (!message || !message[0]) {
        *out = hu_tool_result_fail("'message' is required for send action", 37);
        return HU_OK;
    }
    hu_error_t err = ensure_browser(ctx);
    if (err != HU_OK) {
        *out = hu_tool_result_fail("No supported browser found", 26);
        return HU_OK;
    }

    char *result = NULL;
    size_t result_len = 0;
    err = hu_pwa_send_message(alloc, ctx->browser, app, target, message, &result, &result_len);
    if (err == HU_ERR_NOT_FOUND) {
        char *msg = hu_sprintf(alloc, "App '%s' not found or tab not open.", app);
        if (msg) {
            *out = hu_tool_result_fail_owned(msg, strlen(msg));
        } else {
            *out = hu_tool_result_fail("App not found", 13);
        }
        return HU_OK;
    }
    if (err != HU_OK) {
        char *msg = hu_sprintf(alloc, "Send to %s failed — is the tab open and focused?", app);
        if (msg) {
            *out = hu_tool_result_fail_owned(msg, strlen(msg));
        } else {
            *out = hu_tool_result_fail("Send failed", 11);
        }
        return HU_OK;
    }

    *out = hu_tool_result_ok_owned(result, result_len);
    return HU_OK;
}

static hu_error_t action_navigate(hu_allocator_t *alloc, hu_pwa_tool_ctx_t *ctx,
                                  const char *app, const char *target, hu_tool_result_t *out) {
    if (!app || !app[0]) {
        *out = hu_tool_result_fail("'app' is required for navigate", 30);
        return HU_OK;
    }
    if (!target || !target[0]) {
        *out = hu_tool_result_fail("'target' is required for navigate", 33);
        return HU_OK;
    }
    hu_error_t err = ensure_browser(ctx);
    if (err != HU_OK) {
        *out = hu_tool_result_fail("No supported browser found", 26);
        return HU_OK;
    }

    const hu_pwa_driver_t *drv = hu_pwa_driver_resolve(app);
    if (!drv) {
        *out = hu_tool_result_fail("Unknown app", 11);
        return HU_OK;
    }
    if (!drv->navigate_js) {
        *out = hu_tool_result_fail("Navigation not supported for this app", 37);
        return HU_OK;
    }

    hu_pwa_tab_t tab;
    err = hu_pwa_find_tab(alloc, ctx->browser, drv->url_pattern, &tab);
    if (err != HU_OK) {
        char *msg = hu_sprintf(alloc, "%s tab not found — is it open?", drv->display_name);
        if (msg) {
            *out = hu_tool_result_fail_owned(msg, strlen(msg));
        } else {
            *out = hu_tool_result_fail("Tab not found", 13);
        }
        return HU_OK;
    }

    char *escaped = NULL;
    size_t elen = 0;
    err = hu_pwa_escape_js_string(alloc, target, strlen(target), &escaped, &elen);
    if (err != HU_OK) {
        hu_pwa_tab_free(alloc, &tab);
        return err;
    }

    char *js = hu_sprintf(alloc, drv->navigate_js, escaped);
    alloc->free(alloc->ctx, escaped, elen + 1);
    if (!js) {
        hu_pwa_tab_free(alloc, &tab);
        return HU_ERR_OUT_OF_MEMORY;
    }

    char *result = NULL;
    size_t rlen = 0;
    err = hu_pwa_exec_js(alloc, &tab, js, &result, &rlen);
    alloc->free(alloc->ctx, js, strlen(js) + 1);
    hu_pwa_tab_free(alloc, &tab);

    if (err != HU_OK) {
        *out = hu_tool_result_fail("Navigation failed", 17);
        return HU_OK;
    }
    *out = hu_tool_result_ok_owned(result, rlen);
    return HU_OK;
}

static hu_error_t action_exec_js(hu_allocator_t *alloc, hu_pwa_tool_ctx_t *ctx,
                                 const char *app, const char *javascript, hu_tool_result_t *out) {
    if (!javascript || !javascript[0]) {
        *out = hu_tool_result_fail("'javascript' is required for exec_js", 36);
        return HU_OK;
    }
    hu_error_t err = ensure_browser(ctx);
    if (err != HU_OK) {
        *out = hu_tool_result_fail("No supported browser found", 26);
        return HU_OK;
    }

    const char *url_pattern = app;
    if (app) {
        const hu_pwa_driver_t *drv = hu_pwa_driver_resolve(app);
        if (drv)
            url_pattern = drv->url_pattern;
    }
    if (!url_pattern || !url_pattern[0])
        url_pattern = "";

    hu_pwa_tab_t tab;
    err = hu_pwa_find_tab(alloc, ctx->browser, url_pattern, &tab);
    if (err != HU_OK) {
        *out = hu_tool_result_fail("No matching tab found", 21);
        return HU_OK;
    }

    char *result = NULL;
    size_t rlen = 0;
    err = hu_pwa_exec_js(alloc, &tab, javascript, &result, &rlen);
    hu_pwa_tab_free(alloc, &tab);

    if (err != HU_OK) {
        *out = hu_tool_result_fail("JS execution failed", 19);
        return HU_OK;
    }
    *out = hu_tool_result_ok_owned(result, rlen);
    return HU_OK;
}

/* ── Tool Vtable ───────────────────────────────────────────────────── */

static hu_error_t pwa_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                              hu_tool_result_t *out) {
    if (!args || !out) {
        *out = hu_tool_result_fail("invalid args", 12);
        return HU_ERR_INVALID_ARGUMENT;
    }
    hu_pwa_tool_ctx_t *pctx = (hu_pwa_tool_ctx_t *)ctx;

    const char *action = hu_json_get_string(args, "action");
    if (!action || !action[0]) {
        *out = hu_tool_result_fail("'action' is required", 20);
        return HU_OK;
    }

    const char *app = hu_json_get_string(args, "app");
    const char *target = hu_json_get_string(args, "target");
    const char *message = hu_json_get_string(args, "message");
    const char *javascript = hu_json_get_string(args, "javascript");
    const char *url_pattern = hu_json_get_string(args, "url_pattern");

    if (strcmp(action, "list_apps") == 0)
        return action_list_apps(alloc, out);
    if (strcmp(action, "list_tabs") == 0)
        return action_list_tabs(alloc, pctx, url_pattern, out);
    if (strcmp(action, "read") == 0)
        return action_read(alloc, pctx, app, out);
    if (strcmp(action, "send") == 0)
        return action_send(alloc, pctx, app, target, message, out);
    if (strcmp(action, "navigate") == 0)
        return action_navigate(alloc, pctx, app, target, out);
    if (strcmp(action, "exec_js") == 0)
        return action_exec_js(alloc, pctx, app, javascript, out);

    char *msg = hu_sprintf(alloc, "Unknown action '%s'. Use: list_apps, list_tabs, read, send, navigate, exec_js", action);
    if (msg) {
        *out = hu_tool_result_fail_owned(msg, strlen(msg));
    } else {
        *out = hu_tool_result_fail("Unknown action", 14);
    }
    return HU_OK;
}

static const char *pwa_name(void *ctx) {
    (void)ctx;
    return HU_PWA_TOOL_NAME;
}
static const char *pwa_description(void *ctx) {
    (void)ctx;
    return HU_PWA_TOOL_DESC;
}
static const char *pwa_parameters_json(void *ctx) {
    (void)ctx;
    return HU_PWA_TOOL_PARAMS;
}
static void pwa_deinit(void *ctx, hu_allocator_t *alloc) {
    if (ctx)
        alloc->free(alloc->ctx, ctx, sizeof(hu_pwa_tool_ctx_t));
}

static const hu_tool_vtable_t pwa_vtable = {
    .execute = pwa_execute,
    .name = pwa_name,
    .description = pwa_description,
    .parameters_json = pwa_parameters_json,
    .deinit = pwa_deinit,
};

hu_error_t hu_pwa_tool_create(hu_allocator_t *alloc, hu_tool_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;

    hu_pwa_tool_ctx_t *c = (hu_pwa_tool_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));

    out->ctx = c;
    out->vtable = &pwa_vtable;
    return HU_OK;
}
