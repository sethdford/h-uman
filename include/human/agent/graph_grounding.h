#ifndef HU_AGENT_GRAPH_GROUNDING_H
#define HU_AGENT_GRAPH_GROUNDING_H

#include "human/agent/memory_loader.h"
#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

typedef enum hu_graph_grounding_mode {
    HU_GRAPH_GROUNDING_OFF = 0,
    HU_GRAPH_GROUNDING_SHADOW,
    HU_GRAPH_GROUNDING_ON,
} hu_graph_grounding_mode_t;

/* Reads HU_GRAPH_GROUNDING: unset/"off"/"0" -> OFF, "shadow" -> SHADOW,
 * "on"/"1" -> ON. Unknown values -> OFF (fail-safe). */
hu_graph_grounding_mode_t hu_graph_grounding_mode(void);

/* ── Pure retrieval-scoring predicates (no DB, no allocation) ─────────────
 * Extracted per .claude/rules/security-predicate-extraction.md so the
 * "which graph content is relevant to THIS message" decision is testable
 * without a graph store. */

/* Number of scoreable words in an entity name: alnum runs of >= 3 chars
 * that are not trivial stopwords. 0 means the entity can never match. */
size_t hu_graph_ground_name_word_count(const char *name, size_t name_len);

/* How many scoreable words of `name` appear in `msg`, matched
 * case-insensitively at WORD BOUNDARIES only ("informal" does not match
 * entity "formal"; see .claude/rules/substring-classifier-pitfalls.md). */
size_t hu_graph_ground_entity_match_count(const char *msg, size_t msg_len, const char *name,
                                          size_t name_len);

/* Relevance score for one entity against the incoming message.
 * 0.0 when match_count or name_word_count is 0 (no lexical overlap -> the
 * entity contributes nothing; empty injection is VALID and preferred over
 * generic filler). Otherwise: name-coverage ratio (dominant term, <= 1.0)
 * + bounded mention-count boost (<= 0.25) + recency decay (<= 0.25). */
double hu_graph_ground_score(size_t match_count, size_t name_word_count, int32_t mention_count,
                             int64_t last_seen_ms, int64_t now_ms);

/* Relevance fingerprint for shadow-mode logs: FNV-1a over the first 40
 * bytes of the composed context. 0 for NULL/empty. Lets the old failure
 * signature (5 distinct sizes, constant content) be distinguished from
 * conversation-varying injection directly in the log stream. */
uint32_t hu_graph_ground_fingerprint(const char *content, size_t len);

/* ── Query-conditioned composition ────────────────────────────────────────
 * Replaces the pre-2026-07 static community-summary load (top-3 summaries
 * by size, identical for every message — the 2026-07-22 shadow analysis'
 * "5 distinct sizes" failure). Selects graph entities that lexically
 * overlap `msg`, walks 1 hop, and composes a compact context block from
 * the matched nodes + their relations (including relation `context` text).
 *
 * Best-effort/fail-open: on any error, no graph, or NO MATCH, sets
 * *out=NULL, *out_len=0, *out_matched_entities=0 and returns HU_OK.
 * Caller frees *out via loader->alloc (len+1). `max_chars` caps output
 * (0 -> default 600); the injected block additionally participates in the
 * HU_PROMPT_TRIM graph span downstream. `out_matched_entities` (optional)
 * reports how many seed entities matched — the shadow-log relevance
 * signal. */
hu_error_t hu_graph_ground_compose(hu_memory_loader_t *loader, const char *contact_id,
                                   size_t contact_id_len, const char *msg, size_t msg_len,
                                   size_t max_chars, char **out, size_t *out_len,
                                   size_t *out_matched_entities);

#endif /* HU_AGENT_GRAPH_GROUNDING_H */
