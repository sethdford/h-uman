#include "human/context/context_ext.h"
#include "human/core/allocator.h"
#include "human/core/string.h"
#include "test_framework.h"
#include <stdint.h>
#include <string.h>
static void test_forwarding_query_sql_valid(void) {
    char buf[512];
    size_t len = 0;
    hu_error_t err = hu_forwarding_query_for_contact_sql("alice", 5, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "SELECT") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "alice") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "ORDER BY share_score") != NULL);
}
static void test_events_create_table_valid(void) {
    char buf[1024];
    size_t len = 0;
    hu_error_t err = hu_events_create_table_sql(buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "CREATE TABLE") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "current_events") != NULL);
}
void run_context_ext_tests(void) {
    HU_TEST_SUITE("context_ext");
    HU_RUN_TEST(test_forwarding_query_sql_valid);
    HU_RUN_TEST(test_events_create_table_valid);
}
