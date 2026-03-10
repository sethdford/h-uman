/* Admin/system control protocol handlers: connect, health, sessions, tools, channels,
 * cron, skills, models, nodes, usage, activity, push, auth, etc. */
#include "cp_internal.h"
#include "human/agent.h"
#include "human/agent/awareness.h"
#include "human/bus.h"
#include "human/channel_catalog.h"
#include "human/config.h"
#include "human/core/process_util.h"
#include "human/core/string.h"
#include "human/cost.h"
#include "human/gateway/oauth.h"
#include "human/health.h"
#include "human/observability/bth_metrics.h"
#include "human/observability/metrics_observer.h"
#include "human/security.h"
#include "human/session.h"
#include "human/tool.h"
#include "human/version.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef HU_GATEWAY_POSIX
#include <pthread.h>
#endif

#ifdef HU_HAS_CRON
#include "human/cron.h"
#include "human/crontab.h"
#endif
#ifdef HU_HAS_PUSH
#include "human/gateway/push.h"
#endif
#ifdef HU_HAS_SKILLS
#include "human/skill_registry.h"
#include "human/skillforge.h"
#endif
#ifdef HU_HAS_UPDATE
#include "human/update.h"
#endif

#ifdef HU_GATEWAY_POSIX

/* ── connect ─────────────────────────────────────────────────────────── */

hu_error_t cp_admin_connect(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                            const hu_control_protocol_t *proto, const hu_json_value_t *root,
                            char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;

    cp_json_set_str(alloc, obj, "type", "hello-ok");

    hu_json_value_t *server = hu_json_object_new(alloc);
    if (!server) {
        hu_json_free(alloc, obj);
        return HU_ERR_OUT_OF_MEMORY;
    }
    cp_json_set_str(alloc, server, "version", hu_version_string());
    hu_json_object_set(alloc, obj, "server", server);
    hu_json_object_set(alloc, obj, "protocol", hu_json_number_new(alloc, 1));

    hu_json_value_t *features = hu_json_object_new(alloc);
    if (!features) {
        hu_json_free(alloc, obj);
        return HU_ERR_OUT_OF_MEMORY;
    }
    hu_json_value_t *methods_arr = hu_json_array_new(alloc);
    if (!methods_arr) {
        hu_json_free(alloc, obj);
        return HU_ERR_OUT_OF_MEMORY;
    }

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
        hu_json_value_t *m = hu_json_string_new(alloc, methods[i], strlen(methods[i]));
        if (!m) {
            hu_json_free(alloc, obj);
            return HU_ERR_OUT_OF_MEMORY;
        }
        hu_json_array_push(alloc, methods_arr, m);
    }
    hu_json_object_set(alloc, features, "methods", methods_arr);

    if (app) {
        hu_json_object_set(alloc, features, "sessions",
                           hu_json_bool_new(alloc, app->sessions != NULL));
        hu_json_object_set(alloc, features, "cron", hu_json_bool_new(alloc, app->cron != NULL));
        hu_json_object_set(alloc, features, "skills", hu_json_bool_new(alloc, app->skills != NULL));
        hu_json_object_set(alloc, features, "cost_tracking",
                           hu_json_bool_new(alloc, app->costs != NULL));
    }

    hu_json_object_set(alloc, obj, "features", features);

    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

/* ── health ──────────────────────────────────────────────────────────── */

hu_error_t cp_admin_health(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                           const hu_control_protocol_t *proto, const hu_json_value_t *root,
                           char **out, size_t *out_len) {
    (void)app;
    (void)conn;
    (void)proto;
    (void)root;
    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;
    cp_json_set_str(alloc, obj, "status", "ok");
    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

/* ── capabilities ────────────────────────────────────────────────────── */

hu_error_t cp_admin_capabilities(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                 const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                 char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;

    cp_json_set_str(alloc, obj, "version", hu_version_string());

    size_t tool_count = app ? app->tools_count : 0;
    hu_json_object_set(alloc, obj, "tools", hu_json_number_new(alloc, (double)tool_count));

    size_t ch_count = 0;
    if (app && app->config) {
        size_t total = 0;
        const hu_channel_meta_t *catalog = hu_channel_catalog_all(&total);
        size_t configured = 0;
        for (size_t i = 0; i < total; i++) {
            if (hu_channel_catalog_is_configured(app->config, catalog[i].id))
                configured++;
        }
        ch_count = configured;
    }
    hu_json_object_set(alloc, obj, "channels", hu_json_number_new(alloc, (double)ch_count));

    size_t prov_count = (app && app->config) ? app->config->providers_len : 0;
    hu_json_object_set(alloc, obj, "providers", hu_json_number_new(alloc, (double)prov_count));

    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

/* ── sessions.list ───────────────────────────────────────────────────── */

hu_error_t cp_admin_sessions_list(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                  const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                  char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;

    hu_json_value_t *arr = hu_json_array_new(alloc);

    if (app && app->sessions) {
        size_t count = 0;
        hu_session_summary_t *summaries = hu_session_list(app->sessions, alloc, &count);
        if (summaries) {
            for (size_t i = 0; i < count; i++) {
                hu_json_value_t *s = hu_json_object_new(alloc);
                cp_json_set_str(alloc, s, "key", summaries[i].session_key);
                cp_json_set_str(alloc, s, "label", summaries[i].label);
                hu_json_object_set(alloc, s, "created_at",
                                   hu_json_number_new(alloc, (double)summaries[i].created_at));
                hu_json_object_set(alloc, s, "last_active",
                                   hu_json_number_new(alloc, (double)summaries[i].last_active));
                hu_json_object_set(alloc, s, "turn_count",
                                   hu_json_number_new(alloc, (double)summaries[i].turn_count));
                hu_json_array_push(alloc, arr, s);
            }
            alloc->free(alloc->ctx, summaries, count * sizeof(hu_session_summary_t));
        }
    }

    hu_json_object_set(alloc, obj, "sessions", arr);
    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

/* ── sessions.patch ──────────────────────────────────────────────────── */

hu_error_t cp_admin_sessions_patch(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                   const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                   char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    bool patched = false;

    if (root && app && app->sessions) {
        hu_json_value_t *params = hu_json_object_get(root, "params");
        if (params) {
            const char *key = hu_json_get_string(params, "key");
            const char *label = hu_json_get_string(params, "label");
            if (key && label) {
                hu_error_t e = hu_session_patch(app->sessions, key, label);
                patched = (e == HU_OK);
            }
        }
    }

    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;
    hu_json_object_set(alloc, obj, "patched", hu_json_bool_new(alloc, patched));
    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

/* ── sessions.delete ─────────────────────────────────────────────────── */

hu_error_t cp_admin_sessions_delete(hu_allocator_t *alloc, hu_app_context_t *app,
                                    hu_ws_conn_t *conn, const hu_control_protocol_t *proto,
                                    const hu_json_value_t *root, char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    bool deleted = false;

    if (root && app && app->sessions) {
        hu_json_value_t *params = hu_json_object_get(root, "params");
        if (params) {
            const char *key = hu_json_get_string(params, "key");
            if (key) {
                hu_error_t e = hu_session_delete(app->sessions, key);
                deleted = (e == HU_OK);
            }
        }
    }

    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;
    hu_json_object_set(alloc, obj, "deleted", hu_json_bool_new(alloc, deleted));
    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

/* ── persona.set ─────────────────────────────────────────────────────── */

hu_error_t cp_admin_persona_set(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;

    if (!app || !app->agent) {
        cp_json_set_str(alloc, obj, "error", "no agent (gateway must run with --with-agent)");
        hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
        hu_json_free(alloc, obj);
        return err;
    }

    if (!root) {
        cp_json_set_str(alloc, obj, "error", "params required");
        hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
        hu_json_free(alloc, obj);
        return err;
    }

    hu_json_value_t *params = hu_json_object_get(root, "params");
    hu_json_value_t *name_val = params ? hu_json_object_get(params, "name") : NULL;

    if (!name_val) {
        cp_json_set_str(alloc, obj, "error", "name is required");
        hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
        hu_json_free(alloc, obj);
        return err;
    }

    const char *name = NULL;
    size_t name_len = 0;
    if (name_val->type == HU_JSON_NULL) {
        /* {"name": null} — clear persona */
    } else if (name_val->type == HU_JSON_STRING) {
        name = name_val->data.string.ptr;
        name_len = name_val->data.string.len;
    } else {
        cp_json_set_str(alloc, obj, "error", "name must be string or null");
        hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
        hu_json_free(alloc, obj);
        return err;
    }

#ifdef HU_HAS_PERSONA
    hu_error_t err = hu_agent_set_persona(app->agent, name, name_len);
    if (err != HU_OK) {
        const char *emsg = hu_error_string(err);
        cp_json_set_str(alloc, obj, "error", emsg ? emsg : "failed to set persona");
        hu_error_t serr = hu_json_stringify(alloc, obj, out, out_len);
        hu_json_free(alloc, obj);
        return serr;
    }
#else
    (void)name;
    (void)name_len;
    cp_json_set_str(alloc, obj, "error", "persona support not built (HU_ENABLE_PERSONA=OFF)");
    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
#endif

    hu_json_object_set(alloc, obj, "ok", hu_json_bool_new(alloc, true));
    hu_error_t serr = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return serr;
}

/* ── tools.catalog ───────────────────────────────────────────────────── */

hu_error_t cp_admin_tools_catalog(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                  const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                  char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;

    hu_json_value_t *arr = hu_json_array_new(alloc);

    if (app && app->tools) {
        for (size_t i = 0; i < app->tools_count; i++) {
            hu_tool_t *t = &app->tools[i];
            if (!t->vtable)
                continue;
            hu_json_value_t *tool_obj = hu_json_object_new(alloc);

            const char *name = t->vtable->name ? t->vtable->name(t->ctx) : "";
            const char *desc = t->vtable->description ? t->vtable->description(t->ctx) : "";

            cp_json_set_str(alloc, tool_obj, "name", name);
            cp_json_set_str(alloc, tool_obj, "description", desc);

            const char *params =
                t->vtable->parameters_json ? t->vtable->parameters_json(t->ctx) : NULL;
            if (params) {
                hu_json_value_t *parsed_params = NULL;
                if (hu_json_parse(alloc, params, strlen(params), &parsed_params) == HU_OK &&
                    parsed_params) {
                    hu_json_object_set(alloc, tool_obj, "parameters", parsed_params);
                } else {
                    cp_json_set_str(alloc, tool_obj, "parameters", params);
                }
            }

            hu_json_array_push(alloc, arr, tool_obj);
        }
    }

    hu_json_object_set(alloc, obj, "tools", arr);
    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

/* ── channels.status ─────────────────────────────────────────────────── */

hu_error_t cp_admin_channels_status(hu_allocator_t *alloc, hu_app_context_t *app,
                                    hu_ws_conn_t *conn, const hu_control_protocol_t *proto,
                                    const hu_json_value_t *root, char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;

    hu_json_value_t *arr = hu_json_array_new(alloc);

    size_t catalog_count = 0;
    const hu_channel_meta_t *catalog = hu_channel_catalog_all(&catalog_count);
    for (size_t i = 0; i < catalog_count; i++) {
        hu_json_value_t *ch_obj = hu_json_object_new(alloc);
        cp_json_set_str(alloc, ch_obj, "key", catalog[i].key);
        cp_json_set_str(alloc, ch_obj, "label", catalog[i].label);

        bool build_enabled = hu_channel_catalog_is_build_enabled(catalog[i].id);
        hu_json_object_set(alloc, ch_obj, "build_enabled", hu_json_bool_new(alloc, build_enabled));

        bool configured = false;
        if (app && app->config) {
            configured = hu_channel_catalog_is_configured(app->config, catalog[i].id);
        }
        hu_json_object_set(alloc, ch_obj, "configured", hu_json_bool_new(alloc, configured));

        char status_buf[64];
        const char *status = "unavailable";
        if (app && app->config) {
            status = hu_channel_catalog_status_text(app->config, &catalog[i], status_buf,
                                                    sizeof(status_buf));
        }
        cp_json_set_str(alloc, ch_obj, "status", status);

        hu_json_array_push(alloc, arr, ch_obj);
    }

    hu_json_object_set(alloc, obj, "channels", arr);
    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

/* ── models.list ─────────────────────────────────────────────────────── */

hu_error_t cp_admin_models_list(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;

    hu_json_value_t *arr = hu_json_array_new(alloc);

    if (app && app->config && app->config->providers) {
        for (size_t i = 0; i < app->config->providers_len; i++) {
            hu_provider_entry_t *pe = &app->config->providers[i];
            hu_json_value_t *p = hu_json_object_new(alloc);
            cp_json_set_str(alloc, p, "name", pe->name);
            hu_json_object_set(alloc, p, "has_key",
                               hu_json_bool_new(alloc, pe->api_key && pe->api_key[0]));
            cp_json_set_str(alloc, p, "base_url", pe->base_url);
            hu_json_object_set(alloc, p, "native_tools", hu_json_bool_new(alloc, pe->native_tools));
            hu_json_object_set(
                alloc, p, "is_default",
                hu_json_bool_new(alloc, app->config->default_provider &&
                                            strcmp(pe->name, app->config->default_provider) == 0));
            hu_json_array_push(alloc, arr, p);
        }
    }

    cp_json_set_str(alloc, obj, "default_model",
                    (app && app->config) ? app->config->default_model : "");
    hu_json_object_set(alloc, obj, "providers", arr);
    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

/* ── nodes.list ──────────────────────────────────────────────────────── */

hu_error_t cp_admin_nodes_list(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                               const hu_control_protocol_t *proto, const hu_json_value_t *root,
                               char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;

    hu_json_value_t *arr = hu_json_array_new(alloc);

    size_t node_count = 0;
    const char *default_id = "local";
    const char *default_status = "online";

    if (app && app->config && app->config->nodes_len > 0) {
        node_count = app->config->nodes_len;
        for (size_t i = 0; i < node_count; i++) {
            hu_json_value_t *n = hu_json_object_new(alloc);
            const char *name = app->config->nodes[i].name ? app->config->nodes[i].name : default_id;
            const char *status =
                app->config->nodes[i].status ? app->config->nodes[i].status : default_status;
            cp_json_set_str(alloc, n, "id", name);
            cp_json_set_str(alloc, n, "type", "gateway");
            cp_json_set_str(alloc, n, "status", status);
            if (strcmp(name, "local") == 0) {
                cp_json_set_str(alloc, n, "version", hu_version_string());
#if !defined(HU_IS_TEST)
                {
                    char hostname[256] = {0};
                    if (gethostname(hostname, sizeof(hostname) - 1) == 0)
                        cp_json_set_str(alloc, n, "hostname", hostname);
#if defined(__APPLE__) || defined(__linux__)
                    struct timespec ts;
                    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
                        hu_json_object_set(alloc, n, "uptime_secs",
                                           hu_json_number_new(alloc, (double)ts.tv_sec));
#endif
                }
#endif
                hu_json_object_set(alloc, n, "ws_connections", hu_json_number_new(alloc, 1.0));
            } else {
                hu_json_object_set(alloc, n, "ws_connections", hu_json_number_new(alloc, 0.0));
            }
            hu_json_array_push(alloc, arr, n);
        }
    } else {
        /* Backward compatibility: no config or no nodes configured → default local node */
        hu_json_value_t *local = hu_json_object_new(alloc);
        cp_json_set_str(alloc, local, "id", default_id);
        cp_json_set_str(alloc, local, "type", "gateway");
        cp_json_set_str(alloc, local, "status", default_status);
        cp_json_set_str(alloc, local, "version", hu_version_string());
#if !defined(HU_IS_TEST)
        {
            char hostname[256] = {0};
            if (gethostname(hostname, sizeof(hostname) - 1) == 0)
                cp_json_set_str(alloc, local, "hostname", hostname);
#if defined(__APPLE__) || defined(__linux__)
            struct timespec ts;
            if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
                hu_json_object_set(alloc, local, "uptime_secs",
                                   hu_json_number_new(alloc, (double)ts.tv_sec));
#endif
        }
#endif
        hu_json_object_set(alloc, local, "ws_connections",
                           hu_json_number_new(alloc, (app && app->config) ? 1.0 : 0.0));
        hu_json_array_push(alloc, arr, local);
    }

    hu_json_object_set(alloc, obj, "nodes", arr);
    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

/* ── usage.summary ───────────────────────────────────────────────────── */

hu_error_t cp_admin_usage_summary(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                  const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                  char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;

    if (app && app->costs) {
        hu_cost_summary_t summary;
        int64_t now = (int64_t)time(NULL);
        hu_cost_get_summary(app->costs, now, &summary);

        hu_json_object_set(alloc, obj, "session_cost_usd",
                           hu_json_number_new(alloc, summary.session_cost_usd));
        hu_json_object_set(alloc, obj, "daily_cost_usd",
                           hu_json_number_new(alloc, summary.daily_cost_usd));
        hu_json_object_set(alloc, obj, "monthly_cost_usd",
                           hu_json_number_new(alloc, summary.monthly_cost_usd));
        hu_json_object_set(alloc, obj, "total_tokens",
                           hu_json_number_new(alloc, (double)summary.total_tokens));
        hu_json_object_set(alloc, obj, "request_count",
                           hu_json_number_new(alloc, (double)summary.request_count));
    } else {
        hu_json_object_set(alloc, obj, "session_cost_usd", hu_json_number_new(alloc, 0));
        hu_json_object_set(alloc, obj, "daily_cost_usd", hu_json_number_new(alloc, 0));
        hu_json_object_set(alloc, obj, "monthly_cost_usd", hu_json_number_new(alloc, 0));
        hu_json_object_set(alloc, obj, "total_tokens", hu_json_number_new(alloc, 0));
        hu_json_object_set(alloc, obj, "request_count", hu_json_number_new(alloc, 0));
    }

    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

/* ── metrics.snapshot ─────────────────────────────────────────────────── */

hu_error_t cp_admin_metrics_snapshot(hu_allocator_t *alloc, hu_app_context_t *app,
                                     hu_ws_conn_t *conn, const hu_control_protocol_t *proto,
                                     const hu_json_value_t *root, char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;

    /* health: uptime_seconds, pid */
    hu_health_snapshot_t health;
    hu_health_snapshot(&health);
    hu_json_value_t *health_obj = hu_json_object_new(alloc);
    if (health_obj) {
        hu_json_object_set(alloc, health_obj, "uptime_seconds",
                           hu_json_number_new(alloc, (double)health.uptime_seconds));
        hu_json_object_set(alloc, health_obj, "pid", hu_json_number_new(alloc, (double)health.pid));
        hu_json_object_set(alloc, obj, "health", health_obj);
    }
    if (health.components)
        free(health.components);

    /* metrics: total_requests, total_tokens, total_tool_calls, total_errors,
     * avg_latency_ms, active_sessions */
    hu_metrics_snapshot_t metrics = {0};
    if (app && app->agent && app->agent->observer)
        hu_metrics_observer_snapshot(*app->agent->observer, &metrics);
    hu_json_value_t *metrics_obj = hu_json_object_new(alloc);
    if (metrics_obj) {
        hu_json_object_set(alloc, metrics_obj, "total_requests",
                           hu_json_number_new(alloc, (double)metrics.total_requests));
        hu_json_object_set(alloc, metrics_obj, "total_tokens",
                           hu_json_number_new(alloc, (double)metrics.total_tokens));
        hu_json_object_set(alloc, metrics_obj, "total_tool_calls",
                           hu_json_number_new(alloc, (double)metrics.total_tool_calls));
        hu_json_object_set(alloc, metrics_obj, "total_errors",
                           hu_json_number_new(alloc, (double)metrics.total_errors));
        hu_json_object_set(alloc, metrics_obj, "avg_latency_ms",
                           hu_json_number_new(alloc, metrics.avg_latency_ms));
        hu_json_object_set(alloc, metrics_obj, "active_sessions",
                           hu_json_number_new(alloc, (double)metrics.active_sessions));
        hu_json_object_set(alloc, obj, "metrics", metrics_obj);
    }

    /* bth: all 22 BTH counters */
    hu_json_value_t *bth_obj = hu_json_object_new(alloc);
    if (bth_obj) {
        const hu_bth_metrics_t *m =
            (app && app->agent && app->agent->bth_metrics) ? app->agent->bth_metrics : NULL;
        uint32_t v;
#define BTH_SET(field)    \
    v = m ? m->field : 0; \
    hu_json_object_set(alloc, bth_obj, #field, hu_json_number_new(alloc, (double)v))
        BTH_SET(emotions_surfaced);
        BTH_SET(facts_extracted);
        BTH_SET(commitment_followups);
        BTH_SET(pattern_insights);
        BTH_SET(emotions_promoted);
        BTH_SET(events_extracted);
        BTH_SET(mood_contexts_built);
        BTH_SET(silence_checkins);
        BTH_SET(event_followups);
        BTH_SET(starters_built);
        BTH_SET(typos_applied);
        BTH_SET(corrections_sent);
        BTH_SET(thinking_responses);
        BTH_SET(callbacks_triggered);
        BTH_SET(reactions_sent);
        BTH_SET(link_contexts);
        BTH_SET(attachment_contexts);
        BTH_SET(ab_evaluations);
        BTH_SET(ab_alternates_chosen);
        BTH_SET(replay_analyses);
        BTH_SET(egraph_contexts);
        BTH_SET(vision_descriptions);
        BTH_SET(total_turns);
#undef BTH_SET
        hu_json_object_set(alloc, obj, "bth", bth_obj);
    }

    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

/* ── activity.recent ─────────────────────────────────────────────────── */

hu_error_t cp_admin_activity_recent(hu_allocator_t *alloc, hu_app_context_t *app,
                                    hu_ws_conn_t *conn, const hu_control_protocol_t *proto,
                                    const hu_json_value_t *root, char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;

    hu_json_value_t *events_arr = hu_json_array_new(alloc);
    if (!events_arr) {
        hu_json_free(alloc, obj);
        return HU_ERR_OUT_OF_MEMORY;
    }

    int64_t now_ms = (int64_t)time(NULL) * 1000;

    if (app && app->agent) {
        const hu_awareness_t *aw = hu_agent_get_awareness(app->agent);
        if (aw) {
            const hu_awareness_state_t *s = &aw->state;

            if (s->total_errors > 0) {
                size_t nerr = s->total_errors < HU_AWARENESS_MAX_RECENT_ERRORS
                                  ? s->total_errors
                                  : HU_AWARENESS_MAX_RECENT_ERRORS;
                for (size_t i = 0; i < nerr; i++) {
                    size_t idx = (s->error_write_idx + HU_AWARENESS_MAX_RECENT_ERRORS - nerr + i) %
                                 HU_AWARENESS_MAX_RECENT_ERRORS;
                    hu_json_value_t *ev = hu_json_object_new(alloc);
                    if (!ev)
                        continue;
                    cp_json_set_str(alloc, ev, "type", "error");
                    cp_json_set_str(alloc, ev, "message", s->recent_errors[idx].text);
                    int64_t ts = s->recent_errors[idx].timestamp_ms > 0
                                     ? (int64_t)s->recent_errors[idx].timestamp_ms
                                     : now_ms;
                    hu_json_object_set(alloc, ev, "time", hu_json_number_new(alloc, (double)ts));
                    hu_json_array_push(alloc, events_arr, ev);
                }
            }

            if (s->messages_received > 0) {
                hu_json_value_t *ev = hu_json_object_new(alloc);
                if (ev) {
                    cp_json_set_str(alloc, ev, "type", "system");
                    char msg[128];
                    snprintf(msg, sizeof(msg), "%llu messages received, %llu tool calls",
                             (unsigned long long)s->messages_received,
                             (unsigned long long)s->tool_calls);
                    cp_json_set_str(alloc, ev, "message", msg);
                    hu_json_object_set(alloc, ev, "time",
                                       hu_json_number_new(alloc, (double)now_ms));
                    hu_json_array_push(alloc, events_arr, ev);
                }
            }

            if (s->health_degraded) {
                hu_json_value_t *ev = hu_json_object_new(alloc);
                if (ev) {
                    cp_json_set_str(alloc, ev, "type", "error");
                    cp_json_set_str(alloc, ev, "message", "System health is degraded");
                    hu_json_object_set(alloc, ev, "time",
                                       hu_json_number_new(alloc, (double)now_ms));
                    hu_json_array_push(alloc, events_arr, ev);
                }
            }
        }
    }

    hu_json_object_set(alloc, obj, "events", events_arr);
    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

/* ── exec.approval.resolve ───────────────────────────────────────────── */

hu_error_t cp_admin_exec_approval(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                  const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                  char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    bool resolved = false;
    bool approved = false;

    if (root && app && app->bus) {
        hu_json_value_t *params = hu_json_object_get(root, "params");
        if (params) {
            const char *id = hu_json_get_string(params, "id");
            approved = hu_json_get_bool(params, "approved", false);
            if (id && id[0]) {
                hu_bus_publish_simple(app->bus, approved ? HU_BUS_TOOL_CALL : HU_BUS_ERROR,
                                      "approval", id, approved ? "approved" : "denied");
                resolved = true;
            }
        }
    }

    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;
    hu_json_object_set(alloc, obj, "resolved", hu_json_bool_new(alloc, resolved));
    hu_json_object_set(alloc, obj, "approved", hu_json_bool_new(alloc, approved));
    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

/* ── auth.token ──────────────────────────────────────────────────────── */

hu_error_t cp_admin_auth_token(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                               const hu_control_protocol_t *proto, const hu_json_value_t *root,
                               char **out, size_t *out_len) {
    (void)app;
    const char *token = NULL;
    if (root) {
        hu_json_value_t *params = hu_json_object_get(root, "params");
        if (params)
            token = hu_json_get_string(params, "token");
    }
    if (!token || !token[0]) {
        hu_json_value_t *obj = hu_json_object_new(alloc);
        if (!obj)
            return HU_ERR_OUT_OF_MEMORY;
        cp_json_set_str(alloc, obj, "error", "missing token");
        hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
        hu_json_free(alloc, obj);
        return err;
    }
    bool valid = false;
    if (proto->pairing_guard && hu_pairing_guard_is_authenticated(proto->pairing_guard, token))
        valid = true;
    else if (proto->auth_token && proto->auth_token[0] &&
             hu_pairing_guard_constant_time_eq(token, proto->auth_token))
        valid = true;
    if (valid) {
        conn->authenticated = true;
        hu_json_value_t *obj = hu_json_object_new(alloc);
        if (!obj)
            return HU_ERR_OUT_OF_MEMORY;
        cp_json_set_str(alloc, obj, "result", "authenticated");
        hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
        hu_json_free(alloc, obj);
        return err;
    }
    (void)alloc;
    (void)out;
    (void)out_len;
    return HU_ERR_GATEWAY_AUTH;
}

/* ── auth.oauth.start ────────────────────────────────────────────────── */

hu_error_t cp_admin_oauth_start(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                char **out, size_t *out_len) {
    (void)app;
    (void)conn;
    hu_oauth_ctx_t *ctx = (hu_oauth_ctx_t *)proto->oauth_ctx;
    if (!ctx || !proto->oauth_pending_store || !proto->oauth_pending_lookup ||
        !proto->oauth_pending_remove) {
        hu_json_value_t *obj = hu_json_object_new(alloc);
        if (!obj)
            return HU_ERR_OUT_OF_MEMORY;
        cp_json_set_str(alloc, obj, "error", "OAuth not configured");
        hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
        hu_json_free(alloc, obj);
        return err;
    }
    const char *provider = NULL;
    const char *redirect_uri = NULL;
    if (root) {
        hu_json_value_t *params = hu_json_object_get(root, "params");
        if (params) {
            provider = hu_json_get_string(params, "provider");
            redirect_uri = hu_json_get_string(params, "redirect_uri");
        }
    }
    if (!provider || !provider[0]) {
        hu_json_value_t *obj = hu_json_object_new(alloc);
        if (!obj)
            return HU_ERR_OUT_OF_MEMORY;
        cp_json_set_str(alloc, obj, "error", "provider is required");
        hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
        hu_json_free(alloc, obj);
        return err;
    }
    const char *ctx_provider = hu_oauth_get_provider(ctx);
    if (ctx_provider && strcmp(ctx_provider, provider) != 0) {
        hu_json_value_t *obj = hu_json_object_new(alloc);
        if (!obj)
            return HU_ERR_OUT_OF_MEMORY;
        cp_json_set_str(alloc, obj, "error", "unsupported provider");
        hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
        hu_json_free(alloc, obj);
        return err;
    }
    char verifier[64];
    char challenge[64];
    char state[48];
    if (hu_oauth_generate_pkce(ctx, verifier, sizeof(verifier), challenge, sizeof(challenge)) !=
        HU_OK)
        return HU_ERR_OUT_OF_MEMORY;
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
    hu_error_t err = hu_oauth_build_auth_url(ctx, challenge, strlen(challenge), state,
                                             strlen(state), url, sizeof(url));
    if (err != HU_OK) {
        proto->oauth_pending_remove(proto->oauth_pending_ctx, state);
        return err;
    }
    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;
    cp_json_set_str(alloc, obj, "url", url);
    cp_json_set_str(alloc, obj, "state", state);
    (void)redirect_uri;
    err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

/* ── auth.oauth.callback ─────────────────────────────────────────────── */

hu_error_t cp_admin_oauth_callback(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                   const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                   char **out, size_t *out_len) {
    (void)app;
    (void)conn;
    hu_oauth_ctx_t *ctx = (hu_oauth_ctx_t *)proto->oauth_ctx;
    if (!ctx || !proto->oauth_pending_lookup || !proto->oauth_pending_remove) {
        hu_json_value_t *obj = hu_json_object_new(alloc);
        if (!obj)
            return HU_ERR_OUT_OF_MEMORY;
        cp_json_set_str(alloc, obj, "error", "OAuth not configured");
        hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
        hu_json_free(alloc, obj);
        return err;
    }
    const char *code = NULL;
    const char *state = NULL;
    if (root) {
        hu_json_value_t *params = hu_json_object_get(root, "params");
        if (params) {
            code = hu_json_get_string(params, "code");
            state = hu_json_get_string(params, "state");
        }
    }
    if (!code || !code[0] || !state || !state[0]) {
        hu_json_value_t *obj = hu_json_object_new(alloc);
        if (!obj)
            return HU_ERR_OUT_OF_MEMORY;
        cp_json_set_str(alloc, obj, "error", "code and state are required");
        hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
        hu_json_free(alloc, obj);
        return err;
    }
    const char *verifier = proto->oauth_pending_lookup(proto->oauth_pending_ctx, state);
    if (!verifier) {
        hu_json_value_t *obj = hu_json_object_new(alloc);
        if (!obj)
            return HU_ERR_OUT_OF_MEMORY;
        cp_json_set_str(alloc, obj, "error", "invalid or expired state");
        hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
        hu_json_free(alloc, obj);
        return err;
    }
    hu_oauth_session_t session = {0};
    hu_error_t err =
        hu_oauth_exchange_code(ctx, code, strlen(code), verifier, strlen(verifier), &session);
    proto->oauth_pending_remove(proto->oauth_pending_ctx, state);
    if (err != HU_OK) {
        hu_json_value_t *obj = hu_json_object_new(alloc);
        if (!obj)
            return HU_ERR_OUT_OF_MEMORY;
        cp_json_set_str(alloc, obj, "error", "token exchange failed");
        hu_error_t serr = hu_json_stringify(alloc, obj, out, out_len);
        hu_json_free(alloc, obj);
        return serr;
    }
    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;
    cp_json_set_str(alloc, obj, "token", session.access_token);
    hu_json_value_t *user = hu_json_object_new(alloc);
    if (user) {
        cp_json_set_str(alloc, user, "id", session.user_id);
        hu_json_object_set(alloc, obj, "user", user);
    }
    err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

/* ── auth.oauth.refresh ──────────────────────────────────────────────── */

hu_error_t cp_admin_oauth_refresh(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                  const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                  char **out, size_t *out_len) {
    (void)app;
    (void)conn;
    hu_oauth_ctx_t *ctx = (hu_oauth_ctx_t *)proto->oauth_ctx;
    if (!ctx) {
        hu_json_value_t *obj = hu_json_object_new(alloc);
        if (!obj)
            return HU_ERR_OUT_OF_MEMORY;
        cp_json_set_str(alloc, obj, "error", "OAuth not configured");
        hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
        hu_json_free(alloc, obj);
        return err;
    }
    const char *refresh_token = NULL;
    if (root) {
        hu_json_value_t *params = hu_json_object_get(root, "params");
        if (params)
            refresh_token = hu_json_get_string(params, "refresh_token");
    }
    if (!refresh_token || !refresh_token[0]) {
        hu_json_value_t *obj = hu_json_object_new(alloc);
        if (!obj)
            return HU_ERR_OUT_OF_MEMORY;
        cp_json_set_str(alloc, obj, "error", "refresh_token is required");
        hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
        hu_json_free(alloc, obj);
        return err;
    }
    hu_oauth_session_t session = {0};
    size_t rt_len = strlen(refresh_token);
    if (rt_len >= sizeof(session.refresh_token))
        rt_len = sizeof(session.refresh_token) - 1;
    memcpy(session.refresh_token, refresh_token, rt_len);
    session.refresh_token[rt_len] = '\0';
    hu_error_t err = hu_oauth_refresh_token(ctx, &session);
    if (err != HU_OK) {
        hu_json_value_t *obj = hu_json_object_new(alloc);
        if (!obj)
            return HU_ERR_OUT_OF_MEMORY;
        cp_json_set_str(alloc, obj, "error", "refresh failed");
        hu_error_t serr = hu_json_stringify(alloc, obj, out, out_len);
        hu_json_free(alloc, obj);
        return serr;
    }
    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;
    cp_json_set_str(alloc, obj, "access_token", session.access_token);
    int64_t expires_in = session.expires_at - (int64_t)time(NULL);
    if (expires_in < 0)
        expires_in = 0;
    hu_json_object_set(alloc, obj, "expires_in", hu_json_number_new(alloc, (double)expires_in));
    err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

#ifdef HU_HAS_CRON
/* ── cron.list ───────────────────────────────────────────────────────── */

hu_error_t cp_admin_cron_list(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                              const hu_control_protocol_t *proto, const hu_json_value_t *root,
                              char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;

    hu_json_value_t *arr = hu_json_array_new(alloc);

    if (app && app->cron) {
        size_t count = 0;
        const hu_cron_job_t *jobs = hu_cron_list_jobs(app->cron, &count);
        for (size_t i = 0; i < count; i++) {
            hu_json_value_t *j = hu_json_object_new(alloc);
            hu_json_object_set(alloc, j, "id", hu_json_number_new(alloc, (double)jobs[i].id));
            cp_json_set_str(alloc, j, "name", jobs[i].name);
            cp_json_set_str(alloc, j, "expression", jobs[i].expression);
            cp_json_set_str(alloc, j, "command", jobs[i].command);
            hu_json_object_set(alloc, j, "enabled", hu_json_bool_new(alloc, jobs[i].enabled));
            hu_json_object_set(alloc, j, "next_run",
                               hu_json_number_new(alloc, (double)jobs[i].next_run_secs));
            hu_json_object_set(alloc, j, "last_run",
                               hu_json_number_new(alloc, (double)jobs[i].last_run_secs));
            cp_json_set_str(alloc, j, "type",
                            jobs[i].type == HU_CRON_JOB_AGENT ? "agent" : "shell");
            cp_json_set_str(alloc, j, "channel", jobs[i].channel);
            cp_json_set_str(alloc, j, "last_status", jobs[i].last_status);
            hu_json_object_set(alloc, j, "paused", hu_json_bool_new(alloc, jobs[i].paused));
            hu_json_object_set(alloc, j, "one_shot", hu_json_bool_new(alloc, jobs[i].one_shot));
            hu_json_object_set(alloc, j, "created_at",
                               hu_json_number_new(alloc, (double)jobs[i].created_at_s));
            hu_json_array_push(alloc, arr, j);
        }
    }

    hu_json_object_set(alloc, obj, "jobs", arr);
    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

/* ── cron.add ────────────────────────────────────────────────────────── */

hu_error_t cp_admin_cron_add(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                             const hu_control_protocol_t *proto, const hu_json_value_t *root,
                             char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    bool added = false;
    uint64_t new_id = 0;

    if (root && app && app->cron && app->alloc) {
        hu_json_value_t *params = hu_json_object_get(root, "params");
        if (params) {
            const char *expr = hu_json_get_string(params, "expression");
            const char *cmd = hu_json_get_string(params, "command");
            const char *name = hu_json_get_string(params, "name");
            const char *type_str = hu_json_get_string(params, "type");
            const char *prompt = hu_json_get_string(params, "prompt");
            const char *channel = hu_json_get_string(params, "channel");
            bool one_shot = hu_json_get_bool(params, "one_shot", false);
            if (expr && type_str && strcmp(type_str, "agent") == 0 && prompt) {
                hu_error_t e = hu_cron_add_agent_job(app->cron, app->alloc, expr, prompt, channel,
                                                     name ? name : "Agent task", &new_id);
                added = (e == HU_OK);
                if (added && one_shot)
                    hu_cron_set_job_one_shot(app->cron, new_id, true);
            } else if (expr && cmd) {
                hu_error_t e =
                    hu_cron_add_job(app->cron, app->alloc, expr, cmd, name ? name : cmd, &new_id);
                added = (e == HU_OK);
                if (added) {
                    if (one_shot)
                        hu_cron_set_job_one_shot(app->cron, new_id, true);
                    char *cron_path = NULL;
                    size_t cron_path_len = 0;
                    if (hu_crontab_get_path(app->alloc, &cron_path, &cron_path_len) == HU_OK) {
                        char *unused_id = NULL;
                        hu_crontab_add(app->alloc, cron_path, expr, strlen(expr), cmd, strlen(cmd),
                                       &unused_id);
                        if (unused_id)
                            app->alloc->free(app->alloc->ctx, unused_id, strlen(unused_id) + 1);
                        app->alloc->free(app->alloc->ctx, cron_path, cron_path_len + 1);
                    }
                }
            }
        }
    }

    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;
    hu_json_object_set(alloc, obj, "added", hu_json_bool_new(alloc, added));
    if (added)
        hu_json_object_set(alloc, obj, "id", hu_json_number_new(alloc, (double)new_id));
    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

/* ── cron.remove ─────────────────────────────────────────────────────── */

hu_error_t cp_admin_cron_remove(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    bool removed = false;

    if (root && app && app->cron) {
        hu_json_value_t *params = hu_json_object_get(root, "params");
        if (params) {
            double id_num = hu_json_get_number(params, "id", -1.0);
            if (id_num >= 0) {
                uint64_t job_id = (uint64_t)id_num;
                hu_error_t e = hu_cron_remove_job(app->cron, job_id);
                removed = (e == HU_OK);
                if (removed && app->alloc) {
                    char *cron_path = NULL;
                    size_t cron_path_len = 0;
                    if (hu_crontab_get_path(app->alloc, &cron_path, &cron_path_len) == HU_OK) {
                        char id_str[32];
                        snprintf(id_str, sizeof(id_str), "%llu", (unsigned long long)job_id);
                        hu_crontab_remove(app->alloc, cron_path, id_str);
                        app->alloc->free(app->alloc->ctx, cron_path, cron_path_len + 1);
                    }
                }
            }
        }
    }

    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;
    hu_json_object_set(alloc, obj, "removed", hu_json_bool_new(alloc, removed));
    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

/* ── cron.run ────────────────────────────────────────────────────────── */

#if !HU_IS_TEST && defined(HU_GATEWAY_POSIX)
typedef struct cron_run_work {
    hu_app_context_t *app;
    uint64_t job_id;
    char *command;
    char *channel;
    hu_cron_job_type_t type;
} cron_run_work_t;

static void *cron_run_worker(void *arg) {
    cron_run_work_t *w = (cron_run_work_t *)arg;
    if (!w || !w->app || !w->app->alloc || !w->command) {
        if (w && w->app && w->app->alloc) {
            if (w->command)
                w->app->alloc->free(w->app->alloc->ctx, w->command, strlen(w->command) + 1);
            if (w->channel)
                w->app->alloc->free(w->app->alloc->ctx, w->channel, strlen(w->channel) + 1);
            w->app->alloc->free(w->app->alloc->ctx, w, sizeof(cron_run_work_t));
        }
        return NULL;
    }
    hu_app_context_t *app = w->app;
    hu_allocator_t *alloc = app->alloc;
    uint64_t job_id = w->job_id;
    bool started = false;

    if (w->type == HU_CRON_JOB_AGENT && app->agent) {
        char *reply = NULL;
        size_t reply_len = 0;
        app->agent->active_channel = w->channel ? w->channel : "gateway";
        app->agent->active_channel_len = strlen(app->agent->active_channel);
        app->agent->active_job_id = job_id;
        hu_error_t run_err =
            hu_agent_turn(app->agent, w->command, strlen(w->command), &reply, &reply_len);
        app->agent->active_job_id = 0;
        started = (run_err == HU_OK);
        hu_cron_add_run(app->cron, app->alloc, job_id, (int64_t)time(NULL),
                        started ? "completed" : "failed", reply);
        if (reply)
            app->alloc->free(app->alloc->ctx, reply, reply_len + 1);
    } else {
        const char *argv[] = {"/bin/sh", "-c", w->command, NULL};
        hu_run_result_t run_result = {0};
        hu_error_t run_err = hu_process_run(app->alloc, argv, NULL, 65536, &run_result);
        started = (run_err == HU_OK);
        const char *run_output = run_result.stdout_buf;
        hu_cron_add_run(app->cron, app->alloc, job_id, (int64_t)time(NULL),
                        started ? "completed" : "failed", run_output);
        hu_run_result_free(app->alloc, &run_result);
    }

    if (app->bus) {
        hu_bus_event_t bev;
        memset(&bev, 0, sizeof(bev));
        bev.type = HU_BUS_CRON_COMPLETED;
        snprintf(bev.channel, HU_BUS_CHANNEL_LEN, "cron");
        snprintf(bev.id, HU_BUS_ID_LEN, "%llu", (unsigned long long)job_id);
        snprintf(bev.message, HU_BUS_MSG_LEN, "%s", started ? "completed" : "failed");
        hu_bus_publish(app->bus, &bev);
    }

    if (w && alloc) {
        if (w->command)
            alloc->free(alloc->ctx, w->command, strlen(w->command) + 1);
        if (w->channel)
            alloc->free(alloc->ctx, w->channel, strlen(w->channel) + 1);
        alloc->free(alloc->ctx, w, sizeof(cron_run_work_t));
    }
    return NULL;
}
#endif

hu_error_t cp_admin_cron_run(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                             const hu_control_protocol_t *proto, const hu_json_value_t *root,
                             char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    bool started = false;
    const char *status_msg = "no cron scheduler";

    if (root && app && app->cron && app->alloc) {
        hu_json_value_t *params = hu_json_object_get(root, "params");
        if (params) {
            double id_num = hu_json_get_number(params, "id", -1.0);
            if (id_num >= 0) {
                uint64_t job_id = (uint64_t)id_num;
                const hu_cron_job_t *job = hu_cron_get_job(app->cron, job_id);
                if (job && job->command) {
                    hu_cron_add_run(app->cron, app->alloc, job_id, (int64_t)time(NULL), "running",
                                    NULL);
                    if (app->bus) {
                        hu_bus_event_t bev;
                        memset(&bev, 0, sizeof(bev));
                        bev.type = HU_BUS_CRON_STARTED;
                        snprintf(bev.channel, HU_BUS_CHANNEL_LEN, "cron");
                        snprintf(bev.id, HU_BUS_ID_LEN, "%llu", (unsigned long long)job_id);
                        snprintf(bev.message, HU_BUS_MSG_LEN, "%s", job->name ? job->name : "");
                        hu_bus_publish(app->bus, &bev);
                    }
#if !HU_IS_TEST && defined(HU_GATEWAY_POSIX)
                    cron_run_work_t *w = (cron_run_work_t *)app->alloc->alloc(
                        app->alloc->ctx, sizeof(cron_run_work_t));
                    if (w) {
                        memset(w, 0, sizeof(*w));
                        w->app = app;
                        w->job_id = job_id;
                        w->command = hu_strdup(app->alloc, job->command);
                        w->channel = job->channel ? hu_strdup(app->alloc, job->channel) : NULL;
                        w->type = job->type;
                        if (w->command) {
                            pthread_attr_t attr;
                            pthread_t tid;
                            pthread_attr_init(&attr);
                            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
                            if (pthread_create(&tid, &attr, cron_run_worker, w) == 0) {
                                started = true;
                                status_msg = "running";
                            } else {
                                app->alloc->free(app->alloc->ctx, w->command,
                                                 strlen(w->command) + 1);
                                if (w->channel)
                                    app->alloc->free(app->alloc->ctx, w->channel,
                                                     strlen(w->channel) + 1);
                                app->alloc->free(app->alloc->ctx, w, sizeof(cron_run_work_t));
                                status_msg = "failed to start worker";
                            }
                            pthread_attr_destroy(&attr);
                        } else {
                            app->alloc->free(app->alloc->ctx, w, sizeof(cron_run_work_t));
                            status_msg = "out of memory";
                        }
                    } else {
                        status_msg = "out of memory";
                    }
#else
                    started = true;
#if HU_IS_TEST
                    status_msg = "running";
#else
                    /* Non-POSIX or fallback: run synchronously (blocks WebSocket thread) */
                    if (job->type == HU_CRON_JOB_AGENT && app->agent) {
                        char *reply = NULL;
                        size_t reply_len = 0;
                        app->agent->active_channel = job->channel ? job->channel : "gateway";
                        app->agent->active_channel_len = strlen(app->agent->active_channel);
                        app->agent->active_job_id = job_id;
                        hu_error_t run_err = hu_agent_turn(
                            app->agent, job->command, strlen(job->command), &reply, &reply_len);
                        app->agent->active_job_id = 0;
                        started = (run_err == HU_OK);
                        hu_cron_add_run(app->cron, app->alloc, job_id, (int64_t)time(NULL),
                                        started ? "completed" : "failed", reply);
                        if (reply)
                            app->alloc->free(app->alloc->ctx, reply, reply_len + 1);
                    } else {
                        const char *argv[] = {"/bin/sh", "-c", job->command, NULL};
                        hu_run_result_t run_result = {0};
                        hu_error_t run_err =
                            hu_process_run(app->alloc, argv, NULL, 65536, &run_result);
                        started = (run_err == HU_OK);
                        const char *run_output = run_result.stdout_buf;
                        hu_cron_add_run(app->cron, app->alloc, job_id, (int64_t)time(NULL),
                                        started ? "completed" : "failed", run_output);
                        hu_run_result_free(app->alloc, &run_result);
                    }
                    if (app->bus) {
                        hu_bus_event_t bev;
                        memset(&bev, 0, sizeof(bev));
                        bev.type = HU_BUS_CRON_COMPLETED;
                        snprintf(bev.channel, HU_BUS_CHANNEL_LEN, "cron");
                        snprintf(bev.id, HU_BUS_ID_LEN, "%llu", (unsigned long long)job_id);
                        snprintf(bev.message, HU_BUS_MSG_LEN, "%s",
                                 started ? "completed" : "failed");
                        hu_bus_publish(app->bus, &bev);
                    }
                    status_msg = started ? "completed" : "failed";
#endif
#endif
                } else {
                    status_msg = "job not found";
                }
            } else {
                status_msg = "missing job id";
            }
        }
    }

    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;
    hu_json_object_set(alloc, obj, "started", hu_json_bool_new(alloc, started));
    cp_json_set_str(alloc, obj, "status", status_msg);
    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

/* ── cron.update ─────────────────────────────────────────────────────── */

hu_error_t cp_admin_cron_update(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    bool updated = false;

    if (root && app && app->cron && app->alloc) {
        hu_json_value_t *params = hu_json_object_get(root, "params");
        if (params) {
            double id_num = hu_json_get_number(params, "id", -1.0);
            if (id_num >= 0) {
                uint64_t job_id = (uint64_t)id_num;
                const char *expr = hu_json_get_string(params, "expression");
                const char *cmd = hu_json_get_string(params, "command");
                hu_json_value_t *en_val = hu_json_object_get(params, "enabled");
                bool en = true;
                bool *en_ptr = NULL;
                if (en_val && en_val->type == HU_JSON_BOOL) {
                    en = en_val->data.boolean;
                    en_ptr = &en;
                }
                hu_error_t e = hu_cron_update_job(app->cron, app->alloc, job_id, expr, cmd, en_ptr);
                updated = (e == HU_OK);
            }
        }
    }

    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;
    hu_json_object_set(alloc, obj, "updated", hu_json_bool_new(alloc, updated));
    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

/* ── cron.runs ───────────────────────────────────────────────────────── */

hu_error_t cp_admin_cron_runs(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                              const hu_control_protocol_t *proto, const hu_json_value_t *root,
                              char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;
    hu_json_value_t *arr = hu_json_array_new(alloc);

    if (root && app && app->cron) {
        hu_json_value_t *params = hu_json_object_get(root, "params");
        double id_num = params ? hu_json_get_number(params, "id", -1.0) : -1.0;
        double limit_num = params ? hu_json_get_number(params, "limit", 10.0) : 10.0;
        if (id_num >= 0) {
            size_t count = 0;
            const hu_cron_run_t *runs =
                hu_cron_list_runs(app->cron, (uint64_t)id_num, (size_t)limit_num, &count);
            for (size_t i = 0; i < count; i++) {
                hu_json_value_t *r = hu_json_object_new(alloc);
                hu_json_object_set(alloc, r, "id", hu_json_number_new(alloc, (double)runs[i].id));
                hu_json_object_set(alloc, r, "started_at",
                                   hu_json_number_new(alloc, (double)runs[i].started_at_s));
                hu_json_object_set(alloc, r, "finished_at",
                                   hu_json_number_new(alloc, (double)runs[i].finished_at_s));
                cp_json_set_str(alloc, r, "status", runs[i].status);
                hu_json_array_push(alloc, arr, r);
            }
        }
    }

    hu_json_object_set(alloc, obj, "runs", arr);
    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

#endif /* HU_HAS_CRON */

#ifdef HU_HAS_SKILLS
/* ── skills.list ─────────────────────────────────────────────────────── */

hu_error_t cp_admin_skills_list(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;

    hu_json_value_t *arr = hu_json_array_new(alloc);

    if (app && app->skills) {
        hu_skill_t *skills = NULL;
        size_t count = 0;
        hu_error_t e = hu_skillforge_list_skills(app->skills, &skills, &count);
        if (e == HU_OK && skills) {
            for (size_t i = 0; i < count; i++) {
                hu_json_value_t *s = hu_json_object_new(alloc);
                cp_json_set_str(alloc, s, "name", skills[i].name);
                cp_json_set_str(alloc, s, "description", skills[i].description);
                hu_json_object_set(alloc, s, "enabled", hu_json_bool_new(alloc, skills[i].enabled));
                hu_json_array_push(alloc, arr, s);
            }
        }
    }

    hu_json_object_set(alloc, obj, "skills", arr);
    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

/* ── skills.enable / skills.disable ──────────────────────────────────── */

static hu_error_t cp_admin_skill_toggle(hu_allocator_t *alloc, hu_app_context_t *app,
                                        const hu_json_value_t *root, bool enable, char **out,
                                        size_t *out_len) {
    bool success = false;

    if (root && app && app->skills) {
        hu_json_value_t *params = hu_json_object_get(root, "params");
        if (params) {
            const char *name = hu_json_get_string(params, "name");
            if (name) {
                hu_error_t e = enable ? hu_skillforge_enable(app->skills, name)
                                      : hu_skillforge_disable(app->skills, name);
                success = (e == HU_OK);
            }
        }
    }

    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;
    hu_json_object_set(alloc, obj, enable ? "enabled" : "disabled",
                       hu_json_bool_new(alloc, success));
    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

hu_error_t cp_admin_skills_enable(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                  const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                  char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    return cp_admin_skill_toggle(alloc, app, root, true, out, out_len);
}

hu_error_t cp_admin_skills_disable(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                   const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                   char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    return cp_admin_skill_toggle(alloc, app, root, false, out, out_len);
}

/* ── skills.install ──────────────────────────────────────────────────── */

hu_error_t cp_admin_skills_install(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                   const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                   char **out, size_t *out_len) {
    (void)app;
    (void)conn;
    (void)proto;
    bool installed = false;
    const char *error_msg = NULL;

    if (root) {
        hu_json_value_t *params = hu_json_object_get(root, "params");
        if (params) {
            const char *name = hu_json_get_string(params, "name");
            const char *url = hu_json_get_string(params, "url");
            if (name && name[0]) {
                hu_error_t e = hu_skillforge_install(name, url);
                if (e == HU_OK) {
                    installed = true;
                } else {
                    error_msg = hu_error_string(e);
                }
            } else {
                error_msg = "name is required";
            }
        }
    }

    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;
    hu_json_object_set(alloc, obj, "installed", hu_json_bool_new(alloc, installed));
    if (error_msg)
        cp_json_set_str(alloc, obj, "error", error_msg);
    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

/* ── skills.search ───────────────────────────────────────────────────── */

hu_error_t cp_admin_skills_search(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                  const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                  char **out, size_t *out_len) {
    (void)app;
    (void)conn;
    (void)proto;
    const char *query = NULL;
    if (root) {
        hu_json_value_t *params = hu_json_object_get(root, "params");
        if (params)
            query = hu_json_get_string(params, "query");
    }

    hu_skill_registry_entry_t *entries = NULL;
    size_t count = 0;
    hu_error_t e = hu_skill_registry_search(alloc, query, &entries, &count);

    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj) {
        if (entries)
            hu_skill_registry_entries_free(alloc, entries, count);
        return HU_ERR_OUT_OF_MEMORY;
    }

    hu_json_value_t *arr = hu_json_array_new(alloc);
    if (e == HU_OK && entries) {
        for (size_t i = 0; i < count; i++) {
            hu_json_value_t *entry = hu_json_object_new(alloc);
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
            hu_json_array_push(alloc, arr, entry);
        }
        hu_skill_registry_entries_free(alloc, entries, count);
    }

    hu_json_object_set(alloc, obj, "entries", arr);
    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

/* ── skills.uninstall ────────────────────────────────────────────────── */

hu_error_t cp_admin_skills_uninstall(hu_allocator_t *alloc, hu_app_context_t *app,
                                     hu_ws_conn_t *conn, const hu_control_protocol_t *proto,
                                     const hu_json_value_t *root, char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    bool success = false;

    if (root && app && app->skills) {
        hu_json_value_t *params = hu_json_object_get(root, "params");
        if (params) {
            const char *name = hu_json_get_string(params, "name");
            if (name) {
                hu_error_t e = hu_skillforge_uninstall(app->skills, name);
                success = (e == HU_OK);
            }
        }
    }

    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;
    hu_json_object_set(alloc, obj, "uninstalled", hu_json_bool_new(alloc, success));
    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

/* ── skills.update ───────────────────────────────────────────────────── */

hu_error_t cp_admin_skills_update(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                  const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                  char **out, size_t *out_len) {
    (void)app;
    (void)conn;
    (void)proto;
    (void)root;
    hu_error_t e = hu_skill_registry_update(alloc);
    bool success = (e == HU_OK);

    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;
    hu_json_object_set(alloc, obj, "updated", hu_json_bool_new(alloc, success));
    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

#endif /* HU_HAS_SKILLS */

#ifdef HU_HAS_UPDATE
/* ── update.check ────────────────────────────────────────────────────── */

hu_error_t cp_admin_update_check(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                 const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                 char **out, size_t *out_len) {
    (void)app;
    (void)conn;
    (void)proto;
    (void)root;
    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;
    const char *current = hu_version_string();
    cp_json_set_str(alloc, obj, "current", current);
    char latest[64] = {0};
    hu_error_t check_err = hu_update_check(latest, sizeof(latest));
    if (check_err == HU_OK && latest[0]) {
        cp_json_set_str(alloc, obj, "latest", latest);
        hu_json_object_set(alloc, obj, "available",
                           hu_json_bool_new(alloc, strcmp(latest, current) != 0));
    } else {
        cp_json_set_str(alloc, obj, "latest", current);
        hu_json_object_set(alloc, obj, "available", hu_json_bool_new(alloc, false));
        if (check_err != HU_OK)
            cp_json_set_str(alloc, obj, "error", hu_error_string(check_err));
    }
    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

/* ── update.run ──────────────────────────────────────────────────────── */

hu_error_t cp_admin_update_run(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                               const hu_control_protocol_t *proto, const hu_json_value_t *root,
                               char **out, size_t *out_len) {
    (void)app;
    (void)conn;
    (void)proto;
    (void)root;
    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;
    hu_error_t apply_err = hu_update_apply();
    if (apply_err == HU_OK)
        cp_json_set_str(alloc, obj, "status", "updated");
    else {
        cp_json_set_str(alloc, obj, "status", hu_error_string(apply_err));
        cp_json_set_str(alloc, obj, "error", hu_error_string(apply_err));
    }
    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

#endif /* HU_HAS_UPDATE */

#ifdef HU_HAS_PUSH
/* ── push.register ──────────────────────────────────────────────────── */

hu_error_t cp_admin_push_register(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                  const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                  char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    bool registered = false;
    const char *error_msg = NULL;

    if (root && app && app->push) {
        hu_json_value_t *params = hu_json_object_get(root, "params");
        if (params) {
            const char *token = hu_json_get_string(params, "token");
            const char *provider_str = hu_json_get_string(params, "provider");
            if (token && token[0]) {
                hu_push_provider_t provider = HU_PUSH_NONE;
                if (provider_str) {
                    if (strcmp(provider_str, "fcm") == 0)
                        provider = HU_PUSH_FCM;
                    else if (strcmp(provider_str, "apns") == 0)
                        provider = HU_PUSH_APNS;
                }
                if (provider != HU_PUSH_NONE) {
                    hu_error_t e = hu_push_register_token(app->push, token, provider);
                    registered = (e == HU_OK);
                    if (!registered)
                        error_msg = hu_error_string(e);
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

    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;
    hu_json_object_set(alloc, obj, "registered", hu_json_bool_new(alloc, registered));
    if (error_msg)
        cp_json_set_str(alloc, obj, "error", error_msg);
    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

/* ── push.unregister ────────────────────────────────────────────────── */

hu_error_t cp_admin_push_unregister(hu_allocator_t *alloc, hu_app_context_t *app,
                                    hu_ws_conn_t *conn, const hu_control_protocol_t *proto,
                                    const hu_json_value_t *root, char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    bool unregistered = false;
    const char *error_msg = NULL;

    if (root && app && app->push) {
        hu_json_value_t *params = hu_json_object_get(root, "params");
        if (params) {
            const char *token = hu_json_get_string(params, "token");
            if (token && token[0]) {
                hu_error_t e = hu_push_unregister_token(app->push, token);
                unregistered = (e == HU_OK);
                if (!unregistered)
                    error_msg = hu_error_string(e);
            } else {
                error_msg = "token is required";
            }
        }
    } else if (!app || !app->push) {
        error_msg = "push not configured";
    }

    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;
    hu_json_object_set(alloc, obj, "unregistered", hu_json_bool_new(alloc, unregistered));
    if (error_msg)
        cp_json_set_str(alloc, obj, "error", error_msg);
    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

#endif /* HU_HAS_PUSH */

#endif /* HU_GATEWAY_POSIX */
