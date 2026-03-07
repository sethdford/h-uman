#include "seaclaw/gateway/control_protocol.h"
#include "seaclaw/agent.h"
#include "seaclaw/agent/awareness.h"
#include "seaclaw/bus.h"
#include "seaclaw/channel_catalog.h"
#include "seaclaw/config.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/process_util.h"
#include "seaclaw/cost.h"
#include "seaclaw/gateway/oauth.h"
#include "seaclaw/security.h"
#ifdef SC_HAS_CRON
#include "seaclaw/cron.h"
#include "seaclaw/crontab.h"
#endif
#ifdef SC_HAS_PUSH
#include "seaclaw/gateway/push.h"
#endif
#include "seaclaw/session.h"
#ifdef SC_HAS_SKILLS
#include "seaclaw/skill_registry.h"
#include "seaclaw/skillforge.h"
#endif
#include "seaclaw/tool.h"
#ifdef SC_HAS_UPDATE
#include "seaclaw/update.h"
#endif
#include "seaclaw/version.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef SC_GATEWAY_POSIX

/* ── JSON helper: add string field ───────────────────────────────────── */

static void json_set_str(sc_allocator_t *a, sc_json_value_t *obj, const char *key,
                         const char *val) {
    if (!val)
        val = "";
    sc_json_object_set(a, obj, key, sc_json_string_new(a, val, strlen(val)));
}

/* ── connect ─────────────────────────────────────────────────────────── */

static sc_error_t build_connect_response(sc_allocator_t *alloc, const sc_app_context_t *app,
                                         char **out, size_t *out_len) {
    sc_json_value_t *root = sc_json_object_new(alloc);
    if (!root)
        return SC_ERR_OUT_OF_MEMORY;

    json_set_str(alloc, root, "type", "hello-ok");

    sc_json_value_t *server = sc_json_object_new(alloc);
    json_set_str(alloc, server, "version", sc_version_string());
    sc_json_object_set(alloc, root, "server", server);
    sc_json_object_set(alloc, root, "protocol", sc_json_number_new(alloc, 1));

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

    sc_json_object_set(alloc, root, "features", features);

    sc_error_t err = sc_json_stringify(alloc, root, out, out_len);
    sc_json_free(alloc, root);
    return err;
}

/* ── health ──────────────────────────────────────────────────────────── */

static sc_error_t handle_health(sc_allocator_t *alloc, char **out, size_t *out_len) {
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;
    json_set_str(alloc, obj, "status", "ok");
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── config.get ──────────────────────────────────────────────────────── */

static sc_error_t handle_config_get(sc_allocator_t *alloc, const sc_app_context_t *app, char **out,
                                    size_t *out_len) {
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;

    if (app && app->config) {
        sc_json_object_set(alloc, obj, "exists", sc_json_bool_new(alloc, true));
        json_set_str(alloc, obj, "workspace_dir", app->config->workspace_dir);
        json_set_str(alloc, obj, "default_provider", app->config->default_provider);
        json_set_str(alloc, obj, "default_model", app->config->default_model);
        sc_json_object_set(alloc, obj, "max_tokens",
                           sc_json_number_new(alloc, (double)app->config->max_tokens));
        sc_json_object_set(alloc, obj, "temperature",
                           sc_json_number_new(alloc, app->config->temperature));

        sc_json_value_t *sec = sc_json_object_new(alloc);
        if (sec) {
            sc_json_object_set(alloc, sec, "autonomy_level",
                               sc_json_number_new(alloc, app->config->security.autonomy_level));
            json_set_str(alloc, sec, "sandbox", app->config->security.sandbox);
            sc_json_value_t *sbc = sc_json_object_new(alloc);
            if (sbc) {
                sc_json_object_set(
                    alloc, sbc, "enabled",
                    sc_json_bool_new(alloc, app->config->security.sandbox_config.enabled));
                json_set_str(alloc, sbc, "backend",
                             app->config->security.sandbox ? app->config->security.sandbox
                                                           : "auto");
                sc_json_value_t *np = sc_json_object_new(alloc);
                if (np) {
                    sc_json_object_set(
                        alloc, np, "enabled",
                        sc_json_bool_new(alloc,
                                         app->config->security.sandbox_config.net_proxy.enabled));
                    sc_json_object_set(
                        alloc, np, "deny_all",
                        sc_json_bool_new(alloc,
                                         app->config->security.sandbox_config.net_proxy.deny_all));
                    json_set_str(alloc, np, "proxy_addr",
                                 app->config->security.sandbox_config.net_proxy.proxy_addr);
                    sc_json_object_set(alloc, sbc, "net_proxy", np);
                }
                sc_json_object_set(alloc, sec, "sandbox_config", sbc);
            }
            sc_json_object_set(alloc, obj, "security", sec);
        }
    } else {
        sc_json_object_set(alloc, obj, "exists", sc_json_bool_new(alloc, false));
    }

    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── config.schema ───────────────────────────────────────────────────── */

static sc_error_t handle_config_schema(sc_allocator_t *alloc, char **out, size_t *out_len) {
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;

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

static sc_error_t handle_capabilities(sc_allocator_t *alloc, const sc_app_context_t *app,
                                      char **out, size_t *out_len) {
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;

    json_set_str(alloc, obj, "version", sc_version_string());

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

/* ── chat.send ───────────────────────────────────────────────────────── */

static sc_error_t handle_chat_send(sc_allocator_t *alloc, const sc_app_context_t *app,
                                   const sc_json_value_t *root, char **out, size_t *out_len) {
    const char *message = NULL;
    const char *session_key = "default";

    if (root) {
        sc_json_value_t *params = sc_json_object_get(root, "params");
        if (params) {
            const char *m = sc_json_get_string(params, "message");
            if (m)
                message = m;
            const char *sk = sc_json_get_string(params, "sessionKey");
            if (sk)
                session_key = sk;
        }
    }

    if (!message || !message[0]) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj)
            return SC_ERR_OUT_OF_MEMORY;
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
        if (ml >= SC_BUS_MSG_LEN)
            ml = SC_BUS_MSG_LEN - 1;
        memcpy(ev.message, message, ml);
        ev.message[ml] = '\0';
        sc_bus_publish(app->bus, &ev);
    }

    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;
    json_set_str(alloc, obj, "status", "queued");
    json_set_str(alloc, obj, "sessionKey", session_key);
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── chat.history ────────────────────────────────────────────────────── */

static sc_error_t handle_chat_history(sc_allocator_t *alloc, const sc_app_context_t *app,
                                      const sc_json_value_t *root, char **out, size_t *out_len) {
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;

    sc_json_value_t *msgs = sc_json_array_new(alloc);

    const char *session_key = "default";
    if (root) {
        sc_json_value_t *params = sc_json_object_get(root, "params");
        if (params) {
            const char *sk = sc_json_get_string(params, "sessionKey");
            if (sk)
                session_key = sk;
        }
    }

    if (app && app->sessions) {
        sc_session_t *sess = sc_session_get_or_create(app->sessions, session_key);
        if (sess) {
            for (size_t i = 0; i < sess->message_count; i++) {
                sc_json_value_t *m_obj = sc_json_object_new(alloc);
                json_set_str(alloc, m_obj, "role", sess->messages[i].role);
                if (sess->messages[i].content)
                    json_set_str(alloc, m_obj, "content", sess->messages[i].content);
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

static sc_error_t handle_chat_abort(sc_allocator_t *alloc, const sc_app_context_t *app, char **out,
                                    size_t *out_len) {
    bool aborted = false;
    if (app && app->bus) {
        sc_bus_publish_simple(app->bus, SC_BUS_ERROR, "control-ui", "", "chat.abort");
        aborted = true;
    }
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;
    sc_json_object_set(alloc, obj, "aborted", sc_json_bool_new(alloc, aborted));
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── config.set ──────────────────────────────────────────────────────── */

static sc_error_t handle_config_set(sc_allocator_t *alloc, sc_app_context_t *app,
                                    const sc_json_value_t *root, char **out, size_t *out_len) {
    bool saved = false;

    if (root && app && app->config) {
        sc_json_value_t *params = sc_json_object_get(root, "params");
        if (params) {
            const char *raw = sc_json_get_string(params, "raw");
            if (raw) {
                sc_error_t parse_err = sc_config_parse_json(app->config, raw, strlen(raw));
                if (parse_err == SC_OK) {
                    sc_error_t save_err = sc_config_save(app->config);
                    saved = (save_err == SC_OK);
                }
            }
        }
    }

    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;
    sc_json_object_set(alloc, obj, "saved", sc_json_bool_new(alloc, saved));
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── config.apply ────────────────────────────────────────────────────── */

static sc_error_t handle_config_apply(sc_allocator_t *alloc, sc_app_context_t *app,
                                      const sc_json_value_t *root, char **out, size_t *out_len) {
    bool applied = false;
    bool saved = false;

    if (root && app && app->config) {
        sc_json_value_t *params = sc_json_object_get(root, "params");
        if (params) {
            const char *raw = sc_json_get_string(params, "raw");
            if (raw) {
                sc_error_t parse_err = sc_config_parse_json(app->config, raw, strlen(raw));
                if (parse_err == SC_OK) {
                    applied = true;
                    sc_error_t save_err = sc_config_save(app->config);
                    saved = (save_err == SC_OK);
                }
            }
        }
    }

    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;
    sc_json_object_set(alloc, obj, "applied", sc_json_bool_new(alloc, applied));
    sc_json_object_set(alloc, obj, "saved", sc_json_bool_new(alloc, saved));
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── sessions.list ───────────────────────────────────────────────────── */

static sc_error_t handle_sessions_list(sc_allocator_t *alloc, const sc_app_context_t *app,
                                       char **out, size_t *out_len) {
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
            alloc->free(alloc->ctx, summaries, count * sizeof(sc_session_summary_t));
        }
    }

    sc_json_object_set(alloc, obj, "sessions", arr);
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── sessions.patch ──────────────────────────────────────────────────── */

static sc_error_t handle_sessions_patch(sc_allocator_t *alloc, sc_app_context_t *app,
                                        const sc_json_value_t *root, char **out, size_t *out_len) {
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

/* ── persona.set ─────────────────────────────────────────────────────── */

static sc_error_t handle_persona_set(sc_allocator_t *alloc, sc_app_context_t *app,
                                     const sc_json_value_t *root, char **out, size_t *out_len) {
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;

    if (!app || !app->agent) {
        json_set_str(alloc, obj, "error", "no agent (gateway must run with --with-agent)");
        sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
        sc_json_free(alloc, obj);
        return err;
    }

    if (!root) {
        json_set_str(alloc, obj, "error", "params required");
        sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
        sc_json_free(alloc, obj);
        return err;
    }

    sc_json_value_t *params = sc_json_object_get(root, "params");
    sc_json_value_t *name_val = params ? sc_json_object_get(params, "name") : NULL;

    if (!name_val) {
        json_set_str(alloc, obj, "error", "name is required");
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
        json_set_str(alloc, obj, "error", "name must be string or null");
        sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
        sc_json_free(alloc, obj);
        return err;
    }

#ifdef SC_HAS_PERSONA
    sc_error_t err = sc_agent_set_persona(app->agent, name, name_len);
    if (err != SC_OK) {
        const char *emsg = sc_error_string(err);
        json_set_str(alloc, obj, "error", emsg ? emsg : "failed to set persona");
        sc_error_t serr = sc_json_stringify(alloc, obj, out, out_len);
        sc_json_free(alloc, obj);
        return serr;
    }
#else
    (void)name;
    (void)name_len;
    json_set_str(alloc, obj, "error", "persona support not built (SC_ENABLE_PERSONA=OFF)");
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
#endif

    sc_json_object_set(alloc, obj, "ok", sc_json_bool_new(alloc, true));
    sc_error_t serr = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return serr;
}

/* ── sessions.delete ─────────────────────────────────────────────────── */

static sc_error_t handle_sessions_delete(sc_allocator_t *alloc, sc_app_context_t *app,
                                         const sc_json_value_t *root, char **out, size_t *out_len) {
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

/* ── tools.catalog ───────────────────────────────────────────────────── */

static sc_error_t handle_tools_catalog(sc_allocator_t *alloc, const sc_app_context_t *app,
                                       char **out, size_t *out_len) {
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

            json_set_str(alloc, tool_obj, "name", name);
            json_set_str(alloc, tool_obj, "description", desc);

            const char *params =
                t->vtable->parameters_json ? t->vtable->parameters_json(t->ctx) : NULL;
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

static sc_error_t handle_channels_status(sc_allocator_t *alloc, const sc_app_context_t *app,
                                         char **out, size_t *out_len) {
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;

    sc_json_value_t *arr = sc_json_array_new(alloc);

    size_t catalog_count = 0;
    const sc_channel_meta_t *catalog = sc_channel_catalog_all(&catalog_count);
    for (size_t i = 0; i < catalog_count; i++) {
        sc_json_value_t *ch_obj = sc_json_object_new(alloc);
        json_set_str(alloc, ch_obj, "key", catalog[i].key);
        json_set_str(alloc, ch_obj, "label", catalog[i].label);

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
        json_set_str(alloc, ch_obj, "status", status);

        sc_json_array_push(alloc, arr, ch_obj);
    }

    sc_json_object_set(alloc, obj, "channels", arr);
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── cron.list ───────────────────────────────────────────────────────── */
#ifdef SC_HAS_CRON

static sc_error_t handle_cron_list(sc_allocator_t *alloc, const sc_app_context_t *app, char **out,
                                   size_t *out_len) {
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
            json_set_str(alloc, j, "name", jobs[i].name);
            json_set_str(alloc, j, "expression", jobs[i].expression);
            json_set_str(alloc, j, "command", jobs[i].command);
            sc_json_object_set(alloc, j, "enabled", sc_json_bool_new(alloc, jobs[i].enabled));
            sc_json_object_set(alloc, j, "next_run",
                               sc_json_number_new(alloc, (double)jobs[i].next_run_secs));
            sc_json_object_set(alloc, j, "last_run",
                               sc_json_number_new(alloc, (double)jobs[i].last_run_secs));
            json_set_str(alloc, j, "type", jobs[i].type == SC_CRON_JOB_AGENT ? "agent" : "shell");
            json_set_str(alloc, j, "channel", jobs[i].channel);
            json_set_str(alloc, j, "last_status", jobs[i].last_status);
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

static sc_error_t handle_cron_add(sc_allocator_t *alloc, sc_app_context_t *app,
                                  const sc_json_value_t *root, char **out, size_t *out_len) {
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

static sc_error_t handle_cron_remove(sc_allocator_t *alloc, sc_app_context_t *app,
                                     const sc_json_value_t *root, char **out, size_t *out_len) {
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

static sc_error_t handle_cron_run(sc_allocator_t *alloc, sc_app_context_t *app,
                                  const sc_json_value_t *root, char **out, size_t *out_len) {
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
    json_set_str(alloc, obj, "status", status_msg);
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── cron.update ─────────────────────────────────────────────────────── */

static sc_error_t handle_cron_update(sc_allocator_t *alloc, sc_app_context_t *app,
                                     const sc_json_value_t *root, char **out, size_t *out_len) {
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

static sc_error_t handle_cron_runs(sc_allocator_t *alloc, const sc_app_context_t *app,
                                   const sc_json_value_t *root, char **out, size_t *out_len) {
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
                json_set_str(alloc, r, "status", runs[i].status);
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

/* ── skills.list ─────────────────────────────────────────────────────── */
#ifdef SC_HAS_SKILLS

static sc_error_t handle_skills_list(sc_allocator_t *alloc, const sc_app_context_t *app, char **out,
                                     size_t *out_len) {
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
                json_set_str(alloc, s, "name", skills[i].name);
                json_set_str(alloc, s, "description", skills[i].description);
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

static sc_error_t handle_skill_toggle(sc_allocator_t *alloc, sc_app_context_t *app,
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

/* ── skills.install ──────────────────────────────────────────────────── */

static sc_error_t handle_skills_install(sc_allocator_t *alloc, sc_app_context_t *app,
                                        const sc_json_value_t *root, char **out, size_t *out_len) {
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
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;
    sc_json_object_set(alloc, obj, "installed", sc_json_bool_new(alloc, installed));
    if (error_msg)
        json_set_str(alloc, obj, "error", error_msg);
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── skills.search ───────────────────────────────────────────────────── */

static sc_error_t handle_skills_search(sc_allocator_t *alloc, const sc_json_value_t *root,
                                       char **out, size_t *out_len) {
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
            json_set_str(alloc, entry, "name", entries[i].name);
            if (entries[i].description)
                json_set_str(alloc, entry, "description", entries[i].description);
            if (entries[i].version)
                json_set_str(alloc, entry, "version", entries[i].version);
            if (entries[i].author)
                json_set_str(alloc, entry, "author", entries[i].author);
            if (entries[i].url)
                json_set_str(alloc, entry, "url", entries[i].url);
            if (entries[i].tags)
                json_set_str(alloc, entry, "tags", entries[i].tags);
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

static sc_error_t handle_skills_uninstall(sc_allocator_t *alloc, sc_app_context_t *app,
                                          const sc_json_value_t *root, char **out,
                                          size_t *out_len) {
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

static sc_error_t handle_skills_update(sc_allocator_t *alloc, char **out, size_t *out_len) {
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

/* ── models.list ─────────────────────────────────────────────────────── */

static sc_error_t handle_models_list(sc_allocator_t *alloc, const sc_app_context_t *app, char **out,
                                     size_t *out_len) {
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;

    sc_json_value_t *arr = sc_json_array_new(alloc);

    if (app && app->config && app->config->providers) {
        for (size_t i = 0; i < app->config->providers_len; i++) {
            sc_provider_entry_t *pe = &app->config->providers[i];
            sc_json_value_t *p = sc_json_object_new(alloc);
            json_set_str(alloc, p, "name", pe->name);
            sc_json_object_set(alloc, p, "has_key",
                               sc_json_bool_new(alloc, pe->api_key && pe->api_key[0]));
            json_set_str(alloc, p, "base_url", pe->base_url);
            sc_json_object_set(alloc, p, "native_tools", sc_json_bool_new(alloc, pe->native_tools));
            sc_json_object_set(
                alloc, p, "is_default",
                sc_json_bool_new(alloc, app->config->default_provider &&
                                            strcmp(pe->name, app->config->default_provider) == 0));
            sc_json_array_push(alloc, arr, p);
        }
    }

    json_set_str(alloc, obj, "default_model",
                 (app && app->config) ? app->config->default_model : "");
    sc_json_object_set(alloc, obj, "providers", arr);
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── nodes.list ──────────────────────────────────────────────────────── */

static sc_error_t handle_nodes_list(sc_allocator_t *alloc, const sc_app_context_t *app, char **out,
                                    size_t *out_len) {
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
            json_set_str(alloc, n, "id", name);
            json_set_str(alloc, n, "type", "gateway");
            json_set_str(alloc, n, "status", status);
            if (strcmp(name, "local") == 0) {
                json_set_str(alloc, n, "version", sc_version_string());
#if !defined(SC_IS_TEST)
                {
                    char hostname[256] = {0};
                    if (gethostname(hostname, sizeof(hostname) - 1) == 0)
                        json_set_str(alloc, n, "hostname", hostname);
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
        json_set_str(alloc, local, "id", default_id);
        json_set_str(alloc, local, "type", "gateway");
        json_set_str(alloc, local, "status", default_status);
        json_set_str(alloc, local, "version", sc_version_string());
#if !defined(SC_IS_TEST)
        {
            char hostname[256] = {0};
            if (gethostname(hostname, sizeof(hostname) - 1) == 0)
                json_set_str(alloc, local, "hostname", hostname);
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

/* ── update.check ────────────────────────────────────────────────────── */
#ifdef SC_HAS_UPDATE

static sc_error_t handle_update_check(sc_allocator_t *alloc, char **out, size_t *out_len) {
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;
    const char *current = sc_version_string();
    json_set_str(alloc, obj, "current", current);
    char latest[64] = {0};
    sc_error_t check_err = sc_update_check(latest, sizeof(latest));
    if (check_err == SC_OK && latest[0]) {
        json_set_str(alloc, obj, "latest", latest);
        sc_json_object_set(alloc, obj, "available",
                           sc_json_bool_new(alloc, strcmp(latest, current) != 0));
    } else {
        json_set_str(alloc, obj, "latest", current);
        sc_json_object_set(alloc, obj, "available", sc_json_bool_new(alloc, false));
        if (check_err != SC_OK)
            json_set_str(alloc, obj, "error", sc_error_string(check_err));
    }
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── update.run ──────────────────────────────────────────────────────── */

static sc_error_t handle_update_run(sc_allocator_t *alloc, char **out, size_t *out_len) {
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;
    sc_error_t apply_err = sc_update_apply();
    if (apply_err == SC_OK)
        json_set_str(alloc, obj, "status", "updated");
    else {
        json_set_str(alloc, obj, "status", sc_error_string(apply_err));
        json_set_str(alloc, obj, "error", sc_error_string(apply_err));
    }
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

#endif /* SC_HAS_UPDATE */

/* ── exec.approval.resolve ───────────────────────────────────────────── */

static sc_error_t handle_exec_approval(sc_allocator_t *alloc, const sc_app_context_t *app,
                                       const sc_json_value_t *root, char **out, size_t *out_len) {
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

/* ── activity.recent ─────────────────────────────────────────────────── */

static sc_error_t handle_activity_recent(sc_allocator_t *alloc, const sc_app_context_t *app,
                                         char **out, size_t *out_len) {
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
                    json_set_str(alloc, ev, "type", "error");
                    json_set_str(alloc, ev, "message", s->recent_errors[idx].text);
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
                    json_set_str(alloc, ev, "type", "system");
                    char msg[128];
                    snprintf(msg, sizeof(msg), "%llu messages received, %llu tool calls",
                             (unsigned long long)s->messages_received,
                             (unsigned long long)s->tool_calls);
                    json_set_str(alloc, ev, "message", msg);
                    sc_json_object_set(alloc, ev, "time",
                                       sc_json_number_new(alloc, (double)now_ms));
                    sc_json_array_push(alloc, events_arr, ev);
                }
            }

            if (s->health_degraded) {
                sc_json_value_t *ev = sc_json_object_new(alloc);
                if (ev) {
                    json_set_str(alloc, ev, "type", "error");
                    json_set_str(alloc, ev, "message", "System health is degraded");
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

/* ── usage.summary ───────────────────────────────────────────────────── */

static sc_error_t handle_usage_summary(sc_allocator_t *alloc, const sc_app_context_t *app,
                                       char **out, size_t *out_len) {
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

/* ── push.register ──────────────────────────────────────────────────── */
#ifdef SC_HAS_PUSH

static sc_error_t handle_push_register(sc_allocator_t *alloc, sc_app_context_t *app,
                                       const sc_json_value_t *root, char **out, size_t *out_len) {
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
        json_set_str(alloc, obj, "error", error_msg);
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── push.unregister ────────────────────────────────────────────────── */

static sc_error_t handle_push_unregister(sc_allocator_t *alloc, sc_app_context_t *app,
                                         const sc_json_value_t *root, char **out, size_t *out_len) {
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
        json_set_str(alloc, obj, "error", error_msg);
    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

#endif /* SC_HAS_PUSH */

/* ── auth.oauth.start ────────────────────────────────────────────────── */

static sc_error_t handle_oauth_start(sc_allocator_t *alloc, const sc_control_protocol_t *proto,
                                     const sc_json_value_t *root, char **out, size_t *out_len) {
    sc_oauth_ctx_t *ctx = (sc_oauth_ctx_t *)proto->oauth_ctx;
    if (!ctx || !proto->oauth_pending_store || !proto->oauth_pending_lookup ||
        !proto->oauth_pending_remove) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj)
            return SC_ERR_OUT_OF_MEMORY;
        json_set_str(alloc, obj, "error", "OAuth not configured");
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
        json_set_str(alloc, obj, "error", "provider is required");
        sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
        sc_json_free(alloc, obj);
        return err;
    }
    const char *ctx_provider = sc_oauth_get_provider(ctx);
    if (ctx_provider && strcmp(ctx_provider, provider) != 0) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj)
            return SC_ERR_OUT_OF_MEMORY;
        json_set_str(alloc, obj, "error", "unsupported provider");
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
    json_set_str(alloc, obj, "url", url);
    json_set_str(alloc, obj, "state", state);
    (void)redirect_uri;
    err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── auth.oauth.callback ─────────────────────────────────────────────── */

static sc_error_t handle_oauth_callback(sc_allocator_t *alloc, const sc_control_protocol_t *proto,
                                        const sc_json_value_t *root, char **out, size_t *out_len) {
    sc_oauth_ctx_t *ctx = (sc_oauth_ctx_t *)proto->oauth_ctx;
    if (!ctx || !proto->oauth_pending_lookup || !proto->oauth_pending_remove) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj)
            return SC_ERR_OUT_OF_MEMORY;
        json_set_str(alloc, obj, "error", "OAuth not configured");
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
        json_set_str(alloc, obj, "error", "code and state are required");
        sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
        sc_json_free(alloc, obj);
        return err;
    }
    const char *verifier = proto->oauth_pending_lookup(proto->oauth_pending_ctx, state);
    if (!verifier) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj)
            return SC_ERR_OUT_OF_MEMORY;
        json_set_str(alloc, obj, "error", "invalid or expired state");
        sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
        sc_json_free(alloc, obj);
        return err;
    }
    sc_oauth_session_t session = {0};
#if SC_IS_TEST
    sc_error_t err =
        sc_oauth_exchange_code(ctx, code, strlen(code), verifier, strlen(verifier), &session);
#else
    sc_error_t err =
        sc_oauth_exchange_code(ctx, code, strlen(code), verifier, strlen(verifier), &session);
#endif
    proto->oauth_pending_remove(proto->oauth_pending_ctx, state);
    if (err != SC_OK) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj)
            return SC_ERR_OUT_OF_MEMORY;
        json_set_str(alloc, obj, "error", "token exchange failed");
        sc_error_t serr = sc_json_stringify(alloc, obj, out, out_len);
        sc_json_free(alloc, obj);
        return serr;
    }
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;
    json_set_str(alloc, obj, "token", session.access_token);
    sc_json_value_t *user = sc_json_object_new(alloc);
    if (user) {
        json_set_str(alloc, user, "id", session.user_id);
        sc_json_object_set(alloc, obj, "user", user);
    }
    err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── auth.oauth.refresh ──────────────────────────────────────────────── */

static sc_error_t handle_oauth_refresh(sc_allocator_t *alloc, const sc_control_protocol_t *proto,
                                       const sc_json_value_t *root, char **out, size_t *out_len) {
    sc_oauth_ctx_t *ctx = (sc_oauth_ctx_t *)proto->oauth_ctx;
    if (!ctx) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj)
            return SC_ERR_OUT_OF_MEMORY;
        json_set_str(alloc, obj, "error", "OAuth not configured");
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
        json_set_str(alloc, obj, "error", "refresh_token is required");
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
        json_set_str(alloc, obj, "error", "refresh failed");
        sc_error_t serr = sc_json_stringify(alloc, obj, out, out_len);
        sc_json_free(alloc, obj);
        return serr;
    }
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;
    json_set_str(alloc, obj, "access_token", session.access_token);
    int64_t expires_in = session.expires_at - (int64_t)time(NULL);
    if (expires_in < 0)
        expires_in = 0;
    sc_json_object_set(alloc, obj, "expires_in", sc_json_number_new(alloc, (double)expires_in));
    err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

/* ── RPC dispatch table ──────────────────────────────────────────────── */

static sc_error_t handle_auth_token(sc_allocator_t *alloc, sc_ws_conn_t *conn,
                                    const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                    char **out, size_t *out_len);

typedef sc_error_t (*sc_rpc_handler_fn)(sc_allocator_t *alloc, sc_app_context_t *app,
                                        sc_ws_conn_t *conn, const sc_control_protocol_t *proto,
                                        const sc_json_value_t *root, char **out, size_t *out_len);

typedef struct {
    const char *method;
    sc_rpc_handler_fn handler;
} sc_rpc_entry_t;

static sc_error_t rpc_auth_token(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                 const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                 char **out, size_t *out_len) {
    (void)app;
    return handle_auth_token(alloc, conn, proto, root, out, out_len);
}

static sc_error_t rpc_oauth_start(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                  const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                  char **out, size_t *out_len) {
    (void)app;
    (void)conn;
    return handle_oauth_start(alloc, proto, root, out, out_len);
}

static sc_error_t rpc_oauth_callback(sc_allocator_t *alloc, sc_app_context_t *app,
                                     sc_ws_conn_t *conn, const sc_control_protocol_t *proto,
                                     const sc_json_value_t *root, char **out, size_t *out_len) {
    (void)app;
    (void)conn;
    return handle_oauth_callback(alloc, proto, root, out, out_len);
}

static sc_error_t rpc_oauth_refresh(sc_allocator_t *alloc, sc_app_context_t *app,
                                    sc_ws_conn_t *conn, const sc_control_protocol_t *proto,
                                    const sc_json_value_t *root, char **out, size_t *out_len) {
    (void)app;
    (void)conn;
    return handle_oauth_refresh(alloc, proto, root, out, out_len);
}

static sc_error_t rpc_connect(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                              const sc_control_protocol_t *proto, const sc_json_value_t *root,
                              char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    return build_connect_response(alloc, app, out, out_len);
}

static sc_error_t rpc_health(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                             const sc_control_protocol_t *proto, const sc_json_value_t *root,
                             char **out, size_t *out_len) {
    (void)app;
    (void)conn;
    (void)proto;
    (void)root;
    return handle_health(alloc, out, out_len);
}

static sc_error_t rpc_config_get(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                 const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                 char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    return handle_config_get(alloc, app, out, out_len);
}

static sc_error_t rpc_config_schema(sc_allocator_t *alloc, sc_app_context_t *app,
                                    sc_ws_conn_t *conn, const sc_control_protocol_t *proto,
                                    const sc_json_value_t *root, char **out, size_t *out_len) {
    (void)app;
    (void)conn;
    (void)proto;
    (void)root;
    return handle_config_schema(alloc, out, out_len);
}

static sc_error_t rpc_capabilities(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                   const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                   char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    return handle_capabilities(alloc, app, out, out_len);
}

static sc_error_t rpc_chat_send(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    return handle_chat_send(alloc, app, root, out, out_len);
}

static sc_error_t rpc_chat_history(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                   const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                   char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    return handle_chat_history(alloc, app, root, out, out_len);
}

static sc_error_t rpc_chat_abort(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                 const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                 char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    return handle_chat_abort(alloc, app, out, out_len);
}

static sc_error_t rpc_config_set(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                 const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                 char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    return handle_config_set(alloc, app, root, out, out_len);
}

static sc_error_t rpc_config_apply(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                   const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                   char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    return handle_config_apply(alloc, app, root, out, out_len);
}

static sc_error_t rpc_sessions_list(sc_allocator_t *alloc, sc_app_context_t *app,
                                    sc_ws_conn_t *conn, const sc_control_protocol_t *proto,
                                    const sc_json_value_t *root, char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    return handle_sessions_list(alloc, app, out, out_len);
}

static sc_error_t rpc_sessions_patch(sc_allocator_t *alloc, sc_app_context_t *app,
                                     sc_ws_conn_t *conn, const sc_control_protocol_t *proto,
                                     const sc_json_value_t *root, char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    return handle_sessions_patch(alloc, app, root, out, out_len);
}

static sc_error_t rpc_sessions_delete(sc_allocator_t *alloc, sc_app_context_t *app,
                                      sc_ws_conn_t *conn, const sc_control_protocol_t *proto,
                                      const sc_json_value_t *root, char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    return handle_sessions_delete(alloc, app, root, out, out_len);
}

static sc_error_t rpc_persona_set(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                  const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                  char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    return handle_persona_set(alloc, app, root, out, out_len);
}

static sc_error_t rpc_tools_catalog(sc_allocator_t *alloc, sc_app_context_t *app,
                                    sc_ws_conn_t *conn, const sc_control_protocol_t *proto,
                                    const sc_json_value_t *root, char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    return handle_tools_catalog(alloc, app, out, out_len);
}

static sc_error_t rpc_channels_status(sc_allocator_t *alloc, sc_app_context_t *app,
                                      sc_ws_conn_t *conn, const sc_control_protocol_t *proto,
                                      const sc_json_value_t *root, char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    return handle_channels_status(alloc, app, out, out_len);
}

#ifdef SC_HAS_CRON
static sc_error_t rpc_cron_list(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    return handle_cron_list(alloc, app, out, out_len);
}

static sc_error_t rpc_cron_add(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                               const sc_control_protocol_t *proto, const sc_json_value_t *root,
                               char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    return handle_cron_add(alloc, app, root, out, out_len);
}

static sc_error_t rpc_cron_remove(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                  const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                  char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    return handle_cron_remove(alloc, app, root, out, out_len);
}

static sc_error_t rpc_cron_run(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                               const sc_control_protocol_t *proto, const sc_json_value_t *root,
                               char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    return handle_cron_run(alloc, app, root, out, out_len);
}

static sc_error_t rpc_cron_update(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                  const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                  char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    return handle_cron_update(alloc, app, root, out, out_len);
}

static sc_error_t rpc_cron_runs(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    return handle_cron_runs(alloc, app, root, out, out_len);
}
#endif

#ifdef SC_HAS_SKILLS
static sc_error_t rpc_skills_list(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                  const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                  char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    return handle_skills_list(alloc, app, out, out_len);
}

static sc_error_t rpc_skills_enable(sc_allocator_t *alloc, sc_app_context_t *app,
                                    sc_ws_conn_t *conn, const sc_control_protocol_t *proto,
                                    const sc_json_value_t *root, char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    return handle_skill_toggle(alloc, app, root, true, out, out_len);
}

static sc_error_t rpc_skills_disable(sc_allocator_t *alloc, sc_app_context_t *app,
                                     sc_ws_conn_t *conn, const sc_control_protocol_t *proto,
                                     const sc_json_value_t *root, char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    return handle_skill_toggle(alloc, app, root, false, out, out_len);
}

static sc_error_t rpc_skills_install(sc_allocator_t *alloc, sc_app_context_t *app,
                                     sc_ws_conn_t *conn, const sc_control_protocol_t *proto,
                                     const sc_json_value_t *root, char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    return handle_skills_install(alloc, app, root, out, out_len);
}

static sc_error_t rpc_skills_search(sc_allocator_t *alloc, sc_app_context_t *app,
                                    sc_ws_conn_t *conn, const sc_control_protocol_t *proto,
                                    const sc_json_value_t *root, char **out, size_t *out_len) {
    (void)app;
    (void)conn;
    (void)proto;
    return handle_skills_search(alloc, root, out, out_len);
}

static sc_error_t rpc_skills_uninstall(sc_allocator_t *alloc, sc_app_context_t *app,
                                       sc_ws_conn_t *conn, const sc_control_protocol_t *proto,
                                       const sc_json_value_t *root, char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    return handle_skills_uninstall(alloc, app, root, out, out_len);
}

static sc_error_t rpc_skills_update(sc_allocator_t *alloc, sc_app_context_t *app,
                                    sc_ws_conn_t *conn, const sc_control_protocol_t *proto,
                                    const sc_json_value_t *root, char **out, size_t *out_len) {
    (void)app;
    (void)conn;
    (void)proto;
    (void)root;
    return handle_skills_update(alloc, out, out_len);
}
#endif

static sc_error_t rpc_models_list(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                  const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                  char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    return handle_models_list(alloc, app, out, out_len);
}

static sc_error_t rpc_nodes_list(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                 const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                 char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    return handle_nodes_list(alloc, app, out, out_len);
}

#ifdef SC_HAS_UPDATE
static sc_error_t rpc_update_check(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                   const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                   char **out, size_t *out_len) {
    (void)app;
    (void)conn;
    (void)proto;
    (void)root;
    return handle_update_check(alloc, out, out_len);
}

static sc_error_t rpc_update_run(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                 const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                 char **out, size_t *out_len) {
    (void)app;
    (void)conn;
    (void)proto;
    (void)root;
    return handle_update_run(alloc, out, out_len);
}
#endif

static sc_error_t rpc_exec_approval_resolve(sc_allocator_t *alloc, sc_app_context_t *app,
                                            sc_ws_conn_t *conn, const sc_control_protocol_t *proto,
                                            const sc_json_value_t *root, char **out,
                                            size_t *out_len) {
    (void)conn;
    (void)proto;
    return handle_exec_approval(alloc, app, root, out, out_len);
}

static sc_error_t rpc_usage_summary(sc_allocator_t *alloc, sc_app_context_t *app,
                                    sc_ws_conn_t *conn, const sc_control_protocol_t *proto,
                                    const sc_json_value_t *root, char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    return handle_usage_summary(alloc, app, out, out_len);
}

static sc_error_t rpc_activity_recent(sc_allocator_t *alloc, sc_app_context_t *app,
                                      sc_ws_conn_t *conn, const sc_control_protocol_t *proto,
                                      const sc_json_value_t *root, char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    return handle_activity_recent(alloc, app, out, out_len);
}

#ifdef SC_HAS_PUSH
static sc_error_t rpc_push_register(sc_allocator_t *alloc, sc_app_context_t *app,
                                    sc_ws_conn_t *conn, const sc_control_protocol_t *proto,
                                    const sc_json_value_t *root, char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    return handle_push_register(alloc, app, root, out, out_len);
}

static sc_error_t rpc_push_unregister(sc_allocator_t *alloc, sc_app_context_t *app,
                                      sc_ws_conn_t *conn, const sc_control_protocol_t *proto,
                                      const sc_json_value_t *root, char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    return handle_push_unregister(alloc, app, root, out, out_len);
}
#endif

static const sc_rpc_entry_t s_rpc_table[] = {
    {"auth.token", rpc_auth_token},
    {"auth.oauth.start", rpc_oauth_start},
    {"auth.oauth.callback", rpc_oauth_callback},
    {"auth.oauth.refresh", rpc_oauth_refresh},
    {"connect", rpc_connect},
    {"health", rpc_health},
    {"config.get", rpc_config_get},
    {"config.schema", rpc_config_schema},
    {"capabilities", rpc_capabilities},
    {"chat.send", rpc_chat_send},
    {"chat.history", rpc_chat_history},
    {"chat.abort", rpc_chat_abort},
    {"config.set", rpc_config_set},
    {"config.apply", rpc_config_apply},
    {"sessions.list", rpc_sessions_list},
    {"sessions.patch", rpc_sessions_patch},
    {"sessions.delete", rpc_sessions_delete},
    {"persona.set", rpc_persona_set},
    {"tools.catalog", rpc_tools_catalog},
    {"channels.status", rpc_channels_status},
#ifdef SC_HAS_CRON
    {"cron.list", rpc_cron_list},
    {"cron.add", rpc_cron_add},
    {"cron.remove", rpc_cron_remove},
    {"cron.run", rpc_cron_run},
    {"cron.update", rpc_cron_update},
    {"cron.runs", rpc_cron_runs},
#endif
#ifdef SC_HAS_SKILLS
    {"skills.list", rpc_skills_list},
    {"skills.enable", rpc_skills_enable},
    {"skills.disable", rpc_skills_disable},
    {"skills.install", rpc_skills_install},
    {"skills.search", rpc_skills_search},
    {"skills.uninstall", rpc_skills_uninstall},
    {"skills.update", rpc_skills_update},
#endif
    {"models.list", rpc_models_list},
    {"nodes.list", rpc_nodes_list},
#ifdef SC_HAS_UPDATE
    {"update.check", rpc_update_check},
    {"update.run", rpc_update_run},
#endif
    {"exec.approval.resolve", rpc_exec_approval_resolve},
    {"usage.summary", rpc_usage_summary},
    {"activity.recent", rpc_activity_recent},
#ifdef SC_HAS_PUSH
    {"push.register", rpc_push_register},
    {"push.unregister", rpc_push_unregister},
#endif
    {NULL, NULL},
};

/* ── Auth helpers ───────────────────────────────────────────────────── */

static bool is_public_method(const char *method) {
    return strcmp(method, "health") == 0 || strcmp(method, "status") == 0 ||
           strcmp(method, "version") == 0 || strcmp(method, "connect") == 0 ||
           strcmp(method, "capabilities") == 0 || strcmp(method, "auth.oauth.start") == 0 ||
           strcmp(method, "auth.oauth.callback") == 0 || strcmp(method, "auth.oauth.refresh") == 0;
}

/* ── auth.token ──────────────────────────────────────────────────────── */

static sc_error_t handle_auth_token(sc_allocator_t *alloc, sc_ws_conn_t *conn,
                                    const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                    char **out, size_t *out_len) {
    (void)alloc;
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
        json_set_str(alloc, obj, "error", "missing token");
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
        json_set_str(alloc, obj, "result", "authenticated");
        sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
        sc_json_free(alloc, obj);
        return err;
    }
    (void)alloc;
    (void)out;
    (void)out_len;
    return SC_ERR_GATEWAY_AUTH;
}

/* ── Method dispatcher ───────────────────────────────────────────────── */

static sc_error_t build_method_response(sc_allocator_t *alloc, const char *method,
                                        const sc_json_value_t *root, sc_app_context_t *app,
                                        sc_ws_conn_t *conn, const sc_control_protocol_t *proto,
                                        char **payload_out, size_t *payload_len_out) {
    *payload_out = NULL;
    *payload_len_out = 0;

    for (const sc_rpc_entry_t *e = s_rpc_table; e->method; e++) {
        if (strcmp(method, e->method) == 0) {
            return e->handler(alloc, app, conn, proto, root, payload_out, payload_len_out);
        }
    }
    return SC_ERR_NOT_FOUND;
}

#endif /* SC_GATEWAY_POSIX */

/* ── Protocol lifecycle ──────────────────────────────────────────────── */

void sc_control_protocol_init(sc_control_protocol_t *proto, sc_allocator_t *alloc,
                              sc_ws_server_t *ws) {
    if (!proto)
        return;
    proto->alloc = alloc;
    proto->ws = ws;
    proto->event_seq = 0;
    proto->app_ctx = NULL;
    proto->require_pairing = false;
    proto->pairing_guard = NULL;
    proto->auth_token = NULL;
    proto->oauth_ctx = NULL;
    proto->oauth_pending_ctx = NULL;
    proto->oauth_pending_store = NULL;
    proto->oauth_pending_lookup = NULL;
    proto->oauth_pending_remove = NULL;
}

void sc_control_protocol_deinit(sc_control_protocol_t *proto) {
    (void)proto;
}

void sc_control_set_app_ctx(sc_control_protocol_t *proto, sc_app_context_t *ctx) {
    if (proto)
        proto->app_ctx = ctx;
}

void sc_control_set_auth(sc_control_protocol_t *proto, bool require_pairing,
                         sc_pairing_guard_t *pairing_guard, const char *auth_token) {
    if (!proto)
        return;
    proto->require_pairing = require_pairing;
    proto->pairing_guard = pairing_guard;
    proto->auth_token = auth_token && auth_token[0] ? auth_token : NULL;
}

void sc_control_set_oauth(sc_control_protocol_t *proto, void *oauth_ctx) {
    if (!proto)
        return;
    proto->oauth_ctx = oauth_ctx;
}

void sc_control_set_oauth_pending(sc_control_protocol_t *proto, void *ctx,
                                  void (*store)(void *ctx, const char *state, const char *verifier),
                                  const char *(*lookup)(void *ctx, const char *state),
                                  void (*remove)(void *ctx, const char *state)) {
    if (!proto)
        return;
    proto->oauth_pending_ctx = ctx;
    proto->oauth_pending_store = store;
    proto->oauth_pending_lookup = lookup;
    proto->oauth_pending_remove = remove;
}

/* ── Incoming message handler ────────────────────────────────────────── */

void sc_control_on_message(sc_ws_conn_t *conn, const char *data, size_t data_len, void *ctx) {
    sc_control_protocol_t *proto = (sc_control_protocol_t *)ctx;
    if (!proto || !proto->alloc || !proto->ws)
        return;

#ifdef SC_GATEWAY_POSIX
    /* Simple global RPC rate limit: max 60 calls per second across all connections */
    uint64_t now_ms = (uint64_t)time(NULL) * 1000;
    if (now_ms - proto->rpc_window_ms > 1000) {
        proto->rpc_count = 0;
        proto->rpc_window_ms = now_ms;
    }
    if (++proto->rpc_count > 60) {
        sc_control_send_response(conn, "0", false, "{\"error\":\"rate limited\"}");
        return;
    }

    sc_json_value_t *root = NULL;
    if (sc_json_parse(proto->alloc, data, data_len, &root) != SC_OK)
        return;
    if (!root || root->type != SC_JSON_OBJECT) {
        if (root)
            sc_json_free(proto->alloc, root);
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

    /* Auth check: when pairing required, only public methods and auth.token allowed unauthenticated
     */
    if (proto->require_pairing && !conn->authenticated && !is_public_method(method) &&
        strcmp(method, "auth.token") != 0) {
        sc_control_send_response(
            conn, id_raw, false,
            "{\"error\":\"unauthorized\",\"message\":\"Authentication required\"}");
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
    sc_error_t err = build_method_response(proto->alloc, method, root, proto->app_ctx, conn, proto,
                                           &payload, &payload_len);
    sc_json_free(proto->alloc, root);

    bool ok = (err == SC_OK);
    if (!payload && ok) {
        sc_json_value_t *empty = sc_json_object_new(proto->alloc);
        if (empty) {
            sc_json_stringify(proto->alloc, empty, &payload, &payload_len);
            sc_json_free(proto->alloc, empty);
        }
        if (!payload)
            payload = (char *)proto->alloc->alloc(proto->alloc->ctx, 3);
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
            } else if (err == SC_ERR_GATEWAY_AUTH) {
                n = (size_t)snprintf(payload, 64, "{\"error\":\"invalid_token\"}");
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
            pos += (size_t)snprintf(res_buf, res_cap, "{\"type\":\"res\",\"id\":\"");
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
                    if (c == '"' || c == '\\')
                        res_buf[pos++] = '\\';
                    res_buf[pos++] = c;
                }
                if (pos < res_cap)
                    res_buf[pos++] = '"';
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
    if (!proto || !proto->ws || !event_name)
        return;

#ifdef SC_GATEWAY_POSIX
    uint64_t seq = proto->event_seq++;
    const char *payload = payload_json ? payload_json : "{}";
    size_t payload_len = strlen(payload);
    size_t event_len = strlen(event_name);

    size_t cap = 128 + event_len + payload_len;
    char *buf = (char *)proto->alloc->alloc(proto->alloc->ctx, cap);
    if (!buf)
        return;

    int n = snprintf(buf, cap, "{\"type\":\"event\",\"event\":\"%s\",\"payload\":%s,\"seq\":%llu}",
                     event_name, payload, (unsigned long long)seq);

    if (n > 0 && (size_t)n < cap)
        sc_ws_server_broadcast(proto->ws, buf, (size_t)n);

    proto->alloc->free(proto->alloc->ctx, buf, cap);
#endif
}

sc_error_t sc_control_send_response(sc_ws_conn_t *conn, const char *id, bool ok,
                                    const char *payload_json) {
    if (!conn || !id)
        return SC_ERR_INVALID_ARGUMENT;

#ifdef SC_GATEWAY_POSIX
    sc_allocator_t alloc = sc_system_allocator();
    const char *payload = payload_json ? payload_json : "{}";
    size_t payload_len = strlen(payload);
    size_t id_len = strlen(id);
    size_t cap = 96 + id_len * 2 + payload_len;

    char *buf = (char *)alloc.alloc(alloc.ctx, cap);
    if (!buf)
        return SC_ERR_OUT_OF_MEMORY;

    size_t pos = 0;
    pos += (size_t)snprintf(buf + pos, cap - pos, "{\"type\":\"res\",\"id\":\"");
    for (size_t i = 0; i < id_len && pos + 4 < cap; i++) {
        char c = id[i];
        if (c == '"' || c == '\\') {
            buf[pos++] = '\\';
            buf[pos++] = c;
        } else
            buf[pos++] = c;
    }
    pos += (size_t)snprintf(buf + pos, cap - pos, "\",\"ok\":%s,\"payload\":%s}",
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
