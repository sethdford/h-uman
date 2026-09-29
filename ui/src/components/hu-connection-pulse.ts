import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";

@customElement("hu-connection-pulse")
export class HuConnectionPulse extends LitElement {
  @property({ type: String }) status: "connected" | "connecting" | "disconnected" = "disconnected";

  static override styles = css`
    :host {
      display: inline-flex;
      align-items: center;
      gap: var(--hu-space-xs);
    }

    .pulse-container {
      position: relative;
      width: 8px;
      height: 8px;
    }

    .dot {
      position: absolute;
      inset: 0;
      border-radius: 50%;
      background: var(--hu-error);
      transition: background var(--hu-duration-normal) var(--hu-ease-out);
    }

    :host([status="connected"]) .dot {
      background: var(--hu-accent);
    }

    :host([status="connecting"]) .dot {
      background: var(--hu-warning);
    }

    .ring {
      position: absolute;
      inset: calc(-1 * var(--hu-space-xs));
      border-radius: 50%;
      border: 1.5px solid var(--hu-accent);
      opacity: 0;
      animation: none;
    }

    :host([status="connected"]) .ring {
      animation: hu-pulse-ring var(--hu-duration-extra-slow, 3s) var(--hu-ease-out) infinite;
    }

    :host([status="connecting"]) .ring {
      border-color: var(--hu-warning);
      animation: hu-pulse-ring var(--hu-duration-slow, 1.5s) var(--hu-ease-out) infinite;
    }

    @keyframes hu-pulse-ring {
      0% {
        transform: scale(1);
        opacity: 0.6;
      }
      100% {
        transform: scale(2.5);
        opacity: 0;
      }
    }

    @keyframes hu-status-breathe {
      0%,
      100% {
        opacity: 0.6;
        transform: scale(0.98);
      }
      50% {
        opacity: 1;
        transform: scale(1.02);
      }
    }

    .dot.hu-ambient-breathe {
      animation: hu-status-breathe var(--hu-ambient-breathing-duration) var(--hu-ease-in-out)
        infinite;
    }

    .label {
      font-family: var(--hu-font);
      font-size: var(--hu-text-xs);
      color: var(--hu-text-muted);
      font-variant-numeric: tabular-nums;
    }

    @media (prefers-reduced-motion: reduce) {
      .ring {
        animation: none !important;
      }
      .dot.hu-ambient-breathe {
        animation: none;
        opacity: 1;
        transform: none;
      }
    }
  `;

  override render() {
    return html`
      <div class="pulse-container" role="status" aria-label=${`Connection: ${this.status}`}>
        <div class="dot ${this.status === "disconnected" ? "" : "hu-ambient-breathe"}"></div>
        <div class="ring"></div>
      </div>
      ${
        this.status !== "disconnected"
          ? html`<span class="label"
              >${this.status === "connected" ? "Live" : "Connecting..."}</span
            >`
          : html`<span class="label">Offline</span>`
      }
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-connection-pulse": HuConnectionPulse;
  }
}
