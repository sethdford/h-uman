#include <stddef.h>
#include <string.h>

/* Forward declarations for embedded data arrays */
extern const unsigned char data_memory_emotion_prefixes_json[];
extern const size_t data_memory_emotion_prefixes_json_len;
extern const unsigned char data_memory_relationship_words_json[];
extern const size_t data_memory_relationship_words_json_len;
extern const unsigned char data_memory_topic_patterns_json[];
extern const size_t data_memory_topic_patterns_json_len;
extern const unsigned char data_memory_commitment_prefixes_json[];
extern const size_t data_memory_commitment_prefixes_json_len;
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
extern const unsigned char data_prompts_tone_hints_json[];
extern const size_t data_prompts_tone_hints_json_len;
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
extern const unsigned char data_channels_telegram_commands_txt[];
extern const size_t data_channels_telegram_commands_txt_len;
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
extern const unsigned char data_conversation_vulnerability_keywords_json[];
extern const size_t data_conversation_vulnerability_keywords_json_len;
extern const unsigned char data_conversation_filler_words_json[];
extern const size_t data_conversation_filler_words_json_len;
extern const unsigned char data_conversation_conversation_intros_json[];
extern const size_t data_conversation_conversation_intros_json_len;
extern const unsigned char data_conversation_time_gap_phrases_json[];
extern const size_t data_conversation_time_gap_phrases_json_len;
extern const unsigned char data_conversation_backchannel_phrases_json[];
extern const size_t data_conversation_backchannel_phrases_json_len;
extern const unsigned char data_conversation_emotional_words_json[];
extern const size_t data_conversation_emotional_words_json_len;

typedef struct {
    const char *path;
    const unsigned char *data;
    const size_t *len_ptr;
} hu_embedded_data_entry_t;

static const hu_embedded_data_entry_t hu_embedded_data_registry[] = {
    { "memory/emotion_prefixes.json", data_memory_emotion_prefixes_json, &data_memory_emotion_prefixes_json_len },
    { "memory/relationship_words.json", data_memory_relationship_words_json, &data_memory_relationship_words_json_len },
    { "memory/topic_patterns.json", data_memory_topic_patterns_json, &data_memory_topic_patterns_json_len },
    { "memory/commitment_prefixes.json", data_memory_commitment_prefixes_json, &data_memory_commitment_prefixes_json_len },
    { "memory/emotion_adjectives.json", data_memory_emotion_adjectives_json, &data_memory_emotion_adjectives_json_len },
    { "security/command_lists.json", data_security_command_lists_json, &data_security_command_lists_json_len },
    { "agent/commitment_patterns.json", data_agent_commitment_patterns_json, &data_agent_commitment_patterns_json_len },
    { "prompts/safety_rules.txt", data_prompts_safety_rules_txt, &data_prompts_safety_rules_txt_len },
    { "prompts/default_identity.txt", data_prompts_default_identity_txt, &data_prompts_default_identity_txt_len },
    { "prompts/tone_hints.json", data_prompts_tone_hints_json, &data_prompts_tone_hints_json_len },
    { "prompts/reasoning_instruction.txt", data_prompts_reasoning_instruction_txt, &data_prompts_reasoning_instruction_txt_len },
    { "prompts/autonomy_full.txt", data_prompts_autonomy_full_txt, &data_prompts_autonomy_full_txt_len },
    { "prompts/group_chat_hint.txt", data_prompts_group_chat_hint_txt, &data_prompts_group_chat_hint_txt_len },
    { "prompts/persona_reinforcement.txt", data_prompts_persona_reinforcement_txt, &data_prompts_persona_reinforcement_txt_len },
    { "prompts/autonomy_supervised.txt", data_prompts_autonomy_supervised_txt, &data_prompts_autonomy_supervised_txt_len },
    { "prompts/autonomy_readonly.txt", data_prompts_autonomy_readonly_txt, &data_prompts_autonomy_readonly_txt_len },
    { "persona/circadian_phases.json", data_persona_circadian_phases_json, &data_persona_circadian_phases_json_len },
    { "persona/relationship_stages.json", data_persona_relationship_stages_json, &data_persona_relationship_stages_json_len },
    { "channels/telegram_commands.txt", data_channels_telegram_commands_txt, &data_channels_telegram_commands_txt_len },
    { "conversation/personal_sharing.json", data_conversation_personal_sharing_json, &data_conversation_personal_sharing_json_len },
    { "conversation/starters.json", data_conversation_starters_json, &data_conversation_starters_json_len },
    { "conversation/ai_disclosure_patterns.json", data_conversation_ai_disclosure_patterns_json, &data_conversation_ai_disclosure_patterns_json_len },
    { "conversation/contractions.json", data_conversation_contractions_json, &data_conversation_contractions_json_len },
    { "conversation/crisis_keywords.json", data_conversation_crisis_keywords_json, &data_conversation_crisis_keywords_json_len },
    { "conversation/engage_words.json", data_conversation_engage_words_json, &data_conversation_engage_words_json_len },
    { "conversation/vulnerability_keywords.json", data_conversation_vulnerability_keywords_json, &data_conversation_vulnerability_keywords_json_len },
    { "conversation/filler_words.json", data_conversation_filler_words_json, &data_conversation_filler_words_json_len },
    { "conversation/conversation_intros.json", data_conversation_conversation_intros_json, &data_conversation_conversation_intros_json_len },
    { "conversation/time_gap_phrases.json", data_conversation_time_gap_phrases_json, &data_conversation_time_gap_phrases_json_len },
    { "conversation/backchannel_phrases.json", data_conversation_backchannel_phrases_json, &data_conversation_backchannel_phrases_json_len },
    { "conversation/emotional_words.json", data_conversation_emotional_words_json, &data_conversation_emotional_words_json_len },
    { NULL, NULL, NULL }  /* Sentinel */
};

static const size_t hu_embedded_data_count = 31;  /* excluding sentinel */

typedef struct {
    const char *path;
    const unsigned char *data;
    size_t len;
} hu_embedded_data_result_t;

static hu_embedded_data_result_t hu_embedded_data_result;

const hu_embedded_data_result_t *hu_embedded_data_lookup(const char *path) {
    if (path == NULL)
        return NULL;

    for (size_t i = 0; i < hu_embedded_data_count; i++) {
        if (strcmp(hu_embedded_data_registry[i].path, path) == 0) {
            hu_embedded_data_result.path = hu_embedded_data_registry[i].path;
            hu_embedded_data_result.data = hu_embedded_data_registry[i].data;
            hu_embedded_data_result.len = *hu_embedded_data_registry[i].len_ptr;
            return &hu_embedded_data_result;
        }
    }

    return NULL;
}
