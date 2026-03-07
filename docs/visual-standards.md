# SeaClaw Visual Design Standards

> Normative reference for visual hierarchy, composition, depth, spacing, and aesthetic
> quality. Agents must consult this document when creating or reviewing any visual UI change.

## SOTA References

This document synthesizes visual design principles from:

- **Apple Human Interface Guidelines** — Clarity, Deference, Depth; vibrancy; SF Symbols
- **Material Design 3** — Dynamic color, elevation, shape, emphasis
- **Edward Tufte** — Data-ink ratio, chartjunk elimination, small multiples
- **Dieter Rams** — 10 Principles of Good Design (less is more, honest, long-lasting)
- **Gestalt Psychology** — Proximity, similarity, continuity, closure, figure-ground

---

## 1. Visual Hierarchy Principles

### 1.1 The Squint Test (Dieter Rams: Good Design is as Little Design as Possible)

If you squint at a screen and can't immediately identify the primary action and content
area, the hierarchy has failed. Visual hierarchy is established through:

1. **Size** — Larger elements are more important
2. **Weight** — Bolder elements draw attention first
3. **Color** — Accent color signals primary actions
4. **Position** — Top-left (F-pattern) and center draw eyes first
5. **Contrast** — High contrast elements are noticed before low contrast
6. **Whitespace** — Isolated elements command more attention than crowded ones

### 1.2 Reading Patterns

**F-Pattern** (scanning): Users scan horizontally across the top, then move down the left
edge. Place the most important content in these zones:

```
╔═══════════════════════════════╗
║ ███████████████████████████   ║  ← Primary scan line
║ ██████████████                ║  ← Secondary scan line
║ █                             ║
║ █  Content follows            ║  ← Left-edge gravity
║ █  the left edge              ║
╚═══════════════════════════════╝
```

**Z-Pattern** (landing pages): Eye moves top-left → top-right → bottom-left → bottom-right.
Use for hero sections and marketing layouts.

Rules:

- Critical actions (primary CTA, send button) go in the first scan line or bottom-right
- Navigation goes in the top-left or left sidebar (persistent across F-pattern)
- Status indicators go in the top bar (always visible without scrolling)

### 1.3 Emphasis Levels (Material 3)

Every element has an emphasis level. M3 defines three; SeaClaw maps them to tokens:

| Emphasis | Visual Treatment                      | Token Pattern         | Example                            |
| -------- | ------------------------------------- | --------------------- | ---------------------------------- |
| High     | Accent color, bold weight, elevated   | `--sc-accent`, `bold` | Primary CTA, active nav            |
| Medium   | Subtle accent, medium weight, surface | `--sc-text`, `medium` | Secondary actions, section headers |
| Low      | Muted color, regular weight, flat     | `--sc-text-muted`     | Timestamps, metadata, hints        |

Rules:

- A screen should have exactly ONE high-emphasis element (the primary action)
- Maximum 2–3 medium-emphasis elements visible simultaneously
- Low-emphasis elements should not compete with medium or high
- Never use high emphasis for decorative or non-interactive elements

---

## 2. Color Application

### 2.1 The 60-30-10 Rule

Classic interior design proportion applied to UI:

| Proportion | Role       | SeaClaw Token                                  |
| ---------- | ---------- | ---------------------------------------------- |
| 60%        | Background | `--sc-bg`, `--sc-bg-surface`                   |
| 30%        | Secondary  | `--sc-text`, `--sc-border`, `--sc-bg-elevated` |
| 10%        | Accent     | `--sc-accent`, `--sc-accent-secondary`         |

Rules:

- Accent color is used sparingly — only for primary actions, active states, and links
- Background dominates — most of the screen is neutral surface
- Text and borders provide structure without competing with accent
- Never use more than 3 accent colors on a single screen

### 2.2 Semantic Color Usage

Colors carry meaning. Never override semantic associations:

| Color  | Meaning       | Token          | Usage                              |
| ------ | ------------- | -------------- | ---------------------------------- |
| Green  | Success, safe | `--sc-success` | Confirmations, connected status    |
| Amber  | Warning       | `--sc-warning` | Caution, pending states            |
| Coral  | Error, danger | `--sc-error`   | Errors, destructive actions ONLY   |
| Blue   | Information   | `--sc-info`    | Help text, informational callouts  |
| Accent | Brand, action | `--sc-accent`  | CTAs, links, focus, selected state |

Rules:

- Coral is NEVER used as a general accent — reserved exclusively for error/destructive
- Green (accent) vs green (success): accent uses `--sc-accent`, success uses `--sc-success`
- Don't rely on color alone — always pair with icon, label, or pattern (a11y requirement)

### 2.3 Dark Mode (Default)

SeaClaw's default is dark mode. Light mode is secondary.

Dark mode rules:

- Elevation is communicated through lighter surfaces (not shadows alone)
- Higher elevation = lighter background (`--sc-bg` → `--sc-bg-surface` → `--sc-bg-elevated`)
- Shadows are subtle — primary depth cue is surface brightness
- Text uses off-white (`--sc-text`), never pure `#ffffff` (reduces eye strain)
- Accent colors may be slightly desaturated for comfortable viewing

Light mode rules:

- Elevation communicated through shadows primarily
- Backgrounds are warm whites, not cold `#ffffff`
- Text uses near-black (`--sc-text`), never pure `#000000`
- Accent colors slightly darkened for contrast compliance

---

## 3. Spacing System

### 3.1 The 4px Grid (Material 3)

All spacing derives from a 4px base unit. SeaClaw's spacing tokens:

| Token            | Value | Use                                 |
| ---------------- | ----- | ----------------------------------- |
| `--sc-space-2xs` | 2px   | Hairline gaps, icon-to-label inline |
| `--sc-space-xs`  | 4px   | Tight padding, inline element gaps  |
| `--sc-space-sm`  | 8px   | Compact padding, small gaps         |
| `--sc-space-md`  | 12px  | Standard padding, list item gaps    |
| `--sc-space-lg`  | 16px  | Section padding, card padding       |
| `--sc-space-xl`  | 24px  | Page-level padding, section margins |
| `--sc-space-2xl` | 32px  | Hero padding, major section breaks  |
| `--sc-space-3xl` | 48px  | Page margins, dramatic spacing      |

### 3.2 Spacing Rhythm

Consistent spacing creates visual rhythm and reduces cognitive load:

- **Within a component**: Use `--sc-space-sm` to `--sc-space-md`
- **Between components**: Use `--sc-space-lg` to `--sc-space-xl`
- **Between sections**: Use `--sc-space-2xl` to `--sc-space-3xl`
- **Page padding**: `--sc-space-xl` on desktop, `--sc-space-md` on mobile

Rules:

- Never skip more than 2 steps in the scale (e.g., don't jump from `xs` to `2xl`)
- Vertical spacing is generally larger than horizontal within the same context
- Breathing room (whitespace) is a feature, not wasted space
- Dense UIs (tables, code) may use tighter spacing, but still token-based

### 3.3 Whitespace as Design Element (Tufte: Data-Ink Ratio)

Empty space is not wasted space. It is a deliberate design choice that:

- Creates visual breathing room
- Separates concerns (Gestalt: proximity)
- Directs attention to content
- Communicates quality and craftsmanship

Rules:

- A view should never feel "cramped" — if it does, add spacing or reduce content
- Cards should have generous internal padding (`--sc-space-lg` minimum)
- Empty states should have extra vertical breathing room (`--sc-space-2xl`)
- Don't fill space just because it's empty — emptiness communicates completeness

---

## 4. Shape & Border System

### 4.1 Border Radius Scale

| Token              | Value  | Use                                  |
| ------------------ | ------ | ------------------------------------ |
| `--sc-radius-sm`   | 4px    | Small elements, tags, badges         |
| `--sc-radius`      | 8px    | Buttons, inputs, standard components |
| `--sc-radius-lg`   | 12px   | Cards, panels, modals                |
| `--sc-radius-xl`   | 16px   | Hero sections, large containers      |
| `--sc-radius-full` | 9999px | Pills, avatars, circular buttons     |

Rules:

- Nested elements use progressively smaller radii (container `lg`, child `md`, input `sm`)
- Never mix sharp corners and rounded corners on the same element
- Radius should feel proportional to element size
- Interactive elements always have some radius (minimum `--sc-radius-sm`)

### 4.2 Border & Divider Usage

| Token                | Use                                            |
| -------------------- | ---------------------------------------------- |
| `--sc-border`        | Standard borders (cards, inputs, separators)   |
| `--sc-border-subtle` | Soft borders (elevated surfaces, hover states) |
| `--sc-border-strong` | Emphasis borders (active states, focus)        |

Rules:

- Prefer `--sc-border-subtle` for surface delineation (less visual noise)
- Reserve `--sc-border-strong` for active/focus states
- Use `--sc-border` for form inputs and explicit boundaries
- Never use `border: 1px solid black` or any raw color value

---

## 5. Shadow & Elevation (Apple: Depth + M3: Elevation)

### 5.1 Shadow Hierarchy

| Token                     | Use                                    | Elevation Level |
| ------------------------- | -------------------------------------- | --------------- |
| `--sc-shadow-sm`          | Subtle lift, cards at rest             | Surface (1)     |
| `--sc-shadow-md`          | Hover state, slight elevation          | Elevated (2)    |
| `--sc-shadow-lg`          | Popovers, dropdowns, floating elements | Overlay (3)     |
| `--sc-shadow-card`        | Default card shadow                    | Surface (1)     |
| `--sc-shadow-glow-accent` | Accent glow for primary actions        | Interactive     |

### 5.2 Elevation Rules

- Shadow intensity correlates with elevation — higher = stronger shadow
- Dark mode: shadows are minimal; elevation communicated via surface brightness
- Light mode: shadows are the primary depth cue
- Interactive elements lift on hover (shadow `sm` → `md`)
- Modals cast the largest shadow + background dimming

---

## 6. Iconography (Phosphor Regular)

### 6.1 Icon System

**Phosphor Regular** is the canonical icon library. No exceptions.

Rules:

- Never use emoji as UI icons (no ⚠️, 💬, 🔧, ⚡, ⚙️)
- Never create one-off SVGs when a Phosphor equivalent exists
- Icons use `currentColor` for fill — color is inherited from parent
- Icon sizes: 16px (inline), 20px (standard), 24px (large), 32px+ (hero)
- Icons are decorative unless they convey meaning not present in text
  - Decorative: `aria-hidden="true"`
  - Meaningful: appropriate `aria-label`

### 6.2 Icon + Text Pairing

- Buttons: Icon left of label, `--sc-space-xs` gap
- Navigation: Icon above label (mobile) or left of label (desktop)
- Status: Icon left of status text, colored semantically
- Never use icon-only buttons without a tooltip or `aria-label`

---

## 7. Typography Rules (Beyond Token Reference)

### 7.1 Typographic Hierarchy

Every screen should have clear typographic levels:

| Level   | Token            | Weight   | Use                          |
| ------- | ---------------- | -------- | ---------------------------- |
| Display | `--sc-text-3xl`  | Bold     | Page titles, hero text       |
| Heading | `--sc-text-xl`   | Semibold | Section headers              |
| Title   | `--sc-text-lg`   | Semibold | Card titles, dialog titles   |
| Body    | `--sc-text-base` | Regular  | Primary content              |
| Caption | `--sc-text-sm`   | Regular  | Secondary text, descriptions |
| Micro   | `--sc-text-xs`   | Regular  | Timestamps, metadata, badges |

Rules:

- Maximum 3 type sizes visible in any single view section
- Don't combine sizes that are too close (e.g., `base` and `sm` in the same line)
- Heading hierarchy follows document structure (h2 → h3 → h4, never skip)
- Line height: 1.1–1.2 for headings, 1.5–1.6 for body text

### 7.2 Text Truncation

- Single line: `text-overflow: ellipsis; overflow: hidden; white-space: nowrap`
- Multi-line: `-webkit-line-clamp` with a defined max-lines
- Always provide full text via tooltip or expand interaction
- Never truncate critical information (status, errors, actions)

### 7.3 Number Formatting

- Use `tabular-nums` for data columns and statistics
- Use locale-aware formatting for dates, currencies, percentages
- Align numeric columns on the decimal point

---

## 8. Component Visual Patterns

### 8.1 Cards

The foundational content container. Rules:

- Background: `--sc-bg-surface` with `--sc-surface-gradient`
- Border: `1px solid var(--sc-border)` or `--sc-border-subtle`
- Radius: `--sc-radius-lg`
- Padding: `--sc-space-lg` (minimum)
- Shadow: `--sc-shadow-card`
- Hover: lift to `--sc-shadow-md` + `translateY(-2px)`
- Clickable cards have `cursor: pointer` and focus ring

### 8.2 Buttons

| Variant     | Background               | Text              | Border        | Use                      |
| ----------- | ------------------------ | ----------------- | ------------- | ------------------------ |
| Primary     | `--sc-accent` + gradient | `--sc-on-accent`  | none          | Primary CTA (1 per view) |
| Secondary   | `--sc-bg-surface`        | `--sc-text`       | `--sc-border` | Secondary actions        |
| Destructive | `--sc-error` + gradient  | `white`           | none          | Delete, remove           |
| Ghost       | transparent              | `--sc-text-muted` | none          | Tertiary actions         |

All buttons:

- Minimum height: 44px (touch target)
- Padding: `--sc-space-sm` vertical, `--sc-space-lg` horizontal
- Radius: `--sc-radius`
- Active: `translateY(1px) scaleY(0.97) scaleX(1.01)` (squash & stretch)
- Focus: `outline: 2px solid var(--sc-accent); outline-offset: 2px`
- Disabled: `opacity: var(--sc-opacity-disabled)`

### 8.3 Forms & Inputs

- Background: `--sc-bg` (inset, darker than surface)
- Border: `--sc-border`, focus: `--sc-accent` + `0 0 0 3px var(--sc-accent-subtle)`
- Label: above input, `--sc-text-sm`, `--sc-weight-medium`
- Error: `--sc-error` border + inline error message below
- Placeholder: `--sc-text-muted`
- Font: `var(--sc-font)`, `var(--sc-text-base)`

### 8.4 Status Indicators

| State        | Color          | Shape      | Animation                          |
| ------------ | -------------- | ---------- | ---------------------------------- |
| Connected    | `--sc-success` | Dot (10px) | Subtle glow pulse                  |
| Disconnected | `--sc-error`   | Dot (10px) | None                               |
| Connecting   | `--sc-warning` | Dot (10px) | `sc-status-pulse` (2s ease-in-out) |
| Active       | `--sc-accent`  | Ring       | Spring pulse ring                  |

---

## 9. Data Visualization (Tufte Principles)

### 9.1 Data-Ink Ratio

Maximize the proportion of ink devoted to data. Minimize non-data ink.

Rules:

- Remove grid lines unless essential for reading values
- Use direct labels instead of legends when practical
- No 3D charts, no decorative gradients on data series
- Axis labels: minimal but sufficient
- Use `--sc-chart-categorical-*` tokens for series colors

### 9.2 Small Multiples

When comparing across categories, use repeated small charts with identical scales
rather than one complex chart with many overlapping series.

### 9.3 Sparklines

Inline mini-charts (`sc-sparkline`) for showing trends without full chart infrastructure.
Use in stat cards, table cells, and dashboard tiles.

---

## 10. Quality Checklist

Before shipping any visual change, verify:

- [ ] **Hierarchy**: Can you identify the primary action by squinting?
- [ ] **Tokens**: Zero raw hex colors, pixel values, or font-family declarations?
- [ ] **Contrast**: 4.5:1 text, 3:1 UI components (check both themes)?
- [ ] **Touch targets**: All interactive elements ≥ 44×44px?
- [ ] **Empty state**: What does this look like with zero data?
- [ ] **Loading state**: Is there a skeleton that mirrors final layout?
- [ ] **Error state**: What happens when data fails to load?
- [ ] **Motion**: All animations use tokens? Reduced motion respected?
- [ ] **Icons**: Phosphor Regular only? No emoji?
- [ ] **Spacing**: Token-based? Consistent rhythm?
- [ ] **Dark + Light**: Works in both themes?

---

## 11. Cross-Reference

| Document             | Covers                                           |
| -------------------- | ------------------------------------------------ |
| `design-strategy.md` | Token values, color, typography, data-viz        |
| `design-system.md`   | Component API, platform-specific implementation  |
| `ux-patterns.md`     | Layout archetypes, interaction patterns          |
| `motion-design.md`   | Animation principles, spring system, performance |
| `AGENTS.md` §12      | Enforcement rules for agents                     |
