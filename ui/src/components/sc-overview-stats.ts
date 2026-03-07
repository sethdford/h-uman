import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";
import "../components/sc-stat-card.js";
import "../components/sc-metric-row.js";
import "../components/sc-stats-row.js";

export interface StatMetric {
  label: string;
  value?: number;
  valueStr?: string;
}

export interface MetricRowItem {
  label: string;
  value: string;
  accent?: "success" | "error";
}

@customElement("sc-overview-stats")
export class ScOverviewStats extends LitElement {
  @property({ type: Array }) metrics: StatMetric[] = [];
  @property({ type: Array }) metricRowItems: MetricRowItem[] = [];

  static override styles = css`
    :host {
      display: block;
    }

    .metrics-block {
      margin-bottom: var(--sc-space-2xl);
    }
  `;

  override render() {
    return html`
      <div class="metrics-block">
        <sc-stats-row>
          ${this.metrics.map(
            (m, i) => html`
              <sc-stat-card
                .value=${m.value ?? 0}
                .valueStr=${m.valueStr ?? ""}
                .label=${m.label}
                style="--sc-stagger-delay: ${i * 50}ms"
              ></sc-stat-card>
            `,
          )}
        </sc-stats-row>
        <sc-metric-row .items=${this.metricRowItems}></sc-metric-row>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-overview-stats": ScOverviewStats;
  }
}
