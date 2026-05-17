---
plan: docs/plans/2026-05-10-imessage-private-api-feasibility.md
auditor: group-2-channels
audited_at: 2026-05-17
implemented: NONE
proven: NONE
wired: N/A
verdict: OBSOLETE
confidence: HIGH
---

## Plan Summary
Feasibility analysis for iMessage Tier B private-API send-side capabilities (effects-on-send, inline replies, sticker/Memoji send, edit/unsend) via IMCore / AXUIElement / imagent XPC. Explicitly a design-only doc with a recorded **decision NOT to ship** in this work cycle.

## Key Claims (from the plan)
- Claim 1 (explicit): "Design doc only. Nothing in this document is implemented." (line 3-5)
- Claim 2: Tier A (FDA-aware circuit breaker + doctor command) is load-bearing and just landed — Tier B work is gated on 30+ days of Tier A telemetry.
- Claim 3: Proposed API additions (`hu_imessage_send_with_effect`, `hu_imessage_send_inline_reply`, capability probe `hu_imessage_effect_path`) are NOT to be built.
- Claim 4: References existing partial IMCore wiring (`imcore_init`, `imcore_start_typing`, `imcore_stop_typing`) and AX wiring (`ax_open_conversation`, `ax_start_typing`, `ax_tapback`) — these are pre-existing Tier A surface, not Tier B.
- Claim 5: Effect-name table `hu_imessage_effect_name` (inbound mapping) already exists.

### Implemented? (code exists)
- Pre-existing references in src/channels/imessage.c (referenced by the plan as ground truth, NOT as Tier B implementation):
  - `imcore_init` at imessage.c:510, 3034 — pre-existing Tier A.
  - `imcore_start_typing` at imessage.c:511 — pre-existing Tier A.
  - `hu_imessage_effect_name` at imessage.c:983, 1735 — pre-existing inbound mapping.
- Tier B proposed APIs: NOT FOUND.
  - `hu_imessage_send_with_effect`: grep returned nothing.
  - `hu_imessage_send_inline_reply`: grep returned nothing.
  - `hu_imessage_effect_path` / `HU_IMSG_EFFECT_PATH_IMCORE`: grep returned nothing.

### Proven? (tests exist)
- No Tier B tests exist (none were supposed to).

### Wired? (called in runtime path / dispatch)
- N/A — plan is design-only, explicitly NOT scheduled for implementation.

## Gaps
- None. The plan is functioning as designed: a documented "do not build" decision with explicit reopen conditions.

## Notes
- Verdict OBSOLETE per template definition: "Plan is a design doc that was never meant to be executed (e.g., RFCs that were rejected)."
- Plan's "Conditions to revisit" section lists 4 checkboxes that must be true before reopening — none of them have been ticked in the repo (no signed/notarized build, no second user, no per-macOS selector probe baseline).
- Companion to Tier A iMessage work which IS shipped (FDA breaker, doctor, AX/IMCore typing).
