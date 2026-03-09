import { LitElement, html, css, nothing } from "lit";
import { customElement, property } from "lit/decorators.js";

@customElement("sc-typing-indicator")
export class ScTypingIndicator extends LitElement {
  @property({ type: String }) elapsed = "";

  static override styles = css`
    @keyframes sc-indicator-enter {
      0% {
        opacity: 0;
        transform: translateY(var(--sc-space-md)) scale(0.9);
      }
      60% {
        opacity: 1;
        transform: translateY(calc(-1 * var(--sc-space-2xs))) scale(1.02);
      }
      100% {
        opacity: 1;
        transform: translateY(0) scale(1);
      }
    }

    @keyframes sc-glow-pulse {
      0%,
      100% {
        box-shadow:
          var(--sc-shadow-xs),
          0 0 0 0 color-mix(in srgb, var(--sc-accent) 0%, transparent);
      }
      50% {
        box-shadow:
          var(--sc-shadow-xs),
          0 0 12px 2px color-mix(in srgb, var(--sc-accent) 18%, transparent);
      }
    }

    @keyframes sc-dot-wave {
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
      gap: var(--sc-space-sm);
      position: relative;
      padding: var(--sc-space-sm) var(--sc-space-md);
      background: color-mix(in srgb, var(--sc-surface-container) 65%, transparent);
      backdrop-filter: blur(var(--sc-blur-md));
      -webkit-backdrop-filter: blur(var(--sc-blur-md));
      border: 1px solid color-mix(in srgb, var(--sc-accent) 12%, transparent);
      border-radius: var(--sc-radius-2xl) var(--sc-radius-2xl) var(--sc-radius-2xl)
        var(--sc-radius-xs);
      animation:
        sc-indicator-enter var(--sc-duration-normal)
          var(--sc-ease-spring, cubic-bezier(0.34, 1.56, 0.64, 1)) both,
        sc-glow-pulse 2.4s var(--sc-ease-in-out) 0.3s infinite;
    }

    .indicator::after {
      content: "";
      position: absolute;
      bottom: -1px;
      left: calc(-1 * var(--sc-space-sm));
      width: var(--sc-space-sm);
      height: calc(var(--sc-space-sm) + var(--sc-space-2xs));
      background: color-mix(in srgb, var(--sc-surface-container) 65%, transparent);
      clip-path: polygon(100% 0, 100% 100%, 0 100%);
    }

    .dots {
      display: flex;
      align-items: center;
      gap: var(--sc-space-xs);
    }

    .dot {
      width: var(--sc-space-xs);
      height: var(--sc-space-xs);
      border-radius: var(--sc-radius-full);
      background: var(--sc-accent);
      animation: sc-dot-wave var(--sc-duration-slow) var(--sc-ease-in-out) infinite;
    }

    .dot:nth-child(2) {
      animation-delay: var(--sc-duration-normal);
    }

    .dot:nth-child(3) {
      animation-delay: var(--sc-duration-moderate);
    }

    .elapsed {
      font-family: var(--sc-font);
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
      white-space: nowrap;
      font-variant-numeric: tabular-nums;
      opacity: 0;
      animation: sc-indicator-enter var(--sc-duration-normal) var(--sc-ease-out) 3s both;
    }

    @media (prefers-reduced-transparency: reduce) {
      .indicator {
        backdrop-filter: none;
        -webkit-backdrop-filter: none;
        background: var(--sc-surface-container);
      }
      .indicator::after {
        background: var(--sc-surface-container);
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
    "sc-typing-indicator": ScTypingIndicator;
  }
}
