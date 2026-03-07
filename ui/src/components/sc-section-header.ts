import { LitElement, html, css, nothing } from "lit";
import { customElement, property } from "lit/decorators.js";

@customElement("sc-section-header")
export class ScSectionHeader extends LitElement {
  @property({ type: String }) heading = "";
  @property({ type: String }) description = "";

  static override styles = css`
    .header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: var(--sc-space-lg);
      margin-bottom: var(--sc-space-xl);
    }

    .heading {
      margin: 0;
      font-size: var(--sc-text-xl);
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text);
    }

    .description {
      margin: var(--sc-space-xs) 0 0;
      font-size: var(--sc-text-sm);
      color: var(--sc-text-muted);
    }

    .actions {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
      flex-shrink: 0;
    }

    @media (max-width: 640px) /* --sc-breakpoint-md */ {
      .header {
        flex-wrap: wrap;
      }
    }
  `;

  override render() {
    return html`
      <div class="header">
        <div class="text">
          <h2 class="heading">${this.heading}</h2>
          ${this.description ? html`<p class="description">${this.description}</p>` : nothing}
        </div>
        <div class="actions">
          <slot></slot>
        </div>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-section-header": ScSectionHeader;
  }
}
