# Project Prism — SOTA Chat Design Overhaul

## Problem

After Project Scalpel decomposed the chat page into clean components, the architecture is solid but the visual design is functional, not exceptional. Deep research across Dribbble, Behance, and production UIs (Claude.ai, ChatGPT, Linear, Vercel) reveals significant gaps in atmosphere, micro-interactions, and premium feel.

## Design Philosophy: "Deep Steel"

Hybrid of three influences:

- **Linear**: Information density, keyboard-first, 1px borders, near-monochrome, restraint
- **Claude.ai**: Generous whitespace, soft transitions, professional clarity
- **Dribbble Premium**: Selective glassmorphism, layered shadows, accent glow, stagger animations

Core principle: **restraint creates luxury**. Glass on 1-2 layers max. Shadows do the heavy lifting. Blue is the only color. Everything else is gray.

## 1. Color Tokens — "Deep Steel" Palette

### Dark Mode (primary)

| Token                | Value                    | Usage                             |
| -------------------- | ------------------------ | --------------------------------- |
| `--sc-bg`            | `#0c0e14`                | Page background — deep navy-black |
| `--sc-bg-surface`    | `#12151e`                | Cards, panels, message bubbles    |
| `--sc-bg-elevated`   | `#1a1e2a`                | Hover, popovers, active states    |
| `--sc-border`        | `rgba(148,163,194,0.08)` | Hairline borders                  |
| `--sc-border-subtle` | `rgba(148,163,194,0.04)` | Dividers                          |
| `--sc-text`          | `#e2e8f0`                | Primary text — cool white         |
| `--sc-text-muted`    | `#64748b`                | Secondary text — slate-500        |
| `--sc-accent`        | `#3b82f6`                | Blue-500 — single accent          |
| `--sc-accent-hover`  | `#60a5fa`                | Blue-400 — luminous hover         |
| `--sc-accent-subtle` | `rgba(59,130,246,0.10)`  | Glow backgrounds                  |
| `--sc-accent-text`   | `#93c5fd`                | Blue-300 — text on dark           |

### Light Mode

| Token              | Value                 |
| ------------------ | --------------------- |
| `--sc-bg`          | `#f8fafc`             |
| `--sc-bg-surface`  | `#f1f5f9`             |
| `--sc-bg-elevated` | `#e2e8f0`             |
| `--sc-border`      | `rgba(15,23,42,0.06)` |
| `--sc-text`        | `#0f172a`             |
| `--sc-text-muted`  | `#64748b`             |
| Same blue accent   | —                     |

### Shadows (layered depth)

| Token              | Value                                                      |
| ------------------ | ---------------------------------------------------------- |
| `--sc-shadow-sm`   | `0 1px 2px rgba(0,0,0,0.3), 0 1px 3px rgba(0,0,0,0.15)`    |
| `--sc-shadow-md`   | `0 4px 12px rgba(0,0,0,0.4), 0 2px 4px rgba(0,0,0,0.2)`    |
| `--sc-shadow-lg`   | `0 12px 40px rgba(0,0,0,0.5), 0 4px 12px rgba(0,0,0,0.25)` |
| `--sc-shadow-glow` | `0 0 30px rgba(59,130,246,0.08)`                           |

### Glass Treatments

| Surface         | Background            | Blur                  | Usage         |
| --------------- | --------------------- | --------------------- | ------------- |
| Sessions panel  | `rgba(18,21,30,0.85)` | 24px + saturate(180%) | Side panel    |
| Message actions | `rgba(18,21,30,0.90)` | 16px                  | Hover toolbar |
| Context menus   | `rgba(26,30,42,0.92)` | 20px                  | Dropdowns     |

## 2. Composer — "The Command Bar"

Current: flat textarea with text "Send" button.

Target:

- Rounded container (`--sc-radius-lg`) with `--sc-bg-surface` background
- 1px border transitions to `--sc-accent` + glow on focus
- **Send button**: solid accent circle (36px), Phosphor "arrow-up" icon — like iMessage
- **Attach button**: ghost icon, accent on hover
- Inner shadow on textarea for depth
- Character count fades in after 100+ chars

Micro-interactions:

- Focus: border → accent (200ms), faint blue glow expands
- Send press: `scale(0.95)` → spring back
- Post-send: textarea height animates to min-height (spring)
- Attached files: stagger slide-up (50ms per file)

Empty state:

- 2x2 bento grid of suggestion cards (not flat pills)
- Each card: Phosphor icon (24px) + title + one-line description
- Cards: `--sc-bg-surface`, hover → `--sc-bg-elevated` + `--sc-shadow-glow`
- Stagger entrance (100ms per card)
- Above grid: centered brand mark + "How can I help?" in large light weight

## 3. Message List — "The Timeline"

### Message Bubbles

- **User**: gradient `#1e40af` → `#2563eb` (deep → vibrant blue), white text, right-aligned, max-width 75%
- **Assistant**: `--sc-bg-surface` background, 1px cool border, left-aligned, max-width 85%
- Both: `--sc-radius-lg` on all corners except tail corner (`--sc-radius-sm`) — directional
- Timestamp: appears on hover, slides down 150ms
- Stagger entrance: 50ms \* index (capped at 300ms)

### Tool Calls

- Compact card with left accent border (blue=running, green=success, red=error)
- Collapse/expand with smooth height animation
- Running: subtle blue pulse on border

### Thinking Indicator

- Three dots with stagger bounce (blue tint)
- "Thinking..." in muted, elapsed time tabular-nums
- Abort: ghost button, red on hover

### Scroll Pill

- Glass pill with `--sc-shadow-md`
- Blue accent left border + arrow-down icon
- Spring bounce entrance

## 4. Sessions Panel — "The Library"

Current: basic list.

Target:

- 280px, glass background, `--sc-shadow-lg` on right edge
- Slide-in: translateX + opacity (spring easing), not just width
- **Time groups**: "Today", "Yesterday", "This Week", "Older" — uppercase `--sc-text-2xs`, letter-spacing 0.05em
- Active session: left accent border + accent-subtle background
- Hover: elevated background, delete button fades in
- **Inline rename**: double-click to edit title (contenteditable)
- **New Chat**: full-width, plus icon + text, accent border + glow on hover

## 5. Message Actions — "The Toolbar"

Current: glass toolbar, basic icons.

Target:

- Glass background with blue tint
- 150ms fade + `translateY(-4px)` entrance
- **Copy feedback**: icon swaps to checkmark for 1.5s, green tint
- **Tooltips**: glass tooltip with pointer, 100ms delay
- Buttons: 28px squares, `--sc-radius-sm`, hover → `--sc-bg-elevated`

## 6. Status Bar — "The HUD"

Current: small dot + label + toggle.

Target:

- Session title (editable on click) centered
- Connection indicator left
- Sessions toggle far left
- Keyboard hints far right: `Cmd+F` search
- 1px bottom border, transparent background
- No background fill — lets depth show through

## 7. Focus & Interaction States

Global focus ring: `0 0 0 2px var(--sc-accent), 0 0 12px rgba(59,130,246,0.2)` — glow, not just outline.

Active states: accent-subtle background + left accent border.

Custom scrollbar: thin (`6px`), `--sc-bg-elevated` thumb, transparent track, rounded.

## 8. Animation Tokens

| Token                    | Value                                  | Usage            |
| ------------------------ | -------------------------------------- | ---------------- |
| `--sc-spring-micro`      | `cubic-bezier(0.34,1.56,0.64,1)`       | Button press     |
| `--sc-spring-standard`   | `cubic-bezier(0.22,1,0.36,1)`          | Panel slide      |
| `--sc-spring-expressive` | `cubic-bezier(0.175,0.885,0.32,1.275)` | Entrance         |
| `--sc-stagger-delay`     | `50ms`                                 | Per-item stagger |
| `--sc-stagger-max`       | `300ms`                                | Stagger cap      |

All animations: `prefers-reduced-motion: reduce` → `transition: none; animation: none;`

## Components Modified

| Component                | Changes                                                                |
| ------------------------ | ---------------------------------------------------------------------- |
| `design-tokens/`         | New Deep Steel palette, shadow tokens, glass tokens                    |
| `sc-composer`            | Command bar styling, circle send button, bento suggestions, focus glow |
| `sc-message-list`        | Directional bubbles, user gradient, stagger, hover timestamps          |
| `sc-message-actions`     | Copy feedback, tooltips, blue-tinted glass                             |
| `sc-chat-sessions-panel` | Time groups, inline rename, spring entrance, shadow-lg                 |
| `chat-view.ts`           | HUD status bar, editable title, keyboard hints                         |
| `sc-file-preview`        | Glass cards, shadow depth                                              |
| `sc-thinking`            | Blue-tinted dots, stagger bounce                                       |
| `sc-tool-result`         | Left border accent states, collapse animation                          |

## Success Criteria

1. Dark mode feels like Bloomberg terminal meets luxury automotive — deep, confident, blue-steel
2. Light mode feels like Claude.ai — clean, professional, airy
3. Glass is precious — only 3 surfaces use it
4. Every interactive element has a micro-interaction (not decorative — functional feedback)
5. `prefers-reduced-motion` strips all animation
6. WCAG 2.1 AA contrast on all text
7. All existing 293 unit tests + 60 E2E tests pass
8. No new dependencies
