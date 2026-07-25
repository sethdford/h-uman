import { LitElement, html, css, nothing } from "lit";
import { customElement, property, state } from "lit/decorators.js";

export interface RingProgressItem {
  value: number;
  max?: number;
  label?: string;
  color?: string;
}

@customElement("hu-ring-progress")
export class HuRingProgress extends LitElement {
  @property({ attribute: false }) rings: RingProgressItem[] = [];
  @property({ type: Number }) size = 140;
  @property({ type: Number }) strokeWidth = 10;

  @state() private _reducedMotion = false;
  @state() private _celebrate = false;
  @state() private _mounted = false;
  /** Unique SVG defs ids when multiple instances exist */
  private _svgUid = "";

  static override styles = css`
    :host {
      display: inline-block;
      position: relative;
    }
    svg {
      display: block;
      width: 100%;
      height: auto;
    }
    .track {
      fill: none;
      stroke: var(--hu-border-subtle);
      opacity: 0.5;
    }
    .progress {
      fill: none;
      stroke-linecap: round;
      transform: rotate(-90deg);
      transform-origin: center;
      transition: stroke-dashoffset var(--hu-duration-slower, 450ms)
        var(--hu-ease-out, cubic-bezier(0.22, 1, 0.36, 1));
      filter: drop-shadow(0 0 1px color-mix(in srgb, var(--hu-accent) 25%, transparent));
    }
    .ring-tip {
      pointer-events: none;
      transition: opacity var(--hu-duration-fast) var(--hu-ease-out);
    }
    .progress.celebrate {
      animation: hu-ring-celebrate var(--hu-duration-normal, 250ms)
        var(--hu-ease-spring, cubic-bezier(0.34, 1.56, 0.64, 1)) 2;
    }
    .labels {
      display: flex;
      flex-wrap: wrap;
      justify-content: center;
      gap: var(--hu-space-xs);
      margin-top: var(--hu-space-sm);
      font-family: var(--hu-font);
      font-size: var(--hu-text-2xs);
      color: var(--hu-text-muted);
      text-align: center;
    }
    @keyframes hu-ring-celebrate {
      0%,
      100% {
        opacity: 1;
      }
      50% {
        opacity: 0.88;
        filter: drop-shadow(0 0 8px var(--hu-accent));
      }
    }
    @media (prefers-reduced-motion: reduce) {
      .progress {
        transition: none;
      }
      .progress.celebrate {
        animation: none;
      }
    }
  `;

  override connectedCallback(): void {
    super.connectedCallback();
    this._reducedMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
    this._svgUid =
      typeof crypto !== "undefined" && "randomUUID" in crypto
        ? crypto.randomUUID().replace(/-/g, "")
        : `rp${Math.floor(Math.random() * 1e9)}`;
  }

  override updated(changed: Map<string, unknown>): void {
    if (changed.has("rings") && !this._reducedMotion && this._mounted) {
      const prev = (changed.get("rings") as RingProgressItem[] | undefined) ?? [];
      const next = this.rings ?? [];
      const anyOver = next.some((r, i) => {
        const max = r.max ?? 1;
        const v = r.value / max;
        const prevV = (prev[i]?.value ?? 0) / (prev[i]?.max ?? max);
        return v >= 1 && prevV < 1;
      });
      if (anyOver) {
        this._celebrate = true;
        setTimeout(() => {
          this._celebrate = false;
          this.requestUpdate();
        }, 600);
      }
    }
  }

  private _defaultColor(i: number): string {
    const n = (i % 16) + 1;
    return `var(--hu-chart-categorical-${n})`;
  }

  private _ringGeometry(index: number) {
    const vb = 120;
    const cx = vb / 2;
    const cy = vb / 2;
    const outer = 54;
    const step = 12;
    const r = outer - index * step;
    const sw = Math.max(5, this.strokeWidth - index * 2);
    const c = 2 * Math.PI * r;
    return { cx, cy, r, sw, c, vb };
  }

  /** Arc endpoint (radians from +x), after visual -90° start = top of ring */
  private _tipPosition(cx: number, cy: number, r: number, drawRatio: number) {
    const theta = -Math.PI / 2 + 2 * Math.PI * drawRatio;
    return { tx: cx + r * Math.cos(theta), ty: cy + r * Math.sin(theta) };
  }

  override render() {
    const items = (this.rings ?? []).slice(0, 3);
    if (items.length === 0) {
      return html`<span class="labels" role="status">No rings</span>`;
    }

    const labels = items.map((r) => r.label).filter(Boolean);
    const uid = this._svgUid || "rp0";

    return html`
      <div style="width:${this.size}px" role="img" aria-label="Ring progress" class="ring-wrap">
        <svg viewBox="0 0 120 120" aria-hidden="true">
          <defs>
            <filter id="${uid}-tipBlur" x="-100%" y="-100%" width="300%" height="300%">
              <feGaussianBlur in="SourceGraphic" stdDeviation="1.8" result="b" />
              <feMerge>
                <feMergeNode in="b" />
                <feMergeNode in="SourceGraphic" />
              </feMerge>
            </filter>
            ${items.map((ring, i) => {
              const color = ring.color ?? this._defaultColor(i);
              return html`
                <linearGradient
                  id="${uid}-lg${i}"
                  gradientUnits="userSpaceOnUse"
                  x1="0"
                  y1="60"
                  x2="120"
                  y2="60"
                >
                  <stop offset="0%" stop-color=${color} stop-opacity="0.55" />
                  <stop offset="100%" stop-color=${color} stop-opacity="1" />
                </linearGradient>
              `;
            })}
          </defs>
          ${items.map((ring, i) => {
            const { cx, cy, r, sw, c } = this._ringGeometry(i);
            const max = ring.max ?? 1;
            const ratioRaw = ring.value / max;
            const drawRatio = Math.min(Math.max(ratioRaw, 0), 1.5);
            const offset = c * (1 - drawRatio);
            const color = ring.color ?? this._defaultColor(i);
            const celebrate = this._celebrate && ratioRaw >= 1;
            const dashOff = this._mounted || this._reducedMotion ? offset : c;
            const { tx, ty } = this._tipPosition(cx, cy, r, drawRatio);
            const showTip = drawRatio > 0.04 && !this._reducedMotion;
            const strokeRef = `url(#${uid}-lg${i})`;
            return html`
              <circle class="track" cx=${cx} cy=${cy} r=${r} stroke-width=${sw} />
              <circle
                class="progress ${celebrate ? "celebrate" : ""}"
                cx=${cx}
                cy=${cy}
                r=${r}
                stroke-width=${sw}
                stroke=${strokeRef}
                style="color: ${color}"
                stroke-dasharray=${c}
                stroke-dashoffset=${dashOff}
              />
              ${
                showTip
                  ? html`
                      <circle
                        class="ring-tip"
                        cx=${tx}
                        cy=${ty}
                        r=${Math.max(2.5, sw * 0.22)}
                        fill=${color}
                        filter=${`url(#${uid}-tipBlur)`}
                        opacity="0.95"
                        aria-hidden="true"
                      />
                    `
                  : nothing
              }
            `;
          })}
        </svg>
        ${
          labels.length
            ? html`<div class="labels">${labels.map((l) => html`<span>${l}</span>`)}</div>`
            : nothing
        }
      </div>
    `;
  }

  override firstUpdated(): void {
    if (!this._reducedMotion) {
      requestAnimationFrame(() => {
        this._mounted = true;
        this.requestUpdate();
      });
    } else {
      this._mounted = true;
    }
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-ring-progress": HuRingProgress;
  }
}
