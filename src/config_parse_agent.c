#include "config_internal.h"
#include "config_parse_internal.h"
#include "human/config.h"
#include "human/core/string.h"
#include <stdio.h>
#include <string.h>

hu_error_t parse_agent(hu_allocator_t *a, hu_config_t *cfg, const hu_json_value_t *obj) {
    if (!obj || obj->type != HU_JSON_OBJECT)
        return HU_OK;
    cfg->agent.llm_compiler_enabled =
        hu_json_get_bool(obj, "llm_compiler", cfg->agent.llm_compiler_enabled);
    cfg->agent.hula_enabled = hu_json_get_bool(obj, "hula", cfg->agent.hula_enabled);
    cfg->agent.mcts_planner_enabled =
        hu_json_get_bool(obj, "mcts_planner", cfg->agent.mcts_planner_enabled);
    cfg->agent.tree_of_thought =
        hu_json_get_bool(obj, "tree_of_thought", cfg->agent.tree_of_thought);
    cfg->agent.constitutional_ai =
        hu_json_get_bool(obj, "constitutional_ai", cfg->agent.constitutional_ai);
    /* US-7.9: pure string-pattern style self-critique (separate from LLM-judge above). */
    cfg->agent.constitutional_style_rules_enabled = hu_json_get_bool(
        obj, "constitutional_style_rules_enabled", cfg->agent.constitutional_style_rules_enabled);
    cfg->agent.speculative_cache =
        hu_json_get_bool(obj, "speculative_cache", cfg->agent.speculative_cache);
    cfg->agent.tool_routing_enabled =
        hu_json_get_bool(obj, "tool_routing", cfg->agent.tool_routing_enabled);
    cfg->agent.multi_agent = hu_json_get_bool(obj, "multi_agent", cfg->agent.multi_agent);
    cfg->agent.compact_context =
        hu_json_get_bool(obj, "compact_context", cfg->agent.compact_context);
    double mti = hu_json_get_number(obj, "max_tool_iterations", cfg->agent.max_tool_iterations);
    if (mti >= 0 && mti <= 100000)
        cfg->agent.max_tool_iterations = (uint32_t)mti;
    double mhm = hu_json_get_number(obj, "max_history_messages", cfg->agent.max_history_messages);
    if (mhm >= 0 && mhm <= 10000)
        cfg->agent.max_history_messages = (uint32_t)mhm;
    cfg->agent.parallel_tools = hu_json_get_bool(obj, "parallel_tools", cfg->agent.parallel_tools);
    const char *td = hu_json_get_string(obj, "tool_dispatcher");
    if (td) {
        if (cfg->agent.tool_dispatcher)
            a->free(a->ctx, cfg->agent.tool_dispatcher, strlen(cfg->agent.tool_dispatcher) + 1);
        cfg->agent.tool_dispatcher = hu_strdup(a, td);
    }
    double tl = hu_json_get_number(obj, "token_limit", cfg->agent.token_limit);
    if (tl >= 0 && tl <= 2000000)
        cfg->agent.token_limit = (uint64_t)tl;
    double sids =
        hu_json_get_number(obj, "session_idle_timeout_secs", cfg->agent.session_idle_timeout_secs);
    if (sids >= 0)
        cfg->agent.session_idle_timeout_secs = (uint64_t)sids;
    double ckr =
        hu_json_get_number(obj, "compaction_keep_recent", cfg->agent.compaction_keep_recent);
    if (ckr >= 0 && ckr <= 200)
        cfg->agent.compaction_keep_recent = (uint32_t)ckr;
    double cms = hu_json_get_number(obj, "compaction_max_summary_chars",
                                    cfg->agent.compaction_max_summary_chars);
    if (cms >= 0 && cms <= 50000)
        cfg->agent.compaction_max_summary_chars = (uint32_t)cms;
    double cmx = hu_json_get_number(obj, "compaction_max_source_chars",
                                    cfg->agent.compaction_max_source_chars);
    if (cmx >= 0 && cmx <= 100000)
        cfg->agent.compaction_max_source_chars = (uint32_t)cmx;
    double mts = hu_json_get_number(obj, "message_timeout_secs", cfg->agent.message_timeout_secs);
    if (mts >= 0)
        cfg->agent.message_timeout_secs = (uint64_t)mts;
    double pmc = hu_json_get_number(obj, "pool_max_concurrent", cfg->agent.pool_max_concurrent);
    if (pmc >= 1 && pmc <= 64)
        cfg->agent.pool_max_concurrent = (uint32_t)pmc;
    double fmd = hu_json_get_number(obj, "fleet_max_spawn_depth", cfg->agent.fleet_max_spawn_depth);
    if (fmd >= 0 && fmd <= 256)
        cfg->agent.fleet_max_spawn_depth = (uint32_t)fmd;
    double fmt =
        hu_json_get_number(obj, "fleet_max_total_spawns", cfg->agent.fleet_max_total_spawns);
    if (fmt >= 0 && fmt <= 100000000)
        cfg->agent.fleet_max_total_spawns = (uint32_t)fmt;
    double fbu = hu_json_get_number(obj, "fleet_budget_usd", cfg->agent.fleet_budget_usd);
    if (fbu >= 0.0 && fbu <= 1.0e9)
        cfg->agent.fleet_budget_usd = fbu;
    const char *dp = hu_json_get_string(obj, "default_profile");
    if (dp) {
        if (cfg->agent.default_profile)
            a->free(a->ctx, cfg->agent.default_profile, strlen(cfg->agent.default_profile) + 1);
        cfg->agent.default_profile = hu_strdup(a, dp);
    }
    double cpw = hu_json_get_number(obj, "context_pressure_warn", cfg->agent.context_pressure_warn);
    if (cpw > 0.0 && cpw <= 1.0)
        cfg->agent.context_pressure_warn = (float)cpw;
    double cpc =
        hu_json_get_number(obj, "context_pressure_compact", cfg->agent.context_pressure_compact);
    if (cpc > 0.0 && cpc <= 1.0)
        cfg->agent.context_pressure_compact = (float)cpc;
    double cct =
        hu_json_get_number(obj, "context_compact_target", cfg->agent.context_compact_target);
    if (cct > 0.0 && cct <= 1.0)
        cfg->agent.context_compact_target = (float)cct;

    const char *srm = hu_json_get_string(obj, "self_rag_mode");
    if (srm) {
        if (cfg->agent.self_rag_mode)
            a->free(a->ctx, cfg->agent.self_rag_mode, strlen(cfg->agent.self_rag_mode) + 1);
        cfg->agent.self_rag_mode = hu_strdup(a, srm);
    }
    cfg->agent.self_rag_streaming =
        hu_json_get_bool(obj, "self_rag_streaming", cfg->agent.self_rag_streaming);

    hu_json_value_t *mc_obj = hu_json_object_get(obj, "metacognition");
    if (mc_obj && mc_obj->type == HU_JSON_OBJECT) {
        hu_metacog_settings_t *m = &cfg->agent.metacognition;
        m->enabled = hu_json_get_bool(mc_obj, "enabled", m->enabled);
        double x;
        x = hu_json_get_number(mc_obj, "confidence_threshold", m->confidence_threshold);
        if (x >= 0.0 && x <= 1.0)
            m->confidence_threshold = (float)x;
        x = hu_json_get_number(mc_obj, "coherence_threshold", m->coherence_threshold);
        if (x >= 0.0 && x <= 1.0)
            m->coherence_threshold = (float)x;
        x = hu_json_get_number(mc_obj, "repetition_threshold", m->repetition_threshold);
        if (x >= 0.0 && x <= 1.0)
            m->repetition_threshold = (float)x;
        double mr = hu_json_get_number(mc_obj, "max_reflects", m->max_reflects);
        if (mr >= 0.0 && mr <= 100.0)
            m->max_reflects = (uint32_t)mr;
        double mg = hu_json_get_number(mc_obj, "max_regen", m->max_regen);
        if (mg >= 0.0 && mg <= 32.0)
            m->max_regen = (uint32_t)mg;
        double hm = hu_json_get_number(mc_obj, "hysteresis_min", m->hysteresis_min);
        if (hm >= 1.0 && hm <= 20.0)
            m->hysteresis_min = (uint32_t)hm;
        m->use_calibrated_risk =
            hu_json_get_bool(mc_obj, "use_calibrated_risk", m->use_calibrated_risk);
        x = hu_json_get_number(mc_obj, "risk_high_threshold", m->risk_high_threshold);
        if (x >= 0.0 && x <= 1.0)
            m->risk_high_threshold = (float)x;
        x = hu_json_get_number(mc_obj, "w_low_confidence", m->w_low_confidence);
        if (x >= 0.0 && x <= 10.0)
            m->w_low_confidence = (float)x;
        x = hu_json_get_number(mc_obj, "w_low_coherence", m->w_low_coherence);
        if (x >= 0.0 && x <= 10.0)
            m->w_low_coherence = (float)x;
        x = hu_json_get_number(mc_obj, "w_repetition", m->w_repetition);
        if (x >= 0.0 && x <= 10.0)
            m->w_repetition = (float)x;
        x = hu_json_get_number(mc_obj, "w_stuck", m->w_stuck);
        if (x >= 0.0 && x <= 10.0)
            m->w_stuck = (float)x;
        x = hu_json_get_number(mc_obj, "w_low_satisfaction", m->w_low_satisfaction);
        if (x >= 0.0 && x <= 10.0)
            m->w_low_satisfaction = (float)x;
        x = hu_json_get_number(mc_obj, "w_low_trajectory", m->w_low_trajectory);
        if (x >= 0.0 && x <= 10.0)
            m->w_low_trajectory = (float)x;
    }

    /* Claude Code feature integration fields */
    double pl = hu_json_get_number(obj, "permission_level", cfg->agent.permission_level);
    if (pl >= 0 && pl <= 2)
        cfg->agent.permission_level = (uint8_t)pl;
    cfg->agent.session_auto_save =
        hu_json_get_bool(obj, "session_auto_save", cfg->agent.session_auto_save);
    const char *sd = hu_json_get_string(obj, "session_dir");
    if (sd) {
        if (cfg->agent.session_dir)
            a->free(a->ctx, cfg->agent.session_dir, strlen(cfg->agent.session_dir) + 1);
        cfg->agent.session_dir = hu_strdup(a, sd);
    }
    cfg->agent.discover_instructions =
        hu_json_get_bool(obj, "discover_instructions", cfg->agent.discover_instructions);
    cfg->agent.compaction_use_structured =
        hu_json_get_bool(obj, "compaction_use_structured", cfg->agent.compaction_use_structured);

    const char *persona = hu_json_get_string(obj, "persona");
    if (persona) {
        if (cfg->agent.persona)
            a->free(a->ctx, cfg->agent.persona, strlen(cfg->agent.persona) + 1);
        cfg->agent.persona = hu_strdup(a, persona);
    }

    hu_json_value_t *pc_obj = hu_json_object_get(obj, "persona_channels");
    if (pc_obj && pc_obj->type == HU_JSON_OBJECT && pc_obj->data.object.pairs) {
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
                    cfg->agent.persona_channels_count * sizeof(hu_persona_channel_entry_t));
        }
        size_t n = pc_obj->data.object.len;
        if (n > 0) {
            hu_persona_channel_entry_t *arr = (hu_persona_channel_entry_t *)a->alloc(
                a->ctx, n * sizeof(hu_persona_channel_entry_t));
            if (arr) {
                memset(arr, 0, n * sizeof(hu_persona_channel_entry_t));
                size_t count = 0;
                for (size_t i = 0; i < n && count < n; i++) {
                    hu_json_pair_t *p = &pc_obj->data.object.pairs[i];
                    if (!p->key || !p->value || p->value->type != HU_JSON_STRING)
                        continue;
                    const char *ch_name = p->key;
                    const char *per_name = p->value->data.string.ptr;
                    if (!ch_name || !per_name)
                        continue;
                    arr[count].channel = hu_strdup(a, ch_name);
                    arr[count].persona = hu_strndup(a, per_name, p->value->data.string.len);
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

    /* Constitutional AI configurable principles */
    hu_json_value_t *cp_arr = hu_json_object_get(obj, "constitutional_principles");
    if (cp_arr && cp_arr->type == HU_JSON_ARRAY && cp_arr->data.array.len > 0) {
        if (cfg->agent.constitutional_principles)
            a->free(a->ctx, cfg->agent.constitutional_principles,
                    strlen(cfg->agent.constitutional_principles) + 1);
        size_t buf_cap = 512;
        char *buf = (char *)a->alloc(a->ctx, buf_cap);
        if (buf) {
            size_t pos = 0;
            buf[0] = '\0';
            for (size_t i = 0; i < cp_arr->data.array.len && i < 16; i++) {
                hu_json_value_t *item = cp_arr->data.array.items[i];
                if (!item || item->type != HU_JSON_STRING)
                    continue;
                const char *principle = item->data.string.ptr;
                size_t plen = item->data.string.len;
                size_t needed = pos + plen + 8;
                if (needed >= buf_cap) {
                    size_t nc = buf_cap * 2;
                    while (nc < needed)
                        nc *= 2;
                    char *nb = (char *)a->realloc(a->ctx, buf, buf_cap, nc);
                    if (!nb)
                        break;
                    buf = nb;
                    buf_cap = nc;
                }
                int n = snprintf(buf + pos, buf_cap - pos, "- %.*s\n", (int)plen, principle);
                if (n > 0 && (size_t)n < buf_cap - pos)
                    pos += (size_t)n;
            }
            buf[pos] = '\0';
            cfg->agent.constitutional_principles = buf;
        }
    }

    hu_json_value_t *ct_obj = hu_json_object_get(obj, "persona_contacts");
    if (ct_obj) {
        /* Free previous persona_contacts to prevent leak on config reload.
         * Clears even if the new value is non-object (malformed config). */
        if (cfg->agent.persona_contacts && cfg->agent.persona_contacts_count > 0) {
            for (size_t i = 0; i < cfg->agent.persona_contacts_count; i++) {
                if (cfg->agent.persona_contacts[i].channel)
                    a->free(a->ctx, cfg->agent.persona_contacts[i].channel,
                            strlen(cfg->agent.persona_contacts[i].channel) + 1);
                if (cfg->agent.persona_contacts[i].persona)
                    a->free(a->ctx, cfg->agent.persona_contacts[i].persona,
                            strlen(cfg->agent.persona_contacts[i].persona) + 1);
            }
            a->free(a->ctx, cfg->agent.persona_contacts,
                    cfg->agent.persona_contacts_count * sizeof(hu_persona_channel_entry_t));
            cfg->agent.persona_contacts = NULL;
            cfg->agent.persona_contacts_count = 0;
        }
        if (ct_obj->type == HU_JSON_OBJECT) {
            size_t n = ct_obj->data.object.pairs ? ct_obj->data.object.len : 0;
            if (n > 0) {
                hu_persona_channel_entry_t *arr = (hu_persona_channel_entry_t *)a->alloc(
                    a->ctx, n * sizeof(hu_persona_channel_entry_t));
                if (arr) {
                    memset(arr, 0, n * sizeof(hu_persona_channel_entry_t));
                    size_t count = 0;
                    for (size_t i = 0; i < n && count < n; i++) {
                        hu_json_pair_t *p = &ct_obj->data.object.pairs[i];
                        if (!p->key || !p->value || p->value->type != HU_JSON_STRING)
                            continue;
                        arr[count].channel = hu_strdup(a, p->key);
                        arr[count].persona =
                            hu_strndup(a, p->value->data.string.ptr, p->value->data.string.len);
                        if (arr[count].channel && arr[count].persona)
                            count++;
                        else {
                            if (arr[count].channel)
                                a->free(a->ctx, arr[count].channel, strlen(arr[count].channel) + 1);
                            if (arr[count].persona)
                                a->free(a->ctx, arr[count].persona, strlen(arr[count].persona) + 1);
                        }
                    }
                    if (count > 0) {
                        cfg->agent.persona_contacts = arr;
                        cfg->agent.persona_contacts_count = count;
                    } else {
                        a->free(a->ctx, arr, n * sizeof(hu_persona_channel_entry_t));
                    }
                }
            }
        }
    }

    cfg->agent.prompt_cache_enabled =
        hu_json_get_bool(obj, "prompt_cache", cfg->agent.prompt_cache_enabled);
    cfg->agent.agent_comm_enabled =
        hu_json_get_bool(obj, "agent_comm", cfg->agent.agent_comm_enabled);
    const char *ce_type = hu_json_get_string(obj, "context_engine");
    if (ce_type) {
        if (cfg->agent.context_engine_type)
            a->free(a->ctx, cfg->agent.context_engine_type,
                    strlen(cfg->agent.context_engine_type) + 1);
        cfg->agent.context_engine_type = hu_strdup(a, ce_type);
    }

    hu_json_value_t *bon = hu_json_object_get(obj, "best_of_n");
    if (bon && bon->type == HU_JSON_NUMBER) {
        double bv = bon->data.number;
        if (bv >= 0.0 && bv <= 5.0)
            cfg->agent.best_of_n = (uint32_t)bv;
    }

    /* Default on-device to enabled (overridable by JSON) */
#ifdef __APPLE__
    if (!cfg->agent.mr_on_device_enabled)
        cfg->agent.mr_on_device_enabled = true;
#endif

    hu_json_value_t *mr_obj = hu_json_object_get(obj, "model_router");
    if (mr_obj && mr_obj->type == HU_JSON_OBJECT) {
        const char *mr_ref = hu_json_get_string(mr_obj, "reflexive_model");
        if (mr_ref) {
            if (cfg->agent.mr_reflexive_model)
                a->free(a->ctx, cfg->agent.mr_reflexive_model,
                        strlen(cfg->agent.mr_reflexive_model) + 1);
            cfg->agent.mr_reflexive_model = hu_strdup(a, mr_ref);
        }
        const char *mr_conv = hu_json_get_string(mr_obj, "conversational_model");
        if (mr_conv) {
            if (cfg->agent.mr_conversational_model)
                a->free(a->ctx, cfg->agent.mr_conversational_model,
                        strlen(cfg->agent.mr_conversational_model) + 1);
            cfg->agent.mr_conversational_model = hu_strdup(a, mr_conv);
        }
        const char *mr_ana = hu_json_get_string(mr_obj, "analytical_model");
        if (mr_ana) {
            if (cfg->agent.mr_analytical_model)
                a->free(a->ctx, cfg->agent.mr_analytical_model,
                        strlen(cfg->agent.mr_analytical_model) + 1);
            cfg->agent.mr_analytical_model = hu_strdup(a, mr_ana);
        }
        const char *mr_deep = hu_json_get_string(mr_obj, "deep_model");
        if (mr_deep) {
            if (cfg->agent.mr_deep_model)
                a->free(a->ctx, cfg->agent.mr_deep_model, strlen(cfg->agent.mr_deep_model) + 1);
            cfg->agent.mr_deep_model = hu_strdup(a, mr_deep);
        }
        cfg->agent.mr_judge_enabled =
            hu_json_get_bool(mr_obj, "judge_enabled", cfg->agent.mr_judge_enabled);
        const char *mr_judge_model = hu_json_get_string(mr_obj, "judge_model");
        if (mr_judge_model) {
            if (cfg->agent.mr_judge_model)
                a->free(a->ctx, cfg->agent.mr_judge_model, strlen(cfg->agent.mr_judge_model) + 1);
            cfg->agent.mr_judge_model = hu_strdup(a, mr_judge_model);
        }
        const char *s3_local = hu_json_get_string(mr_obj, "s3_local_model");
        if (s3_local) {
            if (cfg->agent.s3_local_model)
                a->free(a->ctx, cfg->agent.s3_local_model, strlen(cfg->agent.s3_local_model) + 1);
            cfg->agent.s3_local_model = hu_strdup(a, s3_local);
        }
        const char *mr_on_dev = hu_json_get_string(mr_obj, "on_device_model");
        if (mr_on_dev) {
            if (cfg->agent.mr_on_device_model)
                a->free(a->ctx, cfg->agent.mr_on_device_model,
                        strlen(cfg->agent.mr_on_device_model) + 1);
            cfg->agent.mr_on_device_model = hu_strdup(a, mr_on_dev);
        }
        cfg->agent.mr_on_device_enabled =
            hu_json_get_bool(mr_obj, "on_device_enabled", cfg->agent.mr_on_device_enabled);
        /* US-13: provider-degradation fallback model. Parsed under
         * agent.model_router because conceptually the model_router
         * decides what model to use; degradation is the routing
         * decision for "primary failed, what next?". Stored on
         * cfg->agent.fallback_model; wired into
         * agent->sota.degradation_config.fallback_model at agent init
         * (agent.c after the existing degradation_config setup). */
        const char *fallback = hu_json_get_string(mr_obj, "fallback_model");
        if (fallback) {
            if (cfg->agent.fallback_model)
                a->free(a->ctx, cfg->agent.fallback_model, strlen(cfg->agent.fallback_model) + 1);
            cfg->agent.fallback_model = hu_strdup(a, fallback);
        }
    }

    return HU_OK;
}
