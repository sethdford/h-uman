import { LitElement, html, css, nothing } from "lit";
import { customElement, property, state } from "lit/decorators.js";

@customElement("sc-sparkline-enhanced")
export class ScSparklineEnhanced extends LitElement {
  @property({ type: Array }) data: number[] = [];
  @property({ type: Number }) width = 200;
  @property({ type: Number }) height = 48;
  @property({ type: String }) color = "var(--sc-accent)";
  @property({ type: Boolean }) showTooltip = true;
  @property({ type: Boolean }) fillGradient = true;
  @property({ type: Number }) dotSize = 4;
  @property({ type: String }) tooltipLabel = "";

  @state() private _hoverIndex = -1;
  @state() private _tooltipX = 0;
  @state() private _tooltipY = 0;

  static override styles = css`
    :host {
      display: block;
      position: relative;
    }
    svg {
      width: 100%;
      height: auto;
    }
    .line {
      fill: none;
      stroke: var(--color);
      stroke-width: 2;
      stroke-linecap: round;
      stroke-linejoin: round;
    }
    .area {
      fill: url(#gradient);
    }
    .dot {
      fill: var(--color);
      filter: drop-shadow(0 0 4px currentColor);
    }
    .tooltip {
      position: absolute;
      background: color-mix(in srgb, var(--sc-bg-overlay) 85%, transparent);
      backdrop-filter: blur(12px);
      -webkit-backdrop-filter: blur(12px);
      border: 1px solid var(--sc-glass-border-color, rgba(255, 255, 255, 0.08));
      border-radius: var(--sc-radius-md);
      padding: var(--sc-space-xs) var(--sc-space-sm);
      font-size: var(--sc-text-xs);
      color: var(--sc-text);
      pointer-events: none;
      transition: opacity 150ms;
      white-space: nowrap;
      z-index: var(--sc-z-tooltip);
    }
    @media (prefers-reduced-motion: reduce) {
      .tooltip {
        transition: none;
      }
    }
  `;

  private _buildPath(points: number[]): { line: string; area: string } {
    if (points.length < 2) return { line: "", area: "" };
    const min = Math.min(...points);
    const max = Math.max(...points);
    const range = max - min || 1;
    const pad = 2;
    const w = this.width - pad * 2;
    const h = this.height - pad * 2;

    const coords = points.map((v, i) => ({
      x: pad + (i / (points.length - 1)) * w,
      y: pad + h - ((v - min) / range) * h,
    }));

    const line = coords
      .map((c, i) => `${i === 0 ? "M" : "L"}${c.x.toFixed(1)},${c.y.toFixed(1)}`)
      .join(" ");
    const area = `${line} L${coords[coords.length - 1].x.toFixed(1)},${this.height.toFixed(1)} L${coords[0].x.toFixed(1)},${this.height.toFixed(1)} Z`;
    return { line, area };
  }

  private _getCoords(): { x: number; y: number }[] {
    const pts = this.data.length >= 2 ? this.data : [0, 0];
    const min = Math.min(...pts);
    const max = Math.max(...pts);
    const range = max - min || 1;
    const pad = 2;
    const w = this.width - pad * 2;
    const h = this.height - pad * 2;
    return pts.map((v, i) => ({
      x: pad + (i / (pts.length - 1)) * w,
      y: pad + h - ((v - min) / range) * h,
    }));
  }

  private _handleMouseMove(e: MouseEvent) {
    const svg = this.renderRoot.querySelector("svg");
    if (!svg || !this.showTooltip || this.data.length < 2) return;
    const hostRect = this.getBoundingClientRect();
    const svgRect = svg.getBoundingClientRect();
    const x = e.clientX - svgRect.left;
    const ratio = x / svgRect.width;
    const index = Math.round(ratio * (this.data.length - 1));
    const clamped = Math.max(0, Math.min(index, this.data.length - 1));
    const coords = this._getCoords();
    const pt = coords[clamped];
    if (pt) {
      this._hoverIndex = clamped;
      this._tooltipX = svgRect.left - hostRect.left + (pt.x / this.width) * svgRect.width;
      this._tooltipY = svgRect.top - hostRect.top + (pt.y / this.height) * svgRect.height - 28;
    }
  }

  private _handleMouseLeave() {
    this._hoverIndex = -1;
  }

  override render() {
    const pts = this.data.length >= 2 ? this.data : [0, 0];
    const { line, area } = this._buildPath(pts);
    const last = pts[pts.length - 1];
    const min = Math.min(...pts);
    const max = Math.max(...pts);
    const range = max - min || 1;
    const pad = 2;
    const h = this.height - pad * 2;
    const lastY = pad + h - ((last - min) / range) * h;
    const lastX = this.width - pad;

    return html`
      <div style="--color: ${this.color}">
        <svg
          width=${this.width}
          height=${this.height}
          viewBox="0 0 ${this.width} ${this.height}"
          aria-hidden="true"
          @mousemove=${this._handleMouseMove}
          @mouseleave=${this._handleMouseLeave}
        >
          <defs>
            <linearGradient id="gradient" x1="0" y1="0" x2="0" y2="1">
              <stop offset="0" stop-color=${this.color} stop-opacity="0.2" />
              <stop offset="1" stop-color=${this.color} stop-opacity="0" />
            </linearGradient>
          </defs>
          ${this.fillGradient
            ? html`<path class="area" d=${area} fill="url(#gradient)" />`
            : nothing}
          <path class="line" d=${line} stroke=${this.color} />
          <circle class="dot" cx=${lastX} cy=${lastY} r=${this.dotSize / 2} fill=${this.color} />
        </svg>
        ${this.showTooltip && this._hoverIndex >= 0
          ? html`
              <div
                class="tooltip"
                style="left: ${this._tooltipX}px; top: ${this
                  ._tooltipY}px; transform: translateX(-50%)"
              >
                ${this.tooltipLabel ? `${this.tooltipLabel}: ` : ""}${this.data[this._hoverIndex]}
              </div>
            `
          : nothing}
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-sparkline-enhanced": ScSparklineEnhanced;
  }
}
