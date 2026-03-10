#include <stddef.h>
#include <string.h>

/* Forward declarations for embedded data arrays */
extern const unsigned char data_memory_relationship_words_json[];
extern const size_t data_memory_relationship_words_json_len;
extern const unsigned char data_memory_emotion_adjectives_json[];
extern const size_t data_memory_emotion_adjectives_json_len;
extern const unsigned char data_security_command_lists_json[];
extern const size_t data_security_command_lists_json_len;
extern const unsigned char data_agent_commitment_patterns_json[];
extern const size_t data_agent_commitment_patterns_json_len;
extern const unsigned char data_prompts_safety_rules_txt[];
extern const size_t data_prompts_safety_rules_txt_len;
extern const unsigned char data_prompts_default_identity_txt[];
extern const size_t data_prompts_default_identity_txt_len;
extern const unsigned char data_prompts_reasoning_instruction_txt[];
extern const size_t data_prompts_reasoning_instruction_txt_len;
extern const unsigned char data_prompts_autonomy_full_txt[];
extern const size_t data_prompts_autonomy_full_txt_len;
extern const unsigned char data_prompts_group_chat_hint_txt[];
extern const size_t data_prompts_group_chat_hint_txt_len;
extern const unsigned char data_prompts_persona_reinforcement_txt[];
extern const size_t data_prompts_persona_reinforcement_txt_len;
extern const unsigned char data_prompts_autonomy_supervised_txt[];
extern const size_t data_prompts_autonomy_supervised_txt_len;
extern const unsigned char data_prompts_autonomy_readonly_txt[];
extern const size_t data_prompts_autonomy_readonly_txt_len;
extern const unsigned char data_persona_circadian_phases_json[];
extern const size_t data_persona_circadian_phases_json_len;
extern const unsigned char data_persona_relationship_stages_json[];
extern const size_t data_persona_relationship_stages_json_len;
extern const unsigned char data_conversation_personal_sharing_json[];
extern const size_t data_conversation_personal_sharing_json_len;
extern const unsigned char data_conversation_starters_json[];
extern const size_t data_conversation_starters_json_len;
extern const unsigned char data_conversation_ai_disclosure_patterns_json[];
extern const size_t data_conversation_ai_disclosure_patterns_json_len;
extern const unsigned char data_conversation_contractions_json[];
extern const size_t data_conversation_contractions_json_len;
extern const unsigned char data_conversation_crisis_keywords_json[];
extern const size_t data_conversation_crisis_keywords_json_len;
extern const unsigned char data_conversation_engage_words_json[];
extern const size_t data_conversation_engage_words_json_len;
extern const unsigned char data_conversation_filler_words_json[];
extern const size_t data_conversation_filler_words_json_len;
extern const unsigned char data_conversation_conversation_intros_json[];
extern const size_t data_conversation_conversation_intros_json_len;
extern const unsigned char data_conversation_backchannel_phrases_json[];
extern const size_t data_conversation_backchannel_phrases_json_len;
extern const unsigned char data_conversation_emotional_words_json[];
extern const size_t data_conversation_emotional_words_json_len;

typedef struct {
    const char *path;
    const unsigned char *data;
    size_t len;
} hu_embedded_data_entry_t;

/* Helper macro to initialize entries with length set at runtime */
#define HU_EMBEDDED_ENTRY(path_str, var) \
    { .path = (path_str), .data = (var), .len = 0 }

static hu_embedded_data_entry_t hu_embedded_data_registry[] = {
    HU_EMBEDDED_ENTRY("memory/relationship_words.json", data_memory_relationship_words_json),
    HU_EMBEDDED_ENTRY("memory/emotion_adjectives.json", data_memory_emotion_adjectives_json),
    HU_EMBEDDED_ENTRY("security/command_lists.json", data_security_command_lists_json),
    HU_EMBEDDED_ENTRY("agent/commitment_patterns.json", data_agent_commitment_patterns_json),
    HU_EMBEDDED_ENTRY("prompts/safety_rules.txt", data_prompts_safety_rules_txt),
    HU_EMBEDDED_ENTRY("prompts/default_identity.txt", data_prompts_default_identity_txt),
    HU_EMBEDDED_ENTRY("prompts/reasoning_instruction.txt", data_prompts_reasoning_instruction_txt),
    HU_EMBEDDED_ENTRY("prompts/autonomy_full.txt", data_prompts_autonomy_full_txt),
    HU_EMBEDDED_ENTRY("prompts/group_chat_hint.txt", data_prompts_group_chat_hint_txt),
    HU_EMBEDDED_ENTRY("prompts/persona_reinforcement.txt", data_prompts_persona_reinforcement_txt),
    HU_EMBEDDED_ENTRY("prompts/autonomy_supervised.txt", data_prompts_autonomy_supervised_txt),
    HU_EMBEDDED_ENTRY("prompts/autonomy_readonly.txt", data_prompts_autonomy_readonly_txt),
    HU_EMBEDDED_ENTRY("persona/circadian_phases.json", data_persona_circadian_phases_json),
    HU_EMBEDDED_ENTRY("persona/relationship_stages.json", data_persona_relationship_stages_json),
    HU_EMBEDDED_ENTRY("conversation/personal_sharing.json", data_conversation_personal_sharing_json),
    HU_EMBEDDED_ENTRY("conversation/starters.json", data_conversation_starters_json),
    HU_EMBEDDED_ENTRY("conversation/ai_disclosure_patterns.json", data_conversation_ai_disclosure_patterns_json),
    HU_EMBEDDED_ENTRY("conversation/contractions.json", data_conversation_contractions_json),
    HU_EMBEDDED_ENTRY("conversation/crisis_keywords.json", data_conversation_crisis_keywords_json),
    HU_EMBEDDED_ENTRY("conversation/engage_words.json", data_conversation_engage_words_json),
    HU_EMBEDDED_ENTRY("conversation/filler_words.json", data_conversation_filler_words_json),
    HU_EMBEDDED_ENTRY("conversation/conversation_intros.json", data_conversation_conversation_intros_json),
    HU_EMBEDDED_ENTRY("conversation/backchannel_phrases.json", data_conversation_backchannel_phrases_json),
    HU_EMBEDDED_ENTRY("conversation/emotional_words.json", data_conversation_emotional_words_json),
    { .path = NULL, .data = NULL, .len = 0 }  /* Sentinel */
};

static const size_t hu_embedded_data_count = 24;  /* excluding sentinel */

const hu_embedded_data_entry_t *hu_embedded_data_lookup(const char *path) {
    if (path == NULL)
        return NULL;

    for (size_t i = 0; i < hu_embedded_data_count; i++) {
        if (strcmp(hu_embedded_data_registry[i].path, path) == 0) {
            /* Set the length from the associated extern variable */
            if (strcmp(path, "memory/relationship_words.json") == 0) {
                hu_embedded_data_registry[i].len = data_memory_relationship_words_json_len;
            }
            if (strcmp(path, "memory/emotion_adjectives.json") == 0) {
                hu_embedded_data_registry[i].len = data_memory_emotion_adjectives_json_len;
            }
            if (strcmp(path, "security/command_lists.json") == 0) {
                hu_embedded_data_registry[i].len = data_security_command_lists_json_len;
            }
            if (strcmp(path, "agent/commitment_patterns.json") == 0) {
                hu_embedded_data_registry[i].len = data_agent_commitment_patterns_json_len;
            }
            if (strcmp(path, "prompts/safety_rules.txt") == 0) {
                hu_embedded_data_registry[i].len = data_prompts_safety_rules_txt_len;
            }
            if (strcmp(path, "prompts/default_identity.txt") == 0) {
                hu_embedded_data_registry[i].len = data_prompts_default_identity_txt_len;
            }
            if (strcmp(path, "prompts/reasoning_instruction.txt") == 0) {
                hu_embedded_data_registry[i].len = data_prompts_reasoning_instruction_txt_len;
            }
            if (strcmp(path, "prompts/autonomy_full.txt") == 0) {
                hu_embedded_data_registry[i].len = data_prompts_autonomy_full_txt_len;
            }
            if (strcmp(path, "prompts/group_chat_hint.txt") == 0) {
                hu_embedded_data_registry[i].len = data_prompts_group_chat_hint_txt_len;
            }
            if (strcmp(path, "prompts/persona_reinforcement.txt") == 0) {
                hu_embedded_data_registry[i].len = data_prompts_persona_reinforcement_txt_len;
            }
            if (strcmp(path, "prompts/autonomy_supervised.txt") == 0) {
                hu_embedded_data_registry[i].len = data_prompts_autonomy_supervised_txt_len;
            }
            if (strcmp(path, "prompts/autonomy_readonly.txt") == 0) {
                hu_embedded_data_registry[i].len = data_prompts_autonomy_readonly_txt_len;
            }
            if (strcmp(path, "persona/circadian_phases.json") == 0) {
                hu_embedded_data_registry[i].len = data_persona_circadian_phases_json_len;
            }
            if (strcmp(path, "persona/relationship_stages.json") == 0) {
                hu_embedded_data_registry[i].len = data_persona_relationship_stages_json_len;
            }
            if (strcmp(path, "conversation/personal_sharing.json") == 0) {
                hu_embedded_data_registry[i].len = data_conversation_personal_sharing_json_len;
            }
            if (strcmp(path, "conversation/starters.json") == 0) {
                hu_embedded_data_registry[i].len = data_conversation_starters_json_len;
            }
            if (strcmp(path, "conversation/ai_disclosure_patterns.json") == 0) {
                hu_embedded_data_registry[i].len = data_conversation_ai_disclosure_patterns_json_len;
            }
            if (strcmp(path, "conversation/contractions.json") == 0) {
                hu_embedded_data_registry[i].len = data_conversation_contractions_json_len;
            }
            if (strcmp(path, "conversation/crisis_keywords.json") == 0) {
                hu_embedded_data_registry[i].len = data_conversation_crisis_keywords_json_len;
            }
            if (strcmp(path, "conversation/engage_words.json") == 0) {
                hu_embedded_data_registry[i].len = data_conversation_engage_words_json_len;
            }
            if (strcmp(path, "conversation/filler_words.json") == 0) {
                hu_embedded_data_registry[i].len = data_conversation_filler_words_json_len;
            }
            if (strcmp(path, "conversation/conversation_intros.json") == 0) {
                hu_embedded_data_registry[i].len = data_conversation_conversation_intros_json_len;
            }
            if (strcmp(path, "conversation/backchannel_phrases.json") == 0) {
                hu_embedded_data_registry[i].len = data_conversation_backchannel_phrases_json_len;
            }
            if (strcmp(path, "conversation/emotional_words.json") == 0) {
                hu_embedded_data_registry[i].len = data_conversation_emotional_words_json_len;
            }
            return &hu_embedded_data_registry[i];
        }
    }

    return NULL;
}
