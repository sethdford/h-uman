#ifndef HU_MEMORY_AGENT_FACTS_H
#define HU_MEMORY_AGENT_FACTS_H

#include "human/core/error.h"
#include "human/memory.h"
#include "human/memory/fact_extract.h"
#include "human/memory/graph.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Contract C3 (SOTA fleet) — the daemon's own outbound text becomes
 * first-class facts with provenance (Mem0-2026 shape: agent-generated
 * facts stored with equal weight but distinguishable source).
 *
 * Why this exists: `hu_graph_ingest_fact` and the deep-extract writers only
 * ever see the USER's half of a conversation. h-uman's own replies vanish
 * the instant they're sent — the graph has no record of what the daemon
 * itself said, so (a) a careless reader can't tell an agent-authored edge
 * from an observation of Seth, and (b) the daemon can never answer "did I
 * already tell them I'd send that?". This module closes both gaps by
 * running the SAME regex fact extractor and commitment detector already
 * used for the user's side against the daemon's OWN reply text, but
 * stamping the result with subject "assistant" and provenance
 * "agent:<message_ref>" so every downstream reader (grounding, world-model,
 * recall, the promise ledger) can tell the two sources apart.
 *
 * Gated OFF -> SHADOW -> LIVE via HU_AGENT_FACTS (hu_gate_mode_from_env),
 * default OFF, per feature-gate-requires-measurement.md. Activation gated
 * on scripts/eval_agent_promise_recall.py: do not flip HU_AGENT_FACTS to
 * default-on without a measurement showing the daemon can actually recall
 * its own commitments from the rows this records. See hu_agent_facts_record_reply
 * in src/memory/agent_facts.c for the gate comment at the call site.
 */

/* Runs the regex fact extractor (fact_extract.h) against `reply`, relabels
 * every extracted fact's subject as "assistant", and ingests each one into
 * `g` via hu_graph_ingest_fact with provenance "agent:<message_ref>" and
 * confidence scaled by 0.6 (an agent's paraphrase of its own reply is a
 * weaker signal than an observation of the user). Also runs
 * hu_conversation_detect_commitment(from_me=true) against `reply`; a
 * detected commitment is stored as a memory row keyed
 * "agent-promise:<contact>:<now>" (category CORE) via `mem`'s vtable.
 *
 * Mode is read from HU_AGENT_FACTS at call time:
 *   OFF (default)  — no-op, returns HU_OK immediately.
 *   SHADOW         — extraction + commitment detection run, results are
 *                     logged ("[agent-facts SHADOW] would ..."), NOTHING
 *                     is written to `g` or `mem`.
 *   LIVE           — extraction + commitment detection run AND written.
 *
 * `g` may be NULL (graph ingestion is skipped, commitment storage still
 * runs); `mem` may be NULL (commitment storage is skipped, graph ingestion
 * still runs). Returns HU_ERR_INVALID_ARGUMENT for a NULL/empty contact_id
 * or reply. Never returns an error for a failed sub-write — those are
 * logged and swallowed, matching hu_daemon_promise_keeper_scan_outbound's
 * contract (a best-effort side channel must not fail the send path). */
hu_error_t hu_agent_facts_record_reply(hu_graph_t *g, hu_memory_t *mem, const char *contact_id,
                                       size_t cid_len, const char *reply, size_t reply_len,
                                       const char *message_ref, int64_t now);

/* Read-only variant for the `human memory agent-facts-dry` CLI hook and for
 * tests: runs the identical extraction + commitment detection as the
 * LIVE/SHADOW paths (subject already relabelled "assistant" on every fact),
 * but never touches a graph or memory store — nothing is written anywhere.
 *
 * `facts_out` receives the extracted facts (subject == "assistant").
 * `commitment_out`/`who_out` receive the commitment description/who per
 * hu_conversation_detect_commitment's contract; `*has_commitment_out` is
 * set to whether a commitment was found. Any of commitment_out/who_out/
 * has_commitment_out may be NULL to skip commitment detection entirely.
 * Returns HU_ERR_INVALID_ARGUMENT for NULL/empty reply or NULL facts_out. */
hu_error_t hu_agent_facts_dry_run(const char *reply, size_t reply_len,
                                  hu_fact_extract_result_t *facts_out, char *commitment_out,
                                  size_t commitment_cap, char *who_out, size_t who_cap,
                                  bool *has_commitment_out);

#ifdef __cplusplus
}
#endif

#endif /* HU_MEMORY_AGENT_FACTS_H */
