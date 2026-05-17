#include "test_framework.h"
#include "human/daemon_reaction_poll.h"
#include "human/config.h"

static void test_daemon_tick_no_op_when_reaction_collection_disabled(void) {
    hu_reaction_collection_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.enabled = false;
    int64_t last = 500;
    int64_t watermark = 400;
    HU_ASSERT_EQ(hu_daemon_tick_reaction_poll(&cfg, 1000, &last, &watermark), HU_OK);
    HU_ASSERT_EQ(last, 500);
}

static void test_daemon_tick_polls_only_after_interval_with_fake_clock(void) {
    hu_daemon_reaction_poll_reset_count_for_test();
    hu_reaction_collection_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.enabled = true;
    cfg.poll_interval_seconds = 30;
    snprintf(cfg.channels[0], sizeof(cfg.channels[0]), "imessage");
    cfg.channel_count = 1;
    setenv("HU_CHATDB", "/tmp/nonexistent-chatdb.sqlite", 1);

    int64_t last = 0;
    int64_t watermark = 1000;
    HU_ASSERT_EQ(hu_daemon_tick_reaction_poll(&cfg, 1000, &last, &watermark), HU_OK);
    HU_ASSERT_EQ(last, 1000);
    HU_ASSERT_EQ(hu_daemon_reaction_poll_get_count_for_test(), 1);

    HU_ASSERT_EQ(hu_daemon_tick_reaction_poll(&cfg, 1025, &last, &watermark), HU_OK);
    HU_ASSERT_EQ(last, 1000);
    HU_ASSERT_EQ(hu_daemon_reaction_poll_get_count_for_test(), 1);

    HU_ASSERT_EQ(hu_daemon_tick_reaction_poll(&cfg, 1030, &last, &watermark), HU_OK);
    HU_ASSERT_EQ(last, 1030);
    HU_ASSERT_EQ(hu_daemon_reaction_poll_get_count_for_test(), 2);

    HU_ASSERT_EQ(hu_daemon_tick_reaction_poll(&cfg, 1045, &last, &watermark), HU_OK);
    HU_ASSERT_EQ(last, 1030);
    HU_ASSERT_EQ(hu_daemon_reaction_poll_get_count_for_test(), 2);

    HU_ASSERT_EQ(hu_daemon_tick_reaction_poll(&cfg, 1060, &last, &watermark), HU_OK);
    HU_ASSERT_EQ(last, 1060);
    HU_ASSERT_EQ(hu_daemon_reaction_poll_get_count_for_test(), 3);
}

static void test_daemon_tick_advances_watermark_after_poll(void) {
    hu_reaction_collection_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.enabled = true;
    snprintf(cfg.chatdb_path, sizeof(cfg.chatdb_path), "/tmp/nonexistent-chatdb.sqlite");
    int64_t last = 0;
    int64_t watermark = 0;
    HU_ASSERT_EQ(hu_daemon_tick_reaction_poll(&cfg, 2000, &last, &watermark), HU_OK);
    HU_ASSERT_EQ(watermark, 2000);
    HU_ASSERT_EQ(last, 2000);
}

void run_daemon_reaction_poll_production_tests(void) {
    HU_TEST_SUITE("daemon_reaction_poll_production");
    HU_RUN_TEST(test_daemon_tick_no_op_when_reaction_collection_disabled);
    HU_RUN_TEST(test_daemon_tick_polls_only_after_interval_with_fake_clock);
    HU_RUN_TEST(test_daemon_tick_advances_watermark_after_poll);
}
