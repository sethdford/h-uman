/*
 * Tests for Cartesia TTS integration.
 * Only compiled when HU_ENABLE_CARTESIA=ON.
 */
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/tts/cartesia.h"
#include "test_framework.h"
#include <stddef.h>
#include <string.h>

#if HU_ENABLE_CARTESIA

static void test_cartesia_null_api_key_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    unsigned char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_cartesia_tts_synthesize(&alloc, NULL, 0, "hello", 5, NULL, &out, &out_len);
    HU_ASSERT_NEQ(err, HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ(out_len, 0);
}

static void test_cartesia_empty_transcript_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    unsigned char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_cartesia_tts_synthesize(&alloc, "test-key", 8, "", 0, NULL, &out, &out_len);
    HU_ASSERT_NEQ(err, HU_OK);
    HU_ASSERT_NULL(out);
}

static void test_cartesia_null_config_uses_defaults(void) {
    /* In HU_IS_TEST, synthesize returns mock MP3 bytes without network */
    hu_allocator_t alloc = hu_system_allocator();
    unsigned char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_cartesia_tts_synthesize(&alloc, "test-key", 8, "Hello", 5, NULL, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    /* Mock returns 4 bytes * 100 = 400 bytes */
    HU_ASSERT_EQ(out_len, 400u);
    /* First 4 bytes are 0xFF 0xFB 0x90 0x00 */
    HU_ASSERT_EQ(out[0], (unsigned char)0xFF);
    HU_ASSERT_EQ(out[1], (unsigned char)0xFB);
    HU_ASSERT_EQ(out[2], (unsigned char)0x90);
    HU_ASSERT_EQ(out[3], (unsigned char)0x00);
    hu_cartesia_tts_free_bytes(&alloc, out, out_len);
}

static void test_cartesia_free_bytes_handles_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cartesia_tts_free_bytes(&alloc, NULL, 0);
    /* No crash */
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
        &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    hu_cartesia_tts_free_bytes(&alloc, out, out_len);
}

void run_cartesia_tests(void) {
    HU_TEST_SUITE("Cartesia TTS");
    HU_RUN_TEST(test_cartesia_null_api_key_returns_error);
    HU_RUN_TEST(test_cartesia_empty_transcript_returns_error);
    HU_RUN_TEST(test_cartesia_null_config_uses_defaults);
    HU_RUN_TEST(test_cartesia_free_bytes_handles_null);
    HU_RUN_TEST(test_cartesia_synthesize_with_config);
}

#else

void run_cartesia_tests(void) {
    (void)0; /* No-op when Cartesia disabled */
}

#endif /* HU_ENABLE_CARTESIA */
