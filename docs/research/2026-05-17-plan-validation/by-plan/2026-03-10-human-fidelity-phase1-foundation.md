---
plan: docs/plans/2026-03-10-human-fidelity-phase1-foundation.md
auditor: group-4-human-fidelity
audited_at: 2026-05-17
implemented: FULL
proven: FULL
wired: FULL
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
Phase 1 of Human Fidelity. Fix broken plumbing (F1 tapback message_id, F2
tapback-vs-type decision), wire auto-vision for photos, timing refinements,
foundational humanization classifiers (F4-F7, F10-F11, F15, F40-F44).

## Key Claims (from the plan)
- Claim 1: iMessage poll loop sets `msgs[count].message_id = rowid`
- Claim 2: Tapback-vs-type classifier `hu_conversation_classify_tapback_decision`
  exists and is called from daemon
- Claim 3: Filler / typing-quirk humanization applied to outgoing messages
- Claim 4: Foundational `hu_tapback_decision_t` enum lives in conversation.h

## Evidence

### Implemented? (code exists)
- `src/channels/imessage.c:3920` — `msgs[count].message_id = rowid;`
- `src/channels/imessage.c:3595` — mock branch: `msgs[i].message_id = (int64_t)(i + 1)`
- `hu_conversation_classify_tapback_decision` exists and is callable
- `hu_conversation_apply_typing_quirks`, `hu_conversation_apply_fillers`
  callable (definitions in `src/context/conversation.c`)

### Proven? (tests exist)
- `tests/test_imessage_extended.c`, `tests/test_imessage_adversarial.c`,
  `tests/test_imessage_chatdb_fixture.c`, `tests/test_imessage_outbound_dedup.c`
- `tests/test_channel_all.c`
- `tests/test_conversation.c`
- `tests/test_persona_filler_roundtrip.c`

### Wired? (called in runtime path / dispatch)
- `src/daemon.c:9293-9294` — `hu_tapback_decision_t tapback_decision =
  hu_conversation_classify_tapback_decision(...)`
- `src/daemon.c:10781` — `hu_conversation_apply_typing_quirks(...)`
- `src/daemon.c:10807` — `hu_conversation_apply_fillers(...)`
- iMessage poll loop ships rowid via `message_id`, which the daemon's
  react path consumes (per the plan's daemon.c:2928 expectation).

## Gaps
- Tapback JXA reliability (Task 2 in plan) was scoped as "investigate and
  implement"; reliability of the AppleScript / accessibility path is platform-
  dependent and not statically auditable here. The C-side wiring is complete.

## Notes
- This phase aligns with CLAUDE.md M1's "Phase 1 done": persona always-on +
  iMessage tapback plumbing fixed. Verified via specific file:line.
- Phase 1 was foundational; later phases depend on it (e.g. Phase 5 voice
  reuses the iMessage attachment path).
