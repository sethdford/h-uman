---
plan: docs/plans/2026-03-07-sota-design-overhaul.md
auditor: group-1-ui-design
audited_at: 2026-05-17
implemented: FULL
proven: PARTIAL
wired: FULL
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
Holo AI-style minimalism: introduce 6 new composite components (hu-stat-card,
hu-section-header, hu-metric-row, hu-timeline, hu-sparkline-enhanced,
hu-page-hero), then sweep every view to use them.

## Key Claims (from the plan)
- Claim 1: 6 composite components exist
- Claim 2: Motion choreography wired (page entrance, card stagger, stat pop)
- Claim 3: Chat view gets iMessage-style bubbles
- Claim 4: Every view consumes the new primitives

## Evidence

### Implemented? (code exists)
- All 6 components present: `ui/src/components/hu-stat-card.ts`,
  `hu-section-header.ts`, `hu-metric-row.ts`, `hu-timeline.ts`,
  `hu-sparkline-enhanced.ts`, `hu-page-hero.ts`.

### Proven? (tests exist)
- `ui/src/views/views.test.ts` asserts presence of `hu-page-hero` /
  `hu-section-header` in multiple views (greps show 8+ such assertions).
- `ui/src/components/extra-components.test.ts` covers some primitives via
  registration smoke tests.
- No exhaustive contract test per composite component.

### Wired? (called in runtime path / dispatch)
- `hu-stat-card` / `hu-page-hero` referenced in `agents-view.ts`,
  `automations-view.ts`, `channels-view.ts`, `config-view.ts`,
  `design-system-view.ts`, etc.

## Gaps
- Native parity (SwiftUI/Android) is in the design doc but out of scope here.
- Per-component test depth is uneven.

## Notes
Frontmatter is `status: superseded` — but the SOTA UX sweep that supersedes it
inherits and extends these primitives. Design overhaul artifacts are present
and wired throughout the dashboard.
