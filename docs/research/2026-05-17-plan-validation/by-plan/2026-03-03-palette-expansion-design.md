---
plan: docs/plans/2026-03-03-palette-expansion-design.md
auditor: group-1-ui-design
audited_at: 2026-05-17
implemented: FULL
proven: NONE
wired: PARTIAL
verdict: PARTIAL
confidence: MEDIUM
---

## Plan Summary
Unify on ocean-teal as the brand primary and add two new ramps — amber
(accent-secondary) and indigo (accent-tertiary) — across base + semantic tokens
and website CSS; demote coral to error-only.

## Key Claims (from the plan)
- Claim 1: amber + indigo primitives in `design-tokens/base.tokens.json`
- Claim 2: `accent-secondary*` and `accent-tertiary*` semantic tokens
- Claim 3: `website/src/styles/global.css` replaces coral with teal/amber/indigo
- Claim 4: Generated outputs (CSS, Swift, Kotlin, C) rebuilt

## Evidence

### Implemented? (code exists)
- `design-tokens/base.tokens.json`: `amber` ramp with description "Warm secondary
  accent — amber/gold for highlights and featured content".
- `design-tokens/semantic.tokens.json`: full set of `accent-secondary` /
  `accent-tertiary` tokens including `text`, `hover`, `subtle`, `strong`,
  `on-accent-secondary`, etc.
- `ui/src/styles/theme.css:1686-1786` consumes `--hu-accent-secondary` and
  `--hu-accent-tertiary` in gradients and color-mix overlays.

### Proven? (tests exist)
- NONE FOUND — no test files under `design-tokens/` or `tests/` reference amber,
  indigo, or accent-secondary/tertiary contracts. Visual regression in CI may
  cover this but is not a token-contract test.

### Wired? (called in runtime path / dispatch)
- UI dashboard: WIRED (`ui/src/styles/theme.css` references both tokens 10+
  times for gradients/mesh).
- Website CSS: `website/src/styles/global.css` does NOT contain literal
  references to coral / amber / indigo / --color-accent / teal — it relies on
  imported `_tokens.css`. Cannot confirm coral was *removed* from website
  surfaces; doc pages still describe the palette correctly.

## Gaps
- No token-contract test pinning the amber/indigo ramps or semantic mappings.
- Could not directly verify website CSS coral demotion via grep (file imports
  tokens; would require inspecting `_tokens.css` and component-level usage).

## Notes
Plan frontmatter declares `status: complete`. Token shipment is verified; runtime
wiring in the dashboard is strong; website-side claim of "coral demoted" requires
deeper inspection.
