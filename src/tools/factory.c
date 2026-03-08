#include "seaclaw/tools/factory.h"
#include "seaclaw/config.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#ifdef SC_HAS_CRON
#include "seaclaw/cron.h"
#endif
#include "seaclaw/mcp.h"
#include "seaclaw/security.h"
#include "seaclaw/tools/agent_query.h"
#include "seaclaw/tools/agent_spawn.h"
#include "seaclaw/tools/apply_patch.h"
#ifdef SC_HAS_TOOLS_BROWSER
#include "seaclaw/tools/browser.h"
#include "seaclaw/tools/browser_open.h"
#include "seaclaw/tools/screenshot.h"
#endif
#ifdef SC_HAS_TOOLS_ADVANCED
#include "seaclaw/tools/canvas.h"
#include "seaclaw/tools/claude_code.h"
#include "seaclaw/tools/composio.h"
#include "seaclaw/tools/database.h"
#include "seaclaw/tools/notebook.h"
#endif
#ifdef SC_HAS_CRON
#include "seaclaw/tools/cron_add.h"
#include "seaclaw/tools/cron_list.h"
#include "seaclaw/tools/cron_remove.h"
#include "seaclaw/tools/cron_run.h"
#include "seaclaw/tools/cron_runs.h"
#include "seaclaw/tools/cron_update.h"
#endif
#include "seaclaw/tools/delegate.h"
#include "seaclaw/tools/diff.h"
#include "seaclaw/tools/file_append.h"
#include "seaclaw/tools/file_edit.h"
#include "seaclaw/tools/file_read.h"
#include "seaclaw/tools/file_write.h"
#include "seaclaw/tools/git.h"
#ifdef SC_HAS_PERIPHERALS
#include "seaclaw/tools/hardware_info.h"
#include "seaclaw/tools/hardware_memory.h"
#include "seaclaw/tools/i2c.h"
#include "seaclaw/tools/spi.h"
#endif
#include "seaclaw/tools/analytics.h"
#include "seaclaw/tools/broadcast.h"
#include "seaclaw/tools/calendar_tool.h"
#include "seaclaw/tools/crm.h"
#include "seaclaw/tools/facebook.h"
#include "seaclaw/tools/firebase.h"
#include "seaclaw/tools/gcloud.h"
#include "seaclaw/tools/homeassistant.h"
#include "seaclaw/tools/http_request.h"
#include "seaclaw/tools/image.h"
#include "seaclaw/tools/instagram.h"
#include "seaclaw/tools/invoice.h"
#include "seaclaw/tools/jira.h"
#include "seaclaw/tools/memory_forget.h"
#include "seaclaw/tools/memory_list.h"
#include "seaclaw/tools/memory_recall.h"
#include "seaclaw/tools/memory_store.h"
#include "seaclaw/tools/message.h"
#include "seaclaw/tools/pdf.h"
#include "seaclaw/tools/pushover.h"
#include "seaclaw/tools/report.h"
#include "seaclaw/tools/social.h"
#include "seaclaw/tools/spreadsheet.h"
#include "seaclaw/tools/twitter.h"
#include "seaclaw/tools/workflow.h"
#ifdef SC_HAS_CRON
#include "seaclaw/tools/schedule.h"
#endif
#ifdef SC_HAS_PERSONA
#include "seaclaw/tools/persona.h"
#endif
#ifdef SC_ENABLE_CURL
#include "seaclaw/tools/paperclip.h"
#endif
#include "seaclaw/tools/schema.h"
#include "seaclaw/tools/send_message.h"
#include "seaclaw/tools/shell.h"
#include "seaclaw/tools/skill_write.h"
#include "seaclaw/tools/spawn.h"
#include "seaclaw/tools/web_fetch.h"
#include "seaclaw/tools/web_search.h"
#include <stdlib.h>
#include <string.h>

#ifdef SC_HAS_CRON
#define SC_TOOLS_CRON_COUNT 7
#else
#define SC_TOOLS_CRON_COUNT 0
#endif
#ifdef SC_HAS_PERSONA
#define SC_TOOLS_PERSONA_COUNT 1
#else
#define SC_TOOLS_PERSONA_COUNT 0
#endif
#define SC_TOOLS_COUNT_BASE         \
    (43 + SC_TOOLS_CRON_COUNT - 1 + \
     SC_TOOLS_PERSONA_COUNT) /* 42 base + homeassistant + persona(0|1) + cron */
#ifdef SC_HAS_TOOLS_BROWSER
#define SC_TOOLS_BROWSER_COUNT 3
#else
#define SC_TOOLS_BROWSER_COUNT 0
#endif
#ifdef SC_HAS_TOOLS_ADVANCED
#define SC_TOOLS_ADVANCED_COUNT 5
#else
#define SC_TOOLS_ADVANCED_COUNT 0
#endif
#ifdef SC_HAS_PERIPHERALS
#define SC_TOOLS_HW_COUNT 4
#else
#define SC_TOOLS_HW_COUNT 0
#endif
#ifdef SC_ENABLE_CURL
#define SC_TOOLS_PAPERCLIP_COUNT 1
#else
#define SC_TOOLS_PAPERCLIP_COUNT 0
#endif
#define SC_TOOLS_COUNT                                                                             \
    (SC_TOOLS_COUNT_BASE + SC_TOOLS_BROWSER_COUNT + SC_TOOLS_ADVANCED_COUNT + SC_TOOLS_HW_COUNT +  \
     SC_TOOLS_PAPERCLIP_COUNT)

static sc_error_t add_tool_ws(sc_allocator_t *alloc, sc_tool_t *tools, size_t *idx, const char *ws,
                              size_t ws_len, sc_security_policy_t *policy,
                              sc_error_t (*create)(sc_allocator_t *, const char *, size_t,
                                                   sc_security_policy_t *, sc_tool_t *)) {
    sc_error_t err = create(alloc, ws ? ws : ".", ws_len ? ws_len : 1, policy, &tools[*idx]);
    if (err != SC_OK)
        return err;
    (*idx)++;
    return SC_OK;
}

sc_error_t sc_tools_create_default(sc_allocator_t *alloc, const char *workspace_dir,
                                   size_t workspace_dir_len, sc_security_policy_t *policy,
                                   const sc_config_t *config, sc_memory_t *memory,
                                   sc_cron_scheduler_t *cron, sc_agent_pool_t *agent_pool,
                                   sc_mailbox_t *mailbox, sc_tool_t **out_tools,
                                   size_t *out_count) {
    if (!alloc || !out_tools || !out_count)
        return SC_ERR_INVALID_ARGUMENT;
#ifndef SC_HAS_CRON
    (void)cron; /* accepted but ignored when cron is disabled */
#endif

    size_t tools_alloc = SC_TOOLS_COUNT * sizeof(sc_tool_t);
    sc_tool_t *tools = (sc_tool_t *)alloc->alloc(alloc->ctx, tools_alloc);
    if (!tools)
        return SC_ERR_OUT_OF_MEMORY;
    memset(tools, 0, tools_alloc);

    size_t idx = 0;
    sc_error_t err;

    err =
        add_tool_ws(alloc, tools, &idx, workspace_dir, workspace_dir_len, policy, sc_shell_create);
    if (err != SC_OK)
        goto fail;

    err = add_tool_ws(alloc, tools, &idx, workspace_dir, workspace_dir_len, policy,
                      sc_file_read_create);
    if (err != SC_OK)
        goto fail;

    err = add_tool_ws(alloc, tools, &idx, workspace_dir, workspace_dir_len, policy,
                      sc_file_write_create);
    if (err != SC_OK)
        goto fail;

    err = add_tool_ws(alloc, tools, &idx, workspace_dir, workspace_dir_len, policy,
                      sc_file_edit_create);
    if (err != SC_OK)
        goto fail;

    err = add_tool_ws(alloc, tools, &idx, workspace_dir, workspace_dir_len, policy,
                      sc_file_append_create);
    if (err != SC_OK)
        goto fail;

    err = add_tool_ws(alloc, tools, &idx, workspace_dir, workspace_dir_len, policy, sc_git_create);
    if (err != SC_OK)
        goto fail;

    err = sc_web_search_create(alloc, config, NULL, 0, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err = sc_web_fetch_create(alloc, 100000, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err = sc_http_request_create(alloc, false, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

#ifdef SC_HAS_TOOLS_BROWSER
    err = sc_browser_create(alloc, false, policy, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err = sc_screenshot_create(alloc, false, policy, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;
#endif

    err = sc_image_create(alloc, NULL, 0, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err = sc_memory_store_create(alloc, memory, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err = sc_memory_recall_create(alloc, memory, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err = sc_memory_list_create(alloc, memory, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err = sc_memory_forget_create(alloc, memory, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err = sc_message_create(alloc, NULL, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err = sc_delegate_create(alloc, policy, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err =
        add_tool_ws(alloc, tools, &idx, workspace_dir, workspace_dir_len, policy, sc_spawn_create);
    if (err != SC_OK)
        goto fail;

#ifdef SC_HAS_CRON
    err = sc_cron_add_create(alloc, cron, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err = sc_cron_list_create(alloc, cron, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err = sc_cron_remove_create(alloc, cron, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err = sc_cron_run_create(alloc, cron, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err = sc_cron_runs_create(alloc, cron, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err = sc_cron_update_create(alloc, cron, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;
#endif

#ifdef SC_HAS_TOOLS_BROWSER
    {
        const char *domains[] = {"example.com"};
        err = sc_browser_open_create(alloc, domains, 1, policy, &tools[idx]);
    }
    if (err != SC_OK)
        goto fail;
    idx++;
#endif

#ifdef SC_HAS_TOOLS_ADVANCED
    err = sc_composio_create(alloc, NULL, 0, "default", 7, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;
#endif

#ifdef SC_HAS_PERIPHERALS
    err = sc_hardware_memory_create(alloc, NULL, 0, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;
#endif

#ifdef SC_HAS_CRON
    err = sc_schedule_create(alloc, cron, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;
#endif

    err = sc_schema_create(alloc, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err = sc_pushover_create(alloc, NULL, 0, NULL, 0, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

#ifdef SC_HAS_PERIPHERALS
    err = sc_hardware_info_create(alloc, false, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err = sc_i2c_create(alloc, NULL, 0, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err = sc_spi_create(alloc, NULL, 0, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;
#endif

#ifdef SC_HAS_TOOLS_ADVANCED
    err = sc_claude_code_create(alloc, policy, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err = sc_database_tool_create(alloc, NULL, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err = sc_notebook_create(alloc, policy, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err = sc_canvas_create(alloc, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;
#endif

    err = add_tool_ws(alloc, tools, &idx, workspace_dir, workspace_dir_len, policy,
                      sc_diff_tool_create);
    if (err != SC_OK)
        goto fail;

    err = sc_apply_patch_create(alloc, policy, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err = sc_agent_query_tool_create(alloc, agent_pool, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err = sc_agent_spawn_tool_create(alloc, agent_pool, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

#ifdef SC_HAS_PERSONA
    err = sc_persona_tool_create(alloc, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;
#endif

    err = sc_send_message_create(alloc, mailbox, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err = sc_pdf_create(alloc, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err = sc_spreadsheet_create(alloc, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err = sc_report_create(alloc, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err = sc_broadcast_create(alloc, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err = sc_calendar_create(alloc, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err = sc_homeassistant_create(alloc, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err = sc_skill_write_create(alloc, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err = sc_jira_create(alloc, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err = sc_social_create(alloc, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err = sc_facebook_tool_create(alloc, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err = sc_instagram_tool_create(alloc, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err = sc_twitter_tool_create(alloc, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err = sc_gcloud_create(alloc, policy, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err = sc_firebase_create(alloc, policy, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err = sc_crm_create(alloc, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err = sc_analytics_create(alloc, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err = sc_invoice_create(alloc, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

    err = sc_workflow_create(alloc, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;

#ifdef SC_ENABLE_CURL
    err = sc_paperclip_tool_create(alloc, &tools[idx]);
    if (err != SC_OK)
        goto fail;
    idx++;
#endif

    /* Load MCP server tools from config when available */
    sc_tool_t *mcp_tools = NULL;
    size_t mcp_count = 0;
    if (config && config->mcp_servers_len > 0) {
        sc_mcp_server_config_t mcp_configs[SC_MCP_SERVERS_MAX];
        for (size_t i = 0; i < config->mcp_servers_len && i < SC_MCP_SERVERS_MAX; i++) {
            mcp_configs[i].command = config->mcp_servers[i].command;
            mcp_configs[i].args = (const char **)config->mcp_servers[i].args;
            mcp_configs[i].args_count = config->mcp_servers[i].args_count;
        }
        sc_mcp_init_tools(alloc, mcp_configs, config->mcp_servers_len, &mcp_tools, &mcp_count);
    }

    if (mcp_count > 0 && mcp_tools) {
        size_t new_alloc = (idx + mcp_count) * sizeof(sc_tool_t);
        sc_tool_t *merged = (sc_tool_t *)alloc->realloc(alloc->ctx, tools, tools_alloc, new_alloc);
        if (merged) {
            tools = merged;
            tools_alloc = new_alloc;
            memcpy(&tools[idx], mcp_tools, mcp_count * sizeof(sc_tool_t));
            idx += mcp_count;
        }
        alloc->free(alloc->ctx, mcp_tools, mcp_count * sizeof(sc_tool_t));
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
                    memmove(&tools[i], &tools[i + 1], (idx - i - 1) * sizeof(sc_tool_t));
                idx--;
                i--;
            }
        }
    }

    /* Shrink to exact size so destroy_default can free with count * sizeof */
    if (idx * sizeof(sc_tool_t) < tools_alloc) {
        size_t new_size = idx * sizeof(sc_tool_t);
        sc_tool_t *shrunk = (sc_tool_t *)alloc->realloc(alloc->ctx, tools, tools_alloc, new_size);
        if (shrunk) {
            tools = shrunk;
            tools_alloc = new_size;
        }
    }

    *out_tools = tools;
    *out_count = idx;
    return SC_OK;

fail:
    for (size_t i = 0; i < idx; i++) {
        if (tools[i].vtable && tools[i].vtable->deinit)
            tools[i].vtable->deinit(tools[i].ctx, alloc);
    }
    alloc->free(alloc->ctx, tools, tools_alloc);
    return err;
}

void sc_tools_destroy_default(sc_allocator_t *alloc, sc_tool_t *tools, size_t count) {
    if (!alloc || !tools)
        return;
    for (size_t i = 0; i < count; i++) {
        if (tools[i].vtable && tools[i].vtable->deinit) {
            tools[i].vtable->deinit(tools[i].ctx, alloc);
        }
    }
    alloc->free(alloc->ctx, tools, count * sizeof(sc_tool_t));
}
