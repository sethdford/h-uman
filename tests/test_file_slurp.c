#include "human/core/file.h"
#include "test_framework.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ──────────────────────────────────────────────────────────────────────────
 * test_file_slurp — exercise hu_file_slurp, the shared whole-file reader
 * that replaces the fopen/fseek/ftell/fread clone in seven modules:
 *
 *   - argument validation rejects NULL alloc/path/out
 *   - missing file → HU_ERR_NOT_FOUND, *out stays NULL
 *   - empty file → HU_OK, len 0, buffer holds just the NUL
 *   - normal file → content + length exact, NUL-terminated
 *   - embedded NUL bytes → out_len is the true byte count, content intact
 *   - file larger than max_bytes → HU_ERR_LIMIT_REACHED, no allocation
 *   - max_bytes == 0 → unlimited
 *   - out_len may be NULL
 *
 * Files are created under /tmp via mkstemp and unlinked per test — no
 * $HOME pollution, fully deterministic, no network.
 * ─────────────────────────────────────────────────────────────────────── */

static char *write_temp_file(const void *data, size_t len) {
    static char tmpl[] = "/tmp/hu_file_slurp_XXXXXX";
    char *path = strdup(tmpl);
    if (!path)
        return NULL;
    int fd = mkstemp(path);
    if (fd < 0) {
        free(path);
        return NULL;
    }
    FILE *f = fdopen(fd, "wb");
    if (!f) {
        close(fd);
        unlink(path);
        free(path);
        return NULL;
    }
    if (len > 0 && fwrite(data, 1, len, f) != len) {
        fclose(f);
        unlink(path);
        free(path);
        return NULL;
    }
    fclose(f);
    return path;
}

static void drop_temp_file(char *path) {
    if (!path)
        return;
    unlink(path);
    free(path);
}

static void file_slurp_rejects_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *buf = (char *)&alloc; /* sentinel to prove *out is reset */
    size_t len = 99;
    HU_ASSERT_EQ(hu_file_slurp(NULL, "/tmp/x", 0, &buf, &len), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_file_slurp(&alloc, NULL, 0, &buf, &len), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_file_slurp(&alloc, "/tmp/x", 0, NULL, &len), HU_ERR_INVALID_ARGUMENT);
}

static void file_slurp_missing_file_returns_not_found(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *buf = (char *)&alloc; /* sentinel */
    size_t len = 99;
    HU_ASSERT_EQ(hu_file_slurp(&alloc, "/tmp/hu_file_slurp_definitely_missing", 0, &buf, &len),
                 HU_ERR_NOT_FOUND);
    HU_ASSERT_NULL(buf);
}

static void file_slurp_reads_normal_file_nul_terminated(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *content = "{\"hello\": \"world\"}\n";
    size_t clen = strlen(content);
    char *path = write_temp_file(content, clen);
    HU_ASSERT_NOT_NULL(path);

    char *buf = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_file_slurp(&alloc, path, 0, &buf, &len), HU_OK);
    HU_ASSERT_NOT_NULL(buf);
    HU_ASSERT_EQ(len, clen);
    HU_ASSERT_EQ(buf[len], '\0');
    HU_ASSERT_STR_EQ(buf, content);

    alloc.free(alloc.ctx, buf, len + 1);
    drop_temp_file(path);
}

static void file_slurp_empty_file_returns_ok_len_zero(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *path = write_temp_file(NULL, 0);
    HU_ASSERT_NOT_NULL(path);

    char *buf = NULL;
    size_t len = 99;
    HU_ASSERT_EQ(hu_file_slurp(&alloc, path, 0, &buf, &len), HU_OK);
    HU_ASSERT_NOT_NULL(buf);
    HU_ASSERT_EQ(len, (size_t)0);
    HU_ASSERT_EQ(buf[0], '\0');

    alloc.free(alloc.ctx, buf, 1);
    drop_temp_file(path);
}

static void file_slurp_embedded_nul_preserves_length(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char content[] = {'a', 'b', '\0', 'c', 'd'};
    char *path = write_temp_file(content, sizeof(content));
    HU_ASSERT_NOT_NULL(path);

    char *buf = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_file_slurp(&alloc, path, 0, &buf, &len), HU_OK);
    HU_ASSERT_NOT_NULL(buf);
    HU_ASSERT_EQ(len, sizeof(content));
    HU_ASSERT_EQ(memcmp(buf, content, sizeof(content)), 0);
    HU_ASSERT_EQ(buf[len], '\0');

    alloc.free(alloc.ctx, buf, len + 1);
    drop_temp_file(path);
}

static void file_slurp_over_max_bytes_returns_limit_reached(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *content = "0123456789"; /* 10 bytes */
    char *path = write_temp_file(content, 10);
    HU_ASSERT_NOT_NULL(path);

    char *buf = (char *)&alloc; /* sentinel */
    size_t len = 99;
    HU_ASSERT_EQ(hu_file_slurp(&alloc, path, 9, &buf, &len), HU_ERR_LIMIT_REACHED);
    HU_ASSERT_NULL(buf);

    drop_temp_file(path);
}

static void file_slurp_at_max_bytes_exactly_is_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *content = "0123456789"; /* 10 bytes */
    char *path = write_temp_file(content, 10);
    HU_ASSERT_NOT_NULL(path);

    char *buf = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_file_slurp(&alloc, path, 10, &buf, &len), HU_OK);
    HU_ASSERT_EQ(len, (size_t)10);
    HU_ASSERT_STR_EQ(buf, content);

    alloc.free(alloc.ctx, buf, len + 1);
    drop_temp_file(path);
}

static void file_slurp_null_out_len_is_allowed(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *content = "no len needed";
    char *path = write_temp_file(content, strlen(content));
    HU_ASSERT_NOT_NULL(path);

    char *buf = NULL;
    HU_ASSERT_EQ(hu_file_slurp(&alloc, path, 0, &buf, NULL), HU_OK);
    HU_ASSERT_NOT_NULL(buf);
    HU_ASSERT_STR_EQ(buf, content);

    alloc.free(alloc.ctx, buf, strlen(content) + 1);
    drop_temp_file(path);
}

void run_file_slurp_tests(void) {
    HU_TEST_SUITE("FileSlurp");
    HU_RUN_TEST(file_slurp_rejects_null_args);
    HU_RUN_TEST(file_slurp_missing_file_returns_not_found);
    HU_RUN_TEST(file_slurp_reads_normal_file_nul_terminated);
    HU_RUN_TEST(file_slurp_empty_file_returns_ok_len_zero);
    HU_RUN_TEST(file_slurp_embedded_nul_preserves_length);
    HU_RUN_TEST(file_slurp_over_max_bytes_returns_limit_reached);
    HU_RUN_TEST(file_slurp_at_max_bytes_exactly_is_ok);
    HU_RUN_TEST(file_slurp_null_out_len_is_allowed);
}
