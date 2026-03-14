import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";
import "../components/hu-stat-card.js";
import "../components/hu-metric-row.js";
import "../components/hu-stats-row.js";

export interface StatMetric {
  label: string;
  value?: number;
  valueStr?: string;
  sparklineData?: number[];
}

export interface MetricRowItem {
  label: string;
  value: string;
  accent?: "success" | "error";
}

@customElement("hu-overview-stats")
export class ScOverviewStats extends LitElement {
  @property({ type: Array }) metrics: StatMetric[] = [];
  @property({ type: Array }) metricRowItems: MetricRowItem[] = [];

  static override styles = css`
    :host {
      display: block;
    }

    .metrics-block {
      margin-bottom: var(--hu-space-2xl);
    }
  `;

  override render() {
    return html`
      <div class="metrics-block">
        <hu-stats-row>
          ${this.metrics.map(
            (m, i) => html`
              <hu-stat-card
                .value=${m.value ?? 0}
                .valueStr=${m.valueStr ?? ""}
                .label=${m.label}
                .sparklineData=${m.sparklineData ?? []}
                style="--hu-stagger-delay: ${i * 50}ms"
              ></hu-stat-card>
            `,
          )}
        </hu-stats-row>
        <hu-metric-row .items=${this.metricRowItems}></hu-metric-row>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-overview-stats": ScOverviewStats;
  }
}
