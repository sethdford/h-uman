/* test_config_banner_runtime_failfast.c
 *
 * Covers two operator-visibility additions landed alongside this file:
 *
 *  1. src/config_validate.c — when hu_config_validate_strict() finds unknown
 *     top-level keys, it now emits ONE consolidated warn-level banner naming
 *     every unknown key, in addition to the existing per-key info lines.
 *     Operators routinely miss per-key info lines in multi-thousand-line
 *     service logs; the banner is the discoverable signal.
 *
 *  2. src/runtime/factory.c — when hu_runtime_from_config() constructs a
 *     stub-tier runtime (gce / wasm / cloudflare — most vtable methods
 *     return HU_ERR_NOT_SUPPORTED), the factory now emits one operator
 *     warning per process per runtime kind, naming the supported runtimes.
 *     Control flow is unchanged: the runtime object is still returned with
 *     HU_OK so existing callers behave the same; only the log changes.
 *
 * Test strategy: hu_log_warn() with a NULL observer falls back to stderr
 * (see include/human/core/log.h). We freopen stderr to a tmpfile, run the
 * code path, restore stderr, and grep the captured output for the expected
 * substrings. This matches the freopen-based capture used by tests/test_cli.c.
 *
 * Note on the one-shot flags: the runtime warning is guarded by static bool
 * flags in factory.c, one per kind. They fire once per *process*, not per
 * call, so each kind can be asserted exactly once in this binary's lifetime.
 * The three kinds are tested in three separate tests; ordering inside the
 * suite is incidental because each kind has its own flag.
 */

#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/arena.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/runtime.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>

/* Capture stderr into a heap-allocated buffer for the duration of `body`.
 * Caller frees the returned string. Returns NULL on capture failure
 * (treated as test failure by HU_ASSERT_NOT_NULL at the call site). */
typedef struct {
    int save_fd;
    char tmpl[64];
    FILE *redirected;
} stderr_capture_t;

static int stderr_capture_begin(stderr_capture_t *cap) {
    snprintf(cap->tmpl, sizeof(cap->tmpl), "/tmp/hu_cfgbanner_XXXXXX");
    int tfd = mkstemp(cap->tmpl);
    if (tfd < 0)
        return -1;
    close(tfd);
    cap->save_fd = dup(STDERR_FILENO);
    if (cap->save_fd < 0) {
        unlink(cap->tmpl);
        return -1;
    }
    cap->redirected = freopen(cap->tmpl, "w", stderr);
    if (!cap->redirected) {
        dup2(cap->save_fd, STDERR_FILENO);
        close(cap->save_fd);
        unlink(cap->tmpl);
        return -1;
    }
    return 0;
}

static char *stderr_capture_end(stderr_capture_t *cap) {
    fflush(stderr);
    dup2(cap->save_fd, STDERR_FILENO);
    close(cap->save_fd);
    FILE *rf = fopen(cap->tmpl, "r");
    if (!rf) {
        unlink(cap->tmpl);
        return NULL;
    }
    /* Up to 16 KB is plenty for our test outputs. */
    char *buf = (char *)calloc(1, 16384);
    if (!buf) {
        fclose(rf);
        unlink(cap->tmpl);
        return NULL;
    }
    size_t n = fread(buf, 1, 16384 - 1, rf);
    buf[n] = '\0';
    fclose(rf);
    unlink(cap->tmpl);
    return buf;
}

/* ─────────────────────────────────────────────────────────────────────
 * Piece 1 — config-parser unknown-key startup banner
 * ─────────────────────────────────────────────────────────────────── */

static void test_config_banner_three_unknown_keys_emits_consolidated_banner(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);

    /* Build a config root with exactly three unknown top-level keys plus the
     * required-for-validation keys. We use the allocator-backed JSON
     * constructors so cleanup happens with the arena. */
    hu_json_value_t *root = hu_json_object_new(&cfg.allocator);
    HU_ASSERT_NOT_NULL(root);
    hu_json_object_set(&cfg.allocator, root, "default_provider",
                       hu_json_string_new(&cfg.allocator, "openai", 6));
    hu_json_object_set(&cfg.allocator, root, "default_model",
                       hu_json_string_new(&cfg.allocator, "gpt-4o", 6));
    /* Three unknown top-level keys: */
    hu_json_object_set(&cfg.allocator, root, "totally_made_up_key_alpha",
                       hu_json_number_new(&cfg.allocator, 1));
    hu_json_object_set(&cfg.allocator, root, "totally_made_up_key_beta",
                       hu_json_number_new(&cfg.allocator, 2));
    hu_json_object_set(&cfg.allocator, root, "totally_made_up_key_gamma",
                       hu_json_number_new(&cfg.allocator, 3));

    /* Set cfg defaults so value validation can proceed (we only care about
     * the unknown-key banner side-effect). */
    cfg.default_provider = "openai";
    cfg.default_model = "gpt-4o";
    cfg.gateway.port = 3000;

    stderr_capture_t cap;
    HU_ASSERT_EQ(stderr_capture_begin(&cap), 0);
    (void)hu_config_validate_strict(&cfg, root, false);
    char *out = stderr_capture_end(&cap);
    HU_ASSERT_NOT_NULL(out);

    /* All three keys must appear in the captured stderr (either via the
     * per-key info lines or the banner — both go through hu_log_*). */
    HU_ASSERT_TRUE(strstr(out, "totally_made_up_key_alpha") != NULL);
    HU_ASSERT_TRUE(strstr(out, "totally_made_up_key_beta") != NULL);
    HU_ASSERT_TRUE(strstr(out, "totally_made_up_key_gamma") != NULL);

    /* The banner specifically must appear exactly once, must say WARNING,
     * must say "3 unknown config keys ignored", and must list all three
     * keys on that one line. */
    const char *banner = strstr(out, "WARNING: 3 unknown config keys ignored:");
    HU_ASSERT_NOT_NULL(banner);
    /* Only one banner line (no second occurrence). */
    HU_ASSERT_TRUE(strstr(banner + 1, "WARNING: 3 unknown config keys ignored:") == NULL);
    /* All three names appear on the banner line itself (before the newline). */
    const char *eol = strchr(banner, '\n');
    size_t banner_len = eol ? (size_t)(eol - banner) : strlen(banner);
    HU_ASSERT_TRUE(memmem(banner, banner_len, "totally_made_up_key_alpha",
                          strlen("totally_made_up_key_alpha")) != NULL);
    HU_ASSERT_TRUE(memmem(banner, banner_len, "totally_made_up_key_beta",
                          strlen("totally_made_up_key_beta")) != NULL);
    HU_ASSERT_TRUE(memmem(banner, banner_len, "totally_made_up_key_gamma",
                          strlen("totally_made_up_key_gamma")) != NULL);
    /* Banner must include the actionable hint. */
    HU_ASSERT_TRUE(
        memmem(banner, banner_len, "Rebuild from source", strlen("Rebuild from source")) != NULL);

    free(out);
    hu_json_free(&cfg.allocator, root);
    hu_arena_destroy(arena);
}

static void test_config_banner_no_unknown_keys_emits_no_banner(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);

    hu_json_value_t *root = hu_json_object_new(&cfg.allocator);
    HU_ASSERT_NOT_NULL(root);
    hu_json_object_set(&cfg.allocator, root, "default_provider",
                       hu_json_string_new(&cfg.allocator, "openai", 6));
    hu_json_object_set(&cfg.allocator, root, "default_model",
                       hu_json_string_new(&cfg.allocator, "gpt-4o", 6));

    cfg.default_provider = "openai";
    cfg.default_model = "gpt-4o";
    cfg.gateway.port = 3000;

    stderr_capture_t cap;
    HU_ASSERT_EQ(stderr_capture_begin(&cap), 0);
    (void)hu_config_validate_strict(&cfg, root, false);
    char *out = stderr_capture_end(&cap);
    HU_ASSERT_NOT_NULL(out);

    /* No banner when there are no unknown keys. */
    HU_ASSERT_TRUE(strstr(out, "unknown config key") == NULL);

    free(out);
    hu_json_free(&cfg.allocator, root);
    hu_arena_destroy(arena);
}

/* ─────────────────────────────────────────────────────────────────────
 * Piece 2 — runtime factory fail-fast for stub runtimes
 * ─────────────────────────────────────────────────────────────────── */

static void assert_stub_warning_for_kind(const char *kind) {
    hu_config_t cfg = {0};
    cfg.runtime.kind = kind;
    hu_runtime_t r;
    stderr_capture_t cap;
    HU_ASSERT_EQ(stderr_capture_begin(&cap), 0);
    hu_error_t err = hu_runtime_from_config(&cfg, &r);
    char *out = stderr_capture_end(&cap);
    HU_ASSERT_NOT_NULL(out);

    /* Control flow unchanged — the existing test_runtime.c suite already
     * pins HU_OK for these kinds when HU_HAS_RUNTIME_EXOTIC=1. */
    HU_ASSERT_EQ(err, HU_OK);

    /* The kind name must appear in the captured warning. */
    HU_ASSERT_TRUE(strstr(out, kind) != NULL);
    /* The supported-runtimes hint must appear so operators know what to
     * switch to (this is the actionable part of the message). */
    HU_ASSERT_TRUE(strstr(out, "Supported runtimes: native, docker") != NULL);
    /* The "stub/incomplete" tier note must appear so operators know why. */
    HU_ASSERT_TRUE(strstr(out, "stub") != NULL || strstr(out, "incomplete") != NULL);

    free(out);
}

static void test_runtime_factory_gce_emits_one_shot_stub_warning(void) {
    assert_stub_warning_for_kind("gce");
}

static void test_runtime_factory_wasm_emits_one_shot_stub_warning(void) {
    assert_stub_warning_for_kind("wasm");
}

static void test_runtime_factory_cloudflare_emits_one_shot_stub_warning(void) {
    assert_stub_warning_for_kind("cloudflare");
}

static void test_runtime_factory_stub_warning_is_one_shot_per_process(void) {
    /* Call gce a second time — the per-kind static flag was flipped by
     * test_runtime_factory_gce_emits_one_shot_stub_warning above, so this
     * subsequent call must NOT re-emit the warning. */
    hu_config_t cfg = {0};
    cfg.runtime.kind = "gce";
    hu_runtime_t r;
    stderr_capture_t cap;
    HU_ASSERT_EQ(stderr_capture_begin(&cap), 0);
    hu_error_t err = hu_runtime_from_config(&cfg, &r);
    char *out = stderr_capture_end(&cap);
    HU_ASSERT_NOT_NULL(out);

    HU_ASSERT_EQ(err, HU_OK);
    /* No second warning. */
    HU_ASSERT_TRUE(strstr(out, "Supported runtimes: native, docker") == NULL);

    free(out);
}

static void test_runtime_factory_native_emits_no_warning(void) {
    hu_config_t cfg = {0};
    cfg.runtime.kind = "native";
    hu_runtime_t r;
    stderr_capture_t cap;
    HU_ASSERT_EQ(stderr_capture_begin(&cap), 0);
    hu_error_t err = hu_runtime_from_config(&cfg, &r);
    char *out = stderr_capture_end(&cap);
    HU_ASSERT_NOT_NULL(out);

    HU_ASSERT_EQ(err, HU_OK);
    /* Production-path runtime must never trigger the stub-tier warning. */
    HU_ASSERT_TRUE(strstr(out, "Supported runtimes: native, docker") == NULL);
    HU_ASSERT_TRUE(strstr(out, "stub") == NULL);

    free(out);
}

#endif /* __unix__ || __APPLE__ */

void run_config_banner_runtime_failfast_tests(void);
void run_config_banner_runtime_failfast_tests(void) {
    HU_TEST_SUITE("config_banner_runtime_failfast");
#if defined(__unix__) || defined(__APPLE__)
    /* Piece 1: config-parser unknown-key startup banner */
    HU_RUN_TEST(test_config_banner_three_unknown_keys_emits_consolidated_banner);
    HU_RUN_TEST(test_config_banner_no_unknown_keys_emits_no_banner);
    /* Piece 2: runtime factory fail-fast (one-shot flag order matters; the
     * "one_shot_per_process" test must run AFTER the gce test that trips
     * the flag the first time). */
    HU_RUN_TEST(test_runtime_factory_gce_emits_one_shot_stub_warning);
    HU_RUN_TEST(test_runtime_factory_wasm_emits_one_shot_stub_warning);
    HU_RUN_TEST(test_runtime_factory_cloudflare_emits_one_shot_stub_warning);
    HU_RUN_TEST(test_runtime_factory_stub_warning_is_one_shot_per_process);
    HU_RUN_TEST(test_runtime_factory_native_emits_no_warning);
#endif
}
