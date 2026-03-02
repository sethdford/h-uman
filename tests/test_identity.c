/* Identity management tests */
#include "test_framework.h"
#include "seaclaw/identity.h"
#include <string.h>

static void test_identity_build_unified(void) {
    sc_identity_t identity;
    memset(&identity, 0, sizeof(identity));
    sc_identity_build_unified(&identity, "telegram", NULL, "user123");
    SC_ASSERT_TRUE(strcmp(identity.channel, "telegram") == 0);
    SC_ASSERT_TRUE(strcmp(identity.user_id, "user123") == 0);
    SC_ASSERT_TRUE(strstr(identity.unified_id, "telegram") != NULL);
    SC_ASSERT_TRUE(strstr(identity.unified_id, "user123") != NULL);
}

static void test_identity_build_with_account(void) {
    sc_identity_t identity;
    memset(&identity, 0, sizeof(identity));
    sc_identity_build_unified(&identity, "slack", "acct_1", "U12345");
    SC_ASSERT_TRUE(strstr(identity.unified_id, "slack") != NULL);
    SC_ASSERT_TRUE(strstr(identity.unified_id, "U12345") != NULL);
}

static void test_identity_resolve_level_default(void) {
    const char *allowlist[] = {"cli:user_x"};
    sc_permission_level_t level = sc_identity_resolve_level("cli:user_y", allowlist, 1);
    SC_ASSERT_EQ(level, SC_PERM_USER); /* default when not in allowlist */
}

static void test_identity_resolve_level_allowlist(void) {
    const char *allowlist[] = {"cli:admin_user"};
    sc_permission_level_t level = sc_identity_resolve_level("cli:admin_user", allowlist, 1);
    SC_ASSERT_EQ(level, SC_PERM_ADMIN); /* in allowlist = admin level */
}

static void test_identity_has_permission(void) {
    SC_ASSERT_TRUE(sc_identity_has_permission(SC_PERM_ADMIN, SC_PERM_USER));
    SC_ASSERT_TRUE(sc_identity_has_permission(SC_PERM_USER, SC_PERM_USER));
    SC_ASSERT_FALSE(sc_identity_has_permission(SC_PERM_VIEWER, SC_PERM_USER));
    SC_ASSERT_FALSE(sc_identity_has_permission(SC_PERM_BLOCKED, SC_PERM_VIEWER));
}

static void test_identity_bot_name_extraction(void) {
    sc_identity_t identity;
    memset(&identity, 0, sizeof(identity));
    sc_identity_build_unified(&identity, "cli", NULL, "default");
    SC_ASSERT_TRUE(strcmp(identity.channel, "cli") == 0);
    SC_ASSERT_TRUE(strcmp(identity.user_id, "default") == 0);
}

static void test_identity_permission_level_hierarchy(void) {
    SC_ASSERT_TRUE(sc_identity_has_permission(SC_PERM_ADMIN, SC_PERM_ADMIN));
    SC_ASSERT_TRUE(sc_identity_has_permission(SC_PERM_ADMIN, SC_PERM_VIEWER));
    SC_ASSERT_TRUE(sc_identity_has_permission(SC_PERM_USER, SC_PERM_VIEWER));
    SC_ASSERT_TRUE(sc_identity_has_permission(SC_PERM_VIEWER, SC_PERM_VIEWER));
    SC_ASSERT_FALSE(sc_identity_has_permission(SC_PERM_BLOCKED, SC_PERM_VIEWER));
    SC_ASSERT_FALSE(sc_identity_has_permission(SC_PERM_VIEWER, SC_PERM_USER));
}

static void test_identity_resolve_level_empty_allowlist(void) {
    sc_permission_level_t level = sc_identity_resolve_level("any:id", NULL, 0);
    SC_ASSERT_EQ(level, SC_PERM_USER);
}

static void test_identity_resolve_level_multiple_allowlist(void) {
    const char *allowlist[] = {"cli:admin1", "cli:admin2", "telegram:admin3"};
    sc_permission_level_t l1 = sc_identity_resolve_level("cli:admin1", allowlist, 3);
    sc_permission_level_t l2 = sc_identity_resolve_level("cli:admin2", allowlist, 3);
    sc_permission_level_t l3 = sc_identity_resolve_level("telegram:admin3", allowlist, 3);
    sc_permission_level_t l4 = sc_identity_resolve_level("cli:random", allowlist, 3);
    SC_ASSERT_EQ(l1, SC_PERM_ADMIN);
    SC_ASSERT_EQ(l2, SC_PERM_ADMIN);
    SC_ASSERT_EQ(l3, SC_PERM_ADMIN);
    SC_ASSERT_EQ(l4, SC_PERM_USER);
}

static void test_identity_build_unified_null_account(void) {
    sc_identity_t identity;
    memset(&identity, 0, sizeof(identity));
    sc_identity_build_unified(&identity, "slack", NULL, "U123");
    SC_ASSERT_TRUE(strstr(identity.unified_id, "slack") != NULL);
    SC_ASSERT_TRUE(strstr(identity.unified_id, "U123") != NULL);
}

static void test_identity_role_matching_admin(void) {
    const char *allowlist[] = {"discord:guild1:admin_role"};
    sc_permission_level_t level = sc_identity_resolve_level("discord:guild1:admin_role", allowlist, 1);
    SC_ASSERT_EQ(level, SC_PERM_ADMIN);
}

static void test_identity_unified_format(void) {
    sc_identity_t identity;
    memset(&identity, 0, sizeof(identity));
    sc_identity_build_unified(&identity, "web", "acct_x", "user_y");
    SC_ASSERT_TRUE(strlen(identity.unified_id) > 0);
    SC_ASSERT_TRUE(strstr(identity.unified_id, "web") != NULL);
    SC_ASSERT_TRUE(strstr(identity.unified_id, "user_y") != NULL);
}

static void test_identity_bot_name_multi_channel(void) {
    sc_identity_t a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    sc_identity_build_unified(&a, "telegram", NULL, "bot_1");
    sc_identity_build_unified(&b, "discord", NULL, "bot_2");
    SC_ASSERT_STR_EQ(a.channel, "telegram");
    SC_ASSERT_STR_EQ(b.channel, "discord");
    SC_ASSERT_TRUE(strcmp(a.unified_id, b.unified_id) != 0);
}

static void test_identity_permission_blocked_nothing(void) {
    SC_ASSERT_FALSE(sc_identity_has_permission(SC_PERM_BLOCKED, SC_PERM_VIEWER));
    SC_ASSERT_FALSE(sc_identity_has_permission(SC_PERM_BLOCKED, SC_PERM_USER));
}

static void test_identity_viewer_read_only(void) {
    SC_ASSERT_TRUE(sc_identity_has_permission(SC_PERM_VIEWER, SC_PERM_VIEWER));
    SC_ASSERT_FALSE(sc_identity_has_permission(SC_PERM_VIEWER, SC_PERM_USER));
}

static void test_identity_inheritance_admin_has_all(void) {
    SC_ASSERT_TRUE(sc_identity_has_permission(SC_PERM_ADMIN, SC_PERM_ADMIN));
    SC_ASSERT_TRUE(sc_identity_has_permission(SC_PERM_ADMIN, SC_PERM_USER));
    SC_ASSERT_TRUE(sc_identity_has_permission(SC_PERM_ADMIN, SC_PERM_VIEWER));
}

static void test_identity_role_specific_channel(void) {
    const char *allowlist[] = {"telegram:admin1", "discord:admin2"};
    sc_permission_level_t l1 = sc_identity_resolve_level("telegram:admin1", allowlist, 2);
    sc_permission_level_t l2 = sc_identity_resolve_level("discord:random", allowlist, 2);
    SC_ASSERT_EQ(l1, SC_PERM_ADMIN);
    SC_ASSERT_EQ(l2, SC_PERM_USER);
}

void run_identity_tests(void) {
    SC_TEST_SUITE("Identity");
    SC_RUN_TEST(test_identity_build_unified);
    SC_RUN_TEST(test_identity_build_with_account);
    SC_RUN_TEST(test_identity_resolve_level_default);
    SC_RUN_TEST(test_identity_resolve_level_allowlist);
    SC_RUN_TEST(test_identity_has_permission);
    SC_RUN_TEST(test_identity_bot_name_extraction);
    SC_RUN_TEST(test_identity_permission_level_hierarchy);
    SC_RUN_TEST(test_identity_resolve_level_empty_allowlist);
    SC_RUN_TEST(test_identity_resolve_level_multiple_allowlist);
    SC_RUN_TEST(test_identity_build_unified_null_account);
    SC_RUN_TEST(test_identity_role_matching_admin);
    SC_RUN_TEST(test_identity_unified_format);
    SC_RUN_TEST(test_identity_bot_name_multi_channel);
    SC_RUN_TEST(test_identity_permission_blocked_nothing);
    SC_RUN_TEST(test_identity_viewer_read_only);
    SC_RUN_TEST(test_identity_inheritance_admin_has_all);
    SC_RUN_TEST(test_identity_role_specific_channel);
}
