import { LitElement, html, css } from "lit";
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

const CATEGORICAL_FALLBACKS = [
  "var(--sc-chart-categorical-1, hsl(90 45% 50%))",
  "var(--sc-chart-categorical-2, hsl(239 84% 67%))",
  "var(--sc-chart-categorical-3, hsl(38 92% 50%))",
  "var(--sc-chart-categorical-4, hsl(0 91% 71%))",
  "var(--sc-chart-categorical-5, hsl(168 76% 42%))",
  "var(--sc-chart-categorical-6, hsl(235 90% 82%))",
  "var(--sc-chart-categorical-7, hsl(48 96% 59%))",
  "var(--sc-chart-categorical-8, hsl(90 45% 62%))",
];

@customElement("sc-chart")
export class ScChart extends LitElement {
  @property({ type: String }) type: "bar" | "line" | "area" | "doughnut" = "bar";
  @property({ attribute: false }) data: ChartData = { labels: [], datasets: [] };
  @property({ type: Number }) height = 200;
  @property({ type: Boolean }) horizontal = false;

  private _chart: { destroy: () => void; resize: () => void } | null = null;
  private _resizeObserver: ResizeObserver | null = null;
  private _chartLoadPromise: Promise<unknown> | null = null;
  private _chartUnavailable = false;

  static override styles = css`
    :host {
      display: block;
      position: relative;
    }
    .wrapper {
      width: 100%;
      height: 100%;
      min-height: var(--height, 200px);
    }
    .wrapper canvas {
      width: 100% !important;
      height: auto !important;
    }
    .empty {
      display: flex;
      align-items: center;
      justify-content: center;
      min-height: var(--height, 200px);
      font-family: var(--sc-font);
      font-size: var(--sc-text-sm);
      color: var(--sc-text-muted);
    }
    @media (prefers-reduced-motion: reduce) {
      .wrapper canvas {
        /* Chart.js animations disabled via options */
      }
    }
  `;

  override connectedCallback(): void {
    super.connectedCallback();
    this._chartLoadPromise = import("https://esm.sh/chart.js@4").catch((err) => {
      // eslint-disable-next-line no-console -- CDN load failure must be visible for debugging
      console.warn("[sc-chart] Chart.js failed to load:", err);
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
    if (
      changed.has("type") ||
      changed.has("data") ||
      changed.has("height") ||
      changed.has("horizontal")
    ) {
      this._scheduleChartUpdate();
    }
  }

  private _scheduleChartUpdate(): void {
    if (this._chart) {
      this._destroyChart();
    }
    if (this._hasData()) {
      this._initChart();
    }
  }

  private _hasData(): boolean {
    const ds = this.data?.datasets;
    return Array.isArray(ds) && ds.length > 0;
  }

  private _getCategoricalColor(index: number): string {
    const token = `--sc-chart-categorical-${(index % 8) + 1}`;
    const val = getComputedStyle(this).getPropertyValue(token).trim();
    return val || CATEGORICAL_FALLBACKS[index % 8];
  }

  private _buildChartDatasets(): Array<Record<string, unknown>> {
    const datasets = this.data?.datasets ?? [];
    const isArea = this.type === "area";
    const chartType = isArea ? "line" : this.type;

    return datasets.map((ds, i) => {
      const color = ds.color ?? ds.backgroundColor ?? this._getCategoricalColor(i);
      const base: Record<string, unknown> = {
        label: ds.label,
        data: ds.data ?? [],
        backgroundColor: ds.backgroundColor ?? color,
        borderColor: color,
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

    const ChartModule = (await this._chartLoadPromise) as {
      default?: new (el: HTMLCanvasElement, config: unknown) => { destroy(): void; resize(): void };
    } | null;
    if (!ChartModule?.default) {
      this._chartUnavailable = true;
      this.requestUpdate();
      return;
    }

    const Chart = ChartModule.default;
    const reducedMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
    const cs = getComputedStyle(this);
    const fontFamily = cs.getPropertyValue("--sc-font").trim() || "Avenir, system-ui, sans-serif";
    const textMuted = cs.getPropertyValue("--sc-text-muted").trim() || "hsl(207 24% 47%)";
    const borderSubtle =
      cs.getPropertyValue("--sc-border-subtle").trim() ||
      "color-mix(in srgb, white 6%, transparent)";

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

    return html`
      <div class="wrapper" style="--height: ${this.height}px; min-height: ${this.height}px">
        <canvas
          role="img"
          aria-label="Chart"
          style="display: block; box-sizing: border-box; height: ${this.height}px; width: 100%"
        ></canvas>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-chart": ScChart;
  }
}
