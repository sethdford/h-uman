#include "test_framework.h"
#include "human/pwa.h"
#include "human/channels/pwa.h"
#include "human/tools/pwa.h"
#include "human/core/json.h"
#include <string.h>

/* ── Driver Lookup ─────────────────────────────────────────────────── */

static void test_driver_find_slack(void) {
    const hu_pwa_driver_t *d = hu_pwa_driver_find("slack");
    HU_ASSERT_NOT_NULL(d);
    HU_ASSERT_STR_EQ(d->app_name, "slack");
    HU_ASSERT_STR_EQ(d->display_name, "Slack");
    HU_ASSERT_NOT_NULL(d->url_pattern);
    HU_ASSERT_NOT_NULL(d->read_messages_js);
    HU_ASSERT_NOT_NULL(d->send_message_js);
}

static void test_driver_find_discord(void) {
    const hu_pwa_driver_t *d = hu_pwa_driver_find("discord");
    HU_ASSERT_NOT_NULL(d);
    HU_ASSERT_STR_EQ(d->app_name, "discord");
}

static void test_driver_find_whatsapp(void) {
    const hu_pwa_driver_t *d = hu_pwa_driver_find("whatsapp");
    HU_ASSERT_NOT_NULL(d);
    HU_ASSERT_STR_EQ(d->url_pattern, "web.whatsapp.com");
}

static void test_driver_find_gmail(void) {
    const hu_pwa_driver_t *d = hu_pwa_driver_find("gmail");
    HU_ASSERT_NOT_NULL(d);
    HU_ASSERT_NOT_NULL(d->read_messages_js);
}

static void test_driver_find_calendar(void) {
    const hu_pwa_driver_t *d = hu_pwa_driver_find("calendar");
    HU_ASSERT_NOT_NULL(d);
    HU_ASSERT(d->send_message_js == NULL);
}

static void test_driver_find_notion(void) {
    const hu_pwa_driver_t *d = hu_pwa_driver_find("notion");
    HU_ASSERT_NOT_NULL(d);
    HU_ASSERT_NOT_NULL(d->send_message_js);
}

static void test_driver_find_twitter(void) {
    const hu_pwa_driver_t *d = hu_pwa_driver_find("twitter");
    HU_ASSERT_NOT_NULL(d);
    HU_ASSERT_STR_EQ(d->url_pattern, "x.com");
}

static void test_driver_find_telegram(void) {
    const hu_pwa_driver_t *d = hu_pwa_driver_find("telegram");
    HU_ASSERT_NOT_NULL(d);
}

static void test_driver_find_facebook(void) {
    const hu_pwa_driver_t *d = hu_pwa_driver_find("facebook");
    HU_ASSERT_NOT_NULL(d);
    HU_ASSERT_STR_EQ(d->url_pattern, "facebook.com");
    HU_ASSERT_NOT_NULL(d->read_messages_js);
    HU_ASSERT_NOT_NULL(d->send_message_js);
    HU_ASSERT_NOT_NULL(d->navigate_js);
}

static void test_driver_find_linkedin(void) {
    const hu_pwa_driver_t *d = hu_pwa_driver_find("linkedin");
    HU_ASSERT_NOT_NULL(d);
}

static void test_driver_find_unknown_returns_null(void) {
    const hu_pwa_driver_t *d = hu_pwa_driver_find("nonexistent");
    HU_ASSERT(d == NULL);
}

static void test_driver_find_null_returns_null(void) {
    const hu_pwa_driver_t *d = hu_pwa_driver_find(NULL);
    HU_ASSERT(d == NULL);
}

static void test_driver_find_by_url_slack(void) {
    const hu_pwa_driver_t *d = hu_pwa_driver_find_by_url("https://app.slack.com/client/T01/C01");
    HU_ASSERT_NOT_NULL(d);
    HU_ASSERT_STR_EQ(d->app_name, "slack");
}

static void test_driver_find_by_url_gmail(void) {
    const hu_pwa_driver_t *d = hu_pwa_driver_find_by_url("https://mail.google.com/mail/u/0/#inbox");
    HU_ASSERT_NOT_NULL(d);
    HU_ASSERT_STR_EQ(d->app_name, "gmail");
}

static void test_driver_find_by_url_unknown(void) {
    const hu_pwa_driver_t *d = hu_pwa_driver_find_by_url("https://example.com");
    HU_ASSERT(d == NULL);
}

static void test_drivers_all_returns_count(void) {
    size_t count = 0;
    const hu_pwa_driver_t *first = hu_pwa_drivers_all(&count);
    HU_ASSERT(count >= 9);
    HU_ASSERT_NOT_NULL(first);
}

/* ── Driver Registry ────────────────────────────────────────────────── */

static void test_registry_init_destroy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_pwa_driver_registry_t reg;
    HU_ASSERT_EQ(hu_pwa_driver_registry_init(&reg), HU_OK);
    HU_ASSERT(reg.custom_drivers == NULL);
    HU_ASSERT_EQ(reg.custom_count, 0);
    hu_pwa_driver_registry_destroy(&alloc, &reg);
}

static void test_registry_add_find_custom_overrides_builtin(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_pwa_driver_registry_t reg;
    HU_ASSERT_EQ(hu_pwa_driver_registry_init(&reg), HU_OK);

    const hu_pwa_driver_t *builtin = hu_pwa_driver_find("slack");
    HU_ASSERT_NOT_NULL(builtin);
    HU_ASSERT_STR_EQ(builtin->display_name, "Slack");

    hu_pwa_driver_t custom = {
        .app_name = "slack",
        .display_name = "Slack Custom",
        .url_pattern = "app.slack.com",
        .read_messages_js = "(function(){return 'custom';})()",
        .send_message_js = "(function(){return 'sent';})()",
        .read_contacts_js = NULL,
        .navigate_js = NULL,
    };
    HU_ASSERT_EQ(hu_pwa_driver_registry_add(&alloc, &reg, &custom), HU_OK);

    const hu_pwa_driver_t *found = hu_pwa_driver_registry_find(&reg, "slack");
    HU_ASSERT_NOT_NULL(found);
    HU_ASSERT_STR_EQ(found->display_name, "Slack Custom");
    HU_ASSERT_STR_EQ(found->read_messages_js, "(function(){return 'custom';})()");

    found = hu_pwa_driver_registry_find(&reg, "discord");
    HU_ASSERT_NOT_NULL(found);
    HU_ASSERT_STR_EQ(found->app_name, "discord");

    found = hu_pwa_driver_registry_find(&reg, "nonexistent");
    HU_ASSERT(found == NULL);

    hu_pwa_driver_registry_destroy(&alloc, &reg);
}

static void test_registry_load_dir_test_mode_returns_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_pwa_driver_registry_t reg;
    HU_ASSERT_EQ(hu_pwa_driver_registry_init(&reg), HU_OK);
    hu_error_t err = hu_pwa_driver_registry_load_dir(&alloc, &reg, "/nonexistent/path");
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(reg.custom_count, 0);
    hu_pwa_driver_registry_destroy(&alloc, &reg);
}

static void test_registry_add_invalid_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_pwa_driver_registry_t reg;
    HU_ASSERT_EQ(hu_pwa_driver_registry_init(&reg), HU_OK);

    hu_pwa_driver_t bad = {
        .app_name = NULL,
        .display_name = "X",
        .url_pattern = "x.com",
        .read_messages_js = NULL,
        .send_message_js = NULL,
        .read_contacts_js = NULL,
        .navigate_js = NULL,
    };
    HU_ASSERT_EQ(hu_pwa_driver_registry_add(&alloc, &reg, &bad), HU_ERR_INVALID_ARGUMENT);

    hu_pwa_driver_registry_destroy(&alloc, &reg);
}

/* ── JS String Escaping ────────────────────────────────────────────── */

static void test_escape_js_basic(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_pwa_escape_js_string(&alloc, "hello world", 11, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(out, "hello world");
    HU_ASSERT_EQ(out_len, 11);
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_escape_js_quotes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_pwa_escape_js_string(&alloc, "say \"hello\"", 11, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(out, "say \\\"hello\\\"");
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_escape_js_newlines(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_pwa_escape_js_string(&alloc, "line1\nline2", 11, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(out, "line1\\nline2");
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_escape_js_backslash(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_pwa_escape_js_string(&alloc, "a\\b", 3, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(out, "a\\\\b");
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_escape_js_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_pwa_escape_js_string(NULL, "x", 1, &out, &out_len), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_pwa_escape_js_string(&alloc, NULL, 0, &out, &out_len), HU_ERR_INVALID_ARGUMENT);
}

static void test_escape_applescript_quotes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_pwa_escape_applescript(&alloc, "say \"hi\"", 8, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(out, "say \\\"hi\\\"");
    alloc.free(alloc.ctx, out, out_len + 1);
}

/* ── Browser Detection ─────────────────────────────────────────────── */

static void test_detect_browser_test_mode(void) {
    hu_pwa_browser_t browser;
    hu_error_t err = hu_pwa_detect_browser(&browser);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(browser, HU_PWA_BROWSER_CHROME);
}

static void test_browser_name_valid(void) {
    HU_ASSERT_STR_EQ(hu_pwa_browser_name(HU_PWA_BROWSER_CHROME), "Google Chrome");
    HU_ASSERT_STR_EQ(hu_pwa_browser_name(HU_PWA_BROWSER_ARC), "Arc");
    HU_ASSERT_STR_EQ(hu_pwa_browser_name(HU_PWA_BROWSER_BRAVE), "Brave Browser");
}

static void test_browser_name_invalid(void) {
    HU_ASSERT_STR_EQ(hu_pwa_browser_name(99), "unknown");
}

/* ── Tab Operations (Test Mode) ────────────────────────────────────── */

static void test_find_tab_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_pwa_tab_t tab;
    hu_error_t err = hu_pwa_find_tab(&alloc, HU_PWA_BROWSER_CHROME, "app.slack.com", &tab);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(tab.window_idx, 1);
    HU_ASSERT_EQ(tab.tab_idx, 1);
    HU_ASSERT_NOT_NULL(tab.url);
    HU_ASSERT(strstr(tab.url, "app.slack.com") != NULL);
    HU_ASSERT_NOT_NULL(tab.title);
    hu_pwa_tab_free(&alloc, &tab);
}

static void test_find_tab_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_pwa_tab_t tab;
    HU_ASSERT_EQ(hu_pwa_find_tab(NULL, HU_PWA_BROWSER_CHROME, "x", &tab), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_pwa_find_tab(&alloc, HU_PWA_BROWSER_CHROME, NULL, &tab), HU_ERR_INVALID_ARGUMENT);
}

static void test_list_tabs_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_pwa_tab_t *tabs = NULL;
    size_t count = 0;
    hu_error_t err = hu_pwa_list_tabs(&alloc, HU_PWA_BROWSER_CHROME, NULL, &tabs, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(count >= 1);
    HU_ASSERT_NOT_NULL(tabs);
    hu_pwa_tabs_free(&alloc, tabs, count);
}

static void test_exec_js_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_pwa_tab_t tab;
    memset(&tab, 0, sizeof(tab));
    tab.browser = HU_PWA_BROWSER_CHROME;
    tab.window_idx = 1;
    tab.tab_idx = 1;
    char *result = NULL;
    size_t rlen = 0;
    hu_error_t err = hu_pwa_exec_js(&alloc, &tab, "document.title", &result, &rlen);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(result);
    HU_ASSERT(rlen > 0);
    alloc.free(alloc.ctx, result, rlen + 1);
}

/* ── High-Level Actions (Test Mode) ────────────────────────────────── */

static void test_send_message_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *result = NULL;
    size_t rlen = 0;
    hu_error_t err = hu_pwa_send_message(&alloc, HU_PWA_BROWSER_CHROME,
                                          "slack", NULL, "hello world", &result, &rlen);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(result);
    alloc.free(alloc.ctx, result, rlen + 1);
}

static void test_read_messages_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *result = NULL;
    size_t rlen = 0;
    hu_error_t err = hu_pwa_read_messages(&alloc, HU_PWA_BROWSER_CHROME,
                                           "slack", &result, &rlen);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(result);
    alloc.free(alloc.ctx, result, rlen + 1);
}

static void test_send_message_unknown_app(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *result = NULL;
    size_t rlen = 0;
    hu_error_t err = hu_pwa_send_message(&alloc, HU_PWA_BROWSER_CHROME,
                                          "nonexistent", NULL, "hi", &result, &rlen);
    HU_ASSERT_EQ(err, HU_ERR_NOT_FOUND);
}

static void test_read_messages_null_args(void) {
    char *r = NULL;
    size_t rl = 0;
    HU_ASSERT_EQ(hu_pwa_read_messages(NULL, HU_PWA_BROWSER_CHROME, "slack", &r, &rl),
                 HU_ERR_INVALID_ARGUMENT);
}

/* ── Tool Vtable ───────────────────────────────────────────────────── */

static void test_pwa_tool_create_destroy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_pwa_tool_create(&alloc, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(tool.vtable);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "pwa");
    HU_ASSERT_NOT_NULL(tool.vtable->description(tool.ctx));
    HU_ASSERT_NOT_NULL(tool.vtable->parameters_json(tool.ctx));
    tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_pwa_tool_list_apps(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_pwa_tool_create(&alloc, &tool);

    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "action", hu_json_string_new(&alloc, "list_apps", 9));

    hu_tool_result_t result;
    memset(&result, 0, sizeof(result));
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.success);
    HU_ASSERT(result.output_len > 0);
    HU_ASSERT(strstr(result.output, "slack") != NULL);
    HU_ASSERT(strstr(result.output, "discord") != NULL);

    hu_json_free(&alloc, args);
    hu_tool_result_free(&alloc, &result);
    tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_pwa_tool_read_slack(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_pwa_tool_create(&alloc, &tool);

    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "action", hu_json_string_new(&alloc, "read", 4));
    hu_json_object_set(&alloc, args, "app", hu_json_string_new(&alloc, "slack", 5));

    hu_tool_result_t result;
    memset(&result, 0, sizeof(result));
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.success);
    HU_ASSERT(result.output_len > 0);

    hu_json_free(&alloc, args);
    hu_tool_result_free(&alloc, &result);
    tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_pwa_tool_unknown_action(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_pwa_tool_create(&alloc, &tool);

    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "action", hu_json_string_new(&alloc, "explode", 7));

    hu_tool_result_t result;
    memset(&result, 0, sizeof(result));
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FALSE(result.success);
    HU_ASSERT_NOT_NULL(result.error_msg);
    HU_ASSERT(strstr(result.error_msg, "Unknown action") != NULL);

    hu_json_free(&alloc, args);
    hu_tool_result_free(&alloc, &result);
    tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_pwa_tool_send_missing_app(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_pwa_tool_create(&alloc, &tool);

    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "action", hu_json_string_new(&alloc, "send", 4));

    hu_tool_result_t result;
    memset(&result, 0, sizeof(result));
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FALSE(result.success);

    hu_json_free(&alloc, args);
    hu_tool_result_free(&alloc, &result);
    tool.vtable->deinit(tool.ctx, &alloc);
}

/* ── Global Registry ───────────────────────────────────────────────── */

static void test_global_registry_resolve_builtin(void) {
    hu_pwa_set_global_registry(NULL);
    const hu_pwa_driver_t *d = hu_pwa_driver_resolve("slack");
    HU_ASSERT_NOT_NULL(d);
    HU_ASSERT_STR_EQ(d->app_name, "slack");
}

static void test_global_registry_resolve_custom_override(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_pwa_driver_registry_t reg;
    hu_pwa_driver_registry_init(&reg);

    hu_pwa_driver_t custom = {
        .app_name = "myapp",
        .display_name = "My App",
        .url_pattern = "myapp.example.com",
        .read_messages_js = "(function(){ return 'custom'; })()",
        .send_message_js = NULL,
        .read_contacts_js = NULL,
        .navigate_js = NULL,
    };
    hu_pwa_driver_registry_add(&alloc, &reg, &custom);
    hu_pwa_set_global_registry(&reg);

    const hu_pwa_driver_t *d = hu_pwa_driver_resolve("myapp");
    HU_ASSERT_NOT_NULL(d);
    HU_ASSERT_STR_EQ(d->app_name, "myapp");
    HU_ASSERT_STR_EQ(d->url_pattern, "myapp.example.com");

    const hu_pwa_driver_t *slack = hu_pwa_driver_resolve("slack");
    HU_ASSERT_NOT_NULL(slack);
    HU_ASSERT_STR_EQ(slack->app_name, "slack");

    hu_pwa_set_global_registry(NULL);
    hu_pwa_driver_registry_destroy(&alloc, &reg);
}

static void test_global_registry_resolve_unknown(void) {
    hu_pwa_set_global_registry(NULL);
    const hu_pwa_driver_t *d = hu_pwa_driver_resolve("nonexistent_app_xyz");
    HU_ASSERT_NULL(d);
}

/* ── Channel ───────────────────────────────────────────────────────── */

static void test_pwa_channel_create_destroy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    const char *apps[] = {"slack", "discord"};
    hu_error_t err = hu_pwa_channel_create(&alloc, apps, 2, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(ch.ctx);
    HU_ASSERT_NOT_NULL(ch.vtable);
    hu_pwa_channel_destroy(&ch);
}

static void test_pwa_channel_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_pwa_channel_create(&alloc, NULL, 0, &ch);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "pwa");
    hu_pwa_channel_destroy(&ch);
}

static void test_pwa_channel_health(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_pwa_channel_create(&alloc, NULL, 0, &ch);
    HU_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    hu_pwa_channel_destroy(&ch);
}

static void test_pwa_channel_send_stores_message(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_pwa_channel_create(&alloc, NULL, 0, &ch);
    hu_error_t err = ch.vtable->send(ch.ctx, "pwa:slack", 9, "hello", 5, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    size_t len = 0;
    const char *last = hu_pwa_channel_test_get_last(&ch, &len);
    HU_ASSERT_NOT_NULL(last);
    HU_ASSERT_STR_EQ(last, "hello");
    HU_ASSERT_EQ(len, 5);
    hu_pwa_channel_destroy(&ch);
}

static void test_pwa_channel_poll_inject(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_pwa_channel_create(&alloc, NULL, 0, &ch);
    hu_pwa_channel_test_inject(&ch, "slack", "alice: Hey there!");
    hu_channel_loop_msg_t msgs[4];
    memset(msgs, 0, sizeof(msgs));
    size_t count = 0;
    hu_error_t err = hu_pwa_channel_poll(ch.ctx, &alloc, msgs, 4, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 1);
    HU_ASSERT_STR_EQ(msgs[0].session_key, "pwa:slack");
    HU_ASSERT_STR_EQ(msgs[0].content, "alice: Hey there!");
    hu_pwa_channel_destroy(&ch);
}

static void test_pwa_channel_poll_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_pwa_channel_create(&alloc, NULL, 0, &ch);
    hu_channel_loop_msg_t msgs[4];
    size_t count = 99;
    hu_error_t err = hu_pwa_channel_poll(ch.ctx, &alloc, msgs, 4, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0);
    hu_pwa_channel_destroy(&ch);
}

/* ── Suite Runner ──────────────────────────────────────────────────── */

void run_pwa_tests(void) {
    HU_TEST_SUITE("pwa_drivers");
    HU_RUN_TEST(test_driver_find_slack);
    HU_RUN_TEST(test_driver_find_discord);
    HU_RUN_TEST(test_driver_find_whatsapp);
    HU_RUN_TEST(test_driver_find_gmail);
    HU_RUN_TEST(test_driver_find_calendar);
    HU_RUN_TEST(test_driver_find_notion);
    HU_RUN_TEST(test_driver_find_twitter);
    HU_RUN_TEST(test_driver_find_telegram);
    HU_RUN_TEST(test_driver_find_linkedin);
    HU_RUN_TEST(test_driver_find_facebook);
    HU_RUN_TEST(test_driver_find_unknown_returns_null);
    HU_RUN_TEST(test_driver_find_null_returns_null);
    HU_RUN_TEST(test_driver_find_by_url_slack);
    HU_RUN_TEST(test_driver_find_by_url_gmail);
    HU_RUN_TEST(test_driver_find_by_url_unknown);
    HU_RUN_TEST(test_drivers_all_returns_count);

    HU_TEST_SUITE("pwa_registry");
    HU_RUN_TEST(test_registry_init_destroy);
    HU_RUN_TEST(test_registry_add_find_custom_overrides_builtin);
    HU_RUN_TEST(test_registry_load_dir_test_mode_returns_ok);
    HU_RUN_TEST(test_registry_add_invalid_returns_error);

    HU_TEST_SUITE("pwa_escaping");
    HU_RUN_TEST(test_escape_js_basic);
    HU_RUN_TEST(test_escape_js_quotes);
    HU_RUN_TEST(test_escape_js_newlines);
    HU_RUN_TEST(test_escape_js_backslash);
    HU_RUN_TEST(test_escape_js_null_args);
    HU_RUN_TEST(test_escape_applescript_quotes);

    HU_TEST_SUITE("pwa_browser");
    HU_RUN_TEST(test_detect_browser_test_mode);
    HU_RUN_TEST(test_browser_name_valid);
    HU_RUN_TEST(test_browser_name_invalid);

    HU_TEST_SUITE("pwa_tabs");
    HU_RUN_TEST(test_find_tab_test_mode);
    HU_RUN_TEST(test_find_tab_null_args);
    HU_RUN_TEST(test_list_tabs_test_mode);
    HU_RUN_TEST(test_exec_js_test_mode);

    HU_TEST_SUITE("pwa_actions");
    HU_RUN_TEST(test_send_message_test_mode);
    HU_RUN_TEST(test_read_messages_test_mode);
    HU_RUN_TEST(test_send_message_unknown_app);
    HU_RUN_TEST(test_read_messages_null_args);

    HU_TEST_SUITE("pwa_tool");
    HU_RUN_TEST(test_pwa_tool_create_destroy);
    HU_RUN_TEST(test_pwa_tool_list_apps);
    HU_RUN_TEST(test_pwa_tool_read_slack);
    HU_RUN_TEST(test_pwa_tool_unknown_action);
    HU_RUN_TEST(test_pwa_tool_send_missing_app);

    HU_TEST_SUITE("pwa_global_registry");
    HU_RUN_TEST(test_global_registry_resolve_builtin);
    HU_RUN_TEST(test_global_registry_resolve_custom_override);
    HU_RUN_TEST(test_global_registry_resolve_unknown);

    HU_TEST_SUITE("pwa_channel");
    HU_RUN_TEST(test_pwa_channel_create_destroy);
    HU_RUN_TEST(test_pwa_channel_name);
    HU_RUN_TEST(test_pwa_channel_health);
    HU_RUN_TEST(test_pwa_channel_send_stores_message);
    HU_RUN_TEST(test_pwa_channel_poll_inject);
    HU_RUN_TEST(test_pwa_channel_poll_empty);
}
