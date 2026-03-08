#ifndef SC_AGENT_PROMPT_H
#define SC_AGENT_PROMPT_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/tool.h"
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * System prompt builder — identity, tools, memory, datetime, constraints
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct sc_prompt_config {
    const char *provider_name;
    size_t provider_name_len;
    const char *model_name;
    size_t model_name_len;
    const char *workspace_dir;
    size_t workspace_dir_len;
    sc_tool_t *tools;
    size_t tools_count;
    const char *memory_context;
    size_t memory_context_len;
    const char *stm_context;
    size_t stm_context_len;
    uint8_t autonomy_level; /* 0=readonly, 1=supervised, 2=full */
    const char *custom_instructions;
    size_t custom_instructions_len;
    const char *persona_prompt; /* overrides default identity when set */
    size_t persona_prompt_len;
    const char *preferences; /* user preference rules */
    size_t preferences_len;
    bool chain_of_thought; /* inject reasoning instructions */
    const char *tone_hint; /* adaptive tone directive */
    size_t tone_hint_len;
    const char *awareness_context; /* situational awareness (channels, errors, health) */
    size_t awareness_context_len;
    const char *outcome_context; /* outcome tracker summary (tool success rates, corrections) */
    size_t outcome_context_len;
    bool persona_immersive;      /* suppress AI-assistant framing for deep persona mode */
    const char *contact_context; /* per-contact profile context (from persona contacts) */
    size_t contact_context_len;
    const char *conversation_context; /* conversation history + awareness (from channel history) */
    size_t conversation_context_len;
    uint32_t max_response_chars; /* 0 = unlimited */
} sc_prompt_config_t;

/* Build the full system prompt. Caller owns returned string; free with alloc. */
sc_error_t sc_prompt_build_system(sc_allocator_t *alloc, const sc_prompt_config_t *config,
                                  char **out, size_t *out_len);

/* Build only the static parts (identity, tools, autonomy, safety, custom).
 * The result can be cached and reused across turns. Caller owns returned string. */
sc_error_t sc_prompt_build_static(sc_allocator_t *alloc, const sc_prompt_config_t *config,
                                  char **out, size_t *out_len);

/* Build full prompt by combining cached static part with dynamic memory context.
 * Avoids rebuilding the static portion each turn. Caller owns returned string. */
sc_error_t sc_prompt_build_with_cache(sc_allocator_t *alloc, const char *static_prompt,
                                      size_t static_prompt_len, const char *memory_context,
                                      size_t memory_context_len, char **out, size_t *out_len);

/* Tone detection — analyze recent user messages to detect communication style.
 * Returns a static string hint suitable for tone_hint field. */
typedef enum sc_tone {
    SC_TONE_NEUTRAL,
    SC_TONE_CASUAL,
    SC_TONE_TECHNICAL,
    SC_TONE_FORMAL,
} sc_tone_t;

sc_tone_t sc_detect_tone(const char *const *user_messages, const size_t *message_lens,
                         size_t count);

const char *sc_tone_hint_string(sc_tone_t tone, size_t *out_len);

#endif /* SC_AGENT_PROMPT_H */
