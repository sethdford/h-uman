#include "human/agent.h"
#include "human/bootstrap.h"
#include "human/channels/pwa.h"
#include "human/config_parse.h"
#include "human/context_engine.h"
#include "human/core/allocator.h"
#include "human/core/arena.h"
#include "human/core/error.h"
#include "human/memory.h"
#include "human/memory/vector.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void bootstrap_null_ctx_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(hu_app_bootstrap(NULL, &alloc, NULL, false, false), HU_ERR_INVALID_ARGUMENT);
}

static void bootstrap_null_alloc_returns_error(void) {
    hu_app_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    HU_ASSERT_EQ(hu_app_bootstrap(&ctx, NULL, NULL, false, false), HU_ERR_INVALID_ARGUMENT);
}

static void teardown_null_is_safe(void) {
    hu_app_teardown(NULL);
}

static void teardown_zero_ctx_is_safe(void) {
    hu_app_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    hu_app_teardown(&ctx);
}

static void bootstrap_minimal_no_agent_no_channels(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_app_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    hu_error_t err = hu_app_bootstrap(&ctx, &alloc, NULL, false, false);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(ctx.alloc);
    HU_ASSERT_NOT_NULL(ctx.cfg);
    HU_ASSERT_NOT_NULL(ctx.tools);
    HU_ASSERT_TRUE(ctx.tools_count > 0);
    HU_ASSERT_TRUE(ctx.channel_count == 0);
    HU_ASSERT_FALSE(ctx.agent_ok);
    hu_app_teardown(&ctx);
}

static void bootstrap_with_agent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_app_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    hu_error_t err = hu_app_bootstrap(&ctx, &alloc, NULL, true, false);
    if (err == HU_OK) {
        HU_ASSERT_NOT_NULL(ctx.provider);
        HU_ASSERT_NOT_NULL(ctx.memory);
        HU_ASSERT_TRUE(ctx.provider_ok);
        hu_app_teardown(&ctx);
    }
}

#if HU_HAS_PWA
/* 2026-09-04 audit: bootstrap registered the PWA poll fn but never called
 * the channel's start(), so hu_pwa_channel_poll returned on every tick and
 * ten configured apps produced nothing. The registered channel must be the
 * started one. */
static void bootstrap_starts_the_pwa_channel_it_registers(void) {
    char dir[] = "/tmp/hu_bootstrap_pwa_XXXXXX";
    HU_ASSERT_NOT_NULL(mkdtemp(dir));
    char cfg_path[256];
    snprintf(cfg_path, sizeof(cfg_path), "%s/config.json", dir);
    FILE *f = fopen(cfg_path, "w");
    HU_ASSERT_NOT_NULL(f);
    fputs("{\"default_provider\":\"ollama\",\"channels\":{\"pwa\":{\"apps\":[\"slack\"]}}}", f);
    fclose(f);

    hu_allocator_t alloc = hu_system_allocator();
    hu_app_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    HU_ASSERT_EQ(hu_app_bootstrap(&ctx, &alloc, cfg_path, false, true), HU_OK);
    const hu_channel_t *pwa = NULL;
    for (size_t i = 0; i < ctx.channel_count; i++)
        if (ctx.channels[i].poll_fn == hu_pwa_channel_poll)
            pwa = ctx.channels[i].channel;
    HU_ASSERT_NOT_NULL(pwa);
    HU_ASSERT_TRUE(hu_pwa_channel_is_running(pwa));
    hu_app_teardown(&ctx);
    unlink(cfg_path);
    rmdir(dir);
}
#endif

#ifdef HU_ENABLE_SQLITE
/* 2026-09-02..04: with HU_SEMANTIC_RECALL on, bootstrap attached the sqlite
 * engine's semantic index to two BLOCK-SCOPED locals and copied them into the
 * app context afterwards. The engine keeps the addresses it is handed and
 * dereferences them on every indexed store, so the first store after
 * bootstrap read a dead stack slot — 21 ASan aborts of the daemon in
 * semantic_index_row, one per restart. The contract: the engine's attached
 * pointers ARE the app-lifetime objects the context exposes, and a store
 * through the engine after bootstrap returns is safe. */
static void bootstrap_semantic_index_points_at_app_lifetime_embedder(void) {
    char dir[] = "/tmp/hu_bootstrap_sem_XXXXXX";
    HU_ASSERT_NOT_NULL(mkdtemp(dir));
    char cfg_path[256];
    snprintf(cfg_path, sizeof(cfg_path), "%s/config.json", dir);
    FILE *f = fopen(cfg_path, "w");
    HU_ASSERT_NOT_NULL(f);
    fputs("{\"default_provider\":\"ollama\",\"memory\":{\"backend\":\"sqlite\"}}", f);
    fclose(f);
    /* Never the real ~/.human/memory.db; the embed URL is never reached
     * because the test transport is a mock (the index insert fails and is
     * logged, exactly as in test_semantic_recall). */
    setenv("HU_MEMORY_SQLITE_PATH", ":memory:", 1);
    setenv("HU_SEMANTIC_RECALL", "shadow", 1);
    setenv("HU_SEMANTIC_EMBED_URL", "http://127.0.0.1:8749", 1);

    hu_allocator_t alloc = hu_system_allocator();
    hu_app_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    hu_error_t err = hu_app_bootstrap(&ctx, &alloc, cfg_path, true, false);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(ctx.memory);
    HU_ASSERT_NOT_NULL(ctx.embedder);
    HU_ASSERT_NOT_NULL(ctx.vector_store);

    hu_embedder_t *eng_emb = NULL;
    hu_vector_store_t *eng_vs = NULL;
    hu_sqlite_memory_get_semantic_index(ctx.memory, &eng_emb, &eng_vs);
    /* Attached at all — otherwise the pointer checks below would be vacuous. */
    HU_ASSERT_NOT_NULL(eng_emb);
    HU_ASSERT_NOT_NULL(eng_vs);
    /* ...and attached to the app-lifetime objects, not to a dead temporary. */
    HU_ASSERT_TRUE(eng_emb == (hu_embedder_t *)ctx.embedder);
    HU_ASSERT_TRUE(eng_vs == (hu_vector_store_t *)ctx.vector_store);
    HU_ASSERT_NOT_NULL(eng_emb->vtable);
    HU_ASSERT_NOT_NULL(eng_emb->ctx);

    /* The store that aborted the daemon: index a row AFTER bootstrap returned.
     * Under ASan the pre-fix code dies here with stack-use-after-scope. */
    HU_ASSERT_EQ(
        ctx.memory->vtable->store(ctx.memory->ctx, "user_a:fact", 11, "likes tea", 9, NULL, "", 0),
        HU_OK);

    hu_app_teardown(&ctx);
    unsetenv("HU_SEMANTIC_EMBED_URL");
    unsetenv("HU_SEMANTIC_RECALL");
    unsetenv("HU_MEMORY_SQLITE_PATH");
    unlink(cfg_path);
    rmdir(dir);
}
#endif

/* 2026-09-20 dead-code audit (task 3): `agent.context_engine: "rag"` parsed
 * fine but bootstrap logged "not implemented" and silently installed the
 * legacy engine instead — `context_engine_rag.c` was complete and unlinked.
 * Pin both directions: "rag" must install the rag vtable, and "legacy"
 * (already covered implicitly by bootstrap_with_agent) must keep installing
 * the legacy one, so a regression that always installs one engine either
 * way is caught. */
static void bootstrap_context_engine_rag_installs_rag_engine(void) {
    char dir[] = "/tmp/hu_bootstrap_ce_rag_XXXXXX";
    HU_ASSERT_NOT_NULL(mkdtemp(dir));
    char cfg_path[256];
    snprintf(cfg_path, sizeof(cfg_path), "%s/config.json", dir);
    FILE *f = fopen(cfg_path, "w");
    HU_ASSERT_NOT_NULL(f);
    fputs("{\"default_provider\":\"ollama\",\"agent\":{\"context_engine\":\"rag\"}}", f);
    fclose(f);

    hu_allocator_t alloc = hu_system_allocator();
    hu_app_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    HU_ASSERT_EQ(hu_app_bootstrap(&ctx, &alloc, cfg_path, true, false), HU_OK);
    HU_ASSERT_NOT_NULL(ctx.agent);
    hu_context_engine_t *ce = (hu_context_engine_t *)ctx.agent->infra.context_engine;
    HU_ASSERT_NOT_NULL(ce);
    HU_ASSERT_NOT_NULL(ce->vtable);
    HU_ASSERT_NOT_NULL(ce->vtable->get_name);
    HU_ASSERT_STR_EQ(ce->vtable->get_name(ce->ctx), "rag");

    hu_app_teardown(&ctx);
    unlink(cfg_path);
    rmdir(dir);
}

static void bootstrap_context_engine_legacy_installs_legacy_engine(void) {
    char dir[] = "/tmp/hu_bootstrap_ce_legacy_XXXXXX";
    HU_ASSERT_NOT_NULL(mkdtemp(dir));
    char cfg_path[256];
    snprintf(cfg_path, sizeof(cfg_path), "%s/config.json", dir);
    FILE *f = fopen(cfg_path, "w");
    HU_ASSERT_NOT_NULL(f);
    fputs("{\"default_provider\":\"ollama\",\"agent\":{\"context_engine\":\"legacy\"}}", f);
    fclose(f);

    hu_allocator_t alloc = hu_system_allocator();
    hu_app_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    HU_ASSERT_EQ(hu_app_bootstrap(&ctx, &alloc, cfg_path, true, false), HU_OK);
    HU_ASSERT_NOT_NULL(ctx.agent);
    hu_context_engine_t *ce = (hu_context_engine_t *)ctx.agent->infra.context_engine;
    HU_ASSERT_NOT_NULL(ce);
    HU_ASSERT_NOT_NULL(ce->vtable);
    HU_ASSERT_NOT_NULL(ce->vtable->get_name);
    HU_ASSERT_STR_EQ(ce->vtable->get_name(ce->ctx), "legacy");

    hu_app_teardown(&ctx);
    unlink(cfg_path);
    rmdir(dir);
}

/* ── Configured-but-not-compiled channels (2026-09-21) ───────────────────────
 * .claude/rules/silent-config-gated-subsystems.md: a config that names a
 * channel the binary was not built with used to be dropped in silence. The
 * whole `#if HU_HAS_X` block vanishes at compile time, so bootstrap never
 * reaches it and the operator gets no line explaining why the channel is dead.
 *
 * These tests do NOT hardcode a channel. The human_tests target compiles a
 * different channel set than the dev and prod presets, so a hardcoded key
 * would silently flip the test between its positive and negative branch
 * without ever failing. Ask the build which channel it lacks instead. */
static const char *const hu_test_channel_keys[] = {
    "email",   "imap",     "imessage",   "gmail",  "pwa",         "telegram", "discord",
    "slack",   "signal",   "whatsapp",   "line",   "google_chat", "facebook", "instagram",
    "twitter", "tiktok",   "google_rcs", "mqtt",   "matrix",      "irc",      "nostr",
    "lark",    "dingtalk", "teams",      "twilio", "onebot",      "qq",
};

static int hu_test_count_substr(const char *haystack, const char *needle) {
    int n = 0;
    for (const char *p = haystack; (p = strstr(p, needle)) != NULL; p += strlen(needle))
        n++;
    return n;
}

static char *hu_test_slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    static char buf[16384];
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

/* Parse `json` into a fresh arena-backed config, run the warning, and return
 * how many channels it reported. Captured stderr lands in `log_path`. */
static size_t hu_test_warn_for_config(const char *json, const char *log_path) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    HU_ASSERT_EQ(hu_config_parse_json(&cfg, json, strlen(json)), HU_OK);

    int dup_fd = dup(fileno(stderr));
    FILE *saved = (dup_fd >= 0) ? fdopen(dup_fd, "w") : NULL;
    HU_ASSERT_NOT_NULL(saved);
    HU_ASSERT_NOT_NULL(freopen(log_path, "w", stderr));

    size_t warned = hu_app_warn_channels_missing_from_build(&cfg, NULL);

    fflush(stderr);
    dup2(fileno(saved), fileno(stderr));
    fclose(saved);

    hu_arena_destroy(arena);
    return warned;
}

static void bootstrap_warns_for_configured_channel_missing_from_build(void) {
    const char *absent = NULL;
    const char *present = NULL;
    for (size_t i = 0; i < sizeof(hu_test_channel_keys) / sizeof(hu_test_channel_keys[0]); i++) {
        const char *k = hu_test_channel_keys[i];
        if (!absent && hu_app_channel_missing_from_build(k))
            absent = k;
        if (!present && !hu_app_channel_missing_from_build(k))
            present = k;
    }

    char log_path[256];
    snprintf(log_path, sizeof(log_path), "/tmp/hu_bootstrap_chan_warn_%d.log", (int)getpid());
    char json[256];

    if (absent) {
        snprintf(json, sizeof(json), "{\"channels\":{\"%s\":{\"token\":\"t\"}}}", absent);
        size_t warned = hu_test_warn_for_config(json, log_path);
        /* Exactly one line, for the one channel this binary cannot start. */
        HU_ASSERT_EQ(warned, 1);

        const char *log = hu_test_slurp(log_path);
        HU_ASSERT_NOT_NULL(log);
        HU_ASSERT_EQ(hu_test_count_substr(log, "is configured but this binary was built without"),
                     1);
        /* The line must name the block AND the option that fixes it — per the
         * rule, "X disabled" is not enough to act on. */
        char needle[64];
        snprintf(needle, sizeof(needle), "channels.%s", absent);
        HU_ASSERT_STR_CONTAINS(log, needle);
        HU_ASSERT_STR_CONTAINS(log, "rebuild with -D");
    }

    if (present) {
        /* A channel this binary does have stays silent. */
        snprintf(json, sizeof(json), "{\"channels\":{\"%s\":{\"token\":\"t\"}}}", present);
        HU_ASSERT_EQ(hu_test_warn_for_config(json, log_path), 0);
    }

    /* A config naming no channels reports nothing, and a NULL config is safe. */
    HU_ASSERT_EQ(hu_test_warn_for_config("{}", log_path), 0);
    HU_ASSERT_EQ(hu_app_warn_channels_missing_from_build(NULL, NULL), 0);
    unlink(log_path);
}

/* HU_BUILT_IN has to agree with the preprocessor, or the warning fires for
 * channels that work and stays silent for channels that don't. bootstrap.c is
 * compiled into this same executable, so both see identical defines. The
 * branches below cover a gate defined to 1 and, where the build has one, a
 * gate that is not defined at all — the case the macro exists to handle. */
static void bootstrap_channel_gate_table_matches_build(void) {
#if HU_HAS_IMESSAGE
    HU_ASSERT_FALSE(hu_app_channel_missing_from_build("imessage"));
#else
    HU_ASSERT_TRUE(hu_app_channel_missing_from_build("imessage"));
#endif
#if HU_HAS_SLACK
    HU_ASSERT_FALSE(hu_app_channel_missing_from_build("slack"));
#else
    HU_ASSERT_TRUE(hu_app_channel_missing_from_build("slack"));
#endif
#if HU_HAS_SIGNAL
    HU_ASSERT_FALSE(hu_app_channel_missing_from_build("signal"));
#else
    HU_ASSERT_TRUE(hu_app_channel_missing_from_build("signal"));
#endif
#if HU_HAS_QQ
    HU_ASSERT_FALSE(hu_app_channel_missing_from_build("qq"));
#else
    HU_ASSERT_TRUE(hu_app_channel_missing_from_build("qq"));
#endif
    /* Keys that are not channels stay with the config validator. */
    HU_ASSERT_FALSE(hu_app_channel_missing_from_build("not_a_channel"));
    HU_ASSERT_FALSE(hu_app_channel_missing_from_build(NULL));
}

void run_bootstrap_tests(void) {
    HU_TEST_SUITE("Bootstrap");

    HU_RUN_TEST(bootstrap_null_ctx_returns_error);
    HU_RUN_TEST(bootstrap_null_alloc_returns_error);
    HU_RUN_TEST(teardown_null_is_safe);
    HU_RUN_TEST(teardown_zero_ctx_is_safe);
    HU_RUN_TEST(bootstrap_minimal_no_agent_no_channels);
    HU_RUN_TEST(bootstrap_with_agent);
    HU_RUN_TEST(bootstrap_context_engine_rag_installs_rag_engine);
    HU_RUN_TEST(bootstrap_context_engine_legacy_installs_legacy_engine);
    HU_RUN_TEST(bootstrap_warns_for_configured_channel_missing_from_build);
    HU_RUN_TEST(bootstrap_channel_gate_table_matches_build);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(bootstrap_semantic_index_points_at_app_lifetime_embedder);
#if HU_HAS_PWA
    HU_RUN_TEST(bootstrap_starts_the_pwa_channel_it_registers);
#endif
#endif
}
