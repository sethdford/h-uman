#include "human/core/allocator.h"
#include "human/core/file_util.h"
#include "test_framework.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Write `len` bytes of `data` to a fresh temp file; returns the path in
 * `path_buf`. Caller unlinks. */
static int write_temp_file(char *path_buf, size_t path_cap, const char *data, size_t len) {
    snprintf(path_buf, path_cap, "/tmp/hu_file_util_test_%d_%ld", (int)getpid(), (long)len);
    FILE *f = fopen(path_buf, "wb");
    if (!f)
        return 0;
    size_t wr = fwrite(data, 1, len, f);
    fclose(f);
    return wr == len;
}

static void test_file_read_all_happy_path_nul_terminated(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char path[128];
    const char *content = "hello\nworld";
    HU_ASSERT_TRUE(write_temp_file(path, sizeof(path), content, strlen(content)));

    char *data = NULL;
    size_t len = 0;
    hu_error_t err = hu_file_read_all(&alloc, path, 1024, &data, &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(data);
    HU_ASSERT_EQ((int)len, (int)strlen(content));
    HU_ASSERT_EQ(data[len], '\0');
    HU_ASSERT_STR_EQ(data, content);

    alloc.free(alloc.ctx, data, len + 1);
    unlink(path);
}

static void test_file_read_all_missing_file_returns_not_found(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *data = NULL;
    size_t len = 7;
    hu_error_t err =
        hu_file_read_all(&alloc, "/tmp/hu_file_util_test_nonexistent_98765", 1024, &data, &len);
    HU_ASSERT_EQ(err, HU_ERR_NOT_FOUND);
    HU_ASSERT_NULL(data);
    HU_ASSERT_EQ((int)len, 0);
}

static void test_file_read_all_empty_file_returns_invalid_format(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char path[128];
    HU_ASSERT_TRUE(write_temp_file(path, sizeof(path), "", 0));

    char *data = NULL;
    size_t len = 7;
    hu_error_t err = hu_file_read_all(&alloc, path, 1024, &data, &len);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_FORMAT);
    HU_ASSERT_NULL(data);
    HU_ASSERT_EQ((int)len, 0);
    unlink(path);
}

static void test_file_read_all_oversize_returns_invalid_format(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char path[128];
    const char *content = "0123456789"; /* 10 bytes, cap 9 */
    HU_ASSERT_TRUE(write_temp_file(path, sizeof(path), content, strlen(content)));

    char *data = NULL;
    size_t len = 0;
    hu_error_t err = hu_file_read_all(&alloc, path, 9, &data, &len);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_FORMAT);
    HU_ASSERT_NULL(data);
    unlink(path);
}

static void test_file_read_all_exact_cap_accepted(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char path[128];
    const char *content = "0123456789"; /* 10 bytes, cap 10: strictly-greater rejects */
    HU_ASSERT_TRUE(write_temp_file(path, sizeof(path), content, strlen(content)));

    char *data = NULL;
    size_t len = 0;
    hu_error_t err = hu_file_read_all(&alloc, path, 10, &data, &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ((int)len, 10);
    alloc.free(alloc.ctx, data, len + 1);
    unlink(path);
}

static void test_file_read_all_zero_cap_means_uncapped(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char path[128];
    const char *content = "uncapped read";
    HU_ASSERT_TRUE(write_temp_file(path, sizeof(path), content, strlen(content)));

    char *data = NULL;
    size_t len = 0;
    hu_error_t err = hu_file_read_all(&alloc, path, 0, &data, &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(data, content);
    alloc.free(alloc.ctx, data, len + 1);
    unlink(path);
}

static void test_file_read_all_binary_content_preserved(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char path[128];
    const char raw[] = {'\x89', 'P', 'N', 'G', '\0', '\xff', 'x'};
    HU_ASSERT_TRUE(write_temp_file(path, sizeof(path), raw, sizeof(raw)));

    char *data = NULL;
    size_t len = 0;
    hu_error_t err = hu_file_read_all(&alloc, path, 1024, &data, &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ((int)len, (int)sizeof(raw));
    HU_ASSERT_EQ(memcmp(data, raw, sizeof(raw)), 0);
    HU_ASSERT_EQ(data[len], '\0'); /* NUL after the embedded-NUL payload */
    alloc.free(alloc.ctx, data, len + 1);
    unlink(path);
}

static void test_file_read_all_null_args_return_invalid_argument(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *data = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_file_read_all(NULL, "/tmp/x", 0, &data, &len), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_file_read_all(&alloc, NULL, 0, &data, &len), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_file_read_all(&alloc, "/tmp/x", 0, NULL, &len), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_file_read_all(&alloc, "/tmp/x", 0, &data, NULL), HU_ERR_INVALID_ARGUMENT);
}

void run_file_util_tests(void) {
    HU_TEST_SUITE("FileUtil");
    HU_RUN_TEST(test_file_read_all_happy_path_nul_terminated);
    HU_RUN_TEST(test_file_read_all_missing_file_returns_not_found);
    HU_RUN_TEST(test_file_read_all_empty_file_returns_invalid_format);
    HU_RUN_TEST(test_file_read_all_oversize_returns_invalid_format);
    HU_RUN_TEST(test_file_read_all_exact_cap_accepted);
    HU_RUN_TEST(test_file_read_all_zero_cap_means_uncapped);
    HU_RUN_TEST(test_file_read_all_binary_content_preserved);
    HU_RUN_TEST(test_file_read_all_null_args_return_invalid_argument);
}
