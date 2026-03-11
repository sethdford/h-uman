---
title: Human Visual Design Standards
---

# Human Visual Design Standards

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

Every element has an emphasis level. M3 defines three; Human maps them to tokens:

| Emphasis | Visual Treatment                      | Token Pattern         | Example                            |
| -------- | ------------------------------------- | --------------------- | ---------------------------------- |
| High     | Accent color, bold weight, elevated   | `--hu-accent`, `bold` | Primary CTA, active nav            |
| Medium   | Subtle accent, medium weight, surface | `--hu-text`, `medium` | Secondary actions, section headers |
| Low      | Muted color, regular weight, flat     | `--hu-text-muted`     | Timestamps, metadata, hints        |

Rules:

- A screen should have exactly ONE high-emphasis element (the primary action)
- Maximum 2–3 medium-emphasis elements visible simultaneously
- Low-emphasis elements should not compete with medium or high
- Never use high emphasis for decorative or non-interactive elements

### 1.5 Category-Defining Principles

These principles go beyond industry standards. They define the ceiling other developer tools
measure against. See `docs/competitive-benchmarks.md` for the named brands we benchmark against.

#### Zero-Compromise Aesthetics

Stripe proved beauty elevates utility by 20%. Human goes further: every pixel must
simultaneously be the most functional AND the most beautiful version of that element in the
developer tools space. No trade-off mentality. If a design choice requires choosing between
craft and function, the design is wrong — find the third option.

Rules:

- Every component must pass both a usability audit AND a visual craft review
- "Good enough" is not acceptable for shipping UI — only "best in class"
- Study Linear, Stripe, and Figma quarterly for craft inspiration

#### Perceptual Performance

Beyond actual metrics, visual design must _feel_ faster than it is. The competitive
ceiling isn't "fast loading" — it's "I never noticed it loaded."

Rules:

- Skeleton loading must perfectly mirror final layout (shape, position, size)
- Optimistic UI for all user-initiated actions (show result before confirmation)
- Instant visual feedback (<16ms) for every interaction, even if data hasn't arrived
- Transition from loading to loaded must be imperceptible (crossfade, not replace)

#### Depth as Language

Our glass system (3 tiers + Apple visionOS materials + choreography) exceeds what
competitors offer. Depth conveys information hierarchy, not just decoration.

Rules:

- Elevation changes must carry semantic meaning (higher = more important/interactive)
- Glass blur density correlates with content focus (sharper = primary, blurrier = ambient)
- Never use depth for decoration alone — every shadow, blur, and layer must communicate

#### Monochrome Confidence

The UI must work beautifully in near-monochrome. Accent color is a surgical instrument,
not a theme. Linear and Superhuman prove that restraint is power.

Rules:

- Every screen must pass a grayscale squint test — hierarchy must be clear without color
- Accent color used for exactly ONE primary action per viewport
- Maximum 10% of any viewport should use accent color (60-30-10 rule)
- Test all views in grayscale as part of design review

#### Calm Technology

Powerful functionality that doesn't demand attention. Subtle animations that guide
without distracting. From Stripe's design philosophy.

Rules:

- Three-tier quality litmus for every component: utility, usability, beauty
- All three addressed simultaneously, never sequentially
- Animations must guide attention, not compete for it
- Information surfaces gradually through progressive disclosure, not all at once

#### Typographic Confidence

Type as a primary design element, not just for reading. Award winners (Malvah, Lando
Norris, Immersive Garden) use type as hero.

Rules:

- Mathematically perfect type scale with modular ratio baked into tokens
- Display type (hero, page titles) may use letter-spacing as texture
- Maximum 3 type sizes per viewport section (existing rule, now enforced as category-defining)
- Type hierarchy alone must communicate page structure — test without icons or color

---

## 2. Color Application

### 2.1 The 60-30-10 Rule

Classic interior design proportion applied to UI:

| Proportion | Role       | Human Token                                    |
| ---------- | ---------- | ---------------------------------------------- |
| 60%        | Background | `--hu-bg`, `--hu-bg-surface`                   |
| 30%        | Secondary  | `--hu-text`, `--hu-border`, `--hu-bg-elevated` |
| 10%        | Accent     | `--hu-accent`, `--hu-accent-secondary`         |

Rules:

- Accent color is used sparingly — only for primary actions, active states, and links
- Background dominates — most of the screen is neutral surface
- Text and borders provide structure without competing with accent
- Never use more than 3 accent colors on a single screen

### 2.2 Semantic Color Usage

Colors carry meaning. Never override semantic associations:

| Color  | Meaning       | Token          | Usage                              |
| ------ | ------------- | -------------- | ---------------------------------- |
| Green  | Success, safe | `--hu-success` | Confirmations, connected status    |
| Amber  | Warning       | `--hu-warning` | Caution, pending states            |
| Coral  | Error, danger | `--hu-error`   | Errors, destructive actions ONLY   |
| Blue   | Information   | `--hu-info`    | Help text, informational callouts  |
| Accent | Brand, action | `--hu-accent`  | CTAs, links, focus, selected state |

Rules:

- Coral is NEVER used as a general accent — reserved exclusively for error/destructive
- Green (accent) vs green (success): accent uses `--hu-accent`, success uses `--hu-success`
- Don't rely on color alone — always pair with icon, label, or pattern (a11y requirement)

### 2.3 Dark Mode (Default)

Human's default is dark mode. Light mode is secondary.

Dark mode rules:

- Elevation is communicated through lighter surfaces (not shadows alone)
- Higher elevation = lighter background (`--hu-bg` → `--hu-bg-surface` → `--hu-bg-elevated`)
- Shadows are subtle — primary depth cue is surface brightness
- Text uses off-white (`--hu-text`), never pure `#ffffff` (reduces eye strain)
- Accent colors may be slightly desaturated for comfortable viewing

Light mode rules:

- Elevation communicated through shadows primarily
- Backgrounds are warm whites, not cold `#ffffff`
- Text uses near-black (`--hu-text`), never pure `#000000`
- Accent colors slightly darkened for contrast compliance

---

## 3. Spacing System

### 3.1 The 4px Grid (Material 3)

All spacing derives from a 4px base unit. Human's spacing tokens:

| Token            | Value | Use                                 |
| ---------------- | ----- | ----------------------------------- |
| `--hu-space-2xs` | 2px   | Hairline gaps, icon-to-label inline |
| `--hu-space-xs`  | 4px   | Tight padding, inline element gaps  |
| `--hu-space-sm`  | 8px   | Compact padding, small gaps         |
| `--hu-space-md`  | 12px  | Standard padding, list item gaps    |
| `--hu-space-lg`  | 16px  | Section padding, card padding       |
| `--hu-space-xl`  | 24px  | Page-level padding, section margins |
| `--hu-space-2xl` | 32px  | Hero padding, major section breaks  |
| `--hu-space-3xl` | 48px  | Page margins, dramatic spacing      |

### 3.2 Spacing Rhythm

Consistent spacing creates visual rhythm and reduces cognitive load:

- **Within a component**: Use `--hu-space-sm` to `--hu-space-md`
- **Between components**: Use `--hu-space-lg` to `--hu-space-xl`
- **Between sections**: Use `--hu-space-2xl` to `--hu-space-3xl`
- **Page padding**: `--hu-space-xl` on desktop, `--hu-space-md` on mobile

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
- Cards should have generous internal padding (`--hu-space-lg` minimum)
- Empty states should have extra vertical breathing room (`--hu-space-2xl`)
- Don't fill space just because it's empty — emptiness communicates completeness

---

## 4. Shape & Border System

### 4.1 Border Radius Scale

| Token              | Value  | Use                                  |
| ------------------ | ------ | ------------------------------------ |
| `--hu-radius-sm`   | 4px    | Small elements, tags, badges         |
| `--hu-radius`      | 8px    | Buttons, inputs, standard components |
| `--hu-radius-lg`   | 12px   | Cards, panels, modals                |
| `--hu-radius-xl`   | 16px   | Hero sections, large containers      |
| `--hu-radius-full` | 9999px | Pills, avatars, circular buttons     |

Rules:

- Nested elements use progressively smaller radii (container `lg`, child `md`, input `sm`)
- Never mix sharp corners and rounded corners on the same element
- Radius should feel proportional to element size
- Interactive elements always have some radius (minimum `--hu-radius-sm`)

### 4.2 Border & Divider Usage

| Token                | Use                                            |
| -------------------- | ---------------------------------------------- |
| `--hu-border`        | Standard borders (cards, inputs, separators)   |
| `--hu-border-subtle` | Soft borders (elevated surfaces, hover states) |
| `--hu-border-strong` | Emphasis borders (active states, focus)        |

Rules:

- Prefer `--hu-border-subtle` for surface delineation (less visual noise)
- Reserve `--hu-border-strong` for active/focus states
- Use `--hu-border` for form inputs and explicit boundaries
- Never use `border: 1px solid black` or any raw color value

---

## 5. Shadow & Elevation (Apple: Depth + M3: Elevation)

### 5.1 Shadow Hierarchy

| Token                     | Use                                    | Elevation Level |
| ------------------------- | -------------------------------------- | --------------- |
| `--hu-shadow-sm`          | Subtle lift, cards at rest             | Surface (1)     |
| `--hu-shadow-md`          | Hover state, slight elevation          | Elevated (2)    |
| `--hu-shadow-lg`          | Popovers, dropdowns, floating elements | Overlay (3)     |
| `--hu-shadow-card`        | Default card shadow                    | Surface (1)     |
| `--hu-shadow-glow-accent` | Accent glow for primary actions        | Interactive     |

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

- Buttons: Icon left of label, `--hu-space-xs` gap
- Navigation: Icon above label (mobile) or left of label (desktop)
- Status: Icon left of status text, colored semantically
- Never use icon-only buttons without a tooltip or `aria-label`

---

## 7. Typography Rules (Beyond Token Reference)

### 7.1 Typographic Hierarchy

Every screen should have clear typographic levels:

| Level   | Token            | Weight   | Use                          |
| ------- | ---------------- | -------- | ---------------------------- |
| Display | `--hu-text-3xl`  | Bold     | Page titles, hero text       |
| Heading | `--hu-text-xl`   | Semibold | Section headers              |
| Title   | `--hu-text-lg`   | Semibold | Card titles, dialog titles   |
| Body    | `--hu-text-base` | Regular  | Primary content              |
| Caption | `--hu-text-sm`   | Regular  | Secondary text, descriptions |
| Micro   | `--hu-text-xs`   | Regular  | Timestamps, metadata, badges |

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

- Background: `--hu-bg-surface` with `--hu-surface-gradient`
- Border: `1px solid var(--hu-border)` or `--hu-border-subtle`
- Radius: `--hu-radius-lg`
- Padding: `--hu-space-lg` (minimum)
- Shadow: `--hu-shadow-card`
- Hover: lift to `--hu-shadow-md` + `translateY(-2px)`
- Clickable cards have `cursor: pointer` and focus ring

### 8.2 Buttons

| Variant     | Background               | Text              | Border        | Use                      |
| ----------- | ------------------------ | ----------------- | ------------- | ------------------------ |
| Primary     | `--hu-accent` + gradient | `--hu-on-accent`  | none          | Primary CTA (1 per view) |
| Secondary   | `--hu-bg-surface`        | `--hu-text`       | `--hu-border` | Secondary actions        |
| Destructive | `--hu-error` + gradient  | `white`           | none          | Delete, remove           |
| Ghost       | transparent              | `--hu-text-muted` | none          | Tertiary actions         |

All buttons:

- Minimum height: 44px (touch target)
- Padding: `--hu-space-sm` vertical, `--hu-space-lg` horizontal
- Radius: `--hu-radius`
- Active: `translateY(1px) scaleY(0.97) scaleX(1.01)` (squash & stretch)
- Focus: `outline: 2px solid var(--hu-accent); outline-offset: 2px`
- Disabled: `opacity: var(--hu-opacity-disabled)`

### 8.3 Forms & Inputs

- Background: `--hu-bg` (inset, darker than surface)
- Border: `--hu-border`, focus: `--hu-accent` + `0 0 0 3px var(--hu-accent-subtle)`
- Label: above input, `--hu-text-sm`, `--hu-weight-medium`
- Error: `--hu-error` border + inline error message below
- Placeholder: `--hu-text-muted`
- Font: `var(--hu-font)`, `var(--hu-text-base)`

### 8.4 Status Indicators

| State        | Color          | Shape      | Animation                          |
| ------------ | -------------- | ---------- | ---------------------------------- |
| Connected    | `--hu-success` | Dot (10px) | Subtle glow pulse                  |
| Disconnected | `--hu-error`   | Dot (10px) | None                               |
| Connecting   | `--hu-warning` | Dot (10px) | `hu-status-pulse` (2s ease-in-out) |
| Active       | `--hu-accent`  | Ring       | Spring pulse ring                  |

---

## 9. Data Visualization (Tufte Principles)

### 9.1 Data-Ink Ratio

Maximize the proportion of ink devoted to data. Minimize non-data ink.

Rules:

- Remove grid lines unless essential for reading values
- Use direct labels instead of legends when practical
- No 3D charts, no decorative gradients on data series
- Axis labels: minimal but sufficient
- Use `--hu-chart-categorical-*` tokens for series colors

### 9.2 Small Multiples

When comparing across categories, use repeated small charts with identical scales
rather than one complex chart with many overlapping series.

### 9.3 Sparklines

Inline mini-charts (`hu-sparkline`) for showing trends without full chart infrastructure.
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
- [ ] **Perceptual speed**: Does the transition from loading to loaded feel instantaneous?
- [ ] **Monochrome test**: Does the screen pass the grayscale squint test?
- [ ] **Competitive ceiling**: Would this screen hold up next to Linear, Stripe, or Vercel?

---

## 11. Cross-Reference

| Document             | Covers                                           |
| -------------------- | ------------------------------------------------ |
| `design-strategy.md` | Token values, color, typography, data-viz        |
| `design-system.md`   | Component API, platform-specific implementation  |
| `ux-patterns.md`     | Layout archetypes, interaction patterns          |
| `motion-design.md`   | Animation principles, spring system, performance |
| `AGENTS.md` §12      | Enforcement rules for agents                     |
