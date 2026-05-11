#ifndef HU_CP_INTERNAL_H
#define HU_CP_INTERNAL_H

#include "human/core/allocator.h"
#include "human/core/json.h"
#include "human/gateway/control_protocol.h"
#include "human/gateway/ws_server.h"
#include <stddef.h>
#include <string.h>

/* Shared JSON helper for control protocol handlers */
static inline void cp_json_set_str(hu_allocator_t *a, hu_json_value_t *obj, const char *key,
                                   const char *val) {
    if (!val)
        val = "";
    hu_json_object_set(a, obj, key, hu_json_string_new(a, val, strlen(val)));
}

/* Chat handlers */
hu_error_t cp_chat_send(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                        const hu_control_protocol_t *proto, const hu_json_value_t *root, char **out,
                        size_t *out_len);
hu_error_t cp_chat_history(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                           const hu_control_protocol_t *proto, const hu_json_value_t *root,
                           char **out, size_t *out_len);
hu_error_t cp_chat_abort(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                         const hu_control_protocol_t *proto, const hu_json_value_t *root,
                         char **out, size_t *out_len);

/* Config handlers */
hu_error_t cp_config_get(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                         const hu_control_protocol_t *proto, const hu_json_value_t *root,
                         char **out, size_t *out_len);
hu_error_t cp_config_schema(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                            const hu_control_protocol_t *proto, const hu_json_value_t *root,
                            char **out, size_t *out_len);
hu_error_t cp_config_set(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                         const hu_control_protocol_t *proto, const hu_json_value_t *root,
                         char **out, size_t *out_len);
hu_error_t cp_config_apply(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                           const hu_control_protocol_t *proto, const hu_json_value_t *root,
                           char **out, size_t *out_len);

/* Admin handlers - declared in cp_admin.h or here */
hu_error_t cp_admin_connect(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                            const hu_control_protocol_t *proto, const hu_json_value_t *root,
                            char **out, size_t *out_len);
hu_error_t cp_admin_health(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                           const hu_control_protocol_t *proto, const hu_json_value_t *root,
                           char **out, size_t *out_len);
hu_error_t cp_admin_capabilities(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                 const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                 char **out, size_t *out_len);
hu_error_t cp_admin_sessions_list(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                  const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                  char **out, size_t *out_len);
hu_error_t cp_admin_sessions_patch(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                   const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                   char **out, size_t *out_len);
hu_error_t cp_admin_sessions_delete(hu_allocator_t *alloc, hu_app_context_t *app,
                                    hu_ws_conn_t *conn, const hu_control_protocol_t *proto,
                                    const hu_json_value_t *root, char **out, size_t *out_len);
hu_error_t cp_admin_persona_set(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                char **out, size_t *out_len);
hu_error_t cp_admin_tools_catalog(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                  const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                  char **out, size_t *out_len);
hu_error_t cp_admin_channels_status(hu_allocator_t *alloc, hu_app_context_t *app,
                                    hu_ws_conn_t *conn, const hu_control_protocol_t *proto,
                                    const hu_json_value_t *root, char **out, size_t *out_len);
hu_error_t cp_admin_models_list(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                char **out, size_t *out_len);
hu_error_t cp_admin_models_decisions(hu_allocator_t *alloc, hu_app_context_t *app,
                                     hu_ws_conn_t *conn, const hu_control_protocol_t *proto,
                                     const hu_json_value_t *root, char **out, size_t *out_len);
hu_error_t cp_admin_nodes_list(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                               const hu_control_protocol_t *proto, const hu_json_value_t *root,
                               char **out, size_t *out_len);
hu_error_t cp_admin_nodes_action(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                 const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                 char **out, size_t *out_len);
hu_error_t cp_admin_agents_list(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                char **out, size_t *out_len);
hu_error_t cp_admin_usage_summary(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                  const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                  char **out, size_t *out_len);
hu_error_t cp_admin_metrics_snapshot(hu_allocator_t *alloc, hu_app_context_t *app,
                                     hu_ws_conn_t *conn, const hu_control_protocol_t *proto,
                                     const hu_json_value_t *root, char **out, size_t *out_len);
/* Track D D2.2 — `metrics.fidelity` returns the same JSON shape as
 * `human ml fidelity-status` so the dashboard tile can drop the
 * demo-gateway mock and render live data. Both surfaces call the
 * shared `src/ml/fidelity.c` primitives — the only difference is
 * how the persona name is resolved (CLI: `--persona`; gateway:
 * `params.persona` or, when omitted, the agent's active persona).
 * The A/B section reads from the canonical
 * `~/.human/last_fidelity_ab.json` (written by
 * `scripts/lora-runner-ab.sh`) when present; otherwise
 * `ab.available` is false. */
hu_error_t cp_admin_metrics_fidelity(hu_allocator_t *alloc, hu_app_context_t *app,
                                     hu_ws_conn_t *conn, const hu_control_protocol_t *proto,
                                     const hu_json_value_t *root, char **out, size_t *out_len);
/* Track D D2.2 — `metrics.directive_telemetry` exposes the
 * per-variant fire counters maintained by
 * `acknowledgment_directive_for_overlay`. The dashboard uses this
 * to verify that channels with overlay-tuned directives actually
 * see the tuned variant fire (e.g. iMessage with casual+emoji
 * really does increment the casual_emoji counter, not the
 * default fallback). Read-only; no parameters. */
hu_error_t cp_admin_metrics_directive_telemetry(hu_allocator_t *alloc, hu_app_context_t *app,
                                                hu_ws_conn_t *conn,
                                                const hu_control_protocol_t *proto,
                                                const hu_json_value_t *root, char **out,
                                                size_t *out_len);
hu_error_t cp_admin_activity_recent(hu_allocator_t *alloc, hu_app_context_t *app,
                                    hu_ws_conn_t *conn, const hu_control_protocol_t *proto,
                                    const hu_json_value_t *root, char **out, size_t *out_len);
hu_error_t cp_admin_exec_approval(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                  const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                  char **out, size_t *out_len);

/* Auth handlers */
hu_error_t cp_admin_auth_token(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                               const hu_control_protocol_t *proto, const hu_json_value_t *root,
                               char **out, size_t *out_len);
hu_error_t cp_admin_oauth_start(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                char **out, size_t *out_len);
hu_error_t cp_admin_oauth_callback(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                   const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                   char **out, size_t *out_len);
hu_error_t cp_admin_oauth_refresh(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                  const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                  char **out, size_t *out_len);

#ifdef HU_HAS_CRON
hu_error_t cp_admin_cron_list(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                              const hu_control_protocol_t *proto, const hu_json_value_t *root,
                              char **out, size_t *out_len);
hu_error_t cp_admin_cron_add(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                             const hu_control_protocol_t *proto, const hu_json_value_t *root,
                             char **out, size_t *out_len);
hu_error_t cp_admin_cron_remove(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                char **out, size_t *out_len);
hu_error_t cp_admin_cron_run(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                             const hu_control_protocol_t *proto, const hu_json_value_t *root,
                             char **out, size_t *out_len);
hu_error_t cp_admin_cron_update(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                char **out, size_t *out_len);
hu_error_t cp_admin_cron_runs(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                              const hu_control_protocol_t *proto, const hu_json_value_t *root,
                              char **out, size_t *out_len);
#endif

#ifdef HU_HAS_SKILLS
hu_error_t cp_admin_skills_list(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                char **out, size_t *out_len);
hu_error_t cp_admin_skills_enable(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                  const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                  char **out, size_t *out_len);
hu_error_t cp_admin_skills_disable(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                   const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                   char **out, size_t *out_len);
hu_error_t cp_admin_skills_install(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                   const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                   char **out, size_t *out_len);
hu_error_t cp_admin_skills_search(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                  const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                  char **out, size_t *out_len);
hu_error_t cp_admin_skills_uninstall(hu_allocator_t *alloc, hu_app_context_t *app,
                                     hu_ws_conn_t *conn, const hu_control_protocol_t *proto,
                                     const hu_json_value_t *root, char **out, size_t *out_len);
hu_error_t cp_admin_skills_update(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                  const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                  char **out, size_t *out_len);
#endif

#ifdef HU_HAS_UPDATE
hu_error_t cp_admin_update_check(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                 const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                 char **out, size_t *out_len);
hu_error_t cp_admin_update_run(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                               const hu_control_protocol_t *proto, const hu_json_value_t *root,
                               char **out, size_t *out_len);
#endif

#ifdef HU_HAS_PUSH
hu_error_t cp_admin_push_register(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                  const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                  char **out, size_t *out_len);
hu_error_t cp_admin_push_unregister(hu_allocator_t *alloc, hu_app_context_t *app,
                                    hu_ws_conn_t *conn, const hu_control_protocol_t *proto,
                                    const hu_json_value_t *root, char **out, size_t *out_len);
#endif

/* Voice handlers */
hu_error_t cp_voice_transcribe(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                               const hu_control_protocol_t *proto, const hu_json_value_t *root,
                               char **out, size_t *out_len);

hu_error_t cp_voice_session_start(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                  const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                  char **out, size_t *out_len);
hu_error_t cp_voice_session_stop(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                 const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                 char **out, size_t *out_len);
hu_error_t cp_voice_session_interrupt(hu_allocator_t *alloc, hu_app_context_t *app,
                                      hu_ws_conn_t *conn, const hu_control_protocol_t *proto,
                                      const hu_json_value_t *root, char **out, size_t *out_len);
hu_error_t cp_voice_audio_end(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                              const hu_control_protocol_t *proto, const hu_json_value_t *root,
                              char **out, size_t *out_len);
hu_error_t cp_voice_config(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                           const hu_control_protocol_t *proto, const hu_json_value_t *root,
                           char **out, size_t *out_len);
hu_error_t cp_voice_tool_response(hu_allocator_t *alloc, hu_app_context_t *app,
                                  hu_ws_conn_t *conn, const hu_control_protocol_t *proto,
                                  const hu_json_value_t *root, char **out, size_t *out_len);
hu_error_t cp_voice_clone(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                          const hu_control_protocol_t *proto, const hu_json_value_t *root,
                          char **out, size_t *out_len);
hu_error_t cp_voice_validate(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                             const hu_control_protocol_t *proto, const hu_json_value_t *root,
                             char **out, size_t *out_len);

/* Memory handlers */
hu_error_t cp_memory_status(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                            const hu_control_protocol_t *proto, const hu_json_value_t *root,
                            char **out, size_t *out_len);
hu_error_t cp_memory_list(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                          const hu_control_protocol_t *proto, const hu_json_value_t *root,
                          char **out, size_t *out_len);
hu_error_t cp_memory_recall(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                            const hu_control_protocol_t *proto, const hu_json_value_t *root,
                            char **out, size_t *out_len);
hu_error_t cp_memory_store(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                           const hu_control_protocol_t *proto, const hu_json_value_t *root,
                           char **out, size_t *out_len);
hu_error_t cp_memory_forget(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                            const hu_control_protocol_t *proto, const hu_json_value_t *root,
                            char **out, size_t *out_len);
hu_error_t cp_memory_ingest(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                            const hu_control_protocol_t *proto, const hu_json_value_t *root,
                            char **out, size_t *out_len);
hu_error_t cp_memory_consolidate(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                 const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                 char **out, size_t *out_len);
hu_error_t cp_memory_graph(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                           const hu_control_protocol_t *proto, const hu_json_value_t *root,
                           char **out, size_t *out_len);

/* HuLa trace persistence (under ~/.human/hula_traces) */
hu_error_t cp_hula_traces_list(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                               const hu_control_protocol_t *proto, const hu_json_value_t *root,
                               char **out, size_t *out_len);
hu_error_t cp_hula_traces_get(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                              const hu_control_protocol_t *proto, const hu_json_value_t *root,
                              char **out, size_t *out_len);
hu_error_t cp_hula_traces_delete(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                 const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                 char **out, size_t *out_len);
hu_error_t cp_hula_traces_analytics(hu_allocator_t *alloc, hu_app_context_t *app,
                                    hu_ws_conn_t *conn, const hu_control_protocol_t *proto,
                                    const hu_json_value_t *root, char **out, size_t *out_len);

/* Turing score RPC handlers */
hu_error_t cp_turing_scores(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                            const hu_control_protocol_t *proto, const hu_json_value_t *root,
                            char **out, size_t *out_len);
hu_error_t cp_turing_trend(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                           const hu_control_protocol_t *proto, const hu_json_value_t *root,
                           char **out, size_t *out_len);
hu_error_t cp_turing_dimensions(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                char **out, size_t *out_len);
hu_error_t cp_turing_contact(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                             const hu_control_protocol_t *proto, const hu_json_value_t *root,
                             char **out, size_t *out_len);
hu_error_t cp_turing_trajectory(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                char **out, size_t *out_len);
hu_error_t cp_turing_ab_tests(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                              const hu_control_protocol_t *proto, const hu_json_value_t *root,
                              char **out, size_t *out_len);
hu_error_t cp_turing_channel(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                             const hu_control_protocol_t *proto, const hu_json_value_t *root,
                             char **out, size_t *out_len);

/* MCP Resources + Prompts RPC handlers */
hu_error_t cp_mcp_resources_list(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                 const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                 char **out, size_t *out_len);
hu_error_t cp_mcp_prompts_list(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                               const hu_control_protocol_t *proto, const hu_json_value_t *root,
                               char **out, size_t *out_len);

/* Security: CoT audit summary */
hu_error_t cp_security_cot_summary(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                   const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                   char **out, size_t *out_len);

/* Canvas handlers */
hu_error_t cp_canvas_list(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                          const hu_control_protocol_t *proto, const hu_json_value_t *root,
                          char **out, size_t *out_len);
hu_error_t cp_canvas_get(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                         const hu_control_protocol_t *proto, const hu_json_value_t *root,
                         char **out, size_t *out_len);
hu_error_t cp_canvas_edit(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                          const hu_control_protocol_t *proto, const hu_json_value_t *root,
                          char **out, size_t *out_len);
hu_error_t cp_canvas_undo(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                          const hu_control_protocol_t *proto, const hu_json_value_t *root,
                          char **out, size_t *out_len);
hu_error_t cp_canvas_redo(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                          const hu_control_protocol_t *proto, const hu_json_value_t *root,
                          char **out, size_t *out_len);

#endif /* HU_CP_INTERNAL_H */
