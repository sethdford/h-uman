#ifndef HUMAN_MEMORY_SEMANTIC_RECALL_H
#define HUMAN_MEMORY_SEMANTIC_RECALL_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/gate_mode.h"
#include "human/memory.h"
#include "human/memory/retrieval.h"
#include "human/memory/vector.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Semantic recall (Phase 2 of docs/plans/2026-08-02-semantic-retrieval/spec.md).
 *
 * Gate: HU_SEMANTIC_RECALL=off|shadow|live (default OFF).
 *   OFF    — nothing changes; the hash embedder + empty in-memory store stay.
 *   SHADOW — real embedder + persistent store are attached and every recall
 *            computes the semantic candidates, LOGS them (count, overlap with
 *            keyword hits, fingerprint) and DROPS them.
 *   LIVE   — semantic candidates are merged into recall.
 * Promotion to LIVE is gated on the Phase-1 harness re-run through this path
 * (feature-gate-requires-measurement.md); SHADOW is what deploys first.
 *
 * Contract C1 (docs/plans/2026-08-02-semantic-retrieval/): SHADOW -> LIVE
 * additionally requires a blind A/B run via scripts/eval_semantic_live_gate.py
 * showing (a) the humanness composite is not lower under LIVE than SHADOW and
 * (b) neither the emotional-intelligence nor the reality-awareness judge axis
 * drops under LIVE. This is not optional test-passing: AlpsBench (arXiv
 * 2603.26680) found that adding memory retrieval improves persona awareness
 * but DEGRADES emotional intelligence and real-vs-hypothetical awareness via
 * over-reliance on retrieved memories — exactly the failure mode this gate
 * exists to catch before it reaches production replies. Do not flip this gate
 * to default-LIVE without a PROMOTE verdict from that script (see
 * docs/plans/2026-08-02-semantic-retrieval/semantic-live-gate-*.json for the
 * measurement record) — a green build and passing unit tests are not this
 * measurement (.claude/rules/feature-gate-requires-measurement.md,
 * .claude/rules/no-number-without-a-measurement.md). */
hu_gate_mode_t hu_semantic_recall_mode(void);

/* Embedding endpoint base URL: $HU_SEMANTIC_EMBED_URL, default the production
 * mlx-server (http://127.0.0.1:8741) which hosts /v1/embeddings in-process. */
const char *hu_semantic_recall_embed_url(void);

#define HU_SEMANTIC_EMBED_DIM 768u /* nomic-embed-text-v2 / modernbert-embed-base */

/* Build the real pair (HTTP embedder + sqlite-vec store on the engine's own
 * DB) and attach them to `mem` so writes are indexed. Returns
 * HU_ERR_NOT_SUPPORTED when `mem` is not a sqlite engine; HU_ERR_INTERNAL when
 * the store could not be created. On success the caller owns out_embedder /
 * out_store and must deinit them AFTER the memory engine. */
hu_error_t hu_semantic_recall_attach(hu_allocator_t *alloc, hu_memory_t *mem,
                                     hu_embedder_t *out_embedder, hu_vector_store_t *out_store);

/* Recall byte budget (2026-09-02 live-gate finding, docs/plans/
 * 2026-08-02-semantic-retrieval/semantic-live-gate-2026-09-02.json): 9 of 40
 * LIVE contexts returned an EMPTY completion where SHADOW returned none. The
 * LIVE arm differs from SHADOW only by the recall block — up to 5 hits of up
 * to 2000 chars each — which crowds the 16 KB prompt cap and the reply's
 * token budget. The block is therefore bounded at the SOURCE: each semantic
 * hit is cut at a word boundary to HU_SEMANTIC_RECALL_HIT_MAX_BYTES and the
 * whole semantic leg is capped at hu_semantic_recall_max_bytes(). The prompt
 * cap reserves the guard tail separately (hu_prompt_positional_cap_apply). */
#define HU_SEMANTIC_RECALL_DEFAULT_MAX_BYTES 1200u
#define HU_SEMANTIC_RECALL_HIT_MAX_BYTES     240u

/* $HU_SEMANTIC_RECALL_MAX_BYTES, default HU_SEMANTIC_RECALL_DEFAULT_MAX_BYTES.
 * Unparsable or non-positive values fail closed to the default. */
size_t hu_semantic_recall_max_bytes(void);

/* Pure: byte length to keep from s[0, len) so the result is <= max_bytes and
 * ends on a word boundary when one lies in the upper half of the window
 * (otherwise a hard cut at max_bytes, backed off any UTF-8 continuation
 * bytes). Returns len when len <= max_bytes; 0 on NULL / max_bytes == 0. */
size_t hu_semantic_recall_truncate_len(const char *s, size_t len, size_t max_bytes);

/* Clamp a semantic retrieval result in place: every entry's content is cut
 * to per_hit_bytes (word boundary), and entries are kept in rank order only
 * while the cumulative kept content stays within budget_bytes — the first
 * entry that would exceed it and every entry after it are dropped and freed.
 * entries/scores are shrunk so hu_retrieval_result_free stays exact.
 * Returns the total content bytes kept. Deterministic for identical input. */
size_t hu_semantic_recall_clamp_result(hu_allocator_t *alloc, hu_retrieval_result_t *res,
                                       size_t budget_bytes, size_t per_hit_bytes);

/* ── Index + recall content policy (2026-09-02 live-gate finding, second
 * half: docs/plans/2026-08-02-semantic-retrieval/
 * semantic-live-gate-2026-09-02-recall-budget.json). After the byte clamp,
 * 6/40 LIVE contexts still returned EMPTY completions. Single diagnostic
 * requests isolated the trigger to the CONTENT of the top hits, not their
 * size: (a) episodic records from the experience writer
 * (src/intelligence/experience.c, key prefix "experience:", body
 * "Task: ...\nActions: ...\nOutcome: ...\nScore: ...") — harness scaffolding
 * that made up 576 of the 1130 rows in the production index and surfaced
 * for almost every query — and (b) hits whose content is an AI-identity
 * confrontation ("are you texting or your ai??", "Is this Seth"). With
 * either in the prompt the adapter emits ~15 think-only tokens that
 * mlx-server strips, which reaches the daemon as an empty reply.
 *
 * scripts/eval_semantic_live_gate.py mirrors hit_is_excluded in Python
 * (hit_is_excluded) because its LIVE arm calls `memory search --semantic`,
 * not the hybrid path. Keep the two cue lists identical. ───────────────── */

/* Index policy: false when a memories row must never enter the semantic
 * index (the "experience:" episodic prefix). Checked at write time and at
 * reindex; NULL / empty keys are not indexable. */
bool hu_semantic_recall_key_is_indexable(const char *key, size_t key_len);

/* Recall policy: true when a semantic hit must not be injected into a reply
 * prompt: a non-indexable key (stale rows indexed before the write-time
 * exclusion), the experience scaffold body under any key, or an AI-identity
 * confrontation. The confrontation cues are matched at WORD BOUNDARIES,
 * case-insensitively (hu_str_contains_word_ci_n) — "ai" must not fire inside
 * "said" / "wait" / "maid" (substring-classifier-pitfalls.md) — and bare
 * "AI" as a topic is deliberately NOT a cue: "Mel started an AI job" is a
 * memory. A short bare identity question ("Is this Seth", <= 4 words
 * starting "is this" / "is that") is excluded without naming the persona. */
bool hu_semantic_recall_hit_is_excluded(const char *key, size_t key_len, const char *content,
                                        size_t content_len);

/* Drop every excluded entry from a semantic result in place, preserving rank
 * order and score alignment; dropped entries are freed and entries/scores
 * are shrunk (freed and NULLed when nothing survives). Returns the number
 * dropped; 0 on NULL / empty input. Run BEFORE the byte clamp so an excluded
 * hit never consumes budget. */
size_t hu_semantic_recall_filter_result(hu_allocator_t *alloc, hu_retrieval_result_t *res);

/* ── Register-conditioned suppression (US-5: protect the LIVE gate from EI drift)
 *
 * Gate: HU_SEMANTIC_RECALL_REGISTER_GATE=off|shadow|live (default OFF).
 *   OFF    — register classification is computed but ignored; LIVE behavior
 *            unchanged from HU_SEMANTIC_RECALL=live without this gate.
 *   SHADOW — register classification is computed and logged ("would suppress")
 *            but semantic recall proceeds unchanged (no output difference).
 *   LIVE   — semantic recall is suppressed for casual-register turns.
 *
 * Boundary: casual if word count (whitespace-delimited, matching Python
 * str.split() semantics) is ≤12 words; substantive if >12 words.
 * NULL / empty input is treated as casual (fails closed: recall is the
 * untested-for-casual behavior per feature-gate-requires-measurement.md).
 *
 * Motivation (docs/research/2026-09-05-sota-fleet-closing-report.md):
 * semantic recall shows +0.110 EI on substantive exchanges but -0.078 on
 * casual ones. This gate protects the composite/EI numbers now drifting ~0.1
 * per run toward the 0.15 revert threshold. Register classification reuses
 * the casual/substantive boundary from scripts/blind_ab/authorship_gap.py
 * (reply ≤12 words = casual) so measurements stay comparable. ──────────────*/

#define HU_SEMANTIC_RECALL_REGISTER_MAX_CASUAL_WORDS 12u

/* True when the turn's register admits semantic recall (word count strictly
 * greater than the casual boundary, i.e. "substantive"). NULL / empty input
 * is treated as casual (fails closed to suppression). Pure: no I/O,
 * no allocation. */
bool hu_semantic_recall_register_admits(const char *query, size_t query_len);

/* $HU_SEMANTIC_RECALL_REGISTER_GATE=off|shadow|live, default OFF. Layered on
 * top of hu_semantic_recall_mode(): only consulted inside the HU_GATE_LIVE
 * branch, so this being non-OFF has zero effect unless HU_SEMANTIC_RECALL is
 * already live. See feature-gate-requires-measurement.md — promotion to LIVE
 * requires scripts/eval_semantic_live_gate.py's register_breakdown.substantive
 * verdict to stay PROMOTE-eligible (composite/EI/reality within the existing
 * tolerances), not merely a green build. */
hu_gate_mode_t hu_semantic_recall_register_gate_mode(void);

#ifdef __cplusplus
}
#endif

#endif /* HUMAN_MEMORY_SEMANTIC_RECALL_H */
