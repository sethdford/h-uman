/* tests/test_voice_reply.c — hu_voice_reply_build_request / audio_to_temp.
 *
 * Pre/post contract throughout: the raw reply carries no tags; the built
 * request must. A tautology-free proof that the Ferni-derived prep layer is
 * what reaches Cartesia (it had zero production callers before 2026-09-20). */
#include "human/persona.h"
#include "human/tts/audio_pipeline.h"
#include "human/tts/cartesia.h"
#include "human/tts/voice_reply.h"
#include "test_framework.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static hu_persona_voice_config_t test_voice(void) {
    hu_persona_voice_config_t v;
    memset(&v, 0, sizeof(v));
    snprintf(v.provider, sizeof(v.provider), "cartesia");
    snprintf(v.voice_id, sizeof(v.voice_id), "test-voice-uuid");
    snprintf(v.default_emotion, sizeof(v.default_emotion), "content");
    v.default_speed = 0.85f;
    v.nonverbals = true;
    return v;
}

static const char *REPLY = "ok so i thought about it more. honestly that trip sounds amazing! "
                           "we should totally do it, what do you think";
static const char *INCOMING = "wanna go to zion in october";

static void test_voice_reply_build_request_rejects_null(void) {
    hu_persona_voice_config_t v = test_voice();
    hu_voice_reply_request_t req;
    HU_ASSERT_EQ(hu_voice_reply_build_request(NULL, REPLY, strlen(REPLY), NULL, 0, 14, 1, &req),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_voice_reply_build_request(&v, NULL, 0, NULL, 0, 14, 1, &req),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_voice_reply_build_request(&v, REPLY, strlen(REPLY), NULL, 0, 14, 1, NULL),
                 HU_ERR_INVALID_ARGUMENT);
}

static void test_voice_reply_build_request_annotates_text_speak(void) {
    hu_persona_voice_config_t v = test_voice();
    hu_voice_reply_request_t req;
    /* precondition: the raw reply is untagged text-speak */
    HU_ASSERT_NULL(strstr(REPLY, "<"));
    HU_ASSERT_EQ(hu_voice_reply_build_request(&v, REPLY, strlen(REPLY), INCOMING, strlen(INCOMING),
                                              14, 7, &req),
                 HU_OK);
    HU_ASSERT_TRUE(req.transcript_len > strlen(REPLY));
    HU_ASSERT_STR_CONTAINS(req.transcript, "<break time=");
    HU_ASSERT_STR_CONTAINS(req.transcript, "<emotion value=");
    HU_ASSERT_TRUE(req.sentence_count >= 2);
    HU_ASSERT_TRUE(req.emotion[0] != '\0');
    HU_ASSERT_STR_EQ(req.tts.model_id, HU_VOICE_REPLY_DEFAULT_MODEL);
    HU_ASSERT_STR_EQ(req.tts.voice_id, "test-voice-uuid");
    HU_ASSERT_STR_EQ(req.tts.emotion, req.emotion);
    HU_ASSERT_FLOAT_EQ(req.tts.speed, 0.85f, 0.001f);
    HU_ASSERT_TRUE(req.tts.volume >= 0.5f && req.tts.volume <= 2.0f);
    HU_ASSERT_TRUE(req.tts.nonverbals);
}

static void test_voice_reply_build_request_persona_model_wins(void) {
    hu_persona_voice_config_t v = test_voice();
    snprintf(v.model, sizeof(v.model), "sonic-3");
    hu_voice_reply_request_t req;
    HU_ASSERT_EQ(hu_voice_reply_build_request(&v, REPLY, strlen(REPLY), NULL, 0, 14, 7, &req),
                 HU_OK);
    HU_ASSERT_STR_EQ(req.tts.model_id, "sonic-3");
}

static void test_voice_reply_build_request_late_night_slows_request_speed(void) {
    hu_persona_voice_config_t v = test_voice();
    hu_voice_reply_request_t day, night;
    HU_ASSERT_EQ(hu_voice_reply_build_request(&v, REPLY, strlen(REPLY), NULL, 0, 14, 7, &day),
                 HU_OK);
    HU_ASSERT_EQ(hu_voice_reply_build_request(&v, REPLY, strlen(REPLY), NULL, 0, 23, 7, &night),
                 HU_OK);
    HU_ASSERT_FLOAT_EQ(day.tts.speed, 0.85f, 0.001f);
    HU_ASSERT_TRUE(night.tts.speed < day.tts.speed);
    HU_ASSERT_FLOAT_EQ(night.tts.speed, 0.85f * 0.92f, 0.005f);
}

/* Cartesia treats <speed ratio> as a multiplier on the request speed that
 * persists until the next tag. A slowed sentence must therefore be followed
 * by a 1.00 reset, and the tagged value must be RELATIVE, not absolute. */
static void test_voice_reply_speed_tags_are_relative_and_reset(void) {
    hu_persona_voice_config_t v = test_voice();
    v.nonverbals = false;
    const char *text = "That is such great news! I am really happy for you and the whole family.";
    hu_voice_reply_request_t req;
    HU_ASSERT_EQ(hu_voice_reply_build_request(&v, text, strlen(text), NULL, 0, 14, 3, &req), HU_OK);
    const char *first = strstr(req.transcript, "<speed ratio=\"");
    HU_ASSERT_NOT_NULL(first);
    double ratio = atof(first + strlen("<speed ratio=\""));
    HU_ASSERT_TRUE(ratio > 0.6 && ratio < 1.0); /* relative slowdown, not 0.85*x */
    HU_ASSERT_NOT_NULL(strstr(first + 1, "<speed ratio=\"1.00\"/>")); /* reset follows */
}

static void test_voice_reply_request_reaches_cartesia_with_ssml(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_voice_config_t v = test_voice();
    hu_voice_reply_request_t req;
    HU_ASSERT_EQ(hu_voice_reply_build_request(&v, REPLY, strlen(REPLY), INCOMING, strlen(INCOMING),
                                              14, 7, &req),
                 HU_OK);
    unsigned char *bytes = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_cartesia_tts_synthesize(&alloc, "test-key", 8, req.transcript,
                                            req.transcript_len, &req.tts, "mp3", &bytes, &len),
                 HU_OK);
    HU_ASSERT_TRUE(len > 0);
    /* The mock records what it was handed: the annotated transcript + config. */
    HU_ASSERT_STR_CONTAINS(hu_cartesia_test_last_transcript(), "<break time=");
    HU_ASSERT_STR_EQ(hu_cartesia_test_last_config()->model_id, HU_VOICE_REPLY_DEFAULT_MODEL);
    HU_ASSERT_FLOAT_EQ(hu_cartesia_test_last_config()->speed, 0.85f, 0.001f);
    HU_ASSERT_STR_EQ(hu_cartesia_test_last_config()->emotion, req.emotion);
    hu_cartesia_tts_free_bytes(&alloc, bytes, len);
}

static void test_voice_reply_audio_to_temp_imessage_yields_caf(void) {
    hu_allocator_t alloc = hu_system_allocator();
    unsigned char bytes[400];
    for (size_t i = 0; i < sizeof(bytes); i += 4) {
        bytes[i] = 0xFF;
        bytes[i + 1] = 0xFB;
        bytes[i + 2] = 0x90;
        bytes[i + 3] = 0x00;
    }
    char path[512] = {0};
    HU_ASSERT_EQ(
        hu_voice_reply_audio_to_temp(&alloc, "imessage", bytes, sizeof(bytes), path, sizeof(path)),
        HU_OK);
    HU_ASSERT_TRUE(path[0] != '\0');
    size_t pl = strlen(path);
#if defined(HU_IS_TEST) && HU_IS_TEST
    /* Under HU_IS_TEST hu_audio_mp3_to_caf skips afconvert and writes its
     * per-process mock as .mp3; the branch taken is still the CAF one. */
    HU_ASSERT_TRUE(pl > 4 && strcmp(path + pl - 4, ".mp3") == 0);
    HU_ASSERT_NOT_NULL(strstr(path, "human-voice-test-"));
#else
    HU_ASSERT_TRUE(pl > 4 && strcmp(path + pl - 4, ".caf") == 0);
#endif
    FILE *f = fopen(path, "rb");
    HU_ASSERT_NOT_NULL(f);
    if (f)
        fclose(f);
    hu_audio_cleanup_temp(path);
    HU_ASSERT_NULL(fopen(path, "rb"));
}

static void test_voice_reply_audio_to_temp_null_channel_still_writes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    unsigned char bytes[8] = {0xFF, 0xFB, 0x90, 0, 0xFF, 0xFB, 0x90, 0};
    char path[512] = {0};
    HU_ASSERT_EQ(
        hu_voice_reply_audio_to_temp(&alloc, NULL, bytes, sizeof(bytes), path, sizeof(path)),
        HU_OK);
    FILE *f = fopen(path, "rb");
    HU_ASSERT_NOT_NULL(f);
    if (f)
        fclose(f);
    hu_audio_cleanup_temp(path);
    HU_ASSERT_EQ(hu_voice_reply_audio_to_temp(&alloc, "imessage", NULL, 0, path, sizeof(path)),
                 HU_ERR_INVALID_ARGUMENT);
}

void run_voice_reply_tests(void) {
    HU_TEST_SUITE("voice_reply");
    HU_RUN_TEST(test_voice_reply_build_request_rejects_null);
    HU_RUN_TEST(test_voice_reply_build_request_annotates_text_speak);
    HU_RUN_TEST(test_voice_reply_build_request_persona_model_wins);
    HU_RUN_TEST(test_voice_reply_build_request_late_night_slows_request_speed);
    HU_RUN_TEST(test_voice_reply_speed_tags_are_relative_and_reset);
    HU_RUN_TEST(test_voice_reply_request_reaches_cartesia_with_ssml);
    HU_RUN_TEST(test_voice_reply_audio_to_temp_imessage_yields_caf);
    HU_RUN_TEST(test_voice_reply_audio_to_temp_null_channel_still_writes);
}
