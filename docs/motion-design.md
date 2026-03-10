---
title: Human Motion Design System
---

# Human Motion Design System

> Normative reference for all animation and motion in Human UI surfaces.
> Every animation must conform to the principles, tokens, and choreography rules below.
> Agents must consult this document before adding or modifying any animation.

## SOTA References

This system synthesizes the best from three animation traditions:

- **Disney/Pixar 12 Principles** — The foundational animation laws (Johnston & Thomas, 1981)
- **Apple Human Interface Guidelines** — Spring-first physics, functional motion, spatial continuity
- **Material Design 3 Motion** — Easing taxonomy, duration tokens, transition patterns

---

## 1. Foundational Principles

### The Disney/Pixar Principles Applied to UI

These twelve principles were developed for character animation but apply directly to interface
motion. Each principle below includes its UI translation and Human implementation.

#### 1.1 Squash & Stretch — Weight and Elasticity

Objects deform under force, conveying weight and material.

**UI Translation**: Interactive elements compress on press and rebound on release.

```css
/* Button press — slight vertical compression */
.btn:active:not(:disabled) {
  transform: translateY(1px) scaleY(0.97) scaleX(1.01);
}
```

Rules:

- Volume must remain constant (scaleY down → scaleX up slightly)
- Maximum deformation: 3–5% (subtle, not cartoonish)
- Use `--hu-ease-spring` for the rebound
- Only on direct-manipulation elements (buttons, toggles, draggable items)

#### 1.2 Anticipation — Preparing for Action

A small preparatory motion before the main action.

**UI Translation**: Hover states, pre-expansion hints, pull-to-refresh resistance.

```css
/* Card lifts slightly before expanding */
.card:hover {
  transform: translateY(-2px);
  box-shadow: var(--hu-shadow-md);
  transition:
    transform var(--hu-duration-fast) var(--hu-ease-out),
    box-shadow var(--hu-duration-fast) var(--hu-ease-out);
}
```

Rules:

- Anticipation duration: `--hu-duration-fast` (100ms) or less
- Movement distance: 1–4px maximum
- Purpose: signal that something will happen, not distract

#### 1.3 Staging — Directing Attention

Present one idea at a time. The most important element animates first.

**UI Translation**: Staggered reveals, dimmed backgrounds, focal animation.

Rules:

- When multiple elements enter, stagger by `--hu-stagger-delay` (50ms)
- Cap total stagger at `--hu-stagger-max` (300ms) — never let a sequence take >300ms to start
- Modal backgrounds dim to focus attention on the dialog
- Only one element should be animating prominently at any time
- Use `--hu-cascade-delay` (30ms) for nested child elements within a parent

#### 1.4 Straight-Ahead vs. Pose-to-Pose — CSS Implementation

**UI Translation**: CSS transitions are pose-to-pose (start state → end state). Keyframe
animations can be straight-ahead (frame by frame).

Rules:

- Prefer CSS transitions for state changes (hover, active, focus, disabled)
- Use `@keyframes` for complex multi-step sequences (loading shimmer, orb pulse)
- All `@keyframes` names prefixed with `hu-` (e.g., `hu-fade-in`, `hu-slide-up`)
- Never use JavaScript `setTimeout` or `setInterval` for animation timing

#### 1.5 Follow-Through & Overlapping Action — Momentum

Elements don't stop abruptly. Child elements continue briefly after parent stops.

**UI Translation**: Spring overshoot, staggered child completion, elastic settling.

```css
/* Parent panel slides in; children stagger behind */
.panel {
  animation: hu-slide-in var(--hu-duration-moderate)
    var(--hu-ease-spring-gentle);
}
.panel .child {
  animation: hu-fade-up var(--hu-duration-normal) var(--hu-ease-out);
  animation-delay: calc(var(--hu-cascade-delay) * var(--hu-child-index, 0));
}
```

Rules:

- Spring easings (`--hu-ease-spring`, `--hu-ease-spring-gentle`) naturally provide overshoot
- Child elements should complete animation 50–150ms after parent
- Maximum 3 levels of cascade depth

#### 1.6 Ease In / Ease Out — Natural Acceleration

Nothing in nature starts or stops instantly. All motion accelerates and decelerates.

**UI Translation**: The easing token hierarchy.

| Motion Type | Token                     | Curve                               | When to Use                        |
| ----------- | ------------------------- | ----------------------------------- | ---------------------------------- |
| Enter       | `--hu-ease-out`           | `cubic-bezier(0.16, 1, 0.3, 1)`     | Elements appearing on screen       |
| Exit        | `--hu-ease-in`            | `cubic-bezier(0.55, 0, 1, 0.45)`    | Elements leaving screen            |
| Move        | `--hu-ease-in-out`        | `cubic-bezier(0.65, 0, 0.35, 1)`    | Elements repositioning             |
| Interact    | `--hu-ease-spring`        | `cubic-bezier(0.34, 1.56, 0.64, 1)` | Direct manipulation response       |
| Gentle      | `--hu-ease-spring-gentle` | `cubic-bezier(0.22, 1.2, 0.36, 1)`  | Subtle interactive feedback        |
| Emphasis    | `--hu-emphasize`          | `cubic-bezier(0.2, 0, 0, 1)`        | Dramatic deceleration (M3-derived) |

Rules:

- **NEVER** use `linear`, `ease`, `ease-in-out` keywords — always use tokens
- **NEVER** write raw `cubic-bezier()` values — always reference the token
- Enter uses ease-out (fast start, gentle landing)
- Exit uses ease-in (gentle start, fast departure)
- Repositioning uses ease-in-out (symmetric)

#### 1.7 Arc — Curved Motion Paths

Natural motion follows curved paths, not straight lines.

**UI Translation**: Elements moving across the screen should follow slight curves.

Rules:

- For simple vertical/horizontal transitions, straight lines are acceptable
- For complex repositioning (list reorder, element swap), use curved paths
- CSS: combine X and Y translations with different easings to create implicit arcs
- Avoid abrupt direction changes

#### 1.8 Secondary Action — Supporting Motion

Additional motion that supports the primary action without competing.

**UI Translation**: Background dimming during modal open, subtle icon rotation on expand.

Rules:

- Secondary animations use reduced opacity or scale compared to primary
- Secondary animations should be removable without losing meaning
- Background animations (ambient drift, gradients) are always secondary
- Never let secondary animation distract from primary content

#### 1.9 Timing — Duration as Meaning

Speed communicates urgency, importance, and distance.

**UI Translation**: The duration token scale.

| Token                    | Value | Semantic              | Example                        |
| ------------------------ | ----- | --------------------- | ------------------------------ |
| `--hu-duration-instant`  | 50ms  | Imperceptible         | Color change, opacity flip     |
| `--hu-duration-fast`     | 100ms | Micro-interaction     | Button state, toggle, hover    |
| `--hu-duration-normal`   | 200ms | Standard transition   | Panel slide, fade in           |
| `--hu-duration-moderate` | 300ms | Deliberate transition | View change, modal open        |
| `--hu-duration-slow`     | 350ms | Complex transition    | Multi-element choreography     |
| `--hu-duration-slower`   | 500ms | Dramatic reveal       | Hero entrance, page transition |
| `--hu-duration-slowest`  | 700ms | Epic                  | Full-screen transition         |

**Material 3 Alignment**: Our scale maps to M3's Short (50–100ms), Medium (200–300ms),
Long (350–500ms), Extra-Long (700ms+) categories.

Rules:

- Small elements animate faster than large elements
- Entering is slightly slower than exiting (enter: `normal`, exit: `fast`)
- Distance correlates with duration — longer travel = longer duration
- Never exceed 700ms for any single animation
- Loading indicators are exempt from max duration (they loop)

#### 1.10 Exaggeration — Emphasis Without Distortion

Amplify motion just enough to be clear, not enough to be absurd.

**UI Translation**: Spring overshoot, scale emphasis, glow effects.

Rules:

- Maximum spring overshoot: 15% (captured in `--hu-ease-spring`)
- Maximum scale emphasis: 1.05x (5% larger)
- Glow effects (`--hu-shadow-glow-accent`) are exaggeration — use sparingly
- Subtle > dramatic. If in doubt, reduce.

#### 1.11 Solid Drawing — Consistent 3D Space

Maintain consistent perspective and dimensionality.

**UI Translation**: Consistent shadow direction, perspective origin, transform axis.

Rules:

- Shadows always cast downward (light source from top)
- `perspective` uses consistent origin across a view
- Cards and panels exist on the same conceptual plane
- `translateZ` changes correspond to shadow changes

#### 1.12 Appeal — Aesthetic Quality

Motion should feel delightful, not just functional.

**UI Translation**: Polish, refinement, personality in micro-interactions.

Rules:

- Every transition should feel intentional, not accidental
- Ambient animations (gradient drift, orb glow) add life to static screens
- Micro-interactions (button press, toggle snap) convey craft quality
- Appeal must not compromise performance or accessibility

---

## 2. Spring Physics System (Apple HIG)

### Philosophy

Apple's WWDC23 "Animate with springs" established that **spring animations should be the
default for all interactive motion**. Springs guarantee velocity continuity when interrupted,
making them inherently more responsive than duration-based animations.

Human follows this principle. Spring presets are the primary animation tool.

### Spring Presets

| Token                    | Stiffness | Damping | Use Case                        |
| ------------------------ | --------- | ------- | ------------------------------- |
| `--hu-spring-micro`      | High      | High    | Buttons, toggles, checkboxes    |
| `--hu-spring-standard`   | Medium    | Medium  | Panels, dropdowns, cards        |
| `--hu-spring-expressive` | Low       | Medium  | Page transitions, view switches |
| `--hu-spring-dramatic`   | Low       | Low     | Hero reveals, modal entrances   |

### CSS Spring Approximation

CSS cannot natively express spring physics. We use `linear()` easing functions that
approximate spring curves, defined in `design-tokens/motion.tokens.json`.

```css
/* Example: spring-standard approximation */
--hu-spring-standard: linear(
  0,
  0.009,
  0.037,
  0.078,
  0.129,
  0.189,
  0.254,
  0.321,
  0.389,
  0.455,
  0.519,
  0.579,
  0.635,
  0.686,
  0.732,
  0.773,
  0.81,
  0.842,
  0.869,
  0.893,
  0.913,
  0.93,
  0.944,
  0.955,
  0.964,
  0.971,
  0.977,
  0.982,
  0.986,
  0.989,
  0.991,
  0.993,
  0.995,
  0.996,
  0.997,
  0.998,
  0.999,
  0.999,
  1
);
```

### Native Platform Springs

| Platform | API                                           | Parameters                       |
| -------- | --------------------------------------------- | -------------------------------- |
| SwiftUI  | `Animation.spring(response:dampingFraction:)` | Map from token stiffness/damping |
| Android  | `MotionScheme.standard/expressive`            | Use M3 motion scheme             |
| CSS      | `linear()` approximation                      | Pre-computed in tokens           |

### When NOT to Use Springs

- Loading indicators (use steady linear or ease-in-out loop)
- Progress bars (use linear interpolation)
- Scroll behavior (browser-native smooth scroll)
- Color transitions (use `--hu-duration-fast` + `--hu-ease-out`)

---

## 3. Transition Patterns

### 3.1 View Transitions

When navigating between views (tab change, route change):

```css
@keyframes hu-view-enter {
  from {
    opacity: 0;
    transform: translateY(8px) scale(0.995);
  }
  to {
    opacity: 1;
    transform: translateY(0) scale(1);
  }
}

.view-enter {
  animation: hu-view-enter var(--hu-duration-moderate) var(--hu-spring-out) both;
}
```

- Duration: `--hu-duration-moderate` (300ms)
- Easing: spring-out for natural deceleration
- Direction: slide up (8px) + scale (0.995→1) + fade
- Reduced motion: `animation: none`

### 3.2 Modal/Sheet Transitions

```
Enter: scale(0.95) + opacity(0) → scale(1) + opacity(1)
Exit:  scale(1) + opacity(1) → scale(0.95) + opacity(0)
Backdrop: opacity(0) → opacity(1) simultaneous with content
```

- Duration: `--hu-duration-moderate` enter, `--hu-duration-normal` exit
- Easing: `--hu-ease-spring-gentle` enter, `--hu-ease-in` exit
- Focus trap activates after enter animation completes

### 3.3 List Item Transitions

For items entering/leaving a list (messages, logs, cards):

```
Enter: translateY(8px) + opacity(0) → translateY(0) + opacity(1)
Exit:  translateY(0) + opacity(1) → translateY(-4px) + opacity(0)
Stagger: 50ms between items (--hu-stagger-delay)
```

- Duration: `--hu-duration-normal`
- Easing: `--hu-ease-out` for enter, `--hu-ease-in` for exit
- Maximum stagger: 300ms total (`--hu-stagger-max`)

### 3.4 Skeleton → Content Transition

```
Skeleton shimmer runs continuously
On data load: skeleton fades out (--hu-duration-fast)
Content fades in (--hu-duration-normal)
```

- Cross-fade, not sequential (skeleton and content overlap briefly)
- Content enters with slight translateY(4px) for subtle lift effect

### 3.5 State Transitions

For interactive state changes (hover, active, focus, disabled):

| State    | Properties                    | Duration                | Easing             |
| -------- | ----------------------------- | ----------------------- | ------------------ |
| Hover    | background, shadow, transform | `--hu-duration-fast`    | `--hu-ease-out`    |
| Active   | transform, shadow             | `--hu-duration-instant` | `--hu-ease-spring` |
| Focus    | outline (focus ring)          | `--hu-duration-fast`    | `--hu-ease-out`    |
| Disabled | opacity                       | `--hu-duration-fast`    | `--hu-ease-out`    |
| Error    | border-color, shadow          | `--hu-duration-fast`    | `--hu-ease-out`    |

---

## 4. Choreography Rules

### 4.1 Stagger Sequences

When multiple elements enter simultaneously, stagger prevents cognitive overload.

```css
.card {
  animation: hu-fade-up var(--hu-duration-normal) var(--hu-ease-out) both;
  animation-delay: calc(var(--hu-stagger-delay) * var(--hu-stagger-index, 0));
}
```

- `--hu-stagger-delay`: 50ms between items
- `--hu-stagger-max`: 300ms cap (item 7+ all start at 300ms)
- `--hu-cascade-delay`: 30ms for nested children within a parent
- Maximum cascade depth: 3 levels

### 4.2 Entrance Choreography

For a page/view entering:

1. **Container** enters first (background, border establish context)
2. **Primary content** enters next (title, main body)
3. **Secondary content** follows (metadata, actions, decorative elements)
4. **Ambient effects** start last (gradients, glows, background animation)

Gap between steps: `--hu-cascade-delay` (30ms)

### 4.3 Coordinated Motion (M3: Container Transform)

When an element transforms into another (e.g., card → detail view):

- Shared elements morph (position, size, border-radius)
- Non-shared elements of origin fade out
- Non-shared elements of destination fade in
- Total duration: `--hu-duration-slow`
- Use `view-transition-name` for CSS View Transitions API where supported

---

## 5. Performance Contract

### 5.1 Compositor-Only Properties

Animations must only animate compositor-friendly properties:

**Allowed** (GPU-accelerated):

- `transform` (translate, scale, rotate)
- `opacity`
- `filter` (drop-shadow, blur)
- `clip-path`

**Prohibited** (triggers layout/paint):

- `width`, `height`, `top`, `left`, `right`, `bottom`
- `margin`, `padding`
- `border-width`, `border-radius` (animate `clip-path` instead)
- `font-size`
- `box-shadow` (use `filter: drop-shadow()` for animated shadows)

Exception: `box-shadow` transitions are acceptable for hover states where the duration
is ≤100ms and the element count is ≤10.

### 5.2 will-change

- Apply `will-change` only to elements that will actually animate
- Remove `will-change` after animation completes (or use `animation-fill-mode`)
- Never apply `will-change` to more than 10 elements simultaneously
- Prefer `transform: translateZ(0)` for layer promotion over `will-change`

### 5.3 Frame Budget

- All animations must maintain 60fps (16.67ms frame budget)
- Test on lowest-spec target device (not just development machine)
- If animation causes jank, simplify or remove — function over form
- Use `content-visibility: auto` for off-screen animated elements

---

## 6. Reduced Motion

### Implementation

```css
@media (prefers-reduced-motion: reduce) {
  *,
  *::before,
  *::after {
    animation-duration: 0.01ms !important;
    animation-iteration-count: 1 !important;
    transition-duration: 0.01ms !important;
    scroll-behavior: auto !important;
  }
}
```

Human's token pipeline handles this globally in `theme.css`. Components using token-based
durations inherit reduced motion automatically.

### Rules

- Never convey essential information through animation alone
- Provide static alternatives for all animated content
- Loading states must work without animation (skeleton shapes visible without shimmer)
- Focus rings must be visible without transition animation
- Auto-playing ambient animations pause under reduced motion

## 6.5 Category-Defining Motion Capabilities

These capabilities push beyond industry standards. They define the motion quality ceiling
that other developer tools measure against.

### Real Spring Physics

Move beyond cubic-bezier approximations. Use `linear()` with 60+ keyframes for true damped
harmonic oscillator curves. Springs must be indistinguishable from native iOS/macOS animations.

Implementation:

- Generate `linear()` curves from spring parameters (mass, stiffness, damping) at build time
- 60+ steps per curve for smooth 60fps playback
- Store curves in `design-tokens/motion.tokens.json` alongside cubic-bezier fallbacks
- Use Web Animations API `spring()` timing function when browsers ship it
- Test: record screen at 120fps, compare frame-by-frame with native SwiftUI spring

### Scroll-Driven Narratives

CSS `scroll-timeline` and `view-timeline` enable animation driven by scroll position rather
than time. The marketing website should rival Awwwards winners for scroll experience.

Patterns:

- **Parallax headers**: Background layers move at different scroll rates using `scroll-timeline`
- **Progressive reveal**: Elements fade in and translate as they enter viewport using `view-timeline`
- **Section entrance choreography**: Staggered element entrance triggered by scroll position
- **Progress indicators**: Reading progress bars driven by scroll position

Rules:

- Always provide a static fallback for browsers without `scroll-timeline` support
- Never hijack scroll behavior — scroll-driven animations enhance, not replace, natural scrolling
- Performance: scroll-driven animations run on the compositor thread — keep them to transform/opacity
- Reduced motion: replace with immediate visibility (no scroll-triggered animation)

### Ambient Intelligence

Subtle motion that responds to environmental context. Not decoration — communication
through motion.

Patterns:

- **Glass blur density**: Backdrop blur intensity shifts subtly with scroll depth
- **Gradient response**: Background gradients shift hue slightly based on pointer proximity
- **Status breathing**: Status indicator pulse rate correlates with system health metrics
- **Time-aware theming**: Subtle color temperature shift based on time of day

Rules:

- Ambient effects must be imperceptible as individual changes — only the cumulative effect is felt
- CPU budget: ambient animations must use <1% CPU when idle
- All ambient effects disabled under `prefers-reduced-motion: reduce`
- Never animate ambient effects on mobile (battery impact)

### Transition Orchestration

View Transitions API enables seamless cross-route morphs. Elements have spatial memory —
a card that expands to detail view morphs, never fade-and-replaces.

Implementation:

- Assign `view-transition-name` to persistent elements (nav, sidebar, selected card)
- Shared elements morph between views (position, size, border-radius)
- Non-shared elements crossfade with stagger
- Duration: `--hu-duration-moderate` (300ms) with `--hu-ease-spring-gentle`
- Fallback: standard fade transition for browsers without View Transitions API

Rules:

- Maximum 5 elements with `view-transition-name` per view (performance)
- Morphing elements must have compatible aspect ratios (no extreme stretching)
- Test with network throttling — transitions must degrade gracefully on slow connections
- `::view-transition-*` pseudo-elements use Human easing tokens, not raw curves

### Narrative Motion

Animation that tells a story. Staggered data reveals that build understanding
incrementally. Inspired by Spotify Wrapped.

Patterns:

- **Data cascade**: Chart data points appear sequentially, building the picture over 500ms
- **Metric reveal**: Hero numbers count up from 0 to value with deceleration curve
- **Timeline playback**: Historical data animates through time, showing progression
- **Connection drawing**: Lines between related elements draw on as relationships become visible

Rules:

- Narrative sequences must be skippable (click to complete instantly)
- Total narrative duration: max 2 seconds for any sequence
- Each step must be meaningful — remove any step that doesn't add understanding
- Reduced motion: show final state immediately, no animation

---

## 7. Cross-Reference

| Document             | Covers                                          |
| -------------------- | ----------------------------------------------- |
| `design-strategy.md` | Token values, color, typography, breakpoints    |
| `design-system.md`   | Component API, platform-specific implementation |
| `ux-patterns.md`     | Layout archetypes, interaction patterns         |
| `AGENTS.md` §12.4    | Enforcement rules for agents                    |
| ADR-0005             | Spring physics decision rationale               |
