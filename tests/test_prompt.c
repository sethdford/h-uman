#include "human/agent/memory_loader.h"
#include "human/agent/prompt.h"
#include "human/agent/prompt_budget.h"
#include "human/agent/prompt_trim.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include "human/persona.h"
#include "human/tools/factory.h"
#include "human/tools/shell.h"
#include "test_framework.h"
#include <stdlib.h>
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
    hu_error_t err = hu_prompt_build_system(&alloc, &cfg, NULL, NULL, &out, &out_len);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(out, "real person") != NULL); /* base identity present */
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
    err = hu_prompt_build_system(&alloc, &cfg, NULL, NULL, &out, &out_len);

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
    hu_error_t err = hu_prompt_build_system(&alloc, &cfg, NULL, NULL, &out, &out_len);

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
    hu_error_t err = hu_prompt_build_system(&alloc, &cfg, NULL, NULL, &out, &out_len);

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
    hu_error_t err = hu_prompt_build_system(&alloc, &cfg, NULL, NULL, &out, &out_len);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "French") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_prompt_build_includes_hula_protocol(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.provider_name = "openai";
    cfg.provider_name_len = 5;
    cfg.model_name = "gpt-4";
    cfg.model_name_len = 5;
    cfg.workspace_dir = ".";
    cfg.workspace_dir_len = 1;
    cfg.autonomy_level = 2;
    cfg.hula_program_protocol = true;

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_prompt_build_system(&alloc, &cfg, NULL, NULL, &out, &out_len);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "<hula_program>") != NULL);
    HU_ASSERT_TRUE(strstr(out, "HuLa") != NULL);

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
    /* Contract: the returned length must match the returned buffer. */
    HU_ASSERT_EQ(ctx_len, strlen(ctx));

    alloc.free(alloc.ctx, ctx, ctx_len + 1);
    mem.vtable->deinit(mem.ctx);
}

/* Regression pin for BUG B (2026-07-13, distinct from the loader strlen fix in
 * commit 85f4d892). A stored memory whose content contains an embedded NUL byte
 * must NOT trigger a heap-buffer-overflow during context assembly.
 *
 * Root cause: the sqlite recall path copied content with hu_strndup (which
 * truncates at the first NUL, so the returned buffer is shorter than the stored
 * byte count) but reported the FULL column byte length as content_len. During
 * context assembly, memory_loader.c calls hu_json_buf_append_raw(content,
 * content_len) → memcpy reads content_len bytes → over-read past the truncated
 * buffer. Under ASan the pre-fix code aborts before hu_memory_loader_load
 * returns. Post-fix, content_len equals the actual returned buffer length. */
static void test_memory_loader_embedded_nul_no_overflow(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");

    /* Array literal so the embedded NUL is preserved; content_len spans it. */
    static const char content[] = "seth loves \0 hidden tail after nul";
    size_t content_len = sizeof(content) - 1; /* 34 — includes bytes past the NUL */

    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    hu_error_t err = mem.vtable->store(mem.ctx, "nul_key", 7, content, content_len, &cat, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);

    hu_memory_loader_t loader;
    err = hu_memory_loader_init(&loader, &alloc, &mem, NULL, 10, 4000);
    HU_ASSERT_EQ(err, HU_OK);

    char *ctx = NULL;
    size_t ctx_len = 0;
    /* Query "seth" (before the NUL) recalls the entry; assembly must not over-read. */
    err = hu_memory_loader_load(&loader, "seth", 4, "", 0, &ctx, &ctx_len);
    HU_ASSERT_EQ(err, HU_OK);

    if (ctx)
        alloc.free(alloc.ctx, ctx, ctx_len + 1);
    mem.vtable->deinit(mem.ctx);
}

/* Direct contract check: a stored+recalled entry's content_len must equal the
 * actual byte length of the returned content buffer. When content has an
 * embedded NUL, hu_strndup truncates the buffer, so content_len must track the
 * truncated length — never the original stored byte count. */
static void test_sqlite_recall_content_len_matches_buffer(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");

    static const char content[] = "prefix\0suffix-past-nul";
    size_t content_len = sizeof(content) - 1; /* 22 — spans the NUL */

    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    hu_error_t err = mem.vtable->store(mem.ctx, "nul_key2", 8, content, content_len, &cat, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);

    hu_memory_entry_t *entries = NULL;
    size_t count = 0;
    err = mem.vtable->recall(mem.ctx, &alloc, "prefix", 6, 10, "", 0, &entries, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(count >= 1);

    /* The invariant: reported length equals the actual NUL-terminated buffer. */
    for (size_t i = 0; i < count; i++) {
        if (entries[i].content)
            HU_ASSERT_EQ(entries[i].content_len, strlen(entries[i].content));
        hu_memory_entry_free_fields(&alloc, &entries[i]);
    }

    alloc.free(alloc.ctx, entries, count * sizeof(hu_memory_entry_t));
    mem.vtable->deinit(mem.ctx);
}
#endif

/* Calibrated-uncertainty Task 3: the [conf=0.X] confidence-tagging addendum
 * must be appended ONLY when the caller flags the query as factual. */
static void test_prompt_addendum_present_on_factual_query(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_config_t cfg = {
        .provider_name = "ollama",
        .provider_name_len = 6,
        .model_name = "llama3",
        .model_name_len = 6,
        .is_factual_query = true,
    };
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_prompt_build_system(&alloc, &cfg, NULL, NULL, &out, &out_len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "CONFIDENCE TAGGING") != NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_prompt_addendum_absent_on_nonfactual_query(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_config_t cfg = {
        .provider_name = "ollama",
        .provider_name_len = 6,
        .model_name = "llama3",
        .model_name_len = 6,
        .is_factual_query = false, /* default — casual turn */
    };
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_prompt_build_system(&alloc, &cfg, NULL, NULL, &out, &out_len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "CONFIDENCE TAGGING") == NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_prompt_graph_context_present(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    hu_prompt_config_t cfg = {
        .provider_name = "ollama",
        .provider_name_len = 6,
        .model_name = "llama3",
        .model_name_len = 6,
        .workspace_dir = ".",
        .workspace_dir_len = 1,
        .autonomy_level = 1,
        .graph_context = "Climbing partner since 2019; talks in short bursts.",
        .graph_context_len = 51,
    };
    hu_error_t err = hu_prompt_build_system(&alloc, &cfg, NULL, NULL, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "Relationship Context") != NULL);
    HU_ASSERT_TRUE(strstr(out, "Climbing partner since 2019") != NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_prompt_graph_context_absent_is_noop(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    hu_prompt_config_t cfg = {
        .provider_name = "ollama",
        .provider_name_len = 6,
        .model_name = "llama3",
        .model_name_len = 6,
        .workspace_dir = ".",
        .workspace_dir_len = 1,
        .autonomy_level = 1,
        /* graph_context deliberately unset */
    };
    hu_error_t err = hu_prompt_build_system(&alloc, &cfg, NULL, NULL, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(out, "Relationship Context") == NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_prompt_graph_context_present_immersive(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    hu_prompt_config_t cfg = {
        .provider_name = "ollama",
        .provider_name_len = 6,
        .model_name = "llama3",
        .model_name_len = 6,
        .workspace_dir = ".",
        .workspace_dir_len = 1,
        .autonomy_level = 1,
        .persona_immersive = true,
        .persona_prompt = "Be yourself.",
        .persona_prompt_len = 12,
        .graph_context = "Climbing partner since 2019; talks in short bursts.",
        .graph_context_len = 51,
    };
    hu_error_t err = hu_prompt_build_system(&alloc, &cfg, NULL, NULL, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "Relationship Context") != NULL);
    HU_ASSERT_TRUE(strstr(out, "Climbing partner since 2019") != NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
}

/* ─── Immersive value-aware trim + compact safety section (HU_PROMPT_TRIM) ──
 *
 * Pre/post contract per integration-done-contract.md: gate OFF preserves
 * today's behavior (prompt stays over budget, oldest memory intact); gate
 * LIVE lands under HU_PROMPT_TRIM_BUDGET_BYTES by dropping MIDDLE bytes
 * while the persona head and the CRITICAL REMINDER tail survive. */

/* Builds a >16K memory context: an OLDEST marker line, filler lines, and a
 * NEWEST marker line. Caller frees (len + 1). */
static char *big_memory_context(hu_allocator_t *alloc, size_t *out_len) {
    const size_t target = 20000;
    char *buf = (char *)alloc->alloc(alloc->ctx, target + 64);
    HU_ASSERT_NOT_NULL(buf);
    size_t len = 0;
    static const char oldest[] = "- OLDEST_FACT_MARKER first thing learned\n";
    memcpy(buf + len, oldest, sizeof(oldest) - 1);
    len += sizeof(oldest) - 1;
    static const char filler[] = "- remembered detail about a shared plan and a place\n";
    while (len < target - 128) {
        memcpy(buf + len, filler, sizeof(filler) - 1);
        len += sizeof(filler) - 1;
    }
    static const char newest[] = "- NEWEST_FACT_MARKER latest thing learned\n";
    memcpy(buf + len, newest, sizeof(newest) - 1);
    len += sizeof(newest) - 1;
    buf[len] = '\0';
    *out_len = len;
    return buf;
}

static hu_prompt_config_t immersive_cfg(const char *memory_ctx, size_t memory_ctx_len,
                                        const hu_persona_t *persona) {
    hu_prompt_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.persona_immersive = true;
    cfg.persona_prompt = "PERSONA_HEAD_MARKER you text like yourself.";
    cfg.persona_prompt_len = strlen(cfg.persona_prompt);
    cfg.memory_context = memory_ctx;
    cfg.memory_context_len = memory_ctx_len;
    cfg.persona = persona;
    cfg.suppress_prompt_budget_diagnostic = true;
    return cfg;
}

static void test_prompt_immersive_trim_live_drops_middle_keeps_tail(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t mem_len = 0;
    char *mem = big_memory_context(&alloc, &mem_len);
    static const char *reinforce[] = {"TAIL_REMINDER_MARKER never break character."};
    hu_persona_t persona;
    memset(&persona, 0, sizeof(persona));
    persona.immersive_reinforcement = (char **)reinforce;
    persona.immersive_reinforcement_count = 1;
    hu_prompt_config_t cfg = immersive_cfg(mem, mem_len, &persona);

    setenv("HU_PROMPT_TRIM", "live", 1);
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_prompt_build_system(&alloc, &cfg, NULL, NULL, &out, &out_len);
    unsetenv("HU_PROMPT_TRIM");
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    /* Post: under the budget the positional cut would otherwise enforce. */
    HU_ASSERT_TRUE(out_len <= (size_t)HU_PROMPT_TRIM_BUDGET_BYTES);
    /* Head (persona) and tail (CRITICAL REMINDER) survive... */
    HU_ASSERT_TRUE(strstr(out, "PERSONA_HEAD_MARKER") != NULL);
    HU_ASSERT_TRUE(strstr(out, "CRITICAL REMINDER") != NULL);
    HU_ASSERT_TRUE(strstr(out, "TAIL_REMINDER_MARKER") != NULL);
    /* ...middle memory bytes are dropped oldest-first, newest kept. */
    HU_ASSERT_TRUE(strstr(out, "OLDEST_FACT_MARKER") == NULL);
    HU_ASSERT_TRUE(strstr(out, "NEWEST_FACT_MARKER") != NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
    alloc.free(alloc.ctx, mem, 20000 + 64);
}

static void test_prompt_immersive_trim_off_preserves_positional_behavior(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t mem_len = 0;
    char *mem = big_memory_context(&alloc, &mem_len);
    static const char *reinforce[] = {"TAIL_REMINDER_MARKER never break character."};
    hu_persona_t persona;
    memset(&persona, 0, sizeof(persona));
    persona.immersive_reinforcement = (char **)reinforce;
    persona.immersive_reinforcement_count = 1;
    hu_prompt_config_t cfg = immersive_cfg(mem, mem_len, &persona);

    unsetenv("HU_PROMPT_TRIM");
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_prompt_build_system(&alloc, &cfg, NULL, NULL, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    /* Pre: the builder does NOT trim when the gate is off — the prompt stays
     * over budget (the positional cut downstream is today's behavior) and
     * the oldest memory line is still present. */
    HU_ASSERT_TRUE(out_len > (size_t)HU_PROMPT_TRIM_BUDGET_BYTES);
    HU_ASSERT_TRUE(strstr(out, "OLDEST_FACT_MARKER") != NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
    alloc.free(alloc.ctx, mem, 20000 + 64);
}

static void test_prompt_immersive_trim_shadow_output_unchanged(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t mem_len = 0;
    char *mem = big_memory_context(&alloc, &mem_len);
    static const char *reinforce[] = {"TAIL_REMINDER_MARKER never break character."};
    hu_persona_t persona;
    memset(&persona, 0, sizeof(persona));
    persona.immersive_reinforcement = (char **)reinforce;
    persona.immersive_reinforcement_count = 1;
    hu_prompt_config_t cfg = immersive_cfg(mem, mem_len, &persona);

    setenv("HU_PROMPT_TRIM", "shadow", 1);
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_prompt_build_system(&alloc, &cfg, NULL, NULL, &out, &out_len);
    unsetenv("HU_PROMPT_TRIM");
    HU_ASSERT_EQ(err, HU_OK);
    /* Shadow logs the plan but must not change the emitted prompt. */
    HU_ASSERT_TRUE(out_len > (size_t)HU_PROMPT_TRIM_BUDGET_BYTES);
    HU_ASSERT_TRUE(strstr(out, "OLDEST_FACT_MARKER") != NULL);
    /* And no safety section either — shadow may not add content. */
    HU_ASSERT_TRUE(strstr(out, "Boundaries (stay in voice)") == NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
    alloc.free(alloc.ctx, mem, 20000 + 64);
}

static void test_prompt_immersive_safety_section_live_only_and_early(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* Small memory context: no trimming needed, isolates the safety append. */
    static const char mem[] = "- MEMFACT_MARKER a small remembered detail\n";
    hu_prompt_config_t cfg = immersive_cfg(mem, sizeof(mem) - 1, NULL);

    setenv("HU_PROMPT_TRIM", "live", 1);
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_prompt_build_system(&alloc, &cfg, NULL, NULL, &out, &out_len);
    unsetenv("HU_PROMPT_TRIM");
    HU_ASSERT_EQ(err, HU_OK);
    const char *safety = strstr(out, "Boundaries (stay in voice)");
    const char *memfact = strstr(out, "MEMFACT_MARKER");
    HU_ASSERT_NOT_NULL((void *)safety);
    HU_ASSERT_NOT_NULL((void *)memfact);
    /* Early placement: right after the persona head, BEFORE memory. */
    HU_ASSERT_TRUE(safety < memfact);
    alloc.free(alloc.ctx, out, out_len + 1);

    /* Gate OFF: exact current behavior — no safety section. */
    out = NULL;
    out_len = 0;
    err = hu_prompt_build_system(&alloc, &cfg, NULL, NULL, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(out, "Boundaries (stay in voice)") == NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_prompt_immersive_trim_extends_to_world_model(void) {
    /* 2026-07-12 soak: on every over-budget production turn the original
     * three spans covered only part of the overage (exemplars/graph were
     * empty, memory small). The span set therefore extends into the next
     * middle sections; world_model is the first (priority after memory).
     * Fixture: small memory + >16K world model. LIVE must consume memory
     * whole, then world_model head-first, and land under budget with the
     * guard tail intact. */
    hu_allocator_t alloc = hu_system_allocator();
    size_t wm_len = 0;
    char *wm = big_memory_context(&alloc, &wm_len); /* reuse builder: OLDEST/NEWEST markers */
    static const char mem[] = "- MEMFACT_MARKER small remembered detail\n";
    static const char *reinforce[] = {"TAIL_REMINDER_MARKER never break character."};
    hu_persona_t persona;
    memset(&persona, 0, sizeof(persona));
    persona.immersive_reinforcement = (char **)reinforce;
    persona.immersive_reinforcement_count = 1;
    hu_prompt_config_t cfg = immersive_cfg(mem, sizeof(mem) - 1, &persona);
    cfg.world_model_context = wm;
    cfg.world_model_context_len = wm_len;

    setenv("HU_PROMPT_TRIM", "live", 1);
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_prompt_build_system(&alloc, &cfg, NULL, NULL, &out, &out_len);
    unsetenv("HU_PROMPT_TRIM");
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(out_len <= (size_t)HU_PROMPT_TRIM_BUDGET_BYTES);
    HU_ASSERT_TRUE(strstr(out, "PERSONA_HEAD_MARKER") != NULL);
    HU_ASSERT_TRUE(strstr(out, "TAIL_REMINDER_MARKER") != NULL);
    /* Memory below its 2 KB floor is PROTECTED (2026-07-25 floors: the
     * un-floored trim deleted all recall on over-budget turns); the
     * cascade skips it and takes the overage from world model instead. */
    HU_ASSERT_TRUE(strstr(out, "MEMFACT_MARKER") != NULL);
    /* world model trimmed head-first: oldest gone, newest kept */
    HU_ASSERT_TRUE(strstr(out, "OLDEST_FACT_MARKER") == NULL);
    HU_ASSERT_TRUE(strstr(out, "NEWEST_FACT_MARKER") != NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
    alloc.free(alloc.ctx, wm, 20000 + 64);
}

static void test_prompt_immersive_tracks_field_stats(void) {
    /* The B3 Phase-1 per-field byte accounting must cover the IMMERSIVE
     * branch — the persona/daemon path is the only one that truncates, and
     * untracked immersive turns feed all-zero observations into the budget
     * accumulator, poisoning the DEAD-field means the structured path's
     * trim gate keys off. */
    hu_allocator_t alloc = hu_system_allocator();
    static const char mem[] = "- MEMFACT_MARKER a small remembered detail\n";
    hu_prompt_config_t cfg = immersive_cfg(mem, sizeof(mem) - 1, NULL);
    cfg.graph_context = "Climbing partner since 2019.";
    cfg.graph_context_len = 28;
    cfg.stm_context = "recent session notes";
    cfg.stm_context_len = 20;
    hu_prompt_field_stat_t stats[HU_PROMPT_FIELD_COUNT];
    memset(stats, 0, sizeof(stats));
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_prompt_build_system(&alloc, &cfg, stats, NULL, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(stats[HU_PROMPT_FIELD_PERSONA_PROMPT].bytes_contributed > 0);
    HU_ASSERT_TRUE(stats[HU_PROMPT_FIELD_MEMORY_CONTEXT].bytes_contributed >= sizeof(mem) - 1);
    HU_ASSERT_TRUE(stats[HU_PROMPT_FIELD_GRAPH_CONTEXT].bytes_contributed >= 28);
    HU_ASSERT_TRUE(stats[HU_PROMPT_FIELD_STM_CONTEXT].bytes_contributed >= 20);
    alloc.free(alloc.ctx, out, out_len + 1);
}

/* ─── Continuity context (SOTA roadmap #13 — the "goldfish" anti-tell) ─────
 *
 * Own-last-send + open promises, rendered as a compact capped section and
 * injected on the reactive path. Contract: present when data exists, absent
 * when empty, hard-capped at HU_CONTINUITY_CTX_MAX_BYTES, and trimmed FIRST
 * under HU_PROMPT_TRIM pressure (lowest-priority tier — never displaces
 * rules/persona). */

static void test_continuity_render_both_parts(void) {
    const char *promises[] = {"send the invoice", "call the plumber"};
    char buf[HU_CONTINUITY_CTX_MAX_BYTES + 1];
    static const char last[] = "on my way, be there in 10";
    size_t n = hu_continuity_context_render(last, sizeof(last) - 1, promises, 2, buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_EQ(n, strlen(buf));
    HU_ASSERT_TRUE(strstr(buf, "Your last message to them: on my way, be there in 10") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "Open promises: send the invoice; call the plumber") != NULL);
}

static void test_continuity_render_empty_returns_zero(void) {
    char buf[64];
    size_t n = hu_continuity_context_render(NULL, 0, NULL, 0, buf, sizeof(buf));
    HU_ASSERT_EQ(n, 0);
    HU_ASSERT_EQ(buf[0], '\0');
    /* Empty-string inputs are the same as absent inputs. */
    const char *empty_promise[] = {""};
    n = hu_continuity_context_render("", 0, empty_promise, 1, buf, sizeof(buf));
    HU_ASSERT_EQ(n, 0);
}

static void test_continuity_render_caps_at_max_bytes(void) {
    /* A last-send far over the cap must be truncated, never overflow. */
    char huge[2048];
    memset(huge, 'x', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';
    const char *promises[] = {"a promise that also takes space in the section"};
    char buf[2048];
    size_t n = hu_continuity_context_render(huge, sizeof(huge) - 1, promises, 1, buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(n <= (size_t)HU_CONTINUITY_CTX_MAX_BYTES);
    HU_ASSERT_EQ(n, strlen(buf));
    HU_ASSERT_TRUE(strstr(buf, "Your last message to them: ") != NULL);
}

static void test_prompt_continuity_section_present_immersive(void) {
    hu_allocator_t alloc = hu_system_allocator();
    static const char mem[] = "- MEMFACT_MARKER a small remembered detail\n";
    hu_prompt_config_t cfg = immersive_cfg(mem, sizeof(mem) - 1, NULL);
    static const char cont[] = "Your last message to them: CONTINUITY_SEND_MARKER\n"
                               "Open promises: CONTINUITY_PROMISE_MARKER\n";
    cfg.continuity_context = cont;
    cfg.continuity_context_len = sizeof(cont) - 1;
    hu_prompt_field_stat_t stats[HU_PROMPT_FIELD_COUNT];
    memset(stats, 0, sizeof(stats));
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_prompt_build_system(&alloc, &cfg, stats, NULL, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "CONTINUITY_SEND_MARKER") != NULL);
    HU_ASSERT_TRUE(strstr(out, "CONTINUITY_PROMISE_MARKER") != NULL);
    /* Wired into the per-field byte accounting so prompt_budget snapshots
     * and the DEAD-field trim can see it. */
    HU_ASSERT_TRUE(stats[HU_PROMPT_FIELD_CONTINUITY_CONTEXT].bytes_contributed >= sizeof(cont) - 1);
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_prompt_continuity_section_absent_when_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    static const char mem[] = "- MEMFACT_MARKER a small remembered detail\n";
    hu_prompt_config_t cfg = immersive_cfg(mem, sizeof(mem) - 1, NULL);
    /* continuity_context deliberately unset */
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_prompt_build_system(&alloc, &cfg, NULL, NULL, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(out, "Your last message to them") == NULL);
    HU_ASSERT_TRUE(strstr(out, "Open promises") == NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_prompt_continuity_survives_under_pressure(void) {
    /* SUPERSEDED POLICY (2026-07-25): continuity used to be the FIRST
     * casualty under budget pressure. The Dermot audit showed the live
     * trim deleting it (cont=112 → 0) along with all recalled memory —
     * own-last-send/open-promise awareness vanished exactly when threads
     * were busiest. Continuity is ≤400 B by construction and now floored
     * at 400 B: it is never cut. The overage comes out of memory above
     * its own 2 KB floor instead. */
    hu_allocator_t alloc = hu_system_allocator();
    size_t mem_len = 0;
    char *mem = big_memory_context(&alloc, &mem_len);
    static const char *reinforce[] = {"TAIL_REMINDER_MARKER never break character."};
    hu_persona_t persona;
    memset(&persona, 0, sizeof(persona));
    persona.immersive_reinforcement = (char **)reinforce;
    persona.immersive_reinforcement_count = 1;
    hu_prompt_config_t cfg = immersive_cfg(mem, mem_len, &persona);
    static const char cont[] = "Your last message to them: CONTINUITY_SEND_MARKER\n";
    cfg.continuity_context = cont;
    cfg.continuity_context_len = sizeof(cont) - 1;

    setenv("HU_PROMPT_TRIM", "live", 1);
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_prompt_build_system(&alloc, &cfg, NULL, NULL, &out, &out_len);
    unsetenv("HU_PROMPT_TRIM");
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(out_len <= (size_t)HU_PROMPT_TRIM_BUDGET_BYTES);
    /* Continuity survives its floor; memory above ITS floor absorbs the
     * overage head-first (newest kept)... */
    HU_ASSERT_TRUE(strstr(out, "CONTINUITY_SEND_MARKER") != NULL);
    HU_ASSERT_TRUE(strstr(out, "NEWEST_FACT_MARKER") != NULL);
    /* ...and the protected head/tail are intact. */
    HU_ASSERT_TRUE(strstr(out, "PERSONA_HEAD_MARKER") != NULL);
    HU_ASSERT_TRUE(strstr(out, "TAIL_REMINDER_MARKER") != NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
    alloc.free(alloc.ctx, mem, 20000 + 64);
}

void run_prompt_tests(void) {
    HU_TEST_SUITE("Prompt and memory loader");
    HU_RUN_TEST(test_prompt_build_basic);
    HU_RUN_TEST(test_prompt_build_with_tools);
    HU_RUN_TEST(test_prompt_build_with_memory);
    HU_RUN_TEST(test_prompt_graph_context_present);
    HU_RUN_TEST(test_prompt_graph_context_absent_is_noop);
    HU_RUN_TEST(test_prompt_graph_context_present_immersive);
    HU_RUN_TEST(test_prompt_immersive_trim_live_drops_middle_keeps_tail);
    HU_RUN_TEST(test_prompt_immersive_trim_off_preserves_positional_behavior);
    HU_RUN_TEST(test_prompt_immersive_trim_shadow_output_unchanged);
    HU_RUN_TEST(test_prompt_immersive_safety_section_live_only_and_early);
    HU_RUN_TEST(test_prompt_immersive_trim_extends_to_world_model);
    HU_RUN_TEST(test_prompt_immersive_tracks_field_stats);
    HU_RUN_TEST(test_continuity_render_both_parts);
    HU_RUN_TEST(test_continuity_render_empty_returns_zero);
    HU_RUN_TEST(test_continuity_render_caps_at_max_bytes);
    HU_RUN_TEST(test_prompt_continuity_section_present_immersive);
    HU_RUN_TEST(test_prompt_continuity_section_absent_when_empty);
    HU_RUN_TEST(test_prompt_continuity_survives_under_pressure);
    HU_RUN_TEST(test_prompt_build_with_stm_context);
    HU_RUN_TEST(test_prompt_build_with_custom_instructions);
    HU_RUN_TEST(test_prompt_build_includes_hula_protocol);
    HU_RUN_TEST(test_prompt_addendum_present_on_factual_query);
    HU_RUN_TEST(test_prompt_addendum_absent_on_nonfactual_query);
    HU_RUN_TEST(test_memory_loader_empty_backend);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_memory_loader_with_entries);
    HU_RUN_TEST(test_memory_loader_embedded_nul_no_overflow);
    HU_RUN_TEST(test_sqlite_recall_content_len_matches_buffer);
#endif
}
