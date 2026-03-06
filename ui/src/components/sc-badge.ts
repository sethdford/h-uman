import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";

type BadgeVariant = "success" | "warning" | "error" | "info" | "neutral";

@customElement("sc-badge")
export class ScBadge extends LitElement {
  @property({ type: String }) variant: BadgeVariant = "neutral";
  @property({ type: Boolean }) dot = false;

  static override styles = css`
    :host {
      display: inline-flex;
    }

    .badge {
      display: inline-flex;
      align-items: center;
      gap: var(--sc-space-xs);
      font-size: var(--sc-text-xs);
      font-weight: var(--sc-weight-medium);
      padding: var(--sc-space-2xs) var(--sc-space-sm);
      border-radius: 9999px;
      font-family: var(--sc-font);
    }

    /* Variants */
    .badge.variant-neutral {
      background: var(--sc-bg-elevated);
      color: var(--sc-text-muted);
    }

    .badge.variant-success {
      background: var(--sc-success-dim);
      color: var(--sc-success);
    }

    .badge.variant-warning {
      background: var(--sc-warning-dim);
      color: var(--sc-warning);
    }

    .badge.variant-error {
      background: var(--sc-error-dim);
      color: var(--sc-error);
    }

    .badge.variant-info {
      background: var(--sc-info-dim);
      color: var(--sc-info);
    }

    .dot {
      width: 6px;
      height: 6px;
      border-radius: 50%;
      flex-shrink: 0;
    }

    .variant-neutral .dot {
      background: var(--sc-text-muted);
    }
    .variant-success .dot {
      background: var(--sc-success);
    }
    .variant-warning .dot {
      background: var(--sc-warning);
    }
    .variant-error .dot {
      background: var(--sc-error);
    }
    .variant-info .dot {
      background: var(--sc-info);
    }
  `;

  override render() {
    const classes = ["badge", `variant-${this.variant}`].join(" ");

    return html`
      <span class=${classes}>
        ${this.dot ? html`<span class="dot" aria-hidden="true"></span>` : null}
        <slot></slot>
      </span>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-badge": ScBadge;
  }
}
