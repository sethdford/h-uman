#include "human/tools/factory.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
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
#include "human/tools/delegate.h"
#include "human/tools/diff.h"
#include "human/tools/file_append.h"
#include "human/tools/file_edit.h"
#include "human/tools/file_read.h"
#include "human/tools/file_write.h"
#include "human/tools/git.h"
#ifdef HU_HAS_PERIPHERALS
#include "human/tools/hardware_info.h"
#include "human/tools/hardware_memory.h"
#include "human/tools/i2c.h"
#include "human/tools/spi.h"
#endif
#include "human/tools/analytics.h"
#include "human/tools/broadcast.h"
#include "human/tools/calendar_tool.h"
#include "human/tools/crm.h"
#include "human/tools/facebook.h"
#include "human/tools/firebase.h"
#include "human/tools/gcloud.h"
#include "human/tools/homeassistant.h"
#include "human/tools/http_request.h"
#include "human/tools/image.h"
#include "human/tools/instagram.h"
#include "human/tools/invoice.h"
#include "human/tools/jira.h"
#include "human/tools/memory_forget.h"
#include "human/tools/memory_list.h"
#include "human/tools/memory_recall.h"
#include "human/tools/memory_store.h"
#include "human/tools/save_for_later.h"
#include "human/tools/message.h"
#include "human/tools/pdf.h"
#include "human/tools/pushover.h"
#include "human/tools/report.h"
#include "human/tools/social.h"
#include "human/tools/spreadsheet.h"
#include "human/tools/twitter.h"
#include "human/tools/workflow.h"
#ifdef HU_HAS_CRON
#include "human/tools/schedule.h"
#endif
#ifdef HU_HAS_PERSONA
#include "human/tools/persona.h"
#endif
#ifdef HU_ENABLE_CURL
#include "human/tools/paperclip.h"
#endif
#include "human/tools/schema.h"
#include "human/tools/send_message.h"
#include "human/tools/shell.h"
#include "human/tools/skill_write.h"
#include "human/tools/spawn.h"
#include "human/tools/web_fetch.h"
#include "human/tools/web_search.h"
#include <stdlib.h>
#include <string.h>

#define HU_WEB_FETCH_MAX_CHARS 100000

#ifdef HU_HAS_CRON
#define HU_TOOLS_CRON_COUNT 7
#else
#define HU_TOOLS_CRON_COUNT 0
#endif
#ifdef HU_HAS_PERSONA
#define HU_TOOLS_PERSONA_COUNT 1
#else
#define HU_TOOLS_PERSONA_COUNT 0
#endif
#define HU_TOOLS_COUNT_BASE         \
    (44 + HU_TOOLS_CRON_COUNT - 1 + \
     HU_TOOLS_PERSONA_COUNT) /* 43 base + save_for_later + homeassistant + persona(0|1) + cron */
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
#define HU_TOOLS_HW_COUNT 4
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
                                   hu_mailbox_t *mailbox, hu_tool_t **out_tools,
                                   size_t *out_count) {
    if (!alloc || !out_tools || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
#ifndef HU_HAS_CRON
    (void)cron; /* accepted but ignored when cron is disabled */
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

    err = hu_message_create(alloc, NULL, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

    err = hu_delegate_create(alloc, policy, &tools[idx]);
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

#ifdef HU_HAS_PERSONA
    err = hu_persona_tool_create(alloc, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;
#endif

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

    err = hu_twitter_tool_create(alloc, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;

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

#ifdef HU_ENABLE_CURL
    err = hu_paperclip_tool_create(alloc, &tools[idx]);
    if (err != HU_OK)
        goto fail;
    idx++;
#endif

    /* Load MCP server tools from config when available */
    hu_tool_t *mcp_tools = NULL;
    size_t mcp_count = 0;
    if (config && config->mcp_servers_len > 0) {
        hu_mcp_server_config_t mcp_configs[HU_MCP_SERVERS_MAX];
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
        }
        alloc->free(alloc->ctx, mcp_tools, mcp_count * sizeof(hu_tool_t));
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
