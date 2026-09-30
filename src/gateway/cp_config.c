/* Config-related control protocol handlers: config.get, config.schema, config.set, config.apply */
#include "cp_internal.h"
#include "human/channel_catalog.h"
#include "human/config.h"
#include "human/config_mutator.h"
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

/* Write one config.set / config.apply request to disk and to the live config.
 *
 * params is either {"key": "a.b", "value": <json>} (what the dashboard sends)
 * or {"raw": "<whole config document>"}. Both go through the config mutator:
 * a key edit changes only that key, a raw document is written verbatim, and
 * neither can drop keys the way re-serializing hu_config_t did. *saved is true
 * only when the file holds the requested state; *why names the refusal. */
static void config_write(hu_allocator_t *alloc, hu_app_context_t *app, const hu_json_value_t *root,
                         bool *saved, const char **why) {
    *saved = false;
    *why = "missing params";
    hu_json_value_t *params = root ? hu_json_object_get(root, "params") : NULL;
    if (!params || !app || !app->config)
        return;

    /* No guessing: a config that was not loaded from a file (tests, embedders)
     * has no file to write, and the default path would be the user's real one. */
    const char *cfg_path = app->config->config_path;
    if (!cfg_path || !cfg_path[0]) {
        *why = "config was not loaded from a file";
        return;
    }

    char *patch = NULL;
    size_t patch_len = 0;
    hu_error_t err = HU_ERR_INVALID_ARGUMENT;
    const char *raw = hu_json_get_string(params, "raw");
    const char *key = hu_json_get_string(params, "key");
    hu_json_value_t *value = hu_json_object_get(params, "value");
    if (raw) {
        err = hu_config_mutator_replace_at(alloc, cfg_path, raw, strlen(raw));
        if (err == HU_OK)
            (void)hu_config_parse_json(app->config, raw, strlen(raw));
    } else if (key && value) {
        char *value_json = NULL;
        size_t value_len = 0;
        err = hu_json_stringify(alloc, value, &value_json, &value_len);
        hu_mutation_result_t res = {0};
        if (err == HU_OK) {
            hu_mutation_options_t opts = {.apply = true};
            err = hu_config_mutator_mutate_at(alloc, cfg_path, HU_MUTATION_SET, key, value_json,
                                              opts, &res);
            alloc->free(alloc->ctx, value_json, value_len + 1);
        }
        if (err == HU_OK) {
            if (hu_config_mutator_build_patch(alloc, res.path, res.new_value_json, &patch,
                                              &patch_len) == HU_OK)
                (void)hu_config_parse_json(app->config, patch, patch_len);
            hu_config_mutator_free_result(alloc, &res);
        }
    }
    if (patch)
        alloc->free(alloc->ctx, patch, patch_len + 1);

    if (err == HU_OK) {
        *saved = true;
        *why = NULL;
        hu_config_set_reload_requested(); /* the daemon's agent re-reads the file */
    } else if (err == HU_ERR_PERMISSION_DENIED) {
        *why = "this setting cannot be changed from the control protocol";
    } else if (err == HU_ERR_INVALID_ARGUMENT) {
        *why = (raw || (key && value)) ? "rejected by config validation" : "missing params";
    } else {
        *why = hu_error_string(err);
    }
}

static hu_error_t config_write_response(hu_allocator_t *alloc, bool saved, const char *why,
                                        bool with_applied, char **out, size_t *out_len) {
    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;
    if (with_applied)
        hu_json_object_set(alloc, obj, "applied", hu_json_bool_new(alloc, saved));
    hu_json_object_set(alloc, obj, "saved", hu_json_bool_new(alloc, saved));
    if (why)
        hu_json_object_set(alloc, obj, "error", hu_json_string_new(alloc, why, strlen(why)));
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
    const char *why = NULL;
    config_write(alloc, app, root, &saved, &why);
    return config_write_response(alloc, saved, why, false, out, out_len);
}

hu_error_t cp_config_apply(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                           const hu_control_protocol_t *proto, const hu_json_value_t *root,
                           char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    bool saved = false;
    const char *why = NULL;
    config_write(alloc, app, root, &saved, &why);
    return config_write_response(alloc, saved, why, true, out, out_len);
}
