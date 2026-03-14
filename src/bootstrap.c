/*
 * Shared bootstrap logic for service-loop and gateway commands.
 * Extracts config loading, provider creation, tools setup, security init,
 * channel creation, and agent creation into a single module.
 */

#include "human/bootstrap.h"
#include "human/agent/mailbox.h"
#include "human/agent/spawn.h"
#include "human/config.h"
#include "human/memory.h"
#include "human/memory/engines.h"
#include "human/memory/factory.h"
#include "human/memory/retrieval.h"
#include "human/memory/vector.h"
#include "human/memory/vector/embedder_gemini_adapter.h"
#include "human/memory/vector/embeddings_gemini.h"
#include "human/observability/log_observer.h"
#include "human/plugin.h"
#include "human/plugin_loader.h"
#include "human/providers/factory.h"
#include "human/runtime.h"
#include "human/security/audit.h"
#include "human/security/sandbox.h"
#include "human/security/sandbox_internal.h"
#include "human/tool.h"
#include "human/tools/factory.h"
#include <stdlib.h>
#include <string.h>

#if HU_HAS_EMAIL
#include "human/channels/email.h"
#endif
#if HU_HAS_IMESSAGE
#include "human/channels/imessage.h"
#endif
#if HU_HAS_PWA
#include "human/channels/pwa.h"
#include "human/pwa.h"
#endif
#if HU_HAS_IMAP
#include "human/channels/imap.h"
#endif
#if HU_HAS_GMAIL
#include "human/channels/gmail.h"
#endif
#if HU_HAS_TELEGRAM
#include "human/channels/telegram.h"
#endif
#if HU_HAS_DISCORD
#include "human/channels/discord.h"
#endif
#if HU_HAS_SLACK
#include "human/channels/slack.h"
#endif
#if HU_HAS_WHATSAPP
#include "human/channels/whatsapp.h"
#endif
#if HU_HAS_FACEBOOK
#include "human/channels/facebook.h"
#endif
#if HU_HAS_LINE
#include "human/channels/line.h"
#endif
#if HU_HAS_GOOGLE_CHAT
#include "human/channels/google_chat.h"
#endif
#if HU_HAS_INSTAGRAM
#include "human/channels/instagram.h"
#endif
#if HU_HAS_TWITTER
#include "human/channels/twitter.h"
#endif
#if HU_HAS_TIKTOK
#include "human/channels/tiktok.h"
#endif
#if HU_HAS_GOOGLE_RCS
#include "human/channels/google_rcs.h"
#endif
#if HU_HAS_MQTT
#include "human/channels/mqtt.h"
#endif
#if HU_HAS_MATRIX
#include "human/channels/matrix.h"
#endif
#if HU_HAS_IRC
#include "human/channels/irc.h"
#endif
#if HU_HAS_NOSTR
#include "human/channels/nostr.h"
#endif
#if HU_HAS_LARK
#include "human/channels/lark.h"
#endif
#if HU_HAS_DINGTALK
#include "human/channels/dingtalk.h"
#endif
#if HU_HAS_TEAMS
#include "human/channels/teams.h"
#endif
#if HU_HAS_TWILIO
#include "human/channels/twilio.h"
#endif
#if HU_HAS_ONEBOT
#include "human/channels/onebot.h"
#endif
#if HU_HAS_QQ
#include "human/channels/qq.h"
#endif

#ifdef HU_HAS_CRON
#include "human/cron.h"
#include "human/crontab.h"
#endif
#include "human/agent/registry.h"
#ifdef HU_HAS_SKILLS
#include "human/skill_registry.h"
#include "human/skillforge.h"
#endif

#define HU_BOOTSTRAP_CHANNELS_MAX 20

/* Channel destroy callback: (ch, alloc). Most channels ignore alloc. */
typedef void (*hu_bootstrap_channel_destroy_fn)(hu_channel_t *ch, hu_allocator_t *alloc);

#if HU_HAS_EMAIL
static void destroy_email_wrap(hu_channel_t *ch, hu_allocator_t *a) {
    (void)a;
    hu_email_destroy(ch);
    (void)ch;
}
#endif
#if HU_HAS_IMESSAGE
static void destroy_imessage_wrap(hu_channel_t *ch, hu_allocator_t *a) {
    (void)a;
    hu_imessage_destroy(ch);
    (void)ch;
}
#endif
#if HU_HAS_PWA
static void destroy_pwa_wrap(hu_channel_t *ch, hu_allocator_t *a) {
    (void)a;
    hu_pwa_channel_destroy(ch);
    (void)ch;
}
#endif
#if HU_HAS_GMAIL
static void destroy_gmail_wrap(hu_channel_t *ch, hu_allocator_t *a) {
    (void)a;
    hu_gmail_destroy(ch);
    (void)ch;
}
#endif
#if HU_HAS_IMAP
static void destroy_imap_wrap(hu_channel_t *ch, hu_allocator_t *a) {
    (void)a;
    hu_imap_destroy(ch);
    (void)ch;
}
#endif
#if HU_HAS_TELEGRAM
static void destroy_telegram_wrap(hu_channel_t *ch, hu_allocator_t *a) {
    (void)a;
    hu_telegram_destroy(ch);
    (void)ch;
}
#endif
#if HU_HAS_DISCORD
static void destroy_discord_wrap(hu_channel_t *ch, hu_allocator_t *a) {
    (void)a;
    hu_discord_destroy(ch);
    (void)ch;
}
#endif
#if HU_HAS_SLACK
static void destroy_slack_wrap(hu_channel_t *ch, hu_allocator_t *a) {
    (void)a;
    hu_slack_destroy(ch);
    (void)ch;
}
#endif
#if HU_HAS_WHATSAPP
static void destroy_whatsapp_wrap(hu_channel_t *ch, hu_allocator_t *a) {
    (void)a;
    hu_whatsapp_destroy(ch);
    (void)ch;
}
#endif
#if HU_HAS_FACEBOOK
static void destroy_facebook_wrap(hu_channel_t *ch, hu_allocator_t *a) {
    (void)a;
    hu_facebook_destroy(ch);
    (void)ch;
}
#endif
#if HU_HAS_LINE
static void destroy_line_wrap(hu_channel_t *ch, hu_allocator_t *a) {
    (void)a;
    hu_line_destroy(ch);
    (void)ch;
}
#endif
#if HU_HAS_GOOGLE_CHAT
static void destroy_google_chat_wrap(hu_channel_t *ch, hu_allocator_t *a) {
    (void)a;
    hu_google_chat_destroy(ch);
    (void)ch;
}
#endif
#if HU_HAS_INSTAGRAM
static void destroy_instagram_wrap(hu_channel_t *ch, hu_allocator_t *a) {
    (void)a;
    hu_instagram_destroy(ch);
    (void)ch;
}
#endif
#if HU_HAS_TWITTER
static void destroy_twitter_wrap(hu_channel_t *ch, hu_allocator_t *a) {
    (void)a;
    hu_twitter_destroy(ch);
    (void)ch;
}
#endif
#if HU_HAS_TIKTOK
static void destroy_tiktok_wrap(hu_channel_t *ch, hu_allocator_t *a) {
    (void)a;
    hu_tiktok_destroy(ch);
    (void)ch;
}
#endif
#if HU_HAS_GOOGLE_RCS
static void destroy_google_rcs_wrap(hu_channel_t *ch, hu_allocator_t *a) {
    (void)a;
    hu_google_rcs_destroy(ch);
    (void)ch;
}
#endif
#if HU_HAS_MQTT
static void destroy_mqtt_wrap(hu_channel_t *ch, hu_allocator_t *a) {
    hu_mqtt_destroy(ch, a);
}
#endif
#if HU_HAS_MATRIX
static void destroy_matrix_wrap(hu_channel_t *ch, hu_allocator_t *a) {
    (void)a;
    hu_matrix_destroy(ch);
    (void)ch;
}
#endif
#if HU_HAS_IRC
static void destroy_irc_wrap(hu_channel_t *ch, hu_allocator_t *a) {
    (void)a;
    hu_irc_destroy(ch);
    (void)ch;
}
#endif
#if HU_HAS_NOSTR
static void destroy_nostr_wrap(hu_channel_t *ch, hu_allocator_t *a) {
    (void)a;
    hu_nostr_destroy(ch);
    (void)ch;
}
#endif
#if HU_HAS_LARK
static void destroy_lark_wrap(hu_channel_t *ch, hu_allocator_t *a) {
    (void)a;
    hu_lark_destroy(ch);
    (void)ch;
}
#endif
#if HU_HAS_DINGTALK
static void destroy_dingtalk_wrap(hu_channel_t *ch, hu_allocator_t *a) {
    (void)a;
    hu_dingtalk_destroy(ch);
    (void)ch;
}
#endif
#if HU_HAS_TEAMS
static void destroy_teams_wrap(hu_channel_t *ch, hu_allocator_t *a) {
    (void)a;
    hu_teams_destroy(ch);
    (void)ch;
}
#endif
#if HU_HAS_TWILIO
static void destroy_twilio_wrap(hu_channel_t *ch, hu_allocator_t *a) {
    (void)a;
    hu_twilio_destroy(ch);
    (void)ch;
}
#endif
#if HU_HAS_ONEBOT
static void destroy_onebot_wrap(hu_channel_t *ch, hu_allocator_t *a) {
    (void)a;
    hu_onebot_destroy(ch);
    (void)ch;
}
#endif
#if HU_HAS_QQ
static void destroy_qq_wrap(hu_channel_t *ch, hu_allocator_t *a) {
    (void)a;
    hu_qq_destroy(ch);
    (void)ch;
}
#endif

/* Internal bootstrap state (opaque to caller) */
typedef struct hu_bootstrap_internal {
    hu_config_t cfg;
    hu_plugin_registry_t *plugin_reg;
    hu_security_policy_t policy;
    hu_sandbox_storage_t *sb_storage;
    hu_sandbox_alloc_t sb_alloc;
    hu_sandbox_t sandbox;
    hu_net_proxy_t net_proxy;
    hu_runtime_t runtime;

    hu_tool_t *tools;
    size_t tools_count;

    hu_provider_t provider;
    hu_memory_t memory;
    hu_agent_t agent;
    hu_embedder_t embedder;
    hu_vector_store_t vector_store;
    hu_retrieval_engine_t retrieval_engine;
    hu_session_store_t session_store;
    hu_agent_pool_t *agent_pool;
    hu_mailbox_t *mailbox;
#ifdef HU_HAS_CRON
    hu_cron_scheduler_t *cron;
#else
    void *cron;
#endif

    hu_channel_t channel_slots[HU_BOOTSTRAP_CHANNELS_MAX];
    hu_service_channel_t channels[HU_BOOTSTRAP_CHANNELS_MAX];
    hu_bootstrap_channel_destroy_fn channel_destroys[HU_BOOTSTRAP_CHANNELS_MAX];
    size_t channel_count;

    hu_observer_t observer;
    FILE *log_fp;

#ifdef HU_HAS_SKILLS
    hu_skillforge_t skillforge;
    bool skillforge_ok;
#endif
    hu_agent_registry_t agent_registry;
    bool agent_registry_ok;

#if HU_HAS_PWA
    hu_pwa_driver_registry_t pwa_driver_registry;
    bool pwa_driver_registry_ok;
#endif

    bool provider_ok;
    bool agent_ok;
} hu_bootstrap_internal_t;

static hu_error_t plugin_register_tool_fn(void *ctx, const char *name, void *tool_vtable) {
    hu_plugin_registry_t *reg = (hu_plugin_registry_t *)ctx;
    if (!reg || !name)
        return HU_ERR_INVALID_ARGUMENT;
    hu_plugin_info_t info = {.name = name, .api_version = HU_PLUGIN_API_VERSION};
    hu_tool_t tool;
    memset(&tool, 0, sizeof(tool));
    tool.ctx = tool_vtable;
    tool.vtable = (const hu_tool_vtable_t *)tool_vtable;
    return hu_plugin_register(reg, &info, &tool, 1);
}
static hu_error_t plugin_register_provider_stub_fn(void *ctx, const char *name,
                                                   void *provider_vtable) {
    (void)ctx;
    (void)name;
    (void)provider_vtable;
    return HU_OK;
}
static hu_error_t plugin_register_channel_stub_fn(void *ctx, const hu_channel_t *channel) {
    (void)ctx;
    (void)channel;
    return HU_OK;
}

hu_error_t hu_app_bootstrap(hu_app_ctx_t *ctx, hu_allocator_t *alloc, const char *config_path,
                            bool with_agent, bool with_channels) {
    (void)config_path;
    if (!ctx || !alloc) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->alloc = alloc;

    hu_bootstrap_internal_t *bi =
        (hu_bootstrap_internal_t *)alloc->alloc(alloc->ctx, sizeof(hu_bootstrap_internal_t));
    if (!bi)
        return HU_ERR_OUT_OF_MEMORY;
    memset(bi, 0, sizeof(*bi));
    ctx->channel_instances = bi;

    hu_error_t err = hu_config_load(alloc, &bi->cfg);
    if (err != HU_OK)
        goto fail;
    ctx->cfg = &bi->cfg;

    const char *ws = bi->cfg.workspace_dir ? bi->cfg.workspace_dir : ".";

    bi->plugin_reg = hu_plugin_registry_create(alloc, 16);
    ctx->plugin_reg = bi->plugin_reg;
    if (bi->plugin_reg) {
        hu_plugin_host_t host = {
            .alloc = alloc,
            .register_tool = plugin_register_tool_fn,
            .register_provider = plugin_register_provider_stub_fn,
            .register_channel = plugin_register_channel_stub_fn,
            .host_ctx = bi->plugin_reg,
        };
        for (size_t i = 0; i < bi->cfg.plugins.plugin_paths_len; i++) {
            if (bi->cfg.plugins.plugin_paths[i]) {
                hu_plugin_info_t info = {0};
                hu_plugin_handle_t *handle = NULL;
                err = hu_plugin_load(alloc, bi->cfg.plugins.plugin_paths[i], &host, &info, &handle);
                if (err != HU_OK) {
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
        err = hu_runtime_from_config(&bi->cfg, &bi->runtime);
        if (err != HU_OK)
            goto fail;
    }

    bi->policy.autonomy = (hu_autonomy_level_t)bi->cfg.security.autonomy_level;
    bi->policy.workspace_dir = ws;
    bi->policy.workspace_only = true;
    bi->policy.allow_shell = (bi->policy.autonomy != HU_AUTONOMY_READ_ONLY);
    if (with_agent && bi->runtime.vtable && bi->runtime.vtable->has_shell_access)
        bi->policy.allow_shell =
            bi->policy.allow_shell && bi->runtime.vtable->has_shell_access(bi->runtime.ctx);
    bi->policy.block_high_risk_commands = (bi->policy.autonomy == HU_AUTONOMY_SUPERVISED);
    bi->policy.require_approval_for_medium_risk = false;
    bi->policy.max_actions_per_hour = 200;
    bi->policy.tracker = hu_rate_tracker_create(alloc, bi->policy.max_actions_per_hour);
    ctx->policy = &bi->policy;

    if (bi->cfg.security.sandbox_config.enabled ||
        bi->cfg.security.sandbox_config.backend != HU_SANDBOX_NONE) {
        bi->sb_storage = hu_sandbox_storage_create(&bi->sb_alloc);
        if (bi->sb_storage) {
            bi->sandbox = hu_sandbox_create(bi->cfg.security.sandbox_config.backend, ws,
                                            bi->sb_storage, &bi->sb_alloc);
            if (bi->sandbox.vtable) {
                bi->policy.sandbox = &bi->sandbox;
#if defined(__linux__)
                if (strcmp(hu_sandbox_name(&bi->sandbox), "firejail") == 0 &&
                    bi->cfg.security.sandbox_config.firejail_args_len > 0) {
                    hu_firejail_sandbox_set_extra_args(
                        (hu_firejail_ctx_t *)bi->sandbox.ctx,
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
                           i < HU_NET_PROXY_MAX_DOMAINS;
             i++) {
            bi->net_proxy.allowed_domains[bi->net_proxy.allowed_domains_count++] =
                bi->cfg.security.sandbox_config.net_proxy.allowed_domains[i];
        }
        bi->policy.net_proxy = &bi->net_proxy;
    }

    if (with_agent)
        bi->memory = hu_memory_create_from_config(alloc, &bi->cfg, ws);
    if (with_agent && bi->memory.vtable && bi->cfg.memory.backend &&
        strcmp(bi->cfg.memory.backend, "sqlite") == 0)
        bi->session_store = hu_sqlite_memory_get_session_store(&bi->memory);
    if (with_agent) {
        ctx->memory = &bi->memory;
        ctx->session_store = &bi->session_store;
    }

#ifdef HU_HAS_CRON
    bi->cron = hu_cron_create(alloc, 64, true);
#else
    bi->cron = NULL;
#endif
    ctx->cron = bi->cron;

    bi->agent_pool = hu_agent_pool_create(alloc, bi->cfg.agent.pool_max_concurrent);
    bi->mailbox = hu_mailbox_create(alloc, 64);
    ctx->agent_pool = bi->agent_pool;
    ctx->mailbox = bi->mailbox;

#ifdef HU_HAS_SKILLS
    {
        err = hu_skillforge_create(alloc, &bi->skillforge);
        if (err == HU_OK) {
            bi->skillforge_ok = true;
            char skills_dir[512];
            size_t sd_len = hu_skill_registry_get_installed_dir(skills_dir, sizeof(skills_dir));
            if (sd_len > 0)
                hu_skillforge_discover(&bi->skillforge, skills_dir);
        }
    }
#endif

    hu_skillforge_t *sf_ptr = NULL;
#ifdef HU_HAS_SKILLS
    if (bi->skillforge_ok) {
        sf_ptr = &bi->skillforge;
        ctx->skillforge = sf_ptr;
    }
#endif

    hu_agent_registry_t *reg_ptr = bi->agent_registry_ok ? &bi->agent_registry : NULL;
    err = hu_tools_create_default(alloc, ws, strlen(ws), &bi->policy, &bi->cfg,
                                  (with_agent && bi->memory.vtable) ? &bi->memory : NULL, bi->cron,
                                  bi->agent_pool, bi->mailbox, sf_ptr, reg_ptr, &bi->tools,
                                  &bi->tools_count);
    if (err != HU_OK)
        goto fail;
    ctx->tools = bi->tools;
    ctx->tools_count = bi->tools_count;

    if (bi->plugin_reg) {
        hu_tool_t *plugin_tools = NULL;
        size_t plugin_tools_count = 0;
        if (hu_plugin_get_tools(bi->plugin_reg, &plugin_tools, &plugin_tools_count) == HU_OK &&
            plugin_tools_count > 0) {
            hu_tool_t *merged = (hu_tool_t *)alloc->alloc(
                alloc->ctx, (bi->tools_count + plugin_tools_count) * sizeof(hu_tool_t));
            if (merged) {
                memcpy(merged, bi->tools, bi->tools_count * sizeof(hu_tool_t));
                memcpy(merged + bi->tools_count, plugin_tools,
                       plugin_tools_count * sizeof(hu_tool_t));
                alloc->free(alloc->ctx, bi->tools, bi->tools_count * sizeof(hu_tool_t));
                bi->tools = merged;
                bi->tools_count += plugin_tools_count;
            }
            alloc->free(alloc->ctx, plugin_tools, plugin_tools_count * sizeof(hu_tool_t));
        }
        ctx->tools = bi->tools;
        ctx->tools_count = bi->tools_count;
    }

    {
        hu_error_t reg_err = hu_agent_registry_create(alloc, &bi->agent_registry);
        if (reg_err == HU_OK) {
            bi->agent_registry_ok = true;
            const char *home = getenv("HOME");
            if (home && home[0]) {
                char agents_dir[512];
                int n = snprintf(agents_dir, sizeof(agents_dir), "%s/.human/agents", home);
                if (n > 0 && (size_t)n < sizeof(agents_dir))
                    hu_agent_registry_discover(&bi->agent_registry, agents_dir);
            }
            ctx->agent_registry = &bi->agent_registry;
        }
    }

#if HU_HAS_PWA
    {
        hu_error_t pwa_err = hu_pwa_driver_registry_init(&bi->pwa_driver_registry);
        if (pwa_err == HU_OK) {
            bi->pwa_driver_registry_ok = true;
            const char *home = getenv("HOME");
            if (home && home[0]) {
                char pwa_dir[512];
                int n = snprintf(pwa_dir, sizeof(pwa_dir), "%s/.human/pwa", home);
                if (n > 0 && (size_t)n < sizeof(pwa_dir))
                    hu_pwa_driver_registry_load_dir(alloc, &bi->pwa_driver_registry, pwa_dir);
            }
            hu_pwa_set_global_registry(&bi->pwa_driver_registry);
        }
    }
#endif

    if (with_agent) {
        const char *prov_name = bi->cfg.default_provider ? bi->cfg.default_provider : "openai";
        size_t prov_name_len = strlen(prov_name);

        err = hu_provider_create_from_config(alloc, &bi->cfg, prov_name, prov_name_len,
                                             &bi->provider);
        if (err == HU_ERR_NOT_SUPPORTED) {
            const char *api_key = hu_config_default_provider_key(&bi->cfg);
            size_t api_key_len = api_key ? strlen(api_key) : 0;
            const char *base_url = hu_config_get_provider_base_url(&bi->cfg, prov_name);
            size_t base_url_len = base_url ? strlen(base_url) : 0;
            err = hu_provider_create(alloc, prov_name, prov_name_len, api_key, api_key_len,
                                     base_url, base_url_len, &bi->provider);
        }
        if (err != HU_OK)
            goto fail;
        ctx->provider = &bi->provider;
        ctx->provider_ok = true;

        const char *gemini_key = getenv("GEMINI_API_KEY");
        if (gemini_key && gemini_key[0]) {
            hu_embedding_provider_t gem_provider =
                hu_embedding_gemini_create(alloc, gemini_key, NULL, 0);
            bi->embedder = hu_embedder_gemini_adapter_create(alloc, gem_provider);
        }
        if (!bi->embedder.ctx) {
            bi->embedder = hu_embedder_local_create(alloc);
        }
        bi->vector_store = hu_vector_store_mem_create(alloc);
        bi->retrieval_engine =
            hu_retrieval_create_with_vector(alloc, &bi->memory, &bi->embedder, &bi->vector_store);
        ctx->embedder = &bi->embedder;
        ctx->vector_store = &bi->vector_store;
        ctx->retrieval = &bi->retrieval_engine;

        const char *log_env = getenv("HUMAN_LOG");
        if (log_env && log_env[0]) {
            bi->log_fp = fopen(log_env, "a");
            if (bi->log_fp)
                bi->observer = hu_log_observer_create(alloc, bi->log_fp);
        }

        const char *model = bi->cfg.default_model ? bi->cfg.default_model : "";
        double temp = bi->cfg.temperature > 0.0 ? bi->cfg.temperature : 0.7;
        uint32_t max_iters =
            bi->cfg.agent.max_tool_iterations > 0 ? bi->cfg.agent.max_tool_iterations : 25;
        uint32_t max_hist =
            bi->cfg.agent.max_history_messages > 0 ? bi->cfg.agent.max_history_messages : 100;
        hu_agent_context_config_t ctx_cfg = {
            .token_limit = bi->cfg.agent.token_limit,
            .pressure_warn = bi->cfg.agent.context_pressure_warn,
            .pressure_compact = bi->cfg.agent.context_pressure_compact,
            .compact_target = bi->cfg.agent.context_compact_target,
            .llm_compiler_enabled = bi->cfg.agent.llm_compiler_enabled,
            .tree_of_thought = bi->cfg.agent.tree_of_thought,
            .constitutional_ai = bi->cfg.agent.constitutional_ai,
            .speculative_cache = bi->cfg.agent.speculative_cache,
            .tool_routing_enabled = bi->cfg.agent.tool_routing_enabled,
        };
        hu_observer_t *obs = bi->observer.vtable ? &bi->observer : NULL;
        err = hu_agent_from_config(
            &bi->agent, alloc, bi->provider, bi->tools, bi->tools_count,
            bi->memory.vtable ? &bi->memory : NULL,
            bi->session_store.vtable ? &bi->session_store : NULL, obs, &bi->policy, model,
            strlen(model), prov_name, prov_name_len, temp, ws, strlen(ws), max_iters, max_hist,
            bi->cfg.memory.auto_save, 2, NULL, 0, bi->cfg.agent.persona,
            bi->cfg.agent.persona ? strlen(bi->cfg.agent.persona) : 0, &ctx_cfg);
        if (err != HU_OK)
            goto fail;
        bi->agent.chain_of_thought = true;
        bi->agent.agent_pool = bi->agent_pool;
        bi->agent.scheduler = (struct hu_cron_scheduler *)bi->cron;
        hu_agent_set_mailbox(&bi->agent, bi->mailbox);
#ifdef HU_HAS_SKILLS
        if (bi->skillforge_ok)
            bi->agent.skillforge = (struct hu_skillforge *)&bi->skillforge;
#endif
        if (bi->agent_registry_ok)
            bi->agent.agent_registry = (struct hu_agent_registry *)&bi->agent_registry;
        bi->agent.policy_engine = NULL;
        if (bi->cfg.policy.enabled)
            bi->agent.policy_engine = hu_policy_engine_create(alloc);
        hu_agent_set_retrieval_engine(&bi->agent, &bi->retrieval_engine);
        if (bi->cfg.security.audit.enabled) {
            hu_audit_config_t acfg = HU_AUDIT_CONFIG_DEFAULT;
            acfg.enabled = true;
            acfg.log_path =
                bi->cfg.security.audit.log_path ? bi->cfg.security.audit.log_path : "audit.log";
            acfg.max_size_mb =
                bi->cfg.security.audit.max_size_mb > 0 ? bi->cfg.security.audit.max_size_mb : 10;
            bi->agent.audit_logger = hu_audit_logger_create(alloc, &acfg, ws);
        }
        ctx->agent = &bi->agent;
        ctx->agent_ok = true;
    }

    if (with_channels) {
        size_t ch_count = 0;
        const hu_config_t *cfg = &bi->cfg;
        (void)cfg;

#if HU_HAS_EMAIL
        if (cfg->channels.email.smtp_host && cfg->channels.email.from_address) {
            err = hu_email_create(
                alloc, cfg->channels.email.smtp_host, strlen(cfg->channels.email.smtp_host),
                cfg->channels.email.smtp_port ? cfg->channels.email.smtp_port : 587,
                cfg->channels.email.from_address, strlen(cfg->channels.email.from_address),
                &bi->channel_slots[ch_count]);
            if (err == HU_OK) {
                if (cfg->channels.email.smtp_user && cfg->channels.email.smtp_pass) {
                    hu_email_set_auth(&bi->channel_slots[ch_count], cfg->channels.email.smtp_user,
                                      strlen(cfg->channels.email.smtp_user),
                                      cfg->channels.email.smtp_pass,
                                      strlen(cfg->channels.email.smtp_pass));
                }
                if (cfg->channels.email.imap_host) {
                    hu_email_set_imap(&bi->channel_slots[ch_count], cfg->channels.email.imap_host,
                                      strlen(cfg->channels.email.imap_host),
                                      cfg->channels.email.imap_port ? cfg->channels.email.imap_port
                                                                    : 993);
                }
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = hu_email_poll;
                bi->channels[ch_count].interval_ms = 30000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_email_wrap;
                ch_count++;
            }
        }
#endif

#if HU_HAS_IMESSAGE
        if (cfg->channels.imessage.default_target && ch_count < HU_BOOTSTRAP_CHANNELS_MAX) {
            err = hu_imessage_create(alloc, cfg->channels.imessage.default_target,
                                     strlen(cfg->channels.imessage.default_target),
                                     (const char *const *)cfg->channels.imessage.allow_from,
                                     cfg->channels.imessage.allow_from_count,
                                     &bi->channel_slots[ch_count]);
            if (err == HU_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = hu_imessage_poll;
                bi->channels[ch_count].interval_ms = 3000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_imessage_wrap;
                ch_count++;
            }
        }
#else
        if (cfg->channels.imessage.default_target)
            fprintf(stderr,
                    "[bootstrap] WARNING: iMessage configured in config but binary was built "
                    "without HU_ENABLE_IMESSAGE — rebuild with -DSC_ENABLE_IMESSAGE=ON\n");
#endif

#if HU_HAS_PWA
        if (cfg->channels.pwa.apps_count > 0 && ch_count < HU_BOOTSTRAP_CHANNELS_MAX) {
            err = hu_pwa_channel_create(alloc,
                                        (const char *const *)cfg->channels.pwa.apps,
                                        cfg->channels.pwa.apps_count,
                                        &bi->channel_slots[ch_count]);
            if (err == HU_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = hu_pwa_channel_poll;
                bi->channels[ch_count].interval_ms =
                    (uint32_t)(cfg->channels.pwa.poll_interval_sec > 0
                                   ? cfg->channels.pwa.poll_interval_sec * 1000
                                   : 5000);
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_pwa_wrap;
                ch_count++;
            }
        }
#endif

#if HU_HAS_GMAIL
        if (cfg->channels.gmail.client_id && cfg->channels.gmail.client_secret &&
            cfg->channels.gmail.refresh_token && ch_count < HU_BOOTSTRAP_CHANNELS_MAX) {
            err = hu_gmail_create(
                alloc, cfg->channels.gmail.client_id, strlen(cfg->channels.gmail.client_id),
                cfg->channels.gmail.client_secret, strlen(cfg->channels.gmail.client_secret),
                cfg->channels.gmail.refresh_token, strlen(cfg->channels.gmail.refresh_token),
                cfg->channels.gmail.poll_interval_sec, &bi->channel_slots[ch_count]);
            if (err == HU_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = hu_gmail_poll;
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

#if HU_HAS_IMAP
        if (cfg->channels.imap.imap_host && cfg->channels.imap.imap_username &&
            cfg->channels.imap.imap_password && ch_count < HU_BOOTSTRAP_CHANNELS_MAX) {
            hu_imap_config_t imap_cfg = {
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
            err = hu_imap_create(alloc, &imap_cfg, &bi->channel_slots[ch_count]);
            if (err == HU_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = hu_imap_poll;
                bi->channels[ch_count].interval_ms = 30000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_imap_wrap;
                ch_count++;
            }
        }
#endif

#if HU_HAS_TELEGRAM
        if (cfg->channels.telegram.token && ch_count < HU_BOOTSTRAP_CHANNELS_MAX) {
            const char *const *af = (const char *const *)cfg->channels.telegram.allow_from;
            err = hu_telegram_create_ex(
                alloc, cfg->channels.telegram.token, strlen(cfg->channels.telegram.token), af,
                cfg->channels.telegram.allow_from_count, &bi->channel_slots[ch_count]);
            if (err == HU_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = hu_telegram_poll;
                bi->channels[ch_count].interval_ms = 1000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_telegram_wrap;
                ch_count++;
            }
        }
#endif

#if HU_HAS_DISCORD
        if (cfg->channels.discord.token && cfg->channels.discord.channel_ids_count > 0 &&
            ch_count < HU_BOOTSTRAP_CHANNELS_MAX) {
            const char **ch_ids = (const char **)cfg->channels.discord.channel_ids;
            err = hu_discord_create_ex(
                alloc, cfg->channels.discord.token, strlen(cfg->channels.discord.token), ch_ids,
                cfg->channels.discord.channel_ids_count, cfg->channels.discord.bot_id,
                cfg->channels.discord.bot_id ? strlen(cfg->channels.discord.bot_id) : 0,
                &bi->channel_slots[ch_count]);
            if (err == HU_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = hu_discord_poll;
                bi->channels[ch_count].webhook_fn = hu_discord_on_webhook;
                bi->channels[ch_count].interval_ms = 2000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_discord_wrap;
                ch_count++;
            }
        }
#endif

#if HU_HAS_SLACK
        if (cfg->channels.slack.token && cfg->channels.slack.channel_ids_count > 0 &&
            ch_count < HU_BOOTSTRAP_CHANNELS_MAX) {
            const char **sl_ids = (const char **)cfg->channels.slack.channel_ids;
            err = hu_slack_create_ex(
                alloc, cfg->channels.slack.token, strlen(cfg->channels.slack.token), sl_ids,
                cfg->channels.slack.channel_ids_count, &bi->channel_slots[ch_count]);
            if (err == HU_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = hu_slack_poll;
                bi->channels[ch_count].webhook_fn = hu_slack_on_webhook;
                bi->channels[ch_count].interval_ms = 3000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_slack_wrap;
                ch_count++;
            }
        }
#endif

#if HU_HAS_WHATSAPP
        if (cfg->channels.whatsapp.phone_number_id && cfg->channels.whatsapp.token &&
            ch_count < HU_BOOTSTRAP_CHANNELS_MAX) {
            err = hu_whatsapp_create(
                alloc, cfg->channels.whatsapp.phone_number_id,
                strlen(cfg->channels.whatsapp.phone_number_id), cfg->channels.whatsapp.token,
                strlen(cfg->channels.whatsapp.token), &bi->channel_slots[ch_count]);
            if (err == HU_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = hu_whatsapp_poll;
                bi->channels[ch_count].webhook_fn = hu_whatsapp_on_webhook;
                bi->channels[ch_count].interval_ms = 1000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_whatsapp_wrap;
                ch_count++;
            }
        }
#endif

#if HU_HAS_FACEBOOK
        if (cfg->channels.facebook.page_id && cfg->channels.facebook.page_access_token &&
            ch_count < HU_BOOTSTRAP_CHANNELS_MAX) {
            err = hu_facebook_create(
                alloc, cfg->channels.facebook.page_id, strlen(cfg->channels.facebook.page_id),
                cfg->channels.facebook.page_access_token,
                strlen(cfg->channels.facebook.page_access_token),
                cfg->channels.facebook.app_secret ? cfg->channels.facebook.app_secret : "",
                cfg->channels.facebook.app_secret ? strlen(cfg->channels.facebook.app_secret) : 0,
                &bi->channel_slots[ch_count]);
            if (err == HU_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = hu_facebook_poll;
                bi->channels[ch_count].webhook_fn = hu_facebook_on_webhook;
                bi->channels[ch_count].interval_ms = 1000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_facebook_wrap;
                ch_count++;
            }
        }
#endif

#if HU_HAS_LINE
        if (cfg->channels.line.channel_token && ch_count < HU_BOOTSTRAP_CHANNELS_MAX) {
            err = hu_line_create_ex(
                alloc, cfg->channels.line.channel_token, strlen(cfg->channels.line.channel_token),
                cfg->channels.line.user_id,
                cfg->channels.line.user_id ? strlen(cfg->channels.line.user_id) : 0,
                &bi->channel_slots[ch_count]);
            if (err == HU_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = hu_line_poll;
                bi->channels[ch_count].webhook_fn = hu_line_on_webhook;
                bi->channels[ch_count].interval_ms = 1000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_line_wrap;
                ch_count++;
            }
        }
#endif

#if HU_HAS_GOOGLE_CHAT
        if (cfg->channels.google_chat.webhook_url && ch_count < HU_BOOTSTRAP_CHANNELS_MAX) {
            err = hu_google_chat_create(alloc, cfg->channels.google_chat.webhook_url,
                                        strlen(cfg->channels.google_chat.webhook_url),
                                        &bi->channel_slots[ch_count]);
            if (err == HU_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = hu_google_chat_poll;
                bi->channels[ch_count].webhook_fn = hu_google_chat_on_webhook;
                bi->channels[ch_count].interval_ms = 1000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_google_chat_wrap;
                ch_count++;
            }
        }
#endif

#if HU_HAS_INSTAGRAM
        if (cfg->channels.instagram.business_account_id && cfg->channels.instagram.access_token &&
            ch_count < HU_BOOTSTRAP_CHANNELS_MAX) {
            err = hu_instagram_create(
                alloc, cfg->channels.instagram.business_account_id,
                strlen(cfg->channels.instagram.business_account_id),
                cfg->channels.instagram.access_token, strlen(cfg->channels.instagram.access_token),
                cfg->channels.instagram.app_secret,
                cfg->channels.instagram.app_secret ? strlen(cfg->channels.instagram.app_secret) : 0,
                &bi->channel_slots[ch_count]);
            if (err == HU_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = hu_instagram_poll;
                bi->channels[ch_count].webhook_fn = hu_instagram_on_webhook;
                bi->channels[ch_count].interval_ms = 1000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_instagram_wrap;
                ch_count++;
            }
        }
#endif

#if HU_HAS_TWITTER
        if (cfg->channels.twitter.bearer_token && ch_count < HU_BOOTSTRAP_CHANNELS_MAX) {
            err = hu_twitter_create(alloc, cfg->channels.twitter.bearer_token,
                                    strlen(cfg->channels.twitter.bearer_token),
                                    &bi->channel_slots[ch_count]);
            if (err == HU_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = hu_twitter_poll;
                bi->channels[ch_count].webhook_fn = hu_twitter_on_webhook;
                bi->channels[ch_count].interval_ms = 1000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_twitter_wrap;
                ch_count++;
            }
        }
#endif

#if HU_HAS_TIKTOK
        if (cfg->channels.tiktok.access_token && ch_count < HU_BOOTSTRAP_CHANNELS_MAX) {
            err = hu_tiktok_create(
                alloc, cfg->channels.tiktok.client_key,
                cfg->channels.tiktok.client_key ? strlen(cfg->channels.tiktok.client_key) : 0,
                cfg->channels.tiktok.client_secret,
                cfg->channels.tiktok.client_secret ? strlen(cfg->channels.tiktok.client_secret) : 0,
                cfg->channels.tiktok.access_token, strlen(cfg->channels.tiktok.access_token),
                &bi->channel_slots[ch_count]);
            if (err == HU_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = hu_tiktok_poll;
                bi->channels[ch_count].webhook_fn = hu_tiktok_on_webhook;
                bi->channels[ch_count].interval_ms = 1000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_tiktok_wrap;
                ch_count++;
            }
        }
#endif

#if HU_HAS_GOOGLE_RCS
        if (cfg->channels.google_rcs.agent_id && cfg->channels.google_rcs.token &&
            ch_count < HU_BOOTSTRAP_CHANNELS_MAX) {
            err = hu_google_rcs_create(
                alloc, cfg->channels.google_rcs.agent_id, strlen(cfg->channels.google_rcs.agent_id),
                cfg->channels.google_rcs.token, strlen(cfg->channels.google_rcs.token),
                &bi->channel_slots[ch_count]);
            if (err == HU_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = hu_google_rcs_poll;
                bi->channels[ch_count].webhook_fn = hu_google_rcs_on_webhook;
                bi->channels[ch_count].interval_ms = 1000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_google_rcs_wrap;
                ch_count++;
            }
        }
#endif

#if HU_HAS_MQTT
        if (cfg->channels.mqtt.broker_url && ch_count < HU_BOOTSTRAP_CHANNELS_MAX) {
            const char *in_t = cfg->channels.mqtt.inbound_topic;
            const char *out_t = cfg->channels.mqtt.outbound_topic;
            const char *usr = cfg->channels.mqtt.username;
            const char *pwd = cfg->channels.mqtt.password;
            err = hu_mqtt_create(alloc, cfg->channels.mqtt.broker_url,
                                 strlen(cfg->channels.mqtt.broker_url), in_t,
                                 in_t ? strlen(in_t) : 0, out_t, out_t ? strlen(out_t) : 0, usr,
                                 usr ? strlen(usr) : 0, pwd, pwd ? strlen(pwd) : 0,
                                 cfg->channels.mqtt.qos, &bi->channel_slots[ch_count]);
            if (err == HU_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = hu_mqtt_poll;
                bi->channels[ch_count].webhook_fn = NULL;
                bi->channels[ch_count].interval_ms = 500;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_mqtt_wrap;
                ch_count++;
            }
        }
#endif

#if HU_HAS_MATRIX
        if (cfg->channels.matrix.homeserver && cfg->channels.matrix.access_token &&
            ch_count < HU_BOOTSTRAP_CHANNELS_MAX) {
            err = hu_matrix_create(
                alloc, cfg->channels.matrix.homeserver, strlen(cfg->channels.matrix.homeserver),
                cfg->channels.matrix.access_token, strlen(cfg->channels.matrix.access_token),
                &bi->channel_slots[ch_count]);
            if (err == HU_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = hu_matrix_poll;
                bi->channels[ch_count].interval_ms = 3000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_matrix_wrap;
                ch_count++;
            }
        }
#endif

#if HU_HAS_IRC
        if (cfg->channels.irc.server && ch_count < HU_BOOTSTRAP_CHANNELS_MAX) {
            err = hu_irc_create(alloc, cfg->channels.irc.server, strlen(cfg->channels.irc.server),
                                cfg->channels.irc.port ? cfg->channels.irc.port : 6667,
                                &bi->channel_slots[ch_count]);
            if (err == HU_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = hu_irc_poll;
                bi->channels[ch_count].interval_ms = 1000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_irc_wrap;
                ch_count++;
            }
        }
#endif

#if HU_HAS_NOSTR
        if (cfg->channels.nostr.relay_url && ch_count < HU_BOOTSTRAP_CHANNELS_MAX) {
            err = hu_nostr_create(
                alloc, cfg->channels.nostr.nak_path ? cfg->channels.nostr.nak_path : "",
                cfg->channels.nostr.nak_path ? strlen(cfg->channels.nostr.nak_path) : 0,
                cfg->channels.nostr.bot_pubkey ? cfg->channels.nostr.bot_pubkey : "",
                cfg->channels.nostr.bot_pubkey ? strlen(cfg->channels.nostr.bot_pubkey) : 0,
                cfg->channels.nostr.relay_url, strlen(cfg->channels.nostr.relay_url),
                cfg->channels.nostr.seckey_hex ? cfg->channels.nostr.seckey_hex : "",
                cfg->channels.nostr.seckey_hex ? strlen(cfg->channels.nostr.seckey_hex) : 0,
                &bi->channel_slots[ch_count]);
            if (err == HU_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = hu_nostr_poll;
                bi->channels[ch_count].interval_ms = 2000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_nostr_wrap;
                ch_count++;
            }
        }
#endif

#if HU_HAS_LARK
        if (((cfg->channels.lark.app_id && cfg->channels.lark.app_secret) ||
             cfg->channels.lark.webhook_url) &&
            ch_count < HU_BOOTSTRAP_CHANNELS_MAX) {
            const char *id_or_wh = cfg->channels.lark.webhook_url ? cfg->channels.lark.webhook_url
                                                                  : cfg->channels.lark.app_id;
            size_t id_len = cfg->channels.lark.webhook_url ? strlen(cfg->channels.lark.webhook_url)
                                                           : strlen(cfg->channels.lark.app_id);
            const char *secret =
                cfg->channels.lark.webhook_url ? "" : cfg->channels.lark.app_secret;
            size_t secret_len =
                cfg->channels.lark.webhook_url ? 0 : strlen(cfg->channels.lark.app_secret);
            err = hu_lark_create(alloc, id_or_wh, id_len, secret, secret_len,
                                 &bi->channel_slots[ch_count]);
            if (err == HU_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = hu_lark_poll;
                bi->channels[ch_count].webhook_fn = hu_lark_on_webhook;
                bi->channels[ch_count].interval_ms = 1000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_lark_wrap;
                ch_count++;
            }
        }
#endif

#if HU_HAS_DINGTALK
        if (((cfg->channels.dingtalk.app_key && cfg->channels.dingtalk.app_secret) ||
             cfg->channels.dingtalk.webhook_url) &&
            ch_count < HU_BOOTSTRAP_CHANNELS_MAX) {
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
            err = hu_dingtalk_create(alloc, key_or_wh, key_len, secret, secret_len,
                                     &bi->channel_slots[ch_count]);
            if (err == HU_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = hu_dingtalk_poll;
                bi->channels[ch_count].webhook_fn = hu_dingtalk_on_webhook;
                bi->channels[ch_count].interval_ms = 1000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_dingtalk_wrap;
                ch_count++;
            }
        }
#endif

#if HU_HAS_TEAMS
        if (cfg->channels.teams.webhook_url && ch_count < HU_BOOTSTRAP_CHANNELS_MAX) {
            err = hu_teams_create(alloc, cfg->channels.teams.webhook_url,
                                  strlen(cfg->channels.teams.webhook_url),
                                  &bi->channel_slots[ch_count]);
            if (err == HU_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = hu_teams_poll;
                bi->channels[ch_count].webhook_fn = hu_teams_on_webhook;
                bi->channels[ch_count].interval_ms = 1000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_teams_wrap;
                ch_count++;
            }
        }
#endif

#if HU_HAS_TWILIO
        if (cfg->channels.twilio.account_sid && cfg->channels.twilio.auth_token &&
            cfg->channels.twilio.from_number && ch_count < HU_BOOTSTRAP_CHANNELS_MAX) {
            const char *tw_to = cfg->channels.twilio.to_number;
            size_t tw_to_len = tw_to ? strlen(tw_to) : 0;
            err = hu_twilio_create(
                alloc, cfg->channels.twilio.account_sid, strlen(cfg->channels.twilio.account_sid),
                cfg->channels.twilio.auth_token, strlen(cfg->channels.twilio.auth_token),
                cfg->channels.twilio.from_number, strlen(cfg->channels.twilio.from_number), tw_to,
                tw_to_len, &bi->channel_slots[ch_count]);
            if (err == HU_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = hu_twilio_poll;
                bi->channels[ch_count].webhook_fn = hu_twilio_on_webhook;
                bi->channels[ch_count].interval_ms = 1000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_twilio_wrap;
                ch_count++;
            }
        }
#endif

#if HU_HAS_ONEBOT
        if (cfg->channels.onebot.api_base && cfg->channels.onebot.user_id &&
            ch_count < HU_BOOTSTRAP_CHANNELS_MAX) {
            const char *tok = cfg->channels.onebot.access_token;
            const char *uid = cfg->channels.onebot.user_id;
            err = hu_onebot_create_ex(alloc, cfg->channels.onebot.api_base,
                                      strlen(cfg->channels.onebot.api_base), tok ? tok : "",
                                      tok ? strlen(tok) : 0, uid, strlen(uid),
                                      &bi->channel_slots[ch_count]);
            if (err == HU_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = hu_onebot_poll;
                bi->channels[ch_count].webhook_fn = hu_onebot_on_webhook;
                bi->channels[ch_count].interval_ms = 1000;
                bi->channels[ch_count].last_poll_ms = 0;
                bi->channel_destroys[ch_count] = destroy_onebot_wrap;
                ch_count++;
            }
        }
#endif

#if HU_HAS_QQ
        if (cfg->channels.qq.app_id && cfg->channels.qq.bot_token &&
            ch_count < HU_BOOTSTRAP_CHANNELS_MAX) {
            const char *ch_id = cfg->channels.qq.channel_id;
            err = hu_qq_create_ex(alloc, cfg->channels.qq.app_id, strlen(cfg->channels.qq.app_id),
                                  cfg->channels.qq.bot_token, strlen(cfg->channels.qq.bot_token),
                                  ch_id ? ch_id : "", ch_id ? strlen(ch_id) : 0,
                                  cfg->channels.qq.sandbox, &bi->channel_slots[ch_count]);
            if (err == HU_OK) {
                bi->channels[ch_count].channel_ctx = bi->channel_slots[ch_count].ctx;
                bi->channels[ch_count].channel = &bi->channel_slots[ch_count];
                bi->channels[ch_count].poll_fn = hu_qq_poll;
                bi->channels[ch_count].webhook_fn = hu_qq_on_webhook;
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

    return HU_OK;

fail:
    hu_app_teardown(ctx);
    return err;
}

void hu_app_teardown(hu_app_ctx_t *ctx) {
    if (!ctx || !ctx->alloc)
        return;
    hu_allocator_t *alloc = ctx->alloc;
    hu_bootstrap_internal_t *bi = (hu_bootstrap_internal_t *)ctx->channel_instances;
    if (!bi)
        return;

    /* Channels (reverse order, though order typically doesn't matter) */
    for (size_t i = 0; i < bi->channel_count && i < HU_BOOTSTRAP_CHANNELS_MAX; i++) {
        if (bi->channel_destroys[i] && bi->channel_slots[i].ctx)
            bi->channel_destroys[i](&bi->channel_slots[i], alloc);
    }

    if (ctx->agent_ok && bi->agent.alloc) {
        if (bi->agent.policy_engine)
            hu_policy_engine_destroy(bi->agent.policy_engine);
        hu_agent_deinit(&bi->agent);
        bi->provider.vtable = NULL;
        bi->provider.ctx = NULL;
    }
    hu_tools_destroy_default(alloc, bi->tools, bi->tools_count);
    if (bi->cron)
        hu_cron_destroy(bi->cron, alloc);
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
        hu_rate_tracker_destroy(bi->policy.tracker);
    if (bi->sb_storage)
        hu_sandbox_storage_destroy(bi->sb_storage, &bi->sb_alloc);
    if (bi->plugin_reg)
        hu_plugin_registry_destroy(bi->plugin_reg);
    if (bi->agent_registry_ok)
        hu_agent_registry_destroy(&bi->agent_registry);
#if HU_HAS_PWA
    if (bi->pwa_driver_registry_ok) {
        hu_pwa_set_global_registry(NULL);
        hu_pwa_driver_registry_destroy(alloc, &bi->pwa_driver_registry);
    }
#endif
#ifdef HU_HAS_SKILLS
    if (bi->skillforge_ok)
        hu_skillforge_destroy(&bi->skillforge);
#endif
    if (bi->agent_pool)
        hu_agent_pool_destroy(bi->agent_pool);
    if (bi->mailbox)
        hu_mailbox_destroy(bi->mailbox);
    hu_config_deinit(&bi->cfg);
    alloc->free(alloc->ctx, bi, sizeof(hu_bootstrap_internal_t));
    ctx->channel_instances = NULL;
    memset(ctx, 0, sizeof(*ctx));
}
