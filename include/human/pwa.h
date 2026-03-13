#ifndef HU_PWA_H
#define HU_PWA_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>

/*
 * PWA Bridge — drive installed Progressive Web Apps via AppleScript + browser.
 * Supports Chrome, Arc, and Brave on macOS. No API keys, no extra deps.
 *
 * Architecture:
 *   C code → hu_process_run("osascript", "-e", script) → Browser tab
 *   Browser tab → execute javascript → DOM interaction → result string
 *
 * Each supported app has a hu_pwa_driver_t that defines selectors and
 * JavaScript snippets for common actions (send message, read messages, etc.).
 */

/* ── Browser Detection ─────────────────────────────────────────────── */

typedef enum hu_pwa_browser {
    HU_PWA_BROWSER_CHROME = 0,
    HU_PWA_BROWSER_ARC,
    HU_PWA_BROWSER_BRAVE,
    HU_PWA_BROWSER_EDGE,
    HU_PWA_BROWSER_COUNT,
} hu_pwa_browser_t;

const char *hu_pwa_browser_name(hu_pwa_browser_t browser);

hu_error_t hu_pwa_detect_browser(hu_pwa_browser_t *out);

/* ── Tab Discovery ─────────────────────────────────────────────────── */

typedef struct hu_pwa_tab {
    int window_idx;
    int tab_idx;
    char *url;
    char *title;
    hu_pwa_browser_t browser;
} hu_pwa_tab_t;

void hu_pwa_tab_free(hu_allocator_t *alloc, hu_pwa_tab_t *tab);
void hu_pwa_tabs_free(hu_allocator_t *alloc, hu_pwa_tab_t *tabs, size_t count);

hu_error_t hu_pwa_find_tab(hu_allocator_t *alloc, hu_pwa_browser_t browser,
                           const char *url_pattern, hu_pwa_tab_t *out);

hu_error_t hu_pwa_list_tabs(hu_allocator_t *alloc, hu_pwa_browser_t browser,
                            const char *url_pattern, hu_pwa_tab_t **out, size_t *count);

/* ── JavaScript Execution ──────────────────────────────────────────── */

hu_error_t hu_pwa_exec_js(hu_allocator_t *alloc, const hu_pwa_tab_t *tab,
                          const char *javascript, char **out_result, size_t *out_len);

hu_error_t hu_pwa_activate_tab(hu_allocator_t *alloc, const hu_pwa_tab_t *tab);

/* ── App Drivers ───────────────────────────────────────────────────── */

#define HU_PWA_MAX_DRIVERS 16

typedef struct hu_pwa_driver {
    const char *app_name;     /* lowercase key, e.g. "slack" */
    const char *display_name; /* human-readable, e.g. "Slack" */
    const char *url_pattern;  /* substring match on tab URL */

    /* JavaScript snippets — each returns a string result.
     * %s in send_message_js is replaced with the escaped message. */
    const char *read_messages_js;
    const char *send_message_js;
    const char *read_contacts_js; /* may be NULL */
    const char *navigate_js;      /* may be NULL; %s = target (channel/contact) */
} hu_pwa_driver_t;

const hu_pwa_driver_t *hu_pwa_driver_find(const char *app_name);
const hu_pwa_driver_t *hu_pwa_driver_find_by_url(const char *url);
const hu_pwa_driver_t *hu_pwa_drivers_all(size_t *count);

/* ── High-Level Actions ────────────────────────────────────────────── */

hu_error_t hu_pwa_send_message(hu_allocator_t *alloc, hu_pwa_browser_t browser,
                               const char *app_name, const char *target,
                               const char *message, char **out_result, size_t *out_len);

hu_error_t hu_pwa_read_messages(hu_allocator_t *alloc, hu_pwa_browser_t browser,
                                const char *app_name, char **out_result, size_t *out_len);

/* ── JS String Escaping ────────────────────────────────────────────── */

hu_error_t hu_pwa_escape_js_string(hu_allocator_t *alloc, const char *input, size_t input_len,
                                   char **out, size_t *out_len);

hu_error_t hu_pwa_escape_applescript(hu_allocator_t *alloc, const char *input, size_t input_len,
                                     char **out, size_t *out_len);

#endif /* HU_PWA_H */
