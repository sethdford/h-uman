/* Contract tests for the daemon's identity-graph load/teardown.
 *
 * This logic ran inline inside hu_service_run's startup preamble for its whole
 * life, which made it untestable — hu_service_run returns before its loop under
 * HU_IS_TEST, and the preamble is inside the same #else branch. Carving it into
 * src/daemon/daemon_identity_graph.c is what made these assertions possible, so
 * they are the point of the carve, not paperwork for check-untested.sh.
 *
 * The load contract has a deliberate asymmetry worth pinning: a MISSING file is
 * the first-run default and must leave the handlers un-wired without erroring,
 * while a file that loads must flip g_identity_graph_loaded and populate the
 * graph. Conflating those two is how "cross-channel canonicalization silently
 * off" would look identical to "no config yet".
 *
 * Hermetic via HOME — the loader resolves $HOME/.human/identity_graph.json. */
#include "test_framework.h"

#include "human/daemon/common.h"
#include "human/daemon/identity_graph.h"
#include "human/memory/identity_resolver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char idg_prev_home[1024];
static bool idg_prev_home_set = false;
static char idg_test_home[512];

static void idg_home_setup(void) {
    const char *prev = getenv("HOME");
    idg_prev_home_set = (prev != NULL);
    if (prev)
        snprintf(idg_prev_home, sizeof(idg_prev_home), "%s", prev);

    snprintf(idg_test_home, sizeof(idg_test_home), "/tmp/hu-identity-graph-test-%d", (int)getpid());
    (void)mkdir(idg_test_home, 0700);

    char human_dir[600];
    snprintf(human_dir, sizeof(human_dir), "%s/.human", idg_test_home);
    (void)mkdir(human_dir, 0700);

    setenv("HOME", idg_test_home, 1);

    /* Start every case from the un-loaded state — the flag is a process-global
     * shared with any sibling case. */
    hu_daemon_identity_graph_teardown();
}

static void idg_home_teardown(void) {
    hu_daemon_identity_graph_teardown();

    char path[700];
    snprintf(path, sizeof(path), "%s/.human/identity_graph.json", idg_test_home);
    (void)unlink(path);

    if (idg_prev_home_set)
        setenv("HOME", idg_prev_home, 1);
    else
        unsetenv("HOME");
}

/* Build a two-channel graph with production symbols and persist it where the
 * loader will look. Using hu_identity_resolve + hu_identity_save rather than a
 * hand-written fixture keeps the test honest about the real on-disk format. */
static bool idg_write_fixture(void) {
    const char *handles[] = {"+15551234567", "alice.example"};
    const char *channels[] = {"imessage", "slack"};
    const char *names[] = {"Alice Example", "Alice Example"};

    hu_identity_graph_t graph;
    memset(&graph, 0, sizeof(graph));
    if (hu_identity_resolve(handles, channels, names, 2, &graph) != HU_OK)
        return false;

    char path[700];
    snprintf(path, sizeof(path), "%s/.human/identity_graph.json", idg_test_home);
    return hu_identity_save(&graph, path) == HU_OK;
}

/* A missing file is the first-run default, NOT an error: the loader must leave
 * the graph un-wired so behavior matches "no canonicalization", rather than
 * half-wiring it. */
static void identity_graph_load_without_file_leaves_graph_unwired(void) {
    idg_home_setup();

    HU_ASSERT_FALSE(g_identity_graph_loaded); /* precondition */

    hu_daemon_identity_graph_load(NULL);

    HU_ASSERT_FALSE(g_identity_graph_loaded);

    idg_home_teardown();
}

/* THE contract: a graph on disk is loaded into the process-global and marked
 * loaded, so the reaction handler and prompt builder get their borrow. */
static void identity_graph_load_marks_loaded_and_populates_contacts(void) {
    idg_home_setup();

    if (!idg_write_fixture()) {
        idg_home_teardown();
        HU_ASSERT_TRUE(false); /* fixture is a precondition, not an outcome */
        return;
    }

    /* Precondition: nothing loaded, no contacts. */
    HU_ASSERT_FALSE(g_identity_graph_loaded);

    hu_daemon_identity_graph_load(NULL);

    /* Postcondition: the flag flipped AND the graph actually carries contacts —
     * the flag alone would pass even if the file parsed to nothing. */
    HU_ASSERT_TRUE(g_identity_graph_loaded);
    HU_ASSERT_TRUE(g_identity_graph.contact_count > 0);

    idg_home_teardown();
}

/* Teardown must clear the flag so a later load is not mistaken for an
 * already-wired graph. */
static void identity_graph_teardown_clears_loaded_flag(void) {
    idg_home_setup();

    if (!idg_write_fixture()) {
        idg_home_teardown();
        HU_ASSERT_TRUE(false);
        return;
    }
    hu_daemon_identity_graph_load(NULL);
    HU_ASSERT_TRUE(g_identity_graph_loaded); /* precondition for the teardown */

    hu_daemon_identity_graph_teardown();

    HU_ASSERT_FALSE(g_identity_graph_loaded);

    idg_home_teardown();
}

/* Teardown runs unconditionally at daemon shutdown, including on paths where no
 * graph was ever loaded. It must not fault or flip state. */
static void identity_graph_teardown_is_idempotent(void) {
    idg_home_setup();

    hu_daemon_identity_graph_teardown();
    hu_daemon_identity_graph_teardown();

    HU_ASSERT_FALSE(g_identity_graph_loaded);

    idg_home_teardown();
}

void run_daemon_identity_graph_tests(void) {
    HU_TEST_SUITE("DaemonIdentityGraph");
    HU_RUN_TEST(identity_graph_load_without_file_leaves_graph_unwired);
    HU_RUN_TEST(identity_graph_load_marks_loaded_and_populates_contacts);
    HU_RUN_TEST(identity_graph_teardown_clears_loaded_flag);
    HU_RUN_TEST(identity_graph_teardown_is_idempotent);
}
