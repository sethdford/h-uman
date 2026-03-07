/* Config-related control protocol handlers: config.get, config.schema, config.set, config.apply */
#include "cp_internal.h"
#include "seaclaw/channel_catalog.h"
#include "seaclaw/config.h"
#include <string.h>

sc_error_t cp_config_get(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                         const sc_control_protocol_t *proto, const sc_json_value_t *root,
                         char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;

    if (app && app->config) {
        sc_json_object_set(alloc, obj, "exists", sc_json_bool_new(alloc, true));
        cp_json_set_str(alloc, obj, "workspace_dir", app->config->workspace_dir);
        cp_json_set_str(alloc, obj, "default_provider", app->config->default_provider);
        cp_json_set_str(alloc, obj, "default_model", app->config->default_model);
        sc_json_object_set(alloc, obj, "max_tokens",
                           sc_json_number_new(alloc, (double)app->config->max_tokens));
        sc_json_object_set(alloc, obj, "temperature",
                           sc_json_number_new(alloc, app->config->temperature));

        sc_json_value_t *sec = sc_json_object_new(alloc);
        if (sec) {
            sc_json_object_set(alloc, sec, "autonomy_level",
                               sc_json_number_new(alloc, app->config->security.autonomy_level));
            cp_json_set_str(alloc, sec, "sandbox", app->config->security.sandbox);
            sc_json_value_t *sbc = sc_json_object_new(alloc);
            if (sbc) {
                sc_json_object_set(
                    alloc, sbc, "enabled",
                    sc_json_bool_new(alloc, app->config->security.sandbox_config.enabled));
                cp_json_set_str(alloc, sbc, "backend",
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
                    cp_json_set_str(alloc, np, "proxy_addr",
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

sc_error_t cp_config_schema(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                            const sc_control_protocol_t *proto, const sc_json_value_t *root,
                            char **out, size_t *out_len) {
    (void)app;
    (void)conn;
    (void)proto;
    (void)root;
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;

    sc_json_value_t *schema = sc_json_object_new(alloc);
    cp_json_set_str(alloc, schema, "type", "object");

    sc_json_value_t *props = sc_json_object_new(alloc);
    sc_json_value_t *wp = sc_json_object_new(alloc);
    cp_json_set_str(alloc, wp, "type", "string");
    cp_json_set_str(alloc, wp, "description", "Workspace directory");
    sc_json_object_set(alloc, props, "workspace_dir", wp);

    sc_json_value_t *dp = sc_json_object_new(alloc);
    cp_json_set_str(alloc, dp, "type", "string");
    cp_json_set_str(alloc, dp, "description", "Default AI provider");
    sc_json_object_set(alloc, props, "default_provider", dp);

    sc_json_value_t *dm = sc_json_object_new(alloc);
    cp_json_set_str(alloc, dm, "type", "string");
    cp_json_set_str(alloc, dm, "description", "Default model");
    sc_json_object_set(alloc, props, "default_model", dm);

    sc_json_value_t *mt = sc_json_object_new(alloc);
    cp_json_set_str(alloc, mt, "type", "integer");
    cp_json_set_str(alloc, mt, "description", "Max tokens per response");
    sc_json_object_set(alloc, props, "max_tokens", mt);

    sc_json_value_t *tp = sc_json_object_new(alloc);
    cp_json_set_str(alloc, tp, "type", "number");
    cp_json_set_str(alloc, tp, "description", "Temperature (0.0 - 2.0)");
    sc_json_object_set(alloc, props, "temperature", tp);

    sc_json_object_set(alloc, schema, "properties", props);
    sc_json_object_set(alloc, obj, "schema", schema);

    sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

sc_error_t cp_config_set(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                         const sc_control_protocol_t *proto, const sc_json_value_t *root,
                         char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
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

sc_error_t cp_config_apply(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                           const sc_control_protocol_t *proto, const sc_json_value_t *root,
                           char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
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
