import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";

export type DeliveryStatus = "sending" | "sent" | "streaming" | "complete" | "failed";

const ARIA_LABELS: Record<DeliveryStatus, string> = {
  sending: "Sending",
  sent: "Sent",
  streaming: "Streaming response",
  complete: "Complete",
  failed: "Failed to send",
};

@customElement("sc-delivery-status")
export class ScDeliveryStatus extends LitElement {
  @property({ type: String }) status: DeliveryStatus = "sent";

  static override styles = css`
    @keyframes sc-delivery-pulse {
      0%,
      100% {
        opacity: 1;
      }
      50% {
        opacity: 0.3;
      }
    }

    @keyframes sc-delivery-dots {
      0%,
      100% {
        opacity: 0.3;
      }
      50% {
        opacity: 1;
      }
    }

    @keyframes sc-delivery-bounce {
      0% {
        transform: scale(0.8);
      }
      70% {
        transform: scale(1.1);
      }
      100% {
        transform: scale(1);
      }
    }

    :host {
      display: inline-flex;
      align-items: center;
      gap: var(--sc-space-2xs);
      font-size: var(--sc-text-2xs, 10px);
      font-family: var(--sc-font);
      color: var(--sc-text-faint);
    }

    .icon {
      display: inline-flex;
      align-items: center;
      justify-content: center;
      width: var(--sc-text-xs);
      height: var(--sc-text-xs);
      flex-shrink: 0;
    }

    .icon svg {
      width: 100%;
      height: 100%;
      fill: currentColor;
    }

    /* Sending: pulsing dot */
    .dot {
      width: var(--sc-space-2xs);
      height: var(--sc-space-2xs);
      border-radius: 50%;
      background: currentColor;
      animation: sc-delivery-pulse var(--sc-duration-slowest) var(--sc-ease-in-out) infinite;
    }

    /* Streaming: three sequenced dots */
    .stream-dots {
      display: inline-flex;
      align-items: center;
      gap: var(--sc-space-2xs);
    }

    .stream-dots .dot {
      width: var(--sc-space-2xs);
      height: var(--sc-space-2xs);
      border-radius: 50%;
      background: currentColor;
      animation: sc-delivery-dots var(--sc-duration-slowest) var(--sc-ease-in-out) infinite;
    }

    .stream-dots .dot:nth-child(1) {
      animation-delay: 0ms;
    }

    .stream-dots .dot:nth-child(2) {
      animation-delay: var(--sc-duration-fast);
    }

    .stream-dots .dot:nth-child(3) {
      animation-delay: calc(var(--sc-duration-fast) * 2);
    }

    /* Streaming: accent color */
    :host([data-status="streaming"]) {
      color: var(--sc-accent);
    }

    /* Complete: accent color + bounce */
    :host([data-status="complete"]) {
      color: var(--sc-accent);
    }

    :host([data-status="complete"]) .icon {
      animation: sc-delivery-bounce var(--sc-duration-moderate) var(--sc-ease-out) 1;
    }

    /* Failed: error color */
    :host([data-status="failed"]) {
      color: var(--sc-error);
    }

    .retry {
      background: none;
      border: none;
      padding: 0;
      margin: 0;
      font: inherit;
      color: inherit;
      text-decoration: underline;
      cursor: pointer;
      font-size: inherit;
    }

    .retry:hover {
      text-decoration: none;
    }

    .retry:focus-visible {
      outline: var(--sc-focus-ring-width) solid var(--sc-focus-ring);
      outline-offset: var(--sc-focus-ring-offset);
    }

    .wrap {
      display: inline-flex;
      align-items: center;
      gap: var(--sc-space-2xs);
    }

    @media (prefers-reduced-motion: reduce) {
      .dot,
      .stream-dots .dot {
        animation: none;
      }

      :host([data-status="complete"]) .icon {
        animation: none;
      }
    }
  `;

  private _onRetry(e: Event) {
    e.preventDefault();
    this.dispatchEvent(
      new CustomEvent("sc-retry", {
        bubbles: true,
        composed: true,
      }),
    );
  }

  override render() {
    this.setAttribute("data-status", this.status);
    this.setAttribute("aria-label", ARIA_LABELS[this.status]);

    switch (this.status) {
      case "sending":
        return html`
          <span role="status" aria-live="polite" class="icon">
            <span class="dot" aria-hidden="true"></span>
          </span>
        `;

      case "sent":
        return html`
          <span role="status" aria-live="polite" class="icon">
            <svg viewBox="0 0 256 256" aria-hidden="true">
              <path
                d="M229.66,77.66l-128,128a8,8,0,0,1-11.32,0l-56-56a8,8,0,0,1,11.32-11.32L96,188.69,218.34,66.34a8,8,0,0,1,11.32,11.32Z"
                fill="currentColor"
              />
            </svg>
          </span>
        `;

      case "streaming":
        return html`
          <span role="status" aria-live="polite" class="stream-dots">
            <span class="dot" aria-hidden="true"></span>
            <span class="dot" aria-hidden="true"></span>
            <span class="dot" aria-hidden="true"></span>
          </span>
        `;

      case "complete":
        return html`
          <span role="status" aria-live="polite" class="icon">
            <svg viewBox="0 0 24 24" aria-hidden="true">
              <path
                d="M18 7l-1.41-1.41-6.34 6.34 1.41 1.41L18 7zm-4.24-.29l-5.66 5.66 1.41 1.41 5.66-5.66-1.41-1.41zM12 19.07l-5.66-5.66 1.41-1.41L12 16.24l5.66-5.66 1.41 1.41L12 19.07z"
                fill="currentColor"
              />
            </svg>
          </span>
        `;

      case "failed":
        return html`
          <span role="status" aria-live="polite" class="wrap">
            <span class="icon">
              <svg viewBox="0 0 256 256" aria-hidden="true">
                <path
                  d="M208.49,192.49a12,12,0,0,1-17,17L128,145,64.49,208.49a12,12,0,0,1-17-17L111,128,47.51,64.49a12,12,0,0,1,17-17L128,111l63.51-63.52a12,12,0,0,1,17,17L145,128Z"
                  fill="currentColor"
                />
              </svg>
            </span>
            <button type="button" class="retry" @click=${this._onRetry} aria-label="Retry sending">
              Retry
            </button>
          </span>
        `;

      default:
        return html`<span role="status" aria-live="polite"></span>`;
    }
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-delivery-status": ScDeliveryStatus;
  }
}
