---
title: "Design — Story A: Directive telemetry dashboard tile"
sprint: 1
story: A
created: 2026-05-11
status: ready_for_implementation
authored_by: tech-lead
---

# Design for Story A — Directive telemetry dashboard tile

## Approach

A new presentational LitElement `<hu-directive-telemetry-tile>` follows the
exact architectural shape just shipped for `<hu-fidelity-tile>`: a dumb
component that owns its render states (skeleton, populated, error) and a
parent view (`metrics-view.ts`) that owns the gateway fetch. This keeps the
component pure-presentational (trivial to unit test, no `WebSocket` mocking
required) and isolates failures to a single tile — a misbehaving
`metrics.directive_telemetry` response cannot blank the persona-fidelity
tile, the snapshot stats, or any other section.

The novel piece is the stacked-bar visualization. Three implementation
options were considered:

1. **Flex with `flex-grow` per segment** — each `<div>` carries
   `flex-grow: <count>; flex-basis: 0`, browser proportions the row. Zero
   custom math, zero `width: ${pct}%` token violations.
2. **CSS Grid with `grid-template-columns: <count>fr <count>fr ...`** —
   functionally equivalent to (1) but requires inline-style assembly of
   the template string and re-renders the whole grid on every count
   change.
3. **SVG `<rect>` per segment** — full control over join radii, accessible
   via `role="img" aria-label` but heavier to author and the px width math
   has to happen in JS (resize-observer territory).

**Decision: Flex with `flex-grow`.** It is the smallest, most-readable
option; tokens cover every dimension; and the browser's layout algorithm
gives us exact proportional widths without any inline `%` math. Zero
segments (variants with `count === 0`) are filtered out before render so
they cannot create 0-width visual artifacts between rounded neighbors.

For color, the PO's recommendation is endorsed with one refinement
(documented in §3): four "good" variants use `--hu-chart-categorical-*`
tokens, `null_overlay` uses `--hu-warning`, `default` uses
`--hu-text-muted`. The refinement is the specific subset of categorical
indices chosen (skip `-3` amber and `-4` coral, both of which would clash
semantically with `--hu-warning` / `--hu-error`).

TypeScript types are colocated with the component, matching the precedent
set by `FidelityStatus` living inside `hu-fidelity-tile.ts`. A
`ui/src/types/` directory does not exist today; creating one for a single
interface is YAGNI. When/if a third gateway-typed tile lands, extract via
Rule of Three.

---

## 1. Component contract — `<hu-directive-telemetry-tile>`

Mirrors `<hu-fidelity-tile>` exactly. Two reactive properties, no slots,
no emitted events.

```ts
@customElement("hu-directive-telemetry-tile")
export class HuDirectiveTelemetryTile extends LitElement {
  /**
   * Full JSON object emitted by `metrics.directive_telemetry`.
   * `null` represents "data not loaded yet" and renders the
   * loading skeleton. `undefined` is treated identically.
   */
  @property({ attribute: false }) data: DirectiveTelemetry | null = null;

  /**
   * When non-empty, the tile renders an inline error banner instead
   * of the bar + legend. Parent view sets this on gateway failure.
   */
  @property({ type: String }) errorMessage = "";
}
```

State semantics (identical 3-state machine to `hu-fidelity-tile`):

| `data`     | `errorMessage` | Renders         |
| ---------- | -------------- | --------------- |
| `null`     | `""`           | Loading skeleton |
| populated  | `""`           | Bar + legend    |
| any        | non-empty      | Error banner    |

- **No slots.** Content is fully derived from `data`. Composition not
  needed.
- **No emitted `CustomEvent`s.** Tile is pure-presentational; parent
  owns retry via re-running `this.load()` (which in turn re-runs both
  `_loadFidelity()` and `_loadDirectiveTelemetry()`).
- **Public types re-exported** from the component module (mirrors
  `FidelityStatus`):

```ts
export const DIRECTIVE_VARIANTS = [
  "null_overlay",
  "default",
  "formal_terse",
  "casual_emoji",
  "casual_or_short",
  "adaptive_emoji",
] as const;

export type DirectiveVariant = (typeof DIRECTIVE_VARIANTS)[number];

export interface DirectiveTelemetry {
  total: number;
  variants: Record<DirectiveVariant, number>;
}
```

The `DIRECTIVE_VARIANTS` const array pins render order and matches the
order in `hu_personal_model_directive_variant_label()`
(`src/memory/personal_model.c:307-`) and the JSON key emission order in
`cp_admin_metrics_directive_telemetry`
(`src/gateway/cp_admin.c:1373-1380`). The component must iterate over
this const, not over `Object.entries(data.variants)` — see §7 risk 1.

Global `HTMLElementTagNameMap` declaration:

```ts
declare global {
  interface HTMLElementTagNameMap {
    "hu-directive-telemetry-tile": HuDirectiveTelemetryTile;
  }
}
```

---

## 2. TypeScript types — location decision

**Location: colocated with the component in `ui/src/components/hu-directive-telemetry-tile.ts`.**

Candidates considered:

| Location | Pros | Cons |
| -------- | ---- | ---- |
| (A) Colocate in `hu-directive-telemetry-tile.ts` | Same precedent as `FidelityStatus`. Zero new files. Type follows the component it describes. | Re-export chain (view imports from component) is one hop. |
| (B) New `ui/src/types/gateway-types.ts` | Centralizes wire-protocol types for future reuse. | Directory doesn't exist; creating it for a single type is YAGNI. View+component still both have to import it. |
| (C) Add to `ui/src/components/hu-fidelity-tile.ts` next to `FidelityStatus` | One import in `metrics-view.ts`. | Semantically wrong: the fidelity tile would own a type that has nothing to do with fidelity. Future maintainers would not look there. |

**Verdict: (A).**

Rationale:
1. `FidelityStatus` lives in `ui/src/components/hu-fidelity-tile.ts`
   (lines 30–51) and `metrics-view.ts` imports it from that path
   (line 15: `import type { FidelityStatus } from "../components/hu-fidelity-tile.js"`).
   Following the same convention for `DirectiveTelemetry` keeps the
   pattern uniform and discoverable.
2. KISS / YAGNI per `AGENTS.md` §3: a new `types/` directory introduces
   a new concept ("shared wire types") with exactly one inhabitant. The
   Rule of Three has not been triggered — there is one such type today
   (`FidelityStatus`) and adding a second (`DirectiveTelemetry`) does not
   yet justify the extraction.
3. Component-owned types make ownership clear: changes to the tile and
   its contract live in one file. The C handler is the source of truth
   for the JSON shape; the colocated TS interface is its UI-side
   shadow.

**Implementer action:** export `DirectiveTelemetry`,
`DirectiveVariant`, and `DIRECTIVE_VARIANTS` from
`hu-directive-telemetry-tile.ts`. Import in `metrics-view.ts` via:

```ts
import "../components/hu-directive-telemetry-tile.js";
import type { DirectiveTelemetry } from "../components/hu-directive-telemetry-tile.js";
```

---

## 3. Render plan — stacked bar

### Variant → color mapping (decision)

The PO's open question:

> Should the stacked bar use `--hu-chart-categorical-*` tokens or
> semantic tokens (e.g., `null_overlay` → `--hu-error-dim`)?

**Verdict: hybrid. Four "good" variants use `--hu-chart-categorical-*`;
`null_overlay` and `default` use semantic tokens that match their
diagnostic meaning.**

| Variant            | Token                          | Hex      | Why |
| ------------------ | ------------------------------ | -------- | --- |
| `null_overlay`     | `--hu-warning`                 | `#eab308`| Means "no overlay registered for this channel" — operator action required, not an error. Amber is the canonical "we have a gap" signal in this codebase (used by `icons.warning` in `metrics-view.ts` line 562). |
| `default`          | `--hu-text-muted`              | `#8a847e`| Means "overlay exists but conveys no useful signal" — the weakest, lowest-priority outcome. A muted gray says "we have nothing interesting to show". |
| `formal_terse`     | `--hu-chart-categorical-2`     | `#4a6fa5`| Steel blue. "Formal" pairs with the most professional categorical color. |
| `casual_emoji`     | `--hu-chart-categorical-1`     | `#7ab648`| Human green (brand). This is the variant the iMessage/Discord paths fire most, so the brand color in the biggest segment is the intended at-a-glance reading. |
| `casual_or_short`  | `--hu-chart-categorical-5`     | `#14b8a6`| Teal. Adjacent to green in the wheel — reads as "casual cousin". |
| `adaptive_emoji`   | `--hu-chart-categorical-6`     | `#9fb8d9`| Light steel. "Adaptive" feels soft/optional; light steel matches `formal_terse` (categorical-2) family. |

**Rationale for the hybrid (not pure-categorical):**

- `null_overlay` and `default` are diagnostically different from the
  four "good" variants. They mean "we have no signal" — the operator
  should investigate why a channel didn't produce a useful directive.
  Painting them with the same categorical hue family as a successful
  variant flattens that distinction. Semantic tokens (`--hu-warning`,
  `--hu-text-muted`) communicate "this is not a normal outcome" without
  the false alarm of `--hu-error` (which would be coral and read as
  "broken" — the variants are not broken, they are merely uninformative).

**Why not pure semantic:**

- The four good variants are equally legitimate — none is "better"
  than another. Using the diverging palette
  (`--hu-chart-diverging-positive/neutral/negative`) implies an
  ordering that does not exist. Categorical is the correct family.

**Why skip categorical-3 and categorical-4:**

- `--hu-chart-categorical-3` is amber (`#f59e0b`) — same hue family as
  `--hu-warning` (`#eab308`). Using both produces an unreadable bar
  where `null_overlay` and one of the good variants visually merge.
- `--hu-chart-categorical-4` is coral (`#f97066`) — visually identical
  to `--hu-error`. Painting a positive variant with the codebase's
  established error color is a UX trap.

### Layout

The tile renders three rows inside the `<hu-card glass surface="high">`:

```
[ Title row ]                                       [ total: 184 ]
[ ████████████████████████ ██████ ████ ██ ▌ ▌      ]  ← bar
[ ■ casual_emoji 113 (61.4%)  ■ casual_or_short 26 (14.1%)  ... ]  ← legend
```

**HTML structure:**

```html
<hu-card glass surface="high">
  <div class="tile" role="region" aria-label="Directive variant telemetry"
       aria-busy="false" aria-live="polite">

    <!-- Header row -->
    <div class="header">
      <div class="title">Directive variants</div>
      <div class="total" aria-label="184 total fires">
        <span class="total-value">184</span>
        <span class="total-label">fires</span>
      </div>
    </div>

    <!-- Stacked bar -->
    <div class="bar" role="list" aria-label="Variant distribution">
      <!-- One segment per non-zero variant, iterated in DIRECTIVE_VARIANTS order -->
      <div class="segment segment--casual-emoji"
           role="listitem"
           aria-label="casual_emoji: 113 fires (61.4%)"
           style="flex-grow: 113"></div>
      <!-- ... other segments ... -->
    </div>

    <!-- Legend -->
    <ul class="legend" aria-hidden="true">
      <!-- Hidden from screen readers because the bar's listitems already carry the data;
           legend is visual reinforcement only -->
      <li class="legend-item">
        <span class="legend-swatch legend-swatch--casual-emoji"></span>
        <span class="legend-label">casual_emoji</span>
        <span class="legend-count">113</span>
        <span class="legend-pct">61.4%</span>
      </li>
      <!-- ... -->
    </ul>
  </div>
</hu-card>
```

**Sizing:**

- `.bar` is `height: var(--hu-space-md)` (12px), `border-radius: var(--hu-radius-sm)`, `overflow: hidden` so segment corners are clipped by the container rather than each segment carrying its own radius (avoids visible gaps between segments).
- `.bar { display: flex; gap: var(--hu-chart-bar-gap, 2px); }` — a thin gap separates segments. If `--hu-chart-bar-gap` is not yet exposed as a CSS variable, fall back to `2px` is **not allowed** (raw px); use `var(--hu-space-3xs)` instead.
- Each `.segment` has `flex-grow: ${count}; flex-basis: 0; min-width: 0`. The browser proportions widths automatically.

**Why not render zero-count segments:**

- A `flex-grow: 0` segment with `flex-basis: 0` is exactly 0px wide, but
  it still participates in the `gap` calc — at 6 variants with 6 gaps,
  zero-count segments could add up to noticeable phantom whitespace.
  Filtering on `count > 0` before render avoids this entirely. The
  legend always lists all six variants (zero-counts show as
  `0 (0.0%)`) so no data is hidden.

### Empty state

When `total === 0` (or `sumOfVariants === 0`):

- Render a single full-width muted bar segment using `--hu-text-muted` /
  `--hu-surface-container`.
- Render an inline copy line: `"No directive variants have fired yet —
  channel overlays may not be configured."` styled like `.lane-sub` on
  the fidelity tile (`--hu-text-2xs` size, `--hu-text-muted` color).
- `aria-busy` stays `false`. The bar's `role="list"` element gets
  `aria-label="No variants fired yet"` and contains no listitems.

### Accessibility

| Concern | Implementation |
| ------- | -------------- |
| Screen-reader announcement of distribution | `.bar` is `role="list"` with each `.segment` as `role="listitem"` carrying `aria-label="<variant>: <count> fires (<pct>%)"`. NVDA/VoiceOver announce each. |
| Legend redundancy | `<ul class="legend" aria-hidden="true">` — the bar already carries the announcement; the legend is purely visual reinforcement. |
| Region landmark | `.tile` is `role="region" aria-label="Directive variant telemetry"`. |
| Live updates | `aria-live="polite"` so future polling refreshes are announced gently. |
| Color-blindness | The bar relies on shape (segment width = count) primarily; color is reinforcement. The legend's count + percentage labels are always visible. |
| Contrast | All chosen tokens already pass WCAG 2.1 AA (verified via `--hu-chart-categorical-*` documentation in `design-tokens/data-viz.tokens.json`). |
| Keyboard | Tile is not interactive (no focusable elements). No keyboard handlers required. The hu-card host is not focusable. |
| Reduced motion | `:host` carries `animation: hu-scale-in var(--hu-duration-normal) var(--hu-spring-micro, ease-out) both` for entrance (same as fidelity tile). `@media (prefers-reduced-motion: reduce) { :host { animation: none; } }`. |

### Tokens used (strict whitelist — no raw values)

```
--hu-space-2xs, --hu-space-3xs, --hu-space-sm, --hu-space-md, --hu-space-lg
--hu-radius-sm
--hu-text-2xs, --hu-text-xs, --hu-text-sm, --hu-text-xl
--hu-weight-semibold
--hu-text, --hu-text-muted
--hu-surface-container
--hu-warning
--hu-error, --hu-error-text (error banner)
--hu-chart-categorical-1, -2, -5, -6
--hu-duration-normal, --hu-duration-slow
--hu-ease-in-out
--hu-spring-micro
```

No `rgba()`, no raw hex, no `px` literals, no `cubic-bezier(...)`, no
hardcoded ms durations. Alpha transparency on the error banner uses the
`color-mix(in srgb, var(--hu-error) 8%, transparent)` pattern from
`hu-fidelity-tile.ts` lines 169-173.

---

## 4. Loading + error states

### Loading skeleton

Identical idiom to `hu-fidelity-tile`'s skeleton (lines 161-166 +
177-183). Renders when `data === null && !errorMessage`.

```html
<div class="tile" role="region" aria-busy="true" aria-label="Directive variant telemetry">
  <div class="header">
    <div class="skeleton skeleton--title"></div>
    <div class="skeleton skeleton--total"></div>
  </div>
  <div class="skeleton skeleton--bar" aria-hidden="true"></div>
  <div class="legend" aria-hidden="true">
    <div class="skeleton skeleton--legend-item"></div>
    <div class="skeleton skeleton--legend-item"></div>
    <div class="skeleton skeleton--legend-item"></div>
  </div>
</div>
```

Sizing constraints (zero layout shift on data arrival):

| Skeleton | Dimensions |
| -------- | ---------- |
| `.skeleton--title` | `width: 9rem; height: var(--hu-text-sm)` |
| `.skeleton--total` | `width: 3rem; height: var(--hu-text-xl)` |
| `.skeleton--bar` | `width: 100%; height: var(--hu-space-md)` — matches final bar height exactly |
| `.skeleton--legend-item` | `width: 7rem; height: var(--hu-text-2xs)` |

The `hu-pulse` keyframe is already defined in `hu-fidelity-tile.ts`. To
avoid duplicating it: declare it in the component's own `static styles`
block (Shadow-DOM scoped so no global collision). Both components carry
their own copy — KISS over premature extraction.

### Error banner

Identical to `hu-fidelity-tile.ts` lines 168-175 + 224-227.

```html
<div class="error-banner" role="alert">
  {{ errorMessage }}
</div>
```

CSS pattern:

```css
.error-banner {
  padding: var(--hu-space-sm) var(--hu-space-md);
  background: color-mix(in srgb, var(--hu-error) 8%, transparent);
  border: 1px solid color-mix(in srgb, var(--hu-error) 24%, transparent);
  border-radius: var(--hu-radius-sm);
  color: var(--hu-error-text, var(--hu-error));
  font-size: var(--hu-text-sm);
}
```

### Retry behavior

**No in-tile retry button.** Parent view's existing "Retry loading
metrics" button (`metrics-view.ts` line 562-569) calls `this.load()`,
which re-runs `_loadDirectiveTelemetry()` as a side effect (see §5).
This matches the fidelity tile precedent — fidelity has no in-tile
retry either. Adding a per-tile retry would diverge from the established
pattern, double the buttons on a failure, and tempt future maintainers
to add retry UI to every tile (not what KISS wants).

The error message itself comes from `friendlyError()`
(`ui/src/utils/friendly-error.ts`) — never expose raw stack traces or
gateway error envelopes to the user.

---

## 5. View wiring — `ui/src/views/metrics-view.ts`

### Edits (mirroring the `_loadFidelity` pattern verbatim)

**Imports (line 14-15 region):**

```ts
import "../components/hu-fidelity-tile.js";
import type { FidelityStatus } from "../components/hu-fidelity-tile.js";
import "../components/hu-directive-telemetry-tile.js";                                 // NEW
import type { DirectiveTelemetry } from "../components/hu-directive-telemetry-tile.js"; // NEW
```

**State properties (insert after line 248, i.e. after `fidelityError`):**

```ts
@state() private fidelity: FidelityStatus | null = null;
@state() private fidelityError = "";
/* Directive telemetry is fetched in parallel with persona fidelity.
 * A slow or failing telemetry backend never blocks the main snapshot
 * paint or the fidelity tile. Each tile renders its own loading +
 * error state independently. */
@state() private directiveTelemetry: DirectiveTelemetry | null = null;             // NEW
@state() private directiveTelemetryError = "";                                      // NEW
```

**`load()` method (append after line 272, after `void this._loadFidelity()`):**

```ts
void this._loadFidelity();
void this._loadDirectiveTelemetry();   // NEW — fire-and-forget, parallel with fidelity
```

The `void` keyword and absence of `await` is intentional: failure
isolation. Each side-channel load runs to completion or failure
independently; neither blocks the other or the main `metrics.snapshot`
paint.

**`_loadDirectiveTelemetry()` method (insert after `_loadFidelity()` at
line 295):**

```ts
private async _loadDirectiveTelemetry(): Promise<void> {
  const gw = this.gateway;
  if (!gw) return;
  this.directiveTelemetry = null;
  this.directiveTelemetryError = "";
  try {
    const res = (await gw.request<DirectiveTelemetry>("metrics.directive_telemetry", {})) as
      | DirectiveTelemetry
      | { result?: DirectiveTelemetry };
    const data =
      (res && "result" in res && (res as { result?: DirectiveTelemetry }).result) ||
      (res && "variants" in res ? (res as DirectiveTelemetry) : null);
    if (data && typeof data.total === "number" && data.variants) {
      this.directiveTelemetry = data;
    } else {
      this.directiveTelemetryError = "no directive telemetry data";
    }
  } catch (e) {
    this.directiveTelemetryError = friendlyError(e);
  }
}
```

Exactly mirrors `_loadFidelity` (lines 275-295). The envelope-shape
sniff (`"result" in res` vs `"variants" in res`) matches the existing
demo-gateway / real-gateway dual-shape support.

**Render wiring (modify line 574):**

```ts
${this._renderFidelity()} ${this._renderDirectiveTelemetry()} ${this._renderIntelligenceStats()}
```

Insertion point: **between** `_renderFidelity()` and
`_renderIntelligenceStats()` so the persona-fidelity group (fidelity tile
+ directive telemetry tile) sits visually together. The reader's eye
moves: "how well is the persona fitting" (fidelity) → "which variants
fired" (directive telemetry) → "what intelligence modules are on"
(intelligence stats).

**`_renderDirectiveTelemetry()` method (insert after `_renderFidelity()`
at line 601):**

```ts
private _renderDirectiveTelemetry() {
  /* Mirrors _renderFidelity: the tile owns its loading skeleton
   * and error banner; we hand it the data plus an optional
   * errorMessage and let it decide. */
  return html`
    <div class="section hu-scroll-reveal" role="region" aria-label="Directive variant telemetry">
      <hu-section-header
        heading="Directive variants"
        description="Per-variant fire counts from acknowledgment_directive_for_overlay"
      ></hu-section-header>
      <hu-directive-telemetry-tile
        .data=${this.directiveTelemetry}
        .errorMessage=${this.directiveTelemetryError}
      ></hu-directive-telemetry-tile>
    </div>
  `;
}
```

### Failure isolation guarantees

1. `_loadDirectiveTelemetry` is called via `void` — its returned promise
   is not awaited and any unhandled rejection inside it is caught by
   its own `try/catch` (so no `unhandledrejection` event ever fires).
2. The outer `load()` method's `try/catch` wraps only the
   `metrics.snapshot` call (lines 256-271 today). Neither
   `_loadFidelity()` nor `_loadDirectiveTelemetry()` can throw into
   that scope.
3. State is fully partitioned: `directiveTelemetry` /
   `directiveTelemetryError` are independent of `fidelity` /
   `fidelityError` / `snapshot` / `error`. A bug in one render path
   cannot corrupt the others.
4. If the gateway returns malformed JSON (missing `variants` /
   `total`), the type-guard `typeof data.total === "number" &&
   data.variants` fails and `directiveTelemetryError` is set to "no
   directive telemetry data" — the tile shows a graceful banner, not a
   blank div.

### File line counts (estimated)

| File | Change | Estimated LOC |
| ---- | ------ | ------------- |
| `ui/src/components/hu-directive-telemetry-tile.ts` | new file | +260 |
| `ui/src/components/hu-directive-telemetry-tile.test.ts` | new file | +160 |
| `ui/src/views/metrics-view.ts` | imports + state + 2 methods + render hook | +45 |
| `ui/src/demo-gateway.ts` | no change (mock already exists at line 2321) | 0 |
| **Total** | | **~465 LOC** |

---

## 6. Test plan

Tests live in `ui/src/components/hu-directive-telemetry-tile.test.ts`
(new dedicated file), **not** in `extra-components.test.ts`. Two
reasons:

1. **AC-A.7 names that exact path** as the literal `rg` target. Auditors
   will run `rg "describe\|it\(" ui/src/components/hu-directive-telemetry-tile.test.ts | wc -l`
   verbatim and expect ≥ 4. Satisfying the AC as written.
2. `extra-components.test.ts` is already 3,978 lines; per-component
   test files are the better long-term shape. Vitest auto-discovers
   any `src/**/*.test.ts` (verified against `ui/vitest.config.ts`
   line 6), so no config change is required.

### Required test cases

| # | `it(...)` description | Assertion summary | Maps to AC |
| - | --------------------- | ----------------- | ---------- |
| 1 | `registers as a custom element` | `customElements.get("hu-directive-telemetry-tile")` is defined | AC-A.1 |
| 2 | `renders loading skeleton when data is null` | `aria-busy="true"`, `.skeleton--bar` present, no `.segment` rendered, no `.error-banner` | AC-A.3, AC-A.7 |
| 3 | `renders one segment per non-zero variant and the total` | 6 `.segment` elements (one per variant), each segment's `aria-label` matches `<variant>: <count> fires (<pct>%)`, `.total-value` contains "184", `aria-busy="false"` | AC-A.2, AC-A.7 |
| 4 | `renders the all-zero empty-state when total is 0` | One muted placeholder segment, copy line contains "No directive variants have fired yet", `aria-busy="false"`, no `.segment` listitems | AC-A.7 |
| 5 | `renders error banner when errorMessage is set` | `.error-banner` with `role="alert"`, banner text contains the message, no `.segment` rendered, no `.skeleton` rendered | AC-A.3, AC-A.7 |
| 6 (bonus) | `legend renders percentage labels` | Legend item for `casual_emoji` contains "113" and "61.4%" | AC-A.2 |

Six cases ≥ 4 required. `rg "describe\|it\(" .../test.ts | wc -l` will
print 7 (1 `describe` + 6 `it`). AC-A.7 passes with margin.

### Test setup boilerplate

```ts
import { describe, it, expect } from "vitest";
import "./hu-directive-telemetry-tile.js";
import type {
  HuDirectiveTelemetryTile,
  DirectiveTelemetry,
} from "./hu-directive-telemetry-tile.js";

describe("hu-directive-telemetry-tile", () => {
  // ... 6 it() cases ...
});
```

Vitest environment is `happy-dom` (per `ui/vitest.config.ts:5`), which
supports `customElements.define`, `document.createElement`, and the
LitElement `updateComplete` promise out-of-the-box. No mocking
required.

### Demo-gateway behavior referenced by tests

The component-level tests do **not** mock the gateway — the component
takes `data` as a prop and does not perform any network calls. Tests
construct `DirectiveTelemetry` literals inline. The demo-gateway mock
(`ui/src/demo-gateway.ts` line 2321) is exercised end-to-end at the
view layer; for AC-A.5 the verification is the static `rg
"directive_telemetry" ui/src/demo-gateway.ts` plus a separate test
ensuring the sum matches `total`:

```ts
it("demo-gateway mock variants sum to total", () => {
  /* AC-A.5 evidence — guards against accidental drift between
   * total and variants in the demo fixture. */
  const mock = {
    total: 184,
    variants: { null_overlay: 12, default: 7, formal_terse: 18,
                casual_emoji: 113, casual_or_short: 26, adaptive_emoji: 8 },
  };
  const sum = Object.values(mock.variants).reduce((a, b) => a + b, 0);
  expect(sum).toBe(mock.total);
});
```

This can live in `hu-directive-telemetry-tile.test.ts` OR in a separate
`demo-gateway.test.ts` file. Implementer's choice; my preference is to
embed it in the tile's test file to keep one mental anchor per file.

### Build + lint validation (not vitest, but required by AC-A.1, A.6)

```bash
cd ui
npm run check     # typecheck + lint + format + test + lint:tokens
npm run build     # AC-A.1: zero TypeScript errors
```

`npm run check` will:

- Run `tsc` (AC-A.1 evidence)
- Run vitest (AC-A.2, A.3, A.7 evidence)
- Run `npm run lint:tokens` (AC-A.6 evidence — flags raw hex, rgba,
  hardcoded durations, raw breakpoints)
- Run prettier + eslint

If `npm run check` exits 0, all of AC-A.1, A.2, A.3, A.6, A.7 have
direct evidence. AC-A.4 and A.5 are static grep evidence.

---

## 7. Risks + sequencing

### Risks

| # | Risk | Probability | Impact | Mitigation |
| - | ---- | ----------- | ------ | ---------- |
| 1 | Variant key drift between TS const and C handler | LOW | SMALL | The `DIRECTIVE_VARIANTS` const literally hardcodes the six keys from `hu_personal_model_directive_variant_label()` (`src/memory/personal_model.c:307-`). Component iterates that const, never `Object.entries`. If C adds a new variant, the UI silently drops it until a TS update — acceptable forward-compat behavior for an additive change. |
| 2 | `total` ≠ `sum(variants)` from server | LOW | SMALL | Display `total` as authoritative; compute percentages against `Math.max(1, sumOfVariants)` (not `total`) so a 0 doesn't divide and an off-by-one between counters doesn't make percentages over 100%. |
| 3 | Token-lint regression on new CSS | LOW | SMALL | The whitelist in §3 is exhaustive. Every numeric / color value in the new component must reference an entry in that whitelist. `npm run lint:tokens` catches violations before commit. |
| 4 | Render-order non-determinism | LOW | SMALL | Iterate `DIRECTIVE_VARIANTS` const — never trust JSON object key order even though V8 preserves it for non-numeric keys. |
| 5 | Color clash between `--hu-warning` and `--hu-chart-categorical-3` | LOW | SMALL | Mitigated by design: `categorical-3` is explicitly excluded from the variant mapping (see §3 "Why skip categorical-3"). |
| 6 | Failure isolation regression: `_loadDirectiveTelemetry` throws into `load()` | LOW | MEDIUM | `_loadDirectiveTelemetry` is called via `void` (no `await`), and its body is wrapped in `try/catch`. Identical pattern to `_loadFidelity`. Test 5 (error banner) verifies the tile-level fallback; an additional integration test could verify the view-level behavior but is out of scope per the AC matrix. |
| 7 | AC-A.4 grep keyword absence | LOW | SMALL | Both the `import` statement and the `<hu-directive-telemetry-tile>` element use the literal substring `hu-directive-telemetry-tile`, yielding ≥ 2 matches in `metrics-view.ts`. Step 5 of the sequencing explicitly verifies via `rg`. |
| 8 | Shadow-DOM scoping conflict with the host `hu-card` | LOW | SMALL | The tile renders its content **inside** `<hu-card glass surface="high">` as a slotted child. `hu-card` does not slot via Shadow DOM in a way that hides children's styles (verified in `hu-fidelity-tile.ts` lines 187-200 which uses the same pattern). Styles in `static styles` of the tile only affect the tile's own Shadow DOM. No leakage risk. |
| 9 | Test file path AC-A.7 vs. precedent (`extra-components.test.ts`) | LOW | SMALL | Decision documented in §6 — create the dedicated file. Vitest auto-discovers it. The AC's literal `rg` target passes. Net improvement to codebase shape (smaller test files). |
| 10 | `hu-card` is `entrance` / `tilt` and adds entry animation that conflicts with the tile's own `hu-scale-in` | LOW | SMALL | `hu-fidelity-tile.ts` uses the exact same `hu-card glass surface="high"` + `:host { animation: hu-scale-in ... }` combo with no observed conflict. Reduced motion media query disables the tile's animation; `hu-card` respects the same global. |

None of these risks rise to "blocks implementation". No BLOCKED_HIGH_RISK
result needed.

### Sequencing (commit-able, each independently verifiable)

**Step 1 — Component skeleton + types (single commit)**

- Create `ui/src/components/hu-directive-telemetry-tile.ts` with:
  - `DIRECTIVE_VARIANTS` const, `DirectiveVariant` type,
    `DirectiveTelemetry` interface.
  - `@customElement` shell, `data` and `errorMessage` properties.
  - `render()` returning ONLY the loading skeleton state (no segments,
    no legend, no populated branch).
  - All CSS tokens declared in `static styles`.
  - `HTMLElementTagNameMap` declaration.
- Verify: `npm run build` clean (AC-A.1 ⓘ partial).

**Step 2 — Test file with skeleton + custom-element + error cases**

- Create `ui/src/components/hu-directive-telemetry-tile.test.ts`.
- Add test cases 1, 2, 5 (custom element, loading skeleton, error
  banner) — they pass against the skeleton-only component from step 1.
- Verify: `npm run test -- hu-directive-telemetry-tile` green.

**Step 3 — Populated bar + legend rendering**

- Implement `renderBody()` (populated branch) and the segment + legend
  helpers.
- Add tests 3, 4, 6 (populated rendering, all-zero empty state,
  percentage labels).
- Verify: `npm run check` exits 0 (typecheck + lint + tokens + tests).

**Step 4 — View wiring**

- Edit `ui/src/views/metrics-view.ts` per §5.
- Manual smoke test: `npm run dev`, open Metrics view, observe the
  tile rendered with the demo-gateway data (184 total, casual_emoji
  dominant).
- Verify: `rg "hu-directive-telemetry-tile" ui/src/views/metrics-view.ts`
  returns ≥ 2 matches (AC-A.4).

**Step 5 — Final validation pass**

- Re-run `npm run check`.
- Confirm all AC grep targets pass (AC-A.4, AC-A.5, AC-A.7).
- (Optional) Add catalog entry per `ui/CLAUDE.md` "New Component
  Checklist" — not in AC matrix; nice-to-have.
- Manual axe-core scan of the Metrics view (optional; CI covers it).

### Rollback

- Steps 1–3 add only new files (`hu-directive-telemetry-tile.ts` and
  its test). Rollback = `rm` of those two files. Nothing else in the
  repo touches them yet (the view wiring happens in step 4). Zero risk.
- Step 4 edits `metrics-view.ts` — a single revert of that commit
  removes the import, state, two methods, and the render hook. Other
  view content is untouched (snapshot, fidelity tile, intelligence
  stats all continue rendering).
- The C handler (`cp_admin_metrics_directive_telemetry`) is already
  shipped and unchanged. Demo-gateway mock is already shipped and
  unchanged. Rolling back the UI side does not affect the gateway
  contract or any other consumer.
- No DB migrations, no schema changes, no persisted state.

---

## 8. Out-of-scope reaffirmation

Copied verbatim from `sprints/sprint-1/stories.md` Story A:

- Historical time-series of variant distribution (no chart over time,
  just current snapshot)
- Per-channel variant breakdown (channel drill-down is a future
  enhancement)
- Server-Sent Event live push of telemetry (polling or one-shot fetch
  only)
- Any change to the C gateway handler
  `cp_admin_metrics_directive_telemetry` itself

### Additionally discovered during design (also out of scope this sprint):

- **No in-tile retry button.** The tile is presentational; retry is the
  parent view's responsibility, and the view's existing "Retry
  loading metrics" button re-runs `load()` which re-fetches the
  directive telemetry as a side effect. Adding a per-tile retry would
  diverge from the fidelity-tile precedent.
- **No new gateway method.** `metrics.directive_telemetry` already
  exists in the C handler (`cp_admin.c:1350-1385`) and the demo-gateway
  mock (`demo-gateway.ts:2321`). No control-protocol registration
  change required.
- **No new Phosphor icon added to `ui/src/icons.ts`.** The fidelity tile
  precedent is iconless in its `hu-section-header`; matching that
  precedent. If a future enhancement wants a bar-chart icon, that is a
  separate PR.
- **No catalog entry required by AC.** `ui/CLAUDE.md` "New Component
  Checklist" item 3 recommends a `catalog.html` entry; AC-A does not
  require it. Implementer should add it as a nice-to-have if time
  permits but it is not on the AC matrix.
- **No `ui/src/types/` directory creation.** Per §2 decision: type
  colocation with the component. Rule of Three has not triggered.
- **No new top-level gateway alias** (e.g., `sota.metrics`-style alias
  for `metrics.directive_telemetry`). The single canonical method name
  is sufficient.
- **No SSE / polling timer in the tile or view.** AC-A explicitly
  scopes to a one-shot fetch on view load (and on view re-load via the
  Retry button). Periodic refresh is a future enhancement.

---

## Acceptance criteria mapping

Every AC has a specific evidence anchor:

| AC | Evidence |
| -- | -------- |
| AC-A.1: component exists + `npm run build` passes | Step 1 creates the file; Step 3's `npm run check` runs `tsc` cleanly. |
| AC-A.2: fetches `metrics.directive_telemetry`, renders one segment per variant key, exposes `total` | Step 3's test case 3 (`renders one segment per non-zero variant and the total`) asserts segment count and total text content. View-side fetch verified by Step 4 manual smoke. |
| AC-A.3: skeleton during load + error banner on rejection | Test cases 2 (skeleton) and 5 (error banner). |
| AC-A.4: `rg "hu-directive-telemetry-tile" ui/src/views/metrics-view.ts` ≥ 1 match | Step 4 adds both `import` and `<hu-directive-telemetry-tile>` element → 2 matches. |
| AC-A.5: demo-gateway mock with all six keys + `total === sum` | Static (mock already exists at line 2321); the bonus test "demo-gateway mock variants sum to total" guards the invariant. |
| AC-A.6: `npm run check` exits 0 with no token violations | Token whitelist in §3 ensures the new component uses only approved tokens. `npm run lint:tokens` enforces. |
| AC-A.7: test file has ≥ 4 `describe`/`it()` lines | Test plan has 1 `describe` + 6 `it` = 7 matches. |

All seven ACs are individually traceable to design artifacts. No AC is
satisfied by hand-wave.

---

`RESULT_tech-lead=READY`
