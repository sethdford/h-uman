import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";

@customElement("sc-card")
export class ScCard extends LitElement {
  @property({ type: Boolean }) hoverable = false;
  @property({ type: Boolean }) clickable = false;
  @property({ type: Boolean }) accent = false;
  @property({ type: Boolean }) elevated = false;

  static styles = css`
    :host {
      display: block;
    }

    .card {
      position: relative;
      background: var(--sc-bg-surface);
      background-image: var(--sc-surface-gradient);
      border: 1px solid var(--sc-border-subtle);
      border-radius: var(--sc-radius-lg);
      padding: var(--sc-space-xl);
      box-shadow: var(--sc-shadow-card);
      border-top: 3px solid transparent;
    }
    .card.elevated {
      box-shadow: var(--sc-shadow-md);
    }
    .card::before {
      content: "";
      position: absolute;
      inset: 0;
      border-radius: inherit;
      background: var(--sc-surface-glow);
      pointer-events: none;
    }
    .card > * {
      position: relative;
      z-index: 1;
    }
    .card.accent {
      border-top-color: var(--sc-accent);
    }

    @media (prefers-color-scheme: dark) {
      .card {
        border: none;
      }
    }

    [data-theme="dark"] .card {
      border: none;
    }

    .card.hoverable {
      transition:
        transform var(--sc-duration-normal) var(--sc-spring-out),
        box-shadow var(--sc-duration-normal) var(--sc-ease-out);
    }
    .card.hoverable:hover {
      transform: translateY(-4px);
      box-shadow: var(--sc-shadow-lg);
    }
    .card.hoverable:active {
      transform: translateY(-1px);
      box-shadow: var(--sc-shadow-sm);
      transition-duration: 80ms;
    }

    .card.clickable {
      cursor: pointer;
      transition:
        transform var(--sc-duration-normal) var(--sc-spring-out),
        box-shadow var(--sc-duration-normal) var(--sc-ease-out);
    }
    .card.clickable:hover {
      transform: translateY(-4px);
      box-shadow: var(--sc-shadow-lg);
    }
    .card.clickable:active {
      transform: translateY(-1px) scale(0.98);
      box-shadow: var(--sc-shadow-sm);
      transition-duration: 80ms;
    }
    .card.clickable:focus-visible {
      outline: var(--sc-focus-ring-width) solid var(--sc-focus-ring);
      outline-offset: var(--sc-focus-ring-offset);
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
