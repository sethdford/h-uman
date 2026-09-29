/* Daemon-side identity-graph load/teardown (Sprint A.7 + B.8).
 *
 * Carved out of hu_service_run's startup preamble (DDD Phase E2 daemon split,
 * file-size ceiling ratchet). The two process-lifetime globals it owns are
 * still declared extern in human/daemon/common.h, so existing consumers are
 * unaffected — only the definitions and the load/teardown logic moved here.
 *
 * What it activates: cross-channel canonicalization. "Alice@imessage" and
 * "Alice@slack" reactions cluster under one persona contact instead of two
 * strangers. Both the reaction handler and the prompt builder borrow the same
 * graph for the process lifetime. */
#ifndef HUMAN_DAEMON_IDENTITY_GRAPH_H
#define HUMAN_DAEMON_IDENTITY_GRAPH_H

#ifdef __cplusplus
extern "C" {
#endif

struct hu_agent;

/* Load ~/.human/identity_graph.json and lend it to the reaction handler and
 * the personal model. agent may be NULL (used only for the observer).
 *
 * Conservative on absence: a missing file is the first-run default, NOT an
 * error — logs at info level and leaves the handlers un-wired, preserving the
 * prior "no canonicalization" behavior. Only real parse failures log an
 * error. */
void hu_daemon_identity_graph_load(struct hu_agent *agent);

/* Detach the borrows. The storage itself is process-lifetime; this just
 * ensures the reaction handler and personal model do not hold a stale pointer
 * if the process ever live-reloads. Idempotent. */
void hu_daemon_identity_graph_teardown(void);

#ifdef __cplusplus
}
#endif

#endif /* HUMAN_DAEMON_IDENTITY_GRAPH_H */
