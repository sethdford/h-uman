#if defined(HU_ENABLE_CARTESIA)

#include "human/core/allocator.h"
#include "human/tts/audio_pipeline.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* The HU_IS_TEST mock in src/tts/audio_pipeline.c names its scratch file
 * after the pid so concurrent suites never share it; mirror that here. */
static const char *expected_mock_path(char *buf, size_t cap, const char *stem, const char *ext) {
    snprintf(buf, cap, "/tmp/%s-%ld.%s", stem, (long)getpid(), ext);
    return buf;
}

static void test_audio_mp3_to_caf_mock_creates_file(void) {
    hu_allocator_t alloc = hu_system_allocator();
    static const unsigned char mock_mp3[] = {0xFF, 0xFB, 0x90, 0x00};
    char out_path[256] = {0};
    hu_error_t err =
        hu_audio_mp3_to_caf(&alloc, mock_mp3, sizeof(mock_mp3), out_path, sizeof(out_path));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(strlen(out_path) > 0);
    char expected[256];
    HU_ASSERT_STR_EQ(out_path,
                     expected_mock_path(expected, sizeof(expected), "human-voice-test", "mp3"));

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
    hu_error_t err =
        hu_audio_mp3_to_caf(&alloc, mock_mp3, sizeof(mock_mp3), out_path, sizeof(out_path));
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

/* Mock path is ~30 bytes incl. NUL; cap 10 cannot fit full path → error after file write */
static void test_audio_mp3_to_caf_mock_path_buffer_too_small(void) {
    hu_allocator_t alloc = hu_system_allocator();
    static const unsigned char mock_mp3[] = {0xFF, 0xFB, 0x90, 0x00};
    char out_path[16] = {0};
    hu_error_t err = hu_audio_mp3_to_caf(&alloc, mock_mp3, sizeof(mock_mp3), out_path, 10);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    char expected[256];
    hu_audio_cleanup_temp(
        expected_mock_path(expected, sizeof(expected), "human-voice-test", "mp3"));
}

static void test_audio_cleanup_temp_null_safe(void) {
    hu_audio_cleanup_temp(NULL);
}

static void test_audio_cleanup_temp_empty_path_safe(void) {
    hu_audio_cleanup_temp("");
}

static void test_audio_tts_bytes_to_temp_writes_mp3(void) {
    hu_allocator_t alloc = hu_system_allocator();
    static const unsigned char data[] = {0xFF, 0xFB, 0x90, 0x00};
    char out_path[256] = {0};
    hu_error_t err =
        hu_audio_tts_bytes_to_temp(&alloc, data, sizeof(data), "mp3", out_path, sizeof(out_path));
    HU_ASSERT_EQ(err, HU_OK);
    char expected[256];
    HU_ASSERT_STR_EQ(out_path,
                     expected_mock_path(expected, sizeof(expected), "human-tts-bytes-test", "mp3"));
    FILE *f = fopen(out_path, "rb");
    HU_ASSERT_NOT_NULL(f);
    fclose(f);
    hu_audio_cleanup_temp(out_path);
}

static void test_audio_tts_bytes_to_temp_writes_wav(void) {
    hu_allocator_t alloc = hu_system_allocator();
    static const unsigned char data[] = {'R', 'I', 'F', 'F'};
    char out_path[256] = {0};
    hu_error_t err =
        hu_audio_tts_bytes_to_temp(&alloc, data, sizeof(data), "wav", out_path, sizeof(out_path));
    HU_ASSERT_EQ(err, HU_OK);
    char expected[256];
    HU_ASSERT_STR_EQ(out_path,
                     expected_mock_path(expected, sizeof(expected), "human-tts-bytes-test", "wav"));
    hu_audio_cleanup_temp(out_path);
}

static void test_audio_tts_bytes_to_temp_rejects_bad_ext(void) {
    hu_allocator_t alloc = hu_system_allocator();
    static const unsigned char data[] = {0x01};
    char out_path[256] = {0};
    hu_error_t err = hu_audio_tts_bytes_to_temp(&alloc, data, 1, "ogg", out_path, sizeof(out_path));
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_audio_pipeline_process_null_alloc_returns_error(void) {
    static const unsigned char buf[] = {0x01, 0x02};
    void *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_audio_pipeline_process(NULL, buf, sizeof(buf), &out, &out_len);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_audio_pipeline_process_null_out_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    static const unsigned char buf[] = {0x01, 0x02};
    size_t out_len = 0;
    hu_error_t err = hu_audio_pipeline_process(&alloc, buf, sizeof(buf), NULL, &out_len);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_audio_pipeline_process_null_out_len_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    static const unsigned char buf[] = {0x01, 0x02};
    void *out = NULL;
    hu_error_t err = hu_audio_pipeline_process(&alloc, buf, sizeof(buf), &out, NULL);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_audio_pipeline_process_null_input_with_len_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    void *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_audio_pipeline_process(&alloc, NULL, 5, &out, &out_len);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_audio_pipeline_process_empty_input_returns_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    void *out = (void *)0xdeadbeef;
    size_t out_len = 999;
    hu_error_t err = hu_audio_pipeline_process(&alloc, "", 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ(out_len, 0);
}

static void test_audio_pipeline_process_pass_through_returns_copy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    static const unsigned char buf[] = {0x52, 0x49, 0x46, 0x46}; /* RIFF - WAV header */
    void *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_audio_pipeline_process(&alloc, buf, sizeof(buf), &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_EQ(out_len, sizeof(buf));
    HU_ASSERT(memcmp(out, buf, sizeof(buf)) == 0);
    alloc.free(alloc.ctx, out, out_len);
}

static void test_audio_pipeline_process_pcm_pass_through(void) {
    hu_allocator_t alloc = hu_system_allocator();
    static const unsigned char pcm[] = {0x00, 0x01, 0x02, 0x03};
    void *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_audio_pipeline_process(&alloc, pcm, sizeof(pcm), &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_EQ(out_len, sizeof(pcm));
    alloc.free(alloc.ctx, out, out_len);
}

void run_audio_pipeline_tests(void) {
    HU_TEST_SUITE("Audio pipeline");
    HU_RUN_TEST(test_audio_mp3_to_caf_mock_creates_file);
    HU_RUN_TEST(test_audio_cleanup_temp_removes_file);
    HU_RUN_TEST(test_audio_mp3_to_caf_null_bytes_returns_error);
    HU_RUN_TEST(test_audio_mp3_to_caf_zero_len_returns_error);
    HU_RUN_TEST(test_audio_mp3_to_caf_null_out_path_returns_error);
    HU_RUN_TEST(test_audio_mp3_to_caf_mock_path_buffer_too_small);
    HU_RUN_TEST(test_audio_cleanup_temp_null_safe);
    HU_RUN_TEST(test_audio_cleanup_temp_empty_path_safe);
    HU_RUN_TEST(test_audio_tts_bytes_to_temp_writes_mp3);
    HU_RUN_TEST(test_audio_tts_bytes_to_temp_writes_wav);
    HU_RUN_TEST(test_audio_tts_bytes_to_temp_rejects_bad_ext);
    HU_RUN_TEST(test_audio_pipeline_process_null_alloc_returns_error);
    HU_RUN_TEST(test_audio_pipeline_process_null_out_returns_error);
    HU_RUN_TEST(test_audio_pipeline_process_null_out_len_returns_error);
    HU_RUN_TEST(test_audio_pipeline_process_null_input_with_len_returns_error);
    HU_RUN_TEST(test_audio_pipeline_process_empty_input_returns_ok);
    HU_RUN_TEST(test_audio_pipeline_process_pass_through_returns_copy);
    HU_RUN_TEST(test_audio_pipeline_process_pcm_pass_through);
}

#endif /* HU_ENABLE_CARTESIA */
