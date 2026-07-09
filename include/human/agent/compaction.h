#ifndef HU_AGENT_COMPACTION_H
#define HU_AGENT_COMPACTION_H

#include "human/agent.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Agent Compaction — context window management
 * ────────────────────────────────────────────────────────────────────────── */

/* Forward declare structured types (full defs in compaction_structured.h) */
struct hu_artifact_pin;

typedef struct hu_compaction_config {
    uint32_t keep_recent;          /* keep this many most-recent messages */
    uint32_t max_summary_chars;    /* max chars in compaction summary */
    uint32_t max_source_chars;     /* max chars from source when building summary */
    uint64_t token_limit;          /* 0 = no token-based trigger */
    uint32_t max_history_messages; /* message count trigger */

    /* Structured compaction v2 fields */
    bool use_structured_summary;       /* emit XML <summary> instead of plain text */
    uint32_t preserve_recent_count;    /* override keep_recent for structured mode */
    bool inject_continuation_preamble; /* prepend continuation context after compaction */
    struct hu_artifact_pin *pinned_artifacts; /* borrowed; messages referencing these are kept */
    size_t pinned_artifacts_count;
} hu_compaction_config_t;

/* Default config values */
#define HU_COMPACTION_DEFAULT_KEEP_RECENT       20
#define HU_COMPACTION_DEFAULT_MAX_SUMMARY_CHARS 2000
#define HU_COMPACTION_DEFAULT_MAX_SOURCE_CHARS  12000
#define HU_COMPACTION_DEFAULT_TOKEN_LIMIT       32000
/* Conversation-history window fed into the prompt. Bumped 50 -> 120 (2026-06-06):
 * the SQLite session store holds hundreds of messages per contact, but only the
 * last N reach the prompt. 50 (~25 exchanges) made long iMessage threads lose
 * earlier context. 120 casual messages is ~3-6 KB (well under the 32K history
 * token budget that still governs compaction), so this widens coherence at
 * negligible token cost. */
#define HU_COMPACTION_DEFAULT_MAX_HISTORY       120

/* Initialize config with defaults. */
void hu_compaction_config_default(hu_compaction_config_t *cfg);

/* Estimate total tokens in history: (sum(content_len) + 3*count) / 4. */
uint64_t hu_estimate_tokens(const hu_owned_message_t *history, size_t history_count);

/* Check if history exceeds token or message limits and should be compacted. */
bool hu_should_compact(const hu_owned_message_t *history, size_t history_count,
                       const hu_compaction_config_t *config);

/* Compact history: keep N most recent, summarize older into single message.
 * Summary is concatenation of key points (role: content) — not LLM-generated.
 * Modifies history in place. Returns HU_OK on success. */
hu_error_t hu_compact_history(hu_allocator_t *alloc, hu_owned_message_t **history,
                              size_t *history_count, size_t *history_cap,
                              const hu_compaction_config_t *config);

/* LLM-enhanced compaction: sends old messages to the provider for summarization.
 * Falls back to rule-based compaction if provider is NULL or the LLM call fails.
 * provider/alloc/history semantics are the same as hu_compact_history. */
hu_error_t hu_compact_history_llm(hu_allocator_t *alloc, hu_owned_message_t **history,
                                  size_t *history_count, size_t *history_cap,
                                  const hu_compaction_config_t *config, hu_provider_t *provider);

/* Pressure-based compaction: remove oldest non-system messages until pressure < target.
 * Replaces removed messages with "[Previous context compacted: N messages summarized]".
 * Preserves system prompt and most recent messages. */
hu_error_t hu_context_compact_for_pressure(hu_allocator_t *alloc, hu_owned_message_t *history,
                                           size_t *history_count, size_t *history_cap,
                                           size_t max_tokens, float target_pressure);

/* Hierarchical summarization: session (~200 words) → chapter (~100 words) → overall (~50 words).
 * Each stage uses the provider's chat_with_system (or fixed mocks when HU_IS_TEST).
 * Caller frees *session_summary, *chapter_summary, *overall_summary. */
hu_error_t hu_compact_hierarchical(hu_allocator_t *alloc, hu_provider_t *provider,
                                   const char *model, size_t model_len,
                                   const char *conversation, size_t conversation_len,
                                   char **session_summary, size_t *session_len,
                                   char **chapter_summary, size_t *chapter_len,
                                   char **overall_summary, size_t *overall_len);

/* Hierarchical compaction: groups messages into chunks, summarizes each chunk,
 * then recursively summarizes summaries until the result fits within limits.
 * chunk_size: messages per chunk (0 = default 10). max_depth: recursion limit (0 = default 3). */
hu_error_t hu_compact_history_hierarchical(hu_allocator_t *alloc, hu_owned_message_t **history,
                                           size_t *history_count, size_t *history_cap,
                                           const hu_compaction_config_t *config,
                                           hu_provider_t *provider,
                                           uint32_t chunk_size, uint32_t max_depth);

#endif /* HU_AGENT_COMPACTION_H */
