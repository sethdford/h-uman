/* Identity management tests */
#include "human/identity.h"
#include "test_framework.h"
#include <string.h>

static void test_identity_build_unified(void) {
    hu_identity_t identity;
    memset(&identity, 0, sizeof(identity));
    hu_identity_build_unified(&identity, "telegram", NULL, "user123");
    HU_ASSERT_TRUE(strcmp(identity.channel, "telegram") == 0);
    HU_ASSERT_TRUE(strcmp(identity.user_id, "user123") == 0);
    HU_ASSERT_TRUE(strstr(identity.unified_id, "telegram") != NULL);
    HU_ASSERT_TRUE(strstr(identity.unified_id, "user123") != NULL);
}

static void test_identity_build_with_account(void) {
    hu_identity_t identity;
    memset(&identity, 0, sizeof(identity));
    hu_identity_build_unified(&identity, "slack", "acct_1", "U12345");
    HU_ASSERT_TRUE(strstr(identity.unified_id, "slack") != NULL);
    HU_ASSERT_TRUE(strstr(identity.unified_id, "U12345") != NULL);
}

static void test_identity_resolve_level_default(void) {
    const char *allowlist[] = {"cli:user_x"};
    hu_permission_level_t level = hu_identity_resolve_level("cli:user_y", allowlist, 1);
    HU_ASSERT_EQ(level, HU_PERM_USER); /* default when not in allowlist */
}

static void test_identity_resolve_level_allowlist(void) {
    const char *allowlist[] = {"cli:admin_user"};
    hu_permission_level_t level = hu_identity_resolve_level("cli:admin_user", allowlist, 1);
    HU_ASSERT_EQ(level, HU_PERM_ADMIN); /* in allowlist = admin level */
}

static void test_identity_has_permission(void) {
    HU_ASSERT_TRUE(hu_identity_has_permission(HU_PERM_ADMIN, HU_PERM_USER));
    HU_ASSERT_TRUE(hu_identity_has_permission(HU_PERM_USER, HU_PERM_USER));
    HU_ASSERT_FALSE(hu_identity_has_permission(HU_PERM_VIEWER, HU_PERM_USER));
    HU_ASSERT_FALSE(hu_identity_has_permission(HU_PERM_BLOCKED, HU_PERM_VIEWER));
}

static void test_identity_bot_name_extraction(void) {
    hu_identity_t identity;
    memset(&identity, 0, sizeof(identity));
    hu_identity_build_unified(&identity, "cli", NULL, "default");
    HU_ASSERT_TRUE(strcmp(identity.channel, "cli") == 0);
    HU_ASSERT_TRUE(strcmp(identity.user_id, "default") == 0);
}

static void test_identity_permission_level_hierarchy(void) {
    HU_ASSERT_TRUE(hu_identity_has_permission(HU_PERM_ADMIN, HU_PERM_ADMIN));
    HU_ASSERT_TRUE(hu_identity_has_permission(HU_PERM_ADMIN, HU_PERM_VIEWER));
    HU_ASSERT_TRUE(hu_identity_has_permission(HU_PERM_USER, HU_PERM_VIEWER));
    HU_ASSERT_TRUE(hu_identity_has_permission(HU_PERM_VIEWER, HU_PERM_VIEWER));
    HU_ASSERT_FALSE(hu_identity_has_permission(HU_PERM_BLOCKED, HU_PERM_VIEWER));
    HU_ASSERT_FALSE(hu_identity_has_permission(HU_PERM_VIEWER, HU_PERM_USER));
}

static void test_identity_resolve_level_empty_allowlist(void) {
    hu_permission_level_t level = hu_identity_resolve_level("any:id", NULL, 0);
    HU_ASSERT_EQ(level, HU_PERM_USER);
}

static void test_identity_resolve_level_multiple_allowlist(void) {
    const char *allowlist[] = {"cli:admin1", "cli:admin2", "telegram:admin3"};
    hu_permission_level_t l1 = hu_identity_resolve_level("cli:admin1", allowlist, 3);
    hu_permission_level_t l2 = hu_identity_resolve_level("cli:admin2", allowlist, 3);
    hu_permission_level_t l3 = hu_identity_resolve_level("telegram:admin3", allowlist, 3);
    hu_permission_level_t l4 = hu_identity_resolve_level("cli:random", allowlist, 3);
    HU_ASSERT_EQ(l1, HU_PERM_ADMIN);
    HU_ASSERT_EQ(l2, HU_PERM_ADMIN);
    HU_ASSERT_EQ(l3, HU_PERM_ADMIN);
    HU_ASSERT_EQ(l4, HU_PERM_USER);
}

static void test_identity_build_unified_null_account(void) {
    hu_identity_t identity;
    memset(&identity, 0, sizeof(identity));
    hu_identity_build_unified(&identity, "slack", NULL, "U123");
    HU_ASSERT_TRUE(strstr(identity.unified_id, "slack") != NULL);
    HU_ASSERT_TRUE(strstr(identity.unified_id, "U123") != NULL);
}

static void test_identity_role_matching_admin(void) {
    const char *allowlist[] = {"discord:guild1:admin_role"};
    hu_permission_level_t level =
        hu_identity_resolve_level("discord:guild1:admin_role", allowlist, 1);
    HU_ASSERT_EQ(level, HU_PERM_ADMIN);
}

static void test_identity_unified_format(void) {
    hu_identity_t identity;
    memset(&identity, 0, sizeof(identity));
    hu_identity_build_unified(&identity, "web", "acct_x", "user_y");
    HU_ASSERT_TRUE(strlen(identity.unified_id) > 0);
    HU_ASSERT_TRUE(strstr(identity.unified_id, "web") != NULL);
    HU_ASSERT_TRUE(strstr(identity.unified_id, "user_y") != NULL);
}

static void test_identity_bot_name_multi_channel(void) {
    hu_identity_t a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    hu_identity_build_unified(&a, "telegram", NULL, "bot_1");
    hu_identity_build_unified(&b, "discord", NULL, "bot_2");
    HU_ASSERT_STR_EQ(a.channel, "telegram");
    HU_ASSERT_STR_EQ(b.channel, "discord");
    HU_ASSERT_TRUE(strcmp(a.unified_id, b.unified_id) != 0);
}

static void test_identity_permission_blocked_nothing(void) {
    HU_ASSERT_FALSE(hu_identity_has_permission(HU_PERM_BLOCKED, HU_PERM_VIEWER));
    HU_ASSERT_FALSE(hu_identity_has_permission(HU_PERM_BLOCKED, HU_PERM_USER));
}

static void test_identity_viewer_read_only(void) {
    HU_ASSERT_TRUE(hu_identity_has_permission(HU_PERM_VIEWER, HU_PERM_VIEWER));
    HU_ASSERT_FALSE(hu_identity_has_permission(HU_PERM_VIEWER, HU_PERM_USER));
}

static void test_identity_inheritance_admin_has_all(void) {
    HU_ASSERT_TRUE(hu_identity_has_permission(HU_PERM_ADMIN, HU_PERM_ADMIN));
    HU_ASSERT_TRUE(hu_identity_has_permission(HU_PERM_ADMIN, HU_PERM_USER));
    HU_ASSERT_TRUE(hu_identity_has_permission(HU_PERM_ADMIN, HU_PERM_VIEWER));
}

static void test_identity_role_specific_channel(void) {
    const char *allowlist[] = {"telegram:admin1", "discord:admin2"};
    hu_permission_level_t l1 = hu_identity_resolve_level("telegram:admin1", allowlist, 2);
    hu_permission_level_t l2 = hu_identity_resolve_level("discord:random", allowlist, 2);
    HU_ASSERT_EQ(l1, HU_PERM_ADMIN);
    HU_ASSERT_EQ(l2, HU_PERM_USER);
}

void run_identity_tests(void) {
    HU_TEST_SUITE("Identity");
    HU_RUN_TEST(test_identity_build_unified);
    HU_RUN_TEST(test_identity_build_with_account);
    HU_RUN_TEST(test_identity_resolve_level_default);
    HU_RUN_TEST(test_identity_resolve_level_allowlist);
    HU_RUN_TEST(test_identity_has_permission);
    HU_RUN_TEST(test_identity_bot_name_extraction);
    HU_RUN_TEST(test_identity_permission_level_hierarchy);
    HU_RUN_TEST(test_identity_resolve_level_empty_allowlist);
    HU_RUN_TEST(test_identity_resolve_level_multiple_allowlist);
    HU_RUN_TEST(test_identity_build_unified_null_account);
    HU_RUN_TEST(test_identity_role_matching_admin);
    HU_RUN_TEST(test_identity_unified_format);
    HU_RUN_TEST(test_identity_bot_name_multi_channel);
    HU_RUN_TEST(test_identity_permission_blocked_nothing);
    HU_RUN_TEST(test_identity_viewer_read_only);
    HU_RUN_TEST(test_identity_inheritance_admin_has_all);
    HU_RUN_TEST(test_identity_role_specific_channel);
}
