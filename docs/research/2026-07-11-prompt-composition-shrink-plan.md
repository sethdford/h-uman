# Prompt Composition Map + Shrink Plan (latency lever)

Date: 2026-07-11. Basis: code-trace of `src/agent/agent_turn.c` +
`src/agent/prompt.c` (subagent sweep, key claims re-verified by hand).
Context: the 2026-05-31 realtime finding stands — prompt REORDERING is a
measured no-op (cache breaks at ~token 56); SHRINKING is the real latency
lever. This doc maps where the ~21KB goes and ranks the cuts.

## Headline findings (verified 2026-07-11)

1. **Positional 16KB truncation fires in production constantly.** The system
   prompt is HARD-CAPPED at 16384 bytes (agent_turn.c:4401-4417), cut at the
   last newline, warn-once per process — and `service-loop-error.log` carries
   **204** truncation events. The cut is positional, not value-aware:
   whatever is assembled last is dropped.

2. **On the persona (iMessage daemon) path, the truncated tail is the
   anti-AI-tell guard.** `persona_immersive` is true whenever a persona
   prompt exists (agent_turn.c:4301), and the immersive branch of
   hu_prompt_build_system returns early (~prompt.c:484) after appending
   conversation context → texting shape rules → CRITICAL REMINDER →
   persona reinforcement. Those last sections are deliberately last for
   recency salience — and they are exactly what the 16K cut deletes on
   heavy turns.

3. **The persona path has NO safety-rules section at all.** The `## Safety`
   block (prompt.c:1032-1116) and the persona-first safety_rules.txt rewrite
   (2026-05-17 doctrine: "stay in character, deflect probes in voice") only
   run on the NON-immersive assistant path; `safety_rules` is never set on
   the agent/daemon path (grep: zero hits in agent_turn.c/daemon). The
   in-character safety rules were written FOR the persona path but never
   wired to it. Output-side validator chains are the only guard today.

Implication: "shrink the prompt" is not only a latency play — it decides
WHICH content survives. Until Phase-2 value-aware trim exists, every byte
added early evicts a byte of the persona guard tail; and adding the missing
safety section without trim would evict ~3KB more of it.

## Composition (major sections, est. bytes)

| Section | Site | Bytes | Gate |
|---|---|---|---|
| Persona (compact + tone/sentiment) | agent_turn.c:2969 | 3.6K | cached |
| Safety rules (fixed) | prompt.c:1033-1114 | 3.0K | none |
| Memory context (facts) | prompt.c:756-770 | 2-4K | should_skip_field |
| GraphRAG grounding | prompt.c:785-798 | 2-4K | should_skip_field + HU_GRAPH_GROUNDING |
| Identity | prompt.c:244-279 | 1-2K | none |
| Personal model | agent_turn.c:4044-4070 | 1-2K | should_skip_field |
| World model (ToM/goals) | prompt.c:855-866 | 1-2K | should_skip_field |
| Humanness ctx (salience) | agent_turn.c:1056-1358 | 0.5-3K | HU_SALIENCE_LIVE (off) |
| Tools schema | prompt.c:522-605 | 0.5-3K | native_tools halves it |
| STM/session + history | prompt.c:886-896, 1387-1395 | 1.5-5K | 20K msg budget |
| Self-exemplars | prompt.c:832-850 | 0.5-1.5K | should_skip_field |
| ~10 smaller sections | various | 2-4K | mostly should_skip_field |

`prompt_budget` Phase 1 (measurement-only) is ALREADY enabled in live config
(`~/.human/config.json: prompt_budget.enabled=true`) — per-field byte stats
accumulate; Phase 2 (value-aware trim) is designed but NOT wired.

## Ranked actions

1. **Wire prompt_budget Phase 2 (value-aware trim) — the durable fix.**
   Replaces positional tail-truncation with priority trim on the immersive
   path: protect head (persona) + tail (CRITICAL REMINDER / reinforcement),
   trim the middle (memory/GraphRAG/exemplars). Effort M. Gate: shadow-log
   trims vs the positional cut for N days, then blind-A/B before LIVE.
2. **Wire the persona-first safety rules into the immersive path** — they
   were rewritten for it (2026-05-17) but never reach it. Must land WITH or
   AFTER #1 (a compact ≤1KB variant placed early), else it evicts 1-3KB of
   the guard tail on truncating turns.
3. **GraphRAG grounding size cap** (2-4K, currently uncapped): hard-cap the
   community summary at ~1K. Note GraphRAG lost its 05-31 A/B (ON win-rate
   43%) and is default-ON anyway — capping it is strictly conservative.
4. **Memory-context compression** (2-4K → 1-2K): dedupe repetitive facts to
   (date, topic, sentiment) tuples. Effort M; gate on blind A/B.
5. **Tools schema**: iMessage daemon path with native-tools provider already
   halves this; verify native mode is active on :8741 path (S).

Not recommended: trimming the 3K safety rules (red-team gate required),
salience-based suppression before its own A/B (HU_SALIENCE_LIVE stays off).

## Verification hooks

- Field stats: prompt_budget Phase-1 accumulators (live since config flag).
- Truncation frequency: `hu_log_warn_once` fires max once/process — grep
  service log for "system prompt truncated" to confirm it's happening, then
  add a per-N-turns counter if a rate is needed (S).
- Any LIVE trim change gates on the blind-A/B human tier
  (feature-gate-requires-measurement.md).
