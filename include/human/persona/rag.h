/* include/human/persona/rag.h — RAG-over-own-messages voice grounding.
 *
 * The May-2026 SOTA finding (docs/research/2026-05-29-sota-voice-fidelity.md):
 * at one-person data volumes, retrieving the user's OWN most-similar past
 * messages as per-turn few-shot grounding tends to beat a fine-tuned adapter,
 * and is what the frontier labs converge on (character + memory/RAG). Unlike
 * the static example banks (pre-mined, topic-keyword-selected), this retrieves
 * dynamically from the full message corpus by similarity to the INCOMING
 * message — the distinctive RAG-personalization move.
 *
 * The three core functions are pure + allocation-free: callers provide the
 * corpus and output buffers. The hot-path convenience wrapper
 * hu_persona_rag_ground_from_file() (bottom) is the one exception — it reads the
 * corpus file and allocates internally. Gated in production behind
 * cfg.agent.rag_grounding_enabled (default off) so the LoRA-vs-RAG A/B decides
 * when to flip it on, per "measure before optimize".
 */
#ifndef HU_PERSONA_RAG_H
#define HU_PERSONA_RAG_H

#include <stddef.h>

#include "human/core/allocator.h"

/* Content-word Jaccard relevance of `candidate` to `query` in [0.0, 1.0]
 * (lowercased tokens, length>2, stopwords excluded). 0 for NULL/empty. */
double hu_persona_rag_relevance(const char *query, const char *candidate);

/* Retrieve up to `k` indices of corpus messages most relevant to `query`,
 * most-relevant first, stable on ties (lower index wins). Only messages with
 * positive relevance are returned, so an off-topic corpus yields fewer (or 0).
 * Writes into out_indices (bounded by out_cap); returns the count written. */
size_t hu_persona_rag_retrieve(const char *query, const char *const *corpus, size_t corpus_n,
                               size_t k, size_t *out_indices, size_t out_cap);

/* Build a few-shot voice-grounding block from `examples` into `buf`:
 *   "Here are examples of how I actually text — match this voice:\n- ex1\n..."
 * Never half-writes an example (stops cleanly at the buffer bound). Returns
 * bytes written (excluding NUL); 0 if nothing written. */
size_t hu_persona_rag_build_block(const char *const *examples, size_t n, char *buf, size_t cap);

/* Hot-path convenience wrapper (NOT pure — reads the corpus file + allocates):
 * load the user's sent-message corpus from `corpus_path` (JSONL, one
 * {"text": "..."} per line; plain-text lines also tolerated), retrieve the top
 * `k` messages most relevant to `query`, and write the grounding block into
 * `out_buf` (bounded by out_cap). Allocates working buffers via `alloc` and
 * frees them all before returning. Returns bytes written (excluding NUL); 0 if
 * the corpus is missing/empty/oversized or nothing relevant was found. */
size_t hu_persona_rag_ground_from_file(const char *query, const char *corpus_path, size_t k,
                                       char *out_buf, size_t out_cap, hu_allocator_t *alloc);

#endif /* HU_PERSONA_RAG_H */
