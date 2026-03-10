import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";

@customElement("hu-sparkline")
export class ScSparkline extends LitElement {
  @property({ type: Array }) data: number[] = [];
  @property({ type: Number }) width = 80;
  @property({ type: Number }) height = 28;
  @property({ type: String }) color = "var(--hu-accent)";
  @property({ type: Boolean }) fill = true;

  static override styles = css`
    :host {
      display: inline-block;
      vertical-align: middle;
    }
    svg {
      display: block;
      overflow: visible;
    }
    .line {
      fill: none;
      stroke-width: 1.5;
      stroke-linecap: round;
      stroke-linejoin: round;
    }
    .area {
      opacity: 0.15;
    }
    .dot {
      filter: drop-shadow(0 0 2px currentColor);
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
      <svg
        width=${this.width}
        height=${this.height}
        viewBox="0 0 ${this.width} ${this.height}"
        aria-hidden="true"
      >
        ${this.fill ? html`<path class="area" d=${area} fill=${this.color} />` : null}
        <path class="line" d=${line} stroke=${this.color} />
        <circle class="dot" cx=${lastX} cy=${lastY} r="2.5" fill=${this.color} />
      </svg>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-sparkline": ScSparkline;
  }
}
