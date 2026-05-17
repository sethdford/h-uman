---
plan: docs/plans/2026-03-07-project-scalpel-design.md
auditor: group-1-ui-design
audited_at: 2026-05-17
implemented: PARTIAL
proven: PARTIAL
wired: PARTIAL
verdict: PARTIAL
confidence: HIGH
---

## Plan Summary
Design doc to decompose the 1,179-line chat-view.ts god object into a thin
~200-line orchestrator + 6 focused components + a ChatController. Promises new
hu-composer, hu-message-list, hu-message-actions, hu-chat-sessions-panel,
hu-file-preview, plus wiring hu-context-menu, fixing dead CSS, adaptive
Shiki code theme.

## Key Claims (from the plan)
- Claim 1: chat-view.ts becomes ~200-line orchestrator
- Claim 2: ChatController owns gateway/streaming/cache state
- Claim 3: hu-composer + hu-message-list + hu-message-actions +
  hu-chat-sessions-panel + hu-file-preview exist
- Claim 4: hu-context-menu wired; dead .tool-card CSS removed; adaptive
  Shiki theme

## Evidence

### Implemented? (code exists)
- ChatController exists: `ui/src/controllers/chat-controller.ts`.
- Exist: `hu-chat-sessions-panel.ts`, `hu-message-actions.ts`,
  `hu-message-group.ts`, `hu-file-preview.ts`, `hu-context-menu.ts`.
- MISSING: `hu-composer.ts`, `hu-message-list.ts`. The composer logic stayed
  inside chat-view.ts; messages now use the `hu-message-thread.ts` umbrella
  component that wasn't in the plan.
- chat-view.ts is STILL 1,289 lines — the ~200-line target was not met.

### Proven? (tests exist)
- `ui/src/controllers/chat-controller.test.ts`,
  `hu-message-actions.test.ts`, `hu-file-preview.test.ts`,
  `hu-chat-sessions-panel.test.ts` exist.
- No hu-composer or hu-message-list tests (because those components don't exist).

### Wired? (called in runtime path / dispatch)
- `chat-view.ts:11` imports ChatController; `:289` instantiates it.
- `chat-view.ts:15` imports hu-message-thread; uses it for message rendering.
- The original 6-component decomposition was not adopted — the codebase took
  the hu-message-thread route instead.

## Gaps
- Composer never extracted; chat-view.ts not slimmed to ~200 lines.
- hu-message-list never created; hu-message-thread used as a richer alternative.

## Notes
Marked `status: superseded`. The architectural direction shifted — message
list/composer became part of `hu-message-thread` rather than two separate
components. ChatController + most action/session components shipped.
