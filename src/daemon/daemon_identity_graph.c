/* src/daemon/daemon_identity_graph.c
 *
 * Owner of the identity-graph singletons and their load/teardown, carved out of
 * hu_service_run's startup preamble (DDD Phase E2 daemon split). The globals
 * stay declared extern in human/daemon/common.h — only their definitions and
 * the surrounding logic moved, so no consumer changed. */

#include "human/daemon/identity_graph.h"

#include "human/agent.h"
#include "human/agent/reaction_handler.h"
#include "human/core/error.h"
#include "human/core/log.h"
#include "human/daemon/common.h"
#include "human/memory/identity_resolver.h"
#include "human/memory/personal_model.h"

#include <stdio.h>
#include <stdlib.h>

/* Process-lifetime singletons. Declared extern in human/daemon/common.h; this
 * module is the "proper owner" that header's comment was waiting for. */
hu_identity_graph_t g_identity_graph;
bool g_identity_graph_loaded = false;

void hu_daemon_identity_graph_load(struct hu_agent *agent) {
    hu_agent_t *ag = (hu_agent_t *)agent;
    hu_observer_t *obs = ag ? ag->observer : NULL;

    const char *home = getenv("HOME");
    char ident_path[1024];
    if (!home || !home[0])
        return;
    if (snprintf(ident_path, sizeof(ident_path), "%s/.human/identity_graph.json", home) <= 0)
        return;

    hu_error_t ie = hu_identity_load(&g_identity_graph, ident_path);
    if (ie == HU_OK) {
        hu_reaction_handler_set_identity_graph(&g_identity_graph);
        /* Sprint B.8 wire — share the same graph borrow with the prompt builder
         * so the IDENTITY suggestion block fires. */
        hu_personal_model_set_identity_graph(&g_identity_graph);
        g_identity_graph_loaded = true;
        hu_log_info("human", obs, "identity graph loaded: %zu contacts from %s",
                    g_identity_graph.contact_count, ident_path);
    } else if (ie == HU_ERR_NOT_FOUND) {
        /* First-run default: no identity_graph.json yet. Info level — operators
         * who want cross-channel merging will know to create the file. */
        hu_log_info("human", obs,
                    "identity graph: no file at %s — cross-channel reactor "
                    "canonicalization disabled (create the file via hu_identity_save "
                    "to enable)",
                    ident_path);
    } else {
        hu_log_error("human", obs,
                     "identity graph: load failed (err=%d) for %s — cross-channel "
                     "canonicalization disabled",
                     (int)ie, ident_path);
    }
}

void hu_daemon_identity_graph_teardown(void) {
    if (!g_identity_graph_loaded)
        return;
    hu_reaction_handler_set_identity_graph(NULL);
    hu_personal_model_set_identity_graph(NULL); /* B.8 teardown */
    g_identity_graph_loaded = false;
}
