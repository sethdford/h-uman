/*
 * Tests for Cartesia TTS integration.
 * Channel format mapping always runs; synthesize tests need HU_ENABLE_CARTESIA=ON.
 */
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/privacy.h"
#include "human/tts/cartesia.h"
#include "human/voice.h"
#include "test_framework.h"
#include <stddef.h>
#include <string.h>

static void test_tts_format_for_channel_imessage_returns_caf(void) {
    HU_ASSERT_STR_EQ(hu_tts_format_for_channel("imessage"), "caf");
}

static void test_tts_format_for_channel_telegram_discord_return_ogg(void) {
    HU_ASSERT_STR_EQ(hu_tts_format_for_channel("telegram"), "ogg");
    HU_ASSERT_STR_EQ(hu_tts_format_for_channel("discord"), "ogg");
}

static void test_tts_format_for_channel_null_and_slack_default_mp3(void) {
    HU_ASSERT_STR_EQ(hu_tts_format_for_channel(NULL), "mp3");
    HU_ASSERT_STR_EQ(hu_tts_format_for_channel("slack"), "mp3");
}

#if HU_ENABLE_CARTESIA

static void test_cartesia_null_api_key_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    unsigned char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_cartesia_tts_synthesize(&alloc, NULL, 0, "hello", 5, NULL, NULL, &out, &out_len);
    HU_ASSERT_NEQ(err, HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ(out_len, 0);
}

static void test_cartesia_empty_transcript_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    unsigned char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_cartesia_tts_synthesize(&alloc, "test-key", 8, "", 0, NULL, NULL, &out, &out_len);
    HU_ASSERT_NEQ(err, HU_OK);
    HU_ASSERT_NULL(out);
}

static void test_cartesia_null_config_uses_defaults(void) {
    /* In HU_IS_TEST, synthesize returns mock audio bytes without network */
    hu_allocator_t alloc = hu_system_allocator();
    unsigned char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_cartesia_tts_synthesize(&alloc, "test-key", 8, "Hello", 5, NULL, NULL, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_EQ(out_len, 400u);
    HU_ASSERT_EQ(out[0], (unsigned char)0xFF);
    HU_ASSERT_EQ(out[1], (unsigned char)0xFB);
    HU_ASSERT_EQ(out[2], (unsigned char)0x90);
    HU_ASSERT_EQ(out[3], (unsigned char)0x00);
    hu_cartesia_tts_free_bytes(&alloc, out, out_len);
}

static void test_cartesia_free_bytes_handles_null(void) {
    /* Crash safety test: verifies NULL bytes does not cause segfault.
     * hu_cartesia_tts_free_bytes is void — no return code to assert. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_cartesia_tts_free_bytes(&alloc, NULL, 0);
}

static void test_cartesia_synthesize_with_config(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cartesia_tts_config_t cfg = {
        .model_id = "sonic-3-2026-01-12",
        .voice_id = "voice-uuid",
        .emotion = "content",
        .speed = 0.95f,
        .volume = 1.0f,
        .nonverbals = false,
    };
    unsigned char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_cartesia_tts_synthesize(&alloc, "test-key", 8, "Hello world", 11, &cfg,
                                                NULL, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    hu_cartesia_tts_free_bytes(&alloc, out, out_len);
}

static void test_cartesia_synthesize_wav_format_returns_mock_bytes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    unsigned char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_cartesia_tts_synthesize(&alloc, "test-key", 8, "Hi", 2, NULL, "wav", &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_EQ(out_len, 400u);
    hu_cartesia_tts_free_bytes(&alloc, out, out_len);
}

/* --- Cartesia STT tests --- */

static void test_cartesia_stt_null_api_key_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *text = NULL;
    size_t tlen = 0;
    hu_error_t err =
        hu_cartesia_stt_transcribe(&alloc, NULL, 0, "/tmp/audio.wav", NULL, &text, &tlen);
    HU_ASSERT_NEQ(err, HU_OK);
    HU_ASSERT_NULL(text);
}

static void test_cartesia_stt_empty_path_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *text = NULL;
    size_t tlen = 0;
    hu_error_t err = hu_cartesia_stt_transcribe(&alloc, "test-key", 8, "", NULL, &text, &tlen);
    HU_ASSERT_NEQ(err, HU_OK);
    HU_ASSERT_NULL(text);
}

static void test_cartesia_stt_mock_returns_text(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *text = NULL;
    size_t tlen = 0;
    hu_cartesia_stt_config_t sc = {.model = "ink-whisper", .language = "en"};
    hu_error_t err =
        hu_cartesia_stt_transcribe(&alloc, "test-key", 8, "/tmp/audio.wav", &sc, &text, &tlen);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(text);
    HU_ASSERT_TRUE(tlen > 0);
    HU_ASSERT_STR_EQ(text, "Cartesia mock transcription");
    alloc.free(alloc.ctx, text, tlen + 1);
}

static void test_cartesia_stt_null_config_uses_defaults(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *text = NULL;
    size_t tlen = 0;
    hu_error_t err =
        hu_cartesia_stt_transcribe(&alloc, "test-key", 8, "/tmp/audio.wav", NULL, &text, &tlen);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(text);
    alloc.free(alloc.ctx, text, tlen + 1);
}

#endif /* HU_ENABLE_CARTESIA */

/* --- Voice routing tests (Cartesia provider) --- */

static void test_voice_stt_file_cartesia_provider_routes_correctly(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_voice_config_t vcfg = {0};
    vcfg.stt_provider = "cartesia";
    vcfg.cartesia_api_key = "test-cartesia-key";
    vcfg.cartesia_api_key_len = 17;
    char *text = NULL;
    size_t tlen = 0;
    hu_error_t err = hu_voice_stt_file(&alloc, &vcfg, "/tmp/test.wav", &text, &tlen);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(text);
    HU_ASSERT_STR_EQ(text, "Cartesia mock transcription");
    alloc.free(alloc.ctx, text, tlen + 1);
}

static void test_voice_tts_cartesia_provider_routes_correctly(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_voice_config_t vcfg = {0};
    vcfg.tts_provider = "cartesia";
    vcfg.cartesia_api_key = "test-cartesia-key";
    vcfg.cartesia_api_key_len = 17;
    void *audio = NULL;
    size_t alen = 0;
    hu_error_t err = hu_voice_tts(&alloc, &vcfg, "Hello", 5, &audio, &alen);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(audio);
    HU_ASSERT_TRUE(alen > 0);
    alloc.free(alloc.ctx, audio, alen);
}

/* Privacy kill-switch: Cartesia is cloud TTS egress. The daemon persona path
 * calls hu_cartesia_tts_synthesize directly (bypassing hu_voice_tts's gate), so
 * the refusal must live at this boundary. Regression for that bypass. */
static void test_cartesia_synthesize_blocked_under_privacy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cartesia_tts_config_t cfg = {0};
    unsigned char *bytes = NULL;
    size_t len = 0;
    hu_privacy_set_enforced(true);
    hu_error_t err =
        hu_cartesia_tts_synthesize(&alloc, "ck-test", 7, "hello", 5, &cfg, "mp3", &bytes, &len);
    hu_privacy_set_enforced(false);
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_NULL(bytes);
}

void run_cartesia_tests(void) {
    HU_TEST_SUITE("Cartesia");
    HU_RUN_TEST(test_tts_format_for_channel_imessage_returns_caf);
    HU_RUN_TEST(test_tts_format_for_channel_telegram_discord_return_ogg);
    HU_RUN_TEST(test_tts_format_for_channel_null_and_slack_default_mp3);
#if HU_ENABLE_CARTESIA
    HU_RUN_TEST(test_cartesia_null_api_key_returns_error);
    HU_RUN_TEST(test_cartesia_empty_transcript_returns_error);
    HU_RUN_TEST(test_cartesia_null_config_uses_defaults);
    HU_RUN_TEST(test_cartesia_free_bytes_handles_null);
    HU_RUN_TEST(test_cartesia_synthesize_with_config);
    HU_RUN_TEST(test_cartesia_synthesize_wav_format_returns_mock_bytes);
    HU_RUN_TEST(test_cartesia_stt_null_api_key_returns_error);
    HU_RUN_TEST(test_cartesia_stt_empty_path_returns_error);
    HU_RUN_TEST(test_cartesia_stt_mock_returns_text);
    HU_RUN_TEST(test_cartesia_stt_null_config_uses_defaults);
#endif
    HU_RUN_TEST(test_voice_stt_file_cartesia_provider_routes_correctly);
    HU_RUN_TEST(test_voice_tts_cartesia_provider_routes_correctly);
    HU_RUN_TEST(test_cartesia_synthesize_blocked_under_privacy);
}
