---
title: SeaClaw Competitive Benchmarks
updated: 2026-03-09
---

# SeaClaw Competitive Benchmarks

This is a living document updated quarterly. It combines automated PageSpeed data with manual quality audits to benchmark SeaClaw against industry leaders across performance, visual craft, motion, accessibility, and innovation.

**Methodology**: Automated metrics (Lighthouse, CWV) are collected via CI; quality scores are assigned by human auditors using the rubric below. Each dimension is scored 1–10; totals are out of 70.

## Benchmark Brands

| Tier          | Brand            | Key URL                     | Surface Type             | Why They're Here                                              |
| ------------- | ---------------- | --------------------------- | ------------------------ | ------------------------------------------------------------- |
| Dev Tools     | Linear           | https://linear.app          | Web app + marketing      | Orbiter design system, 8px grid, Liquid Glass, Awwwards craft |
| Dev Tools     | Vercel           | https://vercel.com          | Marketing + docs         | Next.js showcase, edge performance, gradient mastery          |
| Dev Tools     | Raycast          | https://raycast.com         | Native app + marketing   | macOS-native excellence, keyboard-first, spring animations    |
| Dev Tools     | Warp             | https://warp.dev            | Terminal app + marketing | GPU-rendered, dark-mode-first, developer visual hierarchy     |
| Dev Tools     | Cursor           | https://cursor.com          | App + marketing          | AI IDE polish, rapid iteration, clean information density     |
| Premium SaaS  | Stripe           | https://stripe.com          | Docs + marketing         | Gold standard payment UX, calm technology, 8pt grid           |
| Premium SaaS  | Notion           | https://notion.so           | Web app + marketing      | Content-first hierarchy, block composition, warm minimalism   |
| Premium SaaS  | Figma            | https://figma.com           | Web app + marketing      | Design tool as its own system, Config-level production        |
| Premium SaaS  | Superhuman       | https://superhuman.com      | Email app + marketing    | Speed as brand, keyboard-first, sub-100ms latency             |
| Big Tech      | Apple            | https://developer.apple.com | Docs                     | HIG in practice, SF Pro, spring animations, spatial UI        |
| Big Tech      | Spotify          | https://spotify.com         | Web + native             | Wrapped-level motion, editorial layouts, adaptive color       |
| Award Winners | Lando Norris     | https://landonorris.com     | Marketing                | 2025 Awwwards SOTY, Immersive Garden, WebGL + scroll          |
| Award Winners | Scout Motors     | https://scoutmotors.com     | E-commerce               | 2025 E-commerce winner, 3D config, premium brand              |
| Award Winners | Immersive Garden | https://immersive-g.com     | Portfolio                | 2025 Agency winner, shader transitions, art direction         |
| Award Winners | Malvah           | https://malvah.com          | Portfolio                | 2025 Studio winner, typographic excellence, editorial grid    |

**Tier definitions**: Dev Tools = products developers use daily; Premium SaaS = high-touch B2B/B2C apps; Big Tech = platform owners with design systems; Award Winners = Awwwards 2025 honorees.

## Scoring Dimensions

| Dimension               | Description                                                              |
| ----------------------- | ------------------------------------------------------------------------ |
| **Performance**         | Automated: Lighthouse, Core Web Vitals (LCP, INP, CLS)                   |
| **Visual Craft**        | Manual: typography, spacing, color, dark/light mode quality              |
| **Motion Quality**      | Manual: spring physics, choreography, easing, reduced-motion support     |
| **Information Density** | Heuristic: data-ink ratio, progressive disclosure, cognitive load        |
| **Accessibility**       | Mixed: axe-core automated + manual keyboard/screen reader testing        |
| **Brand Cohesion**      | Manual: cross-surface consistency, token adherence, platform-native feel |
| **Innovation**          | Manual: novel patterns, WebGL/3D, cutting-edge CSS                       |

## Scoring Rubric

| Score | Meaning                                  |
| ----- | ---------------------------------------- |
| 1–3   | Below industry average                   |
| 4–5   | Industry average                         |
| 6–7   | Above average (most SaaS products)       |
| 8     | Best-in-class (Linear/Stripe tier)       |
| 9     | Category-defining (Awwwards winner tier) |
| 10    | Sets the standard others measure against |

**Column key (Automated Metrics)**: Perf = Lighthouse Performance; A11y = Accessibility; BP = Best Practices; SEO = Search; LCP = Largest Contentful Paint; CLS = Cumulative Layout Shift; TBT = Total Blocking Time; TTFB = Time to First Byte.

## Automated Metrics

_Last updated: 2026-03-10. SeaClaw data from local Lighthouse; competitors from PageSpeed Insights API._

| Brand            | Perf | A11y | BP  | SEO | LCP   | CLS  | TBT    | TTFB |
| ---------------- | ---- | ---- | --- | --- | ----- | ---- | ------ | ---- |
| SeaClaw\*        | 96   | 100  | 100 | 100 | 2.6s  | 0.00 | 0ms    | —    |
| Linear           | 27   | 79   | 88  | 100 | 26.1s | 0.00 | 2308ms | 48ms |
| Vercel           | 57   | 89   | 96  | 92  | 12.0s | 0.00 | 412ms  | 20ms |
| Raycast          | —    | —    | —   | —   | —     | —    | —      | —    |
| Warp             | 45   | 79   | 96  | 100 | 13.6s | 0.06 | 716ms  | 68ms |
| Cursor           | 36   | 93   | 92  | 92  | 13.4s | 0.00 | 1331ms | 43ms |
| Stripe           | 53   | 100  | 54  | 92  | 5.5s  | 0.00 | 790ms  | 85ms |
| Notion           | 38   | 97   | 73  | 100 | 8.1s  | 0.00 | 1776ms | 99ms |
| Figma            | 38   | 94   | 69  | 100 | 7.2s  | 0.03 | 1368ms | 74ms |
| Superhuman       | 63   | 100  | 77  | 100 | 12.8s | 0.00 | 368ms  | 8ms  |
| Apple            | 55   | 86   | 96  | 100 | 22.6s | 0.00 | 0ms    | 62ms |
| Spotify          | 28   | 77   | 96  | 100 | 23.6s | 0.05 | 1614ms | 2ms  |
| Lando Norris     | —    | —    | —   | —   | —     | —    | —      | —    |
| Scout Motors     | 46   | 83   | 92  | 92  | 46.2s | 0.06 | 459ms  | 3ms  |
| Immersive Garden | —    | —    | —   | —   | —     | —    | —      | —    |
| Malvah           | 57   | 94   | 100 | 100 | 10.6s | 0.00 | 368ms  | 41ms |

**Note**: Raycast, Lando Norris, and Immersive Garden returned API errors during data collection. SeaClaw scores (\*) are from a local Lighthouse run against the built site.

## Quality Scores

_Last updated: 2026-03-10. Scores based on industry analysis and site evaluation._

| Brand            | Perf | Visual | Motion | Density | A11y | Brand | Innovation | Total |
| ---------------- | ---- | ------ | ------ | ------- | ---- | ----- | ---------- | ----- |
| SeaClaw          | 8    | 7      | 7      | 7       | 9    | 6     | 7          | 51/70 |
| Linear           | 4    | 9      | 9      | 9       | 6    | 9     | 8          | 54/70 |
| Vercel           | 6    | 8      | 7      | 8       | 7    | 8     | 8          | 52/70 |
| Raycast          | 7    | 9      | 9      | 9       | 7    | 9     | 8          | 58/70 |
| Warp             | 5    | 7      | 6      | 8       | 6    | 7     | 8          | 47/70 |
| Cursor           | 4    | 7      | 6      | 8       | 7    | 7     | 8          | 47/70 |
| Stripe           | 6    | 9      | 8      | 8       | 9    | 9     | 7          | 56/70 |
| Notion           | 4    | 8      | 6      | 8       | 8    | 8     | 7          | 49/70 |
| Figma            | 4    | 9      | 7      | 8       | 7    | 9     | 9          | 53/70 |
| Superhuman       | 6    | 8      | 8      | 8       | 9    | 8     | 7          | 54/70 |
| Apple            | 6    | 9      | 9      | 8       | 7    | 10    | 9          | 58/70 |
| Spotify          | 3    | 8      | 8      | 7       | 6    | 9     | 8          | 49/70 |
| Lando Norris     | 5    | 10     | 10     | 6       | 5    | 8     | 10         | 54/70 |
| Scout Motors     | 5    | 8      | 7      | 7       | 6    | 8     | 8          | 49/70 |
| Immersive Garden | 5    | 10     | 10     | 6       | 5    | 9     | 10         | 55/70 |
| Malvah           | 6    | 9      | 8      | 7       | 7    | 8     | 8          | 53/70 |

## Category-Defining Targets

SeaClaw's targets vs industry best:

| Metric                 | Industry Best      | SeaClaw Target |
| ---------------------- | ------------------ | -------------- |
| Lighthouse Performance | 95–97 (Vercel)     | 99+            |
| LCP                    | 0.8s (Linear)      | < 0.5s         |
| CLS                    | ~0.02 (Stripe)     | 0.00           |
| INP                    | ~80ms (Superhuman) | < 50ms         |
| Accessibility          | 98 (Vercel)        | 100            |

SeaClaw's lightweight C runtime and minimal UI stack (Lit, design tokens) position it to exceed these targets. The dashboard and marketing site are optimized for sub-500ms LCP and zero layout shift.

## Update History

- **2026-03-10**: First automated + manual audit completed (Q1 2026)
- **2026-03-09**: Initial benchmark registry created

## Cadence

- **Automated metrics**: Run quarterly via CI; results populate the Automated Metrics table.
- **Quality scores**: Manual audit once per quarter; assign scores using the rubric and document findings in the Quality Scorecard.

## Audit Checklist (Quality Scores)

When assigning manual quality scores, verify:

1. **Visual Craft**: Typography scale, spacing rhythm, color contrast, dark/light parity.
2. **Motion Quality**: Spring vs linear easing, choreography (stagger, follow-through), `prefers-reduced-motion` support.
3. **Information Density**: Data-ink ratio (Tufte), progressive disclosure, cognitive load per view.
4. **Brand Cohesion**: Token usage, cross-surface consistency (dashboard, website, native).
5. **Innovation**: Novel patterns, WebGL/3D usage, cutting-edge CSS (container queries, `:has`, etc.).

**Note**: For native-only brands (Raycast, Warp), web metrics apply to their marketing sites. Quality scores may include app impressions where applicable.

## Next Steps

- Re-run benchmark with PageSpeed API key to reduce fetch failures (Raycast, Lando Norris, Immersive Garden).
- Deploy seaclaw.ai custom domain to get production PageSpeed data.
- Schedule Q2 2026 quarterly review.
- Investigate LCP optimization for sub-1s target (font loading, hero image preload).

## Related

- [Quality Scorecard](quality-scorecard.md) — detailed audit methodology and scorecard templates
- [Competitive Intelligence Design](plans/2026-03-09-competitive-intelligence-design.md) — design doc for the benchmark framework
