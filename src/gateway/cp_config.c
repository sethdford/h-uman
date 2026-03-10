/* Config-related control protocol handlers: config.get, config.schema, config.set, config.apply */
#include "cp_internal.h"
#include "human/channel_catalog.h"
#include "human/config.h"
#include <string.h>

hu_error_t cp_config_get(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                         const hu_control_protocol_t *proto, const hu_json_value_t *root,
                         char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;

    if (app && app->config) {
        hu_json_object_set(alloc, obj, "exists", hu_json_bool_new(alloc, true));
        cp_json_set_str(alloc, obj, "workspace_dir", app->config->workspace_dir);
        cp_json_set_str(alloc, obj, "default_provider", app->config->default_provider);
        cp_json_set_str(alloc, obj, "default_model", app->config->default_model);
        hu_json_object_set(alloc, obj, "max_tokens",
                           hu_json_number_new(alloc, (double)app->config->max_tokens));
        hu_json_object_set(alloc, obj, "temperature",
                           hu_json_number_new(alloc, app->config->temperature));

        hu_json_value_t *sec = hu_json_object_new(alloc);
        if (sec) {
            hu_json_object_set(alloc, sec, "autonomy_level",
                               hu_json_number_new(alloc, app->config->security.autonomy_level));
            cp_json_set_str(alloc, sec, "sandbox", app->config->security.sandbox);
            hu_json_value_t *sbc = hu_json_object_new(alloc);
            if (sbc) {
                hu_json_object_set(
                    alloc, sbc, "enabled",
                    hu_json_bool_new(alloc, app->config->security.sandbox_config.enabled));
                cp_json_set_str(alloc, sbc, "backend",
                                app->config->security.sandbox ? app->config->security.sandbox
                                                              : "auto");
                hu_json_value_t *np = hu_json_object_new(alloc);
                if (np) {
                    hu_json_object_set(
                        alloc, np, "enabled",
                        hu_json_bool_new(alloc,
                                         app->config->security.sandbox_config.net_proxy.enabled));
                    hu_json_object_set(
                        alloc, np, "deny_all",
                        hu_json_bool_new(alloc,
                                         app->config->security.sandbox_config.net_proxy.deny_all));
                    cp_json_set_str(alloc, np, "proxy_addr",
                                    app->config->security.sandbox_config.net_proxy.proxy_addr);
                    hu_json_object_set(alloc, sbc, "net_proxy", np);
                }
                hu_json_object_set(alloc, sec, "sandbox_config", sbc);
            }
            hu_json_object_set(alloc, obj, "security", sec);
        }
    } else {
        hu_json_object_set(alloc, obj, "exists", hu_json_bool_new(alloc, false));
    }

    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

hu_error_t cp_config_schema(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                            const hu_control_protocol_t *proto, const hu_json_value_t *root,
                            char **out, size_t *out_len) {
    (void)app;
    (void)conn;
    (void)proto;
    (void)root;
    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;

    hu_json_value_t *schema = hu_json_object_new(alloc);
    cp_json_set_str(alloc, schema, "type", "object");

    hu_json_value_t *props = hu_json_object_new(alloc);
    hu_json_value_t *wp = hu_json_object_new(alloc);
    cp_json_set_str(alloc, wp, "type", "string");
    cp_json_set_str(alloc, wp, "description", "Workspace directory");
    hu_json_object_set(alloc, props, "workspace_dir", wp);

    hu_json_value_t *dp = hu_json_object_new(alloc);
    cp_json_set_str(alloc, dp, "type", "string");
    cp_json_set_str(alloc, dp, "description", "Default AI provider");
    hu_json_object_set(alloc, props, "default_provider", dp);

    hu_json_value_t *dm = hu_json_object_new(alloc);
    cp_json_set_str(alloc, dm, "type", "string");
    cp_json_set_str(alloc, dm, "description", "Default model");
    hu_json_object_set(alloc, props, "default_model", dm);

    hu_json_value_t *mt = hu_json_object_new(alloc);
    cp_json_set_str(alloc, mt, "type", "integer");
    cp_json_set_str(alloc, mt, "description", "Max tokens per response");
    hu_json_object_set(alloc, props, "max_tokens", mt);

    hu_json_value_t *tp = hu_json_object_new(alloc);
    cp_json_set_str(alloc, tp, "type", "number");
    cp_json_set_str(alloc, tp, "description", "Temperature (0.0 - 2.0)");
    hu_json_object_set(alloc, props, "temperature", tp);

    hu_json_value_t *mem = hu_json_object_new(alloc);
    if (mem) {
        hu_json_value_t *cih = hu_json_object_new(alloc);
        if (cih) {
            cp_json_set_str(alloc, cih, "type", "integer");
            cp_json_set_str(alloc, cih, "description",
                            "Memory consolidation interval in hours (0 = disabled, default 24)");
            hu_json_object_set(alloc, mem, "consolidation_interval_hours", cih);
        }
        hu_json_object_set(alloc, props, "memory", mem);
    }

    hu_json_object_set(alloc, schema, "properties", props);
    hu_json_object_set(alloc, obj, "schema", schema);

    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

hu_error_t cp_config_set(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                         const hu_control_protocol_t *proto, const hu_json_value_t *root,
                         char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    bool saved = false;

    if (root && app && app->config) {
        hu_json_value_t *params = hu_json_object_get(root, "params");
        if (params) {
            const char *raw = hu_json_get_string(params, "raw");
            if (raw) {
                hu_error_t parse_err = hu_config_parse_json(app->config, raw, strlen(raw));
                if (parse_err == HU_OK) {
                    hu_error_t save_err = hu_config_save(app->config);
                    saved = (save_err == HU_OK);
                }
            }
        }
    }

    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;
    hu_json_object_set(alloc, obj, "saved", hu_json_bool_new(alloc, saved));
    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

hu_error_t cp_config_apply(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                           const hu_control_protocol_t *proto, const hu_json_value_t *root,
                           char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    bool applied = false;
    bool saved = false;

    if (root && app && app->config) {
        hu_json_value_t *params = hu_json_object_get(root, "params");
        if (params) {
            const char *raw = hu_json_get_string(params, "raw");
            if (raw) {
                hu_error_t parse_err = hu_config_parse_json(app->config, raw, strlen(raw));
                if (parse_err == HU_OK) {
                    applied = true;
                    hu_error_t save_err = hu_config_save(app->config);
                    saved = (save_err == HU_OK);
                }
            }
        }
    }

    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;
    hu_json_object_set(alloc, obj, "applied", hu_json_bool_new(alloc, applied));
    hu_json_object_set(alloc, obj, "saved", hu_json_bool_new(alloc, saved));
    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}
