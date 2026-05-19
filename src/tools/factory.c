#include "human/tools/factory.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/log.h"
#ifdef HU_HAS_CRON
#include "human/cron.h"
#endif
#include "human/mcp.h"
#include "human/security.h"
#include "human/tools/agent_query.h"
#include "human/tools/agent_spawn.h"
#include "human/tools/apply_patch.h"
#ifdef HU_HAS_TOOLS_BROWSER
#include "human/tools/browser.h"
#include "human/tools/browser_open.h"
#include "human/tools/screenshot.h"
#endif
#ifdef HU_HAS_TOOLS_ADVANCED
#include "human/tools/canvas.h"
#include "human/tools/claude_code.h"
#include "human/tools/composio.h"
#include "human/tools/database.h"
#include "human/tools/declarative.h"
#include "human/tools/notebook.h"
#endif
#ifdef HU_HAS_CRON
#include "human/tools/cron_add.h"
#include "human/tools/cron_list.h"
#include "human/tools/cron_remove.h"
#include "human/tools/cron_run.h"
#include "human/tools/cron_runs.h"
#include "human/tools/cron_update.h"
#endif
#include "human/tools/browser_use.h"
#include "human/tools/code_sandbox.h"
#include "human/tools/computer_use.h"
#include "human/tools/delegate.h"
#include "human/tools/diff.h"
#include "human/tools/file_append.h"
#include "human/tools/file_edit.h"
#include "human/tools/file_read.h"
#include "human/tools/file_write.h"
#include "human/tools/git.h"
#include "human/tools/gui_agent.h"
#include "human/tools/image_gen.h"
#include "human/tools/media_gif.h"
#include "human/tools/media_image.h"
#include "human/tools/media_video.h"
#ifdef HU_HAS_PERIPHERALS
#include "human/tools/hardware_info.h"
#include "human/tools/hardware_memory.h"
#include "human/tools/i2c.h"
#include "human/tools/peripheral_ctrl.h"
#include "human/tools/spi.h"
#endif
#include "human/tools/analytics.h"
#include "human/tools/bff_memory.h"
#include "human/tools/broadcast.h"
#include "human/tools/calendar_tool.h"
#include "human/tools/crm.h"
#include "human/tools/doc_ingest.h"
#include "human/tools/facebook.h"
#include "human/tools/firebase.h"
#include "human/tools/gcloud.h"
#include "human/tools/homeassistant.h"
#include "human/tools/http_request.h"
#include "human/tools/image.h"
#include "human/tools/instagram.h"
#include "human/tools/invoice.h"
#include "human/tools/jira.h"
#include "human/tools/meeting_transcribe.h"
#include "human/tools/memory_forget.h"
#include "human/tools/memory_list.h"
#include "human/tools/memory_recall.h"
#include "human/tools/memory_store.h"
#include "human/tools/message.h"
#include "human/tools/pdf.h"
#include "human/tools/pushover.h"
#include "human/tools/report.h"
#include "human/tools/save_for_later.h"
#include "human/tools/social.h"
#include "human/tools/spreadsheet.h"
#include "human/tools/twitter.h"
#include "human/tools/workflow.h"
#ifdef HU_HAS_CRON
#include "human/tools/schedule.h"
#endif
#include "human/tools/persona.h"
#ifdef HU_ENABLE_CURL
#include "human/tools/paperclip.h"
#endif
#include "human/tools/pwa.h"
#include "human/tools/schema.h"
#include "human/tools/send_message.h"
#include "human/tools/shell.h"
#ifdef HU_HAS_SKILLS
#include "human/tools/skill_run.h"
#endif
#include "human/tools/ask_user.h"
#include "human/tools/db_introspect.h"
#include "human/tools/lsp.h"
#include "human/tools/send_voice_message.h"
#include "human/tools/skill_write.h"
#include "human/tools/spawn.h"
#include "human/tools/task_tools.h"
#include "human/tools/tool_search.h"
#include "human/tools/voice_clone.h"
#include "human/tools/web_fetch.h"
#include "human/tools/web_search.h"
#include "human/tools/webhook_tools.h"
#include <stdlib.h>
#include <string.h>

#define HU_WEB_FETCH_MAX_CHARS 100000

/* Honest tool-registry counters (see include/human/tools/factory.h).
 * Process-lifetime; test seam resets via hu_tools_factory_reset_honesty_counters. */
static unsigned g_factory_twitter_skipped_count = 0;
static unsigned g_factory_lsp_skipped_count = 0;
static int g_factory_twitter_warned_once = 0;
static int g_factory_lsp_warned_once = 0;

unsigned hu_tools_factory_twitter_skipped_count(void) {
    return g_factory_twitter_skipped_count;
}

unsigned hu_tools_factory_lsp_skipped_count(void) {
    return g_factory_lsp_skipped_count;
}

void hu_tools_factory_reset_honesty_counters(void) {
    g_factory_twitter_skipped_count = 0;
    g_factory_lsp_skipped_count = 0;
    g_factory_twitter_warned_once = 0;
    g_factory_lsp_warned_once = 0;
}

/* Predicate: are twitter API credentials configured?
 * Mirrors the conditions src/tools/twitter.c:46 short-circuits on
 * (canned "Twitter API not configured" failure). Until that file gains a real
 * HTTP path, this predicate is the operator-honest gate at registration time:
 * if no credentials are present, the tool would always return that canned
 * failure, so do not register it. */
static bool hu_twitter_credentials_available(const hu_config_t *config) {
    if (!config)
        return false;
    const hu_twitter_channel_config_t *t = &config->channels.twitter;
    if (t->bearer_token && t->bearer_token[0])
        return true;
    if (t->api_key && t->api_key[0])
        return true;
    if (t->access_token && t->access_token[0])
        return true;
    /* Read-only feed bearer also qualifies — twitter tool's search action only
     * needs the bearer token. */
    if (config->feeds.twitter_bearer_token && config->feeds.twitter_bearer_token[0])
        return true;
    return false;
}

#ifdef HU_HAS_CRON
#define HU_TOOLS_CRON_COUNT 7
#else
#define HU_TOOLS_CRON_COUNT 0
#endif
#define HU_TOOLS_PERSONA_COUNT 1
#ifdef HU_ENABLE_CARTESIA
#define HU_TOOLS_CARTESIA_COUNT 1
#else
#define HU_TOOLS_CARTESIA_COUNT 0
#endif
#define HU_TOOLS_WEBHOOK_COUNT       3 /* webhook_register, webhook_poll, webhook_list */
#define HU_TOOLS_DB_INTROSPECT_COUNT 1 /* db_introspect */
/* Base: 59 (core tools incl. lsp + tool_search + 3 media gen) + 5 (ask_user + 4 task tools) + 1
 * (db_introspect) + cron - 1 (skill_run conditional) + persona + cartesia + webhook */
#define HU_TOOLS_COUNT_BASE                                                                     \
    (59 + 5 + HU_TOOLS_DB_INTROSPECT_COUNT + HU_TOOLS_CRON_COUNT - 1 + HU_TOOLS_PERSONA_COUNT + \
     HU_TOOLS_CARTESIA_COUNT + HU_TOOLS_WEBHOOK_COUNT)
#ifdef HU_HAS_TOOLS_BROWSER
#define HU_TOOLS_BROWSER_COUNT 3
#else
#define HU_TOOLS_BROWSER_COUNT 0
#endif
#ifdef HU_HAS_TOOLS_ADVANCED
#define HU_TOOLS_ADVANCED_COUNT 5
#else
#define HU_TOOLS_ADVANCED_COUNT 0
#endif
#ifdef HU_HAS_PERIPHERALS
#define HU_TOOLS_HW_COUNT 5
#else
#define HU_TOOLS_HW_COUNT 0
#endif
#ifdef HU_ENABLE_CURL
#define HU_TOOLS_PAPERCLIP_COUNT 1
#else
#define HU_TOOLS_PAPERCLIP_COUNT 0
#endif
#define HU_TOOLS_COUNT                                                                            \
    (HU_TOOLS_COUNT_BASE + HU_TOOLS_BROWSER_COUNT + HU_TOOLS_ADVANCED_COUNT + HU_TOOLS_HW_COUNT + \
     HU_TOOLS_PAPERCLIP_COUNT)

static hu_error_t add_tool_ws(hu_allocator_t *alloc, hu_tool_t *tools, size_t *idx, const char *ws,
                              size_t ws_len, hu_security_policy_t *policy,
                              hu_error_t (*create)(hu_allocator_t *, const char *, size_t,
                                                   hu_security_policy_t *, hu_tool_t *)) {
    hu_error_t err = create(alloc, ws ? ws : ".", ws_len ? ws_len : 1, policy, &tools[*idx]);
    if (err != HU_OK)
        return err;
    (*idx)++;
    return HU_OK;
}

hu_error_t hu_tools_create_default(hu_allocator_t *alloc, const char *workspace_dir,
                                   size_t workspace_dir_len, hu_security_policy_t *policy,
                                   const hu_config_t *config, hu_memory_t *memory,
                                   hu_cron_scheduler_t *cron, hu_agent_pool_t *agent_pool,
                                   hu_mailbox_t *mailbox, hu_skillforge_t *skillforge,
                                   hu_agent_registry_t *agent_registry, hu_tool_t **out_tools,
                                   size_t *out_count) {
    if (!alloc || !out_tools || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
#ifndef HU_HAS_CRON
    (void)cron;
#endif
#ifndef HU_HAS_SKILLS
    (void)skillforge;
#endif

    size_t tools_alloc = HU_TOOLS_COUNT * sizeof(hu_tool_t);
    hu_tool_t *tools = (hu_tool_t *)alloc->alloc(alloc->ctx, tools_alloc);
    if (!tools)
        return HU_ERR_OUT_OF_MEMORY;
    memset(tools, 0, tools_alloc);

    size_t idx = 0;
    hu_error_t err;

    err =
        add_tool_ws(alloc, tools, &idx, workspace_dir, workspace_dir_len, policy, hu_shell_create);
    if (err != HU_OK)
        goto fail;

    err = add_tool_ws(alloc, tools, &idx, workspace_dir, workspace_dir_len, policy,
                      hu_file_read_create);
    if (err != HU_OK)
        goto fail;

    err = add_tool_ws(alloc, tools, &idx, workspace_dir, workspace_dir_len, policy,
                      hu_file_write_create);
    if (err != HU_OK)
        goto fail;

    err = add_tool_ws(alloc, tools, &idx, workspace_dir, workspace_dir_len, policy,
                      hu_file_edit_create);
    if (err != HU_OK)
        goto fail;

    err = add_tool_ws(alloc, tools, &idx, workspace_dir, workspace_dir_len, policy,
                      hu_file_append_create);
    if (err != HU_OK)
        goto fail;

    err = add_tool_ws(alloc, tools, &idx, workspace_dir, workspace_dir_len, policy, hu_git_create);
    if (err != HU_OK)
        goto fail;

    err = hu_web_search_create(alloc, config, NULL, 0, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_web_fetch_create(alloc, HU_WEB_FETCH_MAX_CHARS, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_http_request_create(alloc, false, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

#ifdef HU_HAS_TOOLS_BROWSER
    err = hu_browser_create(alloc, false, policy, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_screenshot_create(alloc, false, policy, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;
#endif

    err = hu_image_create(alloc, NULL, 0, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_memory_store_create(alloc, memory, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_save_for_later_create(alloc, memory, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_memory_recall_create(alloc, memory, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_memory_list_create(alloc, memory, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_memory_forget_create(alloc, memory, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_bff_memory_create(alloc, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = add_tool_ws(alloc, tools, &idx, workspace_dir, workspace_dir_len, policy,
                      hu_doc_ingest_create);
    if (err != HU_OK)
        goto fail;

    err = add_tool_ws(alloc, tools, &idx, workspace_dir, workspace_dir_len, policy,
                      hu_meeting_transcribe_create);
    if (err != HU_OK)
        goto fail;

    err = hu_message_create(alloc, NULL, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_delegate_create(alloc, policy, agent_pool, agent_registry, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err =
        add_tool_ws(alloc, tools, &idx, workspace_dir, workspace_dir_len, policy, hu_spawn_create);
    if (err != HU_OK)
        goto fail;

#ifdef HU_HAS_CRON
    err = hu_cron_add_create(alloc, cron, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_cron_list_create(alloc, cron, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_cron_remove_create(alloc, cron, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_cron_run_create(alloc, cron, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_cron_runs_create(alloc, cron, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_cron_update_create(alloc, cron, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;
#endif

#ifdef HU_HAS_TOOLS_BROWSER
    {
        const char *domains[] = {"example.com"};
        err = hu_browser_open_create(alloc, domains, 1, policy, &tools[idx]);
    }
    if (err != HU_OK)
        goto fail;
    idx++;
#endif

#ifdef HU_HAS_TOOLS_ADVANCED
    err = hu_composio_create(alloc, NULL, 0, "default", 7, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;
#endif

#ifdef HU_HAS_PERIPHERALS
    err = hu_hardware_memory_create(alloc, NULL, 0, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;
#endif

#ifdef HU_HAS_CRON
    err = hu_schedule_create(alloc, cron, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;
#endif

    err = hu_schema_create(alloc, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_gui_agent_create(alloc, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_computer_use_create(alloc, policy, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_image_gen_create(alloc, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_media_image_create(alloc, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_media_video_create(alloc, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_media_gif_create(alloc, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_browser_use_create(alloc, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_code_sandbox_create(alloc, policy, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_pushover_create(alloc, NULL, 0, NULL, 0, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

#ifdef HU_HAS_PERIPHERALS
    err = hu_hardware_info_create(alloc, false, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_i2c_create(alloc, NULL, 0, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_spi_create(alloc, NULL, 0, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_peripheral_ctrl_tool_create(alloc, NULL, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;
#endif

#ifdef HU_HAS_TOOLS_ADVANCED
    err = hu_claude_code_create(alloc, policy, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_database_tool_create(alloc, NULL, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_notebook_create(alloc, policy, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_canvas_create(alloc, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;
#endif

    err = add_tool_ws(alloc, tools, &idx, workspace_dir, workspace_dir_len, policy,
                      hu_diff_tool_create);
    if (err != HU_OK)
        goto fail;

    err = hu_apply_patch_create(alloc, policy, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_agent_query_tool_create(alloc, agent_pool, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_agent_spawn_tool_create(alloc, agent_pool, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_persona_tool_create(alloc, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_send_message_create(alloc, mailbox, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_pdf_create(alloc, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_spreadsheet_create(alloc, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_report_create(alloc, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_broadcast_create(alloc, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_calendar_create(alloc, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_homeassistant_create(alloc, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_skill_write_create(alloc, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

#ifdef HU_HAS_SKILLS
    err = hu_skill_run_create(alloc, &tools[idx], skillforge, workspace_dir, workspace_dir_len,
                              policy);
    if (err != HU_OK)
        goto fail;
    idx++;
#endif

    err = hu_pwa_tool_create(alloc, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_jira_create(alloc, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_social_create(alloc, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_facebook_tool_create(alloc, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_instagram_tool_create(alloc, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    /* Twitter tool registers ONLY when credentials are configured. Without
     * credentials, twitter_execute() in src/tools/twitter.c falls through to a
     * canned "Twitter API not configured" failure regardless of input; that
     * silent stub misleads operators who see the tool in the registry. The
     * honest behaviour is to NOT register, and to log once at startup naming
     * the missing config so operators can find the fix. */
    if (hu_twitter_credentials_available(config)) {
        err = hu_twitter_tool_create(alloc, &tools[idx]);
        if (err != HU_OK)
            goto fail;
        idx++;
    } else {
        g_factory_twitter_skipped_count++;
        if (!g_factory_twitter_warned_once) {
            g_factory_twitter_warned_once = 1;
            hu_log_info("tools/factory", NULL,
                        "twitter tool not registered: twitter API credentials "
                        "missing from config.twitter (set bearer_token, api_key, "
                        "or access_token to activate)");
        }
    }

    err = hu_gcloud_create(alloc, policy, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_firebase_create(alloc, policy, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_crm_create(alloc, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_analytics_create(alloc, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_invoice_create(alloc, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_workflow_create(alloc, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

#ifdef HU_ENABLE_CARTESIA
    err = add_tool_ws(alloc, tools, &idx, workspace_dir, workspace_dir_len, policy,
                      hu_voice_clone_tool_create);
    if (err != HU_OK)
        goto fail;
#endif

    err = hu_send_voice_message_create(alloc, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    /* lsp tool removed from default registry pending a real implementation.
     * src/tools/lsp.c currently returns canned empty diagnostics in test and
     * "LSP not supported" in production with no LSP client behind it. The
     * source file remains on disk for a future chip to flesh out, but it must
     * not be exposed to agents as if it worked. */
    g_factory_lsp_skipped_count++;
    if (!g_factory_lsp_warned_once) {
        g_factory_lsp_warned_once = 1;
        hu_log_info("tools/factory", NULL,
                    "lsp tool removed pending real implementation "
                    "(src/tools/lsp.c is a canned stub)");
    }

    /* tool_search: searches available tools by name/keyword.
     * Note: pass tools array and current idx (doesn't include itself yet) */
    err = hu_tool_search_create(alloc, tools, idx, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

#ifdef HU_ENABLE_CURL
    err = hu_paperclip_tool_create(alloc, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;
#endif

    /* ask_user tool */
    tools[idx] = hu_tool_ask_user_create(alloc, NULL);
    if (!tools[idx].ctx || !tools[idx].vtable) {
        goto fail;
    }
    idx++;

    /* task_create tool */
    tools[idx] = hu_tool_task_create(alloc, NULL);
    if (!tools[idx].ctx || !tools[idx].vtable) {
        goto fail;
    }
    idx++;

    /* task_update tool */
    tools[idx] = hu_tool_task_update(alloc, NULL);
    if (!tools[idx].ctx || !tools[idx].vtable) {
        goto fail;
    }
    idx++;

    /* task_list tool */
    tools[idx] = hu_tool_task_list(alloc, NULL);
    if (!tools[idx].ctx || !tools[idx].vtable) {
        goto fail;
    }
    idx++;

    /* task_get tool */
    tools[idx] = hu_tool_task_get(alloc, NULL);
    if (!tools[idx].ctx || !tools[idx].vtable) {
        goto fail;
    }
    idx++;

    /* db_introspect tool */
    err = hu_db_introspect_tool_create(alloc, NULL, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    /* Plan 11: Declarative tools from workspace YAML/JSON definitions */
#ifdef HU_HAS_TOOLS_ADVANCED
    {
        hu_declarative_tool_def_t *decl_defs = NULL;
        size_t decl_count = 0;
        const char *tools_dir = workspace_dir ? workspace_dir : ".";
        if (hu_declarative_tools_discover(alloc, tools_dir, &decl_defs, &decl_count) == HU_OK &&
            decl_count > 0) {
            for (size_t di = 0; di < decl_count && idx < HU_TOOLS_COUNT; di++) {
                if (hu_declarative_tool_create(alloc, &decl_defs[di], &tools[idx]) == HU_OK)
                    idx++;
            }
            for (size_t di = 0; di < decl_count; di++)
                hu_declarative_tool_def_free(&decl_defs[di], alloc);
            alloc->free(alloc->ctx, decl_defs, decl_count * sizeof(*decl_defs));
        }
    }
#endif

    /* Load MCP server tools from config when available */
    hu_tool_t *mcp_tools = NULL;
    size_t mcp_count = 0;
    if (config && config->mcp_servers_len > 0) {
        hu_mcp_client_config_t mcp_configs[HU_MCP_SERVERS_MAX];
        for (size_t i = 0; i < config->mcp_servers_len && i < HU_MCP_SERVERS_MAX; i++) {
            mcp_configs[i].command = config->mcp_servers[i].command;
            mcp_configs[i].args = (const char **)config->mcp_servers[i].args;
            mcp_configs[i].args_count = config->mcp_servers[i].args_count;
        }
        hu_mcp_init_tools(alloc, mcp_configs, config->mcp_servers_len, &mcp_tools, &mcp_count);
    }

    if (mcp_count > 0 && mcp_tools) {
        size_t new_alloc = (idx + mcp_count) * sizeof(hu_tool_t);
        hu_tool_t *merged = (hu_tool_t *)alloc->realloc(alloc->ctx, tools, tools_alloc, new_alloc);
        if (merged) {
            tools = merged;
            tools_alloc = new_alloc;
            memcpy(&tools[idx], mcp_tools, mcp_count * sizeof(hu_tool_t));
            idx += mcp_count;
            alloc->free(alloc->ctx, mcp_tools, mcp_count * sizeof(hu_tool_t));
        } else {
            hu_mcp_free_tools(alloc, mcp_tools, mcp_count);
        }
    }

    if (config) {
        for (size_t i = 0; i < idx; i++) {
            if (!tools[i].vtable || !tools[i].vtable->name)
                continue;
            const char *tname = tools[i].vtable->name(tools[i].ctx);
            if (!tname)
                continue;
            bool keep = true;
            if (config->tools.enabled_tools && config->tools.enabled_tools_len > 0) {
                keep = false;
                for (size_t j = 0; j < config->tools.enabled_tools_len; j++) {
                    if (config->tools.enabled_tools[j] &&
                        strcmp(config->tools.enabled_tools[j], tname) == 0) {
                        keep = true;
                        break;
                    }
                }
            }
            if (keep && config->tools.disabled_tools) {
                for (size_t j = 0; j < config->tools.disabled_tools_len; j++) {
                    if (config->tools.disabled_tools[j] &&
                        strcmp(config->tools.disabled_tools[j], tname) == 0) {
                        keep = false;
                        break;
                    }
                }
            }
            if (!keep) {
                if (tools[i].vtable->deinit)
                    tools[i].vtable->deinit(tools[i].ctx, alloc);
                if (i + 1 < idx)
                    memmove(&tools[i], &tools[i + 1], (idx - i - 1) * sizeof(hu_tool_t));
                idx--;
                i--;
            }
        }
    }

    /* Shrink to exact size so destroy_default can free with count * sizeof */
    if (idx * sizeof(hu_tool_t) < tools_alloc) {
        size_t new_size = idx * sizeof(hu_tool_t);
        hu_tool_t *shrunk = (hu_tool_t *)alloc->realloc(alloc->ctx, tools, tools_alloc, new_size);
        if (shrunk) {
            tools = shrunk;
            tools_alloc = new_size;
        }
    }

    *out_tools = tools;
    *out_count = idx;
    return HU_OK;

fail:
    for (size_t i = 0; i < idx; i++) {
        if (tools[i].vtable && tools[i].vtable->deinit)
            tools[i].vtable->deinit(tools[i].ctx, alloc);
    }
    alloc->free(alloc->ctx, tools, tools_alloc);
    return err;
}

void hu_tools_destroy_default(hu_allocator_t *alloc, hu_tool_t *tools, size_t count) {
    if (!alloc || !tools)
        return;
    for (size_t i = 0; i < count; i++) {
        if (tools[i].vtable && tools[i].vtable->deinit) {
            tools[i].vtable->deinit(tools[i].ctx, alloc);
        }
    }
    alloc->free(alloc->ctx, tools, count * sizeof(hu_tool_t));
}
