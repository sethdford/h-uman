---
plan: docs/plans/2026-05-16-audit-followups/01-persona-overlay-wiring.md
auditor: group-11-audit-followups-adrs-renames-memory
audited_at: 2026-05-17
implemented: NONE
proven: NONE
wired: NONE
verdict: NOT_STARTED
confidence: HIGH
---

## Plan Summary
Spec to wire per-channel `hu_persona_overlay_t` (formality/length/emoji) into 4 Tier-1
channel send paths via a new `hu_persona_render_for_channel()` helper. Phase 1 = helper,
Phase 2 = wire 4 channels, Phase 3 = fan out to remaining 39.

## Key Claims (from the plan)
- Claim 1: New `src/persona/render.c` with `hu_persona_render_for_channel(persona, overlay, raw, alloc, &out)`
- Claim 2: 12 unit tests for the helper (per overlay field + combinations + NULL cases)
- Claim 3: Telegram/Discord/Slack/iMessage outbound paths call the helper before send
- Claim 4: Per-channel tests assert overlay is applied
- Claim 5: `hu_persona_get_overlay()` helper for channel name → overlay lookup

## Evidence

### Implemented? (code exists)
- NO `src/persona/render.c` — `ls src/persona/` shows no render.c
- `grep -rn "hu_persona_render_for_channel\|hu_persona_get_overlay\b" src/ include/` returns 0 hits
- Tier-1 channels (`src/channels/telegram.c`, `discord.c`, `slack.c`, `imessage.c`) contain
  zero `hu_persona_overlay_t` references in send paths
- The `hu_persona_overlay_t` struct exists in `include/human/persona.h` (audit's finding:
  "declared in 43 vtables; 0 use it" still holds)

### Proven? (tests exist)
- NONE FOUND — no `tests/test_persona_render*.c` or per-channel overlay tests

### Wired? (called in runtime path / dispatch)
- ORPHANED — overlay struct still declared but no production code reads from it

## Gaps
- Helper function not implemented
- Header for helper not declared
- No tests for the helper
- No channel-level integration
- AC-1 through AC-7 all unmet

## Notes
This is the single largest unblocked item from the May 16 audit. M1 thesis ("channel-aware
persona") still has the structural gap the audit identified. Status in plan's frontmatter
says "Spec — not yet implemented" and that is still accurate as of 2026-05-17.
