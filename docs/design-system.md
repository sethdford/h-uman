# SeaClaw Design System

Overview of the design system philosophy, tokens, and usage rules for all SeaClaw UI surfaces (web dashboard, website, native apps, CLI/TUI).

---

## Philosophy

SeaClaw's design system prioritizes:

- **Consistency** — One canonical source of truth (`design-tokens/`) drives all platforms
- **Accessibility** — WCAG 2.1 AA minimum, prefers-reduced-motion, semantic HTML
- **Lightweight** — No external font CDNs, minimal runtime overhead, single binary mentality
- **Portability** — Same tokens, same philosophy across CSS, Swift, Kotlin, and C

---

## Color System

### Primary Accent — Ocean Teal

The primary accent is Fidelity green (`--sc-accent`), a nature-inspired color that anchors the SeaClaw brand identity.

| Token                | Dark                  | Light                |
| -------------------- | --------------------- | -------------------- |
| `--sc-accent`        | #7AB648               | #5A9A30              |
| `--sc-accent-hover`  | #8DC63F               | #47802A              |
| `--sc-accent-strong` | #a3d46a               | #7AB648              |
| `--sc-accent-subtle` | rgba(122,182,72,0.14) | rgba(90,154,48,0.10) |

### Secondary Accent — Amber

Amber (`--sc-accent-secondary`) provides warm counterpoint for featured content, highlights, and CTAs.

| Token                          | Dark                  | Light                |
| ------------------------------ | --------------------- | -------------------- |
| `--sc-accent-secondary`        | #f59e0b               | #d97706              |
| `--sc-accent-secondary-hover`  | #fbbf24               | #b45309              |
| `--sc-accent-secondary-subtle` | rgba(245,158,11,0.14) | rgba(217,119,6,0.10) |

### Tertiary Accent — Indigo

Indigo (`--sc-accent-tertiary`) adds depth for info states, data visualization, and provider-related UI.

| Token                         | Dark                  | Light                |
| ----------------------------- | --------------------- | -------------------- |
| `--sc-accent-tertiary`        | #6366f1               | #4f46e5              |
| `--sc-accent-tertiary-hover`  | #818cf8               | #4338ca              |
| `--sc-accent-tertiary-subtle` | rgba(99,102,241,0.14) | rgba(79,70,229,0.10) |

### Coral (Error/Destructive Only)

Coral is reserved exclusively for error and destructive states. Do not use coral as a general accent.

| Token            | Dark                   | Light                |
| ---------------- | ---------------------- | -------------------- |
| `--sc-error`     | #f97066                | #e11d48              |
| `--sc-error-dim` | rgba(249,112,102,0.12) | rgba(225,29,72,0.08) |

### Neutral Scale

Backgrounds and surfaces use a stepped neutral ladder:

| Token              | Purpose                               |
| ------------------ | ------------------------------------- |
| `--sc-bg`          | Page background                       |
| `--sc-bg-inset`    | Deepest inset (inputs, nested panels) |
| `--sc-bg-surface`  | Cards, panels, elevated content       |
| `--sc-bg-elevated` | Popovers, dropdowns                   |
| `--sc-bg-overlay`  | Modals, sheets, toasts                |

### Text Hierarchy

| Token             | Purpose                  |
| ----------------- | ------------------------ |
| `--sc-text`       | Primary body text        |
| `--sc-text-muted` | Secondary, labels, hints |
| `--sc-text-faint` | Tertiary, placeholders   |

### Semantic Colors

| Token          | Use                           |
| -------------- | ----------------------------- |
| `--sc-success` | Success states, confirmations |
| `--sc-warning` | Warnings, caution             |
| `--sc-error`   | Errors, destructive actions   |
| `--sc-info`    | Informational, links          |

---

## Typography

### Canonical Typeface: Avenir

Avenir is the canonical typeface across all platforms. Never use Google Fonts or external font CDNs.

| Platform  | Usage                                                                                |
| --------- | ------------------------------------------------------------------------------------ |
| Web       | `var(--sc-font)` — never set `font-family` directly                                  |
| iOS/macOS | `Font.custom("Avenir-Book", size:)`, `Avenir-Medium`, `Avenir-Heavy`, `Avenir-Black` |
| Android   | `AvenirFontFamily` from `Theme.kt`                                                   |
| CLI/TUI   | Terminal font; use token-derived ANSI colors from `design_tokens.h`                  |

### Type Scale Roles

| Token                   | Use                  |
| ----------------------- | -------------------- |
| `--sc-type-display-lg`  | Hero headlines       |
| `--sc-type-display-md`  | Section headings     |
| `--sc-type-headline-lg` | Card titles          |
| `--sc-type-headline-md` | Subsection titles    |
| `--sc-type-body-md`     | Primary body         |
| `--sc-type-body-sm`     | Secondary, captions  |
| `--sc-type-label-md`    | Form labels, badges  |
| `--sc-type-caption`     | Timestamps, metadata |

---

## Motion Principles

### Spring Physics

Four spring presets for different contexts:

| Token                    | Use Case                             |
| ------------------------ | ------------------------------------ |
| `--sc-spring-micro`      | Buttons, toggles, micro-interactions |
| `--sc-spring-standard`   | Panels, dropdowns                    |
| `--sc-spring-expressive` | Page transitions                     |
| `--sc-spring-dramatic`   | Hero reveals, modals                 |

### Choreography

- Use `--sc-stagger-delay` for sequential reveals
- Use `--sc-cascade-delay` for cascading lists
- Never use `setTimeout` for animation timing — use CSS transitions or `requestAnimationFrame`

### Reduced Motion

**Every animation must respect `prefers-reduced-motion: reduce`.** Use the global media query in `theme.css` or add component-level `@media (prefers-reduced-motion: reduce)` overrides. Keyframe names use `sc-` prefix (e.g., `sc-fade-in`, `sc-slide-up`).

---

## Component API Reference

| Component        | Purpose                                                             |
| ---------------- | ------------------------------------------------------------------- |
| `sc-button`      | Primary actions; variants: primary, secondary, destructive, ghost   |
| `sc-badge`       | Status indicators; variants: success, warning, error, info, neutral |
| `sc-card`        | Content containers; optional hoverable, clickable                   |
| `sc-modal`       | Centered dialog overlay; focus trap, Escape to close                |
| `sc-sheet`       | Bottom sheet overlay; swipe to dismiss                              |
| `sc-toast`       | Transient notifications                                             |
| `sc-tooltip`     | Hover/focus hints                                                   |
| `sc-progress`    | Linear progress; determinate or indeterminate                       |
| `sc-skeleton`    | Loading placeholders                                                |
| `sc-avatar`      | User avatars; initials fallback, status indicator                   |
| `sc-tabs`        | Tab navigation                                                      |
| `sc-empty-state` | Empty list/state messaging                                          |

---

## Platform-Specific Notes

### CSS (Web Dashboard, Website)

- All values reference `--sc-*` custom properties from generated `_tokens.css`
- Never use raw hex colors, pixel spacing, or font-family
- Shadow: `var(--sc-shadow-sm)`, `var(--sc-shadow-md)`, `var(--sc-shadow-lg)`
- Duration: `var(--sc-duration-fast)`, `var(--sc-duration-normal)`, `var(--sc-duration-slow)`

### Swift (iOS)

- Use `SCTokens` for colors, spacing, radius
- Spring: `SCTokens.springMicro`, `springStandard`, `springExpressive`, `springDramatic`
- Font: `Font.custom("Avenir-Book", size:)` etc.

### Kotlin (Android)

- Use `DesignTokens` or generated constants from `Theme.kt`
- `AvenirFontFamily` for typography

### C (CLI/TUI)

- `include/seaclaw/design_tokens.h` provides `#define` macros for ANSI colors, spacing
- Token-derived values for consistent terminal output

---

## Token Usage Rules and Anti-Patterns

### Do

- Always use `var(--sc-*)` tokens
- Use semantic tokens (`--sc-text`, `--sc-accent`) instead of base (`--sc-bg`)
- Respect `prefers-color-scheme` and `prefers-reduced-motion`
- Use `--sc-font` / `var(--sc-font-mono)` for typography

### Don't

- Use raw hex colors (e.g., `#7AB648`) — use `var(--sc-accent)`
- Use raw pixel values for spacing — use `var(--sc-space-sm)` etc.
- Use raw `font-family` — use `var(--sc-font)`
- Use emoji as UI icons — use Phosphor Regular from `ui/src/icons.ts`
- Use Google Fonts or external font CDNs
- Create one-off SVGs when a Phosphor icon exists
