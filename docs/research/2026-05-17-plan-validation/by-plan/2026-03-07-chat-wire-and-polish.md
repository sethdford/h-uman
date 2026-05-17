---
plan: docs/plans/2026-03-07-chat-wire-and-polish.md
auditor: group-1-ui-design
audited_at: 2026-05-17
implemented: PARTIAL
proven: PARTIAL
wired: FULL
verdict: SUPERSEDED
confidence: HIGH
---

## Plan Summary
Close gap between rich chat components and SOTA messaging: wire delivery
status, grouped timestamps, polish layout, retry on failed sends, and remove
the now-dead hu-message-list and hu-message-stream components.

## Key Claims (from the plan)
- Claim 1: ChatController sets `status` on user messages (sending/sent/failed)
- Claim 2: hu-message-thread renders hu-delivery-status when status set
- Claim 3: Retry affordance on failed sends; controller has `retry(id)`
- Claim 4: Delete `hu-message-list.ts` and `hu-message-stream.ts`

## Evidence

### Implemented? (code exists)
- `ui/src/controllers/chat-controller.ts:193` sets `status: "failed"`;
  `:208` sets `status: "sending"`; `:264-273` implements `retry()` that flips
  status back to sending; `:307` checks `status === "failed"`.
- `ui/src/components/hu-delivery-status.ts` exists.
- `hu-message-list.ts` is GONE (deletion claim satisfied).
- `hu-message-stream.ts` STILL EXISTS — deletion only half-applied.

### Proven? (tests exist)
- `ui/src/controllers/chat-controller.test.ts` exists.
- `extra-components.test.ts` imports `hu-message-stream.js` (so the surviving
  component is still under test, not orphaned).

### Wired? (called in runtime path / dispatch)
- `chat-view.ts:15` imports hu-message-thread; uses ChatController as the
  reactive controller at line 289 — wiring intact.

## Gaps
- `hu-message-stream.ts` was not removed despite the plan stating "remove
  unused components replaced by hu-message-thread". It's still imported by
  tests; whether it's wired into runtime is unclear.

## Notes
Plan frontmatter is `status: superseded`. Most behavioral targets (status wiring,
retry) shipped; the "delete dead code" cleanup task is half-done.
