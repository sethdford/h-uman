# Sprint 6 — Relational Authenticity

## Sprint Goal

Close the three highest-impact humanness gaps surfaced by deep research:
1. Voice maturity stage is computed but never reaches the LLM (orphaned compute).
2. No emotional contagion — partner's emotion doesn't modulate Seth's mood for the next turn.
3. Style mirroring (case/punctuation) is advisory text to the LLM, not enforced.

## Stories

### US-14: Wire voice maturity into LLM prompt
**Priority:** P1 **Estimate:** XS **Depends on:** none

**As** Seth
**I want** the computed voice maturity stage (FORMAL / WARM / CANDID / INTIMATE) to be surfaced in the LLM prompt as a stage-specific directive
**So that** the agent's tone reflects the relational depth instead of being independent of it

**Acceptance criteria**
- AC1: `src/persona/voice_maturity.c` exposes `hu_voice_maturity_build_directive()` returning a stage-specific guidance string (e.g., INTIMATE → "You've earned vulnerability with this person. Reference shared history naturally; don't perform warmth.").
- AC2: The directive is appended to the agent prompt config alongside `mood_ctx` / `somatic_ctx` at the same merge point in `src/agent/agent_turn.c` (around the prompt-context build).
- AC3: A unit test confirms each of the 4 stages produces a distinct, non-empty directive.
- AC4: Full suite stays at ≥10,326 passing, 0 ASan errors.

**Definition of Done**
- Build clean, full suite green.
- `grep -n "hu_voice_maturity_build_directive" src/agent/` returns ≥1 hit.

---

### US-17: Emotional contagion — partner mood modulates Seth's emotional state
**Priority:** P1 **Estimate:** S **Depends on:** none

**As** Seth
**I want** the partner's detected emotion (sadness, excitement, anger) to apply a valence shift to Seth's `hu_emotional_cognition` BEFORE the emotional context prompt is built
**So that** when Jordan is sad, Seth's reply energy actually drops, and when she's excited, his energy lifts — instead of every turn being emotionally independent

**Acceptance criteria**
- AC1: `src/cognition/emotional.c` (or new helper) exposes `hu_emotional_apply_contagion(seth_cognition, partner_emotion, intensity)` that mutates `seth_cognition`'s valence/arousal by a fraction of partner's signal (clamp to [-1, 1]).
- AC2: `src/agent/agent_turn.c` calls the contagion function AFTER `hu_emotional_state_get_recent()` returns partner emotion (around line 1972) and BEFORE `hu_emotional_cognition_build_prompt()` (around line 3320). Comment block documents the data flow.
- AC3: Contagion fraction is bounded (e.g., 30% of partner intensity by default) so Seth doesn't mirror perfectly — he's affected, not overwritten.
- AC4: A unit test asserts that with no partner emotion, Seth's cognition is unchanged; with partner sadness=0.8 intensity, Seth's valence drops by ~0.24 (0.8 × 0.3).
- AC5: Full suite stays green.

**Definition of Done**
- Build clean, full suite green.
- Test asserts both the no-op case and the contagion case.
- The data flow comment in `agent_turn.c` references this story ID.

---

### US-19: Post-generation case/punctuation mirroring (enforcement layer)
**Priority:** P1 **Estimate:** S **Depends on:** none

**As** Seth
**I want** the agent's outbound text to be POST-PROCESSED to match the partner's case + punctuation patterns from their recent messages — not just LLM-instructed to do so
**So that** style mirroring is enforced regardless of whether the frontier model honors the system-prompt directive

**Acceptance criteria**
- AC1: New function `hu_style_mirror_apply(buf, len, partner_recent_messages, n_messages, alloc)` reads partner's last N messages, computes mirroring signals (uses_lowercase, uses_periods, avg_emoji_count), and rewrites the buffer:
   - If partner uses lowercase in ≥70% of recent messages → lowercase first character of each sentence.
   - If partner skips end-of-sentence periods in ≥70% of recent messages → strip trailing periods.
   - DO NOT modify proper nouns, URLs, or numbers (use a conservative regex).
- AC2: Wired AFTER the validator chain and BEFORE channel send. Pick ONE clean integration point in `src/agent/agent_turn.c` or `src/agent/agent_stream.c`.
- AC3: Tests cover: (a) partner uses lowercase → response lowercased correctly, (b) partner uses periods → response preserved, (c) proper nouns "Jordan" survive even in lowercased text.
- AC4: Full suite green.

**Definition of Done**
- Build clean, full suite green.
- New tests asserting all 3 mirroring rules.
- Function is callable from the agent path and produces a different output than input when mirroring conditions are met.

## Sprint sizing

XS + S + S = ~2 agent-days. One implementer dispatch should fit.

## Deferred to Sprint 7 (research-surfaced backlog)

- Per-contact personal model split (`hu_personal_model_t` is unified)
- Read-state detection on outbound messages
- Calendar-aware inbound pacing
- Genuine boundaries → actual non-response
- Relationship mode (DEEPENING/DRIFTING/REPAIR) surfaced in LLM prompt
- In-joke / nickname lexicon per contact
