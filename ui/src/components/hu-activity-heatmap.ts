import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";

@customElement("hu-activity-heatmap")
export class HuActivityHeatmap extends LitElement {
  @property({ attribute: false }) data: number[] = [];
  @property({ type: Number }) weeks = 12;
  @property({ type: Number }) cellSize = 10;
  @property({ type: Number }) gap = 2;

  static override styles = css`
    :host {
      display: block;
    }

    .heatmap {
      display: flex;
      gap: var(--hu-space-2xs);
      overflow-x: auto;
      scroll-snap-type: x proximity;
      scrollbar-width: none;
    }

    .heatmap::-webkit-scrollbar {
      display: none;
    }

    .week {
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-2xs);
    }

    .cell {
      border-radius: var(--hu-radius-sm);
      transition: transform var(--hu-duration-instant) var(--hu-ease-out);
    }

    .cell:hover {
      transform: scale(1.4);
      z-index: 1;
    }

    .legend {
      display: flex;
      align-items: center;
      gap: var(--hu-space-xs);
      margin-top: var(--hu-space-sm);
      font-family: var(--hu-font);
      font-size: var(--hu-text-xs);
      color: var(--hu-text-muted);
    }

    .legend-cell {
      width: var(--cell-size, var(--hu-space-sm));
      height: var(--cell-size, var(--hu-space-sm));
      border-radius: var(--hu-radius-sm);
    }

    @media (prefers-reduced-motion: reduce) {
      .cell {
        transition: none;
      }
    }
  `;

  private _getColor(value: number, max: number): string {
    if (value === 0) return "var(--hu-surface-container)";
    const intensity = Math.min(value / Math.max(max, 1), 1);
    if (intensity < 0.25) return "var(--hu-chart-sequential-200)";
    if (intensity < 0.5) return "var(--hu-chart-sequential-400)";
    if (intensity < 0.75) return "var(--hu-chart-sequential-600)";
    return "var(--hu-chart-sequential-800)";
  }

  override render() {
    this.style.setProperty("--cell-size", `${this.cellSize}px`);
    const totalCells = this.weeks * 7;
    const cells =
      this.data.length >= totalCells
        ? this.data.slice(-totalCells)
        : [...Array(totalCells - this.data.length).fill(0), ...this.data];
    const max = Math.max(...cells, 1);

    const weeks: number[][] = [];
    for (let w = 0; w < this.weeks; w++) {
      weeks.push(cells.slice(w * 7, (w + 1) * 7));
    }

    return html`
      <div
        class="heatmap"
        role="img"
        aria-label="Activity heatmap showing ${this.weeks} weeks of AI agent activity"
      >
        ${weeks.map(
          (week) => html`
            <div class="week">
              ${week.map(
                (val) => html`
                  <div
                    class="cell"
                    style="width:${this.cellSize}px;height:${
                      this.cellSize
                    }px;background:${this._getColor(val, max)}"
                    title="${val} actions"
                  ></div>
                `,
              )}
            </div>
          `,
        )}
      </div>
      <div class="legend">
        <span>Less</span>
        <div class="legend-cell" style="background:var(--hu-surface-container)"></div>
        <div class="legend-cell" style="background:var(--hu-chart-sequential-200)"></div>
        <div class="legend-cell" style="background:var(--hu-chart-sequential-400)"></div>
        <div class="legend-cell" style="background:var(--hu-chart-sequential-600)"></div>
        <div class="legend-cell" style="background:var(--hu-chart-sequential-800)"></div>
        <span>More</span>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-activity-heatmap": HuActivityHeatmap;
  }
}
