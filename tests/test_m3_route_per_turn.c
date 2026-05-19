/* tests/test_m3_route_per_turn.c
 *
 * M3 Phase G1 / B5 (2026-05-19) — coverage for
 * `hu_agent_m3_route_per_turn` (src/agent/agent.c:1053). The function
 * was wired into agent_turn.c:4211 but had zero direct test coverage
 * (audit finding 2026-05-19).
 *
 * Contracts pinned (positive-shape per
 * .claude/rules/tests-that-pin-bugs.md):
 *
 *   1. NULL agent → no-op (no crash, no log)
 *   2. agent with no contact_routes attached → no-op
 *   3. agent with empty memory_session_id → no-op (no contact to hash)
 *   4. agent with a contact_routes mapping that matches the session id
 *      → tries the swap. The HTTP call goes to an unreachable port so
 *      we never actually contact a real MLX server; we assert the
 *      function returns cleanly (swap "soft-fail" path — current
 *      adapter is left alone, no crash, no provider state corruption).
 *   5. agent with a contact_routes mapping that does NOT match the
 *      session id → no swap attempted (no adapter change observable).
 *   6. Repeated call with the SAME contact + already-loaded adapter →
 *      no second swap round-trip (cached `m3_active_adapter_path`).
 *
 * Side-effect discipline:
 *   - HU_IS_TEST is set; curl is OFF in the test build path or any
 *     network call goes to 127.0.0.1:9 (refused). Either way the swap
 *     surfaces as HU_ERR_IO / HU_ERR_NOT_SUPPORTED, both of which are
 *     handled by the soft-fail branch (`hu_log_warn` + continue).
 *   - No real subprocess spawn. No real HTTP listener.
 *
 * Production-symbol coverage (per
 * .claude/rules/test-references-production-symbol.md):
 * references hu_agent_m3_route_per_turn directly, plus
 * hu_m3_contact_routes_create/destroy and hu_m3_outcome_hash_bytes —
 * the public surface the function reads through.
 *
 * Gate symmetry (per .claude/rules/test-source-gate-symmetry.md):
 *   route_per_turn lives behind `#ifdef HU_ENABLE_ML` in agent.c. We
 *   use the internal-#ifdef-wrap-with-stub-runner pattern so this
 *   test source can stay in the unconditional HU_TEST_SOURCES list
 *   without breaking the no-ML variant builds.
 */

#include "test_framework.h"

#ifdef HU_ENABLE_ML

#include "human/agent.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/m3_contact_routes.h"
#include "human/ml/m3_frontier_adapter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static hu_allocator_t A(void) {
    return hu_system_allocator();
}

/* Build a routes JSON file with one specific route + optional default.
 * Returns 0 on success. */
static int write_routes_file(const char *path, uint64_t contact_hash, const char *adapter_path,
                             const char *default_adapter) {
    FILE *fp = fopen(path, "w");
    if (!fp)
        return -1;
    fprintf(fp,
            "{\"routes\":{\"%llu\":{\"adapter_path\":\"%s\"}},"
            "\"default_adapter\":%s%s%s}",
            (unsigned long long)contact_hash, adapter_path, default_adapter ? "\"" : "",
            default_adapter ? default_adapter : "null", default_adapter ? "\"" : "");
    fclose(fp);
    return 0;
}

/* Build a near-zero agent that route_per_turn will accept. The
 * function reads memory_session_id, m3_contact_routes,
 * m3_active_adapter_path, alloc, observer; everything else is
 * untouched.
 *
 * The HU_MLX_URL env var is set to point at the discard port so we
 * exercise the FAIL-SOFT branch of route_per_turn without touching a
 * real server. */
static void set_unreachable_mlx_url(void) {
    setenv("HUMAN_MLX_URL", "http://127.0.0.1:9/v1", 1);
}

/* ── 1. NULL agent → no-op ───────────────────────────────────────────── */

static void route_per_turn_null_agent_is_noop(void) {
    /* Must not crash. */
    hu_agent_m3_route_per_turn(NULL);
}

/* ── 2. agent without routes → no-op ─────────────────────────────────── */

static void route_per_turn_no_routes_is_noop(void) {
    hu_allocator_t alloc = A();
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    const char *sid = "alice@example.com";
    agent.memory_session_id = sid;
    agent.memory_session_id_len = strlen(sid);
    /* m3_contact_routes left NULL — function must return before any
     * lookup. */
    hu_agent_m3_route_per_turn(&agent);
    /* No allocation should have happened — active_adapter_path stays NULL. */
    HU_ASSERT_NULL((void *)agent.m3_active_adapter_path);
}

/* ── 3. agent with empty session id → no-op ──────────────────────────── */

static void route_per_turn_empty_session_id_is_noop(void) {
    hu_allocator_t alloc = A();
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    /* memory_session_id NULL; routes are present. */
    const char *path = "/tmp/hu_m3_route_per_turn_empty_test.json";
    (void)unlink(path);
    HU_ASSERT_EQ(write_routes_file(path, 42, "/a/some.bin", NULL), 0);
    HU_ASSERT_EQ(hu_m3_contact_routes_create(&alloc, path, &agent.m3_contact_routes), HU_OK);

    hu_agent_m3_route_per_turn(&agent);
    HU_ASSERT_NULL((void *)agent.m3_active_adapter_path);

    hu_m3_contact_routes_destroy(agent.m3_contact_routes);
    (void)unlink(path);
}

/* ── 4. matching contact → swap attempted (unreachable → soft-fail) ──── */

static void route_per_turn_matching_contact_attempts_swap(void) {
    set_unreachable_mlx_url();
    hu_allocator_t alloc = A();
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    const char *sid = "bob@example.com";
    agent.memory_session_id = sid;
    agent.memory_session_id_len = strlen(sid);

    /* Build a routes file keyed by the contact hash for this session id. */
    uint64_t contact_hash = hu_m3_outcome_hash_bytes(sid, strlen(sid));
    const char *path = "/tmp/hu_m3_route_per_turn_matching_test.json";
    (void)unlink(path);
    HU_ASSERT_EQ(write_routes_file(path, contact_hash, "/a/bob-lora.bin", NULL), 0);
    HU_ASSERT_EQ(hu_m3_contact_routes_create(&alloc, path, &agent.m3_contact_routes), HU_OK);

    /* Call route_per_turn — swap to /a/bob-lora.bin will be attempted.
     * The HTTP target is 127.0.0.1:9 (refused), so the swap soft-fails
     * and the cached path stays NULL. The PROPERTY we pin: the function
     * returned without crashing, the agent state is consistent. */
    hu_agent_m3_route_per_turn(&agent);
    /* Soft-fail leaves m3_active_adapter_path NULL (no successful swap). */
    HU_ASSERT_NULL((void *)agent.m3_active_adapter_path);

    hu_m3_contact_routes_destroy(agent.m3_contact_routes);
    if (agent.m3_active_adapter_path)
        alloc.free(alloc.ctx, agent.m3_active_adapter_path,
                   strlen(agent.m3_active_adapter_path) + 1);
    (void)unlink(path);
}

/* ── 5. unknown contact, no default → no swap attempted ──────────────── */

static void route_per_turn_unknown_contact_no_default_is_noop(void) {
    set_unreachable_mlx_url();
    hu_allocator_t alloc = A();
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    const char *sid = "carol@example.com";
    agent.memory_session_id = sid;
    agent.memory_session_id_len = strlen(sid);

    /* Routes file with a single entry that does NOT match carol's hash. */
    const char *path = "/tmp/hu_m3_route_per_turn_unknown_test.json";
    (void)unlink(path);
    HU_ASSERT_EQ(write_routes_file(path, 99999, "/a/someone-else.bin", NULL), 0);
    HU_ASSERT_EQ(hu_m3_contact_routes_create(&alloc, path, &agent.m3_contact_routes), HU_OK);

    hu_agent_m3_route_per_turn(&agent);
    /* No route → no swap → cached adapter remains NULL. */
    HU_ASSERT_NULL((void *)agent.m3_active_adapter_path);

    hu_m3_contact_routes_destroy(agent.m3_contact_routes);
    (void)unlink(path);
}

/* ── 6. unknown contact WITH default → swap attempted on default ─────── */

static void route_per_turn_unknown_contact_with_default_attempts_swap(void) {
    set_unreachable_mlx_url();
    hu_allocator_t alloc = A();
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    const char *sid = "dave@example.com";
    agent.memory_session_id = sid;
    agent.memory_session_id_len = strlen(sid);

    /* Routes file with no specific match but a default_adapter set. */
    const char *path = "/tmp/hu_m3_route_per_turn_default_test.json";
    (void)unlink(path);
    HU_ASSERT_EQ(write_routes_file(path, 11111, "/a/specific.bin", "/a/default.bin"), 0);
    HU_ASSERT_EQ(hu_m3_contact_routes_create(&alloc, path, &agent.m3_contact_routes), HU_OK);

    hu_agent_m3_route_per_turn(&agent);
    /* Default fired; swap attempted; soft-failed against 127.0.0.1:9
     * → cached path stays NULL. The property we pin is "no crash, no
     * corruption" — the swap soft-fail branch is exercised. */
    HU_ASSERT_NULL((void *)agent.m3_active_adapter_path);

    hu_m3_contact_routes_destroy(agent.m3_contact_routes);
    (void)unlink(path);
}

/* ── 7. repeated call with same already-loaded adapter → no re-swap ──── */

static void route_per_turn_skips_swap_when_already_loaded(void) {
    set_unreachable_mlx_url();
    hu_allocator_t alloc = A();
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    const char *sid = "eve@example.com";
    agent.memory_session_id = sid;
    agent.memory_session_id_len = strlen(sid);

    uint64_t contact_hash = hu_m3_outcome_hash_bytes(sid, strlen(sid));
    const char *path = "/tmp/hu_m3_route_per_turn_cached_test.json";
    (void)unlink(path);
    const char *target = "/a/eve-lora.bin";
    HU_ASSERT_EQ(write_routes_file(path, contact_hash, target, NULL), 0);
    HU_ASSERT_EQ(hu_m3_contact_routes_create(&alloc, path, &agent.m3_contact_routes), HU_OK);

    /* Pre-seed m3_active_adapter_path with the target — simulates "we
     * already swapped to this on a prior turn." The function MUST
     * short-circuit and NOT attempt a second swap. We can't observe
     * "didn't HTTP" directly, but we CAN observe that the cached path
     * is unchanged after the call. */
    size_t tlen = strlen(target);
    agent.m3_active_adapter_path = (char *)alloc.alloc(alloc.ctx, tlen + 1);
    HU_ASSERT_NOT_NULL(agent.m3_active_adapter_path);
    memcpy(agent.m3_active_adapter_path, target, tlen + 1);

    hu_agent_m3_route_per_turn(&agent);
    /* Cached path UNCHANGED — short-circuit fired (no swap attempted). */
    HU_ASSERT_NOT_NULL(agent.m3_active_adapter_path);
    HU_ASSERT_STR_EQ(agent.m3_active_adapter_path, target);

    alloc.free(alloc.ctx, agent.m3_active_adapter_path, tlen + 1);
    hu_m3_contact_routes_destroy(agent.m3_contact_routes);
    (void)unlink(path);
}

void run_m3_route_per_turn_tests(void);
void run_m3_route_per_turn_tests(void) {
    HU_TEST_SUITE("m3_route_per_turn");
    HU_RUN_TEST(route_per_turn_null_agent_is_noop);
    HU_RUN_TEST(route_per_turn_no_routes_is_noop);
    HU_RUN_TEST(route_per_turn_empty_session_id_is_noop);
    HU_RUN_TEST(route_per_turn_matching_contact_attempts_swap);
    HU_RUN_TEST(route_per_turn_unknown_contact_no_default_is_noop);
    HU_RUN_TEST(route_per_turn_unknown_contact_with_default_attempts_swap);
    HU_RUN_TEST(route_per_turn_skips_swap_when_already_loaded);
}

#else /* !HU_ENABLE_ML */

/* Stub runner so the symbol resolves at link time in no-ML variants
 * (test-source-gate-symmetry.md rule). */
void run_m3_route_per_turn_tests(void);
void run_m3_route_per_turn_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_ML */
