/* Path security tests */
#include "test_framework.h"
#include "seaclaw/tools/path_security.h"
#include "seaclaw/core/allocator.h"

static void test_path_is_safe_relative_ok(void) {
    SC_ASSERT_TRUE(sc_path_is_safe("foo"));
    SC_ASSERT_TRUE(sc_path_is_safe("foo/bar"));
    SC_ASSERT_TRUE(sc_path_is_safe("subdir/file.txt"));
}

static void test_path_is_safe_traversal_rejected(void) {
    SC_ASSERT_FALSE(sc_path_is_safe(".."));
    SC_ASSERT_FALSE(sc_path_is_safe("../etc/passwd"));
    SC_ASSERT_FALSE(sc_path_is_safe("foo/../etc"));
    SC_ASSERT_FALSE(sc_path_is_safe("./.."));
}

static void test_path_is_safe_encoded_traversal_rejected(void) {
    SC_ASSERT_FALSE(sc_path_is_safe("..%2fetc"));
    SC_ASSERT_FALSE(sc_path_is_safe("%2f.."));
    SC_ASSERT_FALSE(sc_path_is_safe("foo/..%5c"));
    SC_ASSERT_FALSE(sc_path_is_safe("%5c.."));
}

static void test_path_is_safe_absolute_rejected(void) {
    SC_ASSERT_FALSE(sc_path_is_safe("/etc/passwd"));
    SC_ASSERT_FALSE(sc_path_is_safe("/"));
#ifdef _WIN32
    SC_ASSERT_FALSE(sc_path_is_safe("C:\\Windows"));
#endif
}

static void test_path_is_safe_null_rejected(void) {
    SC_ASSERT_FALSE(sc_path_is_safe(NULL));
}

static void test_path_resolved_allowed_workspace_match(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *resolved = "/home/user/proj/foo.txt";
    const char *ws = "/home/user/proj";
    const char **allowed = NULL;
    SC_ASSERT_TRUE(sc_path_resolved_allowed(&alloc, resolved, ws, allowed, 0));
}

static void test_path_resolved_allowed_system_blocked(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *allowed[] = {"*"};
    SC_ASSERT_FALSE(sc_path_resolved_allowed(&alloc, "/etc/passwd", NULL, allowed, 1));
    SC_ASSERT_FALSE(sc_path_resolved_allowed(&alloc, "/bin/sh", NULL, allowed, 1));
}

static void test_path_resolved_allowed_wildcard(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *allowed[] = {"*"};
    SC_ASSERT_TRUE(sc_path_resolved_allowed(&alloc, "/tmp/foo", NULL, allowed, 1));
}

static void test_path_resolved_allowed_prefix_match(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *allowed[] = {"/tmp"};
    SC_ASSERT_TRUE(sc_path_resolved_allowed(&alloc, "/tmp/foo", NULL, allowed, 1));
    SC_ASSERT_FALSE(sc_path_resolved_allowed(&alloc, "/var/tmp/foo", NULL, allowed, 1));
}

static void test_path_traversal_double_dot_slash(void) {
    SC_ASSERT_FALSE(sc_path_is_safe("../foo"));
}

static void test_path_traversal_slash_dot_dot(void) {
    SC_ASSERT_FALSE(sc_path_is_safe("foo/../bar"));
}

static void test_path_traversal_encoded_backslash(void) {
    /* Implementation rejects ..%5c and ..%2f; use a known-rejected pattern */
    SC_ASSERT_FALSE(sc_path_is_safe("..%5cetc"));
}

static void test_path_traversal_mixed_slash(void) {
    SC_ASSERT_FALSE(sc_path_is_safe("a/../b"));
}

static void test_path_safe_simple_file(void) {
    SC_ASSERT_TRUE(sc_path_is_safe("file.txt"));
}

static void test_path_safe_nested_dir(void) {
    SC_ASSERT_TRUE(sc_path_is_safe("a/b/c.txt"));
}

static void test_path_resolved_allowed_same_prefix(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *allowed[] = {"/home/x"};
    SC_ASSERT_TRUE(sc_path_resolved_allowed(&alloc, "/home/x/file", NULL, allowed, 1));
}

static void test_path_resolved_denied_sibling_dir(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *resolved = "/home/user/proj_sibling/secret";
    const char *ws = "/home/user/proj";
    const char **allowed = NULL;
    SC_ASSERT_FALSE(sc_path_resolved_allowed(&alloc, resolved, ws, allowed, 0));
}

void run_path_security_tests(void) {
    SC_TEST_SUITE("Path security");
    SC_RUN_TEST(test_path_is_safe_relative_ok);
    SC_RUN_TEST(test_path_is_safe_traversal_rejected);
    SC_RUN_TEST(test_path_is_safe_encoded_traversal_rejected);
    SC_RUN_TEST(test_path_is_safe_absolute_rejected);
    SC_RUN_TEST(test_path_is_safe_null_rejected);
    SC_RUN_TEST(test_path_resolved_allowed_workspace_match);
    SC_RUN_TEST(test_path_resolved_allowed_system_blocked);
    SC_RUN_TEST(test_path_resolved_allowed_wildcard);
    SC_RUN_TEST(test_path_resolved_allowed_prefix_match);
    SC_RUN_TEST(test_path_traversal_double_dot_slash);
    SC_RUN_TEST(test_path_traversal_slash_dot_dot);
    SC_RUN_TEST(test_path_traversal_encoded_backslash);
    SC_RUN_TEST(test_path_traversal_mixed_slash);
    SC_RUN_TEST(test_path_safe_simple_file);
    SC_RUN_TEST(test_path_safe_nested_dir);
    SC_RUN_TEST(test_path_resolved_allowed_same_prefix);
    SC_RUN_TEST(test_path_resolved_denied_sibling_dir);
}
