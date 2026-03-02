#include "seaclaw/gateway/control_protocol.h"
#include <stdint.h>
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/json.h"
#include "seaclaw/config.h"
#include "seaclaw/session.h"
#include "seaclaw/cron.h"
#include "seaclaw/skillforge.h"
#include "seaclaw/cost.h"
#include "seaclaw/bus.h"
#include "seaclaw/tool.h"
#include "seaclaw/channel_catalog.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef SC_GATEWAY_POSIX

/* ── JSON helper: add string field ───────────────────────────────────── */

static void json_set_str(sc_allocator_t *a, sc_json_value_t *obj,
    const char *key, const char *val) {
    if (!val) val = "";
    sc_json_object_set(a, obj, key,
        sc_json_string_new(a, val, strlen(val)));
}

/* ── connect ─────────────────────────────────────────────────────────── */

static sc_error_t build_connect_response(sc_allocator_t *alloc,
    const sc_app_context_t *app, char **out, size_t *out_len) {
    sc_json_value_t *root = sc_json_object_new(alloc);
    if (!root) return SC_ERR_OUT_OF_MEMORY;

    json_set_str(alloc, root, "type", "hello-ok");

    sc_json_value_t *server = sc_json_object_new(alloc);
    json_set_str(alloc, server, "version", "0.1.0");
    sc_json_object_set(alloc, root, "server", server);
    sc_json_object_set(alloc, root, "protocol", sc_json_number_new(alloc, 1));

    sc_json_value_t *features = sc_json_object_new(alloc);
    sc_json_value_t *methods_arr = sc_json_array_new(alloc);

    static const char *const methods[] = {
        "connect", "health", "config.get", "config.schema", "capabilities",
        "chat.send", "chat.history", "chat.abort",
        "config.set", "config.apply", "sessions.list", "sessions.patch",
        "sessions.delete", "tools.catalog", "channels.status", "cron.list",
        "cron.add", "cron.remove", "cron.run", "skills.list", "skills.install",
        "skills.enable", "skills.disable",
        "update.check", "update.run", "exec.approval.resolve",
        "usage.summary"
    };
    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
        sc_json_value_t *m = sc_json_string_new(alloc, methods[i],
            strlen(methods[i]));
        if (m) sc_json_array_push(alloc, methods_arr, m);
    }
    sc_json_object_set(alloc, features, "methods", methods_arr);

    if (app) {
        sc_json_object_set(alloc, features, "sessions",
            sc_json_bool_new(alloc, app->sessions != NULL));
        sc_json_object_set(alloc, features, "cron",
            sc_json_bool_new(alloc, app->cron != NULL));
        sc_json_object_set(alloc, features, "skills",
            sc_json_bool_new(alloc, app->skills != NULL));
        sc_json_object_set(alloc, features, "cost_tracking",
            sc_json_bool_new(alloc, app->costs != NULL));
    }

    sc_json_object_set(alloc, root, "features", features);

    sc_error_t err = sc_json_stringify(alloc, root, out, out_len);
    sc_json_free(alloc, root);
    return err;
}

/* ── health ──────────────────────────────────────────────────────────── */

static sc_error_t handle_health(sc_allocator_t *alloc, char **out,
    size_t *out_len) {
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj) return SC_ERR_OUT_OF_MEMORY;
    json_set_str(alloc, obj, "status", "ok");
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── config.get ──────────────────────────────────────────────────────── */

static sc_error_t handle_config_get(sc_allocator_t *alloc,
    const sc_app_context_t *app, char **out, size_t *out_len) {
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj) return SC_ERR_OUT_OF_MEMORY;

    if (app && app->config) {
        sc_json_object_set(alloc, obj, "exists",
            sc_json_bool_new(alloc, true));
        json_set_str(alloc, obj, "workspace_dir",
            app->config->workspace_dir);
        json_set_str(alloc, obj, "default_provider",
            app->config->default_provider);
        json_set_str(alloc, obj, "default_model",
            app->config->default_model);
        sc_json_object_set(alloc, obj, "max_tokens",
            sc_json_number_new(alloc, (double)app->config->max_tokens));
        sc_json_object_set(alloc, obj, "temperature",
            sc_json_number_new(alloc, app->config->temperature));
    } else {
        sc_json_object_set(alloc, obj, "exists",
            sc_json_bool_new(alloc, false));
    }

    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── config.schema ───────────────────────────────────────────────────── */

static sc_error_t handle_config_schema(sc_allocator_t *alloc, char **out,
    size_t *out_len) {
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj) return SC_ERR_OUT_OF_MEMORY;

    sc_json_value_t *schema = sc_json_object_new(alloc);
    json_set_str(alloc, schema, "type", "object");

    sc_json_value_t *props = sc_json_object_new(alloc);
    sc_json_value_t *wp = sc_json_object_new(alloc);
    json_set_str(alloc, wp, "type", "string");
    json_set_str(alloc, wp, "description", "Workspace directory");
    sc_json_object_set(alloc, props, "workspace_dir", wp);

    sc_json_value_t *dp = sc_json_object_new(alloc);
    json_set_str(alloc, dp, "type", "string");
    json_set_str(alloc, dp, "description", "Default AI provider");
    sc_json_object_set(alloc, props, "default_provider", dp);

    sc_json_value_t *dm = sc_json_object_new(alloc);
    json_set_str(alloc, dm, "type", "string");
    json_set_str(alloc, dm, "description", "Default model");
    sc_json_object_set(alloc, props, "default_model", dm);

    sc_json_value_t *mt = sc_json_object_new(alloc);
    json_set_str(alloc, mt, "type", "integer");
    json_set_str(alloc, mt, "description", "Max tokens per response");
    sc_json_object_set(alloc, props, "max_tokens", mt);

    sc_json_value_t *tp = sc_json_object_new(alloc);
    json_set_str(alloc, tp, "type", "number");
    json_set_str(alloc, tp, "description", "Temperature (0.0 - 2.0)");
    sc_json_object_set(alloc, props, "temperature", tp);

    sc_json_object_set(alloc, schema, "properties", props);
    sc_json_object_set(alloc, obj, "schema", schema);

    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── capabilities ────────────────────────────────────────────────────── */

static sc_error_t handle_capabilities(sc_allocator_t *alloc,
    const sc_app_context_t *app, char **out, size_t *out_len) {
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj) return SC_ERR_OUT_OF_MEMORY;

    json_set_str(alloc, obj, "version", "0.1.0");

    size_t tool_count = app ? app->tools_count : 0;
    sc_json_object_set(alloc, obj, "tools",
        sc_json_number_new(alloc, (double)tool_count));

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
    sc_json_object_set(alloc, obj, "channels",
        sc_json_number_new(alloc, (double)ch_count));

    size_t prov_count = (app && app->config) ? app->config->providers_len : 0;
    sc_json_object_set(alloc, obj, "providers",
        sc_json_number_new(alloc, (double)prov_count));

    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── chat.send ───────────────────────────────────────────────────────── */

static sc_error_t handle_chat_send(sc_allocator_t *alloc,
    const sc_app_context_t *app, const sc_json_value_t *root,
    char **out, size_t *out_len) {
    const char *message = NULL;
    const char *session_key = "default";

    if (root) {
        sc_json_value_t *params = sc_json_object_get(root, "params");
        if (params) {
            const char *m = sc_json_get_string(params, "message");
            if (m) message = m;
            const char *sk = sc_json_get_string(params, "sessionKey");
            if (sk) session_key = sk;
        }
    }

    if (!message || !message[0]) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj) return SC_ERR_OUT_OF_MEMORY;
        json_set_str(alloc, obj, "status", "rejected");
        json_set_str(alloc, obj, "error", "message is required");
        sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
        sc_json_free(alloc, obj);
        return err;
    }

    if (app && app->bus) {
        sc_bus_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = SC_BUS_MESSAGE_RECEIVED;
        snprintf(ev.channel, SC_BUS_CHANNEL_LEN, "control-ui");
        snprintf(ev.id, SC_BUS_ID_LEN, "%s", session_key);
        ev.payload = (void *)message;
        size_t ml = strlen(message);
        if (ml >= SC_BUS_MSG_LEN) ml = SC_BUS_MSG_LEN - 1;
        memcpy(ev.message, message, ml);
        ev.message[ml] = '\0';
        sc_bus_publish(app->bus, &ev);
    }

    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj) return SC_ERR_OUT_OF_MEMORY;
    json_set_str(alloc, obj, "status", "queued");
    json_set_str(alloc, obj, "sessionKey", session_key);
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── chat.history ────────────────────────────────────────────────────── */

static sc_error_t handle_chat_history(sc_allocator_t *alloc,
    const sc_app_context_t *app, const sc_json_value_t *root,
    char **out, size_t *out_len) {
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj) return SC_ERR_OUT_OF_MEMORY;

    sc_json_value_t *msgs = sc_json_array_new(alloc);

    const char *session_key = "default";
    if (root) {
        sc_json_value_t *params = sc_json_object_get(root, "params");
        if (params) {
            const char *sk = sc_json_get_string(params, "sessionKey");
            if (sk) session_key = sk;
        }
    }

    if (app && app->sessions) {
        sc_session_t *sess = sc_session_get_or_create(app->sessions, session_key);
        if (sess) {
            for (size_t i = 0; i < sess->message_count; i++) {
                sc_json_value_t *m_obj = sc_json_object_new(alloc);
                json_set_str(alloc, m_obj, "role", sess->messages[i].role);
                if (sess->messages[i].content)
                    json_set_str(alloc, m_obj, "content",
                        sess->messages[i].content);
                sc_json_array_push(alloc, msgs, m_obj);
            }
        }
    }

    sc_json_object_set(alloc, obj, "messages", msgs);
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── chat.abort ──────────────────────────────────────────────────────── */

static sc_error_t handle_chat_abort(sc_allocator_t *alloc,
    const sc_app_context_t *app, char **out, size_t *out_len) {
    bool aborted = false;
    if (app && app->bus) {
        sc_bus_publish_simple(app->bus, SC_BUS_ERROR,
            "control-ui", "", "chat.abort");
        aborted = true;
    }
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj) return SC_ERR_OUT_OF_MEMORY;
    sc_json_object_set(alloc, obj, "aborted", sc_json_bool_new(alloc, aborted));
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── config.set ──────────────────────────────────────────────────────── */

static sc_error_t handle_config_set(sc_allocator_t *alloc,
    sc_app_context_t *app, const sc_json_value_t *root,
    char **out, size_t *out_len) {
    bool saved = false;

    if (root && app && app->config) {
        sc_json_value_t *params = sc_json_object_get(root, "params");
        if (params) {
            const char *raw = sc_json_get_string(params, "raw");
            if (raw) {
                sc_error_t parse_err = sc_config_parse_json(app->config,
                    raw, strlen(raw));
                if (parse_err == SC_OK) {
                    sc_error_t save_err = sc_config_save(app->config);
                    saved = (save_err == SC_OK);
                }
            }
        }
    }

    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj) return SC_ERR_OUT_OF_MEMORY;
    sc_json_object_set(alloc, obj, "saved", sc_json_bool_new(alloc, saved));
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── config.apply ────────────────────────────────────────────────────── */

static sc_error_t handle_config_apply(sc_allocator_t *alloc,
    sc_app_context_t *app, const sc_json_value_t *root,
    char **out, size_t *out_len) {
    bool applied = false;
    bool saved = false;

    if (root && app && app->config) {
        sc_json_value_t *params = sc_json_object_get(root, "params");
        if (params) {
            const char *raw = sc_json_get_string(params, "raw");
            if (raw) {
                sc_error_t parse_err = sc_config_parse_json(app->config,
                    raw, strlen(raw));
                if (parse_err == SC_OK) {
                    applied = true;
                    sc_error_t save_err = sc_config_save(app->config);
                    saved = (save_err == SC_OK);
                }
            }
        }
    }

    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj) return SC_ERR_OUT_OF_MEMORY;
    sc_json_object_set(alloc, obj, "applied", sc_json_bool_new(alloc, applied));
    sc_json_object_set(alloc, obj, "saved", sc_json_bool_new(alloc, saved));
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── sessions.list ───────────────────────────────────────────────────── */

static sc_error_t handle_sessions_list(sc_allocator_t *alloc,
    const sc_app_context_t *app, char **out, size_t *out_len) {
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj) return SC_ERR_OUT_OF_MEMORY;

    sc_json_value_t *arr = sc_json_array_new(alloc);

    if (app && app->sessions) {
        size_t count = 0;
        sc_session_summary_t *summaries = sc_session_list(app->sessions,
            alloc, &count);
        if (summaries) {
            for (size_t i = 0; i < count; i++) {
                sc_json_value_t *s = sc_json_object_new(alloc);
                json_set_str(alloc, s, "key", summaries[i].session_key);
                json_set_str(alloc, s, "label", summaries[i].label);
                sc_json_object_set(alloc, s, "created_at",
                    sc_json_number_new(alloc, (double)summaries[i].created_at));
                sc_json_object_set(alloc, s, "last_active",
                    sc_json_number_new(alloc, (double)summaries[i].last_active));
                sc_json_object_set(alloc, s, "turn_count",
                    sc_json_number_new(alloc, (double)summaries[i].turn_count));
                sc_json_array_push(alloc, arr, s);
            }
            alloc->free(alloc->ctx, summaries,
                count * sizeof(sc_session_summary_t));
        }
    }

    sc_json_object_set(alloc, obj, "sessions", arr);
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── sessions.patch ──────────────────────────────────────────────────── */

static sc_error_t handle_sessions_patch(sc_allocator_t *alloc,
    sc_app_context_t *app, const sc_json_value_t *root,
    char **out, size_t *out_len) {
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
    if (!obj) return SC_ERR_OUT_OF_MEMORY;
    sc_json_object_set(alloc, obj, "patched", sc_json_bool_new(alloc, patched));
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── sessions.delete ─────────────────────────────────────────────────── */

static sc_error_t handle_sessions_delete(sc_allocator_t *alloc,
    sc_app_context_t *app, const sc_json_value_t *root,
    char **out, size_t *out_len) {
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
    if (!obj) return SC_ERR_OUT_OF_MEMORY;
    sc_json_object_set(alloc, obj, "deleted", sc_json_bool_new(alloc, deleted));
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── tools.catalog ───────────────────────────────────────────────────── */

static sc_error_t handle_tools_catalog(sc_allocator_t *alloc,
    const sc_app_context_t *app, char **out, size_t *out_len) {
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj) return SC_ERR_OUT_OF_MEMORY;

    sc_json_value_t *arr = sc_json_array_new(alloc);

    if (app && app->tools) {
        for (size_t i = 0; i < app->tools_count; i++) {
            sc_tool_t *t = &app->tools[i];
            if (!t->vtable) continue;
            sc_json_value_t *tool_obj = sc_json_object_new(alloc);

            const char *name = t->vtable->name ? t->vtable->name(t->ctx) : "";
            const char *desc = t->vtable->description ?
                t->vtable->description(t->ctx) : "";

            json_set_str(alloc, tool_obj, "name", name);
            json_set_str(alloc, tool_obj, "description", desc);

            const char *params = t->vtable->parameters_json ?
                t->vtable->parameters_json(t->ctx) : NULL;
            if (params) {
                json_set_str(alloc, tool_obj, "parameters", params);
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

static sc_error_t handle_channels_status(sc_allocator_t *alloc,
    const sc_app_context_t *app, char **out, size_t *out_len) {
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj) return SC_ERR_OUT_OF_MEMORY;

    sc_json_value_t *arr = sc_json_array_new(alloc);

    size_t catalog_count = 0;
    const sc_channel_meta_t *catalog = sc_channel_catalog_all(&catalog_count);
    for (size_t i = 0; i < catalog_count; i++) {
        sc_json_value_t *ch_obj = sc_json_object_new(alloc);
        json_set_str(alloc, ch_obj, "key", catalog[i].key);
        json_set_str(alloc, ch_obj, "label", catalog[i].label);

        bool build_enabled = sc_channel_catalog_is_build_enabled(catalog[i].id);
        sc_json_object_set(alloc, ch_obj, "build_enabled",
            sc_json_bool_new(alloc, build_enabled));

        bool configured = false;
        if (app && app->config) {
            configured = sc_channel_catalog_is_configured(
                app->config, catalog[i].id);
        }
        sc_json_object_set(alloc, ch_obj, "configured",
            sc_json_bool_new(alloc, configured));

        char status_buf[64];
        const char *status = "unavailable";
        if (app && app->config) {
            status = sc_channel_catalog_status_text(app->config,
                &catalog[i], status_buf, sizeof(status_buf));
        }
        json_set_str(alloc, ch_obj, "status", status);

        sc_json_array_push(alloc, arr, ch_obj);
    }

    sc_json_object_set(alloc, obj, "channels", arr);
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── cron.list ───────────────────────────────────────────────────────── */

static sc_error_t handle_cron_list(sc_allocator_t *alloc,
    const sc_app_context_t *app, char **out, size_t *out_len) {
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj) return SC_ERR_OUT_OF_MEMORY;

    sc_json_value_t *arr = sc_json_array_new(alloc);

    if (app && app->cron) {
        size_t count = 0;
        const sc_cron_job_t *jobs = sc_cron_list_jobs(app->cron, &count);
        for (size_t i = 0; i < count; i++) {
            sc_json_value_t *j = sc_json_object_new(alloc);
            sc_json_object_set(alloc, j, "id",
                sc_json_number_new(alloc, (double)jobs[i].id));
            json_set_str(alloc, j, "name", jobs[i].name);
            json_set_str(alloc, j, "expression", jobs[i].expression);
            json_set_str(alloc, j, "command", jobs[i].command);
            sc_json_object_set(alloc, j, "enabled",
                sc_json_bool_new(alloc, jobs[i].enabled));
            sc_json_object_set(alloc, j, "next_run",
                sc_json_number_new(alloc, (double)jobs[i].next_run_secs));
            sc_json_object_set(alloc, j, "last_run",
                sc_json_number_new(alloc, (double)jobs[i].last_run_secs));
            sc_json_array_push(alloc, arr, j);
        }
    }

    sc_json_object_set(alloc, obj, "jobs", arr);
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── cron.add ────────────────────────────────────────────────────────── */

static sc_error_t handle_cron_add(sc_allocator_t *alloc,
    sc_app_context_t *app, const sc_json_value_t *root,
    char **out, size_t *out_len) {
    bool added = false;
    uint64_t new_id = 0;

    if (root && app && app->cron && app->alloc) {
        sc_json_value_t *params = sc_json_object_get(root, "params");
        if (params) {
            const char *expr = sc_json_get_string(params, "expression");
            const char *cmd = sc_json_get_string(params, "command");
            const char *name = sc_json_get_string(params, "name");
            if (expr && cmd) {
                sc_error_t e = sc_cron_add_job(app->cron, app->alloc,
                    expr, cmd, name ? name : cmd, &new_id);
                added = (e == SC_OK);
            }
        }
    }

    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj) return SC_ERR_OUT_OF_MEMORY;
    sc_json_object_set(alloc, obj, "added", sc_json_bool_new(alloc, added));
    if (added)
        sc_json_object_set(alloc, obj, "id",
            sc_json_number_new(alloc, (double)new_id));
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── cron.remove ─────────────────────────────────────────────────────── */

static sc_error_t handle_cron_remove(sc_allocator_t *alloc,
    sc_app_context_t *app, const sc_json_value_t *root,
    char **out, size_t *out_len) {
    bool removed = false;

    if (root && app && app->cron) {
        sc_json_value_t *params = sc_json_object_get(root, "params");
        if (params) {
            double id_num = sc_json_get_number(params, "id", -1.0);
            if (id_num >= 0) {
                uint64_t job_id = (uint64_t)id_num;
                sc_error_t e = sc_cron_remove_job(app->cron, job_id);
                removed = (e == SC_OK);
            }
        }
    }

    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj) return SC_ERR_OUT_OF_MEMORY;
    sc_json_object_set(alloc, obj, "removed", sc_json_bool_new(alloc, removed));
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── cron.run ────────────────────────────────────────────────────────── */

static sc_error_t handle_cron_run(sc_allocator_t *alloc,
    sc_app_context_t *app, const sc_json_value_t *root,
    char **out, size_t *out_len) {
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
                    sc_error_t e = sc_cron_add_run(app->cron, app->alloc,
                        job_id, (int64_t)time(NULL), "running", NULL);
                    started = (e == SC_OK);
                    status_msg = started ? "started" : "failed to start";
                } else {
                    status_msg = "job not found";
                }
            } else {
                status_msg = "missing job id";
            }
        }
    }

    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj) return SC_ERR_OUT_OF_MEMORY;
    sc_json_object_set(alloc, obj, "started", sc_json_bool_new(alloc, started));
    json_set_str(alloc, obj, "status", status_msg);
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── skills.list ─────────────────────────────────────────────────────── */

static sc_error_t handle_skills_list(sc_allocator_t *alloc,
    const sc_app_context_t *app, char **out, size_t *out_len) {
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj) return SC_ERR_OUT_OF_MEMORY;

    sc_json_value_t *arr = sc_json_array_new(alloc);

    if (app && app->skills) {
        sc_skill_t *skills = NULL;
        size_t count = 0;
        sc_error_t e = sc_skillforge_list_skills(app->skills, &skills, &count);
        if (e == SC_OK && skills) {
            for (size_t i = 0; i < count; i++) {
                sc_json_value_t *s = sc_json_object_new(alloc);
                json_set_str(alloc, s, "name", skills[i].name);
                json_set_str(alloc, s, "description", skills[i].description);
                sc_json_object_set(alloc, s, "enabled",
                    sc_json_bool_new(alloc, skills[i].enabled));
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

static sc_error_t handle_skill_toggle(sc_allocator_t *alloc,
    sc_app_context_t *app, const sc_json_value_t *root,
    bool enable, char **out, size_t *out_len) {
    bool success = false;

    if (root && app && app->skills) {
        sc_json_value_t *params = sc_json_object_get(root, "params");
        if (params) {
            const char *name = sc_json_get_string(params, "name");
            if (name) {
                sc_error_t e = enable ?
                    sc_skillforge_enable(app->skills, name) :
                    sc_skillforge_disable(app->skills, name);
                success = (e == SC_OK);
            }
        }
    }

    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj) return SC_ERR_OUT_OF_MEMORY;
    sc_json_object_set(alloc, obj,
        enable ? "enabled" : "disabled",
        sc_json_bool_new(alloc, success));
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── skills.install ──────────────────────────────────────────────────── */

static sc_error_t handle_skills_install(sc_allocator_t *alloc,
    sc_app_context_t *app, const sc_json_value_t *root,
    char **out, size_t *out_len) {
    (void)app;
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
    if (!obj) return SC_ERR_OUT_OF_MEMORY;
    sc_json_object_set(alloc, obj, "installed",
        sc_json_bool_new(alloc, installed));
    if (error_msg) json_set_str(alloc, obj, "error", error_msg);
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── update.check ────────────────────────────────────────────────────── */

static sc_error_t handle_update_check(sc_allocator_t *alloc, char **out,
    size_t *out_len) {
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj) return SC_ERR_OUT_OF_MEMORY;
    sc_json_object_set(alloc, obj, "available", sc_json_bool_new(alloc, false));
    json_set_str(alloc, obj, "current", "0.1.0");
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── update.run ──────────────────────────────────────────────────────── */

static sc_error_t handle_update_run(sc_allocator_t *alloc, char **out,
    size_t *out_len) {
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj) return SC_ERR_OUT_OF_MEMORY;
    json_set_str(alloc, obj, "status", "up_to_date");
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── exec.approval.resolve ───────────────────────────────────────────── */

static sc_error_t handle_exec_approval(sc_allocator_t *alloc,
    const sc_app_context_t *app, const sc_json_value_t *root,
    char **out, size_t *out_len) {
    bool resolved = false;
    bool approved = false;

    if (root && app && app->bus) {
        sc_json_value_t *params = sc_json_object_get(root, "params");
        if (params) {
            const char *id = sc_json_get_string(params, "id");
            approved = sc_json_get_bool(params, "approved", false);
            if (id && id[0]) {
                sc_bus_publish_simple(app->bus,
                    approved ? SC_BUS_TOOL_CALL : SC_BUS_ERROR,
                    "approval", id,
                    approved ? "approved" : "denied");
                resolved = true;
            }
        }
    }

    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj) return SC_ERR_OUT_OF_MEMORY;
    sc_json_object_set(alloc, obj, "resolved",
        sc_json_bool_new(alloc, resolved));
    sc_json_object_set(alloc, obj, "approved",
        sc_json_bool_new(alloc, approved));
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── usage.summary ───────────────────────────────────────────────────── */

static sc_error_t handle_usage_summary(sc_allocator_t *alloc,
    const sc_app_context_t *app, char **out, size_t *out_len) {
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj) return SC_ERR_OUT_OF_MEMORY;

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
        sc_json_object_set(alloc, obj, "session_cost_usd",
            sc_json_number_new(alloc, 0));
        sc_json_object_set(alloc, obj, "daily_cost_usd",
            sc_json_number_new(alloc, 0));
        sc_json_object_set(alloc, obj, "monthly_cost_usd",
            sc_json_number_new(alloc, 0));
        sc_json_object_set(alloc, obj, "total_tokens",
            sc_json_number_new(alloc, 0));
        sc_json_object_set(alloc, obj, "request_count",
            sc_json_number_new(alloc, 0));
    }

    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── Method dispatcher ───────────────────────────────────────────────── */

static sc_error_t build_method_response(sc_allocator_t *alloc,
    const char *method, const sc_json_value_t *root,
    sc_app_context_t *app, char **payload_out, size_t *payload_len_out) {
    *payload_out = NULL;
    *payload_len_out = 0;

    if (strcmp(method, "connect") == 0)
        return build_connect_response(alloc, app, payload_out, payload_len_out);
    if (strcmp(method, "health") == 0)
        return handle_health(alloc, payload_out, payload_len_out);
    if (strcmp(method, "config.get") == 0)
        return handle_config_get(alloc, app, payload_out, payload_len_out);
    if (strcmp(method, "config.schema") == 0)
        return handle_config_schema(alloc, payload_out, payload_len_out);
    if (strcmp(method, "capabilities") == 0)
        return handle_capabilities(alloc, app, payload_out, payload_len_out);
    if (strcmp(method, "chat.send") == 0)
        return handle_chat_send(alloc, app, root, payload_out, payload_len_out);
    if (strcmp(method, "chat.history") == 0)
        return handle_chat_history(alloc, app, root, payload_out, payload_len_out);
    if (strcmp(method, "chat.abort") == 0)
        return handle_chat_abort(alloc, app, payload_out, payload_len_out);
    if (strcmp(method, "config.set") == 0)
        return handle_config_set(alloc, app, root, payload_out, payload_len_out);
    if (strcmp(method, "config.apply") == 0)
        return handle_config_apply(alloc, app, root, payload_out, payload_len_out);
    if (strcmp(method, "sessions.list") == 0)
        return handle_sessions_list(alloc, app, payload_out, payload_len_out);
    if (strcmp(method, "sessions.patch") == 0)
        return handle_sessions_patch(alloc, app, root, payload_out, payload_len_out);
    if (strcmp(method, "sessions.delete") == 0)
        return handle_sessions_delete(alloc, app, root, payload_out, payload_len_out);
    if (strcmp(method, "tools.catalog") == 0)
        return handle_tools_catalog(alloc, app, payload_out, payload_len_out);
    if (strcmp(method, "channels.status") == 0)
        return handle_channels_status(alloc, app, payload_out, payload_len_out);
    if (strcmp(method, "cron.list") == 0)
        return handle_cron_list(alloc, app, payload_out, payload_len_out);
    if (strcmp(method, "cron.add") == 0)
        return handle_cron_add(alloc, app, root, payload_out, payload_len_out);
    if (strcmp(method, "cron.remove") == 0)
        return handle_cron_remove(alloc, app, root, payload_out, payload_len_out);
    if (strcmp(method, "cron.run") == 0)
        return handle_cron_run(alloc, app, root, payload_out, payload_len_out);
    if (strcmp(method, "skills.list") == 0)
        return handle_skills_list(alloc, app, payload_out, payload_len_out);
    if (strcmp(method, "skills.enable") == 0)
        return handle_skill_toggle(alloc, app, root, true, payload_out, payload_len_out);
    if (strcmp(method, "skills.disable") == 0)
        return handle_skill_toggle(alloc, app, root, false, payload_out, payload_len_out);
    if (strcmp(method, "skills.install") == 0)
        return handle_skills_install(alloc, app, root, payload_out, payload_len_out);
    if (strcmp(method, "update.check") == 0)
        return handle_update_check(alloc, payload_out, payload_len_out);
    if (strcmp(method, "update.run") == 0)
        return handle_update_run(alloc, payload_out, payload_len_out);
    if (strcmp(method, "exec.approval.resolve") == 0)
        return handle_exec_approval(alloc, app, root, payload_out, payload_len_out);
    if (strcmp(method, "usage.summary") == 0)
        return handle_usage_summary(alloc, app, payload_out, payload_len_out);

    return SC_ERR_NOT_FOUND;
}

#endif /* SC_GATEWAY_POSIX */

/* ── Protocol lifecycle ──────────────────────────────────────────────── */

void sc_control_protocol_init(sc_control_protocol_t *proto,
    sc_allocator_t *alloc, sc_ws_server_t *ws) {
    if (!proto) return;
    proto->alloc = alloc;
    proto->ws = ws;
    proto->event_seq = 0;
    proto->app_ctx = NULL;
}

void sc_control_protocol_deinit(sc_control_protocol_t *proto) {
    (void)proto;
}

void sc_control_set_app_ctx(sc_control_protocol_t *proto,
    sc_app_context_t *ctx) {
    if (proto) proto->app_ctx = ctx;
}

/* ── Incoming message handler ────────────────────────────────────────── */

void sc_control_on_message(sc_ws_conn_t *conn, const char *data, size_t data_len,
    void *ctx) {
    sc_control_protocol_t *proto = (sc_control_protocol_t *)ctx;
    if (!proto || !proto->alloc || !proto->ws) return;

#ifdef SC_GATEWAY_POSIX
    sc_json_value_t *root = NULL;
    if (sc_json_parse(proto->alloc, data, data_len, &root) != SC_OK)
        return;
    if (!root || root->type != SC_JSON_OBJECT) {
        if (root) sc_json_free(proto->alloc, root);
        return;
    }

    const char *type_str = sc_json_get_string(root, "type");
    if (!type_str || strcmp(type_str, "req") != 0) {
        sc_json_free(proto->alloc, root);
        return;
    }

    const char *id_raw = sc_json_get_string(root, "id");
    const char *method = sc_json_get_string(root, "method");

    if (!id_raw || !method) {
        sc_json_free(proto->alloc, root);
        return;
    }

    size_t id_slen = strlen(id_raw);
    char *id = (char *)proto->alloc->alloc(proto->alloc->ctx, id_slen + 1);
    if (!id) {
        sc_json_free(proto->alloc, root);
        return;
    }
    memcpy(id, id_raw, id_slen + 1);

    char *payload = NULL;
    size_t payload_len = 0;
    sc_error_t err = build_method_response(proto->alloc, method, root,
        proto->app_ctx, &payload, &payload_len);
    sc_json_free(proto->alloc, root);

    bool ok = (err == SC_OK);
    if (!payload && ok) {
        sc_json_value_t *empty = sc_json_object_new(proto->alloc);
        if (empty) {
            sc_json_stringify(proto->alloc, empty, &payload, &payload_len);
            sc_json_free(proto->alloc, empty);
        }
        if (!payload) payload = (char *)proto->alloc->alloc(proto->alloc->ctx, 3);
        if (payload && !payload_len) {
            memcpy(payload, "{}", 3);
            payload_len = 2;
        }
    } else if (err != SC_OK) {
        payload = (char *)proto->alloc->alloc(proto->alloc->ctx, 64);
        if (payload) {
            size_t n = 0;
            if (err == SC_ERR_NOT_FOUND) {
                n = (size_t)snprintf(payload, 64, "{\"error\":\"unknown method\"}");
            } else {
                n = (size_t)snprintf(payload, 64, "{\"error\":\"internal\"}");
            }
            payload_len = n;
        }
    }

    if (payload) {
        size_t res_cap = 256 + payload_len;
        char *res_buf = (char *)proto->alloc->alloc(proto->alloc->ctx, res_cap);
        if (res_buf) {
            size_t pos = 0;
            pos += (size_t)snprintf(res_buf, res_cap,
                "{\"type\":\"res\",\"id\":\"");
            size_t id_len = strlen(id);
            size_t esc_len = id_len * 2 + 16;
            char *id_esc = (char *)proto->alloc->alloc(proto->alloc->ctx, esc_len);
            if (id_esc) {
                size_t j = 0;
                for (size_t i = 0; i < id_len && j + 4 < esc_len; i++) {
                    char c = id[i];
                    if (c == '"' || c == '\\') {
                        id_esc[j++] = '\\';
                        id_esc[j++] = c;
                    } else {
                        id_esc[j++] = c;
                    }
                }
                id_esc[j] = '\0';
                pos += (size_t)snprintf(res_buf + pos, res_cap - pos, "%s\"", id_esc);
                proto->alloc->free(proto->alloc->ctx, id_esc, esc_len);
            } else {
                for (size_t i = 0; i < id_len && pos + 4 < res_cap; i++) {
                    char c = id[i];
                    if (c == '"' || c == '\\') res_buf[pos++] = '\\';
                    res_buf[pos++] = c;
                }
                if (pos < res_cap) res_buf[pos++] = '"';
            }
            pos += (size_t)snprintf(res_buf + pos, res_cap - pos,
                ",\"ok\":%s,\"payload\":", ok ? "true" : "false");
            memcpy(res_buf + pos, payload, payload_len);
            pos += payload_len;
            res_buf[pos++] = '}';
            res_buf[pos] = '\0';
            sc_ws_server_send(conn, res_buf, pos);
            proto->alloc->free(proto->alloc->ctx, res_buf, res_cap);
        }
        if (err == SC_OK && payload) {
            proto->alloc->free(proto->alloc->ctx, payload, payload_len + 1);
        } else if (payload) {
            proto->alloc->free(proto->alloc->ctx, payload, 64);
        }
    }
    proto->alloc->free(proto->alloc->ctx, id, id_slen + 1);
#endif
}

void sc_control_on_close(sc_ws_conn_t *conn, void *ctx) {
    (void)conn;
    (void)ctx;
}

/* ── Outbound helpers ────────────────────────────────────────────────── */

void sc_control_send_event(sc_control_protocol_t *proto, const char *event_name,
    const char *payload_json) {
    if (!proto || !proto->ws || !event_name) return;

#ifdef SC_GATEWAY_POSIX
    uint64_t seq = proto->event_seq++;
    const char *payload = payload_json ? payload_json : "{}";
    size_t payload_len = strlen(payload);
    size_t event_len = strlen(event_name);

    size_t cap = 128 + event_len + payload_len;
    char *buf = (char *)proto->alloc->alloc(proto->alloc->ctx, cap);
    if (!buf) return;

    int n = snprintf(buf, cap,
        "{\"type\":\"event\",\"event\":\"%s\",\"payload\":%s,\"seq\":%llu}",
        event_name, payload, (unsigned long long)seq);

    if (n > 0 && (size_t)n < cap)
        sc_ws_server_broadcast(proto->ws, buf, (size_t)n);

    proto->alloc->free(proto->alloc->ctx, buf, cap);
#endif
}

sc_error_t sc_control_send_response(sc_ws_conn_t *conn, const char *id,
    bool ok, const char *payload_json) {
    if (!conn || !id) return SC_ERR_INVALID_ARGUMENT;

#ifdef SC_GATEWAY_POSIX
    sc_allocator_t alloc = sc_system_allocator();
    const char *payload = payload_json ? payload_json : "{}";
    size_t payload_len = strlen(payload);
    size_t id_len = strlen(id);
    size_t cap = 96 + id_len * 2 + payload_len;

    char *buf = (char *)alloc.alloc(alloc.ctx, cap);
    if (!buf) return SC_ERR_OUT_OF_MEMORY;

    size_t pos = 0;
    pos += (size_t)snprintf(buf + pos, cap - pos,
        "{\"type\":\"res\",\"id\":\"");
    for (size_t i = 0; i < id_len && pos + 4 < cap; i++) {
        char c = id[i];
        if (c == '"' || c == '\\') { buf[pos++] = '\\'; buf[pos++] = c; }
        else buf[pos++] = c;
    }
    pos += (size_t)snprintf(buf + pos, cap - pos,
        "\",\"ok\":%s,\"payload\":%s}",
        ok ? "true" : "false", payload);

    sc_error_t err = sc_ws_server_send(conn, buf, pos);
    alloc.free(alloc.ctx, buf, cap);
    return err;
#else
    (void)ok;
    (void)payload_json;
    return SC_ERR_NOT_SUPPORTED;
#endif
}
