#ifndef HU_MEMORY_FACT_EXTRACT_H
#define HU_MEMORY_FACT_EXTRACT_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/trust.h"
#include <stddef.h>
#include <stdint.h>

/*
 * PlugMem-style propositional fact extraction.
 *
 * Converts raw conversation text into structured knowledge units:
 * - Propositional facts: subject-predicate-object triples with confidence
 * - Prescriptive knowledge: reusable skills/preferences/patterns
 *
 * These replace text-summarization in consolidation for higher information
 * density and better retrieval via the knowledge graph.
 */

#define HU_FACT_MAX_FIELD   256
#define HU_FACT_EXTRACT_MAX 32

typedef enum hu_knowledge_type {
    HU_KNOWLEDGE_PROPOSITIONAL = 0, /* factual: "User likes hiking" */
    HU_KNOWLEDGE_PRESCRIPTIVE,      /* procedural: "When stressed, suggest walk" */
} hu_knowledge_type_t;

typedef struct hu_heuristic_fact {
    hu_knowledge_type_t type;
    char subject[HU_FACT_MAX_FIELD];
    char predicate[HU_FACT_MAX_FIELD];
    char object[HU_FACT_MAX_FIELD];
    float confidence;                    /* 0.0–1.0 extraction confidence */
    char source_hint[HU_FACT_MAX_FIELD]; /* conversation context hint */
    /* Unix timestamp (seconds) of the most recent observation that
     * supports this fact. 0 means the fact has never been refreshed
     * — typically true at extraction time, set by the personal model
     * on insert/duplicate-update. Older observations earn less prompt
     * space via `hu_heuristic_fact_effective_confidence`. */
    int64_t last_seen_at;
    /* SOTA-2026 init-09: per-fact provenance + trust tier. Stamped on
     * insert by `hu_personal_model_ingest`. Defaults to USER_DIRECT
     * when callers pass NULL (only inside #ifdef _HU_PM_SELF_TEST). */
    hu_provenance_t provenance;
    /* Sprint 48 US-48-2: contact handle (iMessage handle or similar).
     * Empty string ("") = global/contact-agnostic fact.
     * Stamped during extraction or ingest by hu_personal_model_ingest_for_contact. */
    char contact_handle[HU_FACT_MAX_FIELD];
} hu_heuristic_fact_t;

/* Default exponential half-life for fact-confidence decay. After
 * 90 days a fact's effective confidence is half of its raw value;
 * after 180 days it's a quarter; after a year it's ~6%. Tuned for
 * chat data where facts about the user (job, location, taste) are
 * relevant for a season but eventually drift. */
#define HU_FACT_CONFIDENCE_HALF_LIFE_SEC ((int64_t)(90LL * 24 * 60 * 60))

/* Effective confidence at `now` (Unix seconds). Returns
 * `fact->confidence` unchanged when `last_seen_at` is 0 (no decay
 * data) or when `now <= last_seen_at`. Otherwise applies exponential
 * decay with the half-life above. Pure, allocator-free, no math.h
 * dependency at the call site (pow2 approximation via bit shifts).
 *
 * The approximation: effective = confidence * 0.5^(age / half_life).
 * For age = 0          → 1.00 * confidence
 * For age = half_life  → 0.50 * confidence
 * For age = 2x         → 0.25 * confidence
 * For age = 4x         → 0.0625 * confidence
 * Beyond ~10 half-lives we floor at 0. */
float hu_heuristic_fact_effective_confidence(const hu_heuristic_fact_t *fact, int64_t now);

typedef struct hu_fact_extract_result {
    hu_heuristic_fact_t facts[HU_FACT_EXTRACT_MAX];
    size_t fact_count;
    size_t propositional_count;
    size_t prescriptive_count;
} hu_fact_extract_result_t;

/*
 * Extract propositional and prescriptive facts from conversation text.
 * Uses heuristic NLP patterns to identify subject-predicate-object triples.
 * High-confidence facts (>= 0.6) should be stored in the knowledge graph.
 */
hu_error_t hu_fact_extract(const char *text, size_t text_len, hu_fact_extract_result_t *result);

/*
 * Deduplicate facts against existing entries.
 * Removes facts that overlap with existing knowledge (by subject+predicate match),
 * compacting the array in place and updating result->fact_count.
 * Returns the number of novel (retained) facts.
 */
size_t hu_fact_dedup(hu_fact_extract_result_t *result, const hu_heuristic_fact_t *existing,
                     size_t existing_count);

/*
 * Format extracted facts as memory store keys and values.
 * Propositional: key = "fact:{subject}:{predicate}:{object}"
 * Prescriptive: key = "skill:{subject}:{predicate}"
 * Caller frees each key/value string via alloc.
 */
hu_error_t hu_fact_format_for_store(hu_allocator_t *alloc, const hu_heuristic_fact_t *fact,
                                    char **key, size_t *key_len, char **value, size_t *value_len);

#endif
