/* Path security tests */
#include "human/core/allocator.h"
#include "human/tools/path_security.h"
#include "test_framework.h"

static void test_path_is_safe_relative_ok(void) {
    HU_ASSERT_TRUE(hu_path_is_safe("foo"));
    HU_ASSERT_TRUE(hu_path_is_safe("foo/bar"));
    HU_ASSERT_TRUE(hu_path_is_safe("subdir/file.txt"));
}

static void test_path_is_safe_traversal_rejected(void) {
    HU_ASSERT_FALSE(hu_path_is_safe(".."));
    HU_ASSERT_FALSE(hu_path_is_safe("../etc/passwd"));
    HU_ASSERT_FALSE(hu_path_is_safe("foo/../etc"));
    HU_ASSERT_FALSE(hu_path_is_safe("./.."));
}

static void test_path_is_safe_encoded_traversal_rejected(void) {
    HU_ASSERT_FALSE(hu_path_is_safe("..%2fetc"));
    HU_ASSERT_FALSE(hu_path_is_safe("%2f.."));
    HU_ASSERT_FALSE(hu_path_is_safe("foo/..%5c"));
    HU_ASSERT_FALSE(hu_path_is_safe("%5c.."));
}

static void test_path_is_safe_absolute_rejected(void) {
    HU_ASSERT_FALSE(hu_path_is_safe("/etc/passwd"));
    HU_ASSERT_FALSE(hu_path_is_safe("/"));
#ifdef _WIN32
    HU_ASSERT_FALSE(hu_path_is_safe("C:\\Windows"));
#endif
}

static void test_path_is_safe_null_rejected(void) {
    HU_ASSERT_FALSE(hu_path_is_safe(NULL));
}

static void test_path_resolved_allowed_workspace_match(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *resolved = "/home/user/proj/foo.txt";
    const char *ws = "/home/user/proj";
    const char **allowed = NULL;
    HU_ASSERT_TRUE(hu_path_resolved_allowed(&alloc, resolved, ws, allowed, 0));
}

static void test_path_resolved_allowed_system_blocked(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *allowed[] = {"*"};
    HU_ASSERT_FALSE(hu_path_resolved_allowed(&alloc, "/etc/passwd", NULL, allowed, 1));
    HU_ASSERT_FALSE(hu_path_resolved_allowed(&alloc, "/bin/sh", NULL, allowed, 1));
}

static void test_path_resolved_allowed_wildcard(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *allowed[] = {"*"};
    HU_ASSERT_TRUE(hu_path_resolved_allowed(&alloc, "/tmp/foo", NULL, allowed, 1));
}

static void test_path_resolved_allowed_prefix_match(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *allowed[] = {"/tmp"};
    HU_ASSERT_TRUE(hu_path_resolved_allowed(&alloc, "/tmp/foo", NULL, allowed, 1));
    HU_ASSERT_FALSE(hu_path_resolved_allowed(&alloc, "/var/tmp/foo", NULL, allowed, 1));
}

static void test_path_traversal_double_dot_slash(void) {
    HU_ASSERT_FALSE(hu_path_is_safe("../foo"));
}

static void test_path_traversal_slash_dot_dot(void) {
    HU_ASSERT_FALSE(hu_path_is_safe("foo/../bar"));
}

static void test_path_traversal_encoded_backslash(void) {
    /* Implementation rejects ..%5c and ..%2f; use a known-rejected pattern */
    HU_ASSERT_FALSE(hu_path_is_safe("..%5cetc"));
}

static void test_path_traversal_mixed_slash(void) {
    HU_ASSERT_FALSE(hu_path_is_safe("a/../b"));
}

static void test_path_safe_simple_file(void) {
    HU_ASSERT_TRUE(hu_path_is_safe("file.txt"));
}

static void test_path_safe_nested_dir(void) {
    HU_ASSERT_TRUE(hu_path_is_safe("a/b/c.txt"));
}

static void test_path_resolved_allowed_same_prefix(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *allowed[] = {"/home/x"};
    HU_ASSERT_TRUE(hu_path_resolved_allowed(&alloc, "/home/x/file", NULL, allowed, 1));
}

static void test_path_resolved_denied_sibling_dir(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *resolved = "/home/user/proj_sibling/secret";
    const char *ws = "/home/user/proj";
    const char **allowed = NULL;
    HU_ASSERT_FALSE(hu_path_resolved_allowed(&alloc, resolved, ws, allowed, 0));
}

void run_path_security_tests(void) {
    HU_TEST_SUITE("Path security");
    HU_RUN_TEST(test_path_is_safe_relative_ok);
    HU_RUN_TEST(test_path_is_safe_traversal_rejected);
    HU_RUN_TEST(test_path_is_safe_encoded_traversal_rejected);
    HU_RUN_TEST(test_path_is_safe_absolute_rejected);
    HU_RUN_TEST(test_path_is_safe_null_rejected);
    HU_RUN_TEST(test_path_resolved_allowed_workspace_match);
    HU_RUN_TEST(test_path_resolved_allowed_system_blocked);
    HU_RUN_TEST(test_path_resolved_allowed_wildcard);
    HU_RUN_TEST(test_path_resolved_allowed_prefix_match);
    HU_RUN_TEST(test_path_traversal_double_dot_slash);
    HU_RUN_TEST(test_path_traversal_slash_dot_dot);
    HU_RUN_TEST(test_path_traversal_encoded_backslash);
    HU_RUN_TEST(test_path_traversal_mixed_slash);
    HU_RUN_TEST(test_path_safe_simple_file);
    HU_RUN_TEST(test_path_safe_nested_dir);
    HU_RUN_TEST(test_path_resolved_allowed_same_prefix);
    HU_RUN_TEST(test_path_resolved_denied_sibling_dir);
}
