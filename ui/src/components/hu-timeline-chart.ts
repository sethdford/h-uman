import { LitElement, html, css, nothing } from "lit";
import { customElement, property, state } from "lit/decorators.js";

export type TimelineBarStatus = "planned" | "active" | "complete";

export interface TimelineBar {
  id: string;
  label: string;
  start: string;
  end: string;
  status?: TimelineBarStatus;
}

@customElement("hu-timeline-chart")
export class HuTimelineChart extends LitElement {
  @property({ attribute: false }) bars: TimelineBar[] = [];
  @property({ type: String }) today = "";
  @property({ type: Number }) rowHeight = 28;
  @property({ type: Number }) paddingLeft = 120;

  @state() private _hover: number | null = null;
  @state() private _tipX = 0;
  @state() private _tipY = 0;

  static override styles = css`
    :host {
      display: block;
      position: relative;
      font-family: var(--hu-font);
    }
    .scroll {
      overflow-x: auto;
      padding-bottom: var(--hu-space-sm);
    }
    svg {
      display: block;
      min-width: 100%;
    }
    .grid {
      stroke: var(--hu-border-subtle);
      stroke-width: 0.5;
    }
    .today {
      stroke: var(--hu-accent-tertiary);
      stroke-width: 1;
      stroke-dasharray: 4 3;
      opacity: 0.7;
    }
    .bar {
      transition: opacity var(--hu-duration-fast) var(--hu-ease-out);
    }
    .bar:hover {
      opacity: 0.92;
    }
    .bar.planned {
      fill: var(--hu-chart-sequential-300);
    }
    .bar.active {
      fill: var(--hu-chart-brand);
    }
    .bar.complete {
      fill: var(--hu-success);
    }
    .label {
      fill: var(--hu-text);
      font-size: var(--hu-text-xs);
    }
    .axis {
      fill: var(--hu-text-muted);
      font-size: var(--hu-text-2xs);
    }
    .tooltip {
      position: fixed;
      z-index: 50;
      padding: var(--hu-space-xs) var(--hu-space-sm);
      background: color-mix(in srgb, var(--hu-bg-overlay) 92%, transparent);
      backdrop-filter: blur(10px);
      border: 1px solid var(--hu-border-subtle);
      border-radius: var(--hu-radius-md);
      font-size: var(--hu-text-xs);
      color: var(--hu-text);
      pointer-events: none;
      max-width: 220px;
      box-shadow: var(--hu-shadow-glass);
    }
  `;

  private _parse(d: string): number {
    const t = Date.parse(d);
    return Number.isFinite(t) ? t : 0;
  }

  private _range(): { min: number; max: number; span: number } {
    let min = Infinity;
    let max = -Infinity;
    for (const b of this.bars) {
      const s = this._parse(b.start);
      const e = this._parse(b.end);
      min = Math.min(min, s, e);
      max = Math.max(max, s, e);
    }
    if (!Number.isFinite(min) || !Number.isFinite(max) || min === max) {
      const now = Date.now();
      return { min: now - 86400000 * 7, max: now + 86400000 * 7, span: 86400000 * 14 };
    }
    const pad = (max - min) * 0.05;
    return { min: min - pad, max: max + pad, span: max - min + 2 * pad };
  }

  private _x(t: number, pl: number, cw: number, range: { min: number; max: number }): number {
    return pl + ((t - range.min) / (range.max - range.min)) * cw;
  }

  override render() {
    const bars = this.bars ?? [];
    if (bars.length === 0) {
      return html`<div role="status" style="color:var(--hu-text-muted);font-size:var(--hu-text-sm)">
        No timeline data
      </div>`;
    }

    const pl = this.paddingLeft;
    const chartW = 560;
    const rh = this.rowHeight;
    const h = bars.length * rh + 40;
    const range = this._range();
    const cw = chartW - pl - 16;
    const todayT = this.today ? this._parse(this.today) : Date.now();
    const todayX =
      todayT >= range.min && todayT <= range.max ? this._x(todayT, pl, cw, range) : null;

    const months: { x: number; label: string }[] = [];
    const step = (range.max - range.min) / 6;
    for (let i = 0; i <= 6; i++) {
      const t = range.min + step * i;
      months.push({
        x: this._x(t, pl, cw, range),
        label: new Date(t).toLocaleDateString(undefined, { month: "short", day: "numeric" }),
      });
    }

    const hoverBar = this._hover != null ? bars[this._hover] : null;

    return html`
      <div class="scroll" role="region" aria-label="Timeline chart">
        <svg width=${chartW} height=${h} viewBox="0 0 ${chartW} ${h}">
          ${months.map(
            (m) => html`
              <line class="grid" x1=${m.x} y1="8" x2=${m.x} y2=${h - 8} />
              <text class="axis" x=${m.x} y=${h - 4} text-anchor="middle">${m.label}</text>
            `,
          )}
          ${
            todayX != null
              ? html`<line class="today" x1=${todayX} y1="8" x2=${todayX} y2=${h - 24} />`
              : nothing
          }
          ${bars.map((b, i) => {
            const y = 12 + i * rh;
            const x1 = this._x(this._parse(b.start), pl, cw, range);
            const x2 = this._x(this._parse(b.end), pl, cw, range);
            const w = Math.max(x2 - x1, 4);
            const st = b.status ?? "active";
            return html`
              <text class="label" x="8" y=${y + rh * 0.62}>${b.label}</text>
              <rect
                class="bar ${st}"
                x=${x1}
                y=${y}
                width=${w}
                height=${rh - 8}
                rx="4"
                @mouseenter=${(e: MouseEvent) => {
                  this._hover = i;
                  this._tipX = e.clientX + 12;
                  this._tipY = e.clientY + 12;
                }}
                @mousemove=${(e: MouseEvent) => {
                  this._tipX = e.clientX + 12;
                  this._tipY = e.clientY + 12;
                }}
                @mouseleave=${() => (this._hover = null)}
              />
            `;
          })}
        </svg>
      </div>
      ${
        hoverBar
          ? html`
              <div class="tooltip" style="left:${this._tipX}px;top:${this._tipY}px">
                <strong>${hoverBar.label}</strong><br />
                ${hoverBar.start} → ${hoverBar.end}<br />
                <span style="color:var(--hu-text-muted)">${hoverBar.status ?? "active"}</span>
              </div>
            `
          : nothing
      }
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-timeline-chart": HuTimelineChart;
  }
}
