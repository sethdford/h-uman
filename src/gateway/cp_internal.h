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

typedef hu_error_t (*hu_cp_handler_fn)(hu_allocator_t *alloc, hu_app_context_t *app,
                                       hu_ws_conn_t *conn, const hu_control_protocol_t *proto,
                                       const hu_json_value_t *root, char **out, size_t *out_len);

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
hu_error_t cp_admin_nodes_list(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                               const hu_control_protocol_t *proto, const hu_json_value_t *root,
                               char **out, size_t *out_len);
hu_error_t cp_admin_usage_summary(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                  const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                  char **out, size_t *out_len);
hu_error_t cp_admin_metrics_snapshot(hu_allocator_t *alloc, hu_app_context_t *app,
                                     hu_ws_conn_t *conn, const hu_control_protocol_t *proto,
                                     const hu_json_value_t *root, char **out, size_t *out_len);
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

#endif /* HU_CP_INTERNAL_H */
