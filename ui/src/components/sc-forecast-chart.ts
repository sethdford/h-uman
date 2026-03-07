import { LitElement, html, css, nothing } from "lit";
import { customElement, property, state } from "lit/decorators.js";

interface DailyCost {
  date: string;
  cost: number;
}

@customElement("sc-forecast-chart")
export class ScForecastChart extends LitElement {
  @property({ type: Array }) history: DailyCost[] = [];
  @property({ type: Number }) projectedTotal = 0;
  @property({ type: Number }) daysInMonth = 31;

  @state() private _hoverDay = -1;
  @state() private _tooltipX = 0;
  @state() private _tooltipY = 0;
  @state() private _hoverValue = 0;
  @state() private _hoverIsProjected = false;

  private readonly PL = 56;
  private readonly PR = 48;
  private readonly PT = 28;
  private readonly PB = 36;
  private readonly VW = 720;
  private readonly VH = 240;

  private get cw() {
    return this.VW - this.PL - this.PR;
  }
  private get ch() {
    return this.VH - this.PT - this.PB;
  }

  static override styles = css`
    :host {
      display: block;
      position: relative;
    }
    svg {
      width: 100%;
      height: auto;
      display: block;
    }
    .grid-line {
      stroke: var(--sc-border-subtle);
      stroke-width: 0.5;
    }
    .axis-label {
      fill: var(--sc-text-muted);
      font-size: var(--sc-text-xs);
      font-family: var(--sc-font);
    }
    .actual-line {
      fill: none;
      stroke: var(--sc-accent);
      stroke-width: 2.5;
      stroke-linecap: round;
      stroke-linejoin: round;
    }
    .projected-line {
      fill: none;
      stroke: var(--sc-accent);
      stroke-width: 2;
      stroke-linecap: round;
      stroke-linejoin: round;
      stroke-dasharray: 8 5;
      opacity: 0.45;
    }
    .today-line {
      stroke: var(--sc-text-muted);
      stroke-width: 1;
      stroke-dasharray: 4 3;
      opacity: 0.35;
    }
    .hover-line {
      stroke: var(--sc-text-muted);
      stroke-width: 1;
      opacity: 0.25;
    }
    .hover-dot {
      fill: var(--sc-accent);
      filter: drop-shadow(0 0 6px var(--sc-accent));
    }
    .end-dot {
      fill: var(--sc-accent);
      opacity: 0.5;
    }
    .end-label {
      fill: var(--sc-text);
      font-size: 12px;
      font-weight: 600;
      font-family: var(--sc-font);
      font-variant-numeric: tabular-nums;
    }
    .today-label {
      fill: var(--sc-text-muted);
      font-size: 10px;
      font-family: var(--sc-font);
      text-transform: uppercase;
      letter-spacing: 0.04em;
    }
    .tooltip {
      position: absolute;
      background: color-mix(in srgb, var(--sc-bg-overlay) 90%, transparent);
      backdrop-filter: blur(12px);
      -webkit-backdrop-filter: blur(12px);
      border: 1px solid
        var(--sc-glass-border-color, color-mix(in srgb, var(--sc-text) 8%, transparent));
      border-radius: var(--sc-radius-md);
      padding: var(--sc-space-xs) var(--sc-space-sm);
      font-size: var(--sc-text-xs);
      color: var(--sc-text);
      pointer-events: none;
      white-space: nowrap;
      z-index: var(--sc-z-tooltip);
      transition: opacity var(--sc-duration-fast) var(--sc-ease-out);
    }
    .tooltip-date {
      color: var(--sc-text-muted);
      margin-bottom: var(--sc-space-2xs);
    }
    .tooltip-value {
      font-weight: var(--sc-weight-semibold);
      font-variant-numeric: tabular-nums;
    }
    .tooltip-projected {
      color: var(--sc-text-muted);
      font-size: 10px;
      font-style: italic;
    }
    @media (prefers-reduced-motion: reduce) {
      .tooltip {
        transition: none;
      }
    }
  `;

  private _getCumulative(): { actual: number[]; projected: number[] } {
    let sum = 0;
    const actual = this.history.map((d) => {
      sum += d.cost;
      return sum;
    });

    const n = this.history.length;
    const remaining = this.daysInMonth - n;
    const dailyAvg = n > 0 ? sum / n : 0;

    const projected: number[] = [];
    let projSum = sum;
    for (let i = 0; i < remaining; i++) {
      projSum += dailyAvg;
      projected.push(projSum);
    }

    return { actual, projected };
  }

  private _dayX(day: number): number {
    return this.PL + ((day - 1) / Math.max(this.daysInMonth - 1, 1)) * this.cw;
  }

  private _valY(val: number, max: number): number {
    if (max <= 0) return this.PT + this.ch;
    return this.PT + this.ch - (val / max) * this.ch;
  }

  private _pathFromPoints(points: { x: number; y: number }[]): string {
    if (points.length < 2) return "";
    return points
      .map((p, i) => `${i === 0 ? "M" : "L"}${p.x.toFixed(1)},${p.y.toFixed(1)}`)
      .join(" ");
  }

  private _areaFromPoints(points: { x: number; y: number }[], bottom: number): string {
    if (points.length < 2) return "";
    const line = this._pathFromPoints(points);
    const last = points[points.length - 1];
    const first = points[0];
    return `${line} L${last.x.toFixed(1)},${bottom.toFixed(1)} L${first.x.toFixed(1)},${bottom.toFixed(1)} Z`;
  }

  private _niceMax(val: number): number {
    if (val <= 5) return Math.ceil(val);
    if (val <= 10) return Math.ceil(val / 2) * 2;
    if (val <= 50) return Math.ceil(val / 10) * 10;
    if (val <= 100) return Math.ceil(val / 20) * 20;
    if (val <= 500) return Math.ceil(val / 100) * 100;
    return Math.ceil(val / 200) * 200;
  }

  private _getYTicks(max: number): number[] {
    const step =
      max <= 5 ? 1 : max <= 10 ? 2 : max <= 50 ? 10 : max <= 100 ? 20 : max <= 500 ? 100 : 200;
    const ticks: number[] = [];
    for (let v = 0; v <= max; v += step) ticks.push(v);
    return ticks;
  }

  private _getXLabels(): number[] {
    const d = this.daysInMonth;
    if (d <= 10) return Array.from({ length: d }, (_, i) => i + 1);
    return [1, 5, 10, 15, 20, 25, d];
  }

  private _formatDay(day: number): string {
    const now = new Date();
    const month = now.toLocaleString("en-US", { month: "short" });
    return `${month} ${day}`;
  }

  private _updateHover(clientX: number): void {
    const svg = this.renderRoot.querySelector("svg");
    if (!svg || this.history.length < 2) return;

    const hostRect = this.getBoundingClientRect();
    const svgRect = svg.getBoundingClientRect();
    const ratioX = (clientX - svgRect.left) / svgRect.width;
    const svgX = ratioX * this.VW;

    const day = Math.round(1 + ((svgX - this.PL) / this.cw) * (this.daysInMonth - 1));
    const clamped = Math.max(1, Math.min(day, this.daysInMonth));

    const { actual, projected } = this._getCumulative();
    const n = this.history.length;
    const allValues = [...actual, ...projected];
    const niceMax = this._niceMax(Math.max(...allValues, 1));

    const isProjected = clamped > n;
    const value = isProjected
      ? (projected[clamped - n - 1] ?? projected[projected.length - 1] ?? 0)
      : (actual[clamped - 1] ?? 0);

    const ptX = this._dayX(clamped);
    const ptY = this._valY(value, niceMax);

    this._hoverDay = clamped;
    this._hoverValue = value;
    this._hoverIsProjected = isProjected;
    this._tooltipX = svgRect.left - hostRect.left + (ptX / this.VW) * svgRect.width;
    this._tooltipY = svgRect.top - hostRect.top + (ptY / this.VH) * svgRect.height - 36;
  }

  private _handleMouseMove(e: MouseEvent): void {
    this._updateHover(e.clientX);
  }

  private _handleTouchMove(e: TouchEvent): void {
    const touch = e.touches[0];
    if (touch) {
      e.preventDefault();
      this._updateHover(touch.clientX);
    }
  }

  private _handleMouseLeave(): void {
    this._hoverDay = -1;
  }

  override render() {
    if (this.history.length < 2) return nothing;

    const { actual, projected } = this._getCumulative();
    const allValues = [...actual, ...projected];
    const niceMax = this._niceMax(Math.max(...allValues, 1));
    const actualDays = this.history.length;
    const bottom = this.PT + this.ch;

    const actualPoints = actual.map((v, i) => ({
      x: this._dayX(i + 1),
      y: this._valY(v, niceMax),
    }));

    const projPoints = [
      actualPoints[actualPoints.length - 1],
      ...projected.map((v, i) => ({
        x: this._dayX(actualDays + i + 1),
        y: this._valY(v, niceMax),
      })),
    ];

    const actualLine = this._pathFromPoints(actualPoints);
    const actualArea = this._areaFromPoints(actualPoints, bottom);
    const projLine = this._pathFromPoints(projPoints);
    const projArea = this._areaFromPoints(projPoints, bottom);

    const gridTicks = this._getYTicks(niceMax);
    const xLabels = this._getXLabels();
    const todayX = this._dayX(actualDays);
    const endPoint = projPoints[projPoints.length - 1];

    const hoverDay = this._hoverDay;

    return html`
      <svg
        viewBox="0 0 ${this.VW} ${this.VH}"
        aria-label="Monthly cost forecast chart"
        role="img"
        @mousemove=${this._handleMouseMove}
        @mouseleave=${this._handleMouseLeave}
        @touchmove=${this._handleTouchMove}
        @touchend=${this._handleMouseLeave}
      >
        <defs>
          <linearGradient id="fc-actual-grad" x1="0" y1="0" x2="0" y2="1">
            <stop offset="0" stop-color="var(--sc-accent)" stop-opacity="0.25" />
            <stop offset="1" stop-color="var(--sc-accent)" stop-opacity="0.02" />
          </linearGradient>
          <linearGradient id="fc-proj-grad" x1="0" y1="0" x2="0" y2="1">
            <stop offset="0" stop-color="var(--sc-accent)" stop-opacity="0.08" />
            <stop offset="1" stop-color="var(--sc-accent)" stop-opacity="0" />
          </linearGradient>
        </defs>

        ${gridTicks.map(
          (tick) => html`
            <line
              class="grid-line"
              x1=${this.PL}
              y1=${this._valY(tick, niceMax)}
              x2=${this.VW - this.PR}
              y2=${this._valY(tick, niceMax)}
            />
            <text
              class="axis-label"
              x=${this.PL - 8}
              y=${this._valY(tick, niceMax) + 4}
              text-anchor="end"
            >
              $${tick}
            </text>
          `,
        )}
        ${xLabels.map(
          (day) => html`
            <text class="axis-label" x=${this._dayX(day)} y=${bottom + 20} text-anchor="middle"
              >${day}</text
            >
          `,
        )}

        <path d=${projArea} fill="url(#fc-proj-grad)" />
        <path class="projected-line" d=${projLine} />

        <path d=${actualArea} fill="url(#fc-actual-grad)" />
        <path class="actual-line" d=${actualLine} />

        <line class="today-line" x1=${todayX} y1=${this.PT} x2=${todayX} y2=${bottom} />
        <text class="today-label" x=${todayX} y=${this.PT - 10} text-anchor="middle">Today</text>

        <circle class="end-dot" cx=${endPoint.x} cy=${endPoint.y} r="4" />
        <text class="end-label" x=${endPoint.x + 10} y=${endPoint.y + 5}>
          $${Math.round(this.projectedTotal)}
        </text>

        ${hoverDay > 0
          ? html`
              <line
                class="hover-line"
                x1=${this._dayX(hoverDay)}
                y1=${this.PT}
                x2=${this._dayX(hoverDay)}
                y2=${bottom}
              />
              <circle
                class="hover-dot"
                cx=${this._dayX(hoverDay)}
                cy=${this._valY(this._hoverValue, niceMax)}
                r="5"
              />
            `
          : nothing}
      </svg>
      ${hoverDay > 0
        ? html`
            <div
              class="tooltip"
              style="left: ${this._tooltipX}px; top: ${this
                ._tooltipY}px; transform: translateX(-50%)"
            >
              <div class="tooltip-date">${this._formatDay(hoverDay)}</div>
              <div class="tooltip-value">$${this._hoverValue.toFixed(2)}</div>
              ${this._hoverIsProjected
                ? html`<div class="tooltip-projected">projected</div>`
                : nothing}
            </div>
          `
        : nothing}
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-forecast-chart": ScForecastChart;
  }
}
