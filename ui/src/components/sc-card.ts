import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";

@customElement("sc-card")
export class ScCard extends LitElement {
  @property({ type: Boolean }) hoverable = false;
  @property({ type: Boolean }) clickable = false;

  static styles = css`
    :host {
      display: block;
    }

    .card {
      background: var(--sc-bg-surface);
      border: 1px solid var(--sc-border-subtle);
      border-radius: var(--sc-radius-lg);
      padding: var(--sc-space-xl);
      box-shadow: var(--sc-shadow-card);
      border-top: 3px solid transparent;
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
        transform var(--sc-duration-fast) var(--sc-ease-out),
        box-shadow var(--sc-duration-fast) var(--sc-ease-out);
    }
    .card.hoverable:hover {
      transform: translateY(-2px);
      box-shadow:
        0 1px 2px rgba(0, 0, 0, 0.06),
        0 8px 24px rgba(0, 0, 0, 0.08);
    }
    .card.hoverable:active {
      transform: translateY(0);
      box-shadow: var(--sc-shadow-sm);
    }

    .card.clickable {
      cursor: pointer;
      transition:
        transform var(--sc-duration-fast) var(--sc-ease-out),
        box-shadow var(--sc-duration-fast) var(--sc-ease-out);
    }
    .card.clickable:active {
      transform: translateY(0) scale(0.99);
      box-shadow: var(--sc-shadow-sm);
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
    const classes = ["card", this.hoverable ? "hoverable" : "", this.clickable ? "clickable" : ""]
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
