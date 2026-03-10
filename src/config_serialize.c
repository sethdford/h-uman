#include "config_internal.h"
#include "human/config.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

hu_error_t hu_config_save(const hu_config_t *cfg) {
    if (!cfg || !cfg->config_path)
        return HU_ERR_INVALID_ARGUMENT;
    hu_allocator_t a = cfg->allocator;

    char dir_buf[HU_MAX_PATH];
    const char *home = getenv("HOME");
    if (home) {
        int n = snprintf(dir_buf, sizeof(dir_buf), "%s/%s", home, HU_CONFIG_DIR);
        if (n > 0 && (size_t)n < sizeof(dir_buf))
            (void)mkdir(dir_buf, 0700);
    }

    hu_json_value_t *root = hu_json_object_new(&a);
    if (!root)
        return HU_ERR_OUT_OF_MEMORY;

    if (cfg->workspace_dir) {
        hu_json_value_t *ws =
            hu_json_string_new(&a, cfg->workspace_dir, strlen(cfg->workspace_dir));
        if (ws)
            hu_json_object_set(&a, root, "workspace", ws);
    }
    if (cfg->default_provider) {
        hu_json_value_t *dp =
            hu_json_string_new(&a, cfg->default_provider, strlen(cfg->default_provider));
        if (dp)
            hu_json_object_set(&a, root, "default_provider", dp);
    }
    if (cfg->default_model) {
        hu_json_value_t *dm =
            hu_json_string_new(&a, cfg->default_model, strlen(cfg->default_model));
        if (dm)
            hu_json_object_set(&a, root, "default_model", dm);
    }
    hu_json_object_set(&a, root, "default_temperature",
                       hu_json_number_new(&a, cfg->default_temperature));
    if (cfg->max_tokens > 0)
        hu_json_object_set(&a, root, "max_tokens", hu_json_number_new(&a, (double)cfg->max_tokens));

    hu_json_object_set(&a, root, "temperature", hu_json_number_new(&a, cfg->temperature));

    /* gateway */
    hu_json_value_t *gw = hu_json_object_new(&a);
    if (gw) {
        hu_json_object_set(&a, gw, "enabled", hu_json_bool_new(&a, cfg->gateway.enabled));
        hu_json_object_set(&a, gw, "port", hu_json_number_new(&a, cfg->gateway.port));
        if (cfg->gateway.host)
            hu_json_object_set(
                &a, gw, "host",
                hu_json_string_new(&a, cfg->gateway.host, strlen(cfg->gateway.host)));
        if (cfg->gateway.control_ui_dir)
            hu_json_object_set(&a, gw, "control_ui_dir",
                               hu_json_string_new(&a, cfg->gateway.control_ui_dir,
                                                  strlen(cfg->gateway.control_ui_dir)));
        hu_json_object_set(&a, gw, "require_pairing",
                           hu_json_bool_new(&a, cfg->gateway.require_pairing));
        hu_json_object_set(&a, root, "gateway", gw);
    }

    /* memory */
    hu_json_value_t *mem = hu_json_object_new(&a);
    if (mem) {
        if (cfg->memory.backend)
            hu_json_object_set(
                &a, mem, "backend",
                hu_json_string_new(&a, cfg->memory.backend, strlen(cfg->memory.backend)));
        if (cfg->memory.sqlite_path)
            hu_json_object_set(
                &a, mem, "sqlite_path",
                hu_json_string_new(&a, cfg->memory.sqlite_path, strlen(cfg->memory.sqlite_path)));
        hu_json_object_set(&a, mem, "auto_save", hu_json_bool_new(&a, cfg->memory.auto_save));
        hu_json_object_set(&a, mem, "consolidation_interval_hours",
                           hu_json_number_new(&a, cfg->memory.consolidation_interval_hours));
        hu_json_object_set(&a, root, "memory", mem);
    }

    /* security */
    hu_json_value_t *sec = hu_json_object_new(&a);
    if (sec) {
        hu_json_object_set(&a, sec, "autonomy_level",
                           hu_json_number_new(&a, cfg->security.autonomy_level));
        if (cfg->security.sandbox)
            hu_json_object_set(
                &a, sec, "sandbox",
                hu_json_string_new(&a, cfg->security.sandbox, strlen(cfg->security.sandbox)));

        /* sandbox_config */
        hu_json_value_t *sbc = hu_json_object_new(&a);
        if (sbc) {
            hu_json_object_set(&a, sbc, "enabled",
                               hu_json_bool_new(&a, cfg->security.sandbox_config.enabled));

            const char *be_str =
                hu_config_sandbox_backend_to_string(cfg->security.sandbox_config.backend);
            hu_json_object_set(&a, sbc, "backend", hu_json_string_new(&a, be_str, strlen(be_str)));

            if (cfg->security.sandbox_config.firejail_args_len > 0 &&
                cfg->security.sandbox_config.firejail_args) {
                hu_json_value_t *fja = hu_json_array_new(&a);
                if (fja) {
                    for (size_t fi = 0; fi < cfg->security.sandbox_config.firejail_args_len; fi++) {
                        const char *arg = cfg->security.sandbox_config.firejail_args[fi];
                        if (arg)
                            hu_json_array_push(&a, fja, hu_json_string_new(&a, arg, strlen(arg)));
                    }
                    hu_json_object_set(&a, sbc, "firejail_args", fja);
                }
            }

            /* net_proxy */
            hu_json_value_t *np = hu_json_object_new(&a);
            if (np) {
                hu_json_object_set(
                    &a, np, "enabled",
                    hu_json_bool_new(&a, cfg->security.sandbox_config.net_proxy.enabled));
                hu_json_object_set(
                    &a, np, "deny_all",
                    hu_json_bool_new(&a, cfg->security.sandbox_config.net_proxy.deny_all));
                if (cfg->security.sandbox_config.net_proxy.proxy_addr)
                    hu_json_object_set(
                        &a, np, "proxy_addr",
                        hu_json_string_new(
                            &a, cfg->security.sandbox_config.net_proxy.proxy_addr,
                            strlen(cfg->security.sandbox_config.net_proxy.proxy_addr)));
                if (cfg->security.sandbox_config.net_proxy.allowed_domains_len > 0 &&
                    cfg->security.sandbox_config.net_proxy.allowed_domains) {
                    hu_json_value_t *da = hu_json_array_new(&a);
                    if (da) {
                        for (size_t di = 0;
                             di < cfg->security.sandbox_config.net_proxy.allowed_domains_len;
                             di++) {
                            const char *dom =
                                cfg->security.sandbox_config.net_proxy.allowed_domains[di];
                            if (dom)
                                hu_json_array_push(&a, da,
                                                   hu_json_string_new(&a, dom, strlen(dom)));
                        }
                        hu_json_object_set(&a, np, "allowed_domains", da);
                    }
                }
                hu_json_object_set(&a, sbc, "net_proxy", np);
            }

            hu_json_object_set(&a, sec, "sandbox_config", sbc);
        }

        hu_json_object_set(&a, root, "security", sec);
    }

    /* tools */
    hu_json_value_t *tools_obj = hu_json_object_new(&a);
    if (tools_obj) {
        if (cfg->tools.shell_timeout_secs > 0)
            hu_json_object_set(&a, tools_obj, "shell_timeout_secs",
                               hu_json_number_new(&a, (double)cfg->tools.shell_timeout_secs));
        if (cfg->tools.max_file_size_bytes > 0)
            hu_json_object_set(&a, tools_obj, "max_file_size_bytes",
                               hu_json_number_new(&a, (double)cfg->tools.max_file_size_bytes));
        hu_json_object_set(&a, root, "tools", tools_obj);
    }

    /* cost */
    hu_json_value_t *cost = hu_json_object_new(&a);
    if (cost) {
        hu_json_object_set(&a, cost, "enabled", hu_json_bool_new(&a, cfg->cost.enabled));
        if (cfg->cost.daily_limit_usd > 0)
            hu_json_object_set(&a, cost, "daily_limit_usd",
                               hu_json_number_new(&a, cfg->cost.daily_limit_usd));
        if (cfg->cost.monthly_limit_usd > 0)
            hu_json_object_set(&a, cost, "monthly_limit_usd",
                               hu_json_number_new(&a, cfg->cost.monthly_limit_usd));
        hu_json_object_set(&a, root, "cost", cost);
    }

    /* agent */
    hu_json_value_t *agent_obj = hu_json_object_new(&a);
    if (agent_obj) {
        if (cfg->agent.max_tool_iterations > 0)
            hu_json_object_set(&a, agent_obj, "max_tool_iterations",
                               hu_json_number_new(&a, (double)cfg->agent.max_tool_iterations));
        if (cfg->agent.max_history_messages > 0)
            hu_json_object_set(&a, agent_obj, "max_history_messages",
                               hu_json_number_new(&a, (double)cfg->agent.max_history_messages));
        hu_json_object_set(&a, root, "agent", agent_obj);
    }

    /* secrets */
    hu_json_value_t *secrets_obj = hu_json_object_new(&a);
    if (secrets_obj) {
        hu_json_object_set(&a, secrets_obj, "encrypt", hu_json_bool_new(&a, cfg->secrets.encrypt));
        hu_json_object_set(&a, root, "secrets", secrets_obj);
    }

    /* identity */
    if (cfg->identity.format) {
        hu_json_value_t *id_obj = hu_json_object_new(&a);
        if (id_obj) {
            hu_json_object_set(
                &a, id_obj, "format",
                hu_json_string_new(&a, cfg->identity.format, strlen(cfg->identity.format)));
            hu_json_object_set(&a, root, "identity", id_obj);
        }
    }

    /* diagnostics */
    hu_json_value_t *diag = hu_json_object_new(&a);
    if (diag) {
        if (cfg->diagnostics.backend)
            hu_json_object_set(
                &a, diag, "backend",
                hu_json_string_new(&a, cfg->diagnostics.backend, strlen(cfg->diagnostics.backend)));
        if (cfg->diagnostics.otel_endpoint)
            hu_json_object_set(&a, diag, "otel_endpoint",
                               hu_json_string_new(&a, cfg->diagnostics.otel_endpoint,
                                                  strlen(cfg->diagnostics.otel_endpoint)));
        hu_json_object_set(&a, diag, "log_tool_calls",
                           hu_json_bool_new(&a, cfg->diagnostics.log_tool_calls));
        hu_json_object_set(&a, diag, "log_llm_io",
                           hu_json_bool_new(&a, cfg->diagnostics.log_llm_io));
        hu_json_object_set(&a, root, "diagnostics", diag);
    }

    /* autonomy */
    hu_json_value_t *auton = hu_json_object_new(&a);
    if (auton) {
        if (cfg->autonomy.level)
            hu_json_object_set(
                &a, auton, "level",
                hu_json_string_new(&a, cfg->autonomy.level, strlen(cfg->autonomy.level)));
        hu_json_object_set(&a, auton, "workspace_only",
                           hu_json_bool_new(&a, cfg->autonomy.workspace_only));
        if (cfg->autonomy.max_actions_per_hour > 0)
            hu_json_object_set(&a, auton, "max_actions_per_hour",
                               hu_json_number_new(&a, (double)cfg->autonomy.max_actions_per_hour));
        hu_json_object_set(&a, root, "autonomy", auton);
    }

    /* reliability */
    hu_json_value_t *rel = hu_json_object_new(&a);
    if (rel) {
        if (cfg->reliability.provider_retries > 0)
            hu_json_object_set(&a, rel, "provider_retries",
                               hu_json_number_new(&a, (double)cfg->reliability.provider_retries));
        if (cfg->reliability.provider_backoff_ms > 0)
            hu_json_object_set(
                &a, rel, "provider_backoff_ms",
                hu_json_number_new(&a, (double)cfg->reliability.provider_backoff_ms));
        hu_json_object_set(&a, root, "reliability", rel);
    }

    /* channels */
    if (cfg->channels.default_channel || cfg->channels.cli) {
        hu_json_value_t *ch = hu_json_object_new(&a);
        if (ch) {
            hu_json_object_set(&a, ch, "cli", hu_json_bool_new(&a, cfg->channels.cli));
            if (cfg->channels.default_channel)
                hu_json_object_set(&a, ch, "default",
                                   hu_json_string_new(&a, cfg->channels.default_channel,
                                                      strlen(cfg->channels.default_channel)));
            hu_json_object_set(&a, root, "channels", ch);
        }
    }

    /* tunnel */
    if (cfg->tunnel.provider) {
        hu_json_value_t *tun = hu_json_object_new(&a);
        if (tun) {
            hu_json_object_set(
                &a, tun, "provider",
                hu_json_string_new(&a, cfg->tunnel.provider, strlen(cfg->tunnel.provider)));
            if (cfg->tunnel.domain)
                hu_json_object_set(
                    &a, tun, "domain",
                    hu_json_string_new(&a, cfg->tunnel.domain, strlen(cfg->tunnel.domain)));
            hu_json_object_set(&a, root, "tunnel", tun);
        }
    }

    /* cron */
    hu_json_value_t *cron_obj = hu_json_object_new(&a);
    if (cron_obj) {
        hu_json_object_set(&a, cron_obj, "enabled", hu_json_bool_new(&a, cfg->cron.enabled));
        if (cfg->cron.interval_minutes > 0)
            hu_json_object_set(&a, cron_obj, "interval_minutes",
                               hu_json_number_new(&a, (double)cfg->cron.interval_minutes));
        hu_json_object_set(&a, root, "cron", cron_obj);
    }

    /* scheduler */
    if (cfg->scheduler.max_concurrent > 0) {
        hu_json_value_t *sched = hu_json_object_new(&a);
        if (sched) {
            hu_json_object_set(&a, sched, "max_concurrent",
                               hu_json_number_new(&a, (double)cfg->scheduler.max_concurrent));
            hu_json_object_set(&a, root, "scheduler", sched);
        }
    }

    /* session */
    if (cfg->session.idle_minutes > 0) {
        hu_json_value_t *sess = hu_json_object_new(&a);
        if (sess) {
            hu_json_object_set(&a, sess, "idle_minutes",
                               hu_json_number_new(&a, (double)cfg->session.idle_minutes));
            hu_json_object_set(&a, root, "session", sess);
        }
    }

    /* peripherals */
    hu_json_value_t *periph = hu_json_object_new(&a);
    if (periph) {
        hu_json_object_set(&a, periph, "enabled", hu_json_bool_new(&a, cfg->peripherals.enabled));
        if (cfg->peripherals.datasheet_dir)
            hu_json_object_set(&a, periph, "datasheet_dir",
                               hu_json_string_new(&a, cfg->peripherals.datasheet_dir,
                                                  strlen(cfg->peripherals.datasheet_dir)));
        hu_json_object_set(&a, root, "peripherals", periph);
    }

    /* hardware */
    if (cfg->hardware.enabled) {
        hu_json_value_t *hw = hu_json_object_new(&a);
        if (hw) {
            hu_json_object_set(&a, hw, "enabled", hu_json_bool_new(&a, cfg->hardware.enabled));
            if (cfg->hardware.transport)
                hu_json_object_set(&a, hw, "transport",
                                   hu_json_string_new(&a, cfg->hardware.transport,
                                                      strlen(cfg->hardware.transport)));
            if (cfg->hardware.serial_port)
                hu_json_object_set(&a, hw, "serial_port",
                                   hu_json_string_new(&a, cfg->hardware.serial_port,
                                                      strlen(cfg->hardware.serial_port)));
            if (cfg->hardware.baud_rate > 0)
                hu_json_object_set(&a, hw, "baud_rate",
                                   hu_json_number_new(&a, (double)cfg->hardware.baud_rate));
            hu_json_object_set(&a, root, "hardware", hw);
        }
    }

    /* browser */
    hu_json_object_set(&a, root, "browser", hu_json_bool_new(&a, cfg->browser.enabled));

    /* mcp_servers */
    if (cfg->mcp_servers_len > 0) {
        hu_json_value_t *mcp_arr = hu_json_array_new(&a);
        if (mcp_arr) {
            for (size_t i = 0; i < cfg->mcp_servers_len; i++) {
                hu_json_value_t *me = hu_json_object_new(&a);
                if (!me)
                    continue;
                if (cfg->mcp_servers[i].name)
                    hu_json_object_set(&a, me, "name",
                                       hu_json_string_new(&a, cfg->mcp_servers[i].name,
                                                          strlen(cfg->mcp_servers[i].name)));
                if (cfg->mcp_servers[i].command)
                    hu_json_object_set(&a, me, "command",
                                       hu_json_string_new(&a, cfg->mcp_servers[i].command,
                                                          strlen(cfg->mcp_servers[i].command)));
                if (cfg->mcp_servers[i].args_count > 0) {
                    hu_json_value_t *args_arr = hu_json_array_new(&a);
                    if (args_arr) {
                        for (size_t j = 0; j < cfg->mcp_servers[i].args_count; j++) {
                            if (cfg->mcp_servers[i].args[j])
                                hu_json_array_push(
                                    &a, args_arr,
                                    hu_json_string_new(&a, cfg->mcp_servers[i].args[j],
                                                       strlen(cfg->mcp_servers[i].args[j])));
                        }
                        hu_json_object_set(&a, me, "args", args_arr);
                    }
                }
                hu_json_array_push(&a, mcp_arr, me);
            }
            hu_json_object_set(&a, root, "mcp_servers", mcp_arr);
        }
    }

    /* providers array */
    if (cfg->providers_len > 0) {
        hu_json_value_t *parr = hu_json_array_new(&a);
        if (parr) {
            for (size_t i = 0; i < cfg->providers_len; i++) {
                hu_json_value_t *pe = hu_json_object_new(&a);
                if (!pe)
                    continue;
                if (cfg->providers[i].name)
                    hu_json_object_set(&a, pe, "name",
                                       hu_json_string_new(&a, cfg->providers[i].name,
                                                          strlen(cfg->providers[i].name)));
                if (cfg->providers[i].base_url)
                    hu_json_object_set(&a, pe, "base_url",
                                       hu_json_string_new(&a, cfg->providers[i].base_url,
                                                          strlen(cfg->providers[i].base_url)));
                if (cfg->providers[i].native_tools)
                    hu_json_object_set(&a, pe, "native_tools",
                                       hu_json_bool_new(&a, cfg->providers[i].native_tools));
                hu_json_array_push(&a, parr, pe);
            }
            hu_json_object_set(&a, root, "providers", parr);
        }
    }

    char *json_str = NULL;
    size_t json_len = 0;
    hu_error_t err = hu_json_stringify(&a, root, &json_str, &json_len);
    hu_json_free(&a, root);
    if (err != HU_OK)
        return err;
    if (!json_str)
        return HU_ERR_OUT_OF_MEMORY;

    FILE *f = fopen(cfg->config_path, "w");
    if (!f) {
        a.free(a.ctx, json_str, json_len + 1);
        return HU_ERR_IO;
    }
    fwrite(json_str, 1, json_len, f);
    fclose(f);
    a.free(a.ctx, json_str, json_len + 1);
    return HU_OK;
}
