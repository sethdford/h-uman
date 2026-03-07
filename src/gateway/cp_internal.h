#ifndef SC_CP_INTERNAL_H
#define SC_CP_INTERNAL_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/json.h"
#include "seaclaw/gateway/control_protocol.h"
#include "seaclaw/gateway/ws_server.h"
#include <stddef.h>
#include <string.h>

/* Shared JSON helper for control protocol handlers */
static inline void cp_json_set_str(sc_allocator_t *a, sc_json_value_t *obj, const char *key,
                                   const char *val) {
    if (!val)
        val = "";
    sc_json_object_set(a, obj, key, sc_json_string_new(a, val, strlen(val)));
}

typedef sc_error_t (*sc_cp_handler_fn)(sc_allocator_t *alloc, sc_app_context_t *app,
                                       sc_ws_conn_t *conn, const sc_control_protocol_t *proto,
                                       const sc_json_value_t *root, char **out, size_t *out_len);

/* Chat handlers */
sc_error_t cp_chat_send(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                        const sc_control_protocol_t *proto, const sc_json_value_t *root, char **out,
                        size_t *out_len);
sc_error_t cp_chat_history(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                           const sc_control_protocol_t *proto, const sc_json_value_t *root,
                           char **out, size_t *out_len);
sc_error_t cp_chat_abort(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                         const sc_control_protocol_t *proto, const sc_json_value_t *root,
                         char **out, size_t *out_len);

/* Config handlers */
sc_error_t cp_config_get(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                         const sc_control_protocol_t *proto, const sc_json_value_t *root,
                         char **out, size_t *out_len);
sc_error_t cp_config_schema(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                            const sc_control_protocol_t *proto, const sc_json_value_t *root,
                            char **out, size_t *out_len);
sc_error_t cp_config_set(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                         const sc_control_protocol_t *proto, const sc_json_value_t *root,
                         char **out, size_t *out_len);
sc_error_t cp_config_apply(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                           const sc_control_protocol_t *proto, const sc_json_value_t *root,
                           char **out, size_t *out_len);

/* Admin handlers - declared in cp_admin.h or here */
sc_error_t cp_admin_connect(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                            const sc_control_protocol_t *proto, const sc_json_value_t *root,
                            char **out, size_t *out_len);
sc_error_t cp_admin_health(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                           const sc_control_protocol_t *proto, const sc_json_value_t *root,
                           char **out, size_t *out_len);
sc_error_t cp_admin_capabilities(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                 const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                 char **out, size_t *out_len);
sc_error_t cp_admin_sessions_list(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                  const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                  char **out, size_t *out_len);
sc_error_t cp_admin_sessions_patch(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                   const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                   char **out, size_t *out_len);
sc_error_t cp_admin_sessions_delete(sc_allocator_t *alloc, sc_app_context_t *app,
                                    sc_ws_conn_t *conn, const sc_control_protocol_t *proto,
                                    const sc_json_value_t *root, char **out, size_t *out_len);
sc_error_t cp_admin_persona_set(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                char **out, size_t *out_len);
sc_error_t cp_admin_tools_catalog(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                  const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                  char **out, size_t *out_len);
sc_error_t cp_admin_channels_status(sc_allocator_t *alloc, sc_app_context_t *app,
                                    sc_ws_conn_t *conn, const sc_control_protocol_t *proto,
                                    const sc_json_value_t *root, char **out, size_t *out_len);
sc_error_t cp_admin_models_list(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                char **out, size_t *out_len);
sc_error_t cp_admin_nodes_list(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                               const sc_control_protocol_t *proto, const sc_json_value_t *root,
                               char **out, size_t *out_len);
sc_error_t cp_admin_usage_summary(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                  const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                  char **out, size_t *out_len);
sc_error_t cp_admin_activity_recent(sc_allocator_t *alloc, sc_app_context_t *app,
                                    sc_ws_conn_t *conn, const sc_control_protocol_t *proto,
                                    const sc_json_value_t *root, char **out, size_t *out_len);
sc_error_t cp_admin_exec_approval(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                  const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                  char **out, size_t *out_len);

/* Auth handlers */
sc_error_t cp_admin_auth_token(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                               const sc_control_protocol_t *proto, const sc_json_value_t *root,
                               char **out, size_t *out_len);
sc_error_t cp_admin_oauth_start(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                char **out, size_t *out_len);
sc_error_t cp_admin_oauth_callback(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                   const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                   char **out, size_t *out_len);
sc_error_t cp_admin_oauth_refresh(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                  const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                  char **out, size_t *out_len);

#ifdef SC_HAS_CRON
sc_error_t cp_admin_cron_list(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                              const sc_control_protocol_t *proto, const sc_json_value_t *root,
                              char **out, size_t *out_len);
sc_error_t cp_admin_cron_add(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                             const sc_control_protocol_t *proto, const sc_json_value_t *root,
                             char **out, size_t *out_len);
sc_error_t cp_admin_cron_remove(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                char **out, size_t *out_len);
sc_error_t cp_admin_cron_run(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                             const sc_control_protocol_t *proto, const sc_json_value_t *root,
                             char **out, size_t *out_len);
sc_error_t cp_admin_cron_update(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                char **out, size_t *out_len);
sc_error_t cp_admin_cron_runs(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                              const sc_control_protocol_t *proto, const sc_json_value_t *root,
                              char **out, size_t *out_len);
#endif

#ifdef SC_HAS_SKILLS
sc_error_t cp_admin_skills_list(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                char **out, size_t *out_len);
sc_error_t cp_admin_skills_enable(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                  const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                  char **out, size_t *out_len);
sc_error_t cp_admin_skills_disable(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                   const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                   char **out, size_t *out_len);
sc_error_t cp_admin_skills_install(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                   const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                   char **out, size_t *out_len);
sc_error_t cp_admin_skills_search(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                  const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                  char **out, size_t *out_len);
sc_error_t cp_admin_skills_uninstall(sc_allocator_t *alloc, sc_app_context_t *app,
                                     sc_ws_conn_t *conn, const sc_control_protocol_t *proto,
                                     const sc_json_value_t *root, char **out, size_t *out_len);
sc_error_t cp_admin_skills_update(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                  const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                  char **out, size_t *out_len);
#endif

#ifdef SC_HAS_UPDATE
sc_error_t cp_admin_update_check(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                 const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                 char **out, size_t *out_len);
sc_error_t cp_admin_update_run(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                               const sc_control_protocol_t *proto, const sc_json_value_t *root,
                               char **out, size_t *out_len);
#endif

#ifdef SC_HAS_PUSH
sc_error_t cp_admin_push_register(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                  const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                  char **out, size_t *out_len);
sc_error_t cp_admin_push_unregister(sc_allocator_t *alloc, sc_app_context_t *app,
                                    sc_ws_conn_t *conn, const sc_control_protocol_t *proto,
                                    const sc_json_value_t *root, char **out, size_t *out_len);
#endif

#endif /* SC_CP_INTERNAL_H */
