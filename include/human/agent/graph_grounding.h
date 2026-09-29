#ifndef HU_AGENT_GRAPH_GROUNDING_H
#define HU_AGENT_GRAPH_GROUNDING_H

#include "human/agent/memory_loader.h"
#include "human/core/error.h"
#include "human/core/gate_mode.h"
#include <stdbool.h>
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

/* ── Contact-anchored fallback (2026-09-27) ───────────────────────────────
 * Lexical seeding needs the incoming message to NAME an entity; casual texts
 * almost never do (0 of 40 real moments, 2026-09-27), so compose returned
 * nothing even for contacts with a populated graph. With
 * HU_GG_CONTACT_FALLBACK, a lexical miss seeds instead from the contact's own
 * top entities (mention count + recency) and renders them the same way.
 * A lexical hit is unaffected. *out_matched_entities stays 0 on the fallback
 * path so logs can tell the two apart. */
#define HU_GG_CONTACT_FALLBACK 0x1u

/* True when an entity name is not a referent and must never seed the
 * fallback: a pronoun-like placeholder the extractors write as a subject
 * ("user", "assistant", "you", "me", "i", "we", "they", "someone"; exact,
 * case-insensitive, whole name only), the contact's own id, or a
 * phone-number-shaped string. Measured 2026-09-28: for 3 of 4 active
 * contacts the fallback's #1 seed by mention count was such a row. */
bool hu_graph_ground_is_placeholder_name(const char *name, size_t name_len, const char *contact_id,
                                         size_t contact_id_len);

/* Seed an entity only when EVERY scoreable word of its name appears in the
 * message (coverage 1.0), not on a lone shared word. Used for the owner's
 * ("self") facts, where topic phrases like "different direction" would
 * otherwise match any message saying "different". */
#define HU_GG_REQUIRE_FULL_NAME 0x2u

/* Reads HU_GRAPH_GROUNDING_SELF_FACTS per hu_gate_mode_parse, unset -> OFF.
 * Owner ("self") facts are matched by full name against the incoming
 * message and, in LIVE, appended under an "About you:" label. */
hu_gate_mode_t hu_graph_grounding_self_facts_mode(void);

hu_error_t hu_graph_ground_compose_ex(hu_memory_loader_t *loader, const char *contact_id,
                                      size_t contact_id_len, const char *msg, size_t msg_len,
                                      size_t max_chars, unsigned flags, char **out, size_t *out_len,
                                      size_t *out_matched_entities);

typedef enum hu_graph_grounding_fallback_mode {
    HU_GG_FALLBACK_OFF = 0,
    HU_GG_FALLBACK_SHADOW,
    HU_GG_FALLBACK_LIVE,
} hu_graph_grounding_fallback_mode_t;

/* Reads HU_GRAPH_GROUNDING_CONTACT_FALLBACK per hu_gate_mode_parse:
 * unset -> OFF (default), "shadow" -> SHADOW, "on"/"live" -> LIVE,
 * unknown -> OFF. */
hu_graph_grounding_fallback_mode_t hu_graph_grounding_contact_fallback_mode(void);

/* ── Turn composition (spec 2026-09-29 §4.6) ───────────────────────────────
 * The lexical -> contact-fallback -> owner-facts composition that
 * hu_agent_load_graph_grounding injects (before its tier gate), with no agent
 * dependency, so `human memory ground --full` measures the real path. Gates
 * arrive as a bitmask (hu_graph_ground_turn_flags_from_env) so tests need no
 * setenv; SHADOW sub-gates are reported in stats and never injected, and the
 * caller logs them. */
#define HU_GG_TURN_FALLBACK_SHADOW 0x01u
#define HU_GG_TURN_FALLBACK_LIVE   0x02u
#define HU_GG_TURN_SELF_SHADOW     0x04u
#define HU_GG_TURN_SELF_LIVE       0x08u

typedef struct hu_graph_ground_turn_stats {
    size_t matched_entities; /* lexical seeds of the contact block */
    bool via_fallback;       /* the contact fallback supplied the block */
    bool via_self;           /* an "About you:" owner block was appended */
    bool fallback_shadow;    /* fallback composed in SHADOW (not injected) */
    size_t fallback_shadow_bytes;
    uint32_t fallback_shadow_fp;
    bool self_shadow; /* owner facts composed in SHADOW (not injected) */
    size_t self_shadow_bytes;
    uint32_t self_shadow_fp;
} hu_graph_ground_turn_stats_t;

/* HU_GRAPH_GROUNDING_CONTACT_FALLBACK and HU_GRAPH_GROUNDING_SELF_FACTS ->
 * HU_GG_TURN_* bits (unset/off/unknown -> no bit). */
unsigned hu_graph_ground_turn_flags_from_env(void);

/* Fail-open (always HU_OK). *out is NULL/0 when nothing composes; the caller
 * frees it via loader->alloc (len + 1). `stats` may be NULL. */
hu_error_t hu_graph_ground_compose_turn(hu_memory_loader_t *loader, const char *contact_id,
                                        size_t contact_id_len, const char *msg, size_t msg_len,
                                        unsigned turn_flags, char **out, size_t *out_len,
                                        hu_graph_ground_turn_stats_t *stats);

#endif /* HU_AGENT_GRAPH_GROUNDING_H */
