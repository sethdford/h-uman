#include "test_framework.h"
#include "human/daemon_reaction_poll.h"
#include "human/config.h"

static void test_daemon_does_not_call_poll_when_feature_flag_off(void) {
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.reaction_collection.enabled = false;
    int poll_call_count = 0;
    hu_daemon_set_poll_call_counter_for_test(&poll_call_count);
    HU_ASSERT_EQ(hu_daemon_tick_for_test(&cfg), HU_OK);
    HU_ASSERT_EQ(poll_call_count, 0);
    hu_daemon_set_poll_call_counter_for_test(NULL);
}

static void test_daemon_calls_poll_when_feature_flag_on(void) {
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.reaction_collection.enabled = true;
    snprintf(cfg.reaction_collection.channels[0],
             sizeof(cfg.reaction_collection.channels[0]), "imessage");
    cfg.reaction_collection.channel_count = 1;
    setenv("HU_CHATDB", "/tmp/nonexistent-chatdb.sqlite", 1);
    int poll_call_count = 0;
    hu_daemon_set_poll_call_counter_for_test(&poll_call_count);
    HU_ASSERT_EQ(hu_daemon_tick_for_test(&cfg), HU_OK);
    HU_ASSERT_TRUE(poll_call_count >= 1);
    hu_daemon_set_poll_call_counter_for_test(NULL);
}

void run_daemon_reaction_poll_tests(void) {
    HU_TEST_SUITE("daemon-reaction-poll");
    HU_RUN_TEST(test_daemon_does_not_call_poll_when_feature_flag_off);
    HU_RUN_TEST(test_daemon_calls_poll_when_feature_flag_on);
}
