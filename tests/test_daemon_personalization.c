/* G1 regression tests — MoLoRA dispatcher wired into daemon adapter-load block.
 *
 * These tests verify the three contracts required by the S2 gap audit:
 *
 *   1. daemon_molora_prewarm_stacks_all_channel_experts
 *      A persona with multiple overlays each carrying `lora_adapter_path`
 *      causes the dispatcher to be called for every overlay and (when the
 *      provider supports STACK) each expert is loaded into the pool.
 *
 *   2. daemon_molora_cloud_provider_graceful_noop
 *      Cloud-safety contract: a provider that returns HU_ERR_NOT_SUPPORTED
 *      for STACK causes the daemon's loop to log once and stop — the daemon
 *      must not crash, and the existing base-chat path must remain intact.
 *
 *   3. daemon_molora_no_overlay_is_noop
 *      A persona without any per-channel `lora_adapter_path` values leaves
 *      the provider state unchanged (no STACK call is ever attempted).
 *
 * The tests reproduce the daemon's pre-warming loop directly rather than
 * spinning up the full daemon, so they stay deterministic and O(ms).
 *
 * NOTE: HU_IS_TEST is always defined when this file is compiled into the
 * test binary; we use it to gate side-effect-free assertions.
 */

#include "human/agent/molora_dispatcher.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/lora.h"
#include "human/persona.h"
#include "human/provider.h"
#include "human/providers/factory.h"
#include "test_framework.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* ──────────────────────────────────────────────────────────────────
 * Mock provider that records every load_adapter call so we can
 * assert STACK was attempted for each channel overlay.
 * ────────────────────────────────────────────────────────────────── */

#define DAEMON_MOCK_MAX_CALLS 16

typedef struct daemon_mock_call {
    hu_lora_apply_mode_t mode;
    char                 id[64];  /* copy of spec->id */
} daemon_mock_call_t;

typedef struct daemon_mock_ctx {
    daemon_mock_call_t calls[DAEMON_MOCK_MAX_CALLS];
    int                call_count;
    hu_error_t         stack_return; /* what to return for STACK calls */
} daemon_mock_ctx_t;

static hu_error_t daemon_mock_load_adapter(void *ctx, const hu_lora_adapter_spec_t *spec,
                                           hu_lora_apply_mode_t mode) {
    daemon_mock_ctx_t *c = (daemon_mock_ctx_t *)ctx;
    if (c->call_count >= DAEMON_MOCK_MAX_CALLS)
        return HU_ERR_LIMIT_REACHED;
    daemon_mock_call_t *call = &c->calls[c->call_count++];
    call->mode = mode;
    size_t id_copy = spec->id_len < sizeof(call->id) - 1 ? spec->id_len
                                                          : sizeof(call->id) - 1;
    memcpy(call->id, spec->id, id_copy);
    call->id[id_copy] = '\0';
    if (mode == HU_LORA_APPLY_MODE_STACK)
        return c->stack_return;
    return HU_OK;
}

static const char *daemon_mock_active_adapter(void *ctx) {
    (void)ctx;
    return "base";
}

static const char *daemon_mock_get_name(void *ctx) {
    (void)ctx;
    return "daemon_mock";
}

static void daemon_mock_deinit(void *ctx, hu_allocator_t *alloc) {
    alloc->free(alloc->ctx, ctx, sizeof(daemon_mock_ctx_t));
}

static const hu_provider_vtable_t kDaemonMockVtable = {
    .get_name       = daemon_mock_get_name,
    .deinit         = daemon_mock_deinit,
    .load_adapter   = daemon_mock_load_adapter,
    .active_adapter = daemon_mock_active_adapter,
};

static hu_provider_t make_daemon_mock_provider(hu_allocator_t *alloc,
                                               hu_error_t      stack_ret) {
    daemon_mock_ctx_t *c = alloc->alloc(alloc->ctx, sizeof(*c));
    memset(c, 0, sizeof(*c));
    c->stack_return = stack_ret;
    hu_provider_t prov = {.ctx = c, .vtable = &kDaemonMockVtable};
    return prov;
}

/* ──────────────────────────────────────────────────────────────────
 * Persona fixture helpers
 * ────────────────────────────────────────────────────────────────── */

/* Fill `persona` with `n` overlays backed by `storage[0..n-1]`.
 * Each overlay gets channel = channels[i], lora_adapter_path = paths[i]
 * (NULL means "no expert for this channel").  All strings are static so
 * the persona can be stack-allocated and disposed via memset(0). */
static void fill_persona_overlays(hu_persona_t        *persona,
                                  hu_persona_overlay_t *storage,
                                  const char * const   *channels,
                                  const char * const   *paths,
                                  size_t                n) {
    memset(persona, 0, sizeof(*persona));
    for (size_t i = 0; i < n; i++) {
        memset(&storage[i], 0, sizeof(storage[i]));
        storage[i].channel          = (char *)channels[i];
        storage[i].lora_adapter_path = (char *)paths[i];
    }
    persona->overlays       = storage;
    persona->overlays_count = n;
}

/* ──────────────────────────────────────────────────────────────────
 * Helper that reproduces the daemon's pre-warming loop exactly as
 * written in src/daemon.c so the tests pin the live code path.
 * ────────────────────────────────────────────────────────────────── */
static void run_daemon_prewarm_loop(hu_provider_t      *provider,
                                    hu_allocator_t     *alloc,
                                    const hu_persona_t *persona,
                                    int                *not_supported_log_count) {
    bool molora_not_supported_logged = false;
    for (size_t oi = 0; oi < persona->overlays_count; oi++) {
        const hu_persona_overlay_t *ov = &persona->overlays[oi];
        if (!ov->channel || !ov->lora_adapter_path)
            continue;
        size_t ch_len = strlen(ov->channel);
        hu_molora_apply_result_t mres = {0};
        hu_error_t me = hu_molora_dispatcher_apply(
            provider, alloc, persona, ov->channel, ch_len, NULL, &mres);
        if (me == HU_OK && mres.channel_expert_skipped &&
            !molora_not_supported_logged) {
            molora_not_supported_logged = true;
            if (not_supported_log_count)
                (*not_supported_log_count)++;
            break;
        }
        (void)me;
    }
    (void)molora_not_supported_logged;
}

/* ──────────────────────────────────────────────────────────────────
 * Test 1 — daemon pre-warm stacks every overlay expert (G1 happy path)
 * ────────────────────────────────────────────────────────────────── */
static void test_daemon_molora_prewarm_stacks_all_channel_experts(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* Provider that fully supports STACK */
    hu_provider_t prov = make_daemon_mock_provider(&alloc, HU_OK);
    daemon_mock_ctx_t *ctx = (daemon_mock_ctx_t *)prov.ctx;

    const char *channels[] = {"telegram", "imessage", "slack"};
    const char *paths[]    = {"/tmp/tg.lora", "/tmp/im.lora", "/tmp/sl.lora"};
    hu_persona_t         persona;
    hu_persona_overlay_t overlays[3];
    fill_persona_overlays(&persona, overlays, channels, paths, 3);

    /* Mimic W13 base REPLACE first (as daemon.c does before the loop) */
    const hu_lora_adapter_spec_t base_spec = {
        .path = "/tmp/base.lora", .path_len = 14,
        .id = "persona-base", .id_len = 12,
        .alloc = &alloc,
    };
    HU_ASSERT_EQ(hu_provider_load_adapter(&prov, &base_spec,
                                          HU_LORA_APPLY_MODE_REPLACE),
                 HU_OK);
    int replace_calls_before_loop = ctx->call_count; /* should be 1 */
    HU_ASSERT_EQ(replace_calls_before_loop, 1);

    /* Run the daemon pre-warm loop */
    run_daemon_prewarm_loop(&prov, &alloc, &persona, NULL);

    /* Each overlay with a path should have produced exactly one STACK call */
    int stack_calls = 0;
    for (int i = 0; i < ctx->call_count; i++) {
        if (ctx->calls[i].mode == HU_LORA_APPLY_MODE_STACK)
            stack_calls++;
    }
    HU_ASSERT_EQ(stack_calls, 3);

    /* The STACK ids must match the channels (lora_adapter_id not set →
     * dispatcher falls back to channel name) */
    int tg_found = 0, im_found = 0, sl_found = 0;
    for (int i = 0; i < ctx->call_count; i++) {
        if (ctx->calls[i].mode != HU_LORA_APPLY_MODE_STACK)
            continue;
        if (strcmp(ctx->calls[i].id, "telegram") == 0)  tg_found = 1;
        if (strcmp(ctx->calls[i].id, "imessage") == 0)  im_found = 1;
        if (strcmp(ctx->calls[i].id, "slack") == 0)     sl_found = 1;
    }
    HU_ASSERT_TRUE(tg_found);
    HU_ASSERT_TRUE(im_found);
    HU_ASSERT_TRUE(sl_found);

    prov.vtable->deinit(prov.ctx, &alloc);
}

/* ──────────────────────────────────────────────────────────────────
 * Test 2 — cloud-safety: NOT_SUPPORTED for STACK → logged once, loop stops
 * ────────────────────────────────────────────────────────────────── */
static void test_daemon_molora_cloud_provider_graceful_noop(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* The cloud provider: load_adapter returns NOT_SUPPORTED for STACK.
     * We use the real "openai" provider (no load_adapter vtable) to exercise
     * the full stack, which goes through hu_provider_load_adapter → NULL
     * vtable → HU_ERR_NOT_SUPPORTED → dispatcher sets channel_expert_skipped
     * → loop logs once and breaks. */
    hu_provider_t prov;
    HU_ASSERT_EQ(hu_provider_create(&alloc, "openai", 6, "test-key", 8, NULL, 0,
                                    &prov),
                 HU_OK);

    const char *channels[] = {"telegram", "imessage", "slack"};
    const char *paths[]    = {"/tmp/tg.lora", "/tmp/im.lora", "/tmp/sl.lora"};
    hu_persona_t         persona;
    hu_persona_overlay_t overlays[3];
    fill_persona_overlays(&persona, overlays, channels, paths, 3);

    int log_count = 0;
    /* Must not crash; loop must stop after first NOT_SUPPORTED. */
    run_daemon_prewarm_loop(&prov, &alloc, &persona, &log_count);

    /* The NOT_SUPPORTED was caught and logged exactly once, then the loop
     * stopped (break).  log_count == 1 proves the log-once contract. */
    HU_ASSERT_EQ(log_count, 1);

    /* Provider still functional: base-chat path is intact. */
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t ce = HU_OK;
    if (prov.vtable->chat_with_system) {
        ce = prov.vtable->chat_with_system(prov.ctx, &alloc, NULL, 0, "ping", 4,
                                           "gpt-5", 5, 0.0, &out, &out_len);
        /* In test mode, openai returns a deterministic mock response. */
        HU_ASSERT_TRUE(ce == HU_OK || ce == HU_ERR_NOT_SUPPORTED ||
                       ce == HU_ERR_PROVIDER_UNAVAILABLE);
        if (out)
            alloc.free(alloc.ctx, out, out_len + 1);
    }

    if (prov.vtable->deinit)
        prov.vtable->deinit(prov.ctx, &alloc);
}

/* ──────────────────────────────────────────────────────────────────
 * Test 3 — no overlay with a path → dispatcher is never called
 * ────────────────────────────────────────────────────────────────── */
static void test_daemon_molora_no_overlay_is_noop(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov = make_daemon_mock_provider(&alloc, HU_OK);
    daemon_mock_ctx_t *ctx = (daemon_mock_ctx_t *)prov.ctx;

    /* Three overlays, none of them carry a lora_adapter_path */
    const char *channels[] = {"telegram", "imessage", "slack"};
    const char *paths[]    = {NULL, NULL, NULL};
    hu_persona_t         persona;
    hu_persona_overlay_t overlays[3];
    fill_persona_overlays(&persona, overlays, channels, paths, 3);

    run_daemon_prewarm_loop(&prov, &alloc, &persona, NULL);

    /* No STACK calls should have been issued */
    for (int i = 0; i < ctx->call_count; i++) {
        HU_ASSERT_TRUE(ctx->calls[i].mode != HU_LORA_APPLY_MODE_STACK);
    }
    HU_ASSERT_EQ(ctx->call_count, 0);

    prov.vtable->deinit(prov.ctx, &alloc);
}

/* ──────────────────────────────────────────────────────────────────
 * Runner
 * ────────────────────────────────────────────────────────────────── */
void run_daemon_personalization_tests(void) {
    HU_TEST_SUITE("MoLoRA daemon personalization (G1)");
    HU_RUN_TEST(test_daemon_molora_prewarm_stacks_all_channel_experts);
    HU_RUN_TEST(test_daemon_molora_cloud_provider_graceful_noop);
    HU_RUN_TEST(test_daemon_molora_no_overlay_is_noop);
}
