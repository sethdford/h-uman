---
title: Human Quality Scorecard
updated: 2026-03-09
---

# Human Quality Scorecard

> Per-surface quality scores across 7 dimensions. Updated quarterly.
> Target: score higher than every benchmark brand on every dimension.

## Scoring Scale

| Score | Meaning                                  |
| ----- | ---------------------------------------- |
| 1-3   | Below industry average                   |
| 4-5   | Industry average                         |
| 6-7   | Above average (most SaaS products)       |
| 8     | Best-in-class (Linear, Stripe tier)      |
| 9     | Category-defining (Awwwards winner tier) |
| 10    | Sets the standard others measure against |

## Current Scores (Q1 2026)

| Surface   | Perf | Visual | Motion | Density | A11y | Brand | Innovation | Total | Target      |
| --------- | ---- | ------ | ------ | ------- | ---- | ----- | ---------- | ----- | ----------- |
| Website   | 8    | 7      | 7      | 7       | 9    | 7     | 7          | 52/70 | 63+ (9 avg) |
| Dashboard | 7    | 7      | 8      | 7       | 8    | 7     | 8          | 52/70 | 63+ (9 avg) |
| iOS       | 5    | 6      | 5      | 6       | 6    | 6     | 5          | 39/70 | 63+ (9 avg) |
| macOS     | 5    | 6      | 5      | 6       | 6    | 6     | 5          | 39/70 | 63+ (9 avg) |
| Android   | 4    | 5      | 5      | 5       | 5    | 5     | 5          | 34/70 | 63+ (9 avg) |

## Benchmark Comparison

| Brand        | Perf | Visual | Motion | Density | A11y | Brand | Innovation | Total |
| ------------ | ---- | ------ | ------ | ------- | ---- | ----- | ---------- | ----- |
| Linear       | 4    | 9      | 9      | 9       | 6    | 9     | 8          | 54/70 |
| Vercel       | 6    | 8      | 7      | 8       | 7    | 8     | 8          | 52/70 |
| Stripe       | 6    | 9      | 8      | 8       | 9    | 9     | 7          | 56/70 |
| Superhuman   | 6    | 8      | 8      | 8       | 9    | 8     | 7          | 54/70 |
| Raycast      | 7    | 9      | 9      | 9       | 7    | 9     | 8          | 58/70 |
| Figma        | 4    | 9      | 7      | 8       | 7    | 9     | 9          | 53/70 |
| Lando Norris | 5    | 10     | 10     | 6       | 5    | 8     | 10         | 54/70 |

## Dimension Rubrics

Detailed rubrics for each of the 7 dimensions:

**Performance (automated)**:

- 1-5: Lighthouse <90, LCP >2s, CLS >0.1
- 6-7: Lighthouse 90-95, LCP 1-2s, CLS <0.1
- 8: Lighthouse 95-97, LCP <1s, CLS <0.05
- 9: Lighthouse 98-99, LCP <0.5s, CLS ~0
- 10: Lighthouse 100, LCP <0.3s, CLS 0.00, INP <50ms

**Visual Craft (manual audit)**:

- 1-5: Inconsistent spacing, raw hex colors, mixed type scales
- 6-7: Token-based, consistent but unremarkable
- 8: Stripe/Linear tier — mathematically precise, dark/light coherent
- 9: Awwwards nomination quality — every pixel intentional
- 10: Sets new standard — competitors study your UI for inspiration

**Motion Quality (manual audit)**:

- 1-5: CSS transitions only, no spring physics, jarring
- 6-7: Smooth transitions, basic easing tokens
- 8: Spring-first, stagger choreography, reduced-motion support
- 9: Physics-accurate springs, scroll-driven narratives, ambient intelligence
- 10: Indistinguishable from native platform animations on every surface

**Information Density (heuristic evaluation)**:

- 1-5: Cluttered or wastefully sparse, legends required
- 6-7: Clean layout, standard data presentation
- 8: Tufte-level data-ink ratio, progressive disclosure
- 9: Information Theater — understanding is involuntary
- 10: Zero learning curve for any data visualization

**Accessibility (automated + manual)**:

- 1-5: WCAG violations, missing focus management
- 6-7: WCAG AA compliant, basic keyboard support
- 8: 98+ Lighthouse a11y, full keyboard nav, screen reader tested
- 9: 100 Lighthouse, VoiceOver/TalkBack tested, high contrast mode
- 10: Exceeds WCAG AAA, cognitive accessibility considered

**Brand Cohesion (cross-platform audit)**:

- 1-5: Inconsistent across surfaces, no design system
- 6-7: Shared tokens, mostly consistent
- 8: Unified system across web + native, platform-appropriate adaptations
- 9: Each surface feels native AND unmistakably Human
- 10: Platform Transcendence — best-designed app on every device

**Innovation (feature inventory)**:

- 1-5: Standard patterns only
- 6-7: Modern CSS, reasonable interactivity
- 8: Cutting-edge CSS (container queries, view transitions)
- 9: Features competitors don't have yet (spatial UI, ambient intelligence)
- 10: Defining new interaction paradigms

## History

| Quarter | Website | Dashboard | iOS | macOS | Android |
| ------- | ------- | --------- | --- | ----- | ------- |
| Q1 2026 | 52      | 52        | 39  | 39    | 34      |

## Action Items from Last Review

- **Website Performance**: Lighthouse 96 — target 99+. Remaining gap: LCP (2.6s local). Investigate font loading and hero image optimization.
- **Visual Craft (all surfaces)**: 7 — target 9. Invest in editorial typography, mathematically precise spacing, dark/light parity polish.
- **Motion Quality (native apps)**: 5 — target 8. Implement spring animations in SwiftUI/Compose, add meaningful transitions.
- **Native App Maturity**: iOS/macOS 39/70, Android 34/70. Priority: bring native apps to feature parity with dashboard.
- **Brand Cohesion**: 6-7 — target 9. Strengthen cross-surface consistency; native apps need design token integration.

## Related

- [Competitive Benchmarks](competitive-benchmarks.md)
- [Design doc](plans/2026-03-09-competitive-intelligence-design.md)
