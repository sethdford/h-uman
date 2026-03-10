#include "human/agent/memory_loader.h"
#include "human/agent/prompt.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include "human/tools/factory.h"
#include "human/tools/shell.h"
#include "test_framework.h"
#include <string.h>

/* ─── Prompt builder tests ───────────────────────────────────────────────── */

static void test_prompt_build_basic(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_config_t cfg = {
        .provider_name = "ollama",
        .provider_name_len = 6,
        .model_name = "llama3",
        .model_name_len = 6,
        .workspace_dir = "/home/user",
        .workspace_dir_len = 10,
        .tools = NULL,
        .tools_count = 0,
        .memory_context = NULL,
        .memory_context_len = 0,
        .autonomy_level = 1,
        .custom_instructions = NULL,
        .custom_instructions_len = 0,
    };

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_prompt_build_system(&alloc, &cfg, &out, &out_len);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(out, "Human") != NULL);
    HU_ASSERT_TRUE(strstr(out, "ollama") != NULL);
    HU_ASSERT_TRUE(strstr(out, "llama3") != NULL);
    HU_ASSERT_TRUE(strstr(out, "/home/user") != NULL);
    HU_ASSERT_TRUE(strstr(out, "supervised") != NULL);
    HU_ASSERT_TRUE(strstr(out, "Available Tools") != NULL);
    HU_ASSERT_TRUE(strstr(out, "(none)") != NULL);
    HU_ASSERT_TRUE(strstr(out, "Memory Context") != NULL);
    HU_ASSERT_TRUE(strstr(out, "Rules") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_prompt_build_with_tools(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_shell_create(&alloc, "/tmp", 4, NULL, &tool);
    HU_ASSERT_EQ(err, HU_OK);

    hu_prompt_config_t cfg = {
        .provider_name = "openai",
        .provider_name_len = 5,
        .model_name = "gpt-4",
        .model_name_len = 5,
        .workspace_dir = ".",
        .workspace_dir_len = 1,
        .tools = &tool,
        .tools_count = 1,
        .memory_context = NULL,
        .memory_context_len = 0,
        .autonomy_level = 2,
        .custom_instructions = NULL,
        .custom_instructions_len = 0,
    };

    char *out = NULL;
    size_t out_len = 0;
    err = hu_prompt_build_system(&alloc, &cfg, &out, &out_len);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "shell") != NULL);
    HU_ASSERT_TRUE(strstr(out, "full") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_prompt_build_with_memory(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *mem = "### Memory: user_pref\nUser prefers dark mode.\n(stored: 2024-01-15)\n\n";
    hu_prompt_config_t cfg = {
        .provider_name = "anthropic",
        .provider_name_len = 9,
        .model_name = "claude-3",
        .model_name_len = 8,
        .workspace_dir = "/ws",
        .workspace_dir_len = 3,
        .tools = NULL,
        .tools_count = 0,
        .memory_context = mem,
        .memory_context_len = strlen(mem),
        .autonomy_level = 0,
        .custom_instructions = NULL,
        .custom_instructions_len = 0,
    };

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_prompt_build_system(&alloc, &cfg, &out, &out_len);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "user_pref") != NULL);
    HU_ASSERT_TRUE(strstr(out, "dark mode") != NULL);
    HU_ASSERT_TRUE(strstr(out, "readonly") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_prompt_build_with_stm_context(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *stm = "**user**: Hello\n\n**assistant**: Hi there!\n\n";
    hu_prompt_config_t cfg = {
        .provider_name = "ollama",
        .provider_name_len = 6,
        .model_name = "llama3",
        .model_name_len = 6,
        .workspace_dir = ".",
        .workspace_dir_len = 1,
        .tools = NULL,
        .tools_count = 0,
        .memory_context = NULL,
        .memory_context_len = 0,
        .stm_context = stm,
        .stm_context_len = strlen(stm),
        .autonomy_level = 1,
        .custom_instructions = NULL,
        .custom_instructions_len = 0,
    };

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_prompt_build_system(&alloc, &cfg, &out, &out_len);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "Session Context") != NULL);
    HU_ASSERT_TRUE(strstr(out, "Hello") != NULL);
    HU_ASSERT_TRUE(strstr(out, "Hi there") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_prompt_build_with_custom_instructions(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *custom = "Always respond in French.";
    hu_prompt_config_t cfg = {
        .provider_name = "ollama",
        .provider_name_len = 6,
        .model_name = "mistral",
        .model_name_len = 7,
        .workspace_dir = ".",
        .workspace_dir_len = 1,
        .tools = NULL,
        .tools_count = 0,
        .memory_context = NULL,
        .memory_context_len = 0,
        .autonomy_level = 1,
        .custom_instructions = custom,
        .custom_instructions_len = strlen(custom),
    };

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_prompt_build_system(&alloc, &cfg, &out, &out_len);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "French") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
}

/* ─── Memory loader tests ────────────────────────────────────────────────── */

static void test_memory_loader_empty_backend(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);

    hu_memory_loader_t loader;
    hu_error_t err = hu_memory_loader_init(&loader, &alloc, &mem, NULL, 10, 4000);
    HU_ASSERT_EQ(err, HU_OK);

    char *ctx = NULL;
    size_t ctx_len = 0;
    err = hu_memory_loader_load(&loader, "query", 5, "", 0, &ctx, &ctx_len);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NULL(ctx);
    HU_ASSERT_EQ(ctx_len, 0);

    mem.vtable->deinit(mem.ctx);
}

#ifdef HU_ENABLE_SQLITE
static void test_memory_loader_with_entries(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");

    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    hu_error_t err =
        mem.vtable->store(mem.ctx, "pref_theme", 10, "User likes light mode", 21, &cat, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);

    hu_memory_loader_t loader;
    err = hu_memory_loader_init(&loader, &alloc, &mem, NULL, 10, 4000);
    HU_ASSERT_EQ(err, HU_OK);

    char *ctx = NULL;
    size_t ctx_len = 0;
    /* Query "light" matches content "User likes light mode" via recall */
    err = hu_memory_loader_load(&loader, "light", 5, "", 0, &ctx, &ctx_len);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(ctx_len > 0);
    HU_ASSERT_TRUE(strstr(ctx, "pref_theme") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "light mode") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "### Memory:") != NULL);

    alloc.free(alloc.ctx, ctx, ctx_len + 1);
    mem.vtable->deinit(mem.ctx);
}
#endif

void run_prompt_tests(void) {
    HU_TEST_SUITE("Prompt and memory loader");
    HU_RUN_TEST(test_prompt_build_basic);
    HU_RUN_TEST(test_prompt_build_with_tools);
    HU_RUN_TEST(test_prompt_build_with_memory);
    HU_RUN_TEST(test_prompt_build_with_stm_context);
    HU_RUN_TEST(test_prompt_build_with_custom_instructions);
    HU_RUN_TEST(test_memory_loader_empty_backend);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_memory_loader_with_entries);
#endif
}
