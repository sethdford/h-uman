/* Admin/system control protocol handlers: connect, health, sessions, tools, channels,
 * cron, skills, models, nodes, usage, activity, push, auth, etc. */
#include "cp_internal.h"
#include "seaclaw/agent.h"
#include "seaclaw/agent/awareness.h"
#include "seaclaw/bus.h"
#include "seaclaw/channel_catalog.h"
#include "seaclaw/config.h"
#include "seaclaw/core/process_util.h"
#include "seaclaw/cost.h"
#include "seaclaw/gateway/oauth.h"
#include "seaclaw/security.h"
#include "seaclaw/session.h"
#include "seaclaw/tool.h"
#include "seaclaw/version.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef SC_HAS_CRON
#include "seaclaw/cron.h"
#include "seaclaw/crontab.h"
#endif
#ifdef SC_HAS_PUSH
#include "seaclaw/gateway/push.h"
#endif
#ifdef SC_HAS_SKILLS
#include "seaclaw/skill_registry.h"
#include "seaclaw/skillforge.h"
#endif
#ifdef SC_HAS_UPDATE
#include "seaclaw/update.h"
#endif

#ifdef SC_GATEWAY_POSIX

/* ── connect ─────────────────────────────────────────────────────────── */

sc_error_t cp_admin_connect(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                            const sc_control_protocol_t *proto, const sc_json_value_t *root,
                            char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;

    cp_json_set_str(alloc, obj, "type", "hello-ok");

    sc_json_value_t *server = sc_json_object_new(alloc);
    cp_json_set_str(alloc, server, "version", sc_version_string());
    sc_json_object_set(alloc, obj, "server", server);
    sc_json_object_set(alloc, obj, "protocol", sc_json_number_new(alloc, 1));

    sc_json_value_t *features = sc_json_object_new(alloc);
    sc_json_value_t *methods_arr = sc_json_array_new(alloc);

    static const char *const methods[] = {
        "auth.token",      "connect",          "health",
        "config.get",      "config.schema",    "capabilities",
        "chat.send",       "chat.history",     "chat.abort",
        "config.set",      "config.apply",     "sessions.list",
        "sessions.patch",  "sessions.delete",  "persona.set",
        "tools.catalog",   "channels.status",  "cron.list",
        "cron.add",        "cron.remove",      "cron.run",
        "cron.update",     "cron.runs",        "skills.list",
        "skills.install",  "skills.enable",    "skills.disable",
        "skills.search",   "skills.uninstall", "skills.update",
        "update.check",    "update.run",       "exec.approval.resolve",
        "usage.summary",   "models.list",      "nodes.list",
        "activity.recent", "push.register",    "push.unregister"};
    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
        sc_json_value_t *m = sc_json_string_new(alloc, methods[i], strlen(methods[i]));
        if (m)
            sc_json_array_push(alloc, methods_arr, m);
    }
    sc_json_object_set(alloc, features, "methods", methods_arr);

    if (app) {
        sc_json_object_set(alloc, features, "sessions",
                           sc_json_bool_new(alloc, app->sessions != NULL));
        sc_json_object_set(alloc, features, "cron", sc_json_bool_new(alloc, app->cron != NULL));
        sc_json_object_set(alloc, features, "skills", sc_json_bool_new(alloc, app->skills != NULL));
        sc_json_object_set(alloc, features, "cost_tracking",
                           sc_json_bool_new(alloc, app->costs != NULL));
    }

    sc_json_object_set(alloc, obj, "features", features);

    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── health ──────────────────────────────────────────────────────────── */

sc_error_t cp_admin_health(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                           const sc_control_protocol_t *proto, const sc_json_value_t *root,
                           char **out, size_t *out_len) {
    (void)app;
    (void)conn;
    (void)proto;
    (void)root;
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;
    cp_json_set_str(alloc, obj, "status", "ok");
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── capabilities ────────────────────────────────────────────────────── */

sc_error_t cp_admin_capabilities(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                 const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                 char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;

    cp_json_set_str(alloc, obj, "version", sc_version_string());

    size_t tool_count = app ? app->tools_count : 0;
    sc_json_object_set(alloc, obj, "tools", sc_json_number_new(alloc, (double)tool_count));

    size_t ch_count = 0;
    if (app && app->config) {
        size_t total = 0;
        const sc_channel_meta_t *catalog = sc_channel_catalog_all(&total);
        size_t configured = 0;
        for (size_t i = 0; i < total; i++) {
            if (sc_channel_catalog_is_configured(app->config, catalog[i].id))
                configured++;
        }
        ch_count = configured;
    }
    sc_json_object_set(alloc, obj, "channels", sc_json_number_new(alloc, (double)ch_count));

    size_t prov_count = (app && app->config) ? app->config->providers_len : 0;
    sc_json_object_set(alloc, obj, "providers", sc_json_number_new(alloc, (double)prov_count));

    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── sessions.list ───────────────────────────────────────────────────── */

sc_error_t cp_admin_sessions_list(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                  const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                  char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;

    sc_json_value_t *arr = sc_json_array_new(alloc);

    if (app && app->sessions) {
        size_t count = 0;
        sc_session_summary_t *summaries = sc_session_list(app->sessions, alloc, &count);
        if (summaries) {
            for (size_t i = 0; i < count; i++) {
                sc_json_value_t *s = sc_json_object_new(alloc);
                cp_json_set_str(alloc, s, "key", summaries[i].session_key);
                cp_json_set_str(alloc, s, "label", summaries[i].label);
                sc_json_object_set(alloc, s, "created_at",
                                   sc_json_number_new(alloc, (double)summaries[i].created_at));
                sc_json_object_set(alloc, s, "last_active",
                                   sc_json_number_new(alloc, (double)summaries[i].last_active));
                sc_json_object_set(alloc, s, "turn_count",
                                   sc_json_number_new(alloc, (double)summaries[i].turn_count));
                sc_json_array_push(alloc, arr, s);
            }
            alloc->free(alloc->ctx, summaries, count * sizeof(sc_session_summary_t));
        }
    }

    sc_json_object_set(alloc, obj, "sessions", arr);
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── sessions.patch ──────────────────────────────────────────────────── */

sc_error_t cp_admin_sessions_patch(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                   const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                   char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    bool patched = false;

    if (root && app && app->sessions) {
        sc_json_value_t *params = sc_json_object_get(root, "params");
        if (params) {
            const char *key = sc_json_get_string(params, "key");
            const char *label = sc_json_get_string(params, "label");
            if (key && label) {
                sc_error_t e = sc_session_patch(app->sessions, key, label);
                patched = (e == SC_OK);
            }
        }
    }

    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;
    sc_json_object_set(alloc, obj, "patched", sc_json_bool_new(alloc, patched));
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── sessions.delete ─────────────────────────────────────────────────── */

sc_error_t cp_admin_sessions_delete(sc_allocator_t *alloc, sc_app_context_t *app,
                                    sc_ws_conn_t *conn, const sc_control_protocol_t *proto,
                                    const sc_json_value_t *root, char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    bool deleted = false;

    if (root && app && app->sessions) {
        sc_json_value_t *params = sc_json_object_get(root, "params");
        if (params) {
            const char *key = sc_json_get_string(params, "key");
            if (key) {
                sc_error_t e = sc_session_delete(app->sessions, key);
                deleted = (e == SC_OK);
            }
        }
    }

    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;
    sc_json_object_set(alloc, obj, "deleted", sc_json_bool_new(alloc, deleted));
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── persona.set ─────────────────────────────────────────────────────── */

sc_error_t cp_admin_persona_set(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;

    if (!app || !app->agent) {
        cp_json_set_str(alloc, obj, "error", "no agent (gateway must run with --with-agent)");
        sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
        sc_json_free(alloc, obj);
        return err;
    }

    if (!root) {
        cp_json_set_str(alloc, obj, "error", "params required");
        sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
        sc_json_free(alloc, obj);
        return err;
    }

    sc_json_value_t *params = sc_json_object_get(root, "params");
    sc_json_value_t *name_val = params ? sc_json_object_get(params, "name") : NULL;

    if (!name_val) {
        cp_json_set_str(alloc, obj, "error", "name is required");
        sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
        sc_json_free(alloc, obj);
        return err;
    }

    const char *name = NULL;
    size_t name_len = 0;
    if (name_val->type == SC_JSON_NULL) {
        /* {"name": null} — clear persona */
    } else if (name_val->type == SC_JSON_STRING) {
        name = name_val->data.string.ptr;
        name_len = name_val->data.string.len;
    } else {
        cp_json_set_str(alloc, obj, "error", "name must be string or null");
        sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
        sc_json_free(alloc, obj);
        return err;
    }

#ifdef SC_HAS_PERSONA
    sc_error_t err = sc_agent_set_persona(app->agent, name, name_len);
    if (err != SC_OK) {
        const char *emsg = sc_error_string(err);
        cp_json_set_str(alloc, obj, "error", emsg ? emsg : "failed to set persona");
        sc_error_t serr = sc_json_stringify(alloc, obj, out, out_len);
        sc_json_free(alloc, obj);
        return serr;
    }
#else
    (void)name;
    (void)name_len;
    cp_json_set_str(alloc, obj, "error", "persona support not built (SC_ENABLE_PERSONA=OFF)");
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
#endif

    sc_json_object_set(alloc, obj, "ok", sc_json_bool_new(alloc, true));
    sc_error_t serr = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return serr;
}

/* ── tools.catalog ───────────────────────────────────────────────────── */

sc_error_t cp_admin_tools_catalog(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                  const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                  char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;

    sc_json_value_t *arr = sc_json_array_new(alloc);

    if (app && app->tools) {
        for (size_t i = 0; i < app->tools_count; i++) {
            sc_tool_t *t = &app->tools[i];
            if (!t->vtable)
                continue;
            sc_json_value_t *tool_obj = sc_json_object_new(alloc);

            const char *name = t->vtable->name ? t->vtable->name(t->ctx) : "";
            const char *desc = t->vtable->description ? t->vtable->description(t->ctx) : "";

            cp_json_set_str(alloc, tool_obj, "name", name);
            cp_json_set_str(alloc, tool_obj, "description", desc);

            const char *params =
                t->vtable->parameters_json ? t->vtable->parameters_json(t->ctx) : NULL;
            if (params) {
                cp_json_set_str(alloc, tool_obj, "parameters", params);
            }

            sc_json_array_push(alloc, arr, tool_obj);
        }
    }

    sc_json_object_set(alloc, obj, "tools", arr);
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── channels.status ─────────────────────────────────────────────────── */

sc_error_t cp_admin_channels_status(sc_allocator_t *alloc, sc_app_context_t *app,
                                    sc_ws_conn_t *conn, const sc_control_protocol_t *proto,
                                    const sc_json_value_t *root, char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;

    sc_json_value_t *arr = sc_json_array_new(alloc);

    size_t catalog_count = 0;
    const sc_channel_meta_t *catalog = sc_channel_catalog_all(&catalog_count);
    for (size_t i = 0; i < catalog_count; i++) {
        sc_json_value_t *ch_obj = sc_json_object_new(alloc);
        cp_json_set_str(alloc, ch_obj, "key", catalog[i].key);
        cp_json_set_str(alloc, ch_obj, "label", catalog[i].label);

        bool build_enabled = sc_channel_catalog_is_build_enabled(catalog[i].id);
        sc_json_object_set(alloc, ch_obj, "build_enabled", sc_json_bool_new(alloc, build_enabled));

        bool configured = false;
        if (app && app->config) {
            configured = sc_channel_catalog_is_configured(app->config, catalog[i].id);
        }
        sc_json_object_set(alloc, ch_obj, "configured", sc_json_bool_new(alloc, configured));

        char status_buf[64];
        const char *status = "unavailable";
        if (app && app->config) {
            status = sc_channel_catalog_status_text(app->config, &catalog[i], status_buf,
                                                    sizeof(status_buf));
        }
        cp_json_set_str(alloc, ch_obj, "status", status);

        sc_json_array_push(alloc, arr, ch_obj);
    }

    sc_json_object_set(alloc, obj, "channels", arr);
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── models.list ─────────────────────────────────────────────────────── */

sc_error_t cp_admin_models_list(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;

    sc_json_value_t *arr = sc_json_array_new(alloc);

    if (app && app->config && app->config->providers) {
        for (size_t i = 0; i < app->config->providers_len; i++) {
            sc_provider_entry_t *pe = &app->config->providers[i];
            sc_json_value_t *p = sc_json_object_new(alloc);
            cp_json_set_str(alloc, p, "name", pe->name);
            sc_json_object_set(alloc, p, "has_key",
                               sc_json_bool_new(alloc, pe->api_key && pe->api_key[0]));
            cp_json_set_str(alloc, p, "base_url", pe->base_url);
            sc_json_object_set(alloc, p, "native_tools", sc_json_bool_new(alloc, pe->native_tools));
            sc_json_object_set(
                alloc, p, "is_default",
                sc_json_bool_new(alloc, app->config->default_provider &&
                                            strcmp(pe->name, app->config->default_provider) == 0));
            sc_json_array_push(alloc, arr, p);
        }
    }

    cp_json_set_str(alloc, obj, "default_model",
                    (app && app->config) ? app->config->default_model : "");
    sc_json_object_set(alloc, obj, "providers", arr);
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── nodes.list ──────────────────────────────────────────────────────── */

sc_error_t cp_admin_nodes_list(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                               const sc_control_protocol_t *proto, const sc_json_value_t *root,
                               char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;

    sc_json_value_t *arr = sc_json_array_new(alloc);

    size_t node_count = 0;
    const char *default_id = "local";
    const char *default_status = "online";

    if (app && app->config && app->config->nodes_len > 0) {
        node_count = app->config->nodes_len;
        for (size_t i = 0; i < node_count; i++) {
            sc_json_value_t *n = sc_json_object_new(alloc);
            const char *name = app->config->nodes[i].name ? app->config->nodes[i].name : default_id;
            const char *status =
                app->config->nodes[i].status ? app->config->nodes[i].status : default_status;
            cp_json_set_str(alloc, n, "id", name);
            cp_json_set_str(alloc, n, "type", "gateway");
            cp_json_set_str(alloc, n, "status", status);
            if (strcmp(name, "local") == 0) {
                cp_json_set_str(alloc, n, "version", sc_version_string());
#if !defined(SC_IS_TEST)
                {
                    char hostname[256] = {0};
                    if (gethostname(hostname, sizeof(hostname) - 1) == 0)
                        cp_json_set_str(alloc, n, "hostname", hostname);
#if defined(__APPLE__) || defined(__linux__)
                    struct timespec ts;
                    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
                        sc_json_object_set(alloc, n, "uptime_secs",
                                           sc_json_number_new(alloc, (double)ts.tv_sec));
#endif
                }
#endif
                sc_json_object_set(alloc, n, "ws_connections", sc_json_number_new(alloc, 1.0));
            } else {
                sc_json_object_set(alloc, n, "ws_connections", sc_json_number_new(alloc, 0.0));
            }
            sc_json_array_push(alloc, arr, n);
        }
    } else {
        /* Backward compatibility: no config or no nodes configured → default local node */
        sc_json_value_t *local = sc_json_object_new(alloc);
        cp_json_set_str(alloc, local, "id", default_id);
        cp_json_set_str(alloc, local, "type", "gateway");
        cp_json_set_str(alloc, local, "status", default_status);
        cp_json_set_str(alloc, local, "version", sc_version_string());
#if !defined(SC_IS_TEST)
        {
            char hostname[256] = {0};
            if (gethostname(hostname, sizeof(hostname) - 1) == 0)
                cp_json_set_str(alloc, local, "hostname", hostname);
#if defined(__APPLE__) || defined(__linux__)
            struct timespec ts;
            if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
                sc_json_object_set(alloc, local, "uptime_secs",
                                   sc_json_number_new(alloc, (double)ts.tv_sec));
#endif
        }
#endif
        sc_json_object_set(alloc, local, "ws_connections",
                           sc_json_number_new(alloc, (app && app->config) ? 1.0 : 0.0));
        sc_json_array_push(alloc, arr, local);
    }

    sc_json_object_set(alloc, obj, "nodes", arr);
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── usage.summary ───────────────────────────────────────────────────── */

sc_error_t cp_admin_usage_summary(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                  const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                  char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;

    if (app && app->costs) {
        sc_cost_summary_t summary;
        int64_t now = (int64_t)time(NULL);
        sc_cost_get_summary(app->costs, now, &summary);

        sc_json_object_set(alloc, obj, "session_cost_usd",
                           sc_json_number_new(alloc, summary.session_cost_usd));
        sc_json_object_set(alloc, obj, "daily_cost_usd",
                           sc_json_number_new(alloc, summary.daily_cost_usd));
        sc_json_object_set(alloc, obj, "monthly_cost_usd",
                           sc_json_number_new(alloc, summary.monthly_cost_usd));
        sc_json_object_set(alloc, obj, "total_tokens",
                           sc_json_number_new(alloc, (double)summary.total_tokens));
        sc_json_object_set(alloc, obj, "request_count",
                           sc_json_number_new(alloc, (double)summary.request_count));
    } else {
        sc_json_object_set(alloc, obj, "session_cost_usd", sc_json_number_new(alloc, 0));
        sc_json_object_set(alloc, obj, "daily_cost_usd", sc_json_number_new(alloc, 0));
        sc_json_object_set(alloc, obj, "monthly_cost_usd", sc_json_number_new(alloc, 0));
        sc_json_object_set(alloc, obj, "total_tokens", sc_json_number_new(alloc, 0));
        sc_json_object_set(alloc, obj, "request_count", sc_json_number_new(alloc, 0));
    }

    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── activity.recent ─────────────────────────────────────────────────── */

sc_error_t cp_admin_activity_recent(sc_allocator_t *alloc, sc_app_context_t *app,
                                    sc_ws_conn_t *conn, const sc_control_protocol_t *proto,
                                    const sc_json_value_t *root, char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;

    sc_json_value_t *events_arr = sc_json_array_new(alloc);
    if (!events_arr) {
        sc_json_free(alloc, obj);
        return SC_ERR_OUT_OF_MEMORY;
    }

    int64_t now_ms = (int64_t)time(NULL) * 1000;

    if (app && app->agent) {
        const sc_awareness_t *aw = sc_agent_get_awareness(app->agent);
        if (aw) {
            const sc_awareness_state_t *s = &aw->state;

            if (s->total_errors > 0) {
                size_t nerr = s->total_errors < SC_AWARENESS_MAX_RECENT_ERRORS
                                  ? s->total_errors
                                  : SC_AWARENESS_MAX_RECENT_ERRORS;
                for (size_t i = 0; i < nerr; i++) {
                    size_t idx = (s->error_write_idx + SC_AWARENESS_MAX_RECENT_ERRORS - nerr + i) %
                                 SC_AWARENESS_MAX_RECENT_ERRORS;
                    sc_json_value_t *ev = sc_json_object_new(alloc);
                    if (!ev)
                        continue;
                    cp_json_set_str(alloc, ev, "type", "error");
                    cp_json_set_str(alloc, ev, "message", s->recent_errors[idx].text);
                    int64_t ts = s->recent_errors[idx].timestamp_ms > 0
                                     ? (int64_t)s->recent_errors[idx].timestamp_ms
                                     : now_ms;
                    sc_json_object_set(alloc, ev, "time", sc_json_number_new(alloc, (double)ts));
                    sc_json_array_push(alloc, events_arr, ev);
                }
            }

            if (s->messages_received > 0) {
                sc_json_value_t *ev = sc_json_object_new(alloc);
                if (ev) {
                    cp_json_set_str(alloc, ev, "type", "system");
                    char msg[128];
                    snprintf(msg, sizeof(msg), "%llu messages received, %llu tool calls",
                             (unsigned long long)s->messages_received,
                             (unsigned long long)s->tool_calls);
                    cp_json_set_str(alloc, ev, "message", msg);
                    sc_json_object_set(alloc, ev, "time",
                                       sc_json_number_new(alloc, (double)now_ms));
                    sc_json_array_push(alloc, events_arr, ev);
                }
            }

            if (s->health_degraded) {
                sc_json_value_t *ev = sc_json_object_new(alloc);
                if (ev) {
                    cp_json_set_str(alloc, ev, "type", "error");
                    cp_json_set_str(alloc, ev, "message", "System health is degraded");
                    sc_json_object_set(alloc, ev, "time",
                                       sc_json_number_new(alloc, (double)now_ms));
                    sc_json_array_push(alloc, events_arr, ev);
                }
            }
        }
    }

    sc_json_object_set(alloc, obj, "events", events_arr);
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── exec.approval.resolve ───────────────────────────────────────────── */

sc_error_t cp_admin_exec_approval(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                  const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                  char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    bool resolved = false;
    bool approved = false;

    if (root && app && app->bus) {
        sc_json_value_t *params = sc_json_object_get(root, "params");
        if (params) {
            const char *id = sc_json_get_string(params, "id");
            approved = sc_json_get_bool(params, "approved", false);
            if (id && id[0]) {
                sc_bus_publish_simple(app->bus, approved ? SC_BUS_TOOL_CALL : SC_BUS_ERROR,
                                      "approval", id, approved ? "approved" : "denied");
                resolved = true;
            }
        }
    }

    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;
    sc_json_object_set(alloc, obj, "resolved", sc_json_bool_new(alloc, resolved));
    sc_json_object_set(alloc, obj, "approved", sc_json_bool_new(alloc, approved));
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── auth.token ──────────────────────────────────────────────────────── */

sc_error_t cp_admin_auth_token(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                               const sc_control_protocol_t *proto, const sc_json_value_t *root,
                               char **out, size_t *out_len) {
    (void)app;
    const char *token = NULL;
    if (root) {
        sc_json_value_t *params = sc_json_object_get(root, "params");
        if (params)
            token = sc_json_get_string(params, "token");
    }
    if (!token || !token[0]) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj)
            return SC_ERR_OUT_OF_MEMORY;
        cp_json_set_str(alloc, obj, "error", "missing token");
        sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
        sc_json_free(alloc, obj);
        return err;
    }
    bool valid = false;
    if (proto->pairing_guard && sc_pairing_guard_is_authenticated(proto->pairing_guard, token))
        valid = true;
    else if (proto->auth_token && proto->auth_token[0] &&
             sc_pairing_guard_constant_time_eq(token, proto->auth_token))
        valid = true;
    if (valid) {
        conn->authenticated = true;
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj)
            return SC_ERR_OUT_OF_MEMORY;
        cp_json_set_str(alloc, obj, "result", "authenticated");
        sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
        sc_json_free(alloc, obj);
        return err;
    }
    (void)alloc;
    (void)out;
    (void)out_len;
    return SC_ERR_GATEWAY_AUTH;
}

/* ── auth.oauth.start ────────────────────────────────────────────────── */

sc_error_t cp_admin_oauth_start(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                char **out, size_t *out_len) {
    (void)app;
    (void)conn;
    sc_oauth_ctx_t *ctx = (sc_oauth_ctx_t *)proto->oauth_ctx;
    if (!ctx || !proto->oauth_pending_store || !proto->oauth_pending_lookup ||
        !proto->oauth_pending_remove) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj)
            return SC_ERR_OUT_OF_MEMORY;
        cp_json_set_str(alloc, obj, "error", "OAuth not configured");
        sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
        sc_json_free(alloc, obj);
        return err;
    }
    const char *provider = NULL;
    const char *redirect_uri = NULL;
    if (root) {
        sc_json_value_t *params = sc_json_object_get(root, "params");
        if (params) {
            provider = sc_json_get_string(params, "provider");
            redirect_uri = sc_json_get_string(params, "redirect_uri");
        }
    }
    if (!provider || !provider[0]) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj)
            return SC_ERR_OUT_OF_MEMORY;
        cp_json_set_str(alloc, obj, "error", "provider is required");
        sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
        sc_json_free(alloc, obj);
        return err;
    }
    const char *ctx_provider = sc_oauth_get_provider(ctx);
    if (ctx_provider && strcmp(ctx_provider, provider) != 0) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj)
            return SC_ERR_OUT_OF_MEMORY;
        cp_json_set_str(alloc, obj, "error", "unsupported provider");
        sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
        sc_json_free(alloc, obj);
        return err;
    }
    char verifier[64];
    char challenge[64];
    char state[48];
    if (sc_oauth_generate_pkce(ctx, verifier, sizeof(verifier), challenge, sizeof(challenge)) !=
        SC_OK)
        return SC_ERR_OUT_OF_MEMORY;
    {
        static const char b64[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)(uintptr_t)ctx;
        for (int i = 0; i < 43; i++) {
            seed = seed * 1103515245u + 12345u;
            state[i] = b64[(seed >> 16) & 63];
        }
        state[43] = '\0';
    }
    proto->oauth_pending_store(proto->oauth_pending_ctx, state, verifier);
    char url[1024];
    sc_error_t err = sc_oauth_build_auth_url(ctx, challenge, strlen(challenge), state,
                                             strlen(state), url, sizeof(url));
    if (err != SC_OK) {
        proto->oauth_pending_remove(proto->oauth_pending_ctx, state);
        return err;
    }
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;
    cp_json_set_str(alloc, obj, "url", url);
    cp_json_set_str(alloc, obj, "state", state);
    (void)redirect_uri;
    err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── auth.oauth.callback ─────────────────────────────────────────────── */

sc_error_t cp_admin_oauth_callback(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                   const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                   char **out, size_t *out_len) {
    (void)app;
    (void)conn;
    sc_oauth_ctx_t *ctx = (sc_oauth_ctx_t *)proto->oauth_ctx;
    if (!ctx || !proto->oauth_pending_lookup || !proto->oauth_pending_remove) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj)
            return SC_ERR_OUT_OF_MEMORY;
        cp_json_set_str(alloc, obj, "error", "OAuth not configured");
        sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
        sc_json_free(alloc, obj);
        return err;
    }
    const char *code = NULL;
    const char *state = NULL;
    if (root) {
        sc_json_value_t *params = sc_json_object_get(root, "params");
        if (params) {
            code = sc_json_get_string(params, "code");
            state = sc_json_get_string(params, "state");
        }
    }
    if (!code || !code[0] || !state || !state[0]) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj)
            return SC_ERR_OUT_OF_MEMORY;
        cp_json_set_str(alloc, obj, "error", "code and state are required");
        sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
        sc_json_free(alloc, obj);
        return err;
    }
    const char *verifier = proto->oauth_pending_lookup(proto->oauth_pending_ctx, state);
    if (!verifier) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj)
            return SC_ERR_OUT_OF_MEMORY;
        cp_json_set_str(alloc, obj, "error", "invalid or expired state");
        sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
        sc_json_free(alloc, obj);
        return err;
    }
    sc_oauth_session_t session = {0};
    sc_error_t err =
        sc_oauth_exchange_code(ctx, code, strlen(code), verifier, strlen(verifier), &session);
    proto->oauth_pending_remove(proto->oauth_pending_ctx, state);
    if (err != SC_OK) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj)
            return SC_ERR_OUT_OF_MEMORY;
        cp_json_set_str(alloc, obj, "error", "token exchange failed");
        sc_error_t serr = sc_json_stringify(alloc, obj, out, out_len);
        sc_json_free(alloc, obj);
        return serr;
    }
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;
    cp_json_set_str(alloc, obj, "token", session.access_token);
    sc_json_value_t *user = sc_json_object_new(alloc);
    if (user) {
        cp_json_set_str(alloc, user, "id", session.user_id);
        sc_json_object_set(alloc, obj, "user", user);
    }
    err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── auth.oauth.refresh ──────────────────────────────────────────────── */

sc_error_t cp_admin_oauth_refresh(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                  const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                  char **out, size_t *out_len) {
    (void)app;
    (void)conn;
    sc_oauth_ctx_t *ctx = (sc_oauth_ctx_t *)proto->oauth_ctx;
    if (!ctx) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj)
            return SC_ERR_OUT_OF_MEMORY;
        cp_json_set_str(alloc, obj, "error", "OAuth not configured");
        sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
        sc_json_free(alloc, obj);
        return err;
    }
    const char *refresh_token = NULL;
    if (root) {
        sc_json_value_t *params = sc_json_object_get(root, "params");
        if (params)
            refresh_token = sc_json_get_string(params, "refresh_token");
    }
    if (!refresh_token || !refresh_token[0]) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj)
            return SC_ERR_OUT_OF_MEMORY;
        cp_json_set_str(alloc, obj, "error", "refresh_token is required");
        sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
        sc_json_free(alloc, obj);
        return err;
    }
    sc_oauth_session_t session = {0};
    size_t rt_len = strlen(refresh_token);
    if (rt_len >= sizeof(session.refresh_token))
        rt_len = sizeof(session.refresh_token) - 1;
    memcpy(session.refresh_token, refresh_token, rt_len);
    session.refresh_token[rt_len] = '\0';
    sc_error_t err = sc_oauth_refresh_token(ctx, &session);
    if (err != SC_OK) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj)
            return SC_ERR_OUT_OF_MEMORY;
        cp_json_set_str(alloc, obj, "error", "refresh failed");
        sc_error_t serr = sc_json_stringify(alloc, obj, out, out_len);
        sc_json_free(alloc, obj);
        return serr;
    }
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;
    cp_json_set_str(alloc, obj, "access_token", session.access_token);
    int64_t expires_in = session.expires_at - (int64_t)time(NULL);
    if (expires_in < 0)
        expires_in = 0;
    sc_json_object_set(alloc, obj, "expires_in", sc_json_number_new(alloc, (double)expires_in));
    err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

#ifdef SC_HAS_CRON
/* ── cron.list ───────────────────────────────────────────────────────── */

sc_error_t cp_admin_cron_list(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                              const sc_control_protocol_t *proto, const sc_json_value_t *root,
                              char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;

    sc_json_value_t *arr = sc_json_array_new(alloc);

    if (app && app->cron) {
        size_t count = 0;
        const sc_cron_job_t *jobs = sc_cron_list_jobs(app->cron, &count);
        for (size_t i = 0; i < count; i++) {
            sc_json_value_t *j = sc_json_object_new(alloc);
            sc_json_object_set(alloc, j, "id", sc_json_number_new(alloc, (double)jobs[i].id));
            cp_json_set_str(alloc, j, "name", jobs[i].name);
            cp_json_set_str(alloc, j, "expression", jobs[i].expression);
            cp_json_set_str(alloc, j, "command", jobs[i].command);
            sc_json_object_set(alloc, j, "enabled", sc_json_bool_new(alloc, jobs[i].enabled));
            sc_json_object_set(alloc, j, "next_run",
                               sc_json_number_new(alloc, (double)jobs[i].next_run_secs));
            sc_json_object_set(alloc, j, "last_run",
                               sc_json_number_new(alloc, (double)jobs[i].last_run_secs));
            cp_json_set_str(alloc, j, "type",
                            jobs[i].type == SC_CRON_JOB_AGENT ? "agent" : "shell");
            cp_json_set_str(alloc, j, "channel", jobs[i].channel);
            cp_json_set_str(alloc, j, "last_status", jobs[i].last_status);
            sc_json_object_set(alloc, j, "paused", sc_json_bool_new(alloc, jobs[i].paused));
            sc_json_object_set(alloc, j, "one_shot", sc_json_bool_new(alloc, jobs[i].one_shot));
            sc_json_object_set(alloc, j, "created_at",
                               sc_json_number_new(alloc, (double)jobs[i].created_at_s));
            sc_json_array_push(alloc, arr, j);
        }
    }

    sc_json_object_set(alloc, obj, "jobs", arr);
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── cron.add ────────────────────────────────────────────────────────── */

sc_error_t cp_admin_cron_add(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                             const sc_control_protocol_t *proto, const sc_json_value_t *root,
                             char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    bool added = false;
    uint64_t new_id = 0;

    if (root && app && app->cron && app->alloc) {
        sc_json_value_t *params = sc_json_object_get(root, "params");
        if (params) {
            const char *expr = sc_json_get_string(params, "expression");
            const char *cmd = sc_json_get_string(params, "command");
            const char *name = sc_json_get_string(params, "name");
            const char *type_str = sc_json_get_string(params, "type");
            const char *prompt = sc_json_get_string(params, "prompt");
            const char *channel = sc_json_get_string(params, "channel");
            bool one_shot = sc_json_get_bool(params, "one_shot", false);
            if (expr && type_str && strcmp(type_str, "agent") == 0 && prompt) {
                sc_error_t e = sc_cron_add_agent_job(app->cron, app->alloc, expr, prompt, channel,
                                                     name ? name : "Agent task", &new_id);
                added = (e == SC_OK);
                if (added && one_shot)
                    sc_cron_set_job_one_shot(app->cron, new_id, true);
            } else if (expr && cmd) {
                sc_error_t e =
                    sc_cron_add_job(app->cron, app->alloc, expr, cmd, name ? name : cmd, &new_id);
                added = (e == SC_OK);
                if (added) {
                    if (one_shot)
                        sc_cron_set_job_one_shot(app->cron, new_id, true);
                    char *cron_path = NULL;
                    size_t cron_path_len = 0;
                    if (sc_crontab_get_path(app->alloc, &cron_path, &cron_path_len) == SC_OK) {
                        char *unused_id = NULL;
                        sc_crontab_add(app->alloc, cron_path, expr, strlen(expr), cmd, strlen(cmd),
                                       &unused_id);
                        if (unused_id)
                            app->alloc->free(app->alloc->ctx, unused_id, strlen(unused_id) + 1);
                        app->alloc->free(app->alloc->ctx, cron_path, cron_path_len + 1);
                    }
                }
            }
        }
    }

    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;
    sc_json_object_set(alloc, obj, "added", sc_json_bool_new(alloc, added));
    if (added)
        sc_json_object_set(alloc, obj, "id", sc_json_number_new(alloc, (double)new_id));
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── cron.remove ─────────────────────────────────────────────────────── */

sc_error_t cp_admin_cron_remove(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    bool removed = false;

    if (root && app && app->cron) {
        sc_json_value_t *params = sc_json_object_get(root, "params");
        if (params) {
            double id_num = sc_json_get_number(params, "id", -1.0);
            if (id_num >= 0) {
                uint64_t job_id = (uint64_t)id_num;
                sc_error_t e = sc_cron_remove_job(app->cron, job_id);
                removed = (e == SC_OK);
                if (removed && app->alloc) {
                    char *cron_path = NULL;
                    size_t cron_path_len = 0;
                    if (sc_crontab_get_path(app->alloc, &cron_path, &cron_path_len) == SC_OK) {
                        char id_str[32];
                        snprintf(id_str, sizeof(id_str), "%llu", (unsigned long long)job_id);
                        sc_crontab_remove(app->alloc, cron_path, id_str);
                        app->alloc->free(app->alloc->ctx, cron_path, cron_path_len + 1);
                    }
                }
            }
        }
    }

    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;
    sc_json_object_set(alloc, obj, "removed", sc_json_bool_new(alloc, removed));
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── cron.run ────────────────────────────────────────────────────────── */

sc_error_t cp_admin_cron_run(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                             const sc_control_protocol_t *proto, const sc_json_value_t *root,
                             char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    bool started = false;
    const char *status_msg = "no cron scheduler";

    if (root && app && app->cron && app->alloc) {
        sc_json_value_t *params = sc_json_object_get(root, "params");
        if (params) {
            double id_num = sc_json_get_number(params, "id", -1.0);
            if (id_num >= 0) {
                uint64_t job_id = (uint64_t)id_num;
                const sc_cron_job_t *job = sc_cron_get_job(app->cron, job_id);
                if (job && job->command) {
                    sc_cron_add_run(app->cron, app->alloc, job_id, (int64_t)time(NULL), "running",
                                    NULL);
                    if (app->bus) {
                        sc_bus_event_t bev;
                        memset(&bev, 0, sizeof(bev));
                        bev.type = SC_BUS_CRON_STARTED;
                        snprintf(bev.channel, SC_BUS_CHANNEL_LEN, "cron");
                        snprintf(bev.id, SC_BUS_ID_LEN, "%llu", (unsigned long long)job_id);
                        snprintf(bev.message, SC_BUS_MSG_LEN, "%s", job->name ? job->name : "");
                        sc_bus_publish(app->bus, &bev);
                    }
#if !SC_IS_TEST
                    if (job->type == SC_CRON_JOB_AGENT && app->agent) {
                        char *reply = NULL;
                        size_t reply_len = 0;
                        app->agent->active_channel = job->channel ? job->channel : "gateway";
                        app->agent->active_channel_len = strlen(app->agent->active_channel);
                        app->agent->active_job_id = job_id;
                        sc_error_t run_err = sc_agent_turn(
                            app->agent, job->command, strlen(job->command), &reply, &reply_len);
                        app->agent->active_job_id = 0;
                        started = (run_err == SC_OK);
                        sc_cron_add_run(app->cron, app->alloc, job_id, (int64_t)time(NULL),
                                        started ? "completed" : "failed", reply);
                        if (reply)
                            app->alloc->free(app->alloc->ctx, reply, reply_len + 1);
                    } else {
                        const char *argv[] = {"/bin/sh", "-c", job->command, NULL};
                        sc_run_result_t run_result = {0};
                        sc_error_t run_err =
                            sc_process_run(app->alloc, argv, NULL, 65536, &run_result);
                        started = (run_err == SC_OK);
                        const char *run_output = run_result.stdout_buf;
                        sc_cron_add_run(app->cron, app->alloc, job_id, (int64_t)time(NULL),
                                        started ? "completed" : "failed", run_output);
                        sc_run_result_free(app->alloc, &run_result);
                    }
#else
                    started = true;
#endif
                    if (app->bus) {
                        sc_bus_event_t bev;
                        memset(&bev, 0, sizeof(bev));
                        bev.type = SC_BUS_CRON_COMPLETED;
                        snprintf(bev.channel, SC_BUS_CHANNEL_LEN, "cron");
                        snprintf(bev.id, SC_BUS_ID_LEN, "%llu", (unsigned long long)job_id);
                        snprintf(bev.message, SC_BUS_MSG_LEN, "%s",
                                 started ? "completed" : "failed");
                        sc_bus_publish(app->bus, &bev);
                    }
                    status_msg = started ? "completed" : "failed";
                } else {
                    status_msg = "job not found";
                }
            } else {
                status_msg = "missing job id";
            }
        }
    }

    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;
    sc_json_object_set(alloc, obj, "started", sc_json_bool_new(alloc, started));
    cp_json_set_str(alloc, obj, "status", status_msg);
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── cron.update ─────────────────────────────────────────────────────── */

sc_error_t cp_admin_cron_update(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    bool updated = false;

    if (root && app && app->cron && app->alloc) {
        sc_json_value_t *params = sc_json_object_get(root, "params");
        if (params) {
            double id_num = sc_json_get_number(params, "id", -1.0);
            if (id_num >= 0) {
                uint64_t job_id = (uint64_t)id_num;
                const char *expr = sc_json_get_string(params, "expression");
                const char *cmd = sc_json_get_string(params, "command");
                sc_json_value_t *en_val = sc_json_object_get(params, "enabled");
                bool en = true;
                bool *en_ptr = NULL;
                if (en_val && en_val->type == SC_JSON_BOOL) {
                    en = en_val->data.boolean;
                    en_ptr = &en;
                }
                sc_error_t e = sc_cron_update_job(app->cron, app->alloc, job_id, expr, cmd, en_ptr);
                updated = (e == SC_OK);
            }
        }
    }

    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;
    sc_json_object_set(alloc, obj, "updated", sc_json_bool_new(alloc, updated));
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── cron.runs ───────────────────────────────────────────────────────── */

sc_error_t cp_admin_cron_runs(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                              const sc_control_protocol_t *proto, const sc_json_value_t *root,
                              char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;
    sc_json_value_t *arr = sc_json_array_new(alloc);

    if (root && app && app->cron) {
        sc_json_value_t *params = sc_json_object_get(root, "params");
        double id_num = params ? sc_json_get_number(params, "id", -1.0) : -1.0;
        double limit_num = params ? sc_json_get_number(params, "limit", 10.0) : 10.0;
        if (id_num >= 0) {
            size_t count = 0;
            const sc_cron_run_t *runs =
                sc_cron_list_runs(app->cron, (uint64_t)id_num, (size_t)limit_num, &count);
            for (size_t i = 0; i < count; i++) {
                sc_json_value_t *r = sc_json_object_new(alloc);
                sc_json_object_set(alloc, r, "id", sc_json_number_new(alloc, (double)runs[i].id));
                sc_json_object_set(alloc, r, "started_at",
                                   sc_json_number_new(alloc, (double)runs[i].started_at_s));
                sc_json_object_set(alloc, r, "finished_at",
                                   sc_json_number_new(alloc, (double)runs[i].finished_at_s));
                cp_json_set_str(alloc, r, "status", runs[i].status);
                sc_json_array_push(alloc, arr, r);
            }
        }
    }

    sc_json_object_set(alloc, obj, "runs", arr);
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

#endif /* SC_HAS_CRON */

#ifdef SC_HAS_SKILLS
/* ── skills.list ─────────────────────────────────────────────────────── */

sc_error_t cp_admin_skills_list(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;

    sc_json_value_t *arr = sc_json_array_new(alloc);

    if (app && app->skills) {
        sc_skill_t *skills = NULL;
        size_t count = 0;
        sc_error_t e = sc_skillforge_list_skills(app->skills, &skills, &count);
        if (e == SC_OK && skills) {
            for (size_t i = 0; i < count; i++) {
                sc_json_value_t *s = sc_json_object_new(alloc);
                cp_json_set_str(alloc, s, "name", skills[i].name);
                cp_json_set_str(alloc, s, "description", skills[i].description);
                sc_json_object_set(alloc, s, "enabled", sc_json_bool_new(alloc, skills[i].enabled));
                sc_json_array_push(alloc, arr, s);
            }
        }
    }

    sc_json_object_set(alloc, obj, "skills", arr);
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── skills.enable / skills.disable ──────────────────────────────────── */

static sc_error_t cp_admin_skill_toggle(sc_allocator_t *alloc, sc_app_context_t *app,
                                        const sc_json_value_t *root, bool enable, char **out,
                                        size_t *out_len) {
    bool success = false;

    if (root && app && app->skills) {
        sc_json_value_t *params = sc_json_object_get(root, "params");
        if (params) {
            const char *name = sc_json_get_string(params, "name");
            if (name) {
                sc_error_t e = enable ? sc_skillforge_enable(app->skills, name)
                                      : sc_skillforge_disable(app->skills, name);
                success = (e == SC_OK);
            }
        }
    }

    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;
    sc_json_object_set(alloc, obj, enable ? "enabled" : "disabled",
                       sc_json_bool_new(alloc, success));
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

sc_error_t cp_admin_skills_enable(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                  const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                  char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    return cp_admin_skill_toggle(alloc, app, root, true, out, out_len);
}

sc_error_t cp_admin_skills_disable(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                   const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                   char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    return cp_admin_skill_toggle(alloc, app, root, false, out, out_len);
}

/* ── skills.install ──────────────────────────────────────────────────── */

sc_error_t cp_admin_skills_install(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                   const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                   char **out, size_t *out_len) {
    (void)app;
    (void)conn;
    (void)proto;
    bool installed = false;
    const char *error_msg = NULL;

    if (root) {
        sc_json_value_t *params = sc_json_object_get(root, "params");
        if (params) {
            const char *name = sc_json_get_string(params, "name");
            const char *url = sc_json_get_string(params, "url");
            if (name && name[0]) {
                sc_error_t e = sc_skillforge_install(name, url);
                if (e == SC_OK) {
                    installed = true;
                } else {
                    error_msg = sc_error_string(e);
                }
            } else {
                error_msg = "name is required";
            }
        }
    }

    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;
    sc_json_object_set(alloc, obj, "installed", sc_json_bool_new(alloc, installed));
    if (error_msg)
        cp_json_set_str(alloc, obj, "error", error_msg);
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── skills.search ───────────────────────────────────────────────────── */

sc_error_t cp_admin_skills_search(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                  const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                  char **out, size_t *out_len) {
    (void)app;
    (void)conn;
    (void)proto;
    const char *query = NULL;
    if (root) {
        sc_json_value_t *params = sc_json_object_get(root, "params");
        if (params)
            query = sc_json_get_string(params, "query");
    }

    sc_skill_registry_entry_t *entries = NULL;
    size_t count = 0;
    sc_error_t e = sc_skill_registry_search(alloc, query, &entries, &count);

    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj) {
        if (entries)
            sc_skill_registry_entries_free(alloc, entries, count);
        return SC_ERR_OUT_OF_MEMORY;
    }

    sc_json_value_t *arr = sc_json_array_new(alloc);
    if (e == SC_OK && entries) {
        for (size_t i = 0; i < count; i++) {
            sc_json_value_t *entry = sc_json_object_new(alloc);
            cp_json_set_str(alloc, entry, "name", entries[i].name);
            if (entries[i].description)
                cp_json_set_str(alloc, entry, "description", entries[i].description);
            if (entries[i].version)
                cp_json_set_str(alloc, entry, "version", entries[i].version);
            if (entries[i].author)
                cp_json_set_str(alloc, entry, "author", entries[i].author);
            if (entries[i].url)
                cp_json_set_str(alloc, entry, "url", entries[i].url);
            if (entries[i].tags)
                cp_json_set_str(alloc, entry, "tags", entries[i].tags);
            sc_json_array_push(alloc, arr, entry);
        }
        sc_skill_registry_entries_free(alloc, entries, count);
    }

    sc_json_object_set(alloc, obj, "entries", arr);
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── skills.uninstall ────────────────────────────────────────────────── */

sc_error_t cp_admin_skills_uninstall(sc_allocator_t *alloc, sc_app_context_t *app,
                                     sc_ws_conn_t *conn, const sc_control_protocol_t *proto,
                                     const sc_json_value_t *root, char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    bool success = false;

    if (root && app && app->skills) {
        sc_json_value_t *params = sc_json_object_get(root, "params");
        if (params) {
            const char *name = sc_json_get_string(params, "name");
            if (name) {
                sc_error_t e = sc_skillforge_uninstall(app->skills, name);
                success = (e == SC_OK);
            }
        }
    }

    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;
    sc_json_object_set(alloc, obj, "uninstalled", sc_json_bool_new(alloc, success));
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── skills.update ───────────────────────────────────────────────────── */

sc_error_t cp_admin_skills_update(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                  const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                  char **out, size_t *out_len) {
    (void)app;
    (void)conn;
    (void)proto;
    (void)root;
    sc_error_t e = sc_skill_registry_update(alloc);
    bool success = (e == SC_OK);

    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;
    sc_json_object_set(alloc, obj, "updated", sc_json_bool_new(alloc, success));
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

#endif /* SC_HAS_SKILLS */

#ifdef SC_HAS_UPDATE
/* ── update.check ────────────────────────────────────────────────────── */

sc_error_t cp_admin_update_check(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                 const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                 char **out, size_t *out_len) {
    (void)app;
    (void)conn;
    (void)proto;
    (void)root;
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;
    const char *current = sc_version_string();
    cp_json_set_str(alloc, obj, "current", current);
    char latest[64] = {0};
    sc_error_t check_err = sc_update_check(latest, sizeof(latest));
    if (check_err == SC_OK && latest[0]) {
        cp_json_set_str(alloc, obj, "latest", latest);
        sc_json_object_set(alloc, obj, "available",
                           sc_json_bool_new(alloc, strcmp(latest, current) != 0));
    } else {
        cp_json_set_str(alloc, obj, "latest", current);
        sc_json_object_set(alloc, obj, "available", sc_json_bool_new(alloc, false));
        if (check_err != SC_OK)
            cp_json_set_str(alloc, obj, "error", sc_error_string(check_err));
    }
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── update.run ──────────────────────────────────────────────────────── */

sc_error_t cp_admin_update_run(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                               const sc_control_protocol_t *proto, const sc_json_value_t *root,
                               char **out, size_t *out_len) {
    (void)app;
    (void)conn;
    (void)proto;
    (void)root;
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;
    sc_error_t apply_err = sc_update_apply();
    if (apply_err == SC_OK)
        cp_json_set_str(alloc, obj, "status", "updated");
    else {
        cp_json_set_str(alloc, obj, "status", sc_error_string(apply_err));
        cp_json_set_str(alloc, obj, "error", sc_error_string(apply_err));
    }
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

#endif /* SC_HAS_UPDATE */

#ifdef SC_HAS_PUSH
/* ── push.register ──────────────────────────────────────────────────── */

sc_error_t cp_admin_push_register(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                  const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                  char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    bool registered = false;
    const char *error_msg = NULL;

    if (root && app && app->push) {
        sc_json_value_t *params = sc_json_object_get(root, "params");
        if (params) {
            const char *token = sc_json_get_string(params, "token");
            const char *provider_str = sc_json_get_string(params, "provider");
            if (token && token[0]) {
                sc_push_provider_t provider = SC_PUSH_NONE;
                if (provider_str) {
                    if (strcmp(provider_str, "fcm") == 0)
                        provider = SC_PUSH_FCM;
                    else if (strcmp(provider_str, "apns") == 0)
                        provider = SC_PUSH_APNS;
                }
                if (provider != SC_PUSH_NONE) {
                    sc_error_t e = sc_push_register_token(app->push, token, provider);
                    registered = (e == SC_OK);
                    if (!registered)
                        error_msg = sc_error_string(e);
                } else {
                    error_msg = "provider must be 'fcm' or 'apns'";
                }
            } else {
                error_msg = "token is required";
            }
        }
    } else if (!app || !app->push) {
        error_msg = "push not configured";
    }

    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;
    sc_json_object_set(alloc, obj, "registered", sc_json_bool_new(alloc, registered));
    if (error_msg)
        cp_json_set_str(alloc, obj, "error", error_msg);
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── push.unregister ────────────────────────────────────────────────── */

sc_error_t cp_admin_push_unregister(sc_allocator_t *alloc, sc_app_context_t *app,
                                    sc_ws_conn_t *conn, const sc_control_protocol_t *proto,
                                    const sc_json_value_t *root, char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    bool unregistered = false;
    const char *error_msg = NULL;

    if (root && app && app->push) {
        sc_json_value_t *params = sc_json_object_get(root, "params");
        if (params) {
            const char *token = sc_json_get_string(params, "token");
            if (token && token[0]) {
                sc_error_t e = sc_push_unregister_token(app->push, token);
                unregistered = (e == SC_OK);
                if (!unregistered)
                    error_msg = sc_error_string(e);
            } else {
                error_msg = "token is required";
            }
        }
    } else if (!app || !app->push) {
        error_msg = "push not configured";
    }

    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;
    sc_json_object_set(alloc, obj, "unregistered", sc_json_bool_new(alloc, unregistered));
    if (error_msg)
        cp_json_set_str(alloc, obj, "error", error_msg);
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

#endif /* SC_HAS_PUSH */

#endif /* SC_GATEWAY_POSIX */
