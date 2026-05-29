#include "human/daemon/director.h"
#include "test_framework.h"
#include <string.h>

static void director_classifies_greeting(void) {
    char out_type[16];
    hu_daemon_classify_comfort_response_type("hey, how are you?", 17, out_type, sizeof(out_type));
    HU_ASSERT_STR_EQ(out_type, "space");
}

static void director_classifies_comfort(void) {
    char out_type[16];
    hu_daemon_classify_comfort_response_type("I'm so sorry you're going through that", 38, out_type,
                                             sizeof(out_type));
    HU_ASSERT_STR_EQ(out_type, "empathy");
}

void run_daemon_director_tests(void) {
    HU_TEST_SUITE("daemon_director");
    HU_RUN_TEST(director_classifies_greeting);
    HU_RUN_TEST(director_classifies_comfort);
}
