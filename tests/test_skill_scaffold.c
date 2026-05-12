#include "test_framework.h"

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/skill_scaffold.h"

static void *test_alloc(void *ctx, size_t size) {
    (void)ctx;
    return malloc(size);
}
static void test_free(void *ctx, void *ptr, size_t size) {
    (void)ctx;
    (void)size;
    free(ptr);
}
static hu_allocator_t s_alloc = {.alloc = test_alloc, .free = test_free};

static void test_manifest_basic(void) {
    hu_skill_scaffold_opts_t opts = {
        .name = "my-skill",
        .description = "A test skill",
        .author = "tester",
        .category = HU_SKILL_CATEGORY_GENERAL,
    };
    char *out = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_skill_scaffold_manifest(&s_alloc, &opts, &out, &len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_GT(len, 0);
    HU_ASSERT_STR_CONTAINS(out, "\"my-skill\"");
    HU_ASSERT_STR_CONTAINS(out, "\"A test skill\"");
    HU_ASSERT_STR_CONTAINS(out, "\"tester\"");
    HU_ASSERT_STR_CONTAINS(out, "\"general\"");
    HU_ASSERT_STR_CONTAINS(out, "\"0.1.0\"");
    s_alloc.free(s_alloc.ctx, out, len + 1);
}

static void test_manifest_data_category(void) {
    hu_skill_scaffold_opts_t opts = {
        .name = "data-fetcher",
        .category = HU_SKILL_CATEGORY_DATA,
    };
    char *out = NULL;
    HU_ASSERT_EQ(hu_skill_scaffold_manifest(&s_alloc, &opts, &out, NULL), HU_OK);
    HU_ASSERT_STR_CONTAINS(out, "\"data\"");
    s_alloc.free(s_alloc.ctx, out, strlen(out) + 1);
}

static void test_manifest_null_args(void) {
    char *out = NULL;
    hu_skill_scaffold_opts_t opts = {.name = "test"};
    HU_ASSERT_EQ(hu_skill_scaffold_manifest(NULL, &opts, &out, NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_skill_scaffold_manifest(&s_alloc, NULL, &out, NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_skill_scaffold_manifest(&s_alloc, &opts, NULL, NULL), HU_ERR_INVALID_ARGUMENT);
    opts.name = NULL;
    HU_ASSERT_EQ(hu_skill_scaffold_manifest(&s_alloc, &opts, &out, NULL), HU_ERR_INVALID_ARGUMENT);
}

static void test_instructions_basic(void) {
    hu_skill_scaffold_opts_t opts = {
        .name = "my-skill",
        .description = "Helpful skill",
    };
    char *out = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_skill_scaffold_instructions(&s_alloc, &opts, &out, &len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_GT(len, 0);
    HU_ASSERT_STR_CONTAINS(out, "# my-skill");
    HU_ASSERT_STR_CONTAINS(out, "Helpful skill");
    HU_ASSERT_STR_CONTAINS(out, "## When to Use");
    HU_ASSERT_STR_CONTAINS(out, "## Instructions");
    HU_ASSERT_STR_CONTAINS(out, "## Examples");
    HU_ASSERT_STR_CONTAINS(out, "## Constraints");
    s_alloc.free(s_alloc.ctx, out, len + 1);
}

static void test_instructions_has_frontmatter(void) {
    hu_skill_scaffold_opts_t opts = {.name = "fm-test"};
    char *out = NULL;
    HU_ASSERT_EQ(hu_skill_scaffold_instructions(&s_alloc, &opts, &out, NULL), HU_OK);
    HU_ASSERT_EQ(strncmp(out, "---\n", 4), 0);
    HU_ASSERT_STR_CONTAINS(out, "name: fm-test");
    HU_ASSERT_STR_CONTAINS(out, "version: 0.1.0");
    s_alloc.free(s_alloc.ctx, out, strlen(out) + 1);
}

static void test_instructions_null_args(void) {
    char *out = NULL;
    hu_skill_scaffold_opts_t opts = {.name = "test"};
    HU_ASSERT_EQ(hu_skill_scaffold_instructions(NULL, &opts, &out, NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_skill_scaffold_instructions(&s_alloc, NULL, &out, NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_skill_scaffold_instructions(&s_alloc, &opts, NULL, NULL), HU_ERR_INVALID_ARGUMENT);
}

static void test_init_validates_name(void) {
    hu_skill_scaffold_opts_t opts = {.name = NULL};
    HU_ASSERT_EQ(hu_skill_scaffold_init(&s_alloc, &opts), HU_ERR_INVALID_ARGUMENT);

    opts.name = "";
    HU_ASSERT_EQ(hu_skill_scaffold_init(&s_alloc, &opts), HU_ERR_INVALID_ARGUMENT);
}

static void test_init_success(void) {
    hu_skill_scaffold_opts_t opts = {
        .name = "test-skill",
        .description = "A scaffold test",
        .author = "bot",
        .category = HU_SKILL_CATEGORY_AUTOMATION,
    };
    HU_ASSERT_EQ(hu_skill_scaffold_init(&s_alloc, &opts), HU_OK);
}

static void test_init_null_alloc(void) {
    hu_skill_scaffold_opts_t opts = {.name = "test"};
    HU_ASSERT_EQ(hu_skill_scaffold_init(NULL, &opts), HU_ERR_INVALID_ARGUMENT);
}

static void test_init_null_opts(void) {
    HU_ASSERT_EQ(hu_skill_scaffold_init(&s_alloc, NULL), HU_ERR_INVALID_ARGUMENT);
}

static void test_manifest_all_categories(void) {
    hu_skill_category_t cats[] = {
        HU_SKILL_CATEGORY_GENERAL,     HU_SKILL_CATEGORY_DATA,     HU_SKILL_CATEGORY_AUTOMATION,
        HU_SKILL_CATEGORY_INTEGRATION, HU_SKILL_CATEGORY_ANALYSIS,
    };
    const char *expected[] = {"general", "data", "automation", "integration", "analysis"};
    for (int i = 0; i < 5; i++) {
        hu_skill_scaffold_opts_t opts = {.name = "cat-test", .category = cats[i]};
        char *out = NULL;
        HU_ASSERT_EQ(hu_skill_scaffold_manifest(&s_alloc, &opts, &out, NULL), HU_OK);
        HU_ASSERT_STR_CONTAINS(out, expected[i]);
        s_alloc.free(s_alloc.ctx, out, strlen(out) + 1);
    }
}

static void test_manifest_default_description(void) {
    hu_skill_scaffold_opts_t opts = {.name = "bare"};
    char *out = NULL;
    HU_ASSERT_EQ(hu_skill_scaffold_manifest(&s_alloc, &opts, &out, NULL), HU_OK);
    HU_ASSERT_STR_CONTAINS(out, "A custom human skill");
    s_alloc.free(s_alloc.ctx, out, strlen(out) + 1);
}

void run_skill_scaffold_tests(void) {
    HU_TEST_SUITE("skill_scaffold");
    HU_RUN_TEST(test_manifest_basic);
    HU_RUN_TEST(test_manifest_data_category);
    HU_RUN_TEST(test_manifest_null_args);
    HU_RUN_TEST(test_instructions_basic);
    HU_RUN_TEST(test_instructions_has_frontmatter);
    HU_RUN_TEST(test_instructions_null_args);
    HU_RUN_TEST(test_init_validates_name);
    HU_RUN_TEST(test_init_success);
    HU_RUN_TEST(test_init_null_alloc);
    HU_RUN_TEST(test_init_null_opts);
    HU_RUN_TEST(test_manifest_all_categories);
    HU_RUN_TEST(test_manifest_default_description);
}
