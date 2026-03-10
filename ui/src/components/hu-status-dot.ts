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

@customElement("hu-status-dot")
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
      background: var(--hu-text-muted);
    }

    .dot.size-sm {
      width: var(--hu-space-sm);
      height: var(--hu-space-sm);
    }

    .dot.size-md {
      width: 0.625rem;
      height: 0.625rem;
    }

    /* connected / operational / online */
    .dot.status-connected,
    .dot.status-operational,
    .dot.status-online {
      background: var(--hu-success);
    }

    /* connecting */
    .dot.status-connecting {
      background: var(--hu-warning);
      animation: hu-status-dot-pulse var(--hu-duration-slow) var(--hu-ease-in-out) infinite;
    }

    /* disconnected / offline */
    .dot.status-disconnected,
    .dot.status-offline {
      background: var(--hu-text-muted);
    }

    /* busy */
    .dot.status-busy {
      background: var(--hu-error);
    }

    /* away */
    .dot.status-away {
      background: var(--hu-warning);
    }

    @keyframes hu-status-dot-pulse {
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
    "hu-status-dot": ScStatusDot;
  }
}
