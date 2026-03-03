import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";

@customElement("sc-empty-state")
export class ScEmptyState extends LitElement {
  static override styles = css`
    :host {
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      text-align: center;
      padding: var(--sc-space-2xl) var(--sc-space-lg);
    }

    .icon {
      font-size: 2.5rem;
      margin-bottom: var(--sc-space-md);
    }

    .heading {
      font-size: var(--sc-text-lg);
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text);
      margin-bottom: var(--sc-space-xs);
    }

    .description {
      font-size: var(--sc-text-sm);
      color: var(--sc-text-muted);
      max-width: 320px;
      line-height: var(--sc-leading-relaxed);
    }

    .slot {
      margin-top: var(--sc-space-md);
    }
  `;

  @property({ type: String }) icon = "";
  @property({ type: String }) heading = "";
  @property({ type: String }) description = "";

  override render() {
    return html`
      ${this.icon ? html`<div class="icon">${this.icon}</div>` : ""}
      ${this.heading ? html`<h2 class="heading">${this.heading}</h2>` : ""}
      ${this.description ? html`<p class="description">${this.description}</p>` : ""}
      <div class="slot"><slot></slot></div>
    `;
  }
}
