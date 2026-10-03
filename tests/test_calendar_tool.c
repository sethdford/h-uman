/* Google Calendar tool: the decisions behind list/create/delete.
 *
 * Before 2026-09-30 `delete` issued a GET and reported {"deleted":true}
 * without deleting anything, `create` pasted title/description into its JSON
 * body with %s and treated any HTTP status as success, and ids went into the
 * request URL unchecked. The network path is compiled out under HU_IS_TEST,
 * so these tests pin the extracted decisions it now uses. */
#include "human/core/allocator.h"
#include "human/core/json.h"
#include "human/tool.h"
#include "human/tools/calendar_tool.h"
#include "test_framework.h"

#include <string.h>

static void calendar_id_accepts_real_ids(void) {
    HU_ASSERT_TRUE(hu_calendar_id_is_safe("primary", 200));
    HU_ASSERT_TRUE(hu_calendar_id_is_safe("user_a@example.com", 200));
    HU_ASSERT_TRUE(hu_calendar_id_is_safe("abc123def456", 1024));
    HU_ASSERT_TRUE(hu_calendar_id_is_safe("evt-1_a.b", 1024));
}

static void calendar_id_rejects_anything_that_can_change_the_url(void) {
    HU_ASSERT_FALSE(hu_calendar_id_is_safe(NULL, 200));
    HU_ASSERT_FALSE(hu_calendar_id_is_safe("", 200));
    HU_ASSERT_FALSE(hu_calendar_id_is_safe("a/b", 200));
    HU_ASSERT_FALSE(hu_calendar_id_is_safe("evt?maxResults=500", 200));
    HU_ASSERT_FALSE(hu_calendar_id_is_safe("evt#frag", 200));
    HU_ASSERT_FALSE(hu_calendar_id_is_safe("evt%2F..", 200));
    HU_ASSERT_FALSE(hu_calendar_id_is_safe("two words", 200));
    HU_ASSERT_FALSE(hu_calendar_id_is_safe("abcdef", 5)); /* over max_len */
}

static void calendar_event_body_keeps_a_hostile_title_as_text(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *title = "x\",\"attendees\":[\"evil@example.com\"],\"summary\":\"y";
    char *body = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_calendar_event_body(&alloc, title, "2026-10-01T09:00:00Z",
                                        "2026-10-01T09:30:00Z", "say \"hi\" \\ bye", &body, &len),
                 HU_OK);
    hu_json_value_t *root = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, body, len, &root), HU_OK);
    HU_ASSERT_STR_EQ(hu_json_get_string(root, "summary"), title);
    HU_ASSERT_NULL(hu_json_object_get(root, "attendees"));
    HU_ASSERT_STR_EQ(hu_json_get_string(root, "description"), "say \"hi\" \\ bye");
    hu_json_value_t *start = hu_json_object_get(root, "start");
    HU_ASSERT_NOT_NULL(start);
    HU_ASSERT_STR_EQ(hu_json_get_string(start, "dateTime"), "2026-10-01T09:00:00Z");
    hu_json_free(&alloc, root);
    alloc.free(alloc.ctx, body, len + 1);
}

static void calendar_event_body_omits_a_missing_description(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *body = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_calendar_event_body(&alloc, "Standup", "a", "b", NULL, &body, &len), HU_OK);
    HU_ASSERT_NULL(strstr(body, "description"));
    alloc.free(alloc.ctx, body, len + 1);
    HU_ASSERT_EQ(hu_calendar_event_body(&alloc, NULL, "a", "b", NULL, &body, &len),
                 HU_ERR_INVALID_ARGUMENT);
}

static void calendar_only_2xx_counts_as_success(void) {
    HU_ASSERT_TRUE(hu_calendar_status_ok(200));
    HU_ASSERT_TRUE(hu_calendar_status_ok(204));
    HU_ASSERT_FALSE(hu_calendar_status_ok(0)); /* transport never answered */
    HU_ASSERT_FALSE(hu_calendar_status_ok(302));
    HU_ASSERT_FALSE(hu_calendar_status_ok(401));
    HU_ASSERT_FALSE(hu_calendar_status_ok(404));
    HU_ASSERT_FALSE(hu_calendar_status_ok(500));
}

static void calendar_tool_offers_only_what_it_implements(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    HU_ASSERT_EQ(hu_calendar_create(&alloc, &tool), HU_OK);
    const char *params = tool.vtable->parameters_json(tool.ctx);
    HU_ASSERT_NULL(strstr(params, "availability"));
    HU_ASSERT_NULL(strstr(params, "\"update\""));
    HU_ASSERT_NULL(strstr(params, "attendees"));

    hu_json_value_t *args = NULL;
    const char *a = "{\"action\":\"availability\"}";
    HU_ASSERT_EQ(hu_json_parse(&alloc, a, strlen(a), &args), HU_OK);
    hu_tool_result_t r;
    memset(&r, 0, sizeof(r));
    HU_ASSERT_EQ(tool.vtable->execute(tool.ctx, &alloc, args, &r), HU_OK);
    HU_ASSERT_FALSE(r.success);
    hu_tool_result_free(&alloc, &r);
    hu_json_free(&alloc, args);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

void run_calendar_tool_tests(void) {
    HU_TEST_SUITE("calendar tool");
    HU_RUN_TEST(calendar_id_accepts_real_ids);
    HU_RUN_TEST(calendar_id_rejects_anything_that_can_change_the_url);
    HU_RUN_TEST(calendar_event_body_keeps_a_hostile_title_as_text);
    HU_RUN_TEST(calendar_event_body_omits_a_missing_description);
    HU_RUN_TEST(calendar_only_2xx_counts_as_success);
    HU_RUN_TEST(calendar_tool_offers_only_what_it_implements);
}
