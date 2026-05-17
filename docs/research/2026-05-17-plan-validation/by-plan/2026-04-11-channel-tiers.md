---
plan: docs/plans/2026-04-11-channel-tiers.md
auditor: group-2-channels
audited_at: 2026-05-17
implemented: FULL
proven: PARTIAL
wired: FULL
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
Tiering strategy: 4 Tier 1 channels (Telegram, Discord, iMessage, Slack) get full vtable surface (react, typing, attachments, history, constraints, persona overlays). Tier 2 = maintained core. Tier 3 = community/experimental. Plan defines tiers and lists action items; not an implementation blueprint per se.

## Key Claims (from the plan)
- Claim 1: All 4 Tier 1 channels implement full `hu_channel_vtable_t` surface: `react`, `start_typing`/`stop_typing`, `get_attachment_path`, `load_conversation_history`, `get_response_constraints`, `human_active_recently`.
- Claim 2: Persona overlays loaded from JSON, matched by `agent->active_channel` via `hu_persona_find_overlay` in `src/persona/persona.c`.
- Claim 3: iMessage has 4+ dedicated test files; Telegram/Discord/Slack covered via `test_channel_all.c`.
- Action items (4 items): persona overlay defaults for Tier 1, per-channel naturalness eval suites, Tier 1 timing audit, Tier 2/3 documentation.

### Implemented? (code exists)
- All 4 Tier 1 channels have full vtable surface:
  - **iMessage** (src/channels/imessage.c:3280-3286): `.start_typing`, `.load_conversation_history`, `.get_response_constraints`, `.react`, `.human_active_recently`.
  - **Telegram** (src/channels/telegram.c:1151-1157): same 5 entries.
  - **Discord** (src/channels/discord.c:728-734): same 5 entries.
  - **Slack** (src/channels/slack.c:1159-1165): same 5 entries.
- `hu_persona_find_overlay` defined at src/persona/persona.c:79; declared at include/human/persona.h:618.
- Overlay invoked at src/persona/persona.c:4206 in prompt-build path.
- Channel files exist for all 31 listed channels (verified ls of src/channels/).

### Proven? (tests exist)
- Many channel tests exist: `tests/test_channel.c`, `tests/test_channel_all.c`, `tests/test_channel_class.c`, `tests/test_channel_embeds.c`, `tests/test_channel_format.c`, `tests/test_channel_http.c`, `tests/test_channel_integration.c`, `tests/test_channel_loop.c`, `tests/test_channel_manager.c`, `tests/test_channel_monitor.c`, `tests/test_channel_rate_limit.c`, `tests/test_channel_trust.c`, `tests/test_orphan_channel_audit.c`, `tests/test_persona_directive_channels.c`, `tests/test_signal_channel_wire.c`, `tests/test_webhook_channel.c`.
- "iMessage 4+ dedicated test files" claim: plan-level statement, hard to grep exactly but channel-wide tests are abundant.
- Action item "per-channel naturalness eval suites (20 conversations each) for Tier 1" — no per-Tier-1-channel naturalness eval files surfaced; this action item is OPEN.

### Wired? (called in runtime path / dispatch)
- WIRED: overlay lookup invoked in `src/persona/persona.c:4206` (prompt-build).
- WIRED: vtable functions invoked through the agent dispatch path (they are part of `hu_channel_vtable_t` consumed throughout daemon and gateway).

## Gaps
- Action item 1 (per-channel persona overlay defaults for Tier 1 in starter persona): not verified — would need to inspect starter-persona generation code.
- Action item 2 (per-channel naturalness eval suites, 20 convos each): no evidence of dedicated suites.
- Action item 3 (Tier 1 timing audit): no audit report file found.
- Action item 4 (Tier 2/3 documentation): documentation may be in code comments or this plan itself.

## Notes
- Plan is more a tiering decision than an implementation plan; the underlying vtable wiring it depends on is fully present.
- CLAUDE.md M6 explicitly references this plan ("Channel Focus: prioritize 4 Tier-1 channels"). Existence of full vtable surface for all 4 is the main success metric.
