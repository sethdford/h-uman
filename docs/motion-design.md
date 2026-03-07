# SeaClaw Motion Design System

> Normative reference for all animation and motion in SeaClaw UI surfaces.
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
motion. Each principle below includes its UI translation and SeaClaw implementation.

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
- Use `--sc-ease-spring` for the rebound
- Only on direct-manipulation elements (buttons, toggles, draggable items)

#### 1.2 Anticipation — Preparing for Action

A small preparatory motion before the main action.

**UI Translation**: Hover states, pre-expansion hints, pull-to-refresh resistance.

```css
/* Card lifts slightly before expanding */
.card:hover {
  transform: translateY(-2px);
  box-shadow: var(--sc-shadow-md);
  transition:
    transform var(--sc-duration-fast) var(--sc-ease-out),
    box-shadow var(--sc-duration-fast) var(--sc-ease-out);
}
```

Rules:

- Anticipation duration: `--sc-duration-fast` (100ms) or less
- Movement distance: 1–4px maximum
- Purpose: signal that something will happen, not distract

#### 1.3 Staging — Directing Attention

Present one idea at a time. The most important element animates first.

**UI Translation**: Staggered reveals, dimmed backgrounds, focal animation.

Rules:

- When multiple elements enter, stagger by `--sc-stagger-delay` (50ms)
- Cap total stagger at `--sc-stagger-max` (300ms) — never let a sequence take >300ms to start
- Modal backgrounds dim to focus attention on the dialog
- Only one element should be animating prominently at any time
- Use `--sc-cascade-delay` (30ms) for nested child elements within a parent

#### 1.4 Straight-Ahead vs. Pose-to-Pose — CSS Implementation

**UI Translation**: CSS transitions are pose-to-pose (start state → end state). Keyframe
animations can be straight-ahead (frame by frame).

Rules:

- Prefer CSS transitions for state changes (hover, active, focus, disabled)
- Use `@keyframes` for complex multi-step sequences (loading shimmer, orb pulse)
- All `@keyframes` names prefixed with `sc-` (e.g., `sc-fade-in`, `sc-slide-up`)
- Never use JavaScript `setTimeout` or `setInterval` for animation timing

#### 1.5 Follow-Through & Overlapping Action — Momentum

Elements don't stop abruptly. Child elements continue briefly after parent stops.

**UI Translation**: Spring overshoot, staggered child completion, elastic settling.

```css
/* Parent panel slides in; children stagger behind */
.panel {
  animation: sc-slide-in var(--sc-duration-moderate)
    var(--sc-ease-spring-gentle);
}
.panel .child {
  animation: sc-fade-up var(--sc-duration-normal) var(--sc-ease-out);
  animation-delay: calc(var(--sc-cascade-delay) * var(--sc-child-index, 0));
}
```

Rules:

- Spring easings (`--sc-ease-spring`, `--sc-ease-spring-gentle`) naturally provide overshoot
- Child elements should complete animation 50–150ms after parent
- Maximum 3 levels of cascade depth

#### 1.6 Ease In / Ease Out — Natural Acceleration

Nothing in nature starts or stops instantly. All motion accelerates and decelerates.

**UI Translation**: The easing token hierarchy.

| Motion Type | Token                     | Curve                               | When to Use                        |
| ----------- | ------------------------- | ----------------------------------- | ---------------------------------- |
| Enter       | `--sc-ease-out`           | `cubic-bezier(0.16, 1, 0.3, 1)`     | Elements appearing on screen       |
| Exit        | `--sc-ease-in`            | `cubic-bezier(0.55, 0, 1, 0.45)`    | Elements leaving screen            |
| Move        | `--sc-ease-in-out`        | `cubic-bezier(0.65, 0, 0.35, 1)`    | Elements repositioning             |
| Interact    | `--sc-ease-spring`        | `cubic-bezier(0.34, 1.56, 0.64, 1)` | Direct manipulation response       |
| Gentle      | `--sc-ease-spring-gentle` | `cubic-bezier(0.22, 1.2, 0.36, 1)`  | Subtle interactive feedback        |
| Emphasis    | `--sc-emphasize`          | `cubic-bezier(0.2, 0, 0, 1)`        | Dramatic deceleration (M3-derived) |

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
| `--sc-duration-instant`  | 50ms  | Imperceptible         | Color change, opacity flip     |
| `--sc-duration-fast`     | 100ms | Micro-interaction     | Button state, toggle, hover    |
| `--sc-duration-normal`   | 200ms | Standard transition   | Panel slide, fade in           |
| `--sc-duration-moderate` | 300ms | Deliberate transition | View change, modal open        |
| `--sc-duration-slow`     | 350ms | Complex transition    | Multi-element choreography     |
| `--sc-duration-slower`   | 500ms | Dramatic reveal       | Hero entrance, page transition |
| `--sc-duration-slowest`  | 700ms | Epic                  | Full-screen transition         |

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

- Maximum spring overshoot: 15% (captured in `--sc-ease-spring`)
- Maximum scale emphasis: 1.05x (5% larger)
- Glow effects (`--sc-shadow-glow-accent`) are exaggeration — use sparingly
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

SeaClaw follows this principle. Spring presets are the primary animation tool.

### Spring Presets

| Token                    | Stiffness | Damping | Use Case                        |
| ------------------------ | --------- | ------- | ------------------------------- |
| `--sc-spring-micro`      | High      | High    | Buttons, toggles, checkboxes    |
| `--sc-spring-standard`   | Medium    | Medium  | Panels, dropdowns, cards        |
| `--sc-spring-expressive` | Low       | Medium  | Page transitions, view switches |
| `--sc-spring-dramatic`   | Low       | Low     | Hero reveals, modal entrances   |

### CSS Spring Approximation

CSS cannot natively express spring physics. We use `linear()` easing functions that
approximate spring curves, defined in `design-tokens/motion.tokens.json`.

```css
/* Example: spring-standard approximation */
--sc-spring-standard: linear(
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
- Color transitions (use `--sc-duration-fast` + `--sc-ease-out`)

---

## 3. Transition Patterns

### 3.1 View Transitions

When navigating between views (tab change, route change):

```css
@keyframes sc-view-enter {
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
  animation: sc-view-enter var(--sc-duration-moderate) var(--sc-spring-out) both;
}
```

- Duration: `--sc-duration-moderate` (300ms)
- Easing: spring-out for natural deceleration
- Direction: slide up (8px) + scale (0.995→1) + fade
- Reduced motion: `animation: none`

### 3.2 Modal/Sheet Transitions

```
Enter: scale(0.95) + opacity(0) → scale(1) + opacity(1)
Exit:  scale(1) + opacity(1) → scale(0.95) + opacity(0)
Backdrop: opacity(0) → opacity(1) simultaneous with content
```

- Duration: `--sc-duration-moderate` enter, `--sc-duration-normal` exit
- Easing: `--sc-ease-spring-gentle` enter, `--sc-ease-in` exit
- Focus trap activates after enter animation completes

### 3.3 List Item Transitions

For items entering/leaving a list (messages, logs, cards):

```
Enter: translateY(8px) + opacity(0) → translateY(0) + opacity(1)
Exit:  translateY(0) + opacity(1) → translateY(-4px) + opacity(0)
Stagger: 50ms between items (--sc-stagger-delay)
```

- Duration: `--sc-duration-normal`
- Easing: `--sc-ease-out` for enter, `--sc-ease-in` for exit
- Maximum stagger: 300ms total (`--sc-stagger-max`)

### 3.4 Skeleton → Content Transition

```
Skeleton shimmer runs continuously
On data load: skeleton fades out (--sc-duration-fast)
Content fades in (--sc-duration-normal)
```

- Cross-fade, not sequential (skeleton and content overlap briefly)
- Content enters with slight translateY(4px) for subtle lift effect

### 3.5 State Transitions

For interactive state changes (hover, active, focus, disabled):

| State    | Properties                    | Duration                | Easing             |
| -------- | ----------------------------- | ----------------------- | ------------------ |
| Hover    | background, shadow, transform | `--sc-duration-fast`    | `--sc-ease-out`    |
| Active   | transform, shadow             | `--sc-duration-instant` | `--sc-ease-spring` |
| Focus    | outline (focus ring)          | `--sc-duration-fast`    | `--sc-ease-out`    |
| Disabled | opacity                       | `--sc-duration-fast`    | `--sc-ease-out`    |
| Error    | border-color, shadow          | `--sc-duration-fast`    | `--sc-ease-out`    |

---

## 4. Choreography Rules

### 4.1 Stagger Sequences

When multiple elements enter simultaneously, stagger prevents cognitive overload.

```css
.card {
  animation: sc-fade-up var(--sc-duration-normal) var(--sc-ease-out) both;
  animation-delay: calc(var(--sc-stagger-delay) * var(--sc-stagger-index, 0));
}
```

- `--sc-stagger-delay`: 50ms between items
- `--sc-stagger-max`: 300ms cap (item 7+ all start at 300ms)
- `--sc-cascade-delay`: 30ms for nested children within a parent
- Maximum cascade depth: 3 levels

### 4.2 Entrance Choreography

For a page/view entering:

1. **Container** enters first (background, border establish context)
2. **Primary content** enters next (title, main body)
3. **Secondary content** follows (metadata, actions, decorative elements)
4. **Ambient effects** start last (gradients, glows, background animation)

Gap between steps: `--sc-cascade-delay` (30ms)

### 4.3 Coordinated Motion (M3: Container Transform)

When an element transforms into another (e.g., card → detail view):

- Shared elements morph (position, size, border-radius)
- Non-shared elements of origin fade out
- Non-shared elements of destination fade in
- Total duration: `--sc-duration-slow`
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

SeaClaw's token pipeline handles this globally in `theme.css`. Components using token-based
durations inherit reduced motion automatically.

### Rules

- Never convey essential information through animation alone
- Provide static alternatives for all animated content
- Loading states must work without animation (skeleton shapes visible without shimmer)
- Focus rings must be visible without transition animation
- Auto-playing ambient animations pause under reduced motion

---

## 7. Cross-Reference

| Document             | Covers                                          |
| -------------------- | ----------------------------------------------- |
| `design-strategy.md` | Token values, color, typography, breakpoints    |
| `design-system.md`   | Component API, platform-specific implementation |
| `ux-patterns.md`     | Layout archetypes, interaction patterns         |
| `AGENTS.md` §12.4    | Enforcement rules for agents                    |
| ADR-0005             | Spring physics decision rationale               |
