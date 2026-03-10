/* Skills and skill registry tests. */
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/skill_registry.h"
#include "human/skills.h"
#include "test_framework.h"
#include <string.h>

static void test_skills_list_delegates_to_skillforge(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skill_t *skills = NULL;
    size_t count = 99;
    hu_error_t err = hu_skills_list(&alloc, "/tmp", &skills, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(skills);
    HU_ASSERT_EQ(count, 3u);
    HU_ASSERT_STR_EQ(skills[0].name, "test-skill");
    hu_skills_free(&alloc, skills, count);
}

static void test_skills_list_null_workspace_uses_dot(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skill_t *skills = NULL;
    size_t count = 0;
    hu_error_t err = hu_skills_list(&alloc, NULL, &skills, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(skills);
    HU_ASSERT_EQ(count, 3u);
    hu_skills_free(&alloc, skills, count);
}

static void test_skills_free_null_safe(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skills_free(&alloc, NULL, 0);
}

static void test_skills_free_with_null_skills(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skills_free(&alloc, NULL, 5);
}

static void test_skills_list_resets_output(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skill_t *skills = (hu_skill_t *)0xdeadbeef;
    size_t count = 42;
    hu_error_t err = hu_skills_list(&alloc, ".", &skills, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(skills);
    HU_ASSERT_EQ(count, 3u);
    hu_skills_free(&alloc, skills, count);
}

static void test_skills_list_null_out_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skill_t *skills = NULL;
    size_t count = 0;
    hu_error_t err = hu_skills_list(&alloc, "/tmp", NULL, &count);
    HU_ASSERT_NEQ(err, HU_OK);
    err = hu_skills_list(&alloc, "/tmp", &skills, NULL);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_skills_list_empty_workspace_path(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skill_t *skills = NULL;
    size_t count = 0;
    hu_error_t err = hu_skills_list(&alloc, "", &skills, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(skills);
    HU_ASSERT_TRUE(count >= 1);
    hu_skills_free(&alloc, skills, count);
}

static void test_skills_list_dot_workspace(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skill_t *skills = NULL;
    size_t count = 0;
    hu_error_t err = hu_skills_list(&alloc, ".", &skills, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(skills);
    HU_ASSERT_EQ(count, 3u);
    hu_skills_free(&alloc, skills, count);
}

/* HU_IS_TEST: skill registry returns mock data without network */
static void test_skill_registry_search_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skill_registry_entry_t *entries = NULL;
    size_t count = 0;
    hu_error_t err = hu_skill_registry_search(&alloc, "code", &entries, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(entries);
    HU_ASSERT_TRUE(count >= 1);
    HU_ASSERT_STR_EQ(entries[0].name, "code-review");
    hu_skill_registry_entries_free(&alloc, entries, count);
}

static void test_skill_registry_install_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_skill_registry_install(&alloc, "code-review");
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
}

static void test_skill_registry_uninstall_mock(void) {
    hu_error_t err = hu_skill_registry_uninstall("nonexistent");
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
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
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
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
    hu_error_t err = hu_skill_registry_search(&alloc, "", &entries, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(entries);
    HU_ASSERT_TRUE(count >= 1);
    hu_skill_registry_entries_free(&alloc, entries, count);
}

static void test_skill_registry_search_null_query(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skill_registry_entry_t *entries = NULL;
    size_t count = 0;
    hu_error_t err = hu_skill_registry_search(&alloc, NULL, &entries, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(entries);
    HU_ASSERT_TRUE(count >= 1);
    hu_skill_registry_entries_free(&alloc, entries, count);
}

static void test_skill_registry_install_null_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_skill_registry_install(&alloc, NULL);
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
}

static void test_skill_registry_install_empty_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_skill_registry_install(&alloc, "");
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
}

static void test_skill_registry_uninstall_null_name(void) {
    hu_error_t err = hu_skill_registry_uninstall(NULL);
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
}

static void test_skill_registry_update_null_alloc(void) {
    hu_error_t err = hu_skill_registry_update(NULL);
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
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
    hu_error_t err = hu_skill_registry_search(&alloc, "code", &entries, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(entries);
    HU_ASSERT_EQ(count, 2u);
    HU_ASSERT_STR_EQ(entries[0].name, "code-review");
    HU_ASSERT_STR_EQ(entries[1].name, "email-digest");
    hu_skill_registry_entries_free(&alloc, entries, count);
}

static void test_skill_registry_entries_free_null_safe(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skill_registry_entries_free(&alloc, NULL, 0);
    hu_skill_registry_entries_free(&alloc, NULL, 5);
}

void run_skills_tests(void) {
    HU_TEST_SUITE("Skills");
    HU_RUN_TEST(test_skills_list_delegates_to_skillforge);
    HU_RUN_TEST(test_skills_list_null_workspace_uses_dot);
    HU_RUN_TEST(test_skills_free_null_safe);
    HU_RUN_TEST(test_skills_free_with_null_skills);
    HU_RUN_TEST(test_skills_list_resets_output);
    HU_RUN_TEST(test_skills_list_null_out_returns_error);
    HU_RUN_TEST(test_skills_list_empty_workspace_path);
    HU_RUN_TEST(test_skills_list_dot_workspace);
    HU_RUN_TEST(test_skill_registry_search_mock);
    HU_RUN_TEST(test_skill_registry_install_mock);
    HU_RUN_TEST(test_skill_registry_uninstall_mock);
    HU_RUN_TEST(test_skill_registry_get_installed_dir);
    HU_RUN_TEST(test_skill_registry_publish_mock);
    HU_RUN_TEST(test_skill_registry_publish_null_dir);
    HU_RUN_TEST(test_skill_registry_search_empty_query);
    HU_RUN_TEST(test_skill_registry_search_null_query);
    HU_RUN_TEST(test_skill_registry_install_null_name);
    HU_RUN_TEST(test_skill_registry_install_empty_name);
    HU_RUN_TEST(test_skill_registry_uninstall_null_name);
    HU_RUN_TEST(test_skill_registry_update_null_alloc);
    HU_RUN_TEST(test_skill_registry_get_installed_dir_writes_path);
    HU_RUN_TEST(test_skill_registry_get_installed_dir_small_buffer);
    HU_RUN_TEST(test_skill_registry_search_mock_returns_entries);
    HU_RUN_TEST(test_skill_registry_entries_free_null_safe);
}
