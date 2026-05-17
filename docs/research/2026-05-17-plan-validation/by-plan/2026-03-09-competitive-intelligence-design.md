---
plan: docs/plans/2026-03-09-competitive-intelligence-design.md
auditor: group-3-better-than-human-gateway-competitive-quality
audited_at: 2026-05-17
implemented: FULL
proven: PARTIAL
wired: FULL
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
Design doc for the competitive benchmarking program: 15-brand registry,
7-scoring dimensions, raised Lighthouse thresholds, new
`scripts/benchmark-competitive.sh`, new `competitive-benchmark.yml`
workflow, quality scorecard. Plan frontmatter: `status: complete`.

## Key Claims (from the plan)
- `docs/competitive-benchmarks.md` — registry of 15 brands
- `docs/quality-scorecard.md` — per-surface scores
- `scripts/benchmark-competitive.sh` — PageSpeed API harness
- `.github/workflows/competitive-benchmark.yml` — weekly job
- Upgraded `visual-standards.md`, `ux-patterns.md`, `motion-design.md`,
  `design-strategy.md` with category-defining bars
- Raised `.lighthouserc.json` thresholds

## Evidence

### Implemented? (code exists) — FULL
- `docs/competitive-benchmarks.md` ✓
- `docs/quality-scorecard.md` ✓
- `scripts/benchmark-competitive.sh` ✓ (10+ references to "competitor"/`benchmark-competitive`)
- `.github/workflows/competitive-benchmark.yml` ✓
- `docs/standards/design/visual-standards.md`, `ux-patterns.md`, `motion-design.md`, `design-strategy.md` ✓
- `.lighthouserc.json` ✓

### Proven? (tests exist) — PARTIAL
- No automated tests for the script itself (it is a CLI shell tool).
- The workflow IS the operational test — it runs weekly per `competitive-benchmark.yml`.
- Lighthouse CI assertions in `.lighthouserc.json` function as enforcement
  thresholds, not unit tests.

### Wired? (called in runtime path / dispatch) — FULL
- Weekly schedule + manual dispatch in `.github/workflows/competitive-benchmark.yml`.
- `.lighthouserc.json` referenced by `ci.yml` / Lighthouse CI.
- Standards docs cited in `AGENTS.md` and project rules.

## Gaps
- No unit test for the bash script (acceptable; it's a network-bound
  harness gated on `PAGESPEED_API_KEY`).
- Confidence the script *currently* succeeds against live competitor URLs
  was not validated in this audit (would require running CI).

## Notes
Companion plan `2026-03-09-competitive-intelligence.md` is the
task-by-task implementation guide for this design doc. Both shipped.
