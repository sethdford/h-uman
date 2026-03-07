/*
 * Shared bootstrap logic for service-loop and gateway commands.
 * Extracts config loading, provider creation, tools setup, security init,
 * channel creation, and agent creation into a single module.
 */

#include "seaclaw/bootstrap.h"
#include "seaclaw/agent/mailbox.h"
#include "seaclaw/agent/spawn.h"
#include "seaclaw/config.h"
#include "seaclaw/memory.h"
#include "seaclaw/memory/engines.h"
#include "seaclaw/memory/factory.h"
#include "seaclaw/memory/retrieval.h"
#include "seaclaw/memory/vector.h"
#include "seaclaw/observability/log_observer.h"
#include "seaclaw/plugin.h"
#include "seaclaw/plugin_loader.h"
#include "seaclaw/providers/factory.h"
#include "seaclaw/runtime.h"
#include "seaclaw/security/audit.h"
#include "seaclaw/security/sandbox.h"
#include "seaclaw/security/sandbox_internal.h"
#include "seaclaw/tool.h"
#include "seaclaw/tools/factory.h"
#include <stdlib.h>
#include <string.h>

#if SC_HAS_EMAIL
#include "seaclaw/channels/email.h"
#endif
#if SC_HAS_IMESSAGE
#include "seaclaw/channels/imessage.h"
#endif
#if SC_HAS_IMAP
#include "seaclaw/channels/imap.h"
#endif
#if SC_HAS_GMAIL
#include "seaclaw/channels/gmail.h"
#endif
#if SC_HAS_TELEGRAM
#include "seaclaw/channels/telegram.h"
#endif
#if SC_HAS_DISCORD
#include "seaclaw/channels/discord.h"
#endif
#if SC_HAS_SLACK
#include "seaclaw/channels/slack.h"
#endif
#if SC_HAS_WHATSAPP
#include "seaclaw/channels/whatsapp.h"
#endif
#if SC_HAS_FACEBOOK
#include "seaclaw/channels/facebook.h"
#endif
#if SC_HAS_LINE
#include "seaclaw/channels/line.h"
#endif
#if SC_HAS_GOOGLE_CHAT
#include "seaclaw/channels/google_chat.h"
#endif
#if SC_HAS_INSTAGRAM
#include "seaclaw/channels/instagram.h"
#endif
#if SC_HAS_TWITTER
#include "seaclaw/channels/twitter.h"
#endif
#if SC_HAS_GOOGLE_RCS
#include "seaclaw/channels/google_rcs.h"
#endif
#if SC_HAS_MQTT
#include "seaclaw/channels/mqtt.h"
#endif
#if SC_HAS_MATRIX
#include "seaclaw/channels/matrix.h"
#endif
#if SC_HAS_IRC
#include "seaclaw/channels/irc.h"
#endif
#if SC_HAS_NOSTR
#include "seaclaw/channels/nostr.h"
#endif
#if SC_HAS_LARK
#include "seaclaw/channels/lark.h"
#endif
#if SC_HAS_DINGTALK
#include "seaclaw/channels/dingtalk.h"
#endif
#if SC_HAS_TEAMS
#include "seaclaw/channels/teams.h"
#endif
#if SC_HAS_TWILIO
#include "seaclaw/channels/twilio.h"
#endif
#if SC_HAS_ONEBOT
#include "seaclaw/channels/onebot.h"
#endif
#if SC_HAS_QQ
#include "seaclaw/channels/qq.h"
#endif

#ifdef SC_HAS_CRON
#include "seaclaw/cron.h"
#include "seaclaw/crontab.h"
#endif

#define SC_BOOTSTRAP_CHANNELS_MAX 20

/* Channel destroy callback: (ch, alloc). Most channels ignore alloc. */
typedef void (*sc_bootstrap_channel_destroy_fn)(sc_channel_t *ch, sc_allocator_t *alloc);

static void destroy_email_wrap(sc_channel_t *ch, sc_allocator_t *a) {
    (void)a;
#if SC_HAS_EMAIL
    sc_email_destroy(ch);
#endif
    (void)ch;
}
static void destroy_imessage_wrap(sc_channel_t *ch, sc_allocator_t *a) {
    (void)a;
#if SC_HAS_IMESSAGE
    sc_imessage_destroy(ch);
#endif
    (void)ch;
}
static void destroy_gmail_wrap(sc_channel_t *ch, sc_allocator_t *a) {
    (void)a;
#if SC_HAS_GMAIL
    sc_gmail_destroy(ch);
#endif
    (void)ch;
}
static void destroy_imap_wrap(sc_channel_t *ch, sc_allocator_t *a) {
    (void)a;
#if SC_HAS_IMAP
    sc_imap_destroy(ch);
#endif
    (void)ch;
}
static void destroy_telegram_wrap(sc_channel_t *ch, sc_allocator_t *a) {
    (void)a;
#if SC_HAS_TELEGRAM
    sc_telegram_destroy(ch);
#endif
    (void)ch;
}
#if SC_HAS_DISCORD
static void destroy_discord_wrap(sc_channel_t *ch, sc_allocator_t *a) {
    (void)a;
    sc_discord_destroy(ch);
    (void)ch;
}
#endif
static void destroy_slack_wrap(sc_channel_t *ch, sc_allocator_t *a) {
    (void)a;
#if SC_HAS_SLACK
    sc_slack_destroy(ch);
#endif
    (void)ch;
}
static void destroy_whatsapp_wrap(sc_channel_t *ch, sc_allocator_t *a) {
    (void)a;
#if SC_HAS_WHATSAPP
    sc_whatsapp_destroy(ch);
#endif
    (void)ch;
}
#if SC_HAS_FACEBOOK
static void destroy_facebook_wrap(sc_channel_t *ch, sc_allocator_t *a) {
    (void)a;
    sc_facebook_destroy(ch);
    (void)ch;
}
#endif
#if SC_HAS_LINE
static void destroy_line_wrap(sc_channel_t *ch, sc_allocator_t *a) {
    (void)a;
    sc_line_destroy(ch);
    (void)ch;
}
#endif
#if SC_HAS_GOOGLE_CHAT
static void destroy_google_chat_wrap(sc_channel_t *ch, sc_allocator_t *a) {
    (void)a;
    sc_google_chat_destroy(ch);
    (void)ch;
}
#endif
#if SC_HAS_INSTAGRAM
static void destroy_instagram_wrap(sc_channel_t *ch, sc_allocator_t *a) {
    (void)a;
    sc_instagram_destroy(ch);
    (void)ch;
}
#endif
#if SC_HAS_TWITTER
static void destroy_twitter_wrap(sc_channel_t *ch, sc_allocator_t *a) {
    (void)a;
    sc_twitter_destroy(ch);
    (void)ch;
}
#endif
#if SC_HAS_GOOGLE_RCS
static void destroy_google_rcs_wrap(sc_channel_t *ch, sc_allocator_t *a) {
    (void)a;
    sc_google_rcs_destroy(ch);
    (void)ch;
}
#endif
#if SC_HAS_MQTT
static void destroy_mqtt_wrap(sc_channel_t *ch, sc_allocator_t *a) {
    sc_mqtt_destroy(ch, a);
}
#endif
#if SC_HAS_MATRIX
static void destroy_matrix_wrap(sc_channel_t *ch, sc_allocator_t *a) {
    (void)a;
    sc_matrix_destroy(ch);
    (void)ch;
}
#endif
#if SC_HAS_IRC
static void destroy_irc_wrap(sc_channel_t *ch, sc_allocator_t *a) {
    (void)a;
    sc_irc_destroy(ch);
    (void)ch;
}
#endif
#if SC_HAS_NOSTR
static void destroy_nostr_wrap(sc_channel_t *ch, sc_allocator_t *a) {
    (void)a;
    sc_nostr_destroy(ch);
    (void)ch;
}
#endif
#if SC_HAS_LARK
static void destroy_lark_wrap(sc_channel_t *ch, sc_allocator_t *a) {
    (void)a;
    sc_lark_destroy(ch);
    (void)ch;
}
#endif
#if SC_HAS_DINGTALK
static void destroy_dingtalk_wrap(sc_channel_t *ch, sc_allocator_t *a) {
    (void)a;
    sc_dingtalk_destroy(ch);
    (void)ch;
}
#endif
#if SC_HAS_TEAMS
static void destroy_teams_wrap(sc_channel_t *ch, sc_allocator_t *a) {
    (void)a;
    sc_teams_destroy(ch);
    (void)ch;
}
#endif
#if SC_HAS_TWILIO
static void destroy_twilio_wrap(sc_channel_t *ch, sc_allocator_t *a) {
    (void)a;
    sc_twilio_destroy(ch);
    (void)ch;
}
#endif
#if SC_HAS_ONEBOT
static void destroy_onebot_wrap(sc_channel_t *ch, sc_allocator_t *a) {
    (void)a;
    sc_onebot_destroy(ch);
    (void)ch;
}
#endif
#if SC_HAS_QQ
static void destroy_qq_wrap(sc_channel_t *ch, sc_allocator_t *a) {
    (void)a;
    sc_qq_destroy(ch);
    (void)ch;
}
#endif

/* Internal bootstrap state (opaque to caller) */
typedef struct sc_bootstrap_internal {
    sc_config_t cfg;
    sc_plugin_registry_t *plugin_reg;
    sc_security_policy_t policy;
    sc_sandbox_storage_t *sb_storage;
    sc_sandbox_alloc_t sb_alloc;
    sc_sandbox_t sandbox;
    sc_net_proxy_t net_proxy;
    sc_runtime_t runtime;

    sc_tool_t *tools;
    size_t tools_count;

    sc_provider_t provider;
    sc_memory_t memory;
    sc_agent_t agent;
    sc_embedder_t embedder;
    sc_vector_store_t vector_store;
    sc_retrieval_engine_t retrieval_engine;
    sc_session_store_t session_store;
    sc_agent_pool_t *agent_pool;
    sc_mailbox_t *mailbox;
#ifdef SC_HAS_CRON
    sc_cron_scheduler_t *cron;
#else
    void *cron;
#endif

    sc_channel_t channel_slots[SC_BOOTSTRAP_CHANNELS_MAX];
    sc_service_channel_t channels[SC_BOOTSTRAP_CHANNELS_MAX];
    sc_bootstrap_channel_destroy_fn channel_destroys[SC_BOOTSTRAP_CHANNELS_MAX];
    size_t channel_count;

    sc_observer_t observer;
    FILE *log_fp;

    bool provider_ok;
    bool agent_ok;
} sc_bootstrap_internal_t;

static sc_error_t plugin_register_tool_fn(void *ctx, const char *name, void *tool_vtable) {
    sc_plugin_registry_t *reg = (sc_plugin_registry_t *)ctx;
    if (!reg || !name)
        return SC_ERR_INVALID_ARGUMENT;
    sc_plugin_info_t info = {.name = name, .api_version = SC_PLUGIN_API_VERSION};
    sc_tool_t tool;
    memset(&tool, 0, sizeof(tool));
    tool.ctx = tool_vtable;
    tool.vtable = (const sc_tool_vtable_t *)tool_vtable;
    return sc_plugin_register(reg, &info, &tool, 1);
}
static sc_error_t plugin_register_provider_stub_fn(void *ctx, const char *name,
                                                   void *provider_vtable) {
    (void)ctx;
    (void)name;
    (void)provider_vtable;
    return SC_OK;
}
static sc_error_t plugin_register_channel_stub_fn(void *ctx, const sc_channel_t *channel) {
    (void)ctx;
    (void)channel;
    return SC_OK;
}

sc_error_t sc_app_bootstrap(sc_app_ctx_t *ctx, sc_allocator_t *alloc, const char *config_path,
                            bool with_agent, bool with_channels) {
    (void)config_path;
    if (!ctx || !alloc) {
        return SC_ERR_INVALID_ARGUMENT;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->alloc = alloc;

    sc_bootstrap_internal_t *bi =
        (sc_bootstrap_internal_t *)alloc->alloc(alloc->ctx, sizeof(sc_bootstrap_internal_t));
    if (!bi)
        return SC_ERR_OUT_OF_MEMORY;
    memset(bi, 0, sizeof(*bi));
    ctx->channel_instances = bi;

    sc_error_t err = sc_config_load(alloc, &bi->cfg);
    if (err != SC_OK)
        goto fail;
    ctx->cfg = &bi->cfg;

    const char *ws = bi->cfg.workspace_dir ? bi->cfg.workspace_dir : ".";

    bi->plugin_reg = sc_plugin_registry_create(alloc, 16);
    ctx->plugin_reg = bi->plugin_reg;
    if (bi->plugin_reg) {
        sc_plugin_host_t host = {
            .alloc = alloc,
            .register_tool = plugin_register_tool_fn,
            .register_provider = plugin_register_provider_stub_fn,
            .register_channel = plugin_register_channel_stub_fn,
            .host_ctx = bi->plugin_reg,
        };
        for (size_t i = 0; i < bi->cfg.plugins.plugin_paths_len; i++) {
            if (bi->cfg.plugins.plugin_paths[i]) {
                sc_plugin_info_t info = {0};
                sc_plugin_handle_t *handle = NULL;
                err = sc_plugin_load(alloc, bi->cfg.plugins.plugin_paths[i], &host, &info, &handle);
                if (err != SC_OK) {
                    /* Non-fatal: log and continue */
                }
            }
        }
    }

    bi->sb_alloc.ctx = alloc->ctx;
    bi->sb_alloc.alloc = alloc->alloc;
    bi->sb_alloc.free = alloc->free;
    ctx->sb_alloc = bi->sb_alloc;

    if (with_agent) {
        err = sc_runtime_from_config(&bi->cfg, &bi->runtime);
        if (err != SC_OK)
            goto fail;
    }

    bi->policy.autonomy = (sc_autonomy_level_t)bi->cfg.security.autonomy_level;
    bi->policy.workspace_dir = ws;
    bi->policy.workspace_only = true;
    bi->policy.allow_shell = (bi->policy.autonomy != SC_AUTONOMY_READ_ONLY);
    if (with_agent && bi->runtime.vtable && bi->runtime.vtable->has_shell_access)
        bi->policy.allow_shell =
            bi->policy.allow_shell && bi->runtime.vtable->has_shell_access(bi->runtime.ctx);
    bi->policy.block_high_risk_commands = (bi->policy.autonomy == SC_AUTONOMY_SUPERVISED);
    bi->policy.require_approval_for_medium_risk = false;
    bi->policy.max_actions_per_hour = 200;
    bi->policy.tracker = sc_rate_tracker_create(alloc, bi->policy.max_actions_per_hour);
    ctx->policy = &bi->policy;

    if (bi->cfg.security.sandbox_config.enabled ||
        bi->cfg.security.sandbox_config.backend != SC_SANDBOX_NONE) {
        bi->sb_storage = sc_sandbox_storage_create(&bi->sb_alloc);
        if (bi->sb_storage) {
            bi->sandbox = sc_sandbox_create(bi->cfg.security.sandbox_config.backend, ws,
                                            bi->sb_storage, &bi->sb_alloc);
            if (bi->sandbox.vtable) {
                bi->policy.sandbox = &bi->sandbox;
#if defined(__linux__)
                if (strcmp(sc_sandbox_name(&bi->sandbox), "firejail") == 0 &&
                    bi->cfg.security.sandbox_config.firejail_args_len > 0) {
                    sc_firejail_sandbox_set_extra_args(
                        (sc_firejail_ctx_t *)bi->sandbox.ctx,
                        (const char *const *)bi->cfg.security.sandbox_config.firejail_args,
                        bi->cfg.security.sandbox_config.firejail_args_len);
                }
#endif
            }
        }
        ctx->sb_storage = bi->sb_storage;
    }

    if (bi->cfg.security.sandbox_config.net_proxy.enabled) {
        bi->net_proxy.enabled = true;
        bi->net_proxy.deny_all = bi->cfg.security.sandbox_config.net_proxy.deny_all;
        bi->net_proxy.proxy_addr = bi->cfg.security.sandbox_config.net_proxy.proxy_addr;
        bi->net_proxy.allowed_domains_count = 0;
        for (size_t i = 0; i < bi->cfg.security.sandbox_config.net_proxy.allowed_domains_len &&
                           i < SC_NET_PROXY_MAX_DOMAINS;
             i++) {
            bi->net_proxy.allowed_domains[bi->net_proxy.allowed_domains_count++] =
                bi->cfg.security.sandbox_config.net_proxy.allowed_domains[i];
        }
        bi->policy.net_proxy = &bi->net_proxy;
    }

    if (with_agent)
        bi->memory = sc_memory_create_from_config(alloc, &bi->cfg, ws);
    if (with_agent && bi->memory.vtable && bi->cfg.memory.backend &&
        strcmp(bi->cfg.memory.backend, "sqlite") == 0)
        bi->session_store = sc_sqlite_memory_get_session_store(&bi->memory);
    if (with_agent) {
        ctx->memory = &bi->memory;
        ctx->session_store = &bi->session_store;
    }

#ifdef SC_HAS_CRON
    bi->cron = sc_cron_create(alloc, 64, true);
#else
    bi->cron = NULL;
#endif
    ctx->cron = bi->cron;

    bi->agent_pool = sc_agent_pool_create(alloc, bi->cfg.agent.pool_max_concurrent);
    bi->mailbox = sc_mailbox_create(alloc, 64);
    ctx->agent_pool = bi->agent_pool;
    ctx->mailbox = bi->mailbox;

    err = sc_tools_create_default(alloc, ws, strlen(ws), &bi->policy, &bi->cfg,
                                  (with_agent && bi->memory.vtable) ? &bi->memory : NULL, bi->cron,
                                  bi->agent_pool, bi->mailbox, &bi->tools, &bi->tools_count);
    if (err != SC_OK)
        goto fail;
    ctx->tools = bi->tools;
    ctx->tools_count = bi->tools_count;

    if (bi->plugin_reg) {
        sc_tool_t *plugin_tools = NULL;
        size_t plugin_tools_count = 0;
        if (sc_plugin_get_tools(bi->plugin_reg, &plugin_tools, &plugin_tools_count) == SC_OK &&
            plugin_tools_count > 0) {
            sc_tool_t *merged = (sc_tool_t *)alloc->alloc(
                alloc->ctx, (bi->tools_count + plugin_tools_count) * sizeof(sc_tool_t));
            if (merged) {
                memcpy(merged, bi->tools, bi->tools_count * sizeof(sc_tool_t));
                memcpy(merged + bi->tools_count, plugin_tools,
                       plugin_tools_count * sizeof(sc_tool_t));
                alloc->free(alloc->ctx, bi->tools, bi->tools_count * sizeof(sc_tool_t));
                bi->tools = merged;
                bi->tools_count += plugin_tools_count;
            }
            alloc->free(alloc->ctx, plugin_tools, plugin_tools_count * sizeof(sc_tool_t));
        }
        ctx->tools = bi->tools;
        ctx->tools_count = bi->tools_count;
    }

    if (with_agent) {
        const char *prov_name = bi->cfg.default_provider ? bi->cfg.default_provider : "openai";
        size_t prov_name_len = strlen(prov_name);

        err = sc_provider_create_from_config(alloc, &bi->cfg, prov_name, prov_name_len,
                                             &bi->provider);
        if (err == SC_ERR_NOT_SUPPORTED) {
            const char *api_key = sc_config_default_provider_key(&bi->cfg);
            size_t api_key_len = api_key ? strlen(api_key) : 0;
            const char *base_url = sc_config_get_provider_base_url(&bi->cfg, prov_name);
            size_t base_url_len = base_url ? strlen(base_url) : 0;
            err = sc_provider_create(alloc, prov_name, prov_name_len, api_key, api_key_len,
                                     base_url, base_url_len, &bi->provider);
        }
        if (err != SC_OK)
            goto fail;
        ctx->provider = &bi->provider;
        ctx->provider_ok = true;

        bi->embedder = sc_embedder_local_create(alloc);
        bi->vector_store = sc_vector_store_mem_create(alloc);
        bi->retrieval_engine =
            sc_retrieval_create_with_vector(alloc, &bi->memory, &bi->embedder, &bi->vector_store);
        ctx->embedder = &bi->embedder;
        ctx->vector_store = &bi->vector_store;
        ctx->retrieval = &bi->retrieval_engine;

        const char *log_env = getenv("SEACLAW_LOG");
        if (log_env && log_env[0]) {
            bi->log_fp = fopen(log_env, "a");
            if (bi->log_fp)
                bi->observer = sc_log_observer_create(alloc, bi->log_fp);
        }

        const char *model = bi->cfg.default_model ? bi->cfg.default_model : "";
        double temp = bi->cfg.temperature > 0.0 ? bi->cfg.temperature : 0.7;
        uint32_t max_iters =
            bi->cfg.agent.max_tool_iterations > 0 ? bi->cfg.agent.max_tool_iterations : 25;
        uint32_t max_hist =
            bi->cfg.agent.max_history_messages > 0 ? bi->cfg.agent.max_history_messages : 100;
        sc_agent_context_config_t ctx_cfg = {
            .token_limit = bi->cfg.agent.token_limit,
            .pressure_warn = bi->cfg.agent.context_pressure_warn,
            .pressure_compact = bi->cfg.agent.context_pressure_compact,
            .compact_target = bi->cfg.agent.context_compact_target,
        };
        sc_observer_t *obs = bi->observer.vtable ? &bi->observer : NULL;
        err = sc_agent_from_config(
            &bi->agent, alloc, bi->provider, bi->tools, bi->tools_count,
            bi->memory.vtable ? &bi->memory : NULL,
            bi->session_store.vtable ? &bi->session_store : NULL, obs, &bi->policy, model,
            strlen(model), prov_name, prov_name_len, temp, ws, strlen(ws), max_iters, max_hist,
            bi->cfg.memory.auto_save, 2, NULL, 0, bi->cfg.agent.persona,
            bi->cfg.agent.persona ? strlen(bi->cfg.agent.persona) : 0, &ctx_cfg);
        if (err != SC_OK)
            goto fail;
        bi->agent.chain_of_thought = true;
        bi->agent.agent_pool = bi->agent_pool;
        bi->agent.scheduler = (struct sc_cron_scheduler *)bi->cron;
        sc_agent_set_mailbox(&bi->agent, bi->mailbox);
        bi->agent.policy_engine = NULL;
        if (bi->cfg.policy.enabled)
            bi->agent.policy_engine = sc_policy_engine_create(alloc);
        sc_agent_set_retrieval_engine(&bi->agent, &bi->retrieval_engine);
        if (bi->cfg.security.audit.enabled) {
            sc_audit_config_t acfg = SC_AUDIT_CONFIG_DEFAULT;
            acfg.enabled = true;
            acfg.log_path =
                bi->cfg.security.audit.log_path ? bi->cfg.security.audit.log_path : "audit.log";
            acfg.max_size_mb =
                bi->cfg.security.audit.max_size_mb > 0 ? bi->cfg.security.audit.max_size_mb : 10;
            bi->agent.audit_logger = sc_audit_logger_create(alloc, &acfg, ws);
        }
        ctx->agent = &bi->agent;
        ctx->agent_ok = true;
    }

    if (with_channels) {
        size_t ch_count = 0;
        const sc_config_t *cfg = &bi->cfg;

#if SC_HAS_EMAIL
        if (cfg->channels.email.smtp_host && cfg->channels.email.from_address) {
            err = sc_email_create(
                alloc, cfg->channels.email.smtp_host, strlen(cfg->channels.email.smtp_host),
                cfg->channels.email.smtp_port ? cfg->channels.email.smtp_port : 587,
                cfg->channels.email.from_address, strlen(cfg->channels.email.from_address),
                &bi->channel_slots[ch_count]);
            if (err == SC_OK) {
                if (cfg->channels.email.smtp_user && cfg->channels.email.smtp_pass) {
                    sc_email_set_auth(&bi->channel_slots[ch_count], cfg->channels.email.smtp_user,
                                      strlen(cfg->channels.email.smtp_user),
                                      cfg->channels.email.smtp_pass,
                                      strlen(cfg->channels.email.smtp_pass));
                }
                if (cfg->channels.email.imap_host) {
                    sc_email_set_imap(&bi->channel_slots[ch_count], cfg->channels.email.imap_host,
                                      strlen(cfg->channels.email.imap_host),
                                      cfg->channels.email.imap_port ? cfg->channels.email.imap_port
                                                                    : 993);
                }
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = sc_email_poll;
                bi->channels[ch_count].interval_ms = 30000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_email_wrap;
                ch_count++;
            }
        }
#endif

#if SC_HAS_IMESSAGE
        if (cfg->channels.imessage.default_target && ch_count < SC_BOOTSTRAP_CHANNELS_MAX) {
            err = sc_imessage_create(alloc, cfg->channels.imessage.default_target,
                                     strlen(cfg->channels.imessage.default_target),
                                     (const char *const *)cfg->channels.imessage.allow_from,
                                     cfg->channels.imessage.allow_from_count,
                                     &bi->channel_slots[ch_count]);
            if (err == SC_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = sc_imessage_poll;
                bi->channels[ch_count].interval_ms = 3000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_imessage_wrap;
                ch_count++;
            }
        }
#endif

#if SC_HAS_GMAIL
        if (cfg->channels.gmail.client_id && cfg->channels.gmail.client_secret &&
            cfg->channels.gmail.refresh_token && ch_count < SC_BOOTSTRAP_CHANNELS_MAX) {
            err = sc_gmail_create(
                alloc, cfg->channels.gmail.client_id, strlen(cfg->channels.gmail.client_id),
                cfg->channels.gmail.client_secret, strlen(cfg->channels.gmail.client_secret),
                cfg->channels.gmail.refresh_token, strlen(cfg->channels.gmail.refresh_token),
                cfg->channels.gmail.poll_interval_sec, &bi->channel_slots[ch_count]);
            if (err == SC_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = sc_gmail_poll;
                bi->channels[ch_count].interval_ms =
                    (uint32_t)(cfg->channels.gmail.poll_interval_sec > 0
                                   ? cfg->channels.gmail.poll_interval_sec * 1000
                                   : 30000);
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_gmail_wrap;
                ch_count++;
            }
        }
#endif

#if SC_HAS_IMAP
        if (cfg->channels.imap.imap_host && cfg->channels.imap.imap_username &&
            cfg->channels.imap.imap_password && ch_count < SC_BOOTSTRAP_CHANNELS_MAX) {
            sc_imap_config_t imap_cfg = {
                .imap_host = cfg->channels.imap.imap_host,
                .imap_host_len = strlen(cfg->channels.imap.imap_host),
                .imap_port = cfg->channels.imap.imap_port ? cfg->channels.imap.imap_port : 993,
                .imap_username = cfg->channels.imap.imap_username,
                .imap_username_len = strlen(cfg->channels.imap.imap_username),
                .imap_password = cfg->channels.imap.imap_password,
                .imap_password_len = strlen(cfg->channels.imap.imap_password),
                .imap_folder =
                    cfg->channels.imap.imap_folder ? cfg->channels.imap.imap_folder : "INBOX",
                .imap_folder_len =
                    cfg->channels.imap.imap_folder ? strlen(cfg->channels.imap.imap_folder) : 6,
                .imap_use_tls = cfg->channels.imap.imap_use_tls,
            };
            err = sc_imap_create(alloc, &imap_cfg, &bi->channel_slots[ch_count]);
            if (err == SC_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = sc_imap_poll;
                bi->channels[ch_count].interval_ms = 30000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_imap_wrap;
                ch_count++;
            }
        }
#endif

#if SC_HAS_TELEGRAM
        if (cfg->channels.telegram.token && ch_count < SC_BOOTSTRAP_CHANNELS_MAX) {
            const char *const *af = (const char *const *)cfg->channels.telegram.allow_from;
            err = sc_telegram_create_ex(
                alloc, cfg->channels.telegram.token, strlen(cfg->channels.telegram.token), af,
                cfg->channels.telegram.allow_from_count, &bi->channel_slots[ch_count]);
            if (err == SC_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = sc_telegram_poll;
                bi->channels[ch_count].interval_ms = 1000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_telegram_wrap;
                ch_count++;
            }
        }
#endif

#if SC_HAS_DISCORD
        if (cfg->channels.discord.token && cfg->channels.discord.channel_ids_count > 0 &&
            ch_count < SC_BOOTSTRAP_CHANNELS_MAX) {
            const char **ch_ids = (const char **)cfg->channels.discord.channel_ids;
            err = sc_discord_create_ex(
                alloc, cfg->channels.discord.token, strlen(cfg->channels.discord.token), ch_ids,
                cfg->channels.discord.channel_ids_count, cfg->channels.discord.bot_id,
                cfg->channels.discord.bot_id ? strlen(cfg->channels.discord.bot_id) : 0,
                &bi->channel_slots[ch_count]);
            if (err == SC_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = sc_discord_poll;
                bi->channels[ch_count].interval_ms = 2000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_discord_wrap;
                ch_count++;
            }
        }
#endif

#if SC_HAS_SLACK
        if (cfg->channels.slack.token && cfg->channels.slack.channel_ids_count > 0 &&
            ch_count < SC_BOOTSTRAP_CHANNELS_MAX) {
            const char **sl_ids = (const char **)cfg->channels.slack.channel_ids;
            err = sc_slack_create_ex(
                alloc, cfg->channels.slack.token, strlen(cfg->channels.slack.token), sl_ids,
                cfg->channels.slack.channel_ids_count, &bi->channel_slots[ch_count]);
            if (err == SC_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = sc_slack_poll;
                bi->channels[ch_count].interval_ms = 3000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_slack_wrap;
                ch_count++;
            }
        }
#endif

#if SC_HAS_WHATSAPP
        if (cfg->channels.whatsapp.phone_number_id && cfg->channels.whatsapp.token &&
            ch_count < SC_BOOTSTRAP_CHANNELS_MAX) {
            err = sc_whatsapp_create(
                alloc, cfg->channels.whatsapp.phone_number_id,
                strlen(cfg->channels.whatsapp.phone_number_id), cfg->channels.whatsapp.token,
                strlen(cfg->channels.whatsapp.token), &bi->channel_slots[ch_count]);
            if (err == SC_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = sc_whatsapp_poll;
                bi->channels[ch_count].webhook_fn = sc_whatsapp_on_webhook;
                bi->channels[ch_count].interval_ms = 1000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_whatsapp_wrap;
                ch_count++;
            }
        }
#endif

#if SC_HAS_FACEBOOK
        if (cfg->channels.facebook.page_id && cfg->channels.facebook.page_access_token &&
            ch_count < SC_BOOTSTRAP_CHANNELS_MAX) {
            err = sc_facebook_create(
                alloc, cfg->channels.facebook.page_id, strlen(cfg->channels.facebook.page_id),
                cfg->channels.facebook.page_access_token,
                strlen(cfg->channels.facebook.page_access_token),
                cfg->channels.facebook.app_secret ? cfg->channels.facebook.app_secret : "",
                cfg->channels.facebook.app_secret ? strlen(cfg->channels.facebook.app_secret) : 0,
                &bi->channel_slots[ch_count]);
            if (err == SC_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = sc_facebook_poll;
                bi->channels[ch_count].webhook_fn = sc_facebook_on_webhook;
                bi->channels[ch_count].interval_ms = 1000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_facebook_wrap;
                ch_count++;
            }
        }
#endif

#if SC_HAS_LINE
        if (cfg->channels.line.channel_token && ch_count < SC_BOOTSTRAP_CHANNELS_MAX) {
            err = sc_line_create_ex(
                alloc, cfg->channels.line.channel_token, strlen(cfg->channels.line.channel_token),
                cfg->channels.line.user_id,
                cfg->channels.line.user_id ? strlen(cfg->channels.line.user_id) : 0,
                &bi->channel_slots[ch_count]);
            if (err == SC_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = sc_line_poll;
                bi->channels[ch_count].webhook_fn = sc_line_on_webhook;
                bi->channels[ch_count].interval_ms = 1000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_line_wrap;
                ch_count++;
            }
        }
#endif

#if SC_HAS_GOOGLE_CHAT
        if (cfg->channels.google_chat.webhook_url && ch_count < SC_BOOTSTRAP_CHANNELS_MAX) {
            err = sc_google_chat_create(alloc, cfg->channels.google_chat.webhook_url,
                                        strlen(cfg->channels.google_chat.webhook_url),
                                        &bi->channel_slots[ch_count]);
            if (err == SC_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = sc_google_chat_poll;
                bi->channels[ch_count].webhook_fn = sc_google_chat_on_webhook;
                bi->channels[ch_count].interval_ms = 1000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_google_chat_wrap;
                ch_count++;
            }
        }
#endif

#if SC_HAS_INSTAGRAM
        if (cfg->channels.instagram.business_account_id && cfg->channels.instagram.access_token &&
            ch_count < SC_BOOTSTRAP_CHANNELS_MAX) {
            err = sc_instagram_create(
                alloc, cfg->channels.instagram.business_account_id,
                strlen(cfg->channels.instagram.business_account_id),
                cfg->channels.instagram.access_token, strlen(cfg->channels.instagram.access_token),
                cfg->channels.instagram.app_secret,
                cfg->channels.instagram.app_secret ? strlen(cfg->channels.instagram.app_secret) : 0,
                &bi->channel_slots[ch_count]);
            if (err == SC_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = sc_instagram_poll;
                bi->channels[ch_count].webhook_fn = sc_instagram_on_webhook;
                bi->channels[ch_count].interval_ms = 1000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_instagram_wrap;
                ch_count++;
            }
        }
#endif

#if SC_HAS_TWITTER
        if (cfg->channels.twitter.bearer_token && ch_count < SC_BOOTSTRAP_CHANNELS_MAX) {
            err = sc_twitter_create(alloc, cfg->channels.twitter.bearer_token,
                                    strlen(cfg->channels.twitter.bearer_token),
                                    &bi->channel_slots[ch_count]);
            if (err == SC_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = sc_twitter_poll;
                bi->channels[ch_count].webhook_fn = sc_twitter_on_webhook;
                bi->channels[ch_count].interval_ms = 1000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_twitter_wrap;
                ch_count++;
            }
        }
#endif

#if SC_HAS_GOOGLE_RCS
        if (cfg->channels.google_rcs.agent_id && cfg->channels.google_rcs.token &&
            ch_count < SC_BOOTSTRAP_CHANNELS_MAX) {
            err = sc_google_rcs_create(
                alloc, cfg->channels.google_rcs.agent_id, strlen(cfg->channels.google_rcs.agent_id),
                cfg->channels.google_rcs.token, strlen(cfg->channels.google_rcs.token),
                &bi->channel_slots[ch_count]);
            if (err == SC_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = sc_google_rcs_poll;
                bi->channels[ch_count].webhook_fn = sc_google_rcs_on_webhook;
                bi->channels[ch_count].interval_ms = 1000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_google_rcs_wrap;
                ch_count++;
            }
        }
#endif

#if SC_HAS_MQTT
        if (cfg->channels.mqtt.broker_url && ch_count < SC_BOOTSTRAP_CHANNELS_MAX) {
            const char *in_t = cfg->channels.mqtt.inbound_topic;
            const char *out_t = cfg->channels.mqtt.outbound_topic;
            const char *usr = cfg->channels.mqtt.username;
            const char *pwd = cfg->channels.mqtt.password;
            err = sc_mqtt_create(alloc, cfg->channels.mqtt.broker_url,
                                 strlen(cfg->channels.mqtt.broker_url), in_t,
                                 in_t ? strlen(in_t) : 0, out_t, out_t ? strlen(out_t) : 0, usr,
                                 usr ? strlen(usr) : 0, pwd, pwd ? strlen(pwd) : 0,
                                 cfg->channels.mqtt.qos, &bi->channel_slots[ch_count]);
            if (err == SC_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = sc_mqtt_poll;
                bi->channels[ch_count].webhook_fn = NULL;
                bi->channels[ch_count].interval_ms = 500;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_mqtt_wrap;
                ch_count++;
            }
        }
#endif

#if SC_HAS_MATRIX
        if (cfg->channels.matrix.homeserver && cfg->channels.matrix.access_token &&
            ch_count < SC_BOOTSTRAP_CHANNELS_MAX) {
            err = sc_matrix_create(
                alloc, cfg->channels.matrix.homeserver, strlen(cfg->channels.matrix.homeserver),
                cfg->channels.matrix.access_token, strlen(cfg->channels.matrix.access_token),
                &bi->channel_slots[ch_count]);
            if (err == SC_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = sc_matrix_poll;
                bi->channels[ch_count].interval_ms = 3000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_matrix_wrap;
                ch_count++;
            }
        }
#endif

#if SC_HAS_IRC
        if (cfg->channels.irc.server && ch_count < SC_BOOTSTRAP_CHANNELS_MAX) {
            err = sc_irc_create(alloc, cfg->channels.irc.server, strlen(cfg->channels.irc.server),
                                cfg->channels.irc.port ? cfg->channels.irc.port : 6667,
                                &bi->channel_slots[ch_count]);
            if (err == SC_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = sc_irc_poll;
                bi->channels[ch_count].interval_ms = 1000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_irc_wrap;
                ch_count++;
            }
        }
#endif

#if SC_HAS_NOSTR
        if (cfg->channels.nostr.relay_url && ch_count < SC_BOOTSTRAP_CHANNELS_MAX) {
            err = sc_nostr_create(
                alloc, cfg->channels.nostr.nak_path ? cfg->channels.nostr.nak_path : "",
                cfg->channels.nostr.nak_path ? strlen(cfg->channels.nostr.nak_path) : 0,
                cfg->channels.nostr.bot_pubkey ? cfg->channels.nostr.bot_pubkey : "",
                cfg->channels.nostr.bot_pubkey ? strlen(cfg->channels.nostr.bot_pubkey) : 0,
                cfg->channels.nostr.relay_url, strlen(cfg->channels.nostr.relay_url),
                cfg->channels.nostr.seckey_hex ? cfg->channels.nostr.seckey_hex : "",
                cfg->channels.nostr.seckey_hex ? strlen(cfg->channels.nostr.seckey_hex) : 0,
                &bi->channel_slots[ch_count]);
            if (err == SC_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = sc_nostr_poll;
                bi->channels[ch_count].interval_ms = 2000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_nostr_wrap;
                ch_count++;
            }
        }
#endif

#if SC_HAS_LARK
        if (((cfg->channels.lark.app_id && cfg->channels.lark.app_secret) ||
             cfg->channels.lark.webhook_url) &&
            ch_count < SC_BOOTSTRAP_CHANNELS_MAX) {
            const char *id_or_wh = cfg->channels.lark.webhook_url ? cfg->channels.lark.webhook_url
                                                                  : cfg->channels.lark.app_id;
            size_t id_len = cfg->channels.lark.webhook_url ? strlen(cfg->channels.lark.webhook_url)
                                                           : strlen(cfg->channels.lark.app_id);
            const char *secret =
                cfg->channels.lark.webhook_url ? "" : cfg->channels.lark.app_secret;
            size_t secret_len =
                cfg->channels.lark.webhook_url ? 0 : strlen(cfg->channels.lark.app_secret);
            err = sc_lark_create(alloc, id_or_wh, id_len, secret, secret_len,
                                 &bi->channel_slots[ch_count]);
            if (err == SC_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = sc_lark_poll;
                bi->channels[ch_count].webhook_fn = sc_lark_on_webhook;
                bi->channels[ch_count].interval_ms = 1000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_lark_wrap;
                ch_count++;
            }
        }
#endif

#if SC_HAS_DINGTALK
        if (((cfg->channels.dingtalk.app_key && cfg->channels.dingtalk.app_secret) ||
             cfg->channels.dingtalk.webhook_url) &&
            ch_count < SC_BOOTSTRAP_CHANNELS_MAX) {
            const char *key_or_wh = cfg->channels.dingtalk.webhook_url
                                        ? cfg->channels.dingtalk.webhook_url
                                        : cfg->channels.dingtalk.app_key;
            size_t key_len = cfg->channels.dingtalk.webhook_url
                                 ? strlen(cfg->channels.dingtalk.webhook_url)
                                 : strlen(cfg->channels.dingtalk.app_key);
            const char *secret =
                cfg->channels.dingtalk.webhook_url ? "" : cfg->channels.dingtalk.app_secret;
            size_t secret_len =
                cfg->channels.dingtalk.webhook_url ? 0 : strlen(cfg->channels.dingtalk.app_secret);
            err = sc_dingtalk_create(alloc, key_or_wh, key_len, secret, secret_len,
                                     &bi->channel_slots[ch_count]);
            if (err == SC_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = sc_dingtalk_poll;
                bi->channels[ch_count].webhook_fn = sc_dingtalk_on_webhook;
                bi->channels[ch_count].interval_ms = 1000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_dingtalk_wrap;
                ch_count++;
            }
        }
#endif

#if SC_HAS_TEAMS
        if (cfg->channels.teams.webhook_url && ch_count < SC_BOOTSTRAP_CHANNELS_MAX) {
            err = sc_teams_create(alloc, cfg->channels.teams.webhook_url,
                                  strlen(cfg->channels.teams.webhook_url),
                                  &bi->channel_slots[ch_count]);
            if (err == SC_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = sc_teams_poll;
                bi->channels[ch_count].webhook_fn = sc_teams_on_webhook;
                bi->channels[ch_count].interval_ms = 1000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_teams_wrap;
                ch_count++;
            }
        }
#endif

#if SC_HAS_TWILIO
        if (cfg->channels.twilio.account_sid && cfg->channels.twilio.auth_token &&
            cfg->channels.twilio.from_number && cfg->channels.twilio.to_number &&
            ch_count < SC_BOOTSTRAP_CHANNELS_MAX) {
            err = sc_twilio_create(
                alloc, cfg->channels.twilio.account_sid, strlen(cfg->channels.twilio.account_sid),
                cfg->channels.twilio.auth_token, strlen(cfg->channels.twilio.auth_token),
                cfg->channels.twilio.from_number, strlen(cfg->channels.twilio.from_number),
                cfg->channels.twilio.to_number, strlen(cfg->channels.twilio.to_number),
                &bi->channel_slots[ch_count]);
            if (err == SC_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = sc_twilio_poll;
                bi->channels[ch_count].webhook_fn = sc_twilio_on_webhook;
                bi->channels[ch_count].interval_ms = 1000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_twilio_wrap;
                ch_count++;
            }
        }
#endif

#if SC_HAS_ONEBOT
        if (cfg->channels.onebot.api_base && cfg->channels.onebot.user_id &&
            ch_count < SC_BOOTSTRAP_CHANNELS_MAX) {
            const char *tok = cfg->channels.onebot.access_token;
            const char *uid = cfg->channels.onebot.user_id;
            err = sc_onebot_create_ex(alloc, cfg->channels.onebot.api_base,
                                      strlen(cfg->channels.onebot.api_base), tok ? tok : "",
                                      tok ? strlen(tok) : 0, uid, strlen(uid),
                                      &bi->channel_slots[ch_count]);
            if (err == SC_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = sc_onebot_poll;
                bi->channels[ch_count].webhook_fn = sc_onebot_on_webhook;
                bi->channels[ch_count].interval_ms = 1000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_onebot_wrap;
                ch_count++;
            }
        }
#endif

#if SC_HAS_QQ
        if (cfg->channels.qq.app_id && cfg->channels.qq.bot_token &&
            ch_count < SC_BOOTSTRAP_CHANNELS_MAX) {
            const char *ch_id = cfg->channels.qq.channel_id;
            err = sc_qq_create_ex(alloc, cfg->channels.qq.app_id, strlen(cfg->channels.qq.app_id),
                                  cfg->channels.qq.bot_token, strlen(cfg->channels.qq.bot_token),
                                  ch_id ? ch_id : "", ch_id ? strlen(ch_id) : 0,
                                  cfg->channels.qq.sandbox, &bi->channel_slots[ch_count]);
            if (err == SC_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = sc_qq_poll;
                bi->channels[ch_count].webhook_fn = sc_qq_on_webhook;
                bi->channels[ch_count].interval_ms = 1000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_qq_wrap;
                ch_count++;
            }
        }
#endif

        bi->channel_count = ch_count;
        ctx->channels = bi->channels;
        ctx->channel_count = ch_count;
    }

    return SC_OK;

fail:
    sc_app_teardown(ctx);
    return err;
}

void sc_app_teardown(sc_app_ctx_t *ctx) {
    if (!ctx || !ctx->alloc)
        return;
    sc_allocator_t *alloc = ctx->alloc;
    sc_bootstrap_internal_t *bi = (sc_bootstrap_internal_t *)ctx->channel_instances;
    if (!bi)
        return;

    /* Channels (reverse order, though order typically doesn't matter) */
    for (size_t i = 0; i < bi->channel_count && i < SC_BOOTSTRAP_CHANNELS_MAX; i++) {
        if (bi->channel_destroys[i] && bi->channel_slots[i].ctx)
            bi->channel_destroys[i](&bi->channel_slots[i], alloc);
    }

    if (ctx->agent_ok && bi->agent.alloc) {
        if (bi->agent.policy_engine)
            sc_policy_engine_destroy(bi->agent.policy_engine);
        sc_agent_deinit(&bi->agent);
    }
    sc_tools_destroy_default(alloc, bi->tools, bi->tools_count);
    if (bi->cron)
        sc_cron_destroy(bi->cron, alloc);
    if (bi->observer.vtable && bi->observer.vtable->deinit)
        bi->observer.vtable->deinit(bi->observer.ctx);
    if (bi->log_fp)
        fclose(bi->log_fp);
    if (bi->retrieval_engine.vtable && bi->retrieval_engine.vtable->deinit)
        bi->retrieval_engine.vtable->deinit(bi->retrieval_engine.ctx, alloc);
    if (bi->vector_store.vtable && bi->vector_store.vtable->deinit)
        bi->vector_store.vtable->deinit(bi->vector_store.ctx, alloc);
    if (bi->embedder.vtable && bi->embedder.vtable->deinit)
        bi->embedder.vtable->deinit(bi->embedder.ctx, alloc);
    if (bi->memory.vtable && bi->memory.vtable->deinit)
        bi->memory.vtable->deinit(bi->memory.ctx);
    if (ctx->provider_ok && bi->provider.vtable && bi->provider.vtable->deinit)
        bi->provider.vtable->deinit(bi->provider.ctx, alloc);
    if (bi->policy.tracker)
        sc_rate_tracker_destroy(bi->policy.tracker);
    if (bi->sb_storage)
        sc_sandbox_storage_destroy(bi->sb_storage, &bi->sb_alloc);
    if (bi->plugin_reg)
        sc_plugin_registry_destroy(bi->plugin_reg);
    if (bi->agent_pool)
        sc_agent_pool_destroy(bi->agent_pool);
    if (bi->mailbox)
        sc_mailbox_destroy(bi->mailbox);
    sc_config_deinit(&bi->cfg);
    alloc->free(alloc->ctx, bi, sizeof(sc_bootstrap_internal_t));
    ctx->channel_instances = NULL;
    memset(ctx, 0, sizeof(*ctx));
}
