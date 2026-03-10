# Competitive Intelligence & Category-Defining Standards — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build competitive benchmarking infrastructure, create benchmark registry, upgrade all design standards docs to category-defining quality bars, and establish automated CI measurement.

**Architecture:** Shell scripts calling PageSpeed Insights API + upgraded Lighthouse CI config + revised markdown design docs + new CI workflow + quality scorecard template.

**Tech Stack:** Bash, curl, jq, Google PageSpeed Insights API v5, Lighthouse CI, GitHub Actions, Markdown.

---

### Task 1: Create Competitive Benchmark Registry

**Files:**

- Create: `docs/competitive-benchmarks.md`

**Step 1: Create the benchmark registry document**

Create `docs/competitive-benchmarks.md` with the full competitive benchmark registry containing:

- YAML frontmatter with title and date
- Introduction explaining this is a living document updated quarterly
- Benchmark brands table (15 brands across 4 tiers: Dev Tools, Premium SaaS, Big Tech, Award Winners)
- Scoring dimensions table (7 dimensions: Performance, Visual Craft, Motion Quality, Information Density, Accessibility, Brand Cohesion, Innovation)
- Scoring rubric (what 1-5 and 6-10 mean for each dimension)
- Initial competitive scores table (placeholder scores marked as "TBD" until first benchmark run)
- URLs for each brand's key pages to benchmark

Use the brand list and dimensions from the design doc at `docs/plans/2026-03-09-competitive-intelligence-design.md` section 2.

**Step 2: Verify the document renders correctly**

Run: `wc -l docs/competitive-benchmarks.md`
Expected: 150+ lines of content

**Step 3: Commit**

```bash
git add docs/competitive-benchmarks.md
git commit -m "docs: add competitive benchmark registry with 15 brands and 7 scoring dimensions"
```

---

### Task 2: Create Automated Competitive PageSpeed Script

**Files:**

- Create: `scripts/benchmark-competitive.sh`

**Step 1: Write the competitive benchmark script**

Create `scripts/benchmark-competitive.sh` that:

1. Accepts an optional `PAGESPEED_API_KEY` environment variable (uses unauthenticated API if not set, with warning)
2. Defines an array of competitor URLs:
   - `https://linear.app` (Linear)
   - `https://vercel.com` (Vercel)
   - `https://raycast.com` (Raycast)
   - `https://warp.dev` (Warp)
   - `https://cursor.com` (Cursor)
   - `https://stripe.com` (Stripe)
   - `https://notion.so` (Notion)
   - `https://figma.com` (Figma)
   - `https://superhuman.com` (Superhuman)
   - `https://developer.apple.com` (Apple Dev)
   - `https://spotify.com` (Spotify)
   - `https://landonorris.com` (Lando Norris)
   - `https://scoutmotors.com` (Scout Motors)
   - `https://immersive-g.com` (Immersive Garden)
   - `https://malvah.com` (Malvah)
3. Also includes `https://human.dev` (our site) as the first entry
4. For each URL, calls the PageSpeed Insights API v5: `https://www.googleapis.com/pagespeedonline/v5/runPagespeed?url=URL&strategy=mobile&category=PERFORMANCE&category=ACCESSIBILITY&category=BEST_PRACTICES&category=SEO`
5. Extracts from JSON response using `jq`:
   - Performance score (`.lighthouseResult.categories.performance.score * 100`)
   - Accessibility score (`.lighthouseResult.categories.accessibility.score * 100`)
   - Best Practices score (`.lighthouseResult.categories["best-practices"].score * 100`)
   - SEO score (`.lighthouseResult.categories.seo.score * 100`)
   - LCP (`.lighthouseResult.audits["largest-contentful-paint"].numericValue`)
   - CLS (`.lighthouseResult.audits["cumulative-layout-shift"].numericValue`)
   - TBT as INP proxy (`.lighthouseResult.audits["total-blocking-time"].numericValue`)
   - TTFB (`.lighthouseResult.audits["server-response-time"].numericValue`)
6. Outputs results to `benchmark-competitive.json` (full data) and prints a markdown summary table to stdout
7. Has a `--human-only` flag for quick self-check
8. Has a `--markdown` flag to output markdown to `docs/competitive-benchmarks-latest.md`
9. Includes rate limiting (1 second between requests) to avoid API throttling
10. Requires `curl` and `jq` (checks at startup)

Make the script executable: `chmod +x scripts/benchmark-competitive.sh`

**Step 2: Run the script in dry-run / self-only mode to verify**

Run: `bash scripts/benchmark-competitive.sh --human-only 2>&1 | head -20`
Expected: Script starts, may fail on actual URL (site may not be deployed) but should handle errors gracefully

**Step 3: Commit**

```bash
git add scripts/benchmark-competitive.sh
git commit -m "feat: add competitive PageSpeed benchmarking script for 15 competitor sites"
```

---

### Task 3: Create Weekly Competitive Benchmark CI Workflow

**Files:**

- Create: `.github/workflows/competitive-benchmark.yml`

**Step 1: Write the CI workflow**

Create `.github/workflows/competitive-benchmark.yml`:

```yaml
name: Competitive Benchmark

on:
  schedule:
    - cron: "0 6 * * 0" # Sunday 6am UTC
  workflow_dispatch:

permissions:
  contents: read

jobs:
  benchmark:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Install jq
        run: sudo apt-get install -y jq
      - name: Run competitive benchmark
        env:
          PAGESPEED_API_KEY: ${{ secrets.PAGESPEED_API_KEY }}
        run: bash scripts/benchmark-competitive.sh --markdown
      - name: Upload results
        uses: actions/upload-artifact@v4
        with:
          name: competitive-benchmark-${{ github.run_number }}
          path: |
            benchmark-competitive.json
            docs/competitive-benchmarks-latest.md
          retention-days: 90
```

**Step 2: Verify YAML syntax**

Run: `python3 -c "import yaml; yaml.safe_load(open('.github/workflows/competitive-benchmark.yml'))" && echo "YAML valid"`
Expected: "YAML valid"

**Step 3: Commit**

```bash
git add .github/workflows/competitive-benchmark.yml
git commit -m "ci: add weekly competitive benchmark workflow with PageSpeed Insights API"
```

---

### Task 4: Upgrade Lighthouse CI Thresholds

**Files:**

- Modify: `.lighthouserc.json`

**Step 1: Read the current config**

Read `.lighthouserc.json` to understand the current structure.

**Step 2: Upgrade thresholds**

Update `.lighthouserc.json` to:

- Raise `categories:performance` from 0.9 to 0.95
- Raise `categories:accessibility` from 0.9 to 0.98
- Raise `categories:best-practices` from 0.9 to 0.95
- Raise `categories:seo` from 0.85 to 0.95
- Add `largest-contentful-paint` assertion: `["warn", { "maxNumericValue": 1500 }]` (warn first, error later)
- Add `cumulative-layout-shift` assertion: `["warn", { "maxNumericValue": 0.05 }]`
- Add `interactive` (TTI) assertion: `["warn", { "maxNumericValue": 2000 }]`
- Increase `numberOfRuns` from 3 to 5 for more stable results
- Keep the 404 page assertMatrix override

Note: Use "warn" not "error" for new CWV assertions initially — phase to "error" after website optimizations land.

**Step 3: Verify JSON syntax**

Run: `python3 -c "import json; json.load(open('.lighthouserc.json'))" && echo "JSON valid"`
Expected: "JSON valid"

**Step 4: Commit**

```bash
git add .lighthouserc.json
git commit -m "perf: raise Lighthouse CI thresholds to category-defining bars (95+ perf, 98+ a11y)"
```

---

### Task 5: Upgrade `visual-standards.md`

**Files:**

- Modify: `docs/visual-standards.md`

**Step 1: Read the current document**

Read `docs/visual-standards.md` fully.

**Step 2: Add new principles section**

After the existing "## 1. Visual Hierarchy Principles" section and before "## 2. Color Application", insert a new section:

```markdown
## 1.5 Category-Defining Principles

These principles go beyond industry standards. They define the ceiling other developer tools measure against.

### Zero-Compromise Aesthetics

Stripe proved beauty elevates utility by 20%. Human goes further: every pixel must simultaneously be the most functional AND the most beautiful version of that element in the developer tools space. No trade-off mentality. If a design choice requires choosing between craft and function, the design is wrong — find the third option.

### Perceptual Performance

Beyond actual performance metrics, visual design must _feel_ faster than it is. Skeleton-to-content transitions, optimistic UI, instant visual feedback before data arrives. The competitive ceiling isn't "fast loading" — it's "I never noticed it loaded."

### Depth as Language

Our glass system (3 tiers + Apple visionOS materials + choreography) exceeds what competitors offer. Spatial UI where depth conveys information hierarchy, not just decoration. Elevation changes carry semantic meaning.

### Monochrome Confidence

The UI must work beautifully in near-monochrome. Accent color is a surgical instrument, not a theme. Linear and Superhuman prove that restraint is power. Test every screen in grayscale — if the hierarchy breaks, the design is wrong.

### Calm Technology

Powerful functionality that doesn't demand attention. Subtle animations that guide without distracting. Three-tier quality litmus: utility (does it work?), usability (is it comfortable?), beauty (is it well-executed?). All three addressed simultaneously, never sequentially.

### Typographic Confidence

Type as a primary design element, not just for reading. Oversized display type, letter-spacing as texture, type-as-hero. Mathematically perfect type scale with modular ratio baked into tokens — no arbitrary sizes.
```

**Step 3: Upgrade the quality checklist (section 10)**

Replace the existing quality checklist with an enhanced version that adds a "Competitive Ceiling" context to each item and adds new items:

After each existing checklist item, the new format includes the competitive reference. Add these new items to the checklist:

- `[ ] **Perceptual speed**: Does the transition from loading to loaded feel instantaneous?`
- `[ ] **Monochrome test**: Does the screen pass the grayscale squint test?`
- `[ ] **Competitive ceiling**: Would this screen hold up next to Linear, Stripe, or Vercel?`

**Step 4: Commit**

```bash
git add docs/visual-standards.md
git commit -m "docs: add category-defining visual principles (zero-compromise, perceptual perf, calm tech)"
```

---

### Task 6: Upgrade `ux-patterns.md`

**Files:**

- Modify: `docs/ux-patterns.md`

**Step 1: Read the current document**

Read `docs/ux-patterns.md` fully.

**Step 2: Add new interaction principles**

After the existing "## 3. Component Interaction Patterns" section, insert a new section:

```markdown
## 3.5 Category-Defining Interaction Principles

### Anticipatory UX

Beyond "fast interactions" — build UX that predicts what you need. Prefetch likely next views. Pre-render command palette results. Load data for the detail pane you'll probably click. The user should never wait. Not "fast" — instant.

Implementation rules:

- Prefetch the top 3 most likely next navigation targets on every view load
- Pre-render command palette results for the 20 most common commands
- Preload detail pane data on list item hover (200ms debounce)
- Use `requestIdleCallback` for speculative prefetching

### Information Theater

Data presentation so clear that understanding is involuntary. No legends needed. No explanation text. The chart IS the explanation. Push beyond "good data-ink ratio" to "zero learning curve data."

Rules:

- Direct labels on all chart elements (no separate legend unless >5 series)
- Sparklines for trend context on every numeric metric
- Color encodes one dimension only — never overload color with multiple meanings
- Annotations directly on data points, not in footnotes

### Keyboard-First Design

Every view has a complete keyboard interaction model. Command palette is primary navigation, not a nice-to-have.

Rules:

- Every action reachable via keyboard shortcut
- Shortcuts discoverable through contextual hints (shown on hover + in command palette)
- Vim-style navigation (j/k for lists, h/l for panes) as opt-in power user mode
- Focus management: every view change moves focus to the primary content area

### Platform Transcendence

Each platform surface designed by someone who lives on that platform, not ported from web.

Rules:

- iOS: Follow Apple HIG spring animations, haptic feedback, swipe gestures natively
- Android: Follow Material 3 motion, adaptive icons, predictive back gesture
- macOS: Native menu bar integration, trackpad gestures, Quick Look support
- Web: Progressive enhancement, offline support, installable PWA
- Never share animation code between platforms — each gets native-feeling motion

### Interaction Latency Contract

Every interaction type has an explicit latency budget, measured in CI.

| Interaction                  | Budget           | Measurement                     |
| ---------------------------- | ---------------- | ------------------------------- |
| Key press to visual feedback | < 16ms (1 frame) | Playwright + performance.now()  |
| Button tap to state change   | < 50ms           | Playwright interaction timing   |
| View transition complete     | < 200ms          | View Transitions API timing     |
| Search results populated     | < 100ms          | Playwright + performance.mark() |
| Command palette response     | < 80ms           | Playwright + performance.mark() |
```

**Step 3: Commit**

```bash
git add docs/ux-patterns.md
git commit -m "docs: add category-defining UX patterns (anticipatory, keyboard-first, latency contract)"
```

---

### Task 7: Upgrade `motion-design.md`

**Files:**

- Modify: `docs/motion-design.md`

**Step 1: Read the current document**

Read `docs/motion-design.md` fully.

**Step 2: Add new motion capabilities**

After the existing "## 6. Reduced Motion" section and before "## 7. Cross-Reference", insert:

```markdown
## 6.5 Category-Defining Motion Capabilities

### Real Spring Physics

Move beyond cubic-bezier approximations. Use `linear()` with 60+ keyframes for true damped harmonic oscillator curves. Springs must be indistinguishable from native iOS/macOS animations.

Implementation:

- Generate `linear()` curves from spring parameters (mass, stiffness, damping) at build time
- 60+ steps per curve for smooth 60fps playback
- Store curves in `design-tokens/motion.tokens.json` alongside cubic-bezier fallbacks
- Use Web Animations API `spring()` timing function when browsers ship it
- Test: record screen at 120fps, compare frame-by-frame with native SwiftUI spring

### Scroll-Driven Narratives

CSS `scroll-timeline` and `view-timeline` enable animation driven by scroll position rather than time. Use for the marketing website's storytelling sections.

Patterns:

- **Parallax headers**: Background layers move at different scroll rates using `scroll-timeline`
- **Progressive reveal**: Elements fade in and translate as they enter the viewport using `view-timeline`
- **Section entrance choreography**: Staggered element entrance triggered by scroll position
- **Progress indicators**: Reading progress bars driven by scroll position

Rules:

- Always provide a static fallback for browsers without `scroll-timeline` support
- Never hijack scroll behavior — scroll-driven animations enhance, not replace, natural scrolling
- Performance: scroll-driven animations run on the compositor thread — keep them to transform/opacity
- Reduced motion: replace with immediate visibility (no scroll-triggered animation)

### Ambient Intelligence

Subtle motion that responds to environmental context. Not decoration — communication through motion.

Patterns:

- **Glass blur density**: Backdrop blur intensity shifts subtly with scroll depth (deeper = denser)
- **Gradient response**: Background gradients shift hue slightly based on mouse/pointer proximity
- **Status breathing**: Status indicators pulse rate correlates with system health metrics
- **Time-aware theming**: Subtle color temperature shift based on time of day (warmer at night)

Rules:

- Ambient effects must be imperceptible as individual changes — only the cumulative effect is felt
- CPU budget: ambient animations must use <1% CPU when idle
- All ambient effects disabled under `prefers-reduced-motion: reduce`
- Never animate ambient effects on mobile (battery impact)

### Transition Orchestration

View Transitions API enables seamless cross-route morphs. Elements have spatial memory.

Implementation:

- Assign `view-transition-name` to persistent elements (nav, sidebar, selected card)
- Shared elements morph between views (position, size, border-radius)
- Non-shared elements crossfade with stagger
- Duration: `--hu-duration-moderate` (300ms) with `--hu-ease-spring-gentle`
- Fallback: standard fade transition for browsers without View Transitions API

Rules:

- Maximum 5 elements with `view-transition-name` per view (performance)
- Morphing elements must have compatible aspect ratios (no extreme stretching)
- Test with Network throttling — transitions must degrade gracefully on slow connections
- `::view-transition-*` pseudo-elements use Human easing tokens, not raw curves

### Narrative Motion

Animation that tells a story. Staggered data reveals that build understanding incrementally.

Patterns:

- **Data cascade**: Chart data points appear sequentially, building the picture over 500ms
- **Metric reveal**: Hero numbers count up from 0 to value with deceleration curve
- **Timeline playback**: Historical data animates through time, showing progression
- **Connection drawing**: Lines between related elements draw on as relationships become visible

Rules:

- Narrative sequences must be skippable (click to complete instantly)
- Total narrative duration: max 2 seconds for any sequence
- Each step must be meaningful — remove any step that doesn't add understanding
- Reduced motion: show final state immediately, no animation
```

**Step 3: Commit**

```bash
git add docs/motion-design.md
git commit -m "docs: add category-defining motion capabilities (real springs, scroll-driven, ambient, orchestration)"
```

---

### Task 8: Upgrade `design-strategy.md`

**Files:**

- Modify: `docs/design-strategy.md`

**Step 1: Read the current document**

Read `docs/design-strategy.md` fully.

**Step 2: Add Competitive Dominance Metrics section**

At the end of the document (before any existing cross-reference section), add:

```markdown
## Competitive Dominance Metrics

Human doesn't benchmark against industry averages — it sets the ceiling others measure against.

| Metric                 | Category Status Quo    | Human Target | Competitive Edge                                |
| ---------------------- | ---------------------- | -------------- | ----------------------------------------------- |
| Lighthouse Performance | 90-95                  | **99+**        | Astro static + zero JS in critical path         |
| LCP                    | 0.8-2.0s               | **< 0.5s**     | Inline critical CSS + prerendered content       |
| CLS                    | 0.01-0.1               | **0.00**       | Explicit dimensions on every element            |
| INP                    | 80-200ms               | **< 50ms**     | Event handler audit + web workers               |
| UI entry bundle        | 200-500KB              | **< 100KB**    | Lit (no framework bloat) + aggressive splitting |
| TTI (dashboard)        | 2-4s                   | **< 1s**       | Minimal JS + streaming render                   |
| C runtime binary       | N/A (Electron: 100MB+) | **< 1.5MB**    | C11 + LTO (competitors can't match)             |
| C runtime startup      | N/A (Electron: 2-5s)   | **< 30ms**     | No VM, no GC, pure native (100x faster)         |
| C runtime RSS          | N/A (Electron: 100MB+) | **< 6MB**      | Zero-allocation hot paths                       |

### Competitive Advantages Nobody Can Match

These are structural advantages from the C11 architecture that web-only competitors cannot replicate:

1. **Binary size**: 1.5MB vs 100MB+ Electron — runs on embedded devices
2. **Startup time**: <30ms vs seconds — feels instant on any hardware
3. **Memory**: <6MB vs 100MB+ — can run alongside other apps without resource contention
4. **No runtime dependency**: libc only — no Node.js, no V8, no JVM
5. **Cross-platform native**: Same C core on every platform, native UI per platform

## Design Innovation Pipeline

Every quarter, evaluate and adopt one emerging web platform feature before competitors:

### Q1 2026 (Current)

- CSS `@starting-style` for entry animations (shipped in Chrome 117+, Safari 17.5+)
- `popover` attribute for lightweight overlays (shipped in all browsers)

### Q2 2026

- Anchor Positioning API for contextual UI (Chrome 125+)
- `scrollbar-color` and `scrollbar-width` for branded scrollbars

### Q3 2026

- View Transitions API level 2 (cross-document transitions)
- CSS `scroll-timeline` for scroll-driven animations

### Q4 2026

- CSS `@container` style queries for component-level theming
- `content-visibility: auto` for off-screen rendering optimization

### Evaluation Criteria

Each feature is evaluated on:

1. **Browser support**: Must be in 2+ major engines or have clean fallback
2. **Performance impact**: Must not regress any Core Web Vital
3. **Design value**: Must enable a pattern competitors don't have yet
4. **Implementation cost**: Must be achievable within a single sprint
```

**Step 3: Commit**

```bash
git add docs/design-strategy.md
git commit -m "docs: add competitive dominance metrics and quarterly design innovation pipeline"
```

---

### Task 9: Create Quality Scorecard

**Files:**

- Create: `docs/quality-scorecard.md`

**Step 1: Create the scorecard document**

Create `docs/quality-scorecard.md`:

```markdown
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
| Website   | TBD  | TBD    | TBD    | TBD     | TBD  | TBD   | TBD        | /70   | 63+ (9 avg) |
| Dashboard | TBD  | TBD    | TBD    | TBD     | TBD  | TBD   | TBD        | /70   | 63+ (9 avg) |
| iOS       | TBD  | TBD    | TBD    | TBD     | TBD  | TBD   | TBD        | /70   | 63+ (9 avg) |
| macOS     | TBD  | TBD    | TBD    | TBD     | TBD  | TBD   | TBD        | /70   | 63+ (9 avg) |
| Android   | TBD  | TBD    | TBD    | TBD     | TBD  | TBD   | TBD        | /70   | 63+ (9 avg) |

## Benchmark Comparison

| Brand        | Perf | Visual | Motion | Density | A11y | Brand | Innovation | Total |
| ------------ | ---- | ------ | ------ | ------- | ---- | ----- | ---------- | ----- |
| Linear       | TBD  | TBD    | TBD    | TBD     | TBD  | TBD   | TBD        | /70   |
| Vercel       | TBD  | TBD    | TBD    | TBD     | TBD  | TBD   | TBD        | /70   |
| Stripe       | TBD  | TBD    | TBD    | TBD     | TBD  | TBD   | TBD        | /70   |
| Superhuman   | TBD  | TBD    | TBD    | TBD     | TBD  | TBD   | TBD        | /70   |
| Raycast      | TBD  | TBD    | TBD    | TBD     | TBD  | TBD   | TBD        | /70   |
| Figma        | TBD  | TBD    | TBD    | TBD     | TBD  | TBD   | TBD        | /70   |
| Lando Norris | TBD  | TBD    | TBD    | TBD     | TBD  | TBD   | TBD        | /70   |

## Dimension Rubrics

### Performance (automated)

- 1-5: Lighthouse <90, LCP >2s, CLS >0.1
- 6-7: Lighthouse 90-95, LCP 1-2s, CLS <0.1
- 8: Lighthouse 95-97, LCP <1s, CLS <0.05
- 9: Lighthouse 98-99, LCP <0.5s, CLS ~0
- 10: Lighthouse 100, LCP <0.3s, CLS 0.00, INP <50ms

### Visual Craft (manual audit)

- 1-5: Inconsistent spacing, raw hex colors, mixed type scales
- 6-7: Token-based, consistent but unremarkable
- 8: Stripe/Linear tier — mathematically precise, dark/light coherent
- 9: Awwwards nomination quality — every pixel intentional
- 10: Sets new standard — competitors study your UI for inspiration

### Motion Quality (manual audit)

- 1-5: CSS transitions only, no spring physics, jarring
- 6-7: Smooth transitions, basic easing tokens
- 8: Spring-first, stagger choreography, reduced-motion support
- 9: Physics-accurate springs, scroll-driven narratives, ambient intelligence
- 10: Indistinguishable from native platform animations on every surface

### Information Density (heuristic evaluation)

- 1-5: Cluttered or wastefully sparse, legends required
- 6-7: Clean layout, standard data presentation
- 8: Tufte-level data-ink ratio, progressive disclosure
- 9: Information Theater — understanding is involuntary
- 10: Zero learning curve for any data visualization

### Accessibility (automated + manual)

- 1-5: WCAG violations, missing focus management
- 6-7: WCAG AA compliant, basic keyboard support
- 8: 98+ Lighthouse a11y, full keyboard nav, screen reader tested
- 9: 100 Lighthouse, VoiceOver/TalkBack tested, high contrast mode
- 10: Exceeds WCAG AAA, cognitive accessibility considered

### Brand Cohesion (cross-platform audit)

- 1-5: Inconsistent across surfaces, no design system
- 6-7: Shared tokens, mostly consistent
- 8: Unified system across web + native, platform-appropriate adaptations
- 9: Each surface feels native AND unmistakably Human
- 10: Platform Transcendence — best-designed app on every device

### Innovation (feature inventory)

- 1-5: Standard patterns only
- 6-7: Modern CSS, reasonable interactivity
- 8: Cutting-edge CSS (container queries, view transitions)
- 9: Features competitors don't have yet (spatial UI, ambient intelligence)
- 10: Defining new interaction paradigms

## History

| Quarter | Website | Dashboard | iOS | macOS | Android |
| ------- | ------- | --------- | --- | ----- | ------- |
| Q1 2026 | TBD     | TBD       | TBD | TBD   | TBD     |

## Action Items from Last Review

_First scorecard — no prior review._
```

**Step 2: Verify**

Run: `wc -l docs/quality-scorecard.md`
Expected: 100+ lines

**Step 3: Commit**

```bash
git add docs/quality-scorecard.md
git commit -m "docs: add quality scorecard with per-surface scoring and competitive benchmarks"
```

---

### Task 10: Update AGENTS.md with New Quality Bars and Doc References

**Files:**

- Modify: `AGENTS.md`

**Step 1: Read AGENTS.md section 12**

Read `AGENTS.md` and find section 12 (UI & Design System Contract).

**Step 2: Add references to new docs**

In AGENTS.md section 12, add to the SOTA Design References table (§12.0):

| Source                     | What We Take                                   | Human Doc                      |
| -------------------------- | ---------------------------------------------- | -------------------------------- |
| **Competitive Benchmarks** | Named brand targets, quantified quality deltas | `docs/competitive-benchmarks.md` |
| **Quality Scorecard**      | Per-surface scoring, quarterly tracking        | `docs/quality-scorecard.md`      |

Add a new subsection §12.13:

```markdown
### 12.13 Competitive Benchmarking (Required)

Required:

- All UI work must be evaluated against the competitive benchmark registry (`docs/competitive-benchmarks.md`).
- Quality scorecard (`docs/quality-scorecard.md`) updated quarterly for all surfaces.
- Lighthouse CI thresholds: Performance ≥95, Accessibility ≥98, Best Practices ≥95, SEO ≥95.
- Core Web Vitals targets: LCP <1.5s (warn), CLS <0.05 (warn), TTI <2s (warn).
- Category-defining targets: LCP <0.5s, CLS 0.00, INP <50ms (stretch goals, tracked).
- Run `scripts/benchmark-competitive.sh` before major website releases.
- New design patterns must reference which competitor inspired them and how we exceed them.
```

**Step 3: Also update CLAUDE.md CI Pipeline table**

Add the new workflow to the CI Pipeline table in CLAUDE.md:

| `competitive-benchmark.yml` | Weekly PageSpeed competitive analysis (15 brands) |

**Step 4: Commit**

```bash
git add AGENTS.md CLAUDE.md
git commit -m "docs: update AGENTS.md and CLAUDE.md with competitive benchmarking requirements"
```

---

### Task 11: Final Verification

**Step 1: Verify all new files exist**

Run: `ls -la docs/competitive-benchmarks.md docs/quality-scorecard.md scripts/benchmark-competitive.sh .github/workflows/competitive-benchmark.yml`
Expected: All 4 files exist

**Step 2: Verify all modified files are syntactically correct**

Run: `python3 -c "import json; json.load(open('.lighthouserc.json'))" && echo "Lighthouse config OK"`
Run: `python3 -c "import yaml; yaml.safe_load(open('.github/workflows/competitive-benchmark.yml'))" && echo "CI workflow OK"`

**Step 3: Build and test to verify no breakage**

Run: `cmake --build build -j$(sysctl -n hw.ncpu 2>/dev/null || nproc) && ./build/human_tests 2>&1 | tail -5`
Expected: All tests pass, 0 failures, 0 ASan errors

**Step 4: Review git log**

Run: `git log --oneline -10`
Expected: See all commits from this plan in sequence
