---
plan: docs/plans/2026-03-07-project-scalpel.md
auditor: group-1-ui-design
audited_at: 2026-05-17
implemented: PARTIAL
proven: PARTIAL
wired: PARTIAL
verdict: PARTIAL
confidence: HIGH
---

## Plan Summary
Implementation plan for Project Scalpel: Phase 1 extracts ChatController + bug
fixes, then progressive decomposition of chat-view.ts into hu-composer,
hu-message-list, hu-message-actions, hu-chat-sessions-panel, hu-file-preview.

## Key Claims (from the plan)
- Claim 1: `ui/src/controllers/chat-controller.ts` + tests
- Claim 2: hu-composer + hu-message-list components
- Claim 3: hu-message-actions, hu-chat-sessions-panel, hu-file-preview
- Claim 4: hu-context-menu wired into chat-view; dead CSS removed

## Evidence

### Implemented? (code exists)
- `ui/src/controllers/chat-controller.ts` exists with full state machine
  (sending/sent/failed, retry, abort, regenerate). Status hooks at lines
  193/208/264/307 (confirmed via grep).
- `hu-message-actions.ts`, `hu-message-group.ts`, `hu-file-preview.ts`,
  `hu-chat-sessions-panel.ts`, `hu-context-menu.ts` exist.
- DOES NOT EXIST: `hu-composer.ts`, `hu-message-list.ts`.
- chat-view.ts: 1,289 lines — the ~200-line target was not achieved.

### Proven? (tests exist)
- `chat-controller.test.ts` exists with comprehensive scenarios documented in
  the plan (send, retry, chunk handling, state transitions).
- `hu-message-actions.test.ts`, `hu-file-preview.test.ts`,
  `hu-chat-sessions-panel.test.ts` all exist.

### Wired? (called in runtime path / dispatch)
- ChatController bound in chat-view.ts:289.
- hu-message-thread (not hu-message-list) is the actual list renderer.

## Gaps
- Composer extraction not done; chat-view.ts not slimmed.
- hu-message-list never created — replaced by hu-message-thread in practice.

## Notes
`status: superseded`. ChatController + most peripheral chat components
shipped. The two largest decomposition targets (composer, message-list)
diverged from the plan in favor of `hu-message-thread`.
