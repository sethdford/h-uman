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

/* Reactive-reply send-path decision (extracted per
 * .claude/rules/security-predicate-extraction.md).
 *
 * The iMessage reactive send loop in daemon.c has three mutually-exclusive
 * delivery paths: choreography (sends N segments), fragment-split (sends K
 * fragments), and a whole-message fallback. The whole-message fallback must
 * fire ONLY when neither of the other two already delivered the reply —
 * i.e. choreography did not fire AND the splitter produced no fragments.
 *
 * Returns true iff the whole-message fallback should send. A missing guard
 * here was the "duplicate reply" bug (choreography sent N segments, then the
 * fallback re-sent the whole reply because frag_count was left at 0).
 *
 * Pure predicate so the decision is unit-testable without driving the daemon
 * loop: pinned by tests/test_daemon_reply_fallback.c. */
static inline bool hu_daemon_should_send_whole_reply_fallback(bool use_choreography,
                                                              size_t frag_count) {
    return !use_choreography && frag_count == 0;
}

#endif /* HU_DAEMON_COMMON_H */
