import { LitElement, html, css, nothing } from "lit";
import { customElement, property } from "lit/decorators.js";

@customElement("sc-metric-row")
export class ScMetricRow extends LitElement {
  @property({ type: Array })
  items: Array<{ label: string; value: string; accent?: string }> = [];

  static override styles = css`
    .row {
      display: flex;
      align-items: center;
      gap: var(--sc-space-lg);
      padding: var(--sc-space-md) 0;
      flex-wrap: wrap;
    }

    .divider {
      width: 1px;
      height: 24px;
      background: var(--sc-border-subtle);
      flex-shrink: 0;
    }

    .item {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-2xs);
    }

    .item-label {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
    }

    .item-value {
      font-size: var(--sc-text-sm);
      font-weight: var(--sc-weight-medium);
      color: var(--sc-text);
    }

    .item-value.success {
      color: var(--sc-success);
    }

    .item-value.error {
      color: var(--sc-error);
    }

    @media (max-width: 480px) /* --sc-breakpoint-sm */ {
      .divider {
        display: none;
      }

      .row {
        gap: var(--sc-space-md);
      }
    }
  `;

  override render() {
    return html`
      <div class="row">
        ${this.items.map(
          (item, i) => html`
            ${i > 0 ? html`<div class="divider"></div>` : nothing}
            <div class="item">
              <span class="item-label">${item.label}</span>
              <span class="item-value ${item.accent || ""}">${item.value}</span>
            </div>
          `,
        )}
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-metric-row": ScMetricRow;
  }
}
