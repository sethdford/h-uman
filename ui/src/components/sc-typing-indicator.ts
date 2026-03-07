import { LitElement, html, css, nothing } from "lit";
import { customElement, property } from "lit/decorators.js";

@customElement("sc-typing-indicator")
export class ScTypingIndicator extends LitElement {
  @property({ type: String }) elapsed = "";

  static override styles = css`
    @keyframes sc-indicator-enter {
      from {
        opacity: 0;
        transform: translateY(var(--sc-space-sm));
      }
      to {
        opacity: 1;
        transform: translateY(0);
      }
    }

    @keyframes sc-dot-bounce {
      0%,
      80%,
      100% {
        transform: translateY(0);
      }
      40% {
        transform: translateY(calc(-1 * var(--sc-space-xs)));
      }
    }

    :host {
      display: block;
    }

    .indicator {
      display: inline-flex;
      align-items: center;
      gap: var(--sc-space-sm);
      position: relative;
      padding: var(--sc-space-sm) var(--sc-space-md);
      background: var(--sc-bg-elevated);
      border: 1px solid var(--sc-border-subtle);
      border-radius: var(--sc-radius-2xl) var(--sc-radius-2xl) var(--sc-radius-2xl)
        var(--sc-radius-xs);
      box-shadow: var(--sc-shadow-xs);
      animation: sc-indicator-enter var(--sc-duration-normal)
        var(--sc-ease-spring, cubic-bezier(0.34, 1.56, 0.64, 1)) both;
    }

    /* Assistant bubble tail (same as sc-chat-bubble) */
    .indicator::after {
      content: "";
      position: absolute;
      bottom: -1px;
      left: calc(-1 * var(--sc-space-sm));
      width: var(--sc-space-sm);
      height: calc(var(--sc-space-sm) + var(--sc-space-2xs));
      background: var(--sc-bg-elevated);
      clip-path: polygon(100% 0, 100% 100%, 0 100%);
    }

    .dots {
      display: flex;
      align-items: center;
      gap: var(--sc-space-2xs);
    }

    .dot {
      width: var(--sc-space-xs);
      height: var(--sc-space-xs);
      border-radius: var(--sc-radius-full);
      background: var(--sc-text-muted);
      animation: sc-dot-bounce var(--sc-duration-slowest)
        var(--sc-ease-spring, cubic-bezier(0.34, 1.56, 0.64, 1)) infinite;
    }

    .dot:nth-child(1) {
      animation-delay: 0ms;
    }

    .dot:nth-child(2) {
      animation-delay: 150ms;
    }

    .dot:nth-child(3) {
      animation-delay: 300ms;
    }

    .elapsed {
      font-family: var(--sc-font);
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
      white-space: nowrap;
    }

    @media (prefers-reduced-motion: reduce) {
      .indicator {
        animation: none;
      }

      .dot {
        animation: none;
      }
    }
  `;

  override render() {
    return html`
      <div class="indicator" role="status" aria-live="polite" aria-label="Assistant is typing">
        <span class="dots" aria-hidden="true">
          <span class="dot"></span>
          <span class="dot"></span>
          <span class="dot"></span>
        </span>
        ${this.elapsed ? html`<span class="elapsed">${this.elapsed}</span>` : nothing}
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-typing-indicator": ScTypingIndicator;
  }
}
