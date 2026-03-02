#include "test_framework.h"
#include "seaclaw/agent/prompt.h"
#include "seaclaw/agent/memory_loader.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/memory.h"
#include "seaclaw/tools/shell.h"
#include "seaclaw/tools/factory.h"
#include <string.h>

/* ─── Prompt builder tests ───────────────────────────────────────────────── */

static void test_prompt_build_basic(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_prompt_config_t cfg = {
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
    sc_error_t err = sc_prompt_build_system(&alloc, &cfg, &out, &out_len);

    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(out);
    SC_ASSERT_TRUE(out_len > 0);
    SC_ASSERT_TRUE(strstr(out, "SeaClaw") != NULL);
    SC_ASSERT_TRUE(strstr(out, "ollama") != NULL);
    SC_ASSERT_TRUE(strstr(out, "llama3") != NULL);
    SC_ASSERT_TRUE(strstr(out, "/home/user") != NULL);
    SC_ASSERT_TRUE(strstr(out, "supervised") != NULL);
    SC_ASSERT_TRUE(strstr(out, "Available Tools") != NULL);
    SC_ASSERT_TRUE(strstr(out, "(none)") != NULL);
    SC_ASSERT_TRUE(strstr(out, "Memory Context") != NULL);
    SC_ASSERT_TRUE(strstr(out, "Rules") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_prompt_build_with_tools(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_error_t err = sc_shell_create(&alloc, "/tmp", 4, NULL, &tool);
    SC_ASSERT_EQ(err, SC_OK);

    sc_prompt_config_t cfg = {
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
    err = sc_prompt_build_system(&alloc, &cfg, &out, &out_len);

    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(out);
    SC_ASSERT_TRUE(strstr(out, "shell") != NULL);
    SC_ASSERT_TRUE(strstr(out, "full") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
    if (tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_prompt_build_with_memory(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *mem = "### Memory: user_pref\nUser prefers dark mode.\n(stored: 2024-01-15)\n\n";
    sc_prompt_config_t cfg = {
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
    sc_error_t err = sc_prompt_build_system(&alloc, &cfg, &out, &out_len);

    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(out);
    SC_ASSERT_TRUE(strstr(out, "user_pref") != NULL);
    SC_ASSERT_TRUE(strstr(out, "dark mode") != NULL);
    SC_ASSERT_TRUE(strstr(out, "readonly") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_prompt_build_with_custom_instructions(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *custom = "Always respond in French.";
    sc_prompt_config_t cfg = {
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
    sc_error_t err = sc_prompt_build_system(&alloc, &cfg, &out, &out_len);

    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(out);
    SC_ASSERT_TRUE(strstr(out, "French") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
}

/* ─── Memory loader tests ────────────────────────────────────────────────── */

static void test_memory_loader_empty_backend(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_none_memory_create(&alloc);

    sc_memory_loader_t loader;
    sc_error_t err = sc_memory_loader_init(&loader, &alloc, &mem, 10, 4000);
    SC_ASSERT_EQ(err, SC_OK);

    char *ctx = NULL;
    size_t ctx_len = 0;
    err = sc_memory_loader_load(&loader, "query", 5, "", 0, &ctx, &ctx_len);

    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NULL(ctx);
    SC_ASSERT_EQ(ctx_len, 0);

    mem.vtable->deinit(mem.ctx);
}

#ifdef SC_ENABLE_SQLITE
static void test_memory_loader_with_entries(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_sqlite_memory_create(&alloc, ":memory:");

    sc_memory_category_t cat = { .tag = SC_MEMORY_CATEGORY_CORE };
    sc_error_t err = mem.vtable->store(mem.ctx, "pref_theme", 10, "User likes light mode", 21, &cat, NULL, 0);
    SC_ASSERT_EQ(err, SC_OK);

    sc_memory_loader_t loader;
    err = sc_memory_loader_init(&loader, &alloc, &mem, 10, 4000);
    SC_ASSERT_EQ(err, SC_OK);

    char *ctx = NULL;
    size_t ctx_len = 0;
    /* Query "light" matches content "User likes light mode" via recall */
    err = sc_memory_loader_load(&loader, "light", 5, "", 0, &ctx, &ctx_len);

    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(ctx);
    SC_ASSERT_TRUE(ctx_len > 0);
    SC_ASSERT_TRUE(strstr(ctx, "pref_theme") != NULL);
    SC_ASSERT_TRUE(strstr(ctx, "light mode") != NULL);
    SC_ASSERT_TRUE(strstr(ctx, "### Memory:") != NULL);

    alloc.free(alloc.ctx, ctx, ctx_len + 1);
    mem.vtable->deinit(mem.ctx);
}
#endif

void run_prompt_tests(void) {
    SC_TEST_SUITE("Prompt and memory loader");
    SC_RUN_TEST(test_prompt_build_basic);
    SC_RUN_TEST(test_prompt_build_with_tools);
    SC_RUN_TEST(test_prompt_build_with_memory);
    SC_RUN_TEST(test_prompt_build_with_custom_instructions);
    SC_RUN_TEST(test_memory_loader_empty_backend);
#ifdef SC_ENABLE_SQLITE
    SC_RUN_TEST(test_memory_loader_with_entries);
#endif
}
