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
      padding: var(--sc-space-lg);
    }

    .card.hoverable {
      transition:
        transform var(--sc-duration-fast) var(--sc-ease-out),
        box-shadow var(--sc-duration-fast) var(--sc-ease-out);
    }
    .card.hoverable:hover {
      transform: translateY(-1px);
      box-shadow: var(--sc-shadow-sm);
    }

    .card.clickable {
      cursor: pointer;
    }
  `;

  render() {
    const classes = ["card", this.hoverable ? "hoverable" : "", this.clickable ? "clickable" : ""]
      .filter(Boolean)
      .join(" ");

    return html`
      <div class=${classes}>
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
