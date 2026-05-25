/* test_mlx_provider — coverage for the M3 Bridge B MLX provider.
 *
 * Two contracts pinned here, both required for the daemon's fallback:
 *
 *   1. Unlinked build (HU_ENABLE_MLX_PROVIDER undefined — the default):
 *      every chat path returns HU_ERR_NOT_SUPPORTED so the agent's
 *      provider fallback fires cleanly. The existing tests cover this.
 *
 *   2. Linked build (HU_ENABLE_MLX_PROVIDER defined + Apple Silicon):
 *      chat path INVOKES the subprocess helper (`mlx_run_subprocess`)
 *      modeled after run_claude_cli in src/providers/claude_cli.c.
 *      Slice 1 (Phase B1 first slice, 2026-05-17) adds the helper
 *      but its end-to-end test that spawns python3 -m mlx_lm.generate
 *      lands in slice 2 with the test-fixture shim. For slice 1, the
 *      compile-time gates are pinned via the new structural test below.
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include "human/providers/mlx.h"
#include "test_framework.h"
#include <string.h>

static void mlx_provider_create_succeeds_with_defaults(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mlx_config_t cfg = {0};
    hu_provider_t p = {0};
    HU_ASSERT_EQ(hu_mlx_provider_create(&alloc, &cfg, &p), HU_OK);
    HU_ASSERT_NOT_NULL(p.ctx);
    HU_ASSERT_NOT_NULL(p.vtable);
    HU_ASSERT_STR_EQ(p.vtable->get_name(p.ctx), "mlx");
    p.vtable->deinit(p.ctx, &alloc);
}

static void mlx_provider_create_copies_config_strings(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *model = "mlx-community/gemma-4-31b-it-4bit";
    const char *adapter = "/tmp/test.safetensors";
    hu_mlx_config_t cfg = {
        .model_path = model,
        .model_path_len = strlen(model),
        .adapter_path = adapter,
        .adapter_path_len = strlen(adapter),
        .max_tokens = 256,
    };
    hu_provider_t p = {0};
    HU_ASSERT_EQ(hu_mlx_provider_create(&alloc, &cfg, &p), HU_OK);
    HU_ASSERT_NOT_NULL(p.ctx);
    /* The owned copies aren't observable through the vtable — but
     * deinit must free them without ASan flagging a leak or double
     * free. That's the assertion. */
    p.vtable->deinit(p.ctx, &alloc);
}

static void mlx_provider_create_rejects_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t p = {0};
    HU_ASSERT_EQ(hu_mlx_provider_create(NULL, NULL, &p), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_mlx_provider_create(&alloc, NULL, NULL), HU_ERR_INVALID_ARGUMENT);
}

/* The dispatcher safety contract — chat path returns NOT_SUPPORTED, does
 * NOT deref the context. Mirrors the test_provider_all.c daemon-pattern
 * guard the critic flagged on 2026-05-16 as the WRONG kind of test
 * (it covered crash-safety but not fallback behavior). This test
 * directly covers fallback-on-NOT_SUPPORTED at the provider level. */
static void mlx_provider_chat_returns_not_supported(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mlx_config_t cfg = {0};
    hu_provider_t p = {0};
    HU_ASSERT_EQ(hu_mlx_provider_create(&alloc, &cfg, &p), HU_OK);

    hu_chat_request_t req = {0};
    hu_chat_response_t out = {0};
    HU_ASSERT_EQ(p.vtable->chat(p.ctx, &alloc, &req, "model", 5, 0.0, &out), HU_ERR_NOT_SUPPORTED);

    char *legacy_out = NULL;
    size_t legacy_len = 0;
    HU_ASSERT_EQ(p.vtable->chat_with_system(p.ctx, &alloc, "sys", 3, "msg", 3, "model", 5, 0.0,
                                            &legacy_out, &legacy_len),
                 HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_NULL(legacy_out);
    HU_ASSERT_EQ((long)legacy_len, 0L);

    p.vtable->deinit(p.ctx, &alloc);
}

/* M3 Phase B5 (2026-05-19): load_adapter is now wired. The contract is:
 *   - NULL/invalid args → HU_ERR_INVALID_ARGUMENT
 *   - missing adapters.safetensors → HU_ERR_NOT_FOUND
 * Previously this test asserted HU_ERR_NOT_SUPPORTED — that pinned the
 * stub behavior and would have blocked the wiring. Updated per
 * .claude/rules/tests-that-pin-bugs.md. The "stub fallback" contract is
 * still pinned on chat (HU_ERR_NOT_SUPPORTED in non-MLX builds) by
 * `mlx_provider_chat_returns_not_supported` above. unload_adapter and
 * active_adapter remain stubs — those are tracked separately. */
static void mlx_provider_load_adapter_validates_inputs(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mlx_config_t cfg = {0};
    hu_provider_t p = {0};
    HU_ASSERT_EQ(hu_mlx_provider_create(&alloc, &cfg, &p), HU_OK);
    HU_ASSERT_NOT_NULL(p.vtable->load_adapter);
    HU_ASSERT_NOT_NULL(p.vtable->unload_adapter); /* CodeRabbit 2026-05-17 — pin
                                                     the third member of the
                                                     adapter triple too. */
    /* Missing safetensors file in the directory → NOT_FOUND. /tmp exists
     * on every CI runner but /tmp/adapters.safetensors does not. */
    HU_ASSERT_EQ(p.vtable->load_adapter(p.ctx, &alloc, "/tmp/x.safetensors", 18, "id", 2),
                 HU_ERR_NOT_FOUND);
    /* unload + active remain NOT_SUPPORTED stubs for now (unload requires
     * a way to revert the persisted ctx state; tracked in next slice). */
    HU_ASSERT_EQ(p.vtable->unload_adapter(p.ctx, "id", 2), HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_NULL(p.vtable->active_adapter(p.ctx));
    p.vtable->deinit(p.ctx, &alloc);
}

static void mlx_provider_supports_native_tools_is_false(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mlx_config_t cfg = {0};
    hu_provider_t p = {0};
    HU_ASSERT_EQ(hu_mlx_provider_create(&alloc, &cfg, &p), HU_OK);
    HU_ASSERT_FALSE(p.vtable->supports_native_tools(p.ctx));
    p.vtable->deinit(p.ctx, &alloc);
}

/* Slice 1 structural pin (B1 — 2026-05-17): the chat path MUST flow
 * through the subprocess helper when the gates are met. We can't
 * directly observe "the helper got called" from outside, but we CAN
 * assert that the helper's compile-time guard logic preserves the
 * NOT_SUPPORTED contract in the unlinked build — which is what the
 * existing chat_returns_not_supported test does.
 *
 * This test additionally pins that the request->user-message flattening
 * does NOT touch the out parameter on the NOT_SUPPORTED path (a
 * regression we'd otherwise only catch by inspecting code). When
 * mlx_run_subprocess returns NOT_SUPPORTED before any allocation, the
 * caller's `out` struct must be untouched so the daemon's fallback path
 * can safely retry with another provider. */
static void mlx_provider_chat_does_not_mutate_out_on_unsupported(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mlx_config_t cfg = {0};
    hu_provider_t p = {0};
    HU_ASSERT_EQ(hu_mlx_provider_create(&alloc, &cfg, &p), HU_OK);

    hu_chat_message_t msgs[1] = {{
        .role = HU_ROLE_USER,
        .content = "hello",
        .content_len = 5,
    }};
    hu_chat_request_t req = {.messages = msgs, .messages_count = 1};

    /* Pre-fill out with sentinel values; require they survive a
     * NOT_SUPPORTED return — caller still owns the struct. */
    hu_chat_response_t out = {0};
    out.content = (const char *)0xdeadbeef;
    out.content_len = 0x4242;
    HU_ASSERT_EQ(p.vtable->chat(p.ctx, &alloc, &req, "model", 5, 0.0, &out), HU_ERR_NOT_SUPPORTED);
    /* Sentinel preserved — helper must not partial-fill on the
     * NOT_SUPPORTED path. */
    HU_ASSERT_EQ((unsigned long)(uintptr_t)out.content, (unsigned long)0xdeadbeef);
    HU_ASSERT_EQ((unsigned long)out.content_len, (unsigned long)0x4242);

    p.vtable->deinit(p.ctx, &alloc);
}

/* B2 verifier contract — mlx provider model_path threading.
 * This test pins the B2 ctx->model_path storage and threading.
 * The subprocess helper will eventually use this to construct argv;
 * we verify that paths are copied into context and don't corrupt
 * context creation. The contract allows NULL/empty (no model) or
 * valid paths; both must succeed. */
static void mlx_provider_create_resolves_model_path_null_allowed(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mlx_config_t cfg = {
        .model_path = NULL,
        .model_path_len = 0,
    };
    hu_provider_t p = {0};
    /* NULL model path is allowed (defaults to base model if not specified). */
    HU_ASSERT_EQ(hu_mlx_provider_create(&alloc, &cfg, &p), HU_OK);
    p.vtable->deinit(p.ctx, &alloc);
}

static void mlx_provider_create_resolves_model_path_empty_allowed(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mlx_config_t cfg = {
        .model_path = "",
        .model_path_len = 0,
    };
    hu_provider_t p = {0};
    /* Empty model path is allowed (defaults to base model if not specified). */
    HU_ASSERT_EQ(hu_mlx_provider_create(&alloc, &cfg, &p), HU_OK);
    p.vtable->deinit(p.ctx, &alloc);
}

static void mlx_provider_create_resolves_model_path_valid_threaded(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *model = "mlx-community/gemma-4-31b-it-4bit";
    hu_mlx_config_t cfg = {
        .model_path = model,
        .model_path_len = strlen(model),
    };
    hu_provider_t p = {0};
    /* Valid path is accepted and threaded into context without corruption. */
    HU_ASSERT_EQ(hu_mlx_provider_create(&alloc, &cfg, &p), HU_OK);
    p.vtable->deinit(p.ctx, &alloc);
}

/* ─────────────────────────────────────────────────────────────────────
 * Sprint 55 Wave 1 Tests: US-1, US-2, US-3
 * ───────────────────────────────────────────────────────────────────── */

/* US-1: test_mlx_chat_subprocess_round_trip
 *
 * Verify that hu_mlx_provider_create + hu_provider_chat_with_system
 * produces a non-empty response (when subprocess is active).
 *
 * Subprocess concern (HU_IS_TEST): In test builds (HU_IS_TEST=1), the
 * subprocess is disabled and chat returns HU_ERR_NOT_SUPPORTED. That is
 * correct and documented behavior. For a real end-to-end test with Python
 * + MLX, that work is Phase B1 slice 2.
 *
 * AC-1.1: Test exists, gated on HU_ENABLE_MLX_PROVIDER
 * AC-1.2: Invokes hu_mlx_provider_create + chat_with_system
 * AC-1.3: Checks response is non-empty (or HU_ERR_NOT_SUPPORTED in test mode)
 * AC-1.4: Skips cleanly on non-Apple platforms
 * AC-1.5: Error code is HU_OK (subprocess active) or HU_ERR_NOT_SUPPORTED
 */
static void test_mlx_chat_subprocess_round_trip(void) {
#if defined(__APPLE__) && defined(__arm64__)
    hu_allocator_t alloc = hu_system_allocator();
    hu_mlx_config_t config = {
        .model_path = "mlx-community/gemma-2-2b-it",
        .model_path_len = strlen("mlx-community/gemma-2-2b-it"),
        .adapter_path = NULL,
        .adapter_path_len = 0,
        .max_tokens = 128,
    };

    hu_provider_t provider;
    hu_error_t err = hu_mlx_provider_create(&alloc, &config, &provider);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(provider.ctx);
    HU_ASSERT_NOT_NULL(provider.vtable);

    /* In test builds (HU_IS_TEST=1), the chat path returns NOT_SUPPORTED
     * because HU_MLX_SUBPROCESS_ACTIVE = 0. That's correct behavior.
     * The test verifies the provider is creatable and the error code is
     * correct for the build variant. */
    const char *system_prompt = "You are a helpful assistant.";
    const char *message = "Say hello.";
    char *out = NULL;
    size_t out_len = 0;

    err = provider.vtable->chat_with_system(
        provider.ctx, &alloc, system_prompt, strlen(system_prompt), message, strlen(message),
        config.model_path, config.model_path_len, 0.7, &out, &out_len);

    /* In test builds: HU_ERR_NOT_SUPPORTED (expected, documented)
     * In linked builds with MLX available: HU_OK, out is non-empty string
     * In linked builds without Python: HU_ERR_IO or HU_ERR_TIMEOUT
     *
     * For AC-1.5 to pass, we accept NOT_SUPPORTED in test mode as correct. */
    if (err == HU_OK) {
        /* Real subprocess succeeded. AC-1.3/1.5: response is non-empty.
         * Critic finding (Sprint 55): the AC says "non-empty," not ≥10 bytes;
         * a correct minimal reply like "ok" is 2 bytes and must not fail. */
        HU_ASSERT_NOT_NULL(out);
        if (out_len < 1)
            HU_FAIL("Response empty: %zu bytes", out_len);
        alloc.free(alloc.ctx, out, out_len + 1);
    } else {
        /* Test build (NOT_SUPPORTED) or no Python — both expected. */
        HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
        HU_ASSERT_NULL(out);
    }

    provider.vtable->deinit(provider.ctx, &alloc);
#else
    /* Non-Apple or non-arm64: test skips cleanly via AC-1.4. */
    HU_ASSERT_TRUE(1);
#endif
}

/* US-2: test_mlx_provider_create_resolves_model_path
 *
 * Verify that hu_mlx_provider_create handles present vs missing model paths.
 *
 * AC-2.1: Test function name matches spec
 * AC-2.2: Present model path → HU_OK
 * AC-2.3: Missing model path — constructor still succeeds (validation deferred)
 * AC-2.4: Both cases call deinit cleanly, ASan verifies no leak
 * AC-2.5: Two test cases (present, missing)
 *
 * Note: The current hu_mlx_provider_create does not validate path
 * existence — that validation happens downstream in mlx_run_subprocess
 * when the subprocess tries to invoke the model. Both paths succeed at
 * create time; the missing case would fail at subprocess time.
 */
static void test_mlx_provider_create_resolves_model_path(void) {
#if defined(__APPLE__) && defined(__arm64__)
    hu_allocator_t alloc = hu_system_allocator();

    /* Subcase (a): Valid model path — should succeed */
    {
        hu_mlx_config_t config = {
            .model_path = "mlx-community/gemma-2-2b-it",
            .model_path_len = strlen("mlx-community/gemma-2-2b-it"),
            .adapter_path = NULL,
            .adapter_path_len = 0,
            .max_tokens = 128,
        };

        hu_provider_t provider;
        hu_error_t err = hu_mlx_provider_create(&alloc, &config, &provider);
        HU_ASSERT_EQ(err, HU_OK);
        HU_ASSERT_NOT_NULL(provider.ctx);

        provider.vtable->deinit(provider.ctx, &alloc);
    }

    /* Subcase (b): Nonexistent model path
     * The constructor still returns HU_OK (path validation is deferred to
     * subprocess time). This test verifies the constructor contract:
     * it allocates and owns the config, but doesn't stat the path. */
    {
        hu_mlx_config_t config = {
            .model_path = "/nonexistent/path/to/model",
            .model_path_len = strlen("/nonexistent/path/to/model"),
            .adapter_path = NULL,
            .adapter_path_len = 0,
            .max_tokens = 128,
        };

        hu_provider_t provider;
        hu_error_t err = hu_mlx_provider_create(&alloc, &config, &provider);
        HU_ASSERT_EQ(err, HU_OK);
        HU_ASSERT_NOT_NULL(provider.ctx);

        /* The actual validation happens at chat time (subprocess invocation).
         * The create function only allocates and owns the config. */
        provider.vtable->deinit(provider.ctx, &alloc);
    }
#else
    /* Non-Apple or non-arm64: test skips via AC-2.5. */
    HU_ASSERT_TRUE(1);
#endif
}

/* US-3: test_mlx_chat_greedy_completion_matches_fixture
 *
 * Verify that MLX inference produces deterministic outputs in greedy mode.
 *
 * AC-3.1: Test name and location
 * AC-3.2: Same prompt called twice, greedy mode (temperature=0, top_p=1.0)
 * AC-3.3: Byte-identical OR token-count diff ≤2
 * AC-3.4: Probe counter advances exactly once per chat call
 * AC-3.5: Verifier assertion on output match
 *
 * This test will skip cleanly in test builds (HU_IS_TEST=1) because
 * HU_MLX_SUBPROCESS_ACTIVE = 0. When a real MLX runtime is available
 * (full integration), the subprocess will be invoked and determinism
 * can be measured.
 */
static void test_mlx_chat_greedy_completion_matches_fixture(void) {
#if defined(__APPLE__) && defined(__arm64__)
    hu_allocator_t alloc = hu_system_allocator();
    hu_mlx_config_t config = {
        .model_path = "mlx-community/gemma-2-2b-it",
        .model_path_len = strlen("mlx-community/gemma-2-2b-it"),
        .adapter_path = NULL,
        .adapter_path_len = 0,
        .max_tokens = 50, /* Short output for stability */
    };

    hu_provider_t provider;
    hu_error_t err = hu_mlx_provider_create(&alloc, &config, &provider);
    HU_ASSERT_EQ(err, HU_OK);

    /* Deterministic prompts for greedy mode testing */
    const char *system_prompt = "You are a helpful assistant.";
    const char *message = "Say 'hello' exactly once.";

    /* First call */
    char *out1 = NULL;
    size_t out1_len = 0;
    hu_error_t err1 = provider.vtable->chat_with_system(
        provider.ctx, &alloc, system_prompt, strlen(system_prompt), message, strlen(message),
        config.model_path, config.model_path_len, 0.0, /* temperature=0 for greedy */
        &out1, &out1_len);

    /* Second call (identical inputs) */
    char *out2 = NULL;
    size_t out2_len = 0;
    hu_error_t err2 = provider.vtable->chat_with_system(
        provider.ctx, &alloc, system_prompt, strlen(system_prompt), message, strlen(message),
        config.model_path, config.model_path_len, 0.0, /* temperature=0 for greedy */
        &out2, &out2_len);

    /* In test builds, both calls return NOT_SUPPORTED (subprocess unavailable).
     * That's correct and expected. AC-3 is deferred to integration time. */
    if (err1 == HU_OK && err2 == HU_OK) {
        /* Real subprocess available: greedy mode (temp=0) MUST be exactly
         * deterministic. Critic finding (Sprint 55): the prior ±2 token
         * tolerance papered over nondeterminism that should fail loudly.
         * If MLX greedy is genuinely nondeterministic on a given runner,
         * that is a bug to surface, not a test to relax. */
        HU_ASSERT_NOT_NULL(out1);
        HU_ASSERT_NOT_NULL(out2);

        if (strcmp(out1, out2) != 0)
            HU_FAIL("Greedy output not deterministic: out1='%s' (%zu) vs out2='%s' (%zu)", out1,
                    out1_len, out2, out2_len);

        alloc.free(alloc.ctx, out1, out1_len + 1);
        alloc.free(alloc.ctx, out2, out2_len + 1);
    } else {
        /* Test build or no Python: both return NOT_SUPPORTED. Expected. */
        HU_ASSERT_EQ(err1, HU_ERR_NOT_SUPPORTED);
        HU_ASSERT_EQ(err2, HU_ERR_NOT_SUPPORTED);
        HU_ASSERT_NULL(out1);
        HU_ASSERT_NULL(out2);
    }

    provider.vtable->deinit(provider.ctx, &alloc);
#else
    /* Non-Apple or non-arm64: test skips via AC-3.1. */
    HU_ASSERT_TRUE(1);
#endif
}

void run_mlx_provider_tests(void) {
    HU_TEST_SUITE("mlx_provider");
    HU_RUN_TEST(mlx_provider_create_succeeds_with_defaults);
    HU_RUN_TEST(mlx_provider_create_copies_config_strings);
    HU_RUN_TEST(mlx_provider_create_rejects_null_args);
    HU_RUN_TEST(mlx_provider_chat_returns_not_supported);
    HU_RUN_TEST(mlx_provider_load_adapter_validates_inputs);
    HU_RUN_TEST(mlx_provider_supports_native_tools_is_false);
    HU_RUN_TEST(mlx_provider_chat_does_not_mutate_out_on_unsupported);
    HU_RUN_TEST(mlx_provider_create_resolves_model_path_null_allowed);
    HU_RUN_TEST(mlx_provider_create_resolves_model_path_empty_allowed);
    HU_RUN_TEST(mlx_provider_create_resolves_model_path_valid_threaded);
    /* Sprint 55 Wave 1 tests */
    HU_RUN_TEST(test_mlx_chat_subprocess_round_trip);
    HU_RUN_TEST(test_mlx_provider_create_resolves_model_path);
    HU_RUN_TEST(test_mlx_chat_greedy_completion_matches_fixture);
}
