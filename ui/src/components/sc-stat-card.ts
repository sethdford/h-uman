import { LitElement, html, css, nothing } from "lit";
import { customElement, property } from "lit/decorators.js";
import "./sc-card.js";
import "./sc-animated-number.js";
import { icons } from "../icons.js";

@customElement("sc-stat-card")
export class ScStatCard extends LitElement {
  @property({ type: Number }) value = 0;
  /** When set, renders this string instead of animated number (e.g. "2d 0h", "5.9 MB") */
  @property({ type: String }) valueStr = "";
  @property({ type: String }) label = "";
  @property({ type: String }) trend = "";
  @property({ type: String }) trendDirection: "up" | "down" | "flat" = "flat";
  @property({ type: Number }) progress = -1;
  @property({ type: String }) accent: "primary" | "secondary" | "tertiary" | "error" = "primary";
  @property({ type: String }) suffix = "";
  @property({ type: String }) prefix = "";

  static override styles = css`
    :host {
      display: block;
      animation: sc-scale-in var(--sc-duration-normal) var(--sc-spring-micro, ease-out) both;
      animation-delay: var(--sc-stagger-delay, 0ms);
    }

    @media (prefers-reduced-motion: reduce) {
      :host {
        animation: none;
      }
    }

    .stat-card {
      position: relative;
      padding: var(--sc-space-lg);
      min-width: 8.75rem;
    }

    .trend {
      position: absolute;
      top: 0;
      right: 0;
      display: flex;
      align-items: center;
      gap: var(--sc-space-2xs);
      font-size: var(--sc-text-xs);
      font-weight: var(--sc-weight-medium);
    }

    .trend.up {
      color: var(--sc-success);
    }

    .trend.down {
      color: var(--sc-error);
    }

    .trend.flat {
      color: var(--sc-text-muted);
    }

    .trend-icon svg {
      width: 0.875rem;
      height: 0.875rem;
    }

    .value {
      font-size: var(--sc-text-2xl);
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text);
    }

    .label {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
      margin-top: var(--sc-space-xs);
      text-transform: uppercase;
      letter-spacing: 0.04em;
    }

    .progress-bar {
      height: 0.125rem;
      background: var(--sc-bg-inset);
      border-radius: var(--sc-radius-full);
      margin-top: var(--sc-space-md);
      overflow: hidden;
    }

    .progress-fill {
      height: 100%;
      border-radius: inherit;
      transition: width var(--sc-duration-slow) var(--sc-ease-out);
    }

    .progress-fill.accent-primary {
      background: var(--sc-accent);
    }

    .progress-fill.accent-secondary {
      background: var(--sc-accent-secondary);
    }

    .progress-fill.accent-tertiary {
      background: var(--sc-accent-tertiary);
    }

    .progress-fill.accent-error {
      background: var(--sc-error);
    }
  `;

  override render() {
    const trendIcon =
      this.trendDirection === "up"
        ? icons["trending-up"]
        : this.trendDirection === "down"
          ? icons["trending-down"]
          : null;

    return html`
      <sc-card glass hoverable>
        <div class="stat-card">
          ${this.trend
            ? html`
                <div class="trend ${this.trendDirection}">
                  ${trendIcon ? html`<span class="trend-icon">${trendIcon}</span>` : nothing}
                  <span class="trend-value">${this.trend}</span>
                </div>
              `
            : nothing}
          <div class="value">
            ${this.valueStr
              ? html`${this.prefix}${this.valueStr}${this.suffix}`
              : html`
                  <sc-animated-number
                    .value=${this.value}
                    .prefix=${this.prefix}
                    .suffix=${this.suffix}
                  ></sc-animated-number>
                `}
          </div>
          <div class="label">${this.label}</div>
          ${this.progress >= 0
            ? html`
                <div class="progress-bar">
                  <div
                    class="progress-fill accent-${this.accent}"
                    style="width: ${this.progress * 100}%"
                  ></div>
                </div>
              `
            : nothing}
        </div>
      </sc-card>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-stat-card": ScStatCard;
  }
}
