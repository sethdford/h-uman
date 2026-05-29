#ifndef HU_DAEMON_COMMON_H
#define HU_DAEMON_COMMON_H

#include "human/agent/governor.h"
#include "human/memory/identity_resolver.h"
#include "human/provider.h"
#include <stdbool.h>
#include <stddef.h>

/* Cross-bucket daemon state. These are process-lifetime singletons shared by
 * more than one daemon module after the Phase 2 split. Defined in daemon.c
 * (until Phase 2b gives them a proper owner); declared here so the extracted
 * modules can reach them without re-introducing file-scope statics. */
extern hu_identity_graph_t g_identity_graph;
extern bool g_identity_graph_loaded;
extern hu_proactive_budget_t gov_budget;
extern bool gov_budget_inited;

/* Director/classification provider state — shared between daemon.c and director.c */
extern hu_provider_t g_classify_provider;
extern bool g_classify_provider_ok;
extern const char *g_classify_model;
extern size_t g_classify_model_len;

#endif /* HU_DAEMON_COMMON_H */
