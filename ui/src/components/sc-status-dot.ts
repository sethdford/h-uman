import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";

export type StatusDotStatus =
  | "connected"
  | "connecting"
  | "disconnected"
  | "operational"
  | "offline"
  | "online"
  | "busy"
  | "away";

export type StatusDotSize = "sm" | "md";

@customElement("sc-status-dot")
export class ScStatusDot extends LitElement {
  @property({ type: String }) status: StatusDotStatus = "disconnected";
  @property({ type: String }) size: StatusDotSize = "sm";

  static override styles = css`
    :host {
      display: inline-block;
      flex-shrink: 0;
    }

    .dot {
      border-radius: 50%;
      background: var(--sc-text-muted);
    }

    .dot.size-sm {
      width: var(--sc-space-sm);
      height: var(--sc-space-sm);
    }

    .dot.size-md {
      width: 0.625rem;
      height: 0.625rem;
    }

    /* connected / operational / online */
    .dot.status-connected,
    .dot.status-operational,
    .dot.status-online {
      background: var(--sc-success);
    }

    /* connecting */
    .dot.status-connecting {
      background: var(--sc-warning);
      animation: sc-status-dot-pulse var(--sc-duration-slow) var(--sc-ease-in-out) infinite;
    }

    /* disconnected / offline */
    .dot.status-disconnected,
    .dot.status-offline {
      background: var(--sc-text-muted);
    }

    /* busy */
    .dot.status-busy {
      background: var(--sc-error);
    }

    /* away */
    .dot.status-away {
      background: var(--sc-warning);
    }

    @keyframes sc-status-dot-pulse {
      0%,
      100% {
        opacity: 1;
      }
      50% {
        opacity: 0.4;
      }
    }

    @media (prefers-reduced-motion: reduce) {
      .dot.status-connecting {
        animation: none;
      }
    }
  `;

  override render() {
    return html`
      <span class="dot size-${this.size} status-${this.status}" aria-hidden="true"></span>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-status-dot": ScStatusDot;
  }
}
