# SOTA UX Sweep Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Bring every dashboard view to Linear/Raycast/Vercel tier by building 7 new components and rewriting/upgrading all 14 views.

**Architecture:** 4 waves, each builds primitives then applies them to views. TDD throughout. Each task produces one commit. Components are LitElement web components using `--hu-*` design tokens exclusively.

**Tech Stack:** LitElement, TypeScript, Chart.js (CDN dynamic import), CSS custom properties, vitest.

**Test runner:** `npx vitest run` from `ui/` directory. Run specific: `npx vitest run -- <filename>`.

**Test pattern:** New components get registration + property + behavior tests in `ui/src/components/extra-components.test.ts` (append to existing file). View tests go in `ui/src/views/views.test.ts`.

---

## Wave 1: "See Everything" — Data Visualization

### Task 1: Create hu-chart component

**Files:**

- Create: `ui/src/components/hu-chart.ts`
- Modify: `ui/src/components/extra-components.test.ts` (append tests)

**Step 1: Write failing tests**

Append to `ui/src/components/extra-components.test.ts`:

```typescript
import "./hu-chart.js";

describe("hu-chart", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-chart")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("hu-chart");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should accept type property", () => {
    const el = document.createElement("hu-chart") as any;
    el.type = "bar";
    expect(el.type).toBe("bar");
  });

  it("should accept data property", () => {
    const el = document.createElement("hu-chart") as any;
    const data = { labels: ["A", "B"], datasets: [{ data: [1, 2] }] };
    el.data = data;
    expect(el.data).toEqual(data);
  });

  it("should default height to 200", () => {
    const el = document.createElement("hu-chart") as any;
    expect(el.height).toBe(200);
  });

  it("should render a canvas element", async () => {
    const el = document.createElement("hu-chart") as any;
    el.type = "bar";
    el.data = { labels: ["A"], datasets: [{ data: [1] }] };
    document.body.appendChild(el);
    await el.updateComplete;
    const canvas = el.shadowRoot?.querySelector("canvas");
    expect(canvas).toBeTruthy();
    el.remove();
  });
});
```

**Step 2: Run tests, verify failure**

Run: `npx vitest run -- extra-components`
Expected: FAIL — cannot resolve `./hu-chart.js`

**Step 3: Implement hu-chart**

Create `ui/src/components/hu-chart.ts`:

```typescript
import { LitElement, html, css } from "lit";
import { customElement, property, query, state } from "lit/decorators.js";

export interface ChartDataset {
  label?: string;
  data: number[];
  color?: string;
  backgroundColor?: string;
}

export interface ChartData {
  labels: string[];
  datasets: ChartDataset[];
}

@customElement("hu-chart")
export class ScChart extends LitElement {
  @property({ type: String }) type: "bar" | "line" | "area" | "doughnut" =
    "bar";
  @property({ type: Object }) data: ChartData = { labels: [], datasets: [] };
  @property({ type: Number }) height = 200;
  @property({ type: Boolean }) horizontal = false;

  @query("canvas") private _canvas!: HTMLCanvasElement;
  @state() private _chartInstance: unknown = null;
  private _resizeObserver?: ResizeObserver;

  static override styles = css`
    :host {
      display: block;
      position: relative;
    }
    .chart-wrap {
      position: relative;
      width: 100%;
    }
    canvas {
      width: 100% !important;
      height: auto !important;
    }
    .empty {
      display: flex;
      align-items: center;
      justify-content: center;
      height: var(--chart-h, 200px);
      color: var(--hu-text-muted);
      font-size: var(--hu-text-sm);
      font-family: var(--hu-font);
    }
  `;

  override render() {
    const hasData = this.data.datasets.some((d) => d.data.length > 0);
    return html`
      <div class="chart-wrap" style="--chart-h: ${this.height}px">
        ${hasData
          ? html`<canvas></canvas>`
          : html`<div class="empty">No data</div>`}
      </div>
    `;
  }

  override firstUpdated() {
    this._initChart();
    this._resizeObserver = new ResizeObserver(() => this._resize());
    this._resizeObserver.observe(this);
  }

  override updated(changed: Map<string, unknown>) {
    if (changed.has("data") || changed.has("type")) {
      this._initChart();
    }
  }

  override disconnectedCallback() {
    super.disconnectedCallback();
    this._destroyChart();
    this._resizeObserver?.disconnect();
  }

  private async _initChart() {
    if (!this._canvas) return;
    this._destroyChart();

    const reducedMotion = window.matchMedia(
      "(prefers-reduced-motion: reduce)",
    ).matches;
    const style = getComputedStyle(this);
    const font =
      style.getPropertyValue("--hu-font").trim() || "Avenir, sans-serif";
    const textMuted =
      style.getPropertyValue("--hu-text-muted").trim() || "#888";
    const border =
      style.getPropertyValue("--hu-border-subtle").trim() || "#333";

    const categoricalColors = [
      style.getPropertyValue("--hu-chart-categorical-1").trim(),
      style.getPropertyValue("--hu-chart-categorical-2").trim(),
      style.getPropertyValue("--hu-chart-categorical-3").trim(),
      style.getPropertyValue("--hu-chart-categorical-4").trim(),
      style.getPropertyValue("--hu-chart-categorical-5").trim(),
      style.getPropertyValue("--hu-chart-categorical-6").trim(),
      style.getPropertyValue("--hu-chart-categorical-7").trim(),
      style.getPropertyValue("--hu-chart-categorical-8").trim(),
    ].filter(Boolean);

    const fallbackColors = [
      "#4caf50",
      "#5c6bc0",
      "#ffa726",
      "#ef5350",
      "#26a69a",
      "#7986cb",
      "#ffca28",
      "#66bb6a",
    ];
    const palette =
      categoricalColors.length >= 2 ? categoricalColors : fallbackColors;

    try {
      const { Chart, registerables } = await import(
        /* @vite-ignore */ "https://esm.sh/chart.js@4"
      );
      Chart.register(...registerables);

      const chartType = this.type === "area" ? "line" : this.type;
      const datasets = this.data.datasets.map((ds, i) => ({
        label: ds.label || `Series ${i + 1}`,
        data: ds.data,
        backgroundColor:
          ds.backgroundColor || ds.color || palette[i % palette.length],
        borderColor: ds.color || palette[i % palette.length],
        borderWidth: chartType === "line" ? 2 : 0,
        fill: this.type === "area",
        tension: 0.3,
        pointRadius: 0,
        pointHoverRadius: 4,
      }));

      this._chartInstance = new Chart(this._canvas, {
        type: chartType,
        data: { labels: this.data.labels, datasets },
        options: {
          responsive: true,
          maintainAspectRatio: false,
          indexAxis: this.horizontal ? "y" : "x",
          animation: reducedMotion ? false : { duration: 600 },
          plugins: {
            legend: {
              display: this.data.datasets.length > 1,
              labels: { font: { family: font }, color: textMuted },
            },
            tooltip: {
              backgroundColor: "rgba(0,0,0,0.8)",
              titleFont: { family: font },
              bodyFont: { family: font },
              cornerRadius: 6,
              padding: 8,
            },
          },
          scales:
            chartType === "doughnut"
              ? {}
              : {
                  x: {
                    grid: { color: border },
                    ticks: {
                      font: { family: font, size: 11 },
                      color: textMuted,
                    },
                  },
                  y: {
                    grid: { color: border },
                    ticks: {
                      font: { family: font, size: 11 },
                      color: textMuted,
                    },
                  },
                },
        },
      });
    } catch {
      // Chart.js not available — canvas stays empty
    }
  }

  private _destroyChart() {
    if (
      this._chartInstance &&
      typeof (this._chartInstance as any).destroy === "function"
    ) {
      (this._chartInstance as any).destroy();
      this._chartInstance = null;
    }
  }

  private _resize() {
    if (
      this._chartInstance &&
      typeof (this._chartInstance as any).resize === "function"
    ) {
      (this._chartInstance as any).resize();
    }
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-chart": ScChart;
  }
}
```

**Step 4: Run tests, verify pass**

Run: `npx vitest run -- extra-components`
Expected: PASS

**Step 5: Commit**

```bash
git add ui/src/components/hu-chart.ts ui/src/components/extra-components.test.ts
git commit -m "feat(ui): add hu-chart component with Chart.js integration"
```

---

### Task 2: Create hu-json-viewer component

**Files:**

- Create: `ui/src/components/hu-json-viewer.ts`
- Modify: `ui/src/components/extra-components.test.ts` (append tests)

**Step 1: Write failing tests**

Append to `ui/src/components/extra-components.test.ts`:

```typescript
import "./hu-json-viewer.js";

describe("hu-json-viewer", () => {
  it("should be defined as a custom element", () => {
    expect(customElements.get("hu-json-viewer")).toBeDefined();
  });

  it("should be creatable", () => {
    const el = document.createElement("hu-json-viewer");
    expect(el).toBeInstanceOf(HTMLElement);
  });

  it("should accept data property", () => {
    const el = document.createElement("hu-json-viewer") as any;
    el.data = { key: "value" };
    expect(el.data).toEqual({ key: "value" });
  });

  it("should default expandedDepth to 2", () => {
    const el = document.createElement("hu-json-viewer") as any;
    expect(el.expandedDepth).toBe(2);
  });

  it("should render primitive values inline", async () => {
    const el = document.createElement("hu-json-viewer") as any;
    el.data = "hello";
    document.body.appendChild(el);
    await el.updateComplete;
    const text = el.shadowRoot?.textContent;
    expect(text).toContain('"hello"');
    el.remove();
  });

  it("should render object keys", async () => {
    const el = document.createElement("hu-json-viewer") as any;
    el.data = { name: "test" };
    document.body.appendChild(el);
    await el.updateComplete;
    const text = el.shadowRoot?.textContent;
    expect(text).toContain("name");
    el.remove();
  });

  it("should render array length indicator", async () => {
    const el = document.createElement("hu-json-viewer") as any;
    el.data = [1, 2, 3];
    document.body.appendChild(el);
    await el.updateComplete;
    const text = el.shadowRoot?.textContent;
    expect(text).toContain("3");
    el.remove();
  });
});
```

**Step 2: Run tests, verify failure**

Run: `npx vitest run -- extra-components`
Expected: FAIL — cannot resolve `./hu-json-viewer.js`

**Step 3: Implement hu-json-viewer**

Create `ui/src/components/hu-json-viewer.ts` — a collapsible tree component that renders any JSON-serializable value. Uses `--hu-font-mono`, semantic colors for types (strings = accent, numbers = amber, booleans = indigo, null = muted). Collapsed past `expandedDepth`. Copy-to-clipboard on right-click. Arrow key navigation for expand/collapse.

**Step 4: Run tests, verify pass**

Run: `npx vitest run -- extra-components`
Expected: PASS

**Step 5: Commit**

```bash
git add ui/src/components/hu-json-viewer.ts ui/src/components/extra-components.test.ts
git commit -m "feat(ui): add hu-json-viewer collapsible tree component"
```

---

### Task 3: Rewrite usage-view

**Files:**

- Modify: `ui/src/views/usage-view.ts`
- Modify: `ui/src/views/views.test.ts` (update usage view tests)

**Step 1: Write/update failing tests**

In `ui/src/views/views.test.ts`, update the usage-view describe block to add:

```typescript
it("should render hu-chart elements when data is present", async () => {
  // Test that the view renders chart components
});

it("should render segmented control for time range", async () => {
  // Test that time range selector exists
});

it("should show empty state when no data", async () => {
  // Test empty state rendering
});
```

**Step 2: Rewrite usage-view.ts**

Full rewrite:

- Import `hu-chart`, `hu-segmented-control`
- Top: 3 stat cards with inline trend sparklines
- Middle: `hu-chart type="area"` for token usage over time, time range toggle (24h/7d/30d)
- Bottom: `hu-chart type="bar" horizontal` for cost breakdown by provider
- Export button
- Proper responsive breakpoints, stagger animations, skeleton loading

**Step 3: Run tests, verify pass**

Run: `npx vitest run`
Expected: All pass

**Step 4: Commit**

```bash
git add ui/src/views/usage-view.ts ui/src/views/views.test.ts
git commit -m "feat(ui): rewrite usage view with real charts and time range selector"
```

---

### Task 4: Rewrite logs-view

**Files:**

- Modify: `ui/src/views/logs-view.ts`
- Modify: `ui/src/views/views.test.ts` (update logs tests)

**Step 1: Rewrite logs-view.ts**

Full rewrite:

- Import `hu-json-viewer`, `hu-segmented-control`
- Full-height layout (flex: 1, no fixed 400px)
- `hu-json-viewer` for payload display instead of `JSON.stringify`
- Level filter chips (All / Chat / Tool / Error / Health) via `hu-segmented-control`
- Pause/resume button
- Relative timestamps with absolute on hover via `title` attribute
- Sticky header with filter + controls
- Log count badge
- Proper empty state

**Step 2: Run tests, verify pass**

Run: `npx vitest run`
Expected: All pass

**Step 3: Commit**

```bash
git add ui/src/views/logs-view.ts ui/src/views/views.test.ts
git commit -m "feat(ui): rewrite logs view with json-viewer, level filters, pause/resume"
```

---

### Task 5: Upgrade overview-view

**Files:**

- Modify: `ui/src/views/overview-view.ts`
- Modify: `ui/src/views/views.test.ts`

**Step 1: Add charts and clickable session rows**

- Import `hu-chart`
- Add `hu-chart type="doughnut"` for channel health
- Make session rows clickable (dispatch navigate event to `#chat:sessionKey`)
- Add activity sparkline

**Step 2: Run tests, verify pass**

Run: `npx vitest run`
Expected: All pass

**Step 3: Commit**

```bash
git add ui/src/views/overview-view.ts ui/src/views/views.test.ts
git commit -m "feat(ui): add charts and clickable sessions to overview"
```

---

## Wave 2: "Find & Act On Anything" — Interactive Tables

### Task 6: Create hu-pagination component

**Files:**

- Create: `ui/src/components/hu-pagination.ts`
- Modify: `ui/src/components/extra-components.test.ts`

**Step 1: Write failing tests**

Tests for: registration, creatable, total/page/pageSize props, page count calculation, fires `hu-page-change`, prev/next buttons, disabled state at boundaries.

**Step 2: Implement hu-pagination**

LitElement component: prev/next buttons, page numbers (collapsed with ellipsis for large sets), page size selector dropdown, "Showing X–Y of Z" label. Uses `--hu-*` tokens. Keyboard navigable.

**Step 3: Run tests, commit**

```bash
git commit -m "feat(ui): add hu-pagination component"
```

---

### Task 7: Create hu-data-table-v2 component

**Files:**

- Create: `ui/src/components/hu-data-table-v2.ts`
- Modify: `ui/src/components/extra-components.test.ts`

**Step 1: Write failing tests**

Tests for: registration, column sorting (click header toggles asc/desc/none), filtering (filterable columns get input), pagination integration, `hu-row-click` event, keyboard navigation (arrow keys), empty state, compact mode, responsive.

**Step 2: Implement hu-data-table-v2**

Extends column definition with `sortable`, `filterable`, `render` function. Internal state for sort column/direction, filter values, current page. Sticky thead. Row hover with `--hu-bg-elevated`. Mobile: collapses to label-value card per row. Keyboard: arrow up/down between rows, Enter fires row-click.

**Step 3: Run tests, commit**

```bash
git commit -m "feat(ui): add hu-data-table-v2 with sort, filter, pagination"
```

---

### Task 8: Rewrite tools-view with data table

**Files:**

- Modify: `ui/src/views/tools-view.ts`
- Modify: `ui/src/views/views.test.ts`

Replace card grid with `hu-data-table-v2`. Row click expands inline `hu-json-viewer` for parameter schema. Single count + search. Remove redundant stats.

```bash
git commit -m "feat(ui): rewrite tools view with interactive data table"
```

---

### Task 9: Rewrite channels-view with data table

**Files:**

- Modify: `ui/src/views/channels-view.ts`
- Modify: `ui/src/views/views.test.ts`

Replace card grid with `hu-data-table-v2` (Name, Status badge, Health, Last Active). Row click opens `hu-sheet` detail. Filter chips via `hu-segmented-control`.

```bash
git commit -m "feat(ui): rewrite channels view with interactive data table"
```

---

### Task 10: Rewrite nodes-view with data table

**Files:**

- Modify: `ui/src/views/nodes-view.ts`
- Modify: `ui/src/views/views.test.ts`

Replace flat cards with `hu-data-table-v2` (Node ID, Status, WS Count, Uptime). Row click detail sheet. Refresh button with timestamp.

```bash
git commit -m "feat(ui): rewrite nodes view with interactive data table"
```

---

### Task 11: Upgrade agents-view

**Files:**

- Modify: `ui/src/views/agents-view.ts`
- Modify: `ui/src/views/views.test.ts`

Sessions list via `hu-data-table-v2` with sort/filter. Clickable rows to `#chat:sessionKey`. Add sessions-per-day bar chart.

```bash
git commit -m "feat(ui): upgrade agents view with data table and chart"
```

---

## Wave 3: "Control Everything" — Forms

### Task 12: Create hu-checkbox component

**Files:**

- Create: `ui/src/components/hu-checkbox.ts`
- Modify: `ui/src/components/extra-components.test.ts`

Tests: registration, checked/indeterminate/disabled/label/error props, fires `hu-change`, keyboard toggle (Space), ARIA attributes.

```bash
git commit -m "feat(ui): add hu-checkbox component"
```

---

### Task 13: Create hu-combobox component

**Files:**

- Create: `ui/src/components/hu-combobox.ts`
- Modify: `ui/src/components/extra-components.test.ts`

Tests: registration, options/value/placeholder props, filters on type, keyboard nav (arrow/Enter/Escape), fires `hu-combobox-change`, free-text mode, ARIA combobox role.

```bash
git commit -m "feat(ui): add hu-combobox autocomplete component"
```

---

### Task 14: Create hu-form-group component

**Files:**

- Create: `ui/src/components/hu-form-group.ts`
- Modify: `ui/src/components/extra-components.test.ts`

Tests: registration, dirty/valid readonly props, validate() method, reset() method, fires `hu-form-submit`, shows inline validation messages.

```bash
git commit -m "feat(ui): add hu-form-group validation coordinator"
```

---

### Task 15: Rewrite config-view

**Files:**

- Modify: `ui/src/views/config-view.ts`
- Modify: `ui/src/views/views.test.ts`

Grouped sections with `hu-form-group`. Inline validation. `hu-combobox` for provider/model. Unsaved changes banner. `hu-code-block` for raw JSON mode.

```bash
git commit -m "feat(ui): rewrite config view with form validation and combobox"
```

---

### Task 16: Rewrite security-view

**Files:**

- Modify: `ui/src/views/security-view.ts`
- Modify: `ui/src/views/views.test.ts`

Replace native `<select>` with `hu-select`. Risk indicator. Editable sections with `hu-switch`. Security score stat card. Pairing management.

```bash
git commit -m "feat(ui): rewrite security view with risk indicators and editable sections"
```

---

### Task 17: Upgrade automations-view

**Files:**

- Modify: `ui/src/views/automations-view.ts`
- Modify: `ui/src/views/views.test.ts`

Modal validation via `hu-form-group`. Run history in `hu-data-table-v2`. Success/failure chart.

```bash
git commit -m "feat(ui): upgrade automations view with validation and run history table"
```

---

## Wave 4: "Delight Everywhere" — Polish

### Task 18: Rewrite models-view

**Files:**

- Modify: `ui/src/views/models-view.ts`
- Modify: `ui/src/views/views.test.ts`

Fix heading to `hu-page-hero`. "Set as default" action. Request distribution doughnut chart. `hu-combobox` for inline editing.

```bash
git commit -m "feat(ui): rewrite models view with proper hero, actions, and chart"
```

---

### Task 19: Upgrade voice-view

**Files:**

- Modify: `ui/src/views/voice-view.ts`
- Modify: `ui/src/views/views.test.ts`

Fix duration stat. Export conversation. Replace `hu-message-stream` with `hu-message-thread`. Session management.

```bash
git commit -m "feat(ui): upgrade voice view with thread, export, and session management"
```

---

### Task 20: Upgrade skills-view

**Files:**

- Modify: `ui/src/views/skills-view.ts`
- Modify: `ui/src/views/views.test.ts`

`hu-json-viewer` for parameters. Tag filter for installed skills. URL install validation.

```bash
git commit -m "feat(ui): upgrade skills view with json-viewer and tag filtering"
```

---

### Task 21: Cross-view consistency pass

**Files:**

- Audit and touch all 14 view files

Checklist:

- [ ] Every view: stagger animation on load (`hu-stagger` class)
- [ ] Every view: skeleton shapes matching content layout
- [ ] Every view: empty state with actionable CTA
- [ ] Every view: responsive at sm/md/lg breakpoints
- [ ] Every stat card: trend indicator (up/down/flat)
- [ ] Every list/table: "no results" state
- [ ] Every view: `view-transition-name` CSS for route animations
- [ ] No raw hex colors, pixel spacing, or pixel radii anywhere

```bash
git commit -m "feat(ui): cross-view consistency pass — stagger, skeletons, empty states, transitions"
```

---

## Validation

After each wave, run:

```bash
cd ui && npx vitest run           # all UI tests pass
cd ui && npx tsc --noEmit         # zero type errors
cd ui && npx prettier --check "src/**/*.ts"  # formatting clean
cd ui && npx eslint src/          # zero lint errors
```

After all waves, run full project validation:

```bash
cd /path/to/project
cmake --build build -j$(sysctl -n hw.ncpu) && ./build/human_tests  # C tests still pass
```
