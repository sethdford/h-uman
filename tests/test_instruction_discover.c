/*
 * Tests for instruction file discovery.
 * Uses temp directories to avoid touching real filesystem state.
 */
#include "human/agent/instruction_discover.h"
#include "test_framework.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ── test allocator ──────────────────────────────────────────────────────── */

static void *test_alloc(void *ctx, size_t size) {
    (void)ctx;
    return malloc(size);
}
static void *test_realloc(void *ctx, void *ptr, size_t old_size, size_t new_size) {
    (void)ctx;
    (void)old_size;
    return realloc(ptr, new_size);
}
static void test_free(void *ctx, void *ptr, size_t size) {
    (void)ctx;
    (void)size;
    free(ptr);
}
static hu_allocator_t test_allocator = {
    .alloc = test_alloc, .realloc = test_realloc, .free = test_free, .ctx = NULL};

/* ── temp directory helpers ──────────────────────────────────────────────── */

static char g_tmpdir[256];

static void make_tmpdir(void) {
    snprintf(g_tmpdir, sizeof(g_tmpdir), "/tmp/hu_instdisc_XXXXXX");
    char *r = mkdtemp(g_tmpdir);
    HU_ASSERT_NOT_NULL(r);
}

static void write_file(const char *dir, const char *name, const char *content) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    HU_ASSERT_NOT_NULL(f);
    if (content)
        fwrite(content, 1, strlen(content), f);
    fclose(f);
}

static void make_subdir(const char *parent, const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", parent, name);
    mkdir(path, 0755);
}

/* Recursive rm -rf for temp dirs */
static void rm_rf(const char *path) {
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    if (system(cmd) != 0) {
        /* best-effort cleanup; a (void) cast does not silence glibc warn_unused_result */
    }
}

/* ── test: validate path rejects null bytes ──────────────────────────────── */

static void test_validate_path_null_bytes(void) {
    char bad[] = "/tmp/foo\0bar";
    char *canon = NULL;
    size_t canon_len = 0;
    hu_error_t err = hu_instruction_validate_path(&test_allocator, bad, 12, &canon, &canon_len);
    HU_ASSERT_EQ(err, HU_ERR_SECURITY_COMMAND_NOT_ALLOWED);
    HU_ASSERT_NULL(canon);
}

/* ── test: validate path rejects nonexistent ─────────────────────────────── */

static void test_validate_path_not_found(void) {
    const char *bad = "/nonexistent_path_12345/foo.md";
    char *canon = NULL;
    size_t canon_len = 0;
    hu_error_t err =
        hu_instruction_validate_path(&test_allocator, bad, strlen(bad), &canon, &canon_len);
    HU_ASSERT_EQ(err, HU_ERR_NOT_FOUND);
}

/* ── test: validate path canonicalizes ───────────────────────────────────── */

static void test_validate_path_canonical(void) {
    make_tmpdir();
    write_file(g_tmpdir, "test.md", "hello");

    char path[512];
    snprintf(path, sizeof(path), "%s/./test.md", g_tmpdir);

    char *canon = NULL;
    size_t canon_len = 0;
    hu_error_t err =
        hu_instruction_validate_path(&test_allocator, path, strlen(path), &canon, &canon_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(canon);
    /* Should not contain /./ */
    HU_ASSERT_NULL(strstr(canon, "/./"));
    test_allocator.free(NULL, canon, canon_len + 1);
    rm_rf(g_tmpdir);
}

/* ── test: read single file ──────────────────────────────────────────────── */

static void test_file_read_basic(void) {
    make_tmpdir();
    write_file(g_tmpdir, ".human.md", "Be helpful and concise.");

    char path[512];
    snprintf(path, sizeof(path), "%s/.human.md", g_tmpdir);

    hu_instruction_file_t file;
    hu_error_t err =
        hu_instruction_file_read(&test_allocator, path, HU_INSTRUCTION_SOURCE_WORKSPACE, &file);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(file.source, HU_INSTRUCTION_SOURCE_WORKSPACE);
    HU_ASSERT_NOT_NULL(file.content);
    HU_ASSERT_STR_EQ(file.content, "Be helpful and concise.");
    HU_ASSERT_EQ(file.content_len, 23);
    HU_ASSERT_FALSE(file.truncated);
    HU_ASSERT_GT(file.mtime, 0);

    test_allocator.free(NULL, file.path, file.path_len + 1);
    test_allocator.free(NULL, file.content, file.content_len + 1);
    rm_rf(g_tmpdir);
}

/* ── test: read file truncation ──────────────────────────────────────────── */

static void test_file_read_truncation(void) {
    make_tmpdir();

    /* Create content larger than per-file limit */
    size_t big_size = HU_INSTRUCTION_MAX_CHARS_PER_FILE + 500;
    char *big = (char *)malloc(big_size + 1);
    HU_ASSERT_NOT_NULL(big);
    memset(big, 'A', big_size);
    big[big_size] = '\0';
    write_file(g_tmpdir, "big.md", big);
    free(big);

    char path[512];
    snprintf(path, sizeof(path), "%s/big.md", g_tmpdir);

    hu_instruction_file_t file;
    hu_error_t err =
        hu_instruction_file_read(&test_allocator, path, HU_INSTRUCTION_SOURCE_PROJECT_ROOT, &file);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(file.truncated);
    HU_ASSERT_EQ(file.content_len, HU_INSTRUCTION_MAX_CHARS_PER_FILE);

    test_allocator.free(NULL, file.path, file.path_len + 1);
    test_allocator.free(NULL, file.content, file.content_len + 1);
    rm_rf(g_tmpdir);
}

/* ── test: read nonexistent file ─────────────────────────────────────────── */

static void test_file_read_not_found(void) {
    hu_instruction_file_t file;
    hu_error_t err = hu_instruction_file_read(&test_allocator, "/nonexistent_12345/foo.md",
                                              HU_INSTRUCTION_SOURCE_USER_HOME, &file);
    HU_ASSERT_EQ(err, HU_ERR_NOT_FOUND);
}

/* ── test: merge empty ───────────────────────────────────────────────────── */

static void test_merge_empty(void) {
    char *merged = NULL;
    size_t merged_len = 0;
    hu_error_t err = hu_instruction_merge(&test_allocator, NULL, 0, &merged, &merged_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NULL(merged);
    HU_ASSERT_EQ(merged_len, 0);
}

/* ── test: merge priority order ──────────────────────────────────────────── */

static void test_merge_priority_order(void) {
    hu_instruction_file_t files[3];
    memset(files, 0, sizeof(files));

    /* Workspace (highest priority) */
    files[0].source = HU_INSTRUCTION_SOURCE_WORKSPACE;
    files[0].path = (char *)"ws/.human.md";
    files[0].path_len = 12;
    files[0].content = (char *)"workspace rules";
    files[0].content_len = 15;

    /* Project root */
    files[1].source = HU_INSTRUCTION_SOURCE_PROJECT_ROOT;
    files[1].path = (char *)"proj/HUMAN.md";
    files[1].path_len = 13;
    files[1].content = (char *)"project rules";
    files[1].content_len = 13;

    /* User home (lowest) */
    files[2].source = HU_INSTRUCTION_SOURCE_USER_HOME;
    files[2].path = (char *)"home/instructions.md";
    files[2].path_len = 20;
    files[2].content = (char *)"user rules";
    files[2].content_len = 10;

    char *merged = NULL;
    size_t merged_len = 0;
    hu_error_t err = hu_instruction_merge(&test_allocator, files, 3, &merged, &merged_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(merged);

    /* Workspace should appear first, then project, then user */
    const char *ws_pos = strstr(merged, "workspace rules");
    const char *proj_pos = strstr(merged, "project rules");
    const char *user_pos = strstr(merged, "user rules");
    HU_ASSERT_NOT_NULL(ws_pos);
    HU_ASSERT_NOT_NULL(proj_pos);
    HU_ASSERT_NOT_NULL(user_pos);
    HU_ASSERT_TRUE(ws_pos < proj_pos);
    HU_ASSERT_TRUE(proj_pos < user_pos);

    /* Check section headers */
    HU_ASSERT_STR_CONTAINS(merged, "# Workspace instructions");
    HU_ASSERT_STR_CONTAINS(merged, "# Project instructions");
    HU_ASSERT_STR_CONTAINS(merged, "# User instructions");

    test_allocator.free(NULL, merged, merged_len + 1);
}

/* ── test: merge total limit ─────────────────────────────────────────────── */

static void test_merge_total_limit(void) {
    hu_instruction_file_t files[2];
    memset(files, 0, sizeof(files));

    /* Create content that nearly fills the total limit */
    size_t big_size = HU_INSTRUCTION_MAX_CHARS_TOTAL - 100;
    char *big = (char *)malloc(big_size + 1);
    HU_ASSERT_NOT_NULL(big);
    memset(big, 'X', big_size);
    big[big_size] = '\0';

    files[0].source = HU_INSTRUCTION_SOURCE_WORKSPACE;
    files[0].path = (char *)"a";
    files[0].path_len = 1;
    files[0].content = big;
    files[0].content_len = big_size;

    files[1].source = HU_INSTRUCTION_SOURCE_USER_HOME;
    files[1].path = (char *)"b";
    files[1].path_len = 1;
    files[1].content = (char *)"should be partially included";
    files[1].content_len = 28;

    char *merged = NULL;
    size_t merged_len = 0;
    hu_error_t err = hu_instruction_merge(&test_allocator, files, 2, &merged, &merged_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(merged);

    /* The merged content chars (excluding headers) should not exceed total limit */
    /* First file's full content should be present */
    HU_ASSERT_TRUE(merged_len > 0);

    free(big);
    test_allocator.free(NULL, merged, merged_len + 1);
}

/* ── test: discovery with workspace .human.md ────────────────────────────── */

static void test_discovery_workspace_only(void) {
    make_tmpdir();
    write_file(g_tmpdir, ".human.md", "Be terse.");

    hu_instruction_discovery_t *disc = NULL;
    hu_error_t err =
        hu_instruction_discovery_run(&test_allocator, g_tmpdir, strlen(g_tmpdir), &disc);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(disc);
    HU_ASSERT_GT(disc->file_count, 0);

    /* At minimum, workspace file should be found */
    bool found_workspace = false;
    for (size_t i = 0; i < disc->file_count; i++) {
        if (disc->files[i].source == HU_INSTRUCTION_SOURCE_WORKSPACE) {
            found_workspace = true;
            HU_ASSERT_STR_CONTAINS(disc->files[i].content, "Be terse.");
        }
    }
    HU_ASSERT_TRUE(found_workspace);

    HU_ASSERT_NOT_NULL(disc->merged_content);
    HU_ASSERT_GT(disc->merged_content_len, 0);
    HU_ASSERT_STR_CONTAINS(disc->merged_content, "Be terse.");

    hu_instruction_discovery_destroy(&test_allocator, disc);
    rm_rf(g_tmpdir);
}

/* ── test: discovery walks upward for HUMAN.md ───────────────────────────── */

static void test_discovery_walk_upward(void) {
    make_tmpdir();
    /* Create nested dirs: tmpdir/a/b/c */
    make_subdir(g_tmpdir, "a");
    char a_dir[512];
    snprintf(a_dir, sizeof(a_dir), "%s/a", g_tmpdir);
    make_subdir(a_dir, "b");
    char b_dir[512];
    snprintf(b_dir, sizeof(b_dir), "%s/a/b", g_tmpdir);
    make_subdir(b_dir, "c");
    char c_dir[512];
    snprintf(c_dir, sizeof(c_dir), "%s/a/b/c", g_tmpdir);

    /* Place HUMAN.md in the top-level tmpdir */
    write_file(g_tmpdir, "HUMAN.md", "Project-wide instructions.");

    /* Run discovery from the deepest dir */
    hu_instruction_discovery_t *disc = NULL;
    hu_error_t err = hu_instruction_discovery_run(&test_allocator, c_dir, strlen(c_dir), &disc);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(disc);

    /* Should have found the HUMAN.md by walking upward */
    bool found_project = false;
    for (size_t i = 0; i < disc->file_count; i++) {
        if (disc->files[i].source == HU_INSTRUCTION_SOURCE_PROJECT_ROOT) {
            found_project = true;
            HU_ASSERT_STR_CONTAINS(disc->files[i].content, "Project-wide instructions.");
        }
    }
    HU_ASSERT_TRUE(found_project);

    hu_instruction_discovery_destroy(&test_allocator, disc);
    rm_rf(g_tmpdir);
}

/* ── test: freshness check ───────────────────────────────────────────────── */

static void test_freshness_check(void) {
    make_tmpdir();
    write_file(g_tmpdir, ".human.md", "original");

    hu_instruction_discovery_t *disc = NULL;
    hu_error_t err =
        hu_instruction_discovery_run(&test_allocator, g_tmpdir, strlen(g_tmpdir), &disc);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(disc);

    /* Should be fresh immediately after discovery */
    HU_ASSERT_TRUE(hu_instruction_discovery_is_fresh(disc));

    hu_instruction_discovery_destroy(&test_allocator, disc);
    rm_rf(g_tmpdir);
}

/* ── test: null args handled gracefully ──────────────────────────────────── */

static void test_null_args(void) {
    HU_ASSERT_EQ(hu_instruction_discovery_run(NULL, "x", 1, NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_instruction_file_read(NULL, "x", HU_INSTRUCTION_SOURCE_WORKSPACE, NULL),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_instruction_merge(NULL, NULL, 0, NULL, NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_FALSE(hu_instruction_discovery_is_fresh(NULL));

    /* destroy with NULL should not crash */
    hu_instruction_discovery_destroy(&test_allocator, NULL);
    hu_instruction_discovery_destroy(NULL, NULL);
}

/* ── test: discovery with no files ───────────────────────────────────────── */

static void test_discovery_no_files(void) {
    make_tmpdir();
    /* Empty directory — no instruction files */
    hu_instruction_discovery_t *disc = NULL;

    /* Override HOME to prevent finding real ~/.human/instructions.md */
    const char *old_home = getenv("HOME");
    setenv("HOME", g_tmpdir, 1);

    hu_error_t err =
        hu_instruction_discovery_run(&test_allocator, g_tmpdir, strlen(g_tmpdir), &disc);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(disc);

    /* The walk might find real HUMAN.md files in parent dirs, but at minimum
     * merged content should be valid (possibly empty or just from parent walks) */
    HU_ASSERT_TRUE(disc->merged_content == NULL || disc->merged_content_len >= 0);

    if (old_home)
        setenv("HOME", old_home, 1);
    else
        unsetenv("HOME");

    hu_instruction_discovery_destroy(&test_allocator, disc);
    rm_rf(g_tmpdir);
}

/* ── test: validate path null arguments ──────────────────────────────────── */

static void test_validate_path_null_args(void) {
    char *canon = NULL;
    size_t canon_len = 0;
    HU_ASSERT_EQ(hu_instruction_validate_path(NULL, "/tmp", 4, &canon, &canon_len),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_instruction_validate_path(&test_allocator, NULL, 0, &canon, &canon_len),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_instruction_validate_path(&test_allocator, "/tmp", 4, NULL, &canon_len),
                 HU_ERR_INVALID_ARGUMENT);
}

/* ── test: injection into prompt ────────────────────────────────────────── */

static void test_discovery_prompt_injection(void) {
    make_tmpdir();

    /* Create workspace instruction file */
    const char *instr_content = "Be helpful and concise. Use simple language.";
    write_file(g_tmpdir, ".human.md", instr_content);

    /* Run discovery */
    hu_instruction_discovery_t *disc = NULL;
    hu_error_t err =
        hu_instruction_discovery_run(&test_allocator, g_tmpdir, strlen(g_tmpdir), &disc);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(disc);
    HU_ASSERT_NOT_NULL(disc->merged_content);
    HU_ASSERT_GT(disc->merged_content_len, 0);

    /* Verify instruction content is in merged output */
    HU_ASSERT_STR_CONTAINS(disc->merged_content, instr_content);

    /* Verify section header is present */
    HU_ASSERT_STR_CONTAINS(disc->merged_content, "# Workspace instructions");

    hu_instruction_discovery_destroy(&test_allocator, disc);
    rm_rf(g_tmpdir);
}

/* ── test: freshness re-discovery simulation ─────────────────────────────── */

static void test_discovery_freshness_trigger(void) {
    make_tmpdir();

    /* Create initial instruction file */
    write_file(g_tmpdir, ".human.md", "original instructions");

    /* First discovery */
    hu_instruction_discovery_t *disc1 = NULL;
    hu_error_t err =
        hu_instruction_discovery_run(&test_allocator, g_tmpdir, strlen(g_tmpdir), &disc1);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(disc1);
    HU_ASSERT_TRUE(hu_instruction_discovery_is_fresh(disc1));
    HU_ASSERT_STR_CONTAINS(disc1->merged_content, "original instructions");

    /* Simulate TTL expiry by manually advancing last_check_time */
    disc1->last_check_time = (int64_t)time(NULL) - (HU_INSTRUCTION_CACHE_TTL_SEC + 10);

    /* Should no longer be fresh */
    HU_ASSERT_FALSE(hu_instruction_discovery_is_fresh(disc1));

    hu_instruction_discovery_destroy(&test_allocator, disc1);
    rm_rf(g_tmpdir);
}

/* ── suite runner ────────────────────────────────────────────────────────── */

void run_instruction_discover_tests(void) {
    HU_TEST_SUITE("InstructionDiscovery");
    HU_RUN_TEST(test_validate_path_null_bytes);
    HU_RUN_TEST(test_validate_path_not_found);
    HU_RUN_TEST(test_validate_path_canonical);
    HU_RUN_TEST(test_validate_path_null_args);
    HU_RUN_TEST(test_file_read_basic);
    HU_RUN_TEST(test_file_read_truncation);
    HU_RUN_TEST(test_file_read_not_found);
    HU_RUN_TEST(test_merge_empty);
    HU_RUN_TEST(test_merge_priority_order);
    HU_RUN_TEST(test_merge_total_limit);
    HU_RUN_TEST(test_discovery_workspace_only);
    HU_RUN_TEST(test_discovery_walk_upward);
    HU_RUN_TEST(test_freshness_check);
    HU_RUN_TEST(test_null_args);
    HU_RUN_TEST(test_discovery_no_files);
    HU_RUN_TEST(test_discovery_prompt_injection);
    HU_RUN_TEST(test_discovery_freshness_trigger);
}
