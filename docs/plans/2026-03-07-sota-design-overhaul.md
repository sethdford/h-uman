# SOTA Design Overhaul — Approved Design

**Direction:** Holo AI-style clean minimalism — precision spacing, subtle gradients, understated elegance. Avenir everywhere. Dark-mode-first.

**Scope:** Full overhaul — every view gets the SOTA treatment.

**Strategy:** Approach C — Design System Upgrade (new composite components) + View Sweep (rebuild every view using the new building blocks).

**Research:** Holo AI (Behance, Nov 2025), JunoMind agent management (Behance, May 2025), OpenClaw glassmorphic dashboard (GitHub, Feb 2026), AI SaaS Dashboard Dark Mode (Behance, Mar 2025). Dribbble glassmorphism stat card patterns. n8n/Make.com/Zapier automation UX patterns.

---

## Phase 1: New Composite Components (Building Blocks)

### 1. `hu-stat-card`

Hero metric card. Every SOTA dashboard leads with numbers.

- **API:** `value` (string/number), `label`, `trend` (string like "+12%"), `trendDirection` ("up"|"down"|"flat"), `progress` (0-1, optional), `accent` ("primary"|"secondary"|"tertiary"|"error").
- **Visual:** Uses `hu-card glass` as base. Oversized number using `hu-animated-number`. Trend badge: green pill for up, red for down, gray for flat. Optional thin progress bar at bottom (2px, accent color). Entrance: `hu-scale-in` with spring easing and stagger delay.
- **Sizing:** Min-width 140px, flex: 1. Responsive: stack 2x2 on mobile.

### 2. `hu-section-header`

Consistent view header replacing ad-hoc h2 elements.

- **API:** `heading`, `description` (optional). Default slot for right-aligned actions.
- **Visual:** Heading: `--hu-headline-lg`, `--hu-weight-semibold`. Description: `--hu-body-md`, `--hu-text-muted`. Flex row with `justify-content: space-between`. Bottom margin: `--hu-space-xl`.

### 3. `hu-metric-row`

Compact horizontal stats for secondary metrics.

- **API:** `items` array of `{ label: string, value: string, accent?: string }`.
- **Visual:** Flex row, items separated by `1px solid --hu-border-subtle` vertical dividers. Label in `--hu-text-xs` muted, value in `--hu-text-sm` `--hu-weight-medium`. Gap: `--hu-space-lg`.

### 4. `hu-timeline`

Upgraded activity feed with vertical dot-line timeline.

- **API:** `items` array of `{ time: string, message: string, status: "success"|"error"|"info"|"pending", detail?: string }`.
- **Visual:** Vertical line (1px `--hu-border-subtle`) on the left. Dot markers: 8px circles colored by status (green success, red error, blue info, gray pending). Time in `--hu-text-xs` muted, message in `--hu-text-sm`. Entrance: cascade animation, 50ms stagger per item.

### 5. `hu-sparkline-enhanced`

Upgraded sparkline with hover tooltips and gradient fill.

- **API:** Extends existing `hu-sparkline`. Add `showTooltip` (boolean), `fillGradient` (boolean), `dotSize` (number).
- **Visual:** SVG line with gradient fill (accent color at 20% → transparent). Dot on current value. Tooltip on hover: glass pill showing exact value + date. Responsive height.

### 6. `hu-page-hero`

Premium landing section for each view. Sets the Holo tone.

- **API:** Default slot for content (typically `hu-section-header` + `hu-stat-card` row).
- **Visual:** Background: CSS `radial-gradient` mesh using the current accent color at 5-8% opacity over `--hu-bg`. Creates a warm, subtle glow. Padding: `--hu-space-2xl` top/bottom. `prefers-reduced-motion`: static background, no animation. Bottom: `1px solid --hu-border-subtle` fade-out.

### Motion & Choreography Wiring

Wire existing but underused tokens into a consistent system:

- **Page entrance:** Views use `hu-page-hero` with `hu-fade-in` + `hu-slide-up` (200ms).
- **Card stagger:** All card grids use `.hu-stagger` with `--hu-choreography-stagger` delay.
- **Stat pop:** `hu-stat-card` uses `hu-scale-in` with `--hu-spring-micro`.
- **Card hover:** Cards use `--hu-motion-card-hover` (translate-y -2px + shadow lift).
- **Button press:** All buttons use `--hu-motion-button-press` (scale 0.97).
- **Reduced motion:** Everything respects `prefers-reduced-motion: reduce`.

### Typography

Avenir is the canonical typeface. No Google Fonts. No font changes. System monospace via `--hu-font-mono` for code contexts only.

---

## Phase 2: View-by-View Sweep

### Chat View — "Better Than iMessage"

The most-used view. Complete redesign.

#### Message Bubbles

- **User messages:** Teal gradient bubble. `--hu-accent` at 85% opacity start, `--hu-accent-hover` end. `border-radius: 20px 20px 6px 20px` (tight bottom-right = "tail" effect, iMessage style). `color: var(--on-accent-text, #fff)`. Max-width: 75%. Right-aligned.
- **Assistant messages:** Glass card. `--hu-glass-subtle` (translucent bg, 12px blur, 1px border at 8% white). `border-radius: 20px 20px 20px 6px` (tight bottom-left). Text: `--hu-text`. Max-width: 85% (wider for code/markdown). Left-aligned.
- **Message spacing:** `--hu-space-lg` between different-sender groups. `--hu-space-xs` between consecutive same-sender messages.

#### Avatars

- **Assistant:** 28px Human wave logo mark in `--hu-accent`, left-aligned above first message in a group.
- **User:** 28px circle with user initials, `--hu-accent-subtle` background, right-aligned above first message in a group.
- Only show on the first message of a consecutive group.

#### Timestamps

- Centered muted timestamp ("Today 10:32 AM") between groups separated by >5 minutes.
- Font: `--hu-text-xs`, `--hu-text-faint`.

#### Message Actions

- Hidden by default. Appear on hover with `hu-fade-in` (100ms), positioned as a floating glass pill above the message.
- Actions: copy, retry, edit (if user message).
- Glass background: `--hu-glass-subtle`.
- Icon buttons: 24px, `--hu-text-muted` → `--hu-text` on hover.

#### Streaming / Typing

- While streaming: `hu-thinking` dots animation, then streaming message with pulsing cursor.
- Cursor: 2px wide, `--hu-accent` color, `hu-pulse` animation.
- New message entrance: `hu-slide-up` 150ms with `--hu-ease-out`. Cascade: 30ms stagger between consecutive messages.

#### Composer

- Remove "0 characters" counter.
- Glass background: `--hu-glass-subtle` on the input area.
- `border-radius: --hu-radius-xl` (16px).
- Send button: circular (40px), `--hu-accent` fill, white arrow-up icon (Phosphor `arrow-up`). Disabled: 40% opacity. Press: `--hu-motion-button-press`.
- Focus glow: `box-shadow: 0 0 0 2px var(--hu-accent-subtle)`.
- File attachment button: ghost icon button, left of send.

#### Code Blocks in Messages

- Inset background: `--hu-bg-inset`, `--hu-radius-md` corners.
- Copy button: appears top-right on hover, glass pill.
- Font: `--hu-font-mono`.

#### Sessions Panel

- Side panel gets glass treatment: `--hu-glass-standard`.
- Session items: subtle hover state, active session highlighted with `--hu-accent-subtle` left border.

---

### Overview View — Mission Control

- **`hu-page-hero`** wrapper with radial gradient mesh.
- **`hu-section-header`:** "Overview" / "Your AI assistant at a glance" + refresh button showing staleness.
- **4 `hu-stat-card`s:** Channels (33), Tools (60+), Uptime (24/7 or actual), Peak RSS (5.9 MB). Animated entrance.
- **`hu-metric-row`:** Sessions today, messages today, avg response time, cost today.
- **`hu-timeline`:** Last 10 events across all channels.
- **`hu-sparkline-enhanced` card:** Messages over last 7 days, hover tooltips.
- **Quick actions row:** 3 glass cards (Chat, New Automation, Voice) with icons and subtle hover lift.

---

### Automations View — Polish Pass

- **`hu-page-hero`** wrapper.
- **`hu-section-header`:** "Automations" / "Manage scheduled agent tasks and shell jobs" + New Automation button.
- **4 `hu-stat-card`s** replacing plain stats bar: Total, Active, Paused, Failed.
- **`hu-metric-row`:** Runs today, success rate, next scheduled, avg duration.
- Automation cards: add `--hu-motion-card-hover` lift.
- Templates section: responsive CSS grid, `hu-card clickable glass`.

---

### Agents View

- **`hu-page-hero`** + `hu-section-header`: "Agents" / "Monitor autonomous agent instances".
- **`hu-stat-card`s:** Active agents, total turns, avg confidence, active automations.
- Agent cards: status badges (idle = gray, running = green pulse, error = red). Glass card style.

### Models View

- **`hu-page-hero`** + `hu-section-header`.
- **`hu-stat-card`s:** Provider count, default model, total tokens, total cost.
- Model cards: cost-per-1K badge, capability tags. Glass card.

### Sessions View

- **`hu-page-hero`** + `hu-section-header`.
- **`hu-stat-card`s:** Active sessions, messages today, avg duration.
- Session cards: `hu-sparkline-enhanced` mini chart showing message count over session.

### Config View

- **`hu-page-hero`** + `hu-section-header`.
- Add loading skeleton (currently missing).
- Form groups wrapped in glass cards for visual grouping.
- Save status badge: "Saved" (green), "Unsaved changes" (amber), "Error" (red).

### Tools View

- **`hu-page-hero`** + `hu-section-header`.
- **`hu-stat-card`s:** Total tools, enabled, categories, most used.
- Tool cards: category badge, usage count. Glass card.

### Channels View

- **`hu-page-hero`** + `hu-section-header`.
- **`hu-stat-card`s:** Total channels, configured, active, messages today.
- Channel cards: connection status badge with color (green connected, amber pending, red error).

### Skills View

- **`hu-page-hero`** + `hu-section-header`.
- Skill cards: "Installed" (green badge) vs "Available" (gray). Glass card.

### Voice View

- Already has nice animations. Add `hu-page-hero` and `hu-stat-card`s (sessions, total duration, avg confidence).

### Usage View

- **`hu-page-hero`** + `hu-section-header`.
- **`hu-stat-card`s:** Tokens today, cost today, requests today, avg cost/request.
- Charts: upgrade to `hu-sparkline-enhanced` with gradient fills and hover tooltips.

### Security View

- **`hu-page-hero`** + `hu-section-header`.
- Add loading skeleton (currently just "Loading..." text).
- Traffic-light `hu-stat-card`s: pairing status (green/red), sandbox status, HTTPS enforcement, policy status.

### Nodes View

- **`hu-page-hero`** + `hu-section-header`.
- Add loading skeleton (currently just "Loading..." text).
- Node cards: connection pulse animation, latency badge.

### Logs View

- **`hu-page-hero`** + `hu-section-header`.
- Add empty state (currently missing).
- Log entries: `hu-timeline` treatment with severity-colored dots (debug=gray, info=blue, warn=amber, error=red).

---

## Implementation Order

### Phase 1: Composite Components (Tasks 1-7)

1. `hu-stat-card`
2. `hu-section-header`
3. `hu-metric-row`
4. `hu-timeline`
5. `hu-sparkline-enhanced`
6. `hu-page-hero`
7. Wire motion choreography globally (stagger, hover, press in `theme.css`)

### Phase 2: View Sweep (Tasks 8-22)

8. **Chat view** (highest priority — complete redesign)
9. Overview view
10. Automations view (polish pass)
11. Agents view
12. Models view
13. Sessions view
14. Config view
15. Tools view
16. Channels view
17. Skills view
18. Voice view (light touch)
19. Usage view
20. Security view
21. Nodes view
22. Logs view

### Phase 3: Verification (Task 23)

23. Build verification, lint check, visual review
