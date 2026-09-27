/* Skill registry, skillforge and skill-run tool tests.
 *
 * The filename heuristic in scripts/check-test-references.sh resolves
 * "skills" to src/intelligence/skills.c, which this file does not exercise;
 * it covers src/skills/skill_registry.c, src/skills/skillforge.c and
 * src/tools/skill_run.c.
 *
 * // @covers-none — heuristic picks src/intelligence/skills.c; see above.
 */
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/skill_registry.h"
#include "human/skillforge.h"
#include "human/tools/skill_run.h"
#include "test_framework.h"
#include <string.h>

extern void hu_skill_registry_resolve_tags_string(hu_json_value_t *tags_val, char *tags_buf,
                                                  size_t tags_buf_len, const char **out_tags_str);

/* HU_IS_TEST: skill registry returns mock data without network */
static void test_skill_registry_search_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skill_registry_entry_t *entries = NULL;
    size_t count = 0;
    hu_error_t err = hu_skill_registry_search(&alloc, NULL, "code", &entries, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(entries);
    HU_ASSERT_TRUE(count >= 1);
    HU_ASSERT_STR_EQ(entries[0].name, "code-review");
    hu_skill_registry_entries_free(&alloc, entries, count);
}

static void test_skill_registry_install_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_skill_registry_install(&alloc, "/tmp/skill-dir");
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_skill_registry_install_by_name_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_skill_registry_install_by_name(&alloc, NULL, "code-review");
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_skill_registry_install_by_name_null_name_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_skill_registry_install_by_name(&alloc, NULL, NULL);
    HU_ASSERT(err != HU_OK);
    err = hu_skill_registry_install_by_name(&alloc, NULL, "");
    HU_ASSERT(err != HU_OK);
}

static void test_skill_registry_install_by_name_null_alloc_fails(void) {
    hu_error_t err = hu_skill_registry_install_by_name(NULL, NULL, "x");
    HU_ASSERT(err != HU_OK);
}

static void test_skill_registry_uninstall_mock(void) {
    hu_error_t err = hu_skill_registry_uninstall("nonexistent");
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_skill_registry_get_installed_dir(void) {
    char buf[256];
    size_t n = hu_skill_registry_get_installed_dir(buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0 || (n == 0 && !getenv("HOME")));
    if (n >= 6)
        HU_ASSERT_TRUE(strstr(buf, "skills") != NULL);
}

static void test_skill_registry_publish_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_skill_registry_publish(&alloc, "/tmp/skill");
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_skill_registry_publish_null_dir(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_skill_registry_publish(&alloc, NULL);
    HU_ASSERT(err != HU_OK);
}

static void test_skill_registry_search_empty_query(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skill_registry_entry_t *entries = NULL;
    size_t count = 0;
    hu_error_t err = hu_skill_registry_search(&alloc, NULL, "", &entries, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(entries);
    HU_ASSERT_TRUE(count >= 1);
    hu_skill_registry_entries_free(&alloc, entries, count);
}

static void test_skill_registry_search_null_query(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skill_registry_entry_t *entries = NULL;
    size_t count = 0;
    hu_error_t err = hu_skill_registry_search(&alloc, NULL, NULL, &entries, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(entries);
    HU_ASSERT_TRUE(count >= 1);
    hu_skill_registry_entries_free(&alloc, entries, count);
}

static void test_skill_registry_install_null_path(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_skill_registry_install(&alloc, NULL);
    HU_ASSERT(err != HU_OK);
}

static void test_skill_registry_install_empty_path(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_skill_registry_install(&alloc, "");
    HU_ASSERT(err != HU_OK);
}

static void test_skill_registry_uninstall_null_name(void) {
    hu_error_t err = hu_skill_registry_uninstall(NULL);
    HU_ASSERT(err != HU_OK);
}

static void test_skill_registry_update_null_alloc(void) {
    hu_error_t err = hu_skill_registry_update(NULL, "/tmp/skill");
    HU_ASSERT(err != HU_OK);
}

static void test_skill_registry_update_null_path(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_skill_registry_update(&alloc, NULL);
    HU_ASSERT(err != HU_OK);
}

static void test_skill_registry_get_installed_dir_writes_path(void) {
    char buf[512];
    size_t n = hu_skill_registry_get_installed_dir(buf, sizeof(buf));
    if (getenv("HOME")) {
        HU_ASSERT_TRUE(n > 0);
        HU_ASSERT_TRUE(strstr(buf, ".human") != NULL || strstr(buf, "skills") != NULL);
    }
}

static void test_skill_registry_get_installed_dir_small_buffer(void) {
    char buf[4];
    size_t n = hu_skill_registry_get_installed_dir(buf, 4);
    HU_ASSERT_TRUE(n == 0 || n < 4);
}

static void test_skill_registry_search_mock_returns_entries(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skill_registry_entry_t *entries = NULL;
    size_t count = 0;
    hu_error_t err = hu_skill_registry_search(&alloc, NULL, "code", &entries, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(entries);
    HU_ASSERT_EQ(count, 2u);
    HU_ASSERT_STR_EQ(entries[0].name, "code-review");
    HU_ASSERT_STR_EQ(entries[1].name, "code-formatter");
    hu_skill_registry_entries_free(&alloc, entries, count);
}

static void test_skill_registry_entries_free_null_safe(void) {
    /* Crash safety test: NULL entries must not crash for any count. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_skill_registry_entries_free(&alloc, NULL, 0);
    hu_skill_registry_entries_free(&alloc, NULL, 5);
}

static void test_skill_registry_tags_array_parses_to_comma_string(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"name\":\"test-skill\",\"description\":\"d\",\"tags\":[\"python\", "
                       "\"automation\"]}";
    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(&alloc, json, strlen(json), &root);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(root);
    hu_json_value_t *tags_val = hu_json_object_get(root, "tags");
    HU_ASSERT_NOT_NULL(tags_val);
    char tags_buf[256];
    const char *tags_str = NULL;
    hu_skill_registry_resolve_tags_string(tags_val, tags_buf, sizeof(tags_buf), &tags_str);
    HU_ASSERT_NOT_NULL(tags_str);
    HU_ASSERT_STR_EQ(tags_str, "python, automation");
    hu_json_free(&alloc, root);
}

static void test_skillforge_load_instructions_from_path(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skillforge_t sf;
    hu_skillforge_create(&alloc, &sf);
    hu_skillforge_discover(&sf, "/tmp");
    hu_skill_t *s = hu_skillforge_get_skill(&sf, "skill-md-mock");
    HU_ASSERT_NOT_NULL(s);
    char *ins = NULL;
    size_t ilen = 0;
    HU_ASSERT_EQ(hu_skillforge_load_instructions(&alloc, s, &ins, &ilen), HU_OK);
    HU_ASSERT_NOT_NULL(ins);
    HU_ASSERT_TRUE(strstr(ins, "Mock SKILL.md") != NULL);
    alloc.free(alloc.ctx, ins, ilen + 1);
    hu_skillforge_destroy(&sf);
}

static void test_skillforge_load_instructions_fallback_to_description(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skillforge_t sf;
    hu_skillforge_create(&alloc, &sf);
    hu_skillforge_discover(&sf, "/tmp");
    hu_skill_t *s = hu_skillforge_get_skill(&sf, "test-skill");
    HU_ASSERT_NOT_NULL(s);
    HU_ASSERT_NULL(s->instructions_path);
    char *ins = NULL;
    HU_ASSERT_EQ(hu_skillforge_load_instructions(&alloc, s, &ins, NULL), HU_OK);
    HU_ASSERT_NOT_NULL(ins);
    HU_ASSERT_STR_EQ(ins, "A test skill for unit tests");
    alloc.free(alloc.ctx, ins, strlen(ins) + 1);
    hu_skillforge_destroy(&sf);
}

static void test_skillforge_read_resource_validates_path(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skillforge_t sf;
    hu_skillforge_create(&alloc, &sf);
    hu_skillforge_discover(&sf, "/tmp");
    hu_skill_t *s = hu_skillforge_get_skill(&sf, "skill-md-mock");
    HU_ASSERT_NOT_NULL(s);
    char *buf = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_skillforge_read_resource(&alloc, s, "../secret", &buf, &n),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NULL(buf);
    hu_skillforge_destroy(&sf);
}

static void test_skillforge_read_resource_not_found(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skillforge_t sf;
    hu_skillforge_create(&alloc, &sf);
    hu_skillforge_discover(&sf, "/tmp");
    hu_skill_t *s = hu_skillforge_get_skill(&sf, "skill-md-mock");
    HU_ASSERT_NOT_NULL(s);
    char *buf = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_skillforge_read_resource(&alloc, s, "missing.txt", &buf, &n), HU_ERR_NOT_FOUND);
    HU_ASSERT_NULL(buf);
    hu_skillforge_destroy(&sf);
}

static void test_skillforge_read_resource_fixture_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skillforge_t sf;
    hu_skillforge_create(&alloc, &sf);
    hu_skillforge_discover(&sf, "/tmp");
    hu_skill_t *s = hu_skillforge_get_skill(&sf, "skill-md-mock");
    HU_ASSERT_NOT_NULL(s);
    char *buf = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_skillforge_read_resource(&alloc, s, "fixture.txt", &buf, &n), HU_OK);
    HU_ASSERT_NOT_NULL(buf);
    HU_ASSERT_TRUE(strstr(buf, "fixture resource") != NULL);
    alloc.free(alloc.ctx, buf, n + 1);
    hu_skillforge_destroy(&sf);
}

static void test_skillforge_keyword_hits_cli_skill(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skillforge_t sf;
    HU_ASSERT_EQ(hu_skillforge_create(&alloc, &sf), HU_OK);
    HU_ASSERT_EQ(hu_skillforge_discover(&sf, "/tmp"), HU_OK);
    hu_skill_t *s = hu_skillforge_get_skill(&sf, "cli-helper");
    HU_ASSERT_NOT_NULL(s);
    const char *msg = "need help with CLI commands";
    int h = hu_skillforge_skill_keyword_hits(s, msg, strlen(msg));
    HU_ASSERT_TRUE(h >= 1);
    hu_skillforge_destroy(&sf);
}

static void test_skillforge_execute_prefers_instructions(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skillforge_t sf;
    hu_skillforge_create(&alloc, &sf);
    hu_skillforge_discover(&sf, "/tmp");
    char *out = NULL;
    HU_ASSERT_EQ(hu_skillforge_execute(&alloc, &sf, "skill-md-mock", &out), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "Do the thing") != NULL);
    alloc.free(alloc.ctx, out, strlen(out) + 1);
    hu_skillforge_destroy(&sf);
}

static void test_skill_run_returns_instructions_content(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skillforge_t sf;
    hu_skillforge_create(&alloc, &sf);
    hu_skillforge_discover(&sf, "/tmp");
    hu_tool_t tool;
    HU_ASSERT_EQ(hu_skill_run_create(&alloc, &tool, &sf, NULL, 0, NULL), HU_OK);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "skill", hu_json_string_new(&alloc, "skill-md-mock", 14));
    hu_tool_result_t result = {0};
    HU_ASSERT_EQ(tool.vtable->execute(tool.ctx, &alloc, args, &result), HU_OK);
    HU_ASSERT_TRUE(result.success);
    HU_ASSERT_NOT_NULL(result.output);
    HU_ASSERT_TRUE(strstr(result.output, "Instructions:") != NULL);
    HU_ASSERT_TRUE(strstr(result.output, "Mock SKILL.md") != NULL);
    if (result.output_owned)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    hu_json_free(&alloc, args);
    tool.vtable->deinit(tool.ctx, &alloc);
    hu_skillforge_destroy(&sf);
}

void run_skills_tests(void) {
    HU_TEST_SUITE("Skills");
    HU_RUN_TEST(test_skill_registry_search_mock);
    HU_RUN_TEST(test_skill_registry_install_mock);
    HU_RUN_TEST(test_skill_registry_install_by_name_mock);
    HU_RUN_TEST(test_skill_registry_install_by_name_null_name_fails);
    HU_RUN_TEST(test_skill_registry_install_by_name_null_alloc_fails);
    HU_RUN_TEST(test_skill_registry_uninstall_mock);
    HU_RUN_TEST(test_skill_registry_get_installed_dir);
    HU_RUN_TEST(test_skill_registry_publish_mock);
    HU_RUN_TEST(test_skill_registry_publish_null_dir);
    HU_RUN_TEST(test_skill_registry_search_empty_query);
    HU_RUN_TEST(test_skill_registry_search_null_query);
    HU_RUN_TEST(test_skill_registry_install_null_path);
    HU_RUN_TEST(test_skill_registry_install_empty_path);
    HU_RUN_TEST(test_skill_registry_uninstall_null_name);
    HU_RUN_TEST(test_skill_registry_update_null_alloc);
    HU_RUN_TEST(test_skill_registry_update_null_path);
    HU_RUN_TEST(test_skill_registry_get_installed_dir_writes_path);
    HU_RUN_TEST(test_skill_registry_get_installed_dir_small_buffer);
    HU_RUN_TEST(test_skill_registry_search_mock_returns_entries);
    HU_RUN_TEST(test_skill_registry_entries_free_null_safe);
    HU_RUN_TEST(test_skill_registry_tags_array_parses_to_comma_string);
    HU_RUN_TEST(test_skillforge_load_instructions_from_path);
    HU_RUN_TEST(test_skillforge_load_instructions_fallback_to_description);
    HU_RUN_TEST(test_skillforge_read_resource_validates_path);
    HU_RUN_TEST(test_skillforge_read_resource_not_found);
    HU_RUN_TEST(test_skillforge_read_resource_fixture_ok);
    HU_RUN_TEST(test_skillforge_keyword_hits_cli_skill);
    HU_RUN_TEST(test_skillforge_execute_prefers_instructions);
    HU_RUN_TEST(test_skill_run_returns_instructions_content);
}
