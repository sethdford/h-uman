/*
 * PWA Bridge — AppleScript-based browser automation for macOS.
 * Drives Chrome, Arc, Brave, or Edge tabs via `osascript` + `execute javascript`.
 */
#include "human/pwa.h"
#include "human/core/process_util.h"
#include "human/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !HU_IS_TEST && defined(__APPLE__)
#include <unistd.h>
#endif

/* ── Browser Names & Detection ─────────────────────────────────────── */

static const char *const BROWSER_NAMES[] = {
    [HU_PWA_BROWSER_CHROME] = "Google Chrome",
    [HU_PWA_BROWSER_ARC] = "Arc",
    [HU_PWA_BROWSER_BRAVE] = "Brave Browser",
    [HU_PWA_BROWSER_EDGE] = "Microsoft Edge",
};

#if !HU_IS_TEST && defined(__APPLE__)
static const char *const BROWSER_PATHS[] = {
    "/Applications/Google Chrome.app",
    "/Applications/Arc.app",
    "/Applications/Brave Browser.app",
    "/Applications/Microsoft Edge.app",
};
#endif

const char *hu_pwa_browser_name(hu_pwa_browser_t browser) {
    if (browser >= HU_PWA_BROWSER_COUNT)
        return "unknown";
    return BROWSER_NAMES[browser];
}

hu_error_t hu_pwa_detect_browser(hu_pwa_browser_t *out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
#if HU_IS_TEST
    *out = HU_PWA_BROWSER_CHROME;
    return HU_OK;
#elif !defined(__APPLE__)
    return HU_ERR_NOT_SUPPORTED;
#else
    for (int i = 0; i < HU_PWA_BROWSER_COUNT; i++) {
        if (access(BROWSER_PATHS[i], F_OK) == 0) {
            *out = (hu_pwa_browser_t)i;
            return HU_OK;
        }
    }
    return HU_ERR_NOT_FOUND;
#endif
}

/* ── String Escaping ───────────────────────────────────────────────── */

hu_error_t hu_pwa_escape_js_string(hu_allocator_t *alloc, const char *input, size_t input_len,
                                   char **out, size_t *out_len) {
    if (!alloc || !input || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    size_t need = 0;
    for (size_t i = 0; i < input_len; i++) {
        char c = input[i];
        if (c == '\\' || c == '\'' || c == '"' || c == '\n' || c == '\r' || c == '\t')
            need += 2;
        else
            need += 1;
    }

    char *buf = (char *)alloc->alloc(alloc->ctx, need + 1);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    size_t pos = 0;
    for (size_t i = 0; i < input_len; i++) {
        char c = input[i];
        switch (c) {
        case '\\':
            buf[pos++] = '\\';
            buf[pos++] = '\\';
            break;
        case '\'':
            buf[pos++] = '\\';
            buf[pos++] = '\'';
            break;
        case '"':
            buf[pos++] = '\\';
            buf[pos++] = '"';
            break;
        case '\n':
            buf[pos++] = '\\';
            buf[pos++] = 'n';
            break;
        case '\r':
            buf[pos++] = '\\';
            buf[pos++] = 'r';
            break;
        case '\t':
            buf[pos++] = '\\';
            buf[pos++] = 't';
            break;
        default:
            buf[pos++] = c;
            break;
        }
    }
    buf[pos] = '\0';
    *out = buf;
    *out_len = pos;
    return HU_OK;
}

hu_error_t hu_pwa_escape_applescript(hu_allocator_t *alloc, const char *input, size_t input_len,
                                     char **out, size_t *out_len) {
    if (!alloc || !input || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    size_t need = 0;
    for (size_t i = 0; i < input_len; i++) {
        if (input[i] == '"' || input[i] == '\\')
            need += 2;
        else
            need += 1;
    }

    char *buf = (char *)alloc->alloc(alloc->ctx, need + 1);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    size_t pos = 0;
    for (size_t i = 0; i < input_len; i++) {
        if (input[i] == '"' || input[i] == '\\')
            buf[pos++] = '\\';
        buf[pos++] = input[i];
    }
    buf[pos] = '\0';
    *out = buf;
    *out_len = pos;
    return HU_OK;
}

/* ── Tab Management ────────────────────────────────────────────────── */

void hu_pwa_tab_free(hu_allocator_t *alloc, hu_pwa_tab_t *tab) {
    if (!tab)
        return;
    if (tab->url)
        alloc->free(alloc->ctx, tab->url, strlen(tab->url) + 1);
    if (tab->title)
        alloc->free(alloc->ctx, tab->title, strlen(tab->title) + 1);
    tab->url = NULL;
    tab->title = NULL;
}

void hu_pwa_tabs_free(hu_allocator_t *alloc, hu_pwa_tab_t *tabs, size_t count) {
    if (!tabs)
        return;
    for (size_t i = 0; i < count; i++)
        hu_pwa_tab_free(alloc, &tabs[i]);
    alloc->free(alloc->ctx, tabs, count * sizeof(hu_pwa_tab_t));
}

/* ── AppleScript Execution Helpers ─────────────────────────────────── */

#if HU_IS_TEST

hu_error_t hu_pwa_find_tab(hu_allocator_t *alloc, hu_pwa_browser_t browser,
                           const char *url_pattern, hu_pwa_tab_t *out) {
    if (!alloc || !url_pattern || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->browser = browser;
    out->window_idx = 1;
    out->tab_idx = 1;
    size_t ulen = 16 + strlen(url_pattern);
    out->url = (char *)alloc->alloc(alloc->ctx, ulen + 1);
    if (!out->url)
        return HU_ERR_OUT_OF_MEMORY;
    int n = snprintf(out->url, ulen + 1, "https://%s/test", url_pattern);
    if (n > 0)
        out->url[n < (int)ulen ? n : (int)ulen] = '\0';
    out->title = hu_strdup(alloc, "Test PWA Tab");
    return HU_OK;
}

hu_error_t hu_pwa_list_tabs(hu_allocator_t *alloc, hu_pwa_browser_t browser,
                            const char *url_pattern, hu_pwa_tab_t **out, size_t *count) {
    if (!alloc || !out || !count)
        return HU_ERR_INVALID_ARGUMENT;
    *count = 1;
    *out = (hu_pwa_tab_t *)alloc->alloc(alloc->ctx, sizeof(hu_pwa_tab_t));
    if (!*out)
        return HU_ERR_OUT_OF_MEMORY;
    return hu_pwa_find_tab(alloc, browser, url_pattern ? url_pattern : "test.app", &(*out)[0]);
}

hu_error_t hu_pwa_exec_js(hu_allocator_t *alloc, const hu_pwa_tab_t *tab,
                          const char *javascript, char **out_result, size_t *out_len) {
    if (!alloc || !tab || !javascript || !out_result || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    char *result = hu_sprintf(alloc, "[test] executed JS in %s tab %d: %.64s",
                              hu_pwa_browser_name(tab->browser), tab->tab_idx, javascript);
    if (!result)
        return HU_ERR_OUT_OF_MEMORY;
    *out_result = result;
    *out_len = strlen(result);
    return HU_OK;
}

hu_error_t hu_pwa_activate_tab(hu_allocator_t *alloc, const hu_pwa_tab_t *tab) {
    (void)alloc;
    (void)tab;
    return HU_OK;
}

#elif !defined(__APPLE__)

hu_error_t hu_pwa_find_tab(hu_allocator_t *alloc, hu_pwa_browser_t browser,
                           const char *url_pattern, hu_pwa_tab_t *out) {
    (void)alloc;
    (void)browser;
    (void)url_pattern;
    (void)out;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_pwa_list_tabs(hu_allocator_t *alloc, hu_pwa_browser_t browser,
                            const char *url_pattern, hu_pwa_tab_t **out, size_t *count) {
    (void)alloc;
    (void)browser;
    (void)url_pattern;
    (void)out;
    (void)count;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_pwa_exec_js(hu_allocator_t *alloc, const hu_pwa_tab_t *tab,
                          const char *javascript, char **out_result, size_t *out_len) {
    (void)alloc;
    (void)tab;
    (void)javascript;
    (void)out_result;
    (void)out_len;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_pwa_activate_tab(hu_allocator_t *alloc, const hu_pwa_tab_t *tab) {
    (void)alloc;
    (void)tab;
    return HU_ERR_NOT_SUPPORTED;
}

#else /* macOS, non-test */

static hu_error_t run_applescript(hu_allocator_t *alloc, const char *script,
                                  char **out, size_t *out_len) {
    const char *argv[] = {"osascript", "-e", script, NULL};
    hu_run_result_t result = {0};
    hu_error_t err = hu_process_run(alloc, argv, NULL, 65536, &result);
    if (err != HU_OK) {
        hu_run_result_free(alloc, &result);
        return err;
    }
    if (!result.success) {
        hu_run_result_free(alloc, &result);
        return HU_ERR_IO;
    }

    size_t len = result.stdout_len;
    while (len > 0 && (result.stdout_buf[len - 1] == '\n' || result.stdout_buf[len - 1] == '\r'))
        len--;

    char *buf = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (!buf) {
        hu_run_result_free(alloc, &result);
        return HU_ERR_OUT_OF_MEMORY;
    }
    if (len > 0)
        memcpy(buf, result.stdout_buf, len);
    buf[len] = '\0';
    hu_run_result_free(alloc, &result);

    *out = buf;
    *out_len = len;
    return HU_OK;
}

hu_error_t hu_pwa_find_tab(hu_allocator_t *alloc, hu_pwa_browser_t browser,
                           const char *url_pattern, hu_pwa_tab_t *out) {
    if (!alloc || !url_pattern || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    const char *app = hu_pwa_browser_name(browser);

    char *escaped_pattern = NULL;
    size_t escaped_len = 0;
    hu_error_t esc_err = hu_pwa_escape_applescript(alloc, url_pattern, strlen(url_pattern),
                                                    &escaped_pattern, &escaped_len);
    if (esc_err != HU_OK)
        return esc_err;

    char script[2048];
    int n = snprintf(script, sizeof(script),
        "tell application \"%s\"\n"
        "  repeat with w from 1 to count of windows\n"
        "    repeat with t from 1 to count of tabs of window w\n"
        "      if URL of tab t of window w contains \"%s\" then\n"
        "        return (w as text) & \"|\" & (t as text) & \"|\" & URL of tab t of window w & \"|\" & title of tab t of window w\n"
        "      end if\n"
        "    end repeat\n"
        "  end repeat\n"
        "  return \"NOT_FOUND\"\n"
        "end tell",
        app, escaped_pattern);
    alloc->free(alloc->ctx, escaped_pattern, escaped_len + 1);
    if (n <= 0 || (size_t)n >= sizeof(script))
        return HU_ERR_PARSE;

    char *result = NULL;
    size_t result_len = 0;
    hu_error_t err = run_applescript(alloc, script, &result, &result_len);
    if (err != HU_OK)
        return err;

    if (strncmp(result, "NOT_FOUND", 9) == 0) {
        alloc->free(alloc->ctx, result, result_len + 1);
        return HU_ERR_NOT_FOUND;
    }

    /* Parse: "win|tab|url|title" */
    out->browser = browser;
    char *p = result;
    char *sep1 = strchr(p, '|');
    if (!sep1) {
        alloc->free(alloc->ctx, result, result_len + 1);
        return HU_ERR_PARSE;
    }
    *sep1 = '\0';
    out->window_idx = atoi(p);
    p = sep1 + 1;

    char *sep2 = strchr(p, '|');
    if (!sep2) {
        alloc->free(alloc->ctx, result, result_len + 1);
        return HU_ERR_PARSE;
    }
    *sep2 = '\0';
    out->tab_idx = atoi(p);
    p = sep2 + 1;

    char *sep3 = strchr(p, '|');
    if (!sep3) {
        alloc->free(alloc->ctx, result, result_len + 1);
        return HU_ERR_PARSE;
    }
    *sep3 = '\0';
    out->url = hu_strdup(alloc, p);
    out->title = hu_strdup(alloc, sep3 + 1);

    alloc->free(alloc->ctx, result, result_len + 1);
    return HU_OK;
}

hu_error_t hu_pwa_list_tabs(hu_allocator_t *alloc, hu_pwa_browser_t browser,
                            const char *url_pattern, hu_pwa_tab_t **out, size_t *count) {
    if (!alloc || !out || !count)
        return HU_ERR_INVALID_ARGUMENT;

    const char *app = hu_pwa_browser_name(browser);
    const char *filter = url_pattern ? url_pattern : "";

    char *escaped_filter = NULL;
    size_t escaped_filter_len = 0;
    hu_error_t esc_err = hu_pwa_escape_applescript(alloc, filter, strlen(filter),
                                                    &escaped_filter, &escaped_filter_len);
    if (esc_err != HU_OK)
        return esc_err;

    char script[2048];
    int n = snprintf(script, sizeof(script),
        "set output to \"\"\n"
        "tell application \"%s\"\n"
        "  repeat with w from 1 to count of windows\n"
        "    repeat with t from 1 to count of tabs of window w\n"
        "      set u to URL of tab t of window w\n"
        "      if u contains \"%s\" or \"%s\" = \"\" then\n"
        "        set output to output & (w as text) & \"|\" & (t as text) & \"|\" & u & \"|\" & title of tab t of window w & \"\\n\"\n"
        "      end if\n"
        "    end repeat\n"
        "  end repeat\n"
        "end tell\n"
        "return output",
        app, escaped_filter, escaped_filter);
    alloc->free(alloc->ctx, escaped_filter, escaped_filter_len + 1);
    if (n <= 0 || (size_t)n >= sizeof(script))
        return HU_ERR_PARSE;

    char *result = NULL;
    size_t result_len = 0;
    hu_error_t err = run_applescript(alloc, script, &result, &result_len);
    if (err != HU_OK)
        return err;

    /* Count lines */
    size_t line_count = 0;
    for (size_t i = 0; i < result_len; i++) {
        if (result[i] == '\n')
            line_count++;
    }
    if (result_len > 0 && result[result_len - 1] != '\n')
        line_count++;
    if (line_count == 0 || (result_len == 0)) {
        alloc->free(alloc->ctx, result, result_len + 1);
        *out = NULL;
        *count = 0;
        return HU_OK;
    }

    hu_pwa_tab_t *tabs = (hu_pwa_tab_t *)alloc->alloc(alloc->ctx, line_count * sizeof(hu_pwa_tab_t));
    if (!tabs) {
        alloc->free(alloc->ctx, result, result_len + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(tabs, 0, line_count * sizeof(hu_pwa_tab_t));

    size_t idx = 0;
    char *line = result;
    for (char *p = result; p <= result + result_len; p++) {
        if (*p == '\n' || *p == '\0' || p == result + result_len) {
            *p = '\0';
            if (p > line && idx < line_count) {
                char *s1 = strchr(line, '|');
                if (s1) {
                    *s1 = '\0';
                    char *s2 = strchr(s1 + 1, '|');
                    if (s2) {
                        *s2 = '\0';
                        char *s3 = strchr(s2 + 1, '|');
                        if (s3) {
                            *s3 = '\0';
                            tabs[idx].browser = browser;
                            tabs[idx].window_idx = atoi(line);
                            tabs[idx].tab_idx = atoi(s1 + 1);
                            tabs[idx].url = hu_strdup(alloc, s2 + 1);
                            tabs[idx].title = hu_strdup(alloc, s3 + 1);
                            idx++;
                        }
                    }
                }
            }
            line = p + 1;
        }
    }

    alloc->free(alloc->ctx, result, result_len + 1);
    *out = tabs;
    *count = idx;
    return HU_OK;
}

hu_error_t hu_pwa_exec_js(hu_allocator_t *alloc, const hu_pwa_tab_t *tab,
                          const char *javascript, char **out_result, size_t *out_len) {
    if (!alloc || !tab || !javascript || !out_result || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    const char *app = hu_pwa_browser_name(tab->browser);

    char *escaped_js = NULL;
    size_t ejs_len = 0;
    hu_error_t err = hu_pwa_escape_applescript(alloc, javascript, strlen(javascript),
                                               &escaped_js, &ejs_len);
    if (err != HU_OK)
        return err;

    size_t script_size = 256 + strlen(app) + ejs_len;
    char *script = (char *)alloc->alloc(alloc->ctx, script_size);
    if (!script) {
        alloc->free(alloc->ctx, escaped_js, ejs_len + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }

    int n = snprintf(script, script_size,
        "tell application \"%s\"\n"
        "  tell tab %d of window %d\n"
        "    execute javascript \"%s\"\n"
        "  end tell\n"
        "end tell",
        app, tab->tab_idx, tab->window_idx, escaped_js);
    alloc->free(alloc->ctx, escaped_js, ejs_len + 1);

    if (n <= 0 || (size_t)n >= script_size) {
        alloc->free(alloc->ctx, script, script_size);
        return HU_ERR_PARSE;
    }

    err = run_applescript(alloc, script, out_result, out_len);
    alloc->free(alloc->ctx, script, script_size);
    return err;
}

hu_error_t hu_pwa_activate_tab(hu_allocator_t *alloc, const hu_pwa_tab_t *tab) {
    if (!alloc || !tab)
        return HU_ERR_INVALID_ARGUMENT;

    const char *app = hu_pwa_browser_name(tab->browser);
    char script[512];
    int n = snprintf(script, sizeof(script),
        "tell application \"%s\"\n"
        "  activate\n"
        "  set active tab index of window %d to %d\n"
        "end tell",
        app, tab->window_idx, tab->tab_idx);
    if (n <= 0 || (size_t)n >= sizeof(script))
        return HU_ERR_PARSE;

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = run_applescript(alloc, script, &out, &out_len);
    if (out)
        alloc->free(alloc->ctx, out, out_len + 1);
    return err;
}

#endif /* __APPLE__ && !HU_IS_TEST */

/* ── High-Level Actions ────────────────────────────────────────────── */

hu_error_t hu_pwa_send_message(hu_allocator_t *alloc, hu_pwa_browser_t browser,
                               const char *app_name, const char *target,
                               const char *message, char **out_result, size_t *out_len) {
    if (!alloc || !app_name || !message || !out_result || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    const hu_pwa_driver_t *drv = hu_pwa_driver_resolve(app_name);
    if (!drv)
        return HU_ERR_NOT_FOUND;
    if (!drv->send_message_js)
        return HU_ERR_NOT_SUPPORTED;

    hu_pwa_tab_t tab;
    hu_error_t err = hu_pwa_find_tab(alloc, browser, drv->url_pattern, &tab);
    if (err != HU_OK)
        return err;

    /* Navigate to target if specified */
    if (target && target[0] && drv->navigate_js) {
        char *escaped_target = NULL;
        size_t et_len = 0;
        err = hu_pwa_escape_js_string(alloc, target, strlen(target), &escaped_target, &et_len);
        if (err == HU_OK) {
            char *nav_js = hu_sprintf(alloc, drv->navigate_js, escaped_target);
            alloc->free(alloc->ctx, escaped_target, et_len + 1);
            if (nav_js) {
                char *nav_result = NULL;
                size_t nav_len = 0;
                hu_pwa_exec_js(alloc, &tab, nav_js, &nav_result, &nav_len);
                if (nav_result)
                    alloc->free(alloc->ctx, nav_result, nav_len + 1);
                alloc->free(alloc->ctx, nav_js, strlen(nav_js) + 1);
            }
        }
    }

    char *escaped_msg = NULL;
    size_t em_len = 0;
    err = hu_pwa_escape_js_string(alloc, message, strlen(message), &escaped_msg, &em_len);
    if (err != HU_OK) {
        hu_pwa_tab_free(alloc, &tab);
        return err;
    }

    char *send_js = hu_sprintf(alloc, drv->send_message_js, escaped_msg);
    alloc->free(alloc->ctx, escaped_msg, em_len + 1);
    if (!send_js) {
        hu_pwa_tab_free(alloc, &tab);
        return HU_ERR_OUT_OF_MEMORY;
    }

    err = hu_pwa_exec_js(alloc, &tab, send_js, out_result, out_len);
    alloc->free(alloc->ctx, send_js, strlen(send_js) + 1);
    hu_pwa_tab_free(alloc, &tab);
    return err;
}

hu_error_t hu_pwa_read_messages(hu_allocator_t *alloc, hu_pwa_browser_t browser,
                                const char *app_name, char **out_result, size_t *out_len) {
    if (!alloc || !app_name || !out_result || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    const hu_pwa_driver_t *drv = hu_pwa_driver_resolve(app_name);
    if (!drv)
        return HU_ERR_NOT_FOUND;
    if (!drv->read_messages_js)
        return HU_ERR_NOT_SUPPORTED;

    hu_pwa_tab_t tab;
    hu_error_t err = hu_pwa_find_tab(alloc, browser, drv->url_pattern, &tab);
    if (err != HU_OK)
        return err;

    err = hu_pwa_exec_js(alloc, &tab, drv->read_messages_js, out_result, out_len);
    hu_pwa_tab_free(alloc, &tab);
    return err;
}
