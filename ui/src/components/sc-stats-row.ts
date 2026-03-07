import { LitElement, html, css } from "lit";
import { customElement } from "lit/decorators.js";

/**
 * Responsive grid layout for stat cards. Uses a slot for children (typically
 * sc-stat-card elements). Encapsulates grid and breakpoint CSS in Shadow DOM.
 */
@customElement("sc-stats-row")
export class ScStatsRow extends LitElement {
  static override styles = css`
    :host {
      display: block;
    }

    .stats-row {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(11.25rem, 1fr));
      gap: var(--sc-space-md);
      margin-bottom: var(--sc-space-2xl);
    }

    @media (max-width: 40rem) /* --sc-breakpoint-md */ {
      .stats-row {
        grid-template-columns: 1fr 1fr;
      }
    }

    @media (max-width: 30rem) /* --sc-breakpoint-sm */ {
      .stats-row {
        grid-template-columns: 1fr;
      }
    }
  `;

  override render() {
    return html`<div class="stats-row"><slot></slot></div>`;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-stats-row": ScStatsRow;
  }
}
