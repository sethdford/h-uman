# SeaClaw Design Strategy

> Single source of truth for visual design decisions. All component code must use
> design tokens (`--sc-*`) rather than raw values. Token source files live in
> `design-tokens/` and generate `ui/src/styles/_tokens.css` via the build pipeline.

## Color Philosophy

SeaClaw uses a **Fidelity Green** primary palette inspired by ocean-meets-finance
aesthetics. The hierarchy is:

| Role      | Palette        | Token prefix                      | Usage                          |
| --------- | -------------- | --------------------------------- | ------------------------------ |
| Primary   | Fidelity green | `--sc-accent`                     | Brand, CTA, links, focus rings |
| Secondary | Amber          | `--sc-secondary`                  | Highlights, featured content   |
| Tertiary  | Indigo         | `--sc-tertiary`                   | Data visualization, depth      |
| Status    | Semantic       | `--sc-success/warning/error/info` | System feedback                |
| Neutral   | Ocean scale    | `--sc-text/bg/border`             | Backgrounds, text, borders     |

### Theme Support

- **Dark mode** (default): Deep ocean backgrounds, light text
- **Light mode**: Coastal whites, dark text — triggered by `prefers-color-scheme: light` or `data-theme="light"`
- **High contrast**: Enhanced boundaries — triggered by `prefers-contrast: more`
- **Wide gamut (P3)**: Enhanced saturation where supported

### Rules

- Never use raw hex (`#xxx`) or `rgba()` in component CSS
- Derive tints using `color-mix(in srgb, var(--sc-*) %, transparent)`
- Data visualization uses the `chart.categorical.*` series (see below)

## Typography

| Token            | Size                         | Usage                      |
| ---------------- | ---------------------------- | -------------------------- |
| `--sc-text-2xs`  | 0.625rem (10px)              | Badges, secondary metadata |
| `--sc-text-xs`   | 0.6875rem (11px)             | Captions, timestamps       |
| `--sc-text-sm`   | 0.8125rem (13px)             | Body secondary             |
| `--sc-text-base` | 0.875rem (14px)              | Body primary               |
| `--sc-text-lg`   | 1rem (16px)                  | Headings                   |
| `--sc-text-xl`   | 1.25rem (20px)               | Section headers            |
| `--sc-text-2xl`  | clamp(1.25rem, 3vw, 1.75rem) | Page titles (fluid)        |
| `--sc-text-3xl`  | clamp(1.5rem, 4vw, 2.25rem)  | Hero titles (fluid)        |
| `--sc-text-hero` | clamp(2rem, 5vw, 3.5rem)     | Landing hero (fluid)       |

### Fonts

- **Sans**: Avenir / Avenir Next / system fallback (`--sc-font`)
- **Mono**: Geist Mono / SF Mono / system fallback (`--sc-font-mono`)
- No Google Fonts or CDN fonts

### Rules

- Always use `var(--sc-text-*)` — never raw `font-size: 10px`
- Use type roles (`typeRole.*` tokens) for composite presets when available
- Fluid sizes (clamp) for 2xl+ only; smaller sizes are fixed for precision

## Motion Strategy

SeaClaw motion follows Apple HIG's **spring-first** principle: prefer physics-based
curves over duration-based timing for a natural, continuous feel.

### Easing Hierarchy

| Token                     | Curve                               | Usage                                    |
| ------------------------- | ----------------------------------- | ---------------------------------------- |
| `--sc-ease-out`           | `cubic-bezier(0.16, 1, 0.3, 1)`     | Default for entering elements            |
| `--sc-ease-in`            | `cubic-bezier(0.55, 0, 1, 0.45)`    | Exiting elements                         |
| `--sc-ease-in-out`        | `cubic-bezier(0.65, 0, 0.35, 1)`    | Repositioning                            |
| `--sc-ease-spring`        | `cubic-bezier(0.34, 1.56, 0.64, 1)` | Buttons, toggles, cards (overshoot)      |
| `--sc-ease-spring-gentle` | `cubic-bezier(0.22, 1.2, 0.36, 1)`  | Modals, panels, hover (subtle overshoot) |
| `--sc-spring-out`         | `linear(...)`                       | CSS spring approximation                 |
| `--sc-spring-bounce`      | `linear(...)`                       | Bounce spring                            |
| `--sc-emphasize`          | `cubic-bezier(0.2, 0, 0, 1)`        | Material 3 dramatic decel                |

### Duration Scale

| Token                    | Value | Usage                 |
| ------------------------ | ----- | --------------------- |
| `--sc-duration-instant`  | 50ms  | Imperceptible         |
| `--sc-duration-fast`     | 100ms | Micro-interactions    |
| `--sc-duration-normal`   | 200ms | Standard transitions  |
| `--sc-duration-moderate` | 300ms | Moderate transitions  |
| `--sc-duration-slow`     | 350ms | Complex transitions   |
| `--sc-duration-slower`   | 500ms | Dramatic reveals      |
| `--sc-duration-slowest`  | 700ms | Hero/page transitions |

### Choreography

- **Stagger delay**: `--sc-stagger-delay` (50ms) between sequential items
- **Stagger max**: `--sc-stagger-max` (300ms) cap to avoid long waits
- **Cascade delay**: `--sc-cascade-delay` (30ms) for nested elements

### Reduced Motion

When `prefers-reduced-motion: reduce` is active, all duration tokens resolve to
`0ms`. Components must use token-based durations so this propagates automatically.

### Rules

- Never use raw `200ms` or `ease-in-out` — always `var(--sc-duration-*)` and `var(--sc-ease-*)`
- Prefer spring easings for interactive elements
- All `@keyframes` names prefixed with `sc-`
- No `setTimeout` or `requestAnimationFrame` for timing — use CSS transitions/animations

## Data Visualization

Chart colors use the `chart.*` token series from `data-viz.tokens.json`.

### Categorical (up to 8 series)

| Series | Color          | Token                      |
| ------ | -------------- | -------------------------- |
| 1      | Fidelity green | `--sc-chart-categorical-1` |
| 2      | Indigo         | `--sc-chart-categorical-2` |
| 3      | Amber          | `--sc-chart-categorical-3` |
| 4      | Coral          | `--sc-chart-categorical-4` |
| 5      | Teal           | `--sc-chart-categorical-5` |
| 6      | Light indigo   | `--sc-chart-categorical-6` |
| 7      | Light amber    | `--sc-chart-categorical-7` |
| 8      | Light green    | `--sc-chart-categorical-8` |

### Sequential (single hue ramp)

Use `--sc-chart-sequential-100` through `--sc-chart-sequential-800` for ordered data
(light to dark within the Fidelity green hue).

### Diverging

- Positive: `--sc-chart-diverging-positive` (green)
- Neutral: `--sc-chart-diverging-neutral` (gray)
- Negative: `--sc-chart-diverging-negative` (coral)

### Rules

- Single-metric charts use `--sc-chart-brand`
- Multi-series charts use categorical series in order (1, 2, 3...)
- Never assign chart colors ad-hoc — always use the series tokens

## Breakpoints

| Token                | Value  | Behavior |
| -------------------- | ------ | -------- |
| `--sc-breakpoint-sm` | 480px  | Mobile   |
| `--sc-breakpoint-md` | 768px  | Tablet   |
| `--sc-breakpoint-lg` | 1024px | Desktop  |
| `--sc-breakpoint-xl` | 1280px | Wide     |

Note: CSS custom properties cannot be used inside `@media` queries directly.
Views should use the raw px values in media queries but document the token name
in a comment: `@media (max-width: 768px) /* --sc-breakpoint-md */`.

## Accessibility Contract

- WCAG 2.1 AA minimum contrast
- Focus rings: `--sc-focus-ring` (2px solid accent, offset by `--sc-focus-ring-offset`)
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
