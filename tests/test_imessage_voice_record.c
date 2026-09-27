/* tests/test_imessage_voice_record.c
 *
 * Native Messages voice delivery (W3): an iMessage voice reply recorded by
 * Messages itself through a BlackHole input instead of sent as a file. The
 * policy and orchestrator are pure; every real-world effect sits behind
 * hu_voice_record_port_t, so these tests touch no audio, AX or chat.db. */
#include "human/channels/imessage_voice_record.h"
#include "test_framework.h"

#include <string.h>

static hu_voice_record_facts_t all_ok(void) {
    hu_voice_record_facts_t f = {.ax_trusted = true,
                                 .messages_running = true,
                                 .blackhole_present = true,
                                 .real_mic_configured = true,
                                 .real_mic_present = true,
                                 .real_mic_busy = false,
                                 .user_idle_sec = 120.0,
                                 .min_idle_sec = 20.0};
    return f;
}

static void test_vrec_mode_parse_defaults_to_attachment(void) {
    HU_ASSERT_EQ(hu_voice_delivery_mode_parse(NULL), HU_VOICE_DELIVERY_ATTACHMENT);
    HU_ASSERT_EQ(hu_voice_delivery_mode_parse(""), HU_VOICE_DELIVERY_ATTACHMENT);
    HU_ASSERT_EQ(hu_voice_delivery_mode_parse("live"), HU_VOICE_DELIVERY_ATTACHMENT);
    HU_ASSERT_EQ(hu_voice_delivery_mode_parse("shadow"), HU_VOICE_DELIVERY_SHADOW);
    HU_ASSERT_EQ(hu_voice_delivery_mode_parse("messages"), HU_VOICE_DELIVERY_MESSAGES);
}

static void test_vrec_memo_send_requires_empty_text_and_one_audio(void) {
    const char *caf[] = {"/tmp/a/Audio Message.caf"};
    const char *mp3[] = {"/tmp/human_dtts_1.mp3"};
    const char *png[] = {"/tmp/pic.png"};
    const char *two[] = {"/tmp/a.caf", "/tmp/b.caf"};
    HU_ASSERT_TRUE(hu_voice_record_is_memo_send(0, caf, 1));
    HU_ASSERT_TRUE(hu_voice_record_is_memo_send(0, mp3, 1));
    HU_ASSERT_FALSE(hu_voice_record_is_memo_send(5, caf, 1));
    HU_ASSERT_FALSE(hu_voice_record_is_memo_send(0, png, 1));
    HU_ASSERT_FALSE(hu_voice_record_is_memo_send(0, two, 2));
    HU_ASSERT_FALSE(hu_voice_record_is_memo_send(0, NULL, 0));
}

static void test_vrec_preflight_ok_when_all_facts_hold(void) {
    hu_voice_record_facts_t f = all_ok();
    HU_ASSERT_EQ(hu_voice_record_preflight(&f), HU_VREC_OK);
}

static void test_vrec_preflight_blocks_each_fact(void) {
    hu_voice_record_facts_t f;
    f = all_ok();
    f.ax_trusted = false;
    HU_ASSERT_EQ(hu_voice_record_preflight(&f), HU_VREC_NO_AX);
    f = all_ok();
    f.messages_running = false;
    HU_ASSERT_EQ(hu_voice_record_preflight(&f), HU_VREC_NO_MESSAGES);
    f = all_ok();
    f.blackhole_present = false;
    HU_ASSERT_EQ(hu_voice_record_preflight(&f), HU_VREC_NO_BLACKHOLE);
    f = all_ok();
    f.real_mic_configured = false;
    HU_ASSERT_EQ(hu_voice_record_preflight(&f), HU_VREC_NO_REAL_MIC);
    f = all_ok();
    f.real_mic_present = false;
    HU_ASSERT_EQ(hu_voice_record_preflight(&f), HU_VREC_NO_REAL_MIC);
    f = all_ok();
    f.real_mic_busy = true;
    HU_ASSERT_EQ(hu_voice_record_preflight(&f), HU_VREC_MIC_BUSY);
    f = all_ok();
    f.user_idle_sec = 5.0;
    HU_ASSERT_EQ(hu_voice_record_preflight(&f), HU_VREC_USER_ACTIVE);
    HU_ASSERT_EQ(hu_voice_record_preflight(NULL), HU_VREC_NO_AX);
}

static void test_vrec_timing_stays_in_human_ranges(void) {
    for (uint32_t s = 0; s < 500; s++) {
        hu_voice_record_timing_t t;
        hu_voice_record_timing(s * 2654435761u, &t);
        HU_ASSERT_TRUE(t.lead_in_ms >= 350 && t.lead_in_ms <= 700);
        HU_ASSERT_TRUE(t.tail_ms >= 500 && t.tail_ms <= 900);
    }
}

static void test_vrec_block_names_are_distinct(void) {
    HU_ASSERT_STR_EQ(hu_voice_record_block_name(HU_VREC_OK), "ok");
    HU_ASSERT_STR_EQ(hu_voice_record_block_name(HU_VREC_MIC_BUSY), "mic_busy");
    HU_ASSERT_STR_EQ(hu_voice_record_block_name(HU_VREC_USER_ACTIVE), "user_active");
}

static void test_vrec_route_only_memo_sends_in_non_attachment_modes(void) {
    const char *caf[] = {"/tmp/Audio Message.caf"};
    HU_ASSERT_EQ(hu_voice_record_route(HU_VOICE_DELIVERY_ATTACHMENT, 0, caf, 1),
                 HU_VREC_ROUTE_ATTACHMENT);
    HU_ASSERT_EQ(hu_voice_record_route(HU_VOICE_DELIVERY_SHADOW, 0, caf, 1), HU_VREC_ROUTE_SHADOW);
    HU_ASSERT_EQ(hu_voice_record_route(HU_VOICE_DELIVERY_MESSAGES, 0, caf, 1),
                 HU_VREC_ROUTE_RECORD);
    HU_ASSERT_EQ(hu_voice_record_route(HU_VOICE_DELIVERY_MESSAGES, 4, caf, 1),
                 HU_VREC_ROUTE_ATTACHMENT);
    HU_ASSERT_EQ(hu_voice_record_route(HU_VOICE_DELIVERY_MESSAGES, 4, NULL, 0),
                 HU_VREC_ROUTE_ATTACHMENT);
}

void run_imessage_voice_record_tests(void) {
    HU_TEST_SUITE("imessage voice record");
    HU_RUN_TEST(test_vrec_mode_parse_defaults_to_attachment);
    HU_RUN_TEST(test_vrec_memo_send_requires_empty_text_and_one_audio);
    HU_RUN_TEST(test_vrec_preflight_ok_when_all_facts_hold);
    HU_RUN_TEST(test_vrec_preflight_blocks_each_fact);
    HU_RUN_TEST(test_vrec_timing_stays_in_human_ranges);
    HU_RUN_TEST(test_vrec_block_names_are_distinct);
    HU_RUN_TEST(test_vrec_route_only_memo_sends_in_non_attachment_modes);
}
