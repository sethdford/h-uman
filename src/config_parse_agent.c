#include "config_internal.h"
#include "config_parse_internal.h"
#include "seaclaw/config.h"
#include "seaclaw/core/string.h"
#include <string.h>

sc_error_t parse_agent(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    if (!obj || obj->type != SC_JSON_OBJECT)
        return SC_OK;
    cfg->agent.llm_compiler_enabled =
        sc_json_get_bool(obj, "llm_compiler", cfg->agent.llm_compiler_enabled);
    cfg->agent.tool_routing_enabled =
        sc_json_get_bool(obj, "tool_routing", cfg->agent.tool_routing_enabled);
    cfg->agent.compact_context =
        sc_json_get_bool(obj, "compact_context", cfg->agent.compact_context);
    double mti = sc_json_get_number(obj, "max_tool_iterations", cfg->agent.max_tool_iterations);
    if (mti >= 0 && mti <= 100000)
        cfg->agent.max_tool_iterations = (uint32_t)mti;
    double mhm = sc_json_get_number(obj, "max_history_messages", cfg->agent.max_history_messages);
    if (mhm >= 0 && mhm <= 10000)
        cfg->agent.max_history_messages = (uint32_t)mhm;
    cfg->agent.parallel_tools = sc_json_get_bool(obj, "parallel_tools", cfg->agent.parallel_tools);
    const char *td = sc_json_get_string(obj, "tool_dispatcher");
    if (td) {
        if (cfg->agent.tool_dispatcher)
            a->free(a->ctx, cfg->agent.tool_dispatcher, strlen(cfg->agent.tool_dispatcher) + 1);
        cfg->agent.tool_dispatcher = sc_strdup(a, td);
    }
    double tl = sc_json_get_number(obj, "token_limit", cfg->agent.token_limit);
    if (tl >= 0 && tl <= 2000000)
        cfg->agent.token_limit = (uint64_t)tl;
    double sids =
        sc_json_get_number(obj, "session_idle_timeout_secs", cfg->agent.session_idle_timeout_secs);
    if (sids >= 0)
        cfg->agent.session_idle_timeout_secs = (uint64_t)sids;
    double ckr =
        sc_json_get_number(obj, "compaction_keep_recent", cfg->agent.compaction_keep_recent);
    if (ckr >= 0 && ckr <= 200)
        cfg->agent.compaction_keep_recent = (uint32_t)ckr;
    double cms = sc_json_get_number(obj, "compaction_max_summary_chars",
                                    cfg->agent.compaction_max_summary_chars);
    if (cms >= 0 && cms <= 50000)
        cfg->agent.compaction_max_summary_chars = (uint32_t)cms;
    double cmx = sc_json_get_number(obj, "compaction_max_source_chars",
                                    cfg->agent.compaction_max_source_chars);
    if (cmx >= 0 && cmx <= 100000)
        cfg->agent.compaction_max_source_chars = (uint32_t)cmx;
    double mts = sc_json_get_number(obj, "message_timeout_secs", cfg->agent.message_timeout_secs);
    if (mts >= 0)
        cfg->agent.message_timeout_secs = (uint64_t)mts;
    double pmc = sc_json_get_number(obj, "pool_max_concurrent", cfg->agent.pool_max_concurrent);
    if (pmc >= 1 && pmc <= 64)
        cfg->agent.pool_max_concurrent = (uint32_t)pmc;
    const char *dp = sc_json_get_string(obj, "default_profile");
    if (dp) {
        if (cfg->agent.default_profile)
            a->free(a->ctx, cfg->agent.default_profile, strlen(cfg->agent.default_profile) + 1);
        cfg->agent.default_profile = sc_strdup(a, dp);
    }
    double cpw = sc_json_get_number(obj, "context_pressure_warn", cfg->agent.context_pressure_warn);
    if (cpw > 0.0 && cpw <= 1.0)
        cfg->agent.context_pressure_warn = (float)cpw;
    double cpc =
        sc_json_get_number(obj, "context_pressure_compact", cfg->agent.context_pressure_compact);
    if (cpc > 0.0 && cpc <= 1.0)
        cfg->agent.context_pressure_compact = (float)cpc;
    double cct =
        sc_json_get_number(obj, "context_compact_target", cfg->agent.context_compact_target);
    if (cct > 0.0 && cct <= 1.0)
        cfg->agent.context_compact_target = (float)cct;
    const char *persona = sc_json_get_string(obj, "persona");
    if (persona) {
        if (cfg->agent.persona)
            a->free(a->ctx, cfg->agent.persona, strlen(cfg->agent.persona) + 1);
        cfg->agent.persona = sc_strdup(a, persona);
    }

    sc_json_value_t *pc_obj = sc_json_object_get(obj, "persona_channels");
    if (pc_obj && pc_obj->type == SC_JSON_OBJECT && pc_obj->data.object.pairs) {
        if (cfg->agent.persona_channels) {
            for (size_t i = 0; i < cfg->agent.persona_channels_count; i++) {
                if (cfg->agent.persona_channels[i].channel)
                    a->free(a->ctx, cfg->agent.persona_channels[i].channel,
                            strlen(cfg->agent.persona_channels[i].channel) + 1);
                if (cfg->agent.persona_channels[i].persona)
                    a->free(a->ctx, cfg->agent.persona_channels[i].persona,
                            strlen(cfg->agent.persona_channels[i].persona) + 1);
            }
            a->free(a->ctx, cfg->agent.persona_channels,
                    cfg->agent.persona_channels_count * sizeof(sc_persona_channel_entry_t));
        }
        size_t n = pc_obj->data.object.len;
        if (n > 0) {
            sc_persona_channel_entry_t *arr = (sc_persona_channel_entry_t *)a->alloc(
                a->ctx, n * sizeof(sc_persona_channel_entry_t));
            if (arr) {
                memset(arr, 0, n * sizeof(sc_persona_channel_entry_t));
                size_t count = 0;
                for (size_t i = 0; i < n && count < n; i++) {
                    sc_json_pair_t *p = &pc_obj->data.object.pairs[i];
                    if (!p->key || !p->value || p->value->type != SC_JSON_STRING)
                        continue;
                    const char *ch_name = p->key;
                    const char *per_name = p->value->data.string.ptr;
                    if (!ch_name || !per_name)
                        continue;
                    arr[count].channel = sc_strdup(a, ch_name);
                    arr[count].persona = sc_strndup(a, per_name, p->value->data.string.len);
                    if (arr[count].channel && arr[count].persona)
                        count++;
                    else {
                        if (arr[count].channel)
                            a->free(a->ctx, arr[count].channel, strlen(arr[count].channel) + 1);
                        if (arr[count].persona)
                            a->free(a->ctx, arr[count].persona, strlen(arr[count].persona) + 1);
                    }
                }
                cfg->agent.persona_channels = arr;
                cfg->agent.persona_channels_count = count;
            }
        }
    }

    sc_json_value_t *ct_obj = sc_json_object_get(obj, "persona_contacts");
    if (ct_obj && ct_obj->type == SC_JSON_OBJECT && ct_obj->data.object.pairs) {
        size_t n = ct_obj->data.object.len;
        if (n > 0) {
            sc_persona_channel_entry_t *arr = (sc_persona_channel_entry_t *)a->alloc(
                a->ctx, n * sizeof(sc_persona_channel_entry_t));
            if (arr) {
                memset(arr, 0, n * sizeof(sc_persona_channel_entry_t));
                size_t count = 0;
                for (size_t i = 0; i < n && count < n; i++) {
                    sc_json_pair_t *p = &ct_obj->data.object.pairs[i];
                    if (!p->key || !p->value || p->value->type != SC_JSON_STRING)
                        continue;
                    arr[count].channel = sc_strdup(a, p->key);
                    arr[count].persona =
                        sc_strndup(a, p->value->data.string.ptr, p->value->data.string.len);
                    if (arr[count].channel && arr[count].persona)
                        count++;
                    else {
                        if (arr[count].channel)
                            a->free(a->ctx, arr[count].channel, strlen(arr[count].channel) + 1);
                        if (arr[count].persona)
                            a->free(a->ctx, arr[count].persona, strlen(arr[count].persona) + 1);
                    }
                }
                cfg->agent.persona_contacts = arr;
                cfg->agent.persona_contacts_count = count;
            }
        }
    }

    return SC_OK;
}
