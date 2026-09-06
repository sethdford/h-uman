/* Tests for the shared persona-card plumbing (src/persona/card_file.c):
 * locate + slurp <persona dir>/<name><suffix>, parse as a JSON object, copy
 * the measurement window. Both the style card and the emotion card sit on
 * these, so a regression here would break two prompt rules at once. */
#include "human/core/allocator.h"
#include "human/persona/card_file.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_tmpdir[256];

static void make_tmpdir(void) {
    snprintf(g_tmpdir, sizeof(g_tmpdir), "/tmp/hu_card_file_XXXXXX");
    HU_ASSERT_NOT_NULL(mkdtemp(g_tmpdir));
    setenv("HU_PERSONA_DIR", g_tmpdir, 1);
}

static void cleanup_tmpdir(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/cf.test-card.json", g_tmpdir);
    unlink(path);
    rmdir(g_tmpdir);
    unsetenv("HU_PERSONA_DIR");
}

static void slurp_missing_file_is_not_found_and_leaves_buf_null(void) {
    make_tmpdir();
    hu_allocator_t alloc = hu_system_allocator();
    char *buf = (char *)1;
    size_t len = 99;
    HU_ASSERT_EQ(hu_persona_card_slurp(&alloc, "cf", 2, ".test-card.json", &buf, &len),
                 HU_ERR_NOT_FOUND);
    HU_ASSERT_NULL(buf);
    HU_ASSERT_EQ(len, (size_t)0);
    cleanup_tmpdir();
}

static void slurp_reads_name_plus_suffix_from_persona_dir(void) {
    make_tmpdir();
    char path[512];
    snprintf(path, sizeof(path), "%s/cf.test-card.json", g_tmpdir);
    FILE *f = fopen(path, "wb");
    HU_ASSERT_NOT_NULL(f);
    fputs("{\"n\":1}", f);
    fclose(f);
    hu_allocator_t alloc = hu_system_allocator();
    char *buf = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_persona_card_slurp(&alloc, "cf", 2, ".test-card.json", &buf, &len), HU_OK);
    HU_ASSERT_NOT_NULL(buf);
    HU_ASSERT_EQ(len, strlen("{\"n\":1}"));
    HU_ASSERT_STR_EQ(buf, "{\"n\":1}");
    alloc.free(alloc.ctx, buf, len + 1);
    /* The suffix is part of the name: a different suffix is a different file. */
    HU_ASSERT_EQ(hu_persona_card_slurp(&alloc, "cf", 2, ".other.json", &buf, &len),
                 HU_ERR_NOT_FOUND);
    cleanup_tmpdir();
}

static void slurp_rejects_bad_arguments(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *buf = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_persona_card_slurp(NULL, "cf", 2, ".x", &buf, &len), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_persona_card_slurp(&alloc, NULL, 0, ".x", &buf, &len), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_persona_card_slurp(&alloc, "cf", 0, ".x", &buf, &len), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_persona_card_slurp(&alloc, "cf", 2, NULL, &buf, &len), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_persona_card_slurp(&alloc, "cf", 2, ".x", NULL, &len), HU_ERR_INVALID_ARGUMENT);
}

static void parse_object_accepts_an_object_and_frees_on_rejection(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *root = NULL;
    const char *obj = "{\"n\":3,\"window\":{\"start\":\"2026-07-07\",\"end\":\"2026-09-05\"}}";
    HU_ASSERT_EQ(hu_persona_card_parse_object(&alloc, obj, strlen(obj), &root), HU_OK);
    HU_ASSERT_NOT_NULL(root);
    HU_ASSERT_EQ(root->type, HU_JSON_OBJECT);
    HU_ASSERT_FLOAT_EQ(hu_json_get_number(root, "n", 0.0), 3.0, 1e-9);
    hu_json_free(&alloc, root);

    root = (hu_json_value_t *)1;
    const char *arr = "[1,2,3]";
    HU_ASSERT_EQ(hu_persona_card_parse_object(&alloc, arr, strlen(arr), &root),
                 HU_ERR_INVALID_ARGUMENT); /* not an object: freed, NULLed */
    HU_ASSERT_NULL(root);

    root = (hu_json_value_t *)1;
    HU_ASSERT_NEQ(hu_persona_card_parse_object(&alloc, "{not json", 9, &root), HU_OK);
    HU_ASSERT_NULL(root);

    HU_ASSERT_EQ(hu_persona_card_parse_object(&alloc, NULL, 0, &root), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_persona_card_parse_object(NULL, obj, strlen(obj), &root),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_persona_card_parse_object(&alloc, obj, strlen(obj), NULL),
                 HU_ERR_INVALID_ARGUMENT);
}

static void copy_window_copies_dates_and_ignores_missing_or_malformed(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *root = NULL;
    const char *obj = "{\"window\":{\"start\":\"2026-07-07\",\"end\":\"2026-09-05\"}}";
    HU_ASSERT_EQ(hu_persona_card_parse_object(&alloc, obj, strlen(obj), &root), HU_OK);
    char start[16] = "unset", end[16] = "unset";
    hu_persona_card_copy_window(root, start, sizeof(start), end, sizeof(end));
    HU_ASSERT_STR_EQ(start, "2026-07-07");
    HU_ASSERT_STR_EQ(end, "2026-09-05");
    hu_json_free(&alloc, root);

    /* No window, window not an object, NULL root: buffers untouched. */
    const char *cases[] = {"{\"n\":1}", "{\"window\":\"2026\"}", "{\"window\":{\"days\":60}}"};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        HU_ASSERT_EQ(hu_persona_card_parse_object(&alloc, cases[i], strlen(cases[i]), &root),
                     HU_OK);
        snprintf(start, sizeof(start), "keep");
        snprintf(end, sizeof(end), "keep");
        hu_persona_card_copy_window(root, start, sizeof(start), end, sizeof(end));
        HU_ASSERT_STR_EQ(start, "keep");
        HU_ASSERT_STR_EQ(end, "keep");
        hu_json_free(&alloc, root);
    }
    snprintf(start, sizeof(start), "keep");
    hu_persona_card_copy_window(NULL, start, sizeof(start), end, sizeof(end));
    HU_ASSERT_STR_EQ(start, "keep");
}

void run_card_file_tests(void) {
    HU_TEST_SUITE("card_file");
    HU_RUN_TEST(slurp_missing_file_is_not_found_and_leaves_buf_null);
    HU_RUN_TEST(slurp_reads_name_plus_suffix_from_persona_dir);
    HU_RUN_TEST(slurp_rejects_bad_arguments);
    HU_RUN_TEST(parse_object_accepts_an_object_and_frees_on_rejection);
    HU_RUN_TEST(copy_window_copies_dates_and_ignores_missing_or_malformed);
}
