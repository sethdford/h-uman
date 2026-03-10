---
title: Human Design Strategy
---

# Human Design Strategy

> Single source of truth for visual design decisions. All component code must use
> design tokens (`--hu-*`) rather than raw values. Token source files live in
> `design-tokens/` and generate `ui/src/styles/_tokens.css` via the build pipeline.

## Color Philosophy

Human uses a **Fidelity Green** primary palette inspired by ocean-meets-finance
aesthetics. The hierarchy is:

| Role      | Palette        | Token prefix                      | Usage                          |
| --------- | -------------- | --------------------------------- | ------------------------------ |
| Primary   | Fidelity green | `--hu-accent`                     | Brand, CTA, links, focus rings |
| Secondary | Amber          | `--hu-secondary`                  | Highlights, featured content   |
| Tertiary  | Indigo         | `--hu-tertiary`                   | Data visualization, depth      |
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
backgrounds, surfaces are tinted with 4-8% of the primary accent (fidelity green),
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

### Tinted State Layers (M3)

Hover, press, and focus overlays are tinted with the primary accent color instead of
neutral white/black. This connects interactive feedback to the brand identity.

| Token                  | Dark Value               | Light Value             | Usage          |
| ---------------------- | ------------------------ | ----------------------- | -------------- |
| `--hu-hover-overlay`   | rgba(122, 182, 72, 0.08) | rgba(90, 154, 48, 0.06) | Hover state    |
| `--hu-pressed-overlay` | rgba(122, 182, 72, 0.12) | rgba(90, 154, 48, 0.10) | Active/pressed |
| `--hu-focus-overlay`   | rgba(122, 182, 72, 0.12) | rgba(90, 154, 48, 0.10) | Focus state    |
| `--hu-dragged-overlay` | rgba(122, 182, 72, 0.16) | rgba(90, 154, 48, 0.14) | Drag state     |

**Rules:**

- Apply overlays as pseudo-element backgrounds, not opacity changes on the element itself
- `--hu-disabled-overlay` remains neutral (white/black) — disabled states should not carry brand color

### Dynamic Color Pipeline

Human generates harmonious color palettes from the brand hex using OKLCH color math,
following Material Design 3's "color from source" pattern.

- **Source hex**: `#7AB648` (fidelity green)
- **Generation**: `design-tokens/dynamic-color-lib.ts` → build pipeline → `_dynamic-color.css`
- **Scales**: primary, secondary (+60° hue), tertiary (+180° hue), neutral (5% chroma), error
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

- Always pair glass with `prefers-reduced-transparency: reduce` fallback (handled globally in `theme.css`)
- Never stack glass on glass without testing — layered blur compounds and reduces legibility
- Glass interactive elements must include `backdrop-filter` in their transition property
- Token source: `design-tokens/glass.tokens.json`
- Visual demo: `docs/design-system-demo.html`

## Data Visualization

Chart colors use the `chart.*` token series from `data-viz.tokens.json`.

### Categorical (up to 8 series)

| Series | Color          | Token                      |
| ------ | -------------- | -------------------------- |
| 1      | Fidelity green | `--hu-chart-categorical-1` |
| 2      | Indigo         | `--hu-chart-categorical-2` |
| 3      | Amber          | `--hu-chart-categorical-3` |
| 4      | Coral          | `--hu-chart-categorical-4` |
| 5      | Teal           | `--hu-chart-categorical-5` |
| 6      | Light indigo   | `--hu-chart-categorical-6` |
| 7      | Light amber    | `--hu-chart-categorical-7` |
| 8      | Light green    | `--hu-chart-categorical-8` |

### Sequential (single hue ramp)

Use `--hu-chart-sequential-100` through `--hu-chart-sequential-800` for ordered data
(light to dark within the Fidelity green hue).

### Diverging

- Positive: `--hu-chart-diverging-positive` (green)
- Neutral: `--hu-chart-diverging-neutral` (gray)
- Negative: `--hu-chart-diverging-negative` (coral)

### Rules

- Single-metric charts use `--hu-chart-brand`
- Multi-series charts use categorical series in order (1, 2, 3...)
- Never assign chart colors ad-hoc — always use the series tokens

## Breakpoints

| Token                | Value  | Behavior |
| -------------------- | ------ | -------- |
| `--hu-breakpoint-sm` | 480px  | Mobile   |
| `--hu-breakpoint-md` | 768px  | Tablet   |
| `--hu-breakpoint-lg` | 1024px | Desktop  |
| `--hu-breakpoint-xl` | 1280px | Wide     |

Note: CSS custom properties cannot be used inside `@media` queries directly.
Views should use the raw px values in media queries but document the token name
in a comment: `@media (max-width: 768px) /* --hu-breakpoint-md */`.

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
| C runtime binary       | N/A (Electron: 100MB+) | **< 1.5MB**    | C11 + LTO (competitors can't match)             |
| C runtime startup      | N/A (Electron: 2-5s)   | **< 30ms**     | No VM, no GC, pure native (100x faster)         |
| C runtime RSS          | N/A (Electron: 100MB+) | **< 6MB**      | Zero-allocation hot paths                       |

### Competitive Advantages Nobody Can Match

These are structural advantages from the C11 architecture that web-only competitors
cannot replicate:

1. **Binary size**: 1.5MB vs 100MB+ Electron — runs on embedded devices
2. **Startup time**: <30ms vs seconds — feels instant on any hardware
3. **Memory**: <6MB vs 100MB+ — runs alongside other apps without resource contention
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
