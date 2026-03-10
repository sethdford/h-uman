#ifndef HU_AGENT_PROMPT_H
#define HU_AGENT_PROMPT_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/tool.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct hu_persona; /* forward declaration; avoid circular deps */

/* ──────────────────────────────────────────────────────────────────────────
 * System prompt builder — identity, tools, memory, datetime, constraints
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_prompt_config {
    const char *provider_name;
    size_t provider_name_len;
    const char *model_name;
    size_t model_name_len;
    const char *workspace_dir;
    size_t workspace_dir_len;
    hu_tool_t *tools;
    size_t tools_count;
    const char *memory_context;
    size_t memory_context_len;
    const char *stm_context;
    size_t stm_context_len;
    const char *commitment_context;
    size_t commitment_context_len;
    const char *pattern_context;
    size_t pattern_context_len;
    const char *adaptive_persona_context;
    size_t adaptive_persona_context_len;
    const char *proactive_context;
    size_t proactive_context_len;
    const char *superhuman_context;
    size_t superhuman_context_len;
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
    uint32_t max_response_chars;      /* 0 = unlimited */
    const struct hu_persona *persona; /* persona struct for externalized prompt fields */
    const char *safety_rules;
    size_t safety_rules_len;
    const char *autonomy_rules;
    size_t autonomy_rules_len;
    const char *reasoning_instruction;
    size_t reasoning_instruction_len;
} hu_prompt_config_t;

/* Build the full system prompt. Caller owns returned string; free with alloc. */
hu_error_t hu_prompt_build_system(hu_allocator_t *alloc, const hu_prompt_config_t *config,
                                  char **out, size_t *out_len);

/* Build only the static parts (identity, tools, autonomy, safety, custom).
 * The result can be cached and reused across turns. Caller owns returned string. */
hu_error_t hu_prompt_build_static(hu_allocator_t *alloc, const hu_prompt_config_t *config,
                                  char **out, size_t *out_len);

/* Build full prompt by combining cached static part with dynamic memory context.
 * Avoids rebuilding the static portion each turn. Caller owns returned string. */
hu_error_t hu_prompt_build_with_cache(hu_allocator_t *alloc, const char *static_prompt,
                                      size_t static_prompt_len, const char *memory_context,
                                      size_t memory_context_len, char **out, size_t *out_len);

/* Tone detection — analyze recent user messages to detect communication style.
 * Returns a static string hint suitable for tone_hint field. */
typedef enum hu_tone {
    HU_TONE_NEUTRAL,
    HU_TONE_CASUAL,
    HU_TONE_TECHNICAL,
    HU_TONE_FORMAL,
} hu_tone_t;

hu_tone_t hu_detect_tone(const char *const *user_messages, const size_t *message_lens,
                         size_t count);

const char *hu_tone_hint_string(hu_tone_t tone, size_t *out_len);

#endif /* HU_AGENT_PROMPT_H */
