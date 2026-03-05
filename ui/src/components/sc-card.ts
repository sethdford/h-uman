import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";

@customElement("sc-card")
export class ScCard extends LitElement {
  @property({ type: Boolean }) hoverable = false;
  @property({ type: Boolean }) clickable = false;
  @property({ type: Boolean }) accent = false;
  @property({ type: Boolean }) elevated = false;
  @property({ type: Boolean }) glass = false;

  static styles = css`
    :host {
      display: block;
    }

    .card {
      position: relative;
      background: var(--sc-bg-surface);
      border: 1px solid color-mix(in srgb, var(--sc-border-subtle, #27282d) 60%, transparent);
      border-radius: var(--sc-radius-xl, 16px);
      padding: var(--sc-space-xl);
      box-shadow:
        0 1px 1px rgba(0, 0, 0, 0.08),
        0 2px 4px rgba(0, 0, 0, 0.06),
        0 4px 16px rgba(0, 0, 0, 0.04);
      overflow: hidden;
    }

    /* Top-edge luminance — the "lit from above" inset highlight */
    .card::before {
      content: "";
      position: absolute;
      inset: 0;
      border-radius: inherit;
      box-shadow: inset 0 1px 0 rgba(255, 255, 255, 0.06);
      pointer-events: none;
      z-index: 1;
    }

    /* Ambient surface glow — very faint accent radial at top */
    .card::after {
      content: "";
      position: absolute;
      top: 0;
      left: 0;
      right: 0;
      height: 120px;
      background: radial-gradient(
        ellipse 80% 60% at 50% -10%,
        color-mix(in srgb, var(--sc-accent) 3%, transparent),
        transparent
      );
      border-radius: inherit;
      pointer-events: none;
    }

    .card > ::slotted(*),
    .card > * {
      position: relative;
      z-index: 2;
    }

    .card.elevated {
      box-shadow:
        0 2px 4px rgba(0, 0, 0, 0.1),
        0 8px 24px rgba(0, 0, 0, 0.12),
        0 16px 48px rgba(0, 0, 0, 0.06);
    }

    .card.accent {
      border-top: 2px solid var(--sc-accent);
    }

    .card.glass {
      background: color-mix(in srgb, var(--sc-bg-surface) 70%, transparent);
      backdrop-filter: blur(24px) saturate(180%);
      -webkit-backdrop-filter: blur(24px) saturate(180%);
      border: 1px solid color-mix(in srgb, var(--sc-border-subtle, #27282d) 40%, transparent);
    }

    .card.glass::before {
      box-shadow: inset 0 1px 0 rgba(255, 255, 255, 0.08);
    }

    .card.hoverable,
    .card.clickable {
      cursor: pointer;
      transition:
        transform 280ms cubic-bezier(0.22, 1, 0.36, 1),
        box-shadow 280ms cubic-bezier(0.22, 1, 0.36, 1),
        border-color 280ms ease;
      will-change: transform;
    }
    .card.hoverable:hover,
    .card.clickable:hover {
      transform: translateY(-3px) scale(1.005);
      box-shadow:
        0 4px 8px rgba(0, 0, 0, 0.1),
        0 12px 32px rgba(0, 0, 0, 0.12),
        0 24px 56px rgba(0, 0, 0, 0.06);
      border-color: color-mix(in srgb, var(--sc-accent) 20%, var(--sc-border-subtle, #27282d));
    }
    .card.hoverable:active,
    .card.clickable:active {
      transform: translateY(-1px) scale(0.985);
      box-shadow:
        0 1px 2px rgba(0, 0, 0, 0.08),
        0 2px 8px rgba(0, 0, 0, 0.06);
      transition-duration: 100ms;
    }
    .card.clickable:focus-visible {
      outline: var(--sc-focus-ring-width) solid var(--sc-focus-ring);
      outline-offset: var(--sc-focus-ring-offset);
    }

    @media (prefers-color-scheme: light) {
      :host(:not([data-theme="dark"])) .card {
        box-shadow:
          0 1px 2px rgba(0, 0, 0, 0.04),
          0 2px 8px rgba(0, 0, 0, 0.04),
          0 4px 16px rgba(0, 0, 0, 0.02);
      }
      :host(:not([data-theme="dark"])) .card::before {
        box-shadow: inset 0 1px 0 rgba(255, 255, 255, 0.8);
      }
      :host(:not([data-theme="dark"])) .card::after {
        background: none;
      }
    }

    @media (prefers-reduced-motion: reduce) {
      .card.hoverable,
      .card.clickable {
        transition: none;
      }
    }
  `;

  private _onKeyDown(e: KeyboardEvent): void {
    if (!this.clickable || (e.key !== "Enter" && e.key !== " ")) return;
    e.preventDefault();
    this.dispatchEvent(new MouseEvent("click", { bubbles: true, composed: true }));
  }

  render() {
    const classes = [
      "card",
      this.hoverable ? "hoverable" : "",
      this.clickable ? "clickable" : "",
      this.accent ? "accent" : "",
      this.elevated ? "elevated" : "",
      this.glass ? "glass" : "",
    ]
      .filter(Boolean)
      .join(" ");

    return html`
      <div
        class=${classes}
        role=${this.clickable ? "button" : undefined}
        tabindex=${this.clickable ? 0 : undefined}
        @keydown=${this._onKeyDown}
      >
        <slot></slot>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-card": ScCard;
  }
}
