#if defined(HU_ENABLE_CARTESIA)

#include "human/core/allocator.h"
#include "human/tts/audio_pipeline.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>

static void test_audio_mp3_to_caf_mock_creates_file(void) {
    hu_allocator_t alloc = hu_system_allocator();
    static const unsigned char mock_mp3[] = {0xFF, 0xFB, 0x90, 0x00};
    char out_path[256] = {0};
    hu_error_t err = hu_audio_mp3_to_caf(&alloc, mock_mp3, sizeof(mock_mp3), out_path, sizeof(out_path));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(strlen(out_path) > 0);
    HU_ASSERT_STR_EQ(out_path, "/tmp/human-voice-test.mp3");

    /* File should exist */
    FILE *f = fopen(out_path, "rb");
    HU_ASSERT_NOT_NULL(f);
    fclose(f);

    hu_audio_cleanup_temp(out_path);

    /* File should be gone after cleanup */
    f = fopen(out_path, "rb");
    HU_ASSERT_NULL(f);
}

static void test_audio_cleanup_temp_removes_file(void) {
    hu_allocator_t alloc = hu_system_allocator();
    static const unsigned char mock_mp3[] = {0x00, 0x01};
    char out_path[256] = {0};
    hu_error_t err = hu_audio_mp3_to_caf(&alloc, mock_mp3, sizeof(mock_mp3), out_path, sizeof(out_path));
    HU_ASSERT_EQ(err, HU_OK);
    hu_audio_cleanup_temp(out_path);
    FILE *f = fopen(out_path, "rb");
    HU_ASSERT_NULL(f);
}

static void test_audio_mp3_to_caf_null_bytes_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char out_path[256] = {0};
    hu_error_t err = hu_audio_mp3_to_caf(&alloc, NULL, 10, out_path, sizeof(out_path));
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_audio_mp3_to_caf_zero_len_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    static const unsigned char buf[] = {0x01};
    char out_path[256] = {0};
    hu_error_t err = hu_audio_mp3_to_caf(&alloc, buf, 0, out_path, sizeof(out_path));
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_audio_mp3_to_caf_null_out_path_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    static const unsigned char buf[] = {0x01};
    hu_error_t err = hu_audio_mp3_to_caf(&alloc, buf, 1, NULL, 256);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_audio_cleanup_temp_null_safe(void) {
    hu_audio_cleanup_temp(NULL);
}

static void test_audio_cleanup_temp_empty_path_safe(void) {
    hu_audio_cleanup_temp("");
}

void run_audio_pipeline_tests(void) {
    HU_TEST_SUITE("Audio pipeline");
    HU_RUN_TEST(test_audio_mp3_to_caf_mock_creates_file);
    HU_RUN_TEST(test_audio_cleanup_temp_removes_file);
    HU_RUN_TEST(test_audio_mp3_to_caf_null_bytes_returns_error);
    HU_RUN_TEST(test_audio_mp3_to_caf_zero_len_returns_error);
    HU_RUN_TEST(test_audio_mp3_to_caf_null_out_path_returns_error);
    HU_RUN_TEST(test_audio_cleanup_temp_null_safe);
    HU_RUN_TEST(test_audio_cleanup_temp_empty_path_safe);
}

#endif /* HU_ENABLE_CARTESIA */
