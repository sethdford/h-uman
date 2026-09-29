---
title: Human Design Strategy
---

# Human Design Strategy

> Single source of truth for visual design decisions. All component code must use
> design tokens (`--hu-*`) rather than raw values. Token source files live in
> `design-tokens/` and generate `ui/src/styles/_tokens.css` via the build pipeline.

## Color Philosophy

Human uses a **Human green** primary palette inspired by ocean-meets-finance
aesthetics. The hierarchy is:

| Role      | Palette        | Token prefix                      | Usage                          |
| --------- | -------------- | --------------------------------- | ------------------------------ |
| Primary   | Human green    | `--hu-accent`                     | Brand, CTA, links, focus rings |
| Secondary | Amber          | `--hu-secondary`                  | Highlights, featured content   |
| Tertiary  | Steel blue     | `--hu-accent-tertiary`            | Data visualization, depth      |
| Status    | Semantic       | `--hu-success/warning/error/info` | System feedback                |
| Neutral   | Ocean scale    | `--hu-text/bg/border`             | Backgrounds, text, borders     |

### Theme Support

- **Dark mode** (default): Deep ocean backgrounds, light text
- **Light mode**: Coastal whites, dark text — triggered by `prefers-color-scheme: light` or `data-theme="light"`
- **High contrast**: Enhanced boundaries — triggered by `prefers-contrast: more`
- **Wide gamut (P3)**: Enhanced saturation where supported

### Rules

- Never use raw hex (`#xxx`) or `rgba()` in component CSS
- Derive tints using `color-mix(in srgb, var(--hu-*) %, transparent)`
- Data visualization uses the `chart.categorical.*` series (see below)

### Tonal Surfaces (M3)

Human implements Material Design 3 tonal surface containers. Instead of pure neutral
backgrounds, surfaces are tinted with 4-8% of the primary accent (human green),
creating a branded warmth across elevation levels.

| Token                            | Dark Value | Light Value | Usage                           |
| -------------------------------- | ---------- | ----------- | ------------------------------- |
| `--hu-surface-dim`               | #030710    | #e8e8e8     | Recessed wells, inset panels    |
| `--hu-surface-container`         | #172631    | #f7f8f5     | Default card/panel background   |
| `--hu-surface-container-high`    | #223540    | #eeefec     | Elevated interactive surfaces   |
| `--hu-surface-container-highest` | #2c4451    | #e7e9e5     | Highest emphasis, active states |
| `--hu-surface-bright`            | #34506a    | #ffffff     | Hero sections, featured cards   |

**When to use tonal surfaces vs plain backgrounds:**

- Use `--hu-surface-container` for cards and panels that need branded identity
- Use `--hu-bg-surface` when you want a neutral, non-tinted surface
- Use `--hu-surface-container-high` for active/selected items in lists
- Use `--hu-surface-container-highest` for the most prominent interactive element
- Token source: `design-tokens/base.tokens.json` (color.tonal.\*), referenced by `semantic.tokens.json`

### Neutral State Layers (M3)

Hover, press, focus, and drag overlays use **neutral** veils (warm white on dark UI, soft black on light UI). Brand identity stays on **focus rings**, links, and primary buttons — not on every interactive surface wash.

| Token                  | Dark Value                 | Light Value           | Usage          |
| ---------------------- | -------------------------- | --------------------- | -------------- |
| `--hu-hover-overlay`   | rgba(255, 255, 255, 0.08)  | rgba(0, 0, 0, 0.06)   | Hover state    |
| `--hu-pressed-overlay` | rgba(255, 255, 255, 0.12)  | rgba(0, 0, 0, 0.10)   | Active/pressed |
| `--hu-focus-overlay`   | rgba(255, 255, 255, 0.12)  | rgba(0, 0, 0, 0.10)   | Focus state    |
| `--hu-dragged-overlay` | rgba(255, 255, 255, 0.16)  | rgba(0, 0, 0, 0.14)   | Drag state     |

**Rules:**

- Apply overlays as pseudo-element backgrounds, not opacity changes on the element itself
- `--hu-disabled-overlay` remains neutral (white/black) — disabled states should not carry brand color

### Dynamic Color Pipeline

Human generates harmonious color palettes from the brand hex using OKLCH color math,
following Material Design 3's "color from source" pattern.

- **Source hex**: `#7AB648` (Human green)
- **Generation**: `design-tokens/dynamic-color-lib.ts` → build pipeline → `_dynamic-color.css`
- **Scales**: primary, secondary (+60° hue), tertiary (fixed steel blue hue ~246°), neutral (warm hue), error
- **Steps per scale**: 50, 100, 200, 300, 400, 500, 600, 700, 800, 900, 950
- **P3 overrides**: Automatic `@media (color-gamut: p3)` block with `color(display-p3 ...)` values
- **Token prefix**: `--hu-dynamic-{group}-{step}` (e.g. `--hu-dynamic-primary-500`)
- **CLI**: `npx tsx design-tokens/dynamic-color.ts --hex "#7AB648" --format css`
- **Build**: Auto-generated during `cd design-tokens && npm run build`

## Typography

| Token            | Size                         | Usage                      |
| ---------------- | ---------------------------- | -------------------------- |
| `--hu-text-2xs`  | 0.625rem (10px)              | Badges, secondary metadata |
| `--hu-text-xs`   | 0.6875rem (11px)             | Captions, timestamps       |
| `--hu-text-sm`   | 0.8125rem (13px)             | Body secondary             |
| `--hu-text-base` | 0.875rem (14px)              | Body primary               |
| `--hu-text-lg`   | 1rem (16px)                  | Headings                   |
| `--hu-text-xl`   | 1.25rem (20px)               | Section headers            |
| `--hu-text-2xl`  | clamp(1.25rem, 3vw, 1.75rem) | Page titles (fluid)        |
| `--hu-text-3xl`  | clamp(1.5rem, 4vw, 2.25rem)  | Hero titles (fluid)        |
| `--hu-text-hero` | clamp(2rem, 5vw, 3.5rem)     | Landing hero (fluid)       |

### Fonts

- **Sans**: Avenir / Avenir Next / system fallback (`--hu-font`)
- **Mono**: Geist Mono / SF Mono / system fallback (`--hu-font-mono`)
- No Google Fonts or CDN fonts

### Rules

- Always use `var(--hu-text-*)` — never raw `font-size: 10px`
- Use type roles (`typeRole.*` tokens) for composite presets when available
- Fluid sizes (clamp) for 2xl+ only; smaller sizes are fixed for precision

## Motion Strategy

Human motion follows Apple HIG's **spring-first** principle: prefer physics-based
curves over duration-based timing for a natural, continuous feel.

### Easing Hierarchy

| Token                     | Curve                               | Usage                                    |
| ------------------------- | ----------------------------------- | ---------------------------------------- |
| `--hu-ease-out`           | `cubic-bezier(0.16, 1, 0.3, 1)`     | Default for entering elements            |
| `--hu-ease-in`            | `cubic-bezier(0.55, 0, 1, 0.45)`    | Exiting elements                         |
| `--hu-ease-in-out`        | `cubic-bezier(0.65, 0, 0.35, 1)`    | Repositioning                            |
| `--hu-ease-spring`        | `cubic-bezier(0.34, 1.56, 0.64, 1)` | Buttons, toggles, cards (overshoot)      |
| `--hu-ease-spring-gentle` | `cubic-bezier(0.22, 1.2, 0.36, 1)`  | Modals, panels, hover (subtle overshoot) |
| `--hu-spring-out`         | `linear(...)`                       | CSS spring approximation                 |
| `--hu-spring-bounce`      | `linear(...)`                       | Bounce spring                            |
| `--hu-emphasize`          | `cubic-bezier(0.2, 0, 0, 1)`        | Material 3 dramatic decel                |

### Duration Scale

| Token                    | Value | Usage                 |
| ------------------------ | ----- | --------------------- |
| `--hu-duration-instant`  | 50ms  | Imperceptible         |
| `--hu-duration-fast`     | 100ms | Micro-interactions    |
| `--hu-duration-normal`   | 200ms | Standard transitions  |
| `--hu-duration-moderate` | 300ms | Moderate transitions  |
| `--hu-duration-slow`     | 350ms | Complex transitions   |
| `--hu-duration-slower`   | 500ms | Dramatic reveals      |
| `--hu-duration-slowest`  | 700ms | Hero/page transitions |

### Choreography

- **Stagger delay**: `--hu-stagger-delay` (50ms) between sequential items
- **Stagger max**: `--hu-stagger-max` (300ms) cap to avoid long waits
- **Cascade delay**: `--hu-cascade-delay` (30ms) for nested elements

### Reduced Motion

When `prefers-reduced-motion: reduce` is active, all duration tokens resolve to
`0ms`. Components must use token-based durations so this propagates automatically.

### Rules

- Never use raw `200ms` or `ease-in-out` — always `var(--hu-duration-*)` and `var(--hu-ease-*)`
- Prefer spring easings for interactive elements
- All `@keyframes` names prefixed with `hu-`
- No `setTimeout` or `requestAnimationFrame` for timing — use CSS transitions/animations

## Glass System

Human's glass effects follow Apple's Liquid Glass design language with three tiers,
choreographed animations, and Apple-style material densities.

### Glass Tiers

| Tier      | Blur | Saturate | Use case                   | CSS class             |
| --------- | ---- | -------- | -------------------------- | --------------------- |
| Subtle    | 12px | 120%     | Nav chrome, ambient panels | `.hu-glass-subtle`    |
| Standard  | 24px | 180%     | Cards, panels, sidebars    | `.hu-glass-standard`  |
| Prominent | 32px | 200%     | Modals, sheets, overlays   | `.hu-glass-prominent` |

Additional depth utilities: `.hu-glass-surface`, `.hu-glass-elevated`, `.hu-glass-floating`.
Tinted variants: add `.hu-glass-tinted` class.
Interactive variants: add `.hu-glass-interactive` class.

### Material Densities (Apple visionOS-style)

| Material   | Blur | Saturate | Opacity | Token prefix                       |
| ---------- | ---- | -------- | ------- | ---------------------------------- |
| Ultra-thin | 8px  | 110%     | 2%      | `--hu-glass-material-ultra-thin-*` |
| Thin       | 16px | 140%     | 4%      | `--hu-glass-material-thin-*`       |
| Regular    | 24px | 180%     | 6%      | `--hu-glass-material-regular-*`    |
| Thick      | 32px | 200%     | 10%     | `--hu-glass-material-thick-*`      |

### Glass Choreography

Glass surfaces animate their blur and saturation on enter/exit instead of snapping.

| Token                                    | Value        | Usage                        |
| ---------------------------------------- | ------------ | ---------------------------- |
| `--hu-glass-choreography-enter-duration` | 350ms        | Glass reveal animation       |
| `--hu-glass-choreography-enter-easing`   | M3 emphasize | Dramatic deceleration        |
| `--hu-glass-choreography-exit-duration`  | 200ms        | Glass dismiss (faster)       |
| `--hu-glass-choreography-state-duration` | 200ms        | Hover/press blur transitions |

CSS classes: `.hu-glass-enter` (reveal), `.hu-glass-exit` (dismiss).

### Glass Rules

- Always pair glass with `prefers-reduced-transparency: reduce` fallback (handled globally in `theme.css`). The same media query flattens mesh/aurora backgrounds to solid `--hu-bg`, disables film grain and chromatic glass borders, and renders gradient text as solid `--hu-text` for readability.
- Never stack glass on glass without testing — layered blur compounds and reduces legibility
- Glass interactive elements must include `backdrop-filter` in their transition property
- Token source: `design-tokens/glass.tokens.json`
- Visual demo: `docs/design-system-demo.html`

## 3D & Spatial Depth

Human's 3D system adds physical depth beyond glass blur. Three.js for marketing WebGL;
CSS `perspective` and `transform` for dashboard card interactions.

### 3D Tokens

| Token | Value | Purpose |
|-------|-------|---------|
| `--hu-3d-perspective` | `1200px` | Default perspective distance for card containers |
| `--hu-3d-card-tilt-max` | `8deg` | Maximum card tilt on pointer interaction |
| `--hu-3d-depth-scale-near` | `1.0` | Scale for foreground elements |
| `--hu-3d-depth-scale-far` | `0.95` | Scale for background elements |
| `--hu-3d-dof-near-blur` | `0px` | Depth-of-field: no blur for near content |
| `--hu-3d-dof-mid-blur` | `2px` | Depth-of-field: slight blur for mid-ground |
| `--hu-3d-dof-far-blur` | `6px` | Depth-of-field: stronger blur for background |
| `--hu-3d-grain-opacity` | `0.005` | Film grain overlay strength (texture, not noise) |

### WebGL Performance Budget

| Metric | Budget |
|--------|--------|
| Particle count | ≤ 5000 |
| Frame rate | 60fps steady-state |
| Load timing | After first contentful paint |
| Bundle impact | < 50KB gzipped (tree-shaken Three.js) |
| Canvas CLS | 0.00 (explicit dimensions) |
| Tab-hidden | Animation loop suspended |

### Rules

- WebGL is marketing-site only; dashboard uses CSS-only 3D
- Always provide CSS gradient mesh fallback for no-WebGL browsers
- Film grain via CSS pseudo-element, not WebGL (simpler, compositable)
- 3D tilt disabled under `prefers-reduced-motion: reduce`
- Maximum tilt 8deg — text must remain readable at all angles

## Ambient Intelligence

Subtle environmental responsiveness that makes the UI feel alive without demanding attention.
Changes are imperceptible individually; the cumulative effect is "the interface breathes."

### Ambient Color Tokens

| Token | Value | Purpose |
|-------|-------|---------|
| `--hu-ambient-glow` | `radial-gradient(ellipse, rgba(122,182,72,0.03), transparent)` | Subtle background life |
| `--hu-ambient-glow-warm` | `radial-gradient(ellipse, rgba(182,156,72,0.02), transparent)` | Secondary warm ambient glow |
| `--hu-ambient-warmth` | `0.5` | Time-aware warmth (0=cool, 1=warm) — set by JS |

### Pointer Proximity Tokens

| Token | Value | Purpose |
|-------|-------|---------|
| `--hu-pointer-proximity-radius` | `200px` | Detection radius around element |
| `--hu-pointer-tilt-factor` | `0.02` | Perspective tilt deg per px offset |
| `--hu-pointer-glow-radius` | `200px` | Glow spread following cursor |
| `--hu-pointer-glow-intensity` | `0.06` | Glow opacity at peak proximity |
| `--hu-pointer-magnetic-strength` | `8px` | Magnetic pull for small targets |
| `--hu-particle-primary` | `rgba(122,182,72,0.15)` | Particle/mesh dot color |
| `--hu-particle-secondary` | `rgba(122,182,72,0.08)` | Distant particle color |
| `--hu-pointer-glow` | `radial-gradient(200px, rgba(122,182,72,0.06), transparent)` | Pointer proximity glow |

### Ambient Motion Budget

| Effect | CPU Budget | Scope |
|--------|-----------|-------|
| Gradient response | < 0.5% | Pointer proximity |
| Status breathing | < 0.1% | Status indicators |
| Time-aware warmth | 0% (CSS only) | Background tint |
| Scroll depth blur | Compositor thread | Glass blur |
| Idle drift | < 0.3% | Particles |
| Particle float | < 0.5% | WebGL particles |
| **Combined** | **< 2.0%** | **All ambient** |

### Rules

- ALL ambient effects disabled under `prefers-reduced-motion: reduce`
- ALL ambient effects disabled on mobile (battery preservation)
- Time-aware theming: maximum 3% color mix (subliminal)
- Idle drift resets on any user interaction
- Pointer effects disabled on touch devices

## Audio Design

Optional multi-sensory layer. Muted by default. Always opt-in.

### Audio Tokens

| Token | Value | Purpose |
|-------|-------|---------|
| `--hu-audio-enabled` | `false` | Global audio toggle |
| `--hu-audio-volume` | `0.3` | Default volume (30%) |
| `--hu-audio-fade-duration` | `500ms` | Crossfade between states |

### Rules

- Audio is ALWAYS muted by default. No autoplay. No exceptions.
- Toggle: floating speaker icon, bottom-right, website only
- No audio in dashboard (productivity context = silence is respect)
- Use Web Audio API for spatial positioning
- Audio files: ≤ 50KB each, lazy-loaded
- Disabled under `prefers-reduced-motion: reduce`
- Disabled on mobile by default (battery, context)

## Data Visualization

Chart colors use the `chart.*` token series from `data-viz.tokens.json`.

### Categorical (up to 16 series)

Core palette (1–8) stays brand-aligned; 9–16 add **Web Color**–style hues from `color.viz-extended` in `base.tokens.json` for dense dashboards (charts only — not buttons or status).

| Series | Color            | Token                       |
| ------ | ---------------- | --------------------------- |
| 1      | Human green      | `--hu-chart-categorical-1` |
| 2      | Steel blue       | `--hu-chart-categorical-2` |
| 3      | Amber            | `--hu-chart-categorical-3` |
| 4      | Coral            | `--hu-chart-categorical-4` |
| 5      | Teal             | `--hu-chart-categorical-5` |
| 6      | Light steel blue | `--hu-chart-categorical-6` |
| 7      | Light amber      | `--hu-chart-categorical-7` |
| 8      | Light green      | `--hu-chart-categorical-8` |
| 9      | Forest           | `--hu-chart-categorical-9` |
| 10     | Cyan             | `--hu-chart-categorical-10` |
| 11     | Violet           | `--hu-chart-categorical-11` |
| 12     | Chartreuse       | `--hu-chart-categorical-12` |
| 13     | Gold             | `--hu-chart-categorical-13` |
| 14     | Vivid orange     | `--hu-chart-categorical-14` |
| 15     | Crimson (series) | `--hu-chart-categorical-15` |
| 16     | Azure            | `--hu-chart-categorical-16` |

### Sequential (single hue ramp)

Use `--hu-chart-sequential-100` through `--hu-chart-sequential-800` for ordered data
(light to dark within the Human green hue).

### Diverging

- Positive: `--hu-chart-diverging-positive` (green)
- Neutral: `--hu-chart-diverging-neutral` (gray)
- Negative: `--hu-chart-diverging-negative` (coral)

### Rules

- Single-metric charts use `--hu-chart-brand`
- Multi-series charts use categorical series in order (1, 2, 3...)
- Never assign chart colors ad-hoc — always use the series tokens

## Breakpoints

Canonical 4-tier system aligned with M3 window size classes. Source of truth: `design-tokens/breakpoints.tokens.json`.

### Viewport Breakpoints (for `@media` queries)

| Token                     | Value  | Use                                           |
| ------------------------- | ------ | --------------------------------------------- |
| `--hu-breakpoint-compact` | 600px  | Mobile: single column, bottom nav             |
| `--hu-breakpoint-medium`  | 905px  | Tablet: collapsed sidebar, 2-column layout    |
| `--hu-breakpoint-expanded`| 1240px | Desktop: full sidebar, multi-column content   |
| `--hu-breakpoint-wide`    | 1440px | Ultrawide: detail panels, wider grids         |

CSS custom properties cannot be used inside `@media` queries directly. Views use raw px values and document the token name in a comment: `@media (max-width: 599px) /* --hu-breakpoint-compact */`.

### Container Query Breakpoints (for `@container` rules)

Use `rem` units so breakpoints respond to container inline-size, not the viewport.

| Name         | Value    | ~px   | Use                                |
| ------------ | -------- | ----- | ---------------------------------- |
| `cq-sm`      | 30rem    | 480px | Narrow cards, single-column stack  |
| `cq-compact` | 37.5rem  | 600px | Compact layout boundary            |
| `cq-medium`  | 48rem    | 768px | Medium layout, 2→1 column grids   |
| `cq-expanded`| 56.5rem  | 904px | Expanded content boundary          |

### Adaptive Spacing

Fluid spacing tokens that scale with viewport via `clamp()`:

| Token                            | Value                        | Range            |
| -------------------------------- | ---------------------------- | ---------------- |
| `--hu-space-adaptive-page-x`     | `clamp(1rem, 3vw, 2.5rem)`  | 16px → 40px      |
| `--hu-space-adaptive-page-y`     | `clamp(1rem, 2vw, 2rem)`    | 16px → 32px      |
| `--hu-space-adaptive-section-gap`| `clamp(1.5rem, 3vw, 3rem)`  | 24px → 48px      |
| `--hu-space-adaptive-card-padding`| `clamp(1rem, 2vw, 1.5rem)` | 16px → 24px      |
| `--hu-space-adaptive-content-gap`| `clamp(0.75rem, 1.5vw, 1.5rem)` | 12px → 24px |

## Accessibility Contract

- WCAG 2.1 AA minimum contrast
- Focus rings: `--hu-focus-ring` (2px solid accent, offset by `--hu-focus-ring-offset`)
- All interactive elements keyboard-navigable
- `prefers-reduced-motion` respected via token pipeline
- `prefers-contrast: more` triggers high-contrast theme
- No information conveyed by color alone — use icons, labels, or patterns as secondary signals

## Governance

- **Token source of truth**: `design-tokens/*.tokens.json` files
- **Generated outputs**: `ui/src/styles/_tokens.css` (web), platform-specific files
- **Lint enforcement**: `ui/scripts/lint-raw-values.sh` flags violations
- **CI check**: `npm run check` includes token drift lint
- **Agent guidance**: `AGENTS.md` section 12 + `.cursor/rules/design-system.mdc`

## Competitive Dominance Metrics

Human doesn't benchmark against industry averages — it sets the ceiling others measure
against. See `docs/competitive-benchmarks.md` for named competitors and scores.

| Metric                 | Category Status Quo    | Human Target | Competitive Edge                                |
| ---------------------- | ---------------------- | -------------- | ----------------------------------------------- |
| Lighthouse Performance | 90-95                  | **99+**        | Astro static + zero JS in critical path         |
| LCP                    | 0.8-2.0s               | **< 0.5s**     | Inline critical CSS + prerendered content       |
| CLS                    | 0.01-0.1               | **0.00**       | Explicit dimensions on every element            |
| INP                    | 80-200ms               | **< 50ms**     | Event handler audit + web workers               |
| UI entry bundle        | 200-500KB              | **< 100KB**    | Lit (no framework bloat) + aggressive splitting |
| TTI (dashboard)        | 2-4s                   | **< 1s**       | Minimal JS + streaming render                   |
| C runtime binary       | N/A (Electron: 100MB+) | **≤ <!-- fp:budget_binary_kb -->2800 KB<!-- /fp -->** (now <!-- fp:binary_kb -->~2760 KB<!-- /fp -->) | C11 + LTO (competitors can't match)             |
| C runtime startup      | N/A (Electron: 2-5s)   | **≤ <!-- fp:budget_startup_ms -->100 ms<!-- /fp -->** (now <!-- fp:startup_range -->3–6 ms<!-- /fp -->) | No VM, no GC, pure native (100x faster)         |
| C runtime RSS          | N/A (Electron: 100MB+) | **≤ <!-- fp:budget_version_rss_mb -->8 MB<!-- /fp -->** peak (now <!-- fp:version_rss_mb -->6.9 MB<!-- /fp -->; <!-- fp:idle_rss_mb -->8.6 MB<!-- /fp --> idle) | Zero-allocation hot paths                       |
| Motion quality score | 8–9 (Linear/Apple)     | **10**         | WebGL + scroll narrative + spring-everything     |
| Visual craft score   | 8–9 (Linear/Stripe)    | **10**         | Pointer-responsive + ambient + cinematic depth   |
| Innovation score     | 8–9 (Immersive Garden) | **10**         | Audio-reactive + 3D + pointer proximity          |

### Competitive Advantages Nobody Can Match

These are structural advantages from the C11 architecture that web-only competitors
cannot replicate:

1. **Binary size**: <!-- fp:binary_mb -->~3 MB<!-- /fp --> vs 100MB+ Electron — runs on embedded devices
2. **Startup time**: <!-- fp:startup_bound --><6 ms<!-- /fp --> vs seconds — feels instant on any hardware
3. **Memory**: <!-- fp:idle_rss_bound --><9 MB<!-- /fp --> idle vs 100MB+ — runs alongside other apps without resource contention
4. **No runtime dependency**: libc only — no Node.js, no V8, no JVM
5. **Cross-platform native**: Same C core on every platform, native UI per platform

## Design Innovation Pipeline

Every quarter, evaluate and adopt one emerging web platform feature before competitors.

### Q1 2026 (Current)

- CSS `@starting-style` for entry animations (shipped in Chrome 117+, Safari 17.5+)
- `popover` attribute for lightweight overlays (shipped in all browsers)

### Q2 2026

- Anchor Positioning API for contextual UI (Chrome 125+)
- `scrollbar-color` and `scrollbar-width` for branded scrollbars
- WebGL particle hero for marketing site (Three.js, lazy-loaded)
- Pointer-responsive 3D cards across dashboard
- Ambient intelligence layer (gradient response, time-aware warmth)
- Audio-reactive optional layer for website

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
