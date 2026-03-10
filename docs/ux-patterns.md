---
title: Human UX Patterns & Layout Architecture
---

# Human UX Patterns & Layout Architecture

> Normative reference for view composition, layout archetypes, responsive adaptation,
> and interaction patterns. All views must conform to one of the archetypes below.
> Agents must consult this document before creating or restructuring any view.

## SOTA References

This document synthesizes principles from:

- **Material 3 Canonical Layouts** — List-Detail, Feed, Supporting Pane
- **Apple Human Interface Guidelines** — Clarity, Deference, Depth; content-first hierarchy
- **Nielsen Norman Group** — F-pattern scanning, progressive disclosure, recognition over recall

---

## 1. Core Layout Principles

### 1.1 Content First, Controls Second (Apple HIG: Deference)

The interface exists to serve content, not itself. Controls defer to content:

- **Content occupies the majority of viewport** — minimum 60% of available height
- **Controls anchor to edges** (top bar, bottom bar) — never sandwiched between content
- **Primary action is always reachable** — within thumb zone on mobile, keyboard-accessible on desktop

### 1.2 Spatial Hierarchy (Material 3: Elevation + Apple: Depth)

Visual layers communicate importance and interactivity:

| Layer    | Z-Index | Token              | Content                                |
| -------- | ------- | ------------------ | -------------------------------------- |
| Base     | 0       | `--hu-bg`          | Page background                        |
| Surface  | 1       | `--hu-bg-surface`  | Cards, panels, conversation areas      |
| Elevated | 2       | `--hu-bg-elevated` | Dropdowns, popovers, floating controls |
| Overlay  | 3       | `--hu-bg-overlay`  | Modals, sheets, toasts                 |
| System   | 4       | —                  | Skip links, disconnect banners         |

Shadows must match elevation: `--hu-shadow-sm` for surface, `--hu-shadow-md` for elevated,
`--hu-shadow-lg` for overlay. Never use shadows without corresponding elevation context.

### 1.3 Responsive Adaptation (Material 3: Window Size Classes)

Views must adapt across four breakpoints. Use Material 3's window size class model:

| Class    | Breakpoint | Columns | Behavior                                    |
| -------- | ---------- | ------- | ------------------------------------------- |
| Compact  | < 480px    | 1       | Single-column, bottom nav, stacked controls |
| Medium   | 480–768px  | 1–2     | Collapsible sidebar, 2-column grids         |
| Expanded | 768–1024px | 2–3     | Persistent sidebar, multi-pane layouts      |
| Wide     | > 1024px   | 3+      | Full canonical layout, supporting panes     |

Rules:

- Navigation collapses to bottom bar below `--hu-breakpoint-md` (768px)
- Grid columns reduce: 3→2→1 as width decreases
- Touch targets minimum 44×44px on all breakpoints (Apple HIG)
- Content padding reduces: `--hu-space-2xl` → `--hu-space-md` on compact

---

## 2. View Archetypes

Every view must conform to one of these archetypes. Do not invent new layout patterns.

### 2.1 Dashboard (Feed)

**Used by**: Overview, Usage, Security

```
┌─────────────────────────────────────┐
│  Page Hero (title + status + CTA)   │  ← flex-shrink: 0
├─────────────────────────────────────┤
│                                     │
│  Bento Grid / Card Feed             │  ← scrollable, auto-flow
│  (stat cards, charts, summaries)    │
│                                     │
└─────────────────────────────────────┘
```

Rules:

- Hero is compact: title + subtitle + 1–2 actions. No oversized heroes.
- Grid uses `auto-fill` / `auto-fit` for responsive column count
- Cards are independently scrollable on overflow
- Skeleton loading mirrors final grid structure exactly

### 2.2 List-Detail (Material 3 Canonical)

**Used by**: Sessions, Channels, Tools, Nodes, Agents, Models

```
Expanded (≥768px):                    Compact (<768px):
┌──────────┬──────────────────┐       ┌──────────────────────┐
│  List    │  Detail          │       │  List OR Detail      │
│  Panel   │  Panel           │       │  (navigation toggle) │
│          │                  │       │                      │
│  ↕scroll │  ↕scroll         │       │  ↕scroll             │
└──────────┴──────────────────┘       └──────────────────────┘
```

Rules:

- List pane: 280–360px fixed width on expanded; full-width on compact
- Detail pane: flex-grow, min-width 0
- Selection in list highlights row and loads detail
- On compact: list→detail is a forward navigation; back button returns to list
- Empty detail state shows `hu-empty-state` with guidance text

### 2.3 Conversational (Voice, Chat)

**Used by**: Chat View, Voice View

```
┌─────────────────────────────────┐
│  Compact Header (status bar)    │  ← flex-shrink: 0
├─────────────────────────────────┤
│                                 │
│  Conversation Area              │  ← flex: 1, overflow-y: auto
│  (messages scroll here)         │
│                                 │
├─────────────────────────────────┤
│  Input Controls (anchored)      │  ← flex-shrink: 0
└─────────────────────────────────┘
```

**This is the most critical archetype to get right.**

Rules:

- **Conversation area MUST be the primary visual element** — it gets `flex: 1`
- **Input controls MUST anchor to the bottom** — never above conversation
- **Header MUST be compact** — one line of title + status. No hero cards, no stat rows.
- Messages auto-scroll to bottom on new content
- Empty state centers vertically within conversation area
- Voice-specific: mic button lives below conversation, above or within input bar
- On mobile: input bar sticks to bottom with `position: sticky` or flex layout
- Keyboard: Enter sends, Shift+Enter for newline

**Anti-patterns (NEVER do these)**:

- Placing conversation below controls (voice bug we just fixed)
- Using `max-height` on conversation (use `flex: 1; min-height: 0` instead)
- Oversized hero/stats above conversation (wastes prime viewport space)
- Placing the mic button above the conversation area

### 2.4 Settings / Configuration

**Used by**: Config View

```
┌─────────────────────────────────┐
│  Page Title                     │
├─────────────────────────────────┤
│  Section 1                      │
│  ┌─ Field ──────────────────┐   │
│  ├─ Field ──────────────────┤   │
│  └──────────────────────────┘   │
│  Section 2                      │
│  ┌─ Field ──────────────────┐   │
│  └──────────────────────────┘   │
└─────────────────────────────────┘
```

Rules:

- Single-column layout, max-width 640px, centered
- Sections grouped by concern with clear headings
- Form fields use full width within container
- Save/apply actions sticky at bottom or inline per section
- Validation errors appear inline below fields, not as toasts

### 2.5 Marketplace / Gallery

**Used by**: Skills View, potentially Providers

```
┌─────────────────────────────────┐
│  Search Bar + Filters           │  ← flex-shrink: 0
├─────────────────────────────────┤
│                                 │
│  Card Grid (responsive)         │  ← scrollable
│  ┌────┐ ┌────┐ ┌────┐          │
│  └────┘ └────┘ └────┘          │
│  ┌────┐ ┌────┐                  │
│  └────┘ └────┘                  │
└─────────────────────────────────┘
```

Rules:

- Search is always visible, never scrolled away (sticky or flex-shrink: 0)
- Cards use consistent aspect ratios within a grid
- Grid adapts: 3→2→1 columns across breakpoints
- Card hover shows elevated shadow + subtle scale
- Empty search state provides helpful suggestions

### 2.6 Log / Terminal

**Used by**: Logs View

```
┌─────────────────────────────────┐
│  Controls (filter, level, clear)│  ← flex-shrink: 0
├─────────────────────────────────┤
│                                 │
│  Log Output (monospace)         │  ← flex: 1, overflow-y: auto
│  newest at bottom               │
│                                 │
└─────────────────────────────────┘
```

Rules:

- Log area uses `--hu-font-mono`
- Auto-scroll to bottom (with scroll-lock toggle)
- Controls anchored at top
- Color-coded log levels using semantic tokens

---

## 3. Component Interaction Patterns

### 3.1 Progressive Disclosure (NNG)

Reveal complexity gradually. Start with the essential; expand on demand.

- Default views show summary state; expand/detail on interaction
- Advanced settings hidden behind "Advanced" toggle or disclosure
- Error details expandable (show message, expand for stack trace)

### 3.2 Recognition Over Recall (NNG / Apple HIG: Clarity)

Users should recognize options, not recall commands.

- Use icons + labels together (not icons alone, except in compact mobile nav)
- Provide placeholder text in empty states explaining what to do
- Command palette (`Cmd+K`) for power users; visual navigation for everyone else
- Recent/frequent items surfaced first in lists

### 3.3 Feedback & Affordance

Every interaction must provide immediate feedback:

| Interaction  | Feedback                          | Timing                   |
| ------------ | --------------------------------- | ------------------------ |
| Button click | Visual depress + state change     | `--hu-duration-fast`     |
| Form submit  | Loading state → success/error     | Immediate → result       |
| Navigation   | View transition animation         | `--hu-duration-moderate` |
| Drag         | Shadow elevation + cursor change  | Immediate                |
| Error        | Inline message + border highlight | `--hu-duration-fast`     |
| Delete       | Confirmation dialog → toast       | Before → after           |

### 3.4 Loading States (Apple HIG: Skeleton)

Never show a blank screen. Every view must have a loading skeleton:

- Skeleton shapes mirror the final content layout exactly
- Use `hu-skeleton` component with appropriate `variant` (card, text, circle)
- Skeleton animates with a shimmer using `--hu-duration-slower`
- Transition from skeleton → content uses `--hu-duration-normal` fade

### 3.5 Empty States

Every list/collection view must handle the empty case:

- Show `hu-empty-state` with: icon + heading + description + optional CTA
- Description tells the user how to populate the empty state
- CTA provides a direct path to the first item creation
- Never show a blank white box

### 3.6 Error States

Errors follow a severity hierarchy:

| Severity | Treatment                        | Example                |
| -------- | -------------------------------- | ---------------------- |
| Field    | Inline below input, `--hu-error` | "Required field"       |
| Section  | Banner within section            | "Could not load data"  |
| Page     | `hu-error-boundary` fallback     | Render crash           |
| System   | Top-level disconnect banner      | WebSocket disconnected |

### 3.7 Category-Defining Interaction Principles

#### Anticipatory UX

Beyond "fast interactions" — build UX that predicts what you need. The user should never
wait. Not "fast" — instant.

Rules:

- Prefetch the top 3 most likely next navigation targets on every view load
- Pre-render command palette results for the 20 most common commands
- Preload detail pane data on list item hover (200ms debounce)
- Use `requestIdleCallback` for speculative prefetching during idle time

#### Information Theater

Data presentation so clear that understanding is involuntary. No legends needed. No
explanation text. The chart IS the explanation. From Tufte, weaponized.

Rules:

- Direct labels on all chart elements (no separate legend unless >5 series)
- Sparklines for trend context on every numeric metric
- Color encodes one dimension only — never overload color with multiple meanings
- Annotations directly on data points, not in footnotes

#### Keyboard-First Design

Every view has a complete keyboard interaction model. Command palette is primary
navigation, not a nice-to-have. From Linear and Superhuman.

Rules:

- Every action reachable via keyboard shortcut
- Shortcuts discoverable through contextual hints (shown on hover + in command palette)
- Vim-style navigation (j/k for lists, h/l for panes) as opt-in power user mode
- Focus management: every view change moves focus to the primary content area

#### Platform Transcendence

Each platform surface must feel native to that platform, not ported from web.

Rules:

- iOS: Apple HIG spring animations, haptic feedback, swipe gestures natively
- Android: Material 3 motion, adaptive icons, predictive back gesture
- macOS: Native menu bar integration, trackpad gestures, Quick Look support
- Web: Progressive enhancement, offline support, installable PWA
- Never share animation code between platforms — each gets native-feeling motion

#### Interaction Latency Contract

Every interaction type has an explicit latency budget, measured and enforced.

| Interaction                  | Budget           | Measurement Method              |
| ---------------------------- | ---------------- | ------------------------------- |
| Key press to visual feedback | < 16ms (1 frame) | Playwright + performance.now()  |
| Button tap to state change   | < 50ms           | Playwright interaction timing   |
| View transition complete     | < 200ms          | View Transitions API timing     |
| Search results populated     | < 100ms          | Playwright + performance.mark() |
| Command palette response     | < 80ms           | Playwright + performance.mark() |

---

## 4. Navigation Patterns

### 4.1 Primary Navigation

- **Desktop**: Persistent sidebar (`hu-sidebar`), collapsible to icon-only
- **Mobile**: Bottom tab bar with 5 primary tabs + "More" sheet for secondary
- Active tab indicated by accent color + bold weight

### 4.2 Secondary Navigation

- Tabs within a view for sub-sections
- Breadcrumbs for deep hierarchies (list → detail → sub-detail)
- Back button for compact list-detail navigation

### 4.3 Keyboard Navigation

All views must be fully keyboard-navigable:

- Tab order follows visual reading order (top→bottom, left→right)
- Arrow keys navigate within grids and lists
- Escape closes modals, sheets, dropdowns
- Focus ring visible on all interactive elements (`--hu-focus-ring`)
- Skip link (`#main-content`) at document start

---

## 5. Accessibility Contract

### 5.1 WCAG 2.1 AA (Minimum)

- Text contrast: 4.5:1 minimum (3:1 for large text)
- UI component contrast: 3:1 minimum
- Focus indicators: visible, 2px minimum width
- Target size: 44×44px minimum touch target (24×24px minimum pointer target)

### 5.2 Semantic Markup

- Headings follow hierarchy (h1 → h2 → h3, never skip levels)
- Lists use `<ul>`/`<ol>`, not divs with styling
- Interactive regions have `role`, `aria-label`, or `aria-labelledby`
- Live regions (`aria-live="polite"`) for dynamic content updates
- Modals have `aria-modal="true"` and focus trap

### 5.3 Motion Accessibility

- All animation respects `prefers-reduced-motion: reduce`
- No essential information conveyed only through animation
- Auto-playing animations can be paused
- Parallax and scroll-jacking are prohibited

---

## 6. Gestalt Principles in Practice

These perceptual principles govern layout decisions:

### Proximity

Related elements are close together; unrelated elements have clear separation.

- Group form fields by section with `--hu-space-2xl` between sections
- Within a group, use `--hu-space-md` between elements
- Card grids use consistent gap (`--hu-space-lg`)

### Similarity

Elements that look alike are perceived as related.

- All cards in a grid use the same component (`hu-card`)
- Status indicators use consistent shape (dot) + semantic color
- Action buttons within a context use consistent sizing and style

### Continuity

Elements arranged on a line or curve are perceived as related.

- Navigation items align on a vertical axis (sidebar) or horizontal axis (tabs)
- Form labels and inputs align on the same vertical grid line
- List items maintain consistent left edge

### Closure

The mind completes incomplete shapes.

- Card borders define clear content boundaries
- Grid layouts imply structure even when not all cells are filled
- Skeleton loading leverages closure — users perceive the final layout

### Figure-Ground

Clear distinction between foreground content and background.

- Modals dim background with overlay (`--hu-bg-overlay`)
- Active/selected items elevated above siblings
- Focus ring distinguishes interactive element from surrounding content

---

## 7. Anti-Patterns (Prohibited)

| Anti-Pattern                      | Why It's Wrong                 | Correct Approach                           |
| --------------------------------- | ------------------------------ | ------------------------------------------ |
| Controls above conversation       | Buries primary content         | Conversation first, controls at bottom     |
| Fixed-height scrollable areas     | Wastes viewport on non-content | `flex: 1; min-height: 0; overflow-y: auto` |
| Oversized page heroes             | Pushes content below fold      | Compact hero: title + status + 1-2 actions |
| Stats/metrics above primary UX    | Distracts from core task       | Stats in sidebar or secondary view         |
| Icon-only buttons without tooltip | Violates recognition principle | Icon + label, or icon + tooltip            |
| Modals for simple confirmations   | Blocks user flow unnecessarily | Inline confirmation or undo pattern        |
| Horizontal scroll on content      | Breaks reading pattern         | Wrap or paginate                           |
| Auto-hiding controls on scroll    | Unpredictable, inaccessible    | Sticky or always-visible                   |
