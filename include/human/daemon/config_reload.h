/* SIGHUP config hot-reload for the daemon service loop.
 *
 * WHY THIS MODULE EXISTS: main.c installs a SIGHUP handler that calls
 * hu_config_set_reload_requested(), but until this module landed nothing in
 * hu_service_run ever read the flag — only the interactive CLI did
 * (src/agent/cli.c). SIGHUP to the daemon set a flag no one consumed, so the
 * installed handler advertised a hot-reload the daemon did not implement.
 *
 * SCOPE — read this before assuming a config edit takes effect on SIGHUP.
 * hu_agent_reload_config re-reads ~/.human/config.json and applies exactly
 * three AGENT-scoped things: the hook registry, the permission base level, and
 * instruction-file discovery. All three are live on the daemon's turn path
 * (the tool gate in hu_agent_internal_tool_gate / _pre_hook_check), so a
 * tightened permission tier or a new deny hook does take effect without a
 * restart.
 *
 * It does NOT refresh the `const hu_config_t *config` that hu_service_run
 * borrows from main.c. Everything the loop reads through that pointer — model
 * routing (config->agent.mr_*), channels, feeds, proactive/initiative gates —
 * keeps its startup values until the daemon restarts. That asymmetry is the
 * whole reason this module logs what did NOT reload alongside what did: a
 * "config reloaded" line with no such caveat is how an inert routing edit
 * looks like an applied one.
 *
 * THREADING: with --with-gateway, main.c serves bus messages by running
 * hu_agent_turn_stream_v2 on the gateway thread under svc_agent_mutex. The
 * service loop does not hold that mutex, and hu_agent_reload_config destroys
 * and recreates agent->hook_registry — so an unguarded reload can free the
 * registry under a concurrent gateway turn. main.c registers that mutex via
 * hu_daemon_config_reload_set_guard so the reload serializes against it. */
#ifndef HUMAN_DAEMON_CONFIG_RELOAD_H
#define HUMAN_DAEMON_CONFIG_RELOAD_H

#include <stdbool.h>

#if !defined(_WIN32)
#include <pthread.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct hu_agent;
struct hu_observer;

#if !defined(_WIN32)
/* Register the mutex that serializes agent access with the gateway bridge
 * thread. Borrowed, not owned — the caller keeps ownership for the process
 * lifetime. NULL clears the registration (the reload then runs unguarded,
 * which is correct only when no other thread touches the agent). */
void hu_daemon_config_reload_set_guard(pthread_mutex_t *guard);
#endif

/* One service-loop tick of the SIGHUP reload check.
 *
 * Consumes the process-global reload flag (set by main.c's SIGHUP handler via
 * hu_config_set_reload_requested). When a request was pending, reloads
 * agent-scoped config under the registered guard and logs both what reloaded
 * and what a restart is still required for.
 *
 * Call at a safe point — the top of the service loop, between turns — never
 * mid-turn: a turn in flight holds pointers into the structures the reload
 * frees.
 *
 * Returns true iff a reload was requested AND performed. Returns false when no
 * request was pending (the common case, every tick), and when a request was
 * pending but there is no agent to apply it to. */
bool hu_daemon_config_reload_tick(struct hu_agent *agent, struct hu_observer *observer);

#ifdef __cplusplus
}
#endif

#endif /* HUMAN_DAEMON_CONFIG_RELOAD_H */
