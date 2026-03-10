import { LitElement, html, css, nothing } from "lit";
import { customElement, property } from "lit/decorators.js";
import "./hu-card.js";
import "./hu-animated-number.js";
import { icons } from "../icons.js";

@customElement("hu-stat-card")
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
      animation: hu-scale-in var(--hu-duration-normal) var(--hu-spring-micro, ease-out) both;
      animation-delay: var(--hu-stagger-delay, 0ms);
    }

    @media (prefers-reduced-motion: reduce) {
      :host {
        animation: none;
      }
    }

    .stat-card {
      position: relative;
      padding: var(--hu-space-lg);
      min-width: 8.75rem;
    }

    .trend {
      position: absolute;
      top: 0;
      right: 0;
      display: flex;
      align-items: center;
      gap: var(--hu-space-2xs);
      font-size: var(--hu-text-xs);
      font-weight: var(--hu-weight-medium);
    }

    .trend.up {
      color: var(--hu-success);
    }

    .trend.down {
      color: var(--hu-error);
    }

    .trend.flat {
      color: var(--hu-text-muted);
    }

    .trend-icon svg {
      width: 0.875rem;
      height: 0.875rem;
    }

    .value {
      font-size: var(--hu-text-2xl);
      font-weight: var(--hu-weight-semibold);
      color: var(--hu-text);
    }

    .label {
      font-size: var(--hu-text-xs);
      color: var(--hu-text-muted);
      margin-top: var(--hu-space-xs);
      text-transform: uppercase;
      letter-spacing: 0.04em;
    }

    .progress-bar {
      height: 0.125rem;
      background: var(--hu-bg-inset);
      border-radius: var(--hu-radius-full);
      margin-top: var(--hu-space-md);
      overflow: hidden;
    }

    .progress-fill {
      height: 100%;
      border-radius: inherit;
      transition: width var(--hu-duration-slow) var(--hu-ease-out);
    }

    .progress-fill.accent-primary {
      background: var(--hu-accent);
    }

    .progress-fill.accent-secondary {
      background: var(--hu-accent-secondary);
    }

    .progress-fill.accent-tertiary {
      background: var(--hu-accent-tertiary);
    }

    .progress-fill.accent-error {
      background: var(--hu-error);
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
      <hu-card glass hoverable>
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
                  <hu-animated-number
                    .value=${this.value}
                    .prefix=${this.prefix}
                    .suffix=${this.suffix}
                  ></hu-animated-number>
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
      </hu-card>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-stat-card": ScStatCard;
  }
}
