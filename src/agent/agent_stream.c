/* Streaming infrastructure: token callback wiring, hu_agent_turn_stream */
#include "agent_internal.h"
#include "human/agent/awareness.h"
#include "human/agent/commands.h"
#include "human/agent/memory_loader.h"
#include "human/agent/outcomes.h"
#include "human/agent/prompt.h"
#include "human/context.h"
#include "human/core/string.h"
#ifdef HU_HAS_PERSONA
#include "human/persona.h"
#endif
#include "human/provider.h"
#include "human/voice.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct stream_token_wrap {
    hu_agent_stream_token_cb on_token;
    void *token_ctx;
} stream_token_wrap_t;

static void stream_chunk_to_token_cb(void *ctx, const hu_stream_chunk_t *chunk) {
    stream_token_wrap_t *w = (stream_token_wrap_t *)ctx;
    if (chunk->is_final || !w->on_token)
        return;
    if (chunk->delta && chunk->delta_len > 0)
        w->on_token(chunk->delta, chunk->delta_len, w->token_ctx);
}

hu_error_t hu_agent_turn_stream(hu_agent_t *agent, const char *msg, size_t msg_len,
                                hu_agent_stream_token_cb on_token, void *token_ctx,
                                char **response_out, size_t *response_len_out) {
    if (!agent || !msg || !response_out)
        return HU_ERR_INVALID_ARGUMENT;
    *response_out = NULL;
    if (response_len_out)
        *response_len_out = 0;

    hu_agent_set_current_for_tools(agent);

    hu_agent_internal_process_mailbox_messages(agent);

    char *slash_resp = hu_agent_handle_slash_command(agent, msg, msg_len);
    if (slash_resp) {
        hu_agent_clear_current_for_tools();
        *response_out = slash_resp;
        if (response_len_out)
            *response_len_out = strlen(slash_resp);
        return HU_OK;
    }

    bool can_stream = (on_token != NULL) && agent->provider.vtable->supports_streaming &&
                      agent->provider.vtable->supports_streaming(agent->provider.ctx) &&
                      agent->provider.vtable->stream_chat;

    if (!can_stream) {
        hu_error_t fallback_err =
            hu_agent_turn(agent, msg, msg_len, response_out, response_len_out);
        if (fallback_err == HU_OK && on_token && *response_out && response_len_out &&
            *response_len_out > 0) {
            size_t chunk_size = 12;
            for (size_t i = 0; i < *response_len_out; i += chunk_size) {
                size_t n = *response_len_out - i;
                if (n > chunk_size)
                    n = chunk_size;
                on_token(*response_out + i, n, token_ctx);
            }
        }
        hu_agent_clear_current_for_tools();
        return fallback_err;
    }

    /* When tools are present, fall back to hu_agent_turn but simulate
     * streaming by chunking the final response to the callback.
     * This keeps the TUI responsive while tools execute. (Tier 2.4) */
    bool has_tools = (agent->tool_specs_count > 0);
    if (has_tools) {
        hu_error_t fallback_err =
            hu_agent_turn(agent, msg, msg_len, response_out, response_len_out);
        if (fallback_err == HU_OK && on_token && *response_out && response_len_out &&
            *response_len_out > 0) {
            size_t chunk_size = 8;
            for (size_t i = 0; i < *response_len_out; i += chunk_size) {
                size_t n = *response_len_out - i;
                if (n > chunk_size)
                    n = chunk_size;
                on_token(*response_out + i, n, token_ctx);
            }
        }
        hu_agent_clear_current_for_tools();
        return fallback_err;
    }

    hu_error_t err =
        hu_agent_internal_append_history(agent, HU_ROLE_USER, msg, msg_len, NULL, 0, NULL, 0);
    if (err != HU_OK) {
        hu_agent_clear_current_for_tools();
        return err;
    }

    char *memory_ctx = NULL;
    size_t memory_ctx_len = 0;
    if (agent->memory && agent->memory->vtable) {
        hu_memory_loader_t loader;
        hu_memory_loader_init(&loader, agent->alloc, agent->memory, agent->retrieval_engine, 10,
                              4000);
        (void)hu_memory_loader_load(&loader, msg, msg_len, "", 0, &memory_ctx, &memory_ctx_len);
    }

    /* Build situational awareness context */
    char *awareness_ctx = NULL;
    size_t awareness_ctx_len = 0;
    if (agent->awareness)
        awareness_ctx = hu_awareness_context(agent->awareness, agent->alloc, &awareness_ctx_len);

    /* Build outcome tracking summary */
    char *outcome_ctx = NULL;
    size_t outcome_ctx_len = 0;
    if (agent->outcomes)
        outcome_ctx = hu_outcome_build_summary(agent->outcomes, agent->alloc, &outcome_ctx_len);

    /* Build persona prompt fresh each turn (channel-dependent; no caching) */
    char *persona_prompt = NULL;
    size_t persona_prompt_len = 0;
#ifdef HU_HAS_PERSONA
    if (agent->persona) {
        const char *ch = agent->active_channel;
        size_t ch_len = agent->active_channel_len;
        hu_error_t perr = hu_persona_build_prompt(agent->alloc, agent->persona, ch, ch_len, NULL, 0,
                                                  &persona_prompt, &persona_prompt_len);
        if (perr != HU_OK) {
            if (memory_ctx)
                agent->alloc->free(agent->alloc->ctx, memory_ctx, memory_ctx_len + 1);
            if (awareness_ctx)
                agent->alloc->free(agent->alloc->ctx, awareness_ctx, awareness_ctx_len + 1);
            if (outcome_ctx)
                agent->alloc->free(agent->alloc->ctx, outcome_ctx, outcome_ctx_len + 1);
            hu_agent_clear_current_for_tools();
            return perr;
        }
    }
#endif

    char *system_prompt = NULL;
    size_t system_prompt_len = 0;
    if (agent->cached_static_prompt && !persona_prompt && !awareness_ctx) {
        err = hu_prompt_build_with_cache(agent->alloc, agent->cached_static_prompt,
                                         agent->cached_static_prompt_len, memory_ctx,
                                         memory_ctx_len, &system_prompt, &system_prompt_len);
        if (memory_ctx)
            agent->alloc->free(agent->alloc->ctx, memory_ctx, memory_ctx_len + 1);
        if (err != HU_OK) {
            hu_agent_clear_current_for_tools();
            return err;
        }
    } else {
        hu_prompt_config_t cfg = {
            .provider_name = agent->provider.vtable->get_name(agent->provider.ctx),
            .provider_name_len = 0,
            .model_name = agent->model_name,
            .model_name_len = agent->model_name_len,
            .workspace_dir = agent->workspace_dir,
            .workspace_dir_len = agent->workspace_dir_len,
            .tools = agent->tools,
            .tools_count = agent->tools_count,
            .memory_context = memory_ctx,
            .memory_context_len = memory_ctx_len,
            .autonomy_level = agent->autonomy_level,
            .custom_instructions = agent->custom_instructions,
            .custom_instructions_len = agent->custom_instructions_len,
            .persona_prompt = persona_prompt,
            .persona_prompt_len = persona_prompt_len,
            .awareness_context = awareness_ctx,
            .awareness_context_len = awareness_ctx_len,
            .outcome_context = outcome_ctx,
            .outcome_context_len = outcome_ctx_len,
            .persona_immersive = (persona_prompt && persona_prompt_len > 0),
            .persona =
#ifdef HU_HAS_PERSONA
                agent->persona
#else
                NULL
#endif
            ,
            .contact_context = agent->contact_context,
            .contact_context_len = agent->contact_context_len,
            .conversation_context = agent->conversation_context,
            .conversation_context_len = agent->conversation_context_len,
            .max_response_chars = agent->max_response_chars,
        };
        err = hu_prompt_build_system(agent->alloc, &cfg, &system_prompt, &system_prompt_len);
        if (persona_prompt)
            agent->alloc->free(agent->alloc->ctx, persona_prompt, persona_prompt_len + 1);
        persona_prompt = NULL;
        if (memory_ctx)
            agent->alloc->free(agent->alloc->ctx, memory_ctx, memory_ctx_len + 1);
        if (awareness_ctx)
            agent->alloc->free(agent->alloc->ctx, awareness_ctx, awareness_ctx_len + 1);
        if (outcome_ctx)
            agent->alloc->free(agent->alloc->ctx, outcome_ctx, outcome_ctx_len + 1);
        if (err != HU_OK) {
            hu_agent_clear_current_for_tools();
            return err;
        }
    }

    hu_chat_message_t *msgs = NULL;
    size_t msgs_count = 0;
    err = hu_context_format_messages(agent->alloc, agent->history, agent->history_count,
                                     agent->max_history_messages, &msgs, &msgs_count);
    if (err != HU_OK) {
        if (system_prompt)
            agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
        return err;
    }

    size_t total_msgs = (msgs ? msgs_count : 0) + 1;
    hu_chat_message_t *all_msgs = (hu_chat_message_t *)agent->alloc->alloc(
        agent->alloc->ctx, total_msgs * sizeof(hu_chat_message_t));
    if (!all_msgs) {
        if (system_prompt)
            agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
        if (msgs)
            agent->alloc->free(agent->alloc->ctx, msgs, msgs_count * sizeof(hu_chat_message_t));
        return HU_ERR_OUT_OF_MEMORY;
    }
    all_msgs[0].role = HU_ROLE_SYSTEM;
    all_msgs[0].content = system_prompt;
    all_msgs[0].content_len = system_prompt_len;
    all_msgs[0].name = NULL;
    all_msgs[0].name_len = 0;
    all_msgs[0].tool_call_id = NULL;
    all_msgs[0].tool_call_id_len = 0;
    all_msgs[0].content_parts = NULL;
    all_msgs[0].content_parts_count = 0;
    for (size_t i = 0; i < (msgs ? msgs_count : 0); i++)
        all_msgs[i + 1] = msgs[i];
    if (msgs)
        agent->alloc->free(agent->alloc->ctx, msgs, msgs_count * sizeof(hu_chat_message_t));
    msgs = all_msgs;
    msgs_count = total_msgs;

    hu_chat_request_t req;
    memset(&req, 0, sizeof(req));
    req.messages = msgs;
    req.messages_count = msgs_count;
    req.model = agent->model_name;
    req.model_len = agent->model_name_len;
    req.temperature = agent->temperature;
    req.tools = (agent->tool_specs_count > 0) ? agent->tool_specs : NULL;
    req.tools_count = agent->tool_specs_count;

    {
        stream_token_wrap_t wrap = {.on_token = on_token, .token_ctx = token_ctx};
        hu_stream_chat_result_t sresp;
        memset(&sresp, 0, sizeof(sresp));
        err = agent->provider.vtable->stream_chat(
            agent->provider.ctx, agent->alloc, &req, agent->model_name, agent->model_name_len,
            agent->temperature, stream_chunk_to_token_cb, &wrap, &sresp);
        if (msgs)
            agent->alloc->free(agent->alloc->ctx, msgs, msgs_count * sizeof(hu_chat_message_t));
        if (system_prompt)
            agent->alloc->free(agent->alloc->ctx, system_prompt, system_prompt_len + 1);
        if (err != HU_OK) {
            hu_agent_clear_current_for_tools();
            return err;
        }
        agent->total_tokens += sresp.usage.total_tokens;
        hu_agent_internal_record_cost(agent, &sresp.usage);
        if (sresp.content && sresp.content_len > 0) {
            (void)hu_agent_internal_append_history(agent, HU_ROLE_ASSISTANT, sresp.content,
                                                   sresp.content_len, NULL, 0, NULL, 0);
            *response_out = hu_strndup(agent->alloc, sresp.content, sresp.content_len);
            hu_agent_internal_maybe_tts(agent, sresp.content, sresp.content_len);
            agent->alloc->free(agent->alloc->ctx, (void *)sresp.content, sresp.content_len + 1);
            if (!*response_out)
                return HU_ERR_OUT_OF_MEMORY;
            if (response_len_out)
                *response_len_out = sresp.content_len;
        }
        hu_agent_clear_current_for_tools();
        return HU_OK;
    }
}
