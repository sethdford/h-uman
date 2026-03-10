#ifndef HU_AGENT_EPISODIC_H
#define HU_AGENT_EPISODIC_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include "human/provider.h"
#include <stddef.h>
#include <stdint.h>

#define HU_EPISODIC_KEY_PREFIX     "_ep:"
#define HU_EPISODIC_KEY_PREFIX_LEN 4
#define HU_EPISODIC_MAX_SUMMARY    512
#define HU_EPISODIC_MAX_LOAD       5

typedef struct hu_episode {
    char *summary;
    size_t summary_len;
    uint64_t timestamp_ms; /* Unix epoch milliseconds */
    char *session_id;
    size_t session_id_len;
} hu_episode_t;

/* Build a short session summary from the conversation for episodic storage.
 * messages is an array of alternating user/assistant content strings.
 * Caller owns the returned string. */
char *hu_episodic_summarize_session(hu_allocator_t *alloc, const char *const *messages,
                                    const size_t *message_lens, size_t message_count,
                                    size_t *out_len);

/* LLM-based session summarization — calls the provider to produce a concise
 * 2-3 sentence summary. Falls back to rule-based on provider failure.
 * Caller owns the returned string. */
char *hu_episodic_summarize_session_llm(hu_allocator_t *alloc, hu_provider_t *provider,
                                        const char *const *messages, const size_t *message_lens,
                                        size_t message_count, size_t *out_len);

/* Store an episode into the memory backend. */
hu_error_t hu_episodic_store(hu_memory_t *memory, hu_allocator_t *alloc, const char *session_id,
                             size_t session_id_len, const char *summary, size_t summary_len);

/* Load recent episodes as formatted context for prompt injection.
 * Caller owns the returned string. */
hu_error_t hu_episodic_load(hu_memory_t *memory, hu_allocator_t *alloc, char **out,
                            size_t *out_len);

#endif /* HU_AGENT_EPISODIC_H */
