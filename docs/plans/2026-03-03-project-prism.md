# Project Prism — Implementation Plan

Design: `docs/plans/2026-03-03-project-prism-design.md`

## Phase 1: Foundation (Tokens + Global Styles)

### Task 1: Deep Steel color tokens

**Files:**

- Modify: `design-tokens/color.json` — add Deep Steel dark/light palette
- Modify: `design-tokens/shadow.json` — add layered shadow tokens + shadow-glow
- Create or modify: `design-tokens/glass.json` — glass treatment tokens
- Run: `npm run tokens:build` (or equivalent) to generate CSS

**Changes:**
Add all tokens from design doc Section 1. Ensure `--sc-bg`, `--sc-bg-surface`, `--sc-bg-elevated`, `--sc-border`, `--sc-border-subtle`, `--sc-text`, `--sc-text-muted`, `--sc-accent`, `--sc-accent-hover`, `--sc-accent-subtle`, `--sc-accent-text` are updated for both dark and light themes.

Add shadow tokens: `--sc-shadow-sm`, `--sc-shadow-md`, `--sc-shadow-lg`, `--sc-shadow-glow`.

Add glass tokens: `--sc-glass-sessions-bg`, `--sc-glass-actions-bg`, `--sc-glass-menu-bg`, `--sc-glass-blur`, `--sc-glass-saturate`.

**Test:** `cd ui && npm run typecheck && npm run test && npm run lint`

**Commit:** `style(tokens): add Deep Steel palette, layered shadows, glass tokens`

### Task 2: Global style overrides — scrollbar, focus, body

**Files:**

- Modify: `ui/src/styles/global.css` (or wherever `:root` / `html` styles live)
- Modify: any shared base styles

**Changes:**

- Custom scrollbar: 6px thumb, `--sc-bg-elevated`, transparent track, rounded
- Global focus-visible: `0 0 0 2px var(--sc-accent), 0 0 12px rgba(59,130,246,0.2)`
- Body/html background: `--sc-bg`
- Selection color: accent-subtle background

**Commit:** `style: global scrollbar, focus glow, selection color`

## Phase 2: Composer Overhaul

### Task 3: sc-composer — command bar styling

**Files:**

- Modify: `ui/src/components/sc-composer.ts`

**Changes:**

- Replace flat textarea border with `--sc-bg-surface` container + inner shadow
- Focus state: border → accent + `--sc-shadow-glow` (200ms transition)
- **Send button**: change from text "Send" to 36px circle with Phosphor "arrow-up" icon, solid `--sc-accent` background, white icon
- Send press: `transform: scale(0.95)` on `:active`, spring ease back
- Attach button: ghost style, accent on hover
- Character count: only show when `value.length > 100`
- Stream elapsed: move into the send button area (inline badge)
- Directional border-radius: `--sc-radius-lg` on container

**Test:** Existing sc-composer tests must pass. Add test for circle send button rendering.

**Commit:** `style(composer): command bar design — circle send, focus glow, inner shadow`

### Task 4: sc-composer — bento suggestion grid

**Files:**

- Modify: `ui/src/components/sc-composer.ts`

**Changes:**

- Replace flat prompt pills with 2x2 grid of suggestion cards
- Each card: Phosphor icon + title + description (one line)
- Card styling: `--sc-bg-surface`, 1px border, `--sc-radius`, hover → elevated + shadow-glow
- Above grid: "How can I help?" heading (large, light weight, centered)
- Stagger entrance: `animation-delay: calc(var(--sc-stagger-delay) * var(--idx))`
- `@keyframes sc-card-enter`: translateY(8px) + opacity 0 → translateY(0) + opacity 1

**Suggestions data:**

```typescript
const SUGGESTIONS = [
  {
    icon: "compass",
    title: "Explore the project",
    desc: "Architecture, patterns, and structure",
  },
  {
    icon: "code",
    title: "Write code",
    desc: "Python, scripts, web scrapers, and more",
  },
  {
    icon: "bug",
    title: "Debug an issue",
    desc: "Trace errors and find root causes",
  },
  {
    icon: "question",
    title: "Ask anything",
    desc: "General questions about capabilities",
  },
];
```

**Commit:** `feat(composer): bento suggestion grid with stagger entrance`

## Phase 3: Message List Polish

### Task 5: Message bubbles — gradient, directional radius, hover timestamps

**Files:**

- Modify: `ui/src/components/sc-message-list.ts`

**Changes:**

- User bubbles: `background: linear-gradient(135deg, #1e40af, #2563eb)`, white text
- Assistant bubbles: `--sc-bg-surface`, 1px `--sc-border`
- Directional radius: user bubbles → bottom-right `--sc-radius-sm`, rest `--sc-radius-lg`. Assistant → bottom-left `--sc-radius-sm`, rest `--sc-radius-lg`
- Timestamp: hidden by default, `.message:hover .message-meta` → opacity 1, translateY(0) from translateY(-4px)
- Stagger entrance: `animation-delay: min(calc(50ms * var(--sc-stagger-index)), 300ms)`
- `@keyframes sc-slide-up`: translateY(12px) opacity(0) → translateY(0) opacity(1) with spring easing

**Commit:** `style(messages): gradient user bubbles, directional radius, hover timestamps`

### Task 6: Tool calls — accent border states, collapse animation

**Files:**

- Modify: `ui/src/components/sc-tool-result.ts`

**Changes:**

- Left border: 3px solid, color based on status (blue=running, green=success, red=error)
- Running: `@keyframes sc-pulse-border`: opacity 1 → 0.4 → 1 (1.5s cycle)
- Collapse/expand: `max-height` transition with overflow hidden
- Card: `--sc-bg-surface` background, `--sc-shadow-sm`

**Commit:** `style(tools): accent border states, pulse animation, collapse`

### Task 7: Thinking indicator — blue dots, stagger bounce

**Files:**

- Modify: `ui/src/components/sc-thinking.ts`
- Modify: `ui/src/components/sc-message-list.ts` (thinking container)

**Changes:**

- Three dots: 6px circles, `--sc-accent` color
- `@keyframes sc-dot-bounce`: translateY(0) → translateY(-6px) → translateY(0), 1.2s infinite
- Each dot: `animation-delay: 0ms, 150ms, 300ms`
- "Thinking..." label: `--sc-text-muted`, italic
- Abort button: ghost border, red text + red border on hover

**Commit:** `style(thinking): blue stagger bounce dots`

## Phase 4: Sessions Panel + Actions

### Task 8: Sessions panel — time groups, spring entrance, inline rename

**Files:**

- Modify: `ui/src/components/sc-chat-sessions-panel.ts`

**Changes:**

- Entrance: `transform: translateX(-100%) → translateX(0)` + `opacity: 0 → 1` (spring easing, 300ms)
- Time groups: compute "Today", "Yesterday", "This Week", "Older" from `ts` field
- Section headers: uppercase, `--sc-text-2xs`, `letter-spacing: 0.05em`, `--sc-text-muted`
- Inline rename: double-click session title → contenteditable, blur/Enter to save, fires `sc-session-rename`
- Shadow-lg on right edge for depth separation
- Glass background with blue tint (tokens from Task 1)

**Commit:** `feat(sessions): time groups, inline rename, spring entrance`

### Task 9: Message actions — copy feedback, tooltips

**Files:**

- Modify: `ui/src/components/sc-message-actions.ts`

**Changes:**

- Copy click: swap copy icon → checkmark icon for 1.5s, green tint on button
- Entrance: `translateY(-4px) opacity(0)` → `translateY(0) opacity(1)` (150ms)
- Glass background: use `--sc-glass-actions-bg` token
- Add tooltips: `title` attribute or custom tooltip element with glass styling, 100ms hover delay

**Commit:** `feat(actions): copy feedback animation, glass tooltips`

## Phase 5: Status Bar + Final Polish

### Task 10: Status bar — HUD redesign

**Files:**

- Modify: `ui/src/views/chat-view.ts`

**Changes:**

- Layout: flex with `justify-content: space-between`
- Left: sessions toggle + connection indicator
- Center: session title (editable via click, contenteditable)
- Right: keyboard shortcut hint `Cmd+F`
- Background: transparent (no `--sc-bg-surface`)
- 1px bottom border only

**Commit:** `style(chat): HUD status bar with editable title`

### Task 11: File preview — glass cards, depth

**Files:**

- Modify: `ui/src/components/sc-file-preview.ts`

**Changes:**

- Card background: `--sc-bg-surface` with `--sc-shadow-sm`
- Image thumbnails: `--sc-radius-sm`, slight inner shadow
- Remove button: glass background, red on hover
- Hover: card lifts with `--sc-shadow-md` (translateY(-2px))

**Commit:** `style(files): glass cards with shadow depth`

### Task 12: Final validation + visual regression

**Validation:**

1. `cd ui && npm run typecheck`
2. `cd ui && npm run test` — all 293+ tests pass
3. `cd ui && npm run lint`
4. `cd ui && npx playwright test` — all E2E pass
5. `cd ui && npx playwright test --update-snapshots` — update baselines
6. `cd build && cmake --build . && ./seaclaw_tests` — all C tests pass

**Commit:** `test: update visual baselines for Project Prism`

## Execution

12 tasks, sequential (each builds on previous token/style work).
Estimated: ~1 session with subagent-driven development.
