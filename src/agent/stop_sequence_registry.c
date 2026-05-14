#include "human/agent/stop_sequence_registry.h"
#include <stddef.h>
#include <string.h>

static const char *const ANTHROPIC_STOPS[] = {"\n\nHuman:", "\n\nUser:"};
static const char *const OPENAI_STOPS[] = {"\n\nUser:", "\nUser:"};
static const char *const GEMINI_STOPS[] = {"\n\nUser:", "\nUser:"};
static const char *const OLLAMA_STOPS[] = {"<|eot_id|>", "<|im_end|>", "\nUser:"};

typedef struct {
    const char *provider;
    const char *const *seqs;
    size_t count;
} provider_entry_t;

static const provider_entry_t PROVIDER_TABLE[] = {
    {"anthropic", ANTHROPIC_STOPS, sizeof(ANTHROPIC_STOPS) / sizeof(ANTHROPIC_STOPS[0])},
    {"openai", OPENAI_STOPS, sizeof(OPENAI_STOPS) / sizeof(OPENAI_STOPS[0])},
    {"openrouter", OPENAI_STOPS, sizeof(OPENAI_STOPS) / sizeof(OPENAI_STOPS[0])},
    {"gemini", GEMINI_STOPS, sizeof(GEMINI_STOPS) / sizeof(GEMINI_STOPS[0])},
    {"ollama", OLLAMA_STOPS, sizeof(OLLAMA_STOPS) / sizeof(OLLAMA_STOPS[0])},
};
static const size_t PROVIDER_TABLE_COUNT = sizeof(PROVIDER_TABLE) / sizeof(PROVIDER_TABLE[0]);

hu_error_t hu_stop_sequence_registry_lookup(const char *provider, size_t provider_len,
                                            const char *channel, size_t channel_len,
                                            const char *const **out_seqs, size_t *out_seqs_count) {
    (void)channel;
    (void)channel_len;
    if (!out_seqs || !out_seqs_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_seqs = NULL;
    *out_seqs_count = 0;
    if (!provider)
        return HU_OK;
    for (size_t i = 0; i < PROVIDER_TABLE_COUNT; i++) {
        size_t plen = strlen(PROVIDER_TABLE[i].provider);
        if (plen == provider_len && memcmp(PROVIDER_TABLE[i].provider, provider, plen) == 0) {
            *out_seqs = PROVIDER_TABLE[i].seqs;
            *out_seqs_count = PROVIDER_TABLE[i].count;
            return HU_OK;
        }
    }
    return HU_OK;
}
