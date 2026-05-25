---
title: "W4 — Self-RAG Inline Verification + Provenance Receipts"
created: 2026-05-10
status: closed
parent: 2026-05-10-memory-roadmap-overview.md
depends_on: 2026-05-10-w1-bitemporal-foundation.md
risk: medium
scope: src/agent/, src/memory/, ui/, src/main.c
last_audit: 2026-05-25
---

# W4 — Self-RAG Inline Verification + Provenance Receipts

## Goal

Wire the project's existing self-RAG / corrective-RAG / claim-verification modules into the synchronous response path so every factual claim is checked against memory before it leaves the agent. Surface provenance ("you told me this on iMessage on Tuesday at 3:14 PM") on every memory-grounded statement. Ship a user-facing memory view ("what does h-uman know about me?") with targeted erasure that satisfies GDPR Article 15/16/17 well before the EU AI Act August 2026 applicability date.

## Motivation

Already in tree:

- `src/memory/self_rag.c` — self-reflective RAG
- `src/memory/corrective_rag.c` — corrective RAG
- `src/memory/adaptive_rag.c` — adaptive RAG
- `src/memory/hallucination_guard.c` — hallucination guard
- `src/memory/verify_claim.c` — claim verification

Search via `rg "verify_claim|self_rag" src/agent/` shows these are not called from `src/agent/agent_turn.c`. They're libraries waiting to be wired in. Effect: hallucinated facts can reach the user even though the verifier exists.

Provenance: `hu_memory_entry_t` already has a `source` field; W1 adds `provenance` to relations. There's no UI surface that shows the user "this is what I'm remembering and where it came from." There's no targeted-delete UX. New America's OTI brief (2025) and EU AI Act August 2026 deadline make this not just nice-to-have.

## Prior art

- Self-RAG (arxiv 2310.11511) — agent reflects on its own retrievals before generating.
- Corrective-RAG (arxiv 2401.15884) — score retrieved chunks, retrieve more on low score.
- Mem0 user/session/agent namespaces — required for targeted GDPR erasure.
- Project prior work: extensive RAG variants already exist; this workstream is wiring, not new algorithms.

## Design

### 1. Inline verification on the response path

`src/agent/agent_turn.c` gets a new step before sending the LLM response to the channel:

1. Extract factual claims from the draft response (pattern: declarative sentences with named entities or numbers).
2. For each claim, call `hu_verify_claim` against memory.
3. If any claim falls below `verify_confidence_threshold` (config, default 0.6):
   - If `verification_mode == "strict"`: rewrite via `hu_corrective_rag_revise`.
   - If `verification_mode == "soft"`: prepend hedge ("I'm not sure but…") and attach provenance.
   - If `verification_mode == "off"`: log only, no rewrite.

Default mode: `soft`. Hedging is preferable to silently rewriting in cases the user might prefer the original phrasing.

New module: `src/agent/response_verifier.c` + `include/human/agent/response_verifier.h`. Pure orchestration; the actual verification logic stays in `src/memory/verify_claim.c`.

### 2. Provenance receipts

Every memory-grounded segment of a response carries an attribution token the agent can render. Format (markdown when channel supports, plain text otherwise):

```
"You start work at 9 — `[from iMessage, Mon 2026-05-09 14:22]`."
```

Attribution is generated from `hu_memory_entry_t.source` and W1's relation `provenance` field. Stored in a per-turn `hu_provenance_buffer_t` so the agent can format inline OR show only when asked.

Persona overlay for each channel decides whether attributions render inline or stay tucked away (config flag `persona.channel_overlay.<channel>.show_provenance`).

### 3. User-facing memory view

New view in the existing UI (`ui/src/views/memory-view.ts`):

- Sidebar: scopes (`global`, `contact`, `topic`, `time-window`)
- Main: list of memories matching the selected scope
  - Each row: content, source, observed-at, event-window (if bitemporal), confidence, "delete" button
- Filter: full-text + by source + by time
- Bulk: "delete everything from <date>", "delete everything mentioning <entity>"
- Audit: "show me everything about <topic>"

CLI mirror: `human memory view` opens the URL; `human memory list --scope=contact:casey`; `human memory delete --entity-id=42`.

Backed by new gateway endpoints (added in `src/gateway/gateway.c`):

```
GET  /api/memory/list?scope=…&filter=…
GET  /api/memory/get/:id
POST /api/memory/delete           { ids: [...] }
POST /api/memory/delete-by-filter { entity_id: …, time_range: [...] }
GET  /api/memory/audit?topic=…
```

Mock responses added in `ui/src/demo-gateway.ts` per `AGENTS.md` 7.5.

### 4. Targeted erasure

```c
hu_error_t hu_memory_erase(
    hu_memory_t *mem, hu_graph_t *graph,
    const hu_memory_erase_request_t *req,
    hu_memory_erase_report_t *out_report);
```

Erasure is cascading by default: deleting an entity removes its relations, its cross-edges (W3), its episode mentions, and any community summaries that reference it (with a re-summarization queued for next AutoDream cycle).

Erasure is auditable: every delete writes a row to `erasure_log` so future audits can show "you deleted this on 2026-05-15 14:10."

### 5. GDPR readiness

Article 15 (right of access): `human memory list` + UI view satisfies.
Article 16 (rectification): `human memory edit --id=… --content=…` + `human persona edit`.
Article 17 (erasure): `hu_memory_erase` + UI delete button.
Article 30 (records of processing): `erasure_log` table; `processing_log` (W6) extends to writes.
EU AI Act audit-trail (10 years for high-risk): conflicts with Article 17. Documented in spec; resolution: erasure removes content but retains hashed audit metadata for the legally-required window.

## File map

| File | Role |
|------|------|
| `include/human/agent/response_verifier.h` | New — public API |
| `src/agent/response_verifier.c` | New — orchestration, hedge generation |
| `src/agent/agent_turn.c` | Insert verifier call before send |
| `src/memory/verify_claim.c` | Existing — extend to return confidence + provenance |
| `src/memory/provenance.c` | New — `hu_provenance_buffer_t` formatting |
| `include/human/memory/provenance.h` | New |
| `src/memory/erase.c` | New — `hu_memory_erase` + `erasure_log` schema |
| `include/human/memory/erase.h` | New |
| `src/gateway/gateway.c` | New endpoints |
| `ui/src/views/memory-view.ts` | New view |
| `ui/src/demo-gateway.ts` | Mock responses |
| `src/main.c` | `human memory view/list/delete/edit/audit` subcommands |
| `tests/test_response_verifier.c` | New |
| `tests/test_provenance.c` | New |
| `tests/test_memory_erase.c` | New — cascade, log, GDPR scenarios |
| `tests/test_gateway_memory.c` | New — endpoint contracts |
| `eval_suites/hallucination_guard.json` | Existing or new — confirm coverage |

## Test strategy

- Verifier: stub claim with score 0.3 → assert hedged in soft mode, rewritten in strict mode, untouched in off mode.
- Provenance: insert memory with source "iMessage 2026-05-09T14:22"; assert formatter produces the expected attribution string.
- Erasure cascade: create entity + relations + cross-edges + summary; erase entity; assert all dependents removed; assert `erasure_log` entry present.
- Gateway: contract tests using existing `tests/test_gateway_*` patterns.
- ASan clean.

## Success criteria

- Hallucination-guard eval: ≥ 50% reduction in hallucinated factual claims vs pre-W4 baseline.
- 100% of memory-grounded sentences in eval transcripts carry provenance attribution.
- GDPR scenario tests pass: list, get, delete (single + bulk + by-filter), audit.
- Lighthouse on memory-view ≥ 95 / 98 / 95 / 95 (per `AGENTS.md` 12.12).
- Binary size delta: < 80 KB (largest of the workstreams; UI ships separately).

## Risks

| Risk | Mitigation |
|------|------------|
| Verifier adds latency to every turn | Fast-path: only verify if response contains declarative claims with entities/numbers; budget per turn; skipped in off mode |
| Hedging makes the agent feel uncertain about everything | Hedge only when `confidence < threshold`; threshold tunable per channel |
| Erasure cascade removes summaries that other entities still reference | Re-summarize via AutoDream rather than fail-deleting; transactional |
| GDPR / EU AI Act conflict (erase vs 10-yr audit) | Documented mitigation: hashed metadata retained, content removed |

## Open questions

1. Should the response verifier re-run if the corrective rewrite still fails? Recommendation: one revision attempt; on second failure, fall back to soft-hedge mode.
2. Should provenance be visible by default in iMessage? Recommendation: no — attribution feels robotic in casual texts; default off in iMessage overlay, on in CLI/dashboard.

## References

- Self-RAG: arxiv 2310.11511
- Corrective-RAG: arxiv 2401.15884
- New America OTI brief on AI memory + privacy (2025)
- EU AI Act applicability: August 2026
