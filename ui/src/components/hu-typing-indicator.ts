import { LitElement, html, css, nothing } from "lit";
import { customElement, property } from "lit/decorators.js";

@customElement("hu-typing-indicator")
export class ScTypingIndicator extends LitElement {
  @property({ type: String }) elapsed = "";

  static override styles = css`
    @keyframes hu-indicator-enter {
      0% {
        opacity: 0;
        transform: translateY(var(--hu-space-md)) scale(0.9);
      }
      60% {
        opacity: 1;
        transform: translateY(calc(-1 * var(--hu-space-2xs))) scale(1.02);
      }
      100% {
        opacity: 1;
        transform: translateY(0) scale(1);
      }
    }

    @keyframes hu-glow-pulse {
      0%,
      100% {
        box-shadow:
          var(--hu-shadow-xs),
          0 0 0 0 color-mix(in srgb, var(--hu-accent) 0%, transparent);
      }
      50% {
        box-shadow:
          var(--hu-shadow-xs),
          0 0 12px 2px color-mix(in srgb, var(--hu-accent) 18%, transparent);
      }
    }

    @keyframes hu-dot-wave {
      0%,
      100% {
        opacity: 0.4;
        transform: scale(0.5);
      }
      50% {
        opacity: 1;
        transform: scale(1.3);
      }
    }

    :host {
      display: block;
    }

    .indicator {
      display: inline-flex;
      align-items: center;
      gap: var(--hu-space-sm);
      position: relative;
      padding: var(--hu-space-sm) var(--hu-space-md);
      background: color-mix(in srgb, var(--hu-surface-container) 65%, transparent);
      backdrop-filter: blur(var(--hu-blur-md));
      -webkit-backdrop-filter: blur(var(--hu-blur-md));
      border: 1px solid color-mix(in srgb, var(--hu-accent) 12%, transparent);
      border-radius: var(--hu-radius-2xl) var(--hu-radius-2xl) var(--hu-radius-2xl)
        var(--hu-radius-xs);
      animation:
        hu-indicator-enter var(--hu-duration-normal)
          var(--hu-ease-spring, cubic-bezier(0.34, 1.56, 0.64, 1)) both,
        hu-glow-pulse 2.4s var(--hu-ease-in-out) 0.3s infinite;
    }

    .indicator::after {
      content: "";
      position: absolute;
      bottom: -1px;
      left: calc(-1 * var(--hu-space-sm));
      width: var(--hu-space-sm);
      height: calc(var(--hu-space-sm) + var(--hu-space-2xs));
      background: color-mix(in srgb, var(--hu-surface-container) 65%, transparent);
      clip-path: polygon(100% 0, 100% 100%, 0 100%);
    }

    .dots {
      display: flex;
      align-items: center;
      gap: var(--hu-space-xs);
    }

    .dot {
      width: var(--hu-space-xs);
      height: var(--hu-space-xs);
      border-radius: var(--hu-radius-full);
      background: var(--hu-accent);
      animation: hu-dot-wave var(--hu-duration-slow) var(--hu-ease-in-out) infinite;
    }

    .dot:nth-child(2) {
      animation-delay: var(--hu-duration-normal);
    }

    .dot:nth-child(3) {
      animation-delay: var(--hu-duration-moderate);
    }

    .elapsed {
      font-family: var(--hu-font);
      font-size: var(--hu-text-xs);
      color: var(--hu-text-muted);
      white-space: nowrap;
      font-variant-numeric: tabular-nums;
      opacity: 0;
      animation: hu-indicator-enter var(--hu-duration-normal) var(--hu-ease-out) 3s both;
    }

    @media (prefers-reduced-transparency: reduce) {
      .indicator {
        backdrop-filter: none;
        -webkit-backdrop-filter: none;
        background: var(--hu-surface-container);
      }
      .indicator::after {
        background: var(--hu-surface-container);
      }
    }

    @media (prefers-reduced-motion: reduce) {
      .indicator {
        animation: none;
      }

      .dot {
        animation: none;
        opacity: 0.6;
      }

      .elapsed {
        animation: none;
        opacity: 1;
      }
    }
  `;

  override render() {
    return html`
      <div class="indicator" role="status" aria-live="polite" aria-label="Assistant is thinking">
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
    "hu-typing-indicator": ScTypingIndicator;
  }
}
