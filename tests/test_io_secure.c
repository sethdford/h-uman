#include "human/core/io_secure.h"
#include "test_framework.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef _WIN32
#include <sys/stat.h>
#endif

/* ──────────────────────────────────────────────────────────────────────────
 * test_io_secure — exercise hu_io_secure_open on the cases that matter:
 *
 *   - argument validation rejects NULLs and unsupported modes
 *   - path-traversal sequences are rejected (..,  %2e, %2E)
 *   - happy path creates the file with the requested mode (0600 / 0644)
 *   - existing file is truncated, not appended
 *
 * The fs interaction uses mkstemp/unlink to stay inside the test temp
 * directory — no $HOME pollution.
 * ─────────────────────────────────────────────────────────────────────── */

static char *make_temp_path(void) {
    static char tmpl[] = "/tmp/hu_io_secure_XXXXXX";
    char *path = strdup(tmpl);
    int fd = mkstemp(path);
    if (fd < 0) {
        free(path);
        return NULL;
    }
    close(fd);
    /* mkstemp creates the file; we want a fresh path that doesn't yet
     * exist so the helper exercises its O_CREAT path properly. */
    unlink(path);
    return path;
}

static void io_secure_rejects_null_path(void) {
    FILE *f = NULL;
    HU_ASSERT_EQ(hu_io_secure_open(NULL, HU_IO_PERM_SECRET, "w", &f), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NULL(f);
}

static void io_secure_rejects_null_mode(void) {
    FILE *f = NULL;
    HU_ASSERT_EQ(hu_io_secure_open("/tmp/x", HU_IO_PERM_SECRET, NULL, &f), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NULL(f);
}

static void io_secure_rejects_null_out(void) {
    HU_ASSERT_EQ(hu_io_secure_open("/tmp/x", HU_IO_PERM_SECRET, "w", NULL),
                 HU_ERR_INVALID_ARGUMENT);
}

static void io_secure_rejects_unsupported_mode(void) {
    FILE *f = NULL;
    /* Append mode would change the security story (existing file
     * preservation) and isn't in the supported set — must reject. */
    HU_ASSERT_EQ(hu_io_secure_open("/tmp/x", HU_IO_PERM_USER, "a", &f), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NULL(f);
    HU_ASSERT_EQ(hu_io_secure_open("/tmp/x", HU_IO_PERM_USER, "r", &f), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NULL(f);
}

static void io_secure_rejects_dot_dot_traversal(void) {
    FILE *f = NULL;
    HU_ASSERT_EQ(hu_io_secure_open("/tmp/../etc/passwd", HU_IO_PERM_SECRET, "w", &f),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NULL(f);
}

static void io_secure_rejects_percent_encoded_traversal(void) {
    FILE *f = NULL;
    HU_ASSERT_EQ(hu_io_secure_open("/tmp/%2e%2e/etc/passwd", HU_IO_PERM_SECRET, "w", &f),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NULL(f);
    HU_ASSERT_EQ(hu_io_secure_open("/tmp/%2E%2E/etc/passwd", HU_IO_PERM_SECRET, "w", &f),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NULL(f);
}

#ifndef _WIN32
static void io_secure_user_creates_0644(void) {
    char *path = make_temp_path();
    HU_ASSERT_NOT_NULL(path);
    FILE *f = NULL;
    HU_ASSERT_EQ(hu_io_secure_open(path, HU_IO_PERM_USER, "w", &f), HU_OK);
    HU_ASSERT_NOT_NULL(f);
    fclose(f);
    struct stat st;
    HU_ASSERT_EQ(stat(path, &st), 0);
    /* Only the bottom 9 bits are interesting — the file type / sticky
     * bits depend on the filesystem. */
    HU_ASSERT_EQ(st.st_mode & 0777, 0644);
    unlink(path);
    free(path);
}

static void io_secure_secret_creates_0600(void) {
    char *path = make_temp_path();
    HU_ASSERT_NOT_NULL(path);
    FILE *f = NULL;
    HU_ASSERT_EQ(hu_io_secure_open(path, HU_IO_PERM_SECRET, "wb", &f), HU_OK);
    HU_ASSERT_NOT_NULL(f);
    fclose(f);
    struct stat st;
    HU_ASSERT_EQ(stat(path, &st), 0);
    HU_ASSERT_EQ(st.st_mode & 0777, 0600);
    unlink(path);
    free(path);
}

static void io_secure_truncates_existing(void) {
    char *path = make_temp_path();
    HU_ASSERT_NOT_NULL(path);
    /* Pre-populate with 100 bytes so we can prove O_TRUNC fired. */
    FILE *seed = fopen(path, "w");
    HU_ASSERT_NOT_NULL(seed);
    for (int i = 0; i < 100; i++)
        fputc('X', seed);
    fclose(seed);
    FILE *f = NULL;
    HU_ASSERT_EQ(hu_io_secure_open(path, HU_IO_PERM_USER, "w", &f), HU_OK);
    HU_ASSERT_NOT_NULL(f);
    fclose(f);
    struct stat st;
    HU_ASSERT_EQ(stat(path, &st), 0);
    HU_ASSERT_EQ(st.st_size, 0);
    unlink(path);
    free(path);
}
/* ── hu_io_secure_write_atomic ───────────────────────────────────────────
 * The contract that matters: a failed write must leave the previous file
 * exactly as it was (config.json used to be truncated in place first). */

static char *make_temp_dir(void) {
    char *dir = strdup("/tmp/hu_io_atomic_XXXXXX");
    if (!dir || !mkdtemp(dir)) {
        free(dir);
        return NULL;
    }
    return dir;
}

static char *read_all(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    static char buf[256];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

static void io_secure_atomic_writes_content_and_mode(void) {
    char *dir = make_temp_dir();
    HU_ASSERT_NOT_NULL(dir);
    char path[512];
    snprintf(path, sizeof(path), "%s/config.json", dir);
    HU_ASSERT_EQ(hu_io_secure_write_atomic(path, HU_IO_PERM_SECRET, "hello", 5), HU_OK);
    HU_ASSERT_STR_EQ(read_all(path), "hello");
    struct stat st;
    HU_ASSERT_EQ(stat(path, &st), 0);
    HU_ASSERT_EQ(st.st_mode & 0777, 0600);
    unlink(path);
    rmdir(dir);
    free(dir);
}

static void io_secure_atomic_replaces_existing_and_leaves_no_temp(void) {
    char *dir = make_temp_dir();
    HU_ASSERT_NOT_NULL(dir);
    char path[512];
    snprintf(path, sizeof(path), "%s/config.json", dir);
    HU_ASSERT_EQ(hu_io_secure_write_atomic(path, HU_IO_PERM_USER, "OLD CONTENT", 11), HU_OK);
    HU_ASSERT_EQ(hu_io_secure_write_atomic(path, HU_IO_PERM_USER, "new", 3), HU_OK);
    HU_ASSERT_STR_EQ(read_all(path), "new");
    struct stat st;
    HU_ASSERT_EQ(stat(path, &st), 0);
    HU_ASSERT_EQ(st.st_mode & 0777, 0644);
    unlink(path);
    /* rmdir succeeds only on an empty directory, which proves no temp
     * file survived next to config.json. */
    HU_ASSERT_EQ(rmdir(dir), 0);
    free(dir);
}

static void io_secure_atomic_failure_preserves_original(void) {
    HU_SKIP_IF(geteuid() == 0, "root bypasses directory permissions");
    char *dir = make_temp_dir();
    HU_ASSERT_NOT_NULL(dir);
    char path[512];
    snprintf(path, sizeof(path), "%s/config.json", dir);
    HU_ASSERT_EQ(hu_io_secure_write_atomic(path, HU_IO_PERM_SECRET, "ORIGINAL", 8), HU_OK);
    /* A read-only directory makes the temp file impossible to create, so
     * the write must fail — and must not touch the existing file. The old
     * in-place writer would have truncated and rewritten it here. */
    HU_ASSERT_EQ(chmod(dir, 0500), 0);
    hu_error_t err = hu_io_secure_write_atomic(path, HU_IO_PERM_SECRET, "NEW", 3);
    HU_ASSERT_EQ(chmod(dir, 0700), 0);
    HU_ASSERT_NEQ(err, HU_OK);
    HU_ASSERT_STR_EQ(read_all(path), "ORIGINAL");
    unlink(path);
    HU_ASSERT_EQ(rmdir(dir), 0);
    free(dir);
}

static void io_secure_atomic_zero_length_creates_empty_file(void) {
    char *dir = make_temp_dir();
    HU_ASSERT_NOT_NULL(dir);
    char path[512];
    snprintf(path, sizeof(path), "%s/empty", dir);
    HU_ASSERT_EQ(hu_io_secure_write_atomic(path, HU_IO_PERM_USER, "", 0), HU_OK);
    struct stat st;
    HU_ASSERT_EQ(stat(path, &st), 0);
    HU_ASSERT_EQ(st.st_size, 0);
    unlink(path);
    rmdir(dir);
    free(dir);
}
#endif /* !_WIN32 */

static void io_secure_atomic_rejects_bad_arguments(void) {
    HU_ASSERT_EQ(hu_io_secure_write_atomic(NULL, HU_IO_PERM_USER, "x", 1), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_io_secure_write_atomic("/tmp/a/../b", HU_IO_PERM_USER, "x", 1),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_io_secure_write_atomic("/tmp/x", HU_IO_PERM_USER, NULL, 1),
                 HU_ERR_INVALID_ARGUMENT);
}

void run_io_secure_tests(void) {
    HU_TEST_SUITE("Core IO secure");
    HU_RUN_TEST(io_secure_rejects_null_path);
    HU_RUN_TEST(io_secure_rejects_null_mode);
    HU_RUN_TEST(io_secure_rejects_null_out);
    HU_RUN_TEST(io_secure_rejects_unsupported_mode);
    HU_RUN_TEST(io_secure_rejects_dot_dot_traversal);
    HU_RUN_TEST(io_secure_rejects_percent_encoded_traversal);
#ifndef _WIN32
    HU_RUN_TEST(io_secure_user_creates_0644);
    HU_RUN_TEST(io_secure_secret_creates_0600);
    HU_RUN_TEST(io_secure_truncates_existing);
    HU_RUN_TEST(io_secure_atomic_writes_content_and_mode);
    HU_RUN_TEST(io_secure_atomic_replaces_existing_and_leaves_no_temp);
    HU_RUN_TEST(io_secure_atomic_failure_preserves_original);
    HU_RUN_TEST(io_secure_atomic_zero_length_creates_empty_file);
#endif
    HU_RUN_TEST(io_secure_atomic_rejects_bad_arguments);
}
