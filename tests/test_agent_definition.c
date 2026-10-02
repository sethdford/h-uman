#include "human/agent/agent_definition.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void *ta_alloc(void *ctx, size_t size) {
    (void)ctx;
    return malloc(size);
}
static void *ta_realloc(void *ctx, void *ptr, size_t old_size, size_t new_size) {
    (void)ctx;
    (void)old_size;
    return realloc(ptr, new_size);
}
static void ta_free(void *ctx, void *ptr, size_t size) {
    (void)ctx;
    (void)size;
    free(ptr);
}
static hu_allocator_t test_alloc = {
    .alloc = ta_alloc, .realloc = ta_realloc, .free = ta_free, .ctx = NULL};

static char g_tmp[256];

static void write_f(const char *dir, const char *name, const char *text) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    HU_ASSERT_NOT_NULL(f);
    if (text)
        fwrite(text, 1, strlen(text), f);
    fclose(f);
}

static void test_agent_definition_load_empty_under_test_without_fixture(void) {
    unsetenv("HU_AGENT_DEFINITION_FIXTURE");
    hu_agent_definition_t def;
    memset(&def, 0xab, sizeof(def));
    hu_error_t err = hu_agent_definition_load(&test_alloc, "/nonexistent/workspace", &def);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NULL(def.soul_name);
    HU_ASSERT_NULL(def.rules);
    HU_ASSERT_EQ(def.rules_count, 0u);
    hu_agent_definition_deinit(&def, &test_alloc);
}

static void test_agent_definition_load_parses_workspace(void) {
    snprintf(g_tmp, sizeof(g_tmp), "/tmp/hu_agentdef_XXXXXX");
    HU_ASSERT_NOT_NULL(mkdtemp(g_tmp));

    write_f(g_tmp, "SOUL.md",
            "---\n"
            "name: Test Soul\n"
            "voice: calm\n"
            "traits: curious, precise\n"
            "---\n"
            "\n"
            "Body line.\n");
    write_f(g_tmp, "RULES.md",
            "# Rules\n"
            "## First rule\n"
            "Do A.\n"
            "## Second\n"
            "Do B.\n");
    write_f(g_tmp, "MEMORY.md", "Remember this.\n");
    write_f(g_tmp, "TOOLS.md", "- tool_a\n  ignored\n- tool_b\n");

    HU_ASSERT_EQ(setenv("HU_AGENT_DEFINITION_FIXTURE", g_tmp, 1), 0);

    hu_agent_definition_t def = {0};
    hu_error_t err = hu_agent_definition_load(&test_alloc, "/ignored", &def);
    HU_ASSERT_EQ(err, HU_OK);

    HU_ASSERT_NOT_NULL(def.soul_name);
    HU_ASSERT_STR_EQ(def.soul_name, "Test Soul");
    HU_ASSERT_NOT_NULL(def.soul_voice);
    HU_ASSERT_STR_EQ(def.soul_voice, "calm");
    HU_ASSERT_EQ(def.soul_traits_count, 2u);
    HU_ASSERT_NOT_NULL(def.soul_traits[0]);
    HU_ASSERT_STR_EQ(def.soul_traits[0], "curious");
    HU_ASSERT_NOT_NULL(def.soul_body);
    HU_ASSERT_TRUE(strstr(def.soul_body, "Body line.") != NULL);

    HU_ASSERT_EQ(def.rules_count, 2u);
    HU_ASSERT_TRUE(strstr(def.rules[0], "## First rule") != NULL);
    HU_ASSERT_TRUE(strstr(def.rules[1], "## Second") != NULL);

    HU_ASSERT_NOT_NULL(def.memory_context);
    HU_ASSERT_STR_EQ(def.memory_context, "Remember this.\n");

    HU_ASSERT_EQ(def.enabled_tools_count, 2u);
    HU_ASSERT_STR_EQ(def.enabled_tools[0], "tool_a");
    HU_ASSERT_STR_EQ(def.enabled_tools[1], "tool_b");

    hu_agent_definition_deinit(&def, &test_alloc);
    unsetenv("HU_AGENT_DEFINITION_FIXTURE");

    char cmd[384];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_tmp);
    if (system(cmd) != 0) {
        /* best-effort cleanup; a (void) cast does not silence glibc warn_unused_result */
    }
}

void run_agent_definition_tests(void) {
    HU_TEST_SUITE("agent_definition");
    HU_RUN_TEST(test_agent_definition_load_empty_under_test_without_fixture);
    HU_RUN_TEST(test_agent_definition_load_parses_workspace);
}
