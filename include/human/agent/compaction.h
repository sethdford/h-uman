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

typedef struct hu_compaction_config {
    uint32_t keep_recent;          /* keep this many most-recent messages */
    uint32_t max_summary_chars;    /* max chars in compaction summary */
    uint32_t max_source_chars;     /* max chars from source when building summary */
    uint64_t token_limit;          /* 0 = no token-based trigger */
    uint32_t max_history_messages; /* message count trigger */
} hu_compaction_config_t;

/* Default config values */
#define HU_COMPACTION_DEFAULT_KEEP_RECENT       20
#define HU_COMPACTION_DEFAULT_MAX_SUMMARY_CHARS 2000
#define HU_COMPACTION_DEFAULT_MAX_SOURCE_CHARS  12000
#define HU_COMPACTION_DEFAULT_TOKEN_LIMIT       32000
#define HU_COMPACTION_DEFAULT_MAX_HISTORY       50

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
hu_error_t hu_compact_history(hu_allocator_t *alloc, hu_owned_message_t *history,
                              size_t *history_count, size_t *history_cap,
                              const hu_compaction_config_t *config);

/* LLM-enhanced compaction: sends old messages to the provider for summarization.
 * Falls back to rule-based compaction if provider is NULL or the LLM call fails.
 * provider/alloc/history semantics are the same as hu_compact_history. */
hu_error_t hu_compact_history_llm(hu_allocator_t *alloc, hu_owned_message_t *history,
                                  size_t *history_count, size_t *history_cap,
                                  const hu_compaction_config_t *config, hu_provider_t *provider);

/* Pressure-based compaction: remove oldest non-system messages until pressure < target.
 * Replaces removed messages with "[Previous context compacted: N messages summarized]".
 * Preserves system prompt and most recent messages. */
hu_error_t hu_context_compact_for_pressure(hu_allocator_t *alloc, hu_owned_message_t *history,
                                           size_t *history_count, size_t *history_cap,
                                           size_t max_tokens, float target_pressure);

#endif /* HU_AGENT_COMPACTION_H */
