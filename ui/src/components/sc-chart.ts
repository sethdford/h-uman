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

const CATEGORICAL_FALLBACKS = [
  "#7ab648",
  "#6366f1",
  "#f59e0b",
  "#f87171",
  "#14b8a6",
  "#a5b4fc",
  "#fcd34d",
  "#a3d46a",
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
    this._chartLoadPromise = import("https://esm.sh/chart.js@4").catch(() => null);
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
    if (!ChartModule?.default) return;

    const Chart = ChartModule.default;
    const reducedMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;

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
              font: { family: "var(--sc-font)" },
              color: getComputedStyle(this).getPropertyValue("--sc-text-muted").trim() || "#5e7a94",
            },
          },
        },
        scales:
          chartType !== "doughnut"
            ? {
                x: {
                  grid: {
                    color:
                      getComputedStyle(this).getPropertyValue("--sc-border-subtle").trim() ||
                      "rgba(255,255,255,0.06)",
                  },
                  ticks: {
                    font: { family: "var(--sc-font)" },
                    color:
                      getComputedStyle(this).getPropertyValue("--sc-text-muted").trim() ||
                      "#5e7a94",
                  },
                },
                y: {
                  grid: {
                    color:
                      getComputedStyle(this).getPropertyValue("--sc-border-subtle").trim() ||
                      "rgba(255,255,255,0.06)",
                  },
                  ticks: {
                    font: { family: "var(--sc-font)" },
                    color:
                      getComputedStyle(this).getPropertyValue("--sc-text-muted").trim() ||
                      "#5e7a94",
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
