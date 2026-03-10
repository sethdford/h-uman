---
title: Competitive Intelligence & Category-Defining Standards
date: 2026-03-09
status: approved
---

# Competitive Intelligence & Category-Defining Standards

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build a formal competitive benchmarking framework, raise all design standards above the industry ceiling, and create automated infrastructure to measure and enforce category-defining quality across every UI surface.

**Architecture:** Competitive benchmark registry (15 brands, 7 scoring dimensions) + upgraded design docs with category-defining bars + automated PageSpeed/Lighthouse/CWV competitive analysis scripts + per-surface quality scorecard.

**Tech Stack:** Google PageSpeed Insights API (v5), Lighthouse CI, axe-core, Playwright, shell scripts, markdown docs.

---

## 1. Design Context

### Problem

Human's design system references SOTA sources (Apple HIG, Material Design 3, Disney/Pixar, Tufte, Dieter Rams, NNG) but lacks:

- **Named competitive targets** — no specific brands to measure against
- **Quantified deltas** — no data on where we stand vs. leaders
- **Category-defining ambition** — current bars are "match industry good" not "redefine the ceiling"
- **Automated competitive measurement** — Lighthouse runs on us but not on competitors
- **Cross-surface quality scoring** — no unified scorecard across website, dashboard, iOS, Android, Flutter

### Vision

Human becomes the brand that other developer tools benchmark against. Not parity — dominance. Every UI surface simultaneously the most functional AND the most beautiful version of that element in the developer tools space.

## 2. Competitive Benchmark Registry

### 2.1 Benchmark Brands (15)

| Tier              | Brand                          | Surface                       | Why They're Here                                                        |
| ----------------- | ------------------------------ | ----------------------------- | ----------------------------------------------------------------------- |
| **Dev Tools**     | Linear                         | Web app + marketing site      | Orbiter design system, 8px grid, Liquid Glass, Awwwards-level craft     |
|                   | Vercel                         | Marketing site + docs         | Next.js showcase, edge performance, gradient mastery, geometric motion  |
|                   | Raycast                        | Native app + marketing site   | macOS-native excellence, keyboard-first UX, spring animations           |
|                   | Warp                           | Terminal app + marketing site | GPU-rendered, dark-mode-first, developer-focused visual hierarchy       |
|                   | Cursor                         | App + marketing site          | AI IDE polish, rapid iteration, clean information density               |
| **Premium SaaS**  | Stripe                         | Docs + marketing site         | Gold standard payment UX, "calm technology", 8pt grid, Inter typography |
|                   | Notion                         | Web app + marketing site      | Content-first hierarchy, block-based composition, warm minimalism       |
|                   | Figma                          | Web app + marketing site      | Design tool as its own design system, Config-level production quality   |
|                   | Superhuman                     | Email app + marketing site    | Speed as brand identity, keyboard-first, sub-100ms interaction latency  |
| **Big Tech**      | Apple (developer.apple.com)    | Docs site                     | HIG lived in practice, SF Pro, spring animations, spatial UI            |
|                   | Spotify                        | Web + native apps             | Wrapped-level motion design, editorial layouts, adaptive color          |
| **Award Winners** | Lando Norris (2025 SOTY)       | Marketing                     | Immersive Garden studio, bleeding-edge WebGL + scroll interaction       |
|                   | Scout Motors (2025 E-commerce) | E-commerce                    | Automotive-grade 3D, config flows, premium brand feel                   |
|                   | Immersive Garden (2025 Agency) | Portfolio                     | Experimental motion, shader-based transitions, art direction            |
|                   | Malvah (2025 Studio)           | Portfolio                     | Typographic excellence, editorial grid, restrained animation            |

### 2.2 Scoring Dimensions (7)

Each brand scored 1-10. Updated quarterly.

| Dimension               | What We Measure                                                               | Tool/Method                               |
| ----------------------- | ----------------------------------------------------------------------------- | ----------------------------------------- |
| **Performance**         | Lighthouse score, LCP, CLS, INP, TTFB                                         | PageSpeed Insights API, Lighthouse CI     |
| **Visual Craft**        | Typography precision, spacing rhythm, color harmony, dark/light coherence     | Manual audit (rubric-based)               |
| **Motion Quality**      | Spring physics, stagger choreography, easing quality, reduced-motion support  | Manual audit + DevTools performance panel |
| **Information Density** | Data-ink ratio, progressive disclosure, cognitive load per viewport           | Heuristic evaluation                      |
| **Accessibility**       | WCAG AA compliance, keyboard nav, screen reader, contrast ratios              | axe-core automated + manual audit         |
| **Brand Cohesion**      | Consistency across surfaces, token adherence, platform-native feel            | Cross-platform audit                      |
| **Innovation**          | Novel interaction patterns, WebGL/3D, adaptive UIs, cutting-edge CSS adoption | Feature inventory                         |

## 3. Category-Defining Quality Bars

### 3.1 Performance — Redefine the Ceiling

| Metric                    | Industry Best         | Human Target | Strategy                                                           |
| ------------------------- | --------------------- | -------------- | ------------------------------------------------------------------ |
| Lighthouse Performance    | 95-97 (Vercel)        | **99+**        | Astro static + aggressive prefetch + edge CDN                      |
| LCP                       | 0.8s (Linear)         | **< 0.5s**     | Inline critical CSS, prerendered content, zero JS in critical path |
| CLS                       | ~0.02 (Stripe)        | **0.00**       | No layout shift — explicit dimensions on all elements              |
| INP                       | ~80ms (Superhuman)    | **< 50ms**     | Event handler audit, web worker offloading, requestIdleCallback    |
| TTFB                      | 45-80ms (Vercel edge) | **< 40ms**     | Edge-first deployment, aggressive caching headers                  |
| Lighthouse Accessibility  | 98 (Vercel)           | **100**        | Automated + manual audit, screen reader testing in CI              |
| Lighthouse Best Practices | 95 (Stripe)           | **100**        | Zero warnings, zero mixed content, zero deprecated APIs            |
| Lighthouse SEO            | 95 (Linear)           | **100**        | Complete structured data, perfect meta tags, sitemap               |
| UI entry bundle           | 200-500KB (SaaS avg)  | **< 100KB**    | Lit (no framework bloat) + aggressive code splitting               |
| Time to Interactive       | 2-4s (typical SaaS)   | **< 1s**       | C runtime + minimal JS + streaming SSR                             |

### 3.2 Interaction Latency — Instant by Default

| Interaction                  | Industry Best      | Human Target                  | How                                    |
| ---------------------------- | ------------------ | ------------------------------- | -------------------------------------- |
| Key press to visual feedback | ~80ms (Superhuman) | **< 16ms** (single frame)       | Optimistic UI, CSS-only state changes  |
| Button tap to state change   | ~100ms (Linear)    | **< 50ms**                      | No async in critical path              |
| View transition              | ~300ms (Material)  | **< 200ms** with spring physics | View Transitions API + preloaded views |
| Search result population     | ~200ms (Raycast)   | **< 100ms**                     | Indexed, local-first search            |
| Command palette response     | ~150ms (Cursor)    | **< 80ms**                      | Pre-indexed command registry           |

### 3.3 C Runtime — Unmatched (Competitors Can't Touch This)

| Metric          | Competitors                | Human                   |
| --------------- | -------------------------- | ------------------------- |
| Binary size     | N/A (web-only or Electron) | **< 1.5MB** full-featured |
| Cold startup    | N/A (Electron: 2-5s)       | **< 30ms**                |
| Peak RSS        | 100MB+ (Electron)          | **< 6MB**                 |
| Test throughput | varies                     | **700+ tests/sec**        |

## 4. Standards Doc Upgrades

### 4.1 `visual-standards.md` — New Principles

#### Zero-Compromise Aesthetics

Stripe proved beauty elevates utility by 20%. Human goes further: every pixel must simultaneously be the most functional AND the most beautiful version of that element in the developer tools space. No trade-off mentality. If a design choice requires choosing between craft and function, the design is wrong — find the third option.

#### Perceptual Performance

Beyond actual performance metrics, visual design must _feel_ faster than it is. Skeleton-to-content transitions, optimistic UI, instant visual feedback before data arrives. The competitive ceiling isn't "fast loading" — it's "I never noticed it loaded."

#### Depth as Language

Our glass system (3 tiers + Apple visionOS materials + choreography) already exceeds Linear's Liquid Glass. Push further: spatial UI where depth conveys information hierarchy, not just decoration. No other dev tool does this.

#### Monochrome Confidence

The UI must work beautifully in near-monochrome. Accent color is a surgical instrument, not a theme. Linear and Superhuman prove that restraint is power.

#### Calm Technology (from Stripe)

Powerful functionality that doesn't demand attention. Subtle animations that guide without distracting. The three-tier quality litmus: utility (does it work?), usability (is it comfortable?), beauty (is it well-executed?). All three addressed simultaneously, never sequentially.

#### Typographic Confidence (from Awwwards winners)

Type as a primary design element, not just for reading. Award winners (Malvah, Lando Norris) use oversized display type, letter-spacing as texture, type-as-hero. Mathematically perfect type scale with modular ratio baked into tokens.

#### New Quality Bar Table

Every quality checklist item gets a "Competitive Ceiling" column showing what the best brands achieve, and a "Human Target" column showing our category-defining bar.

### 4.2 `ux-patterns.md` — New Principles

#### Anticipatory UX

Beyond "fast interactions" — build UX that predicts what you need. Prefetch likely next views. Pre-render command palette results. Load data for the detail pane you'll probably click. The user should never wait. Not "fast" — instant.

#### Information Theater (from Tufte, weaponized)

Data presentation so clear that understanding is involuntary. No legends needed. No explanation text. The chart IS the explanation. Push beyond "good data-ink ratio" to "zero learning curve data."

#### Keyboard-First Design (from Linear/Superhuman)

Every view has a complete keyboard interaction model. Command palette is primary navigation, not a nice-to-have. Shortcuts discoverable through contextual hints.

#### Config-as-Composition (from Figma/Scout Motors)

Complex configuration flows feel like creative composition, not form-filling. Visual previews, drag-and-drop, contextual feedback.

#### Platform Transcendence

Don't just be "good on each platform." Be the app that makes people say "this is the best-designed app on my [device]." Each surface designed by someone who lives on that platform.

#### Sub-100ms Interaction Contract (now sub-50ms)

Measured, not felt. CI enforced. Every interaction type has an explicit latency budget.

### 4.3 `motion-design.md` — New Capabilities

#### Real Spring Physics

Move beyond cubic-bezier approximations. Use `linear()` with 60+ keyframes for true damped harmonic oscillator curves. Springs indistinguishable from native iOS animations. Adopt Web Animations API `spring()` timing function when stable.

#### Scroll-Driven Narratives

CSS `scroll-timeline` and `view-timeline` are stable. Codify scroll-driven animation patterns: parallax headers, progressive reveal sequences, section entrance choreography. The website should rival Lando Norris / Immersive Garden for scroll experience.

#### Ambient Intelligence

Subtle motion that responds to context: glass blur density shifting with scroll depth, gradient animations responding to mouse proximity, status indicators breathing with system health. Communication through motion, not decoration.

#### Transition Orchestration

View Transitions API for seamless cross-page morphs. Elements have spatial memory — a card that expands to detail view morphs, never fade-and-replaces. Shared element transitions across routes.

#### Narrative Motion (from Spotify Wrapped)

Animation that tells a story. Staggered data reveals, sequential storytelling through motion choreography. Loading sequences that build anticipation.

### 4.4 `design-strategy.md` — New Sections

#### Competitive Dominance Metrics

Every token table gets a "Category Status Quo" column and a "Human Redefines" column showing where we set the new ceiling.

#### Design Innovation Pipeline

Every quarter, evaluate and adopt one emerging web platform feature before competitors:

- CSS `@starting-style` for entry animations
- Anchor Positioning API for contextual UI
- `popover` attribute for lightweight overlays
- `scrollbar-color` and `scrollbar-width` for branded scrollbars
- `@container` queries for component-level responsive design
- View Transitions API for cross-page morphs
- `scroll-timeline` for scroll-driven animations

## 5. Automated Benchmarking Infrastructure

### 5.1 Competitive PageSpeed Script

New `scripts/benchmark-competitive.sh`:

- Calls Google PageSpeed Insights API (v5) for Human + all 15 competitor marketing sites
- Collects: Performance, Accessibility, Best Practices, SEO + Core Web Vitals (LCP, CLS, INP, TTFB)
- Outputs: `benchmark-competitive.json` + `docs/competitive-benchmarks.md` (markdown summary table)
- Runs on-demand locally and weekly in CI (scheduled GitHub Action)
- Requires `PAGESPEED_API_KEY` secret

### 5.2 Enhanced Lighthouse CI

Upgrade `.lighthouserc.json`:

```json
{
  "ci": {
    "collect": {
      "staticDistDir": "website/dist",
      "numberOfRuns": 5
    },
    "assert": {
      "assertions": {
        "categories:performance": ["error", { "minScore": 0.95 }],
        "categories:accessibility": ["error", { "minScore": 0.98 }],
        "categories:best-practices": ["error", { "minScore": 0.95 }],
        "categories:seo": ["error", { "minScore": 0.95 }],
        "largest-contentful-paint": ["error", { "maxNumericValue": 1500 }],
        "cumulative-layout-shift": ["error", { "maxNumericValue": 0.05 }],
        "interactive": ["error", { "maxNumericValue": 2000 }]
      }
    }
  }
}
```

### 5.3 New CI Workflow: `competitive-benchmark.yml`

- Trigger: weekly schedule (Sunday night) + manual dispatch
- Jobs: run `scripts/benchmark-competitive.sh` against all 15 competitors
- Artifact: upload `benchmark-competitive.json` and markdown report
- Alert: post to Slack/Discord if Human drops below any competitor on any Core Web Vital

### 5.4 UI Dashboard Performance Tests

Add to Playwright E2E:

- INP measurement for key interactions (button click, view switch, command palette)
- Time to Interactive budget assertion (<2s)
- Interaction latency assertions per interaction type

### 5.5 Quality Scorecard

New `docs/quality-scorecard.md`:

- Per-surface scoring (website, dashboard, iOS, macOS, Android) on all 7 dimensions
- Updated quarterly with competitive comparison
- Tracked in `quality-scorecard-history.json` for trend analysis

## 6. New Documents Created

| Document                                      | Purpose                                         |
| --------------------------------------------- | ----------------------------------------------- |
| `docs/competitive-benchmarks.md`              | Full benchmark registry with scored comparisons |
| `docs/quality-scorecard.md`                   | Per-surface quality scores                      |
| `scripts/benchmark-competitive.sh`            | Automated competitive PageSpeed analysis        |
| `.github/workflows/competitive-benchmark.yml` | Weekly competitive benchmark CI job             |

## 7. Documents Modified

| Document                   | Changes                                                                   |
| -------------------------- | ------------------------------------------------------------------------- |
| `docs/visual-standards.md` | +6 new principles, upgraded quality checklist with competitive ceiling    |
| `docs/ux-patterns.md`      | +6 new patterns, interaction latency contract                             |
| `docs/motion-design.md`    | +4 new capabilities (real springs, scroll-driven, ambient, orchestration) |
| `docs/design-strategy.md`  | +2 new sections (competitive dominance, innovation pipeline)              |
| `.lighthouserc.json`       | Raised thresholds + added CWV assertions                                  |
| `AGENTS.md` (§12)          | Reference new benchmark docs, updated quality bars                        |

## 8. Success Criteria

- [ ] All 15 benchmark brands scored on all 7 dimensions
- [ ] Competitive PageSpeed script runs successfully against all competitors
- [ ] Lighthouse CI thresholds raised and passing
- [ ] All four design docs upgraded with category-defining principles
- [ ] Quality scorecard established for all 5 UI surfaces
- [ ] Weekly competitive benchmark CI job running
- [ ] AGENTS.md updated with new quality bars and doc references

## 9. Risks

| Risk                                                         | Mitigation                                                            |
| ------------------------------------------------------------ | --------------------------------------------------------------------- |
| PageSpeed API rate limits                                    | Use API key, batch requests, cache results                            |
| Aggressive Lighthouse thresholds break CI                    | Phase in: warning first, error after website optimizations land       |
| Manual scoring dimensions are subjective                     | Create detailed rubrics with example scores for calibration           |
| Quarterly scorecard cadence too slow                         | Start quarterly, accelerate to monthly if momentum allows             |
| Some targets (0.00 CLS, <16ms key response) are aspirational | Track as stretch goals, enforce achievable intermediate targets in CI |
