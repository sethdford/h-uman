import { LitElement, html, css, nothing } from "lit";
import { customElement, property } from "lit/decorators.js";

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

/**
 * Matches `chart.categorical` count in `design-tokens/data-viz.tokens.json`.
 * Exported for tests and docs; keep in sync when adding series.
 */
export const HU_CHART_CATEGORICAL_SERIES_COUNT = 16;
const CATEGORICAL_SERIES_COUNT = HU_CHART_CATEGORICAL_SERIES_COUNT;

/** Matches `chart.sequential` keys in `design-tokens/data-viz.tokens.json`. */
const SEQUENTIAL_STOP_KEYS = [100, 200, 300, 400, 500, 600, 700, 800] as const;

const CATEGORICAL_FALLBACKS = [
  "var(--hu-chart-categorical-1, hsl(90 45% 50%))",
  "var(--hu-chart-categorical-2, hsl(239 84% 67%))",
  "var(--hu-chart-categorical-3, hsl(38 92% 50%))",
  "var(--hu-chart-categorical-4, hsl(0 91% 71%))",
  "var(--hu-chart-categorical-5, hsl(168 76% 42%))",
  "var(--hu-chart-categorical-6, hsl(235 90% 82%))",
  "var(--hu-chart-categorical-7, hsl(48 96% 59%))",
  "var(--hu-chart-categorical-8, hsl(90 45% 62%))",
  "var(--hu-chart-categorical-9, hsl(158 100% 24%))",
  "var(--hu-chart-categorical-10, hsl(198 100% 44%))",
  "var(--hu-chart-categorical-11, hsl(271 50% 58%))",
  "var(--hu-chart-categorical-12, hsl(64 71% 47%))",
  "var(--hu-chart-categorical-13, hsl(52 98% 48%))",
  "var(--hu-chart-categorical-14, hsl(24 100% 50%))",
  "var(--hu-chart-categorical-15, hsl(0 100% 40%))",
  "var(--hu-chart-categorical-16, hsl(205 65% 48%))",
];

@customElement("hu-chart")
export class ScChart extends LitElement {
  @property({ type: String }) type: "bar" | "line" | "area" | "doughnut" = "bar";
  @property({ attribute: false }) data: ChartData = { labels: [], datasets: [] };
  @property({ type: Number }) height = 200;
  @property({ type: Boolean }) horizontal = false;
  @property({ type: Boolean }) hideLegend = false;

  private _chart: {
    destroy: () => void;
    resize: () => void;
    data: { labels: unknown[]; datasets: unknown[] };
    update: (mode?: string) => void;
  } | null = null;
  private _resizeObserver: ResizeObserver | null = null;
  private _chartLoadPromise: Promise<unknown> | null = null;
  private _chartUnavailable = false;
  /** Serializes async init/patch so rapid updates do not double-construct Chart.js on one canvas. */
  private _chartWorkChain: Promise<void> = Promise.resolve();

  static override styles = css`
    :host {
      display: block;
      position: relative;
    }
    .wrapper {
      width: 100%;
      position: relative;
    }
    .wrapper canvas {
      display: block;
    }
    .doughnut-center {
      position: absolute;
      top: 50%;
      left: 50%;
      transform: translate(-50%, -50%);
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      pointer-events: none;
      font-family: var(--hu-font);
      font-size: var(--hu-text-lg);
      font-weight: var(--hu-weight-semibold);
      color: var(--hu-text);
      font-variant-numeric: tabular-nums;
    }
    .empty {
      display: flex;
      align-items: center;
      justify-content: center;
      min-height: var(--height, 200px);
      font-family: var(--hu-font);
      font-size: var(--hu-text-sm);
      color: var(--hu-text-muted);
    }
    @media (prefers-reduced-motion: reduce) {
      .wrapper canvas {
        /* Chart.js animations disabled via options */
      }
    }
  `;

  override connectedCallback(): void {
    super.connectedCallback();
    // Use the npm package so Vitest/Node can resolve the module (https: imports fail there).
    this._chartLoadPromise = import("chart.js/auto").catch(() => {
      this._chartUnavailable = true;
      this.requestUpdate();
      return null;
    });
  }

  override disconnectedCallback(): void {
    this._destroyChart();
    this._resizeObserver?.disconnect();
    this._resizeObserver = null;
    super.disconnectedCallback();
  }

  override updated(changed: Map<string, unknown>): void {
    if (!this._hasData()) {
      this._destroyChart();
      return;
    }

    const structChanged =
      changed.has("type") ||
      changed.has("height") ||
      changed.has("horizontal") ||
      changed.has("hideLegend");

    // Chart.destroy() can remove the canvas from the DOM. Re-render first so Lit restores
    // a fresh <canvas>, then init on the next updated pass (see !this._chart branch).
    if (this._chart && structChanged) {
      this._destroyChart();
      this.requestUpdate();
      return;
    }

    if (!this._chart) {
      this._enqueueChartWork(async () => {
        if (!this._hasData() || this._chartUnavailable) return;
        if (this._chart) {
          this._patchChartData();
          return;
        }
        await this._initChart();
      });
      return;
    }

    if (changed.has("data")) {
      this._patchChartData();
    }
  }

  private _enqueueChartWork(work: () => Promise<void>): void {
    this._chartWorkChain = this._chartWorkChain.then(work).catch(() => {});
  }

  private _patchChartData(): void {
    const chart = this._chart;
    if (!chart || !this._hasData()) return;
    chart.data.labels = [...(this.data?.labels ?? [])];
    chart.data.datasets = this._buildChartDatasets() as never[];
    const reducedMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
    chart.update(reducedMotion ? "none" : "default");
  }

  private _hasData(): boolean {
    const ds = this.data?.datasets;
    return Array.isArray(ds) && ds.length > 0;
  }

  private _getCategoricalColor(index: number): string {
    const slot = (index % CATEGORICAL_SERIES_COUNT) + 1;
    const token = `--hu-chart-categorical-${slot}`;
    const val = getComputedStyle(this).getPropertyValue(token).trim();
    return val || CATEGORICAL_FALLBACKS[index % CATEGORICAL_SERIES_COUNT];
  }

  private _getSequentialColor(stopKey: (typeof SEQUENTIAL_STOP_KEYS)[number]): string {
    const token = `--hu-chart-sequential-${stopKey}`;
    const val = getComputedStyle(this).getPropertyValue(token).trim();
    return val || `var(${token})`;
  }

  private _buildSequentialPalette(length: number): string[] {
    if (length <= 0) return [];
    if (length === 1) return [this._getSequentialColor(500)];
    const maxK = SEQUENTIAL_STOP_KEYS.length - 1;
    return Array.from({ length }, (_, i) => {
      const t = i / (length - 1);
      const idx = Math.round(t * maxK);
      const key = SEQUENTIAL_STOP_KEYS[Math.min(maxK, Math.max(0, idx))];
      return this._getSequentialColor(key);
    });
  }

  private _maybeSequentialPalette(
    ds: ChartDataset,
    datasetCount: number,
    datasetIndex: number,
  ): string[] | null {
    if (datasetIndex !== 0 || datasetCount !== 1) return null;
    if (this.type !== "bar" && this.type !== "doughnut") return null;
    if (ds.color != null || ds.backgroundColor != null) return null;
    const n = ds.data?.length ?? 0;
    if (n <= 1) return null;
    return this._buildSequentialPalette(n);
  }

  private _buildChartDatasets(): Array<Record<string, unknown>> {
    const datasets = this.data?.datasets ?? [];
    const isArea = this.type === "area";
    const chartType = isArea ? "line" : this.type;

    return datasets.map((ds, i) => {
      const seq = this._maybeSequentialPalette(ds, datasets.length, i);
      const color = ds.color ?? ds.backgroundColor ?? this._getCategoricalColor(i);
      const fillAndStroke = seq ?? color;
      const base: Record<string, unknown> = {
        label: ds.label,
        data: ds.data ?? [],
        backgroundColor: ds.backgroundColor ?? fillAndStroke,
        borderColor: fillAndStroke,
        borderWidth: chartType === "line" || isArea ? 2 : 1,
      };
      if (isArea) {
        base.fill = true;
        base.tension = 0.3;
      }
      return base;
    });
  }

  private async _initChart(): Promise<void> {
    const canvas = this.renderRoot.querySelector("canvas");
    if (!canvas || !this._hasData()) return;

    type ChartInstance = {
      destroy(): void;
      resize(): void;
      data: { labels: unknown[]; datasets: unknown[] };
      update(mode?: string): void;
    };
    type ChartCtor = new (el: HTMLCanvasElement, config: unknown) => ChartInstance;

    const mod = (await this._chartLoadPromise) as { default?: ChartCtor; Chart?: ChartCtor } | null;
    // chart.js ships named `Chart` (no default); support both shapes for compatibility.
    const Chart = mod?.default ?? mod?.Chart;
    if (!Chart) {
      this._chartUnavailable = true;
      this.requestUpdate();
      return;
    }
    const reducedMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
    const cs = getComputedStyle(this);
    const fontFamily = cs.getPropertyValue("--hu-font").trim() || "Avenir, system-ui, sans-serif";
    const textMuted = cs.getPropertyValue("--hu-text-muted").trim() || "hsl(207 24% 47%)";
    const borderSubtle =
      cs.getPropertyValue("--hu-border-subtle").trim() ||
      "color-mix(in srgb, var(--hu-color-white) 6%, transparent)";

    const chartType = this.type === "area" ? "line" : this.type;
    const config = {
      type: chartType,
      data: {
        labels: this.data?.labels ?? [],
        datasets: this._buildChartDatasets(),
      },
      options: {
        responsive: true,
        maintainAspectRatio: false,
        animation: reducedMotion ? false : undefined,
        transitions: reducedMotion ? { active: { animation: { duration: 0 } } } : undefined,
        indexAxis: this.horizontal ? "y" : "x",
        plugins: {
          legend: {
            display: !this.hideLegend,
            labels: {
              font: { family: fontFamily },
              color: textMuted,
            },
          },
        },
        scales:
          chartType !== "doughnut"
            ? {
                x: {
                  grid: { color: borderSubtle },
                  ticks: {
                    font: { family: fontFamily },
                    color: textMuted,
                  },
                },
                y: {
                  grid: { color: borderSubtle },
                  ticks: {
                    font: { family: fontFamily },
                    color: textMuted,
                  },
                },
              }
            : undefined,
      },
    };

    this._chart = new Chart(canvas, config);

    const wrapper = this.renderRoot.querySelector(".wrapper");
    if (wrapper) {
      this._resizeObserver?.disconnect();
      this._resizeObserver = new ResizeObserver(() => {
        this._chart?.resize();
      });
      this._resizeObserver.observe(wrapper);
    }
  }

  private _destroyChart(): void {
    this._chart?.destroy();
    this._chart = null;
    this._resizeObserver?.disconnect();
  }

  override render() {
    if (!this._hasData()) {
      return html`
        <div class="empty" style="--height: ${this.height}px" role="status" aria-label="No data">
          No data
        </div>
      `;
    }

    if (this._chartUnavailable) {
      return html`
        <div
          class="empty"
          style="--height: ${this.height}px"
          role="status"
          aria-label="Chart unavailable"
        >
          Chart unavailable
        </div>
      `;
    }

    const total =
      this.type === "doughnut" && this.data?.datasets?.[0]?.data
        ? this.data.datasets[0].data.reduce((a, b) => a + b, 0)
        : 0;
    const showCenter = this.type === "doughnut" && this.hideLegend && total > 0;

    return html`
      <div class="wrapper" style="--height: ${this.height}px; min-height: ${this.height}px">
        <canvas
          role="img"
          aria-label="Chart"
          style="display: block; box-sizing: border-box; height: ${this.height}px; width: 100%"
        ></canvas>
        ${
          showCenter
            ? html`<div class="doughnut-center" aria-hidden="true">${total.toLocaleString()}</div>`
            : nothing
        }
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-chart": ScChart;
  }
}
