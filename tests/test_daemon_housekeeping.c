/* FIX 3 — Daemon housekeeping wire e2e proof.
 *
 * The daemon's main loop schedules hu_autodream_run and
 * hu_persona_evolver_run in a 3 AM window, sharing the bitemporal graph
 * opened from ~/.human/graph.db. This test exercises that exact wiring
 * pattern without spinning up the daemon: open a graph, seed it with
 * representative state, then call the same housekeeping functions the
 * daemon would call with the same config. The functions are independently
 * unit-tested; this test guards the integration shape (config layout,
 * argument plumbing, report inspection) from drift. */

#include "human/agent/autodream.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/graph.h"
#include "human/persona/persona_deltas.h"
#include "test_framework.h"

#include <string.h>
#include <time.h>

#ifdef HU_ENABLE_SQLITE

static void daemon_housekeeping_runs_autodream_and_evolver_e2e(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    HU_ASSERT_EQ(hu_graph_open(&alloc, NULL, 0, &g), HU_OK);

    int64_t now_ms = (int64_t)time(NULL) * 1000LL;

    /* Seed three corroborating persona deltas so the evolver has something
     * to apply (corroboration_min default is 3). The propose API requires
     * a non-empty contact_id; we use "u1" here and run the evolver with the
     * same scope. */
    for (int i = 0; i < 3; i++) {
        int64_t delta_id = 0;
        HU_ASSERT_EQ(
            hu_persona_delta_propose(g, "u1", 2, HU_PERSONA_DELTA_TONE, "slack", "warmer", 0.9f,
                                     "test-fixture", now_ms + i, &delta_id),
            HU_OK);
        HU_ASSERT_GT((long)delta_id, 0L);
    }

    /* AutoDream: same config the daemon constructs at 3:00 AM. */
    hu_autodream_config_t ad_cfg = hu_autodream_default_config();
    ad_cfg.now_ms = now_ms;
    hu_autodream_report_t ad_report;
    memset(&ad_report, 0, sizeof(ad_report));
    HU_ASSERT_EQ(hu_autodream_run(&alloc, g, &ad_cfg, &ad_report), HU_OK);
    HU_ASSERT_GT((long)ad_report.finished_at_ms, 0L);
    HU_ASSERT_FALSE(ad_report.budget_exceeded);

    /* Persona evolver: same config the daemon constructs at 3:05 AM. The
     * daemon uses empty contact_id ("" / 0) for the global pass; here we
     * scope to "u1" so we can prove the evolver actually processed the
     * seeded deltas. */
    hu_persona_evolver_config_t pe_cfg = hu_persona_evolver_default_config();
    pe_cfg.now_ms = now_ms;
    hu_persona_evolver_report_t pe_report;
    memset(&pe_report, 0, sizeof(pe_report));
    HU_ASSERT_EQ(hu_persona_evolver_run(g, "u1", 2, &pe_cfg, &pe_report), HU_OK);
    HU_ASSERT_GT((long)pe_report.proposed_total, 0L);

    /* The daemon's actual call uses empty contact_id (global). Verify that
     * works without erroring. */
    memset(&pe_report, 0, sizeof(pe_report));
    HU_ASSERT_EQ(hu_persona_evolver_run(g, "", 0, &pe_cfg, &pe_report), HU_OK);

    hu_graph_close(g, &alloc);
}

/* Adversarial: same wire on an empty graph must not crash and must report
 * zero work done. The daemon will hit this on day-1 installs. */
static void daemon_housekeeping_handles_empty_graph(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    HU_ASSERT_EQ(hu_graph_open(&alloc, NULL, 0, &g), HU_OK);

    int64_t now_ms = (int64_t)time(NULL) * 1000LL;
    hu_autodream_config_t ad_cfg = hu_autodream_default_config();
    ad_cfg.now_ms = now_ms;
    hu_autodream_report_t ad_report;
    memset(&ad_report, 0, sizeof(ad_report));
    HU_ASSERT_EQ(hu_autodream_run(&alloc, g, &ad_cfg, &ad_report), HU_OK);
    HU_ASSERT_EQ((long)ad_report.quarantine_reviewed, 0L);
    HU_ASSERT_EQ((long)ad_report.communities_summarized, 0L);

    hu_persona_evolver_config_t pe_cfg = hu_persona_evolver_default_config();
    pe_cfg.now_ms = now_ms;
    hu_persona_evolver_report_t pe_report;
    memset(&pe_report, 0, sizeof(pe_report));
    HU_ASSERT_EQ(hu_persona_evolver_run(g, "", 0, &pe_cfg, &pe_report), HU_OK);
    HU_ASSERT_EQ((long)pe_report.proposed_total, 0L);
    HU_ASSERT_EQ((long)pe_report.applied, 0L);

    hu_graph_close(g, &alloc);
}

void run_daemon_housekeeping_tests(void) {
    HU_TEST_SUITE("DaemonHousekeeping");
    HU_RUN_TEST(daemon_housekeeping_runs_autodream_and_evolver_e2e);
    HU_RUN_TEST(daemon_housekeeping_handles_empty_graph);
}

#else  /* !HU_ENABLE_SQLITE */

void run_daemon_housekeeping_tests(void) {
    HU_TEST_SUITE("DaemonHousekeeping");
    /* No-op: housekeeping is sqlite-only. */
}

#endif
