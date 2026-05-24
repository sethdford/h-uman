#include "human/agent.h"
#include "agent_internal.h"
#include "human/agent/approval_gate.h"
#include "human/agent/awareness.h"
#include "human/agent/commitment_store.h"
#include "human/agent/humanness.h"
#include "human/agent/idempotency.h"
#include "human/agent/pattern_radar.h"
#include "human/agent/persona_eval.h"
#include "human/agent/response_verifier.h"
#include "human/agent/superhuman.h"
#include "human/agent/superhuman_commitment.h"
#include "human/agent/superhuman_emotional.h"
#include "human/agent/superhuman_predictive.h"
#include "human/agent/superhuman_silence.h"
#include "human/agent/workflow_event.h"
#include "human/agent/world_model_bridge.h"
#include "human/config.h"
#include "human/core/log.h"
#include "human/memory/consolidation.h"
#include "human/memory/promotion.h"
#include "human/memory/tiers.h"
#include "human/webhook.h"
#ifdef HU_ENABLE_SQLITE
#include "human/agent/scheduler.h"
#include "human/cognition/db.h"
#include "human/intelligence/meta_learning.h"
#endif
#include "human/agent/acp_bridge.h"
#include "human/agent/agent_comm.h"
#include "human/agent/commands.h"
#include "human/agent/compaction.h"
#include "human/agent/dispatcher.h"
#include "human/agent/episodic.h"
#include "human/agent/input_guard.h"
#include "human/agent/kv_cache.h"
#include "human/agent/mailbox.h"
#include "human/agent/memory_loader.h"
#include "human/agent/outcomes.h"
#include "human/agent/planner.h"
#include "human/agent/preferences.h"
#include "human/agent/prompt.h"
#include "human/agent/prompt_cache.h"
#include "human/agent/reflection.h"
#include "human/agent/speculative.h"
#include "human/agent/task_list.h"
#include "human/agent/team.h"
#include "human/agent/tool_context.h"
#include "human/agent/undo.h"
#include "human/context_engine.h"
#include "human/persona/circadian.h"
#include "human/persona/relationship.h"
#include "human/tools/cache_ttl.h"
#ifdef HU_HAS_SKILLS
#include "human/skillforge.h"
#endif
#include "human/context.h"
#include "human/context_tokens.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/hook.h"
#include "human/hook_pipeline.h"
#include "human/memory/stm.h"
#include "human/observer.h"
#include "human/persona.h"
#include "human/persona/creative_voice.h"
#include "human/persona/genuine_boundaries.h"
#include "human/persona/narrative_self.h"
#include "human/provider.h"
#include "human/security/arg_inspector.h"
#include "human/voice.h"
#ifdef HU_ENABLE_ML
#include "human/ml/m3_contact_routes.h"
#include "human/ml/m3_frontier_adapter.h"
#include "human/ml/m3_id_map.h"
#include "human/ml/mlx_admin.h"
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

hu_error_t hu_agent_internal_load_persona_eval(hu_agent_t *agent, const char *model_path) {
    /* Sprint 46 R5.3 (refactored for testability per audit FAIL).
     * See agent_internal.h for contract. */
    if (!agent || !agent->alloc)
        return HU_ERR_INVALID_ARGUMENT;
    agent->persona_eval = NULL;
    hu_persona_eval_model_t *m = NULL;
    hu_error_t err = hu_persona_eval_load(agent->alloc, model_path, &m);
    if (err == HU_OK && m) {
        agent->persona_eval = m;
        return HU_OK;
    }
    /* Either err != HU_OK (missing file or parse error) OR m == NULL
     * (defensive — shouldn't happen on HU_OK). Field stays NULL; caller
     * decides severity. */
    return err == HU_OK ? HU_ERR_IO : err;
}

void hu_agent_internal_generate_trace_id(char *buf) {
    static uint32_t counter = 0;
    uint64_t t = (uint64_t)clock();
    counter++;
    snprintf(buf, 37, "%08x-%04x-%04x-%04x-%08x%04x", (uint32_t)(t & 0xFFFFFFFF),
             (uint16_t)((t >> 32) & 0xFFFF), (uint16_t)(0x4000 | (counter & 0x0FFF)),
             (uint16_t)(0x8000 | ((t >> 16) & 0x3FFF)), (uint32_t)(t * 2654435761u),
             (uint16_t)(counter & 0xFFFF));
}

uint64_t hu_agent_internal_clock_diff_ms(clock_t start, clock_t end) {
    return (uint64_t)((end - start) * 1000 / CLOCKS_PER_SEC);
}

static _Thread_local hu_agent_t *hu__current_agent_for_tools;

void hu_agent_set_current_for_tools(hu_agent_t *agent) {
    hu__current_agent_for_tools = agent;
}
void hu_agent_clear_current_for_tools(void) {
    hu__current_agent_for_tools = NULL;
}
hu_agent_t *hu_agent_get_current_for_tools(void) {
    return hu__current_agent_for_tools;
}

/* Pending voice message state — thread-local, set by send_voice_message tool,
 * consumed by daemon after agent turn. */
#define PV_MAX_TRANSCRIPT 4096
#define PV_MAX_EMOTION    64

static _Thread_local bool pv_active;
static _Thread_local char pv_emotion[PV_MAX_EMOTION];
static _Thread_local char pv_transcript[PV_MAX_TRANSCRIPT];
static _Thread_local size_t pv_transcript_len;

void hu_agent_request_voice_send(const char *emotion, const char *transcript,
                                 size_t transcript_len) {
    pv_active = true;
    pv_emotion[0] = '\0';
    pv_transcript[0] = '\0';
    pv_transcript_len = 0;
    if (emotion && emotion[0]) {
        size_t elen = strlen(emotion);
        if (elen >= PV_MAX_EMOTION)
            elen = PV_MAX_EMOTION - 1;
        memcpy(pv_emotion, emotion, elen);
        pv_emotion[elen] = '\0';
    }
    if (transcript && transcript_len > 0) {
        size_t tlen = transcript_len;
        if (tlen >= PV_MAX_TRANSCRIPT)
            tlen = PV_MAX_TRANSCRIPT - 1;
        /* Clamp to UTF-8 boundary */
        while (tlen > 0 && ((unsigned char)transcript[tlen] & 0xC0) == 0x80)
            tlen--;
        memcpy(pv_transcript, transcript, tlen);
        pv_transcript[tlen] = '\0';
        pv_transcript_len = tlen;
    }
}

bool hu_agent_has_pending_voice(void) {
    return pv_active;
}

const char *hu_agent_pending_voice_emotion(void) {
    if (!pv_active || pv_emotion[0] == '\0')
        return NULL;
    return pv_emotion;
}

const char *hu_agent_pending_voice_transcript(size_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (!pv_active || pv_transcript[0] == '\0')
        return NULL;
    if (out_len)
        *out_len = pv_transcript_len;
    return pv_transcript;
}

void hu_agent_clear_pending_voice(void) {
    pv_active = false;
    pv_emotion[0] = '\0';
    pv_transcript[0] = '\0';
    pv_transcript_len = 0;
}

void hu_agent_internal_record_cost(hu_agent_t *agent, const hu_token_usage_t *usage) {
    if (!agent || !usage)
        return;

    /* Record in cost tracker (backward compat) */
    if (agent->cost_tracker && agent->cost_tracker->enabled) {
        hu_cost_entry_t entry;
        memset(&entry, 0, sizeof(entry));
        entry.model = agent->model_name;
        entry.input_tokens = usage->prompt_tokens;
        entry.output_tokens = usage->completion_tokens;
        entry.total_tokens = usage->total_tokens;
        entry.cost_usd = 0.0;
        entry.timestamp_secs = (int64_t)time(NULL);
        hu_error_t cost_err =
            hu_cost_record_usage(agent->cost_tracker, &entry, agent->active_job_id);
        if (cost_err != HU_OK)
            hu_log_error("agent", NULL, "cost tracking failed: %s", hu_error_string(cost_err));
    }

    /* Record in usage tracker (per-provider tracking) */
    if (agent->usage_tracker) {
        hu_extended_token_usage_t tu;
        memset(&tu, 0, sizeof(tu));
        tu.input_tokens = usage->prompt_tokens;
        tu.output_tokens = usage->completion_tokens;
        tu.cache_read_tokens = 0;  /* not yet exposed in provider API */
        tu.cache_write_tokens = 0; /* not yet exposed in provider API */
        hu_error_t usage_err =
            hu_usage_tracker_record(agent->usage_tracker, agent->model_name, &tu);
        if (usage_err != HU_OK)
            hu_log_error("agent", NULL, "usage tracking failed: %s", hu_error_string(usage_err));
    }
}

void hu_agent_internal_set_scene_direction(hu_agent_t *agent, const char *text, size_t text_len) {
    if (!agent)
        return;
    if (!text || text_len == 0) {
        agent->scene_direction_text = NULL;
        agent->scene_direction_text_len = 0;
        return;
    }
    agent->scene_direction_text = text;
    agent->scene_direction_text_len = text_len;
}

void hu_agent_internal_clear_scene_direction(hu_agent_t *agent) {
    if (!agent)
        return;
    agent->scene_direction_text = NULL;
    agent->scene_direction_text_len = 0;
}

/* Sprint 37 — Director history ring buffer.
 *
 * The buffer is a small fixed-size array (HU_DIRECTOR_HISTORY_MAX = 4)
 * of heap-owned NUL-terminated copies. Push:
 *   1. Reject NULL or text shorter than 30 bytes (G6's threshold —
 *      below it, the entry would be useless).
 *   2. Truncate to HU_DIRECTOR_TEXT_CAP bytes if longer.
 *   3. Allocate a NUL-terminated copy.
 *   4. If buffer is full, free the oldest (slot MAX-1).
 *   5. Shift all entries down one slot (slot K → slot K+1).
 *   6. Place the new copy in slot 0.
 *   7. Increment count, saturating at MAX. */
#define HU_DIRECTOR_PUSH_MIN_LEN 30 /* same as HU_GUARD_DIRECTOR_ECHO_MIN_MATCH */

void hu_agent_internal_push_director_history(hu_agent_t *agent, const char *text, size_t text_len) {
    if (!agent || !agent->alloc || !text || text_len < HU_DIRECTOR_PUSH_MIN_LEN)
        return;

    size_t copy_len = text_len < HU_DIRECTOR_TEXT_CAP ? text_len : HU_DIRECTOR_TEXT_CAP;
    char *copy = (char *)agent->alloc->alloc(agent->alloc->ctx, copy_len + 1);
    if (!copy)
        return;
    memcpy(copy, text, copy_len);
    copy[copy_len] = '\0';

    /* Free oldest slot if buffer is full. */
    if (agent->director_history[HU_DIRECTOR_HISTORY_MAX - 1]) {
        agent->alloc->free(agent->alloc->ctx, agent->director_history[HU_DIRECTOR_HISTORY_MAX - 1],
                           agent->director_history_lens[HU_DIRECTOR_HISTORY_MAX - 1] + 1);
        agent->director_history[HU_DIRECTOR_HISTORY_MAX - 1] = NULL;
        agent->director_history_lens[HU_DIRECTOR_HISTORY_MAX - 1] = 0;
    }
    /* Shift entries down (slot K → K+1). */
    for (size_t i = HU_DIRECTOR_HISTORY_MAX - 1; i > 0; i--) {
        agent->director_history[i] = agent->director_history[i - 1];
        agent->director_history_lens[i] = agent->director_history_lens[i - 1];
    }
    /* Place new entry at slot 0 (most recent). */
    agent->director_history[0] = copy;
    agent->director_history_lens[0] = copy_len;
    if (agent->director_history_count < HU_DIRECTOR_HISTORY_MAX)
        agent->director_history_count++;
}

void hu_agent_internal_free_director_history(hu_agent_t *agent) {
    if (!agent || !agent->alloc)
        return;
    for (size_t i = 0; i < HU_DIRECTOR_HISTORY_MAX; i++) {
        if (agent->director_history[i]) {
            agent->alloc->free(agent->alloc->ctx, agent->director_history[i],
                               agent->director_history_lens[i] + 1);
            agent->director_history[i] = NULL;
            agent->director_history_lens[i] = 0;
        }
    }
    agent->director_history_count = 0;
}

void hu_agent_internal_reset_contact_boundary_state(hu_agent_t *agent) {
    if (!agent)
        return;
    hu_agent_internal_clear_scene_direction(agent);
    hu_agent_internal_free_director_history(agent);
}

size_t hu_agent_internal_recent_assistant_avg_len(const hu_agent_t *agent, size_t max_n) {
    if (!agent || !agent->history || agent->history_count == 0 || max_n == 0)
        return 0;

    /* Collect assistant lengths oldest → newest among the last `max_n`
     * turns so we can apply EWMA forward in time. */
    size_t lens[16];
    if (max_n > sizeof(lens) / sizeof(lens[0]))
        max_n = sizeof(lens) / sizeof(lens[0]);

    size_t collected = 0;
    size_t i = agent->history_count;
    while (i > 0 && collected < max_n) {
        i--;
        const hu_owned_message_t *m = &agent->history[i];
        if (m->role != HU_ROLE_ASSISTANT)
            continue;
        if (!m->content || m->content_len == 0)
            continue;
        lens[collected++] = m->content_len;
    }
    if (collected == 0)
        return 0;

    /* Reverse to chronological order (oldest first). */
    for (size_t a = 0, b = collected - 1; a < b; a++, b--) {
        size_t tmp = lens[a];
        lens[a] = lens[b];
        lens[b] = tmp;
    }

    double ewma = (double)lens[0];
    const double alpha = HU_GUARD_ASSISTANT_LEN_EWMA_ALPHA;
    for (size_t k = 1; k < collected; k++)
        ewma = alpha * (double)lens[k] + (1.0 - alpha) * ewma;

    if (ewma < 1.0)
        return 1;
    return (size_t)ewma;
}

#define HU_AGENT_HISTORY_INIT_CAP 16
#define HU_AGENT_MAX_SLASH_LEN    256

hu_error_t hu_agent_from_config(
    hu_agent_t *out, hu_allocator_t *alloc, hu_provider_t provider, const hu_tool_t *tools,
    size_t tools_count, hu_memory_t *memory, hu_session_store_t *session_store,
    hu_observer_t *observer, hu_security_policy_t *policy, const char *model_name,
    size_t model_name_len, const char *default_provider, size_t default_provider_len,
    double temperature, const char *workspace_dir, size_t workspace_dir_len,
    uint32_t max_tool_iterations, uint32_t max_history_messages, bool auto_save,
    uint8_t autonomy_level, const char *custom_instructions, size_t custom_instructions_len,
    const char *persona, size_t persona_len, const hu_agent_context_config_t *ctx_cfg) {
    if (!out || !alloc || !provider.vtable || !model_name) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    hu_personal_model_init(&out->personal_model);

    /* M2 — restore the user-specific model from disk so daemon restarts no
     * longer wipe accumulated facts/topics/style. Best-effort: NOT_FOUND on
     * first run is normal and PARSE on a schema bump leaves the model fresh
     * (the loader re-initialises `out->personal_model` on mismatch). Gated on
     * `auto_save` so unit tests that pass `auto_save=false` get a clean
     * in-memory model and never touch disk. */
    if (auto_save) {
        char pm_path[1024];
        if (hu_personal_model_resolve_default_path(pm_path, sizeof(pm_path))) {
            (void)hu_personal_model_load(&out->personal_model, pm_path);
        }
    }

    out->alloc = alloc;
    out->provider = provider;
    out->timing_model = (hu_timing_model_t *)alloc->alloc(alloc->ctx, sizeof(hu_timing_model_t));
    if (!out->timing_model)
        return HU_ERR_OUT_OF_MEMORY;
    memset(out->timing_model, 0, sizeof(*out->timing_model));
    out->memory = memory;
    out->retrieval_engine = NULL;
    out->session_store = session_store;
    out->observer = observer;
    out->policy = policy;
    out->temperature = temperature;
    out->max_tool_iterations = max_tool_iterations ? max_tool_iterations : 25;
    out->max_history_messages = max_history_messages ? max_history_messages : 50;
    out->auto_save = auto_save;
    out->autonomy_level = autonomy_level;
    out->permission_level = HU_PERM_DANGER_FULL_ACCESS;
    out->permission_base_level = HU_PERM_DANGER_FULL_ACCESS;
    out->reflection.enabled = true;
    out->reflection.use_llm = true;
    out->reflection.max_retries = 2;
    out->reflection.min_response_tokens = 0;
#ifdef HU_ENABLE_SQLITE
    out->meta_params.default_confidence_threshold = 0.5;
    out->meta_params.refinement_frequency_weeks = 4;
    out->meta_params.discovery_min_feedback_count = 3;
#ifdef HU_ENABLE_SKILLS
    if (memory) {
        sqlite3 *db = hu_sqlite_memory_get_db(memory);
        if (db && hu_meta_learning_load(db, &out->meta_params) == HU_OK) {
            /* loaded from DB; defaults above were overwritten */
        }
    }
#endif
#endif
    out->custom_instructions = NULL;
    out->custom_instructions_len = 0;
    if (custom_instructions && custom_instructions_len > 0) {
        out->custom_instructions = hu_strndup(alloc, custom_instructions, custom_instructions_len);
        if (!out->custom_instructions) {
            alloc->free(alloc->ctx, out->timing_model, sizeof(hu_timing_model_t));
            return HU_ERR_OUT_OF_MEMORY;
        }
        out->custom_instructions_len = custom_instructions_len;
    }

    out->model_name = hu_strndup(alloc, model_name, model_name_len);
    if (!out->model_name) {
        if (out->custom_instructions)
            alloc->free(alloc->ctx, out->custom_instructions, out->custom_instructions_len + 1);
        alloc->free(alloc->ctx, out->timing_model, sizeof(hu_timing_model_t));
        return HU_ERR_OUT_OF_MEMORY;
    }
    out->model_name_len = model_name_len;

    out->default_provider =
        hu_strndup(alloc, default_provider ? default_provider : "openai",
                   default_provider_len ? default_provider_len : strlen("openai"));
    if (!out->default_provider) {
        alloc->free(alloc->ctx, out->model_name, out->model_name_len + 1);
        if (out->custom_instructions)
            alloc->free(alloc->ctx, out->custom_instructions, out->custom_instructions_len + 1);
        alloc->free(alloc->ctx, out->timing_model, sizeof(hu_timing_model_t));
        return HU_ERR_OUT_OF_MEMORY;
    }
    out->default_provider_len = default_provider_len ? default_provider_len : strlen("openai");

    out->workspace_dir = hu_strndup(alloc, workspace_dir ? workspace_dir : ".",
                                    workspace_dir_len ? workspace_dir_len : 1);
    if (!out->workspace_dir) {
        alloc->free(alloc->ctx, out->default_provider, out->default_provider_len + 1);
        alloc->free(alloc->ctx, out->model_name, out->model_name_len + 1);
        if (out->custom_instructions)
            alloc->free(alloc->ctx, out->custom_instructions, out->custom_instructions_len + 1);
        alloc->free(alloc->ctx, out->timing_model, sizeof(hu_timing_model_t));
        return HU_ERR_OUT_OF_MEMORY;
    }
    out->workspace_dir_len = workspace_dir_len ? workspace_dir_len : 1;

    out->token_limit = 0;
    out->context_pressure_warn = 0.85f;
    out->context_pressure_compact = 0.95f;
    out->context_compact_target = 0.70f;
    out->compact_context_enabled = true;
    out->context_pressure_warning_85_emitted = false;
    out->context_pressure_warning_95_emitted = false;
    if (ctx_cfg) {
        out->token_limit = ctx_cfg->token_limit;
        if (ctx_cfg->pressure_warn > 0.0f)
            out->context_pressure_warn = ctx_cfg->pressure_warn;
        if (ctx_cfg->pressure_compact > 0.0f)
            out->context_pressure_compact = ctx_cfg->pressure_compact;
        if (ctx_cfg->compact_target > 0.0f)
            out->context_compact_target = ctx_cfg->compact_target;
        out->compact_context_enabled = ctx_cfg->compact_context;
        out->llm_compiler_enabled = ctx_cfg->llm_compiler_enabled;
        out->mcts_planner_enabled = ctx_cfg->mcts_planner_enabled;
        out->tool_routing_enabled = ctx_cfg->tool_routing_enabled;
        out->tree_of_thought_enabled = ctx_cfg->tree_of_thought;
        out->constitutional_enabled = ctx_cfg->constitutional_ai;
        out->style_rules_enabled = ctx_cfg->constitutional_style_rules_enabled;
        out->multi_agent_enabled = ctx_cfg->multi_agent;
        out->hula_enabled = ctx_cfg->hula_enabled;
        out->compaction_use_structured = ctx_cfg->compaction_use_structured;
        if (ctx_cfg->speculative_cache) {
            hu_speculative_cache_t *cache =
                (hu_speculative_cache_t *)alloc->alloc(alloc->ctx, sizeof(hu_speculative_cache_t));
            if (cache) {
                if (hu_speculative_cache_init(cache, alloc) == HU_OK)
                    out->infra.speculative_cache = cache;
                else
                    alloc->free(alloc->ctx, cache, sizeof(hu_speculative_cache_t));
            }
        }
    }

    out->tools_count = tools_count;
    if (tools_count > 0) {
        out->tools = (hu_tool_t *)alloc->alloc(alloc->ctx, tools_count * sizeof(hu_tool_t));
        if (!out->tools) {
            alloc->free(alloc->ctx, out->workspace_dir, out->workspace_dir_len + 1);
            alloc->free(alloc->ctx, out->default_provider, out->default_provider_len + 1);
            alloc->free(alloc->ctx, out->model_name, out->model_name_len + 1);
            if (out->custom_instructions)
                alloc->free(alloc->ctx, out->custom_instructions, out->custom_instructions_len + 1);
            alloc->free(alloc->ctx, out->timing_model, sizeof(hu_timing_model_t));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(out->tools, tools, tools_count * sizeof(hu_tool_t));

        out->tool_specs =
            (hu_tool_spec_t *)alloc->alloc(alloc->ctx, tools_count * sizeof(hu_tool_spec_t));
        if (!out->tool_specs) {
            alloc->free(alloc->ctx, out->tools, tools_count * sizeof(hu_tool_t));
            alloc->free(alloc->ctx, out->workspace_dir, out->workspace_dir_len + 1);
            alloc->free(alloc->ctx, out->default_provider, out->default_provider_len + 1);
            alloc->free(alloc->ctx, out->model_name, out->model_name_len + 1);
            if (out->custom_instructions)
                alloc->free(alloc->ctx, out->custom_instructions, out->custom_instructions_len + 1);
            alloc->free(alloc->ctx, out->timing_model, sizeof(hu_timing_model_t));
            return HU_ERR_OUT_OF_MEMORY;
        }
        out->tool_specs_count = tools_count;
        for (size_t i = 0; i < tools_count; i++) {
            out->tool_specs[i].name = out->tools[i].vtable->name(out->tools[i].ctx);
            out->tool_specs[i].name_len =
                out->tool_specs[i].name ? strlen(out->tool_specs[i].name) : 0;
            out->tool_specs[i].description = out->tools[i].vtable->description(out->tools[i].ctx);
            out->tool_specs[i].description_len =
                out->tool_specs[i].description ? strlen(out->tool_specs[i].description) : 0;
            out->tool_specs[i].parameters_json =
                out->tools[i].vtable->parameters_json(out->tools[i].ctx);
            out->tool_specs[i].parameters_json_len =
                out->tool_specs[i].parameters_json ? strlen(out->tool_specs[i].parameters_json) : 0;
        }
    } else {
        out->tools = NULL;
        out->tool_specs = NULL;
    }

    out->history = NULL;
    out->history_count = 0;
    out->history_cap = 0;
    out->total_tokens = 0;

    /* Build and cache the static portion of the system prompt */
    {
        hu_prompt_config_t cfg = {
            .provider_name = out->provider.vtable->get_name(out->provider.ctx),
            .provider_name_len = 0,
            .model_name = out->model_name,
            .model_name_len = out->model_name_len,
            .workspace_dir = out->workspace_dir,
            .workspace_dir_len = out->workspace_dir_len,
            .tools = out->tools,
            .tools_count = out->tools_count,
            .memory_context = NULL,
            .memory_context_len = 0,
            .autonomy_level = out->autonomy_level,
            .custom_instructions = out->custom_instructions,
            .custom_instructions_len = out->custom_instructions_len,
        };
        hu_error_t perr = hu_prompt_build_static(alloc, &cfg, &out->cached_static_prompt,
                                                 &out->cached_static_prompt_len);
        if (perr != HU_OK) {
            out->cached_static_prompt = NULL;
            out->cached_static_prompt_len = 0;
        }
        out->cached_static_prompt_cap = out->cached_static_prompt_len;
    }

    if (persona && persona_len > 0) {
        out->persona_name = hu_strndup(alloc, persona, persona_len);
        if (!out->persona_name) {
#ifndef HU_IS_TEST
            hu_log_warn("agent", NULL, "Failed to allocate persona name");
#endif
        } else {
            out->persona_name_len = persona_len;
            out->persona = (hu_persona_t *)alloc->alloc(alloc->ctx, sizeof(hu_persona_t));
            if (out->persona) {
                memset(out->persona, 0, sizeof(hu_persona_t));
                hu_error_t perr = hu_persona_load(alloc, persona, persona_len, out->persona);
                if (perr != HU_OK) {
#ifndef HU_IS_TEST
                    hu_log_error("human", NULL,
                                 "warning: persona '%.*s' not found, running without persona",
                                 (int)persona_len, persona);
#endif
                    alloc->free(alloc->ctx, out->persona, sizeof(hu_persona_t));
                    out->persona = NULL;
                }
            }
        }
    }

    /* M2 — hydrate core identity fields from the loaded persona so
     * build_prompt can emit the name/bio section without separate
     * population. Only fills empty fields to avoid overwriting any
     * values that may have been persisted in the binary blob. */
    if (out->persona && out->persona->name && out->persona->name_len > 0 &&
        out->personal_model.core.user_name[0] == '\0') {
        size_t n = out->persona->name_len < sizeof(out->personal_model.core.user_name) - 1
                       ? out->persona->name_len
                       : sizeof(out->personal_model.core.user_name) - 1;
        memcpy(out->personal_model.core.user_name, out->persona->name, n);
        out->personal_model.core.user_name[n] = '\0';
    }
    if (out->persona && out->persona->identity && out->personal_model.core.user_bio[0] == '\0') {
        size_t n = strlen(out->persona->identity);
        if (n > sizeof(out->personal_model.core.user_bio) - 1)
            n = sizeof(out->personal_model.core.user_bio) - 1;
        memcpy(out->personal_model.core.user_bio, out->persona->identity, n);
        out->personal_model.core.user_bio[n] = '\0';
    }

    out->turn_arena = hu_arena_create(*alloc);

    out->audit_logger = NULL;
    out->undo_stack = hu_undo_stack_create(alloc, 100);
    if (!out->undo_stack) {
        hu_arena_destroy(out->turn_arena);
        out->turn_arena = NULL;
        hu_agent_deinit(out);
        return HU_ERR_OUT_OF_MEMORY;
    }

    {
        const char *session_id = "default";
        hu_error_t serr = hu_stm_init(&out->stm, *alloc, session_id, 7);
        if (serr != HU_OK) {
            hu_agent_deinit(out);
            return serr;
        }
    }

    {
        hu_error_t rerr = hu_pattern_radar_init(&out->radar, *alloc);
        if (rerr != HU_OK) {
            hu_agent_deinit(out);
            return rerr;
        }
    }

    memset(&out->relationship, 0, sizeof(out->relationship));
    hu_relationship_new_session(&out->relationship);

    if (memory && memory->vtable) {
        hu_error_t cerr = hu_commitment_store_create(alloc, memory, &out->commitment_store);
        if (cerr != HU_OK)
            out->commitment_store = NULL;
    } else {
        out->commitment_store = NULL;
    }

    /* Idempotency registry for crash-proof tool execution (HuLa replay engine) */
    hu_idempotency_registry_t *idem_reg = NULL;
    hu_error_t idem_err = hu_idempotency_create(alloc, &idem_reg);
    if (idem_err == HU_OK) {
        out->infra.idempotency_registry = idem_reg;
    } else {
        out->infra.idempotency_registry = NULL;
    }

    /* Workflow event log for durable execution and audit trail */
    hu_workflow_event_log_t *wf_log = NULL;
    hu_error_t wf_err = hu_workflow_event_log_create(alloc, "~/.human/workflows", &wf_log);
    if (wf_err == HU_OK) {
        out->infra.workflow_log = wf_log;
    } else {
        out->infra.workflow_log = NULL;
    }

    /* Approval gate manager for human-in-the-loop workflow pauses */
    hu_gate_manager_t *gate_mgr = NULL;
    hu_error_t gate_err = hu_gate_manager_create(alloc, "~/.human/gates", &gate_mgr);
    if (gate_err == HU_OK) {
        out->infra.gate_manager = gate_mgr;
    } else {
        out->infra.gate_manager = NULL;
    }

    /* Delegation token registry for agent-to-agent authorization */
    out->infra.delegation_registry = hu_delegation_registry_create(alloc);

    /* Webhook manager for incoming webhook event handling */
    hu_webhook_manager_t *webhook_mgr = NULL;
    hu_error_t webhook_err = hu_webhook_manager_create(alloc, &webhook_mgr);
    if (webhook_err == HU_OK) {
        out->infra.webhook_manager = webhook_mgr;
    } else {
        out->infra.webhook_manager = NULL;
    }

    /* Superhuman services */
    {
        hu_error_t sub_err = hu_superhuman_registry_init(&out->superhuman);
        if (sub_err != HU_OK)
            hu_log_warn("agent", NULL, "superhuman registry init failed: %s",
                        hu_error_string(sub_err));
    }
    memset(&out->superhuman_commitment_ctx, 0, sizeof(out->superhuman_commitment_ctx));
    memset(&out->superhuman_emotional_ctx, 0, sizeof(out->superhuman_emotional_ctx));
    memset(&out->superhuman_silence_ctx, 0, sizeof(out->superhuman_silence_ctx));

    if (out->commitment_store) {
        out->superhuman_commitment_ctx.store = out->commitment_store;
        out->superhuman_commitment_ctx.session_id = NULL;
        out->superhuman_commitment_ctx.session_id_len = 0;
        hu_superhuman_service_t svc;
        hu_error_t sub_err =
            hu_superhuman_commitment_service(&out->superhuman_commitment_ctx, &svc);
        if (sub_err != HU_OK)
            hu_log_warn("agent", NULL, "superhuman commitment service init failed: %s",
                        hu_error_string(sub_err));
        else {
            sub_err = hu_superhuman_register(&out->superhuman, svc);
            if (sub_err != HU_OK)
                hu_log_warn("agent", NULL, "superhuman register (commitment) failed: %s",
                            hu_error_string(sub_err));
        }
    }
    {
        hu_superhuman_service_t svc;
        hu_error_t sub_err = hu_superhuman_predictive_service(&out->radar, &svc);
        if (sub_err != HU_OK)
            hu_log_warn("agent", NULL, "superhuman predictive service init failed: %s",
                        hu_error_string(sub_err));
        else {
            sub_err = hu_superhuman_register(&out->superhuman, svc);
            if (sub_err != HU_OK)
                hu_log_warn("agent", NULL, "superhuman register (predictive) failed: %s",
                            hu_error_string(sub_err));
        }
    }
    {
        hu_superhuman_service_t svc;
        hu_error_t sub_err = hu_superhuman_emotional_service(&out->superhuman_emotional_ctx, &svc);
        if (sub_err != HU_OK)
            hu_log_warn("agent", NULL, "superhuman emotional service init failed: %s",
                        hu_error_string(sub_err));
        else {
            sub_err = hu_superhuman_register(&out->superhuman, svc);
            if (sub_err != HU_OK)
                hu_log_warn("agent", NULL, "superhuman register (emotional) failed: %s",
                            hu_error_string(sub_err));
        }
    }
    {
        hu_superhuman_service_t svc;
        hu_error_t sub_err = hu_superhuman_silence_service(&out->superhuman_silence_ctx, &svc);
        if (sub_err != HU_OK)
            hu_log_warn("agent", NULL, "superhuman silence service init failed: %s",
                        hu_error_string(sub_err));
        else {
            sub_err = hu_superhuman_register(&out->superhuman, svc);
            if (sub_err != HU_OK)
                hu_log_warn("agent", NULL, "superhuman register (silence) failed: %s",
                            hu_error_string(sub_err));
        }
    }

    /* SOTA operational modules — safe defaults for production */
    out->sota.dq_config.enabled = true;
    out->sota.dq_config.deduplicate = true;
    out->sota.dq_config.check_encoding = true;

    hu_token_budget_init_defaults(&out->sota.token_budget);
    out->sota.token_budget.enabled = true;

    out->sota.tool_validator.default_level = HU_VALIDATE_SCHEMA;

    out->sota.checkpoint_store.auto_checkpoint = true;
    out->sota.checkpoint_store.interval_steps = 5;

    out->sota.scratchpad.max_bytes = 4096;

    out->sota.mem_policy.enabled = true;
    out->sota.mem_policy.recency_weight = 0.4;
    out->sota.mem_policy.relevance_weight = 0.4;
    out->sota.mem_policy.frequency_weight = 0.2;

    out->sota.gvr_config.enabled = true;
    out->sota.gvr_config.max_revisions = 2;

    out->sota.degradation_config.enabled = true;
    out->sota.degradation_config.max_retries = 1;

    /* SOTA neural subsystems initialization */
    out->sota.srag_config = hu_srag_config_default();
    out->sota.prm_config = hu_prm_config_default();
    {
#ifdef HU_ENABLE_SQLITE
        sqlite3 *sota_db = memory ? hu_sqlite_memory_get_db(memory) : NULL;
#else
        void *sota_db = NULL;
#endif
        {
            hu_error_t sota_sub = hu_adaptive_rag_create(alloc, sota_db, &out->sota.adaptive_rag);
            if (sota_sub != HU_OK)
                hu_log_warn("agent", NULL, "adaptive RAG create failed: %s",
                            hu_error_string(sota_sub));
        }
        {
            hu_error_t sota_sub = hu_tier_manager_create(alloc, sota_db, &out->sota.tier_manager);
            if (sota_sub != HU_OK)
                hu_log_warn("agent", NULL, "tier manager create failed: %s",
                            hu_error_string(sota_sub));
        }
        {
            hu_error_t sota_sub = hu_tier_manager_init_tables(&out->sota.tier_manager);
            if (sota_sub != HU_OK)
                hu_log_warn("agent", NULL, "tier manager init tables failed: %s",
                            hu_error_string(sota_sub));
        }
        {
            hu_error_t sota_sub = hu_tier_manager_load_core(&out->sota.tier_manager);
            if (sota_sub != HU_OK)
                hu_log_warn("agent", NULL, "tier manager load core failed: %s",
                            hu_error_string(sota_sub));
        }
        {
            hu_error_t sota_sub =
                hu_dpo_collector_create(alloc, sota_db, 10000, &out->sota.dpo_collector);
            if (sota_sub != HU_OK)
                hu_log_warn("agent", NULL, "DPO collector create failed: %s",
                            hu_error_string(sota_sub));
        }
        {
            hu_error_t sota_sub = hu_dpo_init_tables(&out->sota.dpo_collector);
            if (sota_sub != HU_OK)
                hu_log_warn("agent", NULL, "DPO init tables failed: %s", hu_error_string(sota_sub));
        }
    }
    out->sota.sota_initialized = true;

    /* Sprint 46 R5.3 — load classifier via helper (refactored for
     * testability per audit FAIL). Failure non-fatal — field stays NULL,
     * downstream gracefully degrades to 0.5. */
    {
        hu_error_t pe_err = hu_agent_internal_load_persona_eval(out, NULL);
        if (pe_err != HU_OK && pe_err != HU_ERR_IO)
            hu_log_warn("agent", NULL,
                        "speaker classifier load failed: %s "
                        "(scoring will return neutral 0.5)",
                        hu_error_string(pe_err));
    }

    hu_emotional_cognition_init(&out->infra.emotional_cognition);
    hu_metacognition_init(&out->infra.metacognition);
    out->infra.current_cognition_mode = HU_COGNITION_FAST;
#ifdef HU_ENABLE_SQLITE
    if (hu_cognition_db_open(&out->infra.cognition_db) != HU_OK)
        out->infra.cognition_db = NULL;
#endif

    /* Initialize instruction discovery from workspace */
    if (out->workspace_dir && out->workspace_dir_len > 0) {
        hu_error_t disc_err = hu_instruction_discovery_run(
            alloc, out->workspace_dir, out->workspace_dir_len, &out->instruction_discovery);
        if (disc_err != HU_OK) {
            out->instruction_discovery = NULL;
        }
    }

    return HU_OK;
}

hu_error_t hu_agent_set_persona(hu_agent_t *agent, const char *name, size_t name_len) {
    if (!agent)
        return HU_ERR_INVALID_ARGUMENT;

    /* Free existing persona */
    if (agent->persona) {
        hu_persona_deinit(agent->alloc, agent->persona);
        agent->alloc->free(agent->alloc->ctx, agent->persona, sizeof(hu_persona_t));
        agent->persona = NULL;
    }
    if (agent->persona_name) {
        agent->alloc->free(agent->alloc->ctx, agent->persona_name, agent->persona_name_len + 1);
        agent->persona_name = NULL;
        agent->persona_name_len = 0;
    }
    if (agent->persona_prompt) {
        agent->alloc->free(agent->alloc->ctx, agent->persona_prompt, agent->persona_prompt_len + 1);
        agent->persona_prompt = NULL;
        agent->persona_prompt_len = 0;
    }

    /* If name is NULL or empty, just clear the persona */
    if (!name || name_len == 0)
        return HU_OK;

    /* Load new persona */
    hu_persona_t *new_persona =
        (hu_persona_t *)agent->alloc->alloc(agent->alloc->ctx, sizeof(hu_persona_t));
    if (!new_persona)
        return HU_ERR_OUT_OF_MEMORY;
    memset(new_persona, 0, sizeof(hu_persona_t));

    hu_error_t err = hu_persona_load(agent->alloc, name, name_len, new_persona);
    if (err != HU_OK) {
        agent->alloc->free(agent->alloc->ctx, new_persona, sizeof(hu_persona_t));
        return err;
    }

    agent->persona = new_persona;
    agent->persona_name = hu_strndup(agent->alloc, name, name_len);
    if (!agent->persona_name) {
        hu_persona_deinit(agent->alloc, new_persona);
        agent->alloc->free(agent->alloc->ctx, new_persona, sizeof(hu_persona_t));
        agent->persona = NULL;
        return HU_ERR_OUT_OF_MEMORY;
    }
    agent->persona_name_len = name_len;

    /* Re-seed frontier state from new persona (narrative, creative voice, boundaries, trust) */
    if (agent->frontiers.initialized) {
        hu_narrative_self_deinit(agent->alloc, &agent->frontiers.narrative);
        hu_creative_voice_deinit(agent->alloc, &agent->frontiers.creative_voice);
        hu_genuine_boundary_set_deinit(agent->alloc, &agent->frontiers.boundaries);
        hu_narrative_self_init(&agent->frontiers.narrative);
        hu_creative_voice_init(&agent->frontiers.creative_voice);
        hu_genuine_boundary_set_init(&agent->frontiers.boundaries);
        hu_tcal_init(&agent->frontiers.trust);

        if (new_persona->identity)
            hu_narrative_self_set_identity(agent->alloc, &agent->frontiers.narrative,
                                           new_persona->identity, strlen(new_persona->identity));
        if (new_persona->biography)
            hu_narrative_self_add_theme(agent->alloc, &agent->frontiers.narrative,
                                        new_persona->biography, strlen(new_persona->biography));
        for (size_t vi = 0; vi < new_persona->values_count && vi < 4; vi++)
            hu_narrative_self_add_theme(agent->alloc, &agent->frontiers.narrative,
                                        new_persona->values[vi], strlen(new_persona->values[vi]));

        for (size_t ti = 0; ti < new_persona->traits_count && ti < 4; ti++)
            hu_creative_voice_add_domain(agent->alloc, &agent->frontiers.creative_voice,
                                         new_persona->traits[ti], strlen(new_persona->traits[ti]));
        if (new_persona->decision_style)
            hu_creative_voice_add_anchor(agent->alloc, &agent->frontiers.creative_voice,
                                         new_persona->decision_style,
                                         strlen(new_persona->decision_style));

        for (size_t pi = 0; pi < new_persona->principles_count && pi < 4; pi++)
            hu_genuine_boundary_add(agent->alloc, &agent->frontiers.boundaries, "principle",
                                    new_persona->principles[pi], NULL, 0.8f, 0);
        for (size_t vi = 0; vi < new_persona->values_count && vi < 2; vi++)
            hu_genuine_boundary_add(agent->alloc, &agent->frontiers.boundaries, "value",
                                    new_persona->values[vi], NULL, 0.7f, 0);
    }

    return HU_OK;
}

void hu_agent_set_mailbox(hu_agent_t *agent, hu_mailbox_t *mailbox) {
    if (!agent)
        return;
    uint64_t id = agent->agent_id ? agent->agent_id : (uint64_t)(uintptr_t)agent;
    if (agent->mailbox) {
        hu_error_t err = hu_mailbox_unregister(agent->mailbox, id);
        if (err != HU_OK)
            hu_log_error("agent", NULL, "warning: mailbox unregister failed: %s",
                         hu_error_string(err));
    }
    agent->mailbox = mailbox;
    if (agent->mailbox) {
        hu_error_t err = hu_mailbox_register(agent->mailbox, id);
        if (err != HU_OK)
            hu_log_error("agent", NULL, "warning: mailbox register failed: %s",
                         hu_error_string(err));
    }
}

void hu_agent_set_skillforge(hu_agent_t *agent, struct hu_skillforge *skillforge) {
    if (!agent)
        return;
#ifdef HU_HAS_SKILLS
    agent->skillforge = skillforge;
#else
    (void)skillforge;
#endif
}

void hu_agent_set_cost_tracker(hu_agent_t *agent, hu_cost_tracker_t *tracker) {
    if (!agent)
        return;
    agent->cost_tracker = tracker;
}

void hu_agent_set_task_list(hu_agent_t *agent, hu_task_list_t *task_list) {
    if (!agent)
        return;
    agent->task_list = task_list;
}

void hu_agent_set_retrieval_engine(hu_agent_t *agent, hu_retrieval_engine_t *engine) {
    if (!agent)
        return;
    agent->retrieval_engine = engine;
}

void hu_agent_set_skill_route_embedder(hu_agent_t *agent, hu_embedder_t *embedder) {
    if (!agent)
        return;
    agent->skill_route_embedder = embedder;
}

void hu_agent_set_awareness(hu_agent_t *agent, struct hu_awareness *awareness) {
    if (!agent)
        return;
    agent->awareness = awareness;
}

const struct hu_awareness *hu_agent_get_awareness(const hu_agent_t *agent) {
    return agent ? agent->awareness : NULL;
}

void hu_agent_set_outcomes(hu_agent_t *agent, struct hu_outcome_tracker *tracker) {
    if (!agent)
        return;
    agent->outcomes = tracker;
}

void hu_agent_set_learner(hu_agent_t *agent, struct hu_learner *learner) {
    if (!agent)
        return;
    agent->learner = learner;
}

#ifdef HU_ENABLE_ML
void hu_agent_m3_adapter_attach(hu_agent_t *agent, const char *path) {
    if (!agent || !agent->alloc)
        return;
    if (agent->m3_adapter) {
        hu_m3_frontier_adapter_close(agent->alloc, agent->m3_adapter);
        agent->m3_adapter = NULL;
        /* Clear the global accessor before freeing so the gateway
         * endpoint doesn't read a freed pointer in the small window
         * between close() and the next attach. */
        hu_m3_outcomes_register_global_adapter(NULL);
    }
    if (!path || path[0] == '\0')
        return;
    size_t path_len = strlen(path);
    hu_m3_frontier_adapter_t *opened = NULL;
    if (hu_m3_frontier_adapter_try_open(agent->alloc, path, path_len, &opened) == HU_OK && opened) {
        agent->m3_adapter = opened;
        /* B3 v0 (2026-05-17 r2): publish the freshly-opened adapter to
         * the global accessor so /v1/m3/outcomes can read it. */
        hu_m3_outcomes_register_global_adapter(opened);

        /* Phase C2 (2026-05-18): open the id map alongside the adapter.
         * Lives next to the adapter probe path. Failure to open the map
         * is non-fatal — outcomes regress to model_id=0/adapter_id=0
         * (unknown) but the loop still functions. */
        if (!agent->m3_id_map) {
            char map_path[2048];
            int mn = snprintf(map_path, sizeof(map_path), "%s/.human/training-data/m3_id_map.json",
                              getenv("HOME") ? getenv("HOME") : "/tmp");
            if (mn > 0 && (size_t)mn < sizeof(map_path)) {
                (void)hu_m3_id_map_create(agent->alloc, map_path, &agent->m3_id_map);
            }
        }

        /* Phase G1 (2026-05-18): open the contact routes file alongside
         * the id map. Same resilience as id_map — failure is non-fatal,
         * routes file missing/corrupt → empty routes (lookup returns NULL
         * for everything, chat path uses base model). */
        if (!agent->m3_contact_routes) {
            (void)hu_m3_contact_routes_create(agent->alloc, /*path=NULL→default*/ NULL,
                                              &agent->m3_contact_routes);
        }
    }
}

/* G1: per-turn contact routing. Called from agent_turn before the
 * provider chat. Reads memory_session_id → hash → routes lookup; if
 * the target adapter differs from what's loaded, POSTs swap to MLX.
 * Tracks the loaded adapter on the agent so repeated turns with the
 * same contact don't pay the swap round-trip. */
void hu_agent_m3_route_per_turn(hu_agent_t *agent) {
    if (!agent || !agent->m3_contact_routes)
        return;
    if (!agent->memory_session_id || agent->memory_session_id_len == 0)
        return;

    uint64_t contact_hash =
        hu_m3_outcome_hash_bytes(agent->memory_session_id, agent->memory_session_id_len);
    const char *target = hu_m3_contact_routes_lookup(agent->m3_contact_routes, contact_hash);
    if (!target || !target[0])
        return; /* No route for this contact; leave current adapter alone. */

    /* Already loaded? Skip the HTTP round-trip. */
    if (agent->m3_active_adapter_path && strcmp(agent->m3_active_adapter_path, target) == 0)
        return;

    /* Resolve MLX URL (env var or default). mlx_admin_swap_adapter
     * takes the v1 root, so include "/v1" suffix. */
    const char *mlx_url = getenv("HUMAN_MLX_URL");
    if (!mlx_url || !mlx_url[0])
        mlx_url = "http://127.0.0.1:8741/v1";

    hu_mlx_admin_swap_result_t swap_result = {0};
    hu_error_t serr = hu_mlx_admin_swap_adapter(agent->alloc, mlx_url, strlen(mlx_url), target,
                                                strlen(target), &swap_result);
    /* Always free the result fields, even on error paths. */
    bool swap_ok = (serr == HU_OK && swap_result.status_code == 200);
    hu_mlx_admin_swap_result_free(agent->alloc, &swap_result);

    if (!swap_ok) {
        /* Soft fail: log + continue with current adapter. The chat path's
         * correctness MUST NOT depend on routing succeeding. */
        hu_log_warn("agent", agent->observer,
                    "m3 contact-routing swap failed: contact_hash=%llu target=%s err=%d",
                    (unsigned long long)contact_hash, target, (int)serr);
        return;
    }

    /* Swap succeeded — update the cached active adapter path. */
    if (agent->m3_active_adapter_path) {
        agent->alloc->free(agent->alloc->ctx, agent->m3_active_adapter_path,
                           strlen(agent->m3_active_adapter_path) + 1);
    }
    size_t tlen = strlen(target);
    agent->m3_active_adapter_path = (char *)agent->alloc->alloc(agent->alloc->ctx, tlen + 1);
    if (agent->m3_active_adapter_path) {
        memcpy(agent->m3_active_adapter_path, target, tlen + 1);
    }
}

void hu_agent_m3_on_provider_success(hu_agent_t *agent) {
    if (!agent || !agent->m3_adapter)
        return;
    (void)hu_m3_frontier_adapter_noop_infer(agent->m3_adapter);
}

void hu_agent_m3_record_chat_outcome(hu_agent_t *agent, const char *prompt, size_t prompt_len,
                                     const char *response, size_t response_len, uint64_t latency_ms,
                                     const char *contact_id, size_t contact_id_len,
                                     hu_m3_guard_decision_t guard_decision, uint8_t turn_kind,
                                     const hu_token_usage_t *usage) {
    /* Defensive no-op shape mirrors hu_agent_m3_on_provider_success so
     * callers in agent_stream / agent_turn don't have to gate the call.
     * The hot path is "agent has no M3 adapter attached" and that path
     * must be free. */
    if (!agent || !agent->m3_adapter)
        return;

    hu_m3_inference_outcome_t outcome;
    memset(&outcome, 0, sizeof(outcome));

    /* Wall clock at record time, not at request start — gives the
     * training loop a "last seen" anchor. latency_ms (the caller's
     * own measurement) holds the actual inference duration so the
     * loop can derive request start by subtraction if needed. */
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        outcome.timestamp_unix_ms =
            (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
    }
    outcome.latency_ms = latency_ms;
    outcome.prompt_hash = hu_m3_outcome_hash_bytes(prompt, prompt_len);
    outcome.response_hash = hu_m3_outcome_hash_bytes(response, response_len);
    outcome.contact_id_hash = hu_m3_outcome_hash_bytes(contact_id, contact_id_len);
    /* prompt_tokens / completion_tokens — Phase C1 (2026-05-18): prefer
     * provider-reported counts when the caller passes a usage block.
     * Fall back to a bytes/4 estimate (~English BPE rule) when usage is
     * NULL or all-zero. The fallback never reports zero when there's
     * real content, which is the property the M3 driver's selection
     * policy relies on. Per-field fallback (rather than all-or-nothing)
     * handles providers that report only prompt_tokens or only
     * completion_tokens. */
    if (usage && usage->prompt_tokens > 0) {
        outcome.prompt_tokens = usage->prompt_tokens;
    } else {
        outcome.prompt_tokens = (uint32_t)(prompt_len / 4);
    }
    if (usage && usage->completion_tokens > 0) {
        outcome.completion_tokens = usage->completion_tokens;
    } else {
        outcome.completion_tokens = (uint32_t)(response_len / 4);
    }
    outcome.guard_decision = (uint8_t)guard_decision;
    outcome.turn_kind = turn_kind;

    /* Phase C2 (2026-05-18): resolve active model + adapter to stable
     * uint16 ids via the persisted id-map. Falls back to 0 (= unknown)
     * for any of:
     *   - no id_map attached (cold install before bootstrap)
     *   - no model_name on the agent (rare; agent without provider)
     *   - provider has no active_adapter accessor (base model only)
     * Lookup is O(N) over a small N — fine in this hot path because the
     * vec_find scan stops on first match and N is bounded at 65535. */
    if (agent->m3_id_map && agent->model_name && agent->model_name_len > 0) {
        outcome.model_id = hu_m3_id_map_lookup_or_insert_model(agent->m3_id_map, agent->model_name,
                                                               agent->model_name_len);
    }
    if (agent->m3_id_map) {
        const char *active = hu_provider_active_adapter(&agent->provider);
        if (active && active[0]) {
            outcome.adapter_id =
                hu_m3_id_map_lookup_or_insert_adapter(agent->m3_id_map, active, strlen(active));
        }
    }
    /* Flush new ids opportunistically. Most calls are no-ops (dirty
     * bit false). The first 1-2 outcomes per session cause a small
     * write; the rest are free. Worth-it tradeoff: a daemon crash
     * before next flush would lose at most one model/adapter id, which
     * the next session re-assigns deterministically (same string →
     * same scan position → same id). */
    if (hu_m3_id_map_is_dirty(agent->m3_id_map))
        (void)hu_m3_id_map_save(agent->m3_id_map);

    (void)hu_m3_frontier_adapter_record_outcome(agent->m3_adapter, &outcome);
}
#else
void hu_agent_m3_adapter_attach(hu_agent_t *agent, const char *path) {
    (void)agent;
    (void)path;
}

void hu_agent_m3_on_provider_success(hu_agent_t *agent) {
    (void)agent;
}

void hu_agent_m3_route_per_turn(hu_agent_t *agent) {
    (void)agent;
}

void hu_agent_m3_record_chat_outcome(hu_agent_t *agent, const char *prompt, size_t prompt_len,
                                     const char *response, size_t response_len, uint64_t latency_ms,
                                     const char *contact_id, size_t contact_id_len,
                                     hu_m3_guard_decision_t guard_decision, uint8_t turn_kind,
                                     const hu_token_usage_t *usage) {
    (void)agent;
    (void)prompt;
    (void)prompt_len;
    (void)response;
    (void)response_len;
    (void)latency_ms;
    (void)contact_id;
    (void)contact_id_len;
    (void)guard_decision;
    (void)turn_kind;
    (void)usage;
}
#endif

void hu_agent_set_voice_config(hu_agent_t *agent, hu_voice_config_t *voice_cfg) {
    if (!agent)
        return;
    agent->voice_config = voice_cfg;
    agent->tts_enabled = (voice_cfg != NULL);
}

static void hu_agent_tom_scenario_copy_field(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0)
        return;
    if (!src || src[0] == '\0') {
        dst[0] = '\0';
        return;
    }
    size_t n = strnlen(src, cap - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void hu_agent_set_tom_scenario(hu_agent_t *agent, const char *premise, const char *question,
                               const char *category) {
    if (!agent)
        return;
    hu_agent_tom_scenario_copy_field(agent->tom_scenario_premise,
                                     sizeof(agent->tom_scenario_premise), premise);
    hu_agent_tom_scenario_copy_field(agent->tom_scenario_question,
                                     sizeof(agent->tom_scenario_question), question);
    hu_agent_tom_scenario_copy_field(agent->tom_scenario_category,
                                     sizeof(agent->tom_scenario_category), category);
}

hu_error_t hu_agent_self_rag_apply(hu_agent_t *agent, const char *draft, size_t draft_len, int mode,
                                   char **swapped_out, size_t *swapped_len_out) {
    if (!agent || !draft || draft_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (!agent->w7_facade || !agent->memory_session_id || agent->memory_session_id_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (swapped_out)
        *swapped_out = NULL;
    if (swapped_len_out)
        *swapped_len_out = 0;
    if (mode == HU_VERIFY_OFF)
        return HU_OK;

    bool can_swap = (mode == HU_VERIFY_SOFT || mode == HU_VERIFY_STRICT) && swapped_out;
    hu_w11_outcome_t outcome = HU_W11_OUTCOME_SUPPORTED;
    size_t total = 0, flagged = 0;
    char *modified = NULL;
    size_t modified_len = 0;
    char **mod_ptr = can_swap ? &modified : NULL;
    size_t *mod_len_ptr = can_swap ? &modified_len : NULL;

    hu_error_t err = hu_w11_self_rag_verify_with_provider(
        agent->w7_facade, agent->alloc, &agent->provider, agent->memory_session_id,
        agent->memory_session_id_len, draft, draft_len, mode, 0, &outcome, &total, &flagged,
        mod_ptr, mod_len_ptr);
    if (err != HU_OK) {
        if (modified)
            agent->alloc->free(agent->alloc->ctx, modified, modified_len + 1);
        return err;
    }

    agent->self_rag_runs++;
    agent->self_rag_claims_total += total;
    agent->self_rag_claims_flagged += flagged;
    if (outcome == HU_W11_OUTCOME_ABSTAINED)
        agent->self_rag_abstentions++;

    if (can_swap && modified && modified_len > 0) {
        if (outcome == HU_W11_OUTCOME_ABSTAINED)
            agent->self_rag_refusals_rendered++;
        *swapped_out = modified;
        if (swapped_len_out)
            *swapped_len_out = modified_len;
    } else if (modified) {
        agent->alloc->free(agent->alloc->ctx, modified, modified_len + 1);
    }
    return HU_OK;
}

void hu_agent_self_rag_telemetry(const hu_agent_t *agent, uint64_t *runs, uint64_t *abstentions,
                                 uint64_t *refusals_rendered, uint64_t *claims_total,
                                 uint64_t *claims_flagged) {
    if (runs)
        *runs = agent ? agent->self_rag_runs : 0;
    if (abstentions)
        *abstentions = agent ? agent->self_rag_abstentions : 0;
    if (refusals_rendered)
        *refusals_rendered = agent ? agent->self_rag_refusals_rendered : 0;
    if (claims_total)
        *claims_total = agent ? agent->self_rag_claims_total : 0;
    if (claims_flagged)
        *claims_flagged = agent ? agent->self_rag_claims_flagged : 0;
}

void hu_agent_deinit(hu_agent_t *agent) {
    if (!agent)
        return;
    /* Sprint 46 R5.3 — free the PersonaEval classifier. Safe on NULL
     * (load failure path) — hu_persona_eval_free handles NULL. */
    if (agent->persona_eval) {
        hu_persona_eval_free(agent->alloc, agent->persona_eval);
        agent->persona_eval = NULL;
    }

    /* Sprint 37 — free director history ring buffer first (heap-owned
     * copies, leaks otherwise). Idempotent on agents with no history. */
    hu_agent_internal_free_director_history(agent);

    /* M2 — persist the user-specific model so the next process start can
     * resume with the accumulated facts/topics/style. Best-effort: any IO
     * failure is silently swallowed (the in-memory model still got the
     * benefit of this session). Skip the save when there's nothing worth
     * writing so first-run users don't get an empty 6 KB blob on disk.
     * Runs FIRST in deinit because the personal model is value-typed inside
     * `hu_agent_t` — it stays valid until the struct itself is freed. */
    if (agent->auto_save && hu_personal_model_has_content(&agent->personal_model)) {
        char pm_path[1024];
        if (hu_personal_model_resolve_default_path(pm_path, sizeof(pm_path))) {
            /* Final decay pass before persisting — same opportunistic
             * call agent_turn makes on every user message. Catches the
             * case of a long-running daemon that ingested signal hours
             * ago and is shutting down without another turn to trigger
             * the per-turn decay. Keeps the on-disk model bounded. */
            (void)hu_personal_model_apply_decay(&agent->personal_model, (int64_t)time(NULL));
            (void)hu_personal_model_save(&agent->personal_model, pm_path);
        }
    }
    hu_agent_free_turn_context(agent);
    if (agent->mailbox) {
        uint64_t id = agent->agent_id ? agent->agent_id : (uint64_t)(uintptr_t)agent;
        (void)hu_mailbox_unregister(agent->mailbox, id);
        agent->mailbox = NULL;
    }
    if (agent->timing_model) {
        hu_timing_model_deinit(agent->alloc, agent->timing_model);
        agent->alloc->free(agent->alloc->ctx, agent->timing_model, sizeof(hu_timing_model_t));
        agent->timing_model = NULL;
    }
    hu_agent_clear_history(agent);
    if (agent->history) {
        agent->alloc->free(agent->alloc->ctx, agent->history,
                           agent->history_cap * sizeof(hu_owned_message_t));
        agent->history = NULL;
        agent->history_cap = 0;
    }
    if (agent->tools) {
        agent->alloc->free(agent->alloc->ctx, agent->tools, agent->tools_count * sizeof(hu_tool_t));
        agent->tools = NULL;
    }
    if (agent->tool_specs) {
        agent->alloc->free(agent->alloc->ctx, agent->tool_specs,
                           agent->tool_specs_count * sizeof(hu_tool_spec_t));
        agent->tool_specs = NULL;
    }
    if (agent->infra.prompt_cache) {
        hu_prompt_cache_deinit((hu_prompt_cache_t *)agent->infra.prompt_cache);
        agent->alloc->free(agent->alloc->ctx, agent->infra.prompt_cache, sizeof(hu_prompt_cache_t));
        agent->infra.prompt_cache = NULL;
    }
    if (agent->infra.tool_cache_ttl) {
        hu_tool_cache_ttl_deinit((hu_tool_cache_ttl_t *)agent->infra.tool_cache_ttl);
        agent->alloc->free(agent->alloc->ctx, agent->infra.tool_cache_ttl,
                           sizeof(hu_tool_cache_ttl_t));
        agent->infra.tool_cache_ttl = NULL;
    }
    if (agent->infra.kv_cache) {
        hu_kv_cache_deinit(agent->infra.kv_cache);
        agent->alloc->free(agent->alloc->ctx, agent->infra.kv_cache, sizeof(hu_kv_cache_manager_t));
        agent->infra.kv_cache = NULL;
    }
    if (agent->infra.context_engine) {
        hu_context_engine_t *ce = (hu_context_engine_t *)agent->infra.context_engine;
        if (ce->vtable && ce->vtable->deinit)
            ce->vtable->deinit(ce->ctx, agent->alloc);
        agent->alloc->free(agent->alloc->ctx, ce, sizeof(hu_context_engine_t));
        agent->infra.context_engine = NULL;
    }
    if (agent->infra.acp_inbox) {
        hu_acp_inbox_deinit((hu_acp_inbox_t *)agent->infra.acp_inbox);
        agent->alloc->free(agent->alloc->ctx, agent->infra.acp_inbox, sizeof(hu_acp_inbox_t));
        agent->infra.acp_inbox = NULL;
    }
    if (agent->cached_static_prompt) {
        agent->alloc->free(agent->alloc->ctx, agent->cached_static_prompt,
                           agent->cached_static_prompt_cap + 1);
        agent->cached_static_prompt = NULL;
    }
    if (agent->model_name) {
        agent->alloc->free(agent->alloc->ctx, agent->model_name, agent->model_name_len + 1);
        agent->model_name = NULL;
    }
    if (agent->default_provider) {
        agent->alloc->free(agent->alloc->ctx, agent->default_provider,
                           agent->default_provider_len + 1);
        agent->default_provider = NULL;
    }
    if (agent->workspace_dir) {
        agent->alloc->free(agent->alloc->ctx, agent->workspace_dir, agent->workspace_dir_len + 1);
        agent->workspace_dir = NULL;
    }
    if (agent->custom_instructions) {
        agent->alloc->free(agent->alloc->ctx, agent->custom_instructions,
                           agent->custom_instructions_len + 1);
        agent->custom_instructions = NULL;
    }
    if (agent->persona) {
        hu_persona_deinit(agent->alloc, agent->persona);
        agent->alloc->free(agent->alloc->ctx, agent->persona, sizeof(hu_persona_t));
        agent->persona = NULL;
    }
    if (agent->persona_name) {
        agent->alloc->free(agent->alloc->ctx, agent->persona_name, agent->persona_name_len + 1);
        agent->persona_name = NULL;
    }
    if (agent->persona_prompt) {
        agent->alloc->free(agent->alloc->ctx, agent->persona_prompt, agent->persona_prompt_len + 1);
        agent->persona_prompt = NULL;
    }
    for (size_t mi = 0; mi < agent->generated_media_count; mi++) {
        if (agent->generated_media[mi]) {
            agent->alloc->free(agent->alloc->ctx, agent->generated_media[mi],
                               strlen(agent->generated_media[mi]) + 1);
            agent->generated_media[mi] = NULL;
        }
    }
    agent->generated_media_count = 0;
    /* Promote important STM entities to persistent memory before cleanup */
    if (agent->memory && agent->memory->vtable) {
        hu_promotion_config_t promo_config = HU_PROMOTION_DEFAULTS;
        hu_error_t promo_err =
            hu_promotion_run(agent->alloc, &agent->stm, agent->memory, &promo_config);
        if (promo_err != HU_OK)
            hu_log_error("agent", NULL, "STM promotion failed: %s", hu_error_string(promo_err));
    }
    hu_stm_deinit(&agent->stm);
    if (agent->frontiers.initialized) {
        hu_narrative_self_deinit(agent->alloc, &agent->frontiers.narrative);
        hu_creative_voice_deinit(agent->alloc, &agent->frontiers.creative_voice);
        hu_genuine_boundary_set_deinit(agent->alloc, &agent->frontiers.boundaries);
        hu_attachment_deinit(agent->alloc, &agent->frontiers.attachment);
        hu_rupture_deinit(agent->alloc, &agent->frontiers.rupture);
        hu_growth_narrative_deinit(agent->alloc, &agent->frontiers.growth);
        agent->frontiers.initialized = false;
    }
    if (agent->sota.sota_initialized) {
        hu_adaptive_rag_deinit(&agent->sota.adaptive_rag);
        hu_tier_manager_deinit(&agent->sota.tier_manager);
        hu_dpo_collector_deinit(&agent->sota.dpo_collector);
        agent->sota.sota_initialized = false;
    }
    /* Scratchpad's max_bytes is set in hu_agent_init (agent.c:629) and
     * populated per-turn by hu_agent_turn (agent_turn.c:8601). Without
     * an explicit deinit, the heap copies of each turn's metadata leak
     * (17-18 bytes per turn) — ASan flagged ~108 small leaks across
     * the agent/agent_security/e2e suites on PR55. Deinit unconditionally:
     * `hu_scratchpad_deinit` is NULL-safe (entries[] is a fixed-size
     * inline array) and handles the never-set case gracefully via
     * entry_count==0. */
    hu_scratchpad_deinit(&agent->sota.scratchpad, agent->alloc);
    hu_pattern_radar_deinit(&agent->radar);
    if (agent->commitment_store) {
        hu_commitment_store_destroy(agent->commitment_store);
        agent->commitment_store = NULL;
    }
    if (agent->infra.idempotency_registry) {
        hu_idempotency_destroy(agent->infra.idempotency_registry, agent->alloc);
        agent->infra.idempotency_registry = NULL;
    }
    if (agent->infra.workflow_log) {
        hu_workflow_event_log_destroy(agent->infra.workflow_log, agent->alloc);
        agent->infra.workflow_log = NULL;
    }
    if (agent->infra.gate_manager) {
        hu_gate_manager_destroy(agent->infra.gate_manager, agent->alloc);
        agent->infra.gate_manager = NULL;
    }
    if (agent->infra.delegation_registry) {
        hu_delegation_registry_destroy(agent->infra.delegation_registry);
        agent->infra.delegation_registry = NULL;
    }
    if (agent->hook_registry) {
        hu_hook_registry_destroy(agent->hook_registry, agent->alloc);
        agent->hook_registry = NULL;
    }
    if (agent->infra.webhook_manager) {
        hu_webhook_manager_destroy(agent->alloc, agent->infra.webhook_manager);
        agent->infra.webhook_manager = NULL;
    }
#ifdef HU_ENABLE_ML
    if (agent->m3_adapter) {
        hu_m3_frontier_adapter_close(agent->alloc, agent->m3_adapter);
        agent->m3_adapter = NULL;
    }
    if (agent->m3_id_map) {
        /* Flush any unsaved ids one last time on shutdown — defensive.
         * In normal operation the save-on-dirty path in record_outcome
         * already flushed; this catches the edge case where an id was
         * assigned during a turn that crashed before reaching the
         * record_outcome save. */
        (void)hu_m3_id_map_save(agent->m3_id_map);
        hu_m3_id_map_destroy(agent->m3_id_map);
        agent->m3_id_map = NULL;
    }
    /* G1 (2026-05-18): release contact routes + active adapter cache. */
    if (agent->m3_contact_routes) {
        hu_m3_contact_routes_destroy(agent->m3_contact_routes);
        agent->m3_contact_routes = NULL;
    }
    if (agent->m3_active_adapter_path) {
        agent->alloc->free(agent->alloc->ctx, agent->m3_active_adapter_path,
                           strlen(agent->m3_active_adapter_path) + 1);
        agent->m3_active_adapter_path = NULL;
    }
#endif
#ifdef HU_ENABLE_SQLITE
    if (agent->w14_scheduler) {
        hu_w14_scheduler_close(agent->w14_scheduler, agent->alloc);
        agent->w14_scheduler = NULL;
    }
    if (agent->w7_facade) {
        hu_w7_facade_close(agent->w7_facade, agent->alloc);
        agent->w7_facade = NULL;
    }
    if (agent->w15_audit_log) {
        hu_w7_audit_log_close(agent->w15_audit_log, agent->alloc);
        agent->w15_audit_log = NULL;
    }
    if (agent->infra.cognition_db) {
        hu_cognition_db_close(agent->infra.cognition_db);
        agent->infra.cognition_db = NULL;
    }
#endif
    if (agent->provider.vtable && agent->provider.vtable->deinit) {
        agent->provider.vtable->deinit(agent->provider.ctx, agent->alloc);
        agent->provider.vtable = NULL;
        agent->provider.ctx = NULL;
    }
    if (agent->turn_arena) {
        hu_arena_destroy(agent->turn_arena);
        agent->turn_arena = NULL;
    }
    if (agent->audit_logger) {
        hu_audit_logger_destroy(agent->audit_logger, agent->alloc);
        agent->audit_logger = NULL;
    }
    if (agent->team) {
        hu_team_destroy(agent->team);
        agent->team = NULL;
    }
    if (agent->undo_stack) {
        hu_undo_stack_destroy(agent->undo_stack);
        agent->undo_stack = NULL;
    }
    if (agent->infra.speculative_cache) {
        hu_speculative_cache_deinit(agent->infra.speculative_cache);
        agent->alloc->free(agent->alloc->ctx, agent->infra.speculative_cache,
                           sizeof(hu_speculative_cache_t));
        agent->infra.speculative_cache = NULL;
    }
    if (agent->instruction_discovery) {
        hu_instruction_discovery_destroy(agent->alloc, agent->instruction_discovery);
        agent->instruction_discovery = NULL;
    }
    agent->skill_route_embedder = NULL;
}

#ifdef HU_ENABLE_SQLITE
hu_error_t hu_agent_bind_sqlite_graph(hu_agent_t *agent, struct hu_graph *graph,
                                      hu_allocator_t *alloc) {
    if (!agent || !graph || !alloc)
        return HU_ERR_INVALID_ARGUMENT;
    agent->verifier_graph = graph;
    hu_error_t err = HU_OK;
    if (!agent->w7_facade) {
        hu_w7_facade_t *facade = NULL;
        err = hu_w7_facade_open(graph, alloc, &facade);
        if (err == HU_OK)
            agent->w7_facade = facade;
        else
            hu_log_warn("agent", NULL, "W7 facade open failed: %s", hu_error_string(err));
    }
    if (agent->w7_facade && !agent->w15_audit_log) {
        const char *home = getenv("HOME");
        if (home) {
            char audit_path[512];
            int ap = snprintf(audit_path, sizeof(audit_path), "%s/.human/audit_log.db", home);
            if (ap > 0 && (size_t)ap < sizeof(audit_path)) {
                hu_error_t ae = hu_w7_audit_log_open(agent->w7_facade, alloc, audit_path, NULL,
                                                     &agent->w15_audit_log);
                if (ae != HU_OK)
                    hu_log_warn("agent", NULL, "W15 audit log open failed: %s",
                                hu_error_string(ae));
            }
        }
    }
    if (agent->w7_facade && !agent->w14_scheduler) {
        hu_w14_scheduler_t *sched = NULL;
        hu_error_t se = hu_w14_scheduler_open(agent->w7_facade, alloc, &sched);
        if (se == HU_OK) {
            agent->w14_scheduler = sched;
            if (agent->persona) {
                hu_scheduler_t *inner = hu_w14_scheduler_inner(sched);
                if (inner)
                    hu_scheduler_set_persona(inner, agent->persona);
            }
        } else
            hu_log_warn("agent", NULL, "W14 scheduler open failed: %s", hu_error_string(se));
    }
    return err;
}
#endif

hu_error_t hu_agent_consolidate_memory(hu_agent_t *agent) {
    if (!agent || !agent->memory || !agent->memory->vtable)
        return HU_ERR_INVALID_ARGUMENT;
    hu_consolidation_config_t config = HU_CONSOLIDATION_DEFAULTS;
    config.provider = &agent->provider;
    config.model = agent->model_name;
    config.model_len = agent->model_name_len;
    hu_error_t err = hu_memory_consolidate(agent->alloc, agent->memory, &config);

    /* After consolidation, demote stale recall-tier entries to archival.
     * Uses a sentinel key to trigger a sweep of entries older than the threshold. */
    if (agent->sota.sota_initialized) {
        hu_tier_manager_demote(&agent->sota.tier_manager, "__consolidation_sweep__", 23,
                               HU_TIER_RECALL, HU_TIER_ARCHIVAL);
    }
    return err;
}

hu_error_t hu_agent_internal_ensure_history_cap(hu_agent_t *agent, size_t need) {
    if (agent->history_cap >= need)
        return HU_OK;
    size_t new_cap = agent->history_cap ? agent->history_cap : HU_AGENT_HISTORY_INIT_CAP;
    while (new_cap < need) {
        if (new_cap > SIZE_MAX / 2)
            return HU_ERR_OUT_OF_MEMORY;
        new_cap *= 2;
    }
    hu_owned_message_t *new_arr = (hu_owned_message_t *)agent->alloc->realloc(
        agent->alloc->ctx, agent->history, agent->history_cap * sizeof(hu_owned_message_t),
        new_cap * sizeof(hu_owned_message_t));
    if (!new_arr)
        return HU_ERR_OUT_OF_MEMORY;
    agent->history = new_arr;
    agent->history_cap = new_cap;
    return HU_OK;
}

hu_error_t hu_agent_internal_append_history(hu_agent_t *agent, hu_role_t role, const char *content,
                                            size_t content_len, const char *name, size_t name_len,
                                            const char *tool_call_id, size_t tool_call_id_len) {
    hu_error_t err = hu_agent_internal_ensure_history_cap(agent, agent->history_count + 1);
    if (err != HU_OK)
        return err;
    char *dup = hu_strndup(agent->alloc, content, content_len);
    if (!dup)
        return HU_ERR_OUT_OF_MEMORY;
    /* hu_strndup stops at first '\0' within content_len — stored length must match allocation. */
    const size_t stored_content_len = strlen(dup);
    agent->history[agent->history_count].role = role;
    agent->history[agent->history_count].content = dup;
    agent->history[agent->history_count].content_len = stored_content_len;
    char *name_dup = name_len ? hu_strndup(agent->alloc, name, name_len) : NULL;
    agent->history[agent->history_count].name = name_dup;
    agent->history[agent->history_count].name_len = name_dup ? strlen(name_dup) : 0;
    if (name_len && !name_dup) {
        agent->alloc->free(agent->alloc->ctx, dup, stored_content_len + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    char *tc_dup =
        tool_call_id_len ? hu_strndup(agent->alloc, tool_call_id, tool_call_id_len) : NULL;
    agent->history[agent->history_count].tool_call_id = tc_dup;
    agent->history[agent->history_count].tool_call_id_len = tc_dup ? strlen(tc_dup) : 0;
    if (tool_call_id_len && !tc_dup) {
        agent->alloc->free(agent->alloc->ctx, dup, stored_content_len + 1);
        if (name_dup)
            agent->alloc->free(agent->alloc->ctx, name_dup,
                               agent->history[agent->history_count].name_len + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    agent->history[agent->history_count].tool_calls = NULL;
    agent->history[agent->history_count].tool_calls_count = 0;
    agent->history[agent->history_count].content_parts = NULL;
    agent->history[agent->history_count].content_parts_count = 0;
    agent->history_count++;
    return HU_OK;
}

/* Append assistant message with tool_calls (duplicates and owns tool_calls). */
hu_error_t hu_agent_internal_append_history_with_tool_calls(hu_agent_t *agent, const char *content,
                                                            size_t content_len,
                                                            const hu_tool_call_t *tool_calls,
                                                            size_t tool_calls_count) {
    hu_error_t err = hu_agent_internal_ensure_history_cap(agent, agent->history_count + 1);
    if (err != HU_OK)
        return err;
    char *dup = content && content_len > 0 ? hu_strndup(agent->alloc, content, content_len)
                                           : hu_strndup(agent->alloc, "", 0);
    if (!dup)
        return HU_ERR_OUT_OF_MEMORY;
    const size_t stored_content_len = strlen(dup);
    agent->history[agent->history_count].role = HU_ROLE_ASSISTANT;
    agent->history[agent->history_count].content = dup;
    agent->history[agent->history_count].content_len = stored_content_len;
    agent->history[agent->history_count].name = NULL;
    agent->history[agent->history_count].name_len = 0;
    agent->history[agent->history_count].tool_call_id = NULL;
    agent->history[agent->history_count].tool_call_id_len = 0;
    agent->history[agent->history_count].tool_calls = NULL;
    agent->history[agent->history_count].tool_calls_count = 0;
    agent->history[agent->history_count].content_parts = NULL;
    agent->history[agent->history_count].content_parts_count = 0;
    if (tool_calls && tool_calls_count > 0) {
        hu_tool_call_t *owned = (hu_tool_call_t *)agent->alloc->alloc(
            agent->alloc->ctx, tool_calls_count * sizeof(hu_tool_call_t));
        if (!owned) {
            agent->alloc->free(agent->alloc->ctx, dup, stored_content_len + 1);
            return HU_ERR_OUT_OF_MEMORY;
        }
        memset(owned, 0, tool_calls_count * sizeof(hu_tool_call_t));
        for (size_t i = 0; i < tool_calls_count; i++) {
            const hu_tool_call_t *src = &tool_calls[i];
            if (src->id && src->id_len > 0) {
                owned[i].id = hu_strndup(agent->alloc, src->id, src->id_len);
                if (!owned[i].id) {
                    hu_agent_commands_free_owned_tool_calls(agent->alloc, owned, tool_calls_count);
                    agent->alloc->free(agent->alloc->ctx, dup, stored_content_len + 1);
                    return HU_ERR_OUT_OF_MEMORY;
                }
                owned[i].id_len = src->id_len;
            }
            if (src->name && src->name_len > 0) {
                owned[i].name = hu_strndup(agent->alloc, src->name, src->name_len);
                if (!owned[i].name) {
                    hu_agent_commands_free_owned_tool_calls(agent->alloc, owned, tool_calls_count);
                    agent->alloc->free(agent->alloc->ctx, dup, stored_content_len + 1);
                    return HU_ERR_OUT_OF_MEMORY;
                }
                owned[i].name_len = src->name_len;
            }
            if (src->arguments && src->arguments_len > 0) {
                owned[i].arguments = hu_strndup(agent->alloc, src->arguments, src->arguments_len);
                if (!owned[i].arguments) {
                    hu_agent_commands_free_owned_tool_calls(agent->alloc, owned, tool_calls_count);
                    agent->alloc->free(agent->alloc->ctx, dup, stored_content_len + 1);
                    return HU_ERR_OUT_OF_MEMORY;
                }
                owned[i].arguments_len = src->arguments_len;
            }
        }
        agent->history[agent->history_count].tool_calls = owned;
        agent->history[agent->history_count].tool_calls_count = tool_calls_count;
    }
    agent->history_count++;
    return HU_OK;
}

void hu_agent_clear_history(hu_agent_t *agent) {
    if (!agent || !agent->history)
        return;
    for (size_t i = 0; i < agent->history_count; i++) {
        if (agent->history[i].content) {
            agent->alloc->free(agent->alloc->ctx, agent->history[i].content,
                               agent->history[i].content_len + 1);
        }
        if (agent->history[i].name) {
            agent->alloc->free(agent->alloc->ctx, agent->history[i].name,
                               agent->history[i].name_len + 1);
        }
        if (agent->history[i].tool_call_id) {
            agent->alloc->free(agent->alloc->ctx, agent->history[i].tool_call_id,
                               agent->history[i].tool_call_id_len + 1);
        }
        if (agent->history[i].tool_calls && agent->history[i].tool_calls_count > 0) {
            hu_agent_commands_free_owned_tool_calls(agent->alloc, agent->history[i].tool_calls,
                                                    agent->history[i].tool_calls_count);
            agent->history[i].tool_calls = NULL;
            agent->history[i].tool_calls_count = 0;
        }
        if (agent->history[i].content_parts && agent->history[i].content_parts_count > 0) {
            for (size_t j = 0; j < agent->history[i].content_parts_count; j++) {
                hu_content_part_t *cp = &agent->history[i].content_parts[j];
                if (cp->tag == HU_CONTENT_PART_TEXT && cp->data.text.ptr)
                    agent->alloc->free(agent->alloc->ctx, (void *)cp->data.text.ptr,
                                       cp->data.text.len + 1);
                else if (cp->tag == HU_CONTENT_PART_IMAGE_URL && cp->data.image_url.url)
                    agent->alloc->free(agent->alloc->ctx, (void *)cp->data.image_url.url,
                                       cp->data.image_url.url_len + 1);
                else if (cp->tag == HU_CONTENT_PART_IMAGE_BASE64) {
                    if (cp->data.image_base64.data)
                        agent->alloc->free(agent->alloc->ctx, (void *)cp->data.image_base64.data,
                                           cp->data.image_base64.data_len + 1);
                    if (cp->data.image_base64.media_type)
                        agent->alloc->free(agent->alloc->ctx,
                                           (void *)cp->data.image_base64.media_type,
                                           cp->data.image_base64.media_type_len + 1);
                } else if (cp->tag == HU_CONTENT_PART_AUDIO_BASE64) {
                    if (cp->data.audio_base64.data)
                        agent->alloc->free(agent->alloc->ctx, (void *)cp->data.audio_base64.data,
                                           cp->data.audio_base64.data_len + 1);
                    if (cp->data.audio_base64.media_type)
                        agent->alloc->free(agent->alloc->ctx,
                                           (void *)cp->data.audio_base64.media_type,
                                           cp->data.audio_base64.media_type_len + 1);
                } else if (cp->tag == HU_CONTENT_PART_VIDEO_URL) {
                    if (cp->data.video_url.url)
                        agent->alloc->free(agent->alloc->ctx, (void *)cp->data.video_url.url,
                                           cp->data.video_url.url_len + 1);
                    if (cp->data.video_url.media_type)
                        agent->alloc->free(agent->alloc->ctx, (void *)cp->data.video_url.media_type,
                                           cp->data.video_url.media_type_len + 1);
                }
            }
            agent->alloc->free(agent->alloc->ctx, agent->history[i].content_parts,
                               agent->history[i].content_parts_count * sizeof(hu_content_part_t));
            agent->history[i].content_parts = NULL;
            agent->history[i].content_parts_count = 0;
        }
    }
    agent->history_count = 0;
    if (agent->session_store && agent->session_store->vtable &&
        agent->session_store->vtable->clear_messages) {
        (void)agent->session_store->vtable->clear_messages(agent->session_store->ctx, "", 0);
    }
}

uint32_t hu_agent_estimate_tokens(const char *text, size_t len) {
    if (!text)
        return 0;
    return (uint32_t)((len + 3) / 4);
}

hu_policy_action_t hu_agent_internal_check_policy(hu_agent_t *agent, const char *tool_name,
                                                  const char *arguments) {
    /* First check existing security policy (agent->policy) */
    if (agent->policy && agent->policy->block_high_risk_commands &&
        hu_tool_risk_level(tool_name) == HU_RISK_HIGH)
        return HU_POLICY_DENY;
    /* Then check rule-based policy engine */
    if (!agent->policy_engine)
        return HU_POLICY_ALLOW;
    hu_policy_eval_ctx_t pe_ctx = {
        .tool_name = tool_name, .args_json = arguments ? arguments : "", .session_cost_usd = 0};
    hu_policy_result_t pe_res = hu_policy_engine_evaluate(agent->policy_engine, &pe_ctx);
    return pe_res.action;
}

hu_policy_action_t hu_agent_internal_evaluate_tool_policy(hu_agent_t *agent, const char *tool_name,
                                                          const char *args_json) {
    hu_policy_action_t base = hu_agent_internal_check_policy(agent, tool_name, args_json);
    if (base == HU_POLICY_DENY)
        return HU_POLICY_DENY;
    if (agent->team && agent->agent_id) {
        const hu_team_member_t *member = hu_team_get_member(agent->team, agent->agent_id);
        if (member && !hu_team_role_allows_tool(member->role, tool_name))
            return HU_POLICY_DENY;
    }
    if (agent->policy_engine) {
        hu_policy_eval_ctx_t ctx = {
            .tool_name = tool_name,
            .args_json = args_json ? args_json : "",
            .session_cost_usd = 0.0,
        };
        hu_policy_result_t pr = hu_policy_engine_evaluate(agent->policy_engine, &ctx);
        if (pr.action == HU_POLICY_DENY)
            return HU_POLICY_DENY;
        if (pr.action == HU_POLICY_REQUIRE_APPROVAL)
            return HU_POLICY_REQUIRE_APPROVAL;
    }

    /* AEGIS-style deep argument inspection (arXiv:2603.12621) */
    if (args_json && args_json[0]) {
        hu_arg_inspection_t insp;
        memset(&insp, 0, sizeof(insp));
        if (hu_arg_inspect(tool_name, args_json, strlen(args_json), &insp) == HU_OK) {
            if (hu_arg_inspection_should_block(&insp, agent->policy))
                return HU_POLICY_DENY;
            if (hu_arg_inspection_needs_approval(&insp, agent->policy))
                return HU_POLICY_REQUIRE_APPROVAL;
        }
    }

    return base;
}

hu_tool_t *hu_agent_internal_find_tool(hu_agent_t *agent, const char *name, size_t name_len) {
    if (!name || name_len == 0)
        return NULL;
    for (size_t i = 0; i < agent->tools_count; i++) {
        const char *n = agent->tools[i].vtable->name(agent->tools[i].ctx);
        if (n && name_len == strlen(n) && memcmp(n, name, name_len) == 0) {
            return &agent->tools[i];
        }
    }
    return NULL;
}

bool hu_agent_internal_pre_hook_check(hu_agent_t *agent, const char *tool_name,
                                      size_t tool_name_len, const char *args_json,
                                      size_t args_json_len, hu_tool_result_t *out) {
    if (!agent || !out)
        return true;
    if (!agent->hook_registry)
        return true;

    const char *hook_args = args_json ? args_json : "";
    size_t hook_args_len = args_json ? args_json_len : 0;

    hu_hook_result_t pre_res;
    memset(&pre_res, 0, sizeof(pre_res));
    hu_hook_pipeline_pre_tool(agent->hook_registry, agent->alloc, tool_name, tool_name_len,
                              hook_args, hook_args_len, &pre_res);
    if (pre_res.decision == HU_HOOK_DENY) {
        const char *deny_msg = pre_res.message ? pre_res.message : "denied by hook";
        size_t deny_len = pre_res.message ? pre_res.message_len : 14;
        char *deny_copy = hu_strndup(agent->alloc, deny_msg, deny_len);
        hu_hook_result_free(agent->alloc, &pre_res);
        *out = hu_tool_result_fail(deny_copy ? deny_copy : "denied by hook",
                                   deny_copy ? deny_len : 14);
        out->error_msg_owned = deny_copy != NULL;
        return false;
    }
    hu_hook_result_free(agent->alloc, &pre_res);
    return true;
}

void hu_agent_internal_post_hook_fire(hu_agent_t *agent, const char *tool_name,
                                      size_t tool_name_len, const char *args_json,
                                      size_t args_json_len, const hu_tool_result_t *result) {
    if (!agent || !result)
        return;
    if (!agent->hook_registry)
        return;

    const char *hook_args = args_json ? args_json : "";
    size_t hook_args_len = args_json ? args_json_len : 0;

    /* Mirror scattered-site convention: report error_msg on failure so
     * post-hooks see the actual reason rather than an empty output. */
    const char *output = result->success ? (result->output ? result->output : "")
                                         : (result->error_msg ? result->error_msg : "");
    size_t output_len = result->success ? (result->output ? result->output_len : 0)
                                        : (result->error_msg ? result->error_msg_len : 0);
    hu_hook_result_t post_res;
    memset(&post_res, 0, sizeof(post_res));
    hu_hook_pipeline_post_tool(agent->hook_registry, agent->alloc, tool_name, tool_name_len,
                               hook_args, hook_args_len, output, output_len, result->success,
                               &post_res);
    hu_hook_result_free(agent->alloc, &post_res);
}

hu_error_t hu_agent_internal_dispatch_with_hooks(hu_agent_t *agent, hu_tool_t *tool,
                                                 const char *tool_name, size_t tool_name_len,
                                                 const char *args_json, size_t args_json_len,
                                                 const hu_json_value_t *args_parsed,
                                                 hu_tool_result_t *out) {
    if (!agent || !tool || !out)
        return HU_ERR_INVALID_ARGUMENT;

    /* Pre-tool hook. A DENY decision short-circuits the dispatch: the tool's
     * execute() is NOT called and *out is populated with a failure result.
     * Per the audit-followup contract, the post-hook STILL fires below so
     * auditors observe every tool dispatch regardless of outcome. */
    bool proceed = hu_agent_internal_pre_hook_check(agent, tool_name, tool_name_len, args_json,
                                                    args_json_len, out);

    /* Execute. The tool is responsible for populating *out; we treat a missing
     * execute pointer as an empty-success no-op to match the historical
     * behavior of the inline dispatch sites being replaced. Skipped if the
     * pre-hook denied — that's the security gate the helper exists for. */
    if (proceed) {
        if (tool->vtable && tool->vtable->execute) {
            tool->vtable->execute(tool->ctx, agent->alloc, args_parsed, out);
        } else {
            *out = hu_tool_result_fail("tool has no execute method", 26);
        }
    }

    /* Post-tool hook. Fires unconditionally whenever a registry is configured,
     * including when the pre-hook denied the call: auditors get full visibility. */
    hu_agent_internal_post_hook_fire(agent, tool_name, tool_name_len, args_json, args_json_len,
                                     out);

    return HU_OK;
}

/* Public alias — same signature, same behavior, exposed in include/human/agent.h
 * so callers outside the agent module can dispatch through the canonical
 * pre/post-hook envelope without including agent_internal.h. The internal name
 * is retained for back-compat with existing call sites. */
hu_error_t hu_agent_dispatch_tool(hu_agent_t *agent, hu_tool_t *tool, const char *tool_name,
                                  size_t tool_name_len, const char *args_json, size_t args_json_len,
                                  const hu_json_value_t *args_parsed, hu_tool_result_t *out) {
    return hu_agent_internal_dispatch_with_hooks(agent, tool, tool_name, tool_name_len, args_json,
                                                 args_json_len, args_parsed, out);
}

hu_error_t hu_agent_run_single(hu_agent_t *agent, const char *system_prompt,
                               size_t system_prompt_len, const char *user_message,
                               size_t user_message_len, char **response_out,
                               size_t *response_len_out) {
    if (!agent || !response_out)
        return HU_ERR_INVALID_ARGUMENT;
    if (!agent->provider.vtable || !agent->provider.vtable->chat)
        return HU_ERR_INVALID_ARGUMENT;
    *response_out = NULL;
    if (response_len_out)
        *response_len_out = 0;

    hu_chat_request_t req;
    memset(&req, 0, sizeof(req));
    hu_chat_message_t msgs[2];
    msgs[0].role = HU_ROLE_SYSTEM;
    msgs[0].content = system_prompt ? system_prompt : "";
    msgs[0].content_len = system_prompt_len;
    msgs[0].name = NULL;
    msgs[0].name_len = 0;
    msgs[0].tool_call_id = NULL;
    msgs[0].tool_call_id_len = 0;
    msgs[0].content_parts = NULL;
    msgs[0].content_parts_count = 0;

    msgs[1].role = HU_ROLE_USER;
    msgs[1].content = user_message ? user_message : "";
    msgs[1].content_len = user_message_len;
    msgs[1].name = NULL;
    msgs[1].name_len = 0;
    msgs[1].tool_call_id = NULL;
    msgs[1].tool_call_id_len = 0;
    msgs[1].content_parts = NULL;
    msgs[1].content_parts_count = 0;

    req.messages = msgs;
    req.messages_count = 2;
    req.model = agent->model_name;
    req.model_len = agent->model_name_len;
    req.temperature = agent->temperature;
    req.max_tokens = 0;
    req.tools = NULL;
    req.tools_count = 0;
    req.timeout_secs = 0;
    req.reasoning_effort = NULL;
    req.reasoning_effort_len = 0;

    hu_chat_response_t resp;
    memset(&resp, 0, sizeof(resp));
    hu_error_t err =
        agent->provider.vtable->chat(agent->provider.ctx, agent->alloc, &req, agent->model_name,
                                     agent->model_name_len, agent->temperature, &resp);
    if (err != HU_OK)
        return err;

    if (resp.content && resp.content_len > 0) {
        *response_out = hu_strndup(agent->alloc, resp.content, resp.content_len);
        if (!*response_out) {
            hu_chat_response_free(agent->alloc, &resp);
            return HU_ERR_OUT_OF_MEMORY;
        }
        if (response_len_out)
            *response_len_out = resp.content_len;
    }
    agent->total_tokens += resp.usage.total_tokens;
    hu_agent_internal_record_cost(agent, &resp.usage);
    hu_chat_response_free(agent->alloc, &resp);
    return HU_OK;
}

void hu_agent_internal_maybe_tts(hu_agent_t *agent, const char *text, size_t text_len) {
    if (!agent->tts_enabled || !agent->voice_config || !text || text_len == 0)
        return;
#ifndef HU_IS_TEST
    void *audio = NULL;
    size_t audio_len = 0;
    hu_error_t err =
        hu_voice_tts(agent->alloc, agent->voice_config, text, text_len, &audio, &audio_len);
    if (err == HU_OK && audio && audio_len > 0) {
        hu_voice_play(agent->alloc, audio, audio_len);
        agent->alloc->free(agent->alloc->ctx, audio, audio_len);
    }
#else
    (void)agent;
    (void)text;
    (void)text_len;
#endif
}

void hu_agent_internal_process_mailbox_messages(hu_agent_t *agent) {
    if (!agent->mailbox)
        return;
    uint64_t id = agent->agent_id ? agent->agent_id : (uint64_t)(uintptr_t)agent;
    hu_message_t msg;
    while (hu_mailbox_recv(agent->mailbox, id, &msg) == HU_OK) {
        bool acp_done = false;
        if (msg.payload && msg.payload_len >= 4 && memcmp(msg.payload, "ACP|", 4) == 0) {
            hu_acp_message_t acp;
            memset(&acp, 0, sizeof(acp));
            if (hu_acp_bridge_recv(agent->alloc, &msg, &acp) == HU_OK) {
                char buf[640];
                int n = snprintf(buf, sizeof(buf), "[ACP %s from %.*s]: %.*s",
                                 hu_acp_msg_type_name(acp.type), (int)acp.sender_id_len,
                                 acp.sender_id ? acp.sender_id : "",
                                 acp.payload_len < 500 ? (int)acp.payload_len : 500,
                                 acp.payload ? acp.payload : "");
                hu_acp_message_free(agent->alloc, &acp);
                acp_done = true;
                if (n > 0) {
                    hu_error_t hist_err = hu_agent_internal_append_history(
                        agent, HU_ROLE_USER, buf, (size_t)n, NULL, 0, NULL, 0);
                    if (hist_err != HU_OK)
                        hu_log_error("agent", NULL, "mailbox ACP history append failed: %s",
                                     hu_error_string(hist_err));
                }
            }
        }
        if (!acp_done) {
            char buf[512];
            size_t payload_len = msg.payload_len < 400 ? msg.payload_len : 400;
            int n = snprintf(buf, sizeof(buf), "[Message from agent %llu]: %.*s",
                             (unsigned long long)msg.from_agent, (int)payload_len,
                             msg.payload ? msg.payload : "");
            if (n > 0) {
                hu_error_t hist_err = hu_agent_internal_append_history(agent, HU_ROLE_USER, buf,
                                                                       (size_t)n, NULL, 0, NULL, 0);
                if (hist_err != HU_OK)
                    hu_log_error("agent", NULL, "mailbox message append failed: %s",
                                 hu_error_string(hist_err));
            }
        }
        hu_message_free(agent->alloc, &msg);
    }
}

/* ── Config hot-reload (Feature 1) ─────────────────────────────────────────────── */

hu_error_t hu_agent_reload_config(hu_agent_t *agent, char **summary_out, size_t *summary_len_out) {
    if (!agent || !summary_out)
        return HU_ERR_INVALID_ARGUMENT;

    *summary_out = NULL;
    if (summary_len_out)
        *summary_len_out = 0;

    /* Load fresh config from ~/.human/config.json */
    hu_config_t fresh_cfg;
    memset(&fresh_cfg, 0, sizeof(fresh_cfg));
    hu_error_t err = hu_config_load(agent->alloc, &fresh_cfg);
    if (err != HU_OK) {
        char *error_msg =
            hu_sprintf(agent->alloc, "Failed to reload config: %s", hu_error_string(err));
        *summary_out = error_msg;
        if (summary_len_out && error_msg)
            *summary_len_out = strlen(error_msg);
        return HU_OK; /* Return HU_OK but include error in summary */
    }

    char *summary_buf = (char *)agent->alloc->alloc(agent->alloc->ctx, 512);
    if (!summary_buf) {
        hu_config_deinit(&fresh_cfg);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t offset = 0;

    /* Track what changed */
    bool hooks_changed = false;
    bool permission_changed = false;
    bool instructions_changed = false;

    /* 1. Rebuild hook registry from fresh config */
    if (fresh_cfg.hooks.entries_count > 0) {
        if (agent->hook_registry) {
            hu_hook_registry_destroy(agent->hook_registry, agent->alloc);
            agent->hook_registry = NULL;
        }
        hu_error_t hook_err = hu_hook_registry_create(agent->alloc, &agent->hook_registry);
        if (hook_err == HU_OK) {
            for (size_t i = 0; i < fresh_cfg.hooks.entries_count; i++) {
                hu_hook_config_entry_t *hce = &fresh_cfg.hooks.entries[i];
                if (!hce->command || !hce->event)
                    continue;
                hu_hook_event_t ev = HU_HOOK_PRE_TOOL_EXECUTE;
                if (strcmp(hce->event, "post_tool_execute") == 0)
                    ev = HU_HOOK_POST_TOOL_EXECUTE;
                hu_hook_entry_t he = {0};
                he.name = hce->name;
                he.name_len = hce->name ? strlen(hce->name) : 0;
                he.event = ev;
                he.command = hce->command;
                he.command_len = strlen(hce->command);
                he.timeout_sec = hce->timeout_sec ? hce->timeout_sec : 30;
                he.required = hce->required;
                hu_hook_registry_add(agent->hook_registry, agent->alloc, &he);
            }
            hooks_changed = true;
            offset = hu_buf_appendf(summary_buf, 512, offset, "Hooks: rebuilt (%zu entries)\n",
                                    fresh_cfg.hooks.entries_count);
        }
    } else if (agent->hook_registry) {
        /* Hooks were cleared in new config */
        hu_hook_registry_destroy(agent->hook_registry, agent->alloc);
        agent->hook_registry = NULL;
        hooks_changed = true;
        offset = hu_buf_appendf(summary_buf, 512, offset, "Hooks: cleared\n");
    }

    /* 2. Update permission level if changed */
    hu_permission_level_t new_perm = (hu_permission_level_t)fresh_cfg.agent.permission_level;
    if (new_perm != agent->permission_base_level) {
        agent->permission_base_level = new_perm;
        /* Update effective level if not escalated */
        if (!agent->permission_escalated) {
            agent->permission_level = new_perm;
        }
        permission_changed = true;
        offset = hu_buf_appendf(summary_buf, 512, offset, "Permission level: updated to %u\n",
                                (unsigned int)new_perm);
    }

    /* 3. Re-discover instruction files */
    if (agent->instruction_discovery) {
        hu_instruction_discovery_destroy(agent->alloc, agent->instruction_discovery);
        agent->instruction_discovery = NULL;
    }
    if (fresh_cfg.agent.discover_instructions) {
        hu_error_t instr_err =
            hu_instruction_discovery_run(agent->alloc, agent->workspace_dir,
                                         agent->workspace_dir_len, &agent->instruction_discovery);
        if (instr_err == HU_OK && agent->instruction_discovery) {
            instructions_changed = true;
            offset = hu_buf_appendf(summary_buf, 512, offset,
                                    "Instructions: re-discovered (%zu files)\n",
                                    agent->instruction_discovery->file_count);
        }
    }

    /* Clean up fresh config */
    hu_config_deinit(&fresh_cfg);

    /* Generate final summary */
    if (!hooks_changed && !permission_changed && !instructions_changed) {
        offset = hu_buf_appendf(summary_buf, 512, offset, "No changes detected.");
    } else {
        offset = hu_buf_appendf(summary_buf, 512, offset, "Config reloaded successfully.");
    }

    summary_buf[offset] = '\0';

    *summary_out = summary_buf;
    if (summary_len_out)
        *summary_len_out = (size_t)offset;

    return HU_OK;
}
