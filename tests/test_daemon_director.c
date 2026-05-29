#include "human/daemon/director.h"
#include "test_framework.h"
#include <string.h>

static void classify_comfort_response_type_returns_space_for_greeting(void) {
    const char *msg = "hey, how are you?";
    char out_type[16];
    hu_daemon_classify_comfort_response_type(msg, strlen(msg), out_type, sizeof(out_type));
    HU_ASSERT_STR_EQ(out_type, "space");
}

static void classify_comfort_response_type_returns_empathy_for_apology(void) {
    const char *msg = "I'm so sorry you're going through that";
    char out_type[16];
    hu_daemon_classify_comfort_response_type(msg, strlen(msg), out_type, sizeof(out_type));
    HU_ASSERT_STR_EQ(out_type, "empathy");
}

void run_daemon_director_tests(void) {
    HU_TEST_SUITE("daemon_director");
    HU_RUN_TEST(classify_comfort_response_type_returns_space_for_greeting);
    HU_RUN_TEST(classify_comfort_response_type_returns_empathy_for_apology);
}
