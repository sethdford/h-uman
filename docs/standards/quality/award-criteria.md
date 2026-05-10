---
title: Award-Winning Quality Criteria
updated: 2026-03-14
---

# Award-Winning Quality Criteria

> Concrete, measurable criteria that define award-winning quality for every Human surface. This document codifies what "best in class" means and provides the roadmap from current state to award tier.

## Award Programs & What They Measure

| Program                   | Focus                                                                 | Relevance to Human                   |
| ------------------------- | --------------------------------------------------------------------- | ------------------------------------ |
| **Awwwards**              | Design, usability, creativity, content                                | Website + dashboard — primary target |
| **CSS Design Awards**     | CSS innovation, UX, UI design                                         | Website — CSS craft showcase         |
| **FWA**                   | Forward-thinking digital design                                       | Innovation + motion                  |
| **Apple Design Awards**   | Delight, interaction, social impact, inclusivity, innovation, visuals | iOS + macOS apps                     |
| **Google Play Best Apps** | Design, UX, innovation, social impact                                 | Android app                          |
| **Webby Awards**          | Visual design, UX, functionality, content, innovation                 | Website + dashboard                  |

## Current State vs Award Tier

### Website (57/70 → Target: 63+)

| Dimension     | Current | Award Tier (9)                                                                                                                                       | Gap                                                                                                                 | Priority Fix                         |
| ------------- | ------- | ---------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------- | ------------------------------------ |
| Performance   | 8       | Lighthouse 99+, LCP <0.3s, CLS 0.00, INP <50ms, TTFB <100ms                                                                                          | LCP optimized (bundled Three.js, font preloads trimmed, HTML compression), need production URL for real measurement | Edge deployment + CDN tuning         |
| Visual Craft  | 8       | Zero raw hex/px, editorial typography with 5+ weights, tonal surface depth, micro-gradients on interactive elements, dark/light pixel-perfect parity | Micro-gradients on interactive elements added, typography-as-hero treatment done                                    | Glass polish on remaining cards      |
| Motion        | 8       | Spring physics on every interaction, GSAP ScrollTrigger with parallax, page transition choreography, 3D transforms on hero, scroll-driven narrative  | WebGL hero, scroll-driven animations, @starting-style, custom cursor all implemented                                | Shared-element morph transitions     |
| Density       | 8       | Direct-label charts, animated counters, progressive disclosure, small multiples for comparisons, tabular figures                                     | Comparison section redesigned with direct labels                                                                    | Small multiples for comparative data |
| Accessibility | 9       | Lighthouse 100, axe zero violations, VoiceOver tested, prefers-contrast support, skip links, ARIA landmarks                                          | prefers-contrast:more and forced-colors mode both supported                                                         | VoiceOver manual testing             |
| Brand         | 8       | Avenir everywhere, Phosphor everywhere, 60-30-10 validated, glass system consistent, design token parity with dashboard                              | Branded scrollbar, custom cursor reinforce brand identity                                                           | Cross-surface visual audit           |
| Innovation    | 8       | View Transitions, scroll-driven animations, @starting-style, native popover, anchor positioning, WebGPU or Three.js hero                             | WebGL hero, @view-transition, scroll-timeline, @starting-style, container queries shipped                           | :has() selector adoption             |

### Dashboard (57/70 → Target: 63+)

| Dimension     | Current | Award Tier (9)                                                                                                               | Gap                                                        | Priority Fix                          |
| ------------- | ------- | ---------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------- | ------------------------------------- |
| Performance   | 8       | Bundle <80KB, TTI <1s, no layout thrashing, requestIdleCallback for deferred work                                            | Need to measure and optimize                               | Lighthouse audit                      |
| Visual Craft  | 8       | Tonal surfaces on every card, micro-interactions on every input, consistent elevation hierarchy, editorial stat presentation | Need more tonal variety                                    | Elevation audit                       |
| Motion        | 8       | Spring on all, view transitions between all routes, scroll-driven lists, ambient loading indicators                          | Ambient sidebar indicator added                            | Shared-element morphs on route change |
| Density       | 8       | Sparklines everywhere, animated numbers, direct-label charts, progressive disclosure on all settings                         | Missing sparklines in several views                        | Add sparklines to overview/usage      |
| Accessibility | 9       | 100 axe, full keyboard, focus trap, reduced-motion                                                                           | Almost there                                               | Final axe audit                       |
| Brand         | 8       | Token parity with website, identical component styling                                                                       | Close                                                      | Cross-surface audit                   |
| Innovation    | 8       | Command palette, glass tooltips, scroll-driven, view transitions                                                             | Container queries shipped; ambient sidebar indicator added | :has() selector adoption              |

### iOS App (48/70 → Target: 63+)

| Dimension     | Current | Award Tier (9)                                                                        | Gap                                                       | Priority Fix                      |
| ------------- | ------- | ------------------------------------------------------------------------------------- | --------------------------------------------------------- | --------------------------------- |
| Performance   | 5       | 60fps scrolling, <100ms tap response, memory <50MB                                    | Untested                                                  | Profile and optimize              |
| Visual Craft  | 7       | HUTokens everywhere, SF Symbols + Phosphor, tonal backgrounds, editorial typography   | Missing tonal surfaces, limited views                     | Add overview view, tonal surfaces |
| Motion        | 7       | HUTokens.springExpressive on all transitions, matchedGeometryEffect, hero transitions | Missing matched geometry                                  | Add hero transitions              |
| Density       | 7       | Compact chat bubbles, inline stats, progressive settings                              | Add overview with stats                                   | Add overview with stats           |
| Accessibility | 7       | VoiceOver labels, Dynamic Type, high contrast                                         | Dynamic Type confirmed                                    | VoiceOver manual testing          |
| Brand         | 8       | Avenir, accent colors, spring physics                                                 | Close for existing views                                  | Extend to new views               |
| Innovation    | 7       | Live Activities, App Intents, StoreKit                                                | App Intents (Send + Status) and Live Activity implemented | matchedGeometryEffect             |

### Android App (44/70 → Target: 63+)

| Dimension     | Current | Award Tier (9)                                            | Gap                                          | Priority Fix                        |
| ------------- | ------- | --------------------------------------------------------- | -------------------------------------------- | ----------------------------------- |
| Performance   | 5       | 60fps scrolling, <100ms tap response, memory <50MB        | Untested on real devices                     | Profile and optimize                |
| Visual Craft  | 6       | Full Compose UI with Material3 + HUTokens, tonal surfaces | 3 screens built, need polish                 | Elevation audit, tonal variety      |
| Motion        | 6       | Spring animations on all transitions                      | Spring animations in place                   | matchedGeometryEffect equivalent    |
| Density       | 6       | Compact chat bubbles, overview stats                      | Static data in screens                       | Wire GatewayClient for live data    |
| Accessibility | 7       | TalkBack labels, Material a11y                            | TalkBack content descriptions on all screens | Accessibility Scanner audit         |
| Brand         | 7       | HUTokens, Avenir, accent colors consistent                | Close                                        | Cross-surface audit                 |
| Innovation    | 7       | Glance widget, GatewayClient WebSocket, predictive back   | Widget + WebSocket implemented               | Wire live data, add dynamic content |

## Concrete Award-Winning Checklist

### Awwwards Submission Checklist (Website)

- [ ] Lighthouse Performance >= 98 (production URL — gated on edge deployment)
- [ ] Lighthouse Accessibility = 100 (production URL — gated on edge deployment)
- [ ] Zero CLS on all pages (production URL — gated on edge deployment)
- [ ] LCP < 1.0s on 4G connection (production URL — LCP optimizations shipped: deferred non-critical assets, content-visibility, GPU-composited hero canvas, preload hints)
- [x] Spring physics on every interactive element
- [x] Scroll-driven narrative with parallax (scroll-driven chapter narrative with animation-timeline: scroll(), chapter progress dots, parallax chapter labels)
- [x] At least one 3D/WebGL element (hero or transition) (Perlin noise flow field with ~5000 particles + neural network topology)
- [x] @starting-style enter animations on dynamically added elements
- [x] View Transitions API between pages (if MPA) or routes (if SPA)
- [x] Custom cursor or hover interactions that delight (pointer-reactive 3D tilt cards, magnetic sidebar, pointer-proximity effects)
- [x] Responsive from 320px to 2560px with fluid typography
- [x] Dark mode with equal visual quality to light mode
- [x] prefers-reduced-motion fully supported (95+ files)
- [x] prefers-contrast: more supported
- [x] axe-core zero violations in E2E (accessibility.spec.ts + turing.spec.ts + hula.spec.ts)
- [x] Container queries for responsive layouts
- [x] Original typography treatment (not default system stack)
- [x] No stock photography — all illustrations/assets bespoke
- [x] Schema.org structured data
- [x] OG + Twitter Card meta complete with images
- [x] Ambient intelligence layer (time-aware warmth, status breathing, gradient response)
- [x] Opt-in audio design (procedural Web Audio API sounds, muted by default)
- [x] CSS Anchor Positioning (progressive enhancement on tooltips/popovers)
- [x] Branded scrollbar styling (scrollbar-color with accent)
- [x] Deeper :has() selector adoption (contextual styling without JS)
- [x] Shared-element morph transitions (view-transition-name on list-detail morphs)
- [x] Sparkline connection-draw animations (stroke-dashoffset reveal)
- [x] Static constellation reduced-motion fallback for WebGL hero

### Apple Design Award Checklist (iOS/macOS)

- [x] Uses platform conventions (NavigationStack, TabView, .sheet)
- [x] Dynamic Type support (all text scales)
- [x] VoiceOver labels on every interactive element
- [x] Haptic feedback on meaningful interactions
- [x] Spring animations (UISpringTimingParameters or SwiftUI .spring)
- [x] Widget / Live Activity / App Intent integration
- [x] Proper light/dark/tinted appearance support
- [x] Keyboard/trackpad support (macOS)
- [x] Avenir custom font with proper weight mapping

### Google Play Best App Checklist (Android)

- [x] Material 3 theming with dynamic color support
- [x] Compose-first UI
- [x] Edge-to-edge with proper insets
- [x] Predictive back gesture support
- [x] Accessibility Scanner zero issues
- [x] Spring animations (Compose animation-core)
- [x] Widget integration
- [x] Proper dark theme

## Stretch Targets (Score 10)

| Dimension     | Score 10 Criteria                                                              |
| ------------- | ------------------------------------------------------------------------------ |
| Performance   | Lighthouse 100, LCP <0.2s, zero-JS critical path, streaming SSR                |
| Visual Craft  | Competitors study your UI; featured in design publications                     |
| Motion        | Indistinguishable from native on every platform; custom physics engine         |
| Density       | Zero learning curve; understanding is involuntary (Tufte's "data that speaks") |
| Accessibility | WCAG AAA, cognitive accessibility, neurodiversity considered                   |
| Brand         | Platform Transcendence — best-designed app on every device/OS                  |
| Innovation    | Defining new interaction paradigms that others adopt                           |

## Recent Completions (Q1 2026)

- Design token pipeline across all platforms (CSS, Swift, Kotlin, Dart, C)
- Glass morphism system (`.hu-glass-*` tiers, choreography, visionOS-style materials)
- Spring animations across 36+ files
- `prefers-reduced-motion` in 95+ files
- axe-core accessibility E2E tests (all 14 dashboard views)
- Container queries for responsive layouts (overview-view, website)
- View transitions and scroll-driven animations
- C test suite: 9,500+ tests passing

## Measurement Automation

All criteria marked as "automated" should be enforced in CI:

- Lighthouse scores: `.lighthouserc.json` thresholds
- axe-core violations: `accessibility.spec.ts` E2E test
- Bundle size: `check-bundle-size.sh`
- Token compliance: `lint:tokens` + `lint-raw-colors.sh`
- Component quality: `check:components`
- CLS/LCP/INP: Lighthouse CI assertions

Manual criteria require quarterly audit per `docs/standards/quality/ceremonies.md`.
