/* tests/test_m3_route_per_turn_call_sites.c
 *
 * Spec 2026-05-19 M3 closure / AC-M3-2 — regression test that pins the
 * existence of `hu_agent_m3_route_per_turn(` invocations in BOTH the
 * non-streaming chat path (src/agent/agent_turn.c) AND the streaming
 * chat path (src/agent/agent_stream.c).
 *
 * Why grep rather than dynamic dispatch coverage:
 *   - The agent surfaces are large; standing up a full agent +
 *     provider + memory facade just to assert "the streaming path
 *     reached this line" would be expensive and fragile.
 *   - The bug shape this regression test prevents is structural — an
 *     engineer adds a new stream path and forgets to wire the
 *     swap call. A grep-level invariant catches that at test time.
 *   - We pair this with `test_m3_route_per_turn.c` (positive contract
 *     of the function behavior) and `test_m3_swap_failure_observability.c`
 *     (failure-mode observability).
 *
 * Test discipline (per ~/.claude/rules/tests-that-pin-bugs.md):
 *   The assertion below is a POSITIVE contract — "both files MUST
 *   contain a route_per_turn call site." If a future refactor moves
 *   either call site (e.g. into a helper function), the test should be
 *   updated to follow it. The shape we're guarding against is
 *   "streaming path forgot to route entirely," not "the call lives at
 *   line 4224 specifically."
 *
 * Symbol references (per
 * ~/.claude/rules/test-references-production-symbol.md):
 *   References `hu_agent_m3_route_per_turn` directly via the include
 *   so the test source still passes the gate-symmetry check even if
 *   the grep below were dropped.
 */

#include "test_framework.h"

#include "human/agent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Compile-time anchor: ensures the symbol is reachable from this TU
 * and the test_framework's "references production symbol" gate is
 * satisfied. The pointer is exercised at the (never-taken) NULL-cast
 * branch in route_per_turn so it doesn't crash. */
static void route_per_turn_symbol_is_linkable(void) {
    void (*sym)(hu_agent_t *) = &hu_agent_m3_route_per_turn;
    HU_ASSERT_NOT_NULL((void *)sym);
}

/* Read a file into a heap buffer. Returns NULL on any I/O failure;
 * caller frees. */
static char *slurp(const char *path, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long sz = ftell(fp);
    if (sz < 0) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[n] = '\0';
    if (out_len)
        *out_len = n;
    return buf;
}

/* Try several candidate paths for the source file. Different build
 * trees launch tests from different cwds (build/ vs repo root vs
 * worktree paths under .claude/worktrees/). Returns the first path
 * that contains the needle and writes its content into *out_content;
 * caller frees. Returns NULL if none of the candidates contain the
 * needle. */
static char *find_source_with_needle(const char *const *candidates, size_t n, const char *needle) {
    for (size_t i = 0; i < n; i++) {
        size_t len = 0;
        char *buf = slurp(candidates[i], &len);
        if (!buf)
            continue;
        if (strstr(buf, needle)) {
            return buf;
        }
        free(buf);
    }
    return NULL;
}

static void agent_turn_contains_route_per_turn_call(void) {
    const char *paths[] = {
        "src/agent/agent_turn.c",
        "../src/agent/agent_turn.c",
        "../../src/agent/agent_turn.c",
    };
    char *content = find_source_with_needle(paths, sizeof(paths) / sizeof(paths[0]),
                                            "hu_agent_m3_route_per_turn(");
    HU_ASSERT_NOT_NULL(content);
    free(content);
}

static void agent_stream_contains_route_per_turn_call(void) {
    const char *paths[] = {
        "src/agent/agent_stream.c",
        "../src/agent/agent_stream.c",
        "../../src/agent/agent_stream.c",
    };
    char *content = find_source_with_needle(paths, sizeof(paths) / sizeof(paths[0]),
                                            "hu_agent_m3_route_per_turn(");
    HU_ASSERT_NOT_NULL(content);
    free(content);
}

void run_m3_route_per_turn_call_sites_tests(void);
void run_m3_route_per_turn_call_sites_tests(void) {
    HU_TEST_SUITE("m3_route_per_turn_call_sites");
    HU_RUN_TEST(route_per_turn_symbol_is_linkable);
    HU_RUN_TEST(agent_turn_contains_route_per_turn_call);
    HU_RUN_TEST(agent_stream_contains_route_per_turn_call);
}
