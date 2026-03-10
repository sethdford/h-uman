#include "human/config_parse.h"
#include "human/config.h"
#include "human/core/string.h"
#include <stdio.h>
#include <string.h>

#include "config_internal.h"
#include "config_parse_internal.h"

static hu_error_t parse_nodes(hu_allocator_t *a, hu_config_t *cfg, const hu_json_value_t *arr) {
    if (!arr || arr->type != HU_JSON_ARRAY)
        return HU_OK;
    for (size_t i = 0; i < cfg->nodes_len; i++) {
        if (cfg->nodes[i].name) {
            a->free(a->ctx, cfg->nodes[i].name, strlen(cfg->nodes[i].name) + 1);
            cfg->nodes[i].name = NULL;
        }
        if (cfg->nodes[i].status) {
            a->free(a->ctx, cfg->nodes[i].status, strlen(cfg->nodes[i].status) + 1);
            cfg->nodes[i].status = NULL;
        }
    }
    cfg->nodes_len = 0;
    size_t cap = arr->data.array.len;
    if (cap > HU_NODES_MAX)
        cap = HU_NODES_MAX;
    size_t n = 0;
    for (size_t i = 0; i < cap; i++) {
        const hu_json_value_t *item = arr->data.array.items[i];
        if (!item || item->type != HU_JSON_OBJECT)
            continue;
        const char *name = hu_json_get_string(item, "name");
        if (!name)
            continue;
        const char *status = hu_json_get_string(item, "status");
        cfg->nodes[n].name = hu_strdup(a, name);
        cfg->nodes[n].status = status && status[0] ? hu_strdup(a, status) : hu_strdup(a, "online");
        n++;
    }
    cfg->nodes_len = n;
    return HU_OK;
}

hu_error_t parse_string_array(hu_allocator_t *a, char ***out, size_t *out_len,
                              const hu_json_value_t *arr) {
    if (!arr || arr->type != HU_JSON_ARRAY)
        return HU_OK;
    size_t n = 0;
    for (size_t i = 0; i < arr->data.array.len; i++) {
        if (arr->data.array.items[i] && arr->data.array.items[i]->type == HU_JSON_STRING)
            n++;
    }
    if (n == 0)
        return HU_OK;

    char **list = (char **)a->alloc(a->ctx, n * sizeof(char *));
    if (!list)
        return HU_ERR_OUT_OF_MEMORY;

    size_t j = 0;
    for (size_t i = 0; i < arr->data.array.len && j < n; i++) {
        const hu_json_value_t *v = arr->data.array.items[i];
        if (!v || v->type != HU_JSON_STRING)
            continue;
        const char *s = v->data.string.ptr;
        if (s)
            list[j++] = hu_strdup(a, s);
    }
    *out = list;
    *out_len = j;
    return HU_OK;
}

static hu_error_t parse_autonomy(hu_allocator_t *a, hu_config_t *cfg, const hu_json_value_t *obj) {
    if (!obj || obj->type != HU_JSON_OBJECT)
        return HU_OK;

    const char *level = hu_json_get_string(obj, "level");
    if (level) {
        if (cfg->autonomy.level)
            a->free(a->ctx, cfg->autonomy.level, strlen(cfg->autonomy.level) + 1);
        cfg->autonomy.level = hu_strdup(a, level);
    }
    cfg->autonomy.workspace_only =
        hu_json_get_bool(obj, "workspace_only", cfg->autonomy.workspace_only);
    double max_act =
        hu_json_get_number(obj, "max_actions_per_hour", cfg->autonomy.max_actions_per_hour);
    if (max_act >= 0 && max_act <= 1000000)
        cfg->autonomy.max_actions_per_hour = (uint32_t)max_act;
    cfg->autonomy.require_approval_for_medium_risk = hu_json_get_bool(
        obj, "require_approval_for_medium_risk", cfg->autonomy.require_approval_for_medium_risk);
    cfg->autonomy.block_high_risk_commands =
        hu_json_get_bool(obj, "block_high_risk_commands", cfg->autonomy.block_high_risk_commands);

    hu_json_value_t *ac = hu_json_object_get(obj, "allowed_commands");
    if (ac && ac->type == HU_JSON_ARRAY) {
        if (cfg->autonomy.allowed_commands) {
            for (size_t i = 0; i < cfg->autonomy.allowed_commands_len; i++)
                a->free(a->ctx, cfg->autonomy.allowed_commands[i],
                        strlen(cfg->autonomy.allowed_commands[i]) + 1);
            a->free(a->ctx, cfg->autonomy.allowed_commands,
                    cfg->autonomy.allowed_commands_len * sizeof(char *));
        }
        parse_string_array(a, &cfg->autonomy.allowed_commands, &cfg->autonomy.allowed_commands_len,
                           ac);
    }
    return HU_OK;
}

static hu_error_t parse_cron(hu_allocator_t *a, hu_config_t *cfg, const hu_json_value_t *obj) {
    (void)a;
    if (!obj || obj->type != HU_JSON_OBJECT)
        return HU_OK;
    cfg->cron.enabled = hu_json_get_bool(obj, "enabled", cfg->cron.enabled);
    double im = hu_json_get_number(obj, "interval_minutes", cfg->cron.interval_minutes);
    if (im >= 1 && im <= 1440)
        cfg->cron.interval_minutes = (uint32_t)im;
    double mrh = hu_json_get_number(obj, "max_run_history", cfg->cron.max_run_history);
    if (mrh >= 0 && mrh <= 10000)
        cfg->cron.max_run_history = (uint32_t)mrh;
    return HU_OK;
}

static hu_error_t parse_scheduler(hu_allocator_t *a, hu_config_t *cfg, const hu_json_value_t *obj) {
    (void)a;
    if (!obj || obj->type != HU_JSON_OBJECT)
        return HU_OK;
    double mc = hu_json_get_number(obj, "max_concurrent", cfg->scheduler.max_concurrent);
    if (mc >= 0 && mc <= 256)
        cfg->scheduler.max_concurrent = (uint32_t)mc;
    return HU_OK;
}

static hu_error_t parse_gateway(hu_allocator_t *a, hu_config_t *cfg, const hu_json_value_t *obj) {
    if (!obj || obj->type != HU_JSON_OBJECT)
        return HU_OK;
    cfg->gateway.enabled = hu_json_get_bool(obj, "enabled", cfg->gateway.enabled);
    double port = hu_json_get_number(obj, "port", cfg->gateway.port);
    if (port < 1)
        port = 1;
    else if (port > 65535)
        port = 65535;
    cfg->gateway.port = (uint16_t)port;
    const char *host = hu_json_get_string(obj, "host");
    if (host) {
        if (cfg->gateway.host)
            a->free(a->ctx, cfg->gateway.host, strlen(cfg->gateway.host) + 1);
        cfg->gateway.host = hu_strdup(a, host);
    }
    cfg->gateway.require_pairing =
        hu_json_get_bool(obj, "require_pairing", cfg->gateway.require_pairing);
    const char *at = hu_json_get_string(obj, "auth_token");
    if (at) {
        if (cfg->gateway.auth_token)
            a->free(a->ctx, cfg->gateway.auth_token, strlen(cfg->gateway.auth_token) + 1);
        cfg->gateway.auth_token = hu_strdup(a, at);
    }
    cfg->gateway.allow_public_bind =
        hu_json_get_bool(obj, "allow_public_bind", cfg->gateway.allow_public_bind);
    double prl = hu_json_get_number(obj, "pair_rate_limit_per_minute",
                                    cfg->gateway.pair_rate_limit_per_minute);
    if (prl >= 0 && prl <= 1000)
        cfg->gateway.pair_rate_limit_per_minute = (uint32_t)prl;
    double rlr = hu_json_get_number(obj, "rate_limit_requests", cfg->gateway.rate_limit_requests);
    if (rlr >= 0 && rlr <= 10000)
        cfg->gateway.rate_limit_requests = (int)rlr;
    double rlw = hu_json_get_number(obj, "rate_limit_window", cfg->gateway.rate_limit_window);
    if (rlw >= 0 && rlw <= 86400)
        cfg->gateway.rate_limit_window = (int)rlw;
    const char *whs = hu_json_get_string(obj, "webhook_hmac_secret");
    if (whs) {
        if (cfg->gateway.webhook_hmac_secret)
            a->free(a->ctx, cfg->gateway.webhook_hmac_secret,
                    strlen(cfg->gateway.webhook_hmac_secret) + 1);
        cfg->gateway.webhook_hmac_secret = hu_strdup(a, whs);
    }
    const char *uid = hu_json_get_string(obj, "control_ui_dir");
    if (uid) {
        if (cfg->gateway.control_ui_dir)
            a->free(a->ctx, cfg->gateway.control_ui_dir, strlen(cfg->gateway.control_ui_dir) + 1);
        cfg->gateway.control_ui_dir = hu_strdup(a, uid);
    }
    hu_json_value_t *cors = hu_json_object_get(obj, "cors_origins");
    if (cors && cors->type == HU_JSON_ARRAY) {
        if (cfg->gateway.cors_origins) {
            for (size_t i = 0; i < cfg->gateway.cors_origins_len; i++)
                if (cfg->gateway.cors_origins[i])
                    a->free(a->ctx, cfg->gateway.cors_origins[i],
                            strlen(cfg->gateway.cors_origins[i]) + 1);
            a->free(a->ctx, cfg->gateway.cors_origins,
                    cfg->gateway.cors_origins_len * sizeof(char *));
        }
        parse_string_array(a, &cfg->gateway.cors_origins, &cfg->gateway.cors_origins_len, cors);
    }
    return HU_OK;
}

static hu_error_t parse_memory(hu_allocator_t *a, hu_config_t *cfg, const hu_json_value_t *obj) {
    if (!obj || obj->type != HU_JSON_OBJECT)
        return HU_OK;
    const char *profile = hu_json_get_string(obj, "profile");
    if (profile) {
        if (cfg->memory.profile)
            a->free(a->ctx, cfg->memory.profile, strlen(cfg->memory.profile) + 1);
        cfg->memory.profile = hu_strdup(a, profile);
    }
    const char *backend = hu_json_get_string(obj, "backend");
    if (backend) {
        if (cfg->memory.backend)
            a->free(a->ctx, cfg->memory.backend, strlen(cfg->memory.backend) + 1);
        cfg->memory.backend = hu_strdup(a, backend);
    }
    const char *sqlite_path = hu_json_get_string(obj, "sqlite_path");
    if (sqlite_path) {
        if (cfg->memory.sqlite_path)
            a->free(a->ctx, cfg->memory.sqlite_path, strlen(cfg->memory.sqlite_path) + 1);
        cfg->memory.sqlite_path = hu_strdup(a, sqlite_path);
    }
    double max_ent = hu_json_get_number(obj, "max_entries", cfg->memory.max_entries);
    if (max_ent >= 0 && max_ent <= 1000000)
        cfg->memory.max_entries = (uint32_t)max_ent;
    cfg->memory.auto_save = hu_json_get_bool(obj, "auto_save", cfg->memory.auto_save);
    double cih = hu_json_get_number(obj, "consolidation_interval_hours",
                                    cfg->memory.consolidation_interval_hours);
    if (cih >= 0 && cih <= 8760)
        cfg->memory.consolidation_interval_hours = (uint32_t)cih;

    const char *pg_url = hu_json_get_string(obj, "postgres_url");
    if (pg_url) {
        if (cfg->memory.postgres_url)
            a->free(a->ctx, cfg->memory.postgres_url, strlen(cfg->memory.postgres_url) + 1);
        cfg->memory.postgres_url = hu_strdup(a, pg_url);
    }
    const char *pg_schema = hu_json_get_string(obj, "postgres_schema");
    if (pg_schema) {
        if (cfg->memory.postgres_schema)
            a->free(a->ctx, cfg->memory.postgres_schema, strlen(cfg->memory.postgres_schema) + 1);
        cfg->memory.postgres_schema = hu_strdup(a, pg_schema);
    }
    const char *pg_table = hu_json_get_string(obj, "postgres_table");
    if (pg_table) {
        if (cfg->memory.postgres_table)
            a->free(a->ctx, cfg->memory.postgres_table, strlen(cfg->memory.postgres_table) + 1);
        cfg->memory.postgres_table = hu_strdup(a, pg_table);
    }
    const char *r_host = hu_json_get_string(obj, "redis_host");
    if (r_host) {
        if (cfg->memory.redis_host)
            a->free(a->ctx, cfg->memory.redis_host, strlen(cfg->memory.redis_host) + 1);
        cfg->memory.redis_host = hu_strdup(a, r_host);
    }
    double r_port = hu_json_get_number(obj, "redis_port", cfg->memory.redis_port);
    if (r_port >= 1 && r_port <= 65535)
        cfg->memory.redis_port = (uint16_t)r_port;
    const char *r_prefix = hu_json_get_string(obj, "redis_key_prefix");
    if (r_prefix) {
        if (cfg->memory.redis_key_prefix)
            a->free(a->ctx, cfg->memory.redis_key_prefix, strlen(cfg->memory.redis_key_prefix) + 1);
        cfg->memory.redis_key_prefix = hu_strdup(a, r_prefix);
    }
    const char *api_url = hu_json_get_string(obj, "api_base_url");
    if (api_url) {
        if (cfg->memory.api_base_url)
            a->free(a->ctx, cfg->memory.api_base_url, strlen(cfg->memory.api_base_url) + 1);
        cfg->memory.api_base_url = hu_strdup(a, api_url);
    }
    const char *api_k = hu_json_get_string(obj, "api_key");
    if (api_k) {
        if (cfg->memory.api_key)
            a->free(a->ctx, cfg->memory.api_key, strlen(cfg->memory.api_key) + 1);
        cfg->memory.api_key = hu_strdup(a, api_k);
    }
    double api_tm = hu_json_get_number(obj, "api_timeout_ms", cfg->memory.api_timeout_ms);
    if (api_tm >= 0 && api_tm <= 300000)
        cfg->memory.api_timeout_ms = (uint32_t)api_tm;
    return HU_OK;
}

static hu_error_t parse_tools(hu_allocator_t *a, hu_config_t *cfg, const hu_json_value_t *obj) {
    if (!obj || obj->type != HU_JSON_OBJECT)
        return HU_OK;
    double sts = hu_json_get_number(obj, "shell_timeout_secs", cfg->tools.shell_timeout_secs);
    if (sts >= 1 && sts <= 86400)
        cfg->tools.shell_timeout_secs = (uint64_t)sts;
    double smob =
        hu_json_get_number(obj, "shell_max_output_bytes", cfg->tools.shell_max_output_bytes);
    if (smob >= 0 && smob <= 1073741824)
        cfg->tools.shell_max_output_bytes = (uint32_t)smob;
    double mfsb = hu_json_get_number(obj, "max_file_size_bytes", cfg->tools.max_file_size_bytes);
    if (mfsb >= 0 && mfsb <= 1073741824)
        cfg->tools.max_file_size_bytes = (uint32_t)mfsb;
    double wfmc = hu_json_get_number(obj, "web_fetch_max_chars", cfg->tools.web_fetch_max_chars);
    if (wfmc >= 0 && wfmc <= 10000000)
        cfg->tools.web_fetch_max_chars = (uint32_t)wfmc;
    const char *provider = hu_json_get_string(obj, "web_search_provider");
    if (provider && provider[0]) {
        if (cfg->tools.web_search_provider)
            a->free(a->ctx, cfg->tools.web_search_provider,
                    strlen(cfg->tools.web_search_provider) + 1);
        cfg->tools.web_search_provider = hu_strdup(a, provider);
    }
    hu_json_value_t *en = hu_json_object_get(obj, "enabled_tools");
    if (en && en->type == HU_JSON_ARRAY) {
        if (cfg->tools.enabled_tools) {
            for (size_t i = 0; i < cfg->tools.enabled_tools_len; i++)
                a->free(a->ctx, cfg->tools.enabled_tools[i],
                        strlen(cfg->tools.enabled_tools[i]) + 1);
            a->free(a->ctx, cfg->tools.enabled_tools,
                    cfg->tools.enabled_tools_len * sizeof(char *));
        }
        parse_string_array(a, &cfg->tools.enabled_tools, &cfg->tools.enabled_tools_len, en);
    }
    hu_json_value_t *dis = hu_json_object_get(obj, "disabled_tools");
    if (dis && dis->type == HU_JSON_ARRAY) {
        if (cfg->tools.disabled_tools) {
            for (size_t i = 0; i < cfg->tools.disabled_tools_len; i++)
                a->free(a->ctx, cfg->tools.disabled_tools[i],
                        strlen(cfg->tools.disabled_tools[i]) + 1);
            a->free(a->ctx, cfg->tools.disabled_tools,
                    cfg->tools.disabled_tools_len * sizeof(char *));
        }
        parse_string_array(a, &cfg->tools.disabled_tools, &cfg->tools.disabled_tools_len, dis);
    }
    hu_json_value_t *tmo = hu_json_object_get(obj, "tool_model_overrides");
    if (tmo && tmo->type == HU_JSON_OBJECT && tmo->data.object.pairs) {
        for (size_t i = 0; i < cfg->tools.model_overrides_len; i++) {
            hu_tool_model_override_t *o = &cfg->tools.model_overrides[i];
            if (o->tool_name) {
                a->free(a->ctx, o->tool_name, strlen(o->tool_name) + 1);
                o->tool_name = NULL;
            }
            if (o->provider) {
                a->free(a->ctx, o->provider, strlen(o->provider) + 1);
                o->provider = NULL;
            }
            if (o->model) {
                a->free(a->ctx, o->model, strlen(o->model) + 1);
                o->model = NULL;
            }
        }
        cfg->tools.model_overrides_len = 0;
        for (size_t i = 0; i < tmo->data.object.len &&
                           cfg->tools.model_overrides_len < HU_TOOL_MODEL_OVERRIDES_MAX;
             i++) {
            hu_json_pair_t *p = &tmo->data.object.pairs[i];
            if (!p->key || !p->value || p->value->type != HU_JSON_OBJECT)
                continue;
            const char *prov = hu_json_get_string(p->value, "provider");
            const char *mod = hu_json_get_string(p->value, "model");
            if (!prov || !prov[0] || !mod || !mod[0])
                continue;
            hu_tool_model_override_t *o =
                &cfg->tools.model_overrides[cfg->tools.model_overrides_len];
            o->tool_name = hu_strdup(a, p->key);
            o->provider = hu_strdup(a, prov);
            o->model = hu_strdup(a, mod);
            if (o->tool_name && o->provider && o->model)
                cfg->tools.model_overrides_len++;
            else {
                if (o->tool_name)
                    a->free(a->ctx, o->tool_name, strlen(o->tool_name) + 1);
                if (o->provider)
                    a->free(a->ctx, o->provider, strlen(o->provider) + 1);
                if (o->model)
                    a->free(a->ctx, o->model, strlen(o->model) + 1);
            }
        }
    }
    return HU_OK;
}

static hu_error_t parse_runtime(hu_allocator_t *a, hu_config_t *cfg, const hu_json_value_t *obj) {
    if (!obj || obj->type != HU_JSON_OBJECT)
        return HU_OK;
    const char *kind = hu_json_get_string(obj, "kind");
    if (!kind)
        kind = hu_json_get_string(obj, "type");
    if (kind) {
        if (cfg->runtime.kind)
            a->free(a->ctx, cfg->runtime.kind, strlen(cfg->runtime.kind) + 1);
        cfg->runtime.kind = hu_strdup(a, kind);
    }
    const char *docker_image = hu_json_get_string(obj, "docker_image");
    if (docker_image) {
        if (cfg->runtime.docker_image)
            a->free(a->ctx, cfg->runtime.docker_image, strlen(cfg->runtime.docker_image) + 1);
        cfg->runtime.docker_image = hu_strdup(a, docker_image);
    }
    const char *gce_project = hu_json_get_string(obj, "gce_project");
    if (gce_project) {
        if (cfg->runtime.gce_project)
            a->free(a->ctx, cfg->runtime.gce_project, strlen(cfg->runtime.gce_project) + 1);
        cfg->runtime.gce_project = hu_strdup(a, gce_project);
    }
    const char *gce_zone = hu_json_get_string(obj, "gce_zone");
    if (gce_zone) {
        if (cfg->runtime.gce_zone)
            a->free(a->ctx, cfg->runtime.gce_zone, strlen(cfg->runtime.gce_zone) + 1);
        cfg->runtime.gce_zone = hu_strdup(a, gce_zone);
    }
    const char *gce_instance = hu_json_get_string(obj, "gce_instance");
    if (gce_instance) {
        if (cfg->runtime.gce_instance)
            a->free(a->ctx, cfg->runtime.gce_instance, strlen(cfg->runtime.gce_instance) + 1);
        cfg->runtime.gce_instance = hu_strdup(a, gce_instance);
    }
    return HU_OK;
}

static hu_error_t parse_tunnel(hu_allocator_t *a, hu_config_t *cfg, const hu_json_value_t *obj) {
    if (!obj || obj->type != HU_JSON_OBJECT)
        return HU_OK;
    const char *provider = hu_json_get_string(obj, "provider");
    if (provider) {
        if (cfg->tunnel.provider)
            a->free(a->ctx, cfg->tunnel.provider, strlen(cfg->tunnel.provider) + 1);
        cfg->tunnel.provider = hu_strdup(a, provider);
    }
    const char *domain = hu_json_get_string(obj, "domain");
    if (domain) {
        if (cfg->tunnel.domain)
            a->free(a->ctx, cfg->tunnel.domain, strlen(cfg->tunnel.domain) + 1);
        cfg->tunnel.domain = hu_strdup(a, domain);
    }
    return HU_OK;
}

static hu_error_t parse_mcp_servers(hu_allocator_t *a, hu_config_t *cfg,
                                    const hu_json_value_t *obj) {
    if (!obj || obj->type != HU_JSON_OBJECT)
        return HU_OK;
    cfg->mcp_servers_len = 0;
    for (size_t i = 0; i < obj->data.object.len && cfg->mcp_servers_len < HU_MCP_SERVERS_MAX; i++) {
        hu_json_pair_t *p = &obj->data.object.pairs[i];
        if (!p->key || !p->value || p->value->type != HU_JSON_OBJECT)
            continue;

        hu_mcp_server_entry_t *entry = &cfg->mcp_servers[cfg->mcp_servers_len];
        memset(entry, 0, sizeof(*entry));
        entry->name = hu_strdup(a, p->key);

        const char *cmd = hu_json_get_string(p->value, "command");
        if (cmd)
            entry->command = hu_strdup(a, cmd);

        hu_json_value_t *args_arr = hu_json_object_get(p->value, "args");
        if (args_arr && args_arr->type == HU_JSON_ARRAY) {
            for (size_t j = 0;
                 j < args_arr->data.array.len && entry->args_count < HU_MCP_SERVER_ARGS_MAX; j++) {
                hu_json_value_t *arg_val = args_arr->data.array.items[j];
                if (arg_val && arg_val->type == HU_JSON_STRING && arg_val->data.string.ptr) {
                    entry->args[entry->args_count++] = hu_strdup(a, arg_val->data.string.ptr);
                }
            }
        }
        cfg->mcp_servers_len++;
    }
    return HU_OK;
}

static hu_error_t parse_policy_cfg(hu_allocator_t *a, hu_config_t *cfg,
                                   const hu_json_value_t *obj) {
    if (!obj || obj->type != HU_JSON_OBJECT)
        return HU_OK;
    cfg->policy.enabled = hu_json_get_bool(obj, "enabled", cfg->policy.enabled);
    const char *rj = hu_json_get_string(obj, "rules_json");
    if (rj) {
        if (cfg->policy.rules_json)
            a->free(a->ctx, cfg->policy.rules_json, strlen(cfg->policy.rules_json) + 1);
        cfg->policy.rules_json = hu_strdup(a, rj);
    }
    return HU_OK;
}
static hu_error_t parse_plugins_cfg(hu_allocator_t *a, hu_config_t *cfg,
                                    const hu_json_value_t *obj) {
    if (!obj)
        return HU_OK;
    /* Support "plugins" as object {"enabled": true, "paths": [...]} or as array ["a.so", "b.so"] */
    const hu_json_value_t *paths_arr = NULL;
    if (obj->type == HU_JSON_OBJECT) {
        cfg->plugins.enabled = hu_json_get_bool(obj, "enabled", cfg->plugins.enabled);
        paths_arr = hu_json_object_get(obj, "paths");
        const char *pd = hu_json_get_string(obj, "plugin_dir");
        if (pd) {
            if (cfg->plugins.plugin_dir)
                a->free(a->ctx, cfg->plugins.plugin_dir, strlen(cfg->plugins.plugin_dir) + 1);
            cfg->plugins.plugin_dir = hu_strdup(a, pd);
        }
    } else if (obj->type == HU_JSON_ARRAY) {
        paths_arr = obj;
    } else {
        return HU_OK;
    }
    if (!paths_arr || paths_arr->type != HU_JSON_ARRAY)
        return HU_OK;
    size_t n = paths_arr->data.array.len;
    if (n == 0)
        return HU_OK;
    /* Free existing plugin_paths if re-parsing */
    if (cfg->plugins.plugin_paths) {
        for (size_t i = 0; i < cfg->plugins.plugin_paths_len; i++)
            if (cfg->plugins.plugin_paths[i])
                a->free(a->ctx, cfg->plugins.plugin_paths[i],
                        strlen(cfg->plugins.plugin_paths[i]) + 1);
        a->free(a->ctx, cfg->plugins.plugin_paths, cfg->plugins.plugin_paths_len * sizeof(char *));
        cfg->plugins.plugin_paths = NULL;
        cfg->plugins.plugin_paths_len = 0;
    }
    cfg->plugins.plugin_paths = (char **)a->alloc(a->ctx, n * sizeof(char *));
    if (!cfg->plugins.plugin_paths)
        return HU_ERR_OUT_OF_MEMORY;
    for (size_t i = 0; i < n; i++) {
        const hu_json_value_t *item = paths_arr->data.array.items[i];
        if (item && item->type == HU_JSON_STRING && item->data.string.ptr)
            cfg->plugins.plugin_paths[i] =
                hu_strndup(a, item->data.string.ptr, item->data.string.len);
        else
            cfg->plugins.plugin_paths[i] = NULL;
    }
    cfg->plugins.plugin_paths_len = n;
    return HU_OK;
}
static hu_error_t parse_heartbeat(hu_allocator_t *a, hu_config_t *cfg, const hu_json_value_t *obj) {
    (void)a;
    if (!obj || obj->type != HU_JSON_OBJECT)
        return HU_OK;
    cfg->heartbeat.enabled = hu_json_get_bool(obj, "enabled", cfg->heartbeat.enabled);
    double im = hu_json_get_number(obj, "interval_minutes", cfg->heartbeat.interval_minutes);
    if (!im)
        im = hu_json_get_number(obj, "interval_secs", 0);
    if (im > 0 && im <= 1440)
        cfg->heartbeat.interval_minutes = (uint32_t)im;
    return HU_OK;
}

static hu_error_t parse_reliability(hu_allocator_t *a, hu_config_t *cfg,
                                    const hu_json_value_t *obj) {
    if (!obj || obj->type != HU_JSON_OBJECT)
        return HU_OK;
    const char *pp = hu_json_get_string(obj, "primary_provider");
    if (pp) {
        if (cfg->reliability.primary_provider)
            a->free(a->ctx, cfg->reliability.primary_provider,
                    strlen(cfg->reliability.primary_provider) + 1);
        cfg->reliability.primary_provider = hu_strdup(a, pp);
    }
    double pr = hu_json_get_number(obj, "provider_retries", cfg->reliability.provider_retries);
    if (pr >= 0 && pr <= 20)
        cfg->reliability.provider_retries = (uint32_t)pr;
    double pbm =
        hu_json_get_number(obj, "provider_backoff_ms", cfg->reliability.provider_backoff_ms);
    if (pbm >= 0)
        cfg->reliability.provider_backoff_ms = (uint64_t)pbm;
    double cibs = hu_json_get_number(obj, "channel_initial_backoff_secs",
                                     cfg->reliability.channel_initial_backoff_secs);
    if (cibs >= 0)
        cfg->reliability.channel_initial_backoff_secs = (uint64_t)cibs;
    double cmbs = hu_json_get_number(obj, "channel_max_backoff_secs",
                                     cfg->reliability.channel_max_backoff_secs);
    if (cmbs >= 0)
        cfg->reliability.channel_max_backoff_secs = (uint64_t)cmbs;
    double sps =
        hu_json_get_number(obj, "scheduler_poll_secs", cfg->reliability.scheduler_poll_secs);
    if (sps >= 0)
        cfg->reliability.scheduler_poll_secs = (uint64_t)sps;
    double sr = hu_json_get_number(obj, "scheduler_retries", cfg->reliability.scheduler_retries);
    if (sr >= 0 && sr <= 20)
        cfg->reliability.scheduler_retries = (uint32_t)sr;
    hu_json_value_t *fp = hu_json_object_get(obj, "fallback_providers");
    if (fp && fp->type == HU_JSON_ARRAY) {
        if (cfg->reliability.fallback_providers) {
            for (size_t i = 0; i < cfg->reliability.fallback_providers_len; i++)
                a->free(a->ctx, cfg->reliability.fallback_providers[i],
                        strlen(cfg->reliability.fallback_providers[i]) + 1);
            a->free(a->ctx, cfg->reliability.fallback_providers,
                    cfg->reliability.fallback_providers_len * sizeof(char *));
        }
        parse_string_array(a, &cfg->reliability.fallback_providers,
                           &cfg->reliability.fallback_providers_len, fp);
    }
    return HU_OK;
}

static hu_error_t parse_session(hu_allocator_t *a, hu_config_t *cfg, const hu_json_value_t *obj) {
    (void)a;
    if (!obj || obj->type != HU_JSON_OBJECT)
        return HU_OK;
    double im = hu_json_get_number(obj, "idle_minutes", cfg->session.idle_minutes);
    if (im >= 0 && im <= 1440)
        cfg->session.idle_minutes = (uint32_t)im;
    const char *scope = hu_json_get_string(obj, "dm_scope");
    if (scope) {
        if (strcmp(scope, "main") == 0)
            cfg->session.dm_scope = DirectScopeMain;
        else if (strcmp(scope, "per_peer") == 0)
            cfg->session.dm_scope = DirectScopePerPeer;
        else if (strcmp(scope, "per_channel_peer") == 0)
            cfg->session.dm_scope = DirectScopePerChannelPeer;
        else if (strcmp(scope, "per_account_channel_peer") == 0)
            cfg->session.dm_scope = DirectScopePerAccountChannelPeer;
    }
    return HU_OK;
}

static hu_error_t parse_peripherals(hu_allocator_t *a, hu_config_t *cfg,
                                    const hu_json_value_t *obj) {
    if (!obj || obj->type != HU_JSON_OBJECT)
        return HU_OK;
    cfg->peripherals.enabled = hu_json_get_bool(obj, "enabled", cfg->peripherals.enabled);
    const char *dd = hu_json_get_string(obj, "datasheet_dir");
    if (dd) {
        if (cfg->peripherals.datasheet_dir)
            a->free(a->ctx, cfg->peripherals.datasheet_dir,
                    strlen(cfg->peripherals.datasheet_dir) + 1);
        cfg->peripherals.datasheet_dir = hu_strdup(a, dd);
    }
    return HU_OK;
}

static hu_error_t parse_hardware(hu_allocator_t *a, hu_config_t *cfg, const hu_json_value_t *obj) {
    if (!obj || obj->type != HU_JSON_OBJECT)
        return HU_OK;
    cfg->hardware.enabled = hu_json_get_bool(obj, "enabled", cfg->hardware.enabled);
    const char *transport = hu_json_get_string(obj, "transport");
    if (transport) {
        if (cfg->hardware.transport)
            a->free(a->ctx, cfg->hardware.transport, strlen(cfg->hardware.transport) + 1);
        cfg->hardware.transport = hu_strdup(a, transport);
    }
    const char *serial_port = hu_json_get_string(obj, "serial_port");
    if (serial_port) {
        if (cfg->hardware.serial_port)
            a->free(a->ctx, cfg->hardware.serial_port, strlen(cfg->hardware.serial_port) + 1);
        cfg->hardware.serial_port = hu_strdup(a, serial_port);
    }
    double br = hu_json_get_number(obj, "baud_rate", cfg->hardware.baud_rate);
    if (br >= 0 && br <= 4000000)
        cfg->hardware.baud_rate = (uint32_t)br;
    const char *probe_target = hu_json_get_string(obj, "probe_target");
    if (probe_target) {
        if (cfg->hardware.probe_target)
            a->free(a->ctx, cfg->hardware.probe_target, strlen(cfg->hardware.probe_target) + 1);
        cfg->hardware.probe_target = hu_strdup(a, probe_target);
    }
    return HU_OK;
}

static hu_error_t parse_browser(hu_allocator_t *a, hu_config_t *cfg, const hu_json_value_t *obj) {
    (void)a;
    if (!obj || obj->type != HU_JSON_OBJECT)
        return HU_OK;
    cfg->browser.enabled = hu_json_get_bool(obj, "enabled", cfg->browser.enabled);
    return HU_OK;
}

static hu_error_t parse_cost(hu_allocator_t *a, hu_config_t *cfg, const hu_json_value_t *obj) {
    (void)a;
    if (!obj || obj->type != HU_JSON_OBJECT)
        return HU_OK;
    cfg->cost.enabled = hu_json_get_bool(obj, "enabled", cfg->cost.enabled);
    double dl = hu_json_get_number(obj, "daily_limit_usd", cfg->cost.daily_limit_usd);
    if (dl >= 0 && dl <= 1000000.0)
        cfg->cost.daily_limit_usd = dl;
    double ml = hu_json_get_number(obj, "monthly_limit_usd", cfg->cost.monthly_limit_usd);
    if (ml >= 0 && ml <= 10000000.0)
        cfg->cost.monthly_limit_usd = ml;
    double wp = hu_json_get_number(obj, "warn_at_percent", cfg->cost.warn_at_percent);
    if (wp >= 0 && wp <= 100)
        cfg->cost.warn_at_percent = (uint8_t)wp;
    cfg->cost.allow_override = hu_json_get_bool(obj, "allow_override", cfg->cost.allow_override);
    return HU_OK;
}

static hu_error_t parse_diagnostics(hu_allocator_t *a, hu_config_t *cfg,
                                    const hu_json_value_t *obj) {
    if (!obj || obj->type != HU_JSON_OBJECT)
        return HU_OK;
    const char *backend = hu_json_get_string(obj, "backend");
    if (backend) {
        if (cfg->diagnostics.backend)
            a->free(a->ctx, cfg->diagnostics.backend, strlen(cfg->diagnostics.backend) + 1);
        cfg->diagnostics.backend = hu_strdup(a, backend);
    }
    const char *ote = hu_json_get_string(obj, "otel_endpoint");
    if (ote) {
        if (cfg->diagnostics.otel_endpoint)
            a->free(a->ctx, cfg->diagnostics.otel_endpoint,
                    strlen(cfg->diagnostics.otel_endpoint) + 1);
        cfg->diagnostics.otel_endpoint = hu_strdup(a, ote);
    }
    const char *ots = hu_json_get_string(obj, "otel_service_name");
    if (ots) {
        if (cfg->diagnostics.otel_service_name)
            a->free(a->ctx, cfg->diagnostics.otel_service_name,
                    strlen(cfg->diagnostics.otel_service_name) + 1);
        cfg->diagnostics.otel_service_name = hu_strdup(a, ots);
    }
    cfg->diagnostics.log_tool_calls =
        hu_json_get_bool(obj, "log_tool_calls", cfg->diagnostics.log_tool_calls);
    cfg->diagnostics.log_message_receipts =
        hu_json_get_bool(obj, "log_message_receipts", cfg->diagnostics.log_message_receipts);
    cfg->diagnostics.log_message_payloads =
        hu_json_get_bool(obj, "log_message_payloads", cfg->diagnostics.log_message_payloads);
    cfg->diagnostics.log_llm_io = hu_json_get_bool(obj, "log_llm_io", cfg->diagnostics.log_llm_io);
    if (cfg->diagnostics.log_llm_io || cfg->diagnostics.log_message_payloads) {
        fprintf(stderr, "[SECURITY WARNING] Diagnostic logging of payloads is enabled. "
                        "Logs may contain sensitive data (PII, API keys). "
                        "Do not use in production.\n");
    }
    return HU_OK;
}

hu_error_t hu_config_parse_json(hu_config_t *cfg, const char *content, size_t len) {
    if (!cfg || !content)
        return HU_ERR_INVALID_ARGUMENT;
    hu_allocator_t *a = &cfg->allocator;

    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(a, content, len, &root);
    if (err != HU_OK)
        return err;
    if (!root || root->type != HU_JSON_OBJECT) {
        if (root)
            hu_json_free(a, root);
        return HU_ERR_JSON_PARSE;
    }

    hu_error_t mig_err = hu_config_migrate(a, root);
    if (mig_err != HU_OK) {
        hu_json_free(a, root);
        return mig_err;
    }

    cfg->config_version = (int)hu_json_get_number(root, "config_version", 1.0);

    const char *workspace = hu_json_get_string(root, "workspace");
    if (workspace && strstr(workspace, "..")) {
        workspace = NULL; /* Reject path traversal */
    }
    if (workspace && cfg->workspace_dir_override) {
        a->free(a->ctx, cfg->workspace_dir_override, strlen(cfg->workspace_dir_override) + 1);
    }
    if (workspace) {
        cfg->workspace_dir_override = hu_strdup(a, workspace);
        if (cfg->workspace_dir)
            a->free(a->ctx, cfg->workspace_dir, strlen(cfg->workspace_dir) + 1);
        cfg->workspace_dir = hu_strdup(a, workspace);
    }

    const char *prov = hu_json_get_string(root, "default_provider");
    if (prov) {
        if (cfg->default_provider)
            a->free(a->ctx, cfg->default_provider, strlen(cfg->default_provider) + 1);
        cfg->default_provider = hu_strdup(a, prov);
    }
    const char *model = hu_json_get_string(root, "default_model");
    if (model && model[0]) {
        if (cfg->default_model)
            a->free(a->ctx, cfg->default_model, strlen(cfg->default_model) + 1);
        cfg->default_model = hu_strdup(a, model);
    }
    double temp = hu_json_get_number(root, "default_temperature", cfg->default_temperature);
    if (temp >= 0.0 && temp <= 2.0)
        cfg->default_temperature = temp;

    double mt = hu_json_get_number(root, "max_tokens", (double)cfg->max_tokens);
    if (mt >= 0.0 && mt <= 1000000)
        cfg->max_tokens = (uint32_t)mt;

    hu_json_value_t *prov_arr = hu_json_object_get(root, "providers");
    if (prov_arr) {
        if (prov_arr->type == HU_JSON_ARRAY)
            parse_providers(a, cfg, prov_arr);
        else if (prov_arr->type == HU_JSON_OBJECT) {
            hu_json_value_t *router_obj = hu_json_object_get(prov_arr, "router");
            if (router_obj)
                parse_router(a, cfg, router_obj);
        }
    }

    hu_json_value_t *router_obj = hu_json_object_get(root, "router");
    if (router_obj)
        parse_router(a, cfg, router_obj);

    hu_json_value_t *aut = hu_json_object_get(root, "autonomy");
    if (aut)
        parse_autonomy(a, cfg, aut);

    hu_json_value_t *gw = hu_json_object_get(root, "gateway");
    if (gw)
        parse_gateway(a, cfg, gw);

    hu_json_value_t *mem = hu_json_object_get(root, "memory");
    if (mem)
        parse_memory(a, cfg, mem);

    hu_json_value_t *tools_obj = hu_json_object_get(root, "tools");
    if (tools_obj)
        parse_tools(a, cfg, tools_obj);

    hu_json_value_t *cron_obj = hu_json_object_get(root, "cron");
    if (cron_obj)
        parse_cron(a, cfg, cron_obj);

    hu_json_value_t *sched_obj = hu_json_object_get(root, "scheduler");
    if (sched_obj)
        parse_scheduler(a, cfg, sched_obj);

    hu_json_value_t *rt_obj = hu_json_object_get(root, "runtime");
    if (rt_obj)
        parse_runtime(a, cfg, rt_obj);

    hu_json_value_t *tunnel_obj = hu_json_object_get(root, "tunnel");
    if (tunnel_obj)
        parse_tunnel(a, cfg, tunnel_obj);

    hu_json_value_t *ch_obj = hu_json_object_get(root, "channels");
    if (ch_obj)
        parse_channels(a, cfg, ch_obj);

    hu_json_value_t *agent_obj = hu_json_object_get(root, "agent");
    if (agent_obj)
        parse_agent(a, cfg, agent_obj);

    hu_json_value_t *behavior_obj = hu_json_object_get(root, "behavior");
    if (behavior_obj)
        parse_behavior(a, cfg, behavior_obj);

    hu_json_value_t *heartbeat_obj = hu_json_object_get(root, "heartbeat");
    if (heartbeat_obj)
        parse_heartbeat(a, cfg, heartbeat_obj);

    hu_json_value_t *reliability_obj = hu_json_object_get(root, "reliability");
    if (reliability_obj)
        parse_reliability(a, cfg, reliability_obj);

    hu_json_value_t *diagnostics_obj = hu_json_object_get(root, "diagnostics");
    if (diagnostics_obj)
        parse_diagnostics(a, cfg, diagnostics_obj);

    hu_json_value_t *session_obj = hu_json_object_get(root, "session");
    if (session_obj)
        parse_session(a, cfg, session_obj);

    hu_json_value_t *peripherals_obj = hu_json_object_get(root, "peripherals");
    if (peripherals_obj)
        parse_peripherals(a, cfg, peripherals_obj);

    hu_json_value_t *hardware_obj = hu_json_object_get(root, "hardware");
    if (hardware_obj)
        parse_hardware(a, cfg, hardware_obj);

    hu_json_value_t *browser_obj = hu_json_object_get(root, "browser");
    if (browser_obj)
        parse_browser(a, cfg, browser_obj);

    hu_json_value_t *cost_obj = hu_json_object_get(root, "cost");
    if (cost_obj)
        parse_cost(a, cfg, cost_obj);

    hu_json_value_t *mcp_obj = hu_json_object_get(root, "mcp_servers");
    if (mcp_obj)
        parse_mcp_servers(a, cfg, mcp_obj);

    hu_json_value_t *nodes_obj = hu_json_object_get(root, "nodes");
    if (nodes_obj)
        parse_nodes(a, cfg, nodes_obj);

    hu_json_value_t *policy_obj = hu_json_object_get(root, "policy");
    if (policy_obj)
        parse_policy_cfg(a, cfg, policy_obj);
    hu_json_value_t *plugins_obj = hu_json_object_get(root, "plugins");
    if (plugins_obj)
        parse_plugins_cfg(a, cfg, plugins_obj);
    hu_json_value_t *sec = hu_json_object_get(root, "security");
    if (sec && sec->type == HU_JSON_OBJECT) {
        double al = hu_json_get_number(sec, "autonomy_level", cfg->security.autonomy_level);
        if (al < 0)
            al = 0;
        else if (al > 4)
            al = 4;
        cfg->security.autonomy_level = (uint8_t)al;
        const char *sb = hu_json_get_string(sec, "sandbox");
        if (sb) {
            if (cfg->security.sandbox)
                a->free(a->ctx, cfg->security.sandbox, strlen(cfg->security.sandbox) + 1);
            cfg->security.sandbox = hu_strdup(a, sb);
            cfg->security.sandbox_config.backend = hu_config_parse_sandbox_backend(sb);
        }
        hu_json_value_t *sbox = hu_json_object_get(sec, "sandbox_config");
        if (sbox && sbox->type == HU_JSON_OBJECT) {
            cfg->security.sandbox_config.enabled =
                hu_json_get_bool(sbox, "enabled", cfg->security.sandbox_config.enabled);
            const char *be = hu_json_get_string(sbox, "backend");
            if (be)
                cfg->security.sandbox_config.backend = hu_config_parse_sandbox_backend(be);
            hu_json_value_t *fa = hu_json_object_get(sbox, "firejail_args");
            if (fa && fa->type == HU_JSON_ARRAY) {
                if (cfg->security.sandbox_config.firejail_args) {
                    for (size_t i = 0; i < cfg->security.sandbox_config.firejail_args_len; i++)
                        a->free(a->ctx, cfg->security.sandbox_config.firejail_args[i],
                                strlen(cfg->security.sandbox_config.firejail_args[i]) + 1);
                    a->free(a->ctx, cfg->security.sandbox_config.firejail_args,
                            cfg->security.sandbox_config.firejail_args_len * sizeof(char *));
                }
                parse_string_array(a, &cfg->security.sandbox_config.firejail_args,
                                   &cfg->security.sandbox_config.firejail_args_len, fa);
            }
            hu_json_value_t *np = hu_json_object_get(sbox, "net_proxy");
            if (np && np->type == HU_JSON_OBJECT) {
                cfg->security.sandbox_config.net_proxy.enabled =
                    hu_json_get_bool(np, "enabled", false);
                cfg->security.sandbox_config.net_proxy.deny_all =
                    hu_json_get_bool(np, "deny_all", true);
                const char *pa = hu_json_get_string(np, "proxy_addr");
                if (pa) {
                    if (cfg->security.sandbox_config.net_proxy.proxy_addr)
                        a->free(a->ctx, cfg->security.sandbox_config.net_proxy.proxy_addr,
                                strlen(cfg->security.sandbox_config.net_proxy.proxy_addr) + 1);
                    cfg->security.sandbox_config.net_proxy.proxy_addr = hu_strdup(a, pa);
                }
                hu_json_value_t *ad = hu_json_object_get(np, "allowed_domains");
                if (ad && ad->type == HU_JSON_ARRAY) {
                    if (cfg->security.sandbox_config.net_proxy.allowed_domains) {
                        for (size_t i = 0;
                             i < cfg->security.sandbox_config.net_proxy.allowed_domains_len; i++)
                            a->free(
                                a->ctx, cfg->security.sandbox_config.net_proxy.allowed_domains[i],
                                strlen(cfg->security.sandbox_config.net_proxy.allowed_domains[i]) +
                                    1);
                        a->free(a->ctx, cfg->security.sandbox_config.net_proxy.allowed_domains,
                                cfg->security.sandbox_config.net_proxy.allowed_domains_len *
                                    sizeof(char *));
                    }
                    parse_string_array(a, &cfg->security.sandbox_config.net_proxy.allowed_domains,
                                       &cfg->security.sandbox_config.net_proxy.allowed_domains_len,
                                       ad);
                }
            }
        }
        hu_json_value_t *res = hu_json_object_get(sec, "resources");
        if (res && res->type == HU_JSON_OBJECT) {
            double mfs = hu_json_get_number(res, "max_file_size",
                                            cfg->security.resource_limits.max_file_size);
            if (mfs >= 0)
                cfg->security.resource_limits.max_file_size = (uint64_t)mfs;
            double mrs = hu_json_get_number(res, "max_read_size",
                                            cfg->security.resource_limits.max_read_size);
            if (mrs >= 0)
                cfg->security.resource_limits.max_read_size = (uint64_t)mrs;
            double mmb = hu_json_get_number(res, "max_memory_mb",
                                            cfg->security.resource_limits.max_memory_mb);
            if (mmb >= 0 && mmb <= 1048576)
                cfg->security.resource_limits.max_memory_mb = (uint32_t)mmb;
        }
        hu_json_value_t *aud = hu_json_object_get(sec, "audit");
        if (aud && aud->type == HU_JSON_OBJECT) {
            cfg->security.audit.enabled =
                hu_json_get_bool(aud, "enabled", cfg->security.audit.enabled);
            const char *lp = hu_json_get_string(aud, "log_path");
            if (!lp)
                lp = hu_json_get_string(aud, "log_file");
            if (lp) {
                if (cfg->security.audit.log_path)
                    a->free(a->ctx, cfg->security.audit.log_path,
                            strlen(cfg->security.audit.log_path) + 1);
                cfg->security.audit.log_path = hu_strdup(a, lp);
            }
        }
    }

    hu_json_value_t *gw_obj = hu_json_object_get(root, "gateway");
    if (gw_obj) {
        double port = hu_json_get_number(gw_obj, "port", cfg->gateway.port);
        if (port < 1)
            port = 1;
        else if (port > 65535)
            port = 65535;
        cfg->gateway.port = (uint16_t)port;
    }

    const char *api_k_root = hu_json_get_string(root, "api_key");
    if (api_k_root && api_k_root[0]) {
        if (cfg->api_key)
            a->free(a->ctx, cfg->api_key, strlen(cfg->api_key) + 1);
        cfg->api_key = hu_strdup(a, api_k_root);
    }

    hu_json_value_t *secrets_obj = hu_json_object_get(root, "secrets");
    if (secrets_obj && secrets_obj->type == HU_JSON_OBJECT) {
        cfg->secrets.encrypt = hu_json_get_bool(secrets_obj, "encrypt", cfg->secrets.encrypt);
    }

    hu_json_value_t *identity_obj = hu_json_object_get(root, "identity");
    if (identity_obj && identity_obj->type == HU_JSON_OBJECT) {
        const char *fmt = hu_json_get_string(identity_obj, "format");
        if (fmt) {
            if (cfg->identity.format)
                a->free(a->ctx, cfg->identity.format, strlen(cfg->identity.format) + 1);
            cfg->identity.format = hu_strdup(a, fmt);
        }
    }

    hu_json_free(a, root);
    return HU_OK;
}

hu_error_t hu_config_parse_string_array(hu_allocator_t *alloc, const hu_json_value_t *arr,
                                        char ***out_strings, size_t *out_count) {
    if (!alloc || !out_strings || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_strings = NULL;
    *out_count = 0;

    if (!arr || arr->type != HU_JSON_ARRAY)
        return HU_OK;

    size_t n = 0;
    hu_json_value_t **items = arr->data.array.items;
    if (!items)
        return HU_OK;
    for (size_t i = 0; i < arr->data.array.len; i++) {
        if (items[i] && items[i]->type == HU_JSON_STRING)
            n++;
    }
    if (n == 0)
        return HU_OK;

    char **strings = (char **)alloc->alloc(alloc->ctx, sizeof(char *) * n);
    if (!strings)
        return HU_ERR_OUT_OF_MEMORY;

    size_t j = 0;
    for (size_t i = 0; i < arr->data.array.len && j < n; i++) {
        if (!items[i] || items[i]->type != HU_JSON_STRING)
            continue;
        const char *s = items[i]->data.string.ptr;
        size_t len = items[i]->data.string.len;
        strings[j] = len > 0 ? hu_strndup(alloc, s, len) : hu_strdup(alloc, "");
        if (!strings[j]) {
            while (j > 0) {
                j--;
                alloc->free(alloc->ctx, strings[j], strlen(strings[j]) + 1);
            }
            alloc->free(alloc->ctx, strings, sizeof(char *) * n);
            return HU_ERR_OUT_OF_MEMORY;
        }
        j++;
    }

    *out_strings = strings;
    *out_count = j;
    return HU_OK;
}
