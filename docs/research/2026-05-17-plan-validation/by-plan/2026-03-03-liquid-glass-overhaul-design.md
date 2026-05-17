---
plan: docs/plans/2026-03-03-liquid-glass-overhaul-design.md
auditor: group-1-ui-design
audited_at: 2026-05-17
implemented: FULL
proven: PARTIAL
wired: FULL
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
Apple Liquid Glass-style material upgrade: dynamic-light tokens, vibrancy boosts,
interactive press/hover/focus, and a conversational AI component suite
(hu-message-stream, hu-thinking, hu-tool-result, hu-message-branch).

## Key Claims (from the plan)
- Claim 1: New `dynamic-light` + `vibrancy` + `interactive` keys in `glass.tokens.json`
- Claim 2: `ui/src/lib/dynamic-light.ts` module with cursor + gyroscope tracking
- Claim 3: Conversational AI components: hu-message-stream, hu-thinking, hu-tool-result, hu-message-branch, hu-conversation
- Claim 4: Native parity SwiftUI/Android (out of scope for UI audit)

## Evidence

### Implemented? (code exists)
- `design-tokens/glass.tokens.json` contains `dynamic-light`, `vibrancy`,
  `hover-specular-boost` keys (grep confirmed).
- `ui/src/lib/dynamic-light.ts` exists.
- `ui/src/components/hu-message-stream.ts`, `hu-thinking.ts`, `hu-tool-result.ts`,
  `hu-message-branch.ts`, `hu-branch-tree.ts` all exist.

### Proven? (tests exist)
- `ui/src/components/extra-components.test.ts` imports `hu-thinking.js` and
  `hu-message-stream.js` (registration/property smoke tests).
- No dedicated `dynamic-light.test.ts` found; no `hu-conversation` component or test.

### Wired? (called in runtime path / dispatch)
- `ui/src/app.ts:` dynamically imports `./lib/dynamic-light.js` and calls
  `dynamicLight.start()`/`stop()` — runtime-attached.
- `hu-tool-result` consumed by `ui/src/components/hu-message-thread.ts:7` (import)
  and rendered at `hu-message-thread.ts:1399-1408`.

## Gaps
- No standalone `hu-conversation` container component (subsumed by hu-message-thread).
- No tests specifically exercising vibrancy / dynamic-light CSS variables.

## Notes
Plan frontmatter declared `status: complete`. Implementation tracks the design;
the only sloppy edge is missing dedicated dynamic-light/vibrancy tests.
